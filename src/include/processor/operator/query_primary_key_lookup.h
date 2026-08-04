#pragma once

#include "expression_evaluator/expression_evaluator.h"
#include "processor/operator/filtering_operator.h"
#include "processor/operator/physical_operator.h"
#include "processor/operator/scan/scan_node_table.h"

namespace lbug {
namespace processor {

struct QueryPrimaryKeyLookupPrintInfo final : OPPrintInfo {
    std::string tableName;
    std::string key;
    std::string alias;
    binder::expression_vector properties;

    QueryPrimaryKeyLookupPrintInfo(std::string tableName, std::string key, std::string alias,
        binder::expression_vector properties)
        : tableName{std::move(tableName)}, key{std::move(key)}, alias{std::move(alias)},
          properties{std::move(properties)} {}

    std::string toString() const override;

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<QueryPrimaryKeyLookupPrintInfo>(
            new QueryPrimaryKeyLookupPrintInfo(*this));
    }

private:
    QueryPrimaryKeyLookupPrintInfo(const QueryPrimaryKeyLookupPrintInfo& other)
        : OPPrintInfo{other}, tableName{other.tableName}, key{other.key}, alias{other.alias},
          properties{other.properties} {}
};

class QueryPrimaryKeyLookup final : public PhysicalOperator, SelVectorOverWriter {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP;

public:
    QueryPrimaryKeyLookup(storage::NodeTable* table, ScanOpInfo opInfo, ScanNodeTableInfo tableInfo,
        std::unique_ptr<evaluator::ExpressionEvaluator> keyEvaluator,
        std::unique_ptr<PhysicalOperator> child, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, std::move(child), id, std::move(printInfo)}, table{table},
          opInfo{std::move(opInfo)}, tableInfo{std::move(tableInfo)},
          keyEvaluator{std::move(keyEvaluator)} {}

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;
    bool getNextTuplesInternal(ExecutionContext* context) final;

    std::unique_ptr<PhysicalOperator> copy() final {
        return std::make_unique<QueryPrimaryKeyLookup>(table, opInfo.copy(), tableInfo.copy(),
            keyEvaluator->copy(), children[0]->copy(), getOperatorID(), printInfo->copy());
    }

private:
    storage::NodeTable* table;
    ScanOpInfo opInfo;
    ScanNodeTableInfo tableInfo;
    std::unique_ptr<evaluator::ExpressionEvaluator> keyEvaluator;
    common::ValueVector* nodeIDVector = nullptr;
    std::vector<common::ValueVector*> outVectors;
    std::unique_ptr<storage::TableScanState> scanState;
};

} // namespace processor
} // namespace lbug
