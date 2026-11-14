#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/types/types.h"

namespace lbug {
namespace catalog {
class RelGroupCatalogEntry;
}
namespace common {
class ValueVector;
}
namespace main {
class ClientContext;
}
namespace transaction {
class Transaction;
}
namespace storage {
class Table;
class MemoryManager;

// Scan-based UNIQUE constraint enforcement (MVP: no secondary index structures).
//
// A UNIQUE constraint declares that the tuple of its property columns is distinct across
// all rows of one node table or one rel group. NULL is treated as distinct: any row with
// NULL in a constrained property never conflicts.
//
// Enforcement is scan-based and therefore O(table size) per check: single-row inserts pay
// one scan, COPY validates once at the end of the load, and CREATE CONSTRAINT validates
// once at creation. A future index-backed implementation can replace these helpers
// without touching callers; the catalog entry shape is already index-agnostic.
struct UniqueConstraintChecker {
    // Canonical key for the single selected row of keyVectors (positionally aligned with
    // columnIDs/count). Returns nullopt when any constrained value is NULL (never conflicts).
    static std::optional<std::string> buildKey(const std::vector<common::ValueVector*>& keyVectors);

    // Collect canonical keys of all visible non-NULL rows into onKey. Sees exactly what the
    // calling transaction sees (committed snapshot + own uncommitted writes).
    static void collectExistingKeys(main::ClientContext* context, Table* table,
        const std::vector<common::column_id_t>& columnIDs,
        const std::function<void(std::string&&)>& onKey);

    // Throw if any duplicate non-NULL key exists. Used by CREATE CONSTRAINT and end-of-COPY
    // validation.
    static void validateNoDuplicates(main::ClientContext* context, Table* table,
        const std::vector<common::column_id_t>& columnIDs, const std::string& constraintName);

    // True if the single selected row of keyVectors duplicates an existing row. Returns
    // false when there is no client context (WAL replay / internal writes operate on data
    // that was validated when first written).
    static bool hasDuplicate(main::ClientContext* context, Table* table,
        const std::vector<common::column_id_t>& columnIDs,
        const std::vector<common::ValueVector*>& keyVectors);

    // Single-row insert hook for Table::insert paths. Resolves UNIQUE constraints on the
    // table (node tables) or rel group (rel pair tables), maps the insert state's property
    // vectors onto each constraint's columns, and throws on the first duplicate.
    // No-op when the table has no constraints, or when checks cannot run (WAL replay and
    // other context-less writes replay data that was validated when first written).
    static void checkInsert(main::ClientContext* context, Table* table,
        const std::vector<common::ValueVector*>& propertyVectors);

    // End-of-COPY hook for batch insert paths. Validates every UNIQUE constraint on the
    // target node table or rel group (all pair tables share one key set, so cross-pair
    // duplicates are caught). Throws on the first duplicate; the surrounding transaction
    // aborts and discards the load.
    static void validateTableConstraints(main::ClientContext* context, Table* table);
    // Group-level validation over all (src, dst) pair tables of a rel group.
    static void validateRelGroupNoDuplicates(main::ClientContext* context,
        catalog::RelGroupCatalogEntry* groupEntry,
        const std::vector<common::column_id_t>& columnIDs, const std::string& constraintName);
};

} // namespace storage
} // namespace lbug
