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
- bounded primary-key range scans for constant inequality predicates
- rollback cleanup for failed insert batches
- checkpoint and reload

It does not currently provide secondary indexes, prefix compression, duplicate
row-id leaves, multi-key indexes, partial indexes, expression indexes, or
DuckDB's parallel local-ART merge builder. `CREATE ART INDEX` is intentionally
limited to one native node-table primary-key property. Unsupported index shapes
are rejected during index creation instead of being accepted and failing later
in lookup or insert paths.

Range scans are used only for single-table constant predicates over an
ART-backed primary key. The optimizer uses the range path only after validating
that the query has at most one constant lower-bound predicate and at most one
constant upper-bound predicate. Multiple predicates for the same bound,
non-constant bounds, and complex predicates remain regular filters.

## Key Encoding

`ArtKey::encode` converts a primary-key value into a byte vector. Null keys are
not valid primary keys and are rejected during insert.

Strings are encoded using the same escaping idea as DuckDB's ART keys:

- bytes `0x00` and `0x01` are prefixed with `0x01`
- a trailing `0x00` terminator is appended

Fixed-width numeric values are encoded in order-preserving byte order:

- unsigned integers use big-endian bytes
- signed integers flip the sign bit and then use big-endian bytes
- floating-point values transform IEEE bytes so negative values sort before
  positive values, then use big-endian bytes
- `INT128` and `UINT128` use high-word then low-word big-endian encoding, with
  the signed high-word sign bit flipped for `INT128`

This makes lexicographic key order match value order for supported scalar
primary-key types.

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

Range scans traverse children in byte order and collect terminal offsets whose
encoded keys fall between the requested lower and upper bounds. The scan returns
candidate row offsets in key order, but query output order is not guaranteed;
queries still need `ORDER BY` for ordered results.

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

For constant primary-key inequalities such as `p.ID > 10` or
`p.ID >= 10 AND p.ID < 20`, the filter pushdown optimizer can select the ART
range path when the table has an ART primary-key index. Hash indexes remain
equality-only.

## Checkpoint And Reload

During checkpoint, the ART index collects all terminal key/offset pairs from the
tree and stores them in `ArtPrimaryKeyIndexStorageInfo`. The storage info is
serialized into the existing index catalog storage-info buffer.

On reload, the ART index deserializes the key/offset pairs and rebuilds the
in-memory tree. Built-in `ART` catalog entries are marked as loaded in the same
way as built-in `HASH` entries so inserts can continue after reload.

The persisted ART is not currently page-backed or demand loaded. Opening a
database with an ART index materializes the full in-memory tree by replaying all
stored key/offset entries. Memory use is therefore O(number of indexed keys),
not O(number of queried index pages or blocks). A production disk-backed ART
would need buffer-managed index pages and lazy traversal instead of loading the
entire index at startup.
