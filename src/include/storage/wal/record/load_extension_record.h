#pragma once

#include <string>
#include <utility>

#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct LoadExtensionRecord final : WALRecord {
    std::string path;

    explicit LoadExtensionRecord(std::string path)
        : WALRecord{WALRecordType::LOAD_EXTENSION_RECORD}, path{std::move(path)} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<LoadExtensionRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug
