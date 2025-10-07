#include "options/unity_catalog_options.h"

#include "extension/extension.h"
#include "main/client_context.h"
#include "main/database.h"

namespace lbug {
namespace unity_catalog_extension {

using namespace common;

void UnityCatalogOptions::registerExtensionOptions(main::Database* db) {
    ADD_EXTENSION_OPTION(UnityCatalogToken);
    ADD_EXTENSION_OPTION(UnityCatalogEndPoint);
}

void UnityCatalogOptions::setEnvValue(main::ClientContext* context) {
    auto token = context->getEnvVariable(UnityCatalogToken::NAME);
    auto endPoint = context->getEnvVariable(UnityCatalogEndPoint::NAME);
    if (token != "") {
        context->setExtensionOption(UnityCatalogToken::NAME, Value::createValue(token));
    }
    if (endPoint != "") {
        context->setExtensionOption(UnityCatalogToken::NAME, Value::createValue(endPoint));
    }
}

static std::string getUnityCatalogOptions(main::ClientContext* context, std::string optionName) {
    static common::case_insensitive_map_t<std::string> UNITY_CATALOG_OPTIONS = {
        {UnityCatalogToken::NAME, "TOKEN"}, {UnityCatalogEndPoint::NAME, "ENDPOINT"}};
    auto optionNameInDuckDB = UNITY_CATALOG_OPTIONS.at(optionName);
    auto optionValueInLbug = context->getCurrentSetting(optionName).toString();
    return common::stringFormat("{} '{}',", optionNameInDuckDB, optionValueInLbug);
}

std::string DuckDBUnityCatalogSecretManager::getSecret(main::ClientContext* context) {
    std::string templateQuery = R"(CREATE SECRET (
        {}
        TYPE UC
    );)";
    std::string options = "";
    options += getUnityCatalogOptions(context, UnityCatalogToken::NAME);
    options += getUnityCatalogOptions(context, UnityCatalogEndPoint::NAME);
    return common::stringFormat(templateQuery, options);
}

} // namespace unity_catalog_extension
} // namespace lbug
