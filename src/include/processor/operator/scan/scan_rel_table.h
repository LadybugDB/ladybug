#pragma once

#include "binder/expression/rel_expression.h"
#include "common/enums/extend_direction.h"
#include "processor/operator/scan/scan_node_table.h"
#include "processor/operator/scan/scan_table.h"
#include "storage/predicate/column_predicate.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace storage {
class MemoryManager;
class NodeTable;
} // namespace storage
namespace processor {

struct ScanRelTableInfo : ScanTableInfo {
    common::RelDataDirection direction;

    ScanRelTableInfo(storage::Table* table,
        std::vector<storage::ColumnPredicateSet> columnPredicates,
        common::RelDataDirection direction)
        : ScanTableInfo{table, std::move(columnPredicates)}, direction{direction} {}
    EXPLICIT_COPY_DEFAULT_MOVE(ScanRelTableInfo);

    void initScanState(storage::TableScanState& scanState,
        const std::vector<common::ValueVector*>& outVectors, main::ClientContext* context) override;

private:
    ScanRelTableInfo(const ScanRelTableInfo& other)
        : ScanTableInfo{other}, direction{other.direction} {}
};

struct ScanRelTablePrintInfo final : OPPrintInfo {
    std::vector<std::string> tableNames;
    binder::expression_vector properties;
    std::shared_ptr<binder::NodeExpression> boundNode;
    std::shared_ptr<binder::RelExpression> rel;
    std::shared_ptr<binder::NodeExpression> nbrNode;
    common::ExtendDirection direction;
    std::string alias;

    ScanRelTablePrintInfo(std::vector<std::string> tableNames, binder::expression_vector properties,
        std::shared_ptr<binder::NodeExpression> boundNode,
        std::shared_ptr<binder::RelExpression> rel, std::shared_ptr<binder::NodeExpression> nbrNode,
        common::ExtendDirection direction, std::string alias)
        : tableNames{std::move(tableNames)}, properties{std::move(properties)},
          boundNode{std::move(boundNode)}, rel{std::move(rel)}, nbrNode{std::move(nbrNode)},
          direction{direction}, alias{std::move(alias)} {}

    std::string toString() const override;

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<ScanRelTablePrintInfo>(new ScanRelTablePrintInfo(*this));
    }

private:
    ScanRelTablePrintInfo(const ScanRelTablePrintInfo& other)
        : OPPrintInfo{other}, tableNames{other.tableNames}, properties{other.properties},
          boundNode{other.boundNode}, rel{other.rel}, nbrNode{other.nbrNode},
          direction{other.direction}, alias{other.alias} {}
};

class ScanRelTable final : public ScanTable {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::SCAN_REL_TABLE;

public:
    ScanRelTable(ScanOpInfo info, ScanRelTableInfo tableInfo,
        std::unique_ptr<PhysicalOperator> child, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo, PhysicalOperatorType operatorType = type_)
        : ScanTable{operatorType, std::move(info), std::move(child), id, std::move(printInfo)},
          tableInfo{std::move(tableInfo)}, sourceNodeScanInfo{DataPos::getInvalidPos(), {}} {}

    ScanRelTable(ScanOpInfo info, ScanRelTableInfo tableInfo,
        std::vector<storage::NodeTable*> sourceNodeTables, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo, PhysicalOperatorType operatorType = type_)
        : ScanTable{operatorType, std::move(info), id, std::move(printInfo)},
          tableInfo{std::move(tableInfo)}, sourceNodeTables{std::move(sourceNodeTables)},
          sourceNodeScanInfo{DataPos::getInvalidPos(), {}}, sourceMode{true} {}

    ScanRelTable(ScanOpInfo info, ScanRelTableInfo tableInfo,
        std::vector<ScanNodeTableInfo> sourceNodeTableInfos,
        std::vector<std::shared_ptr<ScanNodeTableSharedState>> sourceNodeSharedStates,
        std::shared_ptr<ScanNodeTableProgressSharedState> sourceNodeProgressSharedState,
        ScanOpInfo sourceNodeScanInfo, physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo,
        PhysicalOperatorType operatorType = type_)
        : ScanTable{operatorType, std::move(info), id, std::move(printInfo)},
          tableInfo{std::move(tableInfo)}, sourceNodeTableInfos{std::move(sourceNodeTableInfos)},
          sourceNodeSharedStates{std::move(sourceNodeSharedStates)},
          sourceNodeProgressSharedState{std::move(sourceNodeProgressSharedState)},
          sourceNodeScanInfo{std::move(sourceNodeScanInfo)}, sourceMode{true},
          sourceNodeScanMode{true} {}

    bool isSource() const override { return sourceMode; }
    bool isParallel() const override { return !sourceMode; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        if (sourceMode) {
            if (sourceNodeScanMode) {
                return std::make_unique<ScanRelTable>(opInfo.copy(), tableInfo.copy(),
                    copyVector(sourceNodeTableInfos), sourceNodeSharedStates,
                    sourceNodeProgressSharedState, sourceNodeScanInfo.copy(), id, printInfo->copy(),
                    operatorType);
            }
            return std::make_unique<ScanRelTable>(opInfo.copy(), tableInfo.copy(), sourceNodeTables,
                id, printInfo->copy(), operatorType);
        }
        return std::make_unique<ScanRelTable>(opInfo.copy(), tableInfo.copy(), children[0]->copy(),
            id, printInfo->copy(), operatorType);
    }

protected:
    void initGlobalStateInternal(ExecutionContext* context) override;
    bool fetchNextBoundNodeBatch(transaction::Transaction* transaction);
    void updatePackedChildSlices(common::sel_t outputSize) const;
    // Pre-allocate packedChildSlices for the current input batch. The number of parents that will
    // be processed in this batch is known up front from cachedBoundNodeSelVector, so we reserve
    // once per batch to keep subsequent appendPackedChildSlice() calls reallocation-free.
    void reservePackedChildSlicesForBatch() const;

protected:
    ScanRelTableInfo tableInfo;
    std::unique_ptr<storage::RelTableScanState> scanState;
    std::vector<storage::NodeTable*> sourceNodeTables;
    std::vector<ScanNodeTableInfo> sourceNodeTableInfos;
    std::vector<std::shared_ptr<ScanNodeTableSharedState>> sourceNodeSharedStates;
    std::shared_ptr<ScanNodeTableProgressSharedState> sourceNodeProgressSharedState;
    ScanOpInfo sourceNodeScanInfo;
    std::unique_ptr<storage::TableScanState> sourceNodeScanState;
    std::vector<common::ValueVector*> sourceNodeOutVectors;
    bool sourceMode = false;
    bool sourceNodeScanMode = false;
    common::idx_t currentSourceTableIdx = 0;
    common::offset_t nextSourceOffset = 0;
    common::row_idx_t currentSourceTableNumRows = 0;
};

} // namespace processor
} // namespace lbug
