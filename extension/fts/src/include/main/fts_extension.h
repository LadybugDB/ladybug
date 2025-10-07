#pragma once

#include "extension/extension.h"

namespace lbug {
namespace fts_extension {

class FtsExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "FTS";

public:
    static void load(main::ClientContext* context);
};

} // namespace fts_extension
} // namespace lbug
