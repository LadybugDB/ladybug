#include <string>

#include "common/enums/storage_format.h"
#include "common/exception/binder.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/external_table_hooks.h"

using namespace lbug;
using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {

// ─────────────────────────────────────────────────────────────
// StorageFormatUtils: generic external formats
// ─────────────────────────────────────────────────────────────

TEST(ExternalFormatUtils, ParseKnownFormat) {
    auto parsed = StorageFormatUtils::parse("icebug-disk");
    EXPECT_EQ(parsed.format, StorageFormat::ICEBUG_DISK);
    EXPECT_TRUE(parsed.externalName.empty());
    parsed = StorageFormatUtils::parse("ICEBUG-DISK");
    EXPECT_EQ(parsed.format, StorageFormat::ICEBUG_DISK);
}

TEST(ExternalFormatUtils, ParseExtensionFormatIsGeneric) {
    // Core does not enumerate extension formats: any other value becomes EXTERNAL.
    auto parsed = StorageFormatUtils::parse("lance");
    EXPECT_EQ(parsed.format, StorageFormat::EXTERNAL);
    EXPECT_EQ(parsed.externalName, "lance");
    parsed = StorageFormatUtils::parse("Lance");
    EXPECT_EQ(parsed.format, StorageFormat::EXTERNAL);
    EXPECT_EQ(parsed.externalName, "lance");
    parsed = StorageFormatUtils::parse("delta");
    EXPECT_EQ(parsed.format, StorageFormat::EXTERNAL);
    EXPECT_EQ(parsed.externalName, "delta");
}

TEST(ExternalFormatUtils, DisplayName) {
    EXPECT_EQ(StorageFormatUtils::displayName(StorageFormat::NONE), "none");
    EXPECT_EQ(StorageFormatUtils::displayName(StorageFormat::ICEBUG_DISK), "icebug-disk");
    EXPECT_EQ(StorageFormatUtils::displayName(StorageFormat::EXTERNAL, "lance"), "lance");
    EXPECT_EQ(StorageFormatUtils::displayName(StorageFormat::EXTERNAL, "LANCE"), "lance");
    EXPECT_EQ(StorageFormatUtils::displayName(StorageFormat::EXTERNAL, ""), "external");
}

TEST(ExternalFormatUtils, Equals) {
    EXPECT_TRUE(StorageFormatUtils::equals(StorageFormat::NONE, "", StorageFormat::NONE, ""));
    EXPECT_TRUE(
        StorageFormatUtils::equals(StorageFormat::ICEBUG_DISK, "", StorageFormat::ICEBUG_DISK, ""));
    EXPECT_TRUE(StorageFormatUtils::equals(StorageFormat::EXTERNAL, "lance",
        StorageFormat::EXTERNAL, "lance"));
    EXPECT_TRUE(StorageFormatUtils::equals(StorageFormat::EXTERNAL, "LANCE",
        StorageFormat::EXTERNAL, "lance"));
    EXPECT_FALSE(StorageFormatUtils::equals(StorageFormat::EXTERNAL, "lance",
        StorageFormat::EXTERNAL, "delta"));
    EXPECT_FALSE(StorageFormatUtils::equals(StorageFormat::EXTERNAL, "lance",
        StorageFormat::ICEBUG_DISK, ""));
    EXPECT_FALSE(
        StorageFormatUtils::equals(StorageFormat::NONE, "", StorageFormat::EXTERNAL, "lance"));
}

// ─────────────────────────────────────────────────────────────
// ColumnarTableHookRegistry: name-keyed lookup + canHandle
// ─────────────────────────────────────────────────────────────

class ColumnarHookRegistryTest : public Test {
protected:
    void TearDown() override {
        ColumnarTableHookRegistry::unregisterHooks("lance");
        ColumnarTableHookRegistry::unregisterHooks("delta");
        ColumnarTableHookRegistry::unregisterHooks("test-format");
    }
};

TEST_F(ColumnarHookRegistryTest, RegisterAndLookupByName) {
    ColumnarTableHooks hooks;
    hooks.formatName = "lance";
    ColumnarTableHookRegistry::registerHooks("lance", std::move(hooks));
    auto found = ColumnarTableHookRegistry::getFormatHooks("lance");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->formatName, "lance");
    // Lookup is case-insensitive.
    EXPECT_NE(ColumnarTableHookRegistry::getFormatHooks("LANCE"), nullptr);
    EXPECT_EQ(ColumnarTableHookRegistry::getFormatHooks("delta"), nullptr);
}

TEST_F(ColumnarHookRegistryTest, CanHandleAlias) {
    ColumnarTableHooks hooks;
    hooks.formatName = "delta";
    hooks.canHandle = [](const std::string& name) { return name == "delta-rs"; };
    ColumnarTableHookRegistry::registerHooks("delta", std::move(hooks));
    EXPECT_NE(ColumnarTableHookRegistry::getFormatHooks("delta"), nullptr);
    EXPECT_NE(ColumnarTableHookRegistry::getFormatHooks("delta-rs"), nullptr);
    EXPECT_EQ(ColumnarTableHookRegistry::getFormatHooks("delta-other"), nullptr);
}

TEST_F(ColumnarHookRegistryTest, LegacyRankImpliesMixedScanSupport) {
    ColumnarTableHooks hooks;
    hooks.formatName = "lance";
    hooks.scanStateRank = 3;
    ColumnarTableHookRegistry::registerHooks("lance", std::move(hooks));
    auto found = ColumnarTableHookRegistry::getFormatHooks("lance");
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->supportsMixedColumnarScan);
}

TEST_F(ColumnarHookRegistryTest, TableBindingRoundTrip) {
    ColumnarTableHooks hooks;
    hooks.formatName = "test-format";
    auto shared = std::make_shared<const ColumnarTableHooks>(std::move(hooks));
    const StorageManager* fakeManager = nullptr;
    EXPECT_EQ(ColumnarTableHookRegistry::getTableHooks(fakeManager, 42), nullptr);
    ColumnarTableHookRegistry::bindTableHooks(fakeManager, 42, shared);
    EXPECT_EQ(ColumnarTableHookRegistry::getTableHooks(fakeManager, 42), shared);
    ColumnarTableHookRegistry::unbindTableHooks(fakeManager, 42);
    EXPECT_EQ(ColumnarTableHookRegistry::getTableHooks(fakeManager, 42), nullptr);
}

// ─────────────────────────────────────────────────────────────
// DDL: external formats without a loaded extension
// ─────────────────────────────────────────────────────────────

class ExternalFormatDDLTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }

    std::string runExpectError(const std::string& query) {
        auto result = conn->query(query);
        EXPECT_FALSE(result->isSuccess()) << query;
        return result->getErrorMessage();
    }
};

TEST_F(ExternalFormatDDLTest, UnknownFormatNodeTableAsksForExtension) {
    runExpectError("CREATE NODE TABLE p(id INT64 PRIMARY KEY) WITH (format = 'delta')");
    auto message =
        runExpectError("CREATE NODE TABLE p(id INT64 PRIMARY KEY) WITH (format = 'delta')");
    EXPECT_NE(message.find("load the extension"), std::string::npos) << message;
    EXPECT_NE(message.find("delta"), std::string::npos) << message;
}

TEST_F(ExternalFormatDDLTest, RelFormatMustMatchNodes) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE person(id INT64 PRIMARY KEY)")->isSuccess());
    // Extension rel format against native nodes: binder no-mixing error names the format.
    auto message =
        runExpectError("CREATE REL TABLE knows(FROM person TO person) WITH (format = 'lance')");
    EXPECT_NE(message.find("Cannot mix lance tables"), std::string::npos) << message;
}

TEST_F(ExternalFormatDDLTest, DistinctExternalFormatsDoNotMix) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE person(id INT64 PRIMARY KEY)")->isSuccess());
    auto message =
        runExpectError("CREATE REL TABLE knows(FROM person TO person) WITH (format = 'DELTA')");
    // Display names are normalized to lowercase.
    EXPECT_NE(message.find("Cannot mix delta tables"), std::string::npos) << message;
}

} // namespace testing
} // namespace lbug
