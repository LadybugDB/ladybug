#pragma once

#include "extension/extension.h"

namespace lbug {
namespace algo_extension {

class AlgoExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "ALGO";

public:
    static void load(main::ClientContext* context);
};

} // namespace algo_extension
} // namespace lbug
