#include "graph_test/private_graph_test.h"

#include <fstream>

#include "common/exception/test.h"
#include "graph_test/base_graph_test.h"
#include "spdlog/spdlog.h"
#include "storage/storage_manager.h"
#include "test_runner/fsm_leak_checker.h"
#include "test_runner/insert_by_row.h"
#include "test_runner/multi_copy_split.h"
#include "test_runner/test_runner.h"
#include "transaction/transaction_manager.h"
#include <format>

using ::testing::Test;
using namespace lbug::binder;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace testing {

void DBTest::createDB(uint64_t checkpointWaitTimeout) {
    if (database != nullptr) {
        database.reset();
    }
    database = std::make_unique<main::Database>(databasePath, *systemConfig);
    getTransactionManager(*database)->setCheckPointWaitTimeoutForTransactionsToLeaveInMicros(
        checkpointWaitTimeout /* 10ms */);
    spdlog::set_level(spdlog::level::info);
}

void DBTest::createNewDB() {
    database.reset();
    conn.reset();
    removeParentDirectoryOfDBPath(databasePath);
    setDatabasePath();
    createDBAndConn();
}

static void removeFile(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return;
    }
    std::error_code removeErrorCode;
    if (std::filesystem::is_directory(path)) {
        return;
    }
    if (!std::filesystem::remove(path, removeErrorCode)) {
        throw Exception(std::format("Error removing directory {}.  Error Message: {}", path,
            removeErrorCode.message()));
    }
}

static uint64_t readStorageVersionFromHeader(const std::string& databasePath) {
    std::ifstream dbFile{databasePath, std::ios::binary};
    if (!dbFile.is_open()) {
        throw TestException("Failed to open database file: " + databasePath);
    }
    char magic[5];
    dbFile.read(magic, 4);
    magic[4] = '\0';
    if (std::string(magic) != "LBUG") {
        throw TestException("Invalid database magic bytes: " + databasePath);
    }
    uint64_t storageVersion = 0;
    dbFile.read(reinterpret_cast<char*>(&storageVersion), sizeof(storageVersion));
    return storageVersion;
}

static void writeStorageVersionToHeader(const std::string& databasePath, uint64_t storageVersion) {
    std::fstream dbFile{databasePath, std::ios::in | std::ios::out | std::ios::binary};
    if (!dbFile.is_open()) {
        throw TestException("Failed to open database file: " + databasePath);
    }
    char magic[5];
    dbFile.read(magic, 4);
    magic[4] = '\0';
    if (std::string(magic) != "LBUG") {
        throw TestException("Invalid database magic bytes: " + databasePath);
    }
    dbFile.seekp(4);
    dbFile.write(reinterpret_cast<const char*>(&storageVersion), sizeof(storageVersion));
    dbFile.flush();
}

void DBTest::runTest(std::vector<TestStatement>& statements, uint64_t checkpointWaitTimeout,
    std::set<std::string> connNames) {
    for (const auto& connName : connNames) {
        concurrentTests.try_emplace(connName, connectionsPaused, *connMap[connName], databasePath);
    }

    bool skipFsmLeakCheck = false;

    for (auto& statement : statements) {
        // special for testing import and export test cases
        if (statement.removeFileFlag) {
            auto filePath = statement.removeFilePath;
            filePath.erase(std::ranges::remove(filePath, '\"').begin(), filePath.end());
            removeFile(filePath);
            continue;
        }
        if (statement.importDBFlag) {
            auto filePath = statement.importFilePath;
            filePath.erase(std::ranges::remove(filePath, '\"').begin(), filePath.end());
            createNewDB();
            setIEDatabasePath(filePath);
            continue;
        }
        if (statement.reloadDBFlag) {
            // For in-mem mode, we skip reload.
            if (!inMemMode) {
                spdlog::info("QUERY: RELOAD DB");
                for (auto& name : connNames) {
                    if (connMap.contains(name)) {
                        connMap.erase(name);
                    }
                }
                createDB(checkpointWaitTimeout);
                createConns(connNames);
            }
            continue;
        }
        if (statement.expectedStorageVersion.has_value()) {
            if (!inMemMode) {
                ASSERT_EQ(readStorageVersionFromHeader(databasePath),
                    statement.expectedStorageVersion.value());
            }
            continue;
        }
        if (statement.storageVersionToSet.has_value()) {
            if (!inMemMode) {
                conn.reset();
                connMap.clear();
                database.reset();
                writeStorageVersionToHeader(databasePath, statement.storageVersionToSet.value());
                createDB(checkpointWaitTimeout);
                createConns(connNames);
            }
            continue;
        }
        if (statement.connectionsStatusFlag == ConcurrentStatusFlag::BEGIN) {
            for (auto& concurrentTest : concurrentTests) {
                concurrentTest.second.reset();
            }
            isConcurrent = true;
            connectionsPaused = true;
            continue;
        }
        if (statement.connectionsStatusFlag == ConcurrentStatusFlag::END) {
            for (auto& concurrentTest : concurrentTests) {
                concurrentTest.second.execute();
            }
            isConcurrent = false;
            connectionsPaused = false;
            for (auto& concurrentTest : concurrentTests) {
                concurrentTest.second.join();
            }
            continue;
        }
        if (statement.manualUseDataset == ManualUseDatasetFlag::SCHEMA) {
            auto dataset = TestHelper::appendLbugRootPath("dataset/" + statement.dataset);
            if (conn) {
                TestHelper::executeScript(dataset + "/" + TestHelper::SCHEMA_FILE_NAME, *conn);
            } else {
                TestHelper::executeScript(dataset + "/" + TestHelper::SCHEMA_FILE_NAME,
                    *(connMap.begin()->second));
            }
            continue;
        }
        if (statement.manualUseDataset == ManualUseDatasetFlag::INSERT) {
            auto& connection = conn ? *conn : *(connMap.begin()->second);
            InsertDatasetByRow insert(statement.dataset, connection);
            insert.init();
            insert.run();
            continue;
        }
        if (statement.multiCopySplits > 0) {
            auto& connection = conn ? *conn : *(connMap.begin()->second);
            SplitMultiCopyRandom split(statement.multiCopySplits, statement.multiCopyTable,
                statement.multiCopySource, connection);
            if (statement.seed.size() == 2) {
                split.setSeed(statement.seed);
            }
            split.init();
            split.run();
            continue;
        }
        if (statement.skipFSMLeakCheckerFlag) {
            skipFsmLeakCheck = true;
            continue;
        }
        if (statement.type == TestStatementType::LOG) {
            spdlog::info("DEBUG LOG: {}", statement.logMessage);
            continue;
        }
        if (conn) {
            TestRunner::runTest(statement, *conn, databasePath);
        } else {
            if (statement.connName.has_value()) {
                auto connName = statement.connName.value();
                if (isConcurrent) {
                    concurrentTests.at(connName).addStatement(statement);
                } else {
                    TestRunner::runTest(statement, *connMap[connName], databasePath);
                }
            } else {
                throw TestException(
                    std::format("No connection name provided for statement: {}", statement.query));
            }
        }
    }

    // Run FSM checker for all tests.
    if (!inMemMode && !skipFsmLeakCheck) {
        main::Connection* leakConn = nullptr;
        // Run FSM leak check only for single-connection tests.
        if (connNames.size() == 1) {
            // conn (originally null) is populated in the case that the connection in connMap is
            // reset. (ex. during import and export). Use conn in this case.
            leakConn = conn ? conn.get() : connMap.begin()->second.get();
            if (!leakConn) {
                throw TestException("FSM leak check has no available connection.");
            }
            FSMLeakChecker::checkForLeakedPages(leakConn);
        }
    }
}

void ConcurrentTestExecutor::runStatements() const {
    while (connectionPaused) {}
    for (auto statement : statements) {
        TestRunner::runTest(statement, connection, databasePath);
    }
}

} // namespace testing
} // namespace lbug
