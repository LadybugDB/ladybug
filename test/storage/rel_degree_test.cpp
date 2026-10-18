#include <algorithm>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "graph_test/private_graph_test.h"
#include "main/client_context.h"
#include "storage/storage_manager.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"
#include "transaction/transaction_context.h"

namespace lbug {
namespace testing {

using common::offset_t;
using common::RelDataDirection;
using common::row_idx_t;
using degree_entries_t = std::vector<std::pair<offset_t, row_idx_t>>;

// The degree APIs of a native rel table, checked against degrees counted by Cypher, over
// checkpointed, committed in-memory and uncommitted rels in two node groups.
class RelDegreeTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }

    void run(const std::string& query) {
        auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    }

    catalog::RelGroupCatalogEntry& relEntry() {
        auto context = getClientContext(*conn);
        auto transaction = transaction::Transaction::Get(*context);
        return catalog::Catalog::Get(*context)
            ->getTableCatalogEntry(transaction, "R")
            ->cast<catalog::RelGroupCatalogEntry>();
    }

    storage::RelTable& relTable() {
        auto context = getClientContext(*conn);
        auto tableID = relEntry().getSingleRelEntryInfo().oid;
        return storage::StorageManager::Get(*context)->getTable(tableID)->cast<storage::RelTable>();
    }

    degree_entries_t cypherDegrees(RelDataDirection direction) {
        auto pattern = direction == RelDataDirection::FWD ? "(a:N)-[:R]->(:N)" : "(a:N)<-[:R]-(:N)";
        auto result =
            conn->query(std::string("MATCH ") + pattern + " RETURN offset(id(a)), count(*)");
        degree_entries_t degrees;
        while (result->hasNext()) {
            auto row = result->getNext();
            degrees.emplace_back(row->getValue(0)->getValue<int64_t>(),
                row->getValue(1)->getValue<int64_t>());
        }
        std::sort(degrees.begin(), degrees.end());
        return degrees;
    }

    void checkDirection(const transaction::Transaction* transaction, RelDataDirection direction) {
        auto& table = relTable();
        auto expected = cypherDegrees(direction);
        ASSERT_FALSE(expected.empty());

        auto entries = table.getDegreeEntries(transaction, direction);
        EXPECT_TRUE(std::is_sorted(entries.begin(), entries.end()));
        EXPECT_EQ(entries, expected);
        // getDegreeForOffset must agree with the entries, including for checkpointed
        // (on-disk CSR header) data read before any scan pins the pages (#1292).
        for (const auto& [offset, degree] : expected) {
            EXPECT_EQ(table.getDegreeForOffset(transaction, direction, offset), degree)
                << "offset " << offset;
        }

        EXPECT_EQ(table.getNumActiveBoundNodes(transaction, direction), expected.size());

        row_idx_t totalDegree = 0;
        row_idx_t maxDegree = 0;
        for (const auto& [_, degree] : expected) {
            totalDegree += degree;
            maxDegree = std::max(maxDegree, degree);
        }
        auto summary = table.getDegreeSummary(transaction, direction);
        EXPECT_EQ(summary.numActiveBoundNodes, expected.size());
        EXPECT_EQ(summary.totalDegree, totalDegree);
        EXPECT_EQ(summary.maxDegree, maxDegree);
        if (direction == RelDataDirection::FWD) {
            auto count = conn->query("MATCH ()-[:R]->() RETURN count(*);");
            ASSERT_TRUE(count->isSuccess()) << count->getErrorMessage();
            EXPECT_EQ(count->getNext()->getValue(0)->getValue<int64_t>(), totalDegree);
        }

        auto topK = expected;
        std::sort(topK.begin(), topK.end(), [](const auto& a, const auto& b) {
            return a.second > b.second || (a.second == b.second && a.first < b.first);
        });
        topK.resize(std::min<size_t>(topK.size(), 3));
        EXPECT_EQ(table.getTopKDegrees(transaction, direction, 3), topK);
    }

    void checkDegrees() {
        auto context = getClientContext(*conn);
        const bool ownTransaction =
            !transaction::TransactionContext::Get(*context)->hasActiveTransaction();
        if (ownTransaction) {
            run("BEGIN TRANSACTION READ ONLY;");
        }
        auto transaction = transaction::Transaction::Get(*context);
        for (auto direction : relEntry().getRelDataDirections()) {
            checkDirection(transaction, direction);
        }
        if (ownTransaction) {
            run("COMMIT;");
        }
    }
};

TEST_F(RelDegreeTest, MatchesCypherAcrossNodeGroupsAndStorageStates) {
    run("CREATE NODE TABLE N(id INT64, PRIMARY KEY(id));");
    run("CREATE REL TABLE R(FROM N TO N);");
    // Checkpointed rels while the node table has one node group.
    run("UNWIND range(0, 131071) AS i CREATE (:N {id: i});");
    run("COPY R FROM (UNWIND range(0, 131071, 997) AS i UNWIND range(1, 1 + i % 4) AS k "
        "RETURN i, (i * 7 + k) % 131072);");
    run("CHECKPOINT;");
    run("UNWIND range(131072, 139999) AS i CREATE (:N {id: i});");
    checkDegrees();

    // Committed, not checkpointed: on top of checkpointed sources and from the second node
    // group, which gets a forward CSR node group with in-memory rels only.
    run("UNWIND [0, 997, 131071, 131072, 139999, 5] AS i "
        "MATCH (a:N {id: i}), (b:N {id: 7}) CREATE (a)-[:R]->(b);");
    checkDegrees();
    run("BEGIN TRANSACTION READ ONLY;");
    auto numNodeGroups = [&](RelDataDirection direction) {
        return relTable().getDirectedTableData(direction)->getNumNodeGroups();
    };
    EXPECT_EQ(numNodeGroups(RelDataDirection::FWD), 2u);
    const auto directions = relEntry().getRelDataDirections();
    const bool hasBwd =
        std::find(directions.begin(), directions.end(), RelDataDirection::BWD) != directions.end();
    if (hasBwd) {
        EXPECT_EQ(numNodeGroups(RelDataDirection::BWD), 1u);
    }
    run("COMMIT;");

    // Uncommitted rels merge with both; the backward ones land past the last CSR node group.
    run("BEGIN TRANSACTION;");
    run("UNWIND [0, 5, 131072, 135000] AS i "
        "MATCH (a:N {id: i}), (b:N {id: 135001}) CREATE (a)-[:R]->(b);");
    checkDegrees();
    run("COMMIT;");
    checkDegrees();
}

} // namespace testing
} // namespace lbug
