#include "storage/storage_utils.h"

#include <filesystem>
#ifdef _WIN32
#include <cctype>
#include <string_view>
#endif

#include "common/null_buffer.h"
#include "common/types/list_t.h"
#include "common/types/string_t.h"
#include "common/types/types.h"
#include "main/client_context.h"
#include "main/db_config.h"
#include "main/settings.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace storage {

namespace {

#ifdef _WIN32
static bool isWindowsDrivePath(std::string_view path) {
    return path.length() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
           path[1] == ':';
}

static bool isWindowsUNCPath(std::string_view path) {
    return path.length() >= 2 &&
           ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'));
}
#endif

} // namespace

std::string StorageUtils::getColumnName(const std::string& propertyName, ColumnType type,
    const std::string& prefix) {
    switch (type) {
    case ColumnType::DATA: {
        return std::format("{}_data", propertyName);
    }
    case ColumnType::NULL_MASK: {
        return std::format("{}_null", propertyName);
    }
    case ColumnType::INDEX: {
        return std::format("{}_index", propertyName);
    }
    case ColumnType::OFFSET: {
        return std::format("{}_offset", propertyName);
    }
    case ColumnType::CSR_OFFSET: {
        return std::format("{}_csr_offset", prefix);
    }
    case ColumnType::CSR_LENGTH: {
        return std::format("{}_csr_length", prefix);
    }
    case ColumnType::STRUCT_CHILD: {
        return std::format("{}_{}_child", propertyName, prefix);
    }
    default: {
        if (prefix.empty()) {
            return propertyName;
        }
        return std::format("{}_{}", prefix, propertyName);
    }
    }
}

std::string StorageUtils::expandPath(const main::ClientContext* context, const std::string& path) {
    if (main::DBConfig::isDBPathInMemory(path)) {
        return path;
    }
    auto fullPath = path;
    // Handle '~' for home directory expansion
    if (path.starts_with('~')) {
        fullPath =
            context->getCurrentSetting(main::HomeDirectorySetting::name).getValue<std::string>() +
            fullPath.substr(1);
    }
    // Normalize the path to resolve '.' and '..'. Avoid std::filesystem::absolute
    // when the input already names a Windows drive/UNC path because absolute may
    // consult current_path(), which can fail if the process cwd has been deleted.
    std::filesystem::path normalizedPath;
    std::filesystem::path filePath{fullPath};
    auto shouldAvoidAbsolute = filePath.is_absolute();
#ifdef _WIN32
    shouldAvoidAbsolute =
        shouldAvoidAbsolute || isWindowsDrivePath(fullPath) || isWindowsUNCPath(fullPath);
#endif
    if (shouldAvoidAbsolute) {
        normalizedPath = filePath.lexically_normal();
    } else {
        normalizedPath = std::filesystem::absolute(filePath).lexically_normal();
    }
    return normalizedPath.string();
}

uint32_t StorageUtils::getDataTypeSize(const LogicalType& type) {
    switch (type.getPhysicalType()) {
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::JSON: {
        return sizeof(string_t);
    }
    case PhysicalTypeID::ARRAY:
    case PhysicalTypeID::LIST: {
        return sizeof(list_t);
    }
    case PhysicalTypeID::STRUCT: {
        uint32_t size = 0;
        const auto fieldsTypes = StructType::getFieldTypes(type);
        for (const auto& fieldType : fieldsTypes) {
            size += getDataTypeSize(*fieldType);
        }
        size += NullBuffer::getNumBytesForNullValues(fieldsTypes.size());
        return size;
    }
    default: {
        return PhysicalTypeUtils::getFixedTypeSize(type.getPhysicalType());
    }
    }
}

} // namespace storage
} // namespace lbug
