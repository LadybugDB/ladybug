#include <algorithm>
#include <filesystem>
#include <fstream>

#include "common/constants.h"
#include "common/file_system/local_file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "graph_test/base_graph_test.h"
#include "main/database.h"
#include "storage/buffer_manager/buffer_manager.h"
#include "storage/checkpointer.h"
#include "storage/storage_manager.h"
#include "test_runner/fsm_leak_checker.h"
#include "test_runner/test_parser.h"
#include "transaction/transaction_manager.h"
#include <format>

using ::testing::TestParamInfo;
using ::testing::Values;
using ::testing::WithParamInterface;

namespace lbug {
namespace testing {

class CopyTestHelper {
public:
    static std::vector<std::unique_ptr<storage::FileHandle>>& getBMFileHandles(
        storage::BufferManager* bm) {
        return bm->fileHandles;
    }
};

class FlakyBufferManager : public storage::BufferManager {
public:
    FlakyBufferManager(const std::string& databasePath, const std::string& spillToDiskPath,
        uint64_t bufferPoolSize, uint64_t maxDBSize, common::VirtualFileSystem* vfs, bool readOnly,
        std::atomic<uint64_t>& failureFrequency, bool canFailDuringExecute,
        bool canFailDuringCheckpoint, bool canFailDuringCommit)
        : storage::BufferManager(databasePath, spillToDiskPath, bufferPoolSize, maxDBSize, vfs,
              readOnly),
          failureFrequency(failureFrequency), canFailDuringCheckpoint(canFailDuringCheckpoint),
          canFailDuringExecute(canFailDuringExecute), canFailDuringCommit(canFailDuringCommit) {}

    bool reserve(uint64_t sizeToReserve) override {
        // we currently can't handle exceptions thrown during rollback
        const bool inRollback = std::current_exception().operator bool();

        const bool inDBInit = ctx == nullptr;

        const bool inCheckpoint =
            ctx && !transaction::TransactionManager::Get(*ctx)->hasActiveWriteTransactionNoLock();
        const bool inCommit =
            !inCheckpoint && ctx && transaction::Transaction::Get(*ctx) &&
            transaction::Transaction::Get(*ctx)->getCommitTS() != common::INVALID_TRANSACTION;
        const bool inExecute = (!inCommit && !inCheckpoint);
        reserveCount = (reserveCount + 1) % failureFrequency;
        if (!inRollback && !inDBInit && (canFailDuringCommit || !inCommit) &&
            (canFailDuringCheckpoint || !inCheckpoint) && (canFailDuringExecute || !inExecute) &&
            reserveCount == 0) {
            failureFrequency = failureFrequency * 2;
            return false;
        }
        return storage::BufferManager::reserve(sizeToReserve);
    }

    void setClientContext(main::ClientContext* newCtx) { ctx = newCtx; }

    std::atomic<uint64_t>& failureFrequency;
    main::ClientContext* ctx{nullptr};
    bool canFailDuringCheckpoint;
    bool canFailDuringExecute;
    bool canFailDuringCommit;
    std::atomic<uint64_t> reserveCount = 0;
};

struct BMExceptionRecoveryTestConfig {
    bool canFailDuringExecute;
    bool canFailDuringCheckpoint;
    bool canFailDuringCommit;
    std::function<void(main::Connection*)> initFunc;
    std::function<std::unique_ptr<main::QueryResult>(main::Connection*, int)> executeFunc;
    std::function<bool(main::QueryResult*)> earlyExitOnFailureFunc;
    std::function<std::unique_ptr<main::QueryResult>(main::Connection*)> checkFunc;
    uint64_t checkResult;
};

class CopyTest : public BaseGraphTest {
public:
    void TearDown() override {
        database.reset();
        conn.reset();
    }

    void SetUp() override {
        BaseGraphTest::SetUp();
        failureFrequency = 32;
    }

    void resetDB(uint64_t bufferPoolSize) {
        systemConfig->bufferPoolSize = bufferPoolSize;
        database.reset();
        conn.reset();
        createDBAndConn();
    }
    void resetDBFlaky(bool canFailDuringExecute = true, bool canFailDuringCheckpoint = true,
        bool canFailDuringCommit = true) {
        database.reset();
        conn.reset();
        systemConfig->bufferPoolSize = main::SystemConfig{}.bufferPoolSize;
        auto constructBMFunc = [&](const main::Database& db) {
            auto bm = std::unique_ptr<FlakyBufferManager>(new FlakyBufferManager(databasePath,
                databasePath + ".copy.tmp", systemConfig->bufferPoolSize, systemConfig->maxDBSize,
                getFileSystem(db), systemConfig->readOnly, failureFrequency, canFailDuringExecute,
                canFailDuringCheckpoint, canFailDuringCommit));
            currentBM = bm.get();
            return bm;
        };
        database = BaseGraphTest::constructDB(databasePath, *systemConfig, constructBMFunc);
        conn = std::make_unique<main::Connection>(database.get());
        currentBM->setClientContext(conn->getClientContext());
    }
    std::string getInputDir() override { UNREACHABLE_CODE; }
    void BMExceptionRecoveryTest(BMExceptionRecoveryTestConfig cfg);
    std::string writeCSV(const std::string& fileName, const std::vector<std::string>& rows) {
        auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
        auto filePath = common::LocalFileSystem::joinPath(tempDir, fileName);
#if defined(_WIN32)
        std::replace(filePath.begin(), filePath.end(), '\\', '/');
#endif
        std::filesystem::create_directories(tempDir);
        std::ofstream file(filePath, std::ios::binary);
        for (const auto& row : rows) {
            file << row << "\n";
        }
        file.close();
        return filePath;
    }
    std::atomic<uint64_t> failureFrequency;
    FlakyBufferManager* currentBM;
};

struct StructuralCSVReaderTestCase {
    std::string name;
    std::vector<std::string> rows;
    std::string query;
    std::vector<std::vector<std::string>> expectedRows;
};

class StructuralCSVReaderTest : public CopyTest,
                                public WithParamInterface<StructuralCSVReaderTestCase> {};

static std::string bindStructuralCSVPath(std::string query, const std::string& filePath) {
    const auto markerPos = query.find("{}");
    DASSERT(markerPos != std::string::npos);
    query.replace(markerPos, 2, filePath);
    return query;
}

static StructuralCSVReaderTestCase makeLargeStructuralCSVTestCase() {
    static constexpr auto numRows = common::DEFAULT_VECTOR_CAPACITY + 17;
    const auto fieldBody =
        std::string(common::CopyConstants::INITIAL_BUFFER_SIZE + 128, 'x') + "\n" + "tail";
    std::vector<std::string> rows;
    rows.reserve(numRows);
    for (auto i = 0u; i < numRows; ++i) {
        rows.push_back(std::to_string(i) + ",\"" + fieldBody + "\"");
    }
    return StructuralCSVReaderTestCase{
        "LargeContinuationAndBufferRefill",
        std::move(rows),
        R"(LOAD FROM "{}" (MULTILINE_PARALLEL=true, AUTO_DETECT=false) RETURN COUNT(*), MAX(SIZE(column1)))",
        {{std::to_string(numRows), std::to_string(fieldBody.size())}},
    };
}

TEST_P(StructuralCSVReaderTest, MultilineParallelStructuralParser) {
    createDBAndConn();
    const auto& testCase = GetParam();
    const auto filePath = writeCSV("structural.csv", testCase.rows);
    auto result = conn->query(bindStructuralCSVPath(testCase.query, filePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    for (const auto& expectedRow : testCase.expectedRows) {
        ASSERT_TRUE(result->hasNext()) << testCase.name;
        auto tuple = result->getNext();
        ASSERT_EQ(expectedRow.size(), tuple->len()) << testCase.name;
        for (auto i = 0u; i < expectedRow.size(); ++i) {
            EXPECT_EQ(expectedRow[i], tuple->getValue(i)->toString()) << testCase.name;
        }
    }
    EXPECT_FALSE(result->hasNext()) << testCase.name;
}

INSTANTIATE_TEST_SUITE_P(CSV, StructuralCSVReaderTest,
    Values(
        StructuralCSVReaderTestCase{
            "QuotedNewline",
            {R"("abc
def")"},
            R"(LOAD FROM "{}" (MULTILINE_PARALLEL=true, AUTO_DETECT=false) RETURN COUNT(*), SIZE(column0))",
            {{"1", "7"}},
        },
        StructuralCSVReaderTestCase{
            "QuotedCRLF",
            {"1,\"abc\r\ndef\""},
            R"(LOAD FROM "{}" (MULTILINE_PARALLEL=true, AUTO_DETECT=false) RETURN COUNT(*), SIZE(column1))",
            {{"1", "8"}},
        },
        StructuralCSVReaderTestCase{
            "CustomDelimiterScalarPlannedRange",
            {"1;\"abc\ndef\""},
            R"(LOAD FROM "{}" (MULTILINE_PARALLEL=true, AUTO_DETECT=false, DELIM=';') RETURN COUNT(*), SIZE(column1))",
            {{"1", "7"}},
        },
        StructuralCSVReaderTestCase{
            "EscapedQuotes",
            {R"(1,"a ""quoted"" value",tail)"},
            R"(LOAD FROM "{}" (MULTILINE_PARALLEL=true, AUTO_DETECT=false) RETURN column0, SIZE(column1), column2)",
            {{"1", "16", "tail"}},
        },
        StructuralCSVReaderTestCase{
            "MultiColumnQuotedDelimiters",
            {R"(1,"first
line","middle,with,delimiters","last
line")",
                R"(2,"alpha","middle
line","omega
tail")"},
            R"(LOAD FROM "{}" (MULTILINE_PARALLEL=true, AUTO_DETECT=false) RETURN column0, SIZE(column1), SIZE(column2), SIZE(column3) ORDER BY column0)",
            {{"1", "10", "22", "9"}, {"2", "5", "11", "10"}},
        },
        makeLargeStructuralCSVTestCase()),
    [](const TestParamInfo<StructuralCSVReaderTest::ParamType>& info) { return info.param.name; });

void CopyTest::BMExceptionRecoveryTest(BMExceptionRecoveryTestConfig cfg) {
    if (inMemMode) {
        failureFrequency = UINT64_MAX;
        resetDBFlaky(cfg.canFailDuringExecute, cfg.canFailDuringCheckpoint,
            cfg.canFailDuringCommit);
    } else {
        createDBAndConn();
    }

    cfg.initFunc(conn.get());

    if (!inMemMode) {
        resetDBFlaky(cfg.canFailDuringExecute, cfg.canFailDuringCheckpoint,
            cfg.canFailDuringCommit);
    }

    for (int i = 0;; i++) {
        ASSERT_LT(i, 20);
        auto result = cfg.executeFunc(conn.get(), i);
        if (!result->isSuccess()) {
            if (cfg.earlyExitOnFailureFunc(result.get())) {
                break;
            }
            ASSERT_EQ(result->getErrorMessage(), "Buffer manager exception: Unable to allocate "
                                                 "memory! The buffer pool is full and no "
                                                 "memory could be freed!");
        } else {
            break;
        }
    }

    if (inMemMode) {
        failureFrequency = UINT64_MAX;
    } else {
        // Reopen the DB so no spurious errors occur during the query
        resetDB(TestHelper::DEFAULT_BUFFER_POOL_SIZE_FOR_TESTING);
    }
    {
        // Test that the table copied as expected after the query
        auto result = cfg.checkFunc(conn.get());
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        ASSERT_TRUE(result->hasNext());
        ASSERT_EQ(cfg.checkResult, result->getNext()->getValue(0)->getValue<int64_t>());
    }

    // TODO(Royi) prevent leaking of allocated pages during checkpoint rollback
    if (!inMemMode && !cfg.canFailDuringCheckpoint) {
        FSMLeakChecker::checkForLeakedPages(conn.get());
    }
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexSorted) {
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto filePath = writeCSV("sorted.csv", {"1", "2", "3"});
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3);
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexRejectsDuplicate) {
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto filePath = writeCSV("duplicate.csv", {"1", "2", "2"});
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_NE(result->getErrorMessage().find("duplicated primary key"), std::string::npos);

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexRejectsNull) {
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto filePath = writeCSV("null.csv", {"1", "", "3"});
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_NE(result->getErrorMessage().find("violates the non-null constraint"),
        std::string::npos);

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexAllowsUnsortedUniqueNodeGroup) {
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto filePath = writeCSV("unsorted.csv", {"2", "1", "3"});
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3);
}

TEST_F(CopyTest, RelCopyWithoutDefaultHashIndex) {
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE REL TABLE Follows(FROM Account TO Account)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto nodePath = writeCSV("rel_nodes.csv", {"1", "2", "3"});
    result = conn->query(std::format("COPY Account FROM '{}'", nodePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto relPath = writeCSV("rels.csv", {"1,2", "2,3", "3,1"});
    result = conn->query(std::format("COPY Follows FROM '{}'", relPath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    result = conn->query("MATCH (:Account)-[f:Follows]->(:Account) RETURN COUNT(f)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3);
}

TEST_F(CopyTest, RelCopyWithoutDefaultHashIndexRejectsMissingEndpoint) {
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE REL TABLE Follows(FROM Account TO Account)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto nodePath = writeCSV("missing_endpoint_nodes.csv", {"1", "2"});
    result = conn->query(std::format("COPY Account FROM '{}'", nodePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const auto relPath = writeCSV("missing_endpoint_rels.csv", {"1,2", "2,3"});
    result = conn->query(std::format("COPY Follows FROM '{}'", relPath));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_NE(result->getErrorMessage().find("Unable to find primary key value"),
        std::string::npos);

    result = conn->query("MATCH (:Account)-[f:Follows]->(:Account) RETURN COUNT(f)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

// The no-hash-index COPY path used to keep all primary keys in an in-memory std::set, which OOMs
// when the table exceeds RAM. The validator now spills sorted runs to disk once an in-memory
// budget is exceeded and stream-merges them in finalize(). The following tests force spilling by
// lowering pk_validator_spill_threshold and drive enough rows to cross several flush boundaries,
// including cross-chunk duplicates that can only be detected during the final merge.

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexSpillsAndSucceeds) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    // Force a flush after roughly every node-group-sized chunk so that a moderately sized input
    // produces several spilled runs.
    result = conn->query("CALL pk_validator_spill_threshold=4096");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    std::vector<std::string> rows;
    // 3000 unique, unsorted ids: enough to span many node groups and several spilled runs.
    rows.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        rows.push_back(std::to_string(2999 - i));
    }
    const auto filePath = writeCSV("spill_unique.csv", rows);
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3000);
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexSpillsDetectsCrossChunkDuplicate) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CALL pk_validator_spill_threshold=4096");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(id INT64, PRIMARY KEY(id))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    std::vector<std::string> rows;
    rows.reserve(3000);
    for (int i = 0; i < 2999; ++i) {
        rows.push_back(std::to_string(2998 - i));
    }
    // Duplicate of an id that was spilled in an earlier run, so it cannot be caught by the
    // per-run sort and must surface during the streaming merge in finalize().
    rows.push_back("0");
    const auto filePath = writeCSV("spill_dup.csv", rows);
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_NE(result->getErrorMessage().find("duplicated primary key"), std::string::npos);

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexSpillsStringPK) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CALL pk_validator_spill_threshold=4096");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(name STRING, PRIMARY KEY(name))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    std::vector<std::string> rows;
    rows.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        rows.push_back(std::format("name_{}", 1999 - i));
    }
    const auto filePath = writeCSV("spill_string.csv", rows);
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2000);
}

TEST_F(CopyTest, NodeCopyWithoutDefaultHashIndexSpillsStringPKDetectsCrossChunkDuplicate) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    createDBAndConn();
    auto result = conn->query("CALL enable_default_hash_index=false");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CALL pk_validator_spill_threshold=4096");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("CREATE NODE TABLE Account(name STRING, PRIMARY KEY(name))");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    std::vector<std::string> rows;
    rows.reserve(2000);
    for (int i = 0; i < 1999; ++i) {
        rows.push_back(std::format("name_{}", 1998 - i));
    }
    // Duplicate of a string PK spilled in an earlier run; only the final merge can catch it.
    rows.push_back("name_0");
    const auto filePath = writeCSV("spill_string_dup.csv", rows);
    result = conn->query(std::format("COPY Account FROM '{}'", filePath));
    ASSERT_FALSE(result->isSuccess());
    ASSERT_NE(result->getErrorMessage().find("duplicated primary key"), std::string::npos);

    result = conn->query("MATCH (a:Account) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(CopyTest, NodeCopyBMExceptionRecoverySameConnection) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = true,
        .canFailDuringCheckpoint = false,
        .canFailDuringCommit = false,
        .initFunc =
            [](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                const auto queryString = std::format(
                    "COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                    LBUG_ROOT_DIRECTORY);

                return conn->query(queryString);
            },
        .earlyExitOnFailureFunc = [](main::QueryResult*) { return false; },
        .checkFunc =
            [](main::Connection* conn) { return conn->query("MATCH (a:account) RETURN COUNT(*)"); },
        .checkResult = 81306};
    BMExceptionRecoveryTest(cfg);
}

TEST_F(CopyTest, NodeCopyBMExceptionRecoverySameConnectionStringKey) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = true,
        .canFailDuringCheckpoint = false,
        .canFailDuringCommit = false,
        .initFunc =
            [](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID STRING, PRIMARY KEY(ID))");
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                const auto queryString = std::format(
                    "COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                    LBUG_ROOT_DIRECTORY);

                return conn->query(queryString);
            },
        .earlyExitOnFailureFunc = [](main::QueryResult*) { return false; },
        .checkFunc =
            [](main::Connection* conn) { return conn->query("MATCH (a:account) RETURN COUNT(*)"); },
        .checkResult = 81306};
    BMExceptionRecoveryTest(cfg);
}

#ifndef __WASM__
TEST_F(CopyTest, RelCopyBMExceptionRecoverySameConnection) {
    if (inMemMode ||
        common::StorageConfig::NODE_GROUP_SIZE_LOG2 != TestParser::STANDARD_NODE_GROUP_SIZE_LOG_2) {
        GTEST_SKIP();
    }
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = true,
        .canFailDuringCheckpoint = false,
        .canFailDuringCommit = false,
        .initFunc =
            [](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
                conn->query("CREATE REL TABLE follows(FROM account TO account);");
                ASSERT_TRUE(conn->query(std::format(
                    "COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                    LBUG_ROOT_DIRECTORY)));
            },
        .executeFunc =
            [this](main::Connection* conn, int i) {
                // This test calibrates injected BM failures against the historical 2-thread COPY
                // schedule. Keep the query-local parallelism fixed so flexible pool sizing in the
                // rest of the system does not make the failure point nondeterministic.
                conn->setMaxNumThreadForExec(2);
                // there are many allocations in the partitioning phase
                // we scale the failure frequency linearly so that we trigger at least one
                // allocation failure in the batch insert phase
                static constexpr auto failureFrequencyMultiplier =
                    512 * ((common::StorageConfig::MAX_SEGMENT_SIZE_LOG2 ==
                               TestParser::STANDARD_MAX_SEGMENT_SIZE_LOG_2) ?
                                  1 :
                                  (1 << 10));
                failureFrequency = failureFrequencyMultiplier * (i + 15);

                return conn->query(std::format(
                    "COPY follows FROM '{}/dataset/snap/twitter/csv/twitter-edges.csv' (DELIM=' ')",
                    LBUG_ROOT_DIRECTORY));
            },
        .earlyExitOnFailureFunc =
            [this](main::QueryResult*) {
                // clear the BM so that the failure frequency isn't messed with by cached pages
                for (auto& fh : CopyTestHelper::getBMFileHandles(currentBM)) {
                    currentBM->removeFilePagesFromFrames(*fh);
                }
                currentBM->removeEvictedCandidates();
                return false;
            },
        .checkFunc =
            [](main::Connection* conn) {
                return conn->query("MATCH (a:account)-[:follows]->(b:account) RETURN COUNT(*)");
            },
        .checkResult = 2420766};
    BMExceptionRecoveryTest(cfg);
}
#endif

TEST_F(CopyTest, NodeInsertBMExceptionDuringCommitRecovery) {
    static constexpr uint64_t numValues = 200000;
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = false,
        .canFailDuringCheckpoint = false,
        .canFailDuringCommit = false,
        .initFunc =
            [this](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
                failureFrequency = 128;
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                const auto queryString =
                    std::format("UNWIND RANGE(1,{}) AS i CREATE (a:account {{ID:i}})", numValues);
                return conn->query(queryString);
            },
        .earlyExitOnFailureFunc = [](main::QueryResult*) { return false; },
        .checkFunc =
            [](main::Connection* conn) { return conn->query("MATCH (a:account) RETURN COUNT(*)"); },
        .checkResult = numValues};
    BMExceptionRecoveryTest(cfg);
}

TEST_F(CopyTest, RelInsertBMExceptionDuringCommitRecovery) {
    static constexpr auto numNodes = 10000;
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = false,
        .canFailDuringCheckpoint = false,
        .canFailDuringCommit = true,
        .initFunc =
            [this](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
                conn->query("CREATE REL TABLE follows(FROM account TO account);");
                const auto queryString =
                    std::format("UNWIND RANGE(1,{}) AS i CREATE (a:account {{ID:i}})", numNodes);
                ASSERT_TRUE(conn->query(queryString)->isSuccess());
                failureFrequency = 32;
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                return conn->query(std::format(
                    "UNWIND RANGE(1,{}) AS i MATCH (a:account), (b:account) WHERE a.ID = i AND "
                    "b.ID = i + 1 CREATE (a)-[f:follows]->(b)",
                    numNodes - 1));
            },
        .earlyExitOnFailureFunc = [](main::QueryResult*) { return false; },
        .checkFunc =
            [](main::Connection* conn) {
                return conn->query("MATCH (a)-[f:follows]->(b) RETURN COUNT(*)");
            },
        .checkResult = numNodes - 1};
    BMExceptionRecoveryTest(cfg);
}

TEST_F(CopyTest, NodeCopyBMExceptionDuringCheckpointRecovery) {
    if (inMemMode || systemConfig->forceCheckpointOnClose == false) {
        GTEST_SKIP();
    }
    static constexpr bool canFailDuringExecute = false;
    static constexpr bool canFailDuringCheckpoint = true;
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = canFailDuringExecute,
        .canFailDuringCheckpoint = canFailDuringCheckpoint,
        .canFailDuringCommit = false,
        .initFunc =
            [this](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID STRING, PRIMARY KEY(ID))");
                failureFrequency = 512;
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                return conn->query(std::format(
                    "COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                    LBUG_ROOT_DIRECTORY));
            },
        .earlyExitOnFailureFunc =
            [this](main::QueryResult*) {
                // make sure the checkpoint when closing the DB doesn't fail
                failureFrequency = UINT64_MAX;
                return true;
            },
        .checkFunc =
            [](main::Connection* conn) { return conn->query("MATCH (a:account) RETURN COUNT(*)"); },
        .checkResult = 81306};
    BMExceptionRecoveryTest(cfg);
}

TEST_F(CopyTest, RelCopyCheckpointBMExceptionRecovery) {
    if (inMemMode || systemConfig->forceCheckpointOnClose == false) {
        GTEST_SKIP();
    }
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = false,
        .canFailDuringCheckpoint = true,
        .canFailDuringCommit = false,
        .initFunc =
            [this](main::Connection* conn) {
                conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
                conn->query("CREATE REL TABLE follows(FROM account TO account);");
                ASSERT_TRUE(conn->query(std::format(
                    "COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                    LBUG_ROOT_DIRECTORY)));
                failureFrequency = 1024;
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                return conn->query(std::format(
                    "COPY follows FROM '{}/dataset/snap/twitter/csv/twitter-edges.csv' (DELIM=' ')",
                    LBUG_ROOT_DIRECTORY));
            },
        .earlyExitOnFailureFunc =
            [this](main::QueryResult*) {
                // make sure the checkpoint when closing the DB doesn't fail
                failureFrequency = UINT64_MAX;
                return true;
            },
        .checkFunc =
            [](main::Connection* conn) {
                return conn->query("MATCH (a:account)-[:follows]->(b:account) RETURN COUNT(*)");
            },
        .checkResult = 2420766};
    BMExceptionRecoveryTest(cfg);
}

TEST_F(CopyTest, NodeInsertBMExceptionDuringCheckpointRecovery) {
    if (inMemMode ||
        common::StorageConfig::NODE_GROUP_SIZE_LOG2 != TestParser::STANDARD_NODE_GROUP_SIZE_LOG_2) {
        GTEST_SKIP();
    }
    static constexpr uint64_t numValues = 200000;
    static constexpr bool canFailDuringExecute = false;
    static constexpr bool canFailDuringCheckpoint = true;
    BMExceptionRecoveryTestConfig cfg{.canFailDuringExecute = canFailDuringExecute,
        .canFailDuringCheckpoint = canFailDuringCheckpoint,
        .canFailDuringCommit = false,
        .initFunc =
            [this](main::Connection* conn) {
                failureFrequency = 512;
                conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
            },
        .executeFunc =
            [](main::Connection* conn, int) {
                return conn->query(
                    std::format("UNWIND RANGE(1,{}) AS i CREATE (a:account {{ID:i}})", numValues));
            },
        .earlyExitOnFailureFunc = [](main::QueryResult*) { return true; },
        .checkFunc =
            [](main::Connection* conn) { return conn->query("MATCH (a:account) RETURN COUNT(*)"); },
        .checkResult = numValues};
    BMExceptionRecoveryTest(cfg);
}

TEST_F(CopyTest, GracefulBMExceptionHandlingManyThreads) {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    // This test runs too slowly with TSAN enabled since it uses 32 threads and is already slow
    GTEST_SKIP();
#endif
#endif
    systemConfig->maxNumThreads = 32;
    resetDB(TestHelper::DEFAULT_BUFFER_POOL_SIZE_FOR_TESTING);

    static constexpr uint32_t repeatCount = 10;
    for (uint32_t i = 0; i < repeatCount; ++i) {
        conn->query("create node table Comment (id int64, creationDate INT64, locationIP STRING, "
                    "browserUsed STRING, content STRING, length INT32, PRIMARY KEY (id))");
        auto result =
            conn->query(std::format("COPY Comment FROM ['{}/dataset/ldbc-sf01/Comment.csv', "
                                    "'{}/dataset/ldbc-sf01/Comment.csv'] (delim='|', header=true, "
                                    "parallel=false)",
                LBUG_ROOT_DIRECTORY, LBUG_ROOT_DIRECTORY));
        ASSERT_FALSE(result->isSuccess());
        conn->query("drop table Comment");
    }

    if (!inMemMode) {
        FSMLeakChecker::checkForLeakedPages(conn.get());
    }
}

TEST_F(CopyTest, OutOfMemoryRecovery) {
    if (inMemMode ||
        common::StorageConfig::NODE_GROUP_SIZE_LOG2 != TestParser::STANDARD_NODE_GROUP_SIZE_LOG_2) {
        GTEST_SKIP();
    }
    // Needs to be small enough that we cannot successfully complete the rel table copy
    resetDB(64 * 1024 * 1024 + TestHelper::HASH_INDEX_MEM / 5);
    conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
    conn->query("CREATE REL TABLE follows(FROM account TO account);");
    {
        auto result = conn->query(
            std::format("COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                LBUG_ROOT_DIRECTORY));
        ASSERT_TRUE(result->isSuccess()) << result->toString();

        result = conn->query(std::format(
            "COPY follows FROM '{}/dataset/snap/twitter/csv/twitter-edges.csv' (DELIM=' ')",
            LBUG_ROOT_DIRECTORY));
        ASSERT_FALSE(result->isSuccess());
        ASSERT_EQ(result->getErrorMessage(),
            "Buffer manager exception: Unable to allocate memory! The buffer pool is full and no "
            "memory could be freed!");
    }
    // Try opening then closing the database
    resetDB(256 * 1024 * 1024 + TestHelper::HASH_INDEX_MEM);
    // Try again with a larger buffer pool size
    resetDB(256 * 1024 * 1024 + TestHelper::HASH_INDEX_MEM);
    {
        auto result = conn->query(std::format(
            "COPY follows FROM '{}/dataset/snap/twitter/csv/twitter-edges.csv' (DELIM=' ')",
            LBUG_ROOT_DIRECTORY));
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        // Test that the table copied as expected after the query
        result = conn->query("MATCH (a:account)-[:follows]->(b:account) RETURN COUNT(*)");
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        ASSERT_TRUE(result->hasNext());
        ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2420766);
    }
}

TEST_F(CopyTest, OutOfMemoryRecoveryDropTable) {
    if (inMemMode ||
        common::StorageConfig::NODE_GROUP_SIZE_LOG2 != TestParser::STANDARD_NODE_GROUP_SIZE_LOG_2) {
        GTEST_SKIP();
    }
    // Needs to be small enough that we cannot successfully complete the rel table copy
    resetDB(64 * 1024 * 1024 + TestHelper::HASH_INDEX_MEM / 5);
    conn->query("CREATE NODE TABLE account(ID INT64, PRIMARY KEY(ID))");
    conn->query("CREATE REL TABLE follows(FROM account TO account);");
    {
        auto result = conn->query(
            std::format("COPY account FROM \"{}/dataset/snap/twitter/csv/twitter-nodes.csv\"",
                LBUG_ROOT_DIRECTORY));
        ASSERT_TRUE(result->isSuccess()) << result->toString();
        result = conn->query(std::format(
            "COPY follows FROM '{}/dataset/snap/twitter/csv/twitter-edges.csv' (DELIM=' ')",
            LBUG_ROOT_DIRECTORY));
        ASSERT_FALSE(result->isSuccess());
        ASSERT_EQ(result->getErrorMessage(), "Buffer manager exception: Unable to allocate "
                                             "memory! The buffer pool is full and no "
                                             "memory could be freed!");
    }
    // Try dropping the table before trying again with a larger buffer pool size
    resetDB(256 * 1024 * 1024 + TestHelper::HASH_INDEX_MEM);
    {
        auto result = conn->query("DROP TABLE follows;");
        ASSERT_TRUE(result->isSuccess()) << result->toString();
        result = conn->query("CREATE REL TABLE follows(FROM account TO account);");
        ASSERT_TRUE(result->isSuccess()) << result->toString();
        result = conn->query(std::format(
            "COPY follows FROM '{}/dataset/snap/twitter/csv/twitter-edges.csv' (DELIM=' ')",
            LBUG_ROOT_DIRECTORY));
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        // Test that the table copied as expected after the query
        result = conn->query("MATCH (a:account)-[:follows]->(b:account) RETURN COUNT(*)");
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        ASSERT_TRUE(result->hasNext());
        ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2420766);
    }
}
} // namespace testing
} // namespace lbug
