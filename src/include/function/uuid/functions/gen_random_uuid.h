#pragma once

#include "common/types/uuid.h"

namespace lbug {
namespace function {

struct GenRandomUUID {
    static void operation(common::uuid& input, void* dataPtr);
};

} // namespace function
} // namespace lbug
