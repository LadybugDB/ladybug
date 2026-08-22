#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "api_test/api_test.h"
#include "binder/binder.h"
#include "binder/bound_table_scan_info.h"
#include "binder/ddl/property_definition.h"
#include "binder/expression/variable_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/constants.h"
#include "common/partition_routing_hook.h"
#include "function/table/bind_data.h"
#include "function/table/simple_table_function.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"
#include <span>

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::main;
using namespace lbug::testing;
using namespace lbug::binder;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace {

// ---------------------------------------------------------------------------
// Mock distributed wrapper.
//
// Models the minimal behavior of a real wrapper: partitions are claimed for
// remote placement when their parent is provisioned remotely; writes are
// shipped to an in-memory sink; reads are served through a wrapper-registered
// table function exposed via a foreign-backed catalog entry.
// ---------------------------------------------------------------------------

struct RemoteRow {
    int64_t id;
    int64_t amount;
};

struct MockWrapper {
    // When true, provisioning notifications claim the parent for remote placement.
    bool provisionRemotely = false;
    std::unordered_set<table_id_t> claimedParents;
    // Force-claims a single partition of a parent (to produce local/remote mixes).
    std::optional<std::pair<table_id_t, uint64_t>> forcedClaim;
    std::vector<std::string> createdRefs;
    std::vector<std::string> droppedRefs;
    std::vector<RemoteRow> pointRows; // captured via insertRow
    std::vector<RemoteRow> chunkRows; // captured via insertChunk
    PartitionScanSpec scanSpec;
    TableFunction ownedScanFunction;

    void reset() {
        provisionRemotely = false;
        claimedParents.clear();
        forcedClaim.reset();
        createdRefs.clear();
        droppedRefs.clear();
        pointRows.clear();
        chunkRows.clear();
        scanSpec = PartitionScanSpec{};
        ownedScanFunction = TableFunction{};
    }

    std::vector<RemoteRow> allRows() const {
        std::vector<RemoteRow> combined = pointRows;
        combined.insert(combined.end(), chunkRows.begin(), chunkRows.end());
        return combined;
    }
};

MockWrapper mock;

std::string refString(PartitionRef ref) {
    return std::format("{}:{}", ref.parentTableID, ref.partitionIndex);
}

bool locateHook(void* /*context*/, PartitionRef ref, PartitionHandle* handleOut) {
    // Any stable non-null marker stands in for a connection/host descriptor.
    *handleOut = reinterpret_cast<void*>(static_cast<uintptr_t>(0xC0FFEE));
    if (mock.claimedParents.contains(ref.parentTableID)) {
        return true;
    }
    if (mock.forcedClaim.has_value() && mock.forcedClaim->first == ref.parentTableID &&
        mock.forcedClaim->second == ref.partitionIndex) {
        return true;
    }
    return false;
}

void onPartitionCreateHook(void* /*context*/, PartitionRef ref, PartitionHandle /*handle*/) {
    mock.createdRefs.push_back(refString(ref));
    if (mock.provisionRemotely) {
        mock.claimedParents.insert(ref.parentTableID);
    }
}

void onPartitionDropHook(void* /*context*/, PartitionRef ref, PartitionHandle /*handle*/) {
    mock.droppedRefs.push_back(refString(ref));
}

// Point-write sink. Column vectors hold exactly one evaluated row in schema order.
nodeID_t insertRowHook(void* /*context*/, PartitionRef ref, PartitionHandle /*handle*/,
    Transaction* /*tx*/, const ValueVector* keyVector,
    std::span<ValueVector* const> columnVectors) {
    const auto idPos = columnVectors[0]->state->getSelVector()[0];
    const auto keySel = keyVector->state->getSelVector()[0];
    mock.pointRows.push_back(
        {columnVectors[0]->getValue<int64_t>(idPos), keyVector->getValue<int64_t>(keySel)});
    return {static_cast<offset_t>(mock.pointRows.size() - 1), ref.parentTableID};
}

// Bulk-write sink. Row j of the run lives at selection position [startRow + j].
void insertChunkHook(void* /*context*/, PartitionRef /*ref*/, PartitionHandle /*handle*/,
    Transaction* /*tx*/, const ValueVector* keyVector, std::span<ValueVector* const> columnVectors,
    uint64_t startRow, uint64_t numRows) {
    const auto& keySel = keyVector->state->getSelVector();
    const auto& idSel = columnVectors[0]->state->getSelVector();
    for (uint64_t j = 0; j < numRows; ++j) {
        mock.chunkRows.push_back({columnVectors[0]->getValue<int64_t>(idSel[startRow + j]),
            keyVector->getValue<int64_t>(keySel[startRow + j])});
    }
}

// Read path: serve the captured rows through a wrapper-owned table function.
// Column layout: [rowid, id, amount].
offset_t remoteScanInternalFunc(const TableFuncMorsel& morsel, const TableFuncInput& /*input*/,
    DataChunk& output) {
    if (!morsel.hasMoreToOutput()) {
        return 0;
    }
    const auto rows = mock.allRows();
    for (auto i = 0u; i < morsel.getMorselSize(); ++i) {
        output.getValueVectorMutable(0).setValue(i, (int64_t)(morsel.startOffset + i));
        output.getValueVectorMutable(1).setValue(i, rows[morsel.startOffset + i].id);
        output.getValueVectorMutable(2).setValue(i, rows[morsel.startOffset + i].amount);
    }
    return morsel.getMorselSize();
}

expression_vector remoteScanColumns(const std::string& nodeUniqueName) {
    expression_vector columns;
    columns.push_back(std::make_shared<VariableExpression>(LogicalType::INT64(),
        nodeUniqueName + "." + std::string(InternalKeyword::ID), "rowid"));
    columns.push_back(
        std::make_shared<VariableExpression>(LogicalType::INT64(), nodeUniqueName + ".id", "id"));
    columns.push_back(std::make_shared<VariableExpression>(LogicalType::INT64(),
        nodeUniqueName + ".amount", "amount"));
    return columns;
}

std::unique_ptr<TableFuncBindData> remoteScanBindFunc(const ClientContext* /*context*/,
    const TableFuncBindInput* input) {
    std::vector<std::string> names{"rowid", "id", "amount"};
    std::vector<LogicalType> types;
    types.emplace_back(LogicalType::INT64());
    types.emplace_back(LogicalType::INT64());
    types.emplace_back(LogicalType::INT64());
    names = TableFunction::extractYieldVariables(names, input->yieldVariables);
    auto columns = input->binder->createVariables(names, types);
    return std::make_unique<TableFuncBindData>(std::move(columns), mock.allRows().size());
}

TableFunction makeRemoteScanFunction() {
    TableFunction func("test_partition_remote_scan", std::vector<LogicalTypeID>{});
    func.tableFunc = SimpleTableFunc::getTableFunc(remoteScanInternalFunc);
    func.bindFunc = remoteScanBindFunc;
    func.initSharedStateFunc = SimpleTableFunc::initSharedState;
    func.initLocalStateFunc = TableFunction::initEmptyLocalState;
    return func;
}

// Register the wrapper's scan function in this database and keep a copy for bindScan to
// hand out. The engine attaches it to clones of the partition's own catalog entries.
void setupRemoteScan(Connection* con) {
    auto* context = con->getClientContext();
    auto catalog = Catalog::Get(*context);
    // Function/entry registration happens outside any active query, so use the dummy
    // transaction like core bootstrap code does.
    auto* transaction = &DUMMY_CHECKPOINT_TRANSACTION;

    if (!catalog->containsFunction(transaction, "test_partition_remote_scan")) {
        function_set fs;
        fs.push_back(std::make_unique<TableFunction>(makeRemoteScanFunction()));
        catalog->addFunction(transaction, CatalogEntryType::TABLE_FUNCTION_ENTRY,
            "test_partition_remote_scan", std::move(fs), true /* isInternal */);
    }

    if (mock.scanSpec.scanFunction != nullptr) {
        return;
    }
    mock.ownedScanFunction = makeRemoteScanFunction();
    mock.scanSpec.scanFunction = &mock.ownedScanFunction;
    mock.scanSpec.createBindData = [](const std::string& nodeUniqueName) {
        return std::make_unique<TableFuncBindData>(remoteScanColumns(nodeUniqueName),
            mock.allRows().size());
    };
}

bool bindScanHook(void* /*context*/, PartitionRef /*ref*/, PartitionHandle /*handle*/,
    PartitionScanSpec* specOut) {
    *specOut = mock.scanSpec;
    return true;
}

struct HooksGuard {
    HooksGuard() { setupHooks(); }
    ~HooksGuard() {
        setPartitionRoutingHooks(nullptr);
        mock.reset();
    }
    // Must outlive the registration: the registry keeps the raw pointer.
    static PartitionRoutingHooks& hookStruct() {
        static PartitionRoutingHooks hooks;
        return hooks;
    }
    static void setupHooks() {
        auto& hooks = hookStruct();
        hooks.context = &mock;
        hooks.locate = locateHook;
        hooks.onPartitionCreate = onPartitionCreateHook;
        hooks.onPartitionDrop = onPartitionDropHook;
        hooks.bindScan = bindScanHook;
        hooks.insertRow = insertRowHook;
        hooks.insertChunk = insertChunkHook;
        setPartitionRoutingHooks(&hooks);
    }
};

table_id_t getTableID(Connection* con, const std::string& name) {
    auto* context = con->getClientContext();
    return Catalog::Get(*context)
        ->getTableCatalogEntry(&DUMMY_CHECKPOINT_TRANSACTION, name)
        ->getTableID();
}

bool hasLocalStorage(Connection* con, table_id_t tableID) {
    return StorageManager::Get(*con->getClientContext())->containsTable(tableID);
}

std::string sortLines(std::string s) {
    std::vector<std::string> lines;
    std::istringstream iss{s};
    for (std::string line; std::getline(iss, line);) {
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream oss;
    for (const auto& line : lines) {
        oss << line << '\n';
    }
    return oss.str();
}

} // namespace

class PartitionRoutingTest : public ApiTest {
    void SetUp() override {
        ApiTest::SetUp();
        setupRemoteScan(conn.get());
    }
};

TEST_F(PartitionRoutingTest, LifecycleAndStorageSkip) {
    HooksGuard guard;

    // Unclaimed partitioned table: fully local, unchanged behavior.
    ASSERT_TRUE(conn->query("CREATE NODE TABLE LocalP (id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    ASSERT_EQ(mock.createdRefs.size(), 2u);

    // Claimed partitioned table: catalog metadata stays local, storage is skipped and the
    // wrapper sees provisioning notifications.
    mock.createdRefs.clear();
    mock.provisionRemotely = true;
    ASSERT_TRUE(conn->query("CREATE NODE TABLE RemoteP (id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 3;")
                    ->isSuccess());
    mock.provisionRemotely = false;
    ASSERT_EQ(mock.createdRefs.size(), 3u);

    const auto remoteParentID = getTableID(conn.get(), "RemoteP");
    for (auto i = 0u; i < 3; ++i) {
        EXPECT_EQ(mock.createdRefs[i], refString(PartitionRef{remoteParentID, i}));
    }

    // Catalog keeps full metadata for every partition...
    for (auto i = 0u; i < 3; ++i) {
        ASSERT_NO_THROW(getTableID(conn.get(), std::format("RemoteP_p{}", i)));
    }
    // ...but no local storage exists for claimed partitions.
    for (auto i = 0u; i < 3; ++i) {
        const auto childID = getTableID(conn.get(), std::format("RemoteP_p{}", i));
        EXPECT_FALSE(hasLocalStorage(conn.get(), childID))
            << "partition " << i << " should have no local storage";
    }
    // The unclaimed sibling owns local storage as usual.
    EXPECT_TRUE(hasLocalStorage(conn.get(), getTableID(conn.get(), "LocalP_p0")));

    // Dropping the parent notifies the wrapper for every partition subgraph.
    ASSERT_TRUE(conn->query("DROP TABLE RemoteP;")->isSuccess());
    ASSERT_EQ(mock.droppedRefs.size(), 3u);
    for (auto i = 0u; i < 3; ++i) {
        EXPECT_EQ(mock.droppedRefs[i], refString(PartitionRef{remoteParentID, i}));
    }
}

TEST_F(PartitionRoutingTest, FullyRemotePointInsertAndScan) {
    HooksGuard guard;

    mock.provisionRemotely = true;
    ASSERT_TRUE(conn->query("CREATE NODE TABLE RP (id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 3;")
                    ->isSuccess());
    mock.provisionRemotely = false;

    // Point inserts route to the wrapper, which assigns node IDs.
    for (auto id = 1; id <= 3; ++id) {
        ASSERT_TRUE(conn->query(std::format("CREATE (:RP {{id: {}, amount: {}}});", id, id * 10))
                        ->isSuccess())
            << std::format("insert {}", id);
    }
    ASSERT_EQ(mock.pointRows.size(), 3u);

    // Reads are served by the wrapper's consolidated scan entry.
    auto result = conn->query("MATCH (n:RP) RETURN n.id, n.amount ORDER BY n.id;");
    ASSERT_TRUE(result->isSuccess()) << result->toString();
    ASSERT_EQ(result->getNumTuples(), 3u);
    EXPECT_EQ(sortLines(result->toString()), "1|10\n2|20\n3|30\nn.id|n.amount\n");
}

TEST_F(PartitionRoutingTest, FullyRemoteBulkInsert) {
    HooksGuard guard;

    mock.provisionRemotely = true;
    ASSERT_TRUE(conn->query("CREATE NODE TABLE RP (id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 3;")
                    ->isSuccess());
    mock.provisionRemotely = false;

    const auto csvPath =
        TestHelper::appendLbugRootPath("test/test_files/partition_routing/routing_bulk.csv");
    auto copyResult = conn->query(std::format("COPY RP FROM '{}';", csvPath));
    ASSERT_TRUE(copyResult->isSuccess()) << copyResult->toString();
    ASSERT_EQ(mock.chunkRows.size(), 4u);

    auto result = conn->query("MATCH (n:RP) RETURN count(*) AS c;");
    ASSERT_TRUE(result->isSuccess()) << result->toString();
    ASSERT_EQ(result->getNumTuples(), 1u);
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 4);
}

TEST_F(PartitionRoutingTest, MixedLocalRemoteScanRejected) {
    HooksGuard guard;

    // Created while nothing is claimed: all partitions are local.
    ASSERT_TRUE(conn->query("CREATE NODE TABLE MP (id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:MP {id: 1, amount: 10});")->isSuccess());

    // Now claim exactly one partition behind the engine's back (models a wrapper that only
    // owns part of a table). Scanning such a parent cannot be planned and must be rejected
    // at bind time rather than silently returning wrong results.
    mock.forcedClaim = {getTableID(conn.get(), "MP"), 1};
    auto result = conn->query("MATCH (n:MP) RETURN n.id;");
    ASSERT_FALSE(result->isSuccess());
    EXPECT_NE(result->toString().find("mix of locally stored and remotely routed"),
        std::string::npos)
        << result->toString();

    // Writes still route correctly under partial claims: each row lands either locally or at
    // the wrapper according to the partition function.
    mock.forcedClaim.reset();
    ASSERT_TRUE(conn->query("CREATE (:MP {id: 2, amount: 20});")->isSuccess());
    auto result2 = conn->query("MATCH (n:MP) RETURN n.id ORDER BY n.id;");
    ASSERT_TRUE(result2->isSuccess()) << result2->toString();
    ASSERT_EQ(result2->getNumTuples(), 2u);
}
