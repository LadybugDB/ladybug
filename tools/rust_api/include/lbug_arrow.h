#pragma once

#include "rust/cxx.h"
#ifdef KUZU_BUNDLED
#include "main/lbug.h"
#else
#include <lbug.hpp>
#endif

namespace kuzu_arrow {

ArrowSchema query_result_get_arrow_schema(const lbug::main::QueryResult& result);
ArrowArray query_result_get_next_arrow_chunk(lbug::main::QueryResult& result, uint64_t chunkSize);

} // namespace kuzu_arrow
