#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/assert.h"
#include "common/exception/runtime.h"
#include "common/serializer/serializer.h"
#include "common/serializer/writer.h"
#include "common/types/types.h"
#include "storage/file_handle.h"
#include "storage/page_range.h"
#include "storage/shadow_file.h"
#include "storage/shadow_utils.h"
#include <span>

namespace lbug::storage {

inline uint64_t getArtVarUintSize(uint64_t value) {
    auto size = 1u;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

inline void writeArtVarUint(common::Serializer& serializer, uint64_t value) {
    while (value >= 0x80) {
        const auto byte = static_cast<uint8_t>(value | 0x80);
        serializer.write(&byte, 1);
        value >>= 7;
    }
    const auto byte = static_cast<uint8_t>(value);
    serializer.write(&byte, 1);
}

template<class READER>
uint64_t readArtVarUint(READER& reader) {
    uint64_t result = 0;
    auto shift = 0u;
    while (true) {
        uint8_t byte = 0;
        reader.read(&byte, 1);
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return result;
        }
        shift += 7;
    }
}

class ArtPageRangeWriter final : public common::Writer {
public:
    ArtPageRangeWriter(PageRange pageRange, FileHandle& fileHandle, ShadowFile& shadowFile)
        : pageRange{pageRange}, fileHandle{fileHandle}, shadowFile{shadowFile} {}

    ~ArtPageRangeWriter() override { flush(); }

    void write(const uint8_t* data, uint64_t size) override {
        auto remaining = size;
        while (remaining > 0) {
            ensurePagePinned();
            const auto pageOffset = bytesWritten % common::LBUG_PAGE_SIZE;
            const auto numBytesToCopy =
                std::min<uint64_t>(remaining, common::LBUG_PAGE_SIZE - pageOffset);
            std::memcpy(currentPage.frame + pageOffset, data + (size - remaining), numBytesToCopy);
            bytesWritten += numBytesToCopy;
            remaining -= numBytesToCopy;
            if (bytesWritten % common::LBUG_PAGE_SIZE == 0) {
                unpinCurrentPage();
            }
        }
    }

    void clear() override { UNREACHABLE_CODE; }

    void flush() override {
        if (!hasPinnedPage) {
            return;
        }
        const auto pageOffset = bytesWritten % common::LBUG_PAGE_SIZE;
        if (pageOffset != 0) {
            std::memset(currentPage.frame + pageOffset, 0, common::LBUG_PAGE_SIZE - pageOffset);
        }
        unpinCurrentPage();
    }

    void sync() override { fileHandle.flushAllDirtyPagesInFrames(); }

    uint64_t getSize() const override { return bytesWritten; }

private:
    void ensurePagePinned() {
        if (hasPinnedPage) {
            return;
        }
        const auto pageOffset = bytesWritten / common::LBUG_PAGE_SIZE;
        DASSERT(pageOffset < pageRange.numPages);
        currentPage = ShadowUtils::createShadowVersionIfNecessaryAndPinPage(pageRange.startPageIdx +
                                                                                pageOffset,
            true /*writing all page bytes; no need to read original*/, fileHandle, shadowFile);
        hasPinnedPage = true;
    }

    void unpinCurrentPage() {
        shadowFile.getShadowingFH().unpinPage(currentPage.shadowPage);
        hasPinnedPage = false;
    }

private:
    PageRange pageRange;
    FileHandle& fileHandle;
    ShadowFile& shadowFile;
    uint64_t bytesWritten = 0;
    ShadowPageAndFrame currentPage{common::INVALID_PAGE_IDX, common::INVALID_PAGE_IDX, nullptr};
    bool hasPinnedPage = false;
};

class ArtPageRangeReader {
public:
    ArtPageRangeReader(FileHandle& fileHandle, PageRange pageRange, uint64_t size)
        : fileHandle{fileHandle}, pageRange{pageRange}, size{size} {}

    void read(uint8_t* data, uint64_t numBytes) {
        if (offset + numBytes > size) {
            throw common::RuntimeException("Cannot read past the end of disk-backed ART storage.");
        }
        auto remaining = numBytes;
        while (remaining > 0) {
            const auto absoluteOffset = pageRange.startPageIdx * common::LBUG_PAGE_SIZE + offset;
            const auto pageIdx = absoluteOffset / common::LBUG_PAGE_SIZE;
            const auto pageOffset = absoluteOffset % common::LBUG_PAGE_SIZE;
            const auto numBytesToCopy =
                std::min<uint64_t>(remaining, common::LBUG_PAGE_SIZE - pageOffset);
            auto* frame = fileHandle.pinPage(pageIdx, PageReadPolicy::READ_PAGE);
            std::memcpy(data + (numBytes - remaining), frame + pageOffset, numBytesToCopy);
            fileHandle.unpinPage(pageIdx);
            offset += numBytesToCopy;
            remaining -= numBytesToCopy;
        }
    }

    void skip(uint64_t numBytes) {
        if (offset + numBytes > size) {
            throw common::RuntimeException("Cannot skip past the end of disk-backed ART storage.");
        }
        offset += numBytes;
    }

    uint64_t getOffset() const { return offset; }

    void setOffset(uint64_t offset_) {
        if (offset_ > size) {
            throw common::RuntimeException("Cannot seek past the end of disk-backed ART storage.");
        }
        offset = offset_;
    }

private:
    FileHandle& fileHandle;
    PageRange pageRange;
    uint64_t size;
    uint64_t offset = 0;
};

class ArtMemoryReader {
public:
    explicit ArtMemoryReader(std::span<const uint8_t> bytes) : bytes{bytes} {}

    void read(uint8_t* data, uint64_t numBytes) {
        if (offset + numBytes > bytes.size()) {
            throw common::RuntimeException("Cannot read past the end of in-memory ART storage.");
        }
        std::memcpy(data, bytes.data() + offset, numBytes);
        offset += numBytes;
    }

    void skip(uint64_t numBytes) {
        if (offset + numBytes > bytes.size()) {
            throw common::RuntimeException("Cannot skip past the end of in-memory ART storage.");
        }
        offset += numBytes;
    }

private:
    std::span<const uint8_t> bytes;
    uint64_t offset = 0;
};

struct ArtDiskNodeHeader {
    std::vector<uint8_t> prefix;
    std::vector<common::offset_t> offsets;
    uint64_t numChildren = 0;
};

template<class READER>
ArtDiskNodeHeader readArtDiskNodeHeader(READER& reader) {
    ArtDiskNodeHeader header;
    const auto prefixSize = readArtVarUint(reader);
    header.prefix.resize(prefixSize);
    if (prefixSize > 0) {
        reader.read(header.prefix.data(), prefixSize);
    }
    const auto numOffsets = readArtVarUint(reader);
    header.offsets.reserve(numOffsets);
    for (auto i = 0u; i < numOffsets; ++i) {
        header.offsets.push_back(readArtVarUint(reader));
    }
    header.numChildren = readArtVarUint(reader);
    return header;
}

template<class READER>
void skipArtDiskTree(READER& reader) {
    const auto prefixSize = readArtVarUint(reader);
    reader.skip(prefixSize);
    const auto numOffsets = readArtVarUint(reader);
    for (auto i = 0u; i < numOffsets; ++i) {
        readArtVarUint(reader);
    }
    const auto numChildren = readArtVarUint(reader);
    for (auto i = 0u; i < numChildren; ++i) {
        uint8_t byte = 0;
        reader.read(&byte, 1);
        uint64_t childSize = 0;
        reader.read(reinterpret_cast<uint8_t*>(&childSize), sizeof(childSize));
        reader.skip(childSize);
    }
}

} // namespace lbug::storage
