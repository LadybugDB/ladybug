#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "common/api.h"
#include "common/enums/storage_format.h"
#include "common/types/types.h"

namespace lbug {
namespace catalog {
class NodeTableCatalogEntry;
class RelGroupCatalogEntry;
} // namespace catalog
namespace common {
class DataChunkState;
class ValueVector;
} // namespace common
namespace main {
class ClientContext;
} // namespace main
namespace transaction {
class Transaction;
} // namespace transaction
namespace storage {
class MemoryManager;
class NodeTable;
class RelTable;
struct RelTableScanState;
class StorageManager;
class Table;
struct TableScanState;

// Hooks implemented by an extension that provides table storage for a
// non-native StorageFormat (e.g. the lance extension for StorageFormat::LANCE).
//
// Core never names the extension's table types. Instead, the StorageManager
// creates extension tables through these factories, and the scan operators
// create scan states / count morsels through these hooks. Tables created this
// way are expected to extend ColumnarNodeTableBase / ColumnarRelTableBase so
// the shared columnar scan machinery (morsel assignment, semi-masks, degree
// summaries) keeps working without further per-format branches in core.
struct ExternalTableHooks {
    // StorageManager table factories.
    std::function<std::unique_ptr<Table>(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
        main::ClientContext* context)>
        createNodeTable = nullptr;
    std::function<std::unique_ptr<Table>(catalog::RelGroupCatalogEntry* relGroupEntry,
        common::table_id_t fromTableID, common::table_id_t toTableID,
        const StorageManager* storageManager, MemoryManager* memoryManager,
        main::ClientContext* context)>
        createRelTable = nullptr;

    // ScanNodeTable support.
    std::function<std::unique_ptr<TableScanState>(NodeTable* table,
        common::ValueVector* nodeIDVector, const std::vector<common::ValueVector*>& outVectors,
        std::shared_ptr<common::DataChunkState> outChunkState, MemoryManager* memoryManager)>
        createNodeScanState = nullptr;
    std::function<common::node_group_idx_t(const transaction::Transaction* transaction,
        NodeTable* table)>
        getNumScanMorsels = nullptr;

    // ScanRelTable / ScanMultiRelTable support.
    std::function<std::unique_ptr<RelTableScanState>(RelTable* table,
        common::ValueVector* boundNodeIDVector, const std::vector<common::ValueVector*>& outVectors,
        std::shared_ptr<common::DataChunkState> outChunkState, MemoryManager* memoryManager)>
        createRelScanState = nullptr;
    // Coverage rank for shared multi-rel scan-state selection. The shared state
    // must serve every table in the scan, so the table with the highest rank
    // creates it. Native tables have rank 0, Arrow rank 1, icebug-disk rank 2;
    // an external backend must pick a rank whose scan state is a subclass of
    // every lower-rank state it can be mixed with (e.g. lance uses rank 3 with
    // a state that extends IceDiskRelTableScanState).
    int scanStateRank = 0;
};

// Global registry for ExternalTableHooks. Format-level hooks are registered by
// extensions at load time; per-table bindings are installed by the
// StorageManager when it creates an extension-backed table and are keyed by
// (StorageManager*, tableID). Bindings hold a shared_ptr to the format hooks,
// so an extension unload cannot leave live tables with dangling hooks.
class LBUG_API ExternalTableHookRegistry {
public:
    static void registerHooks(common::StorageFormat format, ExternalTableHooks hooks);
    static void unregisterHooks(common::StorageFormat format);
    static std::shared_ptr<const ExternalTableHooks> getFormatHooks(common::StorageFormat format);

    static void bindTableHooks(const StorageManager* storageManager, common::table_id_t tableID,
        std::shared_ptr<const ExternalTableHooks> hooks);
    static std::shared_ptr<const ExternalTableHooks> getTableHooks(
        const StorageManager* storageManager, common::table_id_t tableID);
    static void unbindTableHooks(const StorageManager* storageManager, common::table_id_t tableID);
    static void unbindStorageManagerTables(const StorageManager* storageManager);

private:
    ExternalTableHookRegistry() = delete;
};

} // namespace storage
} // namespace lbug
