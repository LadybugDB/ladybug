#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/types/types.h"

namespace lbug {
namespace catalog {
class ConstraintCatalogEntry;
class TableCatalogEntry;
} // namespace catalog
namespace common {
class ValueVector;
class Value;
} // namespace common
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
// A UNIQUE constraint enforces that its key parts are jointly distinct across all rows
// of one node table or one rel group. Key parts are table properties plus, on rel
// groups, the `src`/`dst` endpoints. NULL is treated as distinct (SQL semantics): any
// row with NULL in a constrained part never conflicts.
//
// Enforcement is scan-based and therefore O(table size) per check: single-row inserts
// pay one scan, COPY validates once at the end of the load, and CREATE CONSTRAINT
// validates once at creation. A future index-backed implementation can replace these
// helpers without touching the catalog entry shape. Keys are exact per physical type
// (raw bytes with length prefixes, -0.0 normalized to +0.0); no lossy string form is
// used, so distinct values never collide.
struct UniqueConstraintChecker {
    // One bound key part in constraint order.
    struct ResolvedPart {
        enum class Kind : uint8_t { PROPERTY, SRC, DST };

        Kind kind = Kind::PROPERTY;
        // Storage column to scan. PROPERTY: the property's storage column; DST: the
        // neighbour-ID column; SRC: INVALID (taken from the bound node instead).
        common::column_id_t scanColumnID = common::INVALID_COLUMN_ID;
        // Position of the property in the table's current property list, for indexing
        // insert-state vectors (which are positionally aligned, unlike storage column
        // IDs which can hold gaps after DROP COLUMN). Unused for SRC/DST.
        uint32_t propertyPosition = common::INVALID_COLUMN_ID;
    };

    // Resolve a constraint's key parts against the current table schema.
    static std::vector<ResolvedPart> resolveParts(catalog::TableCatalogEntry* tableEntry,
        const catalog::ConstraintCatalogEntry* constraint);
    static std::vector<ResolvedPart> resolveParts(catalog::TableCatalogEntry* tableEntry,
        const std::vector<common::property_id_t>& propertyIDs);

    // Canonical key for the single selected row of keyVectors (positionally aligned with
    // parts). Returns nullopt when any part is NULL (never conflicts).
    static std::optional<std::string> buildKey(const std::vector<ResolvedPart>& parts,
        const std::vector<common::ValueVector*>& keyVectors);

    // Full-table scan collecting canonical keys of all visible non-NULL rows. Sees exactly
    // what the calling transaction sees (committed snapshot + own uncommitted writes).
    // Sets *stopFlag to request an early exit; scanners check it between batches and rows.
    static void collectExistingKeys(main::ClientContext* context, Table* table,
        const std::vector<ResolvedPart>& parts, const std::function<void(std::string&&)>& onKey,
        bool* stopFlag);

    // Full validation used by CREATE CONSTRAINT.
    static void validateNoDuplicates(main::ClientContext* context, Table* table,
        const std::vector<ResolvedPart>& parts, const std::string& constraintName);

    // True if the single selected row of keyVectors duplicates an existing row. Returns
    // false when there is no client context (WAL replay / internal writes operate on data
    // that was validated when first written).
    static bool hasDuplicate(main::ClientContext* context, Table* table,
        const std::vector<ResolvedPart>& parts,
        const std::vector<common::ValueVector*>& keyVectors);

    // Single-row insert hook for Table::insert paths. Resolves UNIQUE constraints on the
    // table (node tables) or rel group (rel pair tables) and throws on the first
    // duplicate. No-op when the table has no constraints, or when checks cannot run
    // (WAL replay and other context-less writes replay data that was validated when
    // first written).
    static void checkInsert(main::ClientContext* context, Table* table,
        const std::vector<common::ValueVector*>& propertyVectors,
        const common::ValueVector* srcNodeIDVector = nullptr,
        const common::ValueVector* dstNodeIDVector = nullptr);

    // End-of-COPY hook for batch insert paths. Validates every UNIQUE constraint on the
    // target node table or rel group (all pair tables share one key set, so cross-pair
    // duplicates are caught). Throws on the first duplicate; the surrounding transaction
    // aborts and discards the load.
    static void validateTableConstraints(main::ClientContext* context, Table* table);

    // Group-level validation over all (src, dst) pair tables of a rel group.
    static void validateRelGroupNoDuplicates(main::ClientContext* context,
        catalog::TableCatalogEntry* groupEntry, const std::vector<ResolvedPart>& parts,
        const std::string& constraintName);
};

} // namespace storage
} // namespace lbug
