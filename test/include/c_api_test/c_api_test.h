#pragma once

#include "c_api/lbug.h"
#include "graph_test/base_graph_test.h"

namespace lbug {
namespace testing {

// This class starts database in on-disk mode.
class APIDBTest : public BaseGraphTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp();
        createDBAndConn();
        initGraph();
    }
};

class CApiTest : public APIDBTest {
public:
    lbug_database _database;
    lbug_connection connection;

    void SetUp() override {
        APIDBTest::SetUp();
        auto* connCppPointer = conn.release();
        auto* databaseCppPointer = database.release();
        connection = lbug_connection{connCppPointer};
        _database = lbug_database{databaseCppPointer};
    }

    std::string getDatabasePath() { return databasePath; }

    lbug_database* getDatabase() { return &_database; }

    lbug_connection* getConnection() { return &connection; }

    void TearDown() override {
        lbug_connection_destroy(&connection);
        lbug_database_destroy(&_database);
        APIDBTest::TearDown();
    }
};

} // namespace testing
} // namespace lbug
