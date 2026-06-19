#include "storage/stats/column_stats.h"

#include "function/hash/vector_hash_functions.h"

namespace lbug {
namespace storage {

ColumnStats::ColumnStats(const common::LogicalType& dataType) : hashes{nullptr} {
    if (!common::LogicalTypeUtils::isNested(dataType)) {
        hll.emplace();
    }
}

void ColumnStats::update(const common::ValueVector* vector) {
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
