#pragma once

#include <utility>

#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct NodeDeletionRecord final : WALRecord {
    common::table_id_t tableID;
    common::offset_t nodeOffset;
    common::ValueVector* pkVector;
    std::unique_ptr<common::ValueVector> ownedPKVector;

    NodeDeletionRecord()
        : WALRecord{WALRecordType::NODE_DELETION_RECORD}, tableID{common::INVALID_TABLE_ID},
          nodeOffset{common::INVALID_OFFSET}, pkVector{nullptr} {}
    NodeDeletionRecord(common::table_id_t tableID, common::offset_t nodeOffset,
        common::ValueVector* pkVector)
        : WALRecord{WALRecordType::NODE_DELETION_RECORD}, tableID{tableID}, nodeOffset{nodeOffset},
          pkVector{pkVector} {}
    NodeDeletionRecord(common::table_id_t tableID, common::offset_t nodeOffset,
        std::unique_ptr<common::ValueVector> pkVector)
        : WALRecord{WALRecordType::NODE_DELETION_RECORD}, tableID{tableID}, nodeOffset{nodeOffset},
          pkVector{nullptr}, ownedPKVector{std::move(pkVector)} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<NodeDeletionRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);
};

} // namespace storage
} // namespace lbug
