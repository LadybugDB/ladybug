#include "connector/duckdb_secret_manager.h"

#include "s3fs_config.h"
#include <format>

namespace lbug {
namespace duckdb_extension {

static std::string getDuckDBExtensionOptions(httpfs_extension::S3AuthParams lbugOptions) {
    std::string options = "";
    options.append(std::format("KEY_ID '{}',", lbugOptions.accessKeyID));
    options.append(std::format("SECRET '{}',", lbugOptions.secretAccessKey));
    options.append(std::format("ENDPOINT '{}',", lbugOptions.endpoint));
    options.append(std::format("URL_STYLE '{}',", lbugOptions.urlStyle));
    options.append(std::format("REGION '{}',", lbugOptions.region));
    return options;
}

std::string DuckDBSecretManager::getRemoteS3FSSecret(main::ClientContext* context,
    const httpfs_extension::S3FileSystemConfig& config) {
    KU_ASSERT(config.fsName == "S3" || config.fsName == "GCS");
    static constexpr std::string_view templateQuery = R"(CREATE SECRET {}_secret (
        {}
        TYPE {}
    );)";
    return std::format(templateQuery, config.fsName,
        getDuckDBExtensionOptions(config.getAuthParams(context)), config.fsName);
}

} // namespace duckdb_extension
} // namespace lbug
