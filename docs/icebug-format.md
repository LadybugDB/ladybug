# Icebug Format Implementation

*Note: The `structure` keyword is used here to mean either a file or an in-memory structure.*

## Syntax
We might need to extend `ATTACH` to support attaching an Icebug-formatted graph. This would create node/rel tables in the database and point to the CSR structures.

Alternatively, we can create a new syntax: `ATTACH GRAPH`.

## Metadata File
The metadata file(passed to `ATTACH`) describes the structure of the graph, including node labels, node table structures, and relationship structures, which will be used to create the corresponding tables in the database(no need of `schema.cypher`). It also includes information about the CSR structures and how they map to the node/rel tables in the database.

We will not use the `CREATE NODE TABLE` and `CREATE REL TABLE` syntax to create these tables. Instead, we will directly call the internal APIs to create the tables based on the metadata file. This approach avoids exposing public APIs that users might accidentally use to corrupt the icebug-formatted tables.

We won't need to enforce any naming conventions on file names

## new icebug classes
We cannot directly use the existing Arrow/Parquet node and rel table classes because those are specific to their respective table storage formats (we will copy the relevant code to the new classes, however). In the future, we might use a different storage format (or a mix) for these tables. Therefore, we will create new classes, `icebug_node_table` and `icebug_rel_table`, which implement the `node_table` and `rel_table` classes respectively, to represent the node and relationship tables in the Icebug format. CSR structures will be linked in these classes during initialization, similar to how `parquet_rel_table` currently operates.

## WITH storage = 'arrow' and WITH storage = 'parquet'
We will need to delegate these queries to DuckDB. The current arrow/parquet node and rel table classes will be revamped to support this delegation
