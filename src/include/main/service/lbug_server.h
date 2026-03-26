#pragma once

#include <memory>
#include <string>

#include "main/database.h"
#include "main/service/connection_pool.h"
#include "main/service/i_service_manager.h"
#include "main/service/service_config.h"

namespace lbug {
namespace main {

class LbugServer {
public:
    explicit LbugServer(std::unique_ptr<Database> db);
    ~LbugServer();

    LbugServer(const LbugServer&) = delete;
    LbugServer& operator=(const LbugServer&) = delete;

    std::string start(const ServiceConfig& config);
    void stop();
    bool isRunning() const;

private:
    std::string handleQuery(const std::string& cypher);
    std::string handleSchema();

    std::unique_ptr<Database> db_;
    std::unique_ptr<ConnectionPool> pool_;
    std::unique_ptr<IServiceManager> svcMgr_;
};

} // namespace main
} // namespace lbug
