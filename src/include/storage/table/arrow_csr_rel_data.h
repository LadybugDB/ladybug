#pragma once

#include <optional>
#include <vector>

#include "common/arrow/arrow.h"

namespace lbug {
namespace storage {

// One directional adjacency for a CSR-layout Arrow rel table.
// indices batches: struct with child[0] = UINT64 neighbour offset, child[1..] = edge properties
// indptr  batches: struct with child[0] = UINT64 row pointers (indptr[i] = first edge of node i,
//                  N+1 entries where indptr[0]==0)
struct ArrowCsrAdj {
    ArrowSchemaWrapper indicesSchema;
    std::vector<ArrowArrayWrapper> indices;
    ArrowSchemaWrapper indptrSchema;
    std::vector<ArrowArrayWrapper> indptr;
};

// CSR adjacency data for one Arrow rel table.
// fwd is required; bwd enables O(degree) backward scans when present.
struct ArrowCsrRelData {
    ArrowCsrAdj fwd;
    std::optional<ArrowCsrAdj> bwd;
};

} // namespace storage
} // namespace lbug
