#include "storage/table/external_table_hooks.h"

#include <map>
#include <mutex>

#include "common/string_utils.h"

namespace lbug {
namespace storage {

namespace {

struct RegistryState {
    std::mutex mtx;
    std::map<std::string, std::shared_ptr<const ColumnarTableHooks>> formatHooks;
    std::map<std::pair<const StorageManager*, common::table_id_t>,
        std::shared_ptr<const ColumnarTableHooks>>
        tableHooks;
};

RegistryState& getRegistryState() {
    static RegistryState state;
    return state;
}

std::string normalizeKey(const std::string& name) {
    return common::StringUtils::getLower(name);
}

std::shared_ptr<const ColumnarTableHooks> findHooksLocked(RegistryState& state,
    const std::string& normalizedName) {
    auto it = state.formatHooks.find(normalizedName);
    if (it != state.formatHooks.end()) {
        return it->second;
    }
    for (const auto& [_, hooks] : state.formatHooks) {
        if (hooks->canHandle && hooks->canHandle(normalizedName)) {
            return hooks;
        }
    }
    return nullptr;
}

} // namespace

void ColumnarTableHookRegistry::registerHooks(std::string formatName, ColumnarTableHooks hooks) {
    if (hooks.scanStateRank >= 2) {
        hooks.supportsMixedColumnarScan = true;
    }
    if (hooks.formatName.empty()) {
        hooks.formatName = normalizeKey(formatName);
    }
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.formatHooks[normalizeKey(formatName)] =
        std::make_shared<const ColumnarTableHooks>(std::move(hooks));
}

void ColumnarTableHookRegistry::unregisterHooks(const std::string& formatName) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.formatHooks.erase(normalizeKey(formatName));
}

std::shared_ptr<const ColumnarTableHooks> ColumnarTableHookRegistry::getFormatHooks(
    const std::string& formatName) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    return findHooksLocked(state, normalizeKey(formatName));
}

std::shared_ptr<const ColumnarTableHooks> ColumnarTableHookRegistry::getFormatHooks(
    common::StorageFormat format, const std::string& formatName) {
    if (format != common::StorageFormat::EXTERNAL) {
        return nullptr;
    }
    return getFormatHooks(formatName);
}

void ColumnarTableHookRegistry::bindTableHooks(const StorageManager* storageManager,
    common::table_id_t tableID, std::shared_ptr<const ColumnarTableHooks> hooks) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.tableHooks[{storageManager, tableID}] = std::move(hooks);
}

std::shared_ptr<const ColumnarTableHooks> ColumnarTableHookRegistry::getTableHooks(
    const StorageManager* storageManager, common::table_id_t tableID) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    auto it = state.tableHooks.find({storageManager, tableID});
    if (it == state.tableHooks.end()) {
        return nullptr;
    }
    return it->second;
}

void ColumnarTableHookRegistry::unbindTableHooks(const StorageManager* storageManager,
    common::table_id_t tableID) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.tableHooks.erase({storageManager, tableID});
}

void ColumnarTableHookRegistry::unbindStorageManagerTables(const StorageManager* storageManager) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    for (auto it = state.tableHooks.begin(); it != state.tableHooks.end();) {
        if (it->first.first == storageManager) {
            it = state.tableHooks.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace storage
} // namespace lbug
