# WAL TypeSpec Codegen

WAL record declarations live in `records/*.tsp` and are generated with:

```bash
python3 scripts/generate_wal_typespec.py
```

Normal CMake builds also depend on the generated WAL files through the
`generate_wal_typespec` target, so editing an existing `.tsp`, template, or the generator
will regenerate the checked-in `.h`/`.cpp` files before the WAL sources compile.

For review without touching checked-in C++ files:

```bash
python3 scripts/generate_wal_typespec.py --output-root /private/tmp/lbug-wal-gen
```

The generator is a thin wrapper around `uvx tsc-py`. It resolves local TypeSpec imports
such as `import "../common.tsp";`, treats non-`.tsp` imports as C++ includes for the
generated header, flattens the record TypeSpec, and invokes `uvx tsc-py` with the WAL
Jinja templates in `templates/`. Generated C++ is formatted with `clang-format-18` when
available; set `CLANG_FORMAT=/path/to/clang-format` to override the formatter.

Record metadata comments preserve existing WAL wire compatibility:

- `// @defaults=field:value` sets generated default constructor values.
- `// @wire_names=field:wire_key` keeps serialized debug keys stable.
- `// @owned_names=field:member_name` keeps replay-visible owned payload names stable.
- `// @debug_fields=false` keeps records that historically serialized fields without
  debug keys on that same wire format.

`ValueVector` and `ValueVector[]` are Ladybug-specific generator types:

- `ValueVector` emits a raw `common::ValueVector*` for write-side records plus an owned
  `std::unique_ptr<common::ValueVector>` for replay-side records.
- `ValueVector[]` emits the current WAL insertion shape: a raw pointer vector for writes
  and `std::vector<std::unique_ptr<common::ValueVector>>` after deserialization.

Records with polymorphic or variant payload serialization, such as catalog entries and
alter-table payloads, still have TypeSpec declarations. Their custom serialization bodies
live in the templates so the checked-in record headers and serialization sources are
generated consistently. These payloads are modeled as TypeSpec `@discriminated` unions
and parsed by `tsc-py>=0.2.0`, while the C++ templates still own the Ladybug-specific
pointer ownership and factory calls.
