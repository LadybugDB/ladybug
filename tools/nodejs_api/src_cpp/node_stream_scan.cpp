#include "include/node_stream_scan.h"
#include "include/node_scan_replacement.h"

#include "common/constants.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "processor/result/factorized_table.h"

using namespace lbug::common;
using namespace lbug::function;

namespace lbug {

static std::unique_ptr<TableFuncBindData> bindFunc(lbug::main::ClientContext*,
    const TableFuncBindInput* input) {
    auto* statePtr = reinterpret_cast<NodeStreamSourceState*>(input->getLiteralVal<uint8_t*>(0));
    KU_ASSERT(statePtr != nullptr);
    std::shared_ptr<NodeStreamSourceState> state(statePtr, [](NodeStreamSourceState*) {});
    auto* extra = input->extraInput ? input->extraInput->constPtrCast<ExtraScanTableFuncBindInput>()
                                     : nullptr;
    auto columns = input->binder->createVariables(state->columnNames, state->columnTypes);
    return std::make_unique<NodeStreamScanFunctionData>(std::move(columns), state);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput&) {
    return std::make_unique<TableFuncSharedState>(0);
}

static std::unique_ptr<TableFuncLocalState> initLocalState(
    const TableFuncInitLocalStateInput&) {
    return std::make_unique<TableFuncLocalState>();
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto* bindData = input.bindData->constPtrCast<NodeStreamScanFunctionData>();
    auto& state = *bindData->state;
    const uint64_t requestId = NodeStreamRegistry::nextRequestId();
    auto req = std::make_unique<NodeStreamChunkRequest>();
    NodeStreamRegistry::setChunkRequest(requestId, std::move(req));
    NodeStreamChunkRequest* reqPtr = NodeStreamRegistry::getChunkRequest(requestId);
    KU_ASSERT(reqPtr != nullptr);

    state.getChunkTsf.BlockingCall(requestId,
        [](Napi::Env env, Napi::Function jsCallback, uint64_t id) {
            jsCallback.Call({Napi::Number::New(env, static_cast<double>(id))});
        });

    std::vector<std::vector<Value>> rowsCopy;
    {
        std::unique_lock<std::mutex> lock(reqPtr->mtx);
        reqPtr->cv.wait(lock, [reqPtr] { return reqPtr->filled; });
        rowsCopy = reqPtr->rows;
    }
    NodeStreamRegistry::setChunkRequest(requestId, nullptr);

    const offset_t numRows = static_cast<offset_t>(rowsCopy.size());
    if (numRows == 0) {
        return 0;
    }

    const auto numCols = bindData->getNumColumns();
    const auto cap = std::min(numRows, static_cast<offset_t>(DEFAULT_VECTOR_CAPACITY));
    for (offset_t r = 0; r < cap; r++) {
        for (auto c = 0u; c < numCols; c++) {
            auto& vec = output.dataChunk.getValueVectorMutable(c);
            if (c < rowsCopy[r].size() && !rowsCopy[r][c].isNull()) {
                vec.setNull(r, false);
                vec.copyFromValue(r, rowsCopy[r][c]);
            } else {
                vec.setNull(r, true);
            }
        }
    }
    output.dataChunk.state->getSelVectorUnsafe().setSelSize(cap);
    return cap;
}

static double progressFunc(TableFuncSharedState*) {
    return 0.0;
}

TableFunction NodeStreamScanFunction::getFunction() {
    TableFunction func(name, std::vector<LogicalTypeID>{LogicalTypeID::POINTER});
    func.tableFunc = tableFunc;
    func.bindFunc = bindFunc;
    func.initSharedStateFunc = initSharedState;
    func.initLocalStateFunc = initLocalState;
    func.progressFunc = progressFunc;
    return func;
}

} // namespace lbug
