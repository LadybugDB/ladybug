
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
    COPY = 67, COUNT = 68, CREATE = 69, CYCLE = 70, DATABASE = 71, DBTYPE = 72, 
    DEFAULT = 73, DELETE = 74, DESC = 75, DESCENDING = 76, DETACH = 77, 
    DISTINCT = 78, DROP = 79, ELSE = 80, END = 81, ENDS = 82, EXISTS = 83, 
    EXPLAIN = 84, EXPORT = 85, EXTENSION = 86, FALSE = 87, FROM = 88, FORCE = 89, 
    FOR = 90, GLOB = 91, GRAPH = 92, GROUP = 93, HEADERS = 94, HINT = 95, 
    IMPORT = 96, INDEX = 97, IF = 98, IN = 99, INCREMENT = 100, INSTALL = 101, 
    IS = 102, JOIN = 103, KEY = 104, LIMIT = 105, LOAD = 106, LOGICAL = 107, 
    MACRO = 108, MATCH = 109, MAXVALUE = 110, MERGE = 111, MINVALUE = 112, 
    MULTI_JOIN = 113, NO = 114, NODE = 115, NOT = 116, NONE = 117, NULL_ = 118, 
    ON = 119, ONLY = 120, OPTIONS = 121, OPTIONAL = 122, OR = 123, ORDER = 124, 
    PRIMARY = 125, PROFILE = 126, PROJECT = 127, READ = 128, REL = 129, 
    RENAME = 130, RETURN = 131, ROLLBACK = 132, ROLLBACK_SKIP_CHECKPOINT = 133, 
    SEQUENCE = 134, SET = 135, SORTED = 136, SHORTEST = 137, START = 138, 
    STARTS = 139, STRUCT = 140, TABLE = 141, THEN = 142, TO = 143, TRAIL = 144, 
    TRANSACTION = 145, TRUE = 146, TYPE = 147, UNION = 148, UNWIND = 149, 
    UNINSTALL = 150, UPDATE = 151, USE = 152, WHEN = 153, WHERE = 154, WITH = 155, 
    WRITE = 156, WSHORTEST = 157, XOR = 158, SINGLE = 159, YIELD = 160, 
    USER = 161, PASSWORD = 162, ROLE = 163, MAP = 164, DECIMAL = 165, STAR = 166, 
    L_SKIP = 167, INVALID_NOT_EQUAL = 168, COLON = 169, DOTDOT = 170, MINUS = 171, 
    FACTORIAL = 172, StringLiteral = 173, EscapedChar = 174, DecimalInteger = 175, 
    HexLetter = 176, HexDigit = 177, Digit = 178, NonZeroDigit = 179, NonZeroOctDigit = 180, 
    ZeroDigit = 181, ExponentDecimalReal = 182, RegularDecimalReal = 183, 
    UnescapedSymbolicName = 184, IdentifierStart = 185, IdentifierPart = 186, 
    EscapedSymbolicName = 187, SP = 188, WHITESPACE = 189, CypherComment = 190, 
    Unknown = 191
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

