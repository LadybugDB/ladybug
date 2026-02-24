#include "common/exception/parser.h"
#include "parser/ddl/alter.h"
#include "parser/ddl/create_sequence.h"
#include "parser/ddl/create_table.h"
#include "parser/ddl/create_type.h"
#include "parser/ddl/drop.h"
#include "parser/ddl/drop_info.h"
#include "parser/transformer.h"
#include <format>

using namespace lbug::common;
using namespace lbug::catalog;

namespace lbug {
namespace parser {

std::unique_ptr<Statement> Transformer::transformAlterTable(
    CypherParser::IC_AlterTableContext& ctx) {
    if (ctx.iC_AlterOptions()->iC_AddProperty()) {
        return transformAddProperty(ctx);
    }
    if (ctx.iC_AlterOptions()->iC_DropProperty()) {
        return transformDropProperty(ctx);
    }
    if (ctx.iC_AlterOptions()->iC_RenameTable()) {
        return transformRenameTable(ctx);
    }
    if (ctx.iC_AlterOptions()->iC_AddFromToConnection()) {
        return transformAddFromToConnection(ctx);
    }
    if (ctx.iC_AlterOptions()->iC_DropFromToConnection()) {
        return transformDropFromToConnection(ctx);
    }
    return transformRenameProperty(ctx);
}

std::string Transformer::getPKName(CypherParser::IC_CreateNodeTableContext& ctx) {
    auto pkCount = 0;
    std::string pkName;
    auto& propertyDefinitions = *ctx.iC_PropertyDefinitions();
    for (auto& definition : propertyDefinitions.iC_PropertyDefinition()) {
        if (definition->PRIMARY() && definition->KEY()) {
            pkCount++;
            pkName = transformPrimaryKey(*definition->iC_ColumnDefinition());
        }
    }
    if (ctx.iC_CreateNodeConstraint()) {
        // In the case where no pkName has been found, or the Node Constraint's name is different
        // than the pkName found, add the counter.
        if (pkCount == 0 || transformPrimaryKey(*ctx.iC_CreateNodeConstraint()) != pkName) {
            pkCount++;
        }
        pkName = transformPrimaryKey(*ctx.iC_CreateNodeConstraint());
    }
    if (pkCount == 0) {
        // Raise exception when no PRIMARY KEY is specified.
        throw ParserException("Can not find primary key.");
    } else if (pkCount > 1) {
        // Raise exception when multiple PRIMARY KEY are specified.
        throw ParserException("Found multiple primary keys.");
    }
    return pkName;
}

ConflictAction Transformer::transformConflictAction(CypherParser::IC_IfNotExistsContext* ctx) {
    if (ctx != nullptr) {
        return ConflictAction::ON_CONFLICT_DO_NOTHING;
    }
    return ConflictAction::ON_CONFLICT_THROW;
}

std::unique_ptr<Statement> Transformer::transformCreateNodeTable(
    CypherParser::IC_CreateNodeTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto createTableInfo =
        CreateTableInfo(TableType::NODE, tableName, transformConflictAction(ctx.iC_IfNotExists()));
    // If CREATE NODE TABLE AS syntax
    if (ctx.oC_Query()) {
        return std::make_unique<CreateTable>(std::move(createTableInfo),
            std::make_unique<QueryScanSource>(transformQuery(*ctx.oC_Query())));
    } else {
        createTableInfo.propertyDefinitions =
            transformPropertyDefinitions(*ctx.iC_PropertyDefinitions());
        options_t options;
        if (ctx.iC_Options()) {
            options = transformOptions(*ctx.iC_Options());
        }
        createTableInfo.extraInfo =
            std::make_unique<ExtraCreateNodeTableInfo>(getPKName(ctx), std::move(options));
        return std::make_unique<CreateTable>(std::move(createTableInfo));
    }
}

std::unique_ptr<Statement> Transformer::transformCreateRelGroup(
    CypherParser::IC_CreateRelTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    std::string relMultiplicity = "MANY_MANY";
    if (ctx.oC_SymbolicName()) {
        relMultiplicity = transformSymbolicName(*ctx.oC_SymbolicName());
    }
    options_t options;
    if (ctx.iC_Options()) {
        options = transformOptions(*ctx.iC_Options());
    }
    std::vector<std::pair<std::string, std::string>> fromToPairs;
    for (auto& fromTo : ctx.iC_FromToConnections()->iC_FromToConnection()) {
        auto src = transformSchemaName(*fromTo->oC_SchemaName(0));
        auto dst = transformSchemaName(*fromTo->oC_SchemaName(1));
        fromToPairs.emplace_back(src, dst);
    }
    std::unique_ptr<ExtraCreateTableInfo> extraInfo =
        std::make_unique<ExtraCreateRelTableGroupInfo>(relMultiplicity, std::move(fromToPairs),
            std::move(options));
    auto conflictAction = transformConflictAction(ctx.iC_IfNotExists());
    auto createTableInfo = CreateTableInfo(common::TableType::REL, tableName, conflictAction);
    if (ctx.iC_PropertyDefinitions()) {
        createTableInfo.propertyDefinitions =
            transformPropertyDefinitions(*ctx.iC_PropertyDefinitions());
    }
    createTableInfo.extraInfo = std::move(extraInfo);
    if (ctx.oC_Query()) {
        auto scanSource = std::make_unique<QueryScanSource>(transformQuery(*ctx.oC_Query()));
        return std::make_unique<CreateTable>(std::move(createTableInfo), std::move(scanSource));
    } else {
        return std::make_unique<CreateTable>(std::move(createTableInfo));
    }
}

std::unique_ptr<Statement> Transformer::transformCreateSequence(
    CypherParser::IC_CreateSequenceContext& ctx) {
    auto sequenceName = transformSchemaName(*ctx.oC_SchemaName());
    auto createSequenceInfo = CreateSequenceInfo(sequenceName,
        ctx.iC_IfNotExists() ? common::ConflictAction::ON_CONFLICT_DO_NOTHING :
                               common::ConflictAction::ON_CONFLICT_THROW);
    std::unordered_set<SequenceInfoType> applied;
    for (auto seqOption : ctx.iC_SequenceOptions()) {
        SequenceInfoType type; // NOLINT(*-init-variables)
        std::string typeString;
        CypherParser::OC_IntegerLiteralContext* valCtx = nullptr;
        std::string* valOption = nullptr;
        if (seqOption->iC_StartWith()) {
            type = SequenceInfoType::START;
            typeString = "START";
            valCtx = seqOption->iC_StartWith()->oC_IntegerLiteral();
            valOption = &createSequenceInfo.startWith;
            *valOption = seqOption->iC_StartWith()->MINUS() ? "-" : "";
        } else if (seqOption->iC_IncrementBy()) {
            type = SequenceInfoType::INCREMENT;
            typeString = "INCREMENT";
            valCtx = seqOption->iC_IncrementBy()->oC_IntegerLiteral();
            valOption = &createSequenceInfo.increment;
            *valOption = seqOption->iC_IncrementBy()->MINUS() ? "-" : "";
        } else if (seqOption->iC_MinValue()) {
            type = SequenceInfoType::MINVALUE;
            typeString = "MINVALUE";
            if (!seqOption->iC_MinValue()->NO()) {
                valCtx = seqOption->iC_MinValue()->oC_IntegerLiteral();
                valOption = &createSequenceInfo.minValue;
                *valOption = seqOption->iC_MinValue()->MINUS() ? "-" : "";
            }
        } else if (seqOption->iC_MaxValue()) {
            type = SequenceInfoType::MAXVALUE;
            typeString = "MAXVALUE";
            if (!seqOption->iC_MaxValue()->NO()) {
                valCtx = seqOption->iC_MaxValue()->oC_IntegerLiteral();
                valOption = &createSequenceInfo.maxValue;
                *valOption = seqOption->iC_MaxValue()->MINUS() ? "-" : "";
            }
        } else { // seqOption->iC_Cycle()
            type = SequenceInfoType::CYCLE;
            typeString = "CYCLE";
            if (!seqOption->iC_Cycle()->NO()) {
                createSequenceInfo.cycle = true;
            }
        }
        if (applied.find(type) != applied.end()) {
            throw ParserException(typeString + " should be passed at most once.");
        }
        applied.insert(type);

        if (valCtx && valOption) {
            *valOption += valCtx->DecimalInteger()->getText();
        }
    }
    return std::make_unique<CreateSequence>(std::move(createSequenceInfo));
}

std::unique_ptr<Statement> Transformer::transformCreateType(
    CypherParser::IC_CreateTypeContext& ctx) {
    auto name = transformSchemaName(*ctx.oC_SchemaName());
    auto type = transformDataType(*ctx.iC_DataType());
    return std::make_unique<CreateType>(name, type);
}

DropType transformDropType(CypherParser::IC_DropContext& ctx) {
    if (ctx.TABLE()) {
        return DropType::TABLE;
    } else if (ctx.SEQUENCE()) {
        return DropType::SEQUENCE;
    } else if (ctx.MACRO()) {
        return DropType::MACRO;
    } else if (ctx.GRAPH()) {
        return DropType::GRAPH;
    } else {
        UNREACHABLE_CODE;
    }
}

std::unique_ptr<Statement> Transformer::transformDrop(CypherParser::IC_DropContext& ctx) {
    auto name = transformSchemaName(*ctx.oC_SchemaName());
    auto dropType = transformDropType(ctx);
    auto conflictAction = ctx.iC_IfExists() ? common::ConflictAction::ON_CONFLICT_DO_NOTHING :
                                              common::ConflictAction::ON_CONFLICT_THROW;
    return std::make_unique<Drop>(DropInfo{std::move(name), dropType, conflictAction});
}

std::unique_ptr<Statement> Transformer::transformRenameTable(
    CypherParser::IC_AlterTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto newName = transformSchemaName(*ctx.iC_AlterOptions()->iC_RenameTable()->oC_SchemaName());
    auto extraInfo = std::make_unique<ExtraRenameTableInfo>(std::move(newName));
    auto info = AlterInfo(AlterType::RENAME, tableName, std::move(extraInfo));
    return std::make_unique<Alter>(std::move(info));
}

std::unique_ptr<Statement> Transformer::transformAddFromToConnection(
    CypherParser::IC_AlterTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto schemaNameCtx =
        ctx.iC_AlterOptions()->iC_AddFromToConnection()->iC_FromToConnection()->oC_SchemaName();
    DASSERT(schemaNameCtx.size() == 2);
    auto srcTableName = transformSchemaName(*schemaNameCtx[0]);
    auto dstTableName = transformSchemaName(*schemaNameCtx[1]);
    auto extraInfo = std::make_unique<ExtraAddFromToConnection>(std::move(srcTableName),
        std::move(dstTableName));
    ConflictAction action = ConflictAction::ON_CONFLICT_THROW;
    if (ctx.iC_AlterOptions()->iC_AddFromToConnection()->iC_IfNotExists()) {
        action = ConflictAction::ON_CONFLICT_DO_NOTHING;
    }
    auto info = AlterInfo(AlterType::ADD_FROM_TO_CONNECTION, std::move(tableName),
        std::move(extraInfo), action);
    return std::make_unique<Alter>(std::move(info));
}

std::unique_ptr<Statement> Transformer::transformDropFromToConnection(
    CypherParser::IC_AlterTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto schemaNameCtx =
        ctx.iC_AlterOptions()->iC_DropFromToConnection()->iC_FromToConnection()->oC_SchemaName();
    DASSERT(schemaNameCtx.size() == 2);
    auto srcTableName = transformSchemaName(*schemaNameCtx[0]);
    auto dstTableName = transformSchemaName(*schemaNameCtx[1]);
    auto extraInfo = std::make_unique<ExtraAddFromToConnection>(std::move(srcTableName),
        std::move(dstTableName));
    ConflictAction action = ConflictAction::ON_CONFLICT_THROW;
    if (ctx.iC_AlterOptions()->iC_DropFromToConnection()->iC_IfExists()) {
        action = ConflictAction::ON_CONFLICT_DO_NOTHING;
    }
    auto info = AlterInfo(AlterType::DROP_FROM_TO_CONNECTION, std::move(tableName),
        std::move(extraInfo), action);
    return std::make_unique<Alter>(std::move(info));
}

std::unique_ptr<Statement> Transformer::transformAddProperty(
    CypherParser::IC_AlterTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto addPropertyCtx = ctx.iC_AlterOptions()->iC_AddProperty();
    auto propertyName = transformPropertyKeyName(*addPropertyCtx->oC_PropertyKeyName());
    auto dataType = transformDataType(*addPropertyCtx->iC_DataType());
    std::unique_ptr<ParsedExpression> defaultValue = nullptr;
    if (addPropertyCtx->iC_Default()) {
        defaultValue = transformExpression(*addPropertyCtx->iC_Default()->oC_Expression());
    }
    auto extraInfo = std::make_unique<ExtraAddPropertyInfo>(std::move(propertyName),
        std::move(dataType), std::move(defaultValue));
    ConflictAction action = ConflictAction::ON_CONFLICT_THROW;
    if (addPropertyCtx->iC_IfNotExists()) {
        action = ConflictAction::ON_CONFLICT_DO_NOTHING;
    }
    auto info = AlterInfo(AlterType::ADD_PROPERTY, tableName, std::move(extraInfo), action);
    return std::make_unique<Alter>(std::move(info));
}

std::unique_ptr<Statement> Transformer::transformDropProperty(
    CypherParser::IC_AlterTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto dropProperty = ctx.iC_AlterOptions()->iC_DropProperty();
    auto propertyName = transformPropertyKeyName(*dropProperty->oC_PropertyKeyName());
    auto extraInfo = std::make_unique<ExtraDropPropertyInfo>(std::move(propertyName));
    ConflictAction action = ConflictAction::ON_CONFLICT_THROW;
    if (dropProperty->iC_IfExists()) {
        action = ConflictAction::ON_CONFLICT_DO_NOTHING;
    }
    auto info = AlterInfo(AlterType::DROP_PROPERTY, tableName, std::move(extraInfo), action);
    return std::make_unique<Alter>(std::move(info));
}

std::unique_ptr<Statement> Transformer::transformRenameProperty(
    CypherParser::IC_AlterTableContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto propertyName = transformPropertyKeyName(
        *ctx.iC_AlterOptions()->iC_RenameProperty()->oC_PropertyKeyName()[0]);
    auto newName = transformPropertyKeyName(
        *ctx.iC_AlterOptions()->iC_RenameProperty()->oC_PropertyKeyName()[1]);
    auto extraInfo = std::make_unique<ExtraRenamePropertyInfo>(propertyName, newName);
    auto info = AlterInfo(AlterType::RENAME_PROPERTY, tableName, std::move(extraInfo));
    return std::make_unique<Alter>(std::move(info));
}

std::unique_ptr<Statement> Transformer::transformCommentOn(CypherParser::IC_CommentOnContext& ctx) {
    auto tableName = transformSchemaName(*ctx.oC_SchemaName());
    auto comment = transformStringLiteral(*ctx.StringLiteral());
    auto extraInfo = std::make_unique<ExtraCommentInfo>(comment);
    auto info = AlterInfo(AlterType::COMMENT, tableName, std::move(extraInfo));
    return std::make_unique<Alter>(std::move(info));
}

std::vector<ParsedColumnDefinition> Transformer::transformColumnDefinitions(
    CypherParser::IC_ColumnDefinitionsContext& ctx) {
    std::vector<ParsedColumnDefinition> definitions;
    for (auto& definition : ctx.iC_ColumnDefinition()) {
        definitions.emplace_back(transformColumnDefinition(*definition));
    }
    return definitions;
}

ParsedColumnDefinition Transformer::transformColumnDefinition(
    CypherParser::IC_ColumnDefinitionContext& ctx) {
    auto propertyName = transformPropertyKeyName(*ctx.oC_PropertyKeyName());
    auto dataType = transformDataType(*ctx.iC_DataType());
    return ParsedColumnDefinition(propertyName, dataType);
}

std::vector<ParsedPropertyDefinition> Transformer::transformPropertyDefinitions(
    CypherParser::IC_PropertyDefinitionsContext& ctx) {
    std::vector<ParsedPropertyDefinition> definitions;
    for (auto& definition : ctx.iC_PropertyDefinition()) {
        auto columnDefinition = transformColumnDefinition(*definition->iC_ColumnDefinition());
        std::unique_ptr<ParsedExpression> defaultExpr = nullptr;
        if (definition->iC_Default()) {
            defaultExpr = transformExpression(*definition->iC_Default()->oC_Expression());
        }
        definitions.push_back(
            ParsedPropertyDefinition(std::move(columnDefinition), std::move(defaultExpr)));
    }
    return definitions;
}

static std::string convertColumnDefinitionsToString(
    const std::vector<ParsedColumnDefinition>& columnDefinitions) {
    std::string result;
    for (auto& columnDefinition : columnDefinitions) {
        result += std::format("{} {},", columnDefinition.name, columnDefinition.type);
    }
    return result.substr(0, result.length() - 1);
}

std::string Transformer::transformUnionType(CypherParser::IC_UnionTypeContext& ctx) {
    return std::format("{}({})", ctx.UNION()->getText(),
        convertColumnDefinitionsToString(transformColumnDefinitions(*ctx.iC_ColumnDefinitions())));
}

std::string Transformer::transformStructType(CypherParser::IC_StructTypeContext& ctx) {
    return std::format("{}({})", ctx.STRUCT()->getText(),
        convertColumnDefinitionsToString(transformColumnDefinitions(*ctx.iC_ColumnDefinitions())));
}

std::string Transformer::transformMapType(CypherParser::IC_MapTypeContext& ctx) {
    return std::format("{}({},{})", ctx.MAP()->getText(), transformDataType(*ctx.iC_DataType()[0]),
        transformDataType(*ctx.iC_DataType()[1]));
}

std::string Transformer::transformDecimalType(CypherParser::IC_DecimalTypeContext& ctx) {
    return ctx.getText();
}

std::string Transformer::transformDataType(CypherParser::IC_DataTypeContext& ctx) {
    if (ctx.oC_SymbolicName()) {
        return transformSymbolicName(*ctx.oC_SymbolicName());
    } else if (ctx.iC_UnionType()) {
        return transformUnionType(*ctx.iC_UnionType());
    } else if (ctx.iC_StructType()) {
        return transformStructType(*ctx.iC_StructType());
    } else if (ctx.iC_MapType()) {
        return transformMapType(*ctx.iC_MapType());
    } else if (ctx.iC_DecimalType()) {
        return transformDecimalType(*ctx.iC_DecimalType());
    } else {
        return transformDataType(*ctx.iC_DataType()) + ctx.iC_ListIdentifiers()->getText();
    }
}

std::string Transformer::transformPrimaryKey(CypherParser::IC_CreateNodeConstraintContext& ctx) {
    return transformPropertyKeyName(*ctx.oC_PropertyKeyName());
}

std::string Transformer::transformPrimaryKey(CypherParser::IC_ColumnDefinitionContext& ctx) {
    return transformPropertyKeyName(*ctx.oC_PropertyKeyName());
}

} // namespace parser
} // namespace lbug
