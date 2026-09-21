#pragma once

#include <string_view>

namespace lbug {
namespace main {

class ClientContext;

// Returns true when the query text references the schema graph tables
// (`schema_table` / `schema_rel`). Used to trigger an on-demand refresh before
// executing schema-graph MATCH queries.
bool QueryReferencesSchemaGraph(std::string_view query);

// Creates the schema graph tables on first use and repopulates them from the
// current catalog snapshot (main database only). Failures are swallowed: the
// caller's original query still runs (possibly against stale/empty data) so a
// read-only database or an explicit read transaction never breaks.
void EnsureSchemaGraphFresh(ClientContext* context);

} // namespace main
} // namespace lbug
