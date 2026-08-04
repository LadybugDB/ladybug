#pragma once

#include "base_csv_reader.h"
#include "common/types/types.h"
#include "function/function.h"
#include "function/table/bind_input.h"
#include "function/table/scan_file_function.h"
#include "function/table/table_function.h"
#include "processor/operator/persistent/reader/csv/csv_boundary_scanner.h"
#include "processor/operator/persistent/reader/file_error_handler.h"

namespace lbug {
namespace processor {

//! ParallelCSVReader is a class that reads values from a stream in parallel.
class ParallelCSVReader final : public BaseCSVReader {
    friend class ParallelParsingDriver;

public:
    enum class ParseMode : uint8_t { FIXED_BLOCK, PLANNED_RANGE };

    ParallelCSVReader(const std::string& filePath, common::idx_t fileIdx, common::CSVOption option,
        CSVColumnInfo columnInfo, main::ClientContext* context,
        LocalFileErrorHandler* errorHandler);

    bool hasMoreToRead() const;
    uint64_t parseBlock(common::block_idx_t blockIdx, common::DataChunk& resultChunk) override;
    uint64_t parseRange(const CSVParseRange& range, common::DataChunk& resultChunk);
    uint64_t continueBlock(common::DataChunk& resultChunk);

    void reportFinishedBlock();

protected:
    bool handleQuotedNewline() override;

private:
    bool finishedBlock() const;
    void seekToBlockStart();
    void seekToRangeStart();

private:
    ParseMode parseMode = ParseMode::FIXED_BLOCK;
    uint64_t currentRangeStartOffset = 0;
    uint64_t currentRangeEndOffset = 0;
    bool currentUnitStartsAtFileStart = false;
};

struct ParallelCSVLocalState final : public function::TableFuncLocalState {
    std::unique_ptr<ParallelCSVReader> reader;
    std::unique_ptr<LocalFileErrorHandler> errorHandler;
    common::idx_t fileIdx = common::INVALID_IDX;
    uint64_t currentTaskBytes = 0;
};

struct ParallelCSVScanSharedState final : public function::ScanFileWithProgressSharedState {
    struct FileScanPlan {
        uint64_t fileSize = 0;
        uint64_t numFixedBlocks = 0;
        bool usePlannedRanges = false;
        std::vector<CSVParseRange> ranges;
    };

    struct ParseTask {
        common::idx_t fileIdx = common::INVALID_IDX;
        common::block_idx_t unitIdx = 0;
        uint64_t byteSize = 0;
        bool usePlannedRange = false;
        CSVParseRange range{};
    };

    common::CSVOption csvOption;
    CSVColumnInfo columnInfo;
    std::atomic<uint64_t> completedBytes = 0;
    std::vector<SharedFileErrorHandler> errorHandlers;
    populate_func_t populateErrorFunc;
    std::vector<FileScanPlan> filePlans;

    ParallelCSVScanSharedState(common::FileScanInfo fileScanInfo, uint64_t numRows,
        main::ClientContext* context, common::CSVOption csvOption, CSVColumnInfo columnInfo,
        std::vector<FileScanPlan> filePlans);

    ParseTask getNextTask();
    populate_func_t constructPopulateFunc();
};

struct ParallelCSVScan {
    static constexpr const char* name = "READ_CSV_PARALLEL";

    static function::function_set getFunctionSet();
};

} // namespace processor
} // namespace lbug
