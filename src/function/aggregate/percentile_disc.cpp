#include "function/aggregate/percentile_disc.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "binder/expression/literal_expression.h"
#include "common/exception/binder.h"
#include "common/type_utils.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace function {

template<typename T>
struct PercentileDiscElement {
    PercentileDiscElement* next = nullptr;
    T value{};
};

template<typename T>
struct PercentileDiscState : public AggregateStateWithNull {
    explicit PercentileDiscState(double percentile = 0) : percentile{percentile} {}

    uint32_t getStateSize() const override { return sizeof(*this); }

    void writeToVector(common::ValueVector* outputVector, uint64_t pos) override {
        outputVector->setValue(pos, result);
    }

    PercentileDiscElement<T>* head = nullptr;
    PercentileDiscElement<T>* tail = nullptr;
    uint64_t count = 0;
    double percentile;
    T result{};
};

template<typename T>
struct PercentileDiscFunction {
    static std::unique_ptr<AggregateState> initialize() {
        return std::make_unique<PercentileDiscState<T>>();
    }

    static void updateAll(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
        common::InMemOverflowBuffer* overflowBuffer) {
        DASSERT(!input->state->isFlat());
        auto* state = reinterpret_cast<PercentileDiscState<T>*>(state_);
        input->forEachNonNull(
            [&](auto pos) { updateSingleValue(state, input, pos, multiplicity, overflowBuffer); });
    }

    static void updatePos(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
        uint32_t pos, common::InMemOverflowBuffer* overflowBuffer) {
        updateSingleValue(reinterpret_cast<PercentileDiscState<T>*>(state_), input, pos,
            multiplicity, overflowBuffer);
    }

    static void updateSingleValue(PercentileDiscState<T>* state, common::ValueVector* input,
        uint32_t pos, uint64_t multiplicity, common::InMemOverflowBuffer* overflowBuffer) {
        auto value = input->getValue<T>(pos);
        for (auto i = 0u; i < multiplicity; ++i) {
            auto* element = reinterpret_cast<PercentileDiscElement<T>*>(
                overflowBuffer->allocateSpace(sizeof(PercentileDiscElement<T>)));
            element->next = nullptr;
            element->value = value;
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

    static void combine(uint8_t* state_, uint8_t* otherState_,
        common::InMemOverflowBuffer* /*overflowBuffer*/) {
        auto* otherState = reinterpret_cast<PercentileDiscState<T>*>(otherState_);
        if (otherState->isNull) {
            return;
        }
        auto* state = reinterpret_cast<PercentileDiscState<T>*>(state_);
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

    static void finalize(uint8_t* state_) {
        auto* state = reinterpret_cast<PercentileDiscState<T>*>(state_);
        if (state->isNull) {
            return;
        }
        std::vector<T> values;
        values.reserve(state->count);
        for (auto* element = state->head; element != nullptr; element = element->next) {
            values.push_back(element->value);
        }
        std::sort(values.begin(), values.end());
        auto rawIndex = static_cast<int64_t>(std::ceil(state->percentile * values.size())) - 1;
        auto index = std::clamp<int64_t>(rawIndex, 0, static_cast<int64_t>(values.size()) - 1);
        state->result = values[index];
    }
};

static double bindPercentile(const ScalarBindFuncInput& input) {
    if (input.arguments.size() != 2) {
        throw BinderException("percentileDisc requires exactly two arguments.");
    }
    auto literalExpr = dynamic_cast<LiteralExpression*>(input.arguments[1].get());
    if (literalExpr == nullptr) {
        throw BinderException("Second parameter of percentileDisc must be a literal.");
    }
    auto percentile = literalExpr->getValue().getValue<double>();
    if (percentile < 0 || percentile > 1) {
        throw BinderException("percentileDisc percentile must be between 0.0 and 1.0.");
    }
    return percentile;
}

template<typename T>
static std::unique_ptr<FunctionBindData> bindTypedPercentileDisc(const ScalarBindFuncInput& input) {
    auto percentile = bindPercentile(input);
    auto* aggregateFunction = input.definition->ptrCast<AggregateFunction>();
    aggregateFunction->initializeFunc = [percentile]() {
        return std::make_unique<PercentileDiscState<T>>(percentile);
    };
    aggregateFunction->initialNullAggregateState =
        aggregateFunction->createInitialNullAggregateState();
    return FunctionBindData::getSimpleBindData(input.arguments, input.arguments[0]->dataType);
}

function_set AggregatePercentileDiscFunction::getFunctionSet() {
    function_set result;
    for (auto typeID : LogicalTypeUtils::getNumericalLogicalTypeIDs()) {
        TypeUtils::visit(
            LogicalType::getPhysicalType(typeID),
            [&]<IntegerTypes T>(T) {
                for (auto isDistinct : std::vector<bool>{true, false}) {
                    result.push_back(std::make_unique<AggregateFunction>(name,
                        std::vector<LogicalTypeID>{typeID, LogicalTypeID::DOUBLE}, typeID,
                        PercentileDiscFunction<T>::initialize, PercentileDiscFunction<T>::updateAll,
                        PercentileDiscFunction<T>::updatePos, PercentileDiscFunction<T>::combine,
                        PercentileDiscFunction<T>::finalize, isDistinct,
                        bindTypedPercentileDisc<T>));
                }
            },
            [&]<FloatingPointTypes T>(T) {
                for (auto isDistinct : std::vector<bool>{true, false}) {
                    result.push_back(std::make_unique<AggregateFunction>(name,
                        std::vector<LogicalTypeID>{typeID, LogicalTypeID::DOUBLE}, typeID,
                        PercentileDiscFunction<T>::initialize, PercentileDiscFunction<T>::updateAll,
                        PercentileDiscFunction<T>::updatePos, PercentileDiscFunction<T>::combine,
                        PercentileDiscFunction<T>::finalize, isDistinct,
                        bindTypedPercentileDisc<T>));
                }
            },
            [](auto) { UNREACHABLE_CODE; });
    }
    return result;
}

} // namespace function
} // namespace lbug
