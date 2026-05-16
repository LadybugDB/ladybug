# ART Index

Ladybug supports a built-in adaptive radix tree index for node primary keys:

```cypher
CREATE ART INDEX person_pk FOR (p:Person) ON (p.ID);
```

This is implemented by `storage::ArtPrimaryKeyIndex`, registered as the built-in
`ART` index type. It is parallel to the existing `HASH` primary-key index in the
DDL and table lookup paths, but it uses a radix tree keyed by encoded primary-key
bytes instead of a hash table.

## Scope

The ART index currently supports Ladybug's primary-key index workflow:

- building the index from an existing node table during `CREATE ART INDEX`
- checking uniqueness for committed inserts
- primary-key lookup through `NodeTable::lookupPK`
- rollback cleanup for failed insert batches
- checkpoint and reload

It does not currently provide secondary-index scans, range scans, prefix
compression, duplicate row-id leaves, or DuckDB's parallel local-ART merge
builder.

## Key Encoding

`ArtKey::encode` converts a primary-key value into a byte vector. Null keys are
not valid primary keys and are rejected during insert.

Strings are encoded using the same escaping idea as DuckDB's ART keys:

- bytes `0x00` and `0x01` are prefixed with `0x01`
- a trailing `0x00` terminator is appended

Fixed-width numeric values are encoded as their in-memory bytes. This is enough
for the current equality-only primary-key lookup path. If ART is later extended
to ordered scans, numeric encodings will need to become order-preserving.

## Tree Shape

Each tree node may store an optional node-table offset for a complete key and a
set of byte-labeled children. Children use adaptive layouts:

- `Node4`: up to 4 children
- `Node16`: up to 16 children
- `Node48`: up to 48 children, with a 256-byte indirection table
- `Node256`: direct child pointer array for all byte values

Nodes grow as children are inserted. Lookups walk one encoded key byte at a time
from the root to a terminal node and then verify visibility before returning the
stored offset.

## Inserts And Uniqueness

`ArtPrimaryKeyIndex::commitInsert` is called through the generic index commit
path. For each row, it encodes the primary-key value and inserts the key into
the radix tree with the row offset from the node ID vector.

If the terminal node already has a visible offset, insertion fails with the same
duplicate-primary-key error used by the hash index. If the existing offset is not
visible to the caller, the stored offset may be replaced.

## Lookup Path

`NodeTable::lookupPK` now asks the loaded primary-key index to perform a generic
`lookupPrimaryKey` operation. The hash index implements this by delegating to
its existing lookup code. The ART index encodes the lookup key, walks the tree,
and returns the offset only if the supplied visibility function accepts it.

This keeps the planner and table lookup path independent of whether the physical
primary-key index is `HASH` or `ART`.

## Checkpoint And Reload

During checkpoint, the ART index collects all terminal key/offset pairs from the
tree and stores them in `ArtPrimaryKeyIndexStorageInfo`. The storage info is
serialized into the existing index catalog storage-info buffer.

On reload, the ART index deserializes the key/offset pairs and rebuilds the
in-memory tree. Built-in `ART` catalog entries are marked as loaded in the same
way as built-in `HASH` entries so inserts can continue after reload.
