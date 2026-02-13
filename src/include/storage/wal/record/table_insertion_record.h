#pragma once

#include <utility>
#include <vector>

#include "common/enums/table_type.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct TableInsertionRecord final : WALRecord {
    common::table_id_t tableID;
    common::TableType tableType;
    common::row_idx_t numRows;
    std::vector<common::ValueVector*> vectors;
    std::vector<std::unique_ptr<common::ValueVector>> ownedVectors;

    TableInsertionRecord()
        : WALRecord{WALRecordType::TABLE_INSERTION_RECORD}, tableID{common::INVALID_TABLE_ID},
          tableType{common::TableType::UNKNOWN}, numRows{0} {}
    TableInsertionRecord(common::table_id_t tableID, common::TableType tableType,
        common::row_idx_t numRows, const std::vector<common::ValueVector*>& vectors)
        : WALRecord{WALRecordType::TABLE_INSERTION_RECORD}, tableID{tableID}, tableType{tableType},
          numRows{numRows}, vectors{vectors} {}
    TableInsertionRecord(common::table_id_t tableID, common::TableType tableType,
        common::row_idx_t numRows, std::vector<std::unique_ptr<common::ValueVector>> vectors)
        : WALRecord{WALRecordType::TABLE_INSERTION_RECORD}, tableID{tableID}, tableType{tableType},
          numRows{numRows}, ownedVectors{std::move(vectors)} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<TableInsertionRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);
};

} // namespace storage
} // namespace lbug
