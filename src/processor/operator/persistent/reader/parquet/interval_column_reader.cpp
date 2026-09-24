#include "processor/operator/persistent/reader/parquet/interval_column_reader.h"

#include <cstring>

namespace lbug {
namespace processor {

common::interval_t IntervalValueConversion::readParquetInterval(const char* input) {
    common::interval_t result;
    // Parquet plain-encoded interval data is not guaranteed to be 4-byte aligned;
    // use memcpy instead of direct uint32_t loads (misaligned load is UB).
    uint32_t months, days, microsLow;
    memcpy(&months, input, sizeof(months));
    memcpy(&days, input + sizeof(months), sizeof(days));
    memcpy(&microsLow, input + sizeof(months) + sizeof(days), sizeof(microsLow));
    result.months = months;
    result.days = days;
    result.micros = int64_t(microsLow) * 1000;
    return result;
}

common::interval_t IntervalValueConversion::plainRead(ByteBuffer& plainData,
    ColumnReader& /*reader*/) {
    auto intervalLen = common::ParquetConstants::PARQUET_INTERVAL_SIZE;
    plainData.available(intervalLen);
    auto res = readParquetInterval(reinterpret_cast<const char*>(plainData.ptr));
    plainData.inc(intervalLen);
    return res;
}

void IntervalColumnReader::dictionary(const std::shared_ptr<ResizeableBuffer>& dictionaryData,
    uint64_t numEntries) {
    allocateDict(numEntries * sizeof(common::interval_t));
    auto dict_ptr = reinterpret_cast<common::interval_t*>(this->dict->ptr);
    for (auto i = 0u; i < numEntries; i++) {
        dict_ptr[i] = IntervalValueConversion::plainRead(*dictionaryData, *this);
    }
}

} // namespace processor
} // namespace lbug
