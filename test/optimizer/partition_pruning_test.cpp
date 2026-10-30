#include <algorithm>
#include <functional>
#include <stdexcept>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
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

} // namespace testing
} // namespace lbug
