#include "providers/ollama.h"

#include "function/llm_functions.h"
#include "main/client_context.h"

using namespace lbug::common;

namespace lbug {
namespace llm_extension {

std::shared_ptr<EmbeddingProvider> OllamaEmbedding::getInstance() {
    return std::make_shared<OllamaEmbedding>();
}

std::string OllamaEmbedding::getClient() const {
    return endpoint.value_or("http://localhost:11434");
}

std::string OllamaEmbedding::getPath(const std::string& /*model*/) const {
    return "/api/embeddings";
}

httplib::Headers OllamaEmbedding::getHeaders(const std::string& /*model*/,
    const JsonMutDoc& /*payload*/) const {
    return httplib::Headers{{"Content-Type", "application/json"}};
}

JsonMutDoc OllamaEmbedding::getPayload(const std::string& model, const std::string& text) const {
    JsonMutDoc doc;
    auto root = doc.addRoot();
    root.addStr(doc.doc_, "model", model.c_str());
    root.addStr(doc.doc_, "prompt", text.c_str());
    return doc;
}

std::vector<float> OllamaEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = parseJson(res->body);
    auto root = doc.getRoot();
    auto embeddingArr = root.getObjKey("embedding");
    std::vector<float> result;
    for (size_t i = 0; i < embeddingArr.getArrSize(); i++) {
        result.push_back(embeddingArr.getArr(i).getReal());
    }
    return result;
}

void OllamaEmbedding::configure(const std::optional<uint64_t>& dimensions,
    const std::optional<std::string>& endpoint) {
    static const std::string envVarOllamaUrl = "OLLAMA_URL";
    if (dimensions.has_value()) {
        static const auto functionSignatures = CreateEmbedding::getFunctionSet();
        throw(functionSignatures[0]->signatureToString() + '\n' +
              functionSignatures[1]->signatureToString());
    }
    this->endpoint = endpoint;
    if (endpoint.has_value()) {
        return;
    }
    auto envOllamaUrl = main::ClientContext::getEnvVariable(envVarOllamaUrl);
    if (!envOllamaUrl.empty()) {
        this->endpoint = envOllamaUrl;
    }
}

} // namespace llm_extension
} // namespace lbug
