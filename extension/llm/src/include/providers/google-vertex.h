#pragma once

#include "common/copy_constructors.h"
#include "httplib.h"
#include "provider.h"

namespace lbug {
namespace llm_extension {

class GoogleVertexEmbedding final : public EmbeddingProvider {
public:
    GoogleVertexEmbedding() = default;
    DELETE_COPY_DEFAULT_MOVE(GoogleVertexEmbedding);
    ~GoogleVertexEmbedding() override = default;
    static std::shared_ptr<EmbeddingProvider> getInstance();
    std::string getClient() const override;
    std::string getPath(const std::string& model) const override;
    httplib::Headers getHeaders(const std::string& model, const JsonMutDoc& payload) const override;
    JsonMutDoc getPayload(const std::string& model, const std::string& text) const override;
    std::vector<float> parseResponse(const httplib::Result& res) const override;
    void configure(const std::optional<uint64_t>& dimensions,
        const std::optional<std::string>& region) override;

private:
    std::optional<std::string> region;
    std::optional<uint64_t> dimensions;
};

} // namespace llm_extension
} // namespace lbug
