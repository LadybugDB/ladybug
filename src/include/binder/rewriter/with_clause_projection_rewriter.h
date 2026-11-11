#pragma once

#include "binder/bound_statement_visitor.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main

namespace binder {

// WithClauseProjectionRewriter first analyze the properties need to be scanned for each query. And
// then rewrite node/rel expression in WITH clause as their properties. So We avoid eagerly evaluate
// node/rel in WITH clause projection. E.g.
// MATCH (a) WITH a MATCH (a)->(b);
// will be rewritten as
// MATCH (a) WITH a._id MATCH (a)->(b);
// Node properties that only the final RETURN uses may be left out of every WITH and recorded as
// LatePropertyFetch on the last query part instead.
class WithClauseProjectionRewriter final : public BoundStatementVisitor {
public:
    explicit WithClauseProjectionRewriter(const main::ClientContext& context) : context{context} {}

    void visitSingleQueryUnsafe(NormalizedSingleQuery& singleQuery) override;

private:
    const main::ClientContext& context;
};

} // namespace binder
} // namespace lbug
