#include "storage/predicate/column_predicate.h"

#include "binder/expression/literal_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "storage/predicate/constant_predicate.h"
#include "storage/predicate/null_predicate.h"
#include <format>

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace storage {

ZoneMapCheckResult ColumnPredicateSet::checkZoneMap(const MergedColumnChunkStats& stats) const {
    for (auto& predicate : predicates) {
        if (predicate->checkZoneMap(stats) == ZoneMapCheckResult::SKIP_SCAN) {
            return ZoneMapCheckResult::SKIP_SCAN;
        }
    }
    return ZoneMapCheckResult::ALWAYS_SCAN;
}

void ColumnPredicateSet::filter(const ValueVector& vector, SelectionVector& sel) const {
    for (auto& predicate : predicates) {
        const auto type = predicate->getExpressionType();
        if (type == ExpressionType::IS_NULL || type == ExpressionType::IS_NOT_NULL) {
            predicate->constCast<ColumnNullPredicate>().filter(vector, sel);
        } else {
            predicate->constCast<ColumnConstantPredicate>().filter(vector, sel);
        }
    }
}

std::string ColumnPredicateSet::toString() const {
    if (predicates.empty()) {
        return {};
    }
    auto result = predicates[0]->toString();
    for (auto i = 1u; i < predicates.size(); ++i) {
        result += std::format(" AND {}", predicates[i]->toString());
    }
    return result;
}

static bool isColumnRef(ExpressionType type) {
    return type == ExpressionType::PROPERTY || type == ExpressionType::VARIABLE;
}

static bool isIntegerUpTo64Bits(LogicalTypeID type) {
    return LogicalTypeUtils::isIntegral(type) && type != LogicalTypeID::INT128 &&
           type != LogicalTypeID::UINT128;
}

// The pushed-down predicate compares the column's stats, read as the cast's target type, with
// the literal. That is only right when the cast keeps every value and its order in the same
// stats slot: the same type, integer widening up to 64 bits (an unsigned value reads the same
// from the signed slot of a wider type) and FLOAT to DOUBLE. 128-bit targets are excluded because
// a negative 64-bit value is stored without sign extension into the high half.
static bool castKeepsStats(const LogicalType& from, const LogicalType& to) {
    if (from == to) {
        return true;
    }
    if (from.getLogicalTypeID() == LogicalTypeID::FLOAT) {
        return to.getLogicalTypeID() == LogicalTypeID::DOUBLE;
    }
    if (!isIntegerUpTo64Bits(from.getLogicalTypeID()) ||
        !isIntegerUpTo64Bits(to.getLogicalTypeID())) {
        return false;
    }
    auto fromSize = PhysicalTypeUtils::getFixedTypeSize(from.getPhysicalType());
    auto toSize = PhysicalTypeUtils::getFixedTypeSize(to.getPhysicalType());
    if (LogicalTypeUtils::isUnsigned(to)) {
        return LogicalTypeUtils::isUnsigned(from) && fromSize <= toSize;
    }
    return LogicalTypeUtils::isUnsigned(from) ? fromSize < toSize : fromSize <= toSize;
}

static bool isCastedColumnRef(const Expression& expr) {
    if (expr.expressionType == ExpressionType::FUNCTION) {
        const auto& funcExpr = expr.constCast<ScalarFunctionExpression>();
        if (funcExpr.getFunction().name.starts_with("CAST")) {
            DASSERT(funcExpr.getNumChildren() > 0);
            const auto& child = *funcExpr.getChild(0);
            return isColumnRef(child.expressionType) &&
                   castKeepsStats(child.getDataType(), expr.getDataType());
        }
    }
    return false;
}

static bool isColumnOrCastedColumnRef(const Expression& expr) {
    return isColumnRef(expr.expressionType) || isCastedColumnRef(expr);
}

static bool isColumnRefConstantPair(const Expression& left, const Expression& right) {
    return isColumnOrCastedColumnRef(left) && right.expressionType == ExpressionType::LITERAL;
}

static bool columnMatchesExprChild(const Expression& column, const Expression& expr) {
    return (expr.getNumChildren() > 0 && column == *expr.getChild(0));
}

static std::unique_ptr<ColumnPredicate> tryConvertToConstColumnPredicate(const Expression& column,
    const Expression& predicate) {
    if (isColumnRefConstantPair(*predicate.getChild(0), *predicate.getChild(1))) {
        if (column != *predicate.getChild(0) &&
            !columnMatchesExprChild(column, *predicate.getChild(0))) {
            return nullptr;
        }
        auto value = predicate.getChild(1)->constCast<LiteralExpression>().getValue();
        return std::make_unique<ColumnConstantPredicate>(column.toString(),
            predicate.expressionType, value);
    } else if (isColumnRefConstantPair(*predicate.getChild(1), *predicate.getChild(0))) {
        if (column != *predicate.getChild(1) &&
            !columnMatchesExprChild(column, *predicate.getChild(1))) {
            return nullptr;
        }
        auto value = predicate.getChild(0)->constCast<LiteralExpression>().getValue();
        auto expressionType =
            ExpressionTypeUtil::reverseComparisonDirection(predicate.expressionType);
        return std::make_unique<ColumnConstantPredicate>(column.toString(), expressionType, value);
    }
    // Not a predicate that runs on this property.
    return nullptr;
}

static std::unique_ptr<ColumnPredicate> tryConvertToIsNull(const Expression& column,
    const Expression& predicate) {
    // we only convert simple predicates
    if (isColumnOrCastedColumnRef(*predicate.getChild(0)) && column == *predicate.getChild(0)) {
        return std::make_unique<ColumnNullPredicate>(column.toString(), ExpressionType::IS_NULL);
    }
    return nullptr;
}

static std::unique_ptr<ColumnPredicate> tryConvertToIsNotNull(const Expression& column,
    const Expression& predicate) {
    if (isColumnOrCastedColumnRef(*predicate.getChild(0)) && column == *predicate.getChild(0)) {
        return std::make_unique<ColumnNullPredicate>(column.toString(),
            ExpressionType::IS_NOT_NULL);
    }
    return nullptr;
}

std::unique_ptr<ColumnPredicate> ColumnPredicateUtil::tryConvert(const Expression& property,
    const Expression& predicate) {
    if (ExpressionTypeUtil::isComparison(predicate.expressionType)) {
        return tryConvertToConstColumnPredicate(property, predicate);
    }
    switch (predicate.expressionType) {
    case common::ExpressionType::IS_NULL:
        return tryConvertToIsNull(property, predicate);
    case common::ExpressionType::IS_NOT_NULL:
        return tryConvertToIsNotNull(property, predicate);
    default:
        return nullptr;
    }
}

std::string ColumnPredicate::toString() {
    return std::format("{} {}", columnName, ExpressionTypeUtil::toParsableString(expressionType));
}

} // namespace storage
} // namespace lbug
