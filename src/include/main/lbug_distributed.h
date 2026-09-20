#pragma once

// Distributed API: partition routing hooks.
//
// Isolated from the client API in main/lbug.h, shipped by the same amalgamation. These
// hooks let an external wrapper own remote partition subgraphs while the engine stays
// embedded and distribution-agnostic:
//
// - common/partition_routing_hook.h: the hook contract (placement via locate, lifecycle
//   notifications, reads via bindScan, writes via insertRow/insertChunk, lookups).
// - function/table/bind_data.h, binder/expression/variable_expression.h: constructing
//   scan bind data for bindScan (columns exposing `<node>._ID` + `<node>.<property>`).
// - function/table/simple_table_function.h: the morsel-based scan kernel pattern for
//   serving remotely-routed partitions.
//
// A wrapper implementing these hooks needs no other engine headers beyond the
// amalgamated lbug.hpp.

#include "binder/expression/variable_expression.h" // IWYU pragma: export
#include "common/partition_routing_hook.h"         // IWYU pragma: export
#include "function/table/bind_data.h"              // IWYU pragma: export
#include "function/table/simple_table_function.h"  // IWYU pragma: export
