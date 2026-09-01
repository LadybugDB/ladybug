#include "api_test/api_test.h"

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::testing;

static void checkTuple(lbug::processor::FlatTuple* tuple, const std::string& groundTruth) {
    ASSERT_STREQ(tuple->toString().c_str(), groundTruth.c_str());
}

TEST_F(ApiTest, issueTest1) {
    conn->query("CREATE NODE TABLE T(id SERIAL, name STRING, PRIMARY KEY(id));");
    conn->query("CREATE (t:T {name: \"foo\"});");
    auto preparedStatement = conn->prepare("MATCH (t:T {id: $p}) return t.name;");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("p"), 0));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "foo\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, issueTest2) {
    conn->query("CREATE NODE TABLE NodeOne(id INT64, name STRING, PRIMARY KEY(id));");
    conn->query("CREATE NODE TABLE NodeTwo(id INT64, name STRING, PRIMARY KEY(id));");
    conn->query("CREATE Rel TABLE RelA(from NodeOne to NodeOne);");
    conn->query("CREATE Rel TABLE RelB(from NodeTwo to NodeOne, name String);");
    conn->query("CREATE (t: NodeOne {id:1, name: \"Alice\"});");
    conn->query("CREATE (t: NodeOne {id:2, name: \"Jack\"});");
    conn->query("CREATE (t: NodeTwo {id:3, name: \"Bob\"});");
    auto preparedStatement = conn->prepare("MATCH (a:NodeOne { id: $a_id }),"
                                           "(b:NodeTwo { id: $b_id }),"
                                           "(c: NodeOne{ id: $c_id } )"
                                           " MERGE"
                                           " (a)-[:RelA]->(c),"
                                           " (b)-[r:RelB { name: $my_param }]->(c)"
                                           " return r.name;");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("a_id"), 1),
        std::make_pair(std::string("b_id"), 3), std::make_pair(std::string("c_id"), 2),
        std::make_pair(std::string("my_param"), "friend"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "friend\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, issueTest) {
    auto preparedStatement = conn->prepare("RETURN $1 + 1;");
    auto result =
        conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), (int8_t)1));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "2\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, MultiParamsPrepare) {
    auto preparedStatement = conn->prepare(
        "MATCH (a:person) WHERE a.fName STARTS WITH $n OR a.fName CONTAINS $xx RETURN COUNT(*)");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("n"), "A"),
        std::make_pair(std::string("xx"), "ooq"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "2\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareBool) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.isStudent = $1 RETURN COUNT(*)");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), true));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "3\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareInt) {
    auto preparedStatement = conn->prepare("MATCH (a:person) WHERE a.age = 35 RETURN a.age + $1");
    auto result =
        conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), (int64_t)10));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "45\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareDouble) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.age = 35 RETURN a.eyeSight + $1");
    auto result =
        conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), (double)10.5));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "15.500000\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareString) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.fName STARTS WITH $n RETURN COUNT(*)");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("n"), "A"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "1\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareDate) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.birthdate = $n RETURN COUNT(*)");
    auto result = conn->execute(preparedStatement.get(),
        std::make_pair(std::string("n"), Date::fromDate(1900, 1, 1)));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "2\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareTimestamp) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.registerTime = $n RETURN COUNT(*)");
    auto date = Date::fromDate(2011, 8, 20);
    auto time = Time::fromTime(11, 25, 30);
    auto result = conn->execute(preparedStatement.get(),
        std::make_pair(std::string("n"), Timestamp::fromDateTime(date, time)));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "1\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareInterval) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.lastJobDuration = $n RETURN COUNT(*)");
    std::string intervalStr = "3 years 2 days 13 hours 2 minutes";
    auto result = conn->execute(preparedStatement.get(),
        std::make_pair(std::string("n"),
            Interval::fromCString(intervalStr.c_str(), intervalStr.length())));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "2\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareDefaultParam) {
    auto preparedStatement = conn->prepare("RETURN to_int8($1)");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "1"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "1\n");
    ASSERT_FALSE(result->hasNext());
    preparedStatement = conn->prepare("RETURN size($1)");
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), 1));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "1\n");
}

TEST_F(ApiTest, PrepareDefaultListParam) {
    auto preparedStatement = conn->prepare("RETURN [1, $1]");
    auto result =
        conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), (int64_t)1));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "[1,1]\n");
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "as"));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_STREQ(result->getErrorMessage().c_str(),
        "Binder exception: Expression $1 has data type STRING but expected INT64. Implicit cast is "
        "not supported.");
    preparedStatement = conn->prepare("RETURN [$1]");
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "as"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "[as]\n");
    preparedStatement = conn->prepare("RETURN [to_int32($1)]");
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "10"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "[10]\n");
}

TEST_F(ApiTest, PrepareDefaultStructParam) {
    auto preparedStatement = conn->prepare("RETURN {a:$1}");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "10"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "{a: 10}\n");
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), 1));
    ASSERT_TRUE(result->isSuccess());
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "{a: 1}\n");
}

TEST_F(ApiTest, PrepareDefaultMapParam) {
    auto preparedStatement = conn->prepare("RETURN map([$1], [$2])");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "10"),
        std::make_pair(std::string("2"), "abc"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "{10=abc}\n");
}

TEST_F(ApiTest, PrepareDefaultUnionParam) {
    auto preparedStatement = conn->prepare("RETURN union_value(a := $1)");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("1"), "10"));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "10\n");
}

TEST_F(ApiTest, PrepareLargeJoin) {
    auto preparedStatement = conn->prepare(
        " MATCH "
        "(:person)-[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person)-"
        "[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person)-[:knows]->"
        "(:person)-[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person)-"
        "[:knows]->(:person)-[:knows]->(:person)-[:knows]->(:person) RETURN COUNT(*)");
    ASSERT_TRUE(preparedStatement->isSuccess());
}

TEST_F(ApiTest, ParamNotExist) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.fName STARTS WITH $n RETURN COUNT(*)");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("a"), "A"));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_STREQ("Parameter n not found.", result->getErrorMessage().c_str());
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("a"), "A"),
        std::make_pair(std::string("n"), "A"));
    ASSERT_TRUE(result->isSuccess());
    ASSERT_STREQ("1\n", result->getNext()->toString().c_str());
}

TEST_F(ApiTest, ParamTypeError) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.fName STARTS WITH $n RETURN COUNT(*)");
    auto result =
        conn->execute(preparedStatement.get(), std::make_pair(std::string("n"), (int64_t)36));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "0\n");
}

TEST_F(ApiTest, MultipleExecutionOfPreparedStatement) {
    auto preparedStatement =
        conn->prepare("MATCH (a:person) WHERE a.fName STARTS WITH $n RETURN a.ID, a.fName");
    auto result = conn->execute(preparedStatement.get(), std::make_pair(std::string("n"), "A"));
    auto groundTruth = std::vector<std::string>{"0|Alice"};
    ASSERT_EQ(groundTruth, TestHelper::convertResultToString(*result));
    result = conn->execute(preparedStatement.get(), std::make_pair(std::string("n"), "B"));
    groundTruth = std::vector<std::string>{"2|Bob"};
    ASSERT_EQ(groundTruth, TestHelper::convertResultToString(*result));
}

TEST_F(ApiTest, issueTest4) {
    auto preparedStatement = conn->prepare("RETURN CAST($1, 'STRING')");
    auto result = conn->execute(preparedStatement.get(),
        std::make_pair(std::string("1"), int128_t((int32_t)-123456789)));
    ASSERT_TRUE(result->hasNext());
    checkTuple(result->getNext().get(), "-123456789\n");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ApiTest, PrepareExport) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    auto newDBPath = TestHelper::getTempDir("export_db") + "/newdb";
    auto preparedStatement = conn->prepare("EXPORT DATABASE '" + newDBPath + '\'');
    auto result = conn->execute(preparedStatement.get());
    ASSERT_TRUE(result->isSuccess());
}

TEST_F(ApiTest, ParameterWith) {
    auto preparedStatement = conn->prepare("WITH $1 AS x RETURN x");
    ASSERT_TRUE(preparedStatement->isSuccess());
    auto result = conn->execute(preparedStatement.get(),
        std::make_pair(std::string("1"), std::string("abc")));
    auto groupTruth = std::vector<std::string>{"abc"};
    ASSERT_EQ(groupTruth, TestHelper::convertResultToString(*result));
}

// Regression tests for re-executing prepared statements whose physical plan is cached.
// The cached-plan fast path used to consume per-execution shared state on the first run,
// so the second and later executions returned empty results for primary-key/index scans
// and stale/truncated results once aggregates were involved.
static void createItemTableWithArtIndex(lbug::main::Connection* conn) {
    ASSERT_TRUE(conn->query("CALL enable_default_hash_index=false")->isSuccess());
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Item(id INT64, name STRING, price DOUBLE, PRIMARY KEY(id))")
            ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE ART INDEX item_id_idx FOR (a:Item) ON (a.id)")->isSuccess());
}

TEST_F(ApiTest, RepeatedExecutePreparedStatementPrimaryKeyScan) {
    createItemTableWithArtIndex(conn.get());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 50000, name: 'x'})")->isSuccess());

    auto preparedStatement = conn->prepare("MATCH (i:Item) WHERE i.id = 50000 RETURN i.name");
    ASSERT_TRUE(preparedStatement->isSuccess());
    auto groundTruth = std::vector<std::string>{"x"};
    for (auto run = 0u; run < 5; run++) {
        auto result = conn->execute(preparedStatement.get());
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(groundTruth,
            TestHelper::convertResultToString(*result, true /* checkOutputOrder */))
            << "run " << run;
    }
}

TEST_F(ApiTest, RepeatedExecutePreparedStatementPrimaryKeyScanWithParams) {
    createItemTableWithArtIndex(conn.get());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 50000, name: 'x'})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1, name: 'y'})")->isSuccess());

    auto preparedStatement = conn->prepare("MATCH (i:Item) WHERE i.id = $id RETURN i.name");
    ASSERT_TRUE(preparedStatement->isSuccess());
    struct ParamCase {
        int64_t id;
        std::vector<std::string> groundTruth;
    };
    const std::vector<ParamCase> cases = {{50000, {"x"}}, {1, {"y"}}, {50000, {"x"}},
        // A key that is not in the index must return an empty result, also on repeats.
        {9999, {}}};
    for (auto run = 0u; run < cases.size(); run++) {
        const auto& paramCase = cases[run];
        auto result =
            conn->execute(preparedStatement.get(), std::make_pair(std::string("id"), paramCase.id));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(paramCase.groundTruth, TestHelper::convertResultToString(*result))
            << "run " << run;
    }
}

TEST_F(ApiTest, RepeatedExecutePreparedStatementAggregateOverIndexScan) {
    createItemTableWithArtIndex(conn.get());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1, name: 'a', price: 1.0})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 2, name: 'b', price: 2.0})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 3, name: 'c', price: 3.0})")->isSuccess());

    auto countPS = conn->prepare("MATCH (i:Item) WHERE i.id >= 1 AND i.id <= 3 RETURN COUNT(*)");
    ASSERT_TRUE(countPS->isSuccess());
    auto rangePS = conn->prepare("MATCH (i:Item) WHERE i.id >= 2 AND i.id <= 1000 RETURN i.id");
    ASSERT_TRUE(rangePS->isSuccess());
    auto countGroundTruth = std::vector<std::string>{"3"};
    auto rangeGroundTruth = std::vector<std::string>{"2", "3"};
    for (auto run = 0u; run < 5; run++) {
        auto result = conn->execute(countPS.get());
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(countGroundTruth, TestHelper::convertResultToString(*result)) << "run " << run;

        result = conn->execute(rangePS.get());
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(rangeGroundTruth,
            TestHelper::convertResultToString(*result, true /* checkOutputOrder */))
            << "run " << run;
    }
}

TEST_F(ApiTest, RepeatedExecutePreparedStatementGroupByDistinctOrderBy) {
    createItemTableWithArtIndex(conn.get());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1, name: 'a', price: 1.0})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 2, name: 'b', price: 2.0})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 3, name: 'a', price: 3.0})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 4, name: 'b', price: 4.0})")->isSuccess());

    auto groupByPS = conn->prepare("MATCH (i:Item) RETURN i.name, COUNT(*) AS c ORDER BY i.name");
    ASSERT_TRUE(groupByPS->isSuccess());
    auto distinctPS = conn->prepare("MATCH (i:Item) RETURN DISTINCT i.name ORDER BY i.name");
    ASSERT_TRUE(distinctPS->isSuccess());
    auto orderByLimitPS = conn->prepare("MATCH (i:Item) RETURN i.id ORDER BY i.id DESC LIMIT 3");
    ASSERT_TRUE(orderByLimitPS->isSuccess());

    auto groupByGroundTruth = std::vector<std::string>{"a|2", "b|2"};
    auto distinctGroundTruth = std::vector<std::string>{"a", "b"};
    auto orderByLimitGroundTruth = std::vector<std::string>{"4", "3", "2"};
    // Interleave different prepared statements to exercise descriptor reuse across plans.
    for (auto run = 0u; run < 5; run++) {
        auto result = conn->execute(groupByPS.get());
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(groupByGroundTruth,
            TestHelper::convertResultToString(*result, true /* checkOutputOrder */))
            << "run " << run;

        result = conn->execute(distinctPS.get());
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(distinctGroundTruth,
            TestHelper::convertResultToString(*result, true /* checkOutputOrder */))
            << "run " << run;

        result = conn->execute(orderByLimitPS.get());
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(orderByLimitGroundTruth,
            TestHelper::convertResultToString(*result, true /* checkOutputOrder */))
            << "run " << run;
    }
}

// Regression test for https://github.com/LadybugDB/ladybug/issues/849: closing/destroying a
// prepared statement must release its entry in CachedPreparedStatementManager (parsed
// statement, logical plan and cached physical plan). Before the fix every successful
// prepare() permanently registered the entry and memory grew with every prepare() call.
TEST_F(ApiTest, PreparedStatementCloseUnregistersCachedPlan) {
    auto& manager = conn->getClientContext()->getCachedPreparedStatementManager();
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE T849 (id INT64 PRIMARY KEY, name STRING)")->isSuccess());

    constexpr auto numCycles = 100;
    std::vector<std::string> names;
    names.reserve(numCycles);
    for (auto i = 0u; i < numCycles; i++) {
        auto preparedStatement = conn->prepare("MATCH (t:T849) WHERE t.id = $id RETURN t.name");
        ASSERT_TRUE(preparedStatement->isSuccess());
        const auto name = preparedStatement->getName();
        // The statement is registered while it is alive.
        ASSERT_TRUE(manager.containsStatement(name)) << "cycle " << i;
        auto result =
            conn->execute(preparedStatement.get(), std::make_pair(std::string("id"), (int64_t)i));
        ASSERT_TRUE(result->isSuccess()) << "cycle " << i;
        names.push_back(name);
        // Destroying the statement must unregister (free) its cached plan ...
        preparedStatement.reset();
        ASSERT_FALSE(manager.containsStatement(name)) << "cycle " << i;
    }
    // ... and none of the entries from earlier cycles may have accumulated.
    for (auto& name : names) {
        ASSERT_FALSE(manager.containsStatement(name));
    }

    // Statements that are prepared but never executed must be released as well.
    auto preparedStatement = conn->prepare("RETURN $1 + 1");
    ASSERT_TRUE(preparedStatement->isSuccess());
    const auto name = preparedStatement->getName();
    ASSERT_TRUE(manager.containsStatement(name));
    preparedStatement.reset();
    ASSERT_FALSE(manager.containsStatement(name));
}

TEST_F(ApiTest, FailedPrepareIsReleasedOnDestroy) {
    auto& manager = conn->getClientContext()->getCachedPreparedStatementManager();

    // Binder errors: the (mostly empty) cached statement is still registered, so destroying
    // the failed statement must unregister it.
    auto preparedStatement = conn->prepare("MATCH (n:NoSuchTable849) RETURN n");
    ASSERT_FALSE(preparedStatement->isSuccess());
    ASSERT_FALSE(preparedStatement->getName().empty());
    const auto name = preparedStatement->getName();
    preparedStatement.reset();
    ASSERT_FALSE(manager.containsStatement(name));

    // Parse errors return an unregistered error statement (empty name); destroying it must
    // not touch the manager at all.
    auto parseErrorStatement = conn->prepare("THIS IS NOT CYPHER");
    ASSERT_FALSE(parseErrorStatement->isSuccess());
    ASSERT_TRUE(parseErrorStatement->getName().empty());
    parseErrorStatement.reset();
}

// Exercise the thread-local ResultSet reuse path with variable-length data: with the cached
// plan (and cached ResultSet) reused across executions, string overflow buffers and null
// masks from a prior execution must not leak into later ones.
TEST_F(ApiTest, RepeatedExecutePreparedStatementVariableLengthResults) {
    createItemTableWithArtIndex(conn.get());
    // The long names force the use of out-of-line string overflow buffers.
    const std::string longNameA(120, 'a');
    const std::string longNameB(90, 'b');
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1, name: 'short', price: 1.0})")->isSuccess());
    ASSERT_TRUE(
        conn->query("CREATE (:Item {id: 2, name: '" + longNameA + "', price: 2.5})")->isSuccess());
    // No price: exercises null-mask reset across executions.
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 3, name: 'with_null_price'})")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 4, name: '" + longNameB + "'})")->isSuccess());

    auto preparedStatement =
        conn->prepare("MATCH (i:Item) WHERE i.id = $id RETURN i.name, i.price");
    ASSERT_TRUE(preparedStatement->isSuccess());

    struct ParamCase {
        int64_t id;
        std::vector<std::string> groundTruth;
    };
    const std::vector<ParamCase> cases = {{2, {longNameA + "|2.500000"}}, {1, {"short|1.000000"}},
        {3, {"with_null_price|"}}, {4, {longNameB + "|"}}, {2, {longNameA + "|2.500000"}},
        {1, {"short|1.000000"}}};
    for (auto run = 0u; run < cases.size(); run++) {
        const auto& paramCase = cases[run];
        auto result =
            conn->execute(preparedStatement.get(), std::make_pair(std::string("id"), paramCase.id));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(paramCase.groundTruth, TestHelper::convertResultToString(*result))
            << "run " << run;
    }
}

// Regression test for https://github.com/LadybugDB/ladybug/issues/862: re-executing the
// prepared statement of the same parameterized write query string (which takes the cached
// physical-plan fast path once the statement was prepared WITH its parameters) crashed with
// SIGSEGV. The root ResultCollector of a write statement has an empty result schema, so its
// FactorizedTable never allocates block collections or an overflow buffer; clear() in
// prepareForReuse() dereferenced the null collection.
static std::unordered_map<std::string, std::unique_ptr<Value>> makeIdValueParams(int64_t id,
    std::string val) {
    std::unordered_map<std::string, std::unique_ptr<Value>> params;
    params["id"] = std::make_unique<Value>(id);
    params["val"] = std::make_unique<Value>(std::move(val));
    return params;
}

TEST_F(ApiTest, RepeatedExecuteCachedPlanParameterizedWrite) {
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Log(id INT64, value STRING, PRIMARY KEY(id))")->isSuccess());

    // Parameterized CREATE, same statement executed repeatedly (fast path on every run but
    // the first).
    const std::string createQuery = "CREATE (:Log {id: $id, value: $val})";
    auto createStmt = conn->prepareWithParams(createQuery, makeIdValueParams(1, "a"));
    ASSERT_TRUE(createStmt->isSuccess());
    for (auto run = 0u; run < 3; run++) {
        auto result = conn->executeWithParams(createStmt.get(),
            makeIdValueParams(run + 1, "v" + std::to_string(run)));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
    }

    // Parameterized MATCH ... SET, same statement executed repeatedly.
    const std::string setQuery = "MATCH (l:Log) WHERE l.id = $id SET l.value = $val";
    auto setStmt = conn->prepareWithParams(setQuery, makeIdValueParams(1, "x"));
    ASSERT_TRUE(setStmt->isSuccess());
    for (auto run = 0u; run < 2; run++) {
        auto result = conn->executeWithParams(setStmt.get(),
            makeIdValueParams(run + 1, "x" + std::to_string(run)));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
    }

    // Same string writes inside an explicit transaction.
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    auto txStmt = conn->prepareWithParams(createQuery, makeIdValueParams(10, "t1"));
    ASSERT_TRUE(txStmt->isSuccess());
    for (auto run = 0u; run < 2; run++) {
        auto result = conn->executeWithParams(txStmt.get(),
            makeIdValueParams(10 + run, "t" + std::to_string(run)));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
    }
    ASSERT_TRUE(conn->query("COMMIT")->isSuccess());

    ASSERT_EQ(std::vector<std::string>{"5"},
        TestHelper::convertResultToString(*conn->query("MATCH (l:Log) RETURN COUNT(l)")));
}

TEST_F(ApiTest, RepeatedExecuteCachedPlanParameterizedRead) {
    createItemTableWithArtIndex(conn.get());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1, name: 'a', price: 1.0})")->isSuccess());

    // Reads through the fast path must keep working too (they were unaffected by #862, but
    // guard the empty-schema fix against regressing the non-empty-schema case).
    auto readStmt = conn->prepareWithParams("MATCH (i:Item) WHERE i.id = $id RETURN i.name",
        makeIdValueParams(1, "a"));
    ASSERT_TRUE(readStmt->isSuccess());
    for (auto run = 0u; run < 3; run++) {
        auto result = conn->executeWithParams(readStmt.get(), makeIdValueParams(1, "a"));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(std::vector<std::string>{"a"}, TestHelper::convertResultToString(*result))
            << "run " << run;
    }
}

// Regression test for issue #877: re-executing the same parameterized query string (the
// recommended form, which reuses the cached physical plan) returned the first execution's
// rows whenever the plan contained a sort, top-k, join, OPTIONAL MATCH, UNION, subquery or
// a LIMIT/SKIP counter. Root causes: operator copy() dropped sub-pipelines from the cached
// plan tree, and shared states kept per-execution state across executions.
TEST_F(ApiTest, RepeatedParameterizedCachedPlanExecution877) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE N(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE M(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE E(FROM N TO M);")->isSuccess());
    for (auto i = 1; i <= 3; ++i) {
        auto id = std::to_string(i);
        ASSERT_TRUE(conn->query("CREATE (:N {id: " + id + "});")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:M {id: " + id + "});")->isSuccess());
        ASSERT_TRUE(conn->query("MATCH (a:N {id: " + id + "}), (b:M {id: " + id +
                                "}) CREATE (a)-[:E]->(b);")
                        ->isSuccess());
    }

    // Mirrors the recommended Python usage: execute(query, params) prepares the query string
    // and executes it with parameters each time, taking the cached-physical-plan fast path
    // from the second execution on.
    // Mirrors the recommended Python usage: execute(query, params) implicitly prepares the
    // query WITH typed parameters once, then re-executes the same prepared statement with
    // different parameter values, taking the cached-physical-plan fast path from the second
    // execution on.
    auto prepareAndExecute = [&](const std::string& query, int64_t v) {
        static std::unordered_map<std::string, std::unique_ptr<PreparedStatement>> cache;
        auto it = cache.find(query);
        if (it == cache.end()) {
            std::unordered_map<std::string, std::unique_ptr<Value>> prepareParams;
            prepareParams["v"] = std::make_unique<Value>(v);
            auto prepared = conn->prepareWithParams(query, std::move(prepareParams));
            EXPECT_TRUE(prepared->isSuccess()) << query;
            it = cache.emplace(query, std::move(prepared)).first;
        }
        std::unordered_map<std::string, std::unique_ptr<Value>> params;
        params["v"] = std::make_unique<Value>(v);
        return conn->executeWithParams(it->second.get(), std::move(params));
    };

    // {query, expected result rows for v = 1, 2, 3}
    const std::vector<std::tuple<std::string, std::vector<std::vector<std::string>>>> cases = {
        {"MATCH (n:N) WHERE n.id = $v RETURN count(n)", {{"1"}, {"1"}, {"1"}}},
        {"MATCH (n:N) WHERE n.id = $v RETURN n.id ORDER BY n.id", {{"1"}, {"2"}, {"3"}}},
        {"MATCH (n:N) WHERE n.id <= $v RETURN n.id ORDER BY n.id LIMIT 2",
            {{"1"}, {"1", "2"}, {"1", "2"}}},
        {"MATCH (n:N) WHERE n.id = $v RETURN n.id ORDER BY n.id LIMIT 1", {{"1"}, {"2"}, {"3"}}},
        {"MATCH (a:N)-[:E]->(b:M) WHERE a.id = $v RETURN b.id", {{"1"}, {"2"}, {"3"}}},
        {"MATCH (a:N), (b:M) WHERE a.id = $v AND b.id = $v RETURN b.id", {{"1"}, {"2"}, {"3"}}},
        {"MATCH (n:N) WHERE n.id = $v OPTIONAL MATCH (n)-[:E]->(m) RETURN n.id",
            {{"1"}, {"2"}, {"3"}}},
        {"MATCH (n:N) WHERE n.id = $v RETURN n.id UNION ALL MATCH (m:M) WHERE m.id = $v "
         "RETURN m.id",
            {{"1", "1"}, {"2", "2"}, {"3", "3"}}},
        {"MATCH (n:N) WHERE n.id = $v AND EXISTS { MATCH (n)-[:E]->(m) } RETURN n.id",
            {{"1"}, {"2"}, {"3"}}},
        {"MATCH (a:N)-[:E*1..2]->(b:M) WHERE a.id = $v RETURN b.id", {{"1"}, {"2"}, {"3"}}},
    };
    for (auto& [query, expectedPerV] : cases) {
        for (auto v = 1; v <= 3; ++v) {
            // First execution populates the plan cache; the rest exercise the fast path.
            prepareAndExecute(query, v);
            auto result = prepareAndExecute(query, v);
            ASSERT_TRUE(result->isSuccess()) << query;
            ASSERT_EQ(expectedPerV[v - 1], TestHelper::convertResultToString(*result))
                << query << " with v=" << v;
        }
    }
}

// Returns true iff the cached physical plan of the prepared statement named `ps.getName()`
// has been populated (i.e. the statement is allowed to take the cached-plan fast path).
static bool cachedPlanExists(Connection* conn, const PreparedStatement& ps) {
    const auto& manager = conn->getClientContext()->getCachedPreparedStatementManager();
    if (!manager.containsStatement(ps.getName())) {
        return false;
    }
    return manager.getCachedStatement(ps.getName())->physicalPlanCache != nullptr;
}

// Regression coverage for the `enable_cached_prepared_statement` setting: a kill switch for
// latent state-reuse bugs in the cached-physical-plan fast path (see issue #877 and friends).
TEST_F(ApiTest, EnableCachedPreparedStatementSetting) {
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Log(id INT64, value STRING, PRIMARY KEY(id))")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Log {id: 1, value: 'a'})")->isSuccess());
    const std::string readQuery = "MATCH (l:Log) WHERE l.id = $id RETURN l.value";
    const std::string writeQuery = "CREATE (:Log {id: $id, value: $val})";

    // The setting round-trips and rejects invalid values.
    ASSERT_TRUE(conn->query("CALL enable_cached_prepared_statement='reads';")->isSuccess());
    ASSERT_EQ(std::vector<std::string>{"READS"},
        TestHelper::convertResultToString(
            *conn->query("CALL current_setting('enable_cached_prepared_statement') RETURN *")));
    ASSERT_FALSE(conn->query("CALL enable_cached_prepared_statement='banana';")->isSuccess());
    // The default is BOTH: read and write statements both populate the plan cache.
    ASSERT_TRUE(conn->query("CALL enable_cached_prepared_statement='both';")->isSuccess());
    ASSERT_EQ(std::vector<std::string>{"BOTH"},
        TestHelper::convertResultToString(
            *conn->query("CALL current_setting('enable_cached_prepared_statement') RETURN *")));
    auto readStmt = conn->prepareWithParams(readQuery, makeIdValueParams(1, "a"));
    ASSERT_TRUE(readStmt->isSuccess());
    for (auto run = 0; run < 2; ++run) {
        auto result = conn->executeWithParams(readStmt.get(), makeIdValueParams(1, "a"));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(std::vector<std::string>{"a"}, TestHelper::convertResultToString(*result))
            << "run " << run;
    }
    ASSERT_TRUE(cachedPlanExists(conn.get(), *readStmt));

    auto writeStmt = conn->prepareWithParams(writeQuery, makeIdValueParams(100, "w0"));
    ASSERT_TRUE(writeStmt->isSuccess());
    for (auto run = 0; run < 2; ++run) {
        auto result = conn->executeWithParams(writeStmt.get(),
            makeIdValueParams(100 + run, "w" + std::to_string(run)));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
    }
    ASSERT_TRUE(cachedPlanExists(conn.get(), *writeStmt));

    // READS: read plans are cached, write plans are not.
    ASSERT_TRUE(conn->query("CALL enable_cached_prepared_statement='reads';")->isSuccess());
    auto writeStmtReads = conn->prepareWithParams(writeQuery, makeIdValueParams(200, "r0"));
    ASSERT_TRUE(writeStmtReads->isSuccess());
    for (auto run = 0; run < 2; ++run) {
        auto result = conn->executeWithParams(writeStmtReads.get(),
            makeIdValueParams(200 + run, "r" + std::to_string(run)));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
    }
    ASSERT_FALSE(cachedPlanExists(conn.get(), *writeStmtReads));

    auto readStmtReads = conn->prepareWithParams(readQuery, makeIdValueParams(1, "a"));
    ASSERT_TRUE(readStmtReads->isSuccess());
    auto result = conn->executeWithParams(readStmtReads.get(), makeIdValueParams(1, "a"));
    ASSERT_TRUE(result->isSuccess());
    ASSERT_EQ(std::vector<std::string>{"a"}, TestHelper::convertResultToString(*result));
    ASSERT_TRUE(cachedPlanExists(conn.get(), *readStmtReads));

    // WRITES: write plans are cached, read plans are not.
    ASSERT_TRUE(conn->query("CALL enable_cached_prepared_statement='writes';")->isSuccess());
    auto readStmtWrites = conn->prepareWithParams(readQuery, makeIdValueParams(1, "a"));
    ASSERT_TRUE(readStmtWrites->isSuccess());
    result = conn->executeWithParams(readStmtWrites.get(), makeIdValueParams(1, "a"));
    ASSERT_TRUE(result->isSuccess());
    ASSERT_EQ(std::vector<std::string>{"a"}, TestHelper::convertResultToString(*result));
    ASSERT_FALSE(cachedPlanExists(conn.get(), *readStmtWrites));

    auto writeStmtWrites = conn->prepareWithParams(writeQuery, makeIdValueParams(300, "s0"));
    ASSERT_TRUE(writeStmtWrites->isSuccess());
    for (auto run = 0; run < 2; ++run) {
        result = conn->executeWithParams(writeStmtWrites.get(),
            makeIdValueParams(300 + run, "s" + std::to_string(run)));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
    }
    ASSERT_TRUE(cachedPlanExists(conn.get(), *writeStmtWrites));

    // NONE: no plan caching at all, but repeated executions still return correct results.
    ASSERT_TRUE(conn->query("CALL enable_cached_prepared_statement='none';")->isSuccess());
    auto readStmtNone = conn->prepareWithParams(readQuery, makeIdValueParams(1, "a"));
    ASSERT_TRUE(readStmtNone->isSuccess());
    for (auto run = 0; run < 3; ++run) {
        result = conn->executeWithParams(readStmtNone.get(), makeIdValueParams(1, "a"));
        ASSERT_TRUE(result->isSuccess()) << "run " << run;
        ASSERT_EQ(std::vector<std::string>{"a"}, TestHelper::convertResultToString(*result))
            << "run " << run;
    }
    ASSERT_FALSE(cachedPlanExists(conn.get(), *readStmtNone));

    // Restore the default.
    ASSERT_TRUE(conn->query("CALL enable_cached_prepared_statement='both';")->isSuccess());
}

// A predicate built only from parameters reads nothing from the input, so it holds for every row
// or for none. It used to hold for every row regardless: the filter narrowed the shared
// single-value data chunk state rather than the chunk the rows travel in, so it discarded nothing
// and the predicate was silently absent. The equivalent literal predicate is folded by the binder
// and so never reached that path, which is why only the parameterised form was affected.
TEST_F(ApiTest, ParameterOnlyPredicateFilters) {
    auto falsePredicate = conn->prepare("MATCH (a:person) WHERE $depth >= 2 RETURN a.ID;");
    ASSERT_TRUE(falsePredicate->isSuccess()) << falsePredicate->getErrorMessage();
    auto excluded =
        conn->execute(falsePredicate.get(), std::make_pair(std::string("depth"), (int64_t)1));
    ASSERT_TRUE(excluded->isSuccess()) << excluded->getErrorMessage();
    ASSERT_EQ(excluded->getNumTuples(), 0u);

    // The same statement with a value that satisfies the predicate must still return everything.
    auto included =
        conn->execute(falsePredicate.get(), std::make_pair(std::string("depth"), (int64_t)5));
    ASSERT_TRUE(included->isSuccess()) << included->getErrorMessage();
    ASSERT_EQ(included->getNumTuples(), 8u);

    // Mixed with a predicate that does read the input, in both conjunct orders.
    auto mixed = conn->prepare("MATCH (a:person) WHERE $depth >= 2 AND a.ID >= 0 RETURN a.ID;");
    ASSERT_TRUE(mixed->isSuccess()) << mixed->getErrorMessage();
    auto mixedResult = conn->execute(mixed.get(), std::make_pair(std::string("depth"), (int64_t)1));
    ASSERT_TRUE(mixedResult->isSuccess()) << mixedResult->getErrorMessage();
    ASSERT_EQ(mixedResult->getNumTuples(), 0u);

    auto mixedReversed =
        conn->prepare("MATCH (a:person) WHERE a.ID >= 0 AND $depth >= 2 RETURN a.ID;");
    ASSERT_TRUE(mixedReversed->isSuccess()) << mixedReversed->getErrorMessage();
    auto mixedReversedResult =
        conn->execute(mixedReversed.get(), std::make_pair(std::string("depth"), (int64_t)1));
    ASSERT_TRUE(mixedReversedResult->isSuccess()) << mixedReversedResult->getErrorMessage();
    ASSERT_EQ(mixedReversedResult->getNumTuples(), 0u);

    // A bare boolean parameter, with no comparison to fold around.
    auto boolParam = conn->prepare("MATCH (a:person) WHERE $flag RETURN a.ID;");
    ASSERT_TRUE(boolParam->isSuccess()) << boolParam->getErrorMessage();
    auto boolResult = conn->execute(boolParam.get(), std::make_pair(std::string("flag"), false));
    ASSERT_TRUE(boolResult->isSuccess()) << boolResult->getErrorMessage();
    ASSERT_EQ(boolResult->getNumTuples(), 0u);
}

// The same predicate placed after a WITH that projects only a constant. This gated a MATCH below
// it and used to terminate the process rather than return rows.
TEST_F(ApiTest, ParameterOnlyPredicateAfterConstantWith) {
    auto statement =
        conn->prepare("WITH 1 AS gate WHERE $depth >= 2 MATCH (a:person) RETURN a.ID;");
    ASSERT_TRUE(statement->isSuccess()) << statement->getErrorMessage();
    auto excluded =
        conn->execute(statement.get(), std::make_pair(std::string("depth"), (int64_t)1));
    ASSERT_TRUE(excluded->isSuccess()) << excluded->getErrorMessage();
    ASSERT_EQ(excluded->getNumTuples(), 0u);

    auto included =
        conn->execute(statement.get(), std::make_pair(std::string("depth"), (int64_t)5));
    ASSERT_TRUE(included->isSuccess()) << included->getErrorMessage();
    ASSERT_EQ(included->getNumTuples(), 8u);
}
