
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
    COMMENT = 63, COMMIT = 64, COMMIT_SKIP_CHECKPOINT = 65, CONSTRAINT = 66, 
    CONSTRAINTS = 67, CONTAINS = 68, COPY = 69, COUNT = 70, CREATE = 71, 
    CSR = 72, CYCLE = 73, DATABASE = 74, DBTYPE = 75, DEFAULT = 76, DELETE = 77, 
    DESC = 78, DESCENDING = 79, DETACH = 80, DISTINCT = 81, DROP = 82, ELSE = 83, 
    END = 84, ENDS = 85, EXISTS = 86, EXPLAIN = 87, EXPORT = 88, EXTENSION = 89, 
    FALSE = 90, FROM = 91, FORCE = 92, FOR = 93, GLOB = 94, GRAPH = 95, 
    GROUP = 96, HASH = 97, HEADERS = 98, HINT = 99, IMPORT = 100, INDEX = 101, 
    IF = 102, IN = 103, INCREMENT = 104, INSTALL = 105, IS = 106, JOIN = 107, 
    KEY = 108, LIMIT = 109, LIST = 110, LOAD = 111, LOGICAL = 112, MACRO = 113, 
    MATCH = 114, MAXVALUE = 115, MERGE = 116, MINVALUE = 117, MULTI_JOIN = 118, 
    NO = 119, NODE = 120, NOT = 121, NONE = 122, NULL_ = 123, ON = 124, 
    ONLY = 125, OPTIONS = 126, OPTIONAL = 127, OR = 128, ORDER = 129, PRIMARY = 130, 
    PROFILE = 131, PROJECT = 132, RANGE = 133, READ = 134, REL = 135, RENAME = 136, 
    REQUIRE = 137, RETURN = 138, ROLLBACK = 139, ROLLBACK_SKIP_CHECKPOINT = 140, 
    SEQUENCE = 141, SET = 142, SHOW = 143, SORTED = 144, SHORTEST = 145, 
    START = 146, STARTS = 147, STRUCT = 148, TABLE = 149, THEN = 150, TO = 151, 
    TRAIL = 152, TRANSACTION = 153, TRUE = 154, TYPE = 155, UNION = 156, 
    UNIQUE = 157, UNWIND = 158, UNINSTALL = 159, UPDATE = 160, USE = 161, 
    WHEN = 162, WHERE = 163, WITH = 164, WRITE = 165, WSHORTEST = 166, XOR = 167, 
    SINGLE = 168, YIELD = 169, USER = 170, PARTITION = 171, PARTITIONS = 172, 
    PASSWORD = 173, ROLE = 174, MAP = 175, DECIMAL = 176, STAR = 177, L_SKIP = 178, 
    INVALID_NOT_EQUAL = 179, COLON = 180, DOTDOT = 181, MINUS = 182, FACTORIAL = 183, 
    StringLiteral = 184, EscapedChar = 185, DecimalInteger = 186, HexLetter = 187, 
    HexDigit = 188, Digit = 189, NonZeroDigit = 190, NonZeroOctDigit = 191, 
    ZeroDigit = 192, ExponentDecimalReal = 193, RegularDecimalReal = 194, 
    UnescapedSymbolicName = 195, IdentifierStart = 196, IdentifierPart = 197, 
    EscapedSymbolicName = 198, SP = 199, WHITESPACE = 200, CypherComment = 201, 
    Unknown = 202
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

