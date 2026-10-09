#include "common/exception/binder.h"
#include "common/type_utils.h"
#include "function/list/vector_list_functions.h"
#include "function/scalar_function.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace function {

template<typename OPERATION>
static std::unique_ptr<FunctionBindData> bindFuncListAggr(const ScalarBindFuncInput& input) {
    auto scalarFunction = input.definition->ptrCast<ScalarFunction>();
    const auto& resultType = ListType::getChildType(input.arguments[0]->dataType);
    TypeUtils::visit(
        resultType,
        [&scalarFunction]<NumericTypes T>(T) {
            scalarFunction->execFunc =
                ScalarFunction::UnaryExecNestedTypeFunction<list_entry_t, T, OPERATION>;
        },
        [&input, &resultType](auto) {
            throw BinderException(std::format("Unsupported inner data type for {}: {}",
                input.definition->name, LogicalTypeUtils::toString(resultType.getLogicalTypeID())));
        });
    return FunctionBindData::getSimpleBindData(input.arguments, resultType);
}

template<typename OPERATION>
static std::unique_ptr<FunctionBindData> bindFuncListMinMax(const ScalarBindFuncInput& input) {
    auto scalarFunction = input.definition->ptrCast<ScalarFunction>();
    const auto& argType = input.arguments[0]->dataType;
    if (argType.getPhysicalType() != PhysicalTypeID::LIST &&
        argType.getPhysicalType() != PhysicalTypeID::ARRAY) {
        scalarFunction->execFunc =
            ScalarFunction::UnaryExecNestedTypeFunction<list_entry_t, int64_t, OPERATION>;
        std::vector<LogicalType> types;
        types.push_back(LogicalType::LIST(LogicalType::INT64()));
        return std::make_unique<FunctionBindData>(std::move(types), LogicalType::INT64());
    }
    const auto& resultType = ListType::getChildType(argType);
    if (resultType.getLogicalTypeID() == LogicalTypeID::ANY) {
        throw BinderException(std::format("Unsupported inner data type for {}: {}",
            input.definition->name, LogicalTypeUtils::toString(resultType.getLogicalTypeID())));
    }
    TypeUtils::visit(
        resultType.getPhysicalType(),
        [&scalarFunction]<ComparableTypes T>(T) {
            scalarFunction->execFunc =
                ScalarFunction::UnaryExecNestedTypeFunction<list_entry_t, T, OPERATION>;
        },
        [&input, &resultType](auto) {
            throw BinderException(std::format("Unsupported inner data type for {}: {}",
                input.definition->name, LogicalTypeUtils::toString(resultType.getLogicalTypeID())));
        });
    return FunctionBindData::getSimpleBindData(input.arguments, resultType);
}

struct ListSum {
    template<typename T>
    static void operation(common::list_entry_t& input, T& result, common::ValueVector& inputVector,
        common::ValueVector& /*resultVector*/) {
        auto inputDataVector = common::ListVector::getDataVector(&inputVector);
        result = 0;
        for (auto i = 0u; i < input.size; i++) {
            if (inputDataVector->isNull(input.offset + i)) {
                continue;
            }
            result += inputDataVector->getValue<T>(input.offset + i);
        }
    }
};

function_set ListSumFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::LIST}, LogicalTypeID::INT64);
    function->bindFunc = bindFuncListAggr<ListSum>;
    result.push_back(std::move(function));
    return result;
}

struct ListProduct {
    template<typename T>
    static void operation(common::list_entry_t& input, T& result, common::ValueVector& inputVector,
        common::ValueVector& /*resultVector*/) {
        auto inputDataVector = common::ListVector::getDataVector(&inputVector);
        result = 1;
        for (auto i = 0u; i < input.size; i++) {
            if (inputDataVector->isNull(input.offset + i)) {
                continue;
            }
            result *= inputDataVector->getValue<T>(input.offset + i);
        }
    }
};

function_set ListProductFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::LIST}, LogicalTypeID::INT64);
    function->bindFunc = bindFuncListAggr<ListProduct>;
    result.push_back(std::move(function));
    return result;
}

template<bool IS_MIN>
struct ListMinMax {
    template<typename T>
    static void operation(common::list_entry_t& input, T& result, common::ValueVector& inputVector,
        common::ValueVector& resultVector) {
        auto inputDataVector = common::ListVector::getDataVector(&inputVector);
        auto numBytesPerValue = inputDataVector->getNumBytesPerValue();
        auto inputValues = common::ListVector::getListValues(&inputVector, input);
        bool found = false;
        uint8_t* foundPtr = nullptr;
        T best{};
        for (auto i = 0u; i < input.size; i++) {
            if (!inputDataVector->isNull(input.offset + i)) {
                auto val = inputDataVector->getValue<T>(input.offset + i);
                if (!found || (IS_MIN ? val < best : val > best)) {
                    best = val;
                    found = true;
                    foundPtr = inputValues;
                }
            }
            inputValues += numBytesPerValue;
        }
        if (!found) {
            auto resultPos =
                static_cast<sel_t>((reinterpret_cast<uint8_t*>(&result) - resultVector.getData()) /
                                   resultVector.getNumBytesPerValue());
            resultVector.setNull(resultPos, true);
            return;
        }
        resultVector.copyFromVectorData(reinterpret_cast<uint8_t*>(&result), inputDataVector,
            foundPtr);
    }
};

using ListMin = ListMinMax<true>;
using ListMax = ListMinMax<false>;

function_set ListMinFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::LIST}, LogicalTypeID::ANY);
    function->bindFunc = bindFuncListMinMax<ListMin>;
    result.push_back(std::move(function));
    return result;
}

function_set ListMaxFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::LIST}, LogicalTypeID::ANY);
    function->bindFunc = bindFuncListMinMax<ListMax>;
    result.push_back(std::move(function));
    return result;
}

} // namespace function
} // namespace lbug
