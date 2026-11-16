#pragma once

// USDT (User Statically Defined Tracing) probes for external observability.
// See https://docs.ebpf.io/linux/concepts/usdt/
//
// Probes are placed exactly where PROFILE measures wall times, so an eBPF
// tool (e.g. bpftrace) can reconstruct the same per-operator breakdown live,
// without running PROFILE:
//
//   provider lbug, probe query_start / query_end
//       arg0: uint64 query ID (ExecutionContext::queryID)
//       Fired in QueryProcessor::execute, bracketing whole-query execution.
//       Mirrors the WallClockTime attribute of the PROFILE root operator.
//
//   provider lbug, probe operator_start / operator_end
//       arg0: int operator type (static_cast<int>(PhysicalOperatorType),
//             see processor/operator/physical_operator.h for the mapping)
//       arg1: uint32 operator ID (PhysicalOperator::getOperatorID)
//       arg2: uint64 query ID
//       Fired in PhysicalOperator::getNextTuple around getNextTuplesInternal
//       and in Sink::execute around executeInternal -- the same regions timed
//       by OperatorMetrics::executionTime for PROFILE's ExecutionTime.
//
// Linux-only: active when <sys/sdt.h> (systemtap-sdt-dev) is available at
// compile time, detected via __has_include so no CMake changes are needed.
// Everywhere else the macros compile to nothing.
//
// NOTE: SDT probe arguments are evaluated even when no tracer is attached,
// so call sites must only pass trivially cheap expressions (casts and member
// loads -- never string conversions or allocations). A disabled probe itself
// is a NOP sled, so steady-state overhead is a few nanoseconds per firing.
//
// Example:
//   bpftrace -e 'usdt:./build/release/tools/shell/lbug:lbug:operator_start {
//       @start[arg1] = nsecs; }
//     usdt:./build/release/tools/shell/lbug:lbug:operator_end {
//       @ns[arg0, arg1] = sum(nsecs - @start[arg1]); }'

#if defined(__linux__)
#if defined(__has_include) && __has_include(<sys/sdt.h>)
#include <sys/sdt.h>
#define LBUG_HAVE_USDT 1
#endif
#endif

#ifdef LBUG_HAVE_USDT
#define LBUG_USDT_QUERY_START(queryID) DTRACE_PROBE1(lbug, query_start, queryID)
#define LBUG_USDT_QUERY_END(queryID) DTRACE_PROBE1(lbug, query_end, queryID)
#define LBUG_USDT_OPERATOR_START(opType, opID, queryID)                                            \
    DTRACE_PROBE3(lbug, operator_start, opType, opID, queryID)
#define LBUG_USDT_OPERATOR_END(opType, opID, queryID)                                              \
    DTRACE_PROBE3(lbug, operator_end, opType, opID, queryID)
#else
#define LBUG_USDT_QUERY_START(queryID) ((void)0)
#define LBUG_USDT_QUERY_END(queryID) ((void)0)
#define LBUG_USDT_OPERATOR_START(opType, opID, queryID) ((void)0)
#define LBUG_USDT_OPERATOR_END(opType, opID, queryID) ((void)0)
#endif
