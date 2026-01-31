#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/graph_catalog_entry.h"
#include "function/table/bind_data.h"
#include "function/table/simple_table_function.h"
#include "main/client_context.h"
#include "main/database.h"
#include <format>

using namespace lbug::common;
using namespace lbug::catalog;

namespace lbug {
namespace function {

struct GraphInfo {
    std::string name;
    std::string type;

    GraphInfo(std::string name, std::string type) : name{std::move(name)}, type{std::move(type)} {}
};

struct ShowGraphsBindData final : TableFuncBindData {
    std::vector<GraphInfo> graphs;

    ShowGraphsBindData(std::vector<GraphInfo> graphs, binder::expression_vector columns,
        row_idx_t numRows)
        : TableFuncBindData{std::move(columns), numRows}, graphs{std::move(graphs)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ShowGraphsBindData>(graphs, columns, numRows);
    }
};

static offset_t internalTableFunc(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    const auto graphs = input.bindData->constPtrCast<ShowGraphsBindData>()->graphs;
    const auto numGraphsToOutput = morsel.endOffset - morsel.startOffset;
    for (auto i = 0u; i < numGraphsToOutput; i++) {
        const auto graphInfo = graphs[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, graphInfo.name);
        output.getValueVectorMutable(1).setValue(i, graphInfo.type);
    }
    return numGraphsToOutput;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext* context,
    const TableFuncBindInput* input) {
    std::vector<std::string> columnNames;
    std::vector<LogicalType> columnTypes;
    columnNames.emplace_back("name");
    columnTypes.emplace_back(LogicalType::STRING());
    columnNames.emplace_back("type");
    columnTypes.emplace_back(LogicalType::STRING());

    std::vector<GraphInfo> graphInfos;
    auto transaction = transaction::Transaction::Get(*context);
    auto catalog = context->getDatabase()->getCatalog();

    // Get all graphs from the main database catalog
    auto graphEntries = catalog->getGraphEntries(transaction);
    for (auto* entry : graphEntries) {
        std::string graphType = entry->isAnyGraphType() ? "ANY" : "STANDARD";
        graphInfos.emplace_back(entry->getName(), graphType);
    }

    columnNames = TableFunction::extractYieldVariables(columnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(columnNames, columnTypes);
    return std::make_unique<ShowGraphsBindData>(std::move(graphInfos), std::move(columns),
        graphInfos.size());
}

function_set ShowGraphsFunction::getFunctionSet() {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector<LogicalTypeID>{});
    function->tableFunc = SimpleTableFunc::getTableFunc(internalTableFunc);
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = SimpleTableFunc::initSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    functionSet.push_back(std::move(function));
    return functionSet;
}

} // namespace function
} // namespace lbug
