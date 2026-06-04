#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "extension/extension.h"
#include "gtest/gtest.h"

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace lbug::extension;

namespace {

const std::vector<std::string> PROXY_ENV_VARS = {"LADYBUG_HTTP_PROXY", "LADYBUG_HTTPS_PROXY",
    "LADYBUG_ALL_PROXY", "LADYBUG_NO_PROXY", "http_proxy", "HTTP_PROXY", "https_proxy",
    "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY"};

void setEnv(const std::string& key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

void unsetEnv(const std::string& key) {
#ifdef _WIN32
    _putenv_s(key.c_str(), "");
#else
    unsetenv(key.c_str());
#endif
}

class ScopedProxyEnv {
public:
    ScopedProxyEnv() {
        for (auto& key : PROXY_ENV_VARS) {
            const auto value = std::getenv(key.c_str()); // NOLINT(*-mt-unsafe)
            savedEnv.push_back(value == nullptr ? std::nullopt : std::optional<std::string>{value});
            unsetEnv(key);
        }
    }

    ~ScopedProxyEnv() {
        for (auto i = 0u; i < PROXY_ENV_VARS.size(); ++i) {
            if (savedEnv[i]) {
                setEnv(PROXY_ENV_VARS[i], *savedEnv[i]);
            } else {
                unsetEnv(PROXY_ENV_VARS[i]);
            }
        }
    }

private:
    std::vector<std::optional<std::string>> savedEnv;
};

} // namespace

TEST(ExtensionProxyTest, ParseProxyURLWithAuth) {
    auto config = ExtensionUtils::parseProxyConfig("http://user:pass@proxy.example.com:8080");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->host, "proxy.example.com");
    EXPECT_EQ(config->port, 8080);
    EXPECT_EQ(config->username, "user");
    EXPECT_EQ(config->password, "pass");
}

TEST(ExtensionUtilsTest, IdentifiesOfficialExtensions) {
    EXPECT_TRUE(ExtensionUtils::isOfficialExtension("adbc"));
    EXPECT_TRUE(ExtensionUtils::isOfficialExtension("ADBC"));
    EXPECT_FALSE(ExtensionUtils::isOfficialExtension("sqlitescanner"));
}

TEST(ExtensionUtilsTest, OfficialExtensionRepoUsesHTTPS) {
    EXPECT_STREQ(ExtensionUtils::OFFICIAL_EXTENSION_REPO, "https://extension.ladybugdb.com/");
}

TEST(ExtensionUtilsTest, BuildsHTTPSRepoInfo) {
    auto repoInfo =
        ExtensionUtils::getExtensionLibRepoInfo("json", "https://extension.ladybugdb.com/");

    EXPECT_EQ(repoInfo.hostURL, "https://extension.ladybugdb.com");
    EXPECT_EQ(repoInfo.hostPath, "/v" + std::string{LBUG_EXTENSION_VERSION} + "/" + getPlatform() +
                                     "/json/libjson.lbug_extension");
    EXPECT_EQ(repoInfo.repoURL, repoInfo.hostURL + repoInfo.hostPath);
}

TEST(ExtensionUtilsTest, BuildsHTTPRepoInfo) {
    auto repoInfo =
        ExtensionUtils::getExtensionLibRepoInfo("json", "http://extension.ladybugdb.com/");

    EXPECT_EQ(repoInfo.hostURL, "http://extension.ladybugdb.com");
    EXPECT_EQ(repoInfo.hostPath, "/v" + std::string{LBUG_EXTENSION_VERSION} + "/" + getPlatform() +
                                     "/json/libjson.lbug_extension");
    EXPECT_EQ(repoInfo.repoURL, repoInfo.hostURL + repoInfo.hostPath);
}

TEST(ExtensionProxyTest, ParseProxyURLWithoutSchemeUsesDefaultPort) {
    auto config = ExtensionUtils::parseProxyConfig("proxy.example.com");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->host, "proxy.example.com");
    EXPECT_EQ(config->port, 80);
    EXPECT_TRUE(config->username.empty());
    EXPECT_TRUE(config->password.empty());
}

TEST(ExtensionProxyTest, UsesLadybugHTTPProxyForHTTPURLs) {
    ScopedProxyEnv env;
    setEnv("LADYBUG_HTTP_PROXY", "http://proxy.example.com:8080");

    auto config = ExtensionUtils::getProxyConfigForURL("http://extension.ladybugdb.com");

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->host, "proxy.example.com");
    EXPECT_EQ(config->port, 8080);
}

TEST(ExtensionProxyTest, UsesLadybugHTTPSProxyForHTTPSURLs) {
    ScopedProxyEnv env;
    setEnv("LADYBUG_HTTPS_PROXY", "https-proxy.example.com:8443");
    setEnv("LADYBUG_HTTP_PROXY", "http-proxy.example.com:8080");

    auto config = ExtensionUtils::getProxyConfigForURL("https://extension.ladybugdb.com");

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->host, "https-proxy.example.com");
    EXPECT_EQ(config->port, 8443);
}

TEST(ExtensionProxyTest, NoProxyBypassesProxy) {
    ScopedProxyEnv env;
    setEnv("LADYBUG_HTTP_PROXY", "proxy.example.com:8080");
    setEnv("LADYBUG_NO_PROXY", ".ladybugdb.com");

    auto config = ExtensionUtils::getProxyConfigForURL("http://extension.ladybugdb.com");

    EXPECT_FALSE(config.has_value());
}
