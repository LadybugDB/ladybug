#include "main/service/lbug_server.h"

#include "main/service/http_service_manager.h"
#include "main/service/query_result_json_serializer.h"

namespace lbug {
namespace main {

LbugServer::LbugServer(std::unique_ptr<Database> db) : db_(std::move(db)) {}

LbugServer::~LbugServer() {
    stop();
}

std::string LbugServer::start(const ServiceConfig& config) {
    pool_ = std::make_unique<ConnectionPool>(db_.get(), config.poolSize);

    auto queryH = [this](const std::string& cypher) { return handleQuery(cypher); };
    auto schemaH = [this]() { return handleSchema(); };

    svcMgr_ = std::make_unique<HttpServiceManager>(std::move(queryH), std::move(schemaH));
    svcMgr_->init(config);
    return svcMgr_->start();
}

void LbugServer::stop() {
    if (svcMgr_) {
        svcMgr_->stop();
    }
}

bool LbugServer::isRunning() const {
    return svcMgr_ && svcMgr_->isRunning();
}

std::string LbugServer::handleQuery(const std::string& cypher) {
    auto guard = pool_->acquire();
    auto result = guard->query(cypher);
    return queryResultToJson(*result);
}

std::string LbugServer::handleSchema() {
    auto guard = pool_->acquire();
    auto result = guard->query("CALL show_tables() RETURN *");
    return queryResultToJson(*result);
}

} // namespace main
} // namespace lbug
