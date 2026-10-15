#pragma once

#include "binder/expression/expression.h"
#include "common/api.h"

namespace lbug {

namespace main {
class ClientContext;
}

namespace function {

struct LBUG_API FunctionBindData {
    std::vector<common::LogicalType> paramTypes;
    common::LogicalType resultType;
    // TODO: the following two fields should be moved to FunctionLocalState.
    main::ClientContext* clientContext;
    int64_t count;

    explicit FunctionBindData(common::LogicalType dataType)
        : resultType{std::move(dataType)}, clientContext{nullptr}, count{1} {}
    FunctionBindData(std::vector<common::LogicalType> paramTypes, common::LogicalType resultType)
        : paramTypes{std::move(paramTypes)}, resultType{std::move(resultType)},
          clientContext{nullptr}, count{1} {}
    DELETE_COPY_AND_MOVE(FunctionBindData);
    virtual ~FunctionBindData() = default;

    static std::unique_ptr<FunctionBindData> getSimpleBindData(
        const binder::expression_vector& params, const common::LogicalType& resultType);

    template<class TARGET>
    TARGET& cast() {
        return common::dynamic_cast_checked<TARGET&>(*this);
    }

    virtual std::unique_ptr<FunctionBindData> copy() const {
        return std::make_unique<FunctionBindData>(common::LogicalType::copy(paramTypes),
            resultType.copy());
    }
};

struct Function;
using function_set = std::vector<std::unique_ptr<Function>>;

struct ScalarBindFuncInput {
    const binder::expression_vector& arguments;
    Function* definition;
    main::ClientContext* context;
    std::vector<std::string> optionalArguments;

    ScalarBindFuncInput(const binder::expression_vector& arguments, Function* definition,
        main::ClientContext* context, std::vector<std::string> optionalArguments)
        : arguments{arguments}, definition{definition}, context{context},
          optionalArguments{std::move(optionalArguments)} {}
};

using scalar_bind_func =
    std::function<std::unique_ptr<FunctionBindData>(const ScalarBindFuncInput& bindInput)>;

struct LBUG_API Function {
    std::string name;
    std::vector<common::LogicalTypeID> parameterTypeIDs;
    // Optional semantic names for parameters (e.g. {"graph_name"} for GDS functions). When set
    // and sized to match parameterTypeIDs, signatures render as "(graph_name STRING)" instead
    // of the bare "(STRING)".
    std::vector<std::string> parameterNames;
    // Optional full parameter types for display (e.g. LIST(INT64), which a bare
    // LogicalTypeID cannot express). When set and sized to match parameterTypeIDs,
    // signatures render these instead of the bare IDs. Matching and implicit
    // casting continue to use parameterTypeIDs, so this is display-only.
    std::vector<common::LogicalType> parameterTypes;
    bool isReadOnly = true;

    Function() : isReadOnly{true} {};
    Function(std::string name, std::vector<common::LogicalTypeID> parameterTypeIDs)
        : name{std::move(name)}, parameterTypeIDs{std::move(parameterTypeIDs)} {}
    // LogicalType is explicitly-copyable only, so deep-copy parameterTypes here.
    Function(const Function& other)
        : name{other.name}, parameterTypeIDs{other.parameterTypeIDs},
          parameterNames{other.parameterNames},
          parameterTypes{common::LogicalType::copy(other.parameterTypes)},
          isReadOnly{other.isReadOnly} {}
    Function& operator=(const Function& other) {
        if (this != &other) {
            name = other.name;
            parameterTypeIDs = other.parameterTypeIDs;
            parameterNames = other.parameterNames;
            parameterTypes = common::LogicalType::copy(other.parameterTypes);
            isReadOnly = other.isReadOnly;
        }
        return *this;
    }
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;

    virtual ~Function() = default;

    virtual std::string signatureToString() const {
        const bool hasNames =
            !parameterNames.empty() && parameterNames.size() == parameterTypeIDs.size();
        const bool hasTypes =
            !parameterTypes.empty() && parameterTypes.size() == parameterTypeIDs.size();
        if (!hasNames && !hasTypes) {
            return common::LogicalTypeUtils::toString(parameterTypeIDs);
        }
        std::string result = "(";
        for (auto i = 0u; i < parameterTypeIDs.size(); ++i) {
            if (i > 0) {
                result += ",";
            }
            if (hasNames) {
                result += parameterNames[i] + " ";
            }
            result += hasTypes ? parameterTypes[i].toString() :
                                 common::LogicalTypeUtils::toString(parameterTypeIDs[i]);
        }
        return result + ")";
    }

    template<class TARGET>
    const TARGET* constPtrCast() const {
        return common::dynamic_cast_checked<const TARGET*>(this);
    }
    template<class TARGET>
    TARGET* ptrCast() {
        return common::dynamic_cast_checked<TARGET*>(this);
    }
};

struct ScalarOrAggregateFunction : Function {
    common::LogicalTypeID returnTypeID = common::LogicalTypeID::ANY;
    scalar_bind_func bindFunc = nullptr;

    ScalarOrAggregateFunction() : Function{} {}
    ScalarOrAggregateFunction(std::string name, std::vector<common::LogicalTypeID> parameterTypeIDs,
        common::LogicalTypeID returnTypeID)
        : Function{std::move(name), std::move(parameterTypeIDs)}, returnTypeID{returnTypeID} {}
    ScalarOrAggregateFunction(std::string name, std::vector<common::LogicalTypeID> parameterTypeIDs,
        common::LogicalTypeID returnTypeID, scalar_bind_func bindFunc)
        : Function{std::move(name), std::move(parameterTypeIDs)}, returnTypeID{returnTypeID},
          bindFunc{std::move(bindFunc)} {}

    std::string signatureToString() const override {
        auto result = Function::signatureToString();
        result += " -> " + common::LogicalTypeUtils::toString(returnTypeID);
        return result;
    }
};

} // namespace function
} // namespace lbug
