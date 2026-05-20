# Icebug-Memory Storage Format

## Overview

This is LadybugDB's implementation of [Icebug-Memory](https://github.com/Ladybug-Memory/icebug-format), a read-only graph storage format based on Arrow. It is designed for efficient analytical queries on large graphs.

## V1

Implements Icebug-Memory v1

### Creating tables

Icebug-Memory tables can be created using python/C/C++ APIs. Other languages and CLI are currently not supported

- `create_arrow_table(conn, table_name, arrow_schema, arrow_arrays)` for node tables
- `create_arrow_csr_rel_table(connection, tableName, srcTableName, dstTableName,
    fwdIndicesSchema, fwdIndices,
    fwdIndptrSchema, fwdIndptr,
    optional<bwdIndicesSchema>, optional<bwdIndices>,
    optional<bwdIndptrSchema>, optional<bwdIndptr>)` for CSR relationship tables

### Node tables

For each node table, there is a corresponding arrow table containing a primary key column and one column per property as declared in the schema.

### Indices

Each relationship table has a corresponding fwd arrow table containing one row per edge. The first column is always `target` (the destination node offset), followed by zero or more edge property columns as declared in the schema. Optionally, a bwd arrow table can be supplied for efficient reverse traversals.

### Indptr

Each relationship table has a corresponding fwd arrow table containing the CSR row pointers. It has a single integer column with `N+1` entries, where `N` is the number of source nodes. Optionally, a bwd indptr table can be supplied for efficient reverse traversals.

## Convert from other formats

You can convert from other graph formats (e.g. non-csr arrow tables) to Icebug-Memory using the script at https://github.com/Ladybug-Memory/icebug-format

## Lifetime and mutability

Icebug-Memory tables are immutable. `INSERT`, `UPDATE`, `DELETE`, and `ALTER TABLE` are not supported.

The data lifetime is tied to the in-memory Arrow registration. Dropping the table unregisters the Arrow data, and restarting the process requires registering the data again.
