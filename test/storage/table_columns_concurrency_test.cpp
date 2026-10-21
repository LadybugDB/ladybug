#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "graph_test/private_graph_test.h"
#include "main/client_context.h"
#include "storage/storage_manager.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"

namespace lbug {
namespace testing {

// A scan reads its table's columns when it starts. Checkpoint replaces a rel table's column
// vector and ALTER TABLE ADD grows a table's, while scans on other connections start.
class TableColumnsConcurrencyTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
    void run(const std::string& query) {
        auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    }

    // Runs `numReaders` connections that repeat `query` and expect `expected` until `write`
    // returns false, then checks that they read and never failed.
    template<typename F>
    void raceReaders(const std::string& query, int64_t expected, F write) {
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> reads{0};
        std::mutex failureMtx;
        std::string failure;
        auto reader = [&] {
            main::Connection readConn(database.get());
            while (!stop.load()) {
                auto result = readConn.query(query);
                std::string error;
                if (!result->isSuccess()) {
                    error = result->getErrorMessage();
                } else if (auto value = result->getNext()->getValue(0)->toString();
                           value != std::to_string(expected)) {
                    error = "read " + value;
                }
                if (!error.empty()) {
                    std::lock_guard<std::mutex> guard(failureMtx);
                    failure = failure.empty() ? error : failure;
                    return;
                }
                reads++;
            }
        };
        std::vector<std::thread> readers;
        for (auto i = 0; i < 8; i++) {
            readers.emplace_back(reader);
        }
        while (write()) {}
        stop = true;
        for (auto& thread : readers) {
            thread.join();
        }
        EXPECT_GT(reads.load(), 0u);
        std::lock_guard<std::mutex> guard(failureMtx);
        EXPECT_TRUE(failure.empty()) << failure;
    }

    // N(id, v) with ids 0..1999, v = 1, and R(FROM N TO N, p0..p31) with p<k> = k: five rels
    // from each of the first 1000 nodes, checkpointed so that scans read on-disk chunks.
    void createGraph() {
        run("CREATE NODE TABLE N(id INT64, v INT64, PRIMARY KEY(id));");
        std::string properties, values;
        for (auto k = 0; k < 32; k++) {
            properties += ", p" + std::to_string(k) + " INT64";
            values += ", " + std::to_string(k) + " AS p" + std::to_string(k);
        }
        run("CREATE REL TABLE R(FROM N TO N" + properties + ");");
        run("UNWIND range(0, 1999) AS i CREATE (:N {id: i, v: 1});");
        run("COPY R FROM (UNWIND range(0, 4999) AS i RETURN i % 1000, (i * 7) % 1000" + values +
            ");");
        run("CHECKPOINT;");
    }
};

#ifndef __SINGLE_THREADED__
// A rel scan reads its columns with RelTable::getColumn when it starts. Each checkpoint of R
// moves R's columns into a new vector; readers must keep getting the same Column* meanwhile.
TEST_F(TableColumnsConcurrencyTest, RelColumnsStayReadableWhileCheckpointReplacesThem) {
    createGraph();
    // The lookup needs a transaction; the table object outlives it.
    run("BEGIN TRANSACTION READ ONLY;");
    auto context = getClientContext(*conn);
    auto tableID = catalog::Catalog::Get(*context)
                       ->getTableCatalogEntry(transaction::Transaction::Get(*context), "R")
                       ->cast<catalog::RelGroupCatalogEntry>()
                       .getSingleRelEntryInfo()
                       .oid;
    auto& table =
        storage::StorageManager::Get(*context)->getTable(tableID)->cast<storage::RelTable>();
    run("COMMIT;");
    // A Column* stays valid, and getColumn keeps returning it, for every column a checkpoint keeps.
    std::vector<std::pair<common::RelDataDirection, std::vector<storage::Column*>>> expected;
    for (auto direction : table.getStorageDirections()) {
        std::vector<storage::Column*> columns;
        for (common::column_id_t id = 0; id < table.getNumColumns(); id++) {
            columns.push_back(table.getColumn(id, direction));
            ASSERT_NE(columns.back(), nullptr) << id;
        }
        expected.emplace_back(direction, std::move(columns));
    }
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0}, changed{0};
    std::vector<std::thread> readers;
    for (auto i = 0; i < 4; i++) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (const auto& [direction, columns] : expected) {
                    for (common::column_id_t id = 0; id < columns.size(); id++) {
                        if (table.getColumn(id, direction) != columns[id]) {
                            changed++;
                        }
                    }
                }
                reads++;
            }
        });
    }
    uint64_t readsBefore = 0;
    for (auto round = 0; round < 50; round++) {
        if (round == 1) {
            readsBefore = reads.load();
        }
        const auto src = std::to_string(1000 + round);
        run("MATCH (a:N {id: " + src + "}), (b:N {id: 0}) CREATE (a)-[:R {p0: 1}]->(b);");
        run("CHECKPOINT;");
    }
    const auto readsDuring = reads.load() - readsBefore;
    stop = true;
    for (auto& thread : readers) {
        thread.join();
    }
    EXPECT_GT(readsDuring, 0u);
    EXPECT_EQ(changed.load(), 0u);
}

// The two tests below rely on a sanitizer or a crash to detect the race: ADD reallocates the
// vector the scans index, and a read of the freed vector usually still finds the column.
TEST_F(TableColumnsConcurrencyTest, RelScansStartWhileAddColumnGrowsColumns) {
    createGraph();
    auto round = 0;
    raceReaders("MATCH (a:N {id: 5})-[k:R]->(:N) RETURN sum(k.p1);", 5, [&] {
        auto added = conn->query("ALTER TABLE R ADD q" + std::to_string(round) + " INT64;");
        EXPECT_TRUE(added->isSuccess()) << added->getErrorMessage();
        return ++round < 100;
    });
}

TEST_F(TableColumnsConcurrencyTest, NodeScansStartWhileAddColumnGrowsColumns) {
    createGraph();
    auto round = 0;
    raceReaders("MATCH (a:N {id: 5}) RETURN a.v;", 1, [&] {
        auto added = conn->query("ALTER TABLE N ADD q" + std::to_string(round) + " INT64;");
        EXPECT_TRUE(added->isSuccess()) << added->getErrorMessage();
        return ++round < 100;
    });
}
#endif

} // namespace testing
} // namespace lbug
