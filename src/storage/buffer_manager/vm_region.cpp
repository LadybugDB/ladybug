#include "storage/buffer_manager/vm_region.h"

#include <algorithm>

#include "common/system_config.h"
#include "common/system_message.h"
#include <format>

#ifdef _WIN32
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "common/assert.h"
#include "common/exception/buffer_manager.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

using namespace lbug::common;

namespace lbug {
namespace storage {

VMRegion::VMRegion(PageSizeClass pageSizeClass, uint64_t maxRegionSize) : numFrameGroups{0} {
    if (maxRegionSize > static_cast<std::size_t>(-1)) {
        throw BufferManagerException("maxRegionSize is beyond the max available mmap region size.");
    }
    frameSize = pageSizeClass == REGULAR_PAGE ? LBUG_PAGE_SIZE : TEMP_PAGE_SIZE;
    discardGranuleSize = frameSize;
#ifndef _WIN32
    const auto osPageSize = sysconf(_SC_PAGESIZE);
    if (osPageSize <= 0) {
        throw BufferManagerException("Failed to detect the operating system page size.");
    }
    discardGranuleSize = std::max<uint64_t>(frameSize, static_cast<uint64_t>(osPageSize));
#endif
    if (discardGranuleSize % frameSize != 0 || getFrameGroupSize() % discardGranuleSize != 0) {
        throw BufferManagerException(std::format(
            "Unsupported page size combination: frame size {}, discard granule size {}, frame "
            "group size {}.",
            frameSize, discardGranuleSize, getFrameGroupSize()));
    }
    numDiscardGranulesPerFrameGroup = getFrameGroupSize() / discardGranuleSize;
    const auto numBytesForFrameGroup = frameSize * StorageConstants::PAGE_GROUP_SIZE;
    maxNumFrameGroups = (maxRegionSize + numBytesForFrameGroup - 1) / numBytesForFrameGroup;
#ifdef _WIN32
    region = (uint8_t*)VirtualAlloc(NULL, getMaxRegionSize(), MEM_RESERVE, PAGE_READWRITE);
    if (region == NULL) {
        throw BufferManagerException(std::format(
            "VirtualAlloc for size {} failed with error code {}: {}.", getMaxRegionSize(),
            GetLastError(), std::system_category().message(GetLastError())));
    }
#else
    // Create a private anonymous mapping. The mapping is not shared with other processes and not
    // backed by any file, and its content are initialized to zero.
    region = static_cast<uint8_t*>(mmap(NULL, getMaxRegionSize(), PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1 /* fd */, 0 /* offset */));
    if (region == MAP_FAILED) {
        throw BufferManagerException(
            "Mmap for size " + std::to_string(getMaxRegionSize()) + " failed.");
    }
#ifdef MADV_DONTDUMP
    // Tell the kernel not to include this virtual reservation in core dumps.
    // Without this, the kernel walks the entire (potentially multi-TB) sparse
    // reservation during crash dump generation, turning every SIGSEGV into an
    // unkillable process stuck at 100% CPU in the dump path for minutes.
    // MADV_DONTDUMP is available on Linux >= 3.4; macOS has no equivalent.
    madvise(region, getMaxRegionSize(), MADV_DONTDUMP);
#endif
#endif
}

VMRegion::~VMRegion() {
#ifdef _WIN32
    VirtualFree(region, 0, MEM_RELEASE);
#else
    munmap(region, getMaxRegionSize());
#endif
}

uint64_t VMRegion::claimFrame(frame_idx_t frameIdx) {
    const auto frameGroupIdx = getFrameGroupIdx(frameIdx);
    DASSERT(frameGroupIdx < residentFramesPerDiscardGranule.size());
    const auto granuleIdx = getDiscardGranuleIdxInFrameGroup(frameIdx);
    auto& numResidentFrames = residentFramesPerDiscardGranule[frameGroupIdx][granuleIdx];
    const auto previousNumResidentFrames = numResidentFrames.fetch_add(1);
    DASSERT(previousNumResidentFrames < discardGranuleSize / frameSize);
    return previousNumResidentFrames == 0 ? discardGranuleSize : 0;
}

uint64_t VMRegion::releaseFrame(frame_idx_t frameIdx) {
    const auto frameGroupIdx = getFrameGroupIdx(frameIdx);
    DASSERT(frameGroupIdx < residentFramesPerDiscardGranule.size());
    const auto granuleIdx = getDiscardGranuleIdxInFrameGroup(frameIdx);
    auto& numResidentFrames = residentFramesPerDiscardGranule[frameGroupIdx][granuleIdx];
    const auto previousNumResidentFrames = numResidentFrames.fetch_sub(1);
    DASSERT(previousNumResidentFrames > 0);
    if (previousNumResidentFrames != 1) {
        return 0;
    }
    auto* discardGranule =
        region + frameGroupIdx * getFrameGroupSize() + granuleIdx * discardGranuleSize;
#ifdef _WIN32
    // TODO: VirtualAlloc(..., MEM_RESET, ...) may be faster
    // See https://arvid.io/2018/04/02/memory-mapping-on-windows/#1
    // Not sure what the differences are
    if (!VirtualFree(discardGranule, discardGranuleSize, MEM_DECOMMIT)) {
        auto code = GetLastError();
        throw BufferManagerException(std::format(
            "Releasing physical memory associated with a frame failed with error code {}: {}.",
            code, systemErrMessage(code)));
    }

#else
    int error = madvise(discardGranule, discardGranuleSize, MADV_DONTNEED);
    if (error != 0) {
        // LCOV_EXCL_START
        throw BufferManagerException(std::format(
            "Releasing physical memory associated with a frame failed with error code {}: {}.",
            error, posixErrMessage()));
        // LCOV_EXCL_STOP
    }
#endif
    return discardGranuleSize;
}

frame_group_idx_t VMRegion::addNewFrameGroup() {
    std::unique_lock xLck{mtx};
    if (numFrameGroups >= maxNumFrameGroups) {
        // LCOV_EXCL_START
        throw BufferManagerException(
            std::format("Maximum database size of {} bytes has been reached. "
                        "Reopen the database with a larger max_db_size to continue.",
                getMaxRegionSize()));
        // LCOV_EXCL_STOP
    }
    auto residentFrameCounts =
        std::make_unique<std::atomic<uint16_t>[]>(numDiscardGranulesPerFrameGroup);
    for (auto i = 0u; i < numDiscardGranulesPerFrameGroup; ++i) {
        residentFrameCounts[i].store(0);
    }
    residentFramesPerDiscardGranule.push_back(std::move(residentFrameCounts));
    return numFrameGroups++;
}

uint64_t VMRegion::getFrameGroupIdx(frame_idx_t frameIdx) const {
    return frameIdx >> StorageConstants::PAGE_GROUP_SIZE_LOG2;
}

uint64_t VMRegion::getDiscardGranuleIdxInFrameGroup(frame_idx_t frameIdx) const {
    const auto frameIdxInGroup = frameIdx & StorageConstants::PAGE_IDX_IN_GROUP_MASK;
    return frameIdxInGroup * static_cast<uint64_t>(frameSize) / discardGranuleSize;
}

} // namespace storage
} // namespace lbug
