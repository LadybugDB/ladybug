#include "graph_test/private_graph_test.h"
#include "storage/storage_manager.h"
#include "storage/table/ice_disk_node_table.h"
#include "storage/table/ice_disk_rel_table.h"
#include "main/client_context.h"
#include "transaction/transaction.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "storage/table/csr_node_group.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;
using namespace lbug::catalog;

namespace lbug {
namespace testing {

class IceDiskStorageTest : public DBTest {
public:
    std::string getInputDir() override { return TestHelper::appendLbugRootPath("dataset/demo-db/icebug-disk/"); }

    void SetUp() override {
        DBTest::SetUp();
        conn->query("BEGIN TRANSACTION");
        context = getClientContext(*conn);
        storageManager = database->getStorageManager();
    }

    main::ClientContext* context;
    StorageManager* storageManager;
};

TEST_F(IceDiskStorageTest, NodeTableScanTest) {
    auto catalog = Catalog::Get(*context);
    auto transaction = Transaction::Get(*context);
    auto tableEntry = catalog->getTableCatalogEntry(transaction, "user");
    ASSERT_NE(tableEntry, nullptr);
    auto tableID = tableEntry->getTableID();
    auto table = storageManager->getTable(tableID);
    auto nodeTable = dynamic_cast<IceDiskNodeTable*>(table);

    ASSERT_NE(nodeTable, nullptr);
    EXPECT_EQ(nodeTable->getNumTotalRows(transaction), 4);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), database->getMemoryManager());
    auto nameVector = std::make_unique<ValueVector>(LogicalType::STRING(), database->getMemoryManager());
    auto ageVector = std::make_unique<ValueVector>(LogicalType::INT64(), database->getMemoryManager());

    std::vector<ValueVector*> outputVectors = {nameVector.get(), ageVector.get()};
    auto outState = std::make_shared<DataChunkState>();
    IceDiskNodeTableScanState scanState(nodeIDVector.get(), outputVectors, outState);
    
    // name is column 1, age is column 2
    scanState.setToTable(transaction, nodeTable, {1, 2});
    nodeTable->initializeScanCoordination(transaction);
    nodeTable->initScanState(transaction, scanState);

    int count = 0;
    while (nodeTable->scanInternal(transaction, scanState)) {
        auto selSize = outState->getSelVector().getSelSize();
        for (auto i = 0u; i < selSize; i++) {
            auto pos = outState->getSelVector()[i];
            auto name = ((lbug::common::string_t*)nameVector->getData())[pos].getAsString();
            auto age = ((int64_t*)ageVector->getData())[pos];
            if (name == "Adam") { EXPECT_EQ(age, 30); }
            else if (name == "Karissa") { EXPECT_EQ(age, 40); }
            else if (name == "Zhang") { EXPECT_EQ(age, 50); }
            else if (name == "Noura") { EXPECT_EQ(age, 25); }
            count++;
        }
    }
    EXPECT_EQ(count, 4);
}

TEST_F(IceDiskStorageTest, RelTableScanTest) {
    auto catalog = Catalog::Get(*context);
    auto transaction = Transaction::Get(*context);
    auto relGroupEntry =
        dynamic_cast<RelGroupCatalogEntry*>(catalog->getTableCatalogEntry(transaction, "follows"));
    
    ASSERT_NE(relGroupEntry, nullptr);
    
    auto relTableID = relGroupEntry->getSingleRelEntryInfo().oid;
    auto table = storageManager->getTable(relTableID);
    auto relTable = dynamic_cast<IceDiskRelTable*>(table);

    ASSERT_NE(relTable, nullptr);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), database->getMemoryManager());
    nodeIDVector->state = std::make_shared<DataChunkState>();
    auto nbrIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), database->getMemoryManager());
    auto sinceVector = std::make_unique<ValueVector>(LogicalType::INT32(), database->getMemoryManager());

    std::vector<ValueVector*> outputVectors = {nbrIDVector.get(), sinceVector.get()};
    auto outState = std::make_shared<DataChunkState>();
    
    auto memManager = database->getMemoryManager();
    IceDiskRelTableScanState scanState(*memManager, nodeIDVector.get(), outputVectors, outState);
    
    auto userTableEntry = catalog->getTableCatalogEntry(transaction, "user");
    ASSERT_NE(userTableEntry, nullptr);
    auto userTableID = userTableEntry->getTableID();
    
    // In this dataset, Adam is at row offset 1 in nodes_user.parquet.
    nodeIDVector->state->getSelVectorUnsafe().setSelSize(1);
    nodeIDVector->state->getSelVectorUnsafe().setToUnfiltered();
    
    nodeID_t srcNode;
    srcNode.offset = 1;
    srcNode.tableID = userTableID;
    ((lbug::common::nodeID_t*)nodeIDVector->getData())[0] = srcNode;

    auto sinceColumnID = relGroupEntry->getColumnID("since");
    scanState.setToTable(transaction, relTable, {NBR_ID_COLUMN_ID, sinceColumnID});
    relTable->initializeScanCoordination(transaction);
    relTable->initScanState(transaction, scanState);

    int count = 0;
    while (relTable->scanInternal(transaction, scanState)) {
        auto selSize = outState->getSelVector().getSelSize();
        for (auto i = 0u; i < selSize; i++) {
            auto pos = outState->getSelVector()[i];
            auto nbr = ((lbug::common::nodeID_t*)nbrIDVector->getData())[pos];
            // Adam follows Karissa (2) and Zhang (3) in nodes_user.parquet row order.
            EXPECT_TRUE(nbr.offset == 2 || nbr.offset == 3);
            count++;
        }
    }
    EXPECT_EQ(count, 2);

    nodeID_t dstNode;
    dstNode.offset = 2;
    dstNode.tableID = userTableID;
    ((lbug::common::nodeID_t*)nodeIDVector->getData())[0] = dstNode;

    scanState.setToTable(transaction, relTable, {NBR_ID_COLUMN_ID, sinceColumnID}, {},
        RelDataDirection::BWD);
    relTable->initializeScanCoordination(transaction);
    relTable->initScanState(transaction, scanState);

    count = 0;
    while (relTable->scanInternal(transaction, scanState)) {
        auto selSize = outState->getSelVector().getSelSize();
        for (auto i = 0u; i < selSize; i++) {
            auto pos = outState->getSelVector()[i];
            auto nbr = ((lbug::common::nodeID_t*)nbrIDVector->getData())[pos];
            EXPECT_EQ(nbr.offset, 1);
            count++;
        }
    }
    EXPECT_EQ(count, 1);
}

} // namespace testing
} // namespace lbug
