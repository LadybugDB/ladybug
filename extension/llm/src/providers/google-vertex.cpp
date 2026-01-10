#include "providers/google-vertex.h"

#include "common/exception/runtime.h"
#include "function/llm_functions.h"
#include "main/client_context.h"

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
    const JsonMutDoc& /*payload*/) const {
    static const std::string envVar = "GOOGLE_VERTEX_ACCESS_KEY";
    auto env_key = main::ClientContext::getEnvVariable(envVar);
    if (env_key.empty()) {
        throw(RuntimeException("Could not read environmental variable: " + envVar + '\n' +
                               std::string(referenceLbugDocs)));
    }
    return httplib::Headers{{"Content-Type", "application/json"},
        {"Authorization", "Bearer " + env_key}};
}

JsonMutDoc GoogleVertexEmbedding::getPayload(const std::string& /*model*/,
    const std::string& text) const {
    JsonMutDoc doc;
    auto root = doc.addRoot();
    auto instancesArr = root.addArr(doc.doc_, "instances");
    auto instanceObj = instancesArr.addObj(doc.doc_, "");
    instanceObj.addStr(doc.doc_, "content", text.c_str());
    instanceObj.addStr(doc.doc_, "task_type", "RETRIEVAL_DOCUMENT");
    if (dimensions.has_value()) {
        auto paramsObj = root.addObj(doc.doc_, "parameters");
        paramsObj.addSint(doc.doc_, "outputDimensionality", dimensions.value());
    }
    return doc;
}

std::vector<float> GoogleVertexEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = parseJson(res->body);
    auto root = doc.getRoot();
    auto predictionsArr = root.getObjKey("predictions");
    auto pred0 = predictionsArr.getArr(0);
    auto embeddingsObj = pred0.getObjKey("embeddings");
    auto valuesArr = embeddingsObj.getObjKey("values");
    std::vector<float> result;
    for (size_t i = 0; i < valuesArr.getArrSize(); i++) {
        result.push_back(valuesArr.getArr(i).getReal());
    }
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
