#include "providers/voyage-ai.h"

#include "common/exception/runtime.h"
#include "function/llm_functions.h"
#include "main/client_context.h"
#include "yyjson.h"

using namespace lbug::common;

namespace lbug {
namespace llm_extension {

std::shared_ptr<EmbeddingProvider> VoyageAIEmbedding::getInstance() {
    return std::make_shared<VoyageAIEmbedding>();
}

std::string VoyageAIEmbedding::getClient() const {
    return "https://api.voyageai.com";
}

std::string VoyageAIEmbedding::getPath(const std::string& /*model*/) const {
    return "/v1/embeddings";
}

httplib::Headers VoyageAIEmbedding::getHeaders(const std::string& /*model*/,
    const std::string& /*payload*/) const {
    static const std::string envVar = "VOYAGE_API_KEY";
    auto env_key = main::ClientContext::getEnvVariable(envVar);
    if (env_key.empty()) {
        throw(RuntimeException("Could not read environmental variable: " + envVar + '\n' +
                               std::string(referenceLbugDocs)));
    }
    return httplib::Headers{{"Content-Type", "application/json"},
        {"Authorization", "Bearer " + env_key}};
}

std::string VoyageAIEmbedding::getPayload(const std::string& model, const std::string& text) const {
    auto doc = yyjson_mut_doc_new(nullptr);
    auto root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "model", model.c_str());
    yyjson_mut_obj_add_str(doc, root, "input", text.c_str());
    if (dimensions.has_value()) {
        yyjson_mut_obj_add_sint(doc, root, "output_dimension", dimensions.value());
    }
    char* jsonStr = yyjson_mut_write(doc, 0, nullptr);
    std::string result(jsonStr);
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return result;
}

std::vector<float> VoyageAIEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
    auto dataArr = yyjson_obj_get(doc, "data");
    auto embeddingArr = yyjson_arr_get(dataArr, 0);
    auto embeddingVal = yyjson_obj_get(embeddingArr, "embedding");
    std::vector<float> result;
    size_t idx, max;
    yyjson_val* val;
    yyjson_arr_foreach(embeddingVal, idx, max, val) {
        result.push_back(yyjson_get_real(val));
    }
    yyjson_doc_free(doc);
    return result;
}

void VoyageAIEmbedding::configure(const std::optional<uint64_t>& dimensions,
    const std::optional<std::string>& region) {
    if (region.has_value()) {
        static const auto functionSignatures = CreateEmbedding::getFunctionSet();
        throw(functionSignatures[0]->signatureToString() + '\n' +
              functionSignatures[2]->signatureToString());
    }
    this->dimensions = dimensions;
}

} // namespace llm_extension
} // namespace lbug
