#include "function/aggregate/histogram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <sstream>
#include <vector>

#include "binder/expression/literal_expression.h"
#include "common/exception/binder.h"
#include "common/type_utils.h"
#include "common/vector/value_vector.h"
#include "function/aggregate/comparison_funcs.h"
#include "function/aggregate/conversion_funcs.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace function {

/**
 * Adaptive Parallel Histogram
 * ============================
 *
 * Morsel-driven parallelism: each worker thread processes data chunks independently,
 * building a linked list of values in its own HistogramState. The combine() step
 * merges lists in O(1) by splicing linked lists. finalize() sorts all values and
 * computes bin boundaries. Returns MAP(STRING, INT64) where each entry maps a
 * "min-max" range string to the count in that bin.
 *
 * Bin strategies:
 *   - ADAPTIVE (equal-depth): boundaries are placed at quantile positions so each
 *     bin has approximately the same number of elements. Duplicate values are kept
 *     together so identical values never span bin boundaries.
 *   - FIXED-SIZE: when bin_width > 0, bins have uniform width from min to max.
 *
 * SQL interface:
 *   HISTOGRAM(value)                  -> adaptive, 10 bins
 *   HISTOGRAM(value, num_bins)        -> adaptive, custom bins
 *   HISTOGRAM(value, num_bins, width) -> fixed-size if width>0, else adaptive
 */

// ---------------------------------------------------------------------------
// Linked-list element (raw bytes, same pattern as percentile_cont)
// ---------------------------------------------------------------------------
struct HistogramElement {
    HistogramElement* next = nullptr;
};

// ---------------------------------------------------------------------------
// A bin descriptor stored after finalize for writeToVector to consume.
// ---------------------------------------------------------------------------
struct HistogramBin {
    double minVal;
    double maxVal;
    uint64_t count;
};

// ---------------------------------------------------------------------------
// Aggregate state
// ---------------------------------------------------------------------------
struct HistogramState : public AggregateStateWithNull {
    explicit HistogramState(int32_t numBins = 10, double binWidth = 0.0)
        : numBins{numBins}, binWidth{binWidth} {}

    uint32_t getStateSize() const override { return sizeof(*this); }

    void writeToVector(common::ValueVector* outputVector, uint64_t pos) override;

    // Linked list of raw input values (populated during update/combine)
    HistogramElement* head = nullptr;
    HistogramElement* tail = nullptr;
    uint64_t count = 0;

    // Configuration (set at bind time)
    int32_t numBins;
    double binWidth; // 0 = adaptive, > 0 = fixed-size

    // Computed bins (populated in finalize, consumed by writeToVector)
    std::vector<HistogramBin> computedBins;
    bool finalized = false;
};

void HistogramState::writeToVector(common::ValueVector* outputVector, uint64_t pos) {
    auto& bins = computedBins;
    auto numEntries = static_cast<uint64_t>(bins.size());

    // Allocate a list entry for the MAP (which is a LIST of STRUCT)
    auto listEntry = common::ListVector::addList(outputVector, numEntries);
    outputVector->setValue<common::list_entry_t>(pos, listEntry);

    if (numEntries == 0) {
        return;
    }

    // Get the key (STRING) and value (INT64) vectors from the MAP's struct child
    auto keyVector = common::MapVector::getKeyVector(outputVector);
    auto valVector = common::MapVector::getValueVector(outputVector);

    for (uint64_t i = 0; i < numEntries; ++i) {
        auto& bin = bins[i];
        char buf[64];
        double avMin = std::fabs(bin.minVal);
        int precMin = (avMin >= 1000.0) ? 0 : (avMin >= 100.0) ? 1 : (avMin >= 1.0) ? 2 : 3;
        double avMax = std::fabs(bin.maxVal);
        int precMax = (avMax >= 1000.0) ? 0 : (avMax >= 100.0) ? 1 : (avMax >= 1.0) ? 2 : 3;
        snprintf(buf, sizeof(buf), "%.*f-%.*f", precMin, bin.minVal, precMax, bin.maxVal);

        auto posInList = listEntry.offset + i;
        common::StringVector::addString(keyVector, posInList, std::string_view(buf));
        valVector->setValue<int64_t>(posInList, static_cast<int64_t>(bin.count));
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint8_t* getElementValue(HistogramElement* element) {
    return reinterpret_cast<uint8_t*>(element) + sizeof(HistogramElement);
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
static std::unique_ptr<AggregateState> initialize() {
    return std::make_unique<HistogramState>();
}

// ---------------------------------------------------------------------------
// Update helpers
// ---------------------------------------------------------------------------
static void updateSingleValue(HistogramState* state, ValueVector* input, uint32_t pos,
    uint64_t multiplicity, InMemOverflowBuffer* overflowBuffer) {
    auto valueSize = LogicalTypeUtils::getRowLayoutSize(input->dataType);
    for (auto i = 0u; i < multiplicity; ++i) {
        auto* element = reinterpret_cast<HistogramElement*>(
            overflowBuffer->allocateSpace(sizeof(HistogramElement) + valueSize));
        element->next = nullptr;
        input->copyToRowData(pos, getElementValue(element), overflowBuffer);
        if (state->tail) {
            state->tail->next = element;
        } else {
            state->head = element;
        }
        state->tail = element;
        state->count++;
        state->isNull = false;
    }
}

static void updateAll(uint8_t* state_, ValueVector* input, uint64_t multiplicity,
    InMemOverflowBuffer* overflowBuffer) {
    DASSERT(!input->state->isFlat());
    auto* state = reinterpret_cast<HistogramState*>(state_);
    input->forEachNonNull(
        [&](auto pos) { updateSingleValue(state, input, pos, multiplicity, overflowBuffer); });
}

static void updatePos(uint8_t* state_, ValueVector* input, uint64_t multiplicity, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    updateSingleValue(reinterpret_cast<HistogramState*>(state_), input, pos, multiplicity,
        overflowBuffer);
}

// ---------------------------------------------------------------------------
// Combine (merges two parallel partial states in O(1) via list splicing)
// ---------------------------------------------------------------------------
static void combine(uint8_t* state_, uint8_t* otherState_,
    InMemOverflowBuffer* /*overflowBuffer*/) {
    auto* otherState = reinterpret_cast<HistogramState*>(otherState_);
    if (otherState->isNull) {
        return;
    }
    auto* state = reinterpret_cast<HistogramState*>(state_);
    if (state->tail) {
        state->tail->next = otherState->head;
    } else {
        state->head = otherState->head;
    }
    state->tail = otherState->tail;
    state->count += otherState->count;
    state->isNull = false;
    // Detach from the other state so the source doesn't double-free.
    otherState->head = nullptr;
    otherState->tail = nullptr;
    otherState->count = 0;
    otherState->isNull = true;
}

// ---------------------------------------------------------------------------
// Build equal-depth (adaptive) bins from sorted values
// ---------------------------------------------------------------------------
static void buildAdaptiveBins(const std::vector<double>& values, int32_t numBins,
    std::vector<HistogramBin>& bins) {
    uint64_t n = values.size();
    if (n == 0) {
        return;
    }

    int32_t k = std::max(1, std::min(numBins, static_cast<int32_t>(n)));

    if (values.front() == values.back()) {
        bins.push_back({values.front(), values.back(), n});
        return;
    }

    std::vector<double> boundaries(k + 1);
    boundaries[0] = values.front();
    for (int32_t i = 1; i < k; ++i) {
        uint64_t idx = static_cast<uint64_t>(static_cast<double>(i) * static_cast<double>(n) /
                                              static_cast<double>(k));
        if (idx >= n) {
            idx = n - 1;
        }
        boundaries[i] = values[idx];
    }
    boundaries[k] = values.back();

    uint64_t valIdx = 0;
    for (int32_t i = 0; i < k && valIdx < n; ++i) {
        double binMin = boundaries[i];
        double binMax = boundaries[i + 1];

        if (i > 0 && binMin == boundaries[i - 1]) {
            continue;
        }

        uint64_t binCount = 0;
        bool lastIter = (i + 1 >= k) ? true : false;
        while (valIdx < n) {
            if (values[valIdx] < binMax || (lastIter && values[valIdx] == binMax)) {
                ++binCount;
                ++valIdx;
            } else if (!lastIter && values[valIdx] == binMax && valIdx + 1 < n &&
                       values[valIdx + 1] == binMax) {
                ++binCount;
                ++valIdx;
            } else {
                break;
            }
        }

        if (binCount > 0) {
            bins.push_back({binMin, valIdx > 0 ? values[valIdx - 1] : binMax, binCount});
        }
    }

    if (valIdx < n) {
        if (!bins.empty()) {
            bins.back().count += (n - valIdx);
            bins.back().maxVal = values.back();
        } else {
            bins.push_back({values.front(), values.back(), n});
        }
    }
}

// ---------------------------------------------------------------------------
// Build fixed-size bins from sorted values
// ---------------------------------------------------------------------------
static void buildFixedSizeBins(const std::vector<double>& values, double binWidth,
    std::vector<HistogramBin>& bins) {
    if (values.empty()) {
        return;
    }

    double minVal = values.front();
    double maxVal = values.back();

    if (minVal == maxVal) {
        bins.push_back({minVal, maxVal, values.size()});
        return;
    }

    double range = maxVal - minVal;
    int32_t k = std::max(1, static_cast<int32_t>(std::ceil(range / binWidth)));

    std::vector<double> boundaries(static_cast<size_t>(k) + 1);
    for (int32_t i = 0; i <= k; ++i) {
        boundaries[static_cast<size_t>(i)] = minVal + static_cast<double>(i) * binWidth;
    }

    uint64_t valIdx = 0;
    for (int32_t i = 0; i < k && valIdx < values.size(); ++i) {
        double binMin = boundaries[static_cast<size_t>(i)];
        double binMax = boundaries[static_cast<size_t>(i) + 1];

        uint64_t binCount = 0;
        bool lastIter = (i + 1 >= k);
        while (valIdx < values.size()) {
            double v = values[valIdx];
            if (v < binMax || (lastIter && v == binMax)) {
                ++binCount;
                ++valIdx;
            } else {
                break;
            }
        }

        if (binCount > 0) {
            bins.push_back({binMin, values[valIdx - 1], binCount});
        }
    }

    if (valIdx < values.size()) {
        if (!bins.empty()) {
            bins.back().count += (values.size() - valIdx);
            bins.back().maxVal = values.back();
        } else {
            bins.push_back({minVal, values.back(), values.size()});
        }
    }
}

// ---------------------------------------------------------------------------
// Finalize (runs once, after all parallel workers have combined)
// ---------------------------------------------------------------------------
static void finalize(uint8_t* state_, LogicalTypeID typeID) {
    auto* state = reinterpret_cast<HistogramState*>(state_);
    if (state->isNull || state->count == 0) {
        state->computedBins.clear();
        state->finalized = true;
        return;
    }

    // 1. Extract all values as doubles for sorting
    std::vector<double> values;
    values.reserve(state->count);
    for (auto* element = state->head; element != nullptr; element = element->next) {
        values.push_back(valueToDouble(getElementValue(element), typeID));
    }

    // 2. Sort
    std::sort(values.begin(), values.end());

    // 3. Build bins
    if (state->binWidth > 0.0) {
        buildFixedSizeBins(values, state->binWidth, state->computedBins);
    } else {
        buildAdaptiveBins(values, state->numBins, state->computedBins);
    }
    state->finalized = true;
}

// ---------------------------------------------------------------------------
// Bind function: parse optional num_bins and bin_width arguments
// ---------------------------------------------------------------------------
static constexpr const char* HISTOGRAM_NAME = "HISTOGRAM";

static int32_t parseNumBins(const ScalarBindFuncInput& input, uint32_t argIdx) {
    auto* literalExpr = dynamic_cast<LiteralExpression*>(input.arguments[argIdx].get());
    if (literalExpr == nullptr) {
        throw BinderException(
            std::format("{} num_bins parameter must be a literal integer.", HISTOGRAM_NAME));
    }
    auto val = literalExpr->getValue().getValue<int64_t>();
    if (val <= 0) {
        throw BinderException(
            std::format("{} num_bins must be a positive integer.", HISTOGRAM_NAME));
    }
    return static_cast<int32_t>(val);
}

static double parseBinWidth(const ScalarBindFuncInput& input, uint32_t argIdx) {
    auto* literalExpr = dynamic_cast<LiteralExpression*>(input.arguments[argIdx].get());
    if (literalExpr == nullptr) {
        throw BinderException(
            std::format("{} bin_width parameter must be a literal.", HISTOGRAM_NAME));
    }
    auto val = literalExpr->getValue().getValue<double>();
    if (val < 0) {
        throw BinderException(
            std::format("{} bin_width must be non-negative.", HISTOGRAM_NAME));
    }
    return val;
}

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    auto numArgs = input.arguments.size();
    int32_t numBins = 10;
    double binWidth = 0.0;

    if (numArgs == 2) {
        numBins = parseNumBins(input, 1);
    } else if (numArgs >= 3) {
        numBins = parseNumBins(input, 1);
        binWidth = parseBinWidth(input, 2);
    }

    auto* aggFunc = reinterpret_cast<AggregateFunction*>(input.definition);
    aggFunc->initializeFunc = [numBins, binWidth]() {
        return std::make_unique<HistogramState>(numBins, binWidth);
    };
    auto typeID = input.arguments[0]->dataType.getLogicalTypeID();
    aggFunc->finalizeFunc = [typeID](auto state) { finalize(state, typeID); };
    aggFunc->initialNullAggregateState = aggFunc->createInitialNullAggregateState();

    // Return type: MAP(STRING, INT64)
    auto resultType =
        LogicalType::MAP(LogicalType::STRING(), LogicalType::INT64());
    return FunctionBindData::getSimpleBindData(input.arguments, resultType);
}

// ---------------------------------------------------------------------------
// Function set registration
// ---------------------------------------------------------------------------
function_set AggregateHistogramFunction::getFunctionSet() {
    function_set result;
    for (auto typeID : LogicalTypeUtils::getNumericalLogicalTypeIDs()) {
        for (auto isDistinct : std::vector<bool>{true, false}) {
            // 1. HISTOGRAM(value)
            result.push_back(std::make_unique<AggregateFunction>(name,
                std::vector<LogicalTypeID>{typeID}, LogicalTypeID::MAP, initialize, updateAll,
                updatePos, combine, [](auto) {}, isDistinct, bindFunc));

            // 2. HISTOGRAM(value, num_bins)
            result.push_back(std::make_unique<AggregateFunction>(name,
                std::vector<LogicalTypeID>{typeID, LogicalTypeID::INT64}, LogicalTypeID::MAP,
                initialize, updateAll, updatePos, combine, [](auto) {}, isDistinct, bindFunc));

            // 3. HISTOGRAM(value, num_bins, bin_width)
            result.push_back(std::make_unique<AggregateFunction>(name,
                std::vector<LogicalTypeID>{typeID, LogicalTypeID::INT64, LogicalTypeID::DOUBLE},
                LogicalTypeID::MAP, initialize, updateAll, updatePos, combine, [](auto) {},
                isDistinct, bindFunc));
        }
    }
    return result;
}

} // namespace function
} // namespace lbug
