#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/types/timestamp_t.h"
#include "common/types/uuid.h"
#include "graph_test/private_graph_test.h"
#include "planner/operator/logical_plan.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "test_runner/test_runner.h"
#include "transaction/transaction.h"

namespace lbug {
namespace testing {

static constexpr auto UUID_A = "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11";
static constexpr auto UUID_A_UPPER = "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11";
static constexpr auto UUID_B = "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a12";
static constexpr auto UUID_ZERO = "00000000-0000-0000-0000-000000000000";
static constexpr auto UUID_MAX = "ffffffff-ffff-ffff-ffff-ffffffffffff";

static std::string uuidLiteral(const char* value) {
    return "UUID('" + std::string{value} + "')";
}

class PartitionPruningTest : public EmptyDBTest {
public:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }

    void run(const std::string& query) {
        auto result = conn->query(query);
        if (!result->isSuccess()) {
            throw std::runtime_error(query + "\n" + result->getErrorMessage());
        }
    }

    static std::vector<int64_t> ids(main::QueryResult& result) {
        if (!result.isSuccess()) {
            throw std::runtime_error(result.getErrorMessage());
        }
        std::vector<int64_t> values;
        while (result.hasNext()) {
            values.push_back(result.getNext()->getValue(0)->getValue<int64_t>());
        }
        std::sort(values.begin(), values.end());
        return values;
    }

    std::vector<int64_t> ids(const std::string& query) { return ids(*conn->query(query)); }

    std::vector<std::string> tables(const std::string& query) {
        run("BEGIN TRANSACTION READ ONLY");
        auto prepared = conn->prepare(query);
        if (!prepared->isSuccess()) {
            throw std::runtime_error(query + "\n" + prepared->getErrorMessage());
        }
        const auto* cached =
            conn->getClientContext()->getCachedPreparedStatementManager().getCachedStatement(
                prepared->getName());
        std::vector<std::string> names;
        std::function<void(planner::LogicalOperator*)> visit = [&](auto* op) {
            if (op->getOperatorType() == planner::LogicalOperatorType::SCAN_NODE_TABLE) {
                const auto& scan = op->template constCast<planner::LogicalScanNodeTable>();
                for (auto id : scan.getTableIDs()) {
                    names.push_back(
                        database->getCatalog()
                            ->getTableCatalogEntry(
                                transaction::Transaction::Get(*conn->getClientContext()), id)
                            ->getName());
                }
            }
            for (auto i = 0u; i < op->getNumChildren(); ++i) {
                visit(op->getChild(i).get());
            }
        };
        visit(cached->logicalPlan->getLastOperator().get());
        run("COMMIT");
        std::sort(names.begin(), names.end());
        return names;
    }

    void hashTable() {
        run("CREATE NODE TABLE H(id INT64 PRIMARY KEY) PARTITION BY HASH(id) PARTITIONS 4");
        run("UNWIND range(1,8) AS i CREATE (:H {id:i})");
    }

    void listTable() {
        run("CREATE NODE TABLE L(id INT64 PRIMARY KEY, k STRING) PARTITION BY LIST(k)");
        run("CREATE (:L {id:1,k:'east'})");
        run("CREATE (:L {id:2,k:'west'})");
        run("CREATE (:L {id:3,k:'east'})");
    }

    void uuidListTable() {
        run("CREATE NODE TABLE U(id INT64 PRIMARY KEY, k UUID) PARTITION BY LIST(k)");
        run("CREATE (:U {id:1,k:" + uuidLiteral(UUID_A) + "})");
        run("CREATE (:U {id:2,k:" + uuidLiteral(UUID_B) + "})");
        run("CREATE (:U {id:3,k:" + uuidLiteral(UUID_A_UPPER) + "})");
    }

    void rejected(const std::string& query) {
        auto result = conn->query(query);
        ASSERT_FALSE(result->isSuccess()) << query;
        EXPECT_NE(result->getErrorMessage().find("Partition key violates"), std::string::npos)
            << result->getErrorMessage();
    }
};

TEST_F(PartitionPruningTest, PartitionDDLRecreatesTheParentRule) {
    const auto parent = [&]() {
        return database->getCatalog()
            ->getTableCatalogEntry(&transaction::DUMMY_CHECKPOINT_TRANSACTION, "P")
            ->constPtrCast<catalog::NodeTableCatalogEntry>();
    };
    for (const auto& rule : {"HASH(k) PARTITIONS 3", "LIST(k)", "RANGE(k) INTERVAL '1 day'"}) {
        run(std::string{"CREATE NODE TABLE P(id INT64 PRIMARY KEY, k DATE) PARTITION BY "} + rule);
        const auto method = parent()->getPartitionMethod();
        const auto numChildren = parent()->getChildTableIDs().size();
        const auto ddl = parent()->toCypher(catalog::ToCypherInfo{});
        run("DROP TABLE P");
        run(ddl);
        EXPECT_EQ(parent()->getPartitionMethod(), method);
        EXPECT_EQ(parent()->getPartitionColumnName(), "k");
        EXPECT_EQ(parent()->getChildTableIDs().size(), numChildren);
        run("DROP TABLE P");
    }
}

TEST_F(PartitionPruningTest, HashEqualityAndPrimaryKeyLookup) {
    hashTable();
    for (const auto& predicate : {"n.id=2", "2=n.id", "n.id=2 AND n.id>0"}) {
        const auto query = std::string("MATCH(n:H) WHERE ") + predicate + " RETURN n.id";
        EXPECT_EQ(tables(query), std::vector<std::string>{"H_p2"});
        EXPECT_EQ(ids(query), std::vector<int64_t>{2});
        EXPECT_NE(conn->query("EXPLAIN " + query)->toString().find("PRIMARY_KEY_SCAN"),
            std::string::npos);
        EXPECT_NE(conn->query("EXPLAIN " + query)->toString().find("Tables: H_p2"),
            std::string::npos);
    }
    run("CALL enable_zone_map=false");
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id=2 RETURN n.id"), std::vector<std::string>{"H_p2"});
    EXPECT_EQ(ids("MATCH(n:H) WHERE n.id=2 RETURN n.id"), std::vector<int64_t>{2});
}

TEST_F(PartitionPruningTest, ListEqualityAndMissingKey) {
    listTable();
    const std::string query = "MATCH(n:L) WHERE n.k='east' RETURN n.id";
    EXPECT_EQ(tables(query), std::vector<std::string>{"L_p1"});
    EXPECT_EQ(ids(query), (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='east' AND n.id>1 RETURN n.id"), std::vector<int64_t>{3});
    EXPECT_TRUE(tables("MATCH(n:L) WHERE n.k='missing' RETURN n.id").empty());
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='missing' RETURN count(*)"), std::vector<int64_t>{0});
    EXPECT_TRUE(ids("MATCH(n:L) WHERE n.k='east' AND n.k='west' RETURN n.id").empty());
}

TEST_F(PartitionPruningTest, TypedKeysUseTheWriteHashAndEncoding) {
    run("CREATE NODE TABLE S(id INT64 PRIMARY KEY, k STRING) PARTITION BY HASH(k) PARTITIONS 4");
    run("CREATE (:S {id:1,k:'east'})");
    run("CREATE (:S {id:2,k:'west'})");
    EXPECT_EQ(tables("MATCH(n:S) WHERE n.k='east' RETURN n.id"), std::vector<std::string>{"S_p1"});
    EXPECT_EQ(ids("MATCH(n:S) WHERE n.k='east' RETURN n.id"), std::vector<int64_t>{1});
    run("CREATE NODE TABLE I(id INT64 PRIMARY KEY, k INT32) PARTITION BY LIST(k)");
    run("CREATE (:I {id:1,k:2})");
    run("CREATE (:I {id:2,k:3})");
    const auto integer = "MATCH(n:I) WHERE n.k=CAST(2 AS INT32) RETURN n.id";
    EXPECT_EQ(tables(integer), std::vector<std::string>{"I_p1"});
    EXPECT_EQ(ids(integer), std::vector<int64_t>{1});
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) PARTITION BY LIST(k)");
    run("CREATE (:D {id:1,k:date('2026-10-26')})");
    run("CREATE (:D {id:2,k:date('2026-10-27')})");
    const auto date = "MATCH(n:D) WHERE n.k=date('2026-10-26') RETURN n.id";
    EXPECT_EQ(tables(date), std::vector<std::string>{"D_p1"});
    EXPECT_EQ(ids(date), std::vector<int64_t>{1});
}

TEST_F(PartitionPruningTest, DailyMergeUpdatesTheNewlyRoutedNode) {
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE, v INT64) "
        "PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("MATCH(n:D) SET n.v=0");
    for (auto day : {1, 2, 3, 2}) {
        run(std::format("MERGE(n:D {{id:{0}, k:DATE('2024-01-0{0}')}}) "
                        "ON CREATE SET n.v={0} ON MATCH SET n.v=22",
            day));
    }
    EXPECT_EQ(ids("MATCH(n:D) RETURN 100*n.id+n.v"), (std::vector<int64_t>{101, 222, 303}));
    EXPECT_FALSE(conn->query("MERGE(n:D {id:2,k:DATE('2024-01-02')}) "
                             "ON MATCH SET n.k=DATE('2024-01-03')")
                     ->isSuccess());
}

TEST_F(PartitionPruningTest, DailyRoutesCreatesAndCopiesAndValidatesChildren) {
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k TIMESTAMP) "
        "PARTITION BY RANGE(k) INTERVAL '1 day'");
    EXPECT_TRUE(tables("MATCH(n:D) RETURN n.id").empty());
    EXPECT_EQ(ids("MATCH(n:D) RETURN count(*)"), std::vector<int64_t>{0});
    // COPY is the first writer. A late event subsequently creates an earlier day's partition.
    run("COPY D FROM (RETURN 1 AS id, TIMESTAMP('2026-11-22 00:00:00') AS k)");
    run("CREATE (:D {id:2,k:TIMESTAMP('2026-11-22 23:59:59.999999')})");
    run("COPY D FROM (UNWIND [3,4] AS i RETURN i AS id, CASE WHEN i=3 THEN "
        "TIMESTAMP('2026-11-20 19:00:00') ELSE TIMESTAMP('2026-11-22 12:00:00') END AS k)");
    run("MERGE (:D {id:5,k:TIMESTAMP('2026-11-23 00:00:00')})");
    EXPECT_EQ(ids("MATCH(n:D_p0) RETURN n.id"), (std::vector<int64_t>{1, 2, 4}));
    EXPECT_EQ(ids("MATCH(n:D_p1) RETURN n.id"), std::vector<int64_t>{3});
    EXPECT_EQ(ids("MATCH(n:D_p2) RETURN n.id"), std::vector<int64_t>{5});
    rejected("CREATE (:D_p0 {id:6,k:TIMESTAMP('2026-11-21 12:00:00')})");
    rejected("COPY D_p0 FROM (UNWIND [6,7] AS i RETURN i AS id, CASE WHEN i=6 THEN "
             "TIMESTAMP('2026-11-22 00:00:00') ELSE TIMESTAMP('2026-11-23 00:00:00') END AS k)");
    EXPECT_FALSE(conn->query("CREATE (:D {id:6,k:NULL})")->isSuccess());
    EXPECT_FALSE(
        conn->query("COPY D FROM (RETURN 6 AS id, CAST(NULL AS TIMESTAMP) AS k)")->isSuccess());
    EXPECT_FALSE(
        conn->query("MATCH(n:D_p0) SET n.k=TIMESTAMP('2026-11-24 00:00:00')")->isSuccess());
    EXPECT_EQ(ids("MATCH(n:D) RETURN n.id"), (std::vector<int64_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(tables("MATCH(n:D) RETURN n.id").size(), 3u);
}

TEST_F(PartitionPruningTest, DailyEmptyScanWithoutPlanOptimizer) {
    run("CALL enable_plan_optimizer=false");
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("CREATE NODE TABLE Plain(id INT64 PRIMARY KEY, k DATE)");
    run("CREATE (:Plain {id:8,k:DATE('2024-02-29')})");
    EXPECT_EQ(ids("MATCH(n:D) RETURN count(*)"), std::vector<int64_t>{0});
    EXPECT_TRUE(ids("MATCH(n:D) RETURN n.id").empty());
    EXPECT_EQ(ids("MATCH(n:Plain:D) WHERE n.k=DATE('2024-02-29') RETURN n.id"),
        std::vector<int64_t>{8});
    EXPECT_EQ(ids("MATCH(n:Plain) OPTIONAL MATCH(d:D) WHERE n.k=d.k "
                  "RETURN n.id + CASE WHEN d.id IS NULL THEN 0 ELSE 100 END"),
        std::vector<int64_t>{8});
    run("MERGE (:D {id:9,k:DATE('2024-02-29')})");
    EXPECT_EQ(ids("MATCH(n:D) RETURN n.id"), std::vector<int64_t>{9});
}

TEST_F(PartitionPruningTest, DailyCorrelatedLookupBeforeAndAfterFirstWrite) {
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("CREATE NODE TABLE A(id INT64 PRIMARY KEY)");
    run("CREATE (:A {id:1})");
    const auto optional = "MATCH(a:A) OPTIONAL MATCH(d:D) WHERE d.id=a.id "
                          "RETURN a.id + CASE WHEN d.id IS NULL THEN 0 ELSE 100 END";
    const auto lookup = "UNWIND [1] AS i MATCH(d:D) WHERE d.id=i RETURN d.id";
    EXPECT_EQ(ids(optional), std::vector<int64_t>{1});
    EXPECT_TRUE(ids(lookup).empty());
    run("CREATE (:D {id:1,k:DATE('2024-02-29')})");
    EXPECT_EQ(ids(optional), std::vector<int64_t>{101});
    EXPECT_EQ(ids(lookup), std::vector<int64_t>{1});
}

TEST_F(PartitionPruningTest, DailyPrunesOnlyDaysIntersectingThePredicate) {
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k TIMESTAMP) "
        "PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("CREATE (:D {id:1,k:TIMESTAMP('1969-12-31 23:59:59.999999')})");
    run("CREATE (:D {id:2,k:TIMESTAMP('1970-01-01 00:00:00')})");
    run("CREATE (:D {id:3,k:TIMESTAMP('1970-01-01 12:00:00')})");
    run("CREATE (:D {id:4,k:TIMESTAMP('1970-01-03 00:00:00')})");
    auto check = [&](std::string predicate, std::vector<std::string> expectedTables,
                     std::vector<int64_t> expectedRows) {
        SCOPED_TRACE(predicate);
        const auto query = "MATCH(n:D) WHERE " + predicate + " RETURN n.id";
        EXPECT_EQ(tables(query), expectedTables);
        EXPECT_EQ(ids(query), expectedRows);
    };
    check("n.k < TIMESTAMP('1970-01-01 00:00:00')", {"D_p0"}, {1});
    check("TIMESTAMP('1970-01-01 00:00:00') >= n.k", {"D_p0", "D_p1"}, {1, 2});
    check("TIMESTAMP('1969-12-31 23:59:59.999999') < n.k", {"D_p1", "D_p2"}, {2, 3, 4});
    check("TIMESTAMP('1970-01-01 00:00:00') <= n.k AND n.k < TIMESTAMP('1970-01-02 00:00:00')",
        {"D_p1"}, {2, 3});
    check("n.k = TIMESTAMP('1970-01-01 12:00:00')", {"D_p1"}, {3});
    check("n.k = TIMESTAMP('1970-01-02 00:00:00')", {}, {});
    check("n.k >= TIMESTAMP('1970-01-01 12:00:00') AND n.k < TIMESTAMP('1970-01-01 12:00:00')", {},
        {});
    check("n.id = 2", {"D_p0", "D_p1", "D_p2"}, {2});
    check("n.id = 1 OR n.k = TIMESTAMP('1970-01-03 00:00:00')", {"D_p0", "D_p1", "D_p2"}, {1, 4});
    run("CREATE NODE TABLE Plain(id INT64 PRIMARY KEY, k TIMESTAMP)");
    run("CREATE (:Plain {id:2,k:TIMESTAMP('1970-01-01 00:00:00')})");
    const auto mixed = "MATCH(n:D:Plain) WHERE n.k=TIMESTAMP('1970-01-01 00:00:00') RETURN n.id";
    EXPECT_EQ(tables(mixed), (std::vector<std::string>{"D_p1", "Plain"}));
    EXPECT_EQ(ids(mixed), (std::vector<int64_t>{2, 2}));
    auto plan = conn->query("EXPLAIN MATCH(n:D) WHERE n.id=2 AND "
                            "n.k=TIMESTAMP('1970-01-01 00:00:00') RETURN n.id");
    ASSERT_TRUE(plan->isSuccess()) << plan->getErrorMessage();
    EXPECT_NE(plan->toString().find("PRIMARY_KEY_SCAN_NODE_TABLE"), std::string::npos);
}

TEST_F(PartitionPruningTest, DailyUsesNativePrecisionAndUTCDates) {
    for (const std::string type : {"TIMESTAMP_SEC", "TIMESTAMP_MS", "TIMESTAMP_NS"}) {
        SCOPED_TRACE(type);
        const auto table = "D_" + type;
        run("CREATE NODE TABLE " + table + "(id INT64 PRIMARY KEY, k " + type +
            ") PARTITION BY RANGE(k) INTERVAL '1 day'");
        auto cast = [&](const char* time) {
            return "CAST('" + std::string{time} + "' AS " + type + ")";
        };
        run("CREATE (:" + table + " {id:1,k:" + cast("1969-12-31 23:59:59") + "})");
        run("CREATE (:" + table + " {id:2,k:" + cast("1970-01-01 00:00:00") + "})");
        const auto query =
            "MATCH(n:" + table + ") WHERE n.k<" + cast("1970-01-01 00:00:00") + " RETURN n.id";
        EXPECT_EQ(tables(query), std::vector<std::string>{table + "_p0"});
        EXPECT_EQ(ids(query), std::vector<int64_t>{1});
    }
    run("CREATE NODE TABLE Z(id INT64 PRIMARY KEY, k TIMESTAMP_TZ) "
        "PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("CREATE (:Z {id:1,k:CAST('2026-11-23 00:00:00+08:00' AS TIMESTAMP_TZ)})");
    run("CREATE (:Z {id:2,k:CAST('2026-11-22 16:00:00Z' AS TIMESTAMP_TZ)})");
    run("CREATE (:Z {id:3,k:CAST('2026-11-23 00:00:00Z' AS TIMESTAMP_TZ)})");
    const auto utcDay = "MATCH(n:Z) WHERE n.k<CAST('2026-11-23 00:00:00Z' AS TIMESTAMP_TZ) "
                        "RETURN n.id";
    EXPECT_EQ(tables(utcDay), std::vector<std::string>{"Z_p0"});
    EXPECT_EQ(ids(utcDay), (std::vector<int64_t>{1, 2}));

    // Use native parameters so nanosecond bounds never round through the timestamp text parser.
    auto atExtreme = [&](const std::string& op, int64_t tick) {
        auto prepared = conn->prepare("MATCH(n:D_TIMESTAMP_NS) WHERE n.k" + op + "$t RETURN n.id");
        if (!prepared->isSuccess()) {
            throw std::runtime_error(prepared->getErrorMessage());
        }
        std::unordered_map<std::string, std::unique_ptr<common::Value>> params;
        params.emplace("t", std::make_unique<common::Value>(common::timestamp_ns_t{tick}));
        return ids(*conn->executeWithParams(prepared.get(), std::move(params)));
    };
    EXPECT_TRUE(atExtreme("<", std::numeric_limits<int64_t>::min()).empty());
    EXPECT_TRUE(atExtreme(">", std::numeric_limits<int64_t>::max()).empty());
    EXPECT_EQ(atExtreme(">=", -1), std::vector<int64_t>{2});
}

TEST_F(PartitionPruningTest, DailyPreparedBoundsAndNewDaysRespectTransactions) {
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) PARTITION BY RANGE(k) INTERVAL '1 day'");
    auto prepared = conn->prepare("MATCH(n:D) WHERE n.k >= $day RETURN n.id");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    auto execute = [&](int day) {
        return ids(*conn->execute(prepared.get(),
            std::make_pair(std::string{"day"}, common::Date::fromDate(2024, 2, day))));
    };
    EXPECT_TRUE(execute(28).empty());
    run("MERGE (:D {id:1,k:DATE('2024-02-28')})");
    EXPECT_EQ(execute(28), std::vector<int64_t>{1});
    EXPECT_TRUE(execute(29).empty());
    run("BEGIN TRANSACTION");
    run("CREATE (:D {id:2,k:DATE('2024-02-29')})");
    EXPECT_EQ(execute(29), std::vector<int64_t>{2});
    run("ROLLBACK");
    EXPECT_TRUE(execute(29).empty());
    main::Connection reader{database.get()};
    ASSERT_TRUE(reader.query("BEGIN TRANSACTION READ ONLY")->isSuccess());
    const auto on29 = "MATCH(n:D) WHERE n.k=DATE('2024-02-29') RETURN n.id";
    EXPECT_TRUE(ids(*reader.query(on29)).empty());
    run("CREATE (:D {id:3,k:DATE('2024-02-29')})");
    EXPECT_TRUE(ids(*reader.query(on29)).empty());
    // A statement prepared on this snapshot after the concurrent commit still sees no day.
    auto oldSnapshot = reader.prepare(on29);
    ASSERT_TRUE(oldSnapshot->isSuccess()) << oldSnapshot->getErrorMessage();
    EXPECT_TRUE(ids(*reader.execute(oldSnapshot.get())).empty());
    ASSERT_TRUE(reader.query("COMMIT")->isSuccess());
    EXPECT_EQ(ids(*reader.execute(oldSnapshot.get())), std::vector<int64_t>{3});
    EXPECT_EQ(execute(28), (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(execute(29), std::vector<int64_t>{3});
    EXPECT_EQ(tables("MATCH(n:D) WHERE n.k>DATE('2024-02-28') RETURN n.id"),
        std::vector<std::string>{"D_p1"});
}

TEST_F(PartitionPruningTest, ConcurrentFirstWriteToTheSameDay) {
    run("CALL enable_multi_writes=true");
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) PARTITION BY RANGE(k) INTERVAL '1 day'");
    const auto secondRow = "CREATE (:D {id:2,k:DATE('2024-02-29')})";
    run("BEGIN TRANSACTION");
    {
        main::Connection other{database.get()};
        ASSERT_TRUE(other.query("BEGIN TRANSACTION")->isSuccess());
        run("CREATE (:D {id:1,k:DATE('2024-02-29')})");
        auto conflict = other.query(secondRow);
        ASSERT_FALSE(conflict->isSuccess());
        EXPECT_NE(conflict->getErrorMessage().find("Write-write conflict"), std::string::npos);
    }
    run("COMMIT");
    run(secondRow);
    EXPECT_EQ(tables("MATCH(n:D) RETURN n.id"), std::vector<std::string>{"D_p0"});
    EXPECT_EQ(ids("MATCH(n:D_p0) RETURN n.id"), (std::vector<int64_t>{1, 2}));
}

TEST_F(PartitionPruningTest, DailyCheckpointReopenReusesDays) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("CHECKPOINT");
    conn.reset();
    database.reset();
    createDBAndConn();
    EXPECT_EQ(ids("MATCH(n:D) RETURN count(*)"), std::vector<int64_t>{0});
    run("COPY D FROM (UNWIND [1,2] AS i RETURN i AS id, CASE WHEN i=1 THEN DATE('2026-11-22') "
        "ELSE DATE('2026-11-20') END AS k)");
    run("CHECKPOINT");
    conn.reset();
    database.reset();
    createDBAndConn();
    run("CREATE (:D {id:3,k:DATE('2026-11-22')})");
    run("COPY D FROM (RETURN 4 AS id, DATE('2026-11-23') AS k)");
    const auto query = "MATCH(n:D) WHERE n.k>=DATE('2026-11-22') RETURN n.id";
    EXPECT_EQ(tables(query), (std::vector<std::string>{"D_p0", "D_p2"}));
    EXPECT_EQ(ids(query), (std::vector<int64_t>{1, 3, 4}));
    EXPECT_EQ(ids("MATCH(n:D_p1) RETURN n.id"), std::vector<int64_t>{2});
}

TEST_F(PartitionPruningTest, UUIDListCreateCopyAndEqualityPruning) {
    uuidListTable();
    // COPY both previously seen keys and new keys, including UUIDs containing zero bytes.
    run("COPY U FROM (UNWIND [4,5,6,7] AS i RETURN i AS id, "
        "CASE i WHEN 4 THEN " +
        uuidLiteral(UUID_A_UPPER) + " WHEN 5 THEN " + uuidLiteral(UUID_B) + " WHEN 6 THEN " +
        uuidLiteral(UUID_ZERO) + " ELSE " + uuidLiteral(UUID_MAX) + " END AS k)");
    const std::vector<std::pair<const char*, std::vector<int64_t>>> expected{{UUID_A, {1, 3, 4}},
        {UUID_B, {2, 5}}, {UUID_ZERO, {6}}, {UUID_MAX, {7}}};
    for (auto i = 0u; i < expected.size(); ++i) {
        const auto& [key, rows] = expected[i];
        for (const auto& predicate : {"n.k=" + uuidLiteral(key), uuidLiteral(key) + "=n.k"}) {
            const auto query = "MATCH(n:U) WHERE " + predicate + " RETURN n.id";
            EXPECT_EQ(tables(query), std::vector<std::string>{"U_p" + std::to_string(i + 1)});
            EXPECT_EQ(ids(query), rows);
        }
    }
    EXPECT_EQ(ids("MATCH(n:U_p0) RETURN count(*)"), std::vector<int64_t>{0});
    EXPECT_EQ(tables("MATCH(n:U) RETURN n.id").size(), 5u);
    const auto missing = "MATCH(n:U) WHERE n.k=UUID('00000000-0000-0000-0000-000000000001') "
                         "RETURN n.id";
    EXPECT_TRUE(tables(missing).empty());
    EXPECT_TRUE(ids(missing).empty());
}

TEST_F(PartitionPruningTest, UUIDListDirectChildWritesEnforceRouting) {
    uuidListTable();
    rejected("CREATE (:U_p0 {id:9,k:" + uuidLiteral(UUID_A) + "})");
    rejected("CREATE (:U_p1 {id:9,k:" + uuidLiteral(UUID_B) + "})");
    rejected("MERGE (:U_p1 {id:9,k:" + uuidLiteral(UUID_B) + "})");
    rejected("COPY U_p1 FROM (UNWIND [9,10] AS i RETURN i AS id, CASE WHEN i=9 THEN " +
             uuidLiteral(UUID_A) + " ELSE " + uuidLiteral(UUID_B) + " END AS k)");
    rejected("CREATE (:U_p1 {id:9,k:NULL})");
    for (const auto& query :
        {"CREATE (:U {id:9,k:NULL})", "COPY U FROM (RETURN 9 AS id, CAST(NULL AS UUID) AS k)"}) {
        auto result = conn->query(query);
        ASSERT_FALSE(result->isSuccess()) << query;
        EXPECT_NE(result->getErrorMessage().find("NULL"), std::string::npos)
            << result->getErrorMessage();
    }
    EXPECT_EQ(ids("MATCH(n:U) RETURN n.id"), (std::vector<int64_t>{1, 2, 3}));
    run("CREATE (:U_p1 {id:4,k:" + uuidLiteral(UUID_A_UPPER) + "})");
    run("COPY U_p1 FROM (RETURN 5 AS id, " + uuidLiteral(UUID_A) + " AS k)");
    run("MERGE (:U {id:6,k:" + uuidLiteral(UUID_B) + "})");
    EXPECT_EQ(ids("MATCH(n:U_p1) RETURN n.id"), (std::vector<int64_t>{1, 3, 4, 5}));
    EXPECT_EQ(ids("MATCH(n:U_p2) RETURN n.id"), (std::vector<int64_t>{2, 6}));
}

TEST_F(PartitionPruningTest, UUIDPartitionPrimaryKeyLookup) {
    for (const auto& method : {std::string{"LIST(k)"}, std::string{"HASH(k) PARTITIONS 4"}}) {
        const auto table = method.starts_with("LIST") ? "UL" : "UH";
        run("CREATE NODE TABLE " + std::string{table} +
            "(k UUID PRIMARY KEY, id INT64) PARTITION BY " + method);
        run("CREATE (:" + std::string{table} + " {k:" + uuidLiteral(UUID_A) + ",id:1})");
        run("CREATE (:" + std::string{table} + " {k:" + uuidLiteral(UUID_B) + ",id:2})");
        const auto query = "MATCH(n:" + std::string{table} +
                           ") WHERE n.k=" + uuidLiteral(UUID_A_UPPER) + " RETURN n.id";
        EXPECT_EQ(tables(query).size(), 1u);
        EXPECT_EQ(ids(query), std::vector<int64_t>{1});
        auto plan = conn->query("EXPLAIN " + query);
        ASSERT_TRUE(plan->isSuccess()) << plan->getErrorMessage();
        EXPECT_NE(plan->toString().find("PRIMARY_KEY_SCAN_NODE_TABLE"), std::string::npos);
    }
}

TEST_F(PartitionPruningTest, UUIDListPreparedParametersAndCatalogChanges) {
    uuidListTable();
    auto prepared = conn->prepare("MATCH(n:U) WHERE n.k=$key RETURN n.id");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    auto executeUUID = [&](const char* key) {
        return ids(*conn->execute(prepared.get(),
            std::make_pair(std::string{"key"}, common::uuid{common::UUID::fromString(key)})));
    };
    EXPECT_EQ(executeUUID(UUID_A_UPPER), (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(executeUUID(UUID_B), std::vector<int64_t>{2});
    EXPECT_TRUE(executeUUID(UUID_ZERO).empty());
    std::unordered_map<std::string, std::unique_ptr<common::Value>> nullParams;
    nullParams.emplace("key", std::make_unique<common::Value>(
                                  common::Value::createNullValue(common::LogicalType::UUID())));
    EXPECT_TRUE(ids(*conn->executeWithParams(prepared.get(), std::move(nullParams))).empty());
    EXPECT_EQ(executeUUID(UUID_A), (std::vector<int64_t>{1, 3}));

    auto missing = conn->prepare("MATCH(n:U) WHERE n.k=" + uuidLiteral(UUID_ZERO) + " RETURN n.id");
    ASSERT_TRUE(missing->isSuccess()) << missing->getErrorMessage();
    EXPECT_TRUE(ids(*conn->execute(missing.get())).empty());
    run("BEGIN TRANSACTION");
    run("CREATE (:U {id:4,k:" + uuidLiteral(UUID_ZERO) + "})");
    EXPECT_EQ(executeUUID(UUID_ZERO), std::vector<int64_t>{4});
    EXPECT_EQ(ids(*conn->execute(missing.get())), std::vector<int64_t>{4});
    run("ROLLBACK");
    EXPECT_TRUE(executeUUID(UUID_ZERO).empty());
    EXPECT_TRUE(ids(*conn->execute(missing.get())).empty());
    run("COPY U FROM (RETURN 5 AS id, " + uuidLiteral(UUID_ZERO) + " AS k)");
    EXPECT_EQ(executeUUID(UUID_ZERO), std::vector<int64_t>{5});
    EXPECT_EQ(ids(*conn->execute(missing.get())), std::vector<int64_t>{5});
}

TEST_F(PartitionPruningTest, UUIDListCheckpointReopenReusesPartitions) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    uuidListTable();
    run("CHECKPOINT");
    conn.reset();
    database.reset();
    createDBAndConn();
    const auto query = "MATCH(n:U) WHERE n.k=" + uuidLiteral(UUID_A_UPPER) + " RETURN n.id";
    EXPECT_EQ(tables(query), std::vector<std::string>{"U_p1"});
    EXPECT_EQ(ids(query), (std::vector<int64_t>{1, 3}));
    run("COPY U FROM (RETURN 4 AS id, " + uuidLiteral(UUID_A_UPPER) + " AS k)");
    run("COPY U FROM (RETURN 5 AS id, " + uuidLiteral(UUID_MAX) + " AS k)");
    rejected("COPY U_p1 FROM (RETURN 6 AS id, " + uuidLiteral(UUID_B) + " AS k)");
    run("CHECKPOINT");
    conn.reset();
    database.reset();
    createDBAndConn();
    EXPECT_EQ(tables(query), std::vector<std::string>{"U_p1"});
    EXPECT_EQ(ids(query), (std::vector<int64_t>{1, 3, 4}));
    EXPECT_EQ(tables("MATCH(n:U) RETURN n.id").size(), 4u);
    const auto newKey = "MATCH(n:U) WHERE n.k=" + uuidLiteral(UUID_MAX) + " RETURN n.id";
    EXPECT_EQ(tables(newKey), std::vector<std::string>{"U_p3"});
    EXPECT_EQ(ids(newKey), std::vector<int64_t>{5});
}

TEST_F(PartitionPruningTest, UUIDHashCreateCopyAndEqualityPruning) {
    run("CREATE NODE TABLE UH(id INT64 PRIMARY KEY, k UUID) PARTITION BY HASH(k) PARTITIONS 4");
    run("CREATE (:UH {id:1,k:" + uuidLiteral(UUID_A) + "})");
    run("COPY UH FROM (RETURN 2 AS id, " + uuidLiteral(UUID_B) + " AS k)");
    run("COPY UH FROM (RETURN 3 AS id, " + uuidLiteral(UUID_A_UPPER) + " AS k)");
    const auto bucket = ids("RETURN CAST(HASH(" + uuidLiteral(UUID_A) + ") % 4 AS INT64)")[0];
    const auto query = "MATCH(n:UH) WHERE n.k=" + uuidLiteral(UUID_A_UPPER) + " RETURN n.id";
    EXPECT_EQ(tables(query), std::vector<std::string>{"UH_p" + std::to_string(bucket)});
    EXPECT_EQ(ids(query), (std::vector<int64_t>{1, 3}));
    rejected("CREATE (:UH_p" + std::to_string((bucket + 1) % 4) +
             " {id:4,k:" + uuidLiteral(UUID_A) + "})");
}

TEST_F(PartitionPruningTest, ConservativeFallbacksAndMixedLabels) {
    hashTable();
    listTable();
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id>2 RETURN n.id").size(), 4u);
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id=2 OR n.id=6 RETURN n.id").size(), 4u);
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id+1=3 RETURN n.id").size(), 4u);
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id=2.0 RETURN n.id").size(), 4u);
    EXPECT_EQ(tables("MATCH(n:L) WHERE n.id=2 RETURN n.id").size(), 3u);
    run("CREATE NODE TABLE Plain(id INT64 PRIMARY KEY)");
    run("CREATE (:Plain {id:2})");
    const auto query = "MATCH(n:H:Plain) WHERE n.id=2 RETURN n.id";
    EXPECT_EQ(tables(query), (std::vector<std::string>{"H_p2", "Plain"}));
    EXPECT_EQ(ids(query), (std::vector<int64_t>{2, 2}));
}

TEST_F(PartitionPruningTest, PreparedParametersDoNotFreezeThePartition) {
    hashTable();
    listTable();
    auto hash = conn->prepare("MATCH(n:H) WHERE n.id=$key RETURN n.id");
    ASSERT_TRUE(hash->isSuccess()) << hash->getErrorMessage();
    for (const auto key : {2, 6, 1, 8, 2}) {
        auto result = conn->execute(hash.get(), std::make_pair(std::string{"key"}, int64_t{key}));
        EXPECT_EQ(ids(*result), std::vector<int64_t>{key});
    }
    std::unordered_map<std::string, std::unique_ptr<common::Value>> nullParams;
    nullParams.emplace("key", std::make_unique<common::Value>(
                                  common::Value::createNullValue(common::LogicalType::INT64())));
    EXPECT_TRUE(ids(*conn->executeWithParams(hash.get(), std::move(nullParams))).empty());
    EXPECT_EQ(ids(*conn->execute(hash.get(), std::make_pair(std::string{"key"}, int64_t{2}))),
        std::vector<int64_t>{2});
    auto list = conn->prepare("MATCH(n:L) WHERE n.k=$key RETURN n.id");
    ASSERT_TRUE(list->isSuccess()) << list->getErrorMessage();
    for (const auto& key : {"east", "west", "missing", "east"}) {
        auto result =
            conn->execute(list.get(), std::make_pair(std::string{"key"}, std::string{key}));
        const auto expected = std::string{key} == "east" ? std::vector<int64_t>{1, 3} :
                              std::string{key} == "west" ? std::vector<int64_t>{2} :
                                                           std::vector<int64_t>{};
        EXPECT_EQ(ids(*result), expected);
    }
}

TEST_F(PartitionPruningTest, NewListPartitionInvalidatesPreparedEmptyResult) {
    listTable();
    auto prepared = conn->prepare("MATCH(n:L) WHERE n.k='new' RETURN n.id");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    EXPECT_TRUE(ids(*conn->execute(prepared.get())).empty());
    run("CREATE (:L {id:4,k:'new'})");
    EXPECT_EQ(ids(*conn->execute(prepared.get())), std::vector<int64_t>{4});
}

TEST_F(PartitionPruningTest, PreparedQuerySeesNewListPartitionInsideTransaction) {
    listTable();
    auto prepared = conn->prepare("MATCH(n:L) WHERE n.k='new' RETURN n.id");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    EXPECT_TRUE(ids(*conn->execute(prepared.get())).empty());
    run("BEGIN TRANSACTION");
    run("CREATE (:L {id:4,k:'new'})");
    EXPECT_EQ(ids(*conn->execute(prepared.get())), std::vector<int64_t>{4});
    run("ROLLBACK");
    EXPECT_TRUE(ids(*conn->execute(prepared.get())).empty());
    // Reusing the rolled-back ordinal must not leave two managers owning the same file.
    run("CREATE (:L {id:5,k:'new'})");
    run("CHECKPOINT");
    EXPECT_EQ(ids(*conn->execute(prepared.get())), std::vector<int64_t>{5});
}

TEST_F(PartitionPruningTest, JoinsAndOptionalMatchPreserveResults) {
    hashTable();
    listTable();
    EXPECT_EQ(ids("MATCH(h:H), (l:L) WHERE h.id=2 AND l.k='east' RETURN l.id"),
        (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(ids("MATCH(h:H), (l:L) WHERE h.id=l.id AND l.k='east' RETURN h.id"),
        (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(ids("MATCH(h:H) WHERE h.id=2 OPTIONAL MATCH(l:L) WHERE l.k='missing' RETURN h.id"),
        std::vector<int64_t>{2});
}

TEST_F(PartitionPruningTest, NewListPartitionIsInvisibleToOlderSnapshot) {
    listTable();
    main::Connection reader{database.get()};
    ASSERT_TRUE(reader.query("BEGIN TRANSACTION READ ONLY")->isSuccess());
    const auto query = "MATCH(n:L) WHERE n.k='later' RETURN n.id";
    EXPECT_TRUE(ids(*reader.query(query)).empty());
    run("CREATE (:l {id:4,k:'later'})"); // Parent names remain case insensitive.
    EXPECT_TRUE(ids(*reader.query(query)).empty());
    ASSERT_TRUE(reader.query("COMMIT")->isSuccess());
    EXPECT_EQ(ids(*reader.query(query)), std::vector<int64_t>{4});
}

TEST_F(PartitionPruningTest, DirectChildCreateAndMergeEnforceRouting) {
    hashTable();
    listTable();
    rejected("CREATE (:H_p1 {id:2})");
    rejected("MERGE (:H_p1 {id:2})");
    rejected("CREATE (:L_p0 {id:9,k:'east'})");
    rejected("CREATE (:L_p1 {id:9,k:'west'})");
    rejected("CREATE (:L_p1 {id:9,k:NULL})");
    run("CREATE (:L_p1 {id:4,k:'east'})");
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='east' RETURN n.id"), (std::vector<int64_t>{1, 3, 4}));
    EXPECT_EQ(ids("MATCH(n:H) RETURN count(*)"), std::vector<int64_t>{8});
}

TEST_F(PartitionPruningTest, CopyIntoExistingListPartitionsUsesChildOrdinal) {
    listTable();
    run("COPY L FROM (UNWIND [4,5] AS i RETURN i AS id, 'east' AS k)");
    run("COPY L FROM (UNWIND [6,7] AS i RETURN i AS id, 'west' AS k)");
    EXPECT_EQ(ids("MATCH(n:L_p0) RETURN count(*)"), std::vector<int64_t>{0});
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='east' RETURN n.id"), (std::vector<int64_t>{1, 3, 4, 5}));
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='west' RETURN n.id"), (std::vector<int64_t>{2, 6, 7}));
}

TEST_F(PartitionPruningTest, DirectChildCopyValidatesBeforeWriting) {
    hashTable();
    listTable();
    rejected("COPY H_p1 FROM (RETURN 2 AS id)");
    rejected("COPY L_p1 FROM (UNWIND [4,5] AS i RETURN i AS id, "
             "CASE WHEN i=4 THEN 'east' ELSE 'west' END AS k)");
    EXPECT_EQ(ids("MATCH(n:L) RETURN count(*)"), std::vector<int64_t>{3});
    // The invalid row arrives after earlier batches may already have reached local storage.
    rejected("COPY L_p1 FROM (UNWIND range(10,5000) AS i RETURN i AS id, "
             "CASE WHEN i=5000 THEN 'west' ELSE 'east' END AS k)");
    EXPECT_EQ(ids("MATCH(n:L) RETURN count(*)"), std::vector<int64_t>{3});
    run("COPY L_p1 FROM (RETURN 4 AS id, 'east' AS k)");
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='east' RETURN n.id"), (std::vector<int64_t>{1, 3, 4}));
}

TEST_F(PartitionPruningTest, CheckpointReopenPreservesPruning) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    hashTable();
    listTable();
    run("CHECKPOINT");
    conn.reset();
    database.reset();
    createDBAndConn();
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id=2 RETURN n.id"), std::vector<std::string>{"H_p2"});
    EXPECT_EQ(tables("MATCH(n:L) WHERE n.k='east' RETURN n.id"), std::vector<std::string>{"L_p1"});
    run("COPY L FROM (RETURN 4 AS id, 'east' AS k)");
    EXPECT_EQ(ids("MATCH(n:L) WHERE n.k='east' RETURN n.id"), (std::vector<int64_t>{1, 3, 4}));
}

TEST_F(PartitionPruningTest, UnvalidatedParentStaysUnprunedAfterCheckpoint) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    hashTable();
    run("BEGIN TRANSACTION");
    auto* parent =
        database->getCatalog()
            ->getTableCatalogEntry(transaction::Transaction::Get(*conn->getClientContext()), "H")
            ->ptrCast<catalog::NodeTableCatalogEntry>();
    parent->setPartitionPruningSafe(false); // State assigned by legacy catalog deserialization.
    run("COMMIT");
    run("CHECKPOINT");
    conn.reset();
    database.reset();
    createDBAndConn();
    EXPECT_EQ(tables("MATCH(n:H) WHERE n.id=2 RETURN n.id").size(), 4u);
    EXPECT_EQ(ids("MATCH(n:H) WHERE n.id=2 RETURN n.id"), std::vector<int64_t>{2});
}

TEST_F(PartitionPruningTest, DailyDeleteUsesTheActualNodeTable) {
    run("CREATE NODE TABLE D(id INT64 PRIMARY KEY, k DATE) "
        "PARTITION BY RANGE(k) INTERVAL '1 day'");
    run("MATCH (n:D) DELETE n");
    run("MATCH (n:D) DETACH DELETE n");
    run("CREATE NODE TABLE Plain(id INT64 PRIMARY KEY)");
    run("CREATE (:Plain {id:1})");
    run("MATCH (n:D:Plain) DELETE n");
    EXPECT_EQ(ids("MATCH (n:Plain) RETURN count(*)"), std::vector<int64_t>{0});

    run("CREATE (n:D {id:1,k:DATE('2024-01-01')}) DELETE n");
    EXPECT_EQ(ids("MATCH (n:D) RETURN count(*)"), std::vector<int64_t>{0});
    run("CREATE (:D {id:7,k:DATE('2024-01-01')})");
    // Both children can have offset zero and the same PK. Resolve the freshly created
    // second day from the actual node ID, including when only the first child was bound.
    run("CREATE (n:D {id:7,k:DATE('2024-01-02')}) DETACH DELETE n");
    EXPECT_EQ(ids("MATCH (n:D_p0) RETURN n.id"), std::vector<int64_t>{7});
    EXPECT_EQ(ids("MATCH (n:D_p1) RETURN count(*)"), std::vector<int64_t>{0});
    run("MATCH (n:D) WHERE n.k=DATE('2024-01-01') DELETE n");
    EXPECT_EQ(ids("MATCH (n:D) RETURN count(*)"), std::vector<int64_t>{0});
}

} // namespace testing
} // namespace lbug
