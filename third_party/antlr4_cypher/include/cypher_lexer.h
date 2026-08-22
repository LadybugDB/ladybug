
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
    HASH = 95, HEADERS = 96, HINT = 97, IMPORT = 98, INDEX = 99, IF = 100, 
    IN = 101, INCREMENT = 102, INSTALL = 103, IS = 104, JOIN = 105, KEY = 106, 
    LIMIT = 107, LIST = 108, LOAD = 109, LOGICAL = 110, MACRO = 111, MATCH = 112, 
    MAXVALUE = 113, MERGE = 114, MINVALUE = 115, MULTI_JOIN = 116, NO = 117, 
    NODE = 118, NOT = 119, NONE = 120, NULL_ = 121, ON = 122, ONLY = 123, 
    OPTIONS = 124, OPTIONAL = 125, OR = 126, ORDER = 127, PRIMARY = 128, 
    PROFILE = 129, PROJECT = 130, RANGE = 131, READ = 132, REL = 133, RENAME = 134, 
    RETURN = 135, ROLLBACK = 136, ROLLBACK_SKIP_CHECKPOINT = 137, SEQUENCE = 138, 
    SET = 139, SORTED = 140, SHORTEST = 141, START = 142, STARTS = 143, 
    STRUCT = 144, TABLE = 145, THEN = 146, TO = 147, TRAIL = 148, TRANSACTION = 149, 
    TRUE = 150, TYPE = 151, UNION = 152, UNWIND = 153, UNINSTALL = 154, 
    UPDATE = 155, USE = 156, WHEN = 157, WHERE = 158, WITH = 159, WRITE = 160, 
    WSHORTEST = 161, XOR = 162, SINGLE = 163, YIELD = 164, USER = 165, PARTITION = 166, 
    PARTITIONS = 167, PASSWORD = 168, ROLE = 169, MAP = 170, DECIMAL = 171, 
    STAR = 172, L_SKIP = 173, INVALID_NOT_EQUAL = 174, COLON = 175, DOTDOT = 176, 
    MINUS = 177, FACTORIAL = 178, StringLiteral = 179, EscapedChar = 180, 
    DecimalInteger = 181, HexLetter = 182, HexDigit = 183, Digit = 184, 
    NonZeroDigit = 185, NonZeroOctDigit = 186, ZeroDigit = 187, ExponentDecimalReal = 188, 
    RegularDecimalReal = 189, UnescapedSymbolicName = 190, IdentifierStart = 191, 
    IdentifierPart = 192, EscapedSymbolicName = 193, SP = 194, WHITESPACE = 195, 
    CypherComment = 196, Unknown = 197
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

