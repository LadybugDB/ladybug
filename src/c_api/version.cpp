#include "main/version.h"

#include "c_api/helpers.h"
#include "c_api/lbug.h"

char* lbug_get_version() {
    auto version = lbug::main::Version::getVersion();
    if (version == nullptr || version[0] == '\0') {
        version = "0.18.1";
    }
    return convertToOwnedCString(version);
}

uint64_t lbug_get_storage_version() {
    return lbug::main::Version::getStorageVersion();
}
