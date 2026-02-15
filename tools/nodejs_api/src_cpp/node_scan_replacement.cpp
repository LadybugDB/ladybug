#include "include/node_scan_replacement.h"
#include "include/node_stream_scan.h"
#include "include/node_util.h"

#include "function/table/bind_input.h"
#include "main/client_context.h"
#include "main/connection.h"

#include <napi.h>

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::main;

namespace {

std::mutex g_requestMutex;
std::atomic<uint64_t> g_nextRequestId{1};
std::unordered_map<uint64_t, std::unique_ptr<NodeStreamChunkRequest>> g_chunkRequests;

} // namespace

void NodeStreamRegistry::registerSource(const std::string& name, Napi::ThreadSafeFunction tsf,
    std::vector<std::string> columnNames, std::vector<LogicalType> columnTypes) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto state = std::make_shared<NodeStreamSourceState>();
    state->getChunkTsf = std::move(tsf);
    state->columnNames = std::move(columnNames);
    state->columnTypes = std::move(columnTypes);
    sources_[name] = std::move(state);
}

void NodeStreamRegistry::unregisterSource(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    sources_.erase(name);
}

std::vector<scan_replace_handle_t> NodeStreamRegistry::lookup(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sources_.find(name);
    if (it == sources_.end()) {
        return {};
    }
    return {reinterpret_cast<scan_replace_handle_t>(it->second.get())};
}

std::unique_ptr<ScanReplacementData> NodeStreamRegistry::replace(
    std::span<scan_replace_handle_t> handles) const {
    if (handles.empty()) {
        return nullptr;
    }
    auto* statePtr = reinterpret_cast<NodeStreamSourceState*>(handles[0]);
    auto state = std::shared_ptr<NodeStreamSourceState>(statePtr, [](NodeStreamSourceState*) {});
    auto data = std::make_unique<ScanReplacementData>();
    data->func = NodeStreamScanFunction::getFunction();
    data->bindInput.addLiteralParam(Value::createValue(reinterpret_cast<uint8_t*>(statePtr)));
    return data;
}

NodeStreamChunkRequest* NodeStreamRegistry::getChunkRequest(uint64_t requestId) {
    std::lock_guard<std::mutex> lock(g_requestMutex);
    auto it = g_chunkRequests.find(requestId);
    return it != g_chunkRequests.end() ? it->second.get() : nullptr;
}

void NodeStreamRegistry::setChunkRequest(uint64_t requestId,
    std::unique_ptr<NodeStreamChunkRequest> req) {
    std::lock_guard<std::mutex> lock(g_requestMutex);
    if (req) {
        g_chunkRequests[requestId] = std::move(req);
    } else {
        g_chunkRequests.erase(requestId);
    }
}

uint64_t NodeStreamRegistry::nextRequestId() {
    return g_nextRequestId++;
}

static std::vector<scan_replace_handle_t> lookupNodeStream(const std::string& objectName,
    void* registryVoid) {
    auto* registry = static_cast<NodeStreamRegistry*>(registryVoid);
    return registry->lookup(objectName);
}

static std::unique_ptr<ScanReplacementData> replaceNodeStream(
    std::span<scan_replace_handle_t> handles, void* registryVoid) {
    auto* registry = static_cast<const NodeStreamRegistry*>(registryVoid);
    return registry->replace(handles);
}

void addNodeScanReplacement(Connection* connection, NodeStreamRegistry* registry) {
    auto lookup = [registry](const std::string& name) {
        return lookupNodeStream(name, registry);
    };
    auto replace = [registry](std::span<scan_replace_handle_t> handles) {
        return replaceNodeStream(handles, registry);
    };
    connection->getClientContext()->addScanReplace(ScanReplacement(std::move(lookup), replace));
}

void returnChunkFromJS(uint64_t requestId, Napi::Array rowsNapi, bool done) {
    auto* req = NodeStreamRegistry::getChunkRequest(requestId);
    if (!req) {
        return;
    }
    std::vector<std::vector<lbug::common::Value>> rows;
    const size_t numRows = rowsNapi.Length();
    rows.reserve(numRows);
    for (size_t r = 0; r < numRows; r++) {
        Napi::Value rowVal = rowsNapi.Get(r);
        std::vector<lbug::common::Value> row;
        if (rowVal.IsArray()) {
            auto arr = rowVal.As<Napi::Array>();
            for (size_t c = 0; c < arr.Length(); c++) {
                row.push_back(Util::TransformNapiValue(arr.Get(c)));
            }
        } else if (rowVal.IsObject() && !rowVal.IsNull() && !rowVal.IsUndefined()) {
            auto obj = rowVal.As<Napi::Object>();
            auto names = obj.GetPropertyNames();
            for (size_t i = 0; i < names.Length(); i++) {
                row.push_back(Util::TransformNapiValue(obj.Get(names.Get(i))));
            }
        }
        rows.push_back(std::move(row));
    }
    {
        std::lock_guard<std::mutex> lock(req->mtx);
        req->rows = std::move(rows);
        req->done = done;
        req->filled = true;
    }
    req->cv.notify_one();
}
