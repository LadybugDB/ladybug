# Snowflake Arrow Handling

This document lists the Snowflake-specific Arrow metadata cases currently handled by Ladybug.

## Supported Cases

| Snowflake signal | Example metadata | Arrow physical storage | Ladybug result | Notes |
| --- | --- | --- | --- | --- |
| Raw Snowflake decimal type via `DATA_TYPE` | `DATA_TYPE=NUMBER(12,4)` | Any Arrow storage type | `DECIMAL(12,4)` | Parsed from Snowflake table-schema metadata. |
| Raw Snowflake decimal type via `DATA_TYPE` with implicit scale | `DATA_TYPE=NUMBER(18)` | Any Arrow storage type | `DECIMAL(18,0)` | Missing scale defaults to `0`. |
| Raw Snowflake decimal aliases via `DATA_TYPE` | `DATA_TYPE=NUMERIC(10,3)` or `DATA_TYPE=DECIMAL(10,3)` | Any Arrow storage type | `DECIMAL(10,3)` | Matching is case-insensitive and whitespace-tolerant. |
| Snowflake logical decimal metadata | `logicalType=FIXED`, `precision=7`, `scale=2` | Integer-backed Arrow (`INT8/16/32/64`, `UINT8/16/32/64`) | `DECIMAL(7,2)` | Used for query-result Arrow schemas. |
| Snowflake logical decimal metadata | `logicalType=FIXED`, `precision=9`, `scale=2` | Float-backed Arrow (`FLOAT`, `DOUBLE`) | `DECIMAL(9,2)` | Values are cast into decimal backing storage during scan. |
| Snowflake raw type fallback to logical metadata | malformed `DATA_TYPE` plus valid `logicalType=FIXED` metadata | Integer-backed or float-backed Arrow | `DECIMAL(p,s)` from `logicalType` metadata | If raw `DATA_TYPE` parsing fails, Snowflake `logicalType` parsing is tried next. |
| Snowflake metadata precedence over generic metadata | `DATA_TYPE=NUMBER(12,4)` plus generic `logicalType=DECIMAL`, `precision=9`, `scale=3` | Any Arrow storage type | `DECIMAL(12,4)` | Snowflake raw type metadata wins over generic metadata. |

## Current Scope

Only Snowflake decimal semantics are handled today.

Specifically:

- `NUMBER(p,s)`
- `NUMBER(p)`
- `NUMERIC(p,s)`
- `DECIMAL(p,s)`
- `logicalType=FIXED`

## Not Yet Handled

The Snowflake ADBC driver documents additional logical types that are not currently interpreted in a Snowflake-specific way here, including:

- `real`
- `date`
- `time`
- `timestamp_ltz`
- `timestamp_ntz`
- `timestamp_tz`
- `text`
- `binary`
- `variant`
- `object`
- `array`
- `boolean`

For those, Ladybug currently relies on the standard Arrow physical type unless future Snowflake-specific decoding is added.
