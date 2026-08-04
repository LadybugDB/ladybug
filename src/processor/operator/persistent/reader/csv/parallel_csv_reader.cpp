#include "processor/operator/persistent/reader/csv/parallel_csv_reader.h"

#include <cmath>

#include "binder/binder.h"
#include "common/constants.h"
#include "common/file_system/virtual_file_system.h"
#include "function/table/bind_data.h"
#include "processor/execution_context.h"
#include "processor/operator/persistent/reader/csv/serial_csv_reader.h"
#include "processor/operator/persistent/reader/reader_bind_utils.h"

#if defined(_WIN32)
#include <format>
#include <io.h>
#endif

#include "common/system_message.h"
#include "function/table/table_function.h"
#include "processor/operator/persistent/reader/csv/driver.h"

using namespace lbug::common;
using namespace lbug::function;

namespace lbug {
namespace processor {

ParallelCSVReader::ParallelCSVReader(const std::string& filePath, idx_t fileIdx, CSVOption option,
    CSVColumnInfo columnInfo, main::ClientContext* context, LocalFileErrorHandler* errorHandler)
    : BaseCSVReader{filePath, fileIdx, std::move(option), std::move(columnInfo), context,
          errorHandler} {}

bool ParallelCSVReader::hasMoreToRead() const {
    // If we haven't started the first block yet or are done our block, get the next block.
    return buffer != nullptr && !finishedBlock();
}

uint64_t ParallelCSVReader::parseBlock(block_idx_t blockIdx, DataChunk& resultChunk) {
    parseMode = ParseMode::FIXED_BLOCK;
    currentBlockIdx = blockIdx;
    currentUnitStartsAtFileStart = blockIdx == 0;
    resetNumRowsInCurrentBlock();
    seekToBlockStart();
    if (blockIdx == 0) {
        readBOM();
        if (option.hasHeader) {
            const auto [numRowsRead, numErrors] = readHeader();
            errorHandler->setHeaderNumRows(numRowsRead + numErrors);
        }
    }
    if (finishedBlock()) {
        return 0;
    }
    ParallelParsingDriver driver(resultChunk, this);
    const auto [numRowsRead, numErrors] = parseCSV(driver);
    increaseNumRowsInCurrentBlock(numRowsRead, numErrors);
    return numRowsRead;
}

uint64_t ParallelCSVReader::parseRange(const CSVParseRange& range, DataChunk& resultChunk) {
    parseMode = ParseMode::PLANNED_RANGE;
    currentBlockIdx = range.rangeIdx;
    currentRangeStartOffset = range.startOffset;
    currentRangeEndOffset = range.endOffset;
    currentUnitStartsAtFileStart = range.startsAtFileStart;
    resetNumRowsInCurrentBlock();
    seekToRangeStart();
    if (currentUnitStartsAtFileStart) {
        readBOM();
        if (option.hasHeader) {
            const auto [numRowsRead, numErrors] = readHeader();
            errorHandler->setHeaderNumRows(numRowsRead + numErrors);
        }
    }
    if (finishedBlock()) {
        return 0;
    }
    ParallelParsingDriver driver(resultChunk, this);
    const auto [numRowsRead, numErrors] =
        supportsStructuralParser() ? parseCSVStructural(driver) : parseCSV(driver);
    increaseNumRowsInCurrentBlock(numRowsRead, numErrors);
    return numRowsRead;
}

void ParallelCSVReader::reportFinishedBlock() {
    errorHandler->reportFinishedBlock(currentBlockIdx, getNumRowsInCurrentBlock());
}

uint64_t ParallelCSVReader::continueBlock(DataChunk& resultChunk) {
    DASSERT(hasMoreToRead());
    ParallelParsingDriver driver(resultChunk, this);
    const auto [numRowsParsed, numErrors] =
        parseMode == ParseMode::PLANNED_RANGE && supportsStructuralParser() ?
            parseCSVStructural(driver) :
            parseCSV(driver);
    increaseNumRowsInCurrentBlock(numRowsParsed, numErrors);
    return numRowsParsed;
}

void ParallelCSVReader::seekToBlockStart() {
    // Seek to the proper location in the file.
    if (fileInfo->seek(currentBlockIdx * CopyConstants::PARALLEL_BLOCK_SIZE, SEEK_SET) == -1) {
        // LCOV_EXCL_START
        handleCopyException(
            std::format("Failed to seek to block {}: {}", currentBlockIdx, posixErrMessage()),
            true);
        // LCOV_EXCL_STOP
    }
    osFileOffset = currentBlockIdx * CopyConstants::PARALLEL_BLOCK_SIZE;

    if (currentBlockIdx == 0) {
        // First block doesn't search for a newline.
        return;
    }

    // Reset the buffer.
    position = 0;
    bufferSize = 0;
    buffer.reset();
    if (!readBuffer(nullptr)) {
        return;
    }

    // Find the start of the next line.
    do {
        for (; position < bufferSize; position++) {
            if (buffer[position] == '\r') {
                position++;
                if (!maybeReadBuffer(nullptr)) {
                    return;
                }
                if (buffer[position] == '\n') {
                    position++;
                }
                return;
            } else if (buffer[position] == '\n') {
                position++;
                return;
            }
        }
    } while (readBuffer(nullptr));
}

void ParallelCSVReader::seekToRangeStart() {
    if (fileInfo->seek(currentRangeStartOffset, SEEK_SET) == -1) {
        handleCopyException(
            std::format("Failed to seek to range {}: {}", currentBlockIdx, posixErrMessage()),
            true);
    }
    osFileOffset = currentRangeStartOffset;
    position = 0;
    bufferSize = 0;
    buffer.reset();
}

bool ParallelCSVReader::handleQuotedNewline() {
    if (parseMode == ParseMode::PLANNED_RANGE) {
        return true;
    }
    lineContext.setEndOfLine(getFileOffset());
    handleCopyException("Quoted newlines are not supported in parallel CSV reader."
                        " Please specify PARALLEL=FALSE in the options.");
    return false;
}

bool ParallelCSVReader::finishedBlock() const {
    if (parseMode == ParseMode::PLANNED_RANGE) {
        return getFileOffset() >= currentRangeEndOffset;
    }
    // Only stop if we've ventured into the next block by at least a byte.
    // Use `>` because `position` points to just past the newline right now.
    return getFileOffset() > (currentBlockIdx + 1) * CopyConstants::PARALLEL_BLOCK_SIZE;
}

ParallelCSVScanSharedState::ParallelCSVScanSharedState(FileScanInfo fileScanInfo, uint64_t numRows,
    main::ClientContext* context, CSVOption csvOption, CSVColumnInfo columnInfo,
    std::vector<FileScanPlan> filePlans)
    : ScanFileWithProgressSharedState{std::move(fileScanInfo), numRows, context},
      csvOption{std::move(csvOption)}, columnInfo{std::move(columnInfo)}, completedBytes{0},
      filePlans{std::move(filePlans)} {
    errorHandlers.reserve(this->fileScanInfo.getNumFiles());
    for (idx_t i = 0; i < this->fileScanInfo.getNumFiles(); ++i) {
        errorHandlers.emplace_back(i, &mtx);
        totalSize += this->filePlans[i].fileSize;
    }
    populateErrorFunc = constructPopulateFunc();
    for (auto& errorHandler : errorHandlers) {
        errorHandler.setPopulateErrorFunc(populateErrorFunc);
    }
}

populate_func_t ParallelCSVScanSharedState::constructPopulateFunc() {
    const auto numFiles = fileScanInfo.getNumFiles();
    auto localErrorHandlers = std::vector<std::shared_ptr<LocalFileErrorHandler>>(numFiles);
    auto readers = std::vector<std::shared_ptr<SerialCSVReader>>(numFiles);
    for (idx_t i = 0; i < numFiles; ++i) {
        // If we run into errors while reconstructing lines they should be unrecoverable
        localErrorHandlers[i] =
            std::make_shared<LocalFileErrorHandler>(&errorHandlers[i], false, context);
        readers[i] = std::make_shared<SerialCSVReader>(fileScanInfo.filePaths[i], i,
            csvOption.copy(), columnInfo.copy(), context, localErrorHandlers[i].get());
    }
    return [this, movedErrorHandlers = std::move(localErrorHandlers),
               movedReaders = std::move(readers)](CopyFromFileError error,
               idx_t fileIdx) -> PopulatedCopyFromError {
        return BaseCSVReader::basePopulateErrorFunc(std::move(error), &errorHandlers[fileIdx],
            movedReaders[fileIdx].get(), fileScanInfo.getFilePath(fileIdx));
    };
}

ParallelCSVScanSharedState::ParseTask ParallelCSVScanSharedState::getNextTask() {
    std::lock_guard<std::mutex> guard{mtx};
    while (fileIdx < fileScanInfo.getNumFiles()) {
        const auto currentFileIdx = fileIdx.load();
        const auto& plan = filePlans[currentFileIdx];
        const auto unitCount = plan.usePlannedRanges ? plan.ranges.size() : plan.numFixedBlocks;
        if (blockIdx < unitCount) {
            ParseTask task;
            task.fileIdx = currentFileIdx;
            task.unitIdx = blockIdx++;
            task.usePlannedRange = plan.usePlannedRanges;
            if (task.usePlannedRange) {
                task.range = plan.ranges[task.unitIdx];
                task.byteSize = task.range.endOffset - task.range.startOffset;
            } else {
                const auto startOffset = task.unitIdx * CopyConstants::PARALLEL_BLOCK_SIZE;
                const auto endOffset = std::min<uint64_t>(plan.fileSize,
                    startOffset + CopyConstants::PARALLEL_BLOCK_SIZE);
                task.byteSize = endOffset > startOffset ? endOffset - startOffset : 0;
            }
            return task;
        }
        blockIdx = 0;
        fileIdx++;
    }
    return {};
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto& outputChunk = output.dataChunk;

    auto localState = input.localState->ptrCast<ParallelCSVLocalState>();
    auto sharedState = input.sharedState->ptrCast<ParallelCSVScanSharedState>();

    do {
        if (localState->reader != nullptr) {
            if (localState->reader->hasMoreToRead()) {
                auto result = localState->reader->continueBlock(outputChunk);
                outputChunk.state->getSelVectorUnsafe().setSelSize(result);
                if (result > 0) {
                    return result;
                }
            }
            localState->reader->reportFinishedBlock();
            sharedState->completedBytes += localState->currentTaskBytes;
            localState->currentTaskBytes = 0;
        }
        auto task = sharedState->getNextTask();
        if (task.fileIdx == INVALID_IDX) {
            return 0;
        }
        const auto fileIdx = task.fileIdx;
        localState->currentTaskBytes = task.byteSize;
        if (fileIdx != localState->fileIdx || localState->reader == nullptr) {
            localState->fileIdx = fileIdx;
            localState->errorHandler =
                std::make_unique<LocalFileErrorHandler>(&sharedState->errorHandlers[fileIdx],
                    sharedState->csvOption.ignoreErrors, sharedState->context, true);
            localState->reader =
                std::make_unique<ParallelCSVReader>(sharedState->fileScanInfo.filePaths[fileIdx],
                    fileIdx, sharedState->csvOption.copy(), sharedState->columnInfo.copy(),
                    sharedState->context, localState->errorHandler.get());
        }
        auto numRowsRead = task.usePlannedRange ?
                               localState->reader->parseRange(task.range, outputChunk) :
                               localState->reader->parseBlock(task.unitIdx, outputChunk);

        // if there are any pending errors to throw, stop the parsing
        // the actual error will be thrown during finalize
        if (!sharedState->csvOption.ignoreErrors &&
            sharedState->errorHandlers[fileIdx].getNumCachedErrors() > 0) {
            numRowsRead = 0;
        }

        outputChunk.state->getSelVectorUnsafe().setSelSize(numRowsRead);
        if (numRowsRead > 0) {
            return numRowsRead;
        }
        if (localState->reader->isEOF()) {
            localState->reader->reportFinishedBlock();
            sharedState->completedBytes += localState->currentTaskBytes;
            localState->currentTaskBytes = 0;
            localState->errorHandler->finalize();
            localState->reader = nullptr;
            localState->errorHandler = nullptr;
        }
    } while (true);
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto scanInput = dynamic_cast_checked<ExtraScanTableFuncBindInput*>(input->extraInput.get());
    bool detectedHeader = false;

    DialectOption detectedDialect;
    auto csvOption = CSVReaderConfig::construct(scanInput->fileScanInfo.options).option;
    detectedDialect.doDialectDetection = csvOption.autoDetection;

    std::vector<std::string> detectedColumnNames;
    std::vector<LogicalType> detectedColumnTypes;
    SerialCSVScan::bindColumns(scanInput, detectedColumnNames, detectedColumnTypes, detectedDialect,
        detectedHeader, context);

    std::vector<std::string> resultColumnNames;
    std::vector<LogicalType> resultColumnTypes;
    ReaderBindUtils::resolveColumns(scanInput->expectedColumnNames, detectedColumnNames,
        resultColumnNames, scanInput->expectedColumnTypes, detectedColumnTypes, resultColumnTypes);

    if (csvOption.autoDetection) {
        std::string quote(1, detectedDialect.quoteChar);
        std::string delim(1, detectedDialect.delimiter);
        std::string escape(1, detectedDialect.escapeChar);
        scanInput->fileScanInfo.options.insert_or_assign("ESCAPE",
            Value(LogicalType::STRING(), escape));
        scanInput->fileScanInfo.options.insert_or_assign("QUOTE",
            Value(LogicalType::STRING(), quote));
        scanInput->fileScanInfo.options.insert_or_assign("DELIM",
            Value(LogicalType::STRING(), delim));
    }

    if (!csvOption.setHeader && csvOption.autoDetection && detectedHeader) {
        scanInput->fileScanInfo.options.insert_or_assign("HEADER", Value(detectedHeader));
    }

    resultColumnNames =
        TableFunction::extractYieldVariables(resultColumnNames, input->yieldVariables);
    auto resultColumns = input->binder->createVariables(resultColumnNames, resultColumnTypes);
    std::vector<std::string> warningColumnNames;
    std::vector<LogicalType> warningColumnTypes;
    const column_id_t numWarningDataColumns = BaseCSVReader::appendWarningDataColumns(
        warningColumnNames, warningColumnTypes, scanInput->fileScanInfo);
    auto warningColumns =
        input->binder->createInvisibleVariables(warningColumnNames, warningColumnTypes);
    for (auto& column : warningColumns) {
        resultColumns.push_back(column);
    }
    return std::make_unique<ScanFileBindData>(std::move(resultColumns), 0 /* numRows */,
        scanInput->fileScanInfo.copy(), context, numWarningDataColumns);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<ScanFileBindData>();
    auto csvConfig = CSVReaderConfig::construct(bindData->fileScanInfo.options);
    auto csvOption = csvConfig.option.copy();
    auto columnInfo = CSVColumnInfo(bindData->getNumColumns() - bindData->numWarningDataColumns,
        bindData->getColumnSkips(), bindData->numWarningDataColumns);
    std::vector<ParallelCSVScanSharedState::FileScanPlan> filePlans;
    filePlans.reserve(bindData->fileScanInfo.getNumFiles());
    for (idx_t i = 0; i < bindData->fileScanInfo.getNumFiles(); ++i) {
        auto filePath = bindData->fileScanInfo.filePaths[i];
        ParallelCSVScanSharedState::FileScanPlan plan;
        if (csvConfig.multilineParallel) {
            auto scanResult = CSVBoundaryScanner::planFixedChunkOverlap(filePath, i,
                csvOption.copy(), bindData->context);
            plan.fileSize = scanResult.fileSize;
            plan.usePlannedRanges = scanResult.usePlannedRanges;
            if (plan.usePlannedRanges) {
                plan.ranges = std::move(scanResult.ranges);
            }
        } else {
            auto fileInfo = VirtualFileSystem::GetUnsafe(*bindData->context)
                                ->openFile(filePath,
                                    FileOpenFlags(FileFlags::READ_ONLY
#ifdef _WIN32
                                                  | FileFlags::BINARY
#endif
                                        ),
                                    bindData->context);
            plan.fileSize = fileInfo->getFileSize();
        }
        if (!plan.usePlannedRanges && plan.fileSize > 0) {
            plan.numFixedBlocks = (plan.fileSize + CopyConstants::PARALLEL_BLOCK_SIZE - 1) /
                                  CopyConstants::PARALLEL_BLOCK_SIZE;
        }
        filePlans.push_back(std::move(plan));
    }
    return std::make_unique<ParallelCSVScanSharedState>(bindData->fileScanInfo.copy(),
        0 /* numRows */, bindData->context, csvOption.copy(), columnInfo.copy(),
        std::move(filePlans));
}

static std::unique_ptr<TableFuncLocalState> initLocalState(const TableFuncInitLocalStateInput&) {
    auto localState = std::make_unique<ParallelCSVLocalState>();
    localState->fileIdx = std::numeric_limits<decltype(localState->fileIdx)>::max();
    return localState;
}

static double progressFunc(TableFuncSharedState* sharedState) {
    auto state = sharedState->ptrCast<ParallelCSVScanSharedState>();
    if (state->fileIdx >= state->fileScanInfo.getNumFiles()) {
        return 1.0;
    }
    if (state->totalSize == 0) {
        return 0.0;
    }
    uint64_t totalReadSize = state->completedBytes;
    if (totalReadSize > state->totalSize) {
        return 1.0;
    }
    return static_cast<double>(totalReadSize) / state->totalSize;
}

static void finalizeFunc(const ExecutionContext* ctx, TableFuncSharedState* sharedState) {
    auto state = dynamic_cast_checked<ParallelCSVScanSharedState*>(sharedState);
    for (idx_t i = 0; i < state->fileScanInfo.getNumFiles(); ++i) {
        state->errorHandlers[i].throwCachedErrorsIfNeeded();
    }
    WarningContext::Get(*ctx->clientContext)
        ->populateWarnings(ctx->queryID, state->populateErrorFunc, BaseCSVReader::getFileIdxFunc);
}

function_set ParallelCSVScan::getFunctionSet() {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::STRING});
    function->tableFunc = tableFunc;
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = initSharedState;
    function->initLocalStateFunc = initLocalState;
    function->progressFunc = progressFunc;
    function->finalizeFunc = finalizeFunc;
    functionSet.push_back(std::move(function));
    return functionSet;
}

} // namespace processor
} // namespace lbug
