#include "providers/google-gemini.h"

#include "common/exception/runtime.h"
#include "function/llm_functions.h"
#include "main/client_context.h"

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
    const JsonMutDoc& /*payload*/) const {
    return httplib::Headers{{"Content-Type", "application/json"}};
}

JsonMutDoc GoogleGeminiEmbedding::getPayload(const std::string& model,
    const std::string& text) const {
    JsonMutDoc doc;
    auto root = doc.addRoot();
    root.addStr(doc.doc_, "model", ("models/" + model).c_str());
    auto contentObj = root.addObj(doc.doc_, "content");
    auto partsArr = contentObj.addArr(doc.doc_, "parts");
    auto partObj = partsArr.addObj(doc.doc_, "");
    partObj.addStr(doc.doc_, "text", text.c_str());
    return doc;
}

std::vector<float> GoogleGeminiEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = parseJson(res->body);
    auto root = doc.getRoot();
    auto embeddingObj = root.getObjKey("embedding");
    auto valuesArr = embeddingObj.getObjKey("values");
    std::vector<float> result;
    for (size_t i = 0; i < valuesArr.getArrSize(); i++) {
        result.push_back(valuesArr.getArr(i).getReal());
    }
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
