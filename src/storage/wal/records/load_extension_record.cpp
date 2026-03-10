#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void LoadExtensionRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.writeDebuggingInfo("path");
    serializer.write<std::string>(path);
}

std::unique_ptr<LoadExtensionRecord> LoadExtensionRecord::deserialize(Deserializer& deserializer) {
    std::string key;
    deserializer.validateDebuggingInfo(key, "path");
    std::string path;
    deserializer.deserializeValue<std::string>(path);
    return std::make_unique<LoadExtensionRecord>(std::move(path));
}

} // namespace storage
} // namespace lbug
