#include "providers/ollama.h"

#include "function/llm_functions.h"
#include "main/client_context.h"
#include "yyjson.h"

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
    const std::string& /*payload*/) const {
    return httplib::Headers{{"Content-Type", "application/json"}};
}

std::string OllamaEmbedding::getPayload(const std::string& model, const std::string& text) const {
    auto doc = yyjson_mut_doc_new(nullptr);
    auto root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "model", model.c_str());
    yyjson_mut_obj_add_str(doc, root, "prompt", text.c_str());
    char* jsonStr = yyjson_mut_write(doc, 0, nullptr);
    std::string result(jsonStr);
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return result;
}

std::vector<float> OllamaEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
    auto embeddingArr = yyjson_obj_get(doc, "embedding");
    std::vector<float> result;
    size_t idx, max;
    yyjson_val* val;
    yyjson_arr_foreach(embeddingArr, idx, max, val) {
        result.push_back(yyjson_get_real(val));
    }
    yyjson_doc_free(doc);
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
