#include "providers/voyage-ai.h"

#include "common/exception/runtime.h"
#include "function/llm_functions.h"
#include "main/client_context.h"

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
    const JsonMutDoc& /*payload*/) const {
    static const std::string envVar = "VOYAGE_API_KEY";
    auto env_key = main::ClientContext::getEnvVariable(envVar);
    if (env_key.empty()) {
        throw(RuntimeException("Could not read environmental variable: " + envVar + '\n' +
                               std::string(referenceLbugDocs)));
    }
    return httplib::Headers{{"Content-Type", "application/json"},
        {"Authorization", "Bearer " + env_key}};
}

JsonMutDoc VoyageAIEmbedding::getPayload(const std::string& model, const std::string& text) const {
    JsonMutDoc doc;
    auto root = doc.addRoot();
    root.addStr(doc.doc_, "model", model.c_str());
    root.addStr(doc.doc_, "input", text.c_str());
    if (dimensions.has_value()) {
        root.addSint(doc.doc_, "output_dimension", dimensions.value());
    }
    return doc;
}

std::vector<float> VoyageAIEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = parseJson(res->body);
    auto root = doc.getRoot();
    auto dataArr = root.getObjKey("data");
    auto embeddingArr = dataArr.getArr(0);
    auto embeddingVal = embeddingArr.getObjKey("embedding");
    std::vector<float> result;
    for (size_t i = 0; i < embeddingVal.getArrSize(); i++) {
        result.push_back(embeddingVal.getArr(i).getReal());
    }
    return result;
}

void VoyageAIEmbedding::configure(const std::optional<uint64_t>& dimensions,
    const std::optional<std::string>& region) {
    (void)dimensions;
    if (region.has_value()) {
        static const auto functionSignatures = CreateEmbedding::getFunctionSet();
        throw(functionSignatures[0]->signatureToString() + '\n' +
              functionSignatures[2]->signatureToString());
    }
}

} // namespace llm_extension
} // namespace lbug
