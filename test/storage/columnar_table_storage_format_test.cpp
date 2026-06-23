#include <utility>
#include <vector>

#include "api_test/api_test.h"
#include "arrow_test_utils.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "common/enums/table_storage_format.h"
#include "common/enums/table_type.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "main/client_context.h"
#include "storage/storage_manager.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/columnar_node_table_base.h"
#include "storage/table/columnar_rel_table_base.h"
#include "storage/table/table.h"
#include "test_helper/test_helper.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug;
using namespace lbug::common;
using namespace lbug::testing;

class ColumnarTableStorageFormatTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        context = conn->getClientContext();
    }

    lbug::main::ClientContext* context = nullptr;
};

TEST_F(ColumnarTableStorageFormatTest, ReportsRuntimeTableFormats) {
    auto* context = getClientContext(*conn);
    auto* transaction = &transaction::DUMMY_CHECKPOINT_TRANSACTION;
    auto catalog = catalog::Catalog::Get(*context);
    auto storageManager = database->getStorageManager();

    auto expectFormat = [&](const std::string& tableName, TableStorageFormat expected) {
        auto* entry = catalog->getTableCatalogEntry(transaction, tableName);
        ASSERT_NE(entry, nullptr);
        lbug::common::table_id_t tableID = entry->getTableID();

        if (entry->getTableType() == lbug::common::TableType::NODE) {
            auto* table =
                storageManager->getTable(tableID)->ptrCast<lbug::storage::ColumnarNodeTableBase>();
            ASSERT_NE(table, nullptr);
            EXPECT_EQ(expected, table->getStorageFormat()) << tableName;
        } else {
            tableID = entry->ptrCast<catalog::RelGroupCatalogEntry>()->getSingleRelEntryInfo().oid;
            auto* table =
                storageManager->getTable(tableID)->ptrCast<lbug::storage::ColumnarRelTableBase>();
            ASSERT_NE(table, nullptr);
            EXPECT_EQ(expected, table->getStorageFormat()) << tableName;
        }
    };

    const auto iceDiskStorage = TestHelper::appendLbugRootPath("dataset/ice-disk-test/");
    auto iceDiskResult = conn->query(std::format(
        "CREATE NODE TABLE upperversion(id INT64 PRIMARY KEY) WITH (storage = '{}', format = "
        "'icebug-disk')",
        iceDiskStorage));
    ASSERT_TRUE(iceDiskResult->isSuccess()) << iceDiskResult->toString();
    expectFormat("upperversion", TableStorageFormat::ICEBUG_DISK);

    ArrowSchemaWrapper nodeSchema;
    createStructSchema(&nodeSchema, 1);
    createSchema<int64_t>(nodeSchema.children[0], "id");
    std::vector<int64_t> nodeIds = {1, 2, 3};
    auto nodeArray = createStructArray(static_cast<int64_t>(nodeIds.size()),
        {[&](ArrowArray* array) { createInt64Array(array, nodeIds); }});
    std::vector<ArrowArrayWrapper> nodeArrays;
    nodeArrays.push_back(std::move(nodeArray));
    auto nodeCreation = ArrowTableSupport::createViewFromArrowTable(*conn, "arrow_format_person",
        std::move(nodeSchema), std::move(nodeArrays));
    ASSERT_TRUE(nodeCreation.queryResult->isSuccess()) << nodeCreation.queryResult->toString();
    expectFormat("arrow_format_person", TableStorageFormat::ARROW);

    ArrowSchemaWrapper relSchema;
    createStructSchema(&relSchema, 2);
    createSchema<int64_t>(relSchema.children[0], "from");
    createSchema<int64_t>(relSchema.children[1], "to");
    std::vector<int64_t> relFrom = {0, 1};
    std::vector<int64_t> relTo = {1, 2};
    auto relArray = createStructArray(static_cast<int64_t>(relFrom.size()),
        {[&](ArrowArray* array) { createInt64Array(array, relFrom); },
            [&](ArrowArray* array) { createInt64Array(array, relTo); }});
    std::vector<ArrowArrayWrapper> relArrays;
    relArrays.push_back(std::move(relArray));
    auto relCreation = ArrowTableSupport::createRelTableFromArrowTable(*conn, "arrow_format_knows",
        "arrow_format_person", "arrow_format_person", std::move(relSchema), std::move(relArrays));
    ASSERT_TRUE(relCreation.queryResult->isSuccess()) << relCreation.queryResult->toString();
    expectFormat("arrow_format_knows", TableStorageFormat::ARROW);
}
