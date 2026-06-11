#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/serializer.h"
#include "storage/index/art_index.h"
#include "storage/index/art_index_disk_utils.h"
#include "storage/page_allocator.h"
#include "storage/storage_manager.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

uint64_t ArtPrimaryKeyIndex::calculateSerializedTreeSize(const Node& node) const {
    uint64_t numOffsets = node.offset.has_value() ? 1 : 0;
    if (node.overflowOffsets) {
        numOffsets += node.overflowOffsets->size();
    }
    auto size = getArtVarUintSize(node.prefix.size()) + node.prefix.size() +
                getArtVarUintSize(numOffsets) + getArtVarUintSize(node.count);
    if (node.offset.has_value()) {
        size += getArtVarUintSize(node.offset.value());
    }
    if (node.overflowOffsets) {
        for (const auto offset : *node.overflowOffsets) {
            size += getArtVarUintSize(offset);
        }
    }
    auto addChild = [&](const Node& child) {
        size += sizeof(uint8_t);
        size += sizeof(uint64_t);
        size += calculateSerializedTreeSize(child);
    };
    switch (node.kind) {
    case Node::Kind::NODE4:
    case Node::Kind::NODE16:
        for (auto i = 0u; i < node.count; ++i) {
            addChild(*node.small.children[i]);
        }
        break;
    case Node::Kind::NODE48:
        for (auto i = 0u; i < node.node48->childIndex.size(); ++i) {
            const auto pos = node.node48->childIndex[i];
            if (pos != Node::EMPTY_MARKER) {
                addChild(*node.node48->children[pos]);
            }
        }
        break;
    case Node::Kind::NODE256:
        for (auto i = 0u; i < node.node256->children.size(); ++i) {
            if (node.node256->children[i]) {
                addChild(*node.node256->children[i]);
            }
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
    return size;
}

void ArtPrimaryKeyIndex::serializeTree(const Node& node, Serializer& serializer) const {
    writeArtVarUint(serializer, node.prefix.size());
    if (!node.prefix.empty()) {
        serializer.write(node.prefix.data(), node.prefix.size());
    }
    uint64_t numOffsets = node.offset.has_value() ? 1 : 0;
    if (node.overflowOffsets) {
        numOffsets += node.overflowOffsets->size();
    }
    writeArtVarUint(serializer, numOffsets);
    if (node.offset.has_value()) {
        writeArtVarUint(serializer, node.offset.value());
    }
    if (node.overflowOffsets) {
        for (const auto offset : *node.overflowOffsets) {
            writeArtVarUint(serializer, offset);
        }
    }
    writeArtVarUint(serializer, node.count);
    auto writeChild = [&](uint8_t byte, const Node& child) {
        serializer.write(&byte, 1);
        const auto childSize = calculateSerializedTreeSize(child);
        serializer.write<uint64_t>(childSize);
        serializeTree(child, serializer);
    };
    switch (node.kind) {
    case Node::Kind::NODE4:
    case Node::Kind::NODE16:
        for (auto i = 0u; i < node.count; ++i) {
            writeChild(node.small.keys[i], *node.small.children[i]);
        }
        break;
    case Node::Kind::NODE48:
        for (auto i = 0u; i < node.node48->childIndex.size(); ++i) {
            const auto pos = node.node48->childIndex[i];
            if (pos != Node::EMPTY_MARKER) {
                writeChild(static_cast<uint8_t>(i), *node.node48->children[pos]);
            }
        }
        break;
    case Node::Kind::NODE256:
        for (auto i = 0u; i < node.node256->children.size(); ++i) {
            if (node.node256->children[i]) {
                writeChild(static_cast<uint8_t>(i), *node.node256->children[i]);
            }
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
}

std::vector<uint8_t> ArtPrimaryKeyIndex::serializeTreeToBytes() const {
    std::lock_guard lck{mutex};
    if (diskBacked) {
        const auto* fileHandle = diskFileHandle;
        if (fileHandle == nullptr || diskTreeSize == 0) {
            return {};
        }
        std::vector<uint8_t> bytes(diskTreeSize);
        ArtPageRangeReader reader(const_cast<FileHandle&>(*fileHandle), diskTreePageRange,
            diskTreeSize);
        reader.read(bytes.data(), bytes.size());
        return bytes;
    }
    auto writer = std::make_shared<BufferWriter>(calculateSerializedTreeSize(root));
    Serializer serializer(writer);
    serializeTree(root, serializer);
    const auto size = writer->getSize();
    std::vector<uint8_t> bytes(size);
    memcpy(bytes.data(), writer->getBlobData(), size);
    return bytes;
}

uint64_t ArtPrimaryKeyIndex::getSerializedTreeSize() const {
    std::lock_guard lck{mutex};
    return diskBacked ? diskTreeSize : calculateSerializedTreeSize(root);
}

void ArtPrimaryKeyIndex::checkpoint(main::ClientContext*, PageAllocator& pageAllocator,
    ShadowFile& shadowFile) {
    std::lock_guard lck{mutex};
    hasCheckpointRollbackState = false;
    if (diskBacked) {
        return;
    }
    auto& artStorageInfo = storageInfo->cast<ArtPrimaryKeyIndexStorageInfo>();
    checkpointRollbackTreePageRange = artStorageInfo.treePageRange;
    checkpointRollbackTreeSize = artStorageInfo.treeSize;
    hasCheckpointRollbackState = true;

    const auto treeSize = calculateSerializedTreeSize(root);
    const auto numPages = static_cast<page_idx_t>((treeSize + LBUG_PAGE_SIZE - 1) / LBUG_PAGE_SIZE);
    auto pageRange = pageAllocator.allocatePageRange(numPages);
    auto writer =
        std::make_shared<ArtPageRangeWriter>(pageRange, *pageAllocator.getDataFH(), shadowFile);
    auto serializer = Serializer(writer);
    serializeTree(root, serializer);
    writer->flush();

    if (artStorageInfo.treePageRange.startPageIdx != INVALID_PAGE_IDX) {
        pageAllocator.freePageRange(artStorageInfo.treePageRange);
    }
    artStorageInfo.treePageRange = pageRange;
    artStorageInfo.treeSize = treeSize;
    diskTreePageRange = pageRange;
    diskTreeSize = treeSize;
    diskFileHandle = pageAllocator.getDataFH();
}

void ArtPrimaryKeyIndex::rollbackCheckpoint() {
    std::lock_guard lck{mutex};
    if (!hasCheckpointRollbackState) {
        return;
    }
    auto& artStorageInfo = storageInfo->cast<ArtPrimaryKeyIndexStorageInfo>();
    artStorageInfo.treePageRange = checkpointRollbackTreePageRange;
    artStorageInfo.treeSize = checkpointRollbackTreeSize;
    diskTreePageRange = checkpointRollbackTreePageRange;
    diskTreeSize = checkpointRollbackTreeSize;
    hasCheckpointRollbackState = false;
}

void ArtPrimaryKeyIndex::serialize(Serializer& serializer) const {
    std::lock_guard lck{mutex};
    indexInfo.serialize(serializer);
    auto bufferedWriter = storageInfo->serialize();
    serializer.write<uint64_t>(bufferedWriter->getSize());
    serializer.write(bufferedWriter->getData().data.get(), bufferedWriter->getSize());
}

void ArtPrimaryKeyIndex::reclaimStorage(PageAllocator& pageAllocator) const {
    const auto& artStorageInfo = storageInfo->constCast<ArtPrimaryKeyIndexStorageInfo>();
    if (artStorageInfo.treePageRange.startPageIdx != INVALID_PAGE_IDX) {
        pageAllocator.freePageRange(artStorageInfo.treePageRange);
    }
}

std::vector<IndexStorageEntry> ArtPrimaryKeyIndex::getStorageEntries() const {
    std::lock_guard lck{mutex};
    const auto& artStorageInfo = storageInfo->constCast<ArtPrimaryKeyIndexStorageInfo>();
    if (artStorageInfo.treePageRange.startPageIdx == INVALID_PAGE_IDX) {
        return {};
    }
    return {IndexStorageEntry{"tree", artStorageInfo.treePageRange, artStorageInfo.treeSize}};
}

void ArtPrimaryKeyIndex::loadEntries(const ArtPrimaryKeyIndexStorageInfo& storageInfo) {
    static constexpr auto alwaysVisible = [](offset_t) { return true; };
    for (const auto& [keyBytes, offset] : storageInfo.entries) {
        if (indexInfo.isPrimary) {
            insertInternal(ArtKey{keyBytes}, offset, alwaysVisible);
        } else {
            insertSecondaryInternal(ArtKey{keyBytes}, offset);
        }
    }
}

void ArtPrimaryKeyIndex::materializeDiskTree() {
    if (!diskBacked) {
        return;
    }
    DASSERT(diskFileHandle != nullptr);
    ArtPageRangeReader reader{*diskFileHandle, diskTreePageRange, diskTreeSize};
    loadTree(reader, root);
    diskBacked = false;
    diskFileHandle = nullptr;
}

template<class READER>
void ArtPrimaryKeyIndex::loadTree(READER& reader, Node& node) {
    resetNodePayload(node);
    const auto prefixSize = readArtVarUint(reader);
    node.prefix.resize(prefixSize);
    if (prefixSize > 0) {
        reader.read(node.prefix.data(), prefixSize);
    }
    const auto numOffsets = readArtVarUint(reader);
    if (numOffsets > 0) {
        node.offset = readArtVarUint(reader);
    }
    if (numOffsets > 1) {
        node.overflowOffsets = std::make_unique<std::vector<offset_t>>();
        node.overflowOffsets->reserve(numOffsets - 1);
        for (auto i = 1u; i < numOffsets; ++i) {
            node.overflowOffsets->push_back(readArtVarUint(reader));
        }
    }
    const auto numChildren = readArtVarUint(reader);
    for (auto i = 0u; i < numChildren; ++i) {
        uint8_t byte = 0;
        reader.read(&byte, 1);
        uint64_t childSize = 0;
        reader.read(reinterpret_cast<uint8_t*>(&childSize), sizeof(childSize));
        auto* child = allocateNode();
        node.insertChild(*this, byte, child);
        loadTree(reader, *child);
    }
}

std::unique_ptr<Index> ArtPrimaryKeyIndex::load(main::ClientContext*,
    StorageManager* storageManager, IndexInfo indexInfo, std::span<uint8_t> storageInfoBuffer) {
    validateIndexInfo(indexInfo);
    auto storageInfoBufferReader =
        std::make_unique<BufferReader>(storageInfoBuffer.data(), storageInfoBuffer.size());
    auto storageInfo =
        ArtPrimaryKeyIndexStorageInfo::deserialize(std::move(storageInfoBufferReader));
    auto index = std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo), std::move(storageInfo));
    index->diskFileHandle = storageManager->getDataFH();
    return index;
}

std::unique_ptr<ArtPrimaryKeyIndex> ArtPrimaryKeyIndex::loadFromWAL(main::ClientContext*,
    StorageManager*, IndexInfo indexInfo, std::span<uint8_t> treeBytes) {
    validateIndexInfo(indexInfo);
    auto storageInfo = std::make_unique<ArtPrimaryKeyIndexStorageInfo>();
    auto index = std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo), std::move(storageInfo));
    if (!treeBytes.empty()) {
        ArtMemoryReader reader{treeBytes};
        index->loadTree(reader, index->root);
    }
    return index;
}

} // namespace storage
} // namespace lbug
