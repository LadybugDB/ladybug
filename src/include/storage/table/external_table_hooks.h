#pragma once

#include <functional>
#include <memory>
#include <string>
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

// Hooks implemented by an extension that provides columnar table storage for an
// external StorageFormat (e.g. the lance extension for the "lance" format).
//
// Core never names the extension's formats or table types. Instead, the extension
// registers these hooks under its format name(s), the StorageManager creates
// extension tables through the factories, and the scan operators create scan states
// / count morsels through these hooks. Tables created this way are expected to
// extend ColumnarNodeTableBase / ColumnarRelTableBase so the shared columnar scan
// machinery (morsel assignment, semi-masks, degree summaries) keeps working without
// further per-format branches in core.
//
// This surface is columnar-only: a non-columnar backend needs a separate hook
// surface (row-based morsel assignment and degree summaries are not covered here).
struct ColumnarTableHooks {
    // Canonical format name this registration serves (lowercased, e.g. "lance").
    // Used as the default registry key when canHandle is unset.
    std::string formatName;
    // Optional matcher consulted on lookup when the name does not match exactly.
    // Lets one extension serve several names (e.g. versioned aliases) without one
    // registration per name. Receives the lowercased requested name.
    std::function<bool(const std::string& requestedName)> canHandle = nullptr;

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
    // Whether the state returned by createRelScanState can serve as the shared scan
    // state of a multi-rel scan that also covers native, Arrow, and icebug-disk rel
    // tables. The shared state must serve every table in the scan, so a backend that
    // sets this must return a state extending ColumnarRelTableScanState. A backend
    // that cannot cover the in-core columnar tables must leave this false; mixing its
    // tables with Arrow/icebug-disk tables in one multi-rel scan is then rejected with
    // a clear error instead of hitting an invalid downcast.
    bool supportsMixedColumnarScan = false;

    // Deprecated predecessor of supportsMixedColumnarScan: the coverage rank for shared
    // multi-rel scan-state selection (native 0, Arrow 1, icebug-disk 2, extension >= 2
    // covering everything below). Honored on registration (rank >= 2 implies mixed-scan
    // support) until extensions migrate to the bool.
    // TODO: remove after the lance extension migrates to supportsMixedColumnarScan.
    int scanStateRank = 0;
};

// Transitional alias for the pre-rename ExternalTableHooks name.
// TODO: remove after extensions migrate to ColumnarTableHooks.
using ExternalTableHooks = ColumnarTableHooks;

// Global registry for ColumnarTableHooks. Format-level hooks are registered by
// extensions at load time under their format name; per-table bindings are installed
// by the StorageManager when it creates an extension-backed table and are keyed by
// (StorageManager*, tableID). Bindings hold a shared_ptr to the format hooks, so an
// extension unload cannot leave live tables with dangling hooks.
class LBUG_API ColumnarTableHookRegistry {
public:
    static void registerHooks(std::string formatName, ColumnarTableHooks hooks);
    static void unregisterHooks(const std::string& formatName);
    static std::shared_ptr<const ColumnarTableHooks> getFormatHooks(const std::string& formatName);

    // Looks up hooks for a cataloged external format. Returns nullptr when no
    // extension has registered the name.
    static std::shared_ptr<const ColumnarTableHooks> getFormatHooks(common::StorageFormat format,
        const std::string& formatName);

    static void bindTableHooks(const StorageManager* storageManager, common::table_id_t tableID,
        std::shared_ptr<const ColumnarTableHooks> hooks);
    static std::shared_ptr<const ColumnarTableHooks> getTableHooks(
        const StorageManager* storageManager, common::table_id_t tableID);
    static void unbindTableHooks(const StorageManager* storageManager, common::table_id_t tableID);
    static void unbindStorageManagerTables(const StorageManager* storageManager);

private:
    ColumnarTableHookRegistry() = delete;
};

// Transitional alias for the pre-rename registry name.
// TODO: remove after extensions migrate to ColumnarTableHookRegistry.
using ExternalTableHookRegistry = ColumnarTableHookRegistry;

} // namespace storage
} // namespace lbug
