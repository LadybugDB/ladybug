#pragma once

#include "common/copy_constructors.h"
#include "httplib.h"
#include "provider.h"

namespace lbug {
namespace llm_extension {

class OllamaEmbedding final : public EmbeddingProvider {
public:
    OllamaEmbedding() = default;
    DELETE_COPY_DEFAULT_MOVE(OllamaEmbedding);
    ~OllamaEmbedding() override = default;
    static std::shared_ptr<EmbeddingProvider> getInstance();
    std::string getClient() const override;
    std::string getPath(const std::string& model) const override;
    httplib::Headers getHeaders(const std::string& model, const JsonMutDoc& payload) const override;
    JsonMutDoc getPayload(const std::string& model, const std::string& text) const override;
    std::vector<float> parseResponse(const httplib::Result& res) const override;
    void configure(const std::optional<uint64_t>& dimensions,
        const std::optional<std::string>& endpoint) override;

private:
    std::optional<std::string> endpoint;
};

} // namespace llm_extension
} // namespace lbug
