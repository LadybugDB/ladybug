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

-- Node-table UNIQUE
CREATE CONSTRAINT user_email_unique
FOR (u:User)
REQUIRE u.email IS UNIQUE;

SHOW CONSTRAINTS;
CALL show_constraints() RETURN *;
DROP CONSTRAINT review_unique IF EXISTS;
```

## Semantics

- A UNIQUE constraint enforces that the tuple of the listed properties is distinct across
  all rows of one node table or one rel group (all `(FROM, TO)` pairs share the key set,
  so cross-pair duplicates are caught).
- `NULL` is treated as distinct (SQL semantics): a row with `NULL` in any constrained
  property never conflicts, so multiple `NULL`s are allowed.
- Direction arrows in `FOR ()-[r:T]-()` are accepted but ignored; exactly one rel type
  (or one node label) is required.
- Only native (non-partitioned, non-columnar) tables are supported.

## Enforcement points

- Single-row writes (`CREATE`, `MERGE ... ON CREATE`) are checked before the row is
  inserted; violations abort with `Unique constraint <name> violated: duplicate key.`
- `COPY FROM` is validated once at the end of the load; a violation aborts the whole
  transaction (the load is discarded, nothing is partially committed).
- `CREATE CONSTRAINT` validates existing data the same way and refuses if duplicates
  already exist (delete/deduplicate first, then create).
- `SET` on a constrained property is rejected at bind time with a clear error
  (`... Try delete and then insert with the new value.`); per-row revalidation on
  update is not implemented yet.
- `DROP TABLE` drops the table's constraints; `DROP PROPERTY` of a constrained property
  is rejected until the constraint is dropped; `RENAME TABLE/PROPERTY` is reflected in
  `SHOW CONSTRAINTS` and `EXPORT DATABASE`.

## Implementation notes (MVP limitations)

- Enforcement is **scan-based**: each check scans the table (`O(table)` per single-row
  insert, one full scan per `COPY`, one per `CREATE CONSTRAINT`). Bulk single-row
  inserts are slow; prefer `COPY FROM` for loads. A future index-backed implementation
  can replace `storage/UniqueConstraintChecker` without touching the catalog shape.
- Value comparison is exact per physical type. `NaN`, `-0.0`, and decimal-scale
  variants follow canonical string form and may compare as distinct.
- Concurrent writers share the check-then-insert race of the existing PK path; the
  second committer does not re-validate.
- Only `UNIQUE` is enforced. The catalog entry reserves `KEY` / `NOT NULL` / `CHECK`
  variants for later without changing persistence.
