#include "binder/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"

using namespace lbug::common;
using namespace lbug::main;

namespace lbug {
namespace function {

struct CurrentSettingBindData final : TableFuncBindData {
    std::string optionName;

    CurrentSettingBindData(std::string optionName, binder::expression_vector columns,
        offset_t maxOffset)
        : TableFuncBindData{std::move(columns), maxOffset}, optionName{std::move(optionName)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<CurrentSettingBindData>(optionName, columns, numRows);
    }
};

static offset_t internalTableFunc(const TableFuncMorsel& /*morsel*/, const TableFuncInput& input,
    common::DataChunk& output) {
    auto currentSettingBindData = input.bindData->constPtrCast<CurrentSettingBindData>();
    const auto pos = output.state->getSelVector()[0];
    // Read when the function runs: a reused plan must see a setting changed since it was bound.
    output.getValueVectorMutable(0).setValue(pos,
        input.context->clientContext->getCurrentSetting(currentSettingBindData->optionName)
            .toString());
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const ClientContext* context,
    const TableFuncBindInput* input) {
    auto optionName = input->getLiteralVal<std::string>(0);
    std::vector<std::string> columnNames;
    std::vector<LogicalType> columnTypes;
    columnNames.emplace_back(optionName);
    columnTypes.push_back(LogicalType::STRING());
    columnNames = TableFunction::extractYieldVariables(columnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(columnNames, columnTypes);
    // Fails here for an unknown option.
    context->getCurrentSetting(optionName);
    return std::make_unique<CurrentSettingBindData>(optionName, columns, 1 /* one row result */);
}

function_set CurrentSettingFunction::getFunctionSet() {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::STRING});
    function->tableFunc = SimpleTableFunc::getTableFunc(internalTableFunc);
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = SimpleTableFunc::initSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    functionSet.push_back(std::move(function));
    return functionSet;
}

} // namespace function
} // namespace lbug
