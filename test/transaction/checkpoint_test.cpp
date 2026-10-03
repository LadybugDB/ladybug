#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

#include "api_test/private_api_test.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/checksum.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/vector/value_vector.h"
#include "main/database_manager.h"
#include "main/db_config.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/checkpointer.h"
#include "storage/database_header.h"
#include "storage/page_allocator.h"
#include "storage/page_manager.h"
#include "storage/shadow_file.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/string_chunk_data.h"
#include "storage/wal/wal.h"
#include "test_env.h"
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

    static void resetCheckpointer(main::ClientContext& context) {
        TransactionManager::Get(context)->initCheckpointerFunc =
            TransactionManager::initCheckpointer;
    }

private:
    TransactionManager::init_checkpointer_func_t initFunc;
};

class FlakyCheckpointerTest : public PrivateApiTest {
public:
    std::string getInputDir() override { return "empty"; }

    void runFlakyCheckpoint(const FlakyCheckpointer& flakyCheckpointer,
        std::string* errorMessage = nullptr, bool resetWALBeforeCheckpoint = false) {
        conn->query("CALL force_checkpoint_on_close=false;");
        conn->query("CALL auto_checkpoint=false");
        conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);");
        for (auto i = 0; i < 5000; i++) {
            conn->query(std::format("CREATE (a:test {{id: {}, name: 'name_{}'}});", i, i));
        }
        auto context = getClientContext(*conn);
        if (resetWALBeforeCheckpoint) {
            WAL::Get(*context)->reset();
        }
        flakyCheckpointer.setCheckpointer(*context);
        auto res = conn->query("CHECKPOINT;");
        ASSERT_FALSE(res->isSuccess());
        if (errorMessage != nullptr) {
            *errorMessage = res->getErrorMessage();
        }
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

class CheckpointRetryAfterFailureTest : public FlakyCheckpointerTest {
public:
    void SetUp() override {
        FlakyCheckpointerTest::SetUp();
        ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
        ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
        ASSERT_TRUE(
            conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);")->isSuccess());
        ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    }

    void insertNodes(int64_t begin, int64_t end) const {
        for (auto i = begin; i < end; i++) {
            auto res =
                conn->query(std::format("CREATE (a:test {{id: {}, name: 'name_{}'}});", i, i));
            ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        }
    }

    // Runs one CHECKPOINT with the given failing checkpointer, then restores the default one.
    template<typename FAILING_CHECKPOINTER>
    void failCheckpointWith() const {
        FlakyCheckpointer flakyCheckpointer([](main::ClientContext& context) {
            return std::make_unique<FAILING_CHECKPOINTER>(context);
        });
        auto context = getClientContext(*conn);
        flakyCheckpointer.setCheckpointer(*context);
        ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());
        FlakyCheckpointer::resetCheckpointer(*context);
    }

    void failCheckpoint() const { failCheckpointWith<FlakyCheckpointerFailsOnCheckpointStorage>(); }

    void checkpoint() const {
        auto res = conn->query("CHECKPOINT;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    }

    void checkNodes(int64_t expectedCount) const {
        auto res = conn->query("MATCH (a:test) RETURN COUNT(a), MIN(a.id), MAX(a.id);");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        auto tuple = res->getNext();
        ASSERT_EQ(tuple->getValue(0)->getValue<int64_t>(), expectedCount);
        ASSERT_EQ(tuple->getValue(1)->getValue<int64_t>(), 0);
        ASSERT_EQ(tuple->getValue(2)->getValue<int64_t>(), expectedCount - 1);
    }
};

TEST_F(CheckpointRetryAfterFailureTest, RetrySucceedsWithoutNewWrites) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpoint();
    checkpoint();
    EXPECT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));
    createDBAndConn();
    checkNodes(100);
}

TEST_F(CheckpointRetryAfterFailureTest, RetrySucceedsWithNewWrites) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpoint();
    insertNodes(100, 200);
    checkpoint();
    EXPECT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));
    createDBAndConn();
    checkNodes(200);
}

TEST_F(CheckpointRetryAfterFailureTest, RetryFailsWithNewWritesThenReopen) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpoint();
    insertNodes(100, 200);
    failCheckpoint();
    // Close without checkpointing, as a crash would.
    createDBAndConn();
    checkNodes(200);
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
        shadowFile.flushAll(storageManager->getOrInitDatabaseID(clientContext));
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
        shadowFile.flushAll(storageManager->getOrInitDatabaseID(clientContext));
        auto wal = WAL::Get(clientContext);
        if (walRotated) {
            wal->logAndFlushCheckpointToFrozen(&clientContext);
        } else {
            wal->logAndFlushCheckpoint(&clientContext);
        }
        throw RuntimeException("checkpoint failed.");
    }
};

class PartitionChildShadowApplier final : public Checkpointer {
public:
    explicit PartitionChildShadowApplier(main::ClientContext& context) : Checkpointer(context) {}

    void applyPartitionChildShadowPages() { applyShadowPagesForPartitionChildren(); }
};

static void rewriteCheckpointRecordAsLegacy(main::ClientContext& context,
    const std::string& walPath, std::optional<bool> enableChecksumsOverride = std::nullopt);
static void writeShadowDatabaseID(main::ClientContext& context, const std::string& shadowPath,
    uuid databaseID);

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

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

TEST_F(CheckpointRetryAfterFailureTest, RetryAfterFailureWithDurableCheckpointRecord) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();
    auto rejectedWrite = conn->query("CREATE (:test {id: 100, name: 'rejected'});");
    ASSERT_FALSE(rejectedWrite->isSuccess());
    ASSERT_NE(rejectedWrite->getErrorMessage().find("panic state"), std::string::npos);
    EXPECT_FALSE(conn->query("CHECKPOINT;")->isSuccess());
    createDBAndConn();
    checkNodes(100);
}

TEST_F(CheckpointRetryAfterFailureTest, FailedRetryKeepsDurableCheckpointRecord) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();
    const auto frozenWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    ASSERT_TRUE(std::filesystem::exists(frozenWALPath));
    auto retry = conn->query("CHECKPOINT;");
    ASSERT_FALSE(retry->isSuccess());
    ASSERT_NE(retry->getErrorMessage().find("panic state"), std::string::npos);
    ASSERT_TRUE(std::filesystem::exists(frozenWALPath));
    createDBAndConn();
    checkNodes(100);
}

TEST_F(CheckpointRetryAfterFailureTest, MainMarkerRecoversLegacyPartitionShadows) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p1 {id: 2, amount: 20});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto parentDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    const auto activeWALPath = StorageUtils::getWALFilePath(databasePath);
    const auto frozenWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    rewriteCheckpointRecordAsLegacy(*context,
        vfs->fileOrPathExists(frozenWALPath, context) ? frozenWALPath : activeWALPath);
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath),
        parentDatabaseID);
    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    for (auto* childManager : childManagers) {
        const auto shadowPath = StorageUtils::getShadowFilePath(childManager->getDatabasePath());
        ASSERT_TRUE(std::filesystem::exists(shadowPath));
        writeShadowDatabaseID(*context, shadowPath, parentDatabaseID);
    }

    createDBAndConn();
    auto result = conn->query("MATCH (n:partitioned) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    result = conn->query("MATCH (n:partitioned_p0) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n:partitioned_p1) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

TEST_F(CheckpointRetryAfterFailureTest, LegacyMarkerRecoversAppliedChildrenWithoutShadows) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p1 {id: 2, amount: 20});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto parentDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    const auto activeWALPath = StorageUtils::getWALFilePath(databasePath);
    const auto frozenWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    rewriteCheckpointRecordAsLegacy(*context,
        vfs->fileOrPathExists(frozenWALPath, context) ? frozenWALPath : activeWALPath);
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath),
        parentDatabaseID);
    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    std::vector<std::string> childShadowPaths;
    for (auto* childManager : childManagers) {
        childShadowPaths.push_back(
            StorageUtils::getShadowFilePath(childManager->getDatabasePath()));
        ASSERT_TRUE(std::filesystem::exists(childShadowPaths.back()));
    }
    // The legacy protocol applied and removed each child's shadow before the checkpoint
    // committed, so simulate a crash in that window: children applied, shadows gone.
    PartitionChildShadowApplier applier(*context);
    applier.applyPartitionChildShadowPages();
    conn.reset();
    database.reset();
    for (const auto& childShadowPath : childShadowPaths) {
        std::filesystem::remove(childShadowPath);
    }

    createDBAndConn();
    auto result = conn->query("MATCH (n:partitioned) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    result = conn->query("MATCH (n:partitioned_p0) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n:partitioned_p1) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

TEST_F(CheckpointRetryAfterFailureTest, BundleMarkerRequiresEveryChildShadow) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p1 {id: 2, amount: 20});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    std::vector<std::string> childShadowPaths;
    for (auto* childManager : childManagers) {
        childShadowPaths.push_back(
            StorageUtils::getShadowFilePath(childManager->getDatabasePath()));
        ASSERT_TRUE(std::filesystem::exists(childShadowPaths.back()));
    }
    conn.reset();
    database.reset();
    std::filesystem::remove(childShadowPaths[0]);
    try {
        createDBAndConn();
        FAIL() << "Expected a missing partition shadow to block bundle recovery.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("partition shadow file"), std::string::npos)
            << e.what();
        EXPECT_NE(std::string{e.what()}.find("is missing"), std::string::npos) << e.what();
    }
}

TEST_F(CheckpointRetryAfterFailureTest, MissingMainMarkerDiscardsPartitionShadows) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p1 {id: 2, amount: 20});")->isSuccess());

    auto* context = getClientContext(*conn);
    auto stagedCheckpoint = std::make_unique<Checkpointer>(*context);
    stagedCheckpoint->beginCheckpoint(0);
    stagedCheckpoint->checkpointStoragePhase();
    stagedCheckpoint->persistPartitionChildFiles();
    stagedCheckpoint.reset();
    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    std::vector<std::string> childShadowPaths;
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    for (auto* childManager : childManagers) {
        childShadowPaths.push_back(
            StorageUtils::getShadowFilePath(childManager->getDatabasePath()));
        ASSERT_TRUE(std::filesystem::exists(childShadowPaths.back()));
        auto shadowFile = vfs->openFile(childShadowPaths.back(),
            FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
        ShadowFileHeader header;
        shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&header), sizeof(header), 0);
        header.numShadowPages = INVALID_PAGE_IDX;
        shadowFile->writeFile(reinterpret_cast<const uint8_t*>(&header), sizeof(header), 0);
        shadowFile->syncFile();
    }
    createDBAndConn();
    for (const auto& childShadowPath : childShadowPaths) {
        ASSERT_FALSE(std::filesystem::exists(childShadowPath));
    }
    auto result = conn->query("MATCH (n:partitioned) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    result = conn->query("MATCH (n:partitioned_p0) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n:partitioned_p1) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

TEST_F(CheckpointRetryAfterFailureTest, ReadOnlyOpenPreservesOrphanPartitionShadow) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();

    const auto childManagers = main::DatabaseManager::Get(*getClientContext(*conn))
                                   ->getPartitionStorageRegistry()
                                   ->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    const auto shadowPath =
        StorageUtils::getShadowFilePath(childManagers.front()->getDatabasePath());
    const std::string shadowContents = "orphan partition shadow";
    std::ofstream{shadowPath, std::ios::binary}.write(shadowContents.data(), shadowContents.size());
    ASSERT_TRUE(std::filesystem::exists(shadowPath));

    conn.reset();
    database.reset();
    systemConfig->readOnly = true;
    EXPECT_THROW(createDBAndConn(), RuntimeException);
    std::ifstream shadowStream{shadowPath, std::ios::binary};
    const std::string persistedShadow{std::istreambuf_iterator<char>{shadowStream},
        std::istreambuf_iterator<char>{}};
    EXPECT_EQ(persistedShadow, shadowContents);
}

TEST_F(CheckpointRetryAfterFailureTest, ReadOnlyOpenPreservesOrphanGraphShadow) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE GRAPH orphan_shadow_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH orphan_shadow_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:orphan {name: 'x'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    checkpoint();

    const auto graphPath = StorageUtils::getGraphPath(databasePath, "orphan_shadow_graph");
    const auto shadowPath = StorageUtils::getShadowFilePath(graphPath);
    const std::string shadowContents = "orphan graph shadow";
    std::ofstream{shadowPath, std::ios::binary}.write(shadowContents.data(), shadowContents.size());
    ASSERT_TRUE(std::filesystem::exists(shadowPath));

    conn.reset();
    database.reset();
    systemConfig->readOnly = true;
    EXPECT_THROW(createDBAndConn(), RuntimeException);
    std::ifstream shadowStream{shadowPath, std::ios::binary};
    const std::string persistedShadow{std::istreambuf_iterator<char>{shadowStream},
        std::istreambuf_iterator<char>{}};
    EXPECT_EQ(persistedShadow, shadowContents);
}

class CheckpointerFailsBeforeWALRetirement final : public Checkpointer {
public:
    explicit CheckpointerFailsBeforeWALRetirement(main::ClientContext& context)
        : Checkpointer(context) {}

    void beforeWALRetirement(bool) override {
        throw RuntimeException("checkpoint interrupted before WAL retirement");
    }
};

TEST_F(CheckpointRetryAfterFailureTest, PartitionShadowsSurviveUntilMainWALRetirement) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p1 {id: 2, amount: 20});")->isSuccess());
    failCheckpointWith<CheckpointerFailsBeforeWALRetirement>();

    auto* context = getClientContext(*conn);
    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    for (auto* childManager : childManagers) {
        const auto childShadowPath =
            StorageUtils::getShadowFilePath(childManager->getDatabasePath());
        ASSERT_TRUE(std::filesystem::exists(childShadowPath));
        ShadowFile::replayShadowPageRecordsForStorageManager(*context, *childManager,
            ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID);
        ASSERT_TRUE(std::filesystem::exists(childShadowPath));
    }
    auto retry = conn->query("CHECKPOINT;");
    ASSERT_FALSE(retry->isSuccess());
    ASSERT_NE(retry->getErrorMessage().find("panic state"), std::string::npos);
    for (auto* childManager : childManagers) {
        ASSERT_TRUE(std::filesystem::exists(
            StorageUtils::getShadowFilePath(childManager->getDatabasePath())));
    }

    createDBAndConn();
    auto result = conn->query("MATCH (n:partitioned_p0) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n:partitioned_p1) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

class CommittedPartitionMissingBaseTest : public CheckpointRetryAfterFailureTest,
                                          public ::testing::WithParamInterface<bool> {};

TEST_P(CommittedPartitionMissingBaseTest, RejectsWithoutRecreatingFile) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const bool walRotated = GetParam();
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    auto* context = getClientContext(*conn);
    if (!walRotated) {
        WAL::Get(*context)->reset();
    }
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();
    ASSERT_EQ(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)),
        walRotated);

    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    const auto missingPath = childManagers.front()->getDatabasePath();
    conn.reset();
    database.reset();
    ASSERT_TRUE(std::filesystem::remove(missingPath));
    try {
        createDBAndConn();
        FAIL() << "Expected committed recovery to reject a missing partition file.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("partition file"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(std::filesystem::exists(missingPath));
}

INSTANTIATE_TEST_SUITE_P(WALRotation, CommittedPartitionMissingBaseTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Frozen" : "Active"; });

TEST_F(CheckpointRetryAfterFailureTest, CommittedPartitionRecoveryRejectsMissingShadowFile) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto childManagers =
        main::DatabaseManager::Get(*context)->getPartitionStorageRegistry()->getAllManagers();
    ASSERT_EQ(childManagers.size(), 2);
    const auto missingPath =
        StorageUtils::getShadowFilePath(childManagers.front()->getDatabasePath());
    conn.reset();
    database.reset();
    ASSERT_TRUE(std::filesystem::remove(missingPath));
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

TEST_F(CheckpointRetryAfterFailureTest, CommittedRecoveryReplaysChildShadowBeforeParsingChildFile) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    // Power loss during the child's shadow-page apply can persist the child's new header page
    // while the pages it references — including the page-manager metadata — are still torn or
    // missing. Recovery must replay the committed child shadow before parsing the child file,
    // the same repair-then-parse order the main file already follows.
    const auto childPath = StorageUtils::getGraphPath(databasePath, "partitioned_p0");
    const auto childShadowPath = StorageUtils::getShadowFilePath(childPath);
    conn.reset();
    database.reset();

    const auto shadowBytes = readFile(childShadowPath);
    ASSERT_GE(shadowBytes.size(), LBUG_PAGE_SIZE + sizeof(uint64_t));
    ShadowFileHeader shadowHeader;
    std::memcpy(&shadowHeader, shadowBytes.data(), sizeof(shadowHeader));
    ASSERT_GT(shadowHeader.numShadowPages, 0u);
    const auto recordsOffset =
        (static_cast<uint64_t>(shadowHeader.numShadowPages) + 1) * LBUG_PAGE_SIZE;
    uint64_t numRecords = 0;
    ASSERT_GE(shadowBytes.size(), recordsOffset + sizeof(numRecords));
    std::memcpy(&numRecords, shadowBytes.data() + recordsOffset, sizeof(numRecords));
    ASSERT_EQ(numRecords, shadowHeader.numShadowPages);
    std::optional<uint64_t> headerPageOffset;
    for (auto i = 0u; i < numRecords; ++i) {
        const auto recordOffset =
            recordsOffset + sizeof(uint64_t) + i * (sizeof(file_idx_t) + sizeof(page_idx_t));
        page_idx_t pageIdx = INVALID_PAGE_IDX;
        std::memcpy(&pageIdx, shadowBytes.data() + recordOffset + sizeof(file_idx_t),
            sizeof(pageIdx));
        if (pageIdx == common::StorageConstants::DB_HEADER_PAGE_IDX) {
            headerPageOffset = (i + 1) * LBUG_PAGE_SIZE;
        }
    }
    ASSERT_TRUE(headerPageOffset.has_value());
    ASSERT_GE(shadowBytes.size(), *headerPageOffset + LBUG_PAGE_SIZE);

    auto headerPage = std::make_unique<uint8_t[]>(LBUG_PAGE_SIZE);
    std::memcpy(headerPage.get(), shadowBytes.data() + *headerPageOffset, LBUG_PAGE_SIZE);
    common::Deserializer headerDeserializer{
        std::make_unique<common::BufferReader>(headerPage.get(), LBUG_PAGE_SIZE)};
    const auto childHeader = DatabaseHeader::deserialize(headerDeserializer);
    ASSERT_NE(childHeader.metadataPageRange.startPageIdx, INVALID_PAGE_IDX);

    const auto poisonOffset = childHeader.metadataPageRange.startPageIdx * LBUG_PAGE_SIZE;
    auto childBytes = readFile(childPath);
    childBytes.resize(std::max<size_t>(childBytes.size(), poisonOffset + LBUG_PAGE_SIZE), 0);
    std::memcpy(childBytes.data(), shadowBytes.data() + *headerPageOffset, LBUG_PAGE_SIZE);
    std::fill_n(childBytes.begin() + poisonOffset, LBUG_PAGE_SIZE, 0xFF);
    {
        std::ofstream stream{childPath, std::ios::binary};
        stream.write(reinterpret_cast<const char*>(childBytes.data()), childBytes.size());
    }

    createDBAndConn();
    auto result = conn->query("MATCH (a:partitioned_p0) RETURN COUNT(a);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);

    // The recovered page manager must be usable: allocate and durably checkpoint on top of it.
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 2, amount: 20});")->isSuccess());
    checkpoint();
    conn.reset();
    database.reset();
    createDBAndConn();
    result = conn->query("MATCH (a:partitioned_p0) RETURN COUNT(a);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
}

TEST_F(CheckpointRetryAfterFailureTest, CommittedRecoverySurvivesRetriesThatAdvanceExtent) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE partitioned(id INT64 PRIMARY KEY, amount INT64) "
                            "PARTITION BY HASH(amount) PARTITIONS 2;")
                    ->isSuccess());
    checkpoint();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 1, amount: 10});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnLoggingCheckpoint>();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p1 {id: 2, amount: 20});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnLoggingCheckpoint>();
    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 3, amount: 30});")->isSuccess());
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    // Each failed attempt allocates pages it never applies, so the committed checkpoint's
    // page extent recorded in the shadow's header page reaches beyond the physical files.
    // Recovery must still accept every page that extent covers.
    createDBAndConn();
    auto result = conn->query("MATCH (n:partitioned_p0) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    result = conn->query("MATCH (n:partitioned_p1) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);

    ASSERT_TRUE(conn->query("CREATE (:partitioned_p0 {id: 4, amount: 40});")->isSuccess());
    checkpoint();
    conn.reset();
    database.reset();
    createDBAndConn();
    result = conn->query("MATCH (n:partitioned_p0) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3);
}

TEST_F(CheckpointRetryAfterFailureTest, CommittedRecoveryRejectsOversizedShadowPageCount) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    const auto shadowPath = StorageUtils::getShadowFilePath(databasePath);
    auto shadowFile =
        vfs->openFile(shadowPath, FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
    ShadowFileHeader header;
    shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&header), sizeof(header), 0);
    header.numShadowPages = systemConfig->maxDBSize / LBUG_PAGE_SIZE + 1;
    shadowFile->writeFile(reinterpret_cast<const uint8_t*>(&header), sizeof(header), 0);
    shadowFile->syncFile();
    shadowFile.reset();
    conn.reset();
    database.reset();
    try {
        createDBAndConn();
        FAIL() << "Expected oversized shadow page count to be rejected.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("invalid shadow page count"), std::string::npos);
    }
}

TEST_F(CheckpointRetryAfterFailureTest, CommittedRecoveryRejectsInvalidShadowTargetPage) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    const auto shadowPath = StorageUtils::getShadowFilePath(databasePath);
    auto shadowFile =
        vfs->openFile(shadowPath, FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
    ShadowFileHeader header;
    shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&header), sizeof(header), 0);
    ASSERT_GT(header.numShadowPages, 0);
    const auto targetPageOffset =
        (static_cast<uint64_t>(header.numShadowPages) + 1) * LBUG_PAGE_SIZE + sizeof(uint64_t) +
        sizeof(file_idx_t);
    const page_idx_t invalidTarget = INVALID_PAGE_IDX;
    shadowFile->writeFile(reinterpret_cast<const uint8_t*>(&invalidTarget), sizeof(invalidTarget),
        targetPageOffset);
    shadowFile->syncFile();
    shadowFile.reset();
    conn.reset();
    database.reset();
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

class ShadowTargetPageBoundaryTest : public CheckpointRetryAfterFailureTest,
                                     public ::testing::WithParamInterface<bool> {};

TEST_P(ShadowTargetPageBoundaryTest, AcceptsOnlyPagesBelowCheckpointedExtent) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const bool targetIsFirstPagePastExtent = GetParam();
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    auto* storageManager = StorageManager::Get(*context);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    auto shadowFile = vfs->openFile(StorageUtils::getShadowFilePath(databasePath),
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
    ShadowFileHeader header;
    shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&header), sizeof(header), 0);
    ASSERT_GT(header.numShadowPages, 0);

    // The committed checkpoint's page extent is the one recorded in the shadow's embedded
    // database header page, not the physical file size plus the shadow page count.
    const auto recordsOffset = (static_cast<uint64_t>(header.numShadowPages) + 1) * LBUG_PAGE_SIZE;
    std::optional<uint64_t> headerRecordIdx;
    for (auto i = 0u; i < header.numShadowPages; ++i) {
        const auto recordOffset =
            recordsOffset + sizeof(uint64_t) + i * (sizeof(file_idx_t) + sizeof(page_idx_t));
        page_idx_t pageIdx = INVALID_PAGE_IDX;
        shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&pageIdx), sizeof(pageIdx),
            recordOffset + sizeof(file_idx_t));
        if (pageIdx == StorageConstants::DB_HEADER_PAGE_IDX) {
            headerRecordIdx = i;
        }
    }
    ASSERT_TRUE(headerRecordIdx.has_value());
    auto headerPage = std::make_unique<uint8_t[]>(LBUG_PAGE_SIZE);
    shadowFile->readFromFile(headerPage.get(), LBUG_PAGE_SIZE,
        (*headerRecordIdx + 1) * LBUG_PAGE_SIZE);
    common::Deserializer headerDeserializer{
        std::make_unique<common::BufferReader>(headerPage.get(), LBUG_PAGE_SIZE)};
    const auto checkpointHeader = DatabaseHeader::deserialize(headerDeserializer);
    const auto extentPages = static_cast<page_idx_t>(checkpointHeader.dataFileNumPages);
    ASSERT_GT(extentPages, 0);
    const auto target =
        static_cast<page_idx_t>(targetIsFirstPagePastExtent ? extentPages : extentPages - 1);
    const auto targetPageOffset =
        (static_cast<uint64_t>(header.numShadowPages) + 1) * LBUG_PAGE_SIZE + sizeof(uint64_t) +
        sizeof(file_idx_t);
    page_idx_t originalTarget = INVALID_PAGE_IDX;
    shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&originalTarget), sizeof(originalTarget),
        targetPageOffset);
    ASSERT_NE(originalTarget, StorageConstants::DB_HEADER_PAGE_IDX);
    shadowFile->writeFile(reinterpret_cast<const uint8_t*>(&target), sizeof(target),
        targetPageOffset);
    shadowFile->syncFile();
    shadowFile.reset();

    const auto databaseBeforeReplay = readFile(databasePath);
    const auto replay = [&] {
        ShadowFile::replayShadowPageRecordsForStorageManager(*context, *storageManager,
            ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID);
    };
    if (!targetIsFirstPagePastExtent) {
        EXPECT_NO_THROW(replay());
        return;
    }
    try {
        replay();
        FAIL() << "Expected the first page past the checkpointed extent to be rejected.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("invalid target page"), std::string::npos) << e.what();
    }
    EXPECT_EQ(readFile(databasePath), databaseBeforeReplay);
}

INSTANTIATE_TEST_SUITE_P(Extent, ShadowTargetPageBoundaryTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "FirstPagePastExtent" : "LastPageInExtent";
    });

class FlakyCheckpointerFailsOnClearingFiles final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnClearingFiles(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages(bool walRotated) override {
        const auto storageManager = mainStorageManager;
        auto& shadowFile = storageManager->getShadowFile();
        shadowFile.flushAll(storageManager->getOrInitDatabaseID(clientContext));
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

class FlakyCheckpointerFailsAfterRetiringWAL final : public Checkpointer {
public:
    FlakyCheckpointerFailsAfterRetiringWAL(main::ClientContext& context, bool expectedWalRotated,
        bool& reachedExpectedTarget)
        : Checkpointer(context), expectedWalRotated{expectedWalRotated},
          reachedExpectedTarget{reachedExpectedTarget} {}

    void onWALRetired(bool walRotated) override {
        ASSERT_EQ(walRotated, expectedWalRotated);
        reachedExpectedTarget = true;
        throw RuntimeException("checkpoint interrupted after retiring the WAL.");
    }

private:
    bool expectedWalRotated;
    bool& reachedExpectedTarget;
};

class MainWALRetirementTest : public FlakyCheckpointerTest,
                              public ::testing::WithParamInterface<bool> {};

TEST_P(MainWALRetirementTest, RecoverAfterRetiringWALBeforeRemovingShadow) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const auto expectedWalRotated = GetParam();
    bool reachedExpectedTarget = false;
    auto initFlakyCheckpointer = [expectedWalRotated, &reachedExpectedTarget](
                                     main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsAfterRetiringWAL>(context, expectedWalRotated,
            reachedExpectedTarget);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    std::string checkpointError;
    runFlakyCheckpoint(flakyCheckpointer, &checkpointError,
        !expectedWalRotated /* resetWALBeforeCheckpoint */);

    ASSERT_TRUE(reachedExpectedTarget);
    ASSERT_NE(checkpointError.find("refuses further writes until restart"), std::string::npos);
    ASSERT_NE(checkpointError.find("checkpoint interrupted after retiring the WAL"),
        std::string::npos);
    const auto activeWALPath = StorageUtils::getWALFilePath(databasePath);
    const auto checkpointWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    ASSERT_FALSE(std::filesystem::exists(activeWALPath));
    ASSERT_FALSE(std::filesystem::exists(checkpointWALPath));
    ASSERT_TRUE(std::filesystem::exists(StorageUtils::getShadowFilePath(databasePath)));
    auto rejectedWrite = conn->query("CREATE (:test {id: 5000, name: 'rejected'});");
    ASSERT_FALSE(rejectedWrite->isSuccess());
    ASSERT_NE(rejectedWrite->getErrorMessage().find("panic state"), std::string::npos);

    createDBAndConn();
    auto result = conn->query("MATCH (a:test) RETURN COUNT(a);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 5000);
    ASSERT_FALSE(std::filesystem::exists(activeWALPath));
    ASSERT_FALSE(std::filesystem::exists(checkpointWALPath));
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getShadowFilePath(databasePath)));
    ASSERT_TRUE(conn->query("CREATE (:test {id: 5000, name: 'after-recovery'});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    createDBAndConn();
    result = conn->query("MATCH (a:test) RETURN COUNT(a);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 5001);
}

INSTANTIATE_TEST_SUITE_P(ActiveAndFrozen, MainWALRetirementTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Frozen" : "Active"; });

class CheckpointerFailsBeforeGraphShadowApply final : public Checkpointer {
public:
    explicit CheckpointerFailsBeforeGraphShadowApply(main::ClientContext& context)
        : Checkpointer(context) {}

    void beforeGraphShadowApply(StorageManager&) override {
        throw RuntimeException("checkpoint interrupted before graph shadow apply");
    }
};

class LegacyCheckpointRecord final : public WALRecord {
public:
    LegacyCheckpointRecord() : WALRecord{WALRecordType::CHECKPOINT_RECORD} {}

    void serialize(Serializer& serializer) const override { WALRecord::serialize(serializer); }
};

static BinaryData serializeCheckpointRecord(const WALRecord& record) {
    auto writer = std::make_shared<BufferWriter>();
    Serializer serializer{writer};
    WALRecord::serializeWithLength(serializer, record);
    return writer->getData();
}

static void rewriteCheckpointRecord(main::ClientContext& context, const std::string& walPath,
    const WALRecord& replacement, std::optional<bool> enableChecksumsOverride = std::nullopt) {
    const auto currentRecord =
        serializeCheckpointRecord(CheckpointRecord{WAL::CHECKPOINT_BUNDLE_FORMAT_VERSION});
    const auto legacyRecord = serializeCheckpointRecord(replacement);
    const auto enableChecksums =
        enableChecksumsOverride.value_or(context.getDBConfig()->enableChecksums);
    const auto checksumSize = enableChecksums ? sizeof(uint64_t) : 0;
    auto fileInfo = VirtualFileSystem::GetUnsafe(context)->openFile(walPath,
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), &context);
    const auto fileSize = fileInfo->getFileSize();
    if (fileSize < currentRecord.size + checksumSize) {
        throw RuntimeException("Checkpoint WAL does not contain the current checkpoint record.");
    }
    const auto recordOffset = fileSize - currentRecord.size - checksumSize;
    auto actualRecord = std::make_unique<uint8_t[]>(currentRecord.size);
    fileInfo->readFromFile(actualRecord.get(), currentRecord.size, recordOffset);
    if (std::memcmp(actualRecord.get(), currentRecord.data.get(), currentRecord.size) != 0) {
        throw RuntimeException("Checkpoint WAL does not end with the current checkpoint record.");
    }
    if (enableChecksums) {
        uint64_t storedChecksum;
        fileInfo->readFromFile(reinterpret_cast<uint8_t*>(&storedChecksum), sizeof(storedChecksum),
            recordOffset + currentRecord.size);
        if (storedChecksum != checksum(currentRecord.data.get(), currentRecord.size)) {
            throw RuntimeException("Current checkpoint record checksum does not match.");
        }
    }
    fileInfo->truncate(recordOffset);
    fileInfo->writeFile(legacyRecord.data.get(), legacyRecord.size, recordOffset);
    if (enableChecksums) {
        const auto legacyChecksum = checksum(legacyRecord.data.get(), legacyRecord.size);
        fileInfo->writeFile(reinterpret_cast<const uint8_t*>(&legacyChecksum),
            sizeof(legacyChecksum), recordOffset + legacyRecord.size);
    }
    fileInfo->syncFile();
}

static void rewriteCheckpointRecordAsLegacy(main::ClientContext& context,
    const std::string& walPath, std::optional<bool> enableChecksumsOverride) {
    rewriteCheckpointRecord(context, walPath, LegacyCheckpointRecord{}, enableChecksumsOverride);
}

static void writeShadowDatabaseID(main::ClientContext& context, const std::string& shadowPath,
    uuid databaseID) {
    auto fileInfo = VirtualFileSystem::GetUnsafe(context)->openFile(shadowPath,
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), &context);
    ShadowFileHeader header;
    fileInfo->readFromFile(reinterpret_cast<uint8_t*>(&header), sizeof(header), 0);
    header.databaseID = databaseID;
    fileInfo->writeFile(reinterpret_cast<const uint8_t*>(&header), sizeof(header), 0);
    fileInfo->syncFile();
}

static void writeWALHeaderDatabaseID(main::ClientContext& context, const std::string& walPath,
    uuid expectedDatabaseID, uuid databaseID, bool enableChecksums) {
    auto fileInfo = VirtualFileSystem::GetUnsafe(context)->openFile(walPath,
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), &context);
    std::array<uint8_t, sizeof(uuid) + sizeof(uint8_t)> header{};
    fileInfo->readFromFile(header.data(), header.size(), 0);
    uuid currentDatabaseID;
    std::memcpy(&currentDatabaseID, header.data(), sizeof(uuid));
    if (currentDatabaseID.value != expectedDatabaseID.value) {
        throw RuntimeException("WAL header does not hold the expected database ID.");
    }
    std::memcpy(header.data(), &databaseID, sizeof(uuid));
    fileInfo->writeFile(header.data(), header.size(), 0);
    if (enableChecksums) {
        const auto headerChecksum = checksum(header.data(), header.size());
        fileInfo->writeFile(reinterpret_cast<const uint8_t*>(&headerChecksum),
            sizeof(headerChecksum), header.size());
    }
    fileInfo->syncFile();
}

static uuid foreignDatabaseID(uuid databaseID, uint64_t flippedBits = 1) {
    databaseID.value.low ^= flippedBits;
    return databaseID;
}

TEST_F(CheckpointRetryAfterFailureTest, NonStrictRecoveryDiscardsCheckpointWithTrailingRecord) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 1);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto walPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    auto fileInfo =
        vfs->openFile(walPath, FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
    const auto trailingRecord = serializeCheckpointRecord(CommitRecord{});
    auto offset = fileInfo->getFileSize();
    fileInfo->writeFile(trailingRecord.data.get(), trailingRecord.size, offset);
    offset += trailingRecord.size;
    if (context->getDBConfig()->enableChecksums) {
        const auto recordChecksum = checksum(trailingRecord.data.get(), trailingRecord.size);
        fileInfo->writeFile(reinterpret_cast<const uint8_t*>(&recordChecksum),
            sizeof(recordChecksum), offset);
    }
    fileInfo->syncFile();
    fileInfo.reset();
    const auto shadowPath = StorageUtils::getShadowFilePath(databasePath);
    writeShadowDatabaseID(*context, shadowPath,
        foreignDatabaseID(StorageManager::Get(*context)->getOrInitDatabaseID(*context)));

    systemConfig->throwOnWalReplayFailure = false;
    conn.reset();
    database.reset();
    createDBAndConn();
    checkNodes(1);
    EXPECT_FALSE(std::filesystem::exists(walPath));
    EXPECT_FALSE(std::filesystem::exists(shadowPath));
}

enum class LegacyShadowID : uint8_t { MatchesDatabase, MatchesWAL, MatchesNeither };

class LegacyMainWALDatabaseIDTest : public CheckpointRetryAfterFailureTest,
                                    public ::testing::WithParamInterface<LegacyShadowID> {};

TEST_P(LegacyMainWALDatabaseIDTest, RecoversWithSelectedGraphIDs) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto walPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    ASSERT_TRUE(std::filesystem::exists(walPath));
    const auto databaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    const auto walDatabaseID = foreignDatabaseID(databaseID);
    rewriteCheckpointRecordAsLegacy(*context, walPath);
    writeWALHeaderDatabaseID(*context, walPath, databaseID, walDatabaseID,
        context->getDBConfig()->enableChecksums);
    uuid shadowDatabaseID = databaseID;
    switch (GetParam()) {
    case LegacyShadowID::MatchesDatabase:
        break;
    case LegacyShadowID::MatchesWAL:
        shadowDatabaseID = walDatabaseID;
        break;
    case LegacyShadowID::MatchesNeither:
        shadowDatabaseID = foreignDatabaseID(databaseID, 2);
        break;
    }
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath),
        shadowDatabaseID);

    createDBAndConn();
    checkNodes(100);
    EXPECT_FALSE(std::filesystem::exists(walPath));
}

INSTANTIATE_TEST_SUITE_P(ShadowID, LegacyMainWALDatabaseIDTest,
    ::testing::Values(LegacyShadowID::MatchesDatabase, LegacyShadowID::MatchesWAL,
        LegacyShadowID::MatchesNeither),
    [](const ::testing::TestParamInfo<LegacyShadowID>& info) {
        switch (info.param) {
        case LegacyShadowID::MatchesDatabase:
            return std::string{"ShadowMatchesDatabase"};
        case LegacyShadowID::MatchesWAL:
            return std::string{"ShadowMatchesWAL"};
        case LegacyShadowID::MatchesNeither:
            return std::string{"ShadowMatchesNeither"};
        }
        return std::string{};
    });

TEST_F(CheckpointRetryAfterFailureTest, LegacyMainShadowRejectsReplacementDatabase) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    auto* storageManager = StorageManager::Get(*context);
    const auto databaseID = storageManager->getOrInitDatabaseID(*context);
    rewriteCheckpointRecordAsLegacy(*context, StorageUtils::getCheckpointWALFilePath(databasePath));
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath), databaseID);

    const auto replacementPath = databasePath + ".replacement";
    {
        main::Database replacementDatabase{replacementPath, *systemConfig};
        main::Connection replacementConnection{&replacementDatabase};
        ASSERT_TRUE(
            replacementConnection.query("CALL force_checkpoint_on_close=false;")->isSuccess());
        ASSERT_TRUE(
            replacementConnection.query("CREATE NODE TABLE replacement(id INT64 PRIMARY KEY);")
                ->isSuccess());
        ASSERT_TRUE(replacementConnection.query("CHECKPOINT;")->isSuccess());
    }
    const auto replacementHeader = readFile(replacementPath);
    ASSERT_GE(replacementHeader.size(), LBUG_PAGE_SIZE);
    storageManager->getDataFH()->getFileInfo()->writeFile(replacementHeader.data(), LBUG_PAGE_SIZE,
        0);
    storageManager->getDataFH()->getFileInfo()->syncFile();

    conn.reset();
    database.reset();
    const auto databaseBeforeRecovery = readFile(databasePath);
    try {
        createDBAndConn();
        FAIL() << "Expected a legacy main shadow to reject a replacement database file.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("Database ID"), std::string::npos) << e.what();
    }
    EXPECT_EQ(readFile(databasePath), databaseBeforeRecovery);
}

TEST_F(CheckpointRetryAfterFailureTest, BundleRejectsForeignMainWALDatabaseID) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    const auto databaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    writeWALHeaderDatabaseID(*context, StorageUtils::getCheckpointWALFilePath(databasePath),
        databaseID, foreignDatabaseID(databaseID), context->getDBConfig()->enableChecksums);
    conn.reset();
    database.reset();
    const auto databaseBeforeRecovery = readFile(databasePath);
    try {
        createDBAndConn();
        FAIL() << "Expected a bundled checkpoint with a foreign WAL to be rejected.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("Database ID"), std::string::npos) << e.what();
        EXPECT_NE(std::string{e.what()}.find("Do not delete"), std::string::npos) << e.what();
    }
    EXPECT_EQ(readFile(databasePath), databaseBeforeRecovery);
}

TEST_F(CheckpointRetryAfterFailureTest, BundleRejectsShadowWithoutCompatibilityGuard) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath),
        StorageManager::Get(*context)->getOrInitDatabaseID(*context));
    conn.reset();
    database.reset();
    const auto databaseBeforeRecovery = readFile(databasePath);
    try {
        createDBAndConn();
        FAIL() << "Expected a bundled checkpoint without its shadow guard to be rejected.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("compatibility guard"), std::string::npos) << e.what();
    }
    EXPECT_EQ(readFile(databasePath), databaseBeforeRecovery);
}

TEST_F(CheckpointRetryAfterFailureTest, RejectsNewerCheckpointFormatVersion) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    rewriteCheckpointRecord(*context, StorageUtils::getCheckpointWALFilePath(databasePath),
        CheckpointRecord{WAL::CHECKPOINT_BUNDLE_FORMAT_VERSION + 1});
    conn.reset();
    database.reset();
    const auto databaseBeforeRecovery = readFile(databasePath);
    try {
        createDBAndConn();
        FAIL() << "Expected a newer checkpoint format version to be rejected.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("unsupported checkpoint format version 2"),
            std::string::npos)
            << e.what();
    }
    EXPECT_EQ(readFile(databasePath), databaseBeforeRecovery);
}

TEST_F(CheckpointRetryAfterFailureTest, BundledShadowUsesCompatibilityGuardAndRecovers) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    const auto shadowPath = StorageUtils::getShadowFilePath(databasePath);
    auto shadowFile = vfs->openFile(shadowPath, FileOpenFlags(FileFlags::READ_ONLY), context);
    ShadowFileHeader shadowHeader;
    shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&shadowHeader), sizeof(shadowHeader), 0);
    const auto databaseHeader = DatabaseHeader::readDatabaseHeader(
        *StorageManager::Get(*context)->getDataFH()->getFileInfo());
    ASSERT_TRUE(databaseHeader.has_value());
    EXPECT_EQ(shadowHeader.databaseID.value, ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID.value);
    EXPECT_NE(shadowHeader.databaseID.value, databaseHeader->databaseID.value);
    shadowFile.reset();

    createDBAndConn();
    checkNodes(100);
}

TEST_F(CheckpointRetryAfterFailureTest, FinalDuplicateDatabaseHeaderIsValidatedBeforeReplay) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();

    auto* context = getClientContext(*conn);
    auto* storageManager = StorageManager::Get(*context);
    auto* dataFile = storageManager->getDataFH()->getFileInfo();
    const auto databaseBeforeReplay = readFile(databasePath);
    auto corruptHeader = DatabaseHeader::readDatabaseHeader(*dataFile);
    ASSERT_TRUE(corruptHeader.has_value());
    corruptHeader->databaseID.value.low ^= 1;

    auto headerWriter = std::make_shared<BufferWriter>(LBUG_PAGE_SIZE);
    Serializer headerSerializer{headerWriter};
    corruptHeader->serialize(headerSerializer);
    ASSERT_LE(headerWriter->getSize(), LBUG_PAGE_SIZE);
    std::vector<uint8_t> corruptHeaderPage(LBUG_PAGE_SIZE);
    std::memcpy(corruptHeaderPage.data(), headerWriter->getBlobData(), headerWriter->getSize());

    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    const auto shadowPath = StorageUtils::getShadowFilePath(databasePath);
    auto shadowFile =
        vfs->openFile(shadowPath, FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
    ShadowFileHeader shadowHeader;
    shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&shadowHeader), sizeof(shadowHeader), 0);
    ASSERT_GT(shadowHeader.numShadowPages, 0);
    const auto oldRecordsOffset =
        (static_cast<uint64_t>(shadowHeader.numShadowPages) + 1) * LBUG_PAGE_SIZE;
    const auto oldRecordsSize = shadowFile->getFileSize() - oldRecordsOffset;
    auto oldRecords = std::make_unique<uint8_t[]>(oldRecordsSize);
    shadowFile->readFromFile(oldRecords.get(), oldRecordsSize, oldRecordsOffset);
    Deserializer recordDeserializer{
        std::make_unique<BufferReader>(oldRecords.get(), oldRecordsSize)};
    std::vector<ShadowPageRecord> records;
    recordDeserializer.deserializeVector(records);
    const auto pageZeroRecord =
        std::find_if(records.rbegin(), records.rend(), [](const auto& record) {
            return record.originalPageIdx == StorageConstants::DB_HEADER_PAGE_IDX;
        });
    ASSERT_NE(pageZeroRecord, records.rend());
    records.push_back({pageZeroRecord->originalFileIdx, StorageConstants::DB_HEADER_PAGE_IDX});

    auto recordsWriter = std::make_shared<BufferWriter>();
    Serializer recordsSerializer{recordsWriter};
    recordsSerializer.serializeVector(records);
    shadowFile->writeFile(corruptHeaderPage.data(), corruptHeaderPage.size(), oldRecordsOffset);
    const auto newRecordsOffset = oldRecordsOffset + LBUG_PAGE_SIZE;
    shadowFile->writeFile(recordsWriter->getBlobData(), recordsWriter->getSize(), newRecordsOffset);
    shadowFile->truncate(newRecordsOffset + recordsWriter->getSize());
    shadowHeader.numShadowPages++;
    shadowFile->writeFile(reinterpret_cast<const uint8_t*>(&shadowHeader), sizeof(shadowHeader), 0);
    shadowFile->syncFile();
    shadowFile.reset();

    EXPECT_THROW(ShadowFile::replayShadowPageRecordsForStorageManager(*context, *storageManager,
                     ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID),
        RuntimeException);
    EXPECT_EQ(readFile(databasePath), databaseBeforeReplay);
}

struct GraphCheckpointRecoveryParam {
    bool walRotated;
    bool graphShadowAlreadyApplied;
    bool legacySelectedGraphID;
};

class GraphCheckpointRecoveryTest
    : public FlakyCheckpointerTest,
      public ::testing::WithParamInterface<GraphCheckpointRecoveryParam> {};

TEST_P(GraphCheckpointRecoveryTest, MainMarkerRecoversGraphShadow) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE main_test(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    if (GetParam().legacySelectedGraphID) {
        ASSERT_TRUE(conn->query("CREATE GRAPH selector_graph ANY;")->isSuccess());
    }
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:main_test {id: 1});")->isSuccess());
    if (GetParam().legacySelectedGraphID) {
        ASSERT_TRUE(conn->query("USE GRAPH selector_graph;")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:Selector {name: 'Bob'});")->isSuccess());
    }
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());

    auto* context = getClientContext(*conn);
    if (!GetParam().walRotated) {
        WAL::Get(*context)->reset();
    }
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerFailsBeforeGraphShadowApply>(clientContext);
    }).setCheckpointer(*context);
    auto result = conn->query("CHECKPOINT;");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_NE(result->getErrorMessage().find("before graph shadow apply"), std::string::npos);
    auto rejectedWrite = conn->query("CREATE (:main_test {id: 2});");
    ASSERT_FALSE(rejectedWrite->isSuccess());
    ASSERT_NE(rejectedWrite->getErrorMessage().find("panic state"), std::string::npos);

    const auto mainWALPath = GetParam().walRotated ?
                                 StorageUtils::getCheckpointWALFilePath(databasePath) :
                                 StorageUtils::getWALFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    ASSERT_TRUE(std::filesystem::exists(mainWALPath));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    if (GetParam().legacySelectedGraphID) {
        rewriteCheckpointRecordAsLegacy(*context, mainWALPath);
        const auto selectedDatabaseID = main::DatabaseManager::Get(*context)
                                            ->getGraphCatalog("selector_graph")
                                            ->getStorageManager()
                                            ->getOrInitDatabaseID(*context);
        graphStorageManager->getWAL().reset();
        ASSERT_TRUE(conn->query("USE GRAPH selector_graph;")->isSuccess());
        graphStorageManager->getWAL().logAndFlushCheckpoint(context);
        const auto graphWALPath =
            StorageUtils::getWALFilePath(graphStorageManager->getDatabasePath());
        rewriteCheckpointRecordAsLegacy(*context, graphWALPath,
            false /* enableChecksumsOverride */);
        writeWALHeaderDatabaseID(*context, graphWALPath,
            graphStorageManager->getOrInitDatabaseID(*context), selectedDatabaseID,
            false /* enableChecksums */);
        ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
        const auto selectorGraphPath = StorageUtils::getGraphPath(databasePath, "selector_graph");
        for (const auto& path : {StorageUtils::getShadowFilePath(databasePath), graphShadowPath,
                 StorageUtils::getShadowFilePath(selectorGraphPath)}) {
            writeShadowDatabaseID(*context, path, selectedDatabaseID);
        }
    }
    if (GetParam().graphShadowAlreadyApplied) {
        ShadowFile::replayShadowPageRecordsForStorageManager(*context, *graphStorageManager,
            ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID);
        ASSERT_TRUE(std::filesystem::exists(graphShadowPath));
    }

    createDBAndConn();
    result = conn->query("MATCH (n:main_test) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    if (GetParam().legacySelectedGraphID) {
        ASSERT_TRUE(conn->query("USE GRAPH selector_graph;")->isSuccess());
        result = conn->query("MATCH (n:Selector) RETURN COUNT(n);");
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
        ASSERT_FALSE(std::filesystem::exists(StorageUtils::getShadowFilePath(
            StorageUtils::getGraphPath(databasePath, "selector_graph"))));
    }
    ASSERT_FALSE(std::filesystem::exists(mainWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphShadowPath));
}

TEST_F(FlakyCheckpointerTest, LegacyGraphShadowRejectsReplacementBaseBeforeReplay) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE main_test(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH selector_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:main_test {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());

    auto* context = getClientContext(*conn);
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerFailsBeforeGraphShadowApply>(clientContext);
    }).setCheckpointer(*context);
    ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());

    const auto mainWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    rewriteCheckpointRecordAsLegacy(*context, mainWALPath);
    const auto selectedDatabaseID = main::DatabaseManager::Get(*context)
                                        ->getGraphCatalog("selector_graph")
                                        ->getStorageManager()
                                        ->getOrInitDatabaseID(*context);
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath),
        selectedDatabaseID);
    writeShadowDatabaseID(*context, graphShadowPath, selectedDatabaseID);

    const auto replacementPath = databasePath + ".replacement";
    {
        main::Database replacementDatabase{replacementPath, *systemConfig};
        main::Connection replacementConnection{&replacementDatabase};
        ASSERT_TRUE(
            replacementConnection.query("CALL force_checkpoint_on_close=false;")->isSuccess());
        ASSERT_TRUE(replacementConnection.query("CALL auto_checkpoint=false;")->isSuccess());
        ASSERT_TRUE(
            replacementConnection.query("CREATE NODE TABLE replacement(id INT64 PRIMARY KEY);")
                ->isSuccess());
        ASSERT_TRUE(replacementConnection.query("CHECKPOINT;")->isSuccess());
    }
    const auto replacementHeader = readFile(replacementPath);
    ASSERT_GE(replacementHeader.size(), LBUG_PAGE_SIZE);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    graphStorageManager->getWAL().reset();
    std::filesystem::remove(StorageUtils::getCheckpointWALFilePath(graphPath));
    graphStorageManager->getWAL().logAndFlushCheckpoint(context);
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    rewriteCheckpointRecordAsLegacy(*context, graphWALPath, false /* enableChecksumsOverride */);
    writeWALHeaderDatabaseID(*context, graphWALPath,
        graphStorageManager->getOrInitDatabaseID(*context), selectedDatabaseID,
        false /* enableChecksums */);
    graphStorageManager->getDataFH()->getFileInfo()->writeFile(replacementHeader.data(),
        LBUG_PAGE_SIZE, 0);
    graphStorageManager->getDataFH()->getFileInfo()->syncFile();

    conn.reset();
    database.reset();
    const auto graphBeforeRecovery = readFile(graphPath);
    try {
        createDBAndConn();
        FAIL() << "Expected legacy graph shadow to reject a replacement graph file.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("Database ID"), std::string::npos) << e.what();
        EXPECT_NE(std::string{e.what()}.find(graphShadowPath), std::string::npos) << e.what();
    }
    EXPECT_EQ(readFile(graphPath), graphBeforeRecovery);
}

static void writeShadowPageZeroDatabaseID(main::ClientContext& context,
    const std::string& shadowPath, uuid databaseID) {
    auto fileInfo = VirtualFileSystem::GetUnsafe(context)->openFile(shadowPath,
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), &context);
    ShadowFileHeader header;
    fileInfo->readFromFile(reinterpret_cast<uint8_t*>(&header), sizeof(header), 0);
    const auto recordsOffset = (static_cast<uint64_t>(header.numShadowPages) + 1) * LBUG_PAGE_SIZE;
    const auto recordsSize = fileInfo->getFileSize() - recordsOffset;
    auto recordsData = std::make_unique<uint8_t[]>(recordsSize);
    fileInfo->readFromFile(recordsData.get(), recordsSize, recordsOffset);
    Deserializer recordsDeserializer{
        std::make_unique<BufferReader>(recordsData.get(), recordsSize)};
    std::vector<ShadowPageRecord> records;
    recordsDeserializer.deserializeVector(records);
    const auto headerRecord =
        std::find_if(records.rbegin(), records.rend(), [](const auto& record) {
            return record.originalPageIdx == StorageConstants::DB_HEADER_PAGE_IDX;
        });
    if (headerRecord == records.rend()) {
        throw RuntimeException("Shadow file has no database header page.");
    }
    const auto shadowPageOffset =
        (records.size() - static_cast<size_t>(std::distance(records.rbegin(), headerRecord))) *
        LBUG_PAGE_SIZE;
    std::vector<uint8_t> page(LBUG_PAGE_SIZE);
    fileInfo->readFromFile(page.data(), page.size(), shadowPageOffset);
    Deserializer headerDeserializer{std::make_unique<BufferReader>(page.data(), page.size())};
    auto databaseHeader = DatabaseHeader::deserialize(headerDeserializer);
    databaseHeader.databaseID = databaseID;
    auto headerWriter = std::make_shared<BufferWriter>(LBUG_PAGE_SIZE);
    Serializer headerSerializer{headerWriter};
    databaseHeader.serialize(headerSerializer);
    std::fill(page.begin(), page.end(), 0);
    std::memcpy(page.data(), headerWriter->getBlobData(), headerWriter->getSize());
    fileInfo->writeFile(page.data(), page.size(), shadowPageOffset);
    fileInfo->syncFile();
}

enum class LegacyGraphShadowArtifact : uint8_t {
    MainStamped,
    ReplacementGraphBase,
    ForeignPageZero,
    UncommittedBundle
};

class LegacyGraphMarkerAfterMainCheckpointTest
    : public FlakyCheckpointerTest,
      public ::testing::WithParamInterface<LegacyGraphShadowArtifact> {};

TEST_P(LegacyGraphMarkerAfterMainCheckpointTest, ValidatesGraphShadowByPageZero) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const auto artifact = GetParam();
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE main_test(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:main_test {id: 1});")->isSuccess());

    auto* context = getClientContext(*conn);
    WAL::Get(*context)->reset();
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerFailsBeforeGraphShadowApply>(clientContext);
    }).setCheckpointer(*context);
    ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());

    const auto mainWALPath = StorageUtils::getWALFilePath(databasePath);
    const auto mainShadowPath = StorageUtils::getShadowFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    ASSERT_TRUE(std::filesystem::exists(mainWALPath));
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    const auto graphDatabaseID = graphStorageManager->getOrInitDatabaseID(*context);
    graphStorageManager->getWAL().reset();
    graphStorageManager->getWAL().logAndFlushCheckpoint(context);
    rewriteCheckpointRecordAsLegacy(*context, graphWALPath, false /* enableChecksumsOverride */);
    writeWALHeaderDatabaseID(*context, graphWALPath, graphDatabaseID, mainDatabaseID,
        false /* enableChecksums */);
    switch (artifact) {
    case LegacyGraphShadowArtifact::MainStamped:
        writeShadowDatabaseID(*context, graphShadowPath, mainDatabaseID);
        break;
    case LegacyGraphShadowArtifact::ReplacementGraphBase: {
        writeShadowDatabaseID(*context, graphShadowPath, mainDatabaseID);
        const auto replacementPath = databasePath + ".replacement";
        {
            main::Database replacementDatabase{replacementPath, *systemConfig};
            main::Connection replacementConnection{&replacementDatabase};
            ASSERT_TRUE(
                replacementConnection.query("CALL force_checkpoint_on_close=false;")->isSuccess());
            ASSERT_TRUE(
                replacementConnection.query("CREATE NODE TABLE replacement(id INT64 PRIMARY KEY);")
                    ->isSuccess());
            ASSERT_TRUE(replacementConnection.query("CHECKPOINT;")->isSuccess());
        }
        const auto replacementHeader = readFile(replacementPath);
        ASSERT_GE(replacementHeader.size(), LBUG_PAGE_SIZE);
        graphStorageManager->getDataFH()->getFileInfo()->writeFile(replacementHeader.data(),
            LBUG_PAGE_SIZE, 0);
        graphStorageManager->getDataFH()->getFileInfo()->syncFile();
    } break;
    case LegacyGraphShadowArtifact::ForeignPageZero:
        writeShadowDatabaseID(*context, graphShadowPath, graphDatabaseID);
        writeShadowPageZeroDatabaseID(*context, graphShadowPath,
            foreignDatabaseID(graphDatabaseID));
        break;
    case LegacyGraphShadowArtifact::UncommittedBundle:
        break;
    }
    conn.reset();
    database.reset();
    std::filesystem::resize_file(mainWALPath, 0);
    ASSERT_TRUE(std::filesystem::remove(mainShadowPath));

    if (artifact != LegacyGraphShadowArtifact::MainStamped) {
        const auto graphBeforeRecovery = readFile(graphPath);
        try {
            createDBAndConn();
            FAIL() << "Expected the legacy graph shadow to be rejected.";
        } catch (const RuntimeException& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find(graphShadowPath), std::string::npos) << message;
            if (artifact == LegacyGraphShadowArtifact::UncommittedBundle) {
                EXPECT_NE(message.find("never committed"), std::string::npos) << message;
            } else {
                EXPECT_NE(message.find("Database ID"), std::string::npos) << message;
            }
        }
        EXPECT_EQ(readFile(graphPath), graphBeforeRecovery);
        return;
    }
    createDBAndConn();
    auto result = conn->query("MATCH (n:main_test) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    EXPECT_FALSE(std::filesystem::exists(graphWALPath));
    EXPECT_FALSE(std::filesystem::exists(graphShadowPath));
}

INSTANTIATE_TEST_SUITE_P(Artifact, LegacyGraphMarkerAfterMainCheckpointTest,
    ::testing::Values(LegacyGraphShadowArtifact::MainStamped,
        LegacyGraphShadowArtifact::ReplacementGraphBase, LegacyGraphShadowArtifact::ForeignPageZero,
        LegacyGraphShadowArtifact::UncommittedBundle),
    [](const ::testing::TestParamInfo<LegacyGraphShadowArtifact>& info) {
        switch (info.param) {
        case LegacyGraphShadowArtifact::MainStamped:
            return std::string{"MainStamped"};
        case LegacyGraphShadowArtifact::ReplacementGraphBase:
            return std::string{"ReplacementGraphBase"};
        case LegacyGraphShadowArtifact::ForeignPageZero:
            return std::string{"ForeignPageZero"};
        case LegacyGraphShadowArtifact::UncommittedBundle:
            return std::string{"UncommittedBundle"};
        }
        return std::string{};
    });

struct LegacyGraphMarkerRecoveryFixture {
    void setUp(main::Connection* connection) {
        ASSERT_TRUE(connection->query("CALL force_checkpoint_on_close=false;")->isSuccess());
        ASSERT_TRUE(connection->query("CALL auto_checkpoint=false;")->isSuccess());
        ASSERT_TRUE(
            connection->query("CREATE NODE TABLE main_test(id INT64 PRIMARY KEY);")->isSuccess());
        ASSERT_TRUE(connection->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
        ASSERT_TRUE(connection->query("USE GRAPH recovery_graph;")->isSuccess());
        ASSERT_TRUE(connection->query("CREATE (:User {name: 'Alice'});")->isSuccess());
        ASSERT_TRUE(connection->query("USE GRAPH main;")->isSuccess());
        ASSERT_TRUE(connection->query("CREATE (:main_test {id: 1});")->isSuccess());
        ASSERT_TRUE(connection->query("CHECKPOINT;")->isSuccess());
    }
};

TEST_F(FlakyCheckpointerTest, RecoversLegacyFrozenGraphMarkerWithActiveWALCommits) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());

    // Current builds log graph-table commits to the main WAL, so a graph WAL holding real
    // commit records can only be produced by opening the graph data file as a standalone
    // database. The standalone session commits Carol and then fails its checkpoint after the
    // shadow is flushed but before the marker is logged, so Carol's pages live only in the
    // shadow; the later legacy marker subsumes her frozen-WAL records, so recovery replays
    // only the active WAL on top of the shadow. Carol is therefore recoverable only by
    // replaying the shadow, which distinguishes shadow replay from shadow skip, while Bob's
    // active-WAL commit must still be replayed on top. A standalone session has no default
    // graph, so the ANY-graph insert rewrite does not fire there and the committed rows are
    // written with null label/data columns (the serial ids are still assigned and logged), so
    // the replayed rows are asserted by id.
    auto* context = getClientContext(*conn);
    const auto mainWALPath = StorageUtils::getWALFilePath(databasePath);
    const auto mainShadowPath = StorageUtils::getShadowFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    ASSERT_FALSE(std::filesystem::exists(mainWALPath));
    ASSERT_FALSE(std::filesystem::exists(mainShadowPath));
    ASSERT_FALSE(std::filesystem::exists(graphWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphCheckpointWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphShadowPath));
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    const auto graphDatabaseID = graphStorageManager->getOrInitDatabaseID(*context);

    conn.reset();
    database.reset();

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    graphConfig.enableChecksums = false;
    auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto* graphContext = getClientContext(*graphConnection);
    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Carol'});")->isSuccess());
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<FlakyCheckpointerFailsOnLoggingCheckpoint>(clientContext);
    }).setCheckpointer(*graphContext);
    ASSERT_FALSE(graphConnection->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*graphContext);
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));
    // The failed checkpoint undid its WAL rotation, so Carol's commit is back in the active
    // WAL and the frozen WAL is gone.
    ASSERT_TRUE(std::filesystem::exists(graphWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphCheckpointWALPath));

    auto& graphWAL = graphDatabase->getStorageManager()->getWAL();
    graphWAL.logAndFlushCheckpoint(graphContext);
    rewriteCheckpointRecordAsLegacy(*graphContext, graphWALPath,
        false /* enableChecksumsOverride */);
    writeWALHeaderDatabaseID(*graphContext, graphWALPath, graphDatabaseID, mainDatabaseID,
        false /* enableChecksums */);
    writeShadowDatabaseID(*graphContext, graphShadowPath, mainDatabaseID);
    std::filesystem::rename(graphWALPath, graphCheckpointWALPath);
    graphWAL.reset();

    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    ASSERT_TRUE(std::filesystem::exists(graphWALPath));

    graphConnection.reset();
    graphDatabase.reset();

    createDBAndConn();
    auto result = conn->query("MATCH (n:main_test) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n:User {name: 'Alice'}) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n) WHERE n.id = 1 RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1)
        << "Carol is recoverable only by replaying the interrupted checkpoint's shadow.";
    result = conn->query("MATCH (n) WHERE n.id = 2 RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1)
        << "Bob's active-WAL commit must be replayed on top of the recovered shadow state.";
    result = conn->query("MATCH (n) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3);
    EXPECT_FALSE(std::filesystem::exists(graphCheckpointWALPath));
    EXPECT_FALSE(std::filesystem::exists(graphShadowPath));
}

TEST_F(FlakyCheckpointerTest, RejectsActiveGraphMarkerWithNonCheckpointFrozenWAL) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());

    auto* context = getClientContext(*conn);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    const auto graphDatabaseID = graphStorageManager->getOrInitDatabaseID(*context);

    conn.reset();
    database.reset();

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    graphConfig.enableChecksums = false;
    auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto* graphContext = getClientContext(*graphConnection);
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<FlakyCheckpointerFailsOnLoggingCheckpoint>(clientContext);
    }).setCheckpointer(*graphContext);
    ASSERT_FALSE(graphConnection->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*graphContext);
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));

    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    std::filesystem::copy_file(graphWALPath, graphCheckpointWALPath);
    auto& graphWAL = graphDatabase->getStorageManager()->getWAL();
    graphWAL.reset();
    graphWAL.logAndFlushCheckpoint(graphContext);
    rewriteCheckpointRecordAsLegacy(*graphContext, graphWALPath,
        false /* enableChecksumsOverride */);
    writeWALHeaderDatabaseID(*graphContext, graphWALPath, graphDatabaseID, mainDatabaseID,
        false /* enableChecksums */);
    writeShadowDatabaseID(*graphContext, graphShadowPath, mainDatabaseID);

    graphConnection.reset();
    graphDatabase.reset();

    try {
        createDBAndConn();
        FAIL() << "Expected an active-WAL graph checkpoint marker to conflict with a "
                  "non-checkpoint frozen graph WAL.";
    } catch (const RuntimeException& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("an active-WAL checkpoint marker conflicts with a non-checkpoint "
                               "frozen WAL"),
            std::string::npos)
            << message;
    }
    EXPECT_TRUE(std::filesystem::exists(graphWALPath));
    EXPECT_TRUE(std::filesystem::exists(graphCheckpointWALPath));
}

TEST_F(FlakyCheckpointerTest, RecoversInterruptedStandaloneGraphBundleCheckpoint) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());

    // A standalone session that has already committed an insert rotates the graph WAL at its
    // next checkpoint and appends a version 1 CHECKPOINT record to the frozen WAL. A crash
    // after that record is durable — with the graph shadow flushed but not yet applied —
    // leaves the checkpoint's committed pages only in the shadow, so the parent reopen must
    // accept the bundle-format marker and replay the sentinel-stamped shadow. The graph WAL
    // keeps the standalone session's own checksum setting, so recovery must take the checksum
    // flag from the WAL header; this test runs with the harness's default checksums enabled.
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);

    conn.reset();
    database.reset();

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto* graphContext = getClientContext(*graphConnection);
    // A standalone session has no default graph, so the ANY-graph insert rewrite does not
    // fire there and the committed row is written with null label/data columns (the serial
    // id is still assigned). The row is therefore asserted by its id after recovery.
    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(clientContext);
    }).setCheckpointer(*graphContext);
    ASSERT_FALSE(graphConnection->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*graphContext);
    ASSERT_TRUE(std::filesystem::exists(graphCheckpointWALPath));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));

    graphConnection.reset();
    graphDatabase.reset();

    createDBAndConn();
    auto result = conn->query("MATCH (n:main_test) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User {name: 'Alice'}) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    result = conn->query("MATCH (n) WHERE n.id = 1 RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1)
        << "Bob is recoverable only by applying the interrupted checkpoint's shadow.";
    result = conn->query("MATCH (n) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    EXPECT_FALSE(std::filesystem::exists(graphWALPath));
    EXPECT_FALSE(std::filesystem::exists(graphCheckpointWALPath));
    EXPECT_FALSE(std::filesystem::exists(graphShadowPath));
}

TEST_F(FlakyCheckpointerTest, BundleRejectsForeignGraphCheckpointWALDatabaseID) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());

    // RecoversInterruptedStandaloneGraphBundleCheckpoint is the positive row: the parent
    // reopen recovers a standalone session's interrupted bundle checkpoint. Here the same
    // bundle's graph WAL header is rewritten to the parent's database ID. A version 1 marker
    // must still be checked against the graph data file's identity, so the reopen is
    // rejected before the sentinel-stamped shadow is applied and every recovery artifact is
    // left untouched.
    auto* context = getClientContext(*conn);
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    const auto graphDatabaseID = graphStorageManager->getOrInitDatabaseID(*context);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);

    conn.reset();
    database.reset();

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto* graphContext = getClientContext(*graphConnection);
    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(clientContext);
    }).setCheckpointer(*graphContext);
    ASSERT_FALSE(graphConnection->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*graphContext);
    ASSERT_TRUE(std::filesystem::exists(graphCheckpointWALPath));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));

    writeWALHeaderDatabaseID(*graphContext, graphCheckpointWALPath, graphDatabaseID, mainDatabaseID,
        graphContext->getDBConfig()->enableChecksums);

    graphConnection.reset();
    graphDatabase.reset();

    const auto graphDataBefore = readFile(graphPath);
    const auto graphCheckpointWALBefore = readFile(graphCheckpointWALPath);
    const auto graphShadowBefore = readFile(graphShadowPath);
    try {
        createDBAndConn();
        FAIL() << "Expected a bundled graph checkpoint with a foreign WAL to be rejected.";
    } catch (const RuntimeException& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("Database ID"), std::string::npos) << message;
        EXPECT_NE(message.find("Do not delete the WAL or shadow files"), std::string::npos)
            << message;
        EXPECT_NE(message.find("Restore the database file they were written for"),
            std::string::npos)
            << message;
    }
    EXPECT_EQ(readFile(graphPath), graphDataBefore);
    EXPECT_EQ(readFile(graphCheckpointWALPath), graphCheckpointWALBefore);
    EXPECT_EQ(readFile(graphShadowPath), graphShadowBefore);
}

TEST_F(FlakyCheckpointerTest, StandaloneGraphOpenPreservesParentOwnedBundle) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());

    // The parent's bundle checkpoint commits its marker, then crashes before applying the
    // graph shadow. Bob's committed pages then live only in a bundle whose fate only the
    // parent's WAL can decide, so a standalone open of the graph must neither replay that
    // bundle nor discard its shadow: it must refuse and leave every recovery artifact
    // untouched.
    auto* context = getClientContext(*conn);
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerFailsBeforeGraphShadowApply>(clientContext);
    }).setCheckpointer(*context);
    ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*context);

    const auto mainCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    ASSERT_TRUE(std::filesystem::exists(mainCheckpointWALPath));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));
    {
        auto vfs = VirtualFileSystem::GetUnsafe(*context);
        auto shadowFile =
            vfs->openFile(graphShadowPath, FileOpenFlags(FileFlags::READ_ONLY), context);
        ShadowFileHeader shadowHeader;
        shadowFile->readFromFile(reinterpret_cast<uint8_t*>(&shadowHeader), sizeof(shadowHeader),
            0);
        ASSERT_EQ(shadowHeader.databaseID.value, ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID.value);
        ASSERT_EQ(shadowHeader.ownerDatabaseID.value, mainDatabaseID.value);
    }

    conn.reset();
    database.reset();
    const auto graphDataBefore = readFile(graphPath);
    const auto graphShadowBefore = readFile(graphShadowPath);

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    try {
        auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
        FAIL() << "Expected a standalone open of a graph with a parent-owned pending bundle to "
                  "be refused.";
    } catch (const RuntimeException& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("pending checkpoint bundle owned by another database"),
            std::string::npos)
            << message;
    }
    EXPECT_EQ(readFile(graphPath), graphDataBefore);
    EXPECT_EQ(readFile(graphShadowPath), graphShadowBefore);
    EXPECT_TRUE(std::filesystem::exists(mainCheckpointWALPath));

    createDBAndConn();
    auto result = conn->query("MATCH (n:main_test) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User {name: 'Bob'}) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1)
        << "Bob is recoverable only through the preserved parent-owned bundle.";
    result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    EXPECT_FALSE(std::filesystem::exists(graphShadowPath));
}

TEST_F(FlakyCheckpointerTest, RejectsNewerGraphCheckpointFormatVersion) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());

    // A standalone session's bundle checkpoint is interrupted after its version 1 marker
    // commits. Rewriting that marker to a newer format version must make the parent reopen
    // reject the graph WAL before the graph shadow is applied or removed, leaving every
    // recovery artifact untouched.
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);

    conn.reset();
    database.reset();

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto* graphContext = getClientContext(*graphConnection);
    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(clientContext);
    }).setCheckpointer(*graphContext);
    ASSERT_FALSE(graphConnection->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*graphContext);
    ASSERT_TRUE(std::filesystem::exists(graphCheckpointWALPath));
    rewriteCheckpointRecord(*graphContext, graphCheckpointWALPath,
        CheckpointRecord{WAL::CHECKPOINT_BUNDLE_FORMAT_VERSION + 1});

    graphConnection.reset();
    graphDatabase.reset();

    const auto graphDataBefore = readFile(graphPath);
    const auto graphCheckpointWALBefore = readFile(graphCheckpointWALPath);
    const auto graphShadowBefore = readFile(graphShadowPath);
    try {
        createDBAndConn();
        FAIL() << "Expected a newer graph checkpoint format version to be rejected.";
    } catch (const RuntimeException& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("Cannot recover graph WAL"), std::string::npos) << message;
        EXPECT_NE(message.find("unsupported checkpoint format version 2"), std::string::npos)
            << message;
    }
    EXPECT_EQ(readFile(graphPath), graphDataBefore);
    EXPECT_EQ(readFile(graphCheckpointWALPath), graphCheckpointWALBefore);
    EXPECT_EQ(readFile(graphShadowPath), graphShadowBefore);
}

TEST_F(FlakyCheckpointerTest, RejectsGraphBundleShadowWithoutCompatibilityGuard) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    LegacyGraphMarkerRecoveryFixture{}.setUp(conn.get());

    // Same interrupted state as RecoversInterruptedStandaloneGraphBundleCheckpoint, except the
    // graph shadow's sentinel is replaced with the graph's real database ID. A version 1
    // checkpoint only commits a sentinel-stamped shadow, so recovery must reject the shadow
    // instead of applying it, and must leave every recovery artifact untouched.
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);

    conn.reset();
    database.reset();

    auto graphConfig = *systemConfig;
    graphConfig.autoCheckpoint = false;
    graphConfig.forceCheckpointOnClose = false;
    auto graphDatabase = std::make_unique<main::Database>(graphPath, graphConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto* graphContext = getClientContext(*graphConnection);
    ASSERT_TRUE(graphConnection->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(clientContext);
    }).setCheckpointer(*graphContext);
    ASSERT_FALSE(graphConnection->query("CHECKPOINT;")->isSuccess());
    FlakyCheckpointer::resetCheckpointer(*graphContext);
    const auto graphDatabaseID =
        graphDatabase->getStorageManager()->getOrInitDatabaseID(*graphContext);
    writeShadowDatabaseID(*graphContext, graphShadowPath, graphDatabaseID);

    graphConnection.reset();
    graphDatabase.reset();

    const auto graphDataBefore = readFile(graphPath);
    const auto graphCheckpointWALBefore = readFile(graphCheckpointWALPath);
    const auto graphShadowBefore = readFile(graphShadowPath);
    try {
        createDBAndConn();
        FAIL() << "Expected a version 1 graph checkpoint whose shadow is not sentinel-stamped "
                  "to be rejected.";
    } catch (const RuntimeException& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("is missing its checkpoint-bundle compatibility guard"),
            std::string::npos)
            << message;
    }
    EXPECT_EQ(readFile(graphPath), graphDataBefore);
    EXPECT_EQ(readFile(graphCheckpointWALPath), graphCheckpointWALBefore);
    EXPECT_EQ(readFile(graphShadowPath), graphShadowBefore);
}

class CheckpointerObservesWALRetirement final : public Checkpointer {
public:
    CheckpointerObservesWALRetirement(main::ClientContext& context, std::function<void()> observe)
        : Checkpointer(context), observe{std::move(observe)} {}

    void beforeWALRetirement(bool) override {
        observe();
        throw RuntimeException("checkpoint interrupted before retiring the main WAL.");
    }

private:
    std::function<void()> observe;
};

TEST_F(FlakyCheckpointerTest, GraphWALRetiresBeforeMainMarker) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE main_test(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:main_test {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());

    const auto mainCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    const auto graphCheckpointWALPath = StorageUtils::getCheckpointWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    std::ofstream(graphWALPath).close();
    std::ofstream(graphCheckpointWALPath).close();

    bool reachedBeforeMainRetirement = false;
    bool graphWALsRetiredFirst = false;
    FlakyCheckpointer([&](main::ClientContext& context) {
        return std::make_unique<CheckpointerObservesWALRetirement>(context, [&] {
            reachedBeforeMainRetirement = true;
            graphWALsRetiredFirst = !std::filesystem::exists(graphWALPath) &&
                                    !std::filesystem::exists(graphCheckpointWALPath) &&
                                    std::filesystem::exists(mainCheckpointWALPath);
        });
    }).setCheckpointer(*getClientContext(*conn));
    const auto checkpointResult = conn->query("CHECKPOINT;");
    ASSERT_FALSE(checkpointResult->isSuccess());
    ASSERT_TRUE(reachedBeforeMainRetirement);
    EXPECT_TRUE(graphWALsRetiredFirst);
    ASSERT_TRUE(std::filesystem::exists(mainCheckpointWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphCheckpointWALPath));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));

    createDBAndConn();
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    auto result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_FALSE(std::filesystem::exists(graphWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphCheckpointWALPath));
    ASSERT_FALSE(std::filesystem::exists(graphShadowPath));
}

class TornGraphWALHeaderTest : public FlakyCheckpointerTest,
                               public ::testing::WithParamInterface<bool> {};

TEST_P(TornGraphWALHeaderTest, FollowsReplayFailureMode) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const bool throwOnWalReplayFailure = GetParam();
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    conn.reset();
    database.reset();

    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    ASSERT_FALSE(std::filesystem::exists(graphWALPath));
    {
        std::ofstream torn{graphWALPath, std::ios::binary};
        const std::string partialHeader(sizeof(uuid) / 2, '\xAB');
        torn.write(partialHeader.data(), static_cast<std::streamsize>(partialHeader.size()));
    }

    systemConfig->throwOnWalReplayFailure = throwOnWalReplayFailure;
    if (throwOnWalReplayFailure) {
        EXPECT_ANY_THROW(createDBAndConn());
        EXPECT_TRUE(std::filesystem::exists(graphWALPath));
        return;
    }
    createDBAndConn();
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    auto result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    EXPECT_FALSE(std::filesystem::exists(graphWALPath));
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    createDBAndConn();
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
}

INSTANTIATE_TEST_SUITE_P(ReplayFailureMode, TornGraphWALHeaderTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Strict" : "NonStrict"; });

class LegacyGraphMarkerWithoutShadowTest : public FlakyCheckpointerTest,
                                           public ::testing::WithParamInterface<bool> {};

TEST_P(LegacyGraphMarkerWithoutShadowTest, FailsClosed) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const bool frozenMarker = GetParam();
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());

    auto* context = getClientContext(*conn);
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    const auto markerPath =
        frozenMarker ? StorageUtils::getCheckpointWALFilePath(graphPath) : graphWALPath;
    graphStorageManager->getWAL().logAndFlushCheckpoint(context);
    rewriteCheckpointRecordAsLegacy(*context, graphWALPath, false /* enableChecksumsOverride */);
    writeWALHeaderDatabaseID(*context, graphWALPath,
        graphStorageManager->getOrInitDatabaseID(*context), mainDatabaseID,
        false /* enableChecksums */);
    conn.reset();
    database.reset();
    if (frozenMarker) {
        std::filesystem::rename(graphWALPath, markerPath);
    }
    ASSERT_TRUE(std::filesystem::exists(markerPath));
    ASSERT_FALSE(std::filesystem::exists(graphShadowPath));
    const auto graphBeforeRecovery = readFile(graphPath);

    try {
        createDBAndConn();
        FAIL() << "Expected a legacy graph marker without its shadow to be rejected.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find(graphShadowPath + " is missing"), std::string::npos)
            << e.what();
    }
    EXPECT_TRUE(std::filesystem::exists(markerPath));
    EXPECT_EQ(readFile(graphPath), graphBeforeRecovery);
}

INSTANTIATE_TEST_SUITE_P(MarkerWAL, LegacyGraphMarkerWithoutShadowTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Frozen" : "Active"; });

TEST_F(FlakyCheckpointerTest, TornLegacyGraphCheckpointRecordIsDiscarded) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());

    auto* context = getClientContext(*conn);
    const auto mainDatabaseID = StorageManager::Get(*context)->getOrInitDatabaseID(*context);
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);
    graphStorageManager->getWAL().logAndFlushCheckpoint(context);
    rewriteCheckpointRecordAsLegacy(*context, graphWALPath, false /* enableChecksumsOverride */);
    writeWALHeaderDatabaseID(*context, graphWALPath,
        graphStorageManager->getOrInitDatabaseID(*context), mainDatabaseID,
        false /* enableChecksums */);
    conn.reset();
    database.reset();
    std::filesystem::resize_file(graphWALPath, std::filesystem::file_size(graphWALPath) - 1);

    systemConfig->throwOnWalReplayFailure = false;
    createDBAndConn();
    EXPECT_FALSE(std::filesystem::exists(graphWALPath));
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    auto result = conn->query("MATCH (n:User) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

INSTANTIATE_TEST_SUITE_P(ActiveAndFrozen, GraphCheckpointRecoveryTest,
    ::testing::Values(GraphCheckpointRecoveryParam{false, false, false},
        GraphCheckpointRecoveryParam{true, false, false},
        GraphCheckpointRecoveryParam{false, true, false},
        GraphCheckpointRecoveryParam{true, true, false},
        GraphCheckpointRecoveryParam{true, false, true}),
    [](const ::testing::TestParamInfo<GraphCheckpointRecoveryParam>& info) {
        if (info.param.legacySelectedGraphID) {
            return std::string{"FrozenLegacySelectedGraphID"};
        }
        return std::string{info.param.walRotated ? "Frozen" : "Active"} +
               (info.param.graphShadowAlreadyApplied ? "AfterGraphApply" : "BeforeGraphApply");
    });

enum class UnflushedGraphShadow : uint8_t { Empty, ZeroedHeader };

class LegacyMarkerUnflushedGraphShadowTest
    : public FlakyCheckpointerTest,
      public ::testing::WithParamInterface<UnflushedGraphShadow> {};

TEST_P(LegacyMarkerUnflushedGraphShadowTest, DiscardsGraphShadow) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE main_test(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Bob'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:main_test {id: 1});")->isSuccess());

    auto* context = getClientContext(*conn);
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerFailsBeforeGraphShadowApply>(clientContext);
    }).setCheckpointer(*context);
    ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());

    const auto mainWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    const auto graphShadowPath = StorageUtils::getShadowFilePath(graphPath);
    ASSERT_TRUE(std::filesystem::exists(mainWALPath));
    ASSERT_TRUE(std::filesystem::exists(graphShadowPath));
    rewriteCheckpointRecordAsLegacy(*context, mainWALPath);
    writeShadowDatabaseID(*context, StorageUtils::getShadowFilePath(databasePath),
        StorageManager::Get(*context)->getOrInitDatabaseID(*context));
    auto* graphStorageManager = main::DatabaseManager::Get(*context)
                                    ->getGraphCatalog("recovery_graph")
                                    ->getStorageManager();
    graphStorageManager->getWAL().reset();
    std::filesystem::remove(StorageUtils::getCheckpointWALFilePath(graphPath));
    conn.reset();
    database.reset();
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getWALFilePath(graphPath)));
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(graphPath)));
    if (GetParam() == UnflushedGraphShadow::Empty) {
        std::filesystem::resize_file(graphShadowPath, 0);
    } else {
        std::fstream shadow{graphShadowPath, std::ios::binary | std::ios::in | std::ios::out};
        const std::string zeroedHeader(LBUG_PAGE_SIZE, '\0');
        shadow.write(zeroedHeader.data(), static_cast<std::streamsize>(zeroedHeader.size()));
    }
    const auto graphBeforeRecovery = readFile(graphPath);

    createDBAndConn();
    auto result = conn->query("MATCH (n:main_test) RETURN COUNT(n);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
    EXPECT_FALSE(std::filesystem::exists(graphShadowPath));
    EXPECT_EQ(readFile(graphPath), graphBeforeRecovery);
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    result = conn->query("MATCH (n:User) RETURN n.name;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNumTuples(), 1);
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Alice");
}

INSTANTIATE_TEST_SUITE_P(GraphShadow, LegacyMarkerUnflushedGraphShadowTest,
    ::testing::Values(UnflushedGraphShadow::Empty, UnflushedGraphShadow::ZeroedHeader),
    [](const ::testing::TestParamInfo<UnflushedGraphShadow>& info) {
        return std::string{info.param == UnflushedGraphShadow::Empty ? "Empty" : "ZeroedHeader"};
    });

TEST_F(CheckpointRetryAfterFailureTest, CommittedRecoverySkipsCatalogGraphWithoutFiles) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE GRAPH missing_graph ANY;")->isSuccess());
    checkpoint();
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "missing_graph");
    conn.reset();
    database.reset();
    ASSERT_TRUE(std::filesystem::remove(graphPath));

    createDBAndConn();
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    insertNodes(0, 100);
    failCheckpointWith<FlakyCheckpointerFailsOnApplyingShadow>();
    ASSERT_TRUE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));

    createDBAndConn();
    checkNodes(100);
    EXPECT_FALSE(std::filesystem::exists(graphPath));
    EXPECT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));
}

TEST_F(FlakyCheckpointerTest, CommittedGraphRecoveryRejectsMissingBaseFile) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH recovery_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH recovery_graph;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:User {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());

    auto* context = getClientContext(*conn);
    FlakyCheckpointer([](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerFailsBeforeGraphShadowApply>(clientContext);
    }).setCheckpointer(*context);
    ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());

    const auto graphPath = StorageUtils::getGraphPath(databasePath, "recovery_graph");
    conn.reset();
    database.reset();
    ASSERT_TRUE(std::filesystem::remove(graphPath));
    try {
        createDBAndConn();
        FAIL() << "Expected committed recovery to reject a missing graph file.";
    } catch (const RuntimeException& e) {
        EXPECT_NE(std::string{e.what()}.find("graph file " + graphPath), std::string::npos)
            << e.what();
    }
    EXPECT_FALSE(std::filesystem::exists(graphPath));
}

class CheckpointerMakesWALDirectoryReadOnly final : public Checkpointer {
public:
    CheckpointerMakesWALDirectoryReadOnly(main::ClientContext& context,
        std::function<void()> makeReadOnly)
        : Checkpointer(context), makeReadOnly{std::move(makeReadOnly)} {}

    void beforeWALRetirement(bool walRotated) override {
        ASSERT_TRUE(walRotated);
        makeReadOnly();
    }

private:
    std::function<void()> makeReadOnly;
};

class DirectoryPermissionRestorer final {
public:
    DirectoryPermissionRestorer(std::filesystem::path directory, std::filesystem::perms permissions,
        std::filesystem::path probePath)
        : directory{std::move(directory)}, permissions{permissions},
          probePath{std::move(probePath)} {}

    ~DirectoryPermissionRestorer() { restore(); }

    void restore() {
        if (!active) {
            return;
        }
        std::error_code error;
        std::filesystem::permissions(directory, permissions, std::filesystem::perm_options::replace,
            error);
        std::filesystem::remove(probePath, error);
        active = false;
    }

private:
    std::filesystem::path directory;
    std::filesystem::perms permissions;
    std::filesystem::path probePath;
    bool active = true;
};

TEST_F(FlakyCheckpointerTest, RecoverAfterFrozenWALRemovalFailure) {
#ifdef _WIN32
    GTEST_SKIP();
#endif
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    const auto directory = std::filesystem::path(databasePath).parent_path();
    const auto originalPermissions = std::filesystem::status(directory).permissions();
    const auto probePath = directory / ".checkpoint-unlink-probe";
    std::ofstream(probePath).close();
    DirectoryPermissionRestorer permissionRestorer{directory, originalPermissions, probePath};
    bool madeReadOnly = false;
    bool deletionWasBlocked = false;
    auto initFlakyCheckpointer = [&](main::ClientContext& context) {
        return std::make_unique<CheckpointerMakesWALDirectoryReadOnly>(context, [&]() {
            madeReadOnly = true;
            std::filesystem::permissions(directory,
                std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                    std::filesystem::perms::others_write,
                std::filesystem::perm_options::remove);
            std::error_code error;
            const auto removed = std::filesystem::remove(probePath, error);
            deletionWasBlocked = !removed && std::filesystem::exists(probePath);
            if (!deletionWasBlocked) {
                throw RuntimeException("directory permissions do not prevent file deletion");
            }
        });
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    std::string checkpointError;
    runFlakyCheckpoint(flakyCheckpointer, &checkpointError);
    permissionRestorer.restore();

    ASSERT_TRUE(madeReadOnly) << checkpointError;
    if (!deletionWasBlocked) {
        GTEST_SKIP() << "Directory permissions do not prevent file deletion in this environment.";
    }

    ASSERT_NE(checkpointError.find("Frozen WAL retirement failed"), std::string::npos);
    const auto frozenWALPath = StorageUtils::getCheckpointWALFilePath(databasePath);
    ASSERT_TRUE(std::filesystem::exists(frozenWALPath));
    ASSERT_EQ(std::filesystem::file_size(frozenWALPath), 0);
    ASSERT_TRUE(std::filesystem::exists(StorageUtils::getShadowFilePath(databasePath)));
    auto rejectedWrite = conn->query("CREATE (:test {id: 5000, name: 'rejected'});");
    ASSERT_FALSE(rejectedWrite->isSuccess());
    ASSERT_NE(rejectedWrite->getErrorMessage().find("panic state"), std::string::npos);

    createDBAndConn();
    auto result = conn->query("MATCH (a:test) RETURN COUNT(a);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 5000);
    ASSERT_FALSE(std::filesystem::exists(frozenWALPath));
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getShadowFilePath(databasePath)));
}

// Page allocator that fails the first page allocation. In-place checkpoints do not allocate
// pages, so during the storage phase of a checkpoint that only updates a node group with
// persistent data, the first allocation is the flush of an out-of-place column chunk rewrite and
// the failure lands in the middle of that rewrite.
class FailFirstAllocationPageAllocator final : public PageAllocator {
public:
    FailFirstAllocationPageAllocator(PageAllocator& inner, bool& failed)
        : PageAllocator(inner.getDataFH()), inner{inner}, failed{failed} {}

    PageRange allocatePageRange(page_idx_t numPages) override {
        if (!failed) {
            failed = true;
            throw RuntimeException("checkpoint failed.");
        }
        return inner.allocatePageRange(numPages);
    }
    void freePageRange(PageRange block) override { inner.freePageRange(block); }

private:
    PageAllocator& inner;
    bool& failed;
};

class FlakyCheckpointerFailsDuringOutOfPlaceRewrite final : public Checkpointer {
public:
    FlakyCheckpointerFailsDuringOutOfPlaceRewrite(main::ClientContext& context, bool& failed)
        : Checkpointer(context), failed{failed} {}

    bool checkpointStorage() override {
        for (const auto& target : checkpointTargets) {
            FailFirstAllocationPageAllocator pageAllocator(
                *target.storageManager->getDataFH()->getPageManager(), failed);
            const Transaction snapshotTxn(TransactionType::CHECKPOINT,
                Transaction::DUMMY_TRANSACTION_ID, snapshotTS);
            target.storageManager->checkpoint(&clientContext, *target.catalog, snapshotTxn,
                pageAllocator, tableEpochWatermarksByManager.at(target.storageManager));
        }
        throw RuntimeException("expected the injected page allocation failure to fire.");
    }

private:
    bool& failed;
};

// A checkpoint that fails while a column chunk segment is being rewritten out of place must
// leave that segment as it was, so that the data stays readable and later checkpoints succeed.
TEST_F(FlakyCheckpointerTest, RecoverFromFailureDuringOutOfPlaceCheckpoint) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL force_checkpoint_on_close=false;");
    conn->query("CALL auto_checkpoint=false;");
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);")->isSuccess());
    constexpr int64_t numInitialRows = 3000;
    constexpr int64_t numRows = 6000;
    constexpr std::string_view namePrefix = "a longer name so the pages fill up ";
    auto nameOf = [&](int64_t i) { return std::format("{}{}", namePrefix, i); };
    auto insertRows = [&](int64_t start, int64_t end) {
        auto res = conn->query(std::format("UNWIND range({}, {}) AS i CREATE (a:test {{id: i, "
                                           "name: concat('{}', CAST(i AS STRING))}});",
            start, end - 1, namePrefix));
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    };
    auto checkRows = [&]() {
        auto res = conn->query("MATCH (a:test) RETURN COUNT(a), CAST(SUM(a.id) AS INT64), "
                               "CAST(SUM(SIZE(a.name)) AS INT64), MIN(a.name), MAX(a.name);");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        auto row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), numRows);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), numRows * (numRows - 1) / 2);
        int64_t totalNameSize = 0;
        for (auto i = 0; i < numRows; i++) {
            totalNameSize += nameOf(i).size();
        }
        ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), totalNameSize);
        ASSERT_EQ(row->getValue(3)->getValue<std::string>(), nameOf(0));
        ASSERT_EQ(row->getValue(4)->getValue<std::string>(), nameOf(999));
    };
    insertRows(0, numInitialRows);
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    // Appending to the persistent node group outgrows the pages of its column chunks, so the
    // next checkpoint has to rewrite them out of place.
    insertRows(numInitialRows, numRows);

    auto context = getClientContext(*conn);
    bool failed = false;
    FlakyCheckpointer flakyCheckpointer([&failed](main::ClientContext& ctx) {
        return std::make_unique<FlakyCheckpointerFailsDuringOutOfPlaceRewrite>(ctx, failed);
    });
    flakyCheckpointer.setCheckpointer(*context);
    auto res = conn->query("CHECKPOINT;");
    ASSERT_FALSE(res->isSuccess());
    ASSERT_TRUE(failed);
    checkRows();

    FlakyCheckpointer defaultCheckpointer(
        [](main::ClientContext& ctx) { return std::make_unique<Checkpointer>(ctx); });
    defaultCheckpointer.setCheckpointer(*context);
    res = conn->query("CHECKPOINT;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    checkRows();
}

// A checkpoint that fails inside a CSR node group, after the rel data columns were
// checkpointed but before the CSR header, must leave the rel table readable: the failed
// group's in-place data rewrites are undone and its shadow pages dropped, so reads see the
// old data under the old header and a retried checkpoint persists the correct data.
// See LadybugDB/ladybug#1051.
TEST_F(FlakyCheckpointerTest, CSRGroupReadsBackAfterFailedCSRHeaderCheckpoint) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE N(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE R(FROM N TO N, w INT64);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(0, 999) AS i CREATE (:N {id: i});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    // 1000 rels i -> (i+1) % 1000 with w = i.
    auto res = conn->query("UNWIND range(0, 999) AS i MATCH (a:N), (b:N) WHERE a.id = i AND "
                           "b.id = (i + 1) % 1000 CREATE (a)-[:R {w: i}]->(b);");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    // One new rel. The data columns still fit in their pages and are rewritten in place,
    // while the shifted CSR offsets are rewritten out of place, so the injected allocation
    // failure lands in the CSR header checkpoint, mid-node-group.
    res = conn->query("MATCH (a:N), (b:N) WHERE a.id = 0 AND b.id = 500 "
                      "CREATE (a)-[:R {w: 1000}]->(b);");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    // A pending update on a committed rel, so the restore must also preserve uncheckpointed
    // update info (which is not part of the metadata snapshot).
    res = conn->query("MATCH (:N {id: 5})-[e:R]->(:N) SET e.w = 5000;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    // Pending deletes on committed rels, so the restore must also preserve the group's
    // version info (likewise transplanted, not serialized): deleted rels must stay deleted
    // across the failed checkpoint, the retry and the reopen.
    res = conn->query("MATCH (a:N)-[e:R]->(b:N) WHERE e.w < 5 DELETE e;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();

    auto checkRels = [&]() {
        auto res =
            conn->query("MATCH (a:N)-[e:R]->(b:N) RETURN COUNT(e), CAST(SUM(e.w) AS INT64);");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        auto row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 996);
        // 499500 (initial w sum) + 1000 (new rel) - 5 + 5000 (updated rel 5 -> 6)
        // - (0 + 1 + 2 + 3 + 4) (deleted rels).
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 505485);
        // 0 -> {500} (0 -> 1 was deleted).
        res = conn->query("MATCH (a:N {id: 0})-[e:R]->(b:N) RETURN b.id, e.w ORDER BY b.id;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        ASSERT_TRUE(res->hasNext());
        row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 500);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 1000);
        ASSERT_FALSE(res->hasNext());
        // 1 -> {} (1 -> 2 was deleted), 5 -> 6 (updated), 999 -> 0.
        res = conn->query("MATCH (a:N {id: 1})-[e:R]->(b:N) RETURN b.id, e.w;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        ASSERT_FALSE(res->hasNext());
        res = conn->query("MATCH (a:N {id: 5})-[e:R]->(b:N) RETURN b.id, e.w;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 6);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 5000);
        res = conn->query("MATCH (a:N {id: 999})-[e:R]->(b:N) RETURN b.id, e.w;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 0);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 999);
        // Backward direction is unaffected: 0 <- 999, 500 <- {499, 0}.
        res = conn->query("MATCH (a:N)-[e:R]->(b:N {id: 0}) RETURN a.id, e.w;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 999);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 999);
        res = conn->query("MATCH (a:N)-[e:R]->(b:N {id: 500}) RETURN a.id, e.w ORDER BY a.id;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        ASSERT_TRUE(res->hasNext());
        row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 0);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 1000);
        ASSERT_TRUE(res->hasNext());
        row = res->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 499);
        ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 499);
        ASSERT_FALSE(res->hasNext());
    };
    checkRels();

    auto context = getClientContext(*conn);
    bool failed = false;
    FlakyCheckpointer flakyCheckpointer([&failed](main::ClientContext& ctx) {
        return std::make_unique<FlakyCheckpointerFailsDuringOutOfPlaceRewrite>(ctx, failed);
    });
    flakyCheckpointer.setCheckpointer(*context);
    res = conn->query("CHECKPOINT;");
    ASSERT_FALSE(res->isSuccess());
    ASSERT_TRUE(failed);
    // Reads must be unaffected by the failed checkpoint...
    checkRels();

    FlakyCheckpointer::resetCheckpointer(*context);
    res = conn->query("CHECKPOINT;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    checkRels();

    // ...and the retried checkpoint must have persisted the correct data. Release query
    // results before reopening: they borrow the old database's memory.
    res.reset();
    createDBAndConn();
    checkRels();
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

    // Opening must refuse: the garbage data file has no readable database ID, so the pending
    // checkpoint bundle in the shadow is treated as foreign and the open is refused before any
    // shadow page is written, leaving the shadow file intact.
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

// A checkpoint publishes the new chunk metadata of a node group during its storage phase, but
// in-place updates to existing pages are only written to the shadow file until the shadow pages
// are applied at the end of the checkpoint. Read-only transactions are not blocked by a
// checkpoint, so a scan that starts in between must not observe the new metadata together with
// the old page contents (e.g. appended string dictionary offsets that are not on disk yet).
class CheckpointerWithReadBeforeApplyingShadowPages final : public Checkpointer {
public:
    CheckpointerWithReadBeforeApplyingShadowPages(main::ClientContext& clientContext,
        std::function<void()> readFunc)
        : Checkpointer(clientContext), readFunc(std::move(readFunc)) {}

    void logCheckpointAndApplyShadowPages(bool walRotated) override {
        readFunc();
        Checkpointer::logCheckpointAndApplyShadowPages(walRotated);
    }

private:
    std::function<void()> readFunc;
};

#ifndef __SINGLE_THREADED__
TEST_F(FlakyCheckpointerTest, ReadBeforeShadowPagesAreAppliedSeesCommittedStrings) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    conn->query("CALL force_checkpoint_on_close=false;");
    conn->query("CALL auto_checkpoint=false;");
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);")->isSuccess());
    // Strings longer than the inline limit, few enough that the appended values below fit in
    // the pages already allocated to the dictionary, so the checkpoint updates them in place.
    auto getName = [](uint64_t i) { return std::format("persistent string value {:04}", i); };
    auto insertRows = [&](uint64_t start, uint64_t end) {
        for (auto i = start; i < end; i++) {
            auto res =
                conn->query(std::format("CREATE (:test {{id: {}, name: '{}'}});", i, getName(i)));
            ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        }
    };
    constexpr uint64_t numInitialRows = 20;
    constexpr uint64_t numRows = 25;
    insertRows(0, numInitialRows);
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    insertRows(numInitialRows, numRows);

    // Pages of the persistent `name` column chunk (dictionary offsets, string data and indices).
    // An in-place checkpoint of the column updates these pages through the shadow file, while an
    // out-of-place checkpoint writes new pages and leaves these untouched.
    auto context = getClientContext(*conn);
    auto storageManager = StorageManager::Get(*context);
    const auto tableEntry = catalog::Catalog::Get(*context)->getTableCatalogEntry(
        &DUMMY_CHECKPOINT_TRANSACTION, "test");
    auto& nodeTable = storageManager->getTable(tableEntry->getTableID())->cast<NodeTable>();
    const auto* persistentGroup = nodeTable.getNodeGroup(0)->getChunkedNodeGroup(0);
    ASSERT_EQ(persistentGroup->getResidencyState(), ResidencyState::ON_DISK);
    std::unordered_set<page_idx_t> namePages;
    for (const auto* segment :
        persistentGroup->getColumnChunk(tableEntry->getColumnID("name")).getSegments()) {
        const auto& stringChunk = segment->cast<StringChunkData>();
        for (const auto* chunk :
            std::initializer_list<const ColumnChunkData*>{stringChunk.getIndexColumnChunk(),
                stringChunk.getDictionaryChunk().getOffsetChunk(),
                stringChunk.getDictionaryChunk().getStringDataChunk()}) {
            const auto& metadata = chunk->getMetadata();
            for (auto i = 0u; i < metadata.getNumPages(); i++) {
                namePages.insert(metadata.getStartPageIdx() + i);
            }
        }
    }
    ASSERT_FALSE(namePages.empty());

    bool readRan = false;
    uint64_t numShadowedNamePages = 0;
    std::string readError;
    std::vector<std::string> readNames;
    auto readFunc = [&]() {
        // Make sure the `name` column was checkpointed in place, i.e. its updated pages are still
        // pending in the shadow file; otherwise the read below would not read shadow pages.
        auto& shadowFile = storageManager->getShadowFile();
        for (const auto pageIdx : namePages) {
            if (shadowFile.hasShadowPage(storageManager->getDataFH()->getFileIndex(), pageIdx)) {
                numShadowedNamePages++;
            }
        }
        // Run on a separate thread with its own connection, like a concurrent reader would.
        std::thread reader([&]() {
            auto readConn = std::make_unique<main::Connection>(database.get());
            auto res = readConn->query("MATCH (t:test) RETURN t.name ORDER BY t.id;");
            readRan = true;
            if (!res->isSuccess()) {
                readError = res->getErrorMessage();
                return;
            }
            while (res->hasNext()) {
                readNames.push_back(res->getNext()->getValue(0)->getValue<std::string>());
            }
        });
        reader.join();
    };
    FlakyCheckpointer checkpointer([&](main::ClientContext& clientContext) {
        return std::make_unique<CheckpointerWithReadBeforeApplyingShadowPages>(clientContext,
            readFunc);
    });
    checkpointer.setCheckpointer(*context);
    auto res = conn->query("CHECKPOINT;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    ASSERT_TRUE(readRan);
    ASSERT_GT(numShadowedNamePages, 0u);
    ASSERT_TRUE(readError.empty()) << readError;
    ASSERT_EQ(readNames.size(), numRows);
    for (auto i = 0u; i < numRows; i++) {
        EXPECT_EQ(readNames[i], getName(i));
    }
}
#endif // __SINGLE_THREADED__

// Read-only transactions are not blocked by a checkpoint, so a checkpoint can run between two
// vectors of the same table scan. It merges the node group's chunked groups into a single
// persistent one and rewrites its column chunk metadata, while the scan state still holds the
// chunked group index and the metadata from before the checkpoint. The scan must pick up the
// checkpointed state instead of reading the new pages with stale metadata (which returns e.g.
// strings assembled from the wrong dictionary offsets).
class ScanAcrossCheckpointTest : public FlakyCheckpointerTest {
public:
    static constexpr uint64_t NUM_PERSISTENT_ROWS = 5000;
    static constexpr uint64_t NUM_ROWS = 10000;

    static std::string getName(uint64_t id) {
        return std::format("string value {} {}", id, std::string(20 + id % 50, 'x'));
    }

    void insertRows(uint64_t start, uint64_t end) {
        ASSERT_TRUE(conn->query("BEGIN TRANSACTION;")->isSuccess());
        for (auto id = start; id < end; id++) {
            auto res =
                conn->query(std::format("CREATE (:test {{id: {}, name: '{}'}});", id, getName(id)));
            ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        }
        ASSERT_TRUE(conn->query("COMMIT;")->isSuccess());
    }

    // Scans `test` in a read-only transaction and checkpoints after `numScansBeforeCheckpoint`
    // vectors have been scanned.
    void scanAcrossCheckpoint(uint64_t numScansBeforeCheckpoint) {
        if (inMemMode || systemConfig->checkpointThreshold == 0) {
            GTEST_SKIP();
        }
        conn->query("CALL force_checkpoint_on_close=false;");
        conn->query("CALL auto_checkpoint=false;");
        ASSERT_TRUE(
            conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);")->isSuccess());
        // One persistent chunked group, followed by committed rows that are still in memory.
        insertRows(0, NUM_PERSISTENT_ROWS);
        ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
        insertRows(NUM_PERSISTENT_ROWS, NUM_ROWS);

        auto readConn = std::make_unique<main::Connection>(database.get());
        ASSERT_TRUE(readConn->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
        auto* context = getClientContext(*readConn);
        auto* transaction = Transaction::Get(*context);
        const auto* tableEntry =
            catalog::Catalog::Get(*context)->getTableCatalogEntry(transaction, "test");
        auto& nodeTable =
            StorageManager::Get(*context)->getTable(tableEntry->getTableID())->cast<NodeTable>();
        ASSERT_GT(nodeTable.getNodeGroup(0)->getNumChunkedGroups(), 1u);

        auto* memoryManager = MemoryManager::Get(*context);
        std::vector<LogicalType> types;
        types.push_back(LogicalType::INT64());
        types.push_back(LogicalType::STRING());
        auto dataChunk = Table::constructDataChunk(memoryManager, std::move(types));
        ValueVector nodeIDVector(LogicalType::INTERNAL_ID(), memoryManager);
        nodeIDVector.state = dataChunk.state;
        std::vector<ValueVector*> outVectors{&dataChunk.getValueVectorMutable(0),
            &dataChunk.getValueVectorMutable(1)};
        NodeTableScanState scanState(&nodeIDVector, outVectors, dataChunk.state);
        scanState.setToTable(transaction, &nodeTable,
            {tableEntry->getColumnID("id"), tableEntry->getColumnID("name")}, {});
        scanState.source = TableScanSource::COMMITTED;
        scanState.nodeGroupIdx = 0;
        nodeTable.initScanState(transaction, scanState);

        std::vector<bool> scanned(NUM_ROWS, false);
        uint64_t numRowsScanned = 0;
        uint64_t numScans = 0;
        while (true) {
            if (numScans == numScansBeforeCheckpoint) {
                auto res = conn->query("CHECKPOINT;");
                ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
                ASSERT_EQ(nodeTable.getNodeGroup(0)->getNumChunkedGroups(), 1u);
            }
            if (!nodeTable.scan(transaction, scanState)) {
                break;
            }
            numScans++;
            const auto& selVector = dataChunk.state->getSelVector();
            for (auto i = 0u; i < selVector.getSelSize(); i++) {
                const auto pos = selVector[i];
                const auto id = static_cast<uint64_t>(outVectors[0]->getValue<int64_t>(pos));
                ASSERT_LT(id, NUM_ROWS);
                ASSERT_FALSE(scanned[id]) << "row " << id << " was scanned twice";
                scanned[id] = true;
                ASSERT_EQ(outVectors[1]->getValue<string_t>(pos).getAsString(), getName(id));
                numRowsScanned++;
            }
        }
        ASSERT_GT(numScans, numScansBeforeCheckpoint);
        ASSERT_EQ(numRowsScanned, NUM_ROWS);
        ASSERT_TRUE(readConn->query("COMMIT;")->isSuccess());
    }
};

// The checkpoint runs while the scan is in the persistent chunked group.
TEST_F(ScanAcrossCheckpointTest, CheckpointWhileScanningPersistentGroup) {
    scanAcrossCheckpoint(1);
}

// The checkpoint runs while the scan is in a committed in-memory chunked group, which no longer
// exists afterwards.
TEST_F(ScanAcrossCheckpointTest, CheckpointWhileScanningInMemoryGroup) {
    scanAcrossCheckpoint(NUM_PERSISTENT_ROWS / DEFAULT_VECTOR_CAPACITY + 2);
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

TEST_F(ReviewFixesTest, CheckpointUsesMatchingCatalogForMainAndDefaultGraph) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;"));
    auto createGraph = conn->query("CREATE GRAPH ratatouille ANY;");
    ASSERT_TRUE(createGraph->isSuccess()) << createGraph->getErrorMessage();

    auto checkpoint = conn->query("CHECKPOINT;");
    ASSERT_TRUE(checkpoint->isSuccess()) << checkpoint->getErrorMessage();
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));

    checkpoint.reset();
    createGraph.reset();
    createDBAndConn();
    auto useGraph = conn->query("USE GRAPH ratatouille;");
    ASSERT_TRUE(useGraph->isSuccess()) << useGraph->getErrorMessage();
    auto showTables = conn->query("CALL SHOW_TABLES() RETURN count(*)");
    ASSERT_TRUE(showTables->isSuccess()) << showTables->getErrorMessage();
    ASSERT_EQ(showTables->getNext()->getValue(0)->getValue<int64_t>(), 2);
    showTables.reset();
    useGraph.reset();

    auto readOnlyConfig = *systemConfig;
    readOnlyConfig.readOnly = true;
    auto graphPath = StorageUtils::getGraphPath(databasePath, "ratatouille");
    conn.reset();
    database.reset();

    auto graphDatabase = std::make_unique<main::Database>(graphPath, readOnlyConfig);
    auto graphConnection = std::make_unique<main::Connection>(graphDatabase.get());
    auto graphTables = graphConnection->query("CALL SHOW_TABLES() RETURN count(*)");
    ASSERT_TRUE(graphTables->isSuccess()) << graphTables->getErrorMessage();
    ASSERT_EQ(graphTables->getNext()->getValue(0)->getValue<int64_t>(), 2);
}

TEST_F(ReviewFixesTest, ReadOnlyOpenAllowsEmptyGraphWAL) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE GRAPH readonly_graph ANY;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    const auto graphPath = StorageUtils::getGraphPath(databasePath, "readonly_graph");
    const auto graphWALPath = StorageUtils::getWALFilePath(graphPath);

    conn.reset();
    database.reset();
    std::ofstream(graphWALPath).close();
    ASSERT_TRUE(std::filesystem::exists(graphWALPath));
    ASSERT_EQ(std::filesystem::file_size(graphWALPath), 0);
    auto readOnlyConfig = *systemConfig;
    readOnlyConfig.readOnly = true;
    auto readOnlyDatabase = std::make_unique<main::Database>(databasePath, readOnlyConfig);
    auto readOnlyConnection = std::make_unique<main::Connection>(readOnlyDatabase.get());
    auto result = readOnlyConnection->query("USE GRAPH readonly_graph;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = readOnlyConnection->query("CALL SHOW_TABLES() RETURN count(*);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    EXPECT_TRUE(std::filesystem::exists(graphWALPath));
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

// Fix #5 – reclaimTailPagesIfNeeded must not introduce overlap with existing FSM entries
// ─────────────────────────────────────────────────────────────────────────────
// reclaimTailPagesIfNeeded() is called after FSM deserialization during recovery. If it inserts
// the tail directly into freeLists (without merge), it can create overlapping entries.
//
// This test exercises the exact overlap pattern deterministically:
// 1) seed an existing free entry that starts at checkpointNumPages,
// 2) reclaim tail [checkpointNumPages, currentNumPages),
// 3) verify resulting FSM has no overlap.
TEST_F(ReviewFixesTest, ReclaimTailMergesWithDeserializedFSMEntries) {
    if (inMemMode) {
        GTEST_SKIP();
    }

    conn->query("CALL auto_checkpoint=false;");
    conn->query("CREATE NODE TABLE fsm_tail(id INT64 PRIMARY KEY, val STRING);");

    auto* context = getClientContext(*conn);
    auto* storageManager = StorageManager::Get(*context);
    auto* dataFH = storageManager->getDataFH();
    auto* pageManager = dataFH->getPageManager();

    // Ensure we have enough physical pages to craft overlapping ranges.
    dataFH->addNewPages(32);
    const auto currentNumPages = dataFH->getNumPages();
    const auto checkpointNumPages = currentNumPages - 8;

    // Existing (deserialized-equivalent) free entry at tail start.
    pageManager->freeImmediatelyRewritablePageRange(dataFH, PageRange(checkpointNumPages, 2));

    // Recovery-time reclaim path under test.
    pageManager->reclaimTailPagesIfNeeded(checkpointNumPages);

    const auto numEntries = pageManager->getNumFreeEntries();
    const auto freeEntries = pageManager->getFreeEntries(0, numEntries);

    std::vector<std::pair<uint64_t, uint64_t>> entries;
    entries.reserve(freeEntries.size());
    for (const auto& entry : freeEntries) {
        entries.emplace_back(entry.startPageIdx, entry.numPages);
    }
    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (size_t i = 1; i < entries.size(); ++i) {
        const auto prevEnd = entries[i - 1].first + entries[i - 1].second;
        ASSERT_GE(entries[i].first, prevEnd)
            << "Overlapping FSM entries after tail reclaim: [" << entries[i - 1].first << ", "
            << prevEnd << ") and [" << entries[i].first << ", "
            << (entries[i].first + entries[i].second) << ")";
    }
}

TEST_F(ReviewFixesTest, ExplicitCheckpointLeavesPageManagerCleanAfterSecondaryArtIndex) {
    if (inMemMode) {
        GTEST_SKIP();
    }

    conn->query("CALL auto_checkpoint=false;");
    ASSERT_TRUE(conn->query("CREATE NODE TABLE art_clean(id INT64 PRIMARY KEY, name STRING);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:art_clean {id: 1, name: 'alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:art_clean {id: 2, name: 'bob'});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE ART INDEX art_clean_name_idx FOR (n:art_clean) ON (n.name);")
                    ->isSuccess());

    auto checkpoint = conn->query("CHECKPOINT;");
    ASSERT_TRUE(checkpoint->isSuccess()) << checkpoint->getErrorMessage();

    auto* context = getClientContext(*conn);
    auto* pageManager = StorageManager::Get(*context)->getDataFH()->getPageManager();
    EXPECT_FALSE(pageManager->changedSinceLastCheckpoint());
}

TEST_F(ReviewFixesTest, RecoverSecondaryArtIndexCreatedAfterLastCheckpoint) {
    if (inMemMode) {
        GTEST_SKIP();
    }

    conn->query("CALL auto_checkpoint=false;");
    conn->query("CALL force_checkpoint_on_close=false;");
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE art_wal(id INT64 PRIMARY KEY, name STRING);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:art_wal {id: 1, name: 'alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:art_wal {id: 2, name: 'bob'});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(
        conn->query("CREATE ART INDEX art_wal_name_idx FOR (n:art_wal) ON (n.name);")->isSuccess());

    createDBAndConn();

    auto result = conn->query("MATCH (n:art_wal) WHERE n.name = 'bob' RETURN n.id;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    EXPECT_FALSE(result->hasNext());
}

TEST_F(ReviewFixesTest, BulkArtIndexUsesBlockingCheckpointInsteadOfPhysicalWAL) {
    if (inMemMode) {
        GTEST_SKIP();
    }

    setenv("LBUG_CREATE_INDEX_WAL_THRESHOLD", "1", 1);
    conn->query("CALL auto_checkpoint=false;");
    conn->query("CALL force_checkpoint_on_close=false;");
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE art_bulk(id INT64 PRIMARY KEY, name STRING);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:art_bulk {id: 1, name: 'alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:art_bulk {id: 2, name: 'bob'});")->isSuccess());

    auto createIndex =
        conn->query("CREATE ART INDEX art_bulk_name_idx FOR (n:art_bulk) ON (n.name);");
    unsetenv("LBUG_CREATE_INDEX_WAL_THRESHOLD");
    ASSERT_TRUE(createIndex->isSuccess()) << createIndex->getErrorMessage();
    ASSERT_TRUE(createIndex->hasNext());
    EXPECT_EQ(createIndex->getNext()->getValue(0)->getValue<std::string>(),
        "Index art_bulk_name_idx has been created.");
    EXPECT_FALSE(createIndex->hasNext());

    createDBAndConn();

    auto result = conn->query("MATCH (n:art_bulk) WHERE n.name = 'bob' RETURN n.id;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    EXPECT_FALSE(result->hasNext());
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
    conn->query("CREATE NODE TABLE ckpt_adddrop(id INT64 PRIMARY KEY, name STRING, age INT64);");

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

// ─────────────────────────────────────────────────────────────────────────────
// Regression test for: subgraph catalog writes lost after checkpoint + reopen
// when the main database has pre-existing tables.
//
// Bug: DatabaseManager::createGraph() set defaultGraph to the newly created
// graph before the transaction committed, causing Catalog::Get() in
// Transaction::publishCommit() to return the graph's catalog instead of the
// main catalog.  The main catalog's version was never incremented, so
// CHECKPOINT skipped serializing it (changedSinceLastCheckpoint() was false)
// and the graph entry in the main catalog's `graphs` set was lost on reopen.
//
// This only manifested when the database already had tables (a valid
// catalogPageRange from a previous checkpoint), because with a bare database
// the first-time-serialization path (INVALID_PAGE_IDX) rescued the data.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReviewFixesTest, SubgraphCatalogPersistsAfterCheckpointWithPreExistingTables) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }

    // ---- Phase 1: create pre-existing tables (like Hyper-Extract does) ----
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE IF NOT EXISTS Template ("
                            "name STRING, domain STRING, type STRING, tags STRING, "
                            "description STRING, language STRING, output STRING, "
                            "guideline STRING, identifiers STRING, opts STRING, display STRING, "
                            "PRIMARY KEY (name));")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE IF NOT EXISTS KnowledgeAbstract ("
                            "id STRING, template_name STRING, lang STRING, type STRING, "
                            "created_at STRING, updated_at STRING, metadata STRING, "
                            "PRIMARY KEY (id));")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE IF NOT EXISTS Entity ("
                            "id STRING, ka_id STRING, entity_type STRING, data STRING, "
                            "PRIMARY KEY (id));")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE IF NOT EXISTS Relates ("
                            "FROM Entity TO Entity, ka_id STRING, "
                            "relation_type STRING, data STRING"
                            ");")
                    ->isSuccess());

    // First checkpoint: persist the schema so the data file has a valid
    // catalogPageRange.  Without this the bug would be masked (the first
    // checkpoint after graph creation would serialize unconditionally).
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));

    // ---- Phase 2: create a subgraph and checkpoint ----
    ASSERT_TRUE(conn->query("CREATE GRAPH regression_test;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_FALSE(std::filesystem::exists(StorageUtils::getCheckpointWALFilePath(databasePath)));

    // ---- Phase 3: reopen the database ----
    auto graphName = conn->query("CALL show_graphs() RETURN name ORDER BY name;");
    ASSERT_TRUE(graphName->isSuccess()) << graphName->getErrorMessage();
    ASSERT_TRUE(graphName->hasNext());
    // The subgraph must be visible in the current process already. Every node table is also
    // backed by a subgraph, so the list carries the three node tables plus the created graph.
    std::vector<std::string> graphsBeforeReopen;
    while (graphName->hasNext()) {
        graphsBeforeReopen.push_back(graphName->getNext()->getValue(0)->getValue<std::string>());
    }
    ASSERT_EQ(graphsBeforeReopen.size(), 4);
    ASSERT_EQ(graphsBeforeReopen[0], "Entity");
    ASSERT_EQ(graphsBeforeReopen[1], "KnowledgeAbstract");
    ASSERT_EQ(graphsBeforeReopen[2], "Template");
    ASSERT_EQ(graphsBeforeReopen[3], "regression_test");
    graphName.reset();

    // Reopen the database in a fresh connection (simulates a new process).
    createDBAndConn();

    // ---- Phase 4: verify the subgraph survived ----
    auto result = conn->query("CALL show_graphs() RETURN name ORDER BY name;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    std::vector<std::string> graphs;
    while (result->hasNext()) {
        graphs.push_back(result->getNext()->getValue(0)->getValue<std::string>());
    }

    // The list should include the subgraph created above.
    bool found = false;
    for (const auto& g : graphs) {
        if (g == "regression_test") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Subgraph 'regression_test' was lost after checkpoint + reopen. "
                       << "Visible graphs count: " << graphs.size();

    // Also verify we can USE GRAPH and see at least the main database tables
    // (proving the main catalog itself was correctly restored).
    ASSERT_TRUE(conn->query("USE GRAPH regression_test;")->isSuccess());
    auto tables = conn->query("CALL show_tables() RETURN name ORDER BY name;");
    ASSERT_TRUE(tables->isSuccess()) << tables->getErrorMessage();
    // Clean up
    ASSERT_TRUE(conn->query("USE GRAPH main;")->isSuccess());
}

// Regression tests for #1050: a checkpoint that fails after the PK index storage phase must
// leave lookups, uniqueness and reopen behaving as if the checkpoint had not run. The PK
// index used to publish read headers and clear its local storage mid-checkpoint, which
// rollback could not restore (lost keys, accepted duplicates, unopenable DB).
class FailedCheckpointPKIndexTest : public PrivateApiTest {
public:
    std::string getInputDir() override { return "empty"; }

    void SetUp() override {
        PrivateApiTest::SetUp();
        ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
        ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    }

    void failCheckpointOnSerialization() const {
        FlakyCheckpointer flakyCheckpointer([](main::ClientContext& context) {
            return std::make_unique<FlakyCheckpointerFailsOnSerialization>(context);
        });
        auto context = getClientContext(*conn);
        flakyCheckpointer.setCheckpointer(*context);
        ASSERT_FALSE(conn->query("CHECKPOINT;")->isSuccess());
        FlakyCheckpointer::resetCheckpointer(*context);
    }

    void checkpoint() const {
        auto res = conn->query("CHECKPOINT;");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    }

    // Asserts every key in [begin, end) resolves through a PK point lookup.
    void checkPointLookups(const std::function<std::string(int64_t)>& keyOf, int64_t begin,
        int64_t end, const char* phase) const {
        for (auto k = begin; k < end; k++) {
            auto res =
                conn->query(std::format("MATCH (t:test) WHERE t.id = {} RETURN t.id;", keyOf(k)));
            ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
            ASSERT_TRUE(res->hasNext()) << phase << ": PK lookup missed key " << k;
            int rows = 0;
            while (res->hasNext()) {
                res->getNext();
                rows++;
            }
            EXPECT_EQ(rows, 1) << phase << ": key " << k << " returned " << rows << " rows";
        }
    }

    void checkCount(int64_t expected) const {
        auto res = conn->query("MATCH (t:test) RETURN COUNT(t);");
        ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
        ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), expected);
    }
};

TEST_F(FailedCheckpointPKIndexTest, Int64LookupsSurviveFailedCheckpoint) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, v INT64);")->isSuccess());
    for (auto i = 0; i < 300; i++) {
        ASSERT_TRUE(
            conn->query(std::format("CREATE (:test {{id: {}, v: {}}});", i, i))->isSuccess());
    }
    checkpoint();
    for (auto i = 300; i < 400; i++) {
        ASSERT_TRUE(
            conn->query(std::format("CREATE (:test {{id: {}, v: {}}});", i, i))->isSuccess());
    }
    failCheckpointOnSerialization();
    // Keys inserted since the last checkpoint must still resolve, and duplicates of both
    // old and new keys must still be rejected.
    checkPointLookups([](int64_t k) { return std::to_string(k); }, 0, 400, "after failure");
    EXPECT_FALSE(conn->query("CREATE (:test {id: 10, v: -1});")->isSuccess());
    EXPECT_FALSE(conn->query("CREATE (:test {id: 350, v: -1});")->isSuccess());
    checkCount(400);
    // A retry must persist the correct state, and it must survive a reopen.
    checkpoint();
    checkPointLookups([](int64_t k) { return std::to_string(k); }, 0, 400, "after retry");
    checkCount(400);
    createDBAndConn();
    checkCount(400);
    checkPointLookups([](int64_t k) { return std::to_string(k); }, 0, 400, "after reopen");
}

TEST_F(FailedCheckpointPKIndexTest, StringLookupsSurviveFailedCheckpoint) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE test(id STRING PRIMARY KEY, v INT64);")->isSuccess());
    const auto keyOf = [](int64_t k) {
        return std::format("'a-key-longer-than-twelve-bytes-{:04d}'", k);
    };
    for (auto i = 0; i < 100; i++) {
        ASSERT_TRUE(conn->query(std::format("CREATE (:test {{id: {}, v: {}}});", keyOf(i), i))
                        ->isSuccess());
    }
    checkpoint();
    for (auto i = 100; i < 140; i++) {
        ASSERT_TRUE(conn->query(std::format("CREATE (:test {{id: {}, v: {}}});", keyOf(i), i))
                        ->isSuccess());
    }
    failCheckpointOnSerialization();
    checkPointLookups(keyOf, 0, 140, "after failure");
    EXPECT_FALSE(
        conn->query(std::format("CREATE (:test {{id: {}, v: -1}});", keyOf(3)))->isSuccess());
    EXPECT_FALSE(
        conn->query(std::format("CREATE (:test {{id: {}, v: -1}});", keyOf(120)))->isSuccess());
    checkCount(140);
    checkpoint();
    checkPointLookups(keyOf, 0, 140, "after retry");
    checkCount(140);
    createDBAndConn();
    checkCount(140);
    checkPointLookups(keyOf, 0, 140, "after reopen");
}

} // namespace testing
} // namespace lbug
