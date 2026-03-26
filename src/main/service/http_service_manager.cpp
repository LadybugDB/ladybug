#include "main/service/http_service_manager.h"

#include <stdexcept>

#include "yyjson.h"

namespace lbug {
namespace main {

HttpServiceManager::HttpServiceManager(QueryHandler queryHandler, SchemaHandler schemaHandler)
    : queryHandler_(std::move(queryHandler)), schemaHandler_(std::move(schemaHandler)) {}

HttpServiceManager::~HttpServiceManager() {
    stop();
}

void HttpServiceManager::init(const ServiceConfig& config) {
    config_ = config;
}

static std::string extractQueryFromBody(const std::string& body) {
    auto* doc = yyjson_read(body.c_str(), body.size(), 0);
    if (!doc) {
        return "";
    }
    auto* root = yyjson_doc_get_root(doc);
    auto* queryVal = yyjson_obj_get(root, "query");
    std::string query;
    if (queryVal && yyjson_is_str(queryVal)) {
        query = yyjson_get_str(queryVal);
    }
    yyjson_doc_free(doc);
    return query;
}

void HttpServiceManager::registerRoutes() {
    svr_.Post("/cypher", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto cypher = extractQueryFromBody(req.body);
            if (cypher.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"missing or empty 'query' field"})",
                    "application/json");
                return;
            }
            auto json = queryHandler_(cypher);
            res.set_content(json, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            // Build error JSON safely with yyjson
            auto* doc = yyjson_mut_doc_new(nullptr);
            auto* root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_strcpy(doc, root, "error", e.what());
            auto* str = yyjson_mut_write(doc, 0, nullptr);
            res.set_content(str ? str : R"({"error":"internal error"})", "application/json");
            free(str);
            yyjson_mut_doc_free(doc);
        }
    });

    svr_.Get("/cypher", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto cypher = req.get_param_value("q");
            if (cypher.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"missing 'q' parameter"})", "application/json");
                return;
            }
            auto json = queryHandler_(cypher);
            res.set_content(json, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            auto* doc = yyjson_mut_doc_new(nullptr);
            auto* root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_strcpy(doc, root, "error", e.what());
            auto* str = yyjson_mut_write(doc, 0, nullptr);
            res.set_content(str ? str : R"({"error":"internal error"})", "application/json");
            free(str);
            yyjson_mut_doc_free(doc);
        }
    });

    svr_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    svr_.Get("/schema", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto json = schemaHandler_();
            res.set_content(json, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            auto* doc = yyjson_mut_doc_new(nullptr);
            auto* root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_strcpy(doc, root, "error", e.what());
            auto* str = yyjson_mut_write(doc, 0, nullptr);
            res.set_content(str ? str : R"({"error":"internal error"})", "application/json");
            free(str);
            yyjson_mut_doc_free(doc);
        }
    });
}

std::string HttpServiceManager::start() {
    registerRoutes();
    if (!svr_.bind_to_port(config_.host, static_cast<int>(config_.port))) {
        throw std::runtime_error(
            "Failed to bind to " + config_.host + ":" + std::to_string(config_.port));
    }
    listenThread_ = std::thread([this] { svr_.listen_after_bind(); });
    svr_.wait_until_ready();
    running_ = true;
    return "http://" + config_.host + ":" + std::to_string(config_.port);
}

void HttpServiceManager::stop() {
    if (running_) {
        svr_.stop();
        if (listenThread_.joinable()) {
            listenThread_.join();
        }
        running_ = false;
    }
}

bool HttpServiceManager::isRunning() const {
    return running_;
}

} // namespace main
} // namespace lbug
