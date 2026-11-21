#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <new>
#include <thread>

#include "api_test/api_test.h"

using namespace lbug::testing;
using namespace lbug::common;
using namespace lbug::main;

namespace {

// State shared between a forked child and the parent. Lock-free atomics are address-free, so
// they work across processes in a MAP_SHARED mapping.
struct SharedState {
    std::atomic<int> childReady{0};
    std::atomic<int> parentDone{0};
};
static_assert(std::atomic<int>::is_always_lock_free);

constexpr auto deadline = std::chrono::seconds(60);

SharedState* mapSharedState() {
    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (memory == MAP_FAILED) {
        return nullptr;
    }
    return new (memory) SharedState{};
}

// Keeps the child alive, holding its database open, until the parent is done or the deadline
// passes, then exits without running the parent's atexit handlers or gtest teardown.
[[noreturn]] void holdUntilParentDone(SharedState& state, Connection& conn) {
    state.childReady = 1;
    const auto start = std::chrono::steady_clock::now();
    while (!state.parentDone && std::chrono::steady_clock::now() - start < deadline) {
        conn.query("MATCH (:Person) RETURN COUNT(*)");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    _exit(0);
}

// Waits until the child reports that it holds its database open. Fails if the child exits or
// the deadline passes first.
bool waitForChildReady(SharedState& state, pid_t pid) {
    const auto start = std::chrono::steady_clock::now();
    while (!state.childReady) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return false;
        }
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Reaps the child on every exit path of the parent, so a failed assertion cannot leave it
// running. It is asked to stop first, then killed if it does not exit in time.
class ChildGuard {
public:
    ChildGuard(SharedState* state, pid_t pid) : state{state}, pid{pid} {}
    ~ChildGuard() {
        state->parentDone = 1;
        const auto start = std::chrono::steady_clock::now();
        int status = 0;
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        munmap(state, sizeof(SharedState));
    }

private:
    SharedState* state;
    pid_t pid;
};

} // namespace

// Open the database in another process, so these tests exercise the file locks between
// processes. Locks within one process are a separate mechanism (Database's in-process open
// claim).
class DBLockingTest : public ApiTest {
    void SetUp() override { BaseGraphTest::SetUp(); }
};

TEST_F(DBLockingTest, testReadLock) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    auto* state = mapSharedState();
    ASSERT_NE(state, nullptr);
    // Create the database, then close it before forking.
    createDBAndConn();
    ASSERT_TRUE(conn->query("CREATE NODE TABLE Person(name STRING, age INT64, PRIMARY KEY(name));")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Person {name: 'Alice', age: 25});")->isSuccess());
    conn.reset();
    database.reset();

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        systemConfig->readOnly = true;
        try {
            createDBAndConn();
        } catch (...) {
            _exit(1);
        }
        holdUntilParentDone(*state, *conn);
    }
    ChildGuard guard{state, pid};
    ASSERT_TRUE(waitForChildReady(*state, pid)) << "the child could not open the database";
    // Read-only opens take no lock on the data file (only writers do), so a reader in another
    // process blocks neither a writer nor another reader.
    systemConfig->readOnly = false;
    EXPECT_NO_THROW(createDBAndConn());
    conn.reset();
    database.reset();
    systemConfig->readOnly = true;
    EXPECT_NO_THROW(createDBAndConn());
    ASSERT_NE(conn, nullptr);
    EXPECT_TRUE(conn->query("MATCH (:Person) RETURN COUNT(*)")->isSuccess());
}

TEST_F(DBLockingTest, testWriteLock) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    auto* state = mapSharedState();
    ASSERT_NE(state, nullptr);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        systemConfig->readOnly = false;
        try {
            createDBAndConn();
        } catch (...) {
            _exit(1);
        }
        if (!conn->query("CREATE NODE TABLE Person(name STRING, age INT64, PRIMARY KEY(name));")
                 ->isSuccess() ||
            !conn->query("CREATE (:Person {name: 'Alice', age: 25});")->isSuccess()) {
            _exit(2);
        }
        holdUntilParentDone(*state, *conn);
    }
    ChildGuard guard{state, pid};
    ASSERT_TRUE(waitForChildReady(*state, pid)) << "the child could not open the database";
    // A writer in another process holds the data file's write lock, so a second writer is
    // refused. A reader takes no lock and still opens.
    systemConfig->readOnly = false;
    EXPECT_ANY_THROW(createDBAndConn());
    systemConfig->readOnly = true;
    EXPECT_NO_THROW(createDBAndConn());
    ASSERT_NE(conn, nullptr);
    EXPECT_TRUE(conn->query("MATCH (:Person) RETURN COUNT(*)")->isSuccess());
}

TEST_F(DBLockingTest, testReadOnly) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    // A read-only database cannot be created in a new directory.
    systemConfig->readOnly = true;
    EXPECT_ANY_THROW(createDBAndConn());

    // Create and populate the database in another process.
    const pid_t createPid = fork();
    ASSERT_GE(createPid, 0);
    if (createPid == 0) {
        systemConfig->readOnly = false;
        try {
            createDBAndConn();
        } catch (...) {
            _exit(1);
        }
        const bool ok =
            conn->query("CREATE NODE TABLE Person(name STRING, age INT64, PRIMARY KEY(name));")
                ->isSuccess() &&
            conn->query("CREATE (:Person {name: 'Alice', age: 25});")->isSuccess() &&
            conn->query("CHECKPOINT;")->isSuccess();
        _exit(ok ? 0 : 2);
    }
    int status = 0;
    ASSERT_EQ(waitpid(createPid, &status, 0), createPid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "the child failed to create the database";

    // Now connect in read-only mode.
    systemConfig->readOnly = true;
    EXPECT_NO_THROW(createDBAndConn());
    ASSERT_NE(conn, nullptr);
    // We can query the database...
    ASSERT_TRUE(conn->query("MATCH (:Person) RETURN COUNT(*)")->isSuccess());
    // ...but every write is refused by the read-only guard. Each statement would succeed on a
    // writable database, so the message pins why it fails.
    for (const auto* query :
        {"CREATE NODE TABLE university(ID INT64, PRIMARY KEY(ID))", "ALTER TABLE Person DROP age",
            "DROP TABLE Person", "CREATE (:Person {name: 'Bob', age: 25});",
            "MATCH (p:Person) WHERE p.name='Alice' SET p.age=26;",
            "MATCH (p:Person) WHERE p.name='Alice' DELETE p;"}) {
        auto result = conn->query(query);
        EXPECT_FALSE(result->isSuccess()) << query;
        EXPECT_NE(result->getErrorMessage().find("read-only database"), std::string::npos)
            << query << ": " << result->getErrorMessage();
    }
}
