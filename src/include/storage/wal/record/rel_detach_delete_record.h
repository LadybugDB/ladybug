#pragma once

#include <utility>

#include "common/enums/rel_direction.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct RelDetachDeleteRecord final : WALRecord {
    common::table_id_t tableID;
    common::RelDataDirection direction;
    common::ValueVector* srcNodeIDVector;
    std::unique_ptr<common::ValueVector> ownedSrcNodeIDVector;

    RelDetachDeleteRecord()
        : WALRecord{WALRecordType::REL_DETACH_DELETE_RECORD}, tableID{common::INVALID_TABLE_ID},
          direction{common::RelDataDirection::FWD}, srcNodeIDVector{nullptr} {}
    RelDetachDeleteRecord(common::table_id_t tableID, common::RelDataDirection direction,
        common::ValueVector* srcNodeIDVector)
        : WALRecord{WALRecordType::REL_DETACH_DELETE_RECORD}, tableID{tableID},
          direction{direction}, srcNodeIDVector{srcNodeIDVector} {}
    RelDetachDeleteRecord(common::table_id_t tableID, common::RelDataDirection direction,
        std::unique_ptr<common::ValueVector> srcNodeIDVector)
        : WALRecord{WALRecordType::REL_DETACH_DELETE_RECORD}, tableID{tableID},
          direction{direction}, srcNodeIDVector{nullptr},
          ownedSrcNodeIDVector{std::move(srcNodeIDVector)} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<RelDetachDeleteRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);
};

} // namespace storage
} // namespace lbug
