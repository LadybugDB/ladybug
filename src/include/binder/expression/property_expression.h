#pragma once

#include "common/constants.h"
#include "expression.h"

namespace lbug {
namespace catalog {
class TableCatalogEntry;
}
namespace binder {

struct SingleLabelPropertyInfo {
    bool exists;
    bool isPrimaryKey;

    explicit SingleLabelPropertyInfo(bool exists, bool isPrimaryKey)
        : exists{exists}, isPrimaryKey{isPrimaryKey} {}
    EXPLICIT_COPY_DEFAULT_MOVE(SingleLabelPropertyInfo);

private:
    SingleLabelPropertyInfo(const SingleLabelPropertyInfo& other)
        : exists{other.exists}, isPrimaryKey{other.isPrimaryKey} {}
};

class LBUG_API PropertyExpression final : public Expression {
    static constexpr common::ExpressionType expressionType_ = common::ExpressionType::PROPERTY;

public:
    PropertyExpression(common::LogicalType dataType, std::string propertyName,
        std::string uniqueVarName, std::string rawVariableName,
        common::table_id_map_t<SingleLabelPropertyInfo> infos)
        : Expression{expressionType_, std::move(dataType), uniqueVarName + "." + propertyName},
          propertyName{std::move(propertyName)}, uniqueVarName{std::move(uniqueVarName)},
          rawVariableName{std::move(rawVariableName)}, infos{std::move(infos)} {}

    PropertyExpression(const PropertyExpression& other)
        : Expression{expressionType_, other.dataType.copy(), other.uniqueName},
          propertyName{other.propertyName}, uniqueVarName{other.uniqueVarName},
          rawVariableName{other.rawVariableName}, infos{copyUnorderedMap(other.infos)} {}

    // If this property is the primary key of the single table this variable spans, so that it
    // uniquely identifies one row of a scan over the variable. A variable spanning several node
    // tables can hold the same key in each of them, so this is false for it even when the
    // property is the primary key of every one of those tables; ask isPrimaryKey(tableID) for
    // the per-table schema question.
    bool isPrimaryKey() const;
    // If this property is primary key for given table.
    bool isPrimaryKey(common::table_id_t tableID) const;

    std::string getPropertyName() const { return propertyName; }
    std::string getVariableName() const { return uniqueVarName; }
    std::string getRawVariableName() const { return rawVariableName; }

    // If this property exists for given table.
    bool hasProperty(common::table_id_t tableID) const;

    // common::column_id_t getColumnID(const catalog::TableCatalogEntry& entry) const;
    bool isSingleLabel() const { return infos.size() == 1; }
    common::table_id_t getSingleTableID() const { return infos.begin()->first; }

    bool isInternalID() const { return getPropertyName() == common::InternalKeyword::ID; }

    std::string toStringInternal() const override { return rawVariableName + "." + propertyName; }

    std::unique_ptr<PropertyExpression> copy() const {
        return std::make_unique<PropertyExpression>(*this);
    }

private:
    std::string propertyName;
    // unique identifier references to a node/rel table.
    std::string uniqueVarName;
    // printable identifier references to a node/rel table.
    std::string rawVariableName;
    // The same property name may have different info on each table.
    common::table_id_map_t<SingleLabelPropertyInfo> infos;
};

} // namespace binder
} // namespace lbug
