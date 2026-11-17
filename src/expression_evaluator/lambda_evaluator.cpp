#include "expression_evaluator/lambda_evaluator.h"

#include <cstring>

#include "binder/expression/lambda_expression.h"
#include "common/exception/runtime.h"
#include "expression_evaluator/expression_evaluator_visitor.h"
#include "expression_evaluator/list_slice_info.h"
#include "function/list/vector_list_functions.h"
#include "main/client_context.h"
#include "parser/expression/parsed_lambda_expression.h"
#include "storage/buffer_manager/memory_manager.h"
#include <format>

using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::main;
using namespace lbug::storage;

namespace lbug {
namespace evaluator {

// Points dst's lists at src's list data, so evaluators that keep a list's data vector when they
// are compiled, such as properties(), read the elements that gatherSlice references.
static void shareListData(const ValueVector& dst, const ValueVector& src) {
    switch (src.dataType.getPhysicalType()) {
    case PhysicalTypeID::STRUCT: {
        const auto& dstFields = StructVector::getFieldVectors(&dst);
        const auto& srcFields = StructVector::getFieldVectors(&src);
        for (auto i = 0u; i < srcFields.size(); ++i) {
            shareListData(*dstFields[i], *srcFields[i]);
        }
    } break;
    case PhysicalTypeID::LIST:
    case PhysicalTypeID::ARRAY: {
        // Sharing the data vector also shares the structs and lists inside it.
        ListVector::setDataVector(&dst, ListVector::getSharedDataVector(&src));
    } break;
    case PhysicalTypeID::ANY:
    case PhysicalTypeID::BOOL:
    case PhysicalTypeID::INT64:
    case PhysicalTypeID::INT32:
    case PhysicalTypeID::INT16:
    case PhysicalTypeID::INT8:
    case PhysicalTypeID::UINT64:
    case PhysicalTypeID::UINT32:
    case PhysicalTypeID::UINT16:
    case PhysicalTypeID::UINT8:
    case PhysicalTypeID::INT128:
    case PhysicalTypeID::DOUBLE:
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::INTERVAL:
    case PhysicalTypeID::INTERNAL_ID:
    case PhysicalTypeID::ALP_EXCEPTION_FLOAT:
    case PhysicalTypeID::ALP_EXCEPTION_DOUBLE:
    case PhysicalTypeID::UINT128:
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::JSON:
    case PhysicalTypeID::POINTER:
        break;
    }
}

static_assert(sizeof(bool) == sizeof(uint8_t), "BOOL values are gathered as uint8_t");
static_assert(sizeof(float) == sizeof(uint32_t), "FLOAT values are gathered as uint32_t");
static_assert(sizeof(double) == sizeof(uint64_t), "DOUBLE values are gathered as uint64_t");
static_assert(sizeof(uint128_t) == sizeof(int128_t), "UINT128 values are gathered as int128_t");

template<typename T>
static void gatherValues(ValueVector& dst, const ValueVector& src, const SelectionVector& srcSel) {
    DASSERT(src.getNumBytesPerValue() == sizeof(T));
    auto* dstValues = reinterpret_cast<T*>(dst.getData());
    const auto* srcValues = reinterpret_cast<const T*>(src.getData());
    for (sel_t i = 0; i < srcSel.getSelSize(); ++i) {
        dstValues[i] = srcValues[srcSel[i]];
    }
}

static void gatherBytes(ValueVector& dst, const ValueVector& src, const SelectionVector& srcSel) {
    const auto numBytes = src.getNumBytesPerValue();
    for (sel_t i = 0; i < srcSel.getSelSize(); ++i) {
        memcpy(dst.getData() + i * numBytes, src.getData() + srcSel[i] * numBytes, numBytes);
    }
}

// Copies the values of src at srcSel's positions to positions 0 to srcSel.getSelSize() - 1 of dst.
// Strings and the elements of lists are referenced rather than copied, which holds because src is
// not modified while the lambda evaluates the slice.
static void gatherSlice(ValueVector& dst, const SelectionVector& dstSel, const ValueVector& src,
    const SelectionVector& srcSel) {
    if (src.hasNoNullsGuarantee()) {
        dst.setAllNonNull();
    } else {
        for (sel_t i = 0; i < srcSel.getSelSize(); ++i) {
            dst.setNull(i, src.isNull(srcSel[i]));
        }
    }
    switch (src.dataType.getPhysicalType()) {
    case PhysicalTypeID::STRUCT: {
        const auto& dstFields = StructVector::getFieldVectors(&dst);
        const auto& srcFields = StructVector::getFieldVectors(&src);
        for (auto i = 0u; i < srcFields.size(); ++i) {
            gatherSlice(*dstFields[i], dstSel, *srcFields[i], srcSel);
        }
    } break;
    case PhysicalTypeID::LIST:
    case PhysicalTypeID::ARRAY: {
        // Evaluators compiled against dst's data vector at init would read stale elements.
        if (ListVector::getDataVector(&dst) != ListVector::getDataVector(&src)) {
            throw RuntimeException("A list lambda's input replaced its data vector after init.");
        }
        ListVector::copyListEntryAndBufferMetaData(dst, dstSel, src, srcSel);
    } break;
    case PhysicalTypeID::BOOL:
    case PhysicalTypeID::INT8:
    case PhysicalTypeID::UINT8: {
        gatherValues<uint8_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::INT16:
    case PhysicalTypeID::UINT16: {
        gatherValues<uint16_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::INT32:
    case PhysicalTypeID::UINT32:
    case PhysicalTypeID::FLOAT: {
        gatherValues<uint32_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::INT64:
    case PhysicalTypeID::UINT64:
    case PhysicalTypeID::DOUBLE: {
        gatherValues<uint64_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::INT128:
    case PhysicalTypeID::UINT128: {
        gatherValues<int128_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::INTERVAL: {
        gatherValues<interval_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::INTERNAL_ID: {
        gatherValues<internalID_t>(dst, src, srcSel);
    } break;
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::JSON: {
        gatherValues<string_t>(dst, src, srcSel);
    } break;
    // Not list element types: ANY and POINTER vectors are never built, and ALP exceptions exist
    // only in storage.
    case PhysicalTypeID::ANY:
    case PhysicalTypeID::POINTER:
    case PhysicalTypeID::ALP_EXCEPTION_FLOAT:
    case PhysicalTypeID::ALP_EXCEPTION_DOUBLE: {
        gatherBytes(dst, src, srcSel);
    } break;
    }
}

void ListLambdaEvaluator::init(const ResultSet& resultSet, ClientContext* clientContext) {
    for (auto& child : children) {
        child->init(resultSet, clientContext);
    }
    DASSERT(children.size() == 1);
    auto listInputVector = children[0]->resultVector.get();
    // Find all param in lambda, e.g. find x in x->x+1
    auto collector = LambdaParamEvaluatorCollector();
    collector.visit(lambdaRootEvaluator.get());
    auto evaluators = collector.getEvaluators();
    auto lambdaVarState = std::make_shared<DataChunkState>();
    memoryManager = MemoryManager::Get(*clientContext);
    const auto& elementType = ListType::getChildType(listInputVector->dataType);
    if (listLambdaType != ListLambdaType::LIST_REDUCE && !evaluators.empty()) {
        sliceParamVector = std::make_shared<ValueVector>(elementType.copy(), memoryManager);
    }
    for (auto& evaluator : evaluators) {
        // list_reduce updates each lambda variable separately during execution, so each gets its
        // own vector. The other lambdas read every use of their variable from sliceParamVector.
        evaluator->resultVector =
            sliceParamVector != nullptr ?
                sliceParamVector :
                std::make_shared<ValueVector>(elementType.copy(), memoryManager);
        evaluator->resultVector->state = lambdaVarState;
        lambdaParamEvaluators.push_back(evaluator->ptrCast<LambdaParamEvaluator>());
    }
    if (sliceParamVector != nullptr) {
        shareListData(*sliceParamVector, *ListVector::getDataVector(listInputVector));
    }
    lambdaRootEvaluator->init(resultSet, clientContext);
    resolveResultVector(resultSet, memoryManager);
    params.push_back(children[0]->resultVector);
    params.push_back(lambdaRootEvaluator->resultVector);
    auto paramIndices = getParamIndices();
    bindData = ListLambdaBindData{lambdaParamEvaluators, paramIndices, lambdaRootEvaluator.get()};
}

void ListLambdaEvaluator::evaluateInternal() {
    auto* inputVector = params[0].get();
    if (resultVector->dataType.getPhysicalType() == PhysicalTypeID::LIST) {
        ListVector::resizeDataVector(resultVector.get(),
            ListVector::getDataVectorSize(inputVector));
    }
    ListSliceInfo sliceInfo{inputVector};
    bindData.sliceInfo = &sliceInfo;
    auto selVectors = SelectionVector::fromValueVectors(params);
    do {
        sliceInfo.nextSlice();
        if (sliceParamVector != nullptr) {
            copySliceToParamVector(sliceInfo, *inputVector);
        }
        execFunc(params, selVectors, *resultVector, resultVector->getSelVectorPtr(), &bindData);
    } while (!sliceInfo.done());
}

// Many evaluators read an operand at the position they write its result to. The lambda variable
// therefore holds the slice's elements at positions 0 to sliceSize - 1, where the vectors computed
// from it hold their values, instead of selecting them at their offsets in the list data vector.
void ListLambdaEvaluator::copySliceToParamVector(const ListSliceInfo& sliceInfo,
    const ValueVector& listVector) const {
    auto& paramSelVector = sliceParamVector->state->getSelVectorUnsafe();
    paramSelVector.setToUnfiltered(sliceInfo.getSliceSize());
    gatherSlice(*sliceParamVector, paramSelVector, *ListVector::getDataVector(&listVector),
        sliceInfo.getDataSelVector());
}

void ListLambdaEvaluator::evaluate() {
    DASSERT(children.size() == 1);
    children[0]->evaluate();
    evaluateInternal();
}

bool ListLambdaEvaluator::selectInternal(SelectionVector& selVector) {
    DASSERT(children.size() == 1);
    children[0]->evaluate();
    evaluateInternal();
    return updateSelectedPos(selVector);
}

void ListLambdaEvaluator::resolveResultVector(const ResultSet&, MemoryManager* memoryManager) {
    resultVector = std::make_shared<ValueVector>(expression->getDataType().copy(), memoryManager);
    resultVector->state = children[0]->resultVector->state;
    isResultFlat_ = children[0]->isResultFlat();
}

std::vector<idx_t> ListLambdaEvaluator::getParamIndices() {
    const auto& paramNames = getExpression()
                                 ->getChild(1)
                                 ->constCast<binder::LambdaExpression>()
                                 .getParsedLambdaExpr()
                                 ->constCast<parser::ParsedLambdaExpression>()
                                 .getVarNames();
    std::vector<idx_t> index(lambdaParamEvaluators.size());
    for (idx_t i = 0; i < lambdaParamEvaluators.size(); i++) {
        auto paramName = lambdaParamEvaluators[i]->getVarName();
        auto it = std::find(paramNames.begin(), paramNames.end(), paramName);
        if (it != paramNames.end()) {
            index[i] = it - paramNames.begin();
        } else {
            throw RuntimeException(std::format("Lambda paramName {} cannot found.", paramName));
        }
    }
    return index;
}
ListLambdaType ListLambdaEvaluator::checkListLambdaTypeWithFunctionName(std::string functionName) {
    if (0 == functionName.compare(function::ListTransformFunction::name)) {
        return ListLambdaType::LIST_TRANSFORM;
    } else if (0 == functionName.compare(function::ListFilterFunction::name)) {
        return ListLambdaType::LIST_FILTER;
    } else if (0 == functionName.compare(function::ListReduceFunction::name)) {
        return ListLambdaType::LIST_REDUCE;
    } else {
        return ListLambdaType::DEFAULT;
    }
}

} // namespace evaluator
} // namespace lbug
