#include "optimizer/bool_folding_optimizer.h"

#include "binder/expression/literal_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "common/enums/expression_type.h"
#include "common/types/value/value.h"
#include "planner/operator/logical_empty_result.h"
#include "planner/operator/logical_filter.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

// ---------------------------------------------------------------------------
// Helper: detect whether two expressions are logical negations
// ---------------------------------------------------------------------------

/// Returns true if `a` and `b` can never be true simultaneously.
///
/// Recognised forms:
///   - NOT(p) and p
///   - IS_NULL(x) and IS_NOT_NULL(x)
static bool areNegations(const Expression& a, const Expression& b) {
    // NOT(p) == b
    if (a.expressionType == ExpressionType::NOT && a.getNumChildren() == 1 && *a.getChild(0) == b) {
        return true;
    }
    // NOT(q) == a
    if (b.expressionType == ExpressionType::NOT && b.getNumChildren() == 1 && *b.getChild(0) == a) {
        return true;
    }
    // IS_NULL(x) vs IS_NOT_NULL(x)
    if (a.expressionType == ExpressionType::IS_NULL &&
        b.expressionType == ExpressionType::IS_NOT_NULL) {
        return a.getNumChildren() > 0 && b.getNumChildren() > 0 && *a.getChild(0) == *b.getChild(0);
    }
    if (a.expressionType == ExpressionType::IS_NOT_NULL &&
        b.expressionType == ExpressionType::IS_NULL) {
        return a.getNumChildren() > 0 && b.getNumChildren() > 0 && *a.getChild(0) == *b.getChild(0);
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
static std::shared_ptr<Expression> foldBool(const std::shared_ptr<Expression>& expr) {
    // --- NOT -----------------------------------------------------------
    if (expr->expressionType == ExpressionType::NOT) {
        // NOT(NOT(x)) → x — structural, no folding needed.
        auto child = expr->getChild(0);
        if (child->expressionType == ExpressionType::NOT && child->getNumChildren() == 1) {
            return child->getChild(0);
        }
        // Fold the inner expression and invert if it becomes a constant.
        auto folded = foldBool(child);
        auto effective = folded ? folded : child;
        if (effective->expressionType == ExpressionType::LITERAL) {
            auto& lit = effective->constCast<LiteralExpression>();
            return boolLiteral(lit.isNull() || !lit.getValue().getValue<bool>());
        }
        return folded ? effective : nullptr;
    }

    // --- AND -----------------------------------------------------------
    if (expr->expressionType == ExpressionType::AND) {
        auto parts = expr->splitOnAND();
        expression_vector kept;
        bool anyChanged = false;

        // 1) Fold each child; short-circuit on false; drop true.
        for (auto& part : parts) {
            auto folded = foldBool(part);
            auto effective = folded ? folded : part;
            if (folded) {
                anyChanged = true;
            }
            if (effective->expressionType == ExpressionType::LITERAL) {
                auto& lit = effective->constCast<LiteralExpression>();
                if (lit.isNull() || !lit.getValue().getValue<bool>()) {
                    return boolLiteral(false); // false ∧ … ≡ false
                }
                // true conjunct is a no-op – drop it.
                continue;
            }
            kept.push_back(effective);
        }

        // 2) Check pair-wise contradictions among kept children.
        for (auto i = 0u; i < kept.size(); ++i) {
            for (auto j = i + 1; j < kept.size(); ++j) {
                if (areNegations(*kept[i], *kept[j])) {
                    return boolLiteral(false); // p ∧ ¬p ≡ false
                }
            }
        }

        // 3) All conjuncts were true.
        if (kept.empty()) {
            return boolLiteral(true);
        }

        // 4) Single remaining child — e.g. AND(true, x) → x.
        if (kept.size() == 1) {
            return kept[0];
        }

        // 5) Multiple children remain; we currently do not rebuild the
        //    AND tree.  If any child was simplified it is a missed
        //    opportunity, but the predicate is still correct as-is.
        return anyChanged ? kept[0] : nullptr;
    }

    // --- OR ------------------------------------------------------------
    if (expr->expressionType == ExpressionType::OR) {
        // 1) Fold all children in one pass.
        std::vector<std::shared_ptr<Expression>> folded;
        bool anyChanged = false;
        for (auto i = 0u; i < expr->getNumChildren(); ++i) {
            auto f = foldBool(expr->getChild(i));
            auto eff = f ? f : expr->getChild(i);
            if (f) {
                anyChanged = true;
            }
            folded.push_back(eff);
        }

        // 2) Walk children; short-circuit on true, drop false, detect
        //    tautology pairs.
        expression_vector kept;
        for (auto i = 0u; i < folded.size(); ++i) {
            auto& eff = folded[i];
            if (eff->expressionType == ExpressionType::LITERAL) {
                auto& lit = eff->constCast<LiteralExpression>();
                if (!lit.isNull() && lit.getValue().getValue<bool>()) {
                    return boolLiteral(true); // true ∨ … ≡ true
                }
                // false disjunct is a no-op.
                continue;
            }
            // Check for tautology: p ∨ ¬p ≡ true.
            for (auto j = i + 1; j < folded.size(); ++j) {
                if (areNegations(*eff, *folded[j])) {
                    return boolLiteral(true);
                }
            }
            kept.push_back(eff);
        }

        // 3) All disjuncts were false.
        if (kept.empty()) {
            return boolLiteral(false);
        }

        // 4) Single remaining child — e.g. OR(false, x) → x.
        if (kept.size() == 1) {
            return kept[0];
        }

        return anyChanged ? kept[0] : nullptr;
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

    auto folded = foldBool(predicate);
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
