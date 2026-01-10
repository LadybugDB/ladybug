#include "providers/google-gemini.h"

#include "common/exception/runtime.h"
#include "function/llm_functions.h"
#include "main/client_context.h"
#include "yyjson.h"

using namespace lbug::common;

namespace lbug {
namespace llm_extension {

std::shared_ptr<EmbeddingProvider> GoogleGeminiEmbedding::getInstance() {
    return std::make_shared<GoogleGeminiEmbedding>();
}

std::string GoogleGeminiEmbedding::getClient() const {
    return "https://generativelanguage.googleapis.com";
}

std::string GoogleGeminiEmbedding::getPath(const std::string& model) const {
    static const std::string envVar = "GOOGLE_GEMINI_API_KEY";
    auto env_key = main::ClientContext::getEnvVariable(envVar);
    if (env_key.empty()) {
        throw(RuntimeException("Could not read environment variable: " + envVar + "\n" +
                               std::string(referenceLbugDocs)));
    }
    return "/v1beta/models/" + model + ":embedContent?key=" + env_key;
}

httplib::Headers GoogleGeminiEmbedding::getHeaders(const std::string& /*model*/,
    const std::string& /*payload*/) const {
    return httplib::Headers{{"Content-Type", "application/json"}};
}

std::string GoogleGeminiEmbedding::getPayload(const std::string& model,
    const std::string& text) const {
    auto doc = yyjson_mut_doc_new(nullptr);
    auto root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "model", ("models/" + model).c_str());
    auto contentObj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "content", contentObj);
    auto partsArr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, contentObj, "parts", partsArr);
    auto partObj = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(doc, partsArr, partObj);
    yyjson_mut_obj_add_str(doc, partObj, "text", text.c_str());
    char* jsonStr = yyjson_mut_write(doc, 0, nullptr);
    std::string result(jsonStr);
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return result;
}

std::vector<float> GoogleGeminiEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
    auto embeddingObj = yyjson_obj_get(doc, "embedding");
    auto valuesArr = yyjson_obj_get(embeddingObj, "values");
    std::vector<float> result;
    size_t idx, max;
    yyjson_val* val;
    yyjson_arr_foreach(valuesArr, idx, max, val) {
        result.push_back(yyjson_get_real(val));
    }
    yyjson_doc_free(doc);
    return result;
}

void GoogleGeminiEmbedding::configure(const std::optional<uint64_t>& dimensions,
    const std::optional<std::string>& region) {
    if (dimensions.has_value() || region.has_value()) {
        static const auto functionSignatures = CreateEmbedding::getFunctionSet();
        throw(functionSignatures[0]->signatureToString());
    }
}

} // namespace llm_extension
} // namespace lbug
