#pragma once

#include "httplib.h"
#include "yyjson.h"

namespace lbug {
namespace llm_extension {

class EmbeddingProvider {
public:
    static constexpr const char* referenceLbugDocs =
        "For more information, please refer to the official Lbug documentation: "
        "https://docs.ladybugdb.com/extensions/llm/\n";
    virtual ~EmbeddingProvider() = default;
    virtual std::string getClient() const = 0;
    virtual std::string getPath(const std::string& model) const = 0;
    virtual httplib::Headers getHeaders(const std::string& model,
        const std::string& payload) const = 0;
    virtual std::string getPayload(const std::string& model, const std::string& text) const = 0;
    virtual std::vector<float> parseResponse(const httplib::Result& res) const = 0;
    virtual void configure(const std::optional<uint64_t>& dimensions,
        const std::optional<std::string>& regionOrEndpoint) = 0;
};

} // namespace llm_extension
} // namespace lbug
