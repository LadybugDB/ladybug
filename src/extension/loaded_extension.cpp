#include "extension/loaded_extension.h"

#include "common/assert.h"
#include <format>

namespace lbug {
namespace extension {

std::string LoadedExtension::toCypher() {
    switch (source) {
    case ExtensionSource::OFFICIAL:
        return std::format("INSTALL {};\nLOAD EXTENSION {};\n", extensionName, extensionName);
    case ExtensionSource::USER:
        return std::format("LOAD EXTENSION '{}';\n", fullPath);
    case ExtensionSource::STATIC_LINKED:
        return "";
    default:
        KU_UNREACHABLE;
    }
}

} // namespace extension
} // namespace lbug
