#include "c_api/lbug.h"
#include "graph_test/base_graph_test.h"
#include "gtest/gtest.h"

using namespace lbug::main;
using namespace lbug::testing;

// This class starts database without initializing graph.
class APIEmptyDBTest : public BaseGraphTest {
    std::string getInputDir() override { KU_UNREACHABLE; }
};

class CApiDatabaseTest : public APIEmptyDBTest {
public:
    void SetUp() override {
        APIEmptyDBTest::SetUp();
        defaultSystemConfig = lbug_default_system_config();

        // limit memory usage by keeping max number of threads small
        defaultSystemConfig.max_num_threads = 2;
        auto maxDBSizeEnv = TestHelper::getSystemEnv("MAX_DB_SIZE");
        if (!maxDBSizeEnv.empty()) {
            defaultSystemConfig.max_db_size = std::stoull(maxDBSizeEnv);
        }
    }

    lbug_system_config defaultSystemConfig;
};

TEST_F(CApiDatabaseTest, CreationAndDestroy) {
    lbug_database database;
    lbug_state state;
    auto databasePathCStr = databasePath.c_str();
    auto systemConfig = defaultSystemConfig;
    state = lbug_database_init(databasePathCStr, systemConfig, &database);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(database._database, nullptr);
    auto databaseCpp = static_cast<Database*>(database._database);
    ASSERT_NE(databaseCpp, nullptr);
    lbug_database_destroy(&database);
}

TEST_F(CApiDatabaseTest, CreationReadOnly) {
    lbug_database database;
    lbug_connection connection;
    lbug_query_result queryResult;
    lbug_state state;
    auto databasePathCStr = databasePath.c_str();
    auto systemConfig = defaultSystemConfig;
    // First, create a read-write database.
    state = lbug_database_init(databasePathCStr, systemConfig, &database);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(database._database, nullptr);
    auto databaseCpp = static_cast<Database*>(database._database);
    ASSERT_NE(databaseCpp, nullptr);
    lbug_database_destroy(&database);
    // Now, access the same database read-only.
    systemConfig.read_only = true;
    state = lbug_database_init(databasePathCStr, systemConfig, &database);
    if (databasePath == "" || databasePath == ":memory:") {
        ASSERT_EQ(state, LbugError);
        ASSERT_EQ(database._database, nullptr);
        return;
    }
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(database._database, nullptr);
    databaseCpp = static_cast<Database*>(database._database);
    ASSERT_NE(databaseCpp, nullptr);
    // Try to write to the database.
    state = lbug_connection_init(&database, &connection);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_connection_query(&connection,
        "CREATE NODE TABLE User(name STRING, age INT64, reg_date DATE, PRIMARY KEY (name))",
        &queryResult);
    ASSERT_EQ(state, LbugError);
    ASSERT_FALSE(lbug_query_result_is_success(&queryResult));
    lbug_query_result_destroy(&queryResult);
    lbug_connection_destroy(&connection);
    lbug_database_destroy(&database);
}

TEST_F(CApiDatabaseTest, CreationInMemory) {
    lbug_database database;
    lbug_state state;
    auto databasePathCStr = (char*)"";
    state = lbug_database_init(databasePathCStr, defaultSystemConfig, &database);
    ASSERT_EQ(state, LbugSuccess);
    lbug_database_destroy(&database);
    databasePathCStr = (char*)":memory:";
    state = lbug_database_init(databasePathCStr, defaultSystemConfig, &database);
    ASSERT_EQ(state, LbugSuccess);
    lbug_database_destroy(&database);
}

#ifndef __WASM__ // home directory is not available in WASM
TEST_F(CApiDatabaseTest, CreationHomeDir) {
    lbug_database database;
    lbug_connection connection;
    lbug_state state;
    auto databasePathCStr = (char*)"~/ku_test.db";
    state = lbug_database_init(databasePathCStr, defaultSystemConfig, &database);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_connection_init(&database, &connection);
    ASSERT_EQ(state, LbugSuccess);
    auto homePath =
        getClientContext(*(Connection*)(connection._connection))->getClientConfig()->homeDirectory;
    lbug_connection_destroy(&connection);
    lbug_database_destroy(&database);
    std::filesystem::remove_all(homePath + "/ku_test.db");
}
#endif

TEST_F(CApiDatabaseTest, CloseQueryResultAndConnectionAfterDatabaseDestroy) {
    lbug_database database;
    auto databasePathCStr = (char*)":memory:";
    auto systemConfig = lbug_default_system_config();
    systemConfig.buffer_pool_size = 10 * 1024 * 1024; // 10MB
    systemConfig.max_db_size = 1 << 30;               // 1GB
    systemConfig.max_num_threads = 2;
    lbug_state state = lbug_database_init(databasePathCStr, systemConfig, &database);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(database._database, nullptr);
    lbug_connection conn;
    lbug_query_result queryResult;
    state = lbug_connection_init(&database, &conn);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_connection_query(&conn, "RETURN 1+1", &queryResult);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&queryResult));
    lbug_flat_tuple tuple;
    lbug_state resultState = lbug_query_result_get_next(&queryResult, &tuple);
    ASSERT_EQ(resultState, LbugSuccess);
    lbug_value value;
    lbug_state valueState = lbug_flat_tuple_get_value(&tuple, 0, &value);
    ASSERT_EQ(valueState, LbugSuccess);
    int64_t valueInt = INT64_MAX;
    lbug_state valueIntState = lbug_value_get_int64(&value, &valueInt);
    ASSERT_EQ(valueIntState, LbugSuccess);
    ASSERT_EQ(valueInt, 2);
    // Destroy database first, this should not crash
    lbug_database_destroy(&database);
    // Call lbug_connection_query should not crash, but return an error
    state = lbug_connection_query(&conn, "RETURN 1+1", &queryResult);
    ASSERT_EQ(state, LbugError);
    // Call lbug_query_result_get_next should not crash, but return an error
    resultState = lbug_query_result_get_next(&queryResult, &tuple);
    ASSERT_EQ(resultState, LbugError);
    // Now destroy everything, this should not crash
    lbug_query_result_destroy(&queryResult);
    lbug_connection_destroy(&conn);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&tuple);
}

TEST_F(CApiDatabaseTest, UseConnectionAfterDatabaseDestroy) {
    lbug_database db;
    lbug_connection conn;
    lbug_query_result result;

    auto systemConfig = lbug_default_system_config();
    systemConfig.buffer_pool_size = 10 * 1024 * 1024; // 10MB
    systemConfig.max_db_size = 1 << 30;               // 1GB
    systemConfig.max_num_threads = 2;
    auto state = lbug_database_init("", systemConfig, &db);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_connection_init(&db, &conn);
    ASSERT_EQ(state, LbugSuccess);
    lbug_database_destroy(&db);
    state = lbug_connection_query(&conn, "RETURN 0", &result);
    ASSERT_EQ(state, LbugError);

    lbug_connection_destroy(&conn);
}
