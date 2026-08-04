#include "processor/operator/persistent/reader/csv/csv_boundary_scanner.h"

#include <algorithm>
#include <array>

#include "common/constants.h"
#include "common/file_system/virtual_file_system.h"
#include "main/client_context.h"
#include <bit>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace lbug {
namespace processor {

using namespace common;

namespace {

enum class CSVBoundaryScannerCharClass : uint8_t {
    Other,
    Delimiter,
    Quote,
    Escape,
    CarriageReturn,
    LineFeed,
    COUNT,
};

enum CSVBoundaryScannerAction : uint8_t {
    None = 0,
    MarkQuotedMultiline = 1u << 0u,
    MarkLogicalBoundary = 1u << 1u,
    SetFieldStartTrue = 1u << 2u,
    SetFieldStartFalse = 1u << 3u,
    MarkInvalid = 1u << 4u,
};

struct CSVBoundaryScannerTransition {
    CSVBoundaryScannerState nextState;
    uint8_t actions;
};

struct CSVBoundaryScannerMasks {
    uint32_t interesting = 0;
};

enum class CSVBoundaryScannerSeed : uint8_t {
    OutsideFieldStart,
    OutsideFieldMiddle,
    InQuotedField,
    AfterQuote,
    Escaped,
    CarriageReturn,
    COUNT,
};

struct CSVOverlapBoundaryResult {
    bool foundBoundary = false;
    bool detectedQuotedMultiline = false;
    bool sawInvalidQuotedTransition = false;
    uint64_t boundaryOffset = 0;
};

struct CSVOverlapSeedBoundaryResult {
    bool usable = false;
    bool hasBoundaryAtOrAfterCut = false;
    bool detectedQuotedMultiline = false;
    bool sawInvalidQuotedTransition = false;
    uint64_t boundaryOffset = 0;
};

std::vector<CSVParseRange> makeSingleFileRange(idx_t fileIdx, uint64_t fileSize) {
    std::vector<CSVParseRange> ranges;
    if (fileSize > 0) {
        ranges.push_back(CSVParseRange{fileIdx, 0, fileSize, 0, true});
    }
    return ranges;
}

struct CSVAdjustedChunkRanges {
    std::vector<CSVParseRange> ranges;
    bool detectedOversizedLogicalRow = false;
};

struct CSVFileBoundaryProperties {
    bool detectedQuotedMultiline = false;
    bool detectedOversizedLogicalRow = false;
};

CSVAdjustedChunkRanges makeAdjustedChunkRanges(idx_t fileIdx, uint64_t fileSize,
    const std::vector<uint64_t>& adjustedBoundaries) {
    CSVAdjustedChunkRanges result;
    if (fileSize == 0) {
        return result;
    }
    uint64_t currentRangeStart = 0;
    uint64_t previousBoundary = 0;
    block_idx_t nextRangeIdx = 0;
    for (auto boundaryOffset : adjustedBoundaries) {
        if (boundaryOffset <= currentRangeStart) {
            continue;
        }
        result.detectedOversizedLogicalRow =
            result.detectedOversizedLogicalRow ||
            boundaryOffset - previousBoundary > CopyConstants::PARALLEL_BLOCK_SIZE;
        previousBoundary = boundaryOffset;
        result.ranges.push_back(CSVParseRange{fileIdx, currentRangeStart, boundaryOffset,
            nextRangeIdx++, currentRangeStart == 0});
        currentRangeStart = boundaryOffset;
    }
    result.detectedOversizedLogicalRow =
        result.detectedOversizedLogicalRow ||
        fileSize - previousBoundary > CopyConstants::PARALLEL_BLOCK_SIZE;
    if (fileSize > currentRangeStart) {
        result.ranges.push_back(CSVParseRange{fileIdx, currentRangeStart, fileSize, nextRangeIdx++,
            currentRangeStart == 0});
    }
    return result;
}

struct CSVBoundaryScannerRuntimeState {
    CSVBoundaryScannerState state = CSVBoundaryScannerState::OutsideField;
    bool atFieldStart = true;
    uint64_t pendingCarriageReturnOffset = 0;
    bool sawInvalidQuotedTransition = false;
};

CSVBoundaryScannerRuntimeState runtimeStateFromSeed(CSVBoundaryScannerSeed seed,
    uint64_t chunkOffset) {
    CSVBoundaryScannerRuntimeState state;
    switch (seed) {
    case CSVBoundaryScannerSeed::OutsideFieldStart:
        state.state = CSVBoundaryScannerState::OutsideField;
        state.atFieldStart = true;
        break;
    case CSVBoundaryScannerSeed::OutsideFieldMiddle:
        state.state = CSVBoundaryScannerState::OutsideField;
        state.atFieldStart = false;
        break;
    case CSVBoundaryScannerSeed::InQuotedField:
        state.state = CSVBoundaryScannerState::InQuotedField;
        state.atFieldStart = false;
        break;
    case CSVBoundaryScannerSeed::AfterQuote:
        state.state = CSVBoundaryScannerState::AfterQuote;
        state.atFieldStart = false;
        break;
    case CSVBoundaryScannerSeed::Escaped:
        state.state = CSVBoundaryScannerState::Escaped;
        state.atFieldStart = false;
        break;
    case CSVBoundaryScannerSeed::CarriageReturn:
        state.state = CSVBoundaryScannerState::CarriageReturn;
        state.atFieldStart = true;
        state.pendingCarriageReturnOffset = chunkOffset == 0 ? 0 : chunkOffset - 1;
        break;
    case CSVBoundaryScannerSeed::COUNT:
        UNREACHABLE_CODE;
    }
    return state;
}

class CSVBoundaryScannerFSM {
public:
    explicit CSVBoundaryScannerFSM(const CSVOption& option) : classTable{buildClassTable(option)} {}

    template<typename BoundaryFunc>
    void consumeBoringSpan(uint64_t spanLength, BoundaryFunc&& onBoundary,
        CSVBoundaryScannerRuntimeState& runtimeState) const {
        if (spanLength == 0) {
            return;
        }
        switch (runtimeState.state) {
        case CSVBoundaryScannerState::OutsideField:
            runtimeState.atFieldStart = false;
            break;
        case CSVBoundaryScannerState::InQuotedField:
            break;
        case CSVBoundaryScannerState::AfterQuote:
            runtimeState.sawInvalidQuotedTransition = true;
            runtimeState.state = CSVBoundaryScannerState::OutsideField;
            runtimeState.atFieldStart = false;
            break;
        case CSVBoundaryScannerState::Escaped:
            runtimeState.sawInvalidQuotedTransition = true;
            runtimeState.state = CSVBoundaryScannerState::InQuotedField;
            break;
        case CSVBoundaryScannerState::CarriageReturn:
            onBoundary(runtimeState.pendingCarriageReturnOffset + 1);
            runtimeState.state = CSVBoundaryScannerState::OutsideField;
            runtimeState.atFieldStart = false;
            break;
        }
    }

    template<typename BoundaryFunc>
    void step(char c, uint64_t absoluteOffset, BoundaryFunc&& onBoundary,
        bool& detectedQuotedMultiline, CSVBoundaryScannerRuntimeState& runtimeState) const {
        auto cls = getCharClass(c);
        if (runtimeState.state == CSVBoundaryScannerState::CarriageReturn &&
            cls == CSVBoundaryScannerCharClass::LineFeed) {
            onBoundary(absoluteOffset + 1);
            runtimeState.state = CSVBoundaryScannerState::OutsideField;
            runtimeState.atFieldStart = true;
            return;
        }
        if (runtimeState.state == CSVBoundaryScannerState::OutsideField &&
            cls == CSVBoundaryScannerCharClass::Quote) {
            if (runtimeState.atFieldStart) {
                runtimeState.state = CSVBoundaryScannerState::InQuotedField;
                runtimeState.atFieldStart = false;
            } else {
                runtimeState.atFieldStart = false;
            }
            return;
        }
        if (runtimeState.state == CSVBoundaryScannerState::CarriageReturn) {
            onBoundary(runtimeState.pendingCarriageReturnOffset + 1);
            runtimeState.state = CSVBoundaryScannerState::OutsideField;
        }

        const auto transition =
            TRANSITIONS[static_cast<uint8_t>(runtimeState.state)][static_cast<uint8_t>(cls)];
        runtimeState.state = transition.nextState;
        applyActions(transition.actions, absoluteOffset, onBoundary, detectedQuotedMultiline,
            runtimeState);
    }

private:
    static constexpr std::array<std::array<CSVBoundaryScannerTransition,
                                    static_cast<uint8_t>(CSVBoundaryScannerCharClass::COUNT)>,
        5>
        TRANSITIONS{{
            // OutsideField
            {{{CSVBoundaryScannerState::OutsideField, SetFieldStartFalse},
                {CSVBoundaryScannerState::OutsideField, SetFieldStartTrue},
                {CSVBoundaryScannerState::OutsideField, None},
                {CSVBoundaryScannerState::OutsideField, SetFieldStartFalse},
                {CSVBoundaryScannerState::CarriageReturn, SetFieldStartTrue},
                {CSVBoundaryScannerState::OutsideField, MarkLogicalBoundary | SetFieldStartTrue}}},
            // InQuotedField
            {{{CSVBoundaryScannerState::InQuotedField, None},
                {CSVBoundaryScannerState::InQuotedField, None},
                {CSVBoundaryScannerState::AfterQuote, None},
                {CSVBoundaryScannerState::Escaped, None},
                {CSVBoundaryScannerState::InQuotedField, MarkQuotedMultiline},
                {CSVBoundaryScannerState::InQuotedField, MarkQuotedMultiline}}},
            // AfterQuote
            {{{CSVBoundaryScannerState::OutsideField, MarkInvalid | SetFieldStartFalse},
                {CSVBoundaryScannerState::OutsideField, SetFieldStartTrue},
                {CSVBoundaryScannerState::InQuotedField, None},
                {CSVBoundaryScannerState::OutsideField, MarkInvalid | SetFieldStartFalse},
                {CSVBoundaryScannerState::CarriageReturn, SetFieldStartTrue},
                {CSVBoundaryScannerState::OutsideField, MarkLogicalBoundary | SetFieldStartTrue}}},
            // Escaped
            {{{CSVBoundaryScannerState::InQuotedField, MarkInvalid},
                {CSVBoundaryScannerState::InQuotedField, MarkInvalid},
                {CSVBoundaryScannerState::InQuotedField, None},
                {CSVBoundaryScannerState::InQuotedField, None},
                {CSVBoundaryScannerState::InQuotedField, MarkInvalid | MarkQuotedMultiline},
                {CSVBoundaryScannerState::InQuotedField, MarkInvalid | MarkQuotedMultiline}}},
            // CarriageReturn
            {{{CSVBoundaryScannerState::OutsideField, SetFieldStartFalse},
                {CSVBoundaryScannerState::OutsideField, SetFieldStartTrue},
                {CSVBoundaryScannerState::OutsideField, None},
                {CSVBoundaryScannerState::OutsideField, SetFieldStartFalse},
                {CSVBoundaryScannerState::CarriageReturn, SetFieldStartTrue},
                {CSVBoundaryScannerState::OutsideField, MarkLogicalBoundary | SetFieldStartTrue}}},
        }};

    static std::array<CSVBoundaryScannerCharClass, 256> buildClassTable(const CSVOption& option) {
        std::array<CSVBoundaryScannerCharClass, 256> table;
        table.fill(CSVBoundaryScannerCharClass::Other);
        table[static_cast<uint8_t>('\r')] = CSVBoundaryScannerCharClass::CarriageReturn;
        table[static_cast<uint8_t>('\n')] = CSVBoundaryScannerCharClass::LineFeed;
        table[static_cast<uint8_t>(option.delimiter)] = CSVBoundaryScannerCharClass::Delimiter;
        table[static_cast<uint8_t>(option.escapeChar)] = CSVBoundaryScannerCharClass::Escape;
        table[static_cast<uint8_t>(option.quoteChar)] = CSVBoundaryScannerCharClass::Quote;
        return table;
    }

    CSVBoundaryScannerCharClass getCharClass(char c) const {
        return classTable[static_cast<uint8_t>(c)];
    }

    template<typename BoundaryFunc>
    static void applyActions(uint8_t actions, uint64_t absoluteOffset, BoundaryFunc&& onBoundary,
        bool& detectedQuotedMultiline, CSVBoundaryScannerRuntimeState& runtimeState) {
        if ((actions & MarkQuotedMultiline) != 0) {
            detectedQuotedMultiline = true;
        }
        if ((actions & MarkLogicalBoundary) != 0) {
            onBoundary(absoluteOffset + 1);
        }
        if ((actions & SetFieldStartTrue) != 0) {
            runtimeState.atFieldStart = true;
        } else if ((actions & SetFieldStartFalse) != 0) {
            runtimeState.atFieldStart = false;
        }
        if ((actions & MarkInvalid) != 0) {
            runtimeState.sawInvalidQuotedTransition = true;
        }
        if (runtimeState.state == CSVBoundaryScannerState::CarriageReturn) {
            runtimeState.pendingCarriageReturnOffset = absoluteOffset;
        }
    }

    std::array<CSVBoundaryScannerCharClass, 256> classTable;
};

CSVBoundaryScannerMasks scanScalar(const char* buffer, uint64_t bytesToRead,
    const CSVOption& option) {
    CSVBoundaryScannerMasks masks;
    for (uint64_t i = 0; i < bytesToRead; ++i) {
        const auto c = buffer[i];
        if (c == option.delimiter || c == option.quoteChar || c == option.escapeChar || c == '\r' ||
            c == '\n') {
            masks.interesting |= (1u << i);
        }
    }
    return masks;
}

#if defined(__SSE2__)
CSVBoundaryScannerMasks scanSSE2(const char* buffer, const CSVOption& option) {
    const auto data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buffer));
    auto mask = _mm_or_si128(_mm_cmpeq_epi8(data, _mm_set1_epi8(option.delimiter)),
        _mm_cmpeq_epi8(data, _mm_set1_epi8(option.quoteChar)));
    mask = _mm_or_si128(mask, _mm_cmpeq_epi8(data, _mm_set1_epi8(option.escapeChar)));
    mask = _mm_or_si128(mask, _mm_cmpeq_epi8(data, _mm_set1_epi8('\r')));
    mask = _mm_or_si128(mask, _mm_cmpeq_epi8(data, _mm_set1_epi8('\n')));
    return {static_cast<uint32_t>(_mm_movemask_epi8(mask))};
}
#endif

#if defined(__ARM_NEON)
CSVBoundaryScannerMasks scanNEON(const char* buffer, const CSVOption& option) {
    const auto data = vld1q_u8(reinterpret_cast<const uint8_t*>(buffer));
    auto mask = vorrq_u8(vceqq_u8(data, vdupq_n_u8(static_cast<uint8_t>(option.delimiter))),
        vceqq_u8(data, vdupq_n_u8(static_cast<uint8_t>(option.quoteChar))));
    mask = vorrq_u8(mask, vceqq_u8(data, vdupq_n_u8(static_cast<uint8_t>(option.escapeChar))));
    mask = vorrq_u8(mask, vceqq_u8(data, vdupq_n_u8(static_cast<uint8_t>('\r'))));
    mask = vorrq_u8(mask, vceqq_u8(data, vdupq_n_u8(static_cast<uint8_t>('\n'))));
    alignas(16) uint8_t bytes[16];
    vst1q_u8(bytes, mask);
    uint32_t interesting = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (bytes[i] != 0) {
            interesting |= (1u << i);
        }
    }
    return {interesting};
}
#endif

CSVBoundaryScannerMasks scanInterestingBytes(const char* buffer, uint64_t bytesToRead,
    const CSVOption& option) {
#if defined(__SSE2__)
    if (bytesToRead == 16) {
        return scanSSE2(buffer, option);
    }
#elif defined(__ARM_NEON)
    if (bytesToRead == 16) {
        return scanNEON(buffer, option);
    }
#endif
    return scanScalar(buffer, bytesToRead, option);
}

template<typename BoundaryFunc>
void processChunk(const char* buffer, uint64_t bytesToRead, uint64_t chunkOffset,
    const CSVOption& option, const CSVBoundaryScannerFSM& fsm,
    CSVBoundaryScannerRuntimeState& runtimeState, bool& detectedQuotedMultiline,
    BoundaryFunc&& onBoundary) {
    static constexpr uint64_t SIMD_WIDTH = 16;
    uint64_t i = 0;
    while (i < bytesToRead) {
        const auto laneWidth = std::min<uint64_t>(SIMD_WIDTH, bytesToRead - i);
        const auto masks = scanInterestingBytes(buffer + i, laneWidth, option);
        if (masks.interesting == 0) {
            fsm.consumeBoringSpan(laneWidth, onBoundary, runtimeState);
            i += laneWidth;
            continue;
        }

        uint64_t lastProcessed = 0;
        auto interesting = masks.interesting;
        while (interesting != 0) {
            const auto bit = std::countr_zero(interesting);
            fsm.consumeBoringSpan(bit - lastProcessed, onBoundary, runtimeState);
            const auto absoluteOffset = chunkOffset + i + bit;
            fsm.step(buffer[i + bit], absoluteOffset, onBoundary, detectedQuotedMultiline,
                runtimeState);
            lastProcessed = bit + 1;
            interesting &= interesting - 1;
        }
        fsm.consumeBoringSpan(laneWidth - lastProcessed, onBoundary, runtimeState);
        i += laneWidth;
    }
}

CSVFileBoundaryProperties scanWholeFileProperties(FileInfo* fileInfo, uint64_t fileSize,
    const CSVOption& option) {
    CSVFileBoundaryProperties result;
    auto buffer = std::make_unique<char[]>(fileSize);
    fileInfo->readFromFile(buffer.get(), fileSize, 0);

    CSVBoundaryScannerFSM fsm{option};
    auto runtimeState = runtimeStateFromSeed(CSVBoundaryScannerSeed::OutsideFieldStart, 0);
    uint64_t previousBoundary = 0;
    auto onBoundary = [&](uint64_t boundaryOffset) {
        result.detectedOversizedLogicalRow =
            result.detectedOversizedLogicalRow ||
            boundaryOffset - previousBoundary > CopyConstants::PARALLEL_BLOCK_SIZE;
        previousBoundary = boundaryOffset;
    };
    processChunk(buffer.get(), fileSize, 0, option, fsm, runtimeState,
        result.detectedQuotedMultiline, onBoundary);
    result.detectedOversizedLogicalRow =
        result.detectedOversizedLogicalRow ||
        fileSize - previousBoundary > CopyConstants::PARALLEL_BLOCK_SIZE;
    return result;
}

CSVOverlapSeedBoundaryResult scanOverlapSeedForBoundary(const char* buffer, uint64_t bytesToRead,
    uint64_t windowOffset, uint64_t cutOffset, uint64_t fileSize, const CSVOption& option,
    const CSVBoundaryScannerFSM& fsm, CSVBoundaryScannerSeed seed, bool startsAtFileStart) {
    CSVOverlapSeedBoundaryResult result;
    auto runtimeState = runtimeStateFromSeed(seed, windowOffset);
    bool hasBoundaryBeforeOrAtCut = startsAtFileStart;
    auto onBoundary = [&](uint64_t boundaryOffset) {
        if (boundaryOffset <= cutOffset) {
            hasBoundaryBeforeOrAtCut = true;
        }
        if (boundaryOffset >= cutOffset && !result.hasBoundaryAtOrAfterCut) {
            result.hasBoundaryAtOrAfterCut = true;
            result.boundaryOffset = boundaryOffset;
        }
    };
    processChunk(buffer, bytesToRead, windowOffset, option, fsm, runtimeState,
        result.detectedQuotedMultiline, onBoundary);
    result.sawInvalidQuotedTransition = runtimeState.sawInvalidQuotedTransition;
    if (!result.hasBoundaryAtOrAfterCut && windowOffset + bytesToRead == fileSize &&
        runtimeState.state != CSVBoundaryScannerState::InQuotedField &&
        runtimeState.state != CSVBoundaryScannerState::Escaped) {
        result.hasBoundaryAtOrAfterCut = true;
        result.boundaryOffset = fileSize;
    }
    result.usable = hasBoundaryBeforeOrAtCut && !result.sawInvalidQuotedTransition &&
                    result.hasBoundaryAtOrAfterCut;
    return result;
}

CSVOverlapBoundaryResult scanOverlapForBoundary(FileInfo* fileInfo, uint64_t fileSize,
    uint64_t cutOffset, uint64_t overlapSize, const CSVOption& option) {
    const auto windowOffset = cutOffset > overlapSize ? cutOffset - overlapSize : 0;
    const auto windowEnd = std::min<uint64_t>(fileSize, cutOffset + overlapSize);
    const auto bytesToRead = windowEnd - windowOffset;
    auto buffer = std::make_unique<char[]>(bytesToRead);
    fileInfo->readFromFile(buffer.get(), bytesToRead, windowOffset);

    CSVOverlapBoundaryResult result;
    CSVBoundaryScannerFSM fsm{option};
    const auto firstSeed =
        windowOffset == 0 ? static_cast<uint8_t>(CSVBoundaryScannerSeed::OutsideFieldStart) : 0;
    const auto endSeed =
        windowOffset == 0 ? firstSeed + 1 : static_cast<uint8_t>(CSVBoundaryScannerSeed::COUNT);
    for (auto seedIdx = firstSeed; seedIdx < endSeed; ++seedIdx) {
        // CSV quoting is not self-synchronizing in general. A cut is safe only when every usable
        // seed that reaches a real row boundary before the cut agrees on the same next boundary.
        // Disagreement means the window is still ambiguous, so the caller expands the overlap and
        // eventually falls back to a single file-sized range if ambiguity remains.
        auto seedResult =
            scanOverlapSeedForBoundary(buffer.get(), bytesToRead, windowOffset, cutOffset, fileSize,
                option, fsm, static_cast<CSVBoundaryScannerSeed>(seedIdx), windowOffset == 0);
        result.detectedQuotedMultiline =
            result.detectedQuotedMultiline || seedResult.detectedQuotedMultiline;
        result.sawInvalidQuotedTransition =
            result.sawInvalidQuotedTransition || seedResult.sawInvalidQuotedTransition;
        if (!seedResult.usable) {
            continue;
        }
        if (!result.foundBoundary) {
            result.foundBoundary = true;
            result.boundaryOffset = seedResult.boundaryOffset;
        } else if (result.boundaryOffset != seedResult.boundaryOffset) {
            result.foundBoundary = false;
            return result;
        }
    }
    return result;
}

} // namespace

CSVBoundaryScanResult CSVBoundaryScanner::planFixedChunkOverlap(const std::string& filePath,
    idx_t fileIdx, const CSVOption& option, main::ClientContext* context) {
    auto fileInfo = VirtualFileSystem::GetUnsafe(*context)->openFile(filePath,
        FileOpenFlags(FileFlags::READ_ONLY
#ifdef _WIN32
                      | FileFlags::BINARY
#endif
            ),
        context);
    CSVBoundaryScanResult result;
    result.fileSize = fileInfo->getFileSize();
    if (result.fileSize == 0) {
        return result;
    }

    const auto blockSize = CopyConstants::PARALLEL_BLOCK_SIZE;
    const auto numBlocks = (result.fileSize + blockSize - 1) / blockSize;
    if (numBlocks == 1) {
        const auto properties = scanWholeFileProperties(fileInfo.get(), result.fileSize, option);
        result.detectedQuotedMultiline = properties.detectedQuotedMultiline;
        result.detectedOversizedLogicalRow = properties.detectedOversizedLogicalRow;
        result.usePlannedRanges =
            result.detectedQuotedMultiline || result.detectedOversizedLogicalRow;
        if (result.usePlannedRanges) {
            result.ranges = makeSingleFileRange(fileIdx, result.fileSize);
        }
        return result;
    }
    std::vector<uint64_t> adjustedBoundaries;
    adjustedBoundaries.reserve(numBlocks > 0 ? numBlocks - 1 : 0);
    for (uint64_t blockIdx = 1; blockIdx < numBlocks; ++blockIdx) {
        const auto cutOffset = blockIdx * blockSize;
        CSVOverlapBoundaryResult overlapResult;
        auto currentOverlapSize = blockSize;
        while (true) {
            overlapResult = scanOverlapForBoundary(fileInfo.get(), result.fileSize, cutOffset,
                currentOverlapSize, option);
            result.detectedQuotedMultiline =
                result.detectedQuotedMultiline || overlapResult.detectedQuotedMultiline;
            if (overlapResult.foundBoundary) {
                break;
            }
            if (currentOverlapSize >= result.fileSize) {
                result.ranges = makeSingleFileRange(fileIdx, result.fileSize);
                result.usePlannedRanges = !result.ranges.empty();
                return result;
            }
            currentOverlapSize = std::min<uint64_t>(result.fileSize, currentOverlapSize * 2);
        }
        if (!adjustedBoundaries.empty() &&
            overlapResult.boundaryOffset <= adjustedBoundaries.back()) {
            continue;
        }
        // Planned ranges must start and end at logical row boundaries. We only accept a cut when
        // every usable FSM seed in the overlap window resolves to the same next boundary; otherwise
        // the overlap expands, and an unresolved cut falls back to one file-sized range.
        adjustedBoundaries.push_back(overlapResult.boundaryOffset);
    }
    auto adjustedRanges = makeAdjustedChunkRanges(fileIdx, result.fileSize, adjustedBoundaries);
    result.ranges = std::move(adjustedRanges.ranges);
    result.detectedOversizedLogicalRow = adjustedRanges.detectedOversizedLogicalRow;
    result.usePlannedRanges =
        (result.detectedQuotedMultiline || result.detectedOversizedLogicalRow) &&
        !result.ranges.empty();
    if (!result.usePlannedRanges) {
        result.ranges.clear();
    }
    return result;
}

} // namespace processor
} // namespace lbug
