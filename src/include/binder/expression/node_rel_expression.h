#pragma once

#include <unordered_map>
#include <unordered_set>

#include "common/case_insensitive_map.h"
#include "expression.h"
#include "property_expression.h"

namespace lbug {
namespace catalog {
class TableCatalogEntry;
}
namespace main {
class ClientContext;
}
namespace binder {

class LBUG_API NodeOrRelExpression : public Expression {
    static constexpr common::ExpressionType expressionType_ = common::ExpressionType::PATTERN;

public:
    NodeOrRelExpression(common::LogicalType dataType, std::string uniqueName,
        std::string variableName, std::vector<catalog::TableCatalogEntry*> entries)
        : Expression{expressionType_, std::move(dataType), std::move(uniqueName)},
          variableName(std::move(variableName)), entries{std::move(entries)} {}

    void setDataType(common::LogicalType dataType) { this->dataType = std::move(dataType); }

    std::string getVariableName() const { return variableName; }

    bool isEmpty() const { return entries.empty(); }
    virtual bool isMultiLabeled() const = 0;

    common::table_id_vector_t getTableIDs() const;
    common::table_id_set_t getTableIDsSet() const;

    // Table entries
    common::idx_t getNumEntries() const { return entries.size(); }
    const std::vector<catalog::TableCatalogEntry*>& getEntries() const { return entries; }
    catalog::TableCatalogEntry* getEntry(common::idx_t idx) const { return entries[idx]; }
    void setEntries(std::vector<catalog::TableCatalogEntry*> entries_) {
        entries = std::move(entries_);
    }
    void addEntries(const std::vector<catalog::TableCatalogEntry*>& entries_);

    // Property expressions
    void addPropertyExpression(std::shared_ptr<PropertyExpression> property);
    bool hasPropertyExpression(const std::string& propertyName) const {
        return propertyNameToIdx.contains(propertyName);
    }
    std::vector<std::shared_ptr<PropertyExpression>> getPropertyExpressions() const {
        return propertyExprs;
    }
    // Property expressions surfaced in whole-object (`RETURN n`) output. Equal to
    // getPropertyExpressions() unless some are hidden (e.g. an ANY graph's internal `id`/`label`
    // columns, which the whole-object struct already exposes as _ID/_LABEL). Hidden properties
    // stay bound for writes, explicit access, and `.*`; only the whole-object struct omits them.
    std::vector<std::shared_ptr<PropertyExpression>> getProjectedPropertyExpressions() const {
        if (hiddenPropertyNames.empty()) {
            return propertyExprs;
        }
        std::vector<std::shared_ptr<PropertyExpression>> result;
        for (auto& property : propertyExprs) {
            if (!hiddenPropertyNames.contains(property->getPropertyName())) {
                result.push_back(property);
            }
        }
        return result;
    }
    void setHiddenPropertyNames(std::unordered_set<std::string> names) {
        hiddenPropertyNames = std::move(names);
    }
    std::shared_ptr<PropertyExpression> getPropertyExpression(
        const std::string& propertyName) const {
        DASSERT(propertyNameToIdx.contains(propertyName));
        return propertyExprs[propertyNameToIdx.at(propertyName)];
    }
    virtual std::shared_ptr<PropertyExpression> getInternalID() const = 0;

    // Label expression
    void setLabelExpression(std::shared_ptr<Expression> expression) {
        labelExpression = std::move(expression);
    }
    std::shared_ptr<Expression> getLabelExpression() const { return labelExpression; }

    // Property data expressions
    void addPropertyDataExpr(std::string propertyName, std::shared_ptr<Expression> expr) {
        propertyDataExprs.insert({propertyName, expr});
    }
    const common::case_insensitive_map_t<std::shared_ptr<Expression>>&
    getPropertyDataExprRef() const {
        return propertyDataExprs;
    }
    bool hasPropertyDataExpr(const std::string& propertyName) const {
        return propertyDataExprs.contains(propertyName);
    }
    std::shared_ptr<Expression> getPropertyDataExpr(const std::string& propertyName) const {
        DASSERT(propertyDataExprs.contains(propertyName));
        return propertyDataExprs.at(propertyName);
    }

    // Database names for attached databases
    void setDbName(catalog::TableCatalogEntry* entry, std::string dbName) {
        dbNames[entry] = std::move(dbName);
    }
    std::string getDbName(catalog::TableCatalogEntry* entry) const {
        auto it = dbNames.find(entry);
        return it != dbNames.end() ? it->second : "";
    }

    // Original labels from the node/rel pattern (for ANY graphs)
    void setOriginalLabels(std::vector<std::string> labels) { originalLabels = std::move(labels); }
    std::vector<std::string> getOriginalLabels() const { return originalLabels; }

    std::string toStringInternal() const final { return variableName; }

protected:
    std::string variableName;
    // A pattern may bind to multiple tables.
    std::vector<catalog::TableCatalogEntry*> entries;
    // Index over propertyExprs on property name.
    common::case_insensitive_map_t<common::idx_t> propertyNameToIdx;
    // Property expressions with order (aligned with catalog).
    std::vector<std::shared_ptr<PropertyExpression>> propertyExprs;
    // Label expression
    std::shared_ptr<Expression> labelExpression;
    // Property data expressions specified by user in the form of "{propertyName : data}"
    common::case_insensitive_map_t<std::shared_ptr<Expression>> propertyDataExprs;
    // Database names for table entries from attached databases
    std::unordered_map<catalog::TableCatalogEntry*, std::string> dbNames;
    // Original labels from the node/rel pattern (for ANY graphs)
    std::vector<std::string> originalLabels;
    // Property names hidden from the whole-object struct (e.g. ANY internal `id`/`label`).
    std::unordered_set<std::string> hiddenPropertyNames;
};

// True when the node/rel is backed by an ANY graph's internal `_nodes`/`_edges` table.
bool isAnyGraphNodeOrRel(const NodeOrRelExpression& nodeOrRel, main::ClientContext* context);

} // namespace binder
} // namespace lbug
