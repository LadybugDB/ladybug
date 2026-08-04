#pragma once

#include <utility>
#include <vector>

#include "dictionary_chunk.h"
#include "storage/table/column.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/string_chunk_data.h"

namespace lbug {
namespace storage {

class StringColumn;

class DictionaryColumn {
public:
    DictionaryColumn(const std::string& name, FileHandle* dataFH, MemoryManager* mm,
        ShadowFile* shadowFile, bool enableCompression);

    void scan(const SegmentState& state, DictionaryChunk& dictChunk) const;

    DictionaryChunk::string_index_t append(const DictionaryChunk& dictChunk, SegmentState& state,
        std::string_view val) const;

    bool canCommitInPlace(const SegmentState& state, uint64_t numNewStrings,
        uint64_t totalStringLengthToAdd) const;

    Column* getDataColumn() const { return dataColumn.get(); }
    Column* getOffsetColumn() const { return offsetColumn.get(); }

private:
    friend class StringColumn;

    // Offsets to scan should be a sorted list of pairs mapping the index of the entry in the string
    // dictionary (as read from the index column) to the output index in the result vector to store
    // the string.
    void scan(const SegmentState& offsetState, const SegmentState& dataState,
        std::vector<std::pair<DictionaryChunk::string_index_t, uint64_t>>& offsetsToScan,
        common::ValueVector* result, const ColumnChunkMetadata& indexMeta) const;

    // Materializes unique source dictionary indexes into a StringChunkData dictionary.
    // The input indexes identify entries in the persistent source dictionary. The function may
    // reorder them for locality, appends the corresponding string bytes and offsets to the result
    // dictionary, and returns the actual old-index to new-index mapping. It does not write row
    // indexes; callers that know result row positions must update the StringChunkData index chunk.
    std::vector<std::pair<DictionaryChunk::string_index_t, DictionaryChunk::string_index_t>>
    materializeToStringChunkDictionary(const SegmentState& offsetState,
        const SegmentState& dataState, std::vector<DictionaryChunk::string_index_t>& indexesToScan,
        StringChunkData& result, const ColumnChunkMetadata& indexMeta) const;

    void scanOffsets(const SegmentState& state, DictionaryChunk::string_offset_t* offsets,
        uint64_t index, uint64_t numValues, uint64_t dataSize) const;
    void scanValue(const SegmentState& dataState, uint64_t startOffset, uint64_t endOffset,
        common::ValueVector* resultVector, uint64_t offsetInVector) const;
    DictionaryChunk::string_index_t appendScannedValueToDictionary(const SegmentState& dataState,
        uint64_t startOffset, uint64_t length, StringChunkData& result) const;

    static bool canDataCommitInPlace(const SegmentState& dataState,
        uint64_t totalStringLengthToAdd);
    bool canOffsetCommitInPlace(const SegmentState& offsetState, const SegmentState& dataState,
        uint64_t numNewStrings, uint64_t totalStringLengthToAdd) const;

private:
    // The offset column stores the offsets for each index, and the data column stores the data in
    // order. Values are never removed from the dictionary during in-place updates, only appended to
    // the end.
    std::unique_ptr<Column> dataColumn;
    std::unique_ptr<Column> offsetColumn;
};

} // namespace storage
} // namespace lbug
