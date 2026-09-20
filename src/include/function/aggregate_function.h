#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "common/in_mem_overflow_buffer.h"
#include "common/vector/value_vector.h"
#include "function/function.h"

namespace lbug {
namespace function {

struct AggregateState {
    virtual uint32_t getStateSize() const = 0;
    virtual void writeToVector(common::ValueVector* outputVector, uint64_t pos) = 0;
    virtual ~AggregateState() = default;
    template<class TARGET>
    const TARGET& constCast() const {
        return common::dynamic_cast_checked<const TARGET&>(*this);
    }
};

struct AggregateStateWithNull : public AggregateState {
    bool isNull = true;
};

using param_rewrite_function_t = std::function<void(binder::expression_vector&)>;
using aggr_initialize_function_t = std::function<std::unique_ptr<AggregateState>()>;
using aggr_update_all_function_t = std::function<void(uint8_t* state, common::ValueVector* input,
    uint64_t multiplicity, common::InMemOverflowBuffer* overflowBuffer)>;
using aggr_update_pos_function_t = std::function<void(uint8_t* state, common::ValueVector* input,
    uint64_t multiplicity, uint32_t pos, common::InMemOverflowBuffer* overflowBuffer)>;
using aggr_combine_function_t = std::function<void(uint8_t* state, uint8_t* otherState,
    common::InMemOverflowBuffer* overflowBuffer)>;
using aggr_finalize_function_t = std::function<void(uint8_t* state)>;

namespace detail {
// Aggregate states stored in packed factorized-table tuples may be misaligned for their
// concrete state type (tuples have no alignment padding). The per-function update/combine/
// finalize implementations reinterpret_cast the raw bytes and touch members, which is UB on
// a misaligned pointer (UBSan: "member access within misaligned address"). Route every call
// through an aligned staging buffer: states that already satisfy the strictest fundamental
// alignment go straight through (the common case, e.g. heap-allocated simple-aggregate
// states); the rest are copied in with memcpy, executed on the aligned copy, and copied
// back. Two slots are provided so combine() can stage both inputs. See also
// copyStateToAlignedBuffer in processor/map/map_aggregate.cpp for the scan path.
inline constexpr size_t AGG_STATE_ALIGNMENT = alignof(std::max_align_t);

inline bool isAggregateStateAligned(const uint8_t* state) {
    return reinterpret_cast<uintptr_t>(state) % AGG_STATE_ALIGNMENT == 0;
}

inline std::vector<uint8_t>& stagedAggregateStateBuffer(int slot) {
    thread_local std::vector<uint8_t> buffers[2];
    return buffers[slot];
}

inline uint8_t* stageAggregateState(const uint8_t* state, uint32_t size, int slot) {
    auto& buffer = stagedAggregateStateBuffer(slot);
    if (buffer.size() < size) {
        buffer.resize(size);
    }
    memcpy(buffer.data(), state, size);
    return buffer.data();
}
} // namespace detail

struct AggregateFunction final : public ScalarOrAggregateFunction {
    bool isDistinct;
    bool needToHandleNulls = false;
    aggr_initialize_function_t initializeFunc;
    aggr_update_all_function_t updateAllFunc;
    aggr_update_pos_function_t updatePosFunc;
    aggr_combine_function_t combineFunc;
    aggr_finalize_function_t finalizeFunc;
    std::unique_ptr<AggregateState> initialNullAggregateState;
    // Rewrite aggregate on NODE/REL, e.g. COUNT(a) -> COUNT(a._id)
    param_rewrite_function_t paramRewriteFunc;

    AggregateFunction(std::string name, std::vector<common::LogicalTypeID> parameterTypeIDs,
        common::LogicalTypeID returnTypeID, aggr_initialize_function_t initializeFunc,
        aggr_update_all_function_t updateAllFunc, aggr_update_pos_function_t updatePosFunc,
        aggr_combine_function_t combineFunc, aggr_finalize_function_t finalizeFunc, bool isDistinct,
        scalar_bind_func bindFunc = nullptr, param_rewrite_function_t paramRewriteFunc = nullptr)
        : ScalarOrAggregateFunction{std::move(name), std::move(parameterTypeIDs), returnTypeID,
              std::move(bindFunc)},
          isDistinct{isDistinct}, initializeFunc{std::move(initializeFunc)},
          updateAllFunc{std::move(updateAllFunc)}, updatePosFunc{std::move(updatePosFunc)},
          combineFunc{std::move(combineFunc)}, finalizeFunc{std::move(finalizeFunc)},
          paramRewriteFunc{std::move(paramRewriteFunc)} {
        initialNullAggregateState = createInitialNullAggregateState();
    }

    EXPLICIT_COPY_DEFAULT_MOVE(AggregateFunction);

    common::idx_t getAggregateStateSize() const {
        return initialNullAggregateState->getStateSize();
    }

    // NOLINTNEXTLINE(readability-make-member-function-const): Returns a non-const pointer.
    AggregateState* getInitialNullAggregateState() { return initialNullAggregateState.get(); }

    std::unique_ptr<AggregateState> createInitialNullAggregateState() const {
        return initializeFunc();
    }

    void updateAllState(uint8_t* state, common::ValueVector* input, uint64_t multiplicity,
        common::InMemOverflowBuffer* overflowBuffer) const {
        if (detail::isAggregateStateAligned(state)) {
            return updateAllFunc(state, input, multiplicity, overflowBuffer);
        }
        auto size = static_cast<uint32_t>(getAggregateStateSize());
        auto* staged = detail::stageAggregateState(state, size, 0);
        updateAllFunc(staged, input, multiplicity, overflowBuffer);
        memcpy(state, staged, size);
    }

    void updatePosState(uint8_t* state, common::ValueVector* input, uint64_t multiplicity,
        uint32_t pos, common::InMemOverflowBuffer* overflowBuffer) const {
        if (detail::isAggregateStateAligned(state)) {
            return updatePosFunc(state, input, multiplicity, pos, overflowBuffer);
        }
        auto size = static_cast<uint32_t>(getAggregateStateSize());
        auto* staged = detail::stageAggregateState(state, size, 0);
        updatePosFunc(staged, input, multiplicity, pos, overflowBuffer);
        memcpy(state, staged, size);
    }

    void combineState(uint8_t* state, uint8_t* otherState,
        common::InMemOverflowBuffer* overflowBuffer) const {
        if (detail::isAggregateStateAligned(state) && detail::isAggregateStateAligned(otherState)) {
            return combineFunc(state, otherState, overflowBuffer);
        }
        auto size = static_cast<uint32_t>(getAggregateStateSize());
        auto* staged = detail::stageAggregateState(state, size, 0);
        auto* stagedOther = detail::stageAggregateState(otherState, size, 1);
        combineFunc(staged, stagedOther, overflowBuffer);
        memcpy(state, staged, size);
    }

    void finalizeState(uint8_t* state) const {
        if (detail::isAggregateStateAligned(state)) {
            return finalizeFunc(state);
        }
        auto size = static_cast<uint32_t>(getAggregateStateSize());
        auto* staged = detail::stageAggregateState(state, size, 0);
        finalizeFunc(staged);
        memcpy(state, staged, size);
    }

    bool isFunctionDistinct() const { return isDistinct; }

private:
    AggregateFunction(const AggregateFunction& other);
};

struct AggregateFunctionUtils {
    template<typename T>
    static std::unique_ptr<AggregateFunction> getAggFunc(std::string name,
        common::LogicalTypeID inputType, common::LogicalTypeID resultType, bool isDistinct,
        param_rewrite_function_t paramRewriteFunc = nullptr) {
        return std::make_unique<AggregateFunction>(std::move(name),
            std::vector<common::LogicalTypeID>{inputType}, resultType, T::initialize, T::updateAll,
            T::updatePos, T::combine, T::finalize, isDistinct, nullptr /* bindFunc */,
            paramRewriteFunc);
    }

    template<template<typename, typename> class FunctionType>
    static void appendSumOrAvgFuncs(std::string name, common::LogicalTypeID inputType,
        function_set& result);
};

struct AggregateSumFunction {
    static constexpr const char* name = "SUM";

    static function_set getFunctionSet();
};

struct AggregateAvgFunction {
    static constexpr const char* name = "AVG";

    static function_set getFunctionSet();
};

struct AggregateMinFunction {
    static constexpr const char* name = "MIN";

    static function_set getFunctionSet();
};

struct AggregateMaxFunction {
    static constexpr const char* name = "MAX";

    static function_set getFunctionSet();
};

struct CollectFunction {
    static constexpr const char* name = "COLLECT";

    static function_set getFunctionSet();
};

} // namespace function
} // namespace lbug
