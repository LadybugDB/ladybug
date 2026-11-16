# Profiling ladybug with USDT probes + hardware counters

This directory holds the tooling to measure **hardware instructions retired per
physical operator** for a query, using the Linux USDT probes built into the
engine (see `src/include/common/usdt.h`).

## The probes

Provider `lbug`, placed exactly where PROFILE measures wall times:

| Probe | Args | Fires in |
|---|---|---|
| `query_start` / `query_end` | `arg1`: uint64 query ID | `QueryProcessor::execute`, bracketing whole-query execution (≈ PROFILE's `WallClockTime` span) |
| `operator_start` / `operator_end` | `arg1`: int op type, `arg2`: uint32 op ID, `arg3`: uint64 query ID | `PhysicalOperator::getNextTuple` and `Sink::execute`, bracketing the regions timed by `OperatorMetrics::executionTime` |

`arg0` is unused by perf (SDT args are numbered from 1). The op-type int is
`static_cast<int>(PhysicalOperatorType)` in the declaration order of
`src/include/processor/operator/physical_operator.h`.

Probes are Linux-only and compile to no-ops unless `<sys/sdt.h>` is present
(auto-detected via `__has_include`, no CMake changes). Prod binaries get the
header via `systemtap-sdt-dev` / `systemtap-sdt-devel` in
`.github/workflows/precompiled-bin-workflow.yml`. A disabled probe is a NOP
sled, so steady-state overhead is negligible.

## Prerequisites

- A build with the header present (check: `readelf -n <lbug> | grep stapsdt`).
- `perf` with the binary in the build-id cache:
  `perf buildid-cache --add <lbug>` (per user; root needs its own entry).
- Installing the uprobes writes to tracefs, so **the record step needs root**,
  even with `perf_event_paranoid=-1` (that setting only unblocks the counters).

## Recipe A: perf (exact attribution, offline)

```bash
# one-time (root): install the uprobes
sudo perf buildid-cache --add ./build/release/tools/shell/lbug
sudo perf probe --add sdt_lbug:query_start
sudo perf probe --add sdt_lbug:query_end
sudo perf probe --add sdt_lbug:operator_start
sudo perf probe --add sdt_lbug:operator_end

# record: instruction sampling + USDT markers
echo "<query>" | sudo perf record -o op.data -e instructions:u -c 100000 \
  -e sdt_lbug:query_start -e sdt_lbug:query_end \
  -e sdt_lbug:operator_start -e sdt_lbug:operator_end \
  -- ./build/release/tools/shell/lbug --agent mydb.lbdb
sudo chmod 644 op.data

# attribute: per-thread operator stack from the markers; each sample's period
# is credited to the innermost operator (mirrors exclusive ExecutionTime)
perf script -F comm,tid,time,event,period,trace -i op.data | python3 attribute.py
```

Sample output for
`profile MATCH (t:person) WITH DISTINCT gen_random_uuid() as u RETURN count(*)`:

```
OPERATOR                         ID          INSTR      %
PROJECTION                        3     10,300,000  35.5
AGGREGATE                         4      8,200,000  28.3
FLATTEN                           2      3,800,000  13.1
AGGREGATE_FINALIZE                5      2,300,000   7.9
AGGREGATE_SCAN                    6      1,900,000   6.6
PROFILE                          13        700,000   2.4
UNATTRIBUTED                             1,800,000   6.2
TOTAL                                   29,000,000
```

Cross-checked against PROFILE's `ExecutionTime` for the same query
(AGGREGATE 10.4ms, PROJECTION 5.8ms, AGGREGATE_FINALIZE 4.1ms — same top-3).
Clean up afterwards with `perf probe --del 'probe_lbug:*'`.

## Recipe B: bpftrace (live)

```bash
sudo bpftrace op_insns.bt ./build/release/tools/shell/lbug
```

`$1` is the binary path. Keys of `@insns` are `(op_type, op_id)` tuples;
decode the type int with:

```python
import re
src = open("src/include/processor/operator/physical_operator.h").read()
names = [l.strip().rstrip(",") for l in
         re.search(r"enum class PhysicalOperatorType.*?{(.*?)};", src, re.S).group(1).splitlines()
         if l.strip() and not l.strip().startswith("//")]
mapping = {i: n for i, n in enumerate(names)}
```

## Caveats

- **Statistical.** Both recipes sample the PMU; error shrinks with longer
  runs or lower `-c` / sample periods. For exact per-invocation counts you
  would need an eBPF program doing `bpf_perf_event_read()` at each marker.
- **Unattributed instructions are real**: `init`/`finalize`, buffer-manager
  work, task scheduling, and result materialization run inside the query
  window but outside operator regions. A large unattributed share concentrated
  on the main thread *before* `query_start` is shell/parse/startup — make sure
  the attribution window comes from the query markers (as `attribute.py` does).
- **Balance check**: `operator_start`/`operator_end` counts should match per
  thread; a persistent imbalance means lost events and untrustworthy stacks.
- **Overhead**: tracing perturbs wall-clock times — don't compare PROFILE
  timings taken under tracing to untraced ones.
