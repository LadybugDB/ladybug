#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "common/types/value/value.h"

namespace lbug {
namespace common {
enum class LogicalTypeID : uint8_t;
} // namespace common

namespace main {

class ClientContext;

typedef void (*set_context)(ClientContext* context, const common::Value& parameter);
typedef common::Value (*get_setting)(const ClientContext* context);

enum class OptionType : uint8_t { CONFIGURATION = 0, EXTENSION = 1 };

struct Option {
    std::string name;
    common::LogicalTypeID parameterType;
    OptionType optionType;
    bool isConfidential;

    Option(std::string name, common::LogicalTypeID parameterType, OptionType optionType,
        bool isConfidential)
        : name{std::move(name)}, parameterType{parameterType}, optionType{optionType},
          isConfidential{isConfidential} {}

    virtual ~Option() = default;
};

struct ConfigurationOption final : Option {
    set_context setContext;
    get_setting getSetting;

    ConfigurationOption(std::string name, common::LogicalTypeID parameterType,
        set_context setContext, get_setting getSetting)
        : Option{std::move(name), parameterType, OptionType::CONFIGURATION,
              false /* isConfidential */},
          setContext{setContext}, getSetting{getSetting} {}
};

struct ExtensionOption final : Option {
    common::Value defaultValue;

    ExtensionOption(std::string name, common::LogicalTypeID parameterType,
        common::Value defaultValue, bool isConfidential)
        : Option{std::move(name), parameterType, OptionType::EXTENSION, isConfidential},
          defaultValue{std::move(defaultValue)} {}
};

} // namespace main
} // namespace lbug
