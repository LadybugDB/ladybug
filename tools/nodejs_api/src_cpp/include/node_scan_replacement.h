#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "function/table/scan_replacement.h"
#include "function/table/table_function.h"
#include <napi.h>

namespace lbug {
namespace main {
class Connection;
}
namespace common {
class LogicalType;
}
} // namespace lbug

struct NodeStreamChunkRequest {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<lbug::common::Value>> rows;
    std::vector<std::string> columnNames; // schema order for object rows
    bool done = false;
    bool filled = false;
};

struct NodeStreamSourceState {
    Napi::ThreadSafeFunction getChunkTsf;
    std::vector<std::string> columnNames;
    std::vector<lbug::common::LogicalType> columnTypes;
};

class NodeStreamRegistry {
public:
    void registerSource(const std::string& name, Napi::ThreadSafeFunction tsf,
        std::vector<std::string> columnNames, std::vector<lbug::common::LogicalType> columnTypes);
    void unregisterSource(const std::string& name);
    std::vector<lbug::function::scan_replace_handle_t> lookup(const std::string& name) const;
    std::unique_ptr<lbug::function::ScanReplacementData> replace(
        std::span<lbug::function::scan_replace_handle_t> handles) const;

    static NodeStreamChunkRequest* getChunkRequest(uint64_t requestId);
    static void setChunkRequest(uint64_t requestId, std::unique_ptr<NodeStreamChunkRequest> req);
    static uint64_t nextRequestId();

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<NodeStreamSourceState>> sources_;
};

void addNodeScanReplacement(lbug::main::Connection* connection, NodeStreamRegistry* registry);

void returnChunkFromJS(uint64_t requestId, Napi::Array rowsNapi, bool done);
