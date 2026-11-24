#include "storage/buffer_manager/memory_manager.h"

#include <algorithm>
#include <mutex>

#include "common/exception/buffer_manager.h"
#include "common/file_system/virtual_file_system.h"
#include "common/types/types.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/buffer_manager/buffer_manager.h"
#include "storage/file_handle.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

MemoryBuffer::MemoryBuffer(MemoryManager* mm, page_idx_t pageIdx, uint8_t* buffer, uint64_t size)
    : buffer{buffer, static_cast<size_t>(size)}, mm{mm}, pageIdx{pageIdx}, evicted{false} {}

MemoryBuffer::~MemoryBuffer() {
    if (buffer.data() != nullptr && !evicted) {
        if (pageIdx != INVALID_PAGE_IDX || !mm->keepForReuse(buffer)) {
            mm->freeBlock(pageIdx, buffer);
            mm->updateUsedMemoryForFreedBlock(pageIdx, buffer);
        }
        buffer = std::span<uint8_t>();
    }
}

SpillResult MemoryBuffer::setSpilledToDisk(uint64_t filePosition) {
    mm->freeBlock(pageIdx, buffer);
    // reinterpret_cast isn't allowed here, but we shouldn't leave the invalid pointer and
    // still want to store the size
    buffer = std::span(static_cast<uint8_t*>(nullptr), buffer.size());
    evicted = true;
    this->filePosition = filePosition;
    if (pageIdx == INVALID_PAGE_IDX) {
        return SpillResult{buffer.size(), 0};
    } else {
        return SpillResult{0, buffer.size()};
    }
}

void MemoryBuffer::prepareLoadFromDisk() {
    DASSERT(buffer.data() == nullptr && evicted);
    buffer = mm->mallocBuffer(false, buffer.size());
    evicted = false;
}

MemoryManager::MemoryManager(BufferManager* bm, VirtualFileSystem* vfs) : bm{bm} {
    pageSize = TEMP_PAGE_SIZE;
    fh = bm->getFileHandle("mm-256KB", FileHandle::O_IN_MEM_TEMP_FILE, vfs, nullptr);
    // keepForReuse runs in a destructor, so it must not allocate.
    reusableBuffers.reserve(MAX_REUSABLE_MEMORY / REUSABLE_BUFFER_SIZE);
}

MemoryManager::~MemoryManager() {
    releaseReusableBuffers();
}

std::span<uint8_t> MemoryManager::mallocBuffer(bool initializeToZero, uint64_t size) {
    // Retry only if cached memory was actually released, so fail-once injection
    // (fail the first reserve during commit) still fails deterministically.
    if (!bm->reserve(size)) {
        bool released = releaseReusableBuffers();
        if (!released || !bm->reserve(size)) {
            throw BufferManagerException(
                "Unable to allocate memory! The buffer pool is full and no memory could be freed!");
        }
    }
    void* buffer = nullptr;
    bm->nonEvictableMemory += size;
    if (initializeToZero) {
        buffer = calloc(size, 1);
    } else {
        buffer = malloc(size);
    }
    return std::span(static_cast<uint8_t*>(buffer), size);
}

std::unique_ptr<MemoryBuffer> MemoryManager::allocateBuffer(bool initializeToZero, uint64_t size) {
    if (size != TEMP_PAGE_SIZE) [[unlikely]] {
        if (size == REUSABLE_BUFFER_SIZE) {
            if (auto* reused = takeReusableBuffer()) {
                if (initializeToZero) {
                    memset(reused, 0, size);
                }
                return std::make_unique<MemoryBuffer>(this, INVALID_PAGE_IDX, reused, size);
            }
        }
        auto buffer = mallocBuffer(initializeToZero, size);
        return std::make_unique<MemoryBuffer>(this, INVALID_PAGE_IDX, buffer.data(), size);
    }
    page_idx_t pageIdx = INVALID_PAGE_IDX;
    {
        std::scoped_lock<std::mutex> lock(allocatorLock);
        if (freePages.empty()) {
            pageIdx = fh->addNewPage();
        } else {
            pageIdx = freePages.top();
            freePages.pop();
        }
    }
    uint8_t* buffer = nullptr;
    try {
        buffer = bm->pin(*fh, pageIdx, PageReadPolicy::DONT_READ_PAGE);
    } catch (const BufferManagerException&) {
        // The pin failed to reserve memory; kept buffers may free enough. Retry
        // only if something was actually released, so fail-once injection during
        // commit still fails deterministically.
        // Note: pin consumed pageIdx from freePages (or added new page); on failure
        // return it for reuse to avoid leaking the page index.
        {
            std::scoped_lock<std::mutex> lock(allocatorLock);
            freePages.push(pageIdx);
        }
        if (!releaseReusableBuffers()) {
            throw;
        }
        // Retry with a fresh page index from the pool.
        {
            std::scoped_lock<std::mutex> lock(allocatorLock);
            if (freePages.empty()) {
                pageIdx = fh->addNewPage();
            } else {
                pageIdx = freePages.top();
                freePages.pop();
            }
        }
        buffer = bm->pin(*fh, pageIdx, PageReadPolicy::DONT_READ_PAGE);
    }
    auto memoryBuffer = std::make_unique<MemoryBuffer>(this, pageIdx, buffer);
    if (initializeToZero) {
        memset(memoryBuffer->getBuffer().data(), 0, pageSize);
    }
    return memoryBuffer;
}

std::unique_ptr<MemoryBuffer> MemoryManager::reallocateBuffer(
    std::unique_ptr<MemoryBuffer> oldBuffer, bool initializeToZero, uint64_t newSize) {
    // Only the malloc path (INVALID_PAGE_IDX, non-TEMP_PAGE_SIZE, non-reusable)
    // can use realloc. Pool pages (TEMP_PAGE_SIZE) and reusable 1MiB buffers
    // use different allocators; fall back to allocate+copy for those (small).
    auto oldSpan = oldBuffer->getBuffer();
    const auto oldSize = oldSpan.size_bytes();
    const auto oldPageIdx = oldBuffer->pageIdx;
    if (oldPageIdx != INVALID_PAGE_IDX || newSize == common::TEMP_PAGE_SIZE ||
        oldSize == common::TEMP_PAGE_SIZE || newSize == REUSABLE_BUFFER_SIZE ||
        oldSize == REUSABLE_BUFFER_SIZE) {
        auto newBuffer = allocateBuffer(false /*initializeToZero*/, newSize);
        // Explicit template argument: size_bytes() is std::size_t (unsigned long on macOS),
        // while newSize is uint64_t (unsigned long long there), so deduction fails.
        const auto copySize = std::min<uint64_t>(oldSize, newSize);
        if (copySize > 0) {
            memcpy(newBuffer->getBuffer().data(), oldSpan.data(), copySize);
        }
        if (initializeToZero && newSize > copySize) {
            memset(newBuffer->getBuffer().data() + copySize, 0, newSize - copySize);
        } else if (newSize > oldSize) {
            // Preserve ColumnChunkData::resize semantics (zero new tail).
            memset(newBuffer->getBuffer().data() + oldSize, 0, newSize - oldSize);
        }
        return newBuffer;
    }
    if (newSize <= oldSize) {
        return oldBuffer;
    }
    const auto delta = newSize - oldSize;
    // Retry only if cached memory was actually released, so fail-once injection
    // (fail the first reserve during commit) still fails deterministically.
    if (!bm->reserve(delta)) {
        bool released = releaseReusableBuffers();
        if (!released || !bm->reserve(delta)) {
            throw BufferManagerException(
                "Unable to allocate memory! The buffer pool is full and no memory could be freed!");
        }
    }
    // Release ownership without freeing: realloc takes over oldPtr (frees it
    // if it moves, keeps it if it extends in place). Prevent double-free.
    uint8_t* oldPtr = oldSpan.data();
    oldBuffer->preventDestruction();
    // oldBuffer wrapper destroyed here (no free due to preventDestruction).
    oldBuffer.reset();
    void* newPtr = std::realloc(oldPtr, newSize);
    if (newPtr == nullptr) {
        bm->freeUsedMemory(delta);
        bm->nonEvictableMemory -= delta;
        if (oldPtr != nullptr) {
            auto rollback = std::make_unique<MemoryBuffer>(this, INVALID_PAGE_IDX, oldPtr, oldSize);
            (void)rollback;
        }
        throw BufferManagerException(
            "Unable to allocate memory! The buffer pool is full and no memory could be freed!");
    }
    bm->nonEvictableMemory += delta;
    auto newBuffer = std::make_unique<MemoryBuffer>(this, INVALID_PAGE_IDX,
        static_cast<uint8_t*>(newPtr), newSize);
    if (initializeToZero || newSize > oldSize) {
        memset(static_cast<uint8_t*>(newPtr) + oldSize, 0, delta);
    }
    return newBuffer;
}

void MemoryManager::freeBlock(page_idx_t pageIdx, std::span<uint8_t> buffer) {
    if (pageIdx == INVALID_PAGE_IDX) {
        std::free(buffer.data());
    } else {
        bm->unpin(*fh, pageIdx);
    }
}

void MemoryManager::updateUsedMemoryForFreedBlock(page_idx_t pageIdx, std::span<uint8_t> buffer) {
    if (pageIdx == INVALID_PAGE_IDX) {
        bm->freeUsedMemory(buffer.size());
        bm->nonEvictableMemory -= buffer.size();
    } else {
        std::unique_lock<std::mutex> lock(allocatorLock);
        freePages.push(pageIdx);
    }
}

uint64_t MemoryManager::getMaxReusableBuffers() const {
    return std::min(MAX_REUSABLE_MEMORY, bm->getMemoryLimit() / 64) / REUSABLE_BUFFER_SIZE;
}

uint64_t MemoryManager::getNumReusableBuffers() {
    std::scoped_lock lock{allocatorLock};
    return reusableBuffers.size();
}

uint8_t* MemoryManager::takeReusableBuffer() {
    std::scoped_lock lock{allocatorLock};
    if (reusableBuffers.empty()) {
        return nullptr;
    }
    auto* buffer = reusableBuffers.back();
    reusableBuffers.pop_back();
    return buffer;
}

bool MemoryManager::keepForReuse(std::span<uint8_t> buffer) {
    if (buffer.size() != REUSABLE_BUFFER_SIZE) {
        return false;
    }
    const auto maxReusableBuffers = getMaxReusableBuffers();
    std::scoped_lock lock{allocatorLock};
    if (reusableBuffers.size() >= maxReusableBuffers) {
        return false;
    }
    // The constructor reserved room for the most buffers getMaxReusableBuffers allows.
    DASSERT(reusableBuffers.size() < reusableBuffers.capacity());
    reusableBuffers.push_back(buffer.data());
    return true;
}

bool MemoryManager::releaseReusableBuffers() {
    std::scoped_lock lock{allocatorLock};
    const auto released = !reusableBuffers.empty();
    for (auto* buffer : reusableBuffers) {
        const auto span = std::span(buffer, REUSABLE_BUFFER_SIZE);
        freeBlock(INVALID_PAGE_IDX, span);
        updateUsedMemoryForFreedBlock(INVALID_PAGE_IDX, span);
    }
    // clear() keeps the capacity reserved in the constructor.
    reusableBuffers.clear();
    return released;
}

MemoryManager* MemoryManager::Get(const main::ClientContext& context) {
    return context.getDatabase()->getMemoryManager();
}

} // namespace storage
} // namespace lbug
