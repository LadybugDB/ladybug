#pragma once

#include "extension/extension.h"

namespace lbug {
namespace vector_extension {

class VectorExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "VECTOR";

public:
    static void load(main::ClientContext* context);
};
} // namespace vector_extension
} // namespace lbug
