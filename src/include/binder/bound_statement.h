#pragma once

#include "bound_statement_result.h"
#include "common/copy_constructors.h"
#include "common/enums/statement_type.h"

namespace lbug {
namespace binder {

class BoundStatement {
public:
    BoundStatement(common::StatementType statementType, BoundStatementResult statementResult)
        : statementType{statementType}, statementResult{std::move(statementResult)} {}
    DELETE_COPY_DEFAULT_MOVE(BoundStatement);

    virtual ~BoundStatement() = default;

    common::StatementType getStatementType() const { return statementType; }

    const BoundStatementResult* getStatementResult() const { return &statementResult; }
    std::shared_ptr<Expression> getSingleColumnExpr() const {
        return statementResult.getSingleColumnExpr();
    }

    BoundStatementResult* getStatementResultUnsafe() { return &statementResult; }

    template<class TARGET>
    const TARGET& constCast() const {
        return common::dynamic_cast_checked<const TARGET&>(*this);
    }
    template<class TARGET>
    TARGET& cast() {
        return common::dynamic_cast_checked<TARGET&>(*this);
    }

private:
    common::StatementType statementType;
    BoundStatementResult statementResult;
};

} // namespace binder
} // namespace lbug
