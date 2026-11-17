#pragma once

#include <optional>

#include "binder/query/reading_clause/bound_reading_clause.h"
#include "binder/query/return_with_clause/bound_projection_body.h"
#include "binder/query/updating_clause/bound_updating_clause.h"

namespace lbug {
namespace binder {

class NodeExpression;

// Properties of a node that the previous WITH does not carry. The planner reads them by the
// node's internal ID at the start of the query part.
struct LatePropertyFetch {
    std::shared_ptr<NodeExpression> node;
    expression_vector properties;
    // The previous WITH keeps at most PlannerKnobs::LOOKUP_JOIN_MAX_OUTER_CARDINALITY rows. The
    // planner then looks the properties up, unless label pruning left the node without a table.
    bool useLookup = false;
};

class NormalizedQueryPart {
    friend class NormalizedQueryPartMatchRewriter;

public:
    NormalizedQueryPart() = default;
    DELETE_COPY_DEFAULT_MOVE(NormalizedQueryPart);

    void addReadingClause(std::unique_ptr<BoundReadingClause> boundReadingClause) {
        readingClauses.push_back(std::move(boundReadingClause));
    }
    bool hasReadingClause() const { return !readingClauses.empty(); }
    uint32_t getNumReadingClause() const { return readingClauses.size(); }
    BoundReadingClause* getReadingClause(uint32_t idx) const { return readingClauses[idx].get(); }

    void addUpdatingClause(std::unique_ptr<BoundUpdatingClause> boundUpdatingClause) {
        updatingClauses.push_back(std::move(boundUpdatingClause));
    }
    bool hasUpdatingClause() const { return !updatingClauses.empty(); }
    uint32_t getNumUpdatingClause() const { return updatingClauses.size(); }
    BoundUpdatingClause* getUpdatingClause(uint32_t idx) const {
        return updatingClauses[idx].get();
    }

    void setProjectionBody(BoundProjectionBody boundProjectionBody) {
        projectionBody = std::move(boundProjectionBody);
    }
    bool hasProjectionBody() const { return projectionBody.has_value(); }
    BoundProjectionBody* getProjectionBodyUnsafe() {
        DASSERT(projectionBody.has_value());
        return &projectionBody.value();
    }
    const BoundProjectionBody* getProjectionBody() const {
        DASSERT(projectionBody.has_value());
        return &projectionBody.value();
    }

    bool hasProjectionBodyPredicate() const { return projectionBodyPredicate != nullptr; }
    std::shared_ptr<Expression> getProjectionBodyPredicate() const {
        return projectionBodyPredicate;
    }
    void setProjectionBodyPredicate(const std::shared_ptr<Expression>& predicate) {
        projectionBodyPredicate = predicate;
    }

    void setLatePropertyFetches(std::vector<LatePropertyFetch> fetches) {
        latePropertyFetches = std::move(fetches);
    }
    const std::vector<LatePropertyFetch>& getLatePropertyFetches() const {
        return latePropertyFetches;
    }
    expression_set getLateProperties() const {
        expression_set result;
        for (auto& fetch : latePropertyFetches) {
            result.insert(fetch.properties.begin(), fetch.properties.end());
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<BoundReadingClause>> readingClauses;
    std::vector<std::unique_ptr<BoundUpdatingClause>> updatingClauses;
    std::optional<BoundProjectionBody> projectionBody;
    std::shared_ptr<Expression> projectionBodyPredicate;
    std::vector<LatePropertyFetch> latePropertyFetches;
};

} // namespace binder
} // namespace lbug
