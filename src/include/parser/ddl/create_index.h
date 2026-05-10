#pragma once

#include "common/enums/conflict_action.h"
#include "parser/expression/parsed_expression.h"
#include "parser/statement.h"

namespace lbug {
namespace parser {

struct CreateIndexInfo {
    std::string indexType;
    std::string indexName;
    std::string tableName;
    std::string variableName;
    std::string propertyName;
    common::ConflictAction onConflict;
    options_t options;

    CreateIndexInfo(std::string indexType, std::string indexName, std::string tableName,
        std::string variableName, std::string propertyName, common::ConflictAction onConflict,
        options_t options)
        : indexType{std::move(indexType)}, indexName{std::move(indexName)},
          tableName{std::move(tableName)}, variableName{std::move(variableName)},
          propertyName{std::move(propertyName)}, onConflict{onConflict},
          options{std::move(options)} {}
};

class CreateIndex final : public Statement {
    static constexpr common::StatementType type_ = common::StatementType::CREATE_INDEX;

public:
    explicit CreateIndex(CreateIndexInfo info) : Statement{type_}, info{std::move(info)} {}

    const CreateIndexInfo& getInfo() const { return info; }

private:
    CreateIndexInfo info;
};

} // namespace parser
} // namespace lbug
