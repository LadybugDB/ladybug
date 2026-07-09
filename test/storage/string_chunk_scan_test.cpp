#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/vector/value_vector.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/page_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/column_chunk.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/string_chunk_data.h"
#include "storage/table/string_column.h"
#include <span>

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {
namespace {

using MaybeString = std::optional<std::string_view>;
using string_index_t = DictionaryChunk::string_index_t;

constexpr std::string_view NULL_SENTINEL = "<NULL>";

class StringChunkScanTest : public DBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp(); // NOLINT
        createDBAndConn();
    }

    std::string getInputDir() override { return TestHelper::appendLbugRootPath("dataset/empty/"); }
};

MaybeString value(std::string_view str) {
    return MaybeString{str};
}

MaybeString nullValue() {
    return std::nullopt;
}

std::string printableValue(const MaybeString& maybeValue) {
    if (!maybeValue.has_value()) {
        return std::string{NULL_SENTINEL};
    }
    return std::string{*maybeValue};
}

std::vector<std::string> printableValues(std::span<const MaybeString> values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& maybeValue : values) {
        result.push_back(printableValue(maybeValue));
    }
    return result;
}

std::vector<std::string> expectedRange(std::span<const MaybeString> values, offset_t startRow,
    offset_t numRows) {
    std::vector<std::string> result;
    result.reserve(numRows);
    for (auto i = 0u; i < numRows; i++) {
        result.push_back(printableValue(values[startRow + i]));
    }
    return result;
}

std::vector<std::string> expectedPrefixedRange(std::span<const MaybeString> prefix,
    std::span<const MaybeString> values, offset_t startRow, offset_t numRows) {
    auto result = printableValues(prefix);
    auto range = expectedRange(values, startRow, numRows);
    result.insert(result.end(), range.begin(), range.end());
    return result;
}

LogicalType makeStringLikeType(const StringColumn& column) {
    if (column.getDataType().getLogicalTypeID() == LogicalTypeID::JSON) {
        return LogicalType::JSON();
    }
    return LogicalType::STRING();
}

void writeMaybeStringValue(StringChunkData& chunk, ValueVector& vector, offset_t row,
    const MaybeString& maybeValue) {
    vector.setNull(0, !maybeValue.has_value());
    if (maybeValue.has_value()) {
        StringVector::addString(&vector, 0, *maybeValue);
    }
    chunk.write(&vector, 0, row);
}

struct PersistedStringChunk {
    std::unique_ptr<ColumnChunkData> chunk;
    ChunkState state;
};

PersistedStringChunk buildPersistedStringChunk(MemoryManager& memoryManager,
    PageManager& pageManager, StringColumn& column, std::span<const MaybeString> values) {
    auto chunk = ColumnChunkFactory::createColumnChunkData(memoryManager,
        makeStringLikeType(column), true /*enableCompression*/,
        std::max<uint64_t>(values.size(), 1), ResidencyState::IN_MEMORY);
    auto& stringChunk = chunk->cast<StringChunkData>();
    ValueVector valueVector{makeStringLikeType(column), &memoryManager};
    for (auto row = 0u; row < values.size(); row++) {
        writeMaybeStringValue(stringChunk, valueVector, row, values[row]);
    }
    chunk->flush(pageManager);

    ChunkState state;
    state.column = &column;
    state.segmentStates.resize(1);
    chunk->initializeScanState(state.segmentStates[0], &column);
    return {std::move(chunk), std::move(state)};
}

struct MaterializedStringChunk {
    std::vector<std::string> values;
    std::vector<std::optional<string_index_t>> indexes;
    uint64_t dictionarySize;
};

MaterializedStringChunk materializeStringChunk(const StringChunkData& chunk, offset_t numRows) {
    MaterializedStringChunk result;
    result.values.reserve(numRows);
    result.indexes.reserve(numRows);
    result.dictionarySize = chunk.getDictionaryChunk().getOffsetChunk()->getNumValues();
    const auto* indexChunk = chunk.getIndexColumnChunk();
    for (auto row = 0u; row < numRows; row++) {
        if (chunk.isNull(row)) {
            result.values.push_back(std::string{NULL_SENTINEL});
            result.indexes.push_back(std::nullopt);
            continue;
        }
        result.values.push_back(chunk.getValue<std::string>(row));
        result.indexes.push_back(indexChunk->getValue<string_index_t>(row));
    }
    return result;
}

void expectIndexesInBounds(const MaterializedStringChunk& chunk) {
    for (const auto& maybeIndex : chunk.indexes) {
        if (maybeIndex.has_value()) {
            EXPECT_LT(*maybeIndex, chunk.dictionarySize);
        }
    }
}

std::vector<std::string> materializeValueVector(const ValueVector& vector, offset_t numRows) {
    std::vector<std::string> values;
    values.reserve(numRows);
    for (auto row = 0u; row < numRows; row++) {
        if (vector.isNull(row)) {
            values.push_back(std::string{NULL_SENTINEL});
            continue;
        }
        values.push_back(vector.getValue<string_t>(row).getAsString());
    }
    return values;
}

MaterializedStringChunk scanToStringChunk(MemoryManager& memoryManager, StringColumn& column,
    const ChunkState& state, offset_t startRow, offset_t numRows,
    std::span<const MaybeString> prefix = {}) {
    auto outputCapacity = std::max<uint64_t>(prefix.size() + numRows, 1);
    auto outputChunk =
        ColumnChunkFactory::createColumnChunkData(memoryManager, makeStringLikeType(column),
            true /*enableCompression*/, outputCapacity, ResidencyState::IN_MEMORY);
    auto& outputStringChunk = outputChunk->cast<StringChunkData>();
    ValueVector valueVector{makeStringLikeType(column), &memoryManager};
    for (auto row = 0u; row < prefix.size(); row++) {
        writeMaybeStringValue(outputStringChunk, valueVector, row, prefix[row]);
    }

    if (prefix.empty()) {
        column.scan(state, outputChunk.get(), startRow, numRows);
    } else {
        static_cast<const Column&>(column).scanSegment(state.segmentStates[0], outputChunk.get(),
            startRow, numRows);
    }

    return materializeStringChunk(outputStringChunk, prefix.size() + numRows);
}

std::vector<std::string> scanToValueVector(MemoryManager& memoryManager, StringColumn& column,
    const ChunkState& state, offset_t startRow, offset_t numRows) {
    ValueVector outputVector{makeStringLikeType(column), &memoryManager};
    column.scan(state, startRow, numRows, &outputVector, 0 /*offsetInVector*/);
    return materializeValueVector(outputVector, numRows);
}

struct ScanCase {
    std::string name;
    std::vector<MaybeString> values;
    offset_t startRow;
    offset_t numRows;
    std::vector<MaybeString> prefix;
};

} // namespace

TEST_F(StringChunkScanTest, PartialAndFullStringChunkScansMatchValueVectorScans) {
    auto* memoryManager = getMemoryManager(*database);
    auto* storageManager = getStorageManager(*database);
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_value", LogicalType::STRING(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<ScanCase> cases = {
        {
            "duplicate first occurrence before partial range",
            {value("REL_A"), value("REL_B"), value("REL_B"), value("REL_A")},
            1,
            3,
            {},
        },
        {
            "dictionary order differs from row encounter order",
            {value("ALPHA"), value("BETA"), value("ALPHA"), value("GAMMA"), value("BETA")},
            1,
            4,
            {},
        },
        {
            "unique values partial range",
            {value("ALPHA"), value("BETA"), value("GAMMA"), value("DELTA")},
            1,
            3,
            {},
        },
        {
            "all values duplicate",
            {value("ALPHA"), value("ALPHA"), value("ALPHA"), value("ALPHA")},
            1,
            3,
            {},
        },
        {
            "duplicate values separated by null",
            {value("ALPHA"), nullValue(), value("ALPHA"), value("BETA"), value("ALPHA")},
            0,
            5,
            {},
        },
        {
            "all-null partial range",
            {value("ROOT"), nullValue(), nullValue(), nullValue(), value("TAIL")},
            1,
            3,
            {},
        },
        {
            "empty strings and duplicates",
            {value(""), value("ALPHA"), value(""), nullValue(), value("BETA"), value("")},
            0,
            6,
            {},
        },
        {
            "full segment uses entire dictionary fast path",
            {value("REL_A"), value("REL_B"), value("REL_B"), value("REL_A")},
            0,
            4,
            {},
        },
        {
            "partial scan appends after existing dictionary rows",
            {value("REL_A"), value("REL_B"), value("REL_B"), value("REL_A")},
            1,
            3,
            {value("PREFIX_ONE"), value("PREFIX_TWO")},
        },
        {
            "high duplicate pressure partial range",
            {value("ROOT"), value("HOT"), value("HOT"), value("HOT"), value("COLD"), value("HOT"),
                value("COLD"), value("HOT")},
            1,
            7,
            {},
        },
        {
            "low duplicate pressure partial range",
            {value("ROOT"), value("A"), value("B"), value("C"), value("D"), value("E"), value("F"),
                value("A")},
            1,
            7,
            {},
        },
    };

    for (const auto& scanCase : cases) {
        SCOPED_TRACE(scanCase.name);
        auto persisted =
            buildPersistedStringChunk(*memoryManager, pageManager, column, scanCase.values);

        const auto valueVectorValues = scanToValueVector(*memoryManager, column, persisted.state,
            scanCase.startRow, scanCase.numRows);
        const auto stringChunkValues = scanToStringChunk(*memoryManager, column, persisted.state,
            scanCase.startRow, scanCase.numRows, scanCase.prefix);

        EXPECT_EQ(valueVectorValues,
            expectedRange(scanCase.values, scanCase.startRow, scanCase.numRows));
        EXPECT_EQ(stringChunkValues.values, expectedPrefixedRange(scanCase.prefix, scanCase.values,
                                                scanCase.startRow, scanCase.numRows));
        expectIndexesInBounds(stringChunkValues);
    }
}

TEST_F(StringChunkScanTest, PartialStringChunkScansPreserveIndexInvariantsWhenAppending) {
    auto* memoryManager = getMemoryManager(*database);
    auto* storageManager = getStorageManager(*database);
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_value", LogicalType::STRING(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<MaybeString> values = {value("REL_A"), value("REL_B"), value("REL_B"),
        value("REL_A")};
    const std::vector<MaybeString> prefix = {value("PREFIX_ONE"), value("PREFIX_TWO")};
    auto persisted = buildPersistedStringChunk(*memoryManager, pageManager, column, values);

    const auto scanned = scanToStringChunk(*memoryManager, column, persisted.state, 1, 3, prefix);

    EXPECT_EQ(scanned.values, expectedPrefixedRange(prefix, values, 1, 3));
    expectIndexesInBounds(scanned);
    ASSERT_EQ(scanned.indexes.size(), 5u);
    ASSERT_TRUE(scanned.indexes[2].has_value());
    ASSERT_TRUE(scanned.indexes[3].has_value());
    ASSERT_TRUE(scanned.indexes[4].has_value());
    EXPECT_EQ(scanned.indexes[2], scanned.indexes[3]);
    EXPECT_NE(scanned.indexes[2], scanned.indexes[4]);
    EXPECT_GE(*scanned.indexes[2], static_cast<string_index_t>(prefix.size()));
    EXPECT_GE(*scanned.indexes[4], static_cast<string_index_t>(prefix.size()));
}

TEST_F(StringChunkScanTest, JsonStringChunkScansMatchValueVectorScans) {
    auto* memoryManager = getMemoryManager(*database);
    auto* storageManager = getStorageManager(*database);
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_json", LogicalType::JSON(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<MaybeString> values = {value("{\"kind\":\"root\"}"),
        value("{\"kind\":\"edge\"}"), value("{\"kind\":\"edge\"}"), nullValue(),
        value("{\"kind\":\"leaf\"}")};
    auto persisted = buildPersistedStringChunk(*memoryManager, pageManager, column, values);

    const auto valueVectorValues = scanToValueVector(*memoryManager, column, persisted.state, 1, 4);
    const auto stringChunkValues = scanToStringChunk(*memoryManager, column, persisted.state, 1, 4);

    EXPECT_EQ(valueVectorValues, expectedRange(values, 1, 4));
    EXPECT_EQ(stringChunkValues.values, expectedRange(values, 1, 4));
    expectIndexesInBounds(stringChunkValues);
}

TEST_F(StringChunkScanTest, ValueVectorPartialScansRemainCorrectWhenDictionaryOffsetsCanBeSorted) {
    auto* memoryManager = getMemoryManager(*database);
    auto* storageManager = getStorageManager(*database);
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_value", LogicalType::STRING(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<MaybeString> values = {value("REL_A"), value("REL_B"), value("REL_B"),
        value("REL_A")};
    auto persisted = buildPersistedStringChunk(*memoryManager, pageManager, column, values);

    const auto scanned = scanToValueVector(*memoryManager, column, persisted.state, 1, 3);

    EXPECT_EQ(scanned, expectedRange(values, 1, 3));
}

} // namespace testing
} // namespace lbug
