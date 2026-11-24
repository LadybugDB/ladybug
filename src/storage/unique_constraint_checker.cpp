#include "storage/unique_constraint_checker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/constraint_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/data_chunk/data_chunk.h"
#include "common/data_chunk/data_chunk_state.h"
#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/types/int128_t.h"
#include "common/types/interval_t.h"
#include "common/types/types.h"
#include "common/types/value/value.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/storage_manager.h"
#include "storage/table/csr_node_group.h"
#include "storage/table/node_group_collection.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace storage {

namespace {

// Logical type tag first, so values of different (but physically identical) types never
// collide, then a null flag, then the payload. Fixed-size payloads are raw bytes;
// variable-size payloads are length-prefixed so concatenations stay unambiguous
// (e.g. ['a,b'] vs ['a','b'], or {a:'x', b:'y, b: z'} vs {a:'x, b: y', b:'z'}).
// FLOAT/DOUBLE normalize -0.0 to +0.0 to match Value::operator==; NaN payloads are kept
// bit-exact (same-bits NaNs conflict, mirroring exact physical comparison).
void appendUint16(std::string& out, uint16_t v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

void appendUint64(std::string& out, uint64_t v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

void appendRawBytes(std::string& out, const void* data, size_t n) {
    out.append(reinterpret_cast<const char*>(data), n);
}

void appendExactValue(std::string& out, const Value& value);

void appendExactChildren(std::string& out, const Value& value) {
    const auto count = value.getChildrenSize();
    appendUint64(out, count);
    for (auto i = 0u; i < count; i++) {
        const auto* child = value.getChildValue(i);
        if (child == nullptr || child->isNull()) {
            out.push_back(0);
            continue;
        }
        out.push_back(1);
        appendExactValue(out, *child);
    }
}

void appendExactValue(std::string& out, const Value& value) {
    const auto& type = value.getDataType();
    appendUint16(out, static_cast<uint16_t>(type.getLogicalTypeID()));
    if (value.isNull()) {
        out.push_back(0);
        return;
    }
    out.push_back(1);
    switch (type.getPhysicalType()) {
    case PhysicalTypeID::BOOL: {
        out.push_back(value.val.booleanVal ? 1 : 0);
    } break;
    case PhysicalTypeID::INT8: {
        out.push_back(static_cast<char>(value.val.int8Val));
    } break;
    case PhysicalTypeID::INT16: {
        appendRawBytes(out, &value.val.int16Val, sizeof(value.val.int16Val));
    } break;
    case PhysicalTypeID::INT32: {
        appendRawBytes(out, &value.val.int32Val, sizeof(value.val.int32Val));
    } break;
    case PhysicalTypeID::INT64: {
        appendRawBytes(out, &value.val.int64Val, sizeof(value.val.int64Val));
    } break;
    case PhysicalTypeID::INT128: {
        appendRawBytes(out, &value.val.int128Val, sizeof(value.val.int128Val));
    } break;
    case PhysicalTypeID::UINT8: {
        out.push_back(static_cast<char>(value.val.uint8Val));
    } break;
    case PhysicalTypeID::UINT16: {
        appendRawBytes(out, &value.val.uint16Val, sizeof(value.val.uint16Val));
    } break;
    case PhysicalTypeID::UINT32: {
        appendRawBytes(out, &value.val.uint32Val, sizeof(value.val.uint32Val));
    } break;
    case PhysicalTypeID::UINT64: {
        appendRawBytes(out, &value.val.uint64Val, sizeof(value.val.uint64Val));
    } break;
    case PhysicalTypeID::UINT128: {
        appendRawBytes(out, &value.val.uint128Val, sizeof(value.val.uint128Val));
    } break;
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::ALP_EXCEPTION_FLOAT: {
        auto v = value.val.floatVal;
        if (v == 0) {
            v = 0.0f;
        }
        appendRawBytes(out, &v, sizeof(v));
    } break;
    case PhysicalTypeID::DOUBLE:
    case PhysicalTypeID::ALP_EXCEPTION_DOUBLE: {
        auto v = value.val.doubleVal;
        if (v == 0) {
            v = 0.0;
        }
        appendRawBytes(out, &v, sizeof(v));
    } break;
    case PhysicalTypeID::INTERVAL: {
        // Serialize the total micros so that equal intervals (e.g. '1 month'
        // and '30 days') share the same key, consistent with interval_t::operator==.
        // Serializing raw (months, days, micros) would treat them as distinct.
        const auto& iv = value.val.intervalVal;
        const common::int128_t total =
            common::int128_t(iv.months) * common::int128_t(common::Interval::MICROS_PER_MONTH) +
            common::int128_t(iv.days) * common::int128_t(common::Interval::MICROS_PER_DAY) +
            common::int128_t(iv.micros);
        appendRawBytes(out, &total.low, sizeof(total.low));
        appendRawBytes(out, &total.high, sizeof(total.high));
    } break;
    case PhysicalTypeID::INTERNAL_ID: {
        const auto& id = value.val.internalIDVal;
        appendRawBytes(out, &id.offset, sizeof(id.offset));
        appendRawBytes(out, &id.tableID, sizeof(id.tableID));
    } break;
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::JSON: {
        appendUint64(out, value.strVal.size());
        out.append(value.strVal);
    } break;
    case PhysicalTypeID::LIST:
    case PhysicalTypeID::ARRAY: {
        appendExactChildren(out, value);
    } break;
    case PhysicalTypeID::STRUCT: {
        // Field names disambiguate structs whose values coincide field-wise.
        const auto& fields = StructType::getFields(type);
        appendUint64(out, fields.size());
        for (auto& field : fields) {
            appendUint64(out, field.getName().size());
            out.append(field.getName());
        }
        appendExactChildren(out, value);
    } break;
    default: {
        // MAP materializes through the LIST/STRUCT cases above; NODE/REL/POINTER/ANY can
        // never be stored property values. Fall back to the display form rather than
        // failing closed on exotic extension types.
        appendUint64(out, value.toString().size());
        out.append(value.toString());
    } break;
    }
}

// Internal-ID bytes identical to what appendExactValue emits for an INTERNAL_ID value,
// so scan-side SRC keys (built from the bound offset) match insert-side SRC keys (built
// from the bound node-ID vector).
void appendNodeIDBytes(std::string& out, offset_t offset, table_id_t tableID) {
    appendUint16(out, static_cast<uint16_t>(LogicalTypeID::INTERNAL_ID));
    out.push_back(1);
    appendRawBytes(out, &offset, sizeof(offset));
    appendRawBytes(out, &tableID, sizeof(tableID));
}

bool rowIsNull(const std::vector<ValueVector*>& vectors, sel_t pos) {
    for (auto* vec : vectors) {
        if (vec->isNull(pos)) {
            return true;
        }
    }
    return false;
}

std::string buildKeyFromRow(const std::vector<ValueVector*>& vectors, sel_t pos) {
    std::string key;
    for (auto* vec : vectors) {
        auto val = vec->getAsValue(pos);
        appendExactValue(key, *val);
    }
    return key;
}

void throwDuplicateError(const std::string& constraintName) {
    throw RuntimeException(
        std::format("Unique constraint {} violated: duplicate key.", constraintName));
}

} // namespace

std::vector<UniqueConstraintChecker::ResolvedPart> UniqueConstraintChecker::resolveParts(
    TableCatalogEntry* tableEntry, const std::vector<property_id_t>& propertyIDs) {
    std::vector<ResolvedPart> parts;
    // Insert-state vectors are positionally aligned with the table's current property list
    // (unlike storage column IDs, positions have no gaps after DROP COLUMN).
    const auto properties = tableEntry->getProperties();
    for (auto propertyID : propertyIDs) {
        if (propertyID == ConstraintCatalogEntry::SRC_ENDPOINT_ID) {
            parts.push_back(
                ResolvedPart{ResolvedPart::Kind::SRC, INVALID_COLUMN_ID, INVALID_COLUMN_ID});
            continue;
        }
        if (propertyID == ConstraintCatalogEntry::DST_ENDPOINT_ID) {
            parts.push_back(
                ResolvedPart{ResolvedPart::Kind::DST, NBR_ID_COLUMN_ID, INVALID_COLUMN_ID});
            continue;
        }
        uint32_t position = INVALID_COLUMN_ID;
        for (auto i = 0u; i < properties.size(); i++) {
            if (tableEntry->getPropertyID(properties[i].getName()) == propertyID) {
                position = i;
                break;
            }
        }
        if (position == INVALID_COLUMN_ID) {
            throw RuntimeException(std::format(
                "Unique constraint internal error: property ID {} not found in table {}.",
                propertyID, tableEntry->getName()));
        }
        const auto columnID = tableEntry->getColumnID(properties[position].getName());
        parts.push_back(ResolvedPart{ResolvedPart::Kind::PROPERTY, columnID, position});
    }
    return parts;
}

std::vector<UniqueConstraintChecker::ResolvedPart> UniqueConstraintChecker::resolveParts(
    TableCatalogEntry* tableEntry, const ConstraintCatalogEntry* constraint) {
    return resolveParts(tableEntry, constraint->getPropertyIDs());
}

std::optional<std::string> UniqueConstraintChecker::buildKey(const std::vector<ResolvedPart>& parts,
    const std::vector<ValueVector*>& keyVectors) {
    DASSERT(!parts.empty() && parts.size() == keyVectors.size());
    std::string key;
    for (auto i = 0u; i < parts.size(); i++) {
        auto* vec = keyVectors[i];
        if (vec == nullptr) {
            return std::nullopt;
        }
        DASSERT(vec->state->getSelVector().getSelSize() == 1);
        const auto p = vec->state->getSelVector()[0];
        if (vec->isNull(p)) {
            return std::nullopt;
        }
        auto val = vec->getAsValue(p);
        appendExactValue(key, *val);
    }
    return key;
}

static void scanNodeGroups(transaction::Transaction* transaction, NodeTable* table,
    node_group_idx_t numGroups, const std::function<NodeGroup*(node_group_idx_t)>& getGroup,
    TableScanSource source, const std::vector<column_id_t>& columnIDs,
    const std::vector<ValueVector*>& outVectors, std::shared_ptr<DataChunkState> chunkState,
    const std::function<void(std::string&&)>& onKey, bool* stopFlag) {
    auto scanState = std::make_unique<NodeTableScanState>(nullptr, outVectors, chunkState);
    scanState->setToTable(transaction, table, columnIDs, {});
    scanState->source = source;
    for (node_group_idx_t groupIdx = 0; groupIdx < numGroups; groupIdx++) {
        if (*stopFlag) {
            return;
        }
        auto* group = getGroup(groupIdx);
        if (group == nullptr || group->getNumChunkedGroups() == 0) {
            continue;
        }
        scanState->nodeGroup = group;
        scanState->nodeGroupIdx = groupIdx;
        group->initializeScanState(transaction, *scanState);
        while (true) {
            const auto res = group->scan(transaction, *scanState);
            if (res == NODE_GROUP_SCAN_EMPTY_RESULT) {
                break;
            }
            // Rows are written densely from position 0; visibility (deleted rows, own
            // uncommitted state) is expressed through the output selection vector.
            const auto& sel = chunkState->getSelVector();
            for (auto s = 0u; s < sel.getSelSize(); s++) {
                const auto pos = sel[s];
                if (rowIsNull(outVectors, pos)) {
                    continue;
                }
                onKey(buildKeyFromRow(outVectors, pos));
                if (*stopFlag) {
                    return;
                }
            }
        }
    }
}

static void scanCommittedAndLocalNodeKeys(transaction::Transaction* transaction, NodeTable* table,
    const std::vector<column_id_t>& columnIDs, MemoryManager* mm,
    const std::function<void(std::string&&)>& onKey, bool* stopFlag) {
    std::vector<LogicalType> columnTypes;
    for (auto columnID : columnIDs) {
        columnTypes.push_back(table->getColumn(columnID).getDataType().copy());
    }
    auto chunk = Table::constructDataChunk(mm, std::move(columnTypes));
    std::vector<ValueVector*> outVectors;
    for (auto& vec : chunk.valueVectors) {
        outVectors.push_back(vec.get());
    }
    scanNodeGroups(
        transaction, table, table->getNumNodeGroups(),
        [&](node_group_idx_t idx) { return table->getNodeGroup(idx); }, TableScanSource::COMMITTED,
        columnIDs, outVectors, chunk.state, onKey, stopFlag);
    if (*stopFlag) {
        return;
    }
    if (auto* localTable = transaction->getLocalStorage()->getLocalTable(*table)) {
        auto& localGroups = localTable->cast<LocalNodeTable>().getNodeGroups();
        scanNodeGroups(
            transaction, table, localGroups.getNumNodeGroups(),
            [&](node_group_idx_t idx) { return localGroups.getNodeGroup(idx); },
            TableScanSource::UNCOMMITTED, columnIDs, outVectors, chunk.state, onKey, stopFlag);
    }
}

static void scanRelPairKeys(main::ClientContext* context, RelTable* pairTable,
    const std::vector<UniqueConstraintChecker::ResolvedPart>& parts,
    const std::function<void(std::string&&)>& onKey, bool* stopFlag) {
    using ResolvedPart = UniqueConstraintChecker::ResolvedPart;
    auto transaction = transaction::Transaction::Get(*context);
    auto mm = MemoryManager::Get(*context);
    auto storageManager = StorageManager::Get(*context);
    // Use whichever CSR direction the table physically stores; attribute bound/neighbour
    // to SRC/DST accordingly.
    auto* groupEntry = Catalog::Get(*context)
                           ->getTableCatalogEntry(transaction, pairTable->getRelGroupID())
                           ->ptrCast<RelGroupCatalogEntry>();
    const auto& storedDirections = groupEntry->getRelDataDirections();
    const auto scanDirection = std::find(storedDirections.begin(), storedDirections.end(),
                                   RelDataDirection::FWD) != storedDirections.end() ?
                                   RelDataDirection::FWD :
                                   RelDataDirection::BWD;
    const auto boundIsSrc = scanDirection == RelDataDirection::FWD;
    const auto boundTableID =
        boundIsSrc ? pairTable->getFromNodeTableID() : pairTable->getToNodeTableID();
    auto* boundNodeTable = storageManager->getTable(boundTableID)->ptrCast<NodeTable>();
    // Scan columns cover DST (neighbour IDs) and plain properties; SRC comes from the
    // bound node of each single-parent batch.
    std::vector<column_id_t> scanColumnIDs;
    // scanIdx[j] = output position of parts[j], or -1 for SRC.
    std::vector<int64_t> scanIdx;
    for (auto& part : parts) {
        if (part.kind == ResolvedPart::Kind::SRC) {
            scanIdx.push_back(-1);
            continue;
        }
        scanIdx.push_back(scanColumnIDs.size());
        scanColumnIDs.push_back(part.scanColumnID);
    }
    std::vector<LogicalType> columnTypes;
    for (auto columnID : scanColumnIDs) {
        columnTypes.push_back(pairTable->getColumn(columnID, scanDirection)->getDataType().copy());
    }
    auto chunk = Table::constructDataChunk(mm, std::move(columnTypes));
    std::vector<ValueVector*> outVectors;
    for (auto& vec : chunk.valueVectors) {
        outVectors.push_back(vec.get());
    }
    auto boundState = std::make_shared<DataChunkState>();
    auto boundVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm, boundState);
    auto outState = chunk.state;
    auto scanState =
        std::make_unique<RelTableScanState>(*mm, boundVector.get(), outVectors, outState);
    scanState->setToTable(transaction, pairTable, scanColumnIDs, {}, scanDirection);
    // Multi-parent batches: the persistent CSR path packs children of several bound parents
    // into one output batch (served parents in the bound selection vector,
    // packedChildOffsets prefix-summing their rows); other paths keep one parent per batch.
    // Both shapes are handled below. Batching amortizes one initScanState per parent.
    scanState->packedMultiParentScan = true;
    const auto numBoundNodes = boundNodeTable->getNumTotalRows(transaction);
    auto nextOffset = 0u;
    // One output row's SRC endpoint is its batch parent; DST/properties come from the
    // scan's own output vectors. Emits the row's key unless a part is NULL.
    auto emitRowKey = [&](sel_t pos, offset_t parentOffset) {
        std::string key;
        for (auto j = 0u; j < parts.size(); j++) {
            if (parts[j].kind == ResolvedPart::Kind::SRC) {
                appendNodeIDBytes(key, parentOffset, boundTableID);
                continue;
            }
            auto* vec = outVectors[scanIdx[j]];
            if (vec->isNull(pos)) {
                return;
            }
            auto val = vec->getAsValue(pos);
            appendExactValue(key, *val);
        }
        onKey(std::move(key));
    };
    while (nextOffset < numBoundNodes) {
        if (*stopFlag) {
            return;
        }
        const auto batchSize =
            std::min<row_idx_t>(DEFAULT_VECTOR_CAPACITY, numBoundNodes - nextOffset);
        boundVector->state->setToUnflat();
        boundVector->state->getSelVectorUnsafe().setToUnfiltered(batchSize);
        for (auto i = 0u; i < batchSize; i++) {
            boundVector->setValue<nodeID_t>(i, nodeID_t{nextOffset + i, boundTableID});
        }
        nextOffset += batchSize;
        pairTable->initScanState(transaction, *scanState);
        while (pairTable->scan(transaction, *scanState)) {
            const auto& sel = scanState->outState->getSelVector();
            const auto& boundSel = boundVector->state->getSelVector();
            const auto boundSelSize = boundSel.getSelSize();
            if (boundSelSize > 1) {
                // Packed multi-parent batch: rows [packedChildOffsets[p],
                // packedChildOffsets[p+1]) belong to parent p.
                const auto& childOffsets = scanState->packedChildOffsets;
                DASSERT(childOffsets.size() == size_t(boundSelSize) + 1);
                for (auto p = 0u; p < static_cast<uint32_t>(boundSelSize); ++p) {
                    const auto parentOffset = boundVector->readNodeOffset(boundSel[p]);
                    for (auto r = childOffsets[p]; r < childOffsets[p + 1]; ++r) {
                        emitRowKey(sel[r], parentOffset);
                    }
                    if (*stopFlag) {
                        return;
                    }
                }
            } else {
                const auto parentOffset = boundVector->readNodeOffset(boundSel[0]);
                for (auto s = 0u; s < sel.getSelSize(); s++) {
                    emitRowKey(sel[s], parentOffset);
                }
                if (*stopFlag) {
                    return;
                }
            }
        }
        scanState->restoreBoundNodeBatch();
        scanState->releaseScanPin();
    }
}

void UniqueConstraintChecker::collectExistingKeys(main::ClientContext* context, Table* table,
    const std::vector<ResolvedPart>& parts, const std::function<void(std::string&&)>& onKey,
    bool* stopFlag) {
    if (parts.empty()) {
        return;
    }
    if (table->getTableType() == TableType::NODE) {
        auto transaction = transaction::Transaction::Get(*context);
        auto mm = MemoryManager::Get(*context);
        std::vector<column_id_t> columnIDs;
        for (auto& part : parts) {
            DASSERT(part.kind == ResolvedPart::Kind::PROPERTY);
            columnIDs.push_back(part.scanColumnID);
        }
        scanCommittedAndLocalNodeKeys(transaction, table->ptrCast<NodeTable>(), columnIDs, mm,
            onKey, stopFlag);
        return;
    }
    scanRelPairKeys(context, table->ptrCast<RelTable>(), parts, onKey, stopFlag);
}

void UniqueConstraintChecker::validateNoDuplicates(main::ClientContext* context, Table* table,
    const std::vector<ResolvedPart>& parts, const std::string& constraintName) {
    std::unordered_set<std::string> seen;
    bool failed = false;
    collectExistingKeys(
        context, table, parts,
        [&](std::string&& key) {
            if (failed) {
                return;
            }
            if (!seen.insert(key).second) {
                failed = true;
            }
        },
        &failed);
    if (failed) {
        throwDuplicateError(constraintName);
    }
}

bool UniqueConstraintChecker::hasDuplicate(main::ClientContext* context, Table* table,
    const std::vector<ResolvedPart>& parts, const std::vector<ValueVector*>& keyVectors) {
    if (context == nullptr || parts.empty() || keyVectors.size() != parts.size()) {
        return false;
    }
    auto key = buildKey(parts, keyVectors);
    if (!key.has_value()) {
        return false;
    }
    bool found = false;
    collectExistingKeys(
        context, table, parts,
        [&](std::string&& existing) {
            if (!found && existing == *key) {
                found = true;
            }
        },
        &found);
    return found;
}

void UniqueConstraintChecker::validateRelGroupNoDuplicates(main::ClientContext* context,
    TableCatalogEntry* groupEntry, const std::vector<ResolvedPart>& parts,
    const std::string& constraintName) {
    auto storageManager = StorageManager::Get(*context);
    auto& relGroupEntry = groupEntry->constCast<RelGroupCatalogEntry>();
    std::unordered_set<std::string> seen;
    for (auto& pairInfo : relGroupEntry.getRelEntryInfos()) {
        auto* pairTable = storageManager->getTable(pairInfo.oid);
        bool failed = false;
        UniqueConstraintChecker::collectExistingKeys(
            context, pairTable, parts,
            [&](std::string&& key) {
                if (!failed && !seen.insert(key).second) {
                    failed = true;
                }
            },
            &failed);
        if (failed) {
            throwDuplicateError(constraintName);
        }
    }
}

void UniqueConstraintChecker::validateTableConstraints(main::ClientContext* context, Table* table) {
    auto transaction = transaction::Transaction::Get(*context);
    auto catalog = Catalog::Get(*context);
    if (table->getTableType() == TableType::NODE) {
        const auto tableID = table->getTableID();
        auto constraints = catalog->getConstraintEntries(transaction, tableID);
        if (constraints.empty()) {
            return;
        }
        auto* tableEntry = catalog->getTableCatalogEntry(transaction, tableID);
        for (auto* constraint : constraints) {
            validateNoDuplicates(context, table, resolveParts(tableEntry, constraint),
                constraint->getConstraintName());
        }
        return;
    }
    auto* relTable = table->ptrCast<RelTable>();
    const auto groupID = relTable->getRelGroupID();
    auto constraints = catalog->getConstraintEntries(transaction, groupID);
    if (constraints.empty()) {
        return;
    }
    auto* groupEntry = catalog->getTableCatalogEntry(transaction, groupID);
    for (auto* constraint : constraints) {
        validateRelGroupNoDuplicates(context, groupEntry, resolveParts(groupEntry, constraint),
            constraint->getConstraintName());
    }
}

void UniqueConstraintChecker::checkInsert(main::ClientContext* context, Table* table,
    const std::vector<ValueVector*>& propertyVectors, const ValueVector* srcNodeIDVector,
    const ValueVector* dstNodeIDVector) {
    if (context == nullptr) {
        return;
    }
    auto transaction = transaction::Transaction::Get(*context);
    if (transaction == nullptr || transaction->isRecovery()) {
        return;
    }
    auto catalog = Catalog::Get(*context);
    if (table->getTableType() == TableType::NODE) {
        const auto tableID = table->getTableID();
        auto constraints = catalog->getConstraintEntries(transaction, tableID);
        if (constraints.empty()) {
            return;
        }
        auto* tableEntry = catalog->getTableCatalogEntry(transaction, tableID);
        for (auto* constraint : constraints) {
            auto parts = resolveParts(tableEntry, constraint);
            std::vector<ValueVector*> keyVectors;
            for (auto& part : parts) {
                if (part.kind != ResolvedPart::Kind::PROPERTY ||
                    part.propertyPosition >= propertyVectors.size()) {
                    throw RuntimeException(std::format(
                        "Unique constraint {} internal error: insert state has {} vectors.",
                        constraint->getConstraintName(), propertyVectors.size()));
                }
                keyVectors.push_back(propertyVectors[part.propertyPosition]);
            }
            if (hasDuplicate(context, table, parts, keyVectors)) {
                throwDuplicateError(constraint->getConstraintName());
            }
        }
        return;
    }
    auto* relTable = table->ptrCast<RelTable>();
    const auto groupID = relTable->getRelGroupID();
    auto constraints = catalog->getConstraintEntries(transaction, groupID);
    if (constraints.empty()) {
        return;
    }
    auto* groupEntry = catalog->getTableCatalogEntry(transaction, groupID);
    auto storageManager = StorageManager::Get(*context);
    for (auto* constraint : constraints) {
        auto parts = resolveParts(groupEntry, constraint);
        std::vector<ValueVector*> keyVectors;
        for (auto& part : parts) {
            if (part.kind == ResolvedPart::Kind::SRC) {
                keyVectors.push_back(const_cast<ValueVector*>(srcNodeIDVector));
            } else if (part.kind == ResolvedPart::Kind::DST) {
                keyVectors.push_back(const_cast<ValueVector*>(dstNodeIDVector));
            } else {
                if (part.propertyPosition >= propertyVectors.size()) {
                    throw RuntimeException(std::format(
                        "Unique constraint {} internal error: insert state has {} vectors.",
                        constraint->getConstraintName(), propertyVectors.size()));
                }
                keyVectors.push_back(propertyVectors[part.propertyPosition]);
            }
        }
        // A null endpoint means the row is not inserted at all (see LocalRelTable::insert),
        // so there is nothing to check.
        if (std::any_of(keyVectors.begin(), keyVectors.end(),
                [](ValueVector* vec) { return vec == nullptr; })) {
            continue;
        }
        // The constraint spans the whole rel group: check every (src, dst) pair table.
        auto& relGroupEntry = groupEntry->constCast<RelGroupCatalogEntry>();
        for (auto& pairInfo : relGroupEntry.getRelEntryInfos()) {
            auto* pairTable = storageManager->getTable(pairInfo.oid);
            if (hasDuplicate(context, pairTable, parts, keyVectors)) {
                throwDuplicateError(constraint->getConstraintName());
            }
        }
    }
}

} // namespace storage
} // namespace lbug
