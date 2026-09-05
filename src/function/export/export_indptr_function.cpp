#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "common/data_chunk/data_chunk.h"
#include "common/types/types.h"
#include "function/export/export_function.h"
#include "main/client_context.h"
#include "processor/operator/persistent/writer/parquet/parquet_writer.h"
#include "processor/result/factorized_table.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace function {

struct IndptrExportBindData : public ExportFuncBindData {
    IndptrExportBindData(std::vector<std::string> names, std::string fileName)
        : ExportFuncBindData{std::move(names), std::move(fileName)} {}

    std::unique_ptr<ExportFuncBindData> copy() const override {
        auto bindData = std::make_unique<IndptrExportBindData>(columnNames, fileName);
        bindData->types = LogicalType::copy(types);
        return bindData;
    }
};

// The degree query returns one (src OFFSET, degree) row per source node. Sink order
// across threads is undefined, so we collect raw pairs and sort in finalize.
struct IndptrExportLocalState final : public ExportFuncLocalState {
    std::vector<std::pair<int64_t, int64_t>> pairs;
};

struct IndptrExportSharedState final : public ExportFuncSharedState {
    std::vector<std::pair<int64_t, int64_t>> pairs;
    std::mutex mtx;
    std::string fileName;
    main::ClientContext* context = nullptr;

    void init(main::ClientContext& context_, const ExportFuncBindData& bindData) override {
        context = &context_;
        fileName = bindData.fileName;
    }
};

static std::unique_ptr<ExportFuncBindData> bindFunc(ExportFuncBindInput& bindInput) {
    return std::make_unique<IndptrExportBindData>(bindInput.columnNames, bindInput.filePath);
}

static std::unique_ptr<ExportFuncLocalState> initLocalStateFunc(main::ClientContext& /*context*/,
    const ExportFuncBindData& /*bindData*/, std::vector<bool> /*isFlatVec*/) {
    return std::make_unique<IndptrExportLocalState>();
}

static std::shared_ptr<ExportFuncSharedState> createSharedStateFunc() {
    return std::make_shared<IndptrExportSharedState>();
}

static void initSharedStateFunc(ExportFuncSharedState& sharedState, main::ClientContext& context,
    const ExportFuncBindData& bindData) {
    sharedState.init(context, bindData);
}

static void sinkFunc(ExportFuncSharedState& /*sharedState*/, ExportFuncLocalState& localState,
    const ExportFuncBindData& /*bindData*/,
    std::vector<std::shared_ptr<ValueVector>> inputVectors) {
    if (inputVectors.size() < 2) {
        return;
    }
    auto& local = localState.cast<IndptrExportLocalState>();
    auto* srcVec = inputVectors[0].get();
    auto* degVec = inputVectors[1].get();
    if (srcVec->state->isFlat()) {
        auto pos = srcVec->state->getSelVector()[0];
        local.pairs.emplace_back(srcVec->getValue<int64_t>(pos), degVec->getValue<int64_t>(pos));
        return;
    }
    auto& sel = srcVec->state->getSelVector();
    for (auto i = 0u; i < sel.getSelSize(); i++) {
        auto pos = sel[i];
        local.pairs.emplace_back(srcVec->getValue<int64_t>(pos), degVec->getValue<int64_t>(pos));
    }
}

static void combineFunc(ExportFuncSharedState& sharedState, ExportFuncLocalState& localState) {
    auto& shared = sharedState.cast<IndptrExportSharedState>();
    auto& local = localState.cast<IndptrExportLocalState>();
    std::lock_guard lock(shared.mtx);
    shared.pairs.insert(shared.pairs.end(), local.pairs.begin(), local.pairs.end());
}

static void finalizeFunc(ExportFuncSharedState& sharedState) {
    auto& shared = sharedState.cast<IndptrExportSharedState>();
    std::sort(shared.pairs.begin(), shared.pairs.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    // Prefix-sum degrees into N+1 ptr offsets.
    std::vector<int64_t> ptr;
    ptr.reserve(shared.pairs.size() + 1);
    ptr.push_back(0);
    for (auto& [_, degree] : shared.pairs) {
        ptr.push_back(ptr.back() + degree);
    }
    auto mm = MemoryManager::Get(*shared.context);
    // Unflat column: the parquet writer sizes each write batch from the unflat chunk,
    // so a flat column would emit a single row.
    auto tableSchema = FactorizedTableSchema();
    tableSchema.appendColumn(
        ColumnSchema(true, 1 /* dummyGroupPos */, (uint32_t)sizeof(overflow_value_t)));
    auto outFT = FactorizedTable(mm, tableSchema.copy());
    auto vec = std::make_shared<ValueVector>(LogicalType::INT64(), mm);
    vec->setState(std::make_shared<DataChunkState>());
    for (auto i = 0u; i < ptr.size(); i++) {
        vec->setValue<int64_t>(i, ptr[i]);
    }
    vec->state->getSelVectorUnsafe().setToUnfiltered(ptr.size());
    outFT.append({vec.get()});
    std::vector<LogicalType> ptrTypes;
    ptrTypes.push_back(LogicalType::INT64());
    auto writer = std::make_unique<ParquetWriter>(shared.fileName, std::move(ptrTypes),
        std::vector<std::string>{"ptr"}, lbug_parquet::format::CompressionCodec::SNAPPY,
        shared.context);
    writer->flush(outFT);
    writer->finalize();
}

function_set IndptrExportFunction::getFunctionSet() {
    function_set functionSet;
    auto exportFunc = std::make_unique<ExportFunction>(name);
    exportFunc->initLocalState = initLocalStateFunc;
    exportFunc->createSharedState = createSharedStateFunc;
    exportFunc->initSharedState = initSharedStateFunc;
    exportFunc->sink = sinkFunc;
    exportFunc->combine = combineFunc;
    exportFunc->finalize = finalizeFunc;
    exportFunc->bind = bindFunc;
    functionSet.push_back(std::move(exportFunc));
    return functionSet;
}

} // namespace function
} // namespace lbug
