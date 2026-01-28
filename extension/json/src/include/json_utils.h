#pragma once

#include <memory>

#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "yyjson.h"

namespace lbug {
namespace json_extension {

class JsonMutWrapper;

class LBUG_API JsonWrapper {
    JsonWrapper();

public:
    explicit JsonWrapper(yyjson_doc* ptr, std::shared_ptr<char[]> buffer = nullptr);
    ~JsonWrapper();
    JsonWrapper(JsonWrapper& other) = delete;
    JsonWrapper(JsonWrapper&& other);

    yyjson_doc* ptr;
    std::shared_ptr<char[]> buffer;
};

class LBUG_API JsonMutWrapper {
public:
    JsonMutWrapper();
    explicit JsonMutWrapper(yyjson_mut_doc* ptr);
    ~JsonMutWrapper();
    JsonMutWrapper(JsonMutWrapper& other) = delete;
    JsonMutWrapper(JsonMutWrapper&& other);

    yyjson_mut_doc* ptr;
};

LBUG_API JsonWrapper jsonify(const common::ValueVector& vec, uint64_t pos);
LBUG_API yyjson_mut_val* jsonify(JsonMutWrapper& wrapper, const common::ValueVector& vec,
    uint64_t pos);
LBUG_API yyjson_mut_val* jsonifyAsString(JsonMutWrapper& wrapper, const common::ValueVector& vec,
    uint64_t pos);
// Converts an internal Lbug Value into json

LBUG_API std::vector<JsonWrapper> jsonifyQueryResult(
    const std::vector<std::shared_ptr<common::ValueVector>>& columns,
    const std::vector<std::string>& names);
// Converts an entire query result into a sequence of json values
LBUG_API common::LogicalType jsonSchema(const JsonWrapper& wrapper, int64_t depth = -1,
    int64_t breadth = -1);
LBUG_API common::LogicalType jsonSchema(yyjson_val* val, int64_t depth, int64_t breadth);
// depth indicates at what nested depth to stop
// breadth indicates the limit of how many children the root nested type is sampled
// -1 means to scan the whole thing
// may return ANY

LBUG_API void readJsonToValueVector(yyjson_val* val, common::ValueVector& vec, uint64_t pos);

LBUG_API std::string jsonToString(const JsonWrapper& wrapper);
LBUG_API std::string jsonToString(const yyjson_val* val);
LBUG_API JsonWrapper stringToJson(const std::string& str);
LBUG_API JsonWrapper stringToJsonNoError(const std::string& str);
// format can be 'unstructured' or 'array'

LBUG_API JsonWrapper mergeJson(const JsonWrapper& A, const JsonWrapper& B);

LBUG_API std::string jsonExtractToString(const JsonWrapper& wrapper, uint64_t pos);
LBUG_API std::string jsonExtractToString(const JsonWrapper& wrapper, std::string path);

LBUG_API uint32_t jsonArraySize(const JsonWrapper& wrapper);

} // namespace json_extension
} // namespace lbug
