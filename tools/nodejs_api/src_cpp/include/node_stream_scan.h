#pragma once

#include "function/table/bind_data.h"
#include "function/table/table_function.h"

struct NodeStreamSourceState; // defined in node_scan_replacement.h

struct NodeStreamScanFunctionData : lbug::function::TableFuncBindData {
    std::shared_ptr<NodeStreamSourceState> state;

    NodeStreamScanFunctionData(lbug::binder::expression_vector columns,
        std::shared_ptr<NodeStreamSourceState> state)
        : TableFuncBindData(std::move(columns), 0), state(std::move(state)) {}

    std::unique_ptr<lbug::function::TableFuncBindData> copy() const override {
        return std::make_unique<NodeStreamScanFunctionData>(columns, state);
    }
};

namespace lbug {
namespace function {

struct NodeStreamScanFunction {
    static constexpr const char* name = "NODE_STREAM_SCAN";
    static TableFunction getFunction();
};

} // namespace function
} // namespace lbug
