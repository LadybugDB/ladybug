#pragma once

#include <utility>

#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct RelDeletionRecord final : WALRecord {
    common::table_id_t tableID;
    common::ValueVector* srcNodeIDVector;
    common::ValueVector* dstNodeIDVector;
    common::ValueVector* relIDVector;
    std::unique_ptr<common::ValueVector> ownedSrcNodeIDVector;
    std::unique_ptr<common::ValueVector> ownedDstNodeIDVector;
    std::unique_ptr<common::ValueVector> ownedRelIDVector;

    RelDeletionRecord()
        : WALRecord{WALRecordType::REL_DELETION_RECORD}, tableID{common::INVALID_TABLE_ID},
          srcNodeIDVector{nullptr}, dstNodeIDVector{nullptr}, relIDVector{nullptr} {}
    RelDeletionRecord(common::table_id_t tableID, common::ValueVector* srcNodeIDVector,
        common::ValueVector* dstNodeIDVector, common::ValueVector* relIDVector)
        : WALRecord{WALRecordType::REL_DELETION_RECORD}, tableID{tableID},
          srcNodeIDVector{srcNodeIDVector}, dstNodeIDVector{dstNodeIDVector},
          relIDVector{relIDVector} {}
    RelDeletionRecord(common::table_id_t tableID,
        std::unique_ptr<common::ValueVector> srcNodeIDVector,
        std::unique_ptr<common::ValueVector> dstNodeIDVector,
        std::unique_ptr<common::ValueVector> relIDVector)
        : WALRecord{WALRecordType::REL_DELETION_RECORD}, tableID{tableID}, srcNodeIDVector{nullptr},
          dstNodeIDVector{nullptr}, relIDVector{nullptr},
          ownedSrcNodeIDVector{std::move(srcNodeIDVector)},
          ownedDstNodeIDVector{std::move(dstNodeIDVector)},
          ownedRelIDVector{std::move(relIDVector)} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<RelDeletionRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);
};

} // namespace storage
} // namespace lbug
