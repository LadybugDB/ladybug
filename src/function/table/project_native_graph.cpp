#include <algorithm>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/value/nested.h"
#include "function/gds/gds.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/standalone_call_function.h"
#include "graph/graph_entry_set.h"
#include "main/connection.h"
#include "main/database.h"
#include "main/query_result/arrow_query_result.h"
#include "parser/parser.h"
#include "processor/execution_context.h"
#include "storage/storage_manager.h"
#include "storage/table/table.h"
#include "transaction/transaction_context.h"
#include <format>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::catalog;
using namespace lbug::graph;

namespace lbug {
namespace function {

struct ProjectGraphNativeBindData final : TableFuncBindData {
    std::string graphName;
    std::vector<ParsedNativeGraphTableInfo> nodeInfos;
    std::vector<ParsedNativeGraphTableInfo> relInfos;

    ProjectGraphNativeBindData(std::string graphName,
        std::vector<ParsedNativeGraphTableInfo> nodeInfos,
        std::vector<ParsedNativeGraphTableInfo> relInfos)
        : TableFuncBindData{0}, graphName{std::move(graphName)}, nodeInfos{std::move(nodeInfos)},
          relInfos{std::move(relInfos)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ProjectGraphNativeBindData>(graphName, nodeInfos, relInfos);
    }
};

// Materialize each projected rel table as arrow CSR by running the projection scan through the
// arrow CSR collector (queryAsArrow tracks CSR for the MATCH..RETURN rowid shape with no row
// materialization). The result is pinned on the entry for GDS consumers to wrap zero-copy.
// Fallback-first: any condition the CSR can't faithfully represent yet (multiple node tables,
// per-table predicates) or where an internal read connection would not see the caller's data
// (manual transaction: uncommitted writes are invisible to the inner connection) skips
// materialization; consumers fall back to scanning storage.
static void materializeRelCsr(ParsedNativeGraphEntry& entry, main::ClientContext* context) {
    if (entry.nodeInfos.size() != 1) {
        return;
    }
    const auto anyPredicate = [](const ParsedNativeGraphTableInfo& info) {
        return !info.predicate.empty();
    };
    if (std::any_of(entry.nodeInfos.begin(), entry.nodeInfos.end(), anyPredicate) ||
        std::any_of(entry.relInfos.begin(), entry.relInfos.end(), anyPredicate)) {
        return;
    }
    if (!transaction::TransactionContext::Get(*context)->isAutoTransaction()) {
        return;
    }
    static constexpr int64_t ARROW_CHUNK_SIZE = 1 << 16;
    const auto& nodeTable = entry.nodeInfos[0].tableName;
    main::Connection conn{context->getDatabase()};
    entry.relCsrResults.reserve(entry.relInfos.size());
    entry.relCsrEpochs.reserve(entry.relInfos.size());
    for (const auto& relInfo : entry.relInfos) {
        // Capture the rel table's change epoch BEFORE the scan: a mutation racing the
        // materialization bumps the epoch, so consumers see a mismatch and fall back.
        uint64_t epoch = 0;
        const auto* relEntry = catalog::Catalog::Get(*context)->getTableCatalogEntry(
            transaction::Transaction::Get(*context), relInfo.tableName);
        const auto& relGroup = relEntry->constCast<catalog::RelGroupCatalogEntry>();
        if (!relGroup.getRelEntryInfos().empty()) {
            const auto* relTable = storage::StorageManager::Get(*context)->getTable(
                relGroup.getRelEntryInfos()[0].oid);
            if (relTable != nullptr) {
                epoch = relTable->getChangeEpoch();
            }
        }
        const auto quotedNodeTable = common::StringUtils::quoteIdentifier(nodeTable);
        auto query =
            std::format("MATCH (a:{})-[r:{}]->(b:{}) RETURN a.rowid, b.rowid", quotedNodeTable,
                common::StringUtils::quoteIdentifier(relInfo.tableName), quotedNodeTable);
        auto result = conn.queryAsArrow(query, ARROW_CHUNK_SIZE);
        auto* arrowResult = dynamic_cast<main::ArrowQueryResult*>(result.get());
        if (arrowResult != nullptr && arrowResult->isSuccess() && arrowResult->hasCSRMetadata()) {
            entry.relCsrResults.push_back(std::shared_ptr<main::QueryResult>{std::move(result)});
            entry.relCsrEpochs.push_back(epoch);
        } else {
            // Shape not tracked (or scan failed): this rel stays unmaterialized.
            // Record the captured epoch anyway — it is the table's change epoch at projection
            // time, even without a materialized CSR. Consumers check relCsrResults[i] for null
            // to decide whether to use the pinned CSR or fall back to scanning storage.
            entry.relCsrResults.push_back(nullptr);
            entry.relCsrEpochs.push_back(epoch);
        }
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    const auto bindData = dynamic_cast_checked<ProjectGraphNativeBindData*>(input.bindData);
    auto clientContext = input.context->clientContext;
    auto graphEntrySet = GraphEntrySet::Get(*clientContext);
    graphEntrySet->validateGraphNotExist(bindData->graphName);
    auto entry = std::make_unique<ParsedNativeGraphEntry>(bindData->nodeInfos, bindData->relInfos);
    // bind graph entry to check if input is valid or not. Ignore bind result.
    GDSFunction::bindGraphEntry(*clientContext, *entry);
    materializeRelCsr(*entry, clientContext);
    graphEntrySet->addGraph(bindData->graphName, std::move(entry));
    return 0;
}

static std::string getStringVal(const Value& value) {
    value.validateType(LogicalTypeID::STRING);
    return value.getValue<std::string>();
}

static std::vector<ParsedNativeGraphTableInfo> extractGraphEntryTableInfos(const Value& value) {
    std::vector<ParsedNativeGraphTableInfo> infos;
    switch (value.getDataType().getLogicalTypeID()) {
    case LogicalTypeID::LIST: {
        for (auto i = 0u; i < NestedVal::getChildrenSize(&value); ++i) {
            auto tableName = getStringVal(*NestedVal::getChildVal(&value, i));
            infos.emplace_back(tableName, "" /* empty predicate */);
        }
    } break;
    case LogicalTypeID::STRUCT: {
        for (auto i = 0u; i < StructType::getNumFields(value.getDataType()); ++i) {
            auto& field = StructType::getField(value.getDataType(), i);
            auto tableName = field.getName();
            auto predicate = getStringVal(*NestedVal::getChildVal(&value, i));
            infos.emplace_back(tableName, predicate);
        }
    } break;
    default:
        throw BinderException(
            std::format("Argument {} has data type {}. LIST or STRUCT was expected.",
                value.toString(), value.getDataType().toString()));
    }
    return infos;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext*,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto nodeInfos = extractGraphEntryTableInfos(input->getValue(1));
    auto relInfos = extractGraphEntryTableInfos(input->getValue(2));
    return std::make_unique<ProjectGraphNativeBindData>(graphName, nodeInfos, relInfos);
}

function_set ProjectGraphNativeFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::ANY, LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = TableFunction::initEmptySharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = []() { return false; };
    functionSet.push_back(std::move(func));
    return functionSet;
}

} // namespace function
} // namespace lbug
