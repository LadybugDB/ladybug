#pragma once

#include "common/types/types.h"
#include "optional_params.h"
#include "storage/predicate/column_predicate.h"

namespace lbug {
namespace common {
class FileSystem;
}

namespace function {

struct LBUG_API TableFuncBindData {
    binder::expression_vector columns;
    common::row_idx_t numRows;
    std::unique_ptr<OptionalParams> optionalParams = nullptr;

    TableFuncBindData() : numRows{0} {}
    explicit TableFuncBindData(common::row_idx_t numRows) : numRows{numRows} {}
    explicit TableFuncBindData(binder::expression_vector columns)
        : columns{std::move(columns)}, numRows{0} {}
    TableFuncBindData(binder::expression_vector columns, common::row_idx_t numRows)
        : columns{std::move(columns)}, numRows{numRows} {}
    TableFuncBindData(const TableFuncBindData& other)
        : columns{other.columns}, numRows{other.numRows},
          optionalParams{other.optionalParams == nullptr ? nullptr : other.optionalParams->copy()},
          columnSkips{other.columnSkips}, columnPredicates{copyVector(other.columnPredicates)},
          limitNum{other.limitNum}, orderBy{other.orderBy} {}
    TableFuncBindData& operator=(const TableFuncBindData& other) = delete;
    virtual ~TableFuncBindData() = default;

    void evaluateParams(main::ClientContext* context) const {
        if (!optionalParams) {
            return;
        }
        optionalParams->evaluateParams(context);
    }
    common::idx_t getNumColumns() const { return columns.size(); }
    void setColumnSkips(std::vector<bool> skips) { columnSkips = std::move(skips); }
    std::vector<bool> getColumnSkips() const;

    void setColumnPredicates(std::vector<storage::ColumnPredicateSet> predicates) {
        columnPredicates = std::move(predicates);
    }
    const std::vector<storage::ColumnPredicateSet>& getColumnPredicates() const {
        return columnPredicates;
    }

    void setLimitNum(common::row_idx_t limit) { limitNum = limit; }
    common::row_idx_t getLimitNum() const { return limitNum; }

    void setOrderBy(std::string orderBy) { this->orderBy = orderBy; }
    std::string getOrderBy() const { return orderBy; }

    virtual bool getIgnoreErrorsOption() const;

    virtual std::unique_ptr<TableFuncBindData> copy() const;

    // Create a copy of this bind data with a modified SQL query and output columns.
    // This is used by the foreign join push down optimizer to rewrite queries.
    // Returns nullptr if the bind data doesn't support query modification.
    virtual std::unique_ptr<TableFuncBindData> copyWithQuery(const std::string& query,
        binder::expression_vector columns,
        const std::vector<std::string>& columnNamesInResult) const {
        (void)query;
        (void)columns;
        (void)columnNamesInResult;
        return nullptr;
    }

    virtual std::string getDescription() const { return ""; }

    template<class TARGET>
    const TARGET* constPtrCast() const {
        return common::ku_dynamic_cast<const TARGET*>(this);
    }

    template<class TARGET>
    TARGET& cast() {
        return *common::ku_dynamic_cast<TARGET*>(this);
    }

protected:
    std::vector<bool> columnSkips;
    std::vector<storage::ColumnPredicateSet> columnPredicates;
    common::row_idx_t limitNum = common::INVALID_ROW_IDX;
    std::string orderBy;
};

} // namespace function
} // namespace lbug
