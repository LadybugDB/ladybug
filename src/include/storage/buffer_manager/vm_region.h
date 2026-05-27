#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "common/constants.h"
#include "common/types/types.h"

namespace lbug {
namespace storage {

// A VMRegion holds a virtual memory region of a certain size allocated through mmap.
// The region is divided into frame groups, each of which is a group of frames of the same size.
// Each FileHandle should grab a frame group each time when they add a new file page group (see
// `FileHandle::addNewPageGroupWithoutLock`). In this way, each file page group uniquely
// corresponds to a frame group, thus, a page also uniquely corresponds to a frame in a VMRegion.
class VMRegion {
    friend class BufferManager;

public:
    explicit VMRegion(common::PageSizeClass pageSizeClass, uint64_t maxRegionSize);
    ~VMRegion();

    common::frame_group_idx_t addNewFrameGroup();

    // Mark the frame as resident. Returns the number of bytes that should be charged to the
    // buffer pool for this frame.
    uint64_t claimFrame(common::frame_idx_t frameIdx);

    // Use `MADV_DONTNEED` to release physical memory associated with this frame.
    // Returns the number of bytes that should be released from the buffer pool accounting.
    uint64_t releaseFrame(common::frame_idx_t frameIdx);

    // Returns true if the memory address is within the reserved virtual memory region
    bool contains(const uint8_t* address) const {
        return address >= region && address < region + getMaxRegionSize();
    }
    inline uint8_t* getFrame(common::frame_idx_t frameIdx) const {
        return region + (static_cast<std::uint64_t>(frameIdx) * frameSize);
    }

private:
    inline uint64_t getMaxRegionSize() const {
        return maxNumFrameGroups * frameSize * common::StorageConstants::PAGE_GROUP_SIZE;
    }
    inline uint64_t getFrameGroupSize() const {
        return static_cast<uint64_t>(frameSize) * common::StorageConstants::PAGE_GROUP_SIZE;
    }
    uint64_t getDiscardGranuleIdxInFrameGroup(common::frame_idx_t frameIdx) const;
    uint64_t getFrameGroupIdx(common::frame_idx_t frameIdx) const;

private:
    std::mutex mtx;
    uint8_t* region;
    uint32_t frameSize;
    uint64_t discardGranuleSize;
    uint64_t numDiscardGranulesPerFrameGroup;
    uint64_t numFrameGroups;
    uint64_t maxNumFrameGroups;
    std::vector<std::unique_ptr<std::atomic<uint16_t>[]>> residentFramesPerDiscardGranule;
};

} // namespace storage
} // namespace lbug
