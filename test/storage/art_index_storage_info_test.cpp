#include <memory>
#include <string>

#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/serializer.h"
#include "gtest/gtest.h"
#include "storage/index/art_index.h"

using namespace lbug::common;
using namespace lbug::storage;

static std::unique_ptr<IndexStorageInfo> deserialize(const BufferWriter& writer) {
    return ArtPrimaryKeyIndexStorageInfo::deserialize(
        std::make_unique<BufferReader>(writer.getBlobData(), writer.getSize()));
}

// 0.17 stored an ART index as a count followed by (key size, key, offset) entries.
TEST(ArtIndexStorageInfoTest, ReadsTheEntryListOf017) {
    auto writer = std::make_shared<BufferWriter>();
    Serializer ser(writer);
    ser.write<uint64_t>(2);
    for (auto [key, offset] : {std::pair<std::string, offset_t>{"tag0", 0}, {"\xc3\xb3", 7}}) {
        ser.write<uint64_t>(key.size());
        ser.write(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        ser.write<offset_t>(offset);
    }
    auto info = deserialize(*writer);
    const auto& art = info->constCast<ArtPrimaryKeyIndexStorageInfo>();
    ASSERT_EQ(art.treePageRange.startPageIdx, INVALID_PAGE_IDX);
    ASSERT_EQ(art.entries.size(), 2u);
    EXPECT_EQ(std::string(art.entries[1].first.begin(), art.entries[1].first.end()), "\xc3\xb3");
    EXPECT_EQ(art.entries[1].second, 7u);
}

TEST(ArtIndexStorageInfoTest, ReadsAnEmptyEntryListOf017) {
    auto writer = std::make_shared<BufferWriter>();
    Serializer ser(writer);
    ser.write<uint64_t>(0);
    auto info = deserialize(*writer);
    const auto& art = info->constCast<ArtPrimaryKeyIndexStorageInfo>();
    EXPECT_EQ(art.treePageRange.startPageIdx, INVALID_PAGE_IDX);
    EXPECT_TRUE(art.entries.empty());
}

TEST(ArtIndexStorageInfoTest, ReadsTheTreePageRange) {
    ArtPrimaryKeyIndexStorageInfo original{PageRange{12, 3}, 4096};
    auto info = deserialize(*original.serialize());
    const auto& art = info->constCast<ArtPrimaryKeyIndexStorageInfo>();
    EXPECT_EQ(art.treePageRange.startPageIdx, 12u);
    EXPECT_EQ(art.treePageRange.numPages, 3u);
    EXPECT_EQ(art.treeSize, 4096u);
    EXPECT_TRUE(art.entries.empty());
}

// An index loaded from a 0.17 entry list has no tree until a checkpoint writes one, so it must
// serialize the list back rather than an empty page range.
TEST(ArtIndexStorageInfoTest, WritesBackAnEntryListWithoutATree) {
    std::vector<std::pair<std::vector<uint8_t>, offset_t>> entries;
    entries.emplace_back(std::vector<uint8_t>{'a'}, 3);
    entries.emplace_back(std::vector<uint8_t>{'b', 'c'}, 9);
    ArtPrimaryKeyIndexStorageInfo original{std::move(entries)};
    auto info = deserialize(*original.serialize());
    const auto& art = info->constCast<ArtPrimaryKeyIndexStorageInfo>();
    EXPECT_EQ(art.treePageRange.startPageIdx, INVALID_PAGE_IDX);
    ASSERT_EQ(art.entries.size(), 2u);
    EXPECT_EQ(art.entries[1].first, (std::vector<uint8_t>{'b', 'c'}));
    EXPECT_EQ(art.entries[1].second, 9u);
}
