#pragma once

#include "binder/bound_scan_source.h"
#include "binder/expression/expression.h"
#include "binder/expression/literal_expression.h"
#include "common/enums/column_evaluate_type.h"
#include "common/enums/table_type.h"
#include "index_look_up_info.h"

namespace lbug {
namespace binder {

struct ExtraBoundCopyFromInfo {
    virtual ~ExtraBoundCopyFromInfo() = default;
    virtual std::unique_ptr<ExtraBoundCopyFromInfo> copy() const = 0;

    template<class TARGET>
    const TARGET& constCast() const {
        return common::dynamic_cast_checked<const TARGET&>(*this);
    }
};

struct LBUG_API BoundCopyFromInfo {
    // Name of table to copy into.
    std::string tableName;
    // Type of table.
    common::TableType tableType;
    // Data source.
    std::unique_ptr<BoundBaseScanSource> source;
    // Row offset.
    std::shared_ptr<Expression> offset;
    expression_vector columnExprs;
    std::vector<common::ColumnEvaluateType> columnEvaluateTypes;
    std::unique_ptr<ExtraBoundCopyFromInfo> extraInfo;
    bool skipDuplicatePK;

    BoundCopyFromInfo(std::string tableName, common::TableType tableType,
        std::unique_ptr<BoundBaseScanSource> source, std::shared_ptr<Expression> offset,
        expression_vector columnExprs, std::vector<common::ColumnEvaluateType> columnEvaluateTypes,
        std::unique_ptr<ExtraBoundCopyFromInfo> extraInfo, bool skipDuplicatePK = false)
        : tableName{std::move(tableName)}, tableType{tableType}, source{std::move(source)},
          offset{std::move(offset)}, columnExprs{std::move(columnExprs)},
          columnEvaluateTypes{std::move(columnEvaluateTypes)}, extraInfo{std::move(extraInfo)},
          skipDuplicatePK{skipDuplicatePK} {}

    EXPLICIT_COPY_DEFAULT_MOVE(BoundCopyFromInfo);

    expression_vector getSourceColumns() const {
        return source ? source->getColumns() : expression_vector{};
    }
    expression_vector getWarningColumns() const {
        return offset ? source->getWarningColumns() : expression_vector{};
    }

    bool getIgnoreErrorsOption() const { return source ? source->getIgnoreErrorsOption() : false; }
    bool getSkipDuplicatePKOption() const { return skipDuplicatePK; }

private:
    BoundCopyFromInfo(const BoundCopyFromInfo& other)
        : tableName{other.tableName}, tableType{other.tableType}, offset{other.offset},
          columnExprs{other.columnExprs}, columnEvaluateTypes{other.columnEvaluateTypes},
          skipDuplicatePK{other.skipDuplicatePK} {
        source = other.source ? other.source->copy() : nullptr;
        if (other.extraInfo) {
            extraInfo = other.extraInfo->copy();
        }
    }
};

struct ExtraBoundCopyRelInfo final : ExtraBoundCopyFromInfo {
    std::string fromTableName;
    std::string toTableName;
    // We process internal ID column as offset (INT64) column until partitioner. In partitioner,
    // we need to manually change offset(INT64) type to internal ID type.
    std::vector<common::idx_t> internalIDColumnIndices;
    std::vector<IndexLookupInfo> infos;

    ExtraBoundCopyRelInfo(std::string fromTableName, std::string toTableName,
        std::vector<common::idx_t> internalIDColumnIndices, std::vector<IndexLookupInfo> infos)
        : fromTableName{std::move(fromTableName)}, toTableName{std::move(toTableName)},
          internalIDColumnIndices{std::move(internalIDColumnIndices)}, infos{std::move(infos)} {}
    ExtraBoundCopyRelInfo(const ExtraBoundCopyRelInfo& other)
        : fromTableName{other.fromTableName}, toTableName{other.toTableName},
          internalIDColumnIndices{other.internalIDColumnIndices}, infos{other.infos} {}

    std::unique_ptr<ExtraBoundCopyFromInfo> copy() const override {
        return std::make_unique<ExtraBoundCopyRelInfo>(*this);
    }
};

class BoundCopyFrom final : public BoundStatement {
    static constexpr common::StatementType statementType_ = common::StatementType::COPY_FROM;

public:
    explicit BoundCopyFrom(BoundCopyFromInfo info)
        : BoundStatement{statementType_,
              info.getSkipDuplicatePKOption() ?
                  createSkipDuplicatePKResult() :
                  BoundStatementResult::createSingleStringColumnResult()},
          info{std::move(info)} {}

    const BoundCopyFromInfo* getInfo() const { return &info; }

private:
    static BoundStatementResult createSkipDuplicatePKResult() {
        auto result = BoundStatementResult::createSingleStringColumnResult();
        auto skippedCount = std::make_shared<LiteralExpression>(common::Value{int64_t(0)},
            "skipped_duplicate_pk_count");
        auto skippedPKs = std::make_shared<LiteralExpression>(
            common::Value{common::LogicalType::LIST(common::LogicalType::STRING()),
                std::vector<std::unique_ptr<common::Value>>{}},
            "skipped_duplicate_pks");
        result.addColumn("skipped_duplicate_pk_count", skippedCount);
        result.addColumn("skipped_duplicate_pks", skippedPKs);
        return result;
    }

    BoundCopyFromInfo info;
};

} // namespace binder
} // namespace lbug
