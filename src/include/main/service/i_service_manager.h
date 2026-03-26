#pragma once

#include <string>

#include "main/service/service_config.h"

namespace lbug {
namespace main {

class IServiceManager {
public:
    virtual ~IServiceManager() = default;
    virtual void init(const ServiceConfig& config) = 0;
    virtual std::string start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};

} // namespace main
} // namespace lbug
