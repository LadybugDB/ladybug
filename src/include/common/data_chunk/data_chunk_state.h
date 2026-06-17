#pragma once

#include <optional>
#include <vector>

#include "common/data_chunk/sel_vector.h"

namespace lbug {
namespace common {

// F stands for Factorization
enum class FStateType : uint8_t {
    FLAT = 0,
    UNFLAT = 1,
};

class LBUG_API DataChunkState {
public:
    struct PackedChildSlices {
        std::vector<sel_t> parentPositions;
        std::vector<sel_t> offsets;

        void clear() {
            parentPositions.clear();
            offsets.clear();
        }

        bool empty() const { return parentPositions.empty(); }
        sel_t getNumParents() const { return parentPositions.size(); }
        sel_t getNumValues() const { return offsets.empty() ? 0 : offsets.back(); }

        // Pre-allocate for an expected number of parents. Call this before a sequence of
        // append() calls so each append is O(1) amortized with no reallocation.
        // offsets holds one more entry than parentPositions (prefix-sum invariant), so reserve
        // numParents+1 for it.
        void reserve(size_t numParents) {
            parentPositions.reserve(numParents);
            offsets.reserve(numParents + 1);
        }

        // Append a parent slice: parent position and number of values for that parent.
        // Maintains the invariant offsets.size() == parentPositions.size() + 1
        void append(sel_t parentPosition, sel_t numValues) {
            if (offsets.empty()) {
                // initialize offsets with {0, numValues}
                parentPositions.push_back(parentPosition);
                offsets.push_back(0);
                offsets.push_back(numValues);
                return;
            }
            parentPositions.push_back(parentPosition);
            offsets.push_back(offsets.back() + numValues);
        }
    };

    DataChunkState();
    explicit DataChunkState(sel_t capacity) : fStateType{FStateType::UNFLAT} {
        selVector = std::make_shared<SelectionVector>(capacity);
    }

    // returns a dataChunkState for vectors holding a single value.
    static std::shared_ptr<DataChunkState> getSingleValueDataChunkState();

    void initOriginalAndSelectedSize(uint64_t size) { selVector->setSelSize(size); }
    bool isFlat() const { return fStateType == FStateType::FLAT; }
    void setToFlat() { fStateType = FStateType::FLAT; }
    void setToUnflat() { fStateType = FStateType::UNFLAT; }

    const SelectionVector& getSelVector() const { return *selVector; }
    sel_t getSelSize() const { return selVector->getSelSize(); }
    SelectionVector& getSelVectorUnsafe() { return *selVector; }
    std::shared_ptr<SelectionVector> getSelVectorShared() { return selVector; }
    void setSelVector(std::shared_ptr<SelectionVector> selVector_) {
        this->selVector = std::move(selVector_);
    }

    bool hasPackedChildSlices() const { return packedChildSlices.has_value(); }
    const PackedChildSlices& getPackedChildSlices() const {
        DASSERT(packedChildSlices.has_value());
        return *packedChildSlices;
    }
    void setPackedChildSlices(std::vector<sel_t> parentPositions, std::vector<sel_t> offsets) {
        DASSERT(offsets.size() == parentPositions.size() + 1);
        packedChildSlices = PackedChildSlices{std::move(parentPositions), std::move(offsets)};
    }
    void setSingleParentPackedChildSlice(sel_t parentPosition, sel_t numValues) {
        setPackedChildSlices({parentPosition}, {0, numValues});
    }

    // Append a packed child slice for a parent. Creates packedChildSlices if not present.
    void appendPackedChildSlice(sel_t parentPosition, sel_t numValues) {
        if (!packedChildSlices.has_value()) {
            setSingleParentPackedChildSlice(parentPosition, numValues);
            return;
        }
        packedChildSlices->append(parentPosition, numValues);
    }

    // Pre-allocate the packed child slices for an expected number of parents. Creates the
    // optional if not present so subsequent appendPackedChildSlice() calls don't reallocate.
    void reservePackedChildSlices(size_t numParents) {
        if (!packedChildSlices.has_value()) {
            packedChildSlices = PackedChildSlices{};
        }
        packedChildSlices->reserve(numParents);
    }

    void clearPackedChildSlices() { packedChildSlices.reset(); }

private:
    std::shared_ptr<SelectionVector> selVector;
    // TODO: We should get rid of `fStateType` and merge DataChunkState with SelectionVector.
    FStateType fStateType;
    std::optional<PackedChildSlices> packedChildSlices;
};

} // namespace common
} // namespace lbug
