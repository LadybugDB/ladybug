#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "api_test/private_api_test.h"
#include "common/exception/runtime.h"
#include "storage/checkpointer.h"
#include "storage/storage_manager.h"
#include "storage/wal/wal.h"
#include "transaction/transaction_manager.h"
#include <format>

using namespace lbug::common;
using namespace lbug::testing;
using namespace lbug::transaction;
using namespace lbug::storage;

namespace lbug {
namespace testing {

class FlakyCheckpointer {
public:
    explicit FlakyCheckpointer(TransactionManager::init_checkpointer_func_t initFunc)
        : initFunc(std::move(initFunc)) {}

    void setCheckpointer(main::ClientContext& context) const {
        TransactionManager::Get(context)->initCheckpointerFunc = initFunc;
    }

private:
    TransactionManager::init_checkpointer_func_t initFunc;
};

class FlakyCheckpointerTest : public PrivateApiTest {
public:
    std::string getInputDir() override { return "empty"; }

    void runFlakyCheckpoint(const FlakyCheckpointer& flakyCheckpointer) {
        conn->query("CALL force_checkpoint_on_close=false;");
        conn->query("CALL auto_checkpoint=false");
        conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);");
        for (auto i = 0; i < 5000; i++) {
            conn->query(std::format("CREATE (a:test {{id: {}, name: 'name_{}'}});", i, i));
        }
        auto context = getClientContext(*conn);
        flakyCheckpointer.setCheckpointer(*context);
        auto res = conn->query("CHECKPOINT;");
        ASSERT_FALSE(res->isSuccess());
    }

    void runTest(const FlakyCheckpointer& flakyCheckpointer) {
        runFlakyCheckpoint(flakyCheckpointer);
        createDBAndConn();
        auto res = conn->query("MATCH (a:test) RETURN COUNT(a);");
        ASSERT_TRUE(res->isSuccess());
        ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), 5000);
    }
};

class FlakyCheckpointerFailsOnCheckpointStorage final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnCheckpointStorage(main::ClientContext& clientContext)
        : Checkpointer(clientContext) {}

    bool checkpointStorage() override { throw RuntimeException("checkpoint failed."); }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointStorageFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnCheckpointStorage>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnSerialization final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnSerialization(main::ClientContext& context)
        : Checkpointer(context) {}

    void serializeCatalogAndMetadata(DatabaseHeader&, bool) override {
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointSerializeFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnSerialization>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnWritingHeader final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnWritingHeader(main::ClientContext& context)
        : Checkpointer(context) {}

    void writeDatabaseHeader(const DatabaseHeader&) override {
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointWriteHeaderFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnWritingHeader>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnFlushingShadow final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnFlushingShadow(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages(bool /*walRotated*/) override {
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointFlushingShadowFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnFlushingShadow>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnLoggingCheckpoint final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnLoggingCheckpoint(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages(bool /*walRotated*/) override {
        const auto storageManager = mainStorageManager;
        auto& shadowFile = storageManager->getShadowFile();
        shadowFile.flushAll(clientContext);
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointLoggingCheckpointFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnLoggingCheckpoint>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnApplyingShadow final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnApplyingShadow(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages(bool walRotated) override {
        const auto storageManager = mainStorageManager;
        auto& shadowFile = storageManager->getShadowFile();
        shadowFile.flushAll(clientContext);
        auto wal = WAL::Get(clientContext);
        if (walRotated) {
            wal->logAndFlushCheckpointToFrozen(&clientContext);
        } else {
            wal->logAndFlushCheckpoint(&clientContext);
        }
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointApplyingShadowFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnClearingFiles final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnClearingFiles(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages(bool walRotated) override {
        const auto storageManager = mainStorageManager;
        auto& shadowFile = storageManager->getShadowFile();
        shadowFile.flushAll(clientContext);
        auto wal = WAL::Get(clientContext);
        if (walRotated) {
            wal->logAndFlushCheckpointToFrozen(&clientContext);
        } else {
            wal->logAndFlushCheckpoint(&clientContext);
        }
        shadowFile.applyShadowPages(*storageManager, clientContext);
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointClearingFilesFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

// Simulates a situation where a database attempts to replay a shadow file from an older database
// with the same path
TEST_F(FlakyCheckpointerTest, ShadowFileDatabaseIDMismatchExistingDB) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runFlakyCheckpoint(flakyCheckpointer);

    std::filesystem::remove(databasePath);

    // Temporarily rename the shadow file and frozen WAL file.
    // With WAL rotation, the active .wal is renamed to .wal.checkpoint during checkpoint,
    // so the frozen WAL is what survives after a failed checkpoint.
    auto shadowFilePath = StorageUtils::getShadowFilePath(databasePath);
    auto frozenWalFilePath = StorageUtils::getCheckpointWALFilePath(databasePath);
    auto tmpShadowFilePath = shadowFilePath + "1";
    auto tmpFrozenWalFilePath = frozenWalFilePath + "1";
    ASSERT_TRUE(std::filesystem::exists(shadowFilePath));
    ASSERT_TRUE(std::filesystem::exists(frozenWalFilePath));
    std::filesystem::rename(shadowFilePath, tmpShadowFilePath);
    std::filesystem::rename(frozenWalFilePath, tmpFrozenWalFilePath);

    // Recreate a new DB with the same path as before
    createDBAndConn();
    conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);");

    // Close the DB
    conn.reset();
    database.reset();

    // Rename the files to the original names
    std::filesystem::rename(tmpShadowFilePath, shadowFilePath);
    std::filesystem::rename(tmpFrozenWalFilePath, frozenWalFilePath);

    // The shadow file replay should now fail
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

TEST_F(FlakyCheckpointerTest, ShadowFileDatabaseIDMismatchNewDB) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runFlakyCheckpoint(flakyCheckpointer);

    std::filesystem::remove(databasePath);

    // The shadow file replay should now fail
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

TEST_F(FlakyCheckpointerTest, ShadowFileDatabaseIDMismatchCorruptedDB) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runFlakyCheckpoint(flakyCheckpointer);

    std::filesystem::remove(databasePath);

    // Create a new DB file and write garbage bytes to it
    std::ofstream ofs(databasePath);
    ofs << "1a1a1a1a1a1a1a1a1a1a";
    ofs.close();

    // The shadow file replay should now fail
    EXPECT_THROW(createDBAndConn(), InternalException);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReviewFixesTest
// Targeted regression tests for the three fixes made in response to adsharma's
// review comments on PR #332 (feat: non-blocking concurrent checkpoint).
// ─────────────────────────────────────────────────────────────────────────────
class ReviewFixesTest : public PrivateGraphTest {
protected:
    std::string getInputDir() override { return "empty"; }

    void SetUp() override {
        BaseGraphTest::SetUp();
        createDBAndConn();
    }
};

// Fix #1 – lastTimestamp data race
// ─────────────────────────────────────────────────────────────────────────────
// Before the fix, checkpointNoLock() read `lastTimestamp` without holding
// mtxForSerializingPublicFunctionCalls, which is UB when commit() concurrently
// increments it.  The fix snapshots the value under the mutex.
//
// Observable invariant tested here: a write transaction committed while the
// checkpoint drain is waiting for it must be included in the checkpoint.
// Without the fix the snapshot could see a stale (too-low) lastTimestamp,
// causing the MVCC catalog snapshot to exclude the final committed entry.
// After the fix the snapshot is taken under the mutex *after* the drain, so
// it is guaranteed to reflect all commits that happened before the gate.
TEST_F(ReviewFixesTest, CheckpointDrainWaitsForInFlightWrite) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CALL debug_enable_multi_writes=true;");
    conn->query("CREATE NODE TABLE drain_test(id INT64 PRIMARY KEY);");

    // Pre-load N committed rows so the table is non-trivial.
    const int N = 20;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("CREATE (:drain_test {{id: {}}});", i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // Open a write transaction on a second connection and hold it so the
    // checkpoint drain loop is forced to wait.
    auto conn2 = std::make_unique<lbug::main::Connection>(database.get());
    conn2->query("BEGIN TRANSACTION;");
    auto r2 = conn2->query(std::format("CREATE (:drain_test {{id: {}}});", N));
    ASSERT_TRUE(r2->isSuccess()) << r2->getErrorMessage();

    // Start the checkpoint on a background thread.  It will block at the drain
    // step waiting for conn2's write transaction to leave.
    std::promise<bool> ckptOk;
    auto ckptFuture = ckptOk.get_future();
    std::thread ckptThread([&]() {
        auto r = conn->query("CHECKPOINT;");
        ckptOk.set_value(r->isSuccess());
    });

    // Give the checkpoint thread time to reach the drain phase.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Commit the held write — this unblocks the drain.  The checkpoint must
    // then snapshot lastTimestamp *after* this commit is visible.
    conn2->query("COMMIT;");

    ASSERT_TRUE(ckptFuture.get()) << "CHECKPOINT failed";
    ckptThread.join();

    // conn2 holds a raw pointer to `database`; reset it before createDBAndConn()
    // destroys the database, otherwise the conn2 destructor accesses freed memory.
    r2.reset();
    conn2.reset();

    // On reload only checkpointed data is present (WAL has been rotated/cleared).
    // All N+1 rows must be visible because the final commit occurred before the
    // write gate was acquired and snapshotTS must capture it.
    createDBAndConn();
    auto res = conn->query("MATCH (n:drain_test) RETURN count(n) AS c;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    const auto count = res->getNext()->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(count, N + 1) << "Row committed just before the write gate must survive checkpoint";
}

// Fix #2 – remove const_cast from NodeGroup::checkpointInMemOnly and
//            NodeGroup::scanAllInsertedAndVersions
// ─────────────────────────────────────────────────────────────────────────────
// Before the fix, InMemChunkedNodeGroup::flush() and
// ChunkedNodeGroup::scanCommitted() took Transaction*, forcing const_casts at
// the call site.  The parameters are now const Transaction*.
//
// The test verifies end-to-end correctness of the in-memory checkpoint path:
// rows that exist only in RAM at checkpoint time must be flushed to disk and
// survive a database reopen.
TEST_F(ReviewFixesTest, CheckpointInMemOnlyDataIntegrity) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CREATE NODE TABLE inmem_test(id INT64 PRIMARY KEY, val STRING);");

    // Insert enough rows to span multiple in-memory node groups so both
    // checkpointInMemOnly (flush) and scanAllInsertedAndVersions paths are hit.
    const int N = 300;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("CREATE (:inmem_test {{id: {}, val: 'v{}'}});", i, i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // All rows are still in-memory (this is the first ever checkpoint).
    {
        auto ckptRes = conn->query("CHECKPOINT;");
        ASSERT_TRUE(ckptRes->isSuccess()) << ckptRes->getErrorMessage();
        // ckptRes destroyed here — before createDBAndConn() resets the database —
        // so the FactorizedTable it holds is freed while the allocator is still alive.
    }

    createDBAndConn();
    auto res = conn->query("MATCH (n:inmem_test) RETURN count(n) AS c;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), N)
        << "In-memory rows must survive checkpoint + reopen";

    // Spot-check a specific row to verify scanCommitted correctness.
    res = conn->query("MATCH (n:inmem_test) WHERE n.id = 42 RETURN n.val;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<std::string>(), "v42");
}

// Fix #3 – guard vacuumColumnIDs under schemaMtx
// ─────────────────────────────────────────────────────────────────────────────
// NodeTable::checkpoint() called tableEntry->vacuumColumnIDs() after the write
// gate was released but without holding schemaMtx.  Concurrent reader threads
// iterate the same column-ID set under schemaMtx, creating a data race.
// The fix wraps vacuumColumnIDs in a unique_lock on schemaMtx.
//
// This stress test runs concurrent MATCH queries while CHECKPOINT (which calls
// vacuumColumnIDs) is in progress.  A crash or wrong count indicates the race
// is still present; without the fix this frequently trips TSAN or produces
// heap corruption under sanitizers.
#ifndef __SINGLE_THREADED__
TEST_F(ReviewFixesTest, ConcurrentReadsDuringCheckpointVacuum) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CREATE NODE TABLE vacuum_test(id INT64 PRIMARY KEY, name STRING);");

    const int N = 500;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("CREATE (:vacuum_test {{id: {}, name: 'n{}'}});", i, i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // Reader threads run continuous MATCH count queries.  If vacuumColumnIDs
    // races with schemaMtx iteration the count will be wrong or the process
    // will crash.
    std::atomic<bool> stop{false};
    std::vector<std::string> errors;
    std::mutex errorsMtx;

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            auto rConn = std::make_unique<lbug::main::Connection>(database.get());
            while (!stop.load(std::memory_order_acquire)) {
                auto r = rConn->query("MATCH (n:vacuum_test) RETURN count(n) AS c;");
                if (!r->isSuccess()) {
                    std::lock_guard<std::mutex> lk{errorsMtx};
                    errors.push_back("query failed: " + r->getErrorMessage());
                    return;
                }
                auto cnt = r->getNext()->getValue(0)->getValue<int64_t>();
                if (cnt != N) {
                    std::lock_guard<std::mutex> lk{errorsMtx};
                    errors.push_back(std::format("wrong count: got {} expected {}", cnt, N));
                    return;
                }
            }
        });
    }

    // Let the readers warm up, then trigger the checkpoint that calls vacuumColumnIDs.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto ckptRes = conn->query("CHECKPOINT;");
    ASSERT_TRUE(ckptRes->isSuccess()) << ckptRes->getErrorMessage();

    stop.store(true, std::memory_order_release);
    for (auto& t : readers) {
        t.join();
    }

    std::lock_guard<std::mutex> lk{errorsMtx};
    EXPECT_TRUE(errors.empty()) << "Reader errors during checkpoint: " << errors.front();
}
#endif // __SINGLE_THREADED__

// Hash index basic recovery
//
// HashIndexLocalStorage has no per-entry timestamps; a correct fix for post-snapshotTS
// "ghost key" consistency requires timestamp-aware snapshotting inside the hash-index
// infrastructure and is tracked as a follow-up (pre-existing Vela limitation).
// This test validates the baseline: all rows committed before CHECKPOINT are recoverable
// via PK lookup after a reload.
TEST_F(ReviewFixesTest, HashIndexBasicRecoveryAfterCheckpoint) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CREATE NODE TABLE hicac(id INT64 PRIMARY KEY, val STRING);");

    const int N = 20;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("CREATE (:hicac {{id: {}, val: 'v{}'}});", i, i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    {
        auto r = conn->query("CHECKPOINT;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    createDBAndConn();

    // All rows committed before the checkpoint must be recoverable via PK lookup.
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("MATCH (n:hicac) WHERE n.id = {} RETURN n.val;", i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
        ASSERT_TRUE(r->hasNext()) << "Hash index missing entry for id=" << i;
        EXPECT_EQ(r->getNext()->getValue(0)->getValue<std::string>(), "v" + std::to_string(i));
        EXPECT_FALSE(r->hasNext());
    }
}

// Fix #4 – defer destructive column move until after nodeGroups->checkpoint()
// ─────────────────────────────────────────────────────────────────────────────
// NodeTable::checkpoint() used to move columns (vacuuming dropped column IDs)
// BEFORE calling nodeGroups->checkpoint(). If nodeGroups->checkpoint() threw,
// the columns vector was already shrunk but the catalog's column IDs were not
// vacuumed, leaving the table in an inconsistent state.  A subsequent retry
// (e.g., from Database::~Database forceCheckpointOnClose) would index out of
// bounds and crash with a segfault.
//
// The fix defers the destructive column move until AFTER nodeGroups->checkpoint()
// succeeds.  These tests exercise the affected code path with ALTER TABLE
// ADD/DROP COLUMN, which triggers column-ID vacuum during checkpoint.

TEST_F(ReviewFixesTest, CheckpointRecoveryAfterAddColumn) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CREATE NODE TABLE ckpt_add(id INT64 PRIMARY KEY, name STRING);");

    const int N = 100;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("CREATE (:ckpt_add {{id: {}, name: 'n{}'}});", i, i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // Add a new column — this creates a new column ID that must be vacuumed at checkpoint.
    {
        auto r = conn->query("ALTER TABLE ckpt_add ADD extra INT64 DEFAULT 0;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
        // Set the new column on some rows.
        r = conn->query("MATCH (n:ckpt_add) WHERE n.id < 10 SET n.extra = n.id * 10;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
        r = conn->query("CHECKPOINT;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
        // r destroyed here — before createDBAndConn() resets the database —
        // so the FactorizedTable it holds is freed while the allocator is still alive.
    }

    createDBAndConn();

    // Verify row count.
    auto res = conn->query("MATCH (n:ckpt_add) RETURN count(n) AS c;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), N);

    // Verify the new column exists and has correct values.
    res = conn->query("MATCH (n:ckpt_add) WHERE n.id = 5 RETURN n.extra;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), 50);

    res = conn->query("MATCH (n:ckpt_add) WHERE n.id = 50 RETURN n.extra;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(ReviewFixesTest, CheckpointRecoveryAfterAddAndDropColumn) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL auto_checkpoint=false;");
    conn->query(
        "CREATE NODE TABLE ckpt_adddrop(id INT64 PRIMARY KEY, name STRING, age INT64);");

    const int N = 100;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(
            std::format("CREATE (:ckpt_adddrop {{id: {}, name: 'n{}', age: {}}});", i, i, 20 + i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // Add a column, then drop one — exercises both add and remove in the column-ID vacuum.
    {
        auto r = conn->query("ALTER TABLE ckpt_adddrop ADD extra STRING DEFAULT 'hello';");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
        r = conn->query("ALTER TABLE ckpt_adddrop DROP name;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
        r = conn->query("CHECKPOINT;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    createDBAndConn();

    // Verify row count.
    auto res = conn->query("MATCH (n:ckpt_adddrop) RETURN count(n) AS c;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), N);

    // Verify dropped column is gone.
    res = conn->query("MATCH (n:ckpt_adddrop) WHERE n.id = 0 RETURN n.name;");
    ASSERT_FALSE(res->isSuccess());

    // Verify remaining columns are correct.
    res = conn->query("MATCH (n:ckpt_adddrop) WHERE n.id = 0 RETURN n.age, n.extra;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    auto row = res->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 20);
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "hello");
}

TEST_F(ReviewFixesTest, RecoverFromFailedCheckpointAfterAddColumn) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL force_checkpoint_on_close=false;");
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CREATE NODE TABLE ckpt_fail(id INT64 PRIMARY KEY, name STRING);");

    const int N = 100;
    for (int i = 0; i < N; ++i) {
        auto r = conn->query(std::format("CREATE (:ckpt_fail {{id: {}, name: 'n{}'}});", i, i));
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // Add a column so the checkpoint path must vacuum column IDs.
    {
        auto r = conn->query("ALTER TABLE ckpt_fail ADD extra INT64 DEFAULT 42;");
        ASSERT_TRUE(r->isSuccess()) << r->getErrorMessage();
    }

    // Inject a failure at checkpointStorage level.
    auto context = getClientContext(*conn);
    auto initFlakyCheckpointer = [](main::ClientContext& ctx) {
        return std::make_unique<FlakyCheckpointerFailsOnCheckpointStorage>(ctx);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    flakyCheckpointer.setCheckpointer(*context);

    // First checkpoint fails.
    auto ckptRes = conn->query("CHECKPOINT;");
    ASSERT_FALSE(ckptRes->isSuccess());

    // Reopen the database — WAL replay + fresh checkpoint must succeed.
    // Before the fix, if a failure happened inside NodeTable::checkpoint() (between the
    // column move and vacuumColumnIDs), the retry checkpoint in ~Database would crash.
    createDBAndConn();

    // Verify data survives WAL replay.
    auto res = conn->query("MATCH (n:ckpt_fail) RETURN count(n) AS c;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), N);

    // Verify the added column is present with its default value.
    res = conn->query("MATCH (n:ckpt_fail) WHERE n.id = 0 RETURN n.extra;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), 42);
}

} // namespace testing
} // namespace lbug
