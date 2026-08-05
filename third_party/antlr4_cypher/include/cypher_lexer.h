
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
    ACYCLIC = 45, ANY = 46, ADD = 47, ALL = 48, ALTER = 49, ANALYZE = 50, 
    AND = 51, AS = 52, ASC = 53, ASCENDING = 54, ATTACH = 55, BEGIN = 56, 
    BY = 57, CALL = 58, CASE = 59, CAST = 60, CHECKPOINT = 61, COLUMN = 62, 
    COMMENT = 63, COMMIT = 64, COMMIT_SKIP_CHECKPOINT = 65, CONTAINS = 66, 
    COPY = 67, COUNT = 68, CREATE = 69, CSR = 70, CYCLE = 71, DATABASE = 72, 
    DBTYPE = 73, DEFAULT = 74, DELETE = 75, DESC = 76, DESCENDING = 77, 
    DETACH = 78, DISTINCT = 79, DROP = 80, ELSE = 81, END = 82, ENDS = 83, 
    EXISTS = 84, EXPLAIN = 85, EXPORT = 86, EXTENSION = 87, FALSE = 88, 
    FROM = 89, FORCE = 90, FOR = 91, GLOB = 92, GRAPH = 93, GROUP = 94, 
    HEADERS = 95, HINT = 96, IMPORT = 97, INDEX = 98, IF = 99, IN = 100, 
    INCREMENT = 101, INSTALL = 102, IS = 103, JOIN = 104, KEY = 105, LIMIT = 106, 
    LOAD = 107, LOGICAL = 108, MACRO = 109, MATCH = 110, MAXVALUE = 111, 
    MERGE = 112, MINVALUE = 113, MULTI_JOIN = 114, NO = 115, NODE = 116, 
    NOT = 117, NONE = 118, NULL_ = 119, ON = 120, ONLY = 121, OPTIONS = 122, 
    OPTIONAL = 123, OR = 124, ORDER = 125, PRIMARY = 126, PROFILE = 127, 
    PROJECT = 128, READ = 129, REL = 130, RENAME = 131, RETURN = 132, ROLLBACK = 133, 
    ROLLBACK_SKIP_CHECKPOINT = 134, SEQUENCE = 135, SET = 136, SORTED = 137, 
    SHORTEST = 138, START = 139, STARTS = 140, STRUCT = 141, TABLE = 142, 
    THEN = 143, TO = 144, TRAIL = 145, TRANSACTION = 146, TRUE = 147, TYPE = 148, 
    UNION = 149, UNWIND = 150, UNINSTALL = 151, UPDATE = 152, USE = 153, 
    WHEN = 154, WHERE = 155, WITH = 156, WRITE = 157, WSHORTEST = 158, XOR = 159, 
    SINGLE = 160, YIELD = 161, USER = 162, PASSWORD = 163, ROLE = 164, MAP = 165, 
    DECIMAL = 166, STAR = 167, L_SKIP = 168, INVALID_NOT_EQUAL = 169, COLON = 170, 
    DOTDOT = 171, MINUS = 172, FACTORIAL = 173, StringLiteral = 174, EscapedChar = 175, 
    DecimalInteger = 176, HexLetter = 177, HexDigit = 178, Digit = 179, 
    NonZeroDigit = 180, NonZeroOctDigit = 181, ZeroDigit = 182, ExponentDecimalReal = 183, 
    RegularDecimalReal = 184, UnescapedSymbolicName = 185, IdentifierStart = 186, 
    IdentifierPart = 187, EscapedSymbolicName = 188, SP = 189, WHITESPACE = 190, 
    CypherComment = 191, Unknown = 192
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

