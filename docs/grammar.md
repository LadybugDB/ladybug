## Changing the grammar

source: src/antlr4/Cypher.g4
generated: scripts/antrl4/Cypher.g4 (via keywordhandler.py)
generated: third_party/antlr4_cypher/cypher_parser.{cpp,h}

Running make will autogenerate, but manual generation can be done via

(cd scripts/antlr4 && cmake -D ROOT_DIR=../.. -P generate_grammar.cmake)
