#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lbug.hpp"
#include <format>

using namespace lbug::main;
using lbug::common::Value;

static void check(QueryResult& result, const std::string& query) {
    if (!result.isSuccess()) {
        std::cerr << "Query failed: " << query << "\n" << result.getErrorMessage() << "\n";
        std::exit(1);
    }
}

static void runQuery(Connection& conn, const std::string& query) {
    auto result = conn.query(query);
    check(*result, query);
}

static void checkPrepared(PreparedStatement& statement, const std::string& query) {
    if (!statement.isSuccess()) {
        std::cerr << "Prepare failed: " << query << "\n" << statement.getErrorMessage() << "\n";
        std::exit(1);
    }
}

struct Config {
    double seconds = 10.0;
    bool preserveDB = false;
    std::vector<uint64_t> threadCounts = {1, 2, 4, 8};
};

static void usage(const char* program) {
    std::cerr << "Usage: " << program << " [--seconds N] [--preserve-db] [thread_count ...]\n"
              << "Example: " << program << " --seconds 30 --preserve-db 1 2 4 8\n";
}

static Config parseArgs(int argc, char** argv) {
    Config config;
    config.threadCounts.clear();
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--seconds" || arg == "-s") {
            if (++i == argc) {
                throw std::invalid_argument("--seconds requires a value");
            }
            config.seconds = std::stod(argv[i]);
            if (config.seconds <= 0.0) {
                throw std::invalid_argument("--seconds must be greater than 0");
            }
            continue;
        }
        if (arg == "--preserve-db") {
            config.preserveDB = true;
            continue;
        }
        const auto numThreads = std::stoull(arg);
        if (numThreads == 0) {
            throw std::invalid_argument("thread_count must be greater than 0");
        }
        config.threadCounts.push_back(numThreads);
    }
    if (config.threadCounts.empty()) {
        config.threadCounts = {1, 2, 4, 8};
    }
    return config;
}

struct CaseResult {
    uint64_t totalWrites;
    double seconds;
    std::string dbPath;
};

static CaseResult runCase(const Config& benchConfig, uint64_t numThreads) {
    const auto dbPath = std::filesystem::temp_directory_path() /
                        std::format("lbug_node_write_bench_{}_{}", numThreads,
                            std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::remove_all(dbPath);

    SystemConfig systemConfig;
    systemConfig.enableMultiWrites = true;
    systemConfig.autoCheckpoint = false;
    systemConfig.forceCheckpointOnClose = false;
    systemConfig.enableCompression = false;
    Database database(dbPath.string(), systemConfig);
    Connection setup(&database);
    runQuery(setup, "CALL debug_enable_multi_writes=true;");
    runQuery(setup, "CALL enable_default_hash_index=false;");
    runQuery(setup, "CREATE NODE TABLE person(id INT64 PRIMARY KEY, name STRING);");
    runQuery(setup, "CHECKPOINT;");

    std::atomic<uint64_t> ready = 0;
    std::atomic<bool> start = false;
    std::vector<uint64_t> writes(numThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    const auto begin = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration<double>(benchConfig.seconds);
    for (uint64_t threadIdx = 0; threadIdx < numThreads; threadIdx++) {
        threads.emplace_back([&, threadIdx]() {
            Connection conn(&database);
            const std::string createQuery = "CREATE (:person {id: $id, name: $name});";
            std::unordered_map<std::string, std::unique_ptr<Value>> initialParams;
            initialParams.insert({"id", std::make_unique<Value>(static_cast<int64_t>(0))});
            initialParams.insert({"name", std::make_unique<Value>(std::string("Person0"))});
            auto preparedCreate = conn.prepareWithParams(createQuery, std::move(initialParams));
            checkPrepared(*preparedCreate, createQuery);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto stopTime = std::chrono::steady_clock::now() + duration;
            uint64_t localWrites = 0;
            while (true) {
                if ((localWrites & 63) == 0 && std::chrono::steady_clock::now() >= stopTime) {
                    break;
                }
                const auto id = threadIdx + localWrites * numThreads;
                preparedCreate->setParameter("id", Value(static_cast<int64_t>(id)));
                preparedCreate->setParameter("name", Value(std::format("Person{}", id)));
                auto result = conn.execute(preparedCreate.get());
                check(*result, createQuery);
                localWrites++;
            }
            writes[threadIdx] = localWrites;
        });
    }
    while (ready.load(std::memory_order_acquire) != numThreads) {
        std::this_thread::yield();
    }
    const auto startTime = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto endTime = std::chrono::steady_clock::now();
    const auto setupSeconds = std::chrono::duration<double>(startTime - begin).count();
    (void)setupSeconds;

    uint64_t totalWrites = 0;
    for (const auto threadWrites : writes) {
        totalWrites += threadWrites;
    }
    auto result = setup.query("MATCH (p:person) RETURN COUNT(p);");
    check(*result, "MATCH (p:person) RETURN COUNT(p);");
    const auto count = result->getNext()->getValue(0)->getValue<int64_t>();
    const auto expected = static_cast<int64_t>(totalWrites);
    if (count != expected) {
        std::cerr << "Expected " << expected << " rows, found " << count << "\n";
        std::exit(1);
    }

    const auto seconds = std::chrono::duration<double>(endTime - startTime).count();
    if (!benchConfig.preserveDB) {
        std::filesystem::remove_all(dbPath);
    }
    return {.totalWrites = totalWrites, .seconds = seconds, .dbPath = dbPath.string()};
}

int main(int argc, char** argv) {
    Config config;
    try {
        config = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        usage(argv[0]);
        return 1;
    }

    std::cout << "target_seconds,actual_seconds,total_writes,threads,writes_per_sec,db_path\n";
    for (const auto threads : config.threadCounts) {
        const auto result = runCase(config, threads);
        const auto writesPerSec = static_cast<double>(result.totalWrites) / result.seconds;
        std::cout << config.seconds << "," << result.seconds << "," << result.totalWrites << ","
                  << threads << "," << writesPerSec << "," << result.dbPath << "\n";
    }
}
