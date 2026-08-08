#include "main/version.h"

#include "c_api/helpers.h"
#include "c_api/lbug.h"

char* lbug_get_version() {
    LBUG_C_API_GUARD_BEGIN
    auto version = lbug::main::Version::getVersion();
    if (version == nullptr || version[0] == '\0') {
        version = "0.19.1";
    }
    return convertToOwnedCString(version);
    LBUG_C_API_GUARD_END(nullptr)
}

uint64_t lbug_get_storage_version() {
    LBUG_C_API_GUARD_BEGIN
    return lbug::main::Version::getStorageVersion();
    LBUG_C_API_GUARD_END(0)
}
