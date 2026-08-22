#pragma once

#include "function/function.h"

namespace lbug {
namespace function {

struct NodesFunction {
    static constexpr const char* name = "NODES";

    static function_set getFunctionSet();
};

struct RelsFunction {
    static constexpr const char* name = "RELS";

    static function_set getFunctionSet();
};

struct RelationshipsFunction {
    using alias = RelsFunction;

    static constexpr const char* name = "RELATIONSHIPS";
};

struct PropertiesBindData : public FunctionBindData {
    common::idx_t childIdx;
    std::vector<common::struct_field_idx_t> fieldIndices;

    PropertiesBindData(common::LogicalType dataType, common::idx_t childIdx)
        : FunctionBindData{std::move(dataType)}, childIdx{childIdx} {}

    PropertiesBindData(common::LogicalType dataType,
        std::vector<common::struct_field_idx_t> fieldIndices)
        : FunctionBindData{std::move(dataType)}, childIdx{0},
          fieldIndices{std::move(fieldIndices)} {}

    inline std::unique_ptr<FunctionBindData> copy() const override {
        if (!fieldIndices.empty()) {
            return std::make_unique<PropertiesBindData>(resultType.copy(), fieldIndices);
        }
        return std::make_unique<PropertiesBindData>(resultType.copy(), childIdx);
    }
};

struct PropertiesFunction {
    static constexpr const char* name = "PROPERTIES";

    static function_set getFunctionSet();
};

struct IsTrailFunction {
    static constexpr const char* name = "IS_TRAIL";

    static function_set getFunctionSet();
};

struct IsACyclicFunction {
    static constexpr const char* name = "IS_ACYCLIC";

    static function_set getFunctionSet();
};

struct LengthFunction {
    static constexpr const char* name = "LENGTH";

    static function_set getFunctionSet();
};

} // namespace function
} // namespace lbug
