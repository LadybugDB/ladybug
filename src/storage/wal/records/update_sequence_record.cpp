#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void UpdateSequenceRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.write(sequenceID);
    serializer.write(kCount);
}

std::unique_ptr<UpdateSequenceRecord> UpdateSequenceRecord::deserialize(
    Deserializer& deserializer) {
    auto retVal = std::make_unique<UpdateSequenceRecord>();
    deserializer.deserializeValue(retVal->sequenceID);
    deserializer.deserializeValue(retVal->kCount);
    return retVal;
}

} // namespace storage
} // namespace lbug
