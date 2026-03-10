#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void NodeUpdateRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.writeDebuggingInfo("table_id");
    serializer.write<table_id_t>(tableID);
    serializer.writeDebuggingInfo("column_id");
    serializer.write<column_id_t>(columnID);
    serializer.writeDebuggingInfo("node_offset");
    serializer.write<offset_t>(nodeOffset);
    serializer.writeDebuggingInfo("property_vector");
    propertyVector->serialize(serializer);
}

std::unique_ptr<NodeUpdateRecord> NodeUpdateRecord::deserialize(Deserializer& deserializer,
    const main::ClientContext& clientContext) {
    std::string key;
    table_id_t tableID = INVALID_TABLE_ID;
    column_id_t columnID = INVALID_COLUMN_ID;
    offset_t nodeOffset = INVALID_OFFSET;

    deserializer.validateDebuggingInfo(key, "table_id");
    deserializer.deserializeValue<table_id_t>(tableID);
    deserializer.validateDebuggingInfo(key, "column_id");
    deserializer.deserializeValue<column_id_t>(columnID);
    deserializer.validateDebuggingInfo(key, "node_offset");
    deserializer.deserializeValue<offset_t>(nodeOffset);
    deserializer.validateDebuggingInfo(key, "property_vector");
    auto resultChunkState = std::make_shared<DataChunkState>();
    auto ownedVector =
        ValueVector::deSerialize(deserializer, MemoryManager::Get(clientContext), resultChunkState);
    return std::make_unique<NodeUpdateRecord>(tableID, columnID, nodeOffset,
        std::move(ownedVector));
}

} // namespace storage
} // namespace lbug
