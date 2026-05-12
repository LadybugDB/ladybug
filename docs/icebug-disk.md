# Icebug-Disk Storage Format

## Overview

This is LadybugDB's implementation of [Icebug-Disk](https://github.com/Ladybug-Memory/icebug-format), a read-only graph storage format based on Parquet files. It is designed for efficient analytical queries on large graphs.

## V1

Implements Icebug-Disk v1.

### Version

Version is stored in each file's metadata footer as a key-value pair: `icebug_disk_version = v1`.

### schema.cypher

The graph schema is declared in a `schema.cypher`, which can be loaded using `lbug -i schema.cypher` to create tables in Ladybug.

```cypher
CREATE NODE TABLE city(id INT32, name STRING, population INT64, PRIMARY KEY(id)) WITH (storage = 'icebug-disk:<path-to-dir>');
CREATE NODE TABLE user(id INT32, name STRING, age INT64, PRIMARY KEY(id)) WITH (storage = 'icebug-disk:<path-to-dir>');
CREATE REL TABLE follows(FROM user TO user, since INT32) WITH (storage = 'icebug-disk:<path-to-dir>');
CREATE REL TABLE livesin(FROM user TO city) WITH (storage = 'icebug-disk:<path-to-dir>');
```

File paths can be relative or absolute and are resolved as `<path-to-dir>/nodes_{tableName}.parquet` for node tables, and `<path-to-dir>/indices_{tableName}.parquet` and `<path-to-dir>/indptr_{tableName}.parquet` for relationship tables.

`storage = 'icebug-disk'` and `storage = 'icebug-disk:'` both resolve to the current working directory.

Tables can also be created by manually running the above queries in the Ladybug CLI.

If the directory is moved, the affected tables must be dropped and re-created with the updated path. This updates the file pointers in the catalog. A fresh db instance can also be used to run the `CREATE TABLE` queries with the new paths.

Mixed tables are not supported — `CREATE REL TABLE` queries involving both `icebug-disk` and non-`icebug-disk` tables will throw a `BinderException`.

Note: Icebug-disk tables are **immutable**: `ALTER TABLE` operations, Data insertions, updates, and deletions are not supported.

### Node tables

For each node table, there is a corresponding Parquet file named `nodes_{tableName}.parquet` containing a primary key column and one column per property as declared in the schema.

### Indices

Each relationship table has a corresponding `indices_{tableName}.parquet` file containing one row per edge. The first column is always `target` (the destination node offset), followed by zero or more edge property columns as declared in the schema.

### Indptr

Each relationship table has a corresponding `indptr_{tableName}.parquet` file containing the CSR row pointers. It has a single integer column with `N+1` entries, where `N` is the number of source nodes.

## Convert from other formats

You can convert from other graph formats (e.g. duckdb, parquet tables) to Icebug-Disk using the script at https://github.com/Ladybug-Memory/icebug-format
