#pragma once

#include <string>

#include "common/string_utils.h"

namespace lbug {
namespace catalog {

// System tables that expose the database schema as a queryable graph.
//   (t:schema_table) lists every user table (node + rel tables).
//   (s:schema_table)-[r:schema_rel]->(d:schema_table) lists every rel-table
//   connection from a source node table to a destination node table, with
//   r.name / r.id identifying the rel table that provides the connection.
//
// The names are deliberately prefixed (rather than bare `table` / `rel`) so they
// parse unescaped: TABLE is a reserved keyword and `MATCH (a:table)` is a parse
// error, while `schema_table` / `schema_rel` lex as plain identifiers.
static constexpr const char* SCHEMA_TABLE_NAME = "schema_table";
static constexpr const char* SCHEMA_REL_NAME = "schema_rel";

inline bool isSchemaGraphTableName(const std::string& name) {
    return common::StringUtils::caseInsensitiveEquals(name, SCHEMA_TABLE_NAME) ||
           common::StringUtils::caseInsensitiveEquals(name, SCHEMA_REL_NAME);
}

} // namespace catalog
} // namespace lbug
