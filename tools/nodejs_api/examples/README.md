# Examples

Run from `tools/nodejs_api` (after `make nodejs` or `npm run build`):

```bash
node examples/quickstart.mjs
node examples/stream-load.mjs
```

- **quickstart.mjs** — In-memory DB, create table, load data from a stream via `COPY FROM (LOAD FROM ...)`, then query.
- **stream-load.mjs** — Register an async iterable and consume it with `LOAD FROM name RETURN *`.
