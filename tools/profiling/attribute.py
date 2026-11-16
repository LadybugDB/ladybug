#!/usr/bin/env python3
"""Attribute sampled instructions to ladybug operators using USDT markers.

Usage: perf script -F comm,tid,time,event,period,trace -i perf.data | python3 attribute.py

Keeps a per-thread operator stack from sdt_lbug:operator_start/end events and
credits each instructions:u sample (period x count) to the innermost operator,
mirroring PROFILE's exclusive ExecutionTime. Only samples inside the global
[query_start, query_end] window are counted.
"""
import re
import sys

SAMPLE_PERIOD_DEFAULT = 100_000

# op type int -> name, parsed from the engine enum in declaration order.
# tools/profiling/attribute.py -> repo root is two levels up.
NAMES = {}
try:
    from pathlib import Path

    ROOT = Path(__file__).resolve().parents[2]
    with open(ROOT / "src/include/processor/operator/physical_operator.h") as f:
        src = f.read()
    body = re.search(r"enum class PhysicalOperatorType.*?\{(.*?)\};", src, re.S).group(1)
    entries = [
        line.strip().rstrip(",")
        for line in body.splitlines()
        if line.strip() and not line.strip().startswith("//")
    ]
    NAMES = {i: n for i, n in enumerate(entries)}
except Exception as e:
    print(f"warning: could not parse op names: {e}", file=sys.stderr)


def opname(t):
    return NAMES.get(t, f"OP_{t}")


stacks = {}  # tid -> list of (op_type, op_id)
qstart, qend = None, None
insns = {}  # (op_type, op_id) -> instructions
unattributed = 0
events_seen = set()

sdt_re = re.compile(r"(?:sdt|probe)_lbug:(\w+)")
kv_re = re.compile(r"(\w+)=(\d+)")


def parse_tid_time(line):
    # perf script -F comm,tid,time,... renders: "<comm> <tid> <time>: ..."
    m = re.match(r"\s*\S+\s+(\d+)\s+([\d.]+):", line)
    if not m:
        return None, None
    return int(m.group(1)), float(m.group(2))


# Two passes: the query window is only known after seeing query_start/query_end,
# but perf script emits samples before those lines. Buffer first, then attribute.
lines = sys.stdin.readlines()
for line in lines:
    m = sdt_re.search(line)
    if not m:
        continue
    kind = m.group(1)
    t = parse_tid_time(line)[1]
    if t is None:
        continue
    if kind == "query_start":
        qstart = t if qstart is None else min(qstart, t)
    elif kind == "query_end":
        qend = t if qend is None else max(qend, t)

for line in lines:
    m = sdt_re.search(line)
    if m:
        kind = m.group(1)
        events_seen.add(kind)
        tid, _ = parse_tid_time(line)
        if tid is None:
            continue
        args = dict(kv_re.findall(line))
        if kind == "query_start":
            t = parse_tid_time(line)[1]
            qstart = t if qstart is None else min(qstart, t)
        elif kind == "query_end":
            t = parse_tid_time(line)[1]
            qend = t if qend is None else max(qend, t)
        elif kind in ("operator_start", "operator_end"):
            # Prefer named args; fall back to positional numbers after the event.
            if "arg0" in args:
                t, i = int(args["arg0"]), int(args.get("arg1", 0))
            elif "arg1" in args:
                # perf numbers SDT args from 1: arg1=op_type, arg2=op_id, arg3=query_id
                t, i = int(args["arg1"]), int(args.get("arg2", 0))
            else:
                nums = re.findall(r":\s*(\d+)", line.split(kind, 1)[1])
                if len(nums) < 2:
                    continue
                t, i = int(nums[0]), int(nums[1])
            st = stacks.setdefault(tid, [])
            if kind == "operator_start":
                st.append((t, i))
            elif st:
                st.pop()
        continue
    if "instructions:u:" in line or "instructions:" in line:
        tid, t = parse_tid_time(line)
        if tid is None:
            continue
        if qstart is not None and t < qstart:
            continue
        if qend is not None and t > qend:
            continue
        pm = re.search(r":\s+(\d+)\s+instructions", line)
        period = int(pm.group(1)) if pm else SAMPLE_PERIOD_DEFAULT
        st = stacks.get(tid, [])
        if st:
            key = st[-1]
            insns[key] = insns.get(key, 0) + period
        else:
            unattributed += period

total = sum(insns.values()) + unattributed
print(f"SDT events seen: {sorted(events_seen)}")
print(f"query window: [{qstart}, {qend}]")
print(f"{'OPERATOR':28} {'ID':>6} {'INSTR':>14} {'%':>6}")
for (t, i), n in sorted(insns.items(), key=lambda kv: -kv[1]):
    pct = 100.0 * n / total if total else 0
    print(f"{opname(t):28} {i:>6} {n:>14,} {pct:>5.1f}")
print(f"{'UNATTRIBUTED':28} {'':>6} {unattributed:>14,} "
      f"{100.0 * unattributed / total if total else 0:>5.1f}")
print(f"{'TOTAL':28} {'':>6} {total:>14,}")
