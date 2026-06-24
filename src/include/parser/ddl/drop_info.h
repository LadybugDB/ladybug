#pragma once
#include <string>

#include "common/enums/conflict_action.h"
#include "common/enums/drop_type.h"

namespace lbug {
namespace parser {

struct DropInfo {
    std::string name;
    common::DropType dropType;
    common::ConflictAction conflictAction;
    // Only used for DROP INDEX (TableName.IndexName); empty for other drop types.
    std::string indexName;
};

} // namespace parser
} // namespace lbug
