#include "storage/stats/column_stats.h"

#include "common/types/date_t.h"
#include "common/types/timestamp_t.h"
#include "function/hash/vector_hash_functions.h"

namespace lbug {
namespace storage {

ColumnStats::ColumnStats(const common::LogicalType& dataType) : hashes{nullptr} {
    if (!common::LogicalTypeUtils::isNested(dataType)) {
        hll.emplace();
    }
}

void ColumnStats::updateMinMax(const common::ValueVector* vector) {
    using common::LogicalTypeID;
    const auto typeID = vector->dataType.getLogicalTypeID();
    const auto& selVector = vector->state->getSelVector();
    auto track = [this, vector](double value, uint32_t selPos) {
        if (vector->isNull(selPos)) {
            return;
        }
        minValue = minValue.has_value() ? std::min(*minValue, value) : value;
        maxValue = maxValue.has_value() ? std::max(*maxValue, value) : value;
    };
    // Read each value in its physical domain and widen to double. DATE is days since
    // epoch (INT32), timestamps are micros since epoch (INT64); both fit exactly.
    switch (typeID) {
    case LogicalTypeID::INT8:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<int8_t>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::INT16:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<int16_t>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::INT32:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<int32_t>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::INT64:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(static_cast<double>(vector->getValue<int64_t>(selVector[i])), selVector[i]);
        }
        break;
    case LogicalTypeID::UINT8:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<uint8_t>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::UINT16:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<uint16_t>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::UINT32:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<uint32_t>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::UINT64:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(static_cast<double>(vector->getValue<uint64_t>(selVector[i])), selVector[i]);
        }
        break;
    case LogicalTypeID::FLOAT:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<float>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::DOUBLE:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<double>(selVector[i]), selVector[i]);
        }
        break;
    case LogicalTypeID::DATE:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(vector->getValue<common::date_t>(selVector[i]).days, selVector[i]);
        }
        break;
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_TZ:
    case LogicalTypeID::TIMESTAMP_MS:
    case LogicalTypeID::TIMESTAMP_NS:
    case LogicalTypeID::TIMESTAMP_SEC:
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            track(static_cast<double>(vector->getValue<common::timestamp_t>(selVector[i]).value),
                selVector[i]);
        }
        break;
    default:
        break;
    }
}

void ColumnStats::update(const common::ValueVector* vector) {
    updateMinMax(vector);
    if (hll) {
        if (!hashes) {
            hashes = std::make_unique<common::ValueVector>(common::LogicalTypeID::UINT64);
        }
        const auto& selVector = vector->state->getSelVector();
        hashes->state = vector->state;
        function::VectorHashFunction::computeHash(*vector, selVector, *hashes,
            hashes->state->getSelVector());
        DASSERT(hashes->hasNoNullsGuarantee());
        // computeHash writes each hash at the position selected by the selection vector, so we must
        // read back using those same selected positions. Iterating flat positions is only correct
        // when the selection vector is unfiltered (e.g. batch inserts), and would read stale slots
        // for filtered selections (e.g. one-tuple-at-a-time inserts from UNWIND), collapsing the
        // distinct-value estimate.
        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            hll->insertElement(hashes->getValue<common::hash_t>(selVector[i]));
        }
        hashes->state = nullptr;
        hashes->setAllNonNull();
    }
}

} // namespace storage
} // namespace lbug
