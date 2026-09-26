#include "processor/operator/scan/rel_degree_table.h"

#include <unordered_map>
#include <unordered_set>

#include "common/types/int128_t.h"
#include "common/types/types.h"
#include "common/types/uint128_t.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void RelDegreeTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext*) {
    degreeVector = resultSet->getValueVector(degreeOutputPos).get();
    if (mode == planner::RelDegreeTableMode::TOP_K_DEGREES) {
        nodeKeyVector = resultSet->getValueVector(nodeKeyOutputPos).get();
    }
    hasExecuted = false;
}

void RelDegreeTable::writeNodeKey(offset_t offset, sel_t pos) const {
    // Exhaustive per #935: no `default:` so -Wswitch fails the build when a new
    // PhysicalTypeID is added, forcing a conscious decision here. Non-integer keys must
    // never interpret an offset as a variable-length value (e.g. STRING) — that corrupts
    // memory (#1031) — so they produce NULL instead.
    switch (nodeKeyVector->dataType.getPhysicalType()) {
    case PhysicalTypeID::INT8:
        nodeKeyVector->setValue<int8_t>(pos, static_cast<int8_t>(offset));
        break;
    case PhysicalTypeID::INT16:
        nodeKeyVector->setValue<int16_t>(pos, static_cast<int16_t>(offset));
        break;
    case PhysicalTypeID::INT32:
        nodeKeyVector->setValue<int32_t>(pos, static_cast<int32_t>(offset));
        break;
    case PhysicalTypeID::INT64:
        nodeKeyVector->setValue<int64_t>(pos, static_cast<int64_t>(offset));
        break;
    case PhysicalTypeID::UINT8:
        nodeKeyVector->setValue<uint8_t>(pos, static_cast<uint8_t>(offset));
        break;
    case PhysicalTypeID::UINT16:
        nodeKeyVector->setValue<uint16_t>(pos, static_cast<uint16_t>(offset));
        break;
    case PhysicalTypeID::UINT32:
        nodeKeyVector->setValue<uint32_t>(pos, static_cast<uint32_t>(offset));
        break;
    case PhysicalTypeID::UINT64:
        nodeKeyVector->setValue<uint64_t>(pos, static_cast<uint64_t>(offset));
        break;
    case PhysicalTypeID::INT128:
        nodeKeyVector->setValue<int128_t>(pos, static_cast<int128_t>(offset));
        break;
    case PhysicalTypeID::UINT128:
        nodeKeyVector->setValue<uint128_t>(pos, static_cast<uint128_t>(offset));
        break;
    case PhysicalTypeID::ANY:
    case PhysicalTypeID::BOOL:
    case PhysicalTypeID::DOUBLE:
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::INTERVAL:
    case PhysicalTypeID::INTERNAL_ID:
    case PhysicalTypeID::ALP_EXCEPTION_FLOAT:
    case PhysicalTypeID::ALP_EXCEPTION_DOUBLE:
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::JSON:
    case PhysicalTypeID::LIST:
    case PhysicalTypeID::ARRAY:
    case PhysicalTypeID::STRUCT:
    case PhysicalTypeID::POINTER:
        nodeKeyVector->setNull(pos, true);
        break;
    }
}

bool RelDegreeTable::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }
    auto transaction = Transaction::Get(*context->clientContext);
    if (mode == planner::RelDegreeTableMode::ACTIVE_BOUND_COUNT) {
        if (relTables.size() == 1) {
            const auto count = relTables[0]->getNumActiveBoundNodes(transaction, direction);
            degreeVector->state->getSelVectorUnsafe().setToUnfiltered(1);
            degreeVector->setValue<int64_t>(0, static_cast<int64_t>(count));
            hasExecuted = true;
            return true;
        }
        std::unordered_set<offset_t> activeOffsets;
        for (auto* relTable : relTables) {
            for (auto& [offset, _] : relTable->getDegreeEntries(transaction, direction)) {
                activeOffsets.insert(offset);
            }
        }
        degreeVector->state->getSelVectorUnsafe().setToUnfiltered(1);
        degreeVector->setValue<int64_t>(0, static_cast<int64_t>(activeOffsets.size()));
        hasExecuted = true;
        return true;
    }
    if (mode == planner::RelDegreeTableMode::OFFSET_COUNT) {
        row_idx_t count = 0;
        for (auto* relTable : relTables) {
            count += relTable->getDegreeForOffset(transaction, direction, nodeOffset);
        }
        degreeVector->state->getSelVectorUnsafe().setToUnfiltered(1);
        degreeVector->setValue<int64_t>(0, static_cast<int64_t>(count));
        hasExecuted = true;
        return true;
    }

    std::unordered_map<offset_t, row_idx_t> degrees;
    for (auto* relTable : relTables) {
        auto tableEntries = relTables.size() == 1 ?
                                relTable->getTopKDegrees(transaction, direction, limit) :
                                relTable->getDegreeEntries(transaction, direction);
        for (auto& [offset, degree] : tableEntries) {
            degrees[offset] += degree;
        }
    }
    std::vector<std::pair<offset_t, row_idx_t>> entries;
    entries.reserve(degrees.size());
    for (auto& [offset, degree] : degrees) {
        entries.emplace_back(offset, degree);
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    });
    if (entries.size() > limit) {
        entries.resize(limit);
    }
    auto& selVector = degreeVector->state->getSelVectorUnsafe();
    selVector.setToUnfiltered(entries.size());
    if (nodeKeyVector) {
        nodeKeyVector->state->getSelVectorUnsafe().setToUnfiltered(entries.size());
    }
    for (sel_t i = 0; i < entries.size(); ++i) {
        writeNodeKey(entries[i].first, i);
        degreeVector->setValue<int64_t>(i, static_cast<int64_t>(entries[i].second));
    }
    hasExecuted = true;
    return !entries.empty();
}

} // namespace processor
} // namespace lbug
