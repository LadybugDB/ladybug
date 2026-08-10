#pragma once

#include "common/api.h"
#include "common/types/types.h"
#include "function/comparison/comparison_operation.h"

namespace lbug {
namespace common {
class SelectionVector;
class ValueVector;
} // namespace common

namespace function {
namespace simd {

using SelectConstantKernel = common::sel_t (*)(const void* data, common::sel_t count,
    const void* constant, const uint64_t* nullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation);
using SelectVectorKernel = common::sel_t (*)(const void* left, const void* right,
    common::sel_t count, const uint64_t* leftNullMask, const uint64_t* rightNullMask,
    common::sel_t* output, common::PhysicalTypeID type, ComparisonOperation operation);

// Scalar oracle for differential tests and machines without AVX2.
LBUG_API common::sel_t selectConstantScalar(const void* data, common::sel_t count,
    const void* constant, const uint64_t* nullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation);

// Invokes the automatically selected kernel. An x86 build containing AVX2 dispatches once to AVX2
// or scalar; an ARM64 build uses NEON unless scalar execution is explicitly requested.
LBUG_API common::sel_t selectConstant(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, common::PhysicalTypeID type,
    ComparisonOperation operation);

LBUG_API common::sel_t selectVectorScalar(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation);

LBUG_API common::sel_t selectVector(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation);

// Attempts the dense vector-versus-flat filter path. Returns false when the input shape or type is
// unsupported, so the caller can execute the existing generic loop. When it returns true, output
// has been updated even when no rows matched.
LBUG_API bool trySelectConstant(common::ValueVector& data, common::ValueVector& constant,
    ComparisonOperation operation, common::SelectionVector& output);

// Attempts the dense vector-versus-vector path when both vectors share one unfiltered state.
LBUG_API bool trySelectVector(common::ValueVector& left, common::ValueVector& right,
    ComparisonOperation operation, common::SelectionVector& output);

} // namespace simd
} // namespace function
} // namespace lbug
