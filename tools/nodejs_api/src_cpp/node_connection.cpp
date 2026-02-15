#include "include/node_connection.h"

#include <algorithm>
#include <iostream>

#include "include/node_database.h"
#include "include/node_query_result.h"
#include "include/node_scan_replacement.h"
#include "include/node_util.h"
#include "main/lbug.h"

Napi::Object NodeConnection::Init(Napi::Env env, Napi::Object exports) {
    Napi::HandleScope scope(env);

    Napi::Function t = DefineClass(env, "NodeConnection",
        {InstanceMethod("initAsync", &NodeConnection::InitAsync),
            InstanceMethod("initSync", &NodeConnection::InitSync),
            InstanceMethod("executeAsync", &NodeConnection::ExecuteAsync),
            InstanceMethod("queryAsync", &NodeConnection::QueryAsync),
            InstanceMethod("executeSync", &NodeConnection::ExecuteSync),
            InstanceMethod("querySync", &NodeConnection::QuerySync),
            InstanceMethod("setMaxNumThreadForExec", &NodeConnection::SetMaxNumThreadForExec),
            InstanceMethod("setQueryTimeout", &NodeConnection::SetQueryTimeout),
            InstanceMethod("interrupt", &NodeConnection::Interrupt),
            InstanceMethod("close", &NodeConnection::Close),
            InstanceMethod("registerStream", &NodeConnection::RegisterStream),
            InstanceMethod("unregisterStream", &NodeConnection::UnregisterStream),
            InstanceMethod("returnChunk", &NodeConnection::ReturnChunk)});

    exports.Set("NodeConnection", t);
    return exports;
}

NodeConnection::NodeConnection(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NodeConnection>(info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    NodeDatabase* nodeDatabase = Napi::ObjectWrap<NodeDatabase>::Unwrap(info[0].As<Napi::Object>());
    database = nodeDatabase->database;
}

Napi::Value NodeConnection::InitAsync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    auto callback = info[0].As<Napi::Function>();
    auto* asyncWorker = new ConnectionInitAsyncWorker(callback, this);
    asyncWorker->Queue();
    return info.Env().Undefined();
}

Napi::Value NodeConnection::InitSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    try {
        InitCppConnection();
    } catch (const std::exception& exc) {
        Napi::Error::New(env, exc.what()).ThrowAsJavaScriptException();
    }
    return env.Undefined();
}

void NodeConnection::InitCppConnection() {
    this->connection = std::make_shared<Connection>(database.get());
    ProgressBar::Get(*connection->getClientContext())
        ->setDisplay(std::make_shared<NodeProgressBarDisplay>());
    streamRegistry = std::make_unique<NodeStreamRegistry>();
    addNodeScanReplacement(connection.get(), streamRegistry.get());
    // After the connection is initialized, we do not need to hold a reference to the database.
    database.reset();
}

void NodeConnection::SetMaxNumThreadForExec(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    size_t numThreads = info[0].ToNumber().Int64Value();
    try {
        this->connection->setMaxNumThreadForExec(numThreads);
    } catch (const std::exception& exc) {
        Napi::Error::New(env, std::string(exc.what())).ThrowAsJavaScriptException();
    }
}

void NodeConnection::SetQueryTimeout(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    size_t timeout = info[0].ToNumber().Int64Value();
    try {
        this->connection->setQueryTimeOut(timeout);
    } catch (const std::exception& exc) {
        Napi::Error::New(env, std::string(exc.what())).ThrowAsJavaScriptException();
    }
}

void NodeConnection::Interrupt(const Napi::CallbackInfo& info) {
    if (this->connection) {
        this->connection->interrupt();
    }
}

void NodeConnection::Close(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    streamRegistry.reset();
    this->connection.reset();
}

Napi::Value NodeConnection::ExecuteAsync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    auto nodePreparedStatement =
        Napi::ObjectWrap<NodePreparedStatement>::Unwrap(info[0].As<Napi::Object>());
    auto nodeQueryResult = Napi::ObjectWrap<NodeQueryResult>::Unwrap(info[1].As<Napi::Object>());
    auto callback = info[3].As<Napi::Function>();
    try {
        auto params = Util::TransformParametersForExec(info[2].As<Napi::Array>());
        auto asyncWorker = new ConnectionExecuteAsyncWorker(callback, connection,
            nodePreparedStatement->preparedStatement, nodeQueryResult, std::move(params), info[4]);
        asyncWorker->Queue();
    } catch (const std::exception& exc) {
        Napi::Error::New(env, std::string(exc.what())).ThrowAsJavaScriptException();
    }
    return info.Env().Undefined();
}

Napi::Value NodeConnection::QuerySync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    auto statement = info[0].As<Napi::String>().Utf8Value();
    auto nodeQueryResult = Napi::ObjectWrap<NodeQueryResult>::Unwrap(info[1].As<Napi::Object>());
    try {
        auto result = connection->query(statement).release();
        nodeQueryResult->SetQueryResult(result, true);
        if (!result->isSuccess()) {
            Napi::Error::New(env, result->getErrorMessage()).ThrowAsJavaScriptException();
        }
    } catch (const std::exception& exc) {
        Napi::Error::New(env, std::string(exc.what())).ThrowAsJavaScriptException();
    }
    return env.Undefined();
}

Napi::Value NodeConnection::ExecuteSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    auto nodePreparedStatement =
        Napi::ObjectWrap<NodePreparedStatement>::Unwrap(info[0].As<Napi::Object>());
    auto nodeQueryResult = Napi::ObjectWrap<NodeQueryResult>::Unwrap(info[1].As<Napi::Object>());
    try {
        auto params = Util::TransformParametersForExec(info[2].As<Napi::Array>());
        auto result = connection
                          ->executeWithParams(nodePreparedStatement->preparedStatement.get(),
                              std::move(params))
                          .release();
        nodeQueryResult->SetQueryResult(result, true);
        if (!result->isSuccess()) {
            Napi::Error::New(env, result->getErrorMessage()).ThrowAsJavaScriptException();
        }
    } catch (const std::exception& exc) {
        Napi::Error::New(env, std::string(exc.what())).ThrowAsJavaScriptException();
    }
    return env.Undefined();
}

Napi::Value NodeConnection::QueryAsync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    auto statement = info[0].As<Napi::String>().Utf8Value();
    auto nodeQueryResult = Napi::ObjectWrap<NodeQueryResult>::Unwrap(info[1].As<Napi::Object>());
    auto callback = info[2].As<Napi::Function>();
    auto asyncWorker =
        new ConnectionQueryAsyncWorker(callback, connection, statement, nodeQueryResult, info[3]);
    asyncWorker->Queue();
    return info.Env().Undefined();
}

static lbug::common::LogicalType parseColumnType(const std::string& typeStr) {
    std::string upper = typeStr;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "INT64") return lbug::common::LogicalType(lbug::common::LogicalTypeID::INT64);
    if (upper == "INT32") return lbug::common::LogicalType(lbug::common::LogicalTypeID::INT32);
    if (upper == "INT16") return lbug::common::LogicalType(lbug::common::LogicalTypeID::INT16);
    if (upper == "INT8") return lbug::common::LogicalType(lbug::common::LogicalTypeID::INT8);
    if (upper == "UINT64") return lbug::common::LogicalType(lbug::common::LogicalTypeID::UINT64);
    if (upper == "UINT32") return lbug::common::LogicalType(lbug::common::LogicalTypeID::UINT32);
    if (upper == "UINT16") return lbug::common::LogicalType(lbug::common::LogicalTypeID::UINT16);
    if (upper == "UINT8") return lbug::common::LogicalType(lbug::common::LogicalTypeID::UINT8);
    if (upper == "DOUBLE") return lbug::common::LogicalType(lbug::common::LogicalTypeID::DOUBLE);
    if (upper == "FLOAT") return lbug::common::LogicalType(lbug::common::LogicalTypeID::FLOAT);
    if (upper == "STRING") return lbug::common::LogicalType(lbug::common::LogicalTypeID::STRING);
    if (upper == "BOOL" || upper == "BOOLEAN")
        return lbug::common::LogicalType(lbug::common::LogicalTypeID::BOOL);
    if (upper == "DATE") return lbug::common::LogicalType(lbug::common::LogicalTypeID::DATE);
    if (upper == "TIMESTAMP")
        return lbug::common::LogicalType(lbug::common::LogicalTypeID::TIMESTAMP);
    throw std::runtime_error("Unsupported column type for registerStream: " + typeStr);
}

Napi::Value NodeConnection::RegisterStream(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    if (!connection || !streamRegistry) {
        Napi::Error::New(env, "Connection not initialized.").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsFunction() || !info[2].IsArray()) {
        Napi::Error::New(env,
            "registerStream(name, getChunkCallback, columns): name string, getChunkCallback "
            "function(requestId), columns array of { name, type }.")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::string name = info[0].As<Napi::String>().Utf8Value();
    Napi::Function getChunkCallback = info[1].As<Napi::Function>();
    Napi::Array columnsArr = info[2].As<Napi::Array>();
    std::vector<std::string> columnNames;
    std::vector<lbug::common::LogicalType> columnTypes;
    for (uint32_t i = 0; i < columnsArr.Length(); i++) {
        Napi::Value col = columnsArr.Get(i);
        if (!col.IsObject()) continue;
        Napi::Object obj = col.As<Napi::Object>();
        if (!obj.Get("name").IsString() || !obj.Get("type").IsString()) continue;
        columnNames.push_back(obj.Get("name").As<Napi::String>().Utf8Value());
        columnTypes.push_back(
            parseColumnType(obj.Get("type").As<Napi::String>().Utf8Value()));
    }
    if (columnNames.empty()) {
        Napi::Error::New(env, "registerStream: at least one column required.").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        auto tsf = Napi::ThreadSafeFunction::New(
            env, getChunkCallback, "NodeStreamGetChunk", 0, 1);
        streamRegistry->registerSource(name, std::move(tsf), std::move(columnNames),
            std::move(columnTypes));
    } catch (const std::exception& exc) {
        Napi::Error::New(env, std::string(exc.what())).ThrowAsJavaScriptException();
    }
    return env.Undefined();
}

void NodeConnection::UnregisterStream(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    if (!streamRegistry) return;
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::Error::New(env, "unregisterStream(name): name string.").ThrowAsJavaScriptException();
        return;
    }
    streamRegistry->unregisterSource(info[0].As<Napi::String>().Utf8Value());
}

void NodeConnection::ReturnChunk(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsArray() || !info[2].IsBoolean()) {
        Napi::Error::New(env,
            "returnChunk(requestId, rows, done): requestId number, rows array of rows, done boolean.")
            .ThrowAsJavaScriptException();
        return;
    }
    uint64_t requestId = static_cast<uint64_t>(info[0].ToNumber().Int64Value());
    Napi::Array rows = info[1].As<Napi::Array>();
    bool done = info[2].ToBoolean().Value();
    returnChunkFromJS(requestId, rows, done);
}
