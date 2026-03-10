#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void RelDeletionRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.writeDebuggingInfo("table_id");
    serializer.write<table_id_t>(tableID);
    serializer.writeDebuggingInfo("src_node_vector");
    srcNodeIDVector->serialize(serializer);
    serializer.writeDebuggingInfo("dst_node_vector");
    dstNodeIDVector->serialize(serializer);
    serializer.writeDebuggingInfo("rel_id_vector");
    relIDVector->serialize(serializer);
}

std::unique_ptr<RelDeletionRecord> RelDeletionRecord::deserialize(Deserializer& deserializer,
    const main::ClientContext& clientContext) {
    std::string key;
    table_id_t tableID = INVALID_TABLE_ID;

    deserializer.validateDebuggingInfo(key, "table_id");
    deserializer.deserializeValue<table_id_t>(tableID);
    deserializer.validateDebuggingInfo(key, "src_node_vector");
    auto resultChunkState = std::make_shared<DataChunkState>();
    auto srcNodeIDVector =
        ValueVector::deSerialize(deserializer, MemoryManager::Get(clientContext), resultChunkState);
    deserializer.validateDebuggingInfo(key, "dst_node_vector");
    auto dstNodeIDVector =
        ValueVector::deSerialize(deserializer, MemoryManager::Get(clientContext), resultChunkState);
    deserializer.validateDebuggingInfo(key, "rel_id_vector");
    auto relIDVector =
        ValueVector::deSerialize(deserializer, MemoryManager::Get(clientContext), resultChunkState);
    return std::make_unique<RelDeletionRecord>(tableID, std::move(srcNodeIDVector),
        std::move(dstNodeIDVector), std::move(relIDVector));
}

} // namespace storage
} // namespace lbug
