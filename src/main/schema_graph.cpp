#include "main/schema_graph.h"

#include "catalog/schema_graph.h"
#include "common/constants.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/query_result.h"
#include "processor/result/flat_tuple.h"

namespace lbug {
namespace main {

bool QueryReferencesSchemaGraph(std::string_view query) {
    // Only MATCH (read) queries need a fresh snapshot. DDL/DML that manages
    // the system tables themselves (DROP/CREATE/DELETE issued by cleanup
    // tooling or power users) must NOT trigger a refresh, otherwise dropping
    // schema_rel would recreate it via the pre-drop refresh and leak it.
    if (common::StringUtils::getUpper(query).find("MATCH") == std::string::npos) {
        return false;
    }
    // Case-sensitive substring search: the system tables are lowercase, and an
    // exact-case match is what the binder will resolve. Matches inside string
    // literals cause at most one redundant refresh (cheap: a few writes for a
    // tiny schema), never incorrect results.
    return query.find(catalog::SCHEMA_TABLE_NAME) != std::string_view::npos ||
           query.find(catalog::SCHEMA_REL_NAME) != std::string_view::npos;
}

static std::string escapeCypherStringLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'') {
            out += "\\'";
        } else if (c == '\\') {
            out += "\\\\";
        } else {
            out += c;
        }
    }
    return out;
}

static bool execIgnoreErrors(ClientContext* context, const std::string& query) {
    auto result = context->query(query);
    return result && result->isSuccess();
}

void EnsureSchemaGraphFresh(ClientContext* context) {
    if (context == nullptr) {
        return;
    }
    // Best-effort only: never let a refresh failure break the user's query
    // (e.g. read-only database, active read transaction).
    try {
        // 1. Create the system tables on first use.
        if (!execIgnoreErrors(context,
                "CREATE NODE TABLE IF NOT EXISTS schema_table(name STRING, id UINT64, "
                "type STRING, PRIMARY KEY(name))")) {
            return;
        }
        if (!execIgnoreErrors(context,
                "CREATE REL TABLE IF NOT EXISTS schema_rel(FROM schema_table TO schema_table, "
                "name STRING, id UINT64, MANY_MANY)")) {
            return;
        }
        // 2. Snapshot user tables in the main database. SHOW_TABLES also lists
        // graphs / attached databases; restrict to the main database so table
        // names stay unique (schema_table has PRIMARY KEY(name)).
        auto tablesResult = context->query("CALL show_tables() RETURN *");
        if (tablesResult == nullptr || !tablesResult->isSuccess()) {
            return;
        }
        struct TableRow {
            uint64_t id;
            std::string name;
            std::string type;
        };
        std::vector<TableRow> tables;
        while (tablesResult->hasNext()) {
            auto row = tablesResult->getNext();
            auto dbName = row->getValue(3)->toString();
            if (dbName != common::LOCAL_DB_NAME) {
                continue;
            }
            auto name = row->getValue(1)->toString();
            if (catalog::isSchemaGraphTableName(name)) {
                continue;
            }
            TableRow r;
            r.id = row->getValue(0)->getValue<uint64_t>();
            r.name = std::move(name);
            r.type = row->getValue(2)->toString();
            tables.push_back(std::move(r));
        }
        // 3. Clear previous snapshot (DETACH removes incident edges too).
        execIgnoreErrors(context, "MATCH (t:schema_table) DETACH DELETE t");
        // 4. Repopulate nodes: one schema_table node per user table (both NODE
        // and REL tables, so MATCH (t:schema_table) lists the full catalog).
        for (auto& t : tables) {
            auto q = "CREATE (:schema_table {name: '" + escapeCypherStringLiteral(t.name) +
                     "', id: " + std::to_string(t.id) + ", type: '" +
                     escapeCypherStringLiteral(t.type) + "'})";
            execIgnoreErrors(context, q);
        }
        // 5. Repopulate edges: one schema_rel edge per (rel table, src, dst)
        // connection. show_connection lists source/destination table names.
        for (auto& t : tables) {
            if (t.type != "NODE" && t.type != "REL") {
                continue;
            }
            // Only rel tables have connections; skip node tables quickly by
            // probing show_connection (returns 0 rows for node tables would
            // error, so guard by type).
            if (t.type != "REL") {
                continue;
            }
            auto connQuery =
                "CALL show_connection('" + escapeCypherStringLiteral(t.name) + "') RETURN *";
            auto connResult = context->query(connQuery);
            if (connResult == nullptr || !connResult->isSuccess()) {
                continue;
            }
            while (connResult->hasNext()) {
                auto row = connResult->getNext();
                auto srcName = row->getValue(0)->toString();
                auto dstName = row->getValue(1)->toString();
                auto q = "MATCH (s:schema_table), (d:schema_table) WHERE s.name = '" +
                         escapeCypherStringLiteral(srcName) + "' AND d.name = '" +
                         escapeCypherStringLiteral(dstName) + "' CREATE (s)-[:schema_rel {name: '" +
                         escapeCypherStringLiteral(t.name) + "', id: " + std::to_string(t.id) +
                         "}]->(d)";
                execIgnoreErrors(context, q);
            }
        }
    } catch (...) {
        // Swallow everything: refresh is best-effort.
    }
}

} // namespace main
} // namespace lbug
