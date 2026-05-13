#include "binder/binder.h"
#include "function/table/bind_data.h"
#include "function/table/simple_table_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/database_header.h"
#include "storage/storage_manager.h"

namespace lbug {
namespace function {

static common::offset_t internalTableFunc(const TableFuncMorsel& /*morsel*/,
    const TableFuncInput& input, common::DataChunk& output) {
    auto& outputVector = output.getValueVectorMutable(0);
    auto pos = outputVector.state->getSelVector()[0];
    auto* storageManager = storage::StorageManager::Get(*input.context->clientContext);
    const auto* header = storageManager->getOrInitDatabaseHeader(*input.context->clientContext);
    outputVector.setValue(pos, header->storageVersion);
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext*,
    const TableFuncBindInput* input) {
    std::vector<std::string> returnColumnNames;
    std::vector<common::LogicalType> returnTypes;
    returnColumnNames.emplace_back("version");
    returnTypes.emplace_back(common::LogicalType::UINT64());
    returnColumnNames =
        TableFunction::extractYieldVariables(returnColumnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<TableFuncBindData>(std::move(columns), 1 /* one row result */);
}

function_set StorageVersionFunction::getFunctionSet() {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector<common::LogicalTypeID>{});
    function->tableFunc = SimpleTableFunc::getTableFunc(internalTableFunc);
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = SimpleTableFunc::initSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    functionSet.push_back(std::move(function));
    return functionSet;
}

} // namespace function
} // namespace lbug
