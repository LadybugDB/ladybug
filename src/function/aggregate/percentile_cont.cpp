#include "function/aggregate/percentile_cont.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "binder/expression/literal_expression.h"
#include "common/exception/binder.h"
#include "common/type_utils.h"
#include "function/aggregate/comparison_funcs.h"
#include "function/aggregate/conversion_funcs.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace function {

struct PercentileContElement {
    PercentileContElement* next = nullptr;
};

struct PercentileContState : public AggregateStateWithNull {
    explicit PercentileContState(double percentile = 0) : percentile{percentile} {}

    uint32_t getStateSize() const override { return sizeof(*this); }

    void writeToVector(common::ValueVector* outputVector, uint64_t pos) override {
        outputVector->setValue(pos, result);
    }

    PercentileContElement* head = nullptr;
    PercentileContElement* tail = nullptr;
    uint64_t count = 0;
    double percentile;
    double result = 0;
};

static uint8_t* getElementValue(PercentileContElement* element) {
    return reinterpret_cast<uint8_t*>(element) + sizeof(PercentileContElement);
}

static std::unique_ptr<AggregateState> initialize() {
    return std::make_unique<PercentileContState>();
}

static void updateSingleValue(PercentileContState* state, common::ValueVector* input, uint32_t pos,
    uint64_t multiplicity, common::InMemOverflowBuffer* overflowBuffer) {
    auto valueSize = LogicalTypeUtils::getRowLayoutSize(input->dataType);
    for (auto i = 0u; i < multiplicity; ++i) {
        auto* element = reinterpret_cast<PercentileContElement*>(
            overflowBuffer->allocateSpace(sizeof(PercentileContElement) + valueSize));
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

static void updateAll(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
    common::InMemOverflowBuffer* overflowBuffer) {
    DASSERT(!input->state->isFlat());
    auto* state = reinterpret_cast<PercentileContState*>(state_);
    input->forEachNonNull(
        [&](auto pos) { updateSingleValue(state, input, pos, multiplicity, overflowBuffer); });
}

static void updatePos(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
    uint32_t pos, common::InMemOverflowBuffer* overflowBuffer) {
    updateSingleValue(reinterpret_cast<PercentileContState*>(state_), input, pos, multiplicity,
        overflowBuffer);
}

static void combine(uint8_t* state_, uint8_t* otherState_,
    common::InMemOverflowBuffer* /*overflowBuffer*/) {
    auto* otherState = reinterpret_cast<PercentileContState*>(otherState_);
    if (otherState->isNull) {
        return;
    }
    auto* state = reinterpret_cast<PercentileContState*>(state_);
    if (state->tail) {
        state->tail->next = otherState->head;
    } else {
        state->head = otherState->head;
    }
    state->tail = otherState->tail;
    state->count += otherState->count;
    state->isNull = false;
    otherState->head = nullptr;
    otherState->tail = nullptr;
    otherState->count = 0;
    otherState->isNull = true;
}

static void finalize(uint8_t* state_, LogicalTypeID typeID) {
    auto* state = reinterpret_cast<PercentileContState*>(state_);
    if (state->isNull) {
        return;
    }
    std::vector<uint8_t*> values;
    values.reserve(state->count);
    for (auto* element = state->head; element != nullptr; element = element->next) {
        values.push_back(getElementValue(element));
    }
    std::sort(values.begin(), values.end(),
        [typeID](auto left, auto right) { return valueLess(left, right, typeID); });
    auto idx = state->percentile * static_cast<double>(values.size() - 1);
    uint64_t lowIdx = static_cast<uint64_t>(std::floor(idx));
    uint64_t highIdx = static_cast<uint64_t>(std::ceil(idx));
    auto fraction = idx - static_cast<double>(lowIdx);
    double lowVal = valueToDouble(values[lowIdx], typeID);
    double highVal = valueToDouble(values[highIdx], typeID);
    state->result = lowVal + fraction * (highVal - lowVal);
}

static double bindPercentile(const ScalarBindFuncInput& input) {
    if (input.arguments.size() != 2) {
        throw BinderException("percentileCont requires exactly two arguments.");
    }
    auto literalExpr = dynamic_cast<LiteralExpression*>(input.arguments[1].get());
    if (literalExpr == nullptr) {
        throw BinderException("Second parameter of percentileCont must be a literal.");
    }
    auto percentile = literalExpr->getValue().getValue<double>();
    if (percentile < 0 || percentile > 1) {
        throw BinderException("percentileCont percentile must be between 0.0 and 1.0.");
    }
    return percentile;
}

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    auto percentile = bindPercentile(input);
    auto typeID = input.arguments[0]->dataType.getLogicalTypeID();
    auto* aggregateFunction = input.definition->ptrCast<AggregateFunction>();
    aggregateFunction->initializeFunc = [percentile]() {
        return std::make_unique<PercentileContState>(percentile);
    };
    aggregateFunction->finalizeFunc = [typeID](auto state) { finalize(state, typeID); };
    aggregateFunction->initialNullAggregateState =
        aggregateFunction->createInitialNullAggregateState();
    return FunctionBindData::getSimpleBindData(input.arguments, LogicalType::DOUBLE());
}

function_set AggregatePercentileContFunction::getFunctionSet() {
    function_set result;
    for (auto typeID : LogicalTypeUtils::getNumericalLogicalTypeIDs()) {
        for (auto isDistinct : std::vector<bool>{true, false}) {
            result.push_back(std::make_unique<AggregateFunction>(
                name, std::vector<LogicalTypeID>{typeID, LogicalTypeID::DOUBLE},
                LogicalTypeID::DOUBLE, initialize, updateAll, updatePos, combine, [](auto) {},
                isDistinct, bindFunc));
        }
    }
    return result;
}

} // namespace function
} // namespace lbug
