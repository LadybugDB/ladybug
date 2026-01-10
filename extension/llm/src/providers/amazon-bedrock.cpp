#include "providers/amazon-bedrock.h"

#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "common/types/timestamp_t.h"
#include "crypto.h"
#include "function/llm_functions.h"
#include "main/client_context.h"
#include "yyjson.h"

using namespace lbug::common;

namespace lbug {
namespace llm_extension {

std::shared_ptr<EmbeddingProvider> BedrockEmbedding::getInstance() {
    return std::make_shared<BedrockEmbedding>();
}

std::string BedrockEmbedding::getClient() const {
    return "https://bedrock-runtime." + region.value_or("") + ".amazonaws.com";
}

std::string BedrockEmbedding::getPath(const std::string& model) const {
    return "/model/" + model + "/invoke";
}

httplib::Headers BedrockEmbedding::getHeaders(const std::string& model,
    const std::string& payload) const {
    static const std::string envVarAWSAccessKey = "AWS_ACCESS_KEY";
    static const std::string envVarAWSSecretAccessKey = "AWS_SECRET_ACCESS_KEY";
    auto envAWSAccessKey = main::ClientContext::getEnvVariable(envVarAWSAccessKey);
    auto envAWSSecretAccessKey = main::ClientContext::getEnvVariable(envVarAWSSecretAccessKey);
    if (envAWSAccessKey.empty() || envAWSSecretAccessKey.empty()) {
        std::string errMsg = "The following key(s) could not be read from the environment:\n";
        if (envAWSAccessKey.empty()) {
            errMsg += envVarAWSAccessKey + '\n';
        }
        if (envAWSSecretAccessKey.empty()) {
            errMsg += envVarAWSSecretAccessKey + '\n';
        }
        throw(RuntimeException(errMsg + std::string(referenceLbugDocs)));
    }
    std::string service = "bedrock";
    std::string regionStr = this->region.value_or("");
    std::string host = "bedrock-runtime." + regionStr + ".amazonaws.com";

    auto timestamp = Timestamp::getCurrentTimestamp();
    auto dateHeader = Timestamp::getDateHeader(timestamp);
    auto datetimeHeader = Timestamp::getDateTimeHeader(timestamp);

    std::string canonicalUri = StringUtils::encodeURL(getPath(model));
    std::string canonicalQueryString = "";

    httplib::Headers headers{{"host", host}, {"x-amz-date", datetimeHeader}};
    std::string canonicalHeaders;
    std::string signedHeaders;
    for (const auto& header : headers) {
        canonicalHeaders += header.first + ":" + header.second + "\n";
        if (!signedHeaders.empty()) {
            signedHeaders += ";";
        }
        signedHeaders += header.first;
    }

    using namespace httpfs_extension;
    std::string payloadStr = payload;
    hash_bytes payloadHashBytes;
    hash_str payloadHashHex;
    sha256(payloadStr.c_str(), payloadStr.size(), payloadHashBytes);
    hex256(payloadHashBytes, payloadHashHex);
    std::ostringstream canonicalRequest;
    canonicalRequest << "POST\n"
                     << canonicalUri << "\n"
                     << canonicalQueryString << "\n"
                     << canonicalHeaders << "\n"
                     << signedHeaders << "\n"
                     << std::string(reinterpret_cast<char*>(payloadHashHex), sizeof(hash_str));
    std::string canonicalRequestStr = canonicalRequest.str();

    hash_bytes canonicalRequestHashBytes;
    hash_str canonicalRequestHashHex;
    sha256(canonicalRequestStr.c_str(), canonicalRequestStr.size(), canonicalRequestHashBytes);
    hex256(canonicalRequestHashBytes, canonicalRequestHashHex);
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credentialScope =
        std::string(dateHeader) + "/" + regionStr + "/" + service + "/" + "aws4_request";
    std::ostringstream stringToSign;
    stringToSign << algorithm << "\n"
                 << datetimeHeader << "\n"
                 << credentialScope << "\n"
                 << std::string(reinterpret_cast<char*>(canonicalRequestHashHex), sizeof(hash_str));
    std::string stringToSignStr = stringToSign.str();

    hash_bytes kDate, kRegion, kService, kSigning;
    std::string kSecret = "AWS4" + envAWSSecretAccessKey;
    hmac256(dateHeader, kSecret.c_str(), kSecret.size(), kDate);
    hmac256(regionStr, kDate, kRegion);
    hmac256(service, kRegion, kService);
    hmac256("aws4_request", kService, kSigning);
    hash_bytes signatureBytes;
    hash_str signatureHex;
    hmac256(stringToSignStr, kSigning, signatureBytes);
    hex256(signatureBytes, signatureHex);
    std::ostringstream authorizationHeader;
    authorizationHeader << algorithm << " "
                        << "Credential=" << envAWSAccessKey << "/" << credentialScope << ", "
                        << "SignedHeaders=" << signedHeaders << ", "
                        << "Signature="
                        << std::string(reinterpret_cast<const char*>(signatureHex),
                               sizeof(hash_str));
    headers.insert({"Authorization", authorizationHeader.str()});
    return headers;
}

std::string BedrockEmbedding::getPayload(const std::string& /*model*/,
    const std::string& text) const {
    auto doc = yyjson_mut_doc_new(nullptr);
    auto root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "inputText", text.c_str());
    char* jsonStr = yyjson_mut_write(doc, 0, nullptr);
    std::string result(jsonStr);
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return result;
}

std::vector<float> BedrockEmbedding::parseResponse(const httplib::Result& res) const {
    auto doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
    auto embeddingVal = yyjson_obj_get(doc, "embedding");
    std::vector<float> result;
    size_t idx, max;
    yyjson_val* val;
    yyjson_arr_foreach(embeddingVal, idx, max, val) {
        result.push_back(yyjson_get_real(val));
    }
    yyjson_doc_free(doc);
    return result;
}

void BedrockEmbedding::configure(const std::optional<uint64_t>& dimensions,
    const std::optional<std::string>& region) {
    if (dimensions.has_value() || !region.has_value()) {
        static const auto functionSignatures = CreateEmbedding::getFunctionSet();
        throw(functionSignatures[1]->signatureToString());
    }
    this->region = region;
}

} // namespace llm_extension
} // namespace lbug
