
// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"




class  CypherLexer : public antlr4::Lexer {
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
    FROM = 87, FORCE = 88, FOR = 89, GLOB = 90, GRAPH = 91, GROUP = 92, 
    HEADERS = 93, HINT = 94, IMPORT = 95, INDEX = 96, IF = 97, IN = 98, 
    INCREMENT = 99, INSTALL = 100, IS = 101, JOIN = 102, KEY = 103, LIMIT = 104, 
    LOAD = 105, LOGICAL = 106, MACRO = 107, MATCH = 108, MAXVALUE = 109, 
    MERGE = 110, MINVALUE = 111, MULTI_JOIN = 112, NO = 113, NODE = 114, 
    NOT = 115, NONE = 116, NULL_ = 117, ON = 118, ONLY = 119, OPTIONS = 120, 
    OPTIONAL = 121, OR = 122, ORDER = 123, PRIMARY = 124, PROFILE = 125, 
    PROJECT = 126, READ = 127, REL = 128, RENAME = 129, RETURN = 130, ROLLBACK = 131, 
    ROLLBACK_SKIP_CHECKPOINT = 132, SEQUENCE = 133, SET = 134, SHORTEST = 135, 
    START = 136, STARTS = 137, STRUCT = 138, TABLE = 139, THEN = 140, TO = 141, 
    TRAIL = 142, TRANSACTION = 143, TRUE = 144, TYPE = 145, UNION = 146, 
    UNWIND = 147, UNINSTALL = 148, UPDATE = 149, USE = 150, WHEN = 151, 
    WHERE = 152, WITH = 153, WRITE = 154, WSHORTEST = 155, XOR = 156, SINGLE = 157, 
    YIELD = 158, USER = 159, PASSWORD = 160, ROLE = 161, MAP = 162, DECIMAL = 163, 
    STAR = 164, L_SKIP = 165, INVALID_NOT_EQUAL = 166, COLON = 167, DOTDOT = 168, 
    MINUS = 169, FACTORIAL = 170, StringLiteral = 171, EscapedChar = 172, 
    DecimalInteger = 173, HexLetter = 174, HexDigit = 175, Digit = 176, 
    NonZeroDigit = 177, NonZeroOctDigit = 178, ZeroDigit = 179, ExponentDecimalReal = 180, 
    RegularDecimalReal = 181, UnescapedSymbolicName = 182, IdentifierStart = 183, 
    IdentifierPart = 184, EscapedSymbolicName = 185, SP = 186, WHITESPACE = 187, 
    CypherComment = 188, Unknown = 189
  };

  explicit CypherLexer(antlr4::CharStream *input);

  ~CypherLexer() override;


  std::string getGrammarFileName() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const std::vector<std::string>& getChannelNames() const override;

  const std::vector<std::string>& getModeNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;

  const antlr4::atn::ATN& getATN() const override;

  // By default the static state used to implement the lexer is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

  // Individual action functions triggered by action() above.

  // Individual semantic predicate functions triggered by sempred() above.

};

