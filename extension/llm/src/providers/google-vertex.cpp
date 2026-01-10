#include "providers/google-vertex.h"

#include "common/exception/runtime.h"
#include "function/llm_functions.h"
#include "main/client_context.h"
#include "yyjson.h"

using namespace lbug::common;

namespace lbug {
namespace llm_extension {

std::shared_ptr<EmbeddingProvider> GoogleVertexEmbedding::getInstance() {
    return std::make_shared<GoogleVertexEmbedding>();
}

std::string GoogleVertexEmbedding::getClient() const {
    return "https://aiplatform.googleapis.com";
}

std::string GoogleVertexEmbedding::getPath(const std::string& model) const {
    static const std::string envVar = "GOOGLE_CLOUD_PROJECT_ID";
    auto env_project_id = main::ClientContext::getEnvVariable(envVar);
    if (env_project_id.empty()) {
        throw(RuntimeException(
            "Could not get project id from: " + envVar + '\n' + std::string(referenceLbugDocs)));
    }
    return "/v1/projects/" + env_project_id + "/locations/" + region.value_or("") +
           "/publishers/google/models/" + model + ":predict";
}

httplib::Headers GoogleVertexEmbedding::getHeaders(const std::string& /*model*/,
    const std::string& /*payload*/) const {
    static const std::string envVar = "GOOGLE_VERTEX_ACCESS_KEY";
    auto env_key = main::ClientContext::getEnvVariable(envVar);
    if (env_key.empty()) {
        throw(RuntimeException("Could not read environmental variable: " + envVar + '\n' +
                               std::string(referenceLbugDocs)));
    }
    return httplib::Headers{{"Content-Type", "application/json"},
        {"Authorization", "Bearer " + env_key}};
}

std::string GoogleVertexEmbedding::getPayload(const std::string& /*model*/,
    const std::string& text) const {
    auto doc = yyjson_mut_doc_new(nullptr);
    auto root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    auto instancesArr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "instances", instancesArr);
    auto instanceObj = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(doc, instancesArr, instanceObj);
    yyjson_mut_obj_add_str(doc, instanceObj, "content", text.c_str());
    yyjson_mut_obj_add_str(doc, instanceObj, "task_type", "RETRIEVAL_DOCUMENT");
    if (dimensions.has_value()) {
        auto paramsObj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_sint(doc, paramsObj, "outputDimensionality", dimensions.value());
        yyjson_mut_obj_add_val(doc, root, "parameters", paramsObj);
    }
    char* jsonStr = yyjson_mut_write(doc, 0, nullptr);
    std::string result(jsonStr);
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return result;
}

std::vector<float> GoogleVertexEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
    auto predictionsArr = yyjson_obj_get(doc, "predictions");
    auto pred0 = yyjson_arr_get(predictionsArr, 0);
    auto embeddingsObj = yyjson_obj_get(pred0, "embeddings");
    auto valuesArr = yyjson_obj_get(embeddingsObj, "values");
    std::vector<float> result;
    size_t idx, max;
    yyjson_val* val;
    yyjson_arr_foreach(valuesArr, idx, max, val) {
        result.push_back(yyjson_get_real(val));
    }
    yyjson_doc_free(doc);
    return result;
}

void GoogleVertexEmbedding::configure(const std::optional<uint64_t>& dimensions,
    const std::optional<std::string>& region) {
    if (!region.has_value()) {
        static const auto functionSignatures = CreateEmbedding::getFunctionSet();
        throw(functionSignatures[1]->signatureToString() + '\n' +
              functionSignatures[3]->signatureToString());
    }
    this->dimensions = dimensions;
    this->region = region;
}

} // namespace llm_extension
} // namespace lbug
