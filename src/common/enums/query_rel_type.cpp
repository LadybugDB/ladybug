#include "common/enums/query_rel_type.h"

#include "common/assert.h"
#include "function/gds/gds_function_collection.h"

using namespace lbug::function;

namespace lbug {
namespace common {

PathSemantic QueryRelTypeUtils::getPathSemantic(QueryRelType queryRelType) {
    switch (queryRelType) {
    case QueryRelType::VARIABLE_LENGTH_WALK:
        return PathSemantic::WALK;
    case QueryRelType::VARIABLE_LENGTH_TRAIL:
        return PathSemantic::TRAIL;
    case QueryRelType::VARIABLE_LENGTH_ACYCLIC:
    case QueryRelType::SHORTEST:
    case QueryRelType::ALL_SHORTEST:
    case QueryRelType::WEIGHTED_SHORTEST:
    case QueryRelType::ALL_WEIGHTED_SHORTEST:
        return PathSemantic::ACYCLIC;
    default:
        UNREACHABLE_CODE;
    }
}

std::unique_ptr<function::RJAlgorithm> QueryRelTypeUtils::getFunction(QueryRelType type) {
    switch (type) {
    case QueryRelType::VARIABLE_LENGTH_WALK:
    case QueryRelType::VARIABLE_LENGTH_TRAIL:
    case QueryRelType::VARIABLE_LENGTH_ACYCLIC: {
        return VarLenJoinsFunction::getAlgorithm();
    }
    case QueryRelType::SHORTEST: {
        return SingleSPPathsFunction::getAlgorithm();
    }
    case QueryRelType::ALL_SHORTEST: {
        return AllSPPathsFunction::getAlgorithm();
    }
    case QueryRelType::WEIGHTED_SHORTEST: {
        return WeightedSPPathsFunction::getAlgorithm();
    }
    case QueryRelType::ALL_WEIGHTED_SHORTEST: {
        return AllWeightedSPPathsFunction::getAlgorithm();
    }
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace common
} // namespace lbug
