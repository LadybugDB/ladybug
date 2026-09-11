#pragma once

#include <cstdint>

namespace lbug {
namespace planner {

enum class SemiMaskTargetType : uint8_t {
    SCAN_NODE = 0,
    RECURSIVE_EXTEND_INPUT_NODE = 2,
    RECURSIVE_EXTEND_OUTPUT_NODE = 3,
    RECURSIVE_EXTEND_PATH_NODE = 4,
    GDS_GRAPH_NODE = 5,
    // Semi mask on the neighbour node of a (packed) extend, i.e. filter the rel scan
    // output by the join key instead of materializing every expanded edge.
    EXTEND_NBR_NODE = 6,
};

}
} // namespace lbug
