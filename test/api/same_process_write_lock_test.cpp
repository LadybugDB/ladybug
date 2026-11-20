#ifndef _WIN32 // Windows file locks belong to the handle and already refuse a second open.

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "api_test/api_test.h"

using namespace lbug::testing;
using namespace lbug::common;
using namespace lbug::main;

class SameProcessWriteLockTest : public ApiTest {
    void SetUp() override { BaseGraphTest::SetUp(); }
};

// POSIX record locks never conflict within one process, so a second read-write Database on the
// same file used to open and then lose committed rows (each instance replays and checkpoints its
// own view of the file). The second writable open must be refused.
TEST_F(SameProcessWriteLockTest, SecondWritableDatabaseInSameProcessIsRefused) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    systemConfig->readOnly = false;
    createDBAndConn();
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Person(name STRING, PRIMARY KEY(name));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Person {name: 'Alice'});")->isSuccess());

    try {
        Database second{databasePath, *systemConfig};
        FAIL() << "a second read-write Database opened on the same file";
    } catch (const Exception& e) {
        EXPECT_NE(std::string(e.what()).find("already open in this process"), std::string::npos)
            << e.what();
    }

    // The refused open must leave the first Database fully usable.
    ASSERT_TRUE(conn->query("CREATE (:Person {name: 'Bob'});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    auto result = conn->query("MATCH (p:Person) RETURN COUNT(*);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);

    // Once the first Database is closed, the file can be opened for writing again.
    conn.reset();
    database.reset();
    Database reopened{databasePath, *systemConfig};
    Connection reopenedConn{&reopened};
    result = reopenedConn.query("MATCH (p:Person) RETURN COUNT(*);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
}

// The registry keys on file identity, so another path to the same file is refused too.
TEST_F(SameProcessWriteLockTest, SecondWritableOpenThroughHardLinkIsRefused) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    systemConfig->readOnly = false;
    createDBAndConn();
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Person(name STRING, PRIMARY KEY(name));")->isSuccess());
    const auto linkPath = databasePath + ".hardlink";
    ::unlink(linkPath.c_str());
    ASSERT_EQ(::link(databasePath.c_str(), linkPath.c_str()), 0) << strerror(errno);
    EXPECT_ANY_THROW(Database second(linkPath, *systemConfig));
    ::unlink(linkPath.c_str());
    EXPECT_TRUE(conn->query("CREATE (:Person {name: 'Alice'});")->isSuccess());
}

// The same holds for a symlink to the database file.
TEST_F(SameProcessWriteLockTest, SecondWritableOpenThroughSymlinkIsRefused) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    systemConfig->readOnly = false;
    createDBAndConn();
    const auto linkPath = databasePath + ".symlink";
    std::filesystem::remove(linkPath);
    std::filesystem::create_symlink(databasePath, linkPath);
    EXPECT_ANY_THROW(Database second(linkPath, *systemConfig));
    std::filesystem::remove(linkPath);
}

// A database file created by the first open has no identity when the path is claimed; a hard
// link made to it afterwards must still be refused.
TEST_F(SameProcessWriteLockTest, HardLinkToNewlyCreatedDatabaseIsRefused) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    conn.reset();
    database.reset();
    std::filesystem::remove_all(databasePath);
    systemConfig->readOnly = false;
    createDBAndConn();
    const auto linkPath = databasePath + ".hardlink";
    ::unlink(linkPath.c_str());
    ASSERT_EQ(::link(databasePath.c_str(), linkPath.c_str()), 0) << strerror(errno);
    EXPECT_ANY_THROW(Database second(linkPath, *systemConfig));
    ::unlink(linkPath.c_str());
}

// Read-only Databases next to the writer stay allowed (see
// WalTest.LargeWALRecordReplayWithPreviousDBAlive).
TEST_F(SameProcessWriteLockTest, ReadOnlyDatabaseNextToWritableOneStillOpens) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    systemConfig->readOnly = false;
    createDBAndConn();
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Person(name STRING, PRIMARY KEY(name));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Person {name: 'Alice'});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    auto readOnlyConfig = *systemConfig;
    readOnlyConfig.readOnly = true;
    Database readOnly{databasePath, readOnlyConfig};
    Connection readOnlyConn{&readOnly};
    auto result = readOnlyConn.query("MATCH (p:Person) RETURN COUNT(*);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

#ifndef __WASM__ // subprocesses are not supported in WASM
// Closing any descriptor of a file drops the process's POSIX locks on it. The refusal happens
// before the second open, so the first Database keeps its lock and another process still
// cannot open the file for writing.
TEST_F(SameProcessWriteLockTest, RefusedSecondWritableOpenKeepsTheProcessLock) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    systemConfig->readOnly = false;
    createDBAndConn();
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Person(name STRING, PRIMARY KEY(name));")->isSuccess());
    EXPECT_ANY_THROW(Database second(databasePath, *systemConfig));

    pid_t pid = fork();
    if (pid == 0) {
        int status = 0;
        try {
            Database other{databasePath, *systemConfig};
            status = 1; // Opened: the parent's lock was lost.
        } catch (...) {}
        _exit(status);
    }
    ASSERT_GT(pid, 0);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "another process opened the database for writing";
}
#endif

#endif // _WIN32
