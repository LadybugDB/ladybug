#pragma once

#include <unordered_map>

#include "common/copy_constructors.h"
#include "storage/local_storage/local_table.h"
#include "storage/optimistic_allocator.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main
namespace storage {
// Data structures in LocalStorage are not thread-safe.
// For now, we only support single thread insertions and updates. Once we optimize them with
// multiple threads, LocalStorage and its related data structures should be reworked to be
// thread-safe.
class LocalStorage {
public:
    explicit LocalStorage(main::ClientContext& clientContext) : clientContext{clientContext} {}
    DELETE_COPY_AND_MOVE(LocalStorage);

    // Do nothing if the table already exists, otherwise create a new local table.
    LocalTable* getOrCreateLocalTable(Table& table);
    // Return nullptr if no local table exists.
    LocalTable* getLocalTable(common::table_id_t tableID) const;

    // Optimistic page allocation is scoped to one storage manager (each partition child has
    // its own data file and page manager). `sm == nullptr` selects the main database file.
    // Allocators are cached per StorageManager so repeated calls for the same file (e.g. one
    // call per batch-insert target) share a single allocator, and commit/rollback covers each
    // file exactly once.
    PageAllocator* addOptimisticAllocator(StorageManager* sm = nullptr);

    void commit();
    void rollback();

private:
    main::ClientContext& clientContext;
    std::unordered_map<common::table_id_t, std::unique_ptr<LocalTable>> tables;

    // The mutex is only needed when working with the optimistic allocators
    std::mutex mtx;
    std::vector<std::unique_ptr<OptimisticAllocator>> optimisticAllocators;
    std::unordered_map<StorageManager*, OptimisticAllocator*> allocatorsByStorageManager;
};

} // namespace storage
} // namespace lbug
