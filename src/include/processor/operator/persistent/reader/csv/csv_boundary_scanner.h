#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/copier_config/csv_reader_config.h"
#include "common/types/types.h"

namespace lbug {

namespace main {
class ClientContext;
}

namespace processor {

enum class CSVBoundaryScannerState : uint8_t {
    OutsideField,
    InQuotedField,
    AfterQuote,
    Escaped,
    CarriageReturn,
};

struct CSVParseRange {
    common::idx_t fileIdx;
    uint64_t startOffset;
    uint64_t endOffset;
    common::block_idx_t rangeIdx;
    bool startsAtFileStart;
};

struct CSVBoundaryScanResult {
    uint64_t fileSize = 0;
    bool detectedQuotedMultiline = false;
    bool detectedOversizedLogicalRow = false;
    bool usePlannedRanges = false;
    std::vector<CSVParseRange> ranges;
};

class CSVBoundaryScanner {
public:
    static CSVBoundaryScanResult planFixedChunkOverlap(const std::string& filePath,
        common::idx_t fileIdx, const common::CSVOption& option, main::ClientContext* context);
};

} // namespace processor
} // namespace lbug
