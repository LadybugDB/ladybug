#include "processor/operator/scan/scan_table.h"

#include "binder/expression/scalar_function_expression.h"
#include "common/json_utils.h"
#include "common/vector/value_vector.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

void ColumnCaster::init(ValueVector* vectorAfterCasting, storage::MemoryManager* memoryManager) {
    this->vectorAfterCasting = vectorAfterCasting;
    vectorBeforeCasting = std::make_shared<ValueVector>(columnType.copy(), memoryManager);
    vectorBeforeCasting->setState(vectorAfterCasting->state);
    funcInputVectors = {vectorBeforeCasting};
    funcInputSelVectors = {&vectorBeforeCasting->state->getSelVectorUnsafe()};
}

void ColumnCaster::cast() {
    if (jsonExtractPropertyName.has_value()) {
        auto& selVector = vectorAfterCasting->state->getSelVector();
        for (auto i = 0u; i < selVector.getSelSize(); ++i) {
            auto pos = selVector[i];
            if (vectorBeforeCasting->isNull(pos)) {
                vectorAfterCasting->setNull(pos, true);
                continue;
            }
            auto data = vectorBeforeCasting->getValue<common::string_t>(pos).getAsString();
            auto json = json_extension::stringToJsonNoError(data);
            if (json.ptr == nullptr) {
                vectorAfterCasting->setNull(pos, true);
                continue;
            }
            auto extracted =
                json_extension::jsonExtractScalarToString(json, *jsonExtractPropertyName);
            if (extracted.empty()) {
                vectorAfterCasting->setNull(pos, true);
                continue;
            }
            vectorAfterCasting->setNull(pos, false);
            common::StringVector::addString(vectorAfterCasting, pos, extracted);
        }
        return;
    }
    auto& funcExpr = castExpr->constCast<binder::ScalarFunctionExpression>();
    funcExpr.getFunction().execFunc(funcInputVectors, funcInputSelVectors, *vectorAfterCasting,
        &vectorAfterCasting->state->getSelVectorUnsafe(), funcExpr.getBindData());
}

void ScanTableInfo::castColumns() {
    for (auto& caster : columnCasters) {
        if (caster.hasCast() || caster.hasJSONExtract()) {
            caster.cast();
        }
    }
}

void ScanTableInfo::addColumnInfo(column_id_t columnID, ColumnCaster caster) {
    if (caster.hasCast() || caster.hasJSONExtract()) {
        hasColumnCaster = true;
    }
    columnIDs.push_back(columnID);
    columnCasters.push_back(std::move(caster));
}

void ScanTableInfo::initScanStateVectors(TableScanState& scanState,
    const std::vector<ValueVector*>& outVectors, MemoryManager* memoryManager) {
    if (!hasColumnCaster) {
        // Fast path
        scanState.outputVectors = outVectors;
        return;
    }
    scanState.outputVectors.clear();
    for (auto i = 0u; i < columnCasters.size(); ++i) {
        auto& caster = columnCasters[i];
        auto vector = outVectors[i];
        if (!caster.hasCast() && !caster.hasJSONExtract()) {
            // No need to cast
            scanState.outputVectors.push_back(vector);
        } else {
            caster.init(vector, memoryManager);
            scanState.outputVectors.push_back(caster.getVectorBeforeCasting());
        }
    }
}

void ScanTable::initLocalStateInternal(ResultSet*, ExecutionContext*) {
    for (auto& pos : opInfo.outVectorsPos) {
        outVectors.push_back(resultSet->getValueVector(pos).get());
    }
}

} // namespace processor
} // namespace lbug
