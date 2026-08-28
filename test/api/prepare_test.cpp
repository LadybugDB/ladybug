#include "api_test/api_test.h"

using namespace lbug::common;
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
