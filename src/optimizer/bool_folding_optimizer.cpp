#include "optimizer/bool_folding_optimizer.h"

#include <string>
#include <unordered_set>

#include "binder/expression/literal_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "common/enums/expression_type.h"
#include "common/types/value/value.h"
#include "function/boolean/vector_boolean_functions.h"
#include "function/scalar_function.h"
#include "planner/operator/logical_empty_result.h"
#include "planner/operator/logical_filter.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::function;

namespace lbug {
namespace optimizer {

// ---------------------------------------------------------------------------
// Helpers: detect complementary predicates
// ---------------------------------------------------------------------------

/// Whether IS_NULL(x) and IS_NOT_NULL(x) are both among the operands: exactly one of them is
/// true, never NULL.
static bool hasNullCheckComplements(const expression_vector& operands) {
    std::unordered_set<std::string> isNull, isNotNull;
    for (auto& operand : operands) {
        if (operand->getNumChildren() == 0) {
            continue;
        }
        if (operand->expressionType == ExpressionType::IS_NULL) {
            auto name = operand->getChild(0)->getUniqueName();
            if (isNotNull.contains(name)) {
                return true;
            }
            isNull.insert(std::move(name));
        } else if (operand->expressionType == ExpressionType::IS_NOT_NULL) {
            auto name = operand->getChild(0)->getUniqueName();
            if (isNull.contains(name)) {
                return true;
            }
            isNotNull.insert(std::move(name));
        }
    }
    return false;
}

/// Whether p and NOT(p) are both among the operands: never both true, but both NULL when p is.
static bool hasNotComplements(const expression_vector& operands) {
    std::unordered_set<std::string> names, negated;
    for (auto& operand : operands) {
        auto name = operand->getUniqueName();
        if (negated.contains(name)) {
            return true;
        }
        if (operand->expressionType == ExpressionType::NOT && operand->getNumChildren() == 1) {
            auto childName = operand->getChild(0)->getUniqueName();
            if (names.contains(childName)) {
                return true;
            }
            negated.insert(std::move(childName));
        }
        names.insert(std::move(name));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helper: build a boolean literal expression
// ---------------------------------------------------------------------------

static std::shared_ptr<Expression> boolLiteral(bool v) {
    return std::make_shared<LiteralExpression>(Value(v),
        std::string("_lit_") + (v ? "true" : "false"));
}

// ---------------------------------------------------------------------------
// Helper: build AND / OR / NOT over already-boolean children
// ---------------------------------------------------------------------------

// The children must already be BOOL: unlike the binder, this skips implicit casts.
static std::shared_ptr<Expression> boolOperator(ExpressionType type, expression_vector children) {
    for ([[maybe_unused]] auto& child : children) {
        DASSERT(child->getDataType().getLogicalTypeID() == LogicalTypeID::BOOL);
    }
    auto functionName = ExpressionTypeUtil::toString(type);
    scalar_func_exec_t execFunc;
    VectorBooleanFunction::bindExecFunction(type, children, execFunc);
    scalar_func_select_t selectFunc;
    VectorBooleanFunction::bindSelectFunction(type, children, selectFunc);
    std::vector<LogicalTypeID> inputTypeIDs(children.size(), LogicalTypeID::BOOL);
    auto uniqueName = ScalarFunctionExpression::getUniqueName(functionName, children);
    auto function = std::make_unique<ScalarFunction>(functionName, std::move(inputTypeIDs),
        LogicalTypeID::BOOL, execFunc, selectFunc);
    return std::make_shared<ScalarFunctionExpression>(type, std::move(function),
        std::make_unique<FunctionBindData>(LogicalType::BOOL()), std::move(children), uniqueName);
}

// Collects the operands of nested ANDs or ORs (type).
static void splitOperands(ExpressionType type, const std::shared_ptr<Expression>& expr,
    expression_vector& operands) {
    if (expr->expressionType != type) {
        operands.push_back(expr);
        return;
    }
    for (auto i = 0u; i < expr->getNumChildren(); ++i) {
        splitOperands(type, expr->getChild(i), operands);
    }
}

// AND and OR are binary, so several operands are chained left-deep.
static std::shared_ptr<Expression> chainBoolOperator(ExpressionType type,
    const expression_vector& operands) {
    auto result = operands[0];
    for (auto i = 1u; i < operands.size(); ++i) {
        result = boolOperator(type, {result, operands[i]});
    }
    return result;
}

// ---------------------------------------------------------------------------
// Core folding  (tree-propagating version)
// ---------------------------------------------------------------------------

/// Recursively simplify a boolean expression through constant folding and
/// algebraic simplification (bottom-up tree propagation).
///
/// Returns `nullptr` when the expression is already as simple as it can be.
/// Otherwise returns a simplified expression: a LiteralExpression when the
/// whole expression is always-true or always-false, or a non-literal when
/// a subtree collapsed (e.g. `AND(true, x)` → `x`).
///
/// Because children are folded before the parent is examined, nested
/// simplifications cascade upward:
///
///   AND(OR(IS_NULL(x), IS_NOT_NULL(x)), y > 5)
///       → AND(true, y > 5)        [OR tautology folded to true]
///       → y > 5                   [AND(true, x) → x]
///
///   OR(AND(IS_NULL(x), IS_NOT_NULL(x)), n.k = 1)
///       → OR(false, n.k = 1)      [AND contradiction folded to false]
///       → n.k = 1                 [OR(false, x) → x]
///
/// `nullAsFalse` is true where only a TRUE result keeps a row, i.e. directly under the filter
/// and through AND/OR. There NULL may be treated as false; under NOT it may not, so folds that
/// depend on it (NULL literals, NOT(p)/p pairs) are skipped.
static std::shared_ptr<Expression> foldBool(const std::shared_ptr<Expression>& expr,
    bool nullAsFalse) {
    // --- NOT -----------------------------------------------------------
    if (expr->expressionType == ExpressionType::NOT) {
        // NOT(NOT(x)) → x — structural, no folding needed.
        auto child = expr->getChild(0);
        if (child->expressionType == ExpressionType::NOT && child->getNumChildren() == 1) {
            return child->getChild(0);
        }
        // Fold the inner expression and invert if it becomes a constant.
        auto folded = foldBool(child, false /* nullAsFalse */);
        auto effective = folded ? folded : child;
        if (effective->expressionType == ExpressionType::LITERAL) {
            auto& lit = effective->constCast<LiteralExpression>();
            if (lit.isNull()) {
                return effective; // NOT NULL is NULL
            }
            return boolLiteral(!lit.getValue().getValue<bool>());
        }
        return folded ? boolOperator(ExpressionType::NOT, {folded}) : nullptr;
    }

    // --- AND -----------------------------------------------------------
    if (expr->expressionType == ExpressionType::AND) {
        expression_vector parts;
        splitOperands(ExpressionType::AND, expr, parts);
        expression_vector kept;
        bool anyChanged = false;

        // 1) Fold each child; short-circuit on false; drop true.
        for (auto& part : parts) {
            auto folded = foldBool(part, nullAsFalse);
            auto effective = folded ? folded : part;
            if (folded) {
                anyChanged = true;
            }
            if (effective->expressionType == ExpressionType::LITERAL) {
                auto& lit = effective->constCast<LiteralExpression>();
                if (lit.isNull() && !nullAsFalse) {
                    kept.push_back(effective);
                    continue;
                }
                if (lit.isNull() || !lit.getValue().getValue<bool>()) {
                    return boolLiteral(false); // false ∧ … ≡ false
                }
                // true conjunct is a no-op – drop it.
                anyChanged = true;
                continue;
            }
            kept.push_back(effective);
        }

        // 2) Contradictions among kept children. p ∧ ¬p is NULL, not false, when p is NULL.
        if (hasNullCheckComplements(kept) || (nullAsFalse && hasNotComplements(kept))) {
            return boolLiteral(false);
        }

        // 3) All conjuncts were true.
        if (kept.empty()) {
            return boolLiteral(true);
        }

        // 4) Single remaining child — e.g. AND(true, x) → x.
        if (kept.size() == 1) {
            return kept[0];
        }

        // 5) Several children remain: rebuild the AND from them.
        return anyChanged ? chainBoolOperator(ExpressionType::AND, kept) : nullptr;
    }

    // --- OR ------------------------------------------------------------
    if (expr->expressionType == ExpressionType::OR) {
        // 1) Fold all children in one pass.
        std::vector<std::shared_ptr<Expression>> folded;
        bool anyChanged = false;
        for (auto i = 0u; i < expr->getNumChildren(); ++i) {
            auto f = foldBool(expr->getChild(i), nullAsFalse);
            auto eff = f ? f : expr->getChild(i);
            if (f) {
                anyChanged = true;
            }
            folded.push_back(eff);
        }

        // 2) IS_NULL(x) ∨ IS_NOT_NULL(x) is always true. p ∨ ¬p is NULL, not true, when p is NULL.
        if (hasNullCheckComplements(folded)) {
            return boolLiteral(true);
        }

        // 3) Walk children; short-circuit on true, drop false.
        expression_vector kept;
        for (auto i = 0u; i < folded.size(); ++i) {
            auto& eff = folded[i];
            if (eff->expressionType == ExpressionType::LITERAL) {
                auto& lit = eff->constCast<LiteralExpression>();
                if (!lit.isNull() && lit.getValue().getValue<bool>()) {
                    return boolLiteral(true); // true ∨ … ≡ true
                }
                if (lit.isNull() && !nullAsFalse) {
                    kept.push_back(eff);
                    continue;
                }
                // false disjunct, or NULL where NULL counts as false, is a no-op.
                anyChanged = true;
                continue;
            }
            kept.push_back(eff);
        }

        // 4) All disjuncts were false.
        if (kept.empty()) {
            return boolLiteral(false);
        }

        // 5) Single remaining child — e.g. OR(false, x) → x.
        if (kept.size() == 1) {
            return kept[0];
        }

        return anyChanged ? chainBoolOperator(ExpressionType::OR, kept) : nullptr;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// BoolFoldingOptimizer
// ---------------------------------------------------------------------------

void BoolFoldingOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> BoolFoldingOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> BoolFoldingOptimizer::visitFilterReplace(
    std::shared_ptr<LogicalOperator> op) {
    auto& filter = op->constCast<LogicalFilter>();
    auto predicate = filter.getPredicate();

    // Already a literal – handled by the existing filter-push-down logic too,
    // but folding may produce new literals from composite expressions.
    if (predicate->expressionType == ExpressionType::LITERAL) {
        auto& lit = predicate->constCast<LiteralExpression>();
        if (lit.isNull() || !lit.getValue().getValue<bool>()) {
            return std::make_shared<LogicalEmptyResult>(*op->getSchema());
        }
        return op->getChild(0); // true literal → drop filter
    }

    auto folded = foldBool(predicate, true /* nullAsFalse */);
    if (!folded) {
        return op; // no simplification
    }

    if (folded->expressionType == ExpressionType::LITERAL) {
        auto& lit = folded->constCast<LiteralExpression>();
        if (lit.isNull() || !lit.getValue().getValue<bool>()) {
            return std::make_shared<LogicalEmptyResult>(*op->getSchema());
        }
        // Folded to true – the filter is redundant.
        return op->getChild(0);
    }

    // Non-literal simplification – replace the filter predicate.
    auto newFilter = std::make_shared<LogicalFilter>(std::move(folded), op->getChild(0));
    newFilter->computeFlatSchema();
    return newFilter;
}

} // namespace optimizer
} // namespace lbug
