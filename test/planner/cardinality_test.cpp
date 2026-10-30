#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/data_chunk/data_chunk_state.h"
#include "graph_test/private_graph_test.h"
#include "main/client_context.h"
#include "planner/operator/logical_plan_util.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/rel_table.h"
#include "test_helper/test_helper.h"
#include "test_runner/test_runner.h"
#include "transaction/transaction.h"
#include <format>

namespace lbug {
namespace testing {

using common::nodeID_t;
using common::sel_t;

class CardinalityTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    std::string getEncodedPlan(const std::string& query) {
        return planner::LogicalPlanUtil::encodeJoin(*TestRunner::getLogicalPlan(query, *conn));
    }
    std::unique_ptr<planner::LogicalPlan> getRoot(const std::string& query) {
        return TestRunner::getLogicalPlan(query, *conn);
    }
    std::pair<planner::LogicalOperator*, planner::LogicalOperator*> getSource(
        planner::LogicalOperator* op, planner::LogicalOperator* parent = nullptr) {
        if (op->getNumChildren() == 0) {
            return {parent, op};
        }
        return getSource(op->getChild(0).get(), op);
    }
    planner::LogicalOperator* getOpWithType(planner::LogicalOperator* op,
        planner::LogicalOperatorType type) {
        if (op->getOperatorType() == type) {
            return op;
        }
        if (op->getNumChildren() == 0) {
            return nullptr;
        }
        return getOpWithType(op->getChild(0).get(), type);
    }
    uint64_t countOpsWithType(planner::LogicalOperator* op, planner::LogicalOperatorType type) {
        auto result = op->getOperatorType() == type ? 1u : 0u;
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            result += countOpsWithType(op->getChild(i).get(), type);
        }
        return result;
    }
    void expectFilterEstimate(const std::string& table, const std::string& predicate,
        common::cardinality_t expected) {
        auto plan = getRoot(
            std::format("EXPLAIN LOGICAL MATCH (u:{}) WHERE {} RETURN u.id", table, predicate));
        auto [parent, source] = getSource(plan->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType()) << predicate;
        EXPECT_EQ(expected, parent->getCardinality()) << predicate;
    }
};

TEST_F(CardinalityTest, TestOperators) {
    // Filter
    {
        // we only get cardinalities of operators created by the optimizer if we use EXPLAIN LOGICAL
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person) WHERE p1.gender=1 RETURN p1.ID");
        auto [parent, source] = getSource(plan->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::SCAN_NODE_TABLE, source->getOperatorType());
        EXPECT_EQ(8, source->getCardinality());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(4, parent->getCardinality());
    }

    // Range filter selectivity from observed [min, max] (person IDs are 0..10).
    // A filter keeping every row estimates the full input instead of the fixed 0.1.
    {
        auto keepsAll = getRoot("EXPLAIN LOGICAL MATCH (p1: person) WHERE p1.ID >= 0 RETURN p1.ID");
        auto [parent, source] = getSource(keepsAll->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(8, parent->getCardinality());
    }
    {
        // Property on the right flips the comparison.
        auto keepsAll = getRoot("EXPLAIN LOGICAL MATCH (p1: person) WHERE 0 <= p1.ID RETURN p1.ID");
        auto [parent, source] = getSource(keepsAll->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(8, parent->getCardinality());
    }
    {
        // Half the observed range keeps half the rows (uniformity assumption).
        auto keepsHalf =
            getRoot("EXPLAIN LOGICAL MATCH (p1: person) WHERE p1.ID >= 5 RETURN p1.ID");
        auto [parent, source] = getSource(keepsHalf->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(4, parent->getCardinality());
    }
    {
        // A filter outside the range estimates (at least) one row, never zero.
        auto keepsNone = getRoot("EXPLAIN LOGICAL MATCH (p1: person) WHERE p1.ID < 0 RETURN p1.ID");
        auto [parent, source] = getSource(keepsNone->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(1, parent->getCardinality());
    }
    {
        // Binder-inserted CASTs (INT32 column vs INT64 literal) unwrap to the property:
        // a keep-all filter estimates the full scan input (fallback would estimate 1).
        auto keepsAll =
            getRoot("EXPLAIN LOGICAL MATCH (m: movies) WHERE m.length >= 0 RETURN m.name");
        auto [parent, source] = getSource(keepsAll->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(source->getCardinality(), parent->getCardinality());
        EXPECT_GT(parent->getCardinality(), 1);
    }

    // Limit
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person) RETURN p1.ID LIMIT 2");
        auto* limitOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::LIMIT);
        ASSERT_NE(nullptr, limitOp);
        EXPECT_EQ(planner::LogicalOperatorType::LIMIT, limitOp->getOperatorType());
        EXPECT_EQ(2, limitOp->getCardinality());
    }

    // Aggregate
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person) RETURN COUNT(*), MAX(p1.age)");
        auto* aggregateOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::AGGREGATE);
        ASSERT_NE(nullptr, aggregateOp);
        EXPECT_EQ(1, aggregateOp->getCardinality());
    }

    // Cross Product
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person), (p2: person) RETURN p1.ID, p2.ID");
        auto* productOp = getOpWithType(plan->getLastOperator().get(),
            planner::LogicalOperatorType::CROSS_PRODUCT);
        ASSERT_NE(nullptr, productOp);
        EXPECT_EQ(64, productOp->getCardinality());
    }

    // Hash Join (non-ID based join)
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p: person), (o: organisation) WHERE p.ID = "
                            "o.ID RETURN p.fName, o.name");
        auto* joinOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::HASH_JOIN);
        ASSERT_NE(nullptr, joinOp);
        EXPECT_EQ(1, joinOp->getCardinality());
    }

    // Extend + Hash Join
    {
        auto plan = getRoot(
            "EXPLAIN LOGICAL MATCH (p1: person)-[k:knows]->(p2: person) RETURN p1.ID, p2.ID");
        auto* extendOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::EXTEND);
        ASSERT_NE(nullptr, extendOp);
        static constexpr auto numRelsInKnows = 14;
        EXPECT_EQ(numRelsInKnows, extendOp->getCardinality());

        auto* joinOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::HASH_JOIN);
        ASSERT_NE(nullptr, joinOp);
        EXPECT_GT(joinOp->getCardinality(), 1);
    }

    // Intersect + Flatten
    {
        auto plan = getRoot(
            "EXPLAIN LOGICAL MATCH (p1: person)-[k1:knows]->(p2: person)-[k2:knows]->(p3:person), "
            "(p1)-[k3:knows]->(p3) "
            "HINT ((p1 JOIN k1 JOIN p2) MULTI_JOIN k2 MULTI_JOIN k3) JOIN p3 "
            "RETURN p1.ID, p2.ID, p3.ID");
        auto* intersect =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::INTERSECT);
        ASSERT_NE(nullptr, intersect);
        EXPECT_EQ(intersect->getCardinality(), 1);

        auto* flatten =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::FLATTEN);
        ASSERT_NE(nullptr, intersect);
        EXPECT_GT(flatten->getCardinality(), 1);
    }

    // Load From Parquet
    {
        auto plan = getRoot(std::format(
            "LOAD FROM \"{}/dataset/demo-db/parquet/user.parquet\" RETURN *", LBUG_ROOT_DIRECTORY));
        EXPECT_EQ(4, plan->getCardinality());
    }

    // Load From Numpy
    {
        auto plan = getRoot(std::format(
            "LOAD FROM \"{}/dataset/npy-1d/one_dim_int64.npy\" RETURN *", LBUG_ROOT_DIRECTORY));
        EXPECT_EQ(3, plan->getCardinality());
    }
}

static int32_t sameDay(int32_t days) {
    return days;
}

static int64_t dayNumber(int32_t days) {
    return days;
}

TEST_F(CardinalityTest, RangeFilterAcrossTemporalUnits) {
    // Daily holds one day per row from 2020-01-01, SameDay holds 2022-01-01 in each of 100 rows;
    // each day is stored as a DATE and in every timestamp type.
    for (auto [table, lastID, day] :
        {std::tuple{"Daily", 999, "i"}, std::tuple{"SameDay", 99, "731"}}) {
        ASSERT_TRUE(conn->query(std::format("CREATE NODE TABLE {}(id INT64 PRIMARY KEY, d DATE, "
                                            "ts TIMESTAMP, ttz TIMESTAMP_TZ, tms TIMESTAMP_MS, "
                                            "tns TIMESTAMP_NS, tsec TIMESTAMP_SEC);",
                                    table))
                        ->isSuccess());
        ASSERT_TRUE(
            conn->query(std::format("UNWIND range(0, {}) AS i WITH i, timestamp('2020-01-01 "
                                    "00:00:00') + to_days({}) AS t CREATE (:{} {{id: i, d: "
                                    "date('2020-01-01') + to_days({}), ts: t, ttz: CAST(t AS "
                                    "TIMESTAMP_TZ), tms: CAST(t AS TIMESTAMP_MS), tns: CAST(t AS "
                                    "TIMESTAMP_NS), tsec: CAST(t AS TIMESTAMP_SEC)}});",
                            lastID, day, table, day))
                ->isSuccess());
    }
    const std::vector<std::string> constants{"date('2022-01-01')",
        "timestamp('2022-01-01 00:00:00')", "CAST('2022-01-01 00:00:00' AS TIMESTAMP_TZ)",
        "CAST('2022-01-01 00:00:00' AS TIMESTAMP_MS)",
        "CAST('2022-01-01 00:00:00' AS TIMESTAMP_NS)",
        "CAST('2022-01-01 00:00:00' AS TIMESTAMP_SEC)"};
    for (const auto* column : {"d", "ts", "ttz", "tms", "tns", "tsec"}) {
        const auto prop = std::format("u.{}", column);
        for (const auto& constant : constants) {
            // 268 of Daily's 1,000 days lie after 2022-01-01 and 731 before it.
            expectFilterEstimate("Daily", prop + " > " + constant, 268);
            expectFilterEstimate("Daily", prop + " < " + constant, 731);
            expectFilterEstimate("Daily", constant + " < " + prop, 268);
            // SameDay's one value equals the constant: >= keeps all 100 rows, > keeps none.
            expectFilterEstimate("SameDay", prop + " >= " + constant, 100);
            expectFilterEstimate("SameDay", prop + " > " + constant, 1);
        }
    }
    // A user function named CAST_* is stripped like a cast: DATE to DATE keeps the range
    // estimate, DATE to INT64 leaves a DATE range against a number of unknown unit and falls back
    // to the fixed 10% (268 on main, 1,000 without the fallback).
    conn->createScalarFunction("CAST_SAME",
        std::vector<common::LogicalTypeID>{common::LogicalTypeID::DATE},
        common::LogicalTypeID::DATE, &sameDay);
    conn->createScalarFunction("CAST_DAYS",
        std::vector<common::LogicalTypeID>{common::LogicalTypeID::DATE},
        common::LogicalTypeID::INT64, &dayNumber);
    expectFilterEstimate("Daily", "CAST_SAME(u.d) > date('2022-01-01')", 268);
    expectFilterEstimate("Daily", "CAST_DAYS(u.d) > 18993", 100);
}

static int16_t floorToInt16(double x) {
    return static_cast<int16_t>(std::floor(x));
}

TEST_F(CardinalityTest, RangeFilterThroughRoundingCast) {
    // Spread: x = i / 110 (0 to 0.9) and one timestamp per hour from 2022-01-01 00:00 (99 hours).
    // Noon: x = 0.7 and 2022-01-01 12:00 in each of 100 rows. Edge: x = 0.5, a rounding tie, and an
    // instant whose nanoseconds round down in double, so a microsecond grid would floor it below
    // the constant. Each timestamp is stored in every type.
    for (auto [table, x, t] :
        {std::tuple{"Spread", "i / 110.0", "timestamp('2022-01-01 00:00:00') + to_hours(i)"},
            std::tuple{"Noon", "0.7", "timestamp('2022-01-01 12:00:00')"},
            std::tuple{"Edge", "0.5", "timestamp('2022-09-07 00:59:38.477008')"}}) {
        ASSERT_TRUE(conn->query(std::format("CREATE NODE TABLE {}(id INT64 PRIMARY KEY, i INT32, "
                                            "x DOUBLE, f FLOAT, ts TIMESTAMP, ttz TIMESTAMP_TZ, "
                                            "tms TIMESTAMP_MS, tsec TIMESTAMP_SEC, "
                                            "tns TIMESTAMP_NS);",
                                    table))
                        ->isSuccess());
        ASSERT_TRUE(conn->query(std::format("UNWIND range(0, 99) AS i WITH i, {} AS x, {} AS t "
                                            "CREATE (:{} {{id: i, i: CAST(i AS INT32), x: x, f: "
                                            "CAST(x AS FLOAT), ts: t, ttz: CAST(t AS "
                                            "TIMESTAMP_TZ), tms: CAST(t AS TIMESTAMP_MS), tsec: "
                                            "CAST(t AS TIMESTAMP_SEC), tns: CAST(t AS "
                                            "TIMESTAMP_NS)}});",
                                    x, t, table))
                        ->isSuccess());
    }
    // Rounding to an integer splits Spread's x at 0.5: 44 rows above, 56 below (estimated 55).
    for (const auto* predicate :
        {"CAST(u.x AS INT64) >= 1", "CAST(u.x AS INT64) > 0", "1 <= CAST(u.x AS INT64)",
            "CAST(u.x AS INT32) > 0.2", "CAST(u.x AS int) >= 1", "CAST(u.f AS INT16) >= 1"}) {
        expectFilterEstimate("Spread", predicate, 44);
    }
    expectFilterEstimate("Spread", "CAST(u.x AS INT64) < 1", 55);
    expectFilterEstimate("Spread", "CAST(u.x AS INT64) <= 0", 55);
    // Truncating to DATE splits Spread's timestamps at a midnight: 52 rows from 2022-01-03 on
    // (estimated 51), 48 before it.
    std::vector<std::string> afterSecondDay{"CAST(u.ts AS DATE) > date('2022-01-02')",
        "CAST(u.ts AS DATE) >= timestamp('2022-01-02 12:00:00')",
        "CAST(CAST(u.ts AS TIMESTAMP_SEC) AS DATE) > date('2022-01-02')"};
    for (const auto* column : {"ttz", "tms", "tsec", "tns"}) {
        afterSecondDay.push_back(std::format("CAST(u.{} AS DATE) > date('2022-01-02')", column));
    }
    for (const auto& predicate : afterSecondDay) {
        expectFilterEstimate("Spread", predicate, 51);
    }
    expectFilterEstimate("Spread", "CAST(u.ts AS DATE) <= date('2022-01-02')", 48);
    // These keep the plain estimate. An INT32 property compares as CAST(u.i AS INT64) and is
    // already on that grid: 49 as for an INT64 column, though 50 rows match. DECIMAL counts as
    // continuous (61 match). A user function named like a built-in cast, or the one CAST() calls
    // for a CREATE TYPE type, does something unknown (none match).
    for (const auto* name : {"CAST_TO_INT16", "CAST_TO_FLOORED"}) {
        conn->createScalarFunction(name,
            std::vector<common::LogicalTypeID>{common::LogicalTypeID::DOUBLE},
            common::LogicalTypeID::INT16, &floorToInt16);
    }
    ASSERT_TRUE(conn->query("CREATE TYPE FLOORED AS INT16;")->isSuccess());
    expectFilterEstimate("Spread", "u.i >= 50", 49);
    expectFilterEstimate("Spread", "CAST(u.x AS DECIMAL(2,1)) > 0.3", 66);
    expectFilterEstimate("Spread", "CAST_TO_INT16(u.x) >= 1", 1);
    expectFilterEstimate("Spread", "CAST(u.x AS FLOORED) >= 1", 1);
    auto floored = conn->query("MATCH (u:Spread) WHERE CAST(u.x AS FLOORED) >= 1 RETURN count(*)");
    ASSERT_TRUE(floored->isSuccess());
    EXPECT_EQ(0, floored->getNext()->getValue(0)->getValue<int64_t>());
    // A single value decides the predicate after the cast. 0.5 rounds to 0, as the cast does.
    expectFilterEstimate("Noon", "CAST(u.x AS INT64) >= 1", 100);
    expectFilterEstimate("Noon", "CAST(u.ts AS DATE) <= date('2022-01-01')", 100);
    expectFilterEstimate("Noon", "CAST(u.ts AS DATE) > date('2022-01-01')", 1);
    expectFilterEstimate("Edge", "CAST(u.x AS INT64) >= 1", 1);
    auto tie = conn->query("MATCH (u:Edge) WHERE CAST(u.x AS INT64) >= 1 RETURN count(*)");
    ASSERT_TRUE(tie->isSuccess());
    EXPECT_EQ(0, tie->getNext()->getValue(0)->getValue<int64_t>());
    // TIMESTAMP_NS against TIMESTAMP compares as CAST(u.tns AS TIMESTAMP), which gets no
    // microsecond grid.
    expectFilterEstimate("Edge", "u.tns >= timestamp('2022-09-07 00:59:38.477008')", 100);
    expectFilterEstimate("Edge", "u.tns < timestamp('2022-09-07 00:59:38.477008')", 1);
}

TEST_F(CardinalityTest, TestPopulatedAfterOptimizations) {
    // Filter push down
    auto plan = getRoot("EXPLAIN LOGICAL MATCH (a:person)-[e]->(b) "
                        "WHERE a.ID < 0 AND a.fName='Alice' "
                        "RETURN a.gender;");
    std::function<void(planner::LogicalOperator*)> checkFunc;
    checkFunc = [&checkFunc](planner::LogicalOperator* op) {
        EXPECT_GT(op->getCardinality(), 0);
        for (uint32_t i = 0; i < op->getNumChildren(); ++i) {
            checkFunc(op->getChild(i).get());
        }
    };
    checkFunc(plan->getLastOperator().get());
}

TEST_F(CardinalityTest, TestPackedPathExtendOptIn) {
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE PackedPerson(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE PackedFollows(FROM PackedPerson TO PackedPerson);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 2});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 3});")->isSuccess());
    const auto query = "EXPLAIN LOGICAL MATCH "
                       "(a:PackedPerson)-[:PackedFollows]->(b:PackedPerson)"
                       "-[:PackedFollows]->(c:PackedPerson) "
                       "RETURN a.id, b.id, c.id";

    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=false")->isSuccess());
    auto disabledPlan = getRoot(query);
    EXPECT_EQ(0, countOpsWithType(disabledPlan->getLastOperator().get(),
                     planner::LogicalOperatorType::PACKED_EXTEND));

    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    auto enabledPlan = getRoot(query);
    EXPECT_GE(countOpsWithType(enabledPlan->getLastOperator().get(),
                  planner::LogicalOperatorType::PACKED_EXTEND),
        2);
}

TEST_F(CardinalityTest, TestPackedExtendDropsParentsWithoutMatches) {
    // Setup small packed graph where only one `a` has a valid two-hop path a->b->c.
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE PackedPerson(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE PackedFollows(FROM PackedPerson TO PackedPerson);")
                    ->isSuccess());
    // Create 5 nodes
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 2});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 3});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 4});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 5});")->isSuccess());
    // Create edges forming a chain 1->2->3 only. Nodes 4 and 5 (and possibly 2) won't form full
    // a->b->c
    ASSERT_TRUE(conn->query("MATCH (a:PackedPerson {id:1}), (b:PackedPerson {id:2}) CREATE "
                            "(a)-[:PackedFollows]->(b);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:PackedPerson {id:2}), (b:PackedPerson {id:3}) CREATE "
                            "(a)-[:PackedFollows]->(b);")
                    ->isSuccess());

    // Enable packed path extend and run the query. Expect only a.id == 1 to appear in results.
    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    auto res = conn->query(
        "MATCH "
        "(a:PackedPerson)-[:PackedFollows]->(b:PackedPerson)-[:PackedFollows]->(c:PackedPerson) "
        "RETURN DISTINCT a.id ORDER BY a.id");
    std::vector<int64_t> foundAIds;
    while (res->hasNext()) {
        auto tup = res->getNext();
        ASSERT_NE(nullptr, tup);
        foundAIds.push_back(tup->getValue(0)->getValue<int64_t>());
    }
    // Expect exactly one matching a (id==1); parents with no b/c path must be dropped.
    ASSERT_EQ(1u, foundAIds.size());
    EXPECT_EQ(1, foundAIds[0]);
}

TEST_F(CardinalityTest, TestMultiParentPackedScan) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE MPerson(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE MKnows(FROM MPerson TO MPerson);")->isSuccess());

    // 3000 parents; 3/4 of them have 3 children each (6750 edges total, more than one output
    // batch of 2048 children), the rest are zero-child parents. The small per-parent degree
    // forces the multi-parent packed CSR scan to serve children of many parents per output
    // batch (a single-parent batch would hold at most one parent's children).
    const common::offset_t numNodes = 3000;
    const auto tmpDir = TestHelper::getTempDir("multi-parent-packed-scan");
    const auto nodeCSV = tmpDir + "/nodes.csv";
    const auto relCSV = tmpDir + "/rels.csv";
    std::unordered_map<int64_t, uint64_t> expectedCounts;
    {
        std::ofstream nodeFile(nodeCSV);
        std::ofstream relFile(relCSV);
        for (common::offset_t i = 0; i < numNodes; i++) {
            nodeFile << i << "\n";
            if (i % 4 == 0) {
                continue; // zero-child parent
            }
            for (common::offset_t j = 1; j <= 3; j++) {
                const auto b = (i + j) % numNodes;
                relFile << i << "," << b << "\n";
                if ((i + b) % 10 == 0) {
                    expectedCounts[i]++;
                }
            }
        }
    }
    ASSERT_TRUE(conn->query(std::format("COPY MPerson FROM '{}'", nodeCSV))->isSuccess());
    ASSERT_TRUE(conn->query(std::format("COPY MKnows FROM '{}'", relCSV))->isSuccess());
    // Materialize the committed data into persistent CSR chunk groups so the rel scan takes
    // the persistent with-cache path where multi-parent packing is implemented.
    ASSERT_TRUE(conn->query("CHECKPOINT")->isSuccess());

    // Resolve node offsets: parallel COPY does not assign offsets in CSV order, so ids and
    // offsets are permuted relative to each other.
    std::unordered_map<int64_t, common::offset_t> idToOffset;
    {
        auto res = conn->query("MATCH (a:MPerson) RETURN a.id, offset(id(a))");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        while (res->hasNext()) {
            const auto tup = res->getNext();
            idToOffset[tup->getValue(0)->getValue<int64_t>()] =
                tup->getValue(1)->getValue<int64_t>();
        }
        ASSERT_EQ(idToOffset.size(), size_t(numNodes));
    }
    std::unordered_map<common::offset_t, std::vector<common::offset_t>> expectedEdges;
    for (common::offset_t i = 0; i < numNodes; i++) {
        if (i % 4 == 0) {
            continue;
        }
        for (common::offset_t j = 1; j <= 3; j++) {
            expectedEdges[idToOffset.at(i)].push_back(idToOffset.at((i + j) % numNodes));
        }
    }

    // --- SQL level: the PackedFilteredCount pattern (predicate over an nbr property routes the
    // nbr property fetch through a hash join, so these batches stay single-parent — the
    // materialization boundary is exactly what multi-parent packing must not cross; see
    // docs/multi_parent_lifetime.md). Packed and unpacked plans must agree.
    const auto packedCountQuery =
        "MATCH (a:MPerson)-[e:MKnows]->(b:MPerson) WHERE (a.id + b.id) % 10 = 0 "
        "RETURN a.id, COUNT(*)";
    auto collectCounts = [&](const std::string& query) {
        auto res = conn->query(query);
        EXPECT_TRUE(res->isSuccess()) << res->getErrorMessage();
        std::unordered_map<int64_t, uint64_t> counts;
        while (res->hasNext()) {
            const auto tup = res->getNext();
            counts[tup->getValue(0)->getValue<int64_t>()] = tup->getValue(1)->getValue<int64_t>();
        }
        return counts;
    };
    using CountMap = std::map<int64_t, uint64_t>;
    auto expectCountsEqual = [&](const std::unordered_map<int64_t, uint64_t>& actual) {
        EXPECT_EQ(CountMap(expectedCounts.begin(), expectedCounts.end()),
            CountMap(actual.begin(), actual.end()));
    };
    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    expectCountsEqual(collectCounts(packedCountQuery));
    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=false")->isSuccess());
    expectCountsEqual(collectCounts(packedCountQuery));

    // Standard consumers keep the one-parent-per-batch contract and must see every edge.
    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    auto totalRes = conn->query("MATCH (a:MPerson)-[:MKnows]->(b:MPerson) RETURN count(*)");
    ASSERT_TRUE(totalRes->isSuccess()) << totalRes->getErrorMessage();
    ASSERT_TRUE(totalRes->hasNext());
    EXPECT_EQ(6750, totalRes->getNext()->getValue(0)->getValue<int64_t>());
    EXPECT_FALSE(totalRes->hasNext());

    // --- Storage level: drive the rel scan directly with multi-parent packing enabled and
    // verify that batches carry many parents, that the prefix-sum offsets describe each
    // parent's children, and that every edge is served exactly once.
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    auto* clientContext = conn->getClientContext();
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto* catalog = catalog::Catalog::Get(*clientContext);
    auto* nodeEntry = catalog->getTableCatalogEntry(transaction, "MPerson");
    auto* relEntry = catalog->getTableCatalogEntry(transaction, "MKnows");
    const auto nodeTableID = nodeEntry->getTableID();
    const auto relTableID =
        relEntry->ptrCast<catalog::RelGroupCatalogEntry>()->getSingleRelEntryInfo().oid;
    auto* relTable = storage::StorageManager::Get(*clientContext)
                         ->getTable(relTableID)
                         ->ptrCast<storage::RelTable>();
    auto* mm = storage::MemoryManager::Get(*clientContext);

    auto boundState = std::make_shared<common::DataChunkState>();
    auto boundVector =
        std::make_unique<common::ValueVector>(common::LogicalType::INTERNAL_ID(), mm);
    boundVector->state = boundState;
    auto outState = std::make_shared<common::DataChunkState>();
    auto nbrVector = std::make_unique<common::ValueVector>(common::LogicalType::INTERNAL_ID(), mm);
    nbrVector->state = outState;

    storage::RelTableScanState scanState(*mm, boundVector.get(), {nbrVector.get()}, outState);
    scanState.setToTable(transaction, relTable, {0}, {}, common::RelDataDirection::FWD);
    scanState.packedMultiParentScan = true;

    common::offset_t nextParent = 0;
    auto feedNextParentBatch = [&]() {
        if (nextParent >= numNodes) {
            return false;
        }
        const auto end = std::min(nextParent + common::DEFAULT_VECTOR_CAPACITY, numNodes);
        const auto count = end - nextParent;
        boundState->setToUnflat();
        boundState->getSelVectorUnsafe().setToUnfiltered(count);
        for (common::offset_t i = 0; i < count; i++) {
            boundVector->setValue<nodeID_t>(i, nodeID_t{nextParent + i, nodeTableID});
        }
        nextParent = end;
        relTable->initScanState(transaction, scanState);
        return true;
    };

    std::unordered_map<common::offset_t, std::vector<common::offset_t>> edges;
    uint64_t numMultiParentBatches = 0;
    ASSERT_TRUE(feedNextParentBatch());
    for (;;) {
        if (!relTable->scan(transaction, scanState)) {
            if (!feedNextParentBatch()) {
                break;
            }
            continue;
        }
        const auto& boundSelVector = scanState.nodeIDVector->state->getSelVector();
        const auto numParentsInBatch = boundSelVector.getSelSize();
        if (numParentsInBatch > 1) {
            numMultiParentBatches++;
        }
        const auto& outSelVector = scanState.outState->getSelVector();
        const auto& offsets = scanState.packedChildOffsets;
        if (numParentsInBatch > 1) {
            ASSERT_EQ(offsets.size(), size_t(numParentsInBatch) + 1);
            ASSERT_EQ(offsets.back(), outSelVector.getSelSize());
        }
        for (sel_t p = 0; p < numParentsInBatch; p++) {
            const auto parentOffset = boundVector->getValue<nodeID_t>(boundSelVector[p]).offset;
            const auto start = numParentsInBatch > 1 ? offsets[p] : 0;
            const auto end = numParentsInBatch > 1 ? offsets[p + 1] : outSelVector.getSelSize();
            for (auto i = start; i < end; i++) {
                edges[parentOffset].push_back(
                    nbrVector->getValue<nodeID_t>(outSelVector[i]).offset);
            }
        }
    }
    // With degree 3 and 2048-child output batches, packing must actually engage: without it,
    // every batch would hold the children of exactly one parent.
    ASSERT_GT(numMultiParentBatches, 0u);
    ASSERT_EQ(edges.size(), expectedEdges.size());
    for (auto& [parent, children] : expectedEdges) {
        auto actual = edges.at(parent);
        auto expected = children;
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        ASSERT_EQ(actual, expected);
    }
    conn->query("COMMIT");
}

// A computed predicate input such as offset(id(b)) has no vector in the packed child's schema, so
// the PackedFilteredCount mapping must fall back to the regular aggregate instead of looking it up
// (#868). A plain property input still maps to the packed operator.
TEST_F(CardinalityTest, TestPackedFilteredCountComputedInputFallsBack) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE MPerson(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE MKnows(FROM MPerson TO MPerson);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(0, 19) AS i CREATE (:MPerson {id: i});")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:MPerson), (b:MPerson) WHERE b.id = (a.id + 1) % 20 OR "
                            "b.id = (a.id + 4) % 20 CREATE (a)-[:MKnows]->(b);")
                    ->isSuccess());
    const std::string match = "MATCH (a:MPerson)-[e:MKnows]->(b:MPerson) ";
    const auto computed = match + "WHERE (a.id + offset(id(b))) % 10 = 0 RETURN a.id, COUNT(*)";
    const auto plain = match + "WHERE (a.id + b.id) % 10 = 0 RETURN a.id, COUNT(*)";
    auto explain = [&](const std::string& query) {
        auto res = conn->query("EXPLAIN " + query);
        EXPECT_TRUE(res->isSuccess()) << res->getErrorMessage();
        return res->toString();
    };
    auto counts = [&](const std::string& query) {
        auto res = conn->query(query);
        EXPECT_TRUE(res->isSuccess()) << res->getErrorMessage();
        std::map<int64_t, int64_t> result;
        while (res->hasNext()) {
            const auto tup = res->getNext();
            result[tup->getValue(0)->getValue<int64_t>()] = tup->getValue(1)->getValue<int64_t>();
        }
        return result;
    };

    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=false")->isSuccess());
    const auto expected = counts(computed);
    ASSERT_FALSE(expected.empty());

    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    EXPECT_EQ(std::string::npos, explain(computed).find("PACKED_FILTERED_COUNT"));
    EXPECT_EQ(expected, counts(computed));
    EXPECT_NE(std::string::npos, explain(plain).find("PACKED_FILTERED_COUNT"));
}

TEST_F(CardinalityTest, TestPackedChildSliceState) {
    common::DataChunkState state;
    EXPECT_FALSE(state.hasPackedChildSlices());

    // Single-parent convenience: the parent selection vector holds exactly one position.
    auto singleParentSel = std::make_shared<common::SelectionVector>(1);
    singleParentSel->setToFiltered(1);
    (*singleParentSel)[0] = 3;
    state.setSingleParentPackedChildSlice(singleParentSel, 7);
    ASSERT_TRUE(state.hasPackedChildSlices());
    const auto& singleParentSlices = state.getPackedChildSlices();
    ASSERT_EQ(1, singleParentSlices.getNumParents());
    EXPECT_EQ(3, (*singleParentSlices.parentSelVector)[0]);
    EXPECT_EQ(0, singleParentSlices.offsets[0]);
    EXPECT_EQ(7, singleParentSlices.offsets[1]);
    EXPECT_EQ(7, singleParentSlices.getNumValues());

    // Multi-parent: prefix-sum offsets over ALL parents, zeros allowed. Parent 0 owns output
    // positions [0,2), parent 1 has no children ([2,2)), parent 2 owns [2,5), parent 3 [5,7).
    auto multiParentSel = std::make_shared<common::SelectionVector>(4);
    multiParentSel->setToFiltered(4);
    for (auto i = 0u; i < 4; i++) {
        (*multiParentSel)[i] = i + 1;
    }
    state.setPackedChildSlices(multiParentSel, {0, 2, 2, 5, 7});
    const auto& multiParentSlices = state.getPackedChildSlices();
    ASSERT_EQ(4, multiParentSlices.getNumParents());
    EXPECT_EQ(7, multiParentSlices.getNumValues());
    EXPECT_EQ(2, multiParentSlices.offsets[1] - multiParentSlices.offsets[0]);
    EXPECT_EQ(0, multiParentSlices.offsets[2] - multiParentSlices.offsets[1]);
    EXPECT_EQ(3, multiParentSlices.offsets[3] - multiParentSlices.offsets[2]);
    EXPECT_EQ(2, multiParentSlices.offsets[4] - multiParentSlices.offsets[3]);

    state.clearPackedChildSlices();
    EXPECT_FALSE(state.hasPackedChildSlices());
}

TEST_F(CardinalityTest, TestPackedChildSliceAliasedParentSelection) {
    common::DataChunkState state;
    auto& selVector = state.getSelVectorUnsafe();
    // Simulate the scan: the bound-node (parent) selection vector is filtered with selSize 1,
    // pointing its position 0 at parent row 9.
    selVector.setToFiltered(1);
    selVector[0] = 9;

    // The descriptor ALIASES the parent chunk state's selection vector (shared_ptr keeps the
    // object alive; contents are not snapshotted).
    state.setSingleParentPackedChildSlice(state.getSelVectorShared(), 5);
    ASSERT_TRUE(state.hasPackedChildSlices());
    const auto& slices = state.getPackedChildSlices();
    ASSERT_EQ(1, slices.getNumParents());
    EXPECT_EQ(9, (*slices.parentSelVector)[0]);

    // The bound chunk's selection vector contents are rewritten in place for the next input
    // batch (setToFiltered/setToUnfiltered). The alias observes the rewrite: consumers must
    // finish reading the descriptor synchronously with the output batch. See
    // docs/multi_parent_lifetime.md for the lifetime rule.
    selVector[0] = 42;
    EXPECT_EQ(42, (*slices.parentSelVector)[0]);

    // The shared_ptr keeps the SelectionVector object alive even if the chunk state replaces
    // its selection vector.
    auto replacement = std::make_shared<common::SelectionVector>(1);
    replacement->setToFiltered(1);
    (*replacement)[0] = 7;
    state.setSelVector(replacement);
    EXPECT_EQ(42, (*slices.parentSelVector)[0]);
    EXPECT_EQ(5, slices.getNumValues());
}

} // namespace testing
} // namespace lbug
