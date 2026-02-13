#pragma once

#include <utility>

#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct RelUpdateRecord final : WALRecord {
    common::table_id_t tableID;
    common::column_id_t columnID;
    common::ValueVector* srcNodeIDVector;
    common::ValueVector* dstNodeIDVector;
    common::ValueVector* relIDVector;
    common::ValueVector* propertyVector;
    std::unique_ptr<common::ValueVector> ownedSrcNodeIDVector;
    std::unique_ptr<common::ValueVector> ownedDstNodeIDVector;
    std::unique_ptr<common::ValueVector> ownedRelIDVector;
    std::unique_ptr<common::ValueVector> ownedPropertyVector;

    RelUpdateRecord()
        : WALRecord{WALRecordType::REL_UPDATE_RECORD}, tableID{common::INVALID_TABLE_ID},
          columnID{common::INVALID_COLUMN_ID}, srcNodeIDVector{nullptr}, dstNodeIDVector{nullptr},
          relIDVector{nullptr}, propertyVector{nullptr} {}
    RelUpdateRecord(common::table_id_t tableID, common::column_id_t columnID,
        common::ValueVector* srcNodeIDVector, common::ValueVector* dstNodeIDVector,
        common::ValueVector* relIDVector, common::ValueVector* propertyVector)
        : WALRecord{WALRecordType::REL_UPDATE_RECORD}, tableID{tableID}, columnID{columnID},
          srcNodeIDVector{srcNodeIDVector}, dstNodeIDVector{dstNodeIDVector},
          relIDVector{relIDVector}, propertyVector{propertyVector} {}
    RelUpdateRecord(common::table_id_t tableID, common::column_id_t columnID,
        std::unique_ptr<common::ValueVector> srcNodeIDVector,
        std::unique_ptr<common::ValueVector> dstNodeIDVector,
        std::unique_ptr<common::ValueVector> relIDVector,
        std::unique_ptr<common::ValueVector> propertyVector)
        : WALRecord{WALRecordType::REL_UPDATE_RECORD}, tableID{tableID}, columnID{columnID},
          srcNodeIDVector{nullptr}, dstNodeIDVector{nullptr}, relIDVector{nullptr},
          propertyVector{nullptr}, ownedSrcNodeIDVector{std::move(srcNodeIDVector)},
          ownedDstNodeIDVector{std::move(dstNodeIDVector)},
          ownedRelIDVector{std::move(relIDVector)}, ownedPropertyVector{std::move(propertyVector)} {
    }

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<RelUpdateRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);
};

} // namespace storage
} // namespace lbug
