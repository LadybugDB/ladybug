#pragma once

#include <utility>

#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct NodeUpdateRecord final : WALRecord {
    common::table_id_t tableID;
    common::column_id_t columnID;
    common::offset_t nodeOffset;
    common::ValueVector* propertyVector;
    std::unique_ptr<common::ValueVector> ownedPropertyVector;

    NodeUpdateRecord()
        : WALRecord{WALRecordType::NODE_UPDATE_RECORD}, tableID{common::INVALID_TABLE_ID},
          columnID{common::INVALID_COLUMN_ID}, nodeOffset{common::INVALID_OFFSET},
          propertyVector{nullptr} {}
    NodeUpdateRecord(common::table_id_t tableID, common::column_id_t columnID,
        common::offset_t nodeOffset, common::ValueVector* propertyVector)
        : WALRecord{WALRecordType::NODE_UPDATE_RECORD}, tableID{tableID}, columnID{columnID},
          nodeOffset{nodeOffset}, propertyVector{propertyVector} {}
    NodeUpdateRecord(common::table_id_t tableID, common::column_id_t columnID,
        common::offset_t nodeOffset, std::unique_ptr<common::ValueVector> propertyVector)
        : WALRecord{WALRecordType::NODE_UPDATE_RECORD}, tableID{tableID}, columnID{columnID},
          nodeOffset{nodeOffset}, propertyVector{nullptr},
          ownedPropertyVector{std::move(propertyVector)} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<NodeUpdateRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);
};

} // namespace storage
} // namespace lbug
