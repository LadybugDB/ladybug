#pragma once

#include <cstring>

#include "function/aggregate_function.h"

namespace lbug {
namespace function {

struct BaseCountFunction {

    struct CountState : public AggregateState {
        inline uint32_t getStateSize() const override { return sizeof(*this); }
        inline void writeToVector(common::ValueVector* outputVector, uint64_t pos) override {
            // `this` may live in a packed factorized-table tuple without alignment
            // padding; copy the count out with memcpy instead of touching `count`
            // directly (misaligned member access is UB).
            uint64_t countCopy;
            memcpy(&countCopy, reinterpret_cast<const uint8_t*>(this) + COUNT_OFFSET,
                sizeof(countCopy));
            memcpy(outputVector->getData() + pos * outputVector->getNumBytesPerValue(), &countCopy,
                outputVector->getNumBytesPerValue());
        }

        uint64_t count = 0;
    };

    // Offset of CountState::count from the start of the state. States stored in
    // factorized-table tuples are packed without alignment padding, so state_ may be
    // misaligned and member access through reinterpret_cast<CountState*> is UB.
    // CountState is just a vptr followed by the count, hence this offset.
    static constexpr size_t COUNT_OFFSET = sizeof(void*);
    static_assert(sizeof(AggregateState) == sizeof(void*),
        "AggregateState base is expected to hold only a vptr");

    static inline void addToCount(uint8_t* state_, uint64_t delta) {
        uint64_t countCopy;
        memcpy(&countCopy, state_ + COUNT_OFFSET, sizeof(countCopy));
        countCopy += delta;
        memcpy(state_ + COUNT_OFFSET, &countCopy, sizeof(countCopy));
    }

    static inline uint64_t getCount(const uint8_t* state_) {
        uint64_t countCopy;
        memcpy(&countCopy, state_ + COUNT_OFFSET, sizeof(countCopy));
        return countCopy;
    }

    static std::unique_ptr<AggregateState> initialize() {
        auto state = std::make_unique<CountState>();
        return state;
    }

    static void combine(uint8_t* state_, uint8_t* otherState_,
        common::InMemOverflowBuffer* /*overflowBuffer*/) {
        addToCount(state_, getCount(otherState_));
    }

    static void finalize(uint8_t* /*state_*/) {}
};

} // namespace function
} // namespace lbug
