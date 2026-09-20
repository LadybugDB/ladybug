#pragma once

#include "common/types/date_t.h"              // IWYU pragma: export
#include "common/types/dtime_t.h"             // IWYU pragma: export
#include "common/types/int128_t.h"            // IWYU pragma: export
#include "common/types/interval_t.h"          // IWYU pragma: export
#include "common/types/timestamp_t.h"         // IWYU pragma: export
#include "common/types/types.h"               // IWYU pragma: export
#include "common/types/value/nested.h"        // IWYU pragma: export
#include "common/types/value/node.h"          // IWYU pragma: export
#include "common/types/value/recursive_rel.h" // IWYU pragma: export
#include "common/types/value/rel.h"           // IWYU pragma: export
#include "common/types/value/value.h"         // IWYU pragma: export
#include "main/connection.h"                  // IWYU pragma: export
#include "main/database.h"                    // IWYU pragma: export
#include "main/prepared_statement.h"          // IWYU pragma: export
#include "main/query_result.h"                // IWYU pragma: export
#include "main/query_summary.h"               // IWYU pragma: export
#include "main/storage_driver.h"              // IWYU pragma: export
#include "main/version.h"                     // IWYU pragma: export
#include "processor/result/flat_tuple.h"      // IWYU pragma: export
#include "storage/storage_version_info.h"     // IWYU pragma: export

// ===== Distributed API: partition routing hooks =====
// Kept separate from the client API above. These hooks let an external wrapper own
// remote partition subgraphs (placement, lifecycle, reads, writes, lookups) while the
// engine stays embedded and distribution-agnostic. Wrappers need nothing beyond this
// amalgamated header to implement them.
#include "main/lbug_distributed.h" // IWYU pragma: export
