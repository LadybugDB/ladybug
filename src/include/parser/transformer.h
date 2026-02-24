#pragma once

// ANTLR4 generates code with unused parameters.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "cypher_parser.h"
#pragma GCC diagnostic pop

#include "common/enums/conflict_action.h"
#include "extension/transformer_extension.h"
#include "parser/ddl/parsed_property_definition.h"
#include "statement.h"

namespace lbug {
namespace main {
class ClientContext;
}
namespace parser {

class RegularQuery;
class SingleQuery;
class QueryPart;
class UpdatingClause;
class ReadingClause;
class WithClause;
class ReturnClause;
class ProjectionBody;
class PatternElement;
class NodePattern;
class PatternElementChain;
class RelPattern;
struct ParsedCaseAlternative;
struct BaseScanSource;
struct JoinHintNode;
struct YieldVariable;

class Transformer {
public:
    Transformer(CypherParser::IC_StatementsContext& root,
        std::vector<extension::TransformerExtension*> transformerExtensions)
        : root{root}, transformerExtensions{std::move(transformerExtensions)} {}

    std::vector<std::shared_ptr<Statement>> transform();

    void registerTransformExtension(
        std::unique_ptr<extension::TransformerExtension> transformerExtension);

    std::unique_ptr<Statement> transformStatement(CypherParser::OC_StatementContext& ctx);

    std::unique_ptr<ParsedExpression> transformWhere(CypherParser::OC_WhereContext& ctx);

    static std::string transformVariable(CypherParser::OC_VariableContext& ctx);
    std::string transformSchemaName(CypherParser::OC_SchemaNameContext& ctx);
    static std::string transformSymbolicName(CypherParser::OC_SymbolicNameContext& ctx);
    static std::string transformStringLiteral(antlr4::tree::TerminalNode& stringLiteral);
    static common::ConflictAction transformConflictAction(CypherParser::IC_IfNotExistsContext* ctx);

    // Transform copy statement.
    std::unique_ptr<Statement> transformCopyTo(CypherParser::IC_CopyTOContext& ctx);
    std::unique_ptr<Statement> transformCopyFrom(CypherParser::IC_CopyFromContext& ctx);
    std::unique_ptr<Statement> transformCopyFromByColumn(
        CypherParser::IC_CopyFromByColumnContext& ctx);
    std::vector<std::string> transformColumnNames(CypherParser::IC_ColumnNamesContext& ctx);
    std::vector<std::string> transformFilePaths(
        const std::vector<antlr4::tree::TerminalNode*>& stringLiteral);
    std::unique_ptr<BaseScanSource> transformScanSource(CypherParser::IC_ScanSourceContext& ctx);
    options_t transformOptions(CypherParser::IC_OptionsContext& ctx);

    std::unique_ptr<Statement> transformExportDatabase(CypherParser::IC_ExportDatabaseContext& ctx);
    std::unique_ptr<Statement> transformImportDatabase(CypherParser::IC_ImportDatabaseContext& ctx);

    // Transform query statement.
    std::unique_ptr<Statement> transformQuery(CypherParser::OC_QueryContext& ctx);
    std::unique_ptr<Statement> transformRegularQuery(CypherParser::OC_RegularQueryContext& ctx);
    SingleQuery transformSingleQuery(CypherParser::OC_SingleQueryContext& ctx);
    SingleQuery transformSinglePartQuery(CypherParser::OC_SinglePartQueryContext& ctx);
    QueryPart transformQueryPart(CypherParser::IC_QueryPartContext& ctx);

    // Transform updating.
    std::unique_ptr<UpdatingClause> transformUpdatingClause(
        CypherParser::OC_UpdatingClauseContext& ctx);
    std::unique_ptr<UpdatingClause> transformCreate(CypherParser::OC_CreateContext& ctx);
    std::unique_ptr<UpdatingClause> transformMerge(CypherParser::OC_MergeContext& ctx);
    std::unique_ptr<UpdatingClause> transformSet(CypherParser::OC_SetContext& ctx);
    parsed_expr_pair transformSetItem(CypherParser::OC_SetItemContext& ctx);
    std::unique_ptr<UpdatingClause> transformDelete(CypherParser::OC_DeleteContext& ctx);

    // Transform reading.
    std::unique_ptr<ReadingClause> transformReadingClause(
        CypherParser::OC_ReadingClauseContext& ctx);
    std::unique_ptr<ReadingClause> transformMatch(CypherParser::OC_MatchContext& ctx);
    std::unique_ptr<ReadingClause> transformUnwind(CypherParser::OC_UnwindContext& ctx);
    std::vector<YieldVariable> transformYieldVariables(CypherParser::OC_YieldItemsContext& ctx);
    std::unique_ptr<ReadingClause> transformInQueryCall(CypherParser::IC_InQueryCallContext& ctx);
    std::unique_ptr<ReadingClause> transformLoadFrom(CypherParser::IC_LoadFromContext& ctx);
    std::shared_ptr<JoinHintNode> transformJoinHint(CypherParser::IC_JoinNodeContext& ctx);

    // Transform projection.
    WithClause transformWith(CypherParser::OC_WithContext& ctx);
    ReturnClause transformReturn(CypherParser::OC_ReturnContext& ctx);
    ProjectionBody transformProjectionBody(CypherParser::OC_ProjectionBodyContext& ctx);
    std::vector<std::unique_ptr<ParsedExpression>> transformProjectionItems(
        CypherParser::OC_ProjectionItemsContext& ctx);
    std::unique_ptr<ParsedExpression> transformProjectionItem(
        CypherParser::OC_ProjectionItemContext& ctx);

    // Transform graph pattern.
    std::vector<PatternElement> transformPattern(CypherParser::OC_PatternContext& ctx);
    PatternElement transformPatternPart(CypherParser::OC_PatternPartContext& ctx);
    PatternElement transformAnonymousPatternPart(CypherParser::OC_AnonymousPatternPartContext& ctx);
    PatternElement transformPatternElement(CypherParser::OC_PatternElementContext& ctx);
    NodePattern transformNodePattern(CypherParser::OC_NodePatternContext& ctx);
    PatternElementChain transformPatternElementChain(
        CypherParser::OC_PatternElementChainContext& ctx);
    RelPattern transformRelationshipPattern(CypherParser::OC_RelationshipPatternContext& ctx);
    std::vector<s_parsed_expr_pair> transformProperties(CypherParser::IC_PropertiesContext& ctx);
    std::vector<std::string> transformRelTypes(CypherParser::OC_RelationshipTypesContext& ctx);
    std::vector<std::string> transformNodeLabels(CypherParser::OC_NodeLabelsContext& ctx);
    std::string transformLabelName(CypherParser::OC_LabelNameContext& ctx);
    std::string transformRelTypeName(CypherParser::OC_RelTypeNameContext& ctx);

    // Transform expression.
    std::unique_ptr<ParsedExpression> transformExpression(CypherParser::OC_ExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformOrExpression(
        CypherParser::OC_OrExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformXorExpression(
        CypherParser::OC_XorExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformAndExpression(
        CypherParser::OC_AndExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformNotExpression(
        CypherParser::OC_NotExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformComparisonExpression(
        CypherParser::OC_ComparisonExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformBitwiseOrOperatorExpression(
        CypherParser::IC_BitwiseOrOperatorExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformBitwiseAndOperatorExpression(
        CypherParser::IC_BitwiseAndOperatorExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformBitShiftOperatorExpression(
        CypherParser::IC_BitShiftOperatorExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformAddOrSubtractExpression(
        CypherParser::OC_AddOrSubtractExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformMultiplyDivideModuloExpression(
        CypherParser::OC_MultiplyDivideModuloExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformPowerOfExpression(
        CypherParser::OC_PowerOfExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformUnaryAddSubtractOrFactorialExpression(
        CypherParser::OC_UnaryAddSubtractOrFactorialExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformStringListNullOperatorExpression(
        CypherParser::OC_StringListNullOperatorExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformStringOperatorExpression(
        CypherParser::OC_StringOperatorExpressionContext& ctx,
        std::unique_ptr<ParsedExpression> propertyExpression);
    std::unique_ptr<ParsedExpression> transformListOperatorExpression(
        CypherParser::OC_ListOperatorExpressionContext& ctx,
        std::unique_ptr<ParsedExpression> childExpression);
    std::unique_ptr<ParsedExpression> transformNullOperatorExpression(
        CypherParser::OC_NullOperatorExpressionContext& ctx,
        std::unique_ptr<ParsedExpression> propertyExpression);
    std::unique_ptr<ParsedExpression> transformPropertyOrLabelsExpression(
        CypherParser::OC_PropertyOrLabelsExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformAtom(CypherParser::OC_AtomContext& ctx);
    std::unique_ptr<ParsedExpression> transformLiteral(CypherParser::OC_LiteralContext& ctx);
    std::unique_ptr<ParsedExpression> transformBooleanLiteral(
        CypherParser::OC_BooleanLiteralContext& ctx);
    std::unique_ptr<ParsedExpression> transformListLiteral(
        CypherParser::OC_ListLiteralContext& ctx);
    std::unique_ptr<ParsedExpression> transformStructLiteral(
        CypherParser::IC_StructLiteralContext& ctx);
    std::unique_ptr<ParsedExpression> transformParameterExpression(
        CypherParser::OC_ParameterContext& ctx);
    std::unique_ptr<ParsedExpression> transformParenthesizedExpression(
        CypherParser::OC_ParenthesizedExpressionContext& ctx);
    std::unique_ptr<ParsedExpression> transformFunctionInvocation(
        CypherParser::OC_FunctionInvocationContext& ctx);
    std::string transformFunctionName(CypherParser::OC_FunctionNameContext& ctx);
    std::vector<std::string> transformLambdaVariables(CypherParser::IC_LambdaVarsContext& ctx);
    std::unique_ptr<ParsedExpression> transformLambdaParameter(
        CypherParser::IC_LambdaParameterContext& ctx);
    std::unique_ptr<ParsedExpression> transformFunctionParameterExpression(
        CypherParser::IC_FunctionParameterContext& ctx);
    std::unique_ptr<ParsedExpression> transformPathPattern(
        CypherParser::OC_PathPatternsContext& ctx);
    std::unique_ptr<ParsedExpression> transformExistCountSubquery(
        CypherParser::OC_ExistCountSubqueryContext& ctx);
    std::unique_ptr<ParsedExpression> transformOcQuantifier(
        CypherParser::OC_QuantifierContext& ctx);
    std::unique_ptr<ParsedExpression> createPropertyExpression(
        CypherParser::OC_PropertyKeyNameContext& ctx, std::unique_ptr<ParsedExpression> child);
    std::unique_ptr<ParsedExpression> createPropertyExpression(
        CypherParser::OC_PropertyLookupContext& ctx, std::unique_ptr<ParsedExpression> child);
    std::unique_ptr<ParsedExpression> transformCaseExpression(
        CypherParser::OC_CaseExpressionContext& ctx);
    ParsedCaseAlternative transformCaseAlternative(CypherParser::OC_CaseAlternativeContext& ctx);
    std::unique_ptr<ParsedExpression> transformNumberLiteral(
        CypherParser::OC_NumberLiteralContext& ctx, bool negative);
    std::unique_ptr<ParsedExpression> transformProperty(
        CypherParser::OC_PropertyExpressionContext& ctx);
    std::string transformPropertyKeyName(CypherParser::OC_PropertyKeyNameContext& ctx);
    std::unique_ptr<ParsedExpression> transformIntegerLiteral(
        CypherParser::OC_IntegerLiteralContext& ctx, bool negative);
    std::unique_ptr<ParsedExpression> transformDoubleLiteral(
        CypherParser::OC_DoubleLiteralContext& ctx, bool negative);

    // Transform ddl.
    std::unique_ptr<Statement> transformAlterTable(CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformCreateNodeTable(
        CypherParser::IC_CreateNodeTableContext& ctx);
    std::unique_ptr<Statement> transformCreateRelGroup(CypherParser::IC_CreateRelTableContext& ctx);
    std::unique_ptr<Statement> transformCreateSequence(CypherParser::IC_CreateSequenceContext& ctx);
    std::unique_ptr<Statement> transformCreateType(CypherParser::IC_CreateTypeContext& ctx);
    std::unique_ptr<Statement> transformDrop(CypherParser::IC_DropContext& ctx);
    std::unique_ptr<Statement> transformRenameTable(CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformAddFromToConnection(
        CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformDropFromToConnection(
        CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformAddProperty(CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformDropProperty(CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformRenameProperty(CypherParser::IC_AlterTableContext& ctx);
    std::unique_ptr<Statement> transformCommentOn(CypherParser::IC_CommentOnContext& ctx);
    std::string transformUnionType(CypherParser::IC_UnionTypeContext& ctx);
    std::string transformStructType(CypherParser::IC_StructTypeContext& ctx);
    std::string transformMapType(CypherParser::IC_MapTypeContext& ctx);
    std::string transformDecimalType(CypherParser::IC_DecimalTypeContext& ctx);
    std::string transformDataType(CypherParser::IC_DataTypeContext& ctx);
    std::string getPKName(CypherParser::IC_CreateNodeTableContext& ctx);
    std::string transformPrimaryKey(CypherParser::IC_CreateNodeConstraintContext& ctx);
    std::string transformPrimaryKey(CypherParser::IC_ColumnDefinitionContext& ctx);
    std::vector<ParsedColumnDefinition> transformColumnDefinitions(
        CypherParser::IC_ColumnDefinitionsContext& ctx);
    ParsedColumnDefinition transformColumnDefinition(CypherParser::IC_ColumnDefinitionContext& ctx);
    std::vector<ParsedPropertyDefinition> transformPropertyDefinitions(
        CypherParser::IC_PropertyDefinitionsContext& ctx);

    // Transform standalone call.
    std::unique_ptr<Statement> transformStandaloneCall(CypherParser::IC_StandaloneCallContext& ctx);

    // Transform create macro.
    std::unique_ptr<Statement> transformCreateMacro(CypherParser::IC_CreateMacroContext& ctx);
    std::vector<std::string> transformPositionalArgs(CypherParser::IC_PositionalArgsContext& ctx);

    // Transform transaction.
    std::unique_ptr<Statement> transformTransaction(CypherParser::IC_TransactionContext& ctx);

    // Transform extension.
    std::unique_ptr<Statement> transformExtension(CypherParser::IC_ExtensionContext& ctx);

    // Transform attach/detach/use database.
    std::unique_ptr<Statement> transformAttachDatabase(CypherParser::IC_AttachDatabaseContext& ctx);
    std::unique_ptr<Statement> transformDetachDatabase(CypherParser::IC_DetachDatabaseContext& ctx);
    std::unique_ptr<Statement> transformUseDatabase(CypherParser::IC_UseDatabaseContext& ctx);

    // Transform create/use graph.
    std::unique_ptr<Statement> transformCreateGraph(CypherParser::IC_CreateGraphContext& ctx);
    std::unique_ptr<Statement> transformUseGraph(CypherParser::IC_UseGraphContext& ctx);

    std::unique_ptr<Statement> transformExtensionStatement(antlr4::ParserRuleContext* ctx);

private:
    CypherParser::IC_StatementsContext& root;
    std::vector<extension::TransformerExtension*> transformerExtensions;
};

} // namespace parser
} // namespace lbug
