#include "catalog/catalog_entry/catalog_entry_type.h"
#include "common/enums/scan_source_type.h"
#include "gtest/gtest.h"

using namespace lbug;

TEST(EnumToStringTest, CatalogEntryTypes) {
    EXPECT_EQ(catalog::CatalogEntryTypeUtils::toString(catalog::CatalogEntryType::TYPE_ENTRY),
        "TYPE_ENTRY");
    EXPECT_EQ(catalog::CatalogEntryTypeUtils::toString(catalog::CatalogEntryType::INDEX_ENTRY),
        "INDEX_ENTRY");
}

TEST(EnumToStringTest, ScanSourceTypes) {
    EXPECT_EQ(common::ScanSourceTypeUtils::toString(common::ScanSourceType::TABLE_FUNC),
        "TABLE_FUNC");
    EXPECT_EQ(common::ScanSourceTypeUtils::toString(common::ScanSourceType::PARAM), "PARAM");
}

TEST(EnumToStringTest, OutOfRangeValuesStillFail) {
    EXPECT_ANY_THROW(
        catalog::CatalogEntryTypeUtils::toString(static_cast<catalog::CatalogEntryType>(255)));
    EXPECT_ANY_THROW(
        common::ScanSourceTypeUtils::toString(static_cast<common::ScanSourceType>(255)));
}
