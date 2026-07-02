#include <cstdlib>
#include <filesystem>
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

const std::vector<std::string> CA_CERT_ENV_VARS = {"SSL_CERT_FILE", "SSL_CERT_DIR"};

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

class ScopedCaCertEnv {
public:
    ScopedCaCertEnv() {
        for (auto& key : CA_CERT_ENV_VARS) {
            const auto value = std::getenv(key.c_str()); // NOLINT(*-mt-unsafe)
            savedEnv.push_back(value == nullptr ? std::nullopt : std::optional<std::string>{value});
            unsetEnv(key);
        }
    }

    ~ScopedCaCertEnv() {
        for (auto i = 0u; i < CA_CERT_ENV_VARS.size(); ++i) {
            if (savedEnv[i]) {
                setEnv(CA_CERT_ENV_VARS[i], *savedEnv[i]);
            } else {
                unsetEnv(CA_CERT_ENV_VARS[i]);
            }
        }
    }

private:
    std::vector<std::optional<std::string>> savedEnv;
};

bool isReadableNonEmptyFile(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !std::filesystem::is_empty(path, ec);
}

bool isReadableDir(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

} // namespace

TEST(ExtensionCaCertTest, UsesSSLCertFileWhenSetAndExisting) {
    ScopedCaCertEnv env;
    // Use a path that is guaranteed to exist on Linux CI; skip otherwise.
    const std::string knownFile = std::filesystem::exists("/etc/hostname") ? "/etc/hostname" : "";
    if (knownFile.empty()) {
        GTEST_SKIP() << "No known file to use as SSL_CERT_FILE stand-in on this host";
    }
    setEnv("SSL_CERT_FILE", knownFile);

    auto caCertPath = ExtensionUtils::getCaCertPath();
    ASSERT_TRUE(caCertPath.has_value());
    EXPECT_EQ(caCertPath->caCertFilePath, knownFile);
    EXPECT_TRUE(caCertPath->caCertDirPath.empty());
}

TEST(ExtensionCaCertTest, IgnoresStaleSSLCertFileAndFallsThrough) {
    ScopedCaCertEnv env;
    setEnv("SSL_CERT_FILE", "/nonexistent/path/that/does/not/exist.crt");

    auto caCertPath = ExtensionUtils::getCaCertPath();
    // It must NOT return the stale env path.
    if (caCertPath.has_value()) {
        EXPECT_NE(caCertPath->caCertFilePath, "/nonexistent/path/that/does/not/exist.crt");
    }
    // Either returns a well-known path/dir, or nullopt (let OpenSSL defaults apply).
}

TEST(ExtensionCaCertTest, UsesSSLCertDirWhenSetAndIsDirectory) {
    ScopedCaCertEnv env;
    const std::string knownDir = std::filesystem::exists("/tmp") ? "/tmp" : "";
    if (knownDir.empty()) {
        GTEST_SKIP() << "No /tmp directory to use as SSL_CERT_DIR stand-in on this host";
    }
    setEnv("SSL_CERT_DIR", knownDir);

    auto caCertPath = ExtensionUtils::getCaCertPath();
    ASSERT_TRUE(caCertPath.has_value());
    EXPECT_EQ(caCertPath->caCertDirPath, knownDir);
    EXPECT_TRUE(caCertPath->caCertFilePath.empty());
}

TEST(ExtensionCaCertTest, EnvFilePrecedenceOverEnvDir) {
    ScopedCaCertEnv env;
    const std::string knownFile = std::filesystem::exists("/etc/hostname") ? "/etc/hostname" : "";
    const std::string knownDir = std::filesystem::exists("/tmp") ? "/tmp" : "";
    if (knownFile.empty() || knownDir.empty()) {
        GTEST_SKIP() << "Need both a known file and a known dir on this host";
    }
    setEnv("SSL_CERT_FILE", knownFile);
    setEnv("SSL_CERT_DIR", knownDir);

    auto caCertPath = ExtensionUtils::getCaCertPath();
    ASSERT_TRUE(caCertPath.has_value());
    EXPECT_EQ(caCertPath->caCertFilePath, knownFile);
    EXPECT_TRUE(caCertPath->caCertDirPath.empty());
}

TEST(ExtensionCaCertTest, ReturnsNulloptOrValidPathWhenNoEnvSet) {
    ScopedCaCertEnv env;
    auto caCertPath = ExtensionUtils::getCaCertPath();
    if (caCertPath.has_value()) {
        // If it returns a value, it must be a real readable path.
        EXPECT_TRUE(isReadableNonEmptyFile(caCertPath->caCertFilePath) ||
                    isReadableDir(caCertPath->caCertDirPath));
        // Never both empty.
        EXPECT_FALSE(caCertPath->caCertFilePath.empty() && caCertPath->caCertDirPath.empty());
    }
    // std::nullopt is also acceptable: OpenSSL defaults will apply.
}
