#pragma once

#include <cstdint>
#include <string>

namespace lbug {
namespace main {

struct ServiceConfig {
    std::string host = "127.0.0.1";
    uint32_t port = 8000;
    uint32_t poolSize = 4;
};

} // namespace main
} // namespace lbug
