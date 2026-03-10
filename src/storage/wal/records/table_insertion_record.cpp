#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void TableInsertionRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.writeDebuggingInfo("table_id");
    serializer.write<table_id_t>(tableID);
    serializer.writeDebuggingInfo("table_type");
    serializer.write<TableType>(tableType);
    serializer.writeDebuggingInfo("num_rows");
    serializer.write<row_idx_t>(numRows);
    serializer.writeDebuggingInfo("num_vectors");
    serializer.write<idx_t>(vectors.size());
    for (auto& vector : vectors) {
        vector->serialize(serializer);
    }
}

std::unique_ptr<TableInsertionRecord> TableInsertionRecord::deserialize(Deserializer& deserializer,
    const main::ClientContext& clientContext) {
    std::string key;
    table_id_t tableID = INVALID_TABLE_ID;
    auto tableType = TableType::UNKNOWN;
    row_idx_t numRows = INVALID_ROW_IDX;
    idx_t numVectors = 0;
    std::vector<std::unique_ptr<ValueVector>> valueVectors;
    deserializer.validateDebuggingInfo(key, "table_id");
    deserializer.deserializeValue<table_id_t>(tableID);
    deserializer.validateDebuggingInfo(key, "table_type");
    deserializer.deserializeValue<TableType>(tableType);
    deserializer.validateDebuggingInfo(key, "num_rows");
    deserializer.deserializeValue<row_idx_t>(numRows);
    deserializer.validateDebuggingInfo(key, "num_vectors");
    deserializer.deserializeValue(numVectors);
    auto resultChunkState = DataChunkState::getSingleValueDataChunkState();
    valueVectors.reserve(numVectors);
    for (auto i = 0u; i < numVectors; i++) {
        valueVectors.push_back(ValueVector::deSerialize(deserializer,
            MemoryManager::Get(clientContext), resultChunkState));
    }
    return std::make_unique<TableInsertionRecord>(tableID, tableType, numRows,
        std::move(valueVectors));
}

} // namespace storage
} // namespace lbug
