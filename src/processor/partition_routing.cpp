#include "processor/partition_routing.h"

#include "binder/ddl/bound_create_table_info.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/types/date_t.h"
#include "common/types/int128_t.h"
#include "common/types/timestamp_t.h"
#include "common/types/types.h"
#include "function/hash/vector_hash_functions.h"
#include "main/client_context.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::common;
using namespace lbug::catalog;

namespace lbug {
namespace processor {

void computePartitionIndexes(const common::ValueVector& keyVector, uint64_t numPartitions,
    std::vector<uint64_t>& outIndexes) {
    DASSERT(numPartitions > 0);
    const auto& selVector = keyVector.state->getSelVector();
    const auto numTuples = selVector.getSelSize();
    outIndexes.resize(numTuples);

    auto hashVector = std::make_unique<common::ValueVector>(common::LogicalType::UINT64());
    hashVector->state = keyVector.state;
    function::VectorHashFunction::computeHash(keyVector, selVector, *hashVector, selVector);
    for (auto i = 0u; i < numTuples; ++i) {
        const auto pos = selVector[i];
        outIndexes[i] = hashVector->getValue<common::hash_t>(pos) % numPartitions;
    }
}

// ---- LIST partition key encoding ----
//
// The encoding is the value's stable physical representation: fixed-width types encode as their
// little-endian bytes, strings/blobs as a length prefix followed by the raw bytes. Two values are
// routed to the same partition iff they encode identically.

static void appendLE(std::string& out, const void* data, size_t len) {
    out.append(static_cast<const char*>(data), len);
}

static std::string encodeFixed(const void* data, size_t len) {
    std::string out;
    out.reserve(len);
    appendLE(out, data, len);
    return out;
}

std::string encodeListPartitionKey(const common::ValueVector& keyVector, uint32_t pos) {
    DASSERT(!keyVector.isNull(pos));
    std::string out;
    switch (keyVector.dataType.getLogicalTypeID()) {
    case LogicalTypeID::INT8: {
        return encodeFixed(&keyVector.getValue<int8_t>(pos), sizeof(int8_t));
    }
    case LogicalTypeID::INT16: {
        return encodeFixed(&keyVector.getValue<int16_t>(pos), sizeof(int16_t));
    }
    case LogicalTypeID::INT32:
    case LogicalTypeID::DATE: {
        // DATE's physical representation is a 32-bit day count.
        return encodeFixed(&keyVector.getValue<int32_t>(pos), sizeof(int32_t));
    }
    case LogicalTypeID::SERIAL:
    case LogicalTypeID::INT64:
    case LogicalTypeID::TIMESTAMP_SEC:
    case LogicalTypeID::TIMESTAMP_MS:
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_TZ:
    case LogicalTypeID::TIMESTAMP_NS: {
        // All timestamp flavors store an int64 tick count; the flavor is constant per column, so
        // the raw ticks are a stable key.
        return encodeFixed(&keyVector.getValue<int64_t>(pos), sizeof(int64_t));
    }
    case LogicalTypeID::FLOAT: {
        auto value = keyVector.getValue<float>(pos);
        if (value == 0.0f) {
            value = 0.0f; // normalize -0.0
        }
        return encodeFixed(&value, sizeof(float));
    }
    case LogicalTypeID::DOUBLE: {
        auto value = keyVector.getValue<double>(pos);
        if (value == 0.0) {
            value = 0.0; // normalize -0.0
        }
        return encodeFixed(&value, sizeof(double));
    }
    case LogicalTypeID::INT128: {
        return encodeFixed(&keyVector.getValue<int128_t>(pos), sizeof(int128_t));
    }
    case LogicalTypeID::UINT8: {
        return encodeFixed(&keyVector.getValue<uint8_t>(pos), sizeof(uint8_t));
    }
    case LogicalTypeID::UINT16: {
        return encodeFixed(&keyVector.getValue<uint16_t>(pos), sizeof(uint16_t));
    }
    case LogicalTypeID::UINT32: {
        return encodeFixed(&keyVector.getValue<uint32_t>(pos), sizeof(uint32_t));
    }
    case LogicalTypeID::UINT64: {
        return encodeFixed(&keyVector.getValue<uint64_t>(pos), sizeof(uint64_t));
    }
    case LogicalTypeID::UINT128: {
        return encodeFixed(&keyVector.getValue<uint128_t>(pos), sizeof(uint128_t));
    }
    case LogicalTypeID::STRING:
    case LogicalTypeID::BLOB: {
        const auto& s = keyVector.getValue<string_t>(pos);
        const auto len = static_cast<uint32_t>(s.len);
        out.append(reinterpret_cast<const char*>(&len), sizeof(len));
        out.append(reinterpret_cast<const char*>(s.getData()), s.len);
        return out;
    }
    default: {
        // The binder only admits the types above for LIST partitioning.
        UNREACHABLE_CODE;
    }
    }
}

// ---- ListPartitionRouter ----

ListPartitionRouter::ListPartitionRouter(main::ClientContext* context_,
    catalog::NodeTableCatalogEntry* parent_)
    : context{context_}, parent{parent_} {}

ListPartitionRouter::Route ListPartitionRouter::route(const common::ValueVector& keyVector,
    uint32_t pos) {
    return getOrCreatePartition(encodeListPartitionKey(keyVector, pos));
}

ListPartitionRouter::Route ListPartitionRouter::getOrCreatePartition(
    const std::string& encodedKey) {
    std::lock_guard lck{mtx};
    return getOrCreatePartitionLocked(encodedKey);
}

ListPartitionRouter::Route ListPartitionRouter::getOrCreatePartitionLocked(
    const std::string& encodedKey) {
    if (auto it = routesByKey.find(encodedKey); it != routesByKey.end()) {
        return it->second;
    }
    // A partition for this key may already exist (created before this router was constructed, or
    // by a previous query after reopen).
    auto* catalog = Catalog::Get(*context);
    auto* transaction = transaction::Transaction::Get(*context);
    const auto& existingKeys = parent->getListPartitionKeys();
    for (auto i = 0u; i < existingKeys.size(); i++) {
        if (existingKeys[i].first == encodedKey) {
            auto* childEntry = catalog->getTableCatalogEntry(transaction, existingKeys[i].second);
            auto* table = storage::PartitionStorageRegistry::resolveNodeTable(context, *childEntry);
            Route route{table, i};
            routesByKey.emplace(encodedKey, route);
            return route;
        }
    }
    // Create the partition: an ordinary node table with the parent's schema, via the same
    // catalog + storage machinery as CREATE NODE TABLE.
    const auto ordinal = parent->getChildTableIDs().size();
    auto childName = std::format("{}_p{}", parent->getName(), ordinal);
    std::vector<binder::PropertyDefinition> propertyDefinitions;
    propertyDefinitions.reserve(parent->getProperties().size());
    for (const auto& definition : parent->getProperties()) {
        propertyDefinitions.push_back(definition.copy());
    }
    auto extraInfo =
        std::make_unique<binder::BoundExtraCreateNodeTableInfo>(parent->getPrimaryKeyName(),
            std::move(propertyDefinitions), parent->getStorage(), parent->getStorageFormat());
    // Register the parent link so the child is a proper partition subgraph, including across
    // WAL replay (the create record rebuilds via this extra info).
    extraInfo->setPartitionParent(parent->getTableID(), ordinal);
    auto childInfo = binder::BoundCreateTableInfo(CatalogEntryType::NODE_TABLE_ENTRY, childName,
        ConflictAction::ON_CONFLICT_THROW, std::move(extraInfo), false /* isInternal */);
    auto* childEntry =
        catalog->createTableEntry(transaction, childInfo)->ptrCast<TableCatalogEntry>();
    const auto tableID = childEntry->getTableID();
    // Own data file per partition (phase-B); see docs/partitioning.md 6b.
    storage::PartitionStorageRegistry::Get(context)
        ->getOrCreate(context, tableID, childName)
        .createTable(childEntry, context);
    parent->addChildTableID(tableID);
    parent->addListPartition(encodedKey, tableID);
    Route route{storage::PartitionStorageRegistry::Get(context)
                    ->getOrCreate(context, tableID, childName)
                    .getTable(tableID)
                    ->ptrCast<storage::NodeTable>(),
        ordinal};
    routesByKey.emplace(encodedKey, route);
    return route;
}

} // namespace processor
} // namespace lbug
