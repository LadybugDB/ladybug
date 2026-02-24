
iC_Statements
    : oC_Cypher ( SP? ';' SP? oC_Cypher )* SP? EOF ;

oC_Cypher
    : oC_AnyCypherOption? SP? ( oC_Statement ) ( SP? ';' )?;

oC_Statement
    : oC_Query
        | iC_CreateUser
        | iC_CreateRole
        | iC_CreateNodeTable
        | iC_CreateRelTable
        | iC_CreateSequence
        | iC_CreateType
        | iC_Drop
        | iC_AlterTable
        | iC_CopyFrom
        | iC_CopyFromByColumn
        | iC_CopyTO
        | iC_StandaloneCall
        | iC_CreateMacro
        | iC_CommentOn
        | iC_Transaction
        | iC_Extension
        | iC_ExportDatabase
        | iC_ImportDatabase
        | iC_AttachDatabase
        | iC_DetachDatabase
        | iC_UseDatabase
        | iC_CreateGraph
        | iC_UseGraph;

iC_CopyFrom
    : COPY SP oC_SchemaName iC_ColumnNames? SP FROM SP iC_ScanSource ( SP? '(' SP? iC_Options SP? ')' )? ;

iC_ColumnNames
    : SP? '(' SP? (oC_SchemaName ( SP? ',' SP? oC_SchemaName )* SP?)? ')';

iC_ScanSource
    : iC_FilePaths
        | '(' SP? oC_Query SP? ')'
        | oC_Parameter
        | oC_Variable
        | oC_Variable '.' SP? oC_SchemaName
        | oC_FunctionInvocation ;

iC_CopyFromByColumn
    : COPY SP oC_SchemaName SP FROM SP '(' SP? StringLiteral ( SP? ',' SP? StringLiteral )* ')' SP BY SP COLUMN ;

iC_CopyTO
    : COPY SP '(' SP? oC_Query SP? ')' SP TO SP StringLiteral ( SP? '(' SP? iC_Options SP? ')' )? ;

iC_ExportDatabase
    : EXPORT SP DATABASE SP StringLiteral ( SP? '(' SP? iC_Options SP? ')' )? ;

iC_ImportDatabase
    : IMPORT SP DATABASE SP StringLiteral;

iC_AttachDatabase
    : ATTACH SP StringLiteral (SP AS SP oC_SchemaName)? SP '(' SP? DBTYPE SP oC_SymbolicName (SP? ',' SP? iC_Options)? SP? ')' ;

iC_Option
    : oC_SymbolicName (SP? '=' SP? | SP*) oC_Literal | oC_SymbolicName;

iC_Options
    : iC_Option ( SP? ',' SP? iC_Option )* ;

iC_DetachDatabase
    : DETACH SP oC_SchemaName;

iC_UseDatabase
    : USE SP oC_SchemaName;

iC_CreateGraph
    : CREATE SP GRAPH SP oC_SchemaName (SP ANY)?;

iC_UseGraph
    : USE SP GRAPH SP oC_SchemaName;

iC_StandaloneCall
    : CALL SP oC_SymbolicName SP? '=' SP? oC_Expression
        | CALL SP oC_FunctionInvocation;

iC_CommentOn
    : COMMENT SP ON SP TABLE SP oC_SchemaName SP IS SP StringLiteral ;

iC_CreateMacro
    : CREATE SP MACRO SP oC_FunctionName SP? '(' SP? iC_PositionalArgs? SP? iC_DefaultArg? ( SP? ',' SP? iC_DefaultArg )* SP? ')' SP AS SP oC_Expression ;

iC_PositionalArgs
    : oC_SymbolicName ( SP? ',' SP? oC_SymbolicName )* ;

iC_DefaultArg
    : oC_SymbolicName SP? ':' '=' SP? oC_Literal ;

iC_FilePaths
    : '[' SP? StringLiteral ( SP? ',' SP? StringLiteral )* ']'
        | StringLiteral
        | GLOB SP? '(' SP? StringLiteral SP? ')' ;

iC_IfNotExists
    : IF SP NOT SP EXISTS ;

iC_CreateNodeTable
    : CREATE SP NODE SP TABLE SP (iC_IfNotExists SP)? oC_SchemaName ( SP? '(' SP? iC_PropertyDefinitions SP? ( ',' SP? iC_CreateNodeConstraint )? SP? ')' | SP AS SP oC_Query ) ( SP WITH SP? '(' SP? iC_Options SP? ')')? ;

iC_CreateRelTable
    : CREATE SP REL SP TABLE ( SP GROUP )? ( SP iC_IfNotExists )? SP oC_SchemaName
        SP? '(' SP?
            iC_FromToConnections SP? (
            ( ',' SP? iC_PropertyDefinitions SP? )?
            ( ',' SP? oC_SymbolicName SP? )? // Constraints
            ')'
            | ')' SP AS SP oC_Query )
         ( SP WITH SP? '(' SP? iC_Options SP? ')')? ;

iC_FromToConnections
    : iC_FromToConnection ( SP? ',' SP? iC_FromToConnection )* ;

iC_FromToConnection
    : FROM SP oC_SchemaName SP TO SP oC_SchemaName ;

iC_CreateSequence
    : CREATE SP SEQUENCE SP (iC_IfNotExists SP)? oC_SchemaName (SP iC_SequenceOptions)* ;

iC_CreateType
    : CREATE SP TYPE SP oC_SchemaName SP AS SP iC_DataType SP? ;

iC_SequenceOptions
    : iC_IncrementBy
        | iC_MinValue
        | iC_MaxValue
        | iC_StartWith
        | iC_Cycle;

iC_WithPasswd
    : SP WITH SP PASSWORD SP StringLiteral ;

iC_CreateUser
    : CREATE SP USER SP (iC_IfNotExists SP)? oC_Variable iC_WithPasswd? ;

iC_CreateRole
    : CREATE SP ROLE SP (iC_IfNotExists SP)? oC_Variable ;

iC_IncrementBy : INCREMENT SP ( BY SP )? MINUS? oC_IntegerLiteral ;

iC_MinValue : (NO SP MINVALUE) | (MINVALUE SP MINUS? oC_IntegerLiteral) ;

iC_MaxValue : (NO SP MAXVALUE) | (MAXVALUE SP MINUS? oC_IntegerLiteral) ;

iC_StartWith : START SP ( WITH SP )? MINUS? oC_IntegerLiteral ;

iC_Cycle : (NO SP)? CYCLE ;

iC_IfExists
    : IF SP EXISTS ;

iC_Drop
    : DROP SP (TABLE | SEQUENCE | MACRO | GRAPH) SP (iC_IfExists SP)? oC_SchemaName ;

iC_AlterTable
    : ALTER SP TABLE SP oC_SchemaName SP iC_AlterOptions ;

iC_AlterOptions
    : iC_AddProperty
        | iC_DropProperty
        | iC_RenameTable
        | iC_RenameProperty
        | iC_AddFromToConnection
        | iC_DropFromToConnection;

iC_AddProperty
    : ADD SP (iC_IfNotExists SP)? oC_PropertyKeyName SP iC_DataType ( SP iC_Default )? ;

iC_Default
    : DEFAULT SP oC_Expression ;

iC_DropProperty
    : DROP SP (iC_IfExists SP)? oC_PropertyKeyName ;

iC_RenameTable
    : RENAME SP TO SP oC_SchemaName ;

iC_RenameProperty
    : RENAME SP oC_PropertyKeyName SP TO SP oC_PropertyKeyName ;

iC_AddFromToConnection
    : ADD SP (iC_IfNotExists SP)? iC_FromToConnection ;

iC_DropFromToConnection
    : DROP SP (iC_IfExists SP)? iC_FromToConnection ;

iC_ColumnDefinitions: iC_ColumnDefinition ( SP? ',' SP? iC_ColumnDefinition )* ;

iC_ColumnDefinition : oC_PropertyKeyName SP iC_DataType ;

iC_PropertyDefinitions : iC_PropertyDefinition ( SP? ',' SP? iC_PropertyDefinition )* ;

iC_PropertyDefinition : iC_ColumnDefinition ( SP iC_Default )? ( SP PRIMARY SP KEY)?;

iC_CreateNodeConstraint : PRIMARY SP KEY SP? '(' SP? oC_PropertyKeyName SP? ')' ;

DECIMAL: ( 'D' | 'd' ) ( 'E' | 'e' ) ( 'C' | 'c' ) ( 'I' | 'i' ) ( 'M' | 'm' ) ( 'A' | 'a' ) ( 'L' | 'l' ) ;

iC_UnionType
    : UNION SP? '(' SP? iC_ColumnDefinitions SP? ')' ;

iC_StructType
    : STRUCT SP? '(' SP? iC_ColumnDefinitions SP? ')' ;

iC_MapType
    : MAP SP? '(' SP? iC_DataType SP? ',' SP? iC_DataType SP? ')' ;

iC_DecimalType
    : DECIMAL SP? '(' SP? oC_IntegerLiteral SP? ',' SP? oC_IntegerLiteral SP? ')' ;

iC_DataType
    : oC_SymbolicName
        | iC_DataType iC_ListIdentifiers
        | iC_UnionType
        | iC_StructType
        | iC_MapType
        | iC_DecimalType ;

iC_ListIdentifiers : iC_ListIdentifier ( iC_ListIdentifier )* ;

iC_ListIdentifier : '[' oC_IntegerLiteral? ']' ;

oC_AnyCypherOption
    : oC_Explain
        | oC_Profile ;

oC_Explain
    : EXPLAIN (SP LOGICAL)? ;

oC_Profile
    : PROFILE ;

iC_Transaction
    : BEGIN SP TRANSACTION
        | BEGIN SP TRANSACTION SP READ SP ONLY
        | COMMIT
        | ROLLBACK
        | CHECKPOINT;

iC_Extension
    : iC_LoadExtension
        | iC_InstallExtension
        | iC_UninstallExtension
        | iC_UpdateExtension ;

iC_LoadExtension
    : LOAD SP (EXTENSION SP)? ( StringLiteral | oC_Variable ) ;

iC_InstallExtension
    : (FORCE SP)? INSTALL SP oC_Variable (SP FROM SP StringLiteral)?;

iC_UninstallExtension
    : UNINSTALL SP oC_Variable;

iC_UpdateExtension
    : UPDATE SP oC_Variable;

oC_Query
    : oC_RegularQuery ;

oC_RegularQuery
    : oC_SingleQuery ( SP? oC_Union )*
        | (oC_Return SP? )+ oC_SingleQuery { notifyReturnNotAtEnd($ctx->start); }
        ;

oC_Union
     :  ( UNION SP ALL SP? oC_SingleQuery )
         | ( UNION SP? oC_SingleQuery ) ;

oC_SingleQuery
    : oC_SinglePartQuery
        | oC_MultiPartQuery
        ;

oC_SinglePartQuery
    : ( oC_ReadingClause SP? )* oC_Return
        | ( ( oC_ReadingClause SP? )* oC_UpdatingClause ( SP? oC_UpdatingClause )* ( SP? oC_Return )? )
        ;

oC_MultiPartQuery
    : ( iC_QueryPart SP? )+ oC_SinglePartQuery;

iC_QueryPart
    : (oC_ReadingClause SP? )* ( oC_UpdatingClause SP? )* oC_With ;

oC_UpdatingClause
    : oC_Create
        | oC_Merge
        | oC_Set
        | oC_Delete
        ;

oC_ReadingClause
    : oC_Match
        | oC_Unwind
        | iC_InQueryCall
        | iC_LoadFrom
        ;

iC_LoadFrom
    :  LOAD ( SP WITH SP HEADERS SP? '(' SP? iC_ColumnDefinitions SP? ')' )? SP FROM SP iC_ScanSource (SP? '(' SP? iC_Options SP? ')')? (SP? oC_Where)? ;


oC_YieldItem
         :  ( oC_Variable SP AS SP )? oC_Variable ;

oC_YieldItems
          :  oC_YieldItem ( SP? ',' SP? oC_YieldItem )* ;

iC_InQueryCall
    : CALL SP oC_FunctionInvocation (SP? oC_Where)? ( SP? YIELD SP oC_YieldItems )? ;

oC_Match
    : ( OPTIONAL SP )? MATCH SP? oC_Pattern ( SP oC_Where )? ( SP iC_Hint )? ;

iC_Hint
    : HINT SP iC_JoinNode;

iC_JoinNode
    :  iC_JoinNode SP JOIN SP iC_JoinNode
        | iC_JoinNode ( SP MULTI_JOIN SP oC_SchemaName)+
        | '(' SP? iC_JoinNode SP? ')'
        | oC_SchemaName ;

oC_Unwind : UNWIND SP? oC_Expression SP AS SP oC_Variable ;

oC_Create
    : CREATE SP? oC_Pattern ;

// For unknown reason, openCypher use oC_PatternPart instead of oC_Pattern. There should be no difference in terms of planning.
// So we choose to be consistent with oC_Create and use oC_Pattern instead.
oC_Merge : MERGE SP? oC_Pattern ( SP oC_MergeAction )* ;

oC_MergeAction
    :  ( ON SP MATCH SP oC_Set )
        | ( ON SP CREATE SP oC_Set )
        ;

oC_Set
    : SET SP? oC_SetItem ( SP? ',' SP? oC_SetItem )*
        | SET SP? oC_Atom SP? '=' SP? iC_Properties;

oC_SetItem
    : ( oC_PropertyExpression SP? '=' SP? oC_Expression ) ;

oC_Delete
    : ( DETACH SP )? DELETE SP? oC_Expression ( SP? ',' SP? oC_Expression )*;

oC_With
    : WITH oC_ProjectionBody ( SP? oC_Where )? ;

oC_Return
    : RETURN oC_ProjectionBody ;

oC_ProjectionBody
    : ( SP? DISTINCT )? SP oC_ProjectionItems (SP oC_Order )? ( SP oC_Skip )? ( SP oC_Limit )? ;

oC_ProjectionItems
    : ( STAR ( SP? ',' SP? oC_ProjectionItem )* )
        | ( oC_ProjectionItem ( SP? ',' SP? oC_ProjectionItem )* )
        ;

STAR : '*' ;

oC_ProjectionItem
    : ( oC_Expression SP AS SP oC_Variable )
        | oC_Expression
        ;

oC_Order
    : ORDER SP BY SP oC_SortItem ( ',' SP? oC_SortItem )* ;

oC_Skip
    :  L_SKIP SP oC_Expression ;

L_SKIP : ( 'S' | 's' ) ( 'K' | 'k' ) ( 'I' | 'i' ) ( 'P' | 'p' ) ;

oC_Limit
    : LIMIT SP oC_Expression ;

oC_SortItem
    : oC_Expression ( SP? ( ASCENDING | ASC | DESCENDING | DESC ) )? ;

oC_Where
    : WHERE SP oC_Expression ;

oC_Pattern
    : oC_PatternPart ( SP? ',' SP? oC_PatternPart )* ;

oC_PatternPart
    :  ( oC_Variable SP? '=' SP? oC_AnonymousPatternPart )
        | oC_AnonymousPatternPart ;

oC_AnonymousPatternPart
    : oC_PatternElement ;

oC_PatternElement
    : ( oC_NodePattern ( SP? oC_PatternElementChain )* )
        | ( '(' oC_PatternElement ')' )
        ;

oC_NodePattern
    : '(' SP? ( oC_Variable SP? )? ( oC_NodeLabels SP? )? ( iC_Properties SP? )? ')' ;

oC_PatternElementChain
    : oC_RelationshipPattern SP? oC_NodePattern ;

oC_RelationshipPattern
    : ( oC_LeftArrowHead SP? oC_Dash SP? oC_RelationshipDetail? SP? oC_Dash )
        | ( oC_Dash SP? oC_RelationshipDetail? SP? oC_Dash SP? oC_RightArrowHead )
        | ( oC_Dash SP? oC_RelationshipDetail? SP? oC_Dash )
        ;

oC_RelationshipDetail
    : '[' SP? ( oC_Variable SP? )? ( oC_RelationshipTypes SP? )? ( iC_RecursiveDetail SP? )? ( iC_Properties SP? )? ']' ;

// The original oC_Properties definition is  oC_MapLiteral | oC_Parameter.
// We choose to not support parameter as properties which will be the decision for a long time.
// We then substitute with oC_MapLiteral definition. We create oC_MapLiteral only when we decide to add MAP type.
iC_Properties
    : '{' SP? ( oC_PropertyKeyName SP? ':' SP? oC_Expression SP? ( ',' SP? oC_PropertyKeyName SP? ':' SP? oC_Expression SP? )* )? '}';

oC_RelationshipTypes
    :  ':' SP? oC_RelTypeName ( SP? '|' ':'? SP? oC_RelTypeName )* ;

oC_NodeLabels
    :  ':' SP? oC_LabelName ( SP? ('|' ':'? | ':') SP? oC_LabelName )* ;

iC_RecursiveDetail
    : '*' ( SP? iC_RecursiveType)? ( SP? oC_RangeLiteral )? ( SP? iC_RecursiveComprehension )? ;

iC_RecursiveType
    : (ALL SP)? WSHORTEST SP? '(' SP? oC_PropertyKeyName SP? ')'
        | SHORTEST
        | ALL SP SHORTEST
        | TRAIL
        | ACYCLIC ;

oC_RangeLiteral
    :  oC_LowerBound? SP? DOTDOT SP? oC_UpperBound?
        | oC_IntegerLiteral ;

iC_RecursiveComprehension
    : '(' SP? oC_Variable SP? ',' SP? oC_Variable ( SP? '|' SP? oC_Where SP? )? ( SP? '|' SP? iC_RecursiveProjectionItems SP? ',' SP? iC_RecursiveProjectionItems SP? )? ')' ;

iC_RecursiveProjectionItems
    : '{' SP? oC_ProjectionItems? SP? '}' ;

oC_LowerBound
    : DecimalInteger ;

oC_UpperBound
    : DecimalInteger ;

oC_LabelName
    : oC_SchemaName ( '.' oC_SchemaName )? ;

oC_RelTypeName
    : oC_SchemaName ;

oC_Expression
    : oC_OrExpression ;

oC_OrExpression
    : oC_XorExpression ( SP OR SP oC_XorExpression )* ;

oC_XorExpression
    : oC_AndExpression ( SP XOR SP oC_AndExpression )* ;

oC_AndExpression
    : oC_NotExpression ( SP AND SP oC_NotExpression )* ;

oC_NotExpression
    : ( NOT SP? )*  oC_ComparisonExpression;

oC_ComparisonExpression
    : iC_BitwiseOrOperatorExpression ( SP? iC_ComparisonOperator SP? iC_BitwiseOrOperatorExpression )?
        | iC_BitwiseOrOperatorExpression ( SP? INVALID_NOT_EQUAL SP? iC_BitwiseOrOperatorExpression ) { notifyInvalidNotEqualOperator($INVALID_NOT_EQUAL); }
        | iC_BitwiseOrOperatorExpression SP? iC_ComparisonOperator SP? iC_BitwiseOrOperatorExpression ( SP? iC_ComparisonOperator SP? iC_BitwiseOrOperatorExpression )+ { notifyNonBinaryComparison($ctx->start); }
        ;

iC_ComparisonOperator : '=' | '<>' | '<' | '<=' | '>' | '>=' ;

INVALID_NOT_EQUAL : '!=' ;

iC_BitwiseOrOperatorExpression
    : iC_BitwiseAndOperatorExpression ( SP? '|' SP? iC_BitwiseAndOperatorExpression )* ;

iC_BitwiseAndOperatorExpression
    : iC_BitShiftOperatorExpression ( SP? '&' SP? iC_BitShiftOperatorExpression )* ;

iC_BitShiftOperatorExpression
    : oC_AddOrSubtractExpression ( SP? iC_BitShiftOperator SP? oC_AddOrSubtractExpression )* ;

iC_BitShiftOperator : '>>' | '<<' ;

oC_AddOrSubtractExpression
    : oC_MultiplyDivideModuloExpression ( SP? iC_AddOrSubtractOperator SP? oC_MultiplyDivideModuloExpression )* ;

iC_AddOrSubtractOperator : '+' | '-' ;

oC_MultiplyDivideModuloExpression
    : oC_PowerOfExpression ( SP? iC_MultiplyDivideModuloOperator SP? oC_PowerOfExpression )* ;

iC_MultiplyDivideModuloOperator : '*' | '/' | '%' ;

oC_PowerOfExpression
    : oC_StringListNullOperatorExpression ( SP? '^' SP? oC_StringListNullOperatorExpression )* ;

oC_StringListNullOperatorExpression
    : oC_UnaryAddSubtractOrFactorialExpression ( oC_StringOperatorExpression | oC_ListOperatorExpression+ | oC_NullOperatorExpression )? ;

oC_ListOperatorExpression
    : ( SP IN SP? oC_PropertyOrLabelsExpression )
        | ( '[' oC_Expression ']' )
        | ( '[' oC_Expression? ( COLON | DOTDOT ) oC_Expression? ']' ) ;

COLON : ':' ;

DOTDOT : '..' ;

oC_StringOperatorExpression
    :  ( oC_RegularExpression | ( SP STARTS SP WITH ) | ( SP ENDS SP WITH ) | ( SP CONTAINS ) ) SP? oC_PropertyOrLabelsExpression ;

oC_RegularExpression
    :  SP? '=~' ;

oC_NullOperatorExpression
    : ( SP IS SP NULL )
        | ( SP IS SP NOT SP NULL ) ;

MINUS : '-' ;

FACTORIAL : '!' ;

oC_UnaryAddSubtractOrFactorialExpression
    : ( MINUS SP? )* oC_PropertyOrLabelsExpression (SP? FACTORIAL)? ;

oC_PropertyOrLabelsExpression
    : oC_Atom ( SP? oC_PropertyLookup )* ;

oC_Atom
    : oC_Literal
        | oC_Parameter
        | oC_CaseExpression
        | oC_ParenthesizedExpression
        | oC_FunctionInvocation
        | oC_PathPatterns
        | oC_ExistCountSubquery
        | oC_Variable
        | oC_Quantifier
        ;

oC_Quantifier
    :  ( ALL SP? '(' SP? oC_FilterExpression SP? ')' )
        | ( ANY SP? '(' SP? oC_FilterExpression SP? ')' )
        | ( NONE SP? '(' SP? oC_FilterExpression SP? ')' )
        | ( SINGLE SP? '(' SP? oC_FilterExpression SP? ')' )
        ;

oC_FilterExpression
    :  oC_IdInColl SP oC_Where ;

oC_IdInColl
    :  oC_Variable SP IN SP oC_Expression ;

oC_Literal
    : oC_NumberLiteral
        | StringLiteral
        | oC_BooleanLiteral
        | NULL
        | oC_ListLiteral
        | iC_StructLiteral
        ;

oC_BooleanLiteral
    : TRUE
        | FALSE
        ;

oC_ListLiteral
    :  '[' SP? ( oC_Expression SP? ( iC_ListEntry SP? )* )? ']' ;

iC_ListEntry
    : ',' SP? oC_Expression? ;

iC_StructLiteral
    :  '{' SP? iC_StructField SP? ( ',' SP? iC_StructField SP? )* '}' ;

iC_StructField
    :   ( oC_SymbolicName | StringLiteral ) SP? ':' SP? oC_Expression ;

oC_ParenthesizedExpression
    : '(' SP? oC_Expression SP? ')' ;

oC_FunctionInvocation
    : COUNT SP? '(' SP? '*' SP? ')'
        | CAST SP? '(' SP? iC_FunctionParameter SP? ( ( AS SP? iC_DataType ) | ( ',' SP? iC_FunctionParameter ) ) SP? ')'
        | oC_FunctionName SP? '(' SP? ( DISTINCT SP? )? ( iC_FunctionParameter SP? ( ',' SP? iC_FunctionParameter SP? )* )? ')' ;

oC_FunctionName
    : oC_SymbolicName ;

iC_FunctionParameter
    : ( oC_SymbolicName SP? ':' '=' SP? )? oC_Expression
        | iC_LambdaParameter ;

iC_LambdaParameter
    : iC_LambdaVars SP? '-' '>' SP? oC_Expression SP? ;

iC_LambdaVars
    : oC_SymbolicName
    | '(' SP? oC_SymbolicName SP? ( ',' SP? oC_SymbolicName SP?)* ')' ;

oC_PathPatterns
    : oC_NodePattern ( SP? oC_PatternElementChain )+;

oC_ExistCountSubquery
    : (EXISTS | COUNT) SP? '{' SP? MATCH SP? oC_Pattern ( SP? oC_Where )? ( SP? iC_Hint )? SP? '}' ;

oC_PropertyLookup
    : '.' SP? ( oC_PropertyKeyName | STAR ) ;

oC_CaseExpression
    :  ( ( CASE ( SP? oC_CaseAlternative )+ ) | ( CASE SP? oC_Expression ( SP? oC_CaseAlternative )+ ) ) ( SP? ELSE SP? oC_Expression )? SP? END ;

oC_CaseAlternative
    :  WHEN SP? oC_Expression SP? THEN SP? oC_Expression ;

oC_Variable
    : oC_SymbolicName ;

StringLiteral
    : ( '"' ( StringLiteral_0 | EscapedChar )* '"' )
        | ( '\'' ( StringLiteral_1 | EscapedChar )* '\'' )
        ;

EscapedChar
    : '\\' ( '\\' | '\'' | '"' | ( 'B' | 'b' ) | ( 'F' | 'f' ) | ( 'N' | 'n' ) | ( 'R' | 'r' ) | ( 'T' | 't' ) | ( ( 'X' | 'x' ) ( HexDigit HexDigit ) ) | ( ( 'U' | 'u' ) ( HexDigit HexDigit HexDigit HexDigit ) ) | ( ( 'U' | 'u' ) ( HexDigit HexDigit HexDigit HexDigit HexDigit HexDigit HexDigit HexDigit ) ) ) ;

oC_NumberLiteral
    : oC_DoubleLiteral
        | oC_IntegerLiteral
        ;

oC_Parameter
    : '$' ( oC_SymbolicName | DecimalInteger ) ;

oC_PropertyExpression
    : oC_Atom SP? oC_PropertyLookup ;

oC_PropertyKeyName
    : oC_SymbolicName ;

oC_IntegerLiteral
    : DecimalInteger ;

DecimalInteger
    : ZeroDigit
        | ( NonZeroDigit ( Digit )* )
        ;

HexLetter
    : ( 'A' | 'a' )
        | ( 'B' | 'b' )
        | ( 'C' | 'c' )
        | ( 'D' | 'd' )
        | ( 'E' | 'e' )
        | ( 'F' | 'f' )
        ;

HexDigit
    : Digit
        | HexLetter
        ;

Digit
    : ZeroDigit
        | NonZeroDigit
        ;

NonZeroDigit
    : NonZeroOctDigit
        | '8'
        | '9'
        ;

NonZeroOctDigit
    : '1'
        | '2'
        | '3'
        | '4'
        | '5'
        | '6'
        | '7'
        ;

ZeroDigit
    : '0' ;

oC_DoubleLiteral
    : ExponentDecimalReal
        | RegularDecimalReal
        ;

ExponentDecimalReal
    : ( ( Digit )+ | ( ( Digit )+ '.' ( Digit )+ ) | ( '.' ( Digit )+ ) ) ( 'E' | 'e' ) '-'? ( Digit )+ ;

RegularDecimalReal
    : ( Digit )* '.' ( Digit )+ ;

oC_SchemaName
    : oC_SymbolicName ( '.' oC_SymbolicName )? ;

oC_SymbolicName
    : UnescapedSymbolicName
        | EscapedSymbolicName {if ($EscapedSymbolicName.text == "``") { notifyEmptyToken($EscapedSymbolicName); }}
        | HexLetter
        | iC_NonReservedKeywords
        ;

// example of BEGIN and END: TCKWith2.Scenario1
iC_NonReservedKeywords
    : COMMENT
        | ADD
        | ALTER
        | AS
        | ATTACH
        | BEGIN
        | BY
        | CALL
        | CHECKPOINT
        | COMMENT
        | COMMIT
        | CONTAINS
        | COPY
        | COUNT
        | CYCLE
        | DATABASE
        | DECIMAL
        | DELETE
        | DETACH
        | DROP
        | EXPLAIN
        | EXPORT
        | EXTENSION
        | FORCE
        | GRAPH
        | IF
        | IS
        | IMPORT
        | INCREMENT
        | KEY
        | LOAD
        | LOGICAL
        | MATCH
        | MAXVALUE
        | MERGE
        | MINVALUE
        | NO
        | NODE
        | PROJECT
        | READ
        | REL
        | RENAME
        | RETURN
        | ROLLBACK
        | ROLE
        | SEQUENCE
        | SET
        | START
        | STRUCT
        | L_SKIP
        | LIMIT
        | TRANSACTION
        | TYPE
        | USE
        | UNINSTALL
        | UPDATE
        | WRITE
        | FROM
        | TO
        | YIELD
        | USER
        | PASSWORD
        | MAP
        ;

UnescapedSymbolicName
    : IdentifierStart ( IdentifierPart )* ;

IdentifierStart
    : ID_Start
        | Pc
        ;

IdentifierPart
    : ID_Continue
        | Sc
        ;

EscapedSymbolicName
    : ( '`' ( EscapedSymbolicName_0 )* '`' )+ ;

SP
  : ( WHITESPACE )+ ;

WHITESPACE
    : SPACE
        | TAB
        | LF
        | VT
        | FF
        | CR
        | FS
        | GS
        | RS
        | US
        | '\u1680'
        | '\u180e'
        | '\u2000'
        | '\u2001'
        | '\u2002'
        | '\u2003'
        | '\u2004'
        | '\u2005'
        | '\u2006'
        | '\u2008'
        | '\u2009'
        | '\u200a'
        | '\u2028'
        | '\u2029'
        | '\u205f'
        | '\u3000'
        | '\u00a0'
        | '\u2007'
        | '\u202f'
        | CypherComment
        ;

CypherComment
    : ( '/*' ( Comment_1 | ( '*' Comment_2 ) )* '*/' )
        | ( '//' ( Comment_3 )* CR? ( LF | EOF ) )
        ;

oC_LeftArrowHead
    : '<'
        | '\u27e8'
        | '\u3008'
        | '\ufe64'
        | '\uff1c'
        ;

oC_RightArrowHead
    : '>'
        | '\u27e9'
        | '\u3009'
        | '\ufe65'
        | '\uff1e'
        ;

oC_Dash
    : '-'
        | '\u00ad'
        | '\u2010'
        | '\u2011'
        | '\u2012'
        | '\u2013'
        | '\u2014'
        | '\u2015'
        | '\u2212'
        | '\ufe58'
        | '\ufe63'
        | '\uff0d'
        ;

fragment FF : [\f] ;

fragment EscapedSymbolicName_0 : ~[`] ;

fragment RS : [\u001E] ;

fragment ID_Continue : [\p{ID_Continue}] ;

fragment Comment_1 : ~[*] ;

fragment StringLiteral_1 : ~['\\] ;

fragment Comment_3 : ~[\n\r] ;

fragment Comment_2 : ~[/] ;

fragment GS : [\u001D] ;

fragment FS : [\u001C] ;

fragment CR : [\r] ;

fragment Sc : [\p{Sc}] ;

fragment SPACE : [ ] ;

fragment Pc : [\p{Pc}] ;

fragment TAB : [\t] ;

fragment StringLiteral_0 : ~["\\] ;

fragment LF : [\n] ;

fragment VT : [\u000B] ;

fragment US : [\u001F] ;

fragment ID_Start : [\p{ID_Start}] ;

// This is used to capture unknown lexer input (e.g. !) to avoid parser exception.
Unknown : .;
