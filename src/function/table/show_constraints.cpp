#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/constraint_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "main/client_context.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::main;

namespace lbug {
namespace function {

struct ConstraintInfo {
    std::string name;
    std::string tableName;
    std::string tableType;
    std::vector<std::string> propertyNames;
    std::string definition;

    ConstraintInfo(std::string name, std::string tableName, std::string tableType,
        std::vector<std::string> propertyNames, std::string definition)
        : name{std::move(name)}, tableName{std::move(tableName)}, tableType{std::move(tableType)},
          propertyNames{std::move(propertyNames)}, definition{std::move(definition)} {}
};

struct ShowConstraintsBindData final : TableFuncBindData {
    std::vector<ConstraintInfo> constraintsInfo;

    ShowConstraintsBindData(std::vector<ConstraintInfo> constraintsInfo,
        binder::expression_vector columns, offset_t maxOffset)
        : TableFuncBindData{std::move(columns), maxOffset},
          constraintsInfo{std::move(constraintsInfo)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ShowConstraintsBindData>(*this);
    }
};

static offset_t internalTableFunc(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    auto& constraintsInfo =
        input.bindData->constPtrCast<ShowConstraintsBindData>()->constraintsInfo;
    auto numTuplesToOutput = morsel.getMorselSize();
    auto& propertyVector = output.getValueVectorMutable(3);
    auto propertyDataVec = ListVector::getDataVector(&propertyVector);
    for (auto i = 0u; i < numTuplesToOutput; i++) {
        auto constraintInfo = constraintsInfo[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, constraintInfo.name);
        output.getValueVectorMutable(1).setValue(i, constraintInfo.tableName);
        output.getValueVectorMutable(2).setValue(i, constraintInfo.tableType);
        auto listEntry = ListVector::addList(&propertyVector, constraintInfo.propertyNames.size());
        for (auto j = 0u; j < constraintInfo.propertyNames.size(); j++) {
            propertyDataVec->setValue(listEntry.offset + j, constraintInfo.propertyNames[j]);
        }
        propertyVector.setValue(i, listEntry);
        output.getValueVectorMutable(4).setValue(i, constraintInfo.definition);
    }
    return numTuplesToOutput;
}

static binder::expression_vector bindColumns(const TableFuncBindInput& input) {
    std::vector<std::string> columnNames;
    std::vector<LogicalType> columnTypes;
    columnNames.emplace_back("name");
    columnTypes.emplace_back(LogicalType::STRING());
    columnNames.emplace_back("table_name");
    columnTypes.emplace_back(LogicalType::STRING());
    columnNames.emplace_back("table_type");
    columnTypes.emplace_back(LogicalType::STRING());
    columnNames.emplace_back("property_names");
    columnTypes.emplace_back(LogicalType::LIST(LogicalType::STRING()));
    columnNames.emplace_back("definition");
    columnTypes.emplace_back(LogicalType::STRING());
    columnNames = TableFunction::extractYieldVariables(columnNames, input.yieldVariables);
    return input.binder->createVariables(columnNames, columnTypes);
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext* context,
    const TableFuncBindInput* input) {
    std::vector<ConstraintInfo> constraintsInfo;
    auto catalog = Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    for (auto* constraintEntry : catalog->getConstraintEntries(transaction)) {
        auto tableEntry = catalog->getTableCatalogEntry(transaction, constraintEntry->getTableID());
        // The table may have been dropped (dropTableEntry path drops constraints first, but
        // resolve defensively so SHOW never fails). Names are resolved live so RENAME
        // TABLE/PROPERTY is reflected.
        auto tableName = constraintEntry->getTableName();
        auto propertyNames = constraintEntry->getPropertyNames();
        if (tableEntry != nullptr) {
            tableName = tableEntry->getName();
            propertyNames.clear();
            for (auto propertyID : constraintEntry->getPropertyIDs()) {
                propertyNames.push_back(tableEntry->getProperty(propertyID).getName());
            }
        }
        auto tableType = constraintEntry->isRel() ? "REL" : "NODE";
        ConstraintToCypherInfo cypherInfo{context};
        constraintsInfo.emplace_back(constraintEntry->getConstraintName(), std::move(tableName),
            std::move(tableType), std::move(propertyNames), constraintEntry->toCypher(cypherInfo));
    }
    auto numConstraints = constraintsInfo.size();
    return std::make_unique<ShowConstraintsBindData>(std::move(constraintsInfo),
        bindColumns(*input), numConstraints);
}

function_set ShowConstraintsFunction::getFunctionSet() {
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
