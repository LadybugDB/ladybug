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
    // Describes how the children in the current output batch map back to their parents. Used by
    // the PACKED_EXTEND physical operator; see docs/multi_parent_lifetime.md for the full
    // lifetime & representation rationale.
    //
    // Representation (multi-parent packed scan):
    //   - parentSelVector aliases the bound (parent) chunk state's selection vector for the
    //     current output batch. The shared_ptr keeps the SelectionVector OBJECT alive even if
    //     the input state is reused, but the buffer contents are NOT snapshotted: they are
    //     whatever the bound chunk's selection vector currently holds (the parents whose
    //     children are materialized in this batch).
    //   - offsets is a prefix sum over ALL parents in parentSelVector, with
    //     offsets.size() == parentSelVector->getSelSize() + 1; the children of parent i occupy
    //     output positions [offsets[i], offsets[i+1]) in the child chunk's selection vector.
    //     offsets[i] == offsets[i+1] means parent i produced no children in this batch
    //     (consumers must skip zero-length ranges).
    //
    // Lifetime rule: the descriptor is valid only for synchronous consumption of this output
    // batch. Do not persist it across a materialization boundary (e.g. appending to a
    // FactorizedTable and reading back later): the aliased buffer is rewritten in place by the
    // next input batch (setToFiltered/setToUnfiltered) and the descriptor is cleared/reset
    // (ResultSet::resetForReuse, next scan call).
    struct PackedChildSlices {
        std::shared_ptr<SelectionVector> parentSelVector;
        std::vector<sel_t> offsets;

        bool empty() const { return parentSelVector == nullptr; }
        sel_t getNumParents() const { return empty() ? 0 : parentSelVector->getSelSize(); }
        sel_t getNumValues() const { return offsets.empty() ? 0 : offsets.back(); }
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
    void setPackedChildSlices(std::shared_ptr<SelectionVector> parentSelVector,
        std::vector<sel_t> offsets) {
        DASSERT(parentSelVector != nullptr);
        DASSERT(offsets.size() == parentSelVector->getSelSize() + 1);
        packedChildSlices = PackedChildSlices{std::move(parentSelVector), std::move(offsets)};
    }
    // Single-parent convenience: parentSelVector must hold exactly one parent position.
    void setSingleParentPackedChildSlice(std::shared_ptr<SelectionVector> parentSelVector,
        sel_t numValues) {
        DASSERT(parentSelVector->getSelSize() == 1);
        setPackedChildSlices(std::move(parentSelVector), {0, numValues});
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
