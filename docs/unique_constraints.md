# UNIQUE constraints on node and rel tables

## Syntax

```cypher
-- Composite UNIQUE on rel properties (all pairs of the rel group share one key set)
CREATE CONSTRAINT review_unique IF NOT EXISTS
FOR ()-[r:REVIEWED]-()
REQUIRE (r.userId, r.productId) IS UNIQUE;

-- Single-property UNIQUE (parens optional)
CREATE CONSTRAINT follow_key
FOR ()-[r:FOLLOWS]->()
REQUIRE r.since IS UNIQUE;

-- Edge endpoints: the original (src, colA, dst) ask. Direction arrows are accepted
-- but ignored; exactly one rel type (or one node label) is required.
CREATE CONSTRAINT review_edge_unique
FOR ()-[r:REVIEWED]-()
REQUIRE (r.src, r.userId, r.dst) IS UNIQUE;

-- Node-table UNIQUE
CREATE CONSTRAINT user_email_unique
FOR (u:User)
REQUIRE u.email IS UNIQUE;

SHOW CONSTRAINTS;                   -- also: CALL show_constraints() RETURN *;
DROP CONSTRAINT review_unique IF EXISTS;
```

`src`/`dst` in `REQUIRE` refer to the edge's endpoints — unless the rel table defines a
real property with that name, in which case the property wins (backwards compatible).
Endpoint parts work in any position and mix freely with property parts.

## Semantics

- A UNIQUE constraint enforces that its key parts are jointly distinct across all rows
  of one node table or one rel group (all `(FROM, TO)` pairs share one key set, so
  cross-pair duplicates are caught).
- `NULL` is treated as distinct (SQL standard behavior for unique constraints): any row
  with NULL in a constrained part never conflicts, so multiple `NULL`s are allowed.
- Keys are exact per physical type (raw bytes with length prefixes, `-0.0` normalized
  to `+0.0`); no lossy string form is used, so `0.1` vs `0.1000001`, `['a,b']` vs
  `['a', 'b']`, and struct field-boundary collisions can never false-positive.
- Only native (non-partitioned, non-columnar) tables are supported; foreign and
  partitioned tables are rejected at bind time with a clear error. Rel tables with
  BWD-only storage are rejected too (the engine cannot address that layout through
  the directional table data the scans use).

## Enforcement points

- Single-row writes (`CREATE`, `MERGE ... ON CREATE`) are checked before the row is
  inserted; violations abort with `Unique constraint <name> violated: duplicate key.`
- `COPY FROM` is validated once at the end of the load; a violation aborts the whole
  transaction (the load is discarded, nothing is partially committed).
- `CREATE CONSTRAINT` validates existing data the same way and refuses if duplicates
  already exist (delete/deduplicate first, then create).
- `SET` on a constrained property is rejected at bind time with a clear error
  (`... Try delete and then insert with the new value.`); per-row revalidation on
  update is not implemented yet. `SET r.src` / `SET r.dst` fail as unknown properties.
- `DROP TABLE` drops the table's constraints; `DROP PROPERTY` of a constrained property
  is rejected until the constraint is dropped; `RENAME TABLE/PROPERTY` is reflected in
  `SHOW CONSTRAINTS` and `EXPORT DATABASE` (endpoint references are unaffected).
  `ALTER TABLE ... ADD src/dst` and `RENAME ... TO src/dst` are rejected while an
  endpoint (`r.src`/`r.dst`) constraint exists on the table, so the exported spelling
  can never silently switch from the endpoint to a property.

## On-disk format

Constraints persist in their own catalog set with checkpoint and WAL replay support
(storage version 52; files written by older versions open with an empty set).
`EXPORT DATABASE` re-emits them after tables/indexes; `IMPORT DATABASE` replays them.

## Implementation notes (MVP limitations)

- Enforcement is **scan-based**: each check scans the table (one full scan per
  single-row insert, one per `COPY`, one per `CREATE CONSTRAINT`), reading only
  transaction-visible rows. Rel scans bind up to one vector (2048) of source nodes per
  batch and stop at the first duplicate for point checks. Bulk single-row inserts are
  slow; prefer `COPY FROM` for loads. A future index-backed implementation can replace
  `storage/UniqueConstraintChecker` without touching the catalog entry shape.
- Validation builds one in-memory key per row (`COPY`, `CREATE CONSTRAINT` success path)
  in an `unordered_set` that the buffer manager does not track; very large constrained
  tables can use significant heap during validation. Point checks (`CREATE`/`MERGE`)
  stream and stop at the first match instead.
- NaN handling: identical NaN bit patterns conflict; NaN never equals anything else is
  not distinguished (documented divergence from `Value::operator==`, which is false
  for NaN).
- Concurrent writers share the check-then-insert race of the existing PK path; the
  second committer does not re-validate.
- Only `UNIQUE` is enforced. The catalog entry reserves `KEY` / `NOT NULL` / `CHECK`
  variants for later without changing persistence.
