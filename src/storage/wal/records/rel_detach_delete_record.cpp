#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void RelDetachDeleteRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.writeDebuggingInfo("table_id");
    serializer.write<table_id_t>(tableID);
    serializer.writeDebuggingInfo("direction");
    serializer.write<RelDataDirection>(direction);
    serializer.writeDebuggingInfo("src_node_vector");
    srcNodeIDVector->serialize(serializer);
}

std::unique_ptr<RelDetachDeleteRecord> RelDetachDeleteRecord::deserialize(
    Deserializer& deserializer, const main::ClientContext& clientContext) {
    std::string key;
    table_id_t tableID = INVALID_TABLE_ID;
    auto direction = RelDataDirection::INVALID;

    deserializer.validateDebuggingInfo(key, "table_id");
    deserializer.deserializeValue<table_id_t>(tableID);
    deserializer.validateDebuggingInfo(key, "direction");
    deserializer.deserializeValue<RelDataDirection>(direction);
    deserializer.validateDebuggingInfo(key, "src_node_vector");
    auto resultChunkState = std::make_shared<DataChunkState>();
    auto srcNodeIDVector =
        ValueVector::deSerialize(deserializer, MemoryManager::Get(clientContext), resultChunkState);
    return std::make_unique<RelDetachDeleteRecord>(tableID, direction, std::move(srcNodeIDVector));
}

} // namespace storage
} // namespace lbug
