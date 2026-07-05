#include "processor/operator/persistent/reader/csv/base_csv_reader.h"

#include <vector>

#include "common/file_system/virtual_file_system.h"
#include "common/string_utils.h"
#include "common/system_message.h"
#include "common/utils.h"
#include "main/client_context.h"
#include "processor/operator/persistent/reader/csv/driver.h"
#include "processor/operator/persistent/reader/file_error_handler.h"
#include "utf8proc_wrapper.h"
#include <format>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

using namespace lbug::common;

namespace lbug {
namespace processor {

// TODO(Royi) for performance reasons we may want to reduce the number of fields here since each
// field is essentially an extra column during copy
struct CSVWarningSourceData {
    CSVWarningSourceData() = default;
    static CSVWarningSourceData constructFrom(const processor::WarningSourceData& warningData);

    uint64_t startByteOffset;
    uint64_t endByteOffset;
    uint64_t blockIdx;
    uint32_t offsetInBlock;
    common::idx_t fileIdx;
};

CSVWarningSourceData CSVWarningSourceData::constructFrom(
    const processor::WarningSourceData& warningData) {
    DASSERT(warningData.numValues == CopyConstants::CSV_WARNING_DATA_NUM_COLUMNS);

    CSVWarningSourceData ret{};
    warningData.dumpTo(ret.blockIdx, ret.offsetInBlock, ret.startByteOffset, ret.endByteOffset,
        ret.fileIdx);
    return ret;
}

BaseCSVReader::BaseCSVReader(const std::string& filePath, common::idx_t fileIdx,
    common::CSVOption option, CSVColumnInfo columnInfo, main::ClientContext* context,
    LocalFileErrorHandler* errorHandler)
    : context{context}, option{std::move(option)}, columnInfo{std::move(columnInfo)},
      currentBlockIdx(0), numRowsInCurrentBlock(0), curRowIdx(0), numErrors(0), buffer{nullptr},
      bufferIdx(0), bufferSize{0}, position{0}, lineContext(), osFileOffset{0}, fileIdx(fileIdx),
      errorHandler(errorHandler), rowEmpty{false} {
    fileInfo = VirtualFileSystem::GetUnsafe(*context)->openFile(filePath,
        FileOpenFlags(FileFlags::READ_ONLY
#ifdef _WIN32
                      | FileFlags::BINARY
#endif
            ),
        context);
}

bool BaseCSVReader::isEOF() const {
    return getFileOffset() >= fileInfo->getFileSize();
}

uint64_t BaseCSVReader::getFileSize() {
    return fileInfo->getFileSize();
}

template<typename Driver>
bool BaseCSVReader::addValue(Driver& driver, uint64_t rowNum, column_id_t columnIdx,
    std::string_view strVal, std::vector<uint64_t>& escapePositions) {
    std::string valueToAdd;
    // insert the line number into the chunk
    if (!escapePositions.empty()) {
        // remove escape characters (if any)
        std::string newVal = "";
        uint64_t prevPos = 0;
        for (auto i = 0u; i < escapePositions.size(); i++) {
            auto nextPos = escapePositions[i];
            newVal += strVal.substr(prevPos, nextPos - prevPos);
            prevPos = nextPos + 1;
        }
        newVal += strVal.substr(prevPos, strVal.size() - prevPos);
        escapePositions.clear();
        valueToAdd = newVal;
    } else {
        valueToAdd = strVal;
    }
    if (!utf8proc::Utf8Proc::isValid(valueToAdd.data(), valueToAdd.length())) {
        handleCopyException("Invalid UTF8-encoded string.", true /* mustThrow */);
    }
    return driver.addValue(rowNum, columnIdx, valueToAdd);
}

struct SkipRowDriver {
    DriverType driverType = DriverType::SKIP_ROW;
    explicit SkipRowDriver(uint64_t skipNum) : skipNum{skipNum} {}
    bool done(uint64_t rowNum) const { return rowNum >= skipNum; }
    bool addRow(uint64_t, column_id_t, std::optional<WarningDataWithColumnInfo>) { return true; }
    bool addValue(uint64_t, column_id_t, std::string_view) { return true; }

    uint64_t skipNum;
};

BaseCSVReader::parse_result_t BaseCSVReader::handleFirstBlock() {
    uint64_t numRowsRead = 0;
    uint64_t numErrors = 0;
    readBOM();
    if (option.skipNum > 0) {
        SkipRowDriver driver{option.skipNum};
        const auto parseResult = parseCSV(driver);
        numRowsRead += parseResult.first;
        numErrors += parseResult.second;
    }
    if (option.hasHeader) {
        const auto parseResult = readHeader();
        numRowsRead += parseResult.first;
        numErrors += parseResult.second;
    }
    return {numRowsRead, numErrors};
}

void BaseCSVReader::readBOM() {
    if (!maybeReadBuffer(nullptr)) {
        return;
    }
    if (bufferSize >= 3 && buffer[0] == '\xEF' && buffer[1] == '\xBB' && buffer[2] == '\xBF') {
        position = 3;
    }
}

// Dummy driver that just skips a row.
struct HeaderDriver {
    DriverType driverType = DriverType::HEADER;
    bool done(uint64_t) { return true; }
    bool addRow(uint64_t, column_id_t, std::optional<WarningDataWithColumnInfo>) { return true; }
    bool addValue(uint64_t, column_id_t, std::string_view) { return true; }
};

void BaseCSVReader::resetNumRowsInCurrentBlock() {
    numRowsInCurrentBlock = 0;
}

void BaseCSVReader::increaseNumRowsInCurrentBlock(uint64_t numRows, uint64_t numErrors) {
    numRowsInCurrentBlock += numRows + numErrors;
}

uint64_t BaseCSVReader::getNumRowsInCurrentBlock() const {
    return numRowsInCurrentBlock;
}

uint32_t BaseCSVReader::getRowOffsetInCurrentBlock() const {
    return safeIntegerConversion<uint32_t>(numRowsInCurrentBlock + curRowIdx + numErrors);
}

BaseCSVReader::parse_result_t BaseCSVReader::readHeader() {
    HeaderDriver driver;
    return parseCSV(driver);
}

bool BaseCSVReader::readBuffer(uint64_t* start) {
    std::unique_ptr<char[]> oldBuffer = std::move(buffer);

    // the remaining part of the last buffer
    uint64_t remaining = 0;
    if (start != nullptr) {
        DASSERT(*start <= bufferSize);
        remaining = bufferSize - *start;
    }

    uint64_t bufferReadSize = CopyConstants::INITIAL_BUFFER_SIZE;
    while (remaining > bufferReadSize) {
        bufferReadSize *= 2;
    }

    buffer = std::unique_ptr<char[]>(new char[bufferReadSize + remaining + 1]());
    if (remaining > 0) {
        // remaining from last buffer: copy it here
        DASSERT(start != nullptr);
        memcpy(buffer.get(), oldBuffer.get() + *start, remaining);
    }
    auto readCount = fileInfo->readFile(buffer.get() + remaining, bufferReadSize);
    if (readCount == -1) {
        // LCOV_EXCL_START
        lineContext.setEndOfLine(getFileOffset());
        handleCopyException(std::format("Could not read from file: {}", posixErrMessage()), true);
        // LCOV_EXCL_STOP
    }

    // Update buffer size in a way so that the invariant osFileOffset >= bufferSize is never broken
    // This is needed because in the serial CSV reader the progressFunc can call getFileOffset from
    // a different thread
    bufferSize = remaining;
    osFileOffset += readCount;
    bufferSize += readCount;

    buffer[bufferSize] = '\0';
    if (start != nullptr) {
        *start = 0;
    }
    position = remaining;
    ++bufferIdx;
    return readCount > 0;
}

std::string BaseCSVReader::reconstructLine(uint64_t startPosition, uint64_t endPosition,
    bool completeLine) {
    DASSERT(endPosition >= startPosition);

    std::string res;
    // For cases where we cannot perform a seek (e.g. compressed file system) we just return an
    // empty string
    if (fileInfo->canPerformSeek()) {
        res.resize(endPosition - startPosition);
        fileInfo->readFromFile(res.data(), res.size(), startPosition);

        const char* incompleteLineSuffix = completeLine ? "" : "...";
        res += incompleteLineSuffix;
    }

    return StringUtils::ltrimNewlines(StringUtils::rtrimNewlines(res));
}

void BaseCSVReader::skipCurrentLine() {
    do {
        for (; position < bufferSize; ++position) {
            if (isNewLine(buffer[position])) {
                while (position < bufferSize && isNewLine(buffer[position])) {
                    ++position;
                }
                return;
            }
        }
    } while (maybeReadBuffer(nullptr));
}

void BaseCSVReader::handleCopyException(const std::string& message, bool mustThrow) {
    auto endByteOffset = lineContext.endByteOffset;
    if (!lineContext.isCompleteLine) {
        endByteOffset = getFileOffset();
    }
    CopyFromFileError error{message,
        WarningSourceData::constructFrom(currentBlockIdx, getRowOffsetInCurrentBlock(),
            lineContext.startByteOffset, endByteOffset, fileIdx),
        lineContext.isCompleteLine, mustThrow};
    errorHandler->handleError(error);

    // if we reach here it means we are ignoring the error
    ++numErrors;
}

template<typename Driver>
static std::optional<WarningDataWithColumnInfo> getOptionalWarningData(
    const CSVColumnInfo& columnInfo, const CSVOption& option,
    WarningSourceData&& warningSourceData) {
    std::optional<WarningDataWithColumnInfo> warningData;

    // we only care about populating the extra warning data when actually parsing the CSV
    // and not when performing actions like sniffing
    if constexpr (std::is_same_v<Driver, ParallelParsingDriver> ||
                  std::is_same_v<Driver, SerialParsingDriver>) {
        // For now we only populate extra warning data when IGNORE_ERRORS is enabled
        if (option.ignoreErrors) {
            DASSERT(
                columnInfo.numWarningDataColumns == CopyConstants::CSV_WARNING_DATA_NUM_COLUMNS);
            warningData.emplace(warningSourceData, columnInfo.numColumns);
        }
    }
    return warningData;
}

WarningSourceData BaseCSVReader::getWarningSourceData() const {
    return WarningSourceData::constructFrom(currentBlockIdx, getRowOffsetInCurrentBlock(),
        lineContext.startByteOffset, lineContext.endByteOffset, fileIdx);
}

template<typename Driver>
BaseCSVReader::parse_result_t BaseCSVReader::parseCSV(Driver& driver) {
    DASSERT(nullptr != errorHandler);

    // used for parsing algorithm
    curRowIdx = 0;
    numErrors = 0;

    while (true) {
        column_id_t column = 0;
        auto start = position.load();
        bool hasQuotes = false;
        std::vector<uint64_t> escapePositions;
        lineContext.setNewLine(getFileOffset());

        // read values into the buffer (if any)
        if (!maybeReadBuffer(&start)) {
            return {curRowIdx, numErrors};
        }

        // start parsing the first value
        goto value_start;
    value_start:
        // state: value_start
        // this state parses the first character of a value
        if (buffer[position] == option.quoteChar) {
            [[unlikely]]
            // quote: actual value starts in the next position
            // move to in_quotes state
            start = position + 1;
            hasQuotes = true;
            goto in_quotes;
        } else {
            // no quote, move to normal parsing state
            start = position;
            hasQuotes = false;
            goto normal;
        }
    normal:
        // state: normal parsing state
        // this state parses the remainder of a non-quoted value until we reach a delimiter or
        // newline
        do {
            for (; position < bufferSize; position++) {
                if (buffer[position] == option.delimiter) {
                    // delimiter: end the value and add it to the chunk
                    goto add_value;
                } else if (isNewLine(buffer[position])) {
                    // newline: add row
                    goto add_row;
                }
            }
        } while (readBuffer(&start));

        [[unlikely]]
        // file ends during normal scan: go to end state
        goto final_state;
    add_value:
        // We get here after we have a delimiter.
        DASSERT(buffer[position] == option.delimiter ||
                buffer[position] == CopyConstants::DEFAULT_CSV_LIST_END_CHAR);
        // Trim one character if we have quotes.
        if (!addValue(driver, curRowIdx, column,
                std::string_view(buffer.get() + start, position - start - hasQuotes),
                escapePositions)) {
            goto ignore_error;
        }
        column++;

        // Move past the delimiter.
        ++position;
        // Adjust start for MaybeReadBuffer.
        start = position;
        if (!maybeReadBuffer(&start)) {
            [[unlikely]]
            // File ends right after delimiter, go to final state
            goto final_state;
        }
        goto value_start;
    add_row: {
        // We get here after we have a newline.
        DASSERT(isNewLine(buffer[position]));
        lineContext.setEndOfLine(getFileOffset());
        bool isCarriageReturn = buffer[position] == '\r';
        if (!addValue(driver, curRowIdx, column,
                std::string_view(buffer.get() + start, position - start - hasQuotes),
                escapePositions)) {
            goto ignore_error;
        }
        column++;

        curRowIdx += driver.addRow(curRowIdx, column,
            getOptionalWarningData<Driver>(columnInfo, option, getWarningSourceData()));

        column = 0;
        position++;
        // Adjust start for ReadBuffer.
        start = position;
        lineContext.setNewLine(getFileOffset());
        if (!maybeReadBuffer(&start)) {
            // File ends right after newline, go to final state.
            goto final_state;
        }
        if (isCarriageReturn) {
            // \r newline, go to special state that parses an optional \n afterwards
            goto carriage_return;
        } else {
            if (driver.done(curRowIdx)) {
                return {curRowIdx, numErrors};
            }
            goto value_start;
        }
    }
    in_quotes:
        // this state parses the remainder of a quoted value.
        position++;
        do {
            for (; position < bufferSize; position++) {
                if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
                    auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
                    sniffDriver.setEverQuoted();
                }
                if (buffer[position] == option.quoteChar) {
                    // quote: move to unquoted state
                    goto unquote;
                } else if (buffer[position] == option.escapeChar) {
                    // escape: store the escaped position and move to handle_escape state
                    escapePositions.push_back(position - start);
                    goto handle_escape;
                } else if (isNewLine(buffer[position])) {
                    [[unlikely]] if (!handleQuotedNewline()) { goto ignore_error; }
                }
            }
        } while (readBuffer(&start));
        [[unlikely]]
        // still in quoted state at the end of the file, error:
        lineContext.setEndOfLine(getFileOffset());
        if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
            auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
            sniffDriver.setError();
        } else {
            handleCopyException("unterminated quotes.");
        }
        // we are ignoring this error, skip current row and restart state machine
        goto ignore_error;
    unquote:
        DASSERT(hasQuotes && buffer[position] == option.quoteChar);
        // this state handles the state directly after we unquote
        // in this state we expect either another quote (entering the quoted state again, and
        // escaping the quote) or a delimiter/newline, ending the current value and moving on to the
        // next value
        position++;
        if (!maybeReadBuffer(&start)) {
            // file ends right after unquote, go to final state
            goto final_state;
        }
        if (buffer[position] == option.quoteChar &&
            (!option.escapeChar || option.escapeChar == option.quoteChar)) {
            // the escapeChar is used correctly, record this for DialectSniff
            if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
                auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
                sniffDriver.setEverEscaped();
            }
            // escaped quote, return to quoted state and store escape position
            escapePositions.push_back(position - start);
            goto in_quotes;
        } else if (buffer[position] == option.delimiter ||
                   buffer[position] == CopyConstants::DEFAULT_CSV_LIST_END_CHAR) {
            // delimiter, add value
            goto add_value;
        } else if (isNewLine(buffer[position])) {
            goto add_row;
        } else {
            if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
                auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
                sniffDriver.setError();
            } else {
                [[unlikely]] handleCopyException("quote should be followed by "
                                                 "end of file, end of value, end of "
                                                 "row or another quote.");
            }
            goto ignore_error;
        }
    handle_escape:
        // state: handle_escape
        // escape should be followed by a quote or another escape character
        position++;
        if (!maybeReadBuffer(&start)) {
            [[unlikely]] lineContext.setEndOfLine(getFileOffset());
            if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
                auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
                sniffDriver.setError();
            } else {
                handleCopyException("escape at end of file.");
            }
            goto ignore_error;
        }
        if (buffer[position] != option.quoteChar && buffer[position] != option.escapeChar) {
            ++position; // consume the invalid char
            if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
                auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
                sniffDriver.setError();
            } else {
                [[unlikely]] handleCopyException(
                    "neither QUOTE nor ESCAPE is proceeded by ESCAPE.");
            }
            goto ignore_error;
        }
        // the escapeChar is used correctly, record this for DialectSniff
        if (driver.driverType == DriverType::SNIFF_CSV_DIALECT) {
            auto& sniffDriver = reinterpret_cast<SniffCSVDialectDriver&>(driver);
            sniffDriver.setEverEscaped();
        }
        // escape was followed by quote or escape, go back to quoted state
        goto in_quotes;
    carriage_return:
        // this stage optionally skips a newline (\n) character, which allows \r\n to be interpreted
        // as a single line

        // position points to the character after the carriage return.
        if (buffer[position] == '\n') {
            // newline after carriage return: skip
            // increase position by 1 and move start to the new position
            start = ++position;
            if (!maybeReadBuffer(&start)) {
                // file ends right after newline, go to final state
                goto final_state;
            }
        }
        if (driver.done(curRowIdx)) {
            return {curRowIdx, numErrors};
        }

        goto value_start;
    final_state:
        // We get here when the file ends.
        // If we were mid-value, add the remaining value to the chunk.
        lineContext.setEndOfLine(getFileOffset());
        if (position > start) {
            // Add remaining value to chunk.
            if (!addValue(driver, curRowIdx, column,
                    std::string_view(buffer.get() + start, position - start - hasQuotes),
                    escapePositions)) {
                return {curRowIdx, numErrors};
            }
            column++;
        } else if (column > 0) {
            // File ends right after a delimiter with an empty trailing field.
            // Without this, a row like "a,b," (no trailing newline) loses its last
            // empty field and undercounts columns, causing "expected N, got N-1".
            if (!addValue(driver, curRowIdx, column, std::string_view{}, escapePositions)) {
                return {curRowIdx, numErrors};
            }
            column++;
        }
        if (column > 0) {
            curRowIdx += driver.addRow(curRowIdx, column,
                getOptionalWarningData<Driver>(columnInfo, option, getWarningSourceData()));
        }
        return {curRowIdx, numErrors};
    ignore_error:
        // we skip the current row then restart the state machine to continue parsing
        skipCurrentLine();
        if (driver.done(curRowIdx)) {
            return {curRowIdx, numErrors};
        }
        continue;
    }
    UNREACHABLE_CODE;
}

bool BaseCSVReader::supportsStructuralParser() const {
    return option.delimiter == CopyConstants::DEFAULT_CSV_DELIMITER &&
           option.quoteChar == CopyConstants::DEFAULT_CSV_QUOTE_CHAR &&
           option.escapeChar == CopyConstants::DEFAULT_CSV_ESCAPE_CHAR && !option.ignoreErrors;
}

static uint64_t findNextCSVStructuralByteScalar(const char* buffer, uint64_t position,
    uint64_t bufferSize, const CSVOption& option, bool inQuotedField) {
    for (; position < bufferSize; ++position) {
        const auto c = buffer[position];
        if (inQuotedField) {
            if (c == option.quoteChar || c == option.escapeChar || c == '\r' || c == '\n') {
                return position;
            }
        } else if (c == option.delimiter || c == '\r' || c == '\n') {
            return position;
        }
    }
    return bufferSize;
}

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
static uint64_t
findNextCSVStructuralByteAVX2(const char* buffer, uint64_t position, uint64_t bufferSize,
    const CSVOption& option, bool inQuotedField) {
    const auto quote = _mm256_set1_epi8(option.quoteChar);
    const auto escape = _mm256_set1_epi8(option.escapeChar);
    const auto delimiter = _mm256_set1_epi8(option.delimiter);
    const auto carriageReturn = _mm256_set1_epi8('\r');
    const auto newline = _mm256_set1_epi8('\n');

    for (; position + 32 <= bufferSize; position += 32) {
        const auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buffer + position));
        auto mask = _mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpeq_epi8(bytes, carriageReturn),
            _mm256_cmpeq_epi8(bytes, newline)));
        if (inQuotedField) {
            mask |= _mm256_movemask_epi8(
                _mm256_or_si256(_mm256_cmpeq_epi8(bytes, quote), _mm256_cmpeq_epi8(bytes, escape)));
        } else {
            mask |= _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, delimiter));
        }
        if (mask != 0) {
#if defined(__GNUC__) || defined(__clang__)
            return position + static_cast<uint64_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#elif defined(_MSC_VER)
            unsigned long index;
            _BitScanForward(&index, static_cast<unsigned long>(mask));
            return position + static_cast<uint64_t>(index);
#endif
        }
    }
    return findNextCSVStructuralByteScalar(buffer, position, bufferSize, option, inQuotedField);
}

static bool hasAVX2() {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static uint64_t findFirstNeonMatch(uint64_t position, const uint8_t* matches) {
    for (auto i = 0u; i < 16; ++i) {
        if (matches[i] != 0) {
            return position + i;
        }
    }
    return position + 16;
}

static uint64_t findNextCSVStructuralByteNEON(const char* buffer, uint64_t position,
    uint64_t bufferSize, const CSVOption& option, bool inQuotedField) {
    const auto quote = vdupq_n_u8(option.quoteChar);
    const auto escape = vdupq_n_u8(option.escapeChar);
    const auto delimiter = vdupq_n_u8(option.delimiter);
    const auto carriageReturn = vdupq_n_u8('\r');
    const auto newline = vdupq_n_u8('\n');

    alignas(16) uint8_t matches[16];
    for (; position + 16 <= bufferSize; position += 16) {
        const auto bytes = vld1q_u8(reinterpret_cast<const uint8_t*>(buffer + position));
        auto mask = vorrq_u8(vceqq_u8(bytes, carriageReturn), vceqq_u8(bytes, newline));
        if (inQuotedField) {
            mask = vorrq_u8(mask, vorrq_u8(vceqq_u8(bytes, quote), vceqq_u8(bytes, escape)));
        } else {
            mask = vorrq_u8(mask, vceqq_u8(bytes, delimiter));
        }
        if (vmaxvq_u8(mask) != 0) {
            vst1q_u8(matches, mask);
            return findFirstNeonMatch(position, matches);
        }
    }
    return findNextCSVStructuralByteScalar(buffer, position, bufferSize, option, inQuotedField);
}
#endif

static uint64_t findNextCSVStructuralByte(const char* buffer, uint64_t position,
    uint64_t bufferSize, const CSVOption& option, bool inQuotedField) {
#if defined(__x86_64__) || defined(_M_X64)
    static const bool avx2 = hasAVX2();
    if (avx2) {
        return findNextCSVStructuralByteAVX2(buffer, position, bufferSize, option, inQuotedField);
    }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return findNextCSVStructuralByteNEON(buffer, position, bufferSize, option, inQuotedField);
#else
    return findNextCSVStructuralByteScalar(buffer, position, bufferSize, option, inQuotedField);
#endif
}

template<typename Driver>
BaseCSVReader::parse_result_t BaseCSVReader::parseCSVStructural(Driver& driver) {
    DASSERT(nullptr != errorHandler);
    DASSERT(supportsStructuralParser());

    curRowIdx = 0;
    numErrors = 0;

    while (true) {
        column_id_t column = 0;
        auto start = position.load();
        bool hasQuotes = false;
        std::vector<uint64_t> escapePositions;
        lineContext.setNewLine(getFileOffset());

        if (!maybeReadBuffer(&start)) {
            return {curRowIdx, numErrors};
        }

        goto value_start;
    value_start:
        if (buffer[position] == option.quoteChar) {
            start = position + 1;
            hasQuotes = true;
            ++position;
            goto in_quotes;
        }
        start = position;
        hasQuotes = false;
        goto normal;
    normal:
        do {
            position = findNextCSVStructuralByte(buffer.get(), position, bufferSize, option, false);
            if (position >= bufferSize) {
                continue;
            }
            if (buffer[position] == option.delimiter) {
                goto add_value;
            }
            if (isNewLine(buffer[position])) {
                goto add_row;
            }
        } while (readBuffer(&start));
        goto final_state;
    add_value:
        DASSERT(buffer[position] == option.delimiter ||
                buffer[position] == CopyConstants::DEFAULT_CSV_LIST_END_CHAR);
        if (!addValue(driver, curRowIdx, column,
                std::string_view(buffer.get() + start, position - start - hasQuotes),
                escapePositions)) {
            goto ignore_error;
        }
        column++;
        ++position;
        start = position;
        if (!maybeReadBuffer(&start)) {
            goto final_state;
        }
        goto value_start;
    add_row: {
        DASSERT(isNewLine(buffer[position]));
        lineContext.setEndOfLine(getFileOffset());
        bool isCarriageReturn = buffer[position] == '\r';
        if (!addValue(driver, curRowIdx, column,
                std::string_view(buffer.get() + start, position - start - hasQuotes),
                escapePositions)) {
            goto ignore_error;
        }
        column++;

        curRowIdx += driver.addRow(curRowIdx, column,
            getOptionalWarningData<Driver>(columnInfo, option, getWarningSourceData()));

        column = 0;
        position++;
        start = position;
        lineContext.setNewLine(getFileOffset());
        if (!maybeReadBuffer(&start)) {
            goto final_state;
        }
        if (isCarriageReturn) {
            goto carriage_return;
        }
        if (driver.done(curRowIdx)) {
            return {curRowIdx, numErrors};
        }
        goto value_start;
    }
    in_quotes:
        do {
            position = findNextCSVStructuralByte(buffer.get(), position, bufferSize, option, true);
            if (position >= bufferSize) {
                continue;
            }
            if (buffer[position] == option.quoteChar) {
                goto unquote;
            }
            if (buffer[position] == option.escapeChar) {
                escapePositions.push_back(position - start);
                goto handle_escape;
            }
            if (isNewLine(buffer[position])) {
                if (!handleQuotedNewline()) {
                    goto ignore_error;
                }
                ++position;
                goto in_quotes;
            }
        } while (readBuffer(&start));
        lineContext.setEndOfLine(getFileOffset());
        handleCopyException("unterminated quotes.");
        goto ignore_error;
    unquote:
        DASSERT(hasQuotes && buffer[position] == option.quoteChar);
        position++;
        if (!maybeReadBuffer(&start)) {
            goto final_state;
        }
        if (buffer[position] == option.quoteChar) {
            escapePositions.push_back(position - start);
            ++position;
            goto in_quotes;
        }
        if (buffer[position] == option.delimiter ||
            buffer[position] == CopyConstants::DEFAULT_CSV_LIST_END_CHAR) {
            goto add_value;
        }
        if (isNewLine(buffer[position])) {
            goto add_row;
        }
        handleCopyException("quote should be followed by "
                            "end of file, end of value, end of "
                            "row or another quote.");
        goto ignore_error;
    handle_escape:
        position++;
        if (!maybeReadBuffer(&start)) {
            lineContext.setEndOfLine(getFileOffset());
            handleCopyException("escape at end of file.");
            goto ignore_error;
        }
        if (buffer[position] != option.quoteChar && buffer[position] != option.escapeChar) {
            ++position;
            handleCopyException("neither QUOTE nor ESCAPE is proceeded by ESCAPE.");
            goto ignore_error;
        }
        ++position;
        goto in_quotes;
    carriage_return:
        if (buffer[position] == '\n') {
            start = ++position;
            if (!maybeReadBuffer(&start)) {
                goto final_state;
            }
        }
        if (driver.done(curRowIdx)) {
            return {curRowIdx, numErrors};
        }
        goto value_start;
    final_state:
        lineContext.setEndOfLine(getFileOffset());
        if (position > start) {
            if (!addValue(driver, curRowIdx, column,
                    std::string_view(buffer.get() + start, position - start - hasQuotes),
                    escapePositions)) {
                return {curRowIdx, numErrors};
            }
            column++;
        } else if (column > 0) {
            // File ends right after a delimiter with an empty trailing field.
            // Without this, a row like "a,b," (no trailing newline) loses its last
            // empty field and undercounts columns, causing "expected N, got N-1".
            if (!addValue(driver, curRowIdx, column, std::string_view{}, escapePositions)) {
                return {curRowIdx, numErrors};
            }
            column++;
        }
        if (column > 0) {
            curRowIdx += driver.addRow(curRowIdx, column,
                getOptionalWarningData<Driver>(columnInfo, option, getWarningSourceData()));
        }
        return {curRowIdx, numErrors};
    ignore_error:
        skipCurrentLine();
        if (driver.done(curRowIdx)) {
            return {curRowIdx, numErrors};
        }
        continue;
    }
    UNREACHABLE_CODE;
}

column_id_t BaseCSVReader::appendWarningDataColumns(std::vector<std::string>& resultColumnNames,
    std::vector<common::LogicalType>& resultColumnTypes, const common::FileScanInfo& fileScanInfo) {
    const bool ignoreErrors = fileScanInfo.getOption(CopyConstants::IGNORE_ERRORS_OPTION_NAME,
        CopyConstants::DEFAULT_IGNORE_ERRORS);
    column_id_t numWarningDataColumns = 0;
    if (ignoreErrors) {
        numWarningDataColumns = CopyConstants::CSV_WARNING_DATA_NUM_COLUMNS;
        for (idx_t i = 0; i < CopyConstants::CSV_WARNING_DATA_NUM_COLUMNS; ++i) {
            resultColumnNames.emplace_back(CopyConstants::CSV_WARNING_DATA_COLUMN_NAMES[i]);
            resultColumnTypes.emplace_back(CopyConstants::CSV_WARNING_DATA_COLUMN_TYPES[i]);
        }
    }
    return numWarningDataColumns;
}

PopulatedCopyFromError BaseCSVReader::basePopulateErrorFunc(CopyFromFileError error,
    const SharedFileErrorHandler* sharedErrorHandler, BaseCSVReader* reader, std::string filePath) {
    const auto warningData = CSVWarningSourceData::constructFrom(error.warningData);
    const auto lineNumber =
        sharedErrorHandler->getLineNumber(warningData.blockIdx, warningData.offsetInBlock);
    return PopulatedCopyFromError{
        .message = std::move(error.message),
        .filePath = std::move(filePath),
        .skippedLineOrRecord = reader->reconstructLine(warningData.startByteOffset,
            warningData.endByteOffset, error.completedLine),
        .lineNumber = lineNumber,
    };
}

common::idx_t BaseCSVReader::getFileIdxFunc(const CopyFromFileError& error) {
    return CSVWarningSourceData::constructFrom(error.warningData).fileIdx;
}

template BaseCSVReader::parse_result_t BaseCSVReader::parseCSV<ParallelParsingDriver>(
    ParallelParsingDriver&);
template BaseCSVReader::parse_result_t BaseCSVReader::parseCSVStructural<ParallelParsingDriver>(
    ParallelParsingDriver&);
template BaseCSVReader::parse_result_t BaseCSVReader::parseCSV<SerialParsingDriver>(
    SerialParsingDriver&);
template BaseCSVReader::parse_result_t BaseCSVReader::parseCSV<SniffCSVNameAndTypeDriver>(
    SniffCSVNameAndTypeDriver&);
template BaseCSVReader::parse_result_t BaseCSVReader::parseCSV<SniffCSVDialectDriver>(
    SniffCSVDialectDriver&);
template BaseCSVReader::parse_result_t BaseCSVReader::parseCSV<SniffCSVHeaderDriver>(
    SniffCSVHeaderDriver&);

uint64_t BaseCSVReader::getFileOffset() const {
    DASSERT(osFileOffset >= bufferSize);
    return osFileOffset - bufferSize + position;
}

} // namespace processor
} // namespace lbug
