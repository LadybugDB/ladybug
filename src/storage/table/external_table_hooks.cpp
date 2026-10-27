#include "storage/table/external_table_hooks.h"

#include <map>
#include <mutex>

namespace lbug {
namespace storage {

namespace {

struct RegistryState {
    std::mutex mtx;
    std::map<common::StorageFormat, std::shared_ptr<const ExternalTableHooks>> formatHooks;
    std::map<std::pair<const StorageManager*, common::table_id_t>,
        std::shared_ptr<const ExternalTableHooks>>
        tableHooks;
};

RegistryState& getRegistryState() {
    static RegistryState state;
    return state;
}

} // namespace

void ExternalTableHookRegistry::registerHooks(common::StorageFormat format,
    ExternalTableHooks hooks) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.formatHooks[format] = std::make_shared<const ExternalTableHooks>(std::move(hooks));
}

void ExternalTableHookRegistry::unregisterHooks(common::StorageFormat format) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.formatHooks.erase(format);
}

std::shared_ptr<const ExternalTableHooks> ExternalTableHookRegistry::getFormatHooks(
    common::StorageFormat format) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    auto it = state.formatHooks.find(format);
    if (it == state.formatHooks.end()) {
        return nullptr;
    }
    return it->second;
}

void ExternalTableHookRegistry::bindTableHooks(const StorageManager* storageManager,
    common::table_id_t tableID, std::shared_ptr<const ExternalTableHooks> hooks) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.tableHooks[{storageManager, tableID}] = std::move(hooks);
}

std::shared_ptr<const ExternalTableHooks> ExternalTableHookRegistry::getTableHooks(
    const StorageManager* storageManager, common::table_id_t tableID) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    auto it = state.tableHooks.find({storageManager, tableID});
    if (it == state.tableHooks.end()) {
        return nullptr;
    }
    return it->second;
}

void ExternalTableHookRegistry::unbindTableHooks(const StorageManager* storageManager,
    common::table_id_t tableID) {
    auto& state = getRegistryState();
    std::lock_guard lock{state.mtx};
    state.tableHooks.erase({storageManager, tableID});
}

void ExternalTableHookRegistry::unbindStorageManagerTables(const StorageManager* storageManager) {
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
