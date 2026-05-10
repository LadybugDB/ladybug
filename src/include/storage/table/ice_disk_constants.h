#pragma once

#include <string>

#include "common/constants.h"

namespace lbug {
namespace storage {

struct IceDiskConstants {
    static constexpr std::string_view V1 = "v1";

    static constexpr std::string_view CURRENT_VERSION = V1;

    static constexpr std::string_view VERSION_METADATA_KEY = "icebug_disk_version";
};
} // namespace storage
} // namespace lbug
