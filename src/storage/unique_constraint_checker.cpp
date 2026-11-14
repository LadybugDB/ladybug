#include "storage/unique_constraint_checker.h"

#include <unordered_set>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/constraint_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/data_chunk/data_chunk.h"
#include "common/data_chunk/data_chunk_state.h"
#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/types/value/value.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/storage_manager.h"
#include "storage/table/node_group_collection.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace storage {

std::optional<std::string> UniqueConstraintChecker::buildKey(
    const std::vector<ValueVector*>& keyVectors) {
    DASSERT(!keyVectors.empty());
    std::string key;
    for (auto* vec : keyVectors) {
        DASSERT(vec->state->getSelVector().getSelSize() == 1);
        const auto p = vec->state->getSelVector()[0];
        if (vec->isNull(p)) {
            return std::nullopt;
        }
        auto val = vec->getAsValue(p);
        key.push_back(static_cast<char>(vec->dataType.getLogicalTypeID()));
        key += val->toString();
        key.push_back('\x1f');
    }
    return key;
}

static bool rowIsNull(const std::vector<ValueVector*>& vectors, sel_t pos) {
    for (auto* vec : vectors) {
        if (vec->isNull(pos)) {
            return true;
        }
    }
    return false;
}

static std::string buildKeyFromRow(const std::vector<ValueVector*>& vectors, sel_t pos) {
    std::string key;
    for (auto* vec : vectors) {
        auto val = vec->getAsValue(pos);
        key.push_back(static_cast<char>(vec->dataType.getLogicalTypeID()));
        key += val->toString();
        key.push_back('\x1f');
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
            for (auto i = 0u; i < res.numRows; i++) {
                if (rowIsNull(outVectors, i)) {
                    continue;
                }
                onKey(buildKeyFromRow(outVectors, i));
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
    const std::vector<column_id_t>& columnIDs, const std::function<void(std::string&&)>& onKey,
    bool* stopFlag) {
    auto transaction = transaction::Transaction::Get(*context);
    auto mm = MemoryManager::Get(*context);
    auto storageManager = StorageManager::Get(*context);
    auto* srcNodeTable =
        storageManager->getTable(pairTable->getFromNodeTableID())->ptrCast<NodeTable>();
    std::vector<LogicalType> columnTypes;
    for (auto columnID : columnIDs) {
        columnTypes.push_back(
            pairTable->getColumn(columnID, RelDataDirection::FWD)->getDataType().copy());
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
    scanState->setToTable(transaction, pairTable, columnIDs, {}, RelDataDirection::FWD);
    const auto numSrcNodes = srcNodeTable->getNumTotalRows(transaction);
    auto nextOffset = 0u;
    while (nextOffset < numSrcNodes) {
        if (*stopFlag) {
            return;
        }
        // One bound parent per batch: mirrors the common single-parent scan path and
        // avoids the packed multi-parent output layout.
        const auto batchSize = std::min<row_idx_t>(1, numSrcNodes - nextOffset);
        boundVector->state->setToUnflat();
        boundVector->state->getSelVectorUnsafe().setToUnfiltered(batchSize);
        for (auto i = 0u; i < batchSize; i++) {
            boundVector->setValue<nodeID_t>(i,
                nodeID_t{nextOffset + i, pairTable->getFromNodeTableID()});
        }
        nextOffset += batchSize;
        pairTable->initScanState(transaction, *scanState);
        while (pairTable->scan(transaction, *scanState)) {
            const auto outputSize = scanState->outState->getSelVector().getSelSize();
            for (auto i = 0u; i < outputSize; i++) {
                if (rowIsNull(outVectors, i)) {
                    continue;
                }
                onKey(buildKeyFromRow(outVectors, i));
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
    const std::vector<column_id_t>& columnIDs, const std::function<void(std::string&&)>& onKey) {
    if (columnIDs.empty()) {
        return;
    }
    if (table->getTableType() == TableType::NODE) {
        auto transaction = transaction::Transaction::Get(*context);
        auto mm = MemoryManager::Get(*context);
        bool stop = false;
        scanCommittedAndLocalNodeKeys(transaction, table->ptrCast<NodeTable>(), columnIDs, mm,
            onKey, &stop);
        return;
    }
    bool stop = false;
    scanRelPairKeys(context, table->ptrCast<RelTable>(), columnIDs, onKey, &stop);
}

static void throwDuplicateError(const std::string& constraintName) {
    throw RuntimeException(
        std::format("Unique constraint {} violated: duplicate key.", constraintName));
}

void UniqueConstraintChecker::validateNoDuplicates(main::ClientContext* context, Table* table,
    const std::vector<column_id_t>& columnIDs, const std::string& constraintName) {
    std::unordered_set<std::string> seen;
    bool failed = false;
    collectExistingKeys(context, table, columnIDs, [&](std::string&& key) {
        if (failed) {
            return;
        }
        if (!seen.insert(key).second) {
            failed = true;
        }
    });
    if (failed) {
        throwDuplicateError(constraintName);
    }
}

bool UniqueConstraintChecker::hasDuplicate(main::ClientContext* context, Table* table,
    const std::vector<column_id_t>& columnIDs, const std::vector<ValueVector*>& keyVectors) {
    if (context == nullptr || columnIDs.empty() || keyVectors.empty()) {
        return false;
    }
    auto key = buildKey(keyVectors);
    if (!key.has_value()) {
        return false;
    }
    bool found = false;
    collectExistingKeys(context, table, columnIDs, [&](std::string&& existing) {
        if (!found && existing == *key) {
            found = true;
        }
    });
    return found;
}

static std::vector<column_id_t> resolveColumnIDs(catalog::TableCatalogEntry* tableEntry,
    const std::vector<property_id_t>& propertyIDs) {
    std::vector<column_id_t> columnIDs;
    for (auto propertyID : propertyIDs) {
        columnIDs.push_back(tableEntry->getColumnID(tableEntry->getProperty(propertyID).getName()));
    }
    return columnIDs;
}

void UniqueConstraintChecker::validateRelGroupNoDuplicates(main::ClientContext* context,
    RelGroupCatalogEntry* groupEntry, const std::vector<column_id_t>& columnIDs,
    const std::string& constraintName) {
    auto storageManager = StorageManager::Get(*context);
    std::unordered_set<std::string> seen;
    for (auto& pairInfo : groupEntry->getRelEntryInfos()) {
        auto* pairTable = storageManager->getTable(pairInfo.oid);
        bool failed = false;
        UniqueConstraintChecker::collectExistingKeys(context, pairTable, columnIDs,
            [&](std::string&& key) {
                if (!failed && !seen.insert(key).second) {
                    failed = true;
                }
            });
        if (failed) {
            throw RuntimeException(
                std::format("Unique constraint {} violated: duplicate key.", constraintName));
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
            validateNoDuplicates(context, table,
                resolveColumnIDs(tableEntry, constraint->getPropertyIDs()),
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
    auto* groupEntry =
        catalog->getTableCatalogEntry(transaction, groupID)->ptrCast<RelGroupCatalogEntry>();
    for (auto* constraint : constraints) {
        validateRelGroupNoDuplicates(context, groupEntry,
            resolveColumnIDs(groupEntry, constraint->getPropertyIDs()),
            constraint->getConstraintName());
    }
}

void UniqueConstraintChecker::checkInsert(main::ClientContext* context, Table* table,
    const std::vector<ValueVector*>& propertyVectors) {
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
            std::vector<column_id_t> columnIDs;
            std::vector<ValueVector*> keyVectors;
            for (auto propertyID : constraint->getPropertyIDs()) {
                const auto columnID =
                    tableEntry->getColumnID(tableEntry->getProperty(propertyID).getName());
                columnIDs.push_back(columnID);
                DASSERT(columnID < propertyVectors.size());
                keyVectors.push_back(propertyVectors[columnID]);
            }
            if (hasDuplicate(context, table, columnIDs, keyVectors)) {
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
    auto& relGroupEntry = groupEntry->constCast<RelGroupCatalogEntry>();
    // The insert state's vectors align 1:1 with the group properties in order, including
    // the internal _ID property at index 0 (see bindInsertColumnDataExprs over
    // entry->getProperties() and LocalRelTable::insert).
    const auto groupProperties = relGroupEntry.getProperties();
    auto storageManager = StorageManager::Get(*context);
    for (auto* constraint : constraints) {
        std::vector<column_id_t> columnIDs;
        std::vector<ValueVector*> keyVectors;
        for (auto propertyID : constraint->getPropertyIDs()) {
            column_id_t propPos = INVALID_COLUMN_ID;
            for (auto i = 0u; i < groupProperties.size(); i++) {
                if (groupEntry->getPropertyID(groupProperties[i].getName()) == propertyID) {
                    propPos = i;
                    break;
                }
            }
            if (propPos == INVALID_COLUMN_ID || propPos >= propertyVectors.size()) {
                std::string groupProps;
                for (auto& gp : groupProperties) {
                    groupProps += gp.getName() + ",";
                }
                throw RuntimeException(
                    std::format("Unique constraint {} internal error: propertyID {} not mapped "
                                "(group props: {} vectors: {}).",
                        constraint->getConstraintName(), propertyID, groupProps,
                        propertyVectors.size()));
            }
            const auto columnID = groupEntry->getColumnID(groupProperties[propPos].getName());
            columnIDs.push_back(columnID);
            keyVectors.push_back(propertyVectors[propPos]);
        }
        // The constraint spans the whole rel group: check every (src, dst) pair table.
        for (auto& pairInfo : relGroupEntry.getRelEntryInfos()) {
            auto* pairTable = storageManager->getTable(pairInfo.oid);
            if (hasDuplicate(context, pairTable, columnIDs, keyVectors)) {
                throwDuplicateError(constraint->getConstraintName());
            }
        }
    }
}

} // namespace storage
} // namespace lbug
