#include <algorithm>
#include <functional>
#include <numeric>
#include <vector>

#include "api_test/private_api_test.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/data_chunk/data_chunk_state.h"
#include "common/types/date_t.h"
#include "common/types/string_t.h"
#include "common/types/types.h"
#include "graph/graph_entry.h"
#include "graph/on_disk_graph.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/csr_node_group.h"
#include "storage/table/rel_table.h"
#include <format>

namespace lbug {

using common::Date;
using common::date_t;
using common::nodeID_t;
using common::offset_t;
namespace testing {

class RelScanTest : public PrivateApiTest {
public:
    void SetUp() override {
        PrivateApiTest::SetUp();
        conn->query("BEGIN TRANSACTION");
        context = getClientContext(*conn);
        catalog = catalog::Catalog::Get(*context);
        auto transaction = transaction::Transaction::Get(*context);
        std::vector<catalog::TableCatalogEntry*> nodeEntries;
        for (auto& entry : catalog->getNodeTableEntries(transaction)) {
            nodeEntries.push_back(entry);
        }
        std::vector<catalog::TableCatalogEntry*> relEntries;
        for (auto& entry : catalog->getRelGroupEntries(transaction)) {
            relEntries.push_back(entry);
        }
        auto entry = graph::NativeGraphEntry(nodeEntries, relEntries);
        graph = std::make_unique<graph::OnDiskGraph>(context, std::move(entry));

        fwdStorageOnly = (common::DEFAULT_EXTEND_DIRECTION == common::ExtendDirection::FWD);
    }

public:
    std::unique_ptr<graph::OnDiskGraph> graph;
    main::ClientContext* context = nullptr;
    catalog::Catalog* catalog = nullptr;
    bool fwdStorageOnly = false;
};

class VertexScanTest : public RelScanTest {};

class EmptyVertexScanTest : public PrivateApiTest {
public:
    void SetUp() override { PrivateApiTest::SetUp(); }

    std::string getInputDir() override { return "empty"; }

    std::unique_ptr<graph::OnDiskGraph> graph;
    main::ClientContext* context = nullptr;
    catalog::Catalog* catalog = nullptr;
};

// Test correctness of scan fwd
TEST_F(RelScanTest, ScanFwd) {
    auto transaction = transaction::Transaction::Get(*context);
    auto nodeEntry = catalog->getTableCatalogEntry(transaction, "person");
    auto tableID = nodeEntry->getTableID();
    auto relEntry = catalog->getTableCatalogEntry(transaction, "knows");
    auto relTableID =
        relEntry->ptrCast<catalog::RelGroupCatalogEntry>()->getSingleRelEntryInfo().oid;
    auto scanState = graph->prepareRelScan(*relEntry, relTableID, nodeEntry->getTableID(),
        {common::InternalKeyword::ID, "date"});

    std::unordered_map<offset_t, date_t> expectedDates = {
        {0, Date::fromDate(2021, 6, 30)},
        {1, Date::fromDate(2021, 6, 30)},
        {2, Date::fromDate(2021, 6, 30)},
        {3, Date::fromDate(2021, 6, 30)},
        {4, Date::fromDate(1950, 5, 14)},
        {5, Date::fromDate(1950, 5, 14)},
        {6, Date::fromDate(2021, 6, 30)},
        {7, Date::fromDate(1950, 5, 14)},
        {8, Date::fromDate(2000, 1, 1)},
        {9, Date::fromDate(2021, 6, 30)},
        {10, Date::fromDate(1950, 05, 14)},
        {11, Date::fromDate(2000, 1, 1)},
    };

    const auto compare = [&](uint64_t node, common::offset_vec_t expectedNodeOffsets,
                             common::offset_vec_t expectedFwdRelOffsets,
                             common::offset_vec_t expectedBwdRelOffsets) {
        common::offset_vec_t resultNodeOffsets;
        std::vector<nodeID_t> expectedNodes;
        std::ranges::transform(expectedNodeOffsets, std::back_inserter(expectedNodes),
            [&](auto offset) { return nodeID_t{offset, tableID}; });

        common::offset_vec_t resultRelOffsets;
        std::vector<date_t> resultDates;
        for (const auto chunk : graph->scanFwd(nodeID_t{node, tableID}, *scanState)) {
            chunk.forEach([&](auto neighbors, auto propertyVectors, auto i) {
                auto nbr = neighbors[i];
                EXPECT_EQ(nbr.tableID, tableID);
                auto edgeID = propertyVectors[0]->template getValue<nodeID_t>(i);
                resultNodeOffsets.push_back(nbr.offset);
                EXPECT_EQ(edgeID.tableID, relTableID);
                resultRelOffsets.push_back(edgeID.offset);
                auto date = propertyVectors[1]->template getValue<date_t>(i);
                resultDates.push_back(date);
            });
        }
        EXPECT_EQ(resultNodeOffsets, expectedNodeOffsets);
        EXPECT_EQ(resultRelOffsets, expectedFwdRelOffsets);
        for (size_t i = 0; i < resultRelOffsets.size(); i++) {
            EXPECT_EQ(expectedDates[resultRelOffsets[i]], resultDates[i])
                << " Result " << i << " (rel offset " << resultRelOffsets[i] << ") was "
                << Date::toString(resultDates[i]) << " but we expected "
                << Date::toString(expectedDates[resultRelOffsets[i]]);
        }
        EXPECT_EQ(graph->scanFwd(nodeID_t{node, tableID}, *scanState).collectNbrNodes(),
            expectedNodes);

        if (!fwdStorageOnly) {
            resultNodeOffsets.clear();
            resultRelOffsets.clear();
            resultDates.clear();

            for (const auto chunk : graph->scanBwd(nodeID_t{node, tableID}, *scanState)) {
                chunk.forEach([&](auto neighbors, auto propertyVectors, auto i) {
                    auto nbr = neighbors[i];
                    auto edgeID = propertyVectors[0]->template getValue<nodeID_t>(i);
                    auto date = propertyVectors[1]->template getValue<date_t>(i);
                    EXPECT_EQ(nbr.tableID, tableID);
                    resultNodeOffsets.push_back(nbr.offset);
                    EXPECT_EQ(edgeID.tableID, relTableID);
                    resultRelOffsets.push_back(edgeID.offset);
                    resultDates.push_back(date);
                });
            }
            EXPECT_EQ(resultNodeOffsets, expectedNodeOffsets);
            EXPECT_EQ(resultRelOffsets, expectedBwdRelOffsets);
            for (size_t i = 0; i < resultRelOffsets.size(); i++) {
                EXPECT_EQ(expectedDates[resultRelOffsets[i]], resultDates[i])
                    << " Result " << i << " (rel offset " << resultRelOffsets[i] << ") was "
                    << Date::toString(resultDates[i]) << " but we expected "
                    << Date::toString(expectedDates[resultRelOffsets[i]]);
            }
            EXPECT_EQ(graph->scanFwd(nodeID_t{node, tableID}, *scanState).collectNbrNodes(),
                expectedNodes);
        }
    };
    compare(0, {1, 2, 3}, {0, 1, 2}, {3, 6, 9});
    compare(1, {0, 2, 3}, {3, 4, 5}, {0, 7, 10});
    compare(2, {0, 1, 3}, {6, 7, 8}, {1, 4, 11});
}

TEST_F(VertexScanTest, ScanVertexProperties) {
    auto entry = catalog->getTableCatalogEntry(transaction::Transaction::Get(*context), "person");
    std::vector<std::string> properties = {"fname", "height"};

    auto scanState = graph->prepareVertexScan(entry, properties);
    const auto compare =
        [&](offset_t startNodeOffset, offset_t endNodeOffset,
            const std::vector<std::tuple<offset_t, std::string, float>>& expectedNames) {
            std::vector<std::tuple<offset_t, std::string, float>> results;
            for (auto chunk : graph->scanVertices(startNodeOffset, endNodeOffset, *scanState)) {
                for (size_t i = 0; i < chunk.size(); i++) {
                    results.push_back(std::make_tuple(chunk.getNodeIDs()[i].offset,
                        chunk.getProperties<common::string_t>(0)[i].getAsString(),
                        chunk.getProperties<float>(1)[i]));
                }
            }
            ASSERT_EQ(results, expectedNames);
        };
    compare(0, 3, {{0, "Alice", 1.731}, {1, "Bob", 0.99}, {2, "Carol", 1.0}});
    compare(1, 3, {{1, "Bob", 0.99}, {2, "Carol", 1.0}});
    compare(2, 4, {{2, "Carol", 1.0}, {3, "Dan", 1.3}});
}

TEST_F(EmptyVertexScanTest, ScanVertexProperties) {
    auto res = conn->query("CREATE NODE TABLE account(id INT64 PRIMARY KEY);");
    ASSERT_TRUE(res->isSuccess());
    conn->query("CALL threads=1;");
    res = conn->query("UNWIND range(0, 10000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());
    res = conn->query("UNWIND range(10001, 20000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());
    res = conn->query("UNWIND range(20001, 30000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());
    conn->query("BEGIN TRANSACTION");
    context = getClientContext(*conn);
    catalog = catalog::Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    std::vector<catalog::TableCatalogEntry*> nodeEntries;
    for (auto& entry : catalog->getNodeTableEntries(transaction)) {
        nodeEntries.push_back(entry);
    }
    auto entry = graph::NativeGraphEntry(nodeEntries, {});
    graph = std::make_unique<graph::OnDiskGraph>(context, std::move(entry));

    auto tableEntry =
        catalog->getTableCatalogEntry(transaction::Transaction::Get(*context), "account");
    std::vector<std::string> properties = {"id"};
    auto scanState = graph->prepareVertexScan(tableEntry, properties);
    const auto compare = [&](offset_t startNode, offset_t endNode) {
        common::idx_t idx = 0;
        for (auto chunk : graph->scanVertices(startNode, endNode, *scanState)) {
            for (auto i = 0u; i < chunk.size(); i++) {
                auto nodeID = chunk.getNodeIDs()[i];
                auto id = chunk.getProperties<int64_t>(0)[i];
                ASSERT_EQ(nodeID.offset, idx + startNode);
                ASSERT_EQ(id, idx + startNode);
                idx++;
            }
        }
        ASSERT_EQ(idx, endNode - startNode);
    };

    compare(0, 1000);
    compare(1, 20000);
    compare(14444, 20000);
    compare(24444, 29889);
}

TEST_F(EmptyVertexScanTest, ScanVertexPropertiesAfterDeletionReInsert) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    auto res = conn->query("CREATE NODE TABLE account(id INT64 PRIMARY KEY);");
    ASSERT_TRUE(res->isSuccess());
    conn->query("CALL threads=1;");
    res = conn->query("UNWIND range(0, 10000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());
    res = conn->query("CHECKPOINT");
    ASSERT_TRUE(res->isSuccess());
    conn->query("MATCH (a:account) DELETE a;");
    ASSERT_TRUE(res->isSuccess());
    conn->query("CHECKPOINT");
    res = conn->query("UNWIND range(10001, 20000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());
    res = conn->query("UNWIND range(20001, 30000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());

    conn->query("BEGIN TRANSACTION");
    context = getClientContext(*conn);
    catalog = catalog::Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    std::vector<catalog::TableCatalogEntry*> nodeEntries;
    for (auto& entry : catalog->getNodeTableEntries(transaction)) {
        nodeEntries.push_back(entry);
    }
    auto entry = graph::NativeGraphEntry(nodeEntries, {});
    graph = std::make_unique<graph::OnDiskGraph>(context, std::move(entry));

    auto tableEntry =
        catalog->getTableCatalogEntry(transaction::Transaction::Get(*context), "account");
    std::vector<std::string> properties = {"id"};
    auto scanState = graph->prepareVertexScan(tableEntry, properties);
    const auto compare = [&](offset_t startNode, offset_t endNode) {
        common::idx_t idx = 0;
        for (auto chunk : graph->scanVertices(startNode, endNode, *scanState)) {
            for (auto i = 0u; i < chunk.size(); i++) {
                auto nodeID = chunk.getNodeIDs()[i];
                auto id = chunk.getProperties<int64_t>(0)[i];
                ASSERT_EQ(nodeID.offset, idx + startNode);
                ASSERT_EQ(id, idx + startNode);
                idx++;
            }
        }
        ASSERT_EQ(idx, endNode - startNode);
    };

    compare(14444, 20000);
    compare(24444, 29889);

    // Scan from 0 to 1000 should return nothing, since we deleted all nodes.
    common::idx_t idx = 0;
    for (auto chunk : graph->scanVertices(0, 1000, *scanState)) {
        for (auto i = 0u; i < chunk.size(); i++) {
            idx++;
        }
    }
    ASSERT_EQ(idx, 0);

    // Scan from 5000 to 20,000 should scan non-deleted tuples only.
    idx = 10001;
    for (auto chunk : graph->scanVertices(5000, 20000, *scanState)) {
        for (auto i = 0u; i < chunk.size(); i++) {
            auto nodeID = chunk.getNodeIDs()[i];
            auto id = chunk.getProperties<int64_t>(0)[i];
            ASSERT_EQ(nodeID.offset, idx);
            ASSERT_EQ(id, idx);
            idx++;
        }
    }
    ASSERT_EQ(idx, 20000);
}

TEST_F(EmptyVertexScanTest, ScanVertexPropertiesAfterInterleavedDeletion) {
    auto res = conn->query("CREATE NODE TABLE account(id INT64 PRIMARY KEY, name STRING);");
    ASSERT_TRUE(res->isSuccess());
    // Every third name is NULL, so the null mask moves along with the values.
    res = conn->query("UNWIND range(0, 4999) AS i CREATE (a:account {id: i, name: CASE WHEN "
                      "i % 3 = 0 THEN NULL ELSE 'name' + string(i) + ' long enough to not be "
                      "inlined' END});");
    ASSERT_TRUE(res->isSuccess());
    res = conn->query("MATCH (a:account) WHERE a.id % 2 = 0 OR a.id < 10 DELETE a;");
    ASSERT_TRUE(res->isSuccess());

    const auto checkScan = [&](const std::function<bool(int64_t)>& isLive) {
        context = getClientContext(*conn);
        catalog = catalog::Catalog::Get(*context);
        auto transaction = transaction::Transaction::Get(*context);
        auto tableEntry = catalog->getTableCatalogEntry(transaction, "account");
        graph = std::make_unique<graph::OnDiskGraph>(context,
            graph::NativeGraphEntry({tableEntry}, {}));
        auto scanState = graph->prepareVertexScan(tableEntry, {"id", "name"});
        for (auto [start, end] : {std::pair<offset_t, offset_t>{0, 5000}, {1000, 4500}}) {
            std::vector<int64_t> expected;
            for (auto id = (int64_t)start; id < (int64_t)end; id++) {
                if (isLive(id)) {
                    expected.push_back(id);
                }
            }
            std::vector<int64_t> actual;
            for (auto chunk : graph->scanVertices(start, end, *scanState)) {
                for (auto i = 0u; i < chunk.size(); i++) {
                    auto id = chunk.getProperties<int64_t>(0)[i];
                    ASSERT_EQ(chunk.getNodeIDs()[i].offset, (offset_t)id);
                    if (id % 3 != 0) {
                        ASSERT_EQ(chunk.getProperties<common::string_t>(1)[i].getAsString(),
                            "name" + std::to_string(id) + " long enough to not be inlined");
                    }
                    actual.push_back(id);
                }
            }
            ASSERT_EQ(actual, expected);
        }
    };

    conn->query("BEGIN TRANSACTION");
    checkScan([](int64_t id) { return id % 2 == 1 && id >= 10; });
    conn->query("COMMIT");
    if (!inMemMode) {
        res = conn->query("CHECKPOINT");
        ASSERT_TRUE(res->isSuccess());
        conn->query("BEGIN TRANSACTION");
        checkScan([](int64_t id) { return id % 2 == 1 && id >= 10; });
        conn->query("COMMIT");
    }
    conn->query("BEGIN TRANSACTION");
    res = conn->query("MATCH (a:account) WHERE a.id % 3 = 0 DELETE a;");
    ASSERT_TRUE(res->isSuccess());
    checkScan([](int64_t id) { return id % 2 == 1 && id >= 10 && id % 3 != 0; });
    conn->query("ROLLBACK");
}

TEST_F(EmptyVertexScanTest, CompactToSelectionKeepsValuesAndNulls) {
    auto mm = storage::MemoryManager::Get(*getClientContext(*conn));
    auto state = std::make_shared<common::DataChunkState>(common::DEFAULT_VECTOR_CAPACITY);
    std::vector<common::StructField> fields;
    fields.emplace_back("a", common::LogicalType::INT64());
    fields.emplace_back("b", common::LogicalType::STRING());
    common::ValueVector ints(common::LogicalType::INT64(), mm, state);
    common::ValueVector smallInts(common::LogicalType::INT32(), mm, state);
    common::ValueVector flags(common::LogicalType::BOOL(), mm, state);
    common::ValueVector strings(common::LogicalType::STRING(), mm, state);
    common::ValueVector lists(common::LogicalType::LIST(common::LogicalType::INT64()), mm, state);
    common::ValueVector structs(common::LogicalType::STRUCT(std::move(fields)), mm, state);
    auto a = common::StructVector::getFieldVector(&structs, 0);
    auto b = common::StructVector::getFieldVector(&structs, 1);
    auto listValues = common::ListVector::getDataVector(&lists);
    const auto text = [](int64_t row) {
        return "row " + std::to_string(row) + " long enough to not be inlined";
    };
    for (auto row = 0; row < 6; row++) {
        ints.setValue<int64_t>(row, row);
        smallInts.setValue<int32_t>(row, row);
        flags.setValue<bool>(row, row % 2 == 0);
        common::StringVector::addString(&strings, row, text(row));
        auto entry = common::ListVector::addList(&lists, row + 1);
        lists.setValue(row, entry);
        for (auto k = 0u; k < entry.size; k++) {
            listValues->setValue<int64_t>(entry.offset + k, row * 10 + k);
        }
        a->setValue<int64_t>(row, row);
        common::StringVector::addString(b.get(), row, text(row));
    }
    ints.setNull(3, true);
    smallInts.setNull(5, true);
    strings.setNull(2, true);
    lists.setNull(0, true);
    structs.setNull(3, true); // Also nulls the fields of row 3.
    a->setNull(5, true);
    auto& selVector = state->getSelVectorUnsafe();
    // A filtered selection without gaps moves nothing.
    selVector.setToFiltered(3);
    std::iota(selVector.getMutableBuffer().begin(), selVector.getMutableBuffer().begin() + 3, 0);
    graph::compactToSelection(ints, selVector);
    for (auto row = 0; row < 3; row++) {
        ASSERT_EQ(ints.getValue<int64_t>(row), row);
    }
    // Rows 1 and 4 were deleted.
    const std::vector<common::sel_t> rows{0, 2, 3, 5};
    selVector.setToFiltered(rows.size());
    std::copy(rows.begin(), rows.end(), selVector.getMutableBuffer().begin());

    for (auto vector : {&ints, &smallInts, &flags, &strings, &lists, &structs}) {
        graph::compactToSelection(*vector, selVector);
    }
    for (auto i = 0u; i < rows.size(); i++) {
        const auto row = rows[i];
        // A struct slot keeps pointing at its own position; only the fields moved.
        ASSERT_EQ(structs.getValue<common::struct_entry_t>(i).pos, i);
        ASSERT_EQ(smallInts.isNull(i), row == 5);
        if (!smallInts.isNull(i)) {
            ASSERT_EQ(smallInts.getValue<int32_t>(i), (int32_t)row);
        }
        ASSERT_FALSE(flags.isNull(i));
        ASSERT_EQ(flags.getValue<bool>(i), row % 2 == 0);
        ASSERT_EQ(lists.isNull(i), row == 0);
        if (!lists.isNull(i)) {
            auto entry = lists.getValue<common::list_entry_t>(i);
            ASSERT_EQ(entry.size, row + 1);
            for (auto k = 0u; k < entry.size; k++) {
                ASSERT_EQ(listValues->getValue<int64_t>(entry.offset + k), (int64_t)(row * 10 + k));
            }
        }
        ASSERT_EQ(ints.isNull(i), row == 3);
        ASSERT_EQ(strings.isNull(i), row == 2);
        ASSERT_EQ(structs.isNull(i), row == 3);
        ASSERT_EQ(a->isNull(i), row == 3 || row == 5);
        ASSERT_EQ(b->isNull(i), row == 3);
        if (!ints.isNull(i)) {
            ASSERT_EQ(ints.getValue<int64_t>(i), row);
        }
        if (!strings.isNull(i)) {
            ASSERT_EQ(strings.getValue<common::string_t>(i).getAsString(), text(row));
        }
        if (!a->isNull(i)) {
            ASSERT_EQ(a->getValue<int64_t>(i), row);
        }
        if (!b->isNull(i)) {
            ASSERT_EQ(b->getValue<common::string_t>(i).getAsString(), text(row));
        }
    }
}

TEST_F(EmptyVertexScanTest, ScanVertexPropertiesDuringTransaction) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    auto res = conn->query("CREATE NODE TABLE account(id INT64 PRIMARY KEY);");
    ASSERT_TRUE(res->isSuccess());
    conn->query("CALL threads=1;");
    res = conn->query("UNWIND range(0, 10000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());
    conn->query("BEGIN TRANSACTION");
    res = conn->query("UNWIND range(10001, 20000) AS i CREATE (a:account {ID: i});");
    ASSERT_TRUE(res->isSuccess());

    context = getClientContext(*conn);
    catalog = catalog::Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    std::vector<catalog::TableCatalogEntry*> nodeEntries;
    for (auto& entry : catalog->getNodeTableEntries(transaction)) {
        nodeEntries.push_back(entry);
    }
    auto entry = graph::NativeGraphEntry(nodeEntries, {});
    graph = std::make_unique<graph::OnDiskGraph>(context, std::move(entry));

    auto tableEntry =
        catalog->getTableCatalogEntry(transaction::Transaction::Get(*context), "account");
    std::vector<std::string> properties = {"id"};
    auto scanState = graph->prepareVertexScan(tableEntry, properties);
    const auto compare = [&](offset_t startNode, offset_t endNode) {
        common::idx_t idx = 0;
        for (auto chunk : graph->scanVertices(startNode, endNode, *scanState)) {
            for (auto i = 0u; i < chunk.size(); i++) {
                auto nodeID = chunk.getNodeIDs()[i];
                auto id = chunk.getProperties<int64_t>(0)[i];
                ASSERT_EQ(nodeID.offset, idx + startNode);
                ASSERT_EQ(id, idx + startNode);
                idx++;
            }
        }
        ASSERT_EQ(idx, endNode - startNode);
    };

    compare(0, 1000);
    compare(1, 20000);
    conn->query("COMMIT");
}

// Drives a rel scan over chosen bound-node batches and checks how much of the persistent CSR
// header each batch decodes. Node i has one rel, to (i + 1) % numNodes, so a node's neighbour is
// known and its header entries are i - 1 and i (only 0 for node 0).
class CSRHeaderDecodeTest : public PrivateApiTest {
public:
    static constexpr offset_t numNodes = 8000;

    std::string getInputDir() override { return "empty"; }

    void SetUp() override {
        PrivateApiTest::SetUp();
        if (inMemMode) {
            GTEST_SKIP() << "the decode only runs on a persistent CSR header";
        }
        // LBUG_NODE_GROUP_SIZE_LOG2 is a build option.
        if (common::StorageConfig::NODE_GROUP_SIZE < numNodes) {
            GTEST_SKIP() << "the expected decodes assume all nodes in one node group";
        }
        ASSERT_NO_FATAL_FAILURE(runQuery("CREATE NODE TABLE N(id INT64, PRIMARY KEY(id));"));
        ASSERT_NO_FATAL_FAILURE(runQuery("CREATE REL TABLE E(FROM N TO N);"));
        ASSERT_NO_FATAL_FAILURE(
            runQuery(std::format("UNWIND range(0, {}) AS i CREATE (:N {{id: i}});", numNodes - 1)));
        ASSERT_NO_FATAL_FAILURE(runQuery(std::format(
            "MATCH (a:N), (b:N) WHERE b.id = (a.id + 1) % {} CREATE (a)-[:E]->(b);", numNodes)));
        ASSERT_NO_FATAL_FAILURE(runQuery("CHECKPOINT;"));
        ASSERT_NO_FATAL_FAILURE(runQuery("BEGIN TRANSACTION READ ONLY;"));
        context = getClientContext(*conn);
        transaction = transaction::Transaction::Get(*context);
        auto* catalog = catalog::Catalog::Get(*context);
        nodeTableID = catalog->getTableCatalogEntry(transaction, "N")->getTableID();
        const auto relTableID = catalog->getTableCatalogEntry(transaction, "E")
                                    ->ptrCast<catalog::RelGroupCatalogEntry>()
                                    ->getSingleRelEntryInfo()
                                    .oid;
        relTable = storage::StorageManager::Get(*context)
                       ->getTable(relTableID)
                       ->ptrCast<storage::RelTable>();
        auto* mm = storage::MemoryManager::Get(*context);
        boundState = std::make_shared<common::DataChunkState>();
        boundVector = std::make_unique<common::ValueVector>(common::LogicalType::INTERNAL_ID(), mm);
        boundVector->state = boundState;
        outState = std::make_shared<common::DataChunkState>();
        nbrVector = std::make_unique<common::ValueVector>(common::LogicalType::INTERNAL_ID(), mm);
        nbrVector->state = outState;
        scanState = std::make_unique<storage::RelTableScanState>(*mm, boundVector.get(),
            std::vector<common::ValueVector*>{nbrVector.get()}, outState);
        scanState->setToTable(transaction, relTable, {storage::NBR_ID_COLUMN_ID}, {},
            common::RelDataDirection::FWD);
    }

    void TearDown() override {
        // These hold buffers from the database's memory manager, which TearDown closes.
        scanState.reset();
        nbrVector.reset();
        boundVector.reset();
        PrivateApiTest::TearDown();
    }

    void runQuery(const std::string& query) const {
        const auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    }

    // Starts a scan of `nodes` as one bound-node batch.
    void startBatch(const std::vector<offset_t>& nodes) {
        boundState->setToUnflat();
        boundState->getSelVectorUnsafe().setToUnfiltered(nodes.size());
        for (auto i = 0u; i < nodes.size(); i++) {
            boundVector->setValue<nodeID_t>(i, nodeID_t{nodes[i], nodeTableID});
        }
        relTable->initScanState(transaction, *scanState);
    }

    // Scans `nodes` as one bound-node batch and returns their neighbours, sorted.
    std::vector<offset_t> scanBatch(const std::vector<offset_t>& nodes) {
        startBatch(nodes);
        std::vector<offset_t> nbrs;
        while (relTable->scan(transaction, *scanState)) {
            const auto& selVector = outState->getSelVector();
            for (auto i = 0u; i < selVector.getSelSize(); i++) {
                nbrs.push_back(nbrVector->getValue<nodeID_t>(selVector[i]).offset);
            }
        }
        std::ranges::sort(nbrs);
        return nbrs;
    }

    static std::vector<offset_t> nextNodes(const std::vector<offset_t>& nodes) {
        std::vector<offset_t> nbrs;
        for (const auto node : nodes) {
            nbrs.push_back((node + 1) % numNodes);
        }
        std::ranges::sort(nbrs);
        return nbrs;
    }

    storage::CSRNodeGroupScanState& csrState() const {
        return scanState->nodeGroupScanState->cast<storage::CSRNodeGroupScanState>();
    }

    using Ranges = std::vector<std::pair<offset_t, offset_t>>;

    main::ClientContext* context = nullptr;
    transaction::Transaction* transaction = nullptr;
    common::table_id_t nodeTableID = common::INVALID_TABLE_ID;
    storage::RelTable* relTable = nullptr;
    std::shared_ptr<common::DataChunkState> boundState;
    std::unique_ptr<common::ValueVector> boundVector;
    std::shared_ptr<common::DataChunkState> outState;
    std::unique_ptr<common::ValueVector> nbrVector;
    std::unique_ptr<storage::RelTableScanState> scanState;
};

// Nodes far apart decode two entries each, in clusters, not the span between them.
TEST_F(CSRHeaderDecodeTest, SparseBatchDecodesClusters) {
    const std::vector<offset_t> nodes{10, 2000, 5000};
    EXPECT_EQ(scanBatch(nodes), nextNodes(nodes));
    const auto& state = csrState();
    EXPECT_EQ(state.numHeaderSpansDecoded, 1u);
    EXPECT_EQ(state.numHeaderEntriesDecoded, 6u);
    EXPECT_FALSE(state.header->wholeHeader);
    EXPECT_EQ(state.header->decodedRanges, (Ranges{{9, 11}, {1999, 2001}, {4999, 5001}}));
}

// Bound nodes whose entries are less than 256 apart share one cluster.
TEST_F(CSRHeaderDecodeTest, NearbyNodesShareCluster) {
    const std::vector<offset_t> nodes{10, 20, 2000};
    EXPECT_EQ(scanBatch(nodes), nextNodes(nodes));
    const auto& state = csrState();
    EXPECT_EQ(state.numHeaderSpansDecoded, 1u);
    EXPECT_EQ(state.numHeaderEntriesDecoded, 14u);
    EXPECT_EQ(state.header->decodedRanges, (Ranges{{9, 21}, {1999, 2001}}));
}

// Nodes too close for clusters to pay off decode the packed span between them.
TEST_F(CSRHeaderDecodeTest, CloseNodesDecodeSpan) {
    const std::vector<offset_t> nodes{100, 300};
    EXPECT_EQ(scanBatch(nodes), nextNodes(nodes));
    const auto& state = csrState();
    EXPECT_EQ(state.numHeaderEntriesDecoded, 202u);
    EXPECT_EQ(state.header->indexBase, 99u);
    EXPECT_EQ(state.header->decodedRanges, (Ranges{{99, 301}}));
}

// A later batch inside the decoded ranges decodes nothing; one outside them decodes its own.
TEST_F(CSRHeaderDecodeTest, LaterBatchesReuseDecodedRanges) {
    scanBatch({10, 2000, 5000});
    const std::vector<offset_t> covered{10, 5000};
    EXPECT_EQ(scanBatch(covered), nextNodes(covered));
    EXPECT_EQ(csrState().numHeaderSpansDecoded, 1u);
    EXPECT_EQ(csrState().numHeaderEntriesDecoded, 6u);
    const std::vector<offset_t> uncovered{3000, 7000};
    EXPECT_EQ(scanBatch(uncovered), nextNodes(uncovered));
    EXPECT_EQ(csrState().numHeaderSpansDecoded, 2u);
    EXPECT_EQ(csrState().numHeaderEntriesDecoded, 10u);
    EXPECT_EQ(csrState().header->decodedRanges, (Ranges{{2999, 3001}, {6999, 7001}}));
}

// A dense first batch decodes its span.
TEST_F(CSRHeaderDecodeTest, DenseFirstBatchDecodesSpan) {
    std::vector<offset_t> nodes(200);
    std::iota(nodes.begin(), nodes.end(), 1000);
    EXPECT_EQ(scanBatch(nodes), nextNodes(nodes));
    EXPECT_EQ(csrState().header->indexBase, 999u);
    EXPECT_EQ(csrState().header->decodedRanges, (Ranges{{999, 1200}}));
}

// After a first decode, a dense batch of more than 128 bound nodes decodes the whole header.
TEST_F(CSRHeaderDecodeTest, DenseLaterBatchDecodesWholeHeader) {
    scanBatch({10, 2000, 5000});
    std::vector<offset_t> nodes(200);
    std::iota(nodes.begin(), nodes.end(), 1000);
    EXPECT_EQ(scanBatch(nodes), nextNodes(nodes));
    EXPECT_TRUE(csrState().header->wholeHeader);
}

// Partial decodes stop once their entries would add up to the header's size.
TEST_F(CSRHeaderDecodeTest, EntriesBudgetDecodesWholeHeader) {
    std::vector<offset_t> first, second;
    for (offset_t i = 0; i < 60; i++) {
        first.push_back(10 + 100 * i);
        second.push_back(2000 + 100 * i);
    }
    scanBatch(first);
    EXPECT_EQ(csrState().numHeaderEntriesDecoded, 5902u);
    EXPECT_FALSE(csrState().header->wholeHeader);
    EXPECT_EQ(scanBatch(second), nextNodes(second));
    EXPECT_TRUE(csrState().header->wholeHeader);
}

// After 16 partial decodes in a node group the scan decodes the whole header.
TEST_F(CSRHeaderDecodeTest, ManyMissesDecodeWholeHeader) {
    for (offset_t i = 0; i < 16; i++) {
        scanBatch({100 + i * 400, 300 + i * 400});
    }
    EXPECT_EQ(csrState().numHeaderSpansDecoded, 16u);
    EXPECT_FALSE(csrState().header->wholeHeader);
    const std::vector<offset_t> nodes{7500, 7900};
    EXPECT_EQ(scanBatch(nodes), nextNodes(nodes));
    EXPECT_TRUE(csrState().header->wholeHeader);
}

// A refill copies only the current list when the next bound list starts 256 or more rows past it,
// even inside the 2048-row window.
TEST_F(CSRHeaderDecodeTest, RefillStopsAtGap) {
    startBatch({10, 1000});
    ASSERT_TRUE(relTable->scan(transaction, *scanState));
    const auto& state = csrState();
    EXPECT_EQ(state.numCachedRows, 1u);
    // Node 1000's list lies inside the window, so only the gap stops the refill.
    EXPECT_LE(state.numTotalRows, common::DEFAULT_VECTOR_CAPACITY);
    EXPECT_GE(state.header->getStartCSROffset(1000) - state.nextRowToScan, 256u);
}

// Adjacent lists share one refill: the rows of nodes 10 to 14 are copied together.
TEST_F(CSRHeaderDecodeTest, RefillRunsThroughAdjacentLists) {
    startBatch({10, 11, 12, 13, 14});
    ASSERT_TRUE(relTable->scan(transaction, *scanState));
    EXPECT_EQ(csrState().numCachedRows, 5u);
}

} // namespace testing
} // namespace lbug
