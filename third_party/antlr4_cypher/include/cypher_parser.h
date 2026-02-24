
// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"




class  CypherParser : public antlr4::Parser {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, T__3 = 4, T__4 = 5, T__5 = 6, T__6 = 7, 
    T__7 = 8, T__8 = 9, T__9 = 10, T__10 = 11, T__11 = 12, T__12 = 13, T__13 = 14, 
    T__14 = 15, T__15 = 16, T__16 = 17, T__17 = 18, T__18 = 19, T__19 = 20, 
    T__20 = 21, T__21 = 22, T__22 = 23, T__23 = 24, T__24 = 25, T__25 = 26, 
    T__26 = 27, T__27 = 28, T__28 = 29, T__29 = 30, T__30 = 31, T__31 = 32, 
    T__32 = 33, T__33 = 34, T__34 = 35, T__35 = 36, T__36 = 37, T__37 = 38, 
    T__38 = 39, T__39 = 40, T__40 = 41, T__41 = 42, T__42 = 43, T__43 = 44, 
    ACYCLIC = 45, ANY = 46, ADD = 47, ALL = 48, ALTER = 49, AND = 50, AS = 51, 
    ASC = 52, ASCENDING = 53, ATTACH = 54, BEGIN = 55, BY = 56, CALL = 57, 
    CASE = 58, CAST = 59, CHECKPOINT = 60, COLUMN = 61, COMMENT = 62, COMMIT = 63, 
    COMMIT_SKIP_CHECKPOINT = 64, CONTAINS = 65, COPY = 66, COUNT = 67, CREATE = 68, 
    CYCLE = 69, DATABASE = 70, DBTYPE = 71, DEFAULT = 72, DELETE = 73, DESC = 74, 
    DESCENDING = 75, DETACH = 76, DISTINCT = 77, DROP = 78, ELSE = 79, END = 80, 
    ENDS = 81, EXISTS = 82, EXPLAIN = 83, EXPORT = 84, EXTENSION = 85, FALSE = 86, 
    FROM = 87, FORCE = 88, GLOB = 89, GRAPH = 90, GROUP = 91, HEADERS = 92, 
    HINT = 93, IMPORT = 94, IF = 95, IN = 96, INCREMENT = 97, INSTALL = 98, 
    IS = 99, JOIN = 100, KEY = 101, LIMIT = 102, LOAD = 103, LOGICAL = 104, 
    MACRO = 105, MATCH = 106, MAXVALUE = 107, MERGE = 108, MINVALUE = 109, 
    MULTI_JOIN = 110, NO = 111, NODE = 112, NOT = 113, NONE = 114, NULL_ = 115, 
    ON = 116, ONLY = 117, OPTIONAL = 118, OR = 119, ORDER = 120, PRIMARY = 121, 
    PROFILE = 122, PROJECT = 123, READ = 124, REL = 125, RENAME = 126, RETURN = 127, 
    ROLLBACK = 128, ROLLBACK_SKIP_CHECKPOINT = 129, SEQUENCE = 130, SET = 131, 
    SHORTEST = 132, START = 133, STARTS = 134, STRUCT = 135, TABLE = 136, 
    THEN = 137, TO = 138, TRAIL = 139, TRANSACTION = 140, TRUE = 141, TYPE = 142, 
    UNION = 143, UNWIND = 144, UNINSTALL = 145, UPDATE = 146, USE = 147, 
    WHEN = 148, WHERE = 149, WITH = 150, WRITE = 151, WSHORTEST = 152, XOR = 153, 
    SINGLE = 154, YIELD = 155, USER = 156, PASSWORD = 157, ROLE = 158, MAP = 159, 
    DECIMAL = 160, STAR = 161, L_SKIP = 162, INVALID_NOT_EQUAL = 163, COLON = 164, 
    DOTDOT = 165, MINUS = 166, FACTORIAL = 167, StringLiteral = 168, EscapedChar = 169, 
    DecimalInteger = 170, HexLetter = 171, HexDigit = 172, Digit = 173, 
    NonZeroDigit = 174, NonZeroOctDigit = 175, ZeroDigit = 176, ExponentDecimalReal = 177, 
    RegularDecimalReal = 178, UnescapedSymbolicName = 179, IdentifierStart = 180, 
    IdentifierPart = 181, EscapedSymbolicName = 182, SP = 183, WHITESPACE = 184, 
    CypherComment = 185, Unknown = 186
  };

  enum {
    RuleIC_Statements = 0, RuleOC_Cypher = 1, RuleOC_Statement = 2, RuleIC_CopyFrom = 3, 
    RuleIC_ColumnNames = 4, RuleIC_ScanSource = 5, RuleIC_CopyFromByColumn = 6, 
    RuleIC_CopyTO = 7, RuleIC_ExportDatabase = 8, RuleIC_ImportDatabase = 9, 
    RuleIC_AttachDatabase = 10, RuleIC_Option = 11, RuleIC_Options = 12, 
    RuleIC_DetachDatabase = 13, RuleIC_UseDatabase = 14, RuleIC_CreateGraph = 15, 
    RuleIC_UseGraph = 16, RuleIC_StandaloneCall = 17, RuleIC_CommentOn = 18, 
    RuleIC_CreateMacro = 19, RuleIC_PositionalArgs = 20, RuleIC_DefaultArg = 21, 
    RuleIC_FilePaths = 22, RuleIC_IfNotExists = 23, RuleIC_CreateNodeTable = 24, 
    RuleIC_CreateRelTable = 25, RuleIC_FromToConnections = 26, RuleIC_FromToConnection = 27, 
    RuleIC_CreateSequence = 28, RuleIC_CreateType = 29, RuleIC_SequenceOptions = 30, 
    RuleIC_WithPasswd = 31, RuleIC_CreateUser = 32, RuleIC_CreateRole = 33, 
    RuleIC_IncrementBy = 34, RuleIC_MinValue = 35, RuleIC_MaxValue = 36, 
    RuleIC_StartWith = 37, RuleIC_Cycle = 38, RuleIC_IfExists = 39, RuleIC_Drop = 40, 
    RuleIC_AlterTable = 41, RuleIC_AlterOptions = 42, RuleIC_AddProperty = 43, 
    RuleIC_Default = 44, RuleIC_DropProperty = 45, RuleIC_RenameTable = 46, 
    RuleIC_RenameProperty = 47, RuleIC_AddFromToConnection = 48, RuleIC_DropFromToConnection = 49, 
    RuleIC_ColumnDefinitions = 50, RuleIC_ColumnDefinition = 51, RuleIC_PropertyDefinitions = 52, 
    RuleIC_PropertyDefinition = 53, RuleIC_CreateNodeConstraint = 54, RuleIC_UnionType = 55, 
    RuleIC_StructType = 56, RuleIC_MapType = 57, RuleIC_DecimalType = 58, 
    RuleIC_DataType = 59, RuleIC_ListIdentifiers = 60, RuleIC_ListIdentifier = 61, 
    RuleOC_AnyCypherOption = 62, RuleOC_Explain = 63, RuleOC_Profile = 64, 
    RuleIC_Transaction = 65, RuleIC_Extension = 66, RuleIC_LoadExtension = 67, 
    RuleIC_InstallExtension = 68, RuleIC_UninstallExtension = 69, RuleIC_UpdateExtension = 70, 
    RuleOC_Query = 71, RuleOC_RegularQuery = 72, RuleOC_Union = 73, RuleOC_SingleQuery = 74, 
    RuleOC_SinglePartQuery = 75, RuleOC_MultiPartQuery = 76, RuleIC_QueryPart = 77, 
    RuleOC_UpdatingClause = 78, RuleOC_ReadingClause = 79, RuleIC_LoadFrom = 80, 
    RuleOC_YieldItem = 81, RuleOC_YieldItems = 82, RuleIC_InQueryCall = 83, 
    RuleOC_Match = 84, RuleIC_Hint = 85, RuleIC_JoinNode = 86, RuleOC_Unwind = 87, 
    RuleOC_Create = 88, RuleOC_Merge = 89, RuleOC_MergeAction = 90, RuleOC_Set = 91, 
    RuleOC_SetItem = 92, RuleOC_Delete = 93, RuleOC_With = 94, RuleOC_Return = 95, 
    RuleOC_ProjectionBody = 96, RuleOC_ProjectionItems = 97, RuleOC_ProjectionItem = 98, 
    RuleOC_Order = 99, RuleOC_Skip = 100, RuleOC_Limit = 101, RuleOC_SortItem = 102, 
    RuleOC_Where = 103, RuleOC_Pattern = 104, RuleOC_PatternPart = 105, 
    RuleOC_AnonymousPatternPart = 106, RuleOC_PatternElement = 107, RuleOC_NodePattern = 108, 
    RuleOC_PatternElementChain = 109, RuleOC_RelationshipPattern = 110, 
    RuleOC_RelationshipDetail = 111, RuleIC_Properties = 112, RuleOC_RelationshipTypes = 113, 
    RuleOC_NodeLabels = 114, RuleIC_RecursiveDetail = 115, RuleIC_RecursiveType = 116, 
    RuleOC_RangeLiteral = 117, RuleIC_RecursiveComprehension = 118, RuleIC_RecursiveProjectionItems = 119, 
    RuleOC_LowerBound = 120, RuleOC_UpperBound = 121, RuleOC_LabelName = 122, 
    RuleOC_RelTypeName = 123, RuleOC_Expression = 124, RuleOC_OrExpression = 125, 
    RuleOC_XorExpression = 126, RuleOC_AndExpression = 127, RuleOC_NotExpression = 128, 
    RuleOC_ComparisonExpression = 129, RuleIC_ComparisonOperator = 130, 
    RuleIC_BitwiseOrOperatorExpression = 131, RuleIC_BitwiseAndOperatorExpression = 132, 
    RuleIC_BitShiftOperatorExpression = 133, RuleIC_BitShiftOperator = 134, 
    RuleOC_AddOrSubtractExpression = 135, RuleIC_AddOrSubtractOperator = 136, 
    RuleOC_MultiplyDivideModuloExpression = 137, RuleIC_MultiplyDivideModuloOperator = 138, 
    RuleOC_PowerOfExpression = 139, RuleOC_StringListNullOperatorExpression = 140, 
    RuleOC_ListOperatorExpression = 141, RuleOC_StringOperatorExpression = 142, 
    RuleOC_RegularExpression = 143, RuleOC_NullOperatorExpression = 144, 
    RuleOC_UnaryAddSubtractOrFactorialExpression = 145, RuleOC_PropertyOrLabelsExpression = 146, 
    RuleOC_Atom = 147, RuleOC_Quantifier = 148, RuleOC_FilterExpression = 149, 
    RuleOC_IdInColl = 150, RuleOC_Literal = 151, RuleOC_BooleanLiteral = 152, 
    RuleOC_ListLiteral = 153, RuleIC_ListEntry = 154, RuleIC_StructLiteral = 155, 
    RuleIC_StructField = 156, RuleOC_ParenthesizedExpression = 157, RuleOC_FunctionInvocation = 158, 
    RuleOC_FunctionName = 159, RuleIC_FunctionParameter = 160, RuleIC_LambdaParameter = 161, 
    RuleIC_LambdaVars = 162, RuleOC_PathPatterns = 163, RuleOC_ExistCountSubquery = 164, 
    RuleOC_PropertyLookup = 165, RuleOC_CaseExpression = 166, RuleOC_CaseAlternative = 167, 
    RuleOC_Variable = 168, RuleOC_NumberLiteral = 169, RuleOC_Parameter = 170, 
    RuleOC_PropertyExpression = 171, RuleOC_PropertyKeyName = 172, RuleOC_IntegerLiteral = 173, 
    RuleOC_DoubleLiteral = 174, RuleOC_SchemaName = 175, RuleOC_SymbolicName = 176, 
    RuleIC_NonReservedKeywords = 177, RuleOC_LeftArrowHead = 178, RuleOC_RightArrowHead = 179, 
    RuleOC_Dash = 180
  };

  explicit CypherParser(antlr4::TokenStream *input);

  CypherParser(antlr4::TokenStream *input, const antlr4::atn::ParserATNSimulatorOptions &options);

  ~CypherParser() override;

  std::string getGrammarFileName() const override;

  const antlr4::atn::ATN& getATN() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;


  class IC_StatementsContext;
  class OC_CypherContext;
  class OC_StatementContext;
  class IC_CopyFromContext;
  class IC_ColumnNamesContext;
  class IC_ScanSourceContext;
  class IC_CopyFromByColumnContext;
  class IC_CopyTOContext;
  class IC_ExportDatabaseContext;
  class IC_ImportDatabaseContext;
  class IC_AttachDatabaseContext;
  class IC_OptionContext;
  class IC_OptionsContext;
  class IC_DetachDatabaseContext;
  class IC_UseDatabaseContext;
  class IC_CreateGraphContext;
  class IC_UseGraphContext;
  class IC_StandaloneCallContext;
  class IC_CommentOnContext;
  class IC_CreateMacroContext;
  class IC_PositionalArgsContext;
  class IC_DefaultArgContext;
  class IC_FilePathsContext;
  class IC_IfNotExistsContext;
  class IC_CreateNodeTableContext;
  class IC_CreateRelTableContext;
  class IC_FromToConnectionsContext;
  class IC_FromToConnectionContext;
  class IC_CreateSequenceContext;
  class IC_CreateTypeContext;
  class IC_SequenceOptionsContext;
  class IC_WithPasswdContext;
  class IC_CreateUserContext;
  class IC_CreateRoleContext;
  class IC_IncrementByContext;
  class IC_MinValueContext;
  class IC_MaxValueContext;
  class IC_StartWithContext;
  class IC_CycleContext;
  class IC_IfExistsContext;
  class IC_DropContext;
  class IC_AlterTableContext;
  class IC_AlterOptionsContext;
  class IC_AddPropertyContext;
  class IC_DefaultContext;
  class IC_DropPropertyContext;
  class IC_RenameTableContext;
  class IC_RenamePropertyContext;
  class IC_AddFromToConnectionContext;
  class IC_DropFromToConnectionContext;
  class IC_ColumnDefinitionsContext;
  class IC_ColumnDefinitionContext;
  class IC_PropertyDefinitionsContext;
  class IC_PropertyDefinitionContext;
  class IC_CreateNodeConstraintContext;
  class IC_UnionTypeContext;
  class IC_StructTypeContext;
  class IC_MapTypeContext;
  class IC_DecimalTypeContext;
  class IC_DataTypeContext;
  class IC_ListIdentifiersContext;
  class IC_ListIdentifierContext;
  class OC_AnyCypherOptionContext;
  class OC_ExplainContext;
  class OC_ProfileContext;
  class IC_TransactionContext;
  class IC_ExtensionContext;
  class IC_LoadExtensionContext;
  class IC_InstallExtensionContext;
  class IC_UninstallExtensionContext;
  class IC_UpdateExtensionContext;
  class OC_QueryContext;
  class OC_RegularQueryContext;
  class OC_UnionContext;
  class OC_SingleQueryContext;
  class OC_SinglePartQueryContext;
  class OC_MultiPartQueryContext;
  class IC_QueryPartContext;
  class OC_UpdatingClauseContext;
  class OC_ReadingClauseContext;
  class IC_LoadFromContext;
  class OC_YieldItemContext;
  class OC_YieldItemsContext;
  class IC_InQueryCallContext;
  class OC_MatchContext;
  class IC_HintContext;
  class IC_JoinNodeContext;
  class OC_UnwindContext;
  class OC_CreateContext;
  class OC_MergeContext;
  class OC_MergeActionContext;
  class OC_SetContext;
  class OC_SetItemContext;
  class OC_DeleteContext;
  class OC_WithContext;
  class OC_ReturnContext;
  class OC_ProjectionBodyContext;
  class OC_ProjectionItemsContext;
  class OC_ProjectionItemContext;
  class OC_OrderContext;
  class OC_SkipContext;
  class OC_LimitContext;
  class OC_SortItemContext;
  class OC_WhereContext;
  class OC_PatternContext;
  class OC_PatternPartContext;
  class OC_AnonymousPatternPartContext;
  class OC_PatternElementContext;
  class OC_NodePatternContext;
  class OC_PatternElementChainContext;
  class OC_RelationshipPatternContext;
  class OC_RelationshipDetailContext;
  class IC_PropertiesContext;
  class OC_RelationshipTypesContext;
  class OC_NodeLabelsContext;
  class IC_RecursiveDetailContext;
  class IC_RecursiveTypeContext;
  class OC_RangeLiteralContext;
  class IC_RecursiveComprehensionContext;
  class IC_RecursiveProjectionItemsContext;
  class OC_LowerBoundContext;
  class OC_UpperBoundContext;
  class OC_LabelNameContext;
  class OC_RelTypeNameContext;
  class OC_ExpressionContext;
  class OC_OrExpressionContext;
  class OC_XorExpressionContext;
  class OC_AndExpressionContext;
  class OC_NotExpressionContext;
  class OC_ComparisonExpressionContext;
  class IC_ComparisonOperatorContext;
  class IC_BitwiseOrOperatorExpressionContext;
  class IC_BitwiseAndOperatorExpressionContext;
  class IC_BitShiftOperatorExpressionContext;
  class IC_BitShiftOperatorContext;
  class OC_AddOrSubtractExpressionContext;
  class IC_AddOrSubtractOperatorContext;
  class OC_MultiplyDivideModuloExpressionContext;
  class IC_MultiplyDivideModuloOperatorContext;
  class OC_PowerOfExpressionContext;
  class OC_StringListNullOperatorExpressionContext;
  class OC_ListOperatorExpressionContext;
  class OC_StringOperatorExpressionContext;
  class OC_RegularExpressionContext;
  class OC_NullOperatorExpressionContext;
  class OC_UnaryAddSubtractOrFactorialExpressionContext;
  class OC_PropertyOrLabelsExpressionContext;
  class OC_AtomContext;
  class OC_QuantifierContext;
  class OC_FilterExpressionContext;
  class OC_IdInCollContext;
  class OC_LiteralContext;
  class OC_BooleanLiteralContext;
  class OC_ListLiteralContext;
  class IC_ListEntryContext;
  class IC_StructLiteralContext;
  class IC_StructFieldContext;
  class OC_ParenthesizedExpressionContext;
  class OC_FunctionInvocationContext;
  class OC_FunctionNameContext;
  class IC_FunctionParameterContext;
  class IC_LambdaParameterContext;
  class IC_LambdaVarsContext;
  class OC_PathPatternsContext;
  class OC_ExistCountSubqueryContext;
  class OC_PropertyLookupContext;
  class OC_CaseExpressionContext;
  class OC_CaseAlternativeContext;
  class OC_VariableContext;
  class OC_NumberLiteralContext;
  class OC_ParameterContext;
  class OC_PropertyExpressionContext;
  class OC_PropertyKeyNameContext;
  class OC_IntegerLiteralContext;
  class OC_DoubleLiteralContext;
  class OC_SchemaNameContext;
  class OC_SymbolicNameContext;
  class IC_NonReservedKeywordsContext;
  class OC_LeftArrowHeadContext;
  class OC_RightArrowHeadContext;
  class OC_DashContext; 

  class  IC_StatementsContext : public antlr4::ParserRuleContext {
  public:
    IC_StatementsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_CypherContext *> oC_Cypher();
    OC_CypherContext* oC_Cypher(size_t i);
    antlr4::tree::TerminalNode *EOF();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_StatementsContext* iC_Statements();

  class  OC_CypherContext : public antlr4::ParserRuleContext {
  public:
    OC_CypherContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_StatementContext *oC_Statement();
    OC_AnyCypherOptionContext *oC_AnyCypherOption();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_CypherContext* oC_Cypher();

  class  OC_StatementContext : public antlr4::ParserRuleContext {
  public:
    OC_StatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_QueryContext *oC_Query();
    IC_CreateUserContext *iC_CreateUser();
    IC_CreateRoleContext *iC_CreateRole();
    IC_CreateNodeTableContext *iC_CreateNodeTable();
    IC_CreateRelTableContext *iC_CreateRelTable();
    IC_CreateSequenceContext *iC_CreateSequence();
    IC_CreateTypeContext *iC_CreateType();
    IC_DropContext *iC_Drop();
    IC_AlterTableContext *iC_AlterTable();
    IC_CopyFromContext *iC_CopyFrom();
    IC_CopyFromByColumnContext *iC_CopyFromByColumn();
    IC_CopyTOContext *iC_CopyTO();
    IC_StandaloneCallContext *iC_StandaloneCall();
    IC_CreateMacroContext *iC_CreateMacro();
    IC_CommentOnContext *iC_CommentOn();
    IC_TransactionContext *iC_Transaction();
    IC_ExtensionContext *iC_Extension();
    IC_ExportDatabaseContext *iC_ExportDatabase();
    IC_ImportDatabaseContext *iC_ImportDatabase();
    IC_AttachDatabaseContext *iC_AttachDatabase();
    IC_DetachDatabaseContext *iC_DetachDatabase();
    IC_UseDatabaseContext *iC_UseDatabase();
    IC_CreateGraphContext *iC_CreateGraph();
    IC_UseGraphContext *iC_UseGraph();

   
  };

  OC_StatementContext* oC_Statement();

  class  IC_CopyFromContext : public antlr4::ParserRuleContext {
  public:
    IC_CopyFromContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COPY();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_SchemaNameContext *oC_SchemaName();
    antlr4::tree::TerminalNode *FROM();
    IC_ScanSourceContext *iC_ScanSource();
    IC_ColumnNamesContext *iC_ColumnNames();
    IC_OptionsContext *iC_Options();

   
  };

  IC_CopyFromContext* iC_CopyFrom();

  class  IC_ColumnNamesContext : public antlr4::ParserRuleContext {
  public:
    IC_ColumnNamesContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_SchemaNameContext *> oC_SchemaName();
    OC_SchemaNameContext* oC_SchemaName(size_t i);

   
  };

  IC_ColumnNamesContext* iC_ColumnNames();

  class  IC_ScanSourceContext : public antlr4::ParserRuleContext {
  public:
    IC_ScanSourceContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    IC_FilePathsContext *iC_FilePaths();
    OC_QueryContext *oC_Query();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_ParameterContext *oC_Parameter();
    OC_VariableContext *oC_Variable();
    OC_SchemaNameContext *oC_SchemaName();
    OC_FunctionInvocationContext *oC_FunctionInvocation();

   
  };

  IC_ScanSourceContext* iC_ScanSource();

  class  IC_CopyFromByColumnContext : public antlr4::ParserRuleContext {
  public:
    IC_CopyFromByColumnContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COPY();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_SchemaNameContext *oC_SchemaName();
    antlr4::tree::TerminalNode *FROM();
    std::vector<antlr4::tree::TerminalNode *> StringLiteral();
    antlr4::tree::TerminalNode* StringLiteral(size_t i);
    antlr4::tree::TerminalNode *BY();
    antlr4::tree::TerminalNode *COLUMN();

   
  };

  IC_CopyFromByColumnContext* iC_CopyFromByColumn();

  class  IC_CopyTOContext : public antlr4::ParserRuleContext {
  public:
    IC_CopyTOContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COPY();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_QueryContext *oC_Query();
    antlr4::tree::TerminalNode *TO();
    antlr4::tree::TerminalNode *StringLiteral();
    IC_OptionsContext *iC_Options();

   
  };

  IC_CopyTOContext* iC_CopyTO();

  class  IC_ExportDatabaseContext : public antlr4::ParserRuleContext {
  public:
    IC_ExportDatabaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *EXPORT();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *DATABASE();
    antlr4::tree::TerminalNode *StringLiteral();
    IC_OptionsContext *iC_Options();

   
  };

  IC_ExportDatabaseContext* iC_ExportDatabase();

  class  IC_ImportDatabaseContext : public antlr4::ParserRuleContext {
  public:
    IC_ImportDatabaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IMPORT();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *DATABASE();
    antlr4::tree::TerminalNode *StringLiteral();

   
  };

  IC_ImportDatabaseContext* iC_ImportDatabase();

  class  IC_AttachDatabaseContext : public antlr4::ParserRuleContext {
  public:
    IC_AttachDatabaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ATTACH();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *StringLiteral();
    antlr4::tree::TerminalNode *DBTYPE();
    OC_SymbolicNameContext *oC_SymbolicName();
    antlr4::tree::TerminalNode *AS();
    OC_SchemaNameContext *oC_SchemaName();
    IC_OptionsContext *iC_Options();

   
  };

  IC_AttachDatabaseContext* iC_AttachDatabase();

  class  IC_OptionContext : public antlr4::ParserRuleContext {
  public:
    IC_OptionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();
    OC_LiteralContext *oC_Literal();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_OptionContext* iC_Option();

  class  IC_OptionsContext : public antlr4::ParserRuleContext {
  public:
    IC_OptionsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_OptionContext *> iC_Option();
    IC_OptionContext* iC_Option(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_OptionsContext* iC_Options();

  class  IC_DetachDatabaseContext : public antlr4::ParserRuleContext {
  public:
    IC_DetachDatabaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DETACH();
    antlr4::tree::TerminalNode *SP();
    OC_SchemaNameContext *oC_SchemaName();

   
  };

  IC_DetachDatabaseContext* iC_DetachDatabase();

  class  IC_UseDatabaseContext : public antlr4::ParserRuleContext {
  public:
    IC_UseDatabaseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *USE();
    antlr4::tree::TerminalNode *SP();
    OC_SchemaNameContext *oC_SchemaName();

   
  };

  IC_UseDatabaseContext* iC_UseDatabase();

  class  IC_CreateGraphContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateGraphContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *GRAPH();
    OC_SchemaNameContext *oC_SchemaName();
    antlr4::tree::TerminalNode *ANY();

   
  };

  IC_CreateGraphContext* iC_CreateGraph();

  class  IC_UseGraphContext : public antlr4::ParserRuleContext {
  public:
    IC_UseGraphContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *USE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *GRAPH();
    OC_SchemaNameContext *oC_SchemaName();

   
  };

  IC_UseGraphContext* iC_UseGraph();

  class  IC_StandaloneCallContext : public antlr4::ParserRuleContext {
  public:
    IC_StandaloneCallContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CALL();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_SymbolicNameContext *oC_SymbolicName();
    OC_ExpressionContext *oC_Expression();
    OC_FunctionInvocationContext *oC_FunctionInvocation();

   
  };

  IC_StandaloneCallContext* iC_StandaloneCall();

  class  IC_CommentOnContext : public antlr4::ParserRuleContext {
  public:
    IC_CommentOnContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COMMENT();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *ON();
    antlr4::tree::TerminalNode *TABLE();
    OC_SchemaNameContext *oC_SchemaName();
    antlr4::tree::TerminalNode *IS();
    antlr4::tree::TerminalNode *StringLiteral();

   
  };

  IC_CommentOnContext* iC_CommentOn();

  class  IC_CreateMacroContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateMacroContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *MACRO();
    OC_FunctionNameContext *oC_FunctionName();
    antlr4::tree::TerminalNode *AS();
    OC_ExpressionContext *oC_Expression();
    IC_PositionalArgsContext *iC_PositionalArgs();
    std::vector<IC_DefaultArgContext *> iC_DefaultArg();
    IC_DefaultArgContext* iC_DefaultArg(size_t i);

   
  };

  IC_CreateMacroContext* iC_CreateMacro();

  class  IC_PositionalArgsContext : public antlr4::ParserRuleContext {
  public:
    IC_PositionalArgsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_SymbolicNameContext *> oC_SymbolicName();
    OC_SymbolicNameContext* oC_SymbolicName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_PositionalArgsContext* iC_PositionalArgs();

  class  IC_DefaultArgContext : public antlr4::ParserRuleContext {
  public:
    IC_DefaultArgContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();
    antlr4::tree::TerminalNode *COLON();
    OC_LiteralContext *oC_Literal();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_DefaultArgContext* iC_DefaultArg();

  class  IC_FilePathsContext : public antlr4::ParserRuleContext {
  public:
    IC_FilePathsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> StringLiteral();
    antlr4::tree::TerminalNode* StringLiteral(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *GLOB();

   
  };

  IC_FilePathsContext* iC_FilePaths();

  class  IC_IfNotExistsContext : public antlr4::ParserRuleContext {
  public:
    IC_IfNotExistsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IF();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *NOT();
    antlr4::tree::TerminalNode *EXISTS();

   
  };

  IC_IfNotExistsContext* iC_IfNotExists();

  class  IC_CreateNodeTableContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateNodeTableContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *NODE();
    antlr4::tree::TerminalNode *TABLE();
    OC_SchemaNameContext *oC_SchemaName();
    IC_PropertyDefinitionsContext *iC_PropertyDefinitions();
    antlr4::tree::TerminalNode *AS();
    OC_QueryContext *oC_Query();
    IC_IfNotExistsContext *iC_IfNotExists();
    antlr4::tree::TerminalNode *WITH();
    IC_OptionsContext *iC_Options();
    IC_CreateNodeConstraintContext *iC_CreateNodeConstraint();

   
  };

  IC_CreateNodeTableContext* iC_CreateNodeTable();

  class  IC_CreateRelTableContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateRelTableContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *REL();
    antlr4::tree::TerminalNode *TABLE();
    OC_SchemaNameContext *oC_SchemaName();
    IC_FromToConnectionsContext *iC_FromToConnections();
    antlr4::tree::TerminalNode *AS();
    OC_QueryContext *oC_Query();
    antlr4::tree::TerminalNode *GROUP();
    IC_IfNotExistsContext *iC_IfNotExists();
    antlr4::tree::TerminalNode *WITH();
    IC_OptionsContext *iC_Options();
    IC_PropertyDefinitionsContext *iC_PropertyDefinitions();
    OC_SymbolicNameContext *oC_SymbolicName();

   
  };

  IC_CreateRelTableContext* iC_CreateRelTable();

  class  IC_FromToConnectionsContext : public antlr4::ParserRuleContext {
  public:
    IC_FromToConnectionsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_FromToConnectionContext *> iC_FromToConnection();
    IC_FromToConnectionContext* iC_FromToConnection(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_FromToConnectionsContext* iC_FromToConnections();

  class  IC_FromToConnectionContext : public antlr4::ParserRuleContext {
  public:
    IC_FromToConnectionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *FROM();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_SchemaNameContext *> oC_SchemaName();
    OC_SchemaNameContext* oC_SchemaName(size_t i);
    antlr4::tree::TerminalNode *TO();

   
  };

  IC_FromToConnectionContext* iC_FromToConnection();

  class  IC_CreateSequenceContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateSequenceContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *SEQUENCE();
    OC_SchemaNameContext *oC_SchemaName();
    IC_IfNotExistsContext *iC_IfNotExists();
    std::vector<IC_SequenceOptionsContext *> iC_SequenceOptions();
    IC_SequenceOptionsContext* iC_SequenceOptions(size_t i);

   
  };

  IC_CreateSequenceContext* iC_CreateSequence();

  class  IC_CreateTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *TYPE();
    OC_SchemaNameContext *oC_SchemaName();
    antlr4::tree::TerminalNode *AS();
    IC_DataTypeContext *iC_DataType();

   
  };

  IC_CreateTypeContext* iC_CreateType();

  class  IC_SequenceOptionsContext : public antlr4::ParserRuleContext {
  public:
    IC_SequenceOptionsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    IC_IncrementByContext *iC_IncrementBy();
    IC_MinValueContext *iC_MinValue();
    IC_MaxValueContext *iC_MaxValue();
    IC_StartWithContext *iC_StartWith();
    IC_CycleContext *iC_Cycle();

   
  };

  IC_SequenceOptionsContext* iC_SequenceOptions();

  class  IC_WithPasswdContext : public antlr4::ParserRuleContext {
  public:
    IC_WithPasswdContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *WITH();
    antlr4::tree::TerminalNode *PASSWORD();
    antlr4::tree::TerminalNode *StringLiteral();

   
  };

  IC_WithPasswdContext* iC_WithPasswd();

  class  IC_CreateUserContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateUserContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *USER();
    OC_VariableContext *oC_Variable();
    IC_IfNotExistsContext *iC_IfNotExists();
    IC_WithPasswdContext *iC_WithPasswd();

   
  };

  IC_CreateUserContext* iC_CreateUser();

  class  IC_CreateRoleContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateRoleContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *ROLE();
    OC_VariableContext *oC_Variable();
    IC_IfNotExistsContext *iC_IfNotExists();

   
  };

  IC_CreateRoleContext* iC_CreateRole();

  class  IC_IncrementByContext : public antlr4::ParserRuleContext {
  public:
    IC_IncrementByContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *INCREMENT();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_IntegerLiteralContext *oC_IntegerLiteral();
    antlr4::tree::TerminalNode *BY();
    antlr4::tree::TerminalNode *MINUS();

   
  };

  IC_IncrementByContext* iC_IncrementBy();

  class  IC_MinValueContext : public antlr4::ParserRuleContext {
  public:
    IC_MinValueContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *NO();
    antlr4::tree::TerminalNode *SP();
    antlr4::tree::TerminalNode *MINVALUE();
    OC_IntegerLiteralContext *oC_IntegerLiteral();
    antlr4::tree::TerminalNode *MINUS();

   
  };

  IC_MinValueContext* iC_MinValue();

  class  IC_MaxValueContext : public antlr4::ParserRuleContext {
  public:
    IC_MaxValueContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *NO();
    antlr4::tree::TerminalNode *SP();
    antlr4::tree::TerminalNode *MAXVALUE();
    OC_IntegerLiteralContext *oC_IntegerLiteral();
    antlr4::tree::TerminalNode *MINUS();

   
  };

  IC_MaxValueContext* iC_MaxValue();

  class  IC_StartWithContext : public antlr4::ParserRuleContext {
  public:
    IC_StartWithContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *START();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_IntegerLiteralContext *oC_IntegerLiteral();
    antlr4::tree::TerminalNode *WITH();
    antlr4::tree::TerminalNode *MINUS();

   
  };

  IC_StartWithContext* iC_StartWith();

  class  IC_CycleContext : public antlr4::ParserRuleContext {
  public:
    IC_CycleContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CYCLE();
    antlr4::tree::TerminalNode *NO();
    antlr4::tree::TerminalNode *SP();

   
  };

  IC_CycleContext* iC_Cycle();

  class  IC_IfExistsContext : public antlr4::ParserRuleContext {
  public:
    IC_IfExistsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *IF();
    antlr4::tree::TerminalNode *SP();
    antlr4::tree::TerminalNode *EXISTS();

   
  };

  IC_IfExistsContext* iC_IfExists();

  class  IC_DropContext : public antlr4::ParserRuleContext {
  public:
    IC_DropContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DROP();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_SchemaNameContext *oC_SchemaName();
    antlr4::tree::TerminalNode *TABLE();
    antlr4::tree::TerminalNode *SEQUENCE();
    antlr4::tree::TerminalNode *MACRO();
    antlr4::tree::TerminalNode *GRAPH();
    IC_IfExistsContext *iC_IfExists();

   
  };

  IC_DropContext* iC_Drop();

  class  IC_AlterTableContext : public antlr4::ParserRuleContext {
  public:
    IC_AlterTableContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ALTER();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *TABLE();
    OC_SchemaNameContext *oC_SchemaName();
    IC_AlterOptionsContext *iC_AlterOptions();

   
  };

  IC_AlterTableContext* iC_AlterTable();

  class  IC_AlterOptionsContext : public antlr4::ParserRuleContext {
  public:
    IC_AlterOptionsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    IC_AddPropertyContext *iC_AddProperty();
    IC_DropPropertyContext *iC_DropProperty();
    IC_RenameTableContext *iC_RenameTable();
    IC_RenamePropertyContext *iC_RenameProperty();
    IC_AddFromToConnectionContext *iC_AddFromToConnection();
    IC_DropFromToConnectionContext *iC_DropFromToConnection();

   
  };

  IC_AlterOptionsContext* iC_AlterOptions();

  class  IC_AddPropertyContext : public antlr4::ParserRuleContext {
  public:
    IC_AddPropertyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ADD();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_PropertyKeyNameContext *oC_PropertyKeyName();
    IC_DataTypeContext *iC_DataType();
    IC_IfNotExistsContext *iC_IfNotExists();
    IC_DefaultContext *iC_Default();

   
  };

  IC_AddPropertyContext* iC_AddProperty();

  class  IC_DefaultContext : public antlr4::ParserRuleContext {
  public:
    IC_DefaultContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DEFAULT();
    antlr4::tree::TerminalNode *SP();
    OC_ExpressionContext *oC_Expression();

   
  };

  IC_DefaultContext* iC_Default();

  class  IC_DropPropertyContext : public antlr4::ParserRuleContext {
  public:
    IC_DropPropertyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DROP();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_PropertyKeyNameContext *oC_PropertyKeyName();
    IC_IfExistsContext *iC_IfExists();

   
  };

  IC_DropPropertyContext* iC_DropProperty();

  class  IC_RenameTableContext : public antlr4::ParserRuleContext {
  public:
    IC_RenameTableContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *RENAME();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *TO();
    OC_SchemaNameContext *oC_SchemaName();

   
  };

  IC_RenameTableContext* iC_RenameTable();

  class  IC_RenamePropertyContext : public antlr4::ParserRuleContext {
  public:
    IC_RenamePropertyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *RENAME();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_PropertyKeyNameContext *> oC_PropertyKeyName();
    OC_PropertyKeyNameContext* oC_PropertyKeyName(size_t i);
    antlr4::tree::TerminalNode *TO();

   
  };

  IC_RenamePropertyContext* iC_RenameProperty();

  class  IC_AddFromToConnectionContext : public antlr4::ParserRuleContext {
  public:
    IC_AddFromToConnectionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ADD();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    IC_FromToConnectionContext *iC_FromToConnection();
    IC_IfNotExistsContext *iC_IfNotExists();

   
  };

  IC_AddFromToConnectionContext* iC_AddFromToConnection();

  class  IC_DropFromToConnectionContext : public antlr4::ParserRuleContext {
  public:
    IC_DropFromToConnectionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DROP();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    IC_FromToConnectionContext *iC_FromToConnection();
    IC_IfExistsContext *iC_IfExists();

   
  };

  IC_DropFromToConnectionContext* iC_DropFromToConnection();

  class  IC_ColumnDefinitionsContext : public antlr4::ParserRuleContext {
  public:
    IC_ColumnDefinitionsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_ColumnDefinitionContext *> iC_ColumnDefinition();
    IC_ColumnDefinitionContext* iC_ColumnDefinition(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_ColumnDefinitionsContext* iC_ColumnDefinitions();

  class  IC_ColumnDefinitionContext : public antlr4::ParserRuleContext {
  public:
    IC_ColumnDefinitionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_PropertyKeyNameContext *oC_PropertyKeyName();
    antlr4::tree::TerminalNode *SP();
    IC_DataTypeContext *iC_DataType();

   
  };

  IC_ColumnDefinitionContext* iC_ColumnDefinition();

  class  IC_PropertyDefinitionsContext : public antlr4::ParserRuleContext {
  public:
    IC_PropertyDefinitionsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_PropertyDefinitionContext *> iC_PropertyDefinition();
    IC_PropertyDefinitionContext* iC_PropertyDefinition(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_PropertyDefinitionsContext* iC_PropertyDefinitions();

  class  IC_PropertyDefinitionContext : public antlr4::ParserRuleContext {
  public:
    IC_PropertyDefinitionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    IC_ColumnDefinitionContext *iC_ColumnDefinition();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    IC_DefaultContext *iC_Default();
    antlr4::tree::TerminalNode *PRIMARY();
    antlr4::tree::TerminalNode *KEY();

   
  };

  IC_PropertyDefinitionContext* iC_PropertyDefinition();

  class  IC_CreateNodeConstraintContext : public antlr4::ParserRuleContext {
  public:
    IC_CreateNodeConstraintContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *PRIMARY();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *KEY();
    OC_PropertyKeyNameContext *oC_PropertyKeyName();

   
  };

  IC_CreateNodeConstraintContext* iC_CreateNodeConstraint();

  class  IC_UnionTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_UnionTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *UNION();
    IC_ColumnDefinitionsContext *iC_ColumnDefinitions();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_UnionTypeContext* iC_UnionType();

  class  IC_StructTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_StructTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *STRUCT();
    IC_ColumnDefinitionsContext *iC_ColumnDefinitions();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_StructTypeContext* iC_StructType();

  class  IC_MapTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_MapTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MAP();
    std::vector<IC_DataTypeContext *> iC_DataType();
    IC_DataTypeContext* iC_DataType(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_MapTypeContext* iC_MapType();

  class  IC_DecimalTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_DecimalTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DECIMAL();
    std::vector<OC_IntegerLiteralContext *> oC_IntegerLiteral();
    OC_IntegerLiteralContext* oC_IntegerLiteral(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_DecimalTypeContext* iC_DecimalType();

  class  IC_DataTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_DataTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();
    IC_UnionTypeContext *iC_UnionType();
    IC_StructTypeContext *iC_StructType();
    IC_MapTypeContext *iC_MapType();
    IC_DecimalTypeContext *iC_DecimalType();
    IC_DataTypeContext *iC_DataType();
    IC_ListIdentifiersContext *iC_ListIdentifiers();

   
  };

  IC_DataTypeContext* iC_DataType();
  IC_DataTypeContext* iC_DataType(int precedence);
  class  IC_ListIdentifiersContext : public antlr4::ParserRuleContext {
  public:
    IC_ListIdentifiersContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_ListIdentifierContext *> iC_ListIdentifier();
    IC_ListIdentifierContext* iC_ListIdentifier(size_t i);

   
  };

  IC_ListIdentifiersContext* iC_ListIdentifiers();

  class  IC_ListIdentifierContext : public antlr4::ParserRuleContext {
  public:
    IC_ListIdentifierContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_IntegerLiteralContext *oC_IntegerLiteral();

   
  };

  IC_ListIdentifierContext* iC_ListIdentifier();

  class  OC_AnyCypherOptionContext : public antlr4::ParserRuleContext {
  public:
    OC_AnyCypherOptionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ExplainContext *oC_Explain();
    OC_ProfileContext *oC_Profile();

   
  };

  OC_AnyCypherOptionContext* oC_AnyCypherOption();

  class  OC_ExplainContext : public antlr4::ParserRuleContext {
  public:
    OC_ExplainContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *EXPLAIN();
    antlr4::tree::TerminalNode *SP();
    antlr4::tree::TerminalNode *LOGICAL();

   
  };

  OC_ExplainContext* oC_Explain();

  class  OC_ProfileContext : public antlr4::ParserRuleContext {
  public:
    OC_ProfileContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *PROFILE();

   
  };

  OC_ProfileContext* oC_Profile();

  class  IC_TransactionContext : public antlr4::ParserRuleContext {
  public:
    IC_TransactionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *BEGIN();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *TRANSACTION();
    antlr4::tree::TerminalNode *READ();
    antlr4::tree::TerminalNode *ONLY();
    antlr4::tree::TerminalNode *COMMIT();
    antlr4::tree::TerminalNode *ROLLBACK();
    antlr4::tree::TerminalNode *CHECKPOINT();

   
  };

  IC_TransactionContext* iC_Transaction();

  class  IC_ExtensionContext : public antlr4::ParserRuleContext {
  public:
    IC_ExtensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    IC_LoadExtensionContext *iC_LoadExtension();
    IC_InstallExtensionContext *iC_InstallExtension();
    IC_UninstallExtensionContext *iC_UninstallExtension();
    IC_UpdateExtensionContext *iC_UpdateExtension();

   
  };

  IC_ExtensionContext* iC_Extension();

  class  IC_LoadExtensionContext : public antlr4::ParserRuleContext {
  public:
    IC_LoadExtensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LOAD();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *StringLiteral();
    OC_VariableContext *oC_Variable();
    antlr4::tree::TerminalNode *EXTENSION();

   
  };

  IC_LoadExtensionContext* iC_LoadExtension();

  class  IC_InstallExtensionContext : public antlr4::ParserRuleContext {
  public:
    IC_InstallExtensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *INSTALL();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_VariableContext *oC_Variable();
    antlr4::tree::TerminalNode *FORCE();
    antlr4::tree::TerminalNode *FROM();
    antlr4::tree::TerminalNode *StringLiteral();

   
  };

  IC_InstallExtensionContext* iC_InstallExtension();

  class  IC_UninstallExtensionContext : public antlr4::ParserRuleContext {
  public:
    IC_UninstallExtensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *UNINSTALL();
    antlr4::tree::TerminalNode *SP();
    OC_VariableContext *oC_Variable();

   
  };

  IC_UninstallExtensionContext* iC_UninstallExtension();

  class  IC_UpdateExtensionContext : public antlr4::ParserRuleContext {
  public:
    IC_UpdateExtensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *UPDATE();
    antlr4::tree::TerminalNode *SP();
    OC_VariableContext *oC_Variable();

   
  };

  IC_UpdateExtensionContext* iC_UpdateExtension();

  class  OC_QueryContext : public antlr4::ParserRuleContext {
  public:
    OC_QueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_RegularQueryContext *oC_RegularQuery();

   
  };

  OC_QueryContext* oC_Query();

  class  OC_RegularQueryContext : public antlr4::ParserRuleContext {
  public:
    OC_RegularQueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SingleQueryContext *oC_SingleQuery();
    std::vector<OC_UnionContext *> oC_Union();
    OC_UnionContext* oC_Union(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_ReturnContext *> oC_Return();
    OC_ReturnContext* oC_Return(size_t i);

   
  };

  OC_RegularQueryContext* oC_RegularQuery();

  class  OC_UnionContext : public antlr4::ParserRuleContext {
  public:
    OC_UnionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *UNION();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *ALL();
    OC_SingleQueryContext *oC_SingleQuery();

   
  };

  OC_UnionContext* oC_Union();

  class  OC_SingleQueryContext : public antlr4::ParserRuleContext {
  public:
    OC_SingleQueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SinglePartQueryContext *oC_SinglePartQuery();
    OC_MultiPartQueryContext *oC_MultiPartQuery();

   
  };

  OC_SingleQueryContext* oC_SingleQuery();

  class  OC_SinglePartQueryContext : public antlr4::ParserRuleContext {
  public:
    OC_SinglePartQueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ReturnContext *oC_Return();
    std::vector<OC_ReadingClauseContext *> oC_ReadingClause();
    OC_ReadingClauseContext* oC_ReadingClause(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_UpdatingClauseContext *> oC_UpdatingClause();
    OC_UpdatingClauseContext* oC_UpdatingClause(size_t i);

   
  };

  OC_SinglePartQueryContext* oC_SinglePartQuery();

  class  OC_MultiPartQueryContext : public antlr4::ParserRuleContext {
  public:
    OC_MultiPartQueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SinglePartQueryContext *oC_SinglePartQuery();
    std::vector<IC_QueryPartContext *> iC_QueryPart();
    IC_QueryPartContext* iC_QueryPart(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_MultiPartQueryContext* oC_MultiPartQuery();

  class  IC_QueryPartContext : public antlr4::ParserRuleContext {
  public:
    IC_QueryPartContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_WithContext *oC_With();
    std::vector<OC_ReadingClauseContext *> oC_ReadingClause();
    OC_ReadingClauseContext* oC_ReadingClause(size_t i);
    std::vector<OC_UpdatingClauseContext *> oC_UpdatingClause();
    OC_UpdatingClauseContext* oC_UpdatingClause(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_QueryPartContext* iC_QueryPart();

  class  OC_UpdatingClauseContext : public antlr4::ParserRuleContext {
  public:
    OC_UpdatingClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_CreateContext *oC_Create();
    OC_MergeContext *oC_Merge();
    OC_SetContext *oC_Set();
    OC_DeleteContext *oC_Delete();

   
  };

  OC_UpdatingClauseContext* oC_UpdatingClause();

  class  OC_ReadingClauseContext : public antlr4::ParserRuleContext {
  public:
    OC_ReadingClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_MatchContext *oC_Match();
    OC_UnwindContext *oC_Unwind();
    IC_InQueryCallContext *iC_InQueryCall();
    IC_LoadFromContext *iC_LoadFrom();

   
  };

  OC_ReadingClauseContext* oC_ReadingClause();

  class  IC_LoadFromContext : public antlr4::ParserRuleContext {
  public:
    IC_LoadFromContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LOAD();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *FROM();
    IC_ScanSourceContext *iC_ScanSource();
    antlr4::tree::TerminalNode *WITH();
    antlr4::tree::TerminalNode *HEADERS();
    IC_ColumnDefinitionsContext *iC_ColumnDefinitions();
    IC_OptionsContext *iC_Options();
    OC_WhereContext *oC_Where();

   
  };

  IC_LoadFromContext* iC_LoadFrom();

  class  OC_YieldItemContext : public antlr4::ParserRuleContext {
  public:
    OC_YieldItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_VariableContext *> oC_Variable();
    OC_VariableContext* oC_Variable(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *AS();

   
  };

  OC_YieldItemContext* oC_YieldItem();

  class  OC_YieldItemsContext : public antlr4::ParserRuleContext {
  public:
    OC_YieldItemsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_YieldItemContext *> oC_YieldItem();
    OC_YieldItemContext* oC_YieldItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_YieldItemsContext* oC_YieldItems();

  class  IC_InQueryCallContext : public antlr4::ParserRuleContext {
  public:
    IC_InQueryCallContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CALL();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_FunctionInvocationContext *oC_FunctionInvocation();
    OC_WhereContext *oC_Where();
    antlr4::tree::TerminalNode *YIELD();
    OC_YieldItemsContext *oC_YieldItems();

   
  };

  IC_InQueryCallContext* iC_InQueryCall();

  class  OC_MatchContext : public antlr4::ParserRuleContext {
  public:
    OC_MatchContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MATCH();
    OC_PatternContext *oC_Pattern();
    antlr4::tree::TerminalNode *OPTIONAL();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_WhereContext *oC_Where();
    IC_HintContext *iC_Hint();

   
  };

  OC_MatchContext* oC_Match();

  class  IC_HintContext : public antlr4::ParserRuleContext {
  public:
    IC_HintContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *HINT();
    antlr4::tree::TerminalNode *SP();
    IC_JoinNodeContext *iC_JoinNode();

   
  };

  IC_HintContext* iC_Hint();

  class  IC_JoinNodeContext : public antlr4::ParserRuleContext {
  public:
    IC_JoinNodeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_JoinNodeContext *> iC_JoinNode();
    IC_JoinNodeContext* iC_JoinNode(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_SchemaNameContext *> oC_SchemaName();
    OC_SchemaNameContext* oC_SchemaName(size_t i);
    antlr4::tree::TerminalNode *JOIN();
    std::vector<antlr4::tree::TerminalNode *> MULTI_JOIN();
    antlr4::tree::TerminalNode* MULTI_JOIN(size_t i);

   
  };

  IC_JoinNodeContext* iC_JoinNode();
  IC_JoinNodeContext* iC_JoinNode(int precedence);
  class  OC_UnwindContext : public antlr4::ParserRuleContext {
  public:
    OC_UnwindContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *UNWIND();
    OC_ExpressionContext *oC_Expression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *AS();
    OC_VariableContext *oC_Variable();

   
  };

  OC_UnwindContext* oC_Unwind();

  class  OC_CreateContext : public antlr4::ParserRuleContext {
  public:
    OC_CreateContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *CREATE();
    OC_PatternContext *oC_Pattern();
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_CreateContext* oC_Create();

  class  OC_MergeContext : public antlr4::ParserRuleContext {
  public:
    OC_MergeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MERGE();
    OC_PatternContext *oC_Pattern();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_MergeActionContext *> oC_MergeAction();
    OC_MergeActionContext* oC_MergeAction(size_t i);

   
  };

  OC_MergeContext* oC_Merge();

  class  OC_MergeActionContext : public antlr4::ParserRuleContext {
  public:
    OC_MergeActionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ON();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *MATCH();
    OC_SetContext *oC_Set();
    antlr4::tree::TerminalNode *CREATE();

   
  };

  OC_MergeActionContext* oC_MergeAction();

  class  OC_SetContext : public antlr4::ParserRuleContext {
  public:
    OC_SetContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *SET();
    std::vector<OC_SetItemContext *> oC_SetItem();
    OC_SetItemContext* oC_SetItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_AtomContext *oC_Atom();
    IC_PropertiesContext *iC_Properties();

   
  };

  OC_SetContext* oC_Set();

  class  OC_SetItemContext : public antlr4::ParserRuleContext {
  public:
    OC_SetItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_PropertyExpressionContext *oC_PropertyExpression();
    OC_ExpressionContext *oC_Expression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_SetItemContext* oC_SetItem();

  class  OC_DeleteContext : public antlr4::ParserRuleContext {
  public:
    OC_DeleteContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DELETE();
    std::vector<OC_ExpressionContext *> oC_Expression();
    OC_ExpressionContext* oC_Expression(size_t i);
    antlr4::tree::TerminalNode *DETACH();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_DeleteContext* oC_Delete();

  class  OC_WithContext : public antlr4::ParserRuleContext {
  public:
    OC_WithContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WITH();
    OC_ProjectionBodyContext *oC_ProjectionBody();
    OC_WhereContext *oC_Where();
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_WithContext* oC_With();

  class  OC_ReturnContext : public antlr4::ParserRuleContext {
  public:
    OC_ReturnContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *RETURN();
    OC_ProjectionBodyContext *oC_ProjectionBody();

   
  };

  OC_ReturnContext* oC_Return();

  class  OC_ProjectionBodyContext : public antlr4::ParserRuleContext {
  public:
    OC_ProjectionBodyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_ProjectionItemsContext *oC_ProjectionItems();
    antlr4::tree::TerminalNode *DISTINCT();
    OC_OrderContext *oC_Order();
    OC_SkipContext *oC_Skip();
    OC_LimitContext *oC_Limit();

   
  };

  OC_ProjectionBodyContext* oC_ProjectionBody();

  class  OC_ProjectionItemsContext : public antlr4::ParserRuleContext {
  public:
    OC_ProjectionItemsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *STAR();
    std::vector<OC_ProjectionItemContext *> oC_ProjectionItem();
    OC_ProjectionItemContext* oC_ProjectionItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_ProjectionItemsContext* oC_ProjectionItems();

  class  OC_ProjectionItemContext : public antlr4::ParserRuleContext {
  public:
    OC_ProjectionItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ExpressionContext *oC_Expression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *AS();
    OC_VariableContext *oC_Variable();

   
  };

  OC_ProjectionItemContext* oC_ProjectionItem();

  class  OC_OrderContext : public antlr4::ParserRuleContext {
  public:
    OC_OrderContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ORDER();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *BY();
    std::vector<OC_SortItemContext *> oC_SortItem();
    OC_SortItemContext* oC_SortItem(size_t i);

   
  };

  OC_OrderContext* oC_Order();

  class  OC_SkipContext : public antlr4::ParserRuleContext {
  public:
    OC_SkipContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *L_SKIP();
    antlr4::tree::TerminalNode *SP();
    OC_ExpressionContext *oC_Expression();

   
  };

  OC_SkipContext* oC_Skip();

  class  OC_LimitContext : public antlr4::ParserRuleContext {
  public:
    OC_LimitContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LIMIT();
    antlr4::tree::TerminalNode *SP();
    OC_ExpressionContext *oC_Expression();

   
  };

  OC_LimitContext* oC_Limit();

  class  OC_SortItemContext : public antlr4::ParserRuleContext {
  public:
    OC_SortItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ExpressionContext *oC_Expression();
    antlr4::tree::TerminalNode *ASCENDING();
    antlr4::tree::TerminalNode *ASC();
    antlr4::tree::TerminalNode *DESCENDING();
    antlr4::tree::TerminalNode *DESC();
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_SortItemContext* oC_SortItem();

  class  OC_WhereContext : public antlr4::ParserRuleContext {
  public:
    OC_WhereContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WHERE();
    antlr4::tree::TerminalNode *SP();
    OC_ExpressionContext *oC_Expression();

   
  };

  OC_WhereContext* oC_Where();

  class  OC_PatternContext : public antlr4::ParserRuleContext {
  public:
    OC_PatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_PatternPartContext *> oC_PatternPart();
    OC_PatternPartContext* oC_PatternPart(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_PatternContext* oC_Pattern();

  class  OC_PatternPartContext : public antlr4::ParserRuleContext {
  public:
    OC_PatternPartContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_VariableContext *oC_Variable();
    OC_AnonymousPatternPartContext *oC_AnonymousPatternPart();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_PatternPartContext* oC_PatternPart();

  class  OC_AnonymousPatternPartContext : public antlr4::ParserRuleContext {
  public:
    OC_AnonymousPatternPartContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_PatternElementContext *oC_PatternElement();

   
  };

  OC_AnonymousPatternPartContext* oC_AnonymousPatternPart();

  class  OC_PatternElementContext : public antlr4::ParserRuleContext {
  public:
    OC_PatternElementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_NodePatternContext *oC_NodePattern();
    std::vector<OC_PatternElementChainContext *> oC_PatternElementChain();
    OC_PatternElementChainContext* oC_PatternElementChain(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_PatternElementContext *oC_PatternElement();

   
  };

  OC_PatternElementContext* oC_PatternElement();

  class  OC_NodePatternContext : public antlr4::ParserRuleContext {
  public:
    OC_NodePatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_VariableContext *oC_Variable();
    OC_NodeLabelsContext *oC_NodeLabels();
    IC_PropertiesContext *iC_Properties();

   
  };

  OC_NodePatternContext* oC_NodePattern();

  class  OC_PatternElementChainContext : public antlr4::ParserRuleContext {
  public:
    OC_PatternElementChainContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_RelationshipPatternContext *oC_RelationshipPattern();
    OC_NodePatternContext *oC_NodePattern();
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_PatternElementChainContext* oC_PatternElementChain();

  class  OC_RelationshipPatternContext : public antlr4::ParserRuleContext {
  public:
    OC_RelationshipPatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_LeftArrowHeadContext *oC_LeftArrowHead();
    std::vector<OC_DashContext *> oC_Dash();
    OC_DashContext* oC_Dash(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_RelationshipDetailContext *oC_RelationshipDetail();
    OC_RightArrowHeadContext *oC_RightArrowHead();

   
  };

  OC_RelationshipPatternContext* oC_RelationshipPattern();

  class  OC_RelationshipDetailContext : public antlr4::ParserRuleContext {
  public:
    OC_RelationshipDetailContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_VariableContext *oC_Variable();
    OC_RelationshipTypesContext *oC_RelationshipTypes();
    IC_RecursiveDetailContext *iC_RecursiveDetail();
    IC_PropertiesContext *iC_Properties();

   
  };

  OC_RelationshipDetailContext* oC_RelationshipDetail();

  class  IC_PropertiesContext : public antlr4::ParserRuleContext {
  public:
    IC_PropertiesContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<OC_PropertyKeyNameContext *> oC_PropertyKeyName();
    OC_PropertyKeyNameContext* oC_PropertyKeyName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    std::vector<OC_ExpressionContext *> oC_Expression();
    OC_ExpressionContext* oC_Expression(size_t i);

   
  };

  IC_PropertiesContext* iC_Properties();

  class  OC_RelationshipTypesContext : public antlr4::ParserRuleContext {
  public:
    OC_RelationshipTypesContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    std::vector<OC_RelTypeNameContext *> oC_RelTypeName();
    OC_RelTypeNameContext* oC_RelTypeName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_RelationshipTypesContext* oC_RelationshipTypes();

  class  OC_NodeLabelsContext : public antlr4::ParserRuleContext {
  public:
    OC_NodeLabelsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    std::vector<OC_LabelNameContext *> oC_LabelName();
    OC_LabelNameContext* oC_LabelName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_NodeLabelsContext* oC_NodeLabels();

  class  IC_RecursiveDetailContext : public antlr4::ParserRuleContext {
  public:
    IC_RecursiveDetailContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *STAR();
    IC_RecursiveTypeContext *iC_RecursiveType();
    OC_RangeLiteralContext *oC_RangeLiteral();
    IC_RecursiveComprehensionContext *iC_RecursiveComprehension();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_RecursiveDetailContext* iC_RecursiveDetail();

  class  IC_RecursiveTypeContext : public antlr4::ParserRuleContext {
  public:
    IC_RecursiveTypeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WSHORTEST();
    OC_PropertyKeyNameContext *oC_PropertyKeyName();
    antlr4::tree::TerminalNode *ALL();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *SHORTEST();
    antlr4::tree::TerminalNode *TRAIL();
    antlr4::tree::TerminalNode *ACYCLIC();

   
  };

  IC_RecursiveTypeContext* iC_RecursiveType();

  class  OC_RangeLiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_RangeLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DOTDOT();
    OC_LowerBoundContext *oC_LowerBound();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_UpperBoundContext *oC_UpperBound();
    OC_IntegerLiteralContext *oC_IntegerLiteral();

   
  };

  OC_RangeLiteralContext* oC_RangeLiteral();

  class  IC_RecursiveComprehensionContext : public antlr4::ParserRuleContext {
  public:
    IC_RecursiveComprehensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_VariableContext *> oC_Variable();
    OC_VariableContext* oC_Variable(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_WhereContext *oC_Where();
    std::vector<IC_RecursiveProjectionItemsContext *> iC_RecursiveProjectionItems();
    IC_RecursiveProjectionItemsContext* iC_RecursiveProjectionItems(size_t i);

   
  };

  IC_RecursiveComprehensionContext* iC_RecursiveComprehension();

  class  IC_RecursiveProjectionItemsContext : public antlr4::ParserRuleContext {
  public:
    IC_RecursiveProjectionItemsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_ProjectionItemsContext *oC_ProjectionItems();

   
  };

  IC_RecursiveProjectionItemsContext* iC_RecursiveProjectionItems();

  class  OC_LowerBoundContext : public antlr4::ParserRuleContext {
  public:
    OC_LowerBoundContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DecimalInteger();

   
  };

  OC_LowerBoundContext* oC_LowerBound();

  class  OC_UpperBoundContext : public antlr4::ParserRuleContext {
  public:
    OC_UpperBoundContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DecimalInteger();

   
  };

  OC_UpperBoundContext* oC_UpperBound();

  class  OC_LabelNameContext : public antlr4::ParserRuleContext {
  public:
    OC_LabelNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_SchemaNameContext *> oC_SchemaName();
    OC_SchemaNameContext* oC_SchemaName(size_t i);

   
  };

  OC_LabelNameContext* oC_LabelName();

  class  OC_RelTypeNameContext : public antlr4::ParserRuleContext {
  public:
    OC_RelTypeNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SchemaNameContext *oC_SchemaName();

   
  };

  OC_RelTypeNameContext* oC_RelTypeName();

  class  OC_ExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_ExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_OrExpressionContext *oC_OrExpression();

   
  };

  OC_ExpressionContext* oC_Expression();

  class  OC_OrExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_OrExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_XorExpressionContext *> oC_XorExpression();
    OC_XorExpressionContext* oC_XorExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<antlr4::tree::TerminalNode *> OR();
    antlr4::tree::TerminalNode* OR(size_t i);

   
  };

  OC_OrExpressionContext* oC_OrExpression();

  class  OC_XorExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_XorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_AndExpressionContext *> oC_AndExpression();
    OC_AndExpressionContext* oC_AndExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<antlr4::tree::TerminalNode *> XOR();
    antlr4::tree::TerminalNode* XOR(size_t i);

   
  };

  OC_XorExpressionContext* oC_XorExpression();

  class  OC_AndExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_AndExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_NotExpressionContext *> oC_NotExpression();
    OC_NotExpressionContext* oC_NotExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    std::vector<antlr4::tree::TerminalNode *> AND();
    antlr4::tree::TerminalNode* AND(size_t i);

   
  };

  OC_AndExpressionContext* oC_AndExpression();

  class  OC_NotExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_NotExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ComparisonExpressionContext *oC_ComparisonExpression();
    std::vector<antlr4::tree::TerminalNode *> NOT();
    antlr4::tree::TerminalNode* NOT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_NotExpressionContext* oC_NotExpression();

  class  OC_ComparisonExpressionContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *invalid_not_equalToken = nullptr;
    OC_ComparisonExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_BitwiseOrOperatorExpressionContext *> iC_BitwiseOrOperatorExpression();
    IC_BitwiseOrOperatorExpressionContext* iC_BitwiseOrOperatorExpression(size_t i);
    std::vector<IC_ComparisonOperatorContext *> iC_ComparisonOperator();
    IC_ComparisonOperatorContext* iC_ComparisonOperator(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *INVALID_NOT_EQUAL();

   
  };

  OC_ComparisonExpressionContext* oC_ComparisonExpression();

  class  IC_ComparisonOperatorContext : public antlr4::ParserRuleContext {
  public:
    IC_ComparisonOperatorContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;

   
  };

  IC_ComparisonOperatorContext* iC_ComparisonOperator();

  class  IC_BitwiseOrOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    IC_BitwiseOrOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_BitwiseAndOperatorExpressionContext *> iC_BitwiseAndOperatorExpression();
    IC_BitwiseAndOperatorExpressionContext* iC_BitwiseAndOperatorExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_BitwiseOrOperatorExpressionContext* iC_BitwiseOrOperatorExpression();

  class  IC_BitwiseAndOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    IC_BitwiseAndOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_BitShiftOperatorExpressionContext *> iC_BitShiftOperatorExpression();
    IC_BitShiftOperatorExpressionContext* iC_BitShiftOperatorExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_BitwiseAndOperatorExpressionContext* iC_BitwiseAndOperatorExpression();

  class  IC_BitShiftOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    IC_BitShiftOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_AddOrSubtractExpressionContext *> oC_AddOrSubtractExpression();
    OC_AddOrSubtractExpressionContext* oC_AddOrSubtractExpression(size_t i);
    std::vector<IC_BitShiftOperatorContext *> iC_BitShiftOperator();
    IC_BitShiftOperatorContext* iC_BitShiftOperator(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_BitShiftOperatorExpressionContext* iC_BitShiftOperatorExpression();

  class  IC_BitShiftOperatorContext : public antlr4::ParserRuleContext {
  public:
    IC_BitShiftOperatorContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;

   
  };

  IC_BitShiftOperatorContext* iC_BitShiftOperator();

  class  OC_AddOrSubtractExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_AddOrSubtractExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_MultiplyDivideModuloExpressionContext *> oC_MultiplyDivideModuloExpression();
    OC_MultiplyDivideModuloExpressionContext* oC_MultiplyDivideModuloExpression(size_t i);
    std::vector<IC_AddOrSubtractOperatorContext *> iC_AddOrSubtractOperator();
    IC_AddOrSubtractOperatorContext* iC_AddOrSubtractOperator(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_AddOrSubtractExpressionContext* oC_AddOrSubtractExpression();

  class  IC_AddOrSubtractOperatorContext : public antlr4::ParserRuleContext {
  public:
    IC_AddOrSubtractOperatorContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MINUS();

   
  };

  IC_AddOrSubtractOperatorContext* iC_AddOrSubtractOperator();

  class  OC_MultiplyDivideModuloExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_MultiplyDivideModuloExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_PowerOfExpressionContext *> oC_PowerOfExpression();
    OC_PowerOfExpressionContext* oC_PowerOfExpression(size_t i);
    std::vector<IC_MultiplyDivideModuloOperatorContext *> iC_MultiplyDivideModuloOperator();
    IC_MultiplyDivideModuloOperatorContext* iC_MultiplyDivideModuloOperator(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_MultiplyDivideModuloExpressionContext* oC_MultiplyDivideModuloExpression();

  class  IC_MultiplyDivideModuloOperatorContext : public antlr4::ParserRuleContext {
  public:
    IC_MultiplyDivideModuloOperatorContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *STAR();

   
  };

  IC_MultiplyDivideModuloOperatorContext* iC_MultiplyDivideModuloOperator();

  class  OC_PowerOfExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_PowerOfExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_StringListNullOperatorExpressionContext *> oC_StringListNullOperatorExpression();
    OC_StringListNullOperatorExpressionContext* oC_StringListNullOperatorExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_PowerOfExpressionContext* oC_PowerOfExpression();

  class  OC_StringListNullOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_StringListNullOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_UnaryAddSubtractOrFactorialExpressionContext *oC_UnaryAddSubtractOrFactorialExpression();
    OC_StringOperatorExpressionContext *oC_StringOperatorExpression();
    OC_NullOperatorExpressionContext *oC_NullOperatorExpression();
    std::vector<OC_ListOperatorExpressionContext *> oC_ListOperatorExpression();
    OC_ListOperatorExpressionContext* oC_ListOperatorExpression(size_t i);

   
  };

  OC_StringListNullOperatorExpressionContext* oC_StringListNullOperatorExpression();

  class  OC_ListOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_ListOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *IN();
    OC_PropertyOrLabelsExpressionContext *oC_PropertyOrLabelsExpression();
    std::vector<OC_ExpressionContext *> oC_Expression();
    OC_ExpressionContext* oC_Expression(size_t i);
    antlr4::tree::TerminalNode *COLON();
    antlr4::tree::TerminalNode *DOTDOT();

   
  };

  OC_ListOperatorExpressionContext* oC_ListOperatorExpression();

  class  OC_StringOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_StringOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_PropertyOrLabelsExpressionContext *oC_PropertyOrLabelsExpression();
    OC_RegularExpressionContext *oC_RegularExpression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *STARTS();
    antlr4::tree::TerminalNode *WITH();
    antlr4::tree::TerminalNode *ENDS();
    antlr4::tree::TerminalNode *CONTAINS();

   
  };

  OC_StringOperatorExpressionContext* oC_StringOperatorExpression();

  class  OC_RegularExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_RegularExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_RegularExpressionContext* oC_RegularExpression();

  class  OC_NullOperatorExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_NullOperatorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *IS();
    antlr4::tree::TerminalNode *NULL_();
    antlr4::tree::TerminalNode *NOT();

   
  };

  OC_NullOperatorExpressionContext* oC_NullOperatorExpression();

  class  OC_UnaryAddSubtractOrFactorialExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_UnaryAddSubtractOrFactorialExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_PropertyOrLabelsExpressionContext *oC_PropertyOrLabelsExpression();
    std::vector<antlr4::tree::TerminalNode *> MINUS();
    antlr4::tree::TerminalNode* MINUS(size_t i);
    antlr4::tree::TerminalNode *FACTORIAL();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_UnaryAddSubtractOrFactorialExpressionContext* oC_UnaryAddSubtractOrFactorialExpression();

  class  OC_PropertyOrLabelsExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_PropertyOrLabelsExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_AtomContext *oC_Atom();
    std::vector<OC_PropertyLookupContext *> oC_PropertyLookup();
    OC_PropertyLookupContext* oC_PropertyLookup(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_PropertyOrLabelsExpressionContext* oC_PropertyOrLabelsExpression();

  class  OC_AtomContext : public antlr4::ParserRuleContext {
  public:
    OC_AtomContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_LiteralContext *oC_Literal();
    OC_ParameterContext *oC_Parameter();
    OC_CaseExpressionContext *oC_CaseExpression();
    OC_ParenthesizedExpressionContext *oC_ParenthesizedExpression();
    OC_FunctionInvocationContext *oC_FunctionInvocation();
    OC_PathPatternsContext *oC_PathPatterns();
    OC_ExistCountSubqueryContext *oC_ExistCountSubquery();
    OC_VariableContext *oC_Variable();
    OC_QuantifierContext *oC_Quantifier();

   
  };

  OC_AtomContext* oC_Atom();

  class  OC_QuantifierContext : public antlr4::ParserRuleContext {
  public:
    OC_QuantifierContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ALL();
    OC_FilterExpressionContext *oC_FilterExpression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *ANY();
    antlr4::tree::TerminalNode *NONE();
    antlr4::tree::TerminalNode *SINGLE();

   
  };

  OC_QuantifierContext* oC_Quantifier();

  class  OC_FilterExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_FilterExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_IdInCollContext *oC_IdInColl();
    antlr4::tree::TerminalNode *SP();
    OC_WhereContext *oC_Where();

   
  };

  OC_FilterExpressionContext* oC_FilterExpression();

  class  OC_IdInCollContext : public antlr4::ParserRuleContext {
  public:
    OC_IdInCollContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_VariableContext *oC_Variable();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *IN();
    OC_ExpressionContext *oC_Expression();

   
  };

  OC_IdInCollContext* oC_IdInColl();

  class  OC_LiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_LiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_NumberLiteralContext *oC_NumberLiteral();
    antlr4::tree::TerminalNode *StringLiteral();
    OC_BooleanLiteralContext *oC_BooleanLiteral();
    antlr4::tree::TerminalNode *NULL_();
    OC_ListLiteralContext *oC_ListLiteral();
    IC_StructLiteralContext *iC_StructLiteral();

   
  };

  OC_LiteralContext* oC_Literal();

  class  OC_BooleanLiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_BooleanLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *TRUE();
    antlr4::tree::TerminalNode *FALSE();

   
  };

  OC_BooleanLiteralContext* oC_BooleanLiteral();

  class  OC_ListLiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_ListLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_ExpressionContext *oC_Expression();
    std::vector<IC_ListEntryContext *> iC_ListEntry();
    IC_ListEntryContext* iC_ListEntry(size_t i);

   
  };

  OC_ListLiteralContext* oC_ListLiteral();

  class  IC_ListEntryContext : public antlr4::ParserRuleContext {
  public:
    IC_ListEntryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *SP();
    OC_ExpressionContext *oC_Expression();

   
  };

  IC_ListEntryContext* iC_ListEntry();

  class  IC_StructLiteralContext : public antlr4::ParserRuleContext {
  public:
    IC_StructLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<IC_StructFieldContext *> iC_StructField();
    IC_StructFieldContext* iC_StructField(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_StructLiteralContext* iC_StructLiteral();

  class  IC_StructFieldContext : public antlr4::ParserRuleContext {
  public:
    IC_StructFieldContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COLON();
    OC_ExpressionContext *oC_Expression();
    OC_SymbolicNameContext *oC_SymbolicName();
    antlr4::tree::TerminalNode *StringLiteral();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_StructFieldContext* iC_StructField();

  class  OC_ParenthesizedExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_ParenthesizedExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ExpressionContext *oC_Expression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_ParenthesizedExpressionContext* oC_ParenthesizedExpression();

  class  OC_FunctionInvocationContext : public antlr4::ParserRuleContext {
  public:
    OC_FunctionInvocationContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COUNT();
    antlr4::tree::TerminalNode *STAR();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *CAST();
    std::vector<IC_FunctionParameterContext *> iC_FunctionParameter();
    IC_FunctionParameterContext* iC_FunctionParameter(size_t i);
    antlr4::tree::TerminalNode *AS();
    IC_DataTypeContext *iC_DataType();
    OC_FunctionNameContext *oC_FunctionName();
    antlr4::tree::TerminalNode *DISTINCT();

   
  };

  OC_FunctionInvocationContext* oC_FunctionInvocation();

  class  OC_FunctionNameContext : public antlr4::ParserRuleContext {
  public:
    OC_FunctionNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();

   
  };

  OC_FunctionNameContext* oC_FunctionName();

  class  IC_FunctionParameterContext : public antlr4::ParserRuleContext {
  public:
    IC_FunctionParameterContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_ExpressionContext *oC_Expression();
    OC_SymbolicNameContext *oC_SymbolicName();
    antlr4::tree::TerminalNode *COLON();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    IC_LambdaParameterContext *iC_LambdaParameter();

   
  };

  IC_FunctionParameterContext* iC_FunctionParameter();

  class  IC_LambdaParameterContext : public antlr4::ParserRuleContext {
  public:
    IC_LambdaParameterContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    IC_LambdaVarsContext *iC_LambdaVars();
    antlr4::tree::TerminalNode *MINUS();
    OC_ExpressionContext *oC_Expression();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_LambdaParameterContext* iC_LambdaParameter();

  class  IC_LambdaVarsContext : public antlr4::ParserRuleContext {
  public:
    IC_LambdaVarsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_SymbolicNameContext *> oC_SymbolicName();
    OC_SymbolicNameContext* oC_SymbolicName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  IC_LambdaVarsContext* iC_LambdaVars();

  class  OC_PathPatternsContext : public antlr4::ParserRuleContext {
  public:
    OC_PathPatternsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_NodePatternContext *oC_NodePattern();
    std::vector<OC_PatternElementChainContext *> oC_PatternElementChain();
    OC_PatternElementChainContext* oC_PatternElementChain(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_PathPatternsContext* oC_PathPatterns();

  class  OC_ExistCountSubqueryContext : public antlr4::ParserRuleContext {
  public:
    OC_ExistCountSubqueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MATCH();
    OC_PatternContext *oC_Pattern();
    antlr4::tree::TerminalNode *EXISTS();
    antlr4::tree::TerminalNode *COUNT();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    OC_WhereContext *oC_Where();
    IC_HintContext *iC_Hint();

   
  };

  OC_ExistCountSubqueryContext* oC_ExistCountSubquery();

  class  OC_PropertyLookupContext : public antlr4::ParserRuleContext {
  public:
    OC_PropertyLookupContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_PropertyKeyNameContext *oC_PropertyKeyName();
    antlr4::tree::TerminalNode *STAR();
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_PropertyLookupContext* oC_PropertyLookup();

  class  OC_CaseExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_CaseExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *END();
    antlr4::tree::TerminalNode *ELSE();
    std::vector<OC_ExpressionContext *> oC_Expression();
    OC_ExpressionContext* oC_Expression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);
    antlr4::tree::TerminalNode *CASE();
    std::vector<OC_CaseAlternativeContext *> oC_CaseAlternative();
    OC_CaseAlternativeContext* oC_CaseAlternative(size_t i);

   
  };

  OC_CaseExpressionContext* oC_CaseExpression();

  class  OC_CaseAlternativeContext : public antlr4::ParserRuleContext {
  public:
    OC_CaseAlternativeContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *WHEN();
    std::vector<OC_ExpressionContext *> oC_Expression();
    OC_ExpressionContext* oC_Expression(size_t i);
    antlr4::tree::TerminalNode *THEN();
    std::vector<antlr4::tree::TerminalNode *> SP();
    antlr4::tree::TerminalNode* SP(size_t i);

   
  };

  OC_CaseAlternativeContext* oC_CaseAlternative();

  class  OC_VariableContext : public antlr4::ParserRuleContext {
  public:
    OC_VariableContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();

   
  };

  OC_VariableContext* oC_Variable();

  class  OC_NumberLiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_NumberLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_DoubleLiteralContext *oC_DoubleLiteral();
    OC_IntegerLiteralContext *oC_IntegerLiteral();

   
  };

  OC_NumberLiteralContext* oC_NumberLiteral();

  class  OC_ParameterContext : public antlr4::ParserRuleContext {
  public:
    OC_ParameterContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();
    antlr4::tree::TerminalNode *DecimalInteger();

   
  };

  OC_ParameterContext* oC_Parameter();

  class  OC_PropertyExpressionContext : public antlr4::ParserRuleContext {
  public:
    OC_PropertyExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_AtomContext *oC_Atom();
    OC_PropertyLookupContext *oC_PropertyLookup();
    antlr4::tree::TerminalNode *SP();

   
  };

  OC_PropertyExpressionContext* oC_PropertyExpression();

  class  OC_PropertyKeyNameContext : public antlr4::ParserRuleContext {
  public:
    OC_PropertyKeyNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OC_SymbolicNameContext *oC_SymbolicName();

   
  };

  OC_PropertyKeyNameContext* oC_PropertyKeyName();

  class  OC_IntegerLiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_IntegerLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DecimalInteger();

   
  };

  OC_IntegerLiteralContext* oC_IntegerLiteral();

  class  OC_DoubleLiteralContext : public antlr4::ParserRuleContext {
  public:
    OC_DoubleLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ExponentDecimalReal();
    antlr4::tree::TerminalNode *RegularDecimalReal();

   
  };

  OC_DoubleLiteralContext* oC_DoubleLiteral();

  class  OC_SchemaNameContext : public antlr4::ParserRuleContext {
  public:
    OC_SchemaNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<OC_SymbolicNameContext *> oC_SymbolicName();
    OC_SymbolicNameContext* oC_SymbolicName(size_t i);

   
  };

  OC_SchemaNameContext* oC_SchemaName();

  class  OC_SymbolicNameContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *escapedsymbolicnameToken = nullptr;
    OC_SymbolicNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *UnescapedSymbolicName();
    antlr4::tree::TerminalNode *EscapedSymbolicName();
    antlr4::tree::TerminalNode *HexLetter();
    IC_NonReservedKeywordsContext *iC_NonReservedKeywords();

   
  };

  OC_SymbolicNameContext* oC_SymbolicName();

  class  IC_NonReservedKeywordsContext : public antlr4::ParserRuleContext {
  public:
    IC_NonReservedKeywordsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COMMENT();
    antlr4::tree::TerminalNode *ADD();
    antlr4::tree::TerminalNode *ALTER();
    antlr4::tree::TerminalNode *AS();
    antlr4::tree::TerminalNode *ATTACH();
    antlr4::tree::TerminalNode *BEGIN();
    antlr4::tree::TerminalNode *BY();
    antlr4::tree::TerminalNode *CALL();
    antlr4::tree::TerminalNode *CHECKPOINT();
    antlr4::tree::TerminalNode *COMMIT();
    antlr4::tree::TerminalNode *CONTAINS();
    antlr4::tree::TerminalNode *COPY();
    antlr4::tree::TerminalNode *COUNT();
    antlr4::tree::TerminalNode *CYCLE();
    antlr4::tree::TerminalNode *DATABASE();
    antlr4::tree::TerminalNode *DECIMAL();
    antlr4::tree::TerminalNode *DELETE();
    antlr4::tree::TerminalNode *DETACH();
    antlr4::tree::TerminalNode *DROP();
    antlr4::tree::TerminalNode *EXPLAIN();
    antlr4::tree::TerminalNode *EXPORT();
    antlr4::tree::TerminalNode *EXTENSION();
    antlr4::tree::TerminalNode *FORCE();
    antlr4::tree::TerminalNode *GRAPH();
    antlr4::tree::TerminalNode *IF();
    antlr4::tree::TerminalNode *IS();
    antlr4::tree::TerminalNode *IMPORT();
    antlr4::tree::TerminalNode *INCREMENT();
    antlr4::tree::TerminalNode *KEY();
    antlr4::tree::TerminalNode *LOAD();
    antlr4::tree::TerminalNode *LOGICAL();
    antlr4::tree::TerminalNode *MATCH();
    antlr4::tree::TerminalNode *MAXVALUE();
    antlr4::tree::TerminalNode *MERGE();
    antlr4::tree::TerminalNode *MINVALUE();
    antlr4::tree::TerminalNode *NO();
    antlr4::tree::TerminalNode *NODE();
    antlr4::tree::TerminalNode *PROJECT();
    antlr4::tree::TerminalNode *READ();
    antlr4::tree::TerminalNode *REL();
    antlr4::tree::TerminalNode *RENAME();
    antlr4::tree::TerminalNode *RETURN();
    antlr4::tree::TerminalNode *ROLLBACK();
    antlr4::tree::TerminalNode *ROLE();
    antlr4::tree::TerminalNode *SEQUENCE();
    antlr4::tree::TerminalNode *SET();
    antlr4::tree::TerminalNode *START();
    antlr4::tree::TerminalNode *STRUCT();
    antlr4::tree::TerminalNode *L_SKIP();
    antlr4::tree::TerminalNode *LIMIT();
    antlr4::tree::TerminalNode *TRANSACTION();
    antlr4::tree::TerminalNode *TYPE();
    antlr4::tree::TerminalNode *USE();
    antlr4::tree::TerminalNode *UNINSTALL();
    antlr4::tree::TerminalNode *UPDATE();
    antlr4::tree::TerminalNode *WRITE();
    antlr4::tree::TerminalNode *FROM();
    antlr4::tree::TerminalNode *TO();
    antlr4::tree::TerminalNode *YIELD();
    antlr4::tree::TerminalNode *USER();
    antlr4::tree::TerminalNode *PASSWORD();
    antlr4::tree::TerminalNode *MAP();

   
  };

  IC_NonReservedKeywordsContext* iC_NonReservedKeywords();

  class  OC_LeftArrowHeadContext : public antlr4::ParserRuleContext {
  public:
    OC_LeftArrowHeadContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;

   
  };

  OC_LeftArrowHeadContext* oC_LeftArrowHead();

  class  OC_RightArrowHeadContext : public antlr4::ParserRuleContext {
  public:
    OC_RightArrowHeadContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;

   
  };

  OC_RightArrowHeadContext* oC_RightArrowHead();

  class  OC_DashContext : public antlr4::ParserRuleContext {
  public:
    OC_DashContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MINUS();

   
  };

  OC_DashContext* oC_Dash();


  bool sempred(antlr4::RuleContext *_localctx, size_t ruleIndex, size_t predicateIndex) override;

  bool iC_DataTypeSempred(IC_DataTypeContext *_localctx, size_t predicateIndex);
  bool iC_JoinNodeSempred(IC_JoinNodeContext *_localctx, size_t predicateIndex);

  // By default the static state used to implement the parser is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

      virtual void notifyQueryNotConcludeWithReturn(antlr4::Token* startToken) {};
      virtual void notifyNodePatternWithoutParentheses(std::string nodeName, antlr4::Token* startToken) {};
      virtual void notifyInvalidNotEqualOperator(antlr4::Token* startToken) {};
      virtual void notifyEmptyToken(antlr4::Token* startToken) {};
      virtual void notifyReturnNotAtEnd(antlr4::Token* startToken) {};
      virtual void notifyNonBinaryComparison(antlr4::Token* startToken) {};

};

