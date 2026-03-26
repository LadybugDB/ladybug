#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "httplib.h"
#include "main/service/i_service_manager.h"

namespace lbug {
namespace main {

using QueryHandler = std::function<std::string(const std::string& cypher)>;
using SchemaHandler = std::function<std::string()>;

class HttpServiceManager : public IServiceManager {
public:
    HttpServiceManager(QueryHandler queryHandler, SchemaHandler schemaHandler);
    ~HttpServiceManager() override;

    void init(const ServiceConfig& config) override;
    std::string start() override;
    void stop() override;
    bool isRunning() const override;

private:
    void registerRoutes();

    QueryHandler queryHandler_;
    SchemaHandler schemaHandler_;
    ServiceConfig config_;

    httplib::Server svr_;
    std::thread listenThread_;
    std::atomic<bool> running_{false};
};

} // namespace main
} // namespace lbug
