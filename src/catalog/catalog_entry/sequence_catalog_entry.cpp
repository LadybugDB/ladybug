#include "catalog/catalog_entry/sequence_catalog_entry.h"

#include "binder/ddl/bound_create_sequence_info.h"
#include "common/exception/catalog.h"
#include "common/exception/overflow.h"
#include "common/serializer/deserializer.h"
#include "common/string_utils.h"
#include "common/vector/value_vector.h"
#include "function/arithmetic/add.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace catalog {

SequenceData SequenceCatalogEntry::getSequenceData() {
    std::lock_guard lck(mtx);
    return sequenceData;
}

int64_t SequenceCatalogEntry::currVal() {
    std::lock_guard lck(mtx);
    if (sequenceData.usageCount == 0) {
        throw CatalogException(
            "currval: sequence \"" + name +
            "\" is not yet defined. To define the sequence, call nextval first.");
    }
    return sequenceData.currVal;
}

void SequenceCatalogEntry::nextValNoLock() {
    if (sequenceData.usageCount == 0) {
        // initialization of sequence
        sequenceData.usageCount++;
        return;
    }
    bool overflow = false;
    auto next = sequenceData.currVal;
    try {
        function::Add::operation(next, sequenceData.increment, next);
    } catch (const OverflowException&) {
        overflow = true;
    }
    if (sequenceData.cycle) {
        if (overflow) {
            next = sequenceData.increment < 0 ? sequenceData.maxValue : sequenceData.minValue;
        } else if (next < sequenceData.minValue) {
            next = sequenceData.maxValue;
        } else if (next > sequenceData.maxValue) {
            next = sequenceData.minValue;
        }
    } else {
        const bool minError = overflow ? sequenceData.increment < 0 : next < sequenceData.minValue;
        const bool maxError = overflow ? sequenceData.increment > 0 : next > sequenceData.maxValue;
        if (minError) {
            throw CatalogException("nextval: reached minimum value of sequence \"" + name + "\" " +
                                   std::to_string(sequenceData.minValue));
        }
        if (maxError) {
            throw CatalogException("nextval: reached maximum value of sequence \"" + name + "\" " +
                                   std::to_string(sequenceData.maxValue));
        }
    }
    sequenceData.currVal = next;
    sequenceData.usageCount++;
}

// Moves a non-cycling sequence `count` nextval calls ahead in O(1), so that replaying a large
// setval doesn't take `count` steps. Returns false, changing nothing, when the sequence cycles or
// the result would leave its bounds; the caller then steps with nextValNoLock, which reports it.
bool SequenceCatalogEntry::tryAdvanceNoLock(uint64_t count) {
    if (sequenceData.cycle || count == 0) {
        return false;
    }
    const auto steps = sequenceData.usageCount > 0 ? count : count - 1;
    const auto from = sequenceData.usageCount > 0 ? sequenceData.currVal : sequenceData.startValue;
    if (from < sequenceData.minValue || from > sequenceData.maxValue) {
        return false;
    }
    // Unsigned arithmetic is exact here: `from` and both bounds are int64 values.
    const auto step = sequenceData.increment > 0 ?
                          static_cast<uint64_t>(sequenceData.increment) :
                          uint64_t{0} - static_cast<uint64_t>(sequenceData.increment);
    const auto room =
        sequenceData.increment > 0 ?
            static_cast<uint64_t>(sequenceData.maxValue) - static_cast<uint64_t>(from) :
            static_cast<uint64_t>(from) - static_cast<uint64_t>(sequenceData.minValue);
    if (steps > room / step) {
        return false;
    }
    const auto delta = steps * step;
    sequenceData.currVal =
        static_cast<int64_t>(sequenceData.increment > 0 ? static_cast<uint64_t>(from) + delta :
                                                          static_cast<uint64_t>(from) - delta);
    sequenceData.usageCount += count;
    return true;
}

// referenced from DuckDB
void SequenceCatalogEntry::nextKVal(transaction::Transaction* transaction, const uint64_t& count) {
    DASSERT(count > 0);
    SequenceRollbackData rollbackData{};
    {
        std::lock_guard lck(mtx);
        rollbackData = SequenceRollbackData{sequenceData.usageCount, sequenceData.currVal};
        if (!tryAdvanceNoLock(count)) {
            for (auto i = 0ul; i < count; i++) {
                nextValNoLock();
            }
        }
    }
    transaction->pushSequenceChange(this, count, rollbackData);
}

void SequenceCatalogEntry::nextKVal(transaction::Transaction* transaction, const uint64_t& count,
    ValueVector& resultVector) {
    DASSERT(count > 0);
    SequenceRollbackData rollbackData{};
    {
        std::lock_guard lck(mtx);
        rollbackData = SequenceRollbackData{sequenceData.usageCount, sequenceData.currVal};
        for (auto i = 0ul; i < count; i++) {
            nextValNoLock();
            resultVector.setValue(i, sequenceData.currVal);
        }
    }
    transaction->pushSequenceChange(this, count, rollbackData);
}

int64_t SequenceCatalogEntry::setVal(transaction::Transaction* transaction, int64_t value) {
    SequenceRollbackData rollbackData{};
    uint64_t count = 0;
    {
        std::lock_guard lck(mtx);
        if (value < sequenceData.minValue || value > sequenceData.maxValue) {
            throw CatalogException(
                std::format("setval: value {} is out of bounds for sequence \"{}\" ({}..{})", value,
                    name, sequenceData.minValue, sequenceData.maxValue));
        }
        // The sequence only moves forward, through values nextval would return, so the change is
        // logged and replayed as that many nextval calls. A fresh sequence's first nextval
        // returns its start value; later ones add the increment to the current value.
        const auto usedBefore = sequenceData.usageCount > 0;
        const auto from = usedBefore ? sequenceData.currVal : sequenceData.startValue;
        const auto forward = sequenceData.increment > 0 ? value >= from : value <= from;
        // Unsigned differences are exact here: both values lie within the int64 bounds.
        const auto distance = sequenceData.increment > 0 ?
                                  static_cast<uint64_t>(value) - static_cast<uint64_t>(from) :
                                  static_cast<uint64_t>(from) - static_cast<uint64_t>(value);
        const auto step = sequenceData.increment > 0 ?
                              static_cast<uint64_t>(sequenceData.increment) :
                              uint64_t{0} - static_cast<uint64_t>(sequenceData.increment);
        if (!forward || distance % step != 0) {
            throw CatalogException(std::format(
                "setval: sequence \"{}\" can only move forward to a value it would produce; {} "
                "is not reachable from {} by increment {}",
                name, value, from, sequenceData.increment));
        }
        count = distance / step + (usedBefore ? 0 : 1);
        if (count == 0) {
            return value;
        }
        rollbackData = SequenceRollbackData{sequenceData.usageCount, sequenceData.currVal};
        // Equivalent to `count` calls of nextValNoLock: the target is in bounds and ahead of
        // the current value, so none of them wraps or overflows.
        sequenceData.currVal = value;
        sequenceData.usageCount += count;
    }
    transaction->pushSequenceChange(this, count, rollbackData);
    return value;
}

void SequenceCatalogEntry::rollbackVal(const uint64_t& usageCount, const int64_t& currVal) {
    std::lock_guard lck(mtx);
    sequenceData.usageCount = usageCount;
    sequenceData.currVal = currVal;
}

void SequenceCatalogEntry::serialize(Serializer& serializer) const {
    CatalogEntry::serialize(serializer);
    serializer.writeDebuggingInfo("usageCount");
    serializer.write(sequenceData.usageCount);
    serializer.writeDebuggingInfo("currVal");
    serializer.write(sequenceData.currVal);
    serializer.writeDebuggingInfo("increment");
    serializer.write(sequenceData.increment);
    serializer.writeDebuggingInfo("startValue");
    serializer.write(sequenceData.startValue);
    serializer.writeDebuggingInfo("minValue");
    serializer.write(sequenceData.minValue);
    serializer.writeDebuggingInfo("maxValue");
    serializer.write(sequenceData.maxValue);
    serializer.writeDebuggingInfo("cycle");
    serializer.write(sequenceData.cycle);
}

std::unique_ptr<SequenceCatalogEntry> SequenceCatalogEntry::deserialize(
    Deserializer& deserializer) {
    std::string debuggingInfo;
    uint64_t usageCount = 0;
    int64_t currVal = 0;
    int64_t increment = 0;
    int64_t startValue = 0;
    int64_t minValue = 0;
    int64_t maxValue = 0;
    bool cycle = false;
    deserializer.validateDebuggingInfo(debuggingInfo, "usageCount");
    deserializer.deserializeValue(usageCount);
    deserializer.validateDebuggingInfo(debuggingInfo, "currVal");
    deserializer.deserializeValue(currVal);
    deserializer.validateDebuggingInfo(debuggingInfo, "increment");
    deserializer.deserializeValue(increment);
    deserializer.validateDebuggingInfo(debuggingInfo, "startValue");
    deserializer.deserializeValue(startValue);
    deserializer.validateDebuggingInfo(debuggingInfo, "minValue");
    deserializer.deserializeValue(minValue);
    deserializer.validateDebuggingInfo(debuggingInfo, "maxValue");
    deserializer.deserializeValue(maxValue);
    deserializer.validateDebuggingInfo(debuggingInfo, "cycle");
    deserializer.deserializeValue(cycle);
    auto result = std::make_unique<SequenceCatalogEntry>();
    result->sequenceData.usageCount = usageCount;
    result->sequenceData.currVal = currVal;
    result->sequenceData.increment = increment;
    result->sequenceData.startValue = startValue;
    result->sequenceData.minValue = minValue;
    result->sequenceData.maxValue = maxValue;
    result->sequenceData.cycle = cycle;
    return result;
}

std::string SequenceCatalogEntry::toCypher(const ToCypherInfo& /* info */) const {
    const auto quotedName = common::StringUtils::quoteIdentifier(getName());
    return std::format("DROP SEQUENCE IF EXISTS {};\n"
                       "CREATE SEQUENCE IF NOT EXISTS {} START {} INCREMENT {} MINVALUE {} "
                       "MAXVALUE {} {} CYCLE;\n"
                       "RETURN nextval('{}');",
        quotedName, quotedName, sequenceData.currVal, sequenceData.increment, sequenceData.minValue,
        sequenceData.maxValue, sequenceData.cycle ? "" : "NO", getName());
}

BoundCreateSequenceInfo SequenceCatalogEntry::getBoundCreateSequenceInfo(bool isInternal) const {
    return BoundCreateSequenceInfo(name, sequenceData.startValue, sequenceData.increment,
        sequenceData.minValue, sequenceData.maxValue, sequenceData.cycle,
        ConflictAction::ON_CONFLICT_THROW, isInternal);
}

} // namespace catalog
} // namespace lbug
