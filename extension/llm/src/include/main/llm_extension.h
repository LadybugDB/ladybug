#pragma once

#include "extension/extension.h"

namespace lbug {
namespace llm_extension {

class LlmExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "LLM";

public:
    static void load(main::ClientContext* context);
};
} // namespace llm_extension
} // namespace lbug
