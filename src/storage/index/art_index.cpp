#include "storage/index/art_index.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

#include "common/exception/message.h"
#include "common/exception/runtime.h"
#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/types/value/value.h"
#include "common/vector/value_vector.h"
#include "storage/file_handle.h"
#include "storage/page_allocator.h"
#include "storage/shadow_file.h"
#include "storage/shadow_utils.h"
#include "storage/storage_manager.h"
#include <concepts>

using namespace lbug::common;

namespace lbug {
namespace storage {

namespace {

template<typename T>
void appendBigEndian(std::vector<uint8_t>& bytes, T value) {
    using U = std::make_unsigned_t<T>;
    auto unsignedValue = static_cast<U>(value);
    for (auto i = 0u; i < sizeof(T); ++i) {
        const auto shift = (sizeof(T) - i - 1) * 8;
        bytes.push_back(static_cast<uint8_t>(unsignedValue >> shift));
    }
}

template<typename T>
void appendIntegral(std::vector<uint8_t>& bytes, T value) {
    using U = std::make_unsigned_t<T>;
    auto encoded = static_cast<U>(value);
    if constexpr (std::is_signed_v<T>) {
        encoded ^= (U{1} << (sizeof(T) * 8 - 1));
    }
    appendBigEndian(bytes, encoded);
}

template<typename T>
void appendFloat(std::vector<uint8_t>& bytes, T value) {
    using U = std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t, uint64_t>;
    U encoded = 0;
    std::memcpy(&encoded, &value, sizeof(T));
    const auto signBit = U{1} << (sizeof(T) * 8 - 1);
    encoded = (encoded & signBit) != 0 ? ~encoded : encoded ^ signBit;
    appendBigEndian(bytes, encoded);
}

void appendUInt128(std::vector<uint8_t>& bytes, uint64_t high, uint64_t low) {
    appendBigEndian(bytes, high);
    appendBigEndian(bytes, low);
}

void appendInt128(std::vector<uint8_t>& bytes, int64_t high, uint64_t low) {
    appendUInt128(bytes, static_cast<uint64_t>(high) ^ (uint64_t{1} << 63), low);
}

void appendString(std::vector<uint8_t>& bytes, std::string_view value) {
    for (const auto ch : value) {
        const auto byte = static_cast<uint8_t>(ch);
        if (byte <= 1) {
            bytes.push_back(1);
        }
        bytes.push_back(byte);
    }
    bytes.push_back(0);
}

bool shouldPrintDestructorStats() {
    const auto* value = std::getenv("LBUG_ART_INDEX_DESTRUCTOR_STATS");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

uint64_t getVarUintSize(uint64_t value) {
    auto size = 1u;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

void writeVarUint(Serializer& serializer, uint64_t value) {
    while (value >= 0x80) {
        const auto byte = static_cast<uint8_t>(value | 0x80);
        serializer.write(&byte, 1);
        value >>= 7;
    }
    const auto byte = static_cast<uint8_t>(value);
    serializer.write(&byte, 1);
}

template<class READER>
uint64_t readVarUint(READER& reader) {
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

class ArtPageRangeWriter final : public Writer {
public:
    ArtPageRangeWriter(PageRange pageRange, FileHandle& fileHandle, ShadowFile& shadowFile)
        : pageRange{pageRange}, fileHandle{fileHandle}, shadowFile{shadowFile} {}

    ~ArtPageRangeWriter() override { flush(); }

    void write(const uint8_t* data, uint64_t size) override {
        auto remaining = size;
        while (remaining > 0) {
            ensurePagePinned();
            const auto pageOffset = bytesWritten % LBUG_PAGE_SIZE;
            const auto numBytesToCopy = std::min<uint64_t>(remaining, LBUG_PAGE_SIZE - pageOffset);
            std::memcpy(currentPage.frame + pageOffset, data + (size - remaining), numBytesToCopy);
            bytesWritten += numBytesToCopy;
            remaining -= numBytesToCopy;
            if (bytesWritten % LBUG_PAGE_SIZE == 0) {
                unpinCurrentPage();
            }
        }
    }

    void clear() override { UNREACHABLE_CODE; }

    void flush() override {
        if (!hasPinnedPage) {
            return;
        }
        const auto pageOffset = bytesWritten % LBUG_PAGE_SIZE;
        if (pageOffset != 0) {
            std::memset(currentPage.frame + pageOffset, 0, LBUG_PAGE_SIZE - pageOffset);
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
        const auto pageOffset = bytesWritten / LBUG_PAGE_SIZE;
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
    ShadowPageAndFrame currentPage{INVALID_PAGE_IDX, INVALID_PAGE_IDX, nullptr};
    bool hasPinnedPage = false;
};

class ArtPageRangeReader {
public:
    ArtPageRangeReader(FileHandle& fileHandle, PageRange pageRange, uint64_t size)
        : fileHandle{fileHandle}, pageRange{pageRange}, size{size} {}

    void read(uint8_t* data, uint64_t numBytes) {
        if (offset + numBytes > size) {
            throw RuntimeException("Cannot read past the end of disk-backed ART storage.");
        }
        auto remaining = numBytes;
        while (remaining > 0) {
            const auto absoluteOffset = pageRange.startPageIdx * LBUG_PAGE_SIZE + offset;
            const auto pageIdx = absoluteOffset / LBUG_PAGE_SIZE;
            const auto pageOffset = absoluteOffset % LBUG_PAGE_SIZE;
            const auto numBytesToCopy = std::min<uint64_t>(remaining, LBUG_PAGE_SIZE - pageOffset);
            auto* frame = fileHandle.pinPage(pageIdx, PageReadPolicy::READ_PAGE);
            std::memcpy(data + (numBytes - remaining), frame + pageOffset, numBytesToCopy);
            fileHandle.unpinPage(pageIdx);
            offset += numBytesToCopy;
            remaining -= numBytesToCopy;
        }
    }

    void skip(uint64_t numBytes) {
        if (offset + numBytes > size) {
            throw RuntimeException("Cannot skip past the end of disk-backed ART storage.");
        }
        offset += numBytes;
    }

    uint64_t getOffset() const { return offset; }

    void setOffset(uint64_t offset_) {
        if (offset_ > size) {
            throw RuntimeException("Cannot seek past the end of disk-backed ART storage.");
        }
        offset = offset_;
    }

private:
    FileHandle& fileHandle;
    PageRange pageRange;
    uint64_t size;
    uint64_t offset = 0;
};

} // namespace

ArtPrimaryKeyIndex::Node::Node() = default;

ArtPrimaryKeyIndex::NodeBlock::NodeBlock()
    : nodes{static_cast<Node*>(::operator new(sizeof(Node) * NODE_BLOCK_CAPACITY))} {}

ArtPrimaryKeyIndex::NodeBlock::~NodeBlock() {
    for (auto i = 0u; i < used; ++i) {
        nodes[i].~Node();
    }
    ::operator delete(nodes);
}

ArtPrimaryKeyIndex::NodeBlock::NodeBlock(NodeBlock&& other) noexcept
    : nodes{other.nodes}, used{other.used} {
    other.nodes = nullptr;
    other.used = 0;
}

ArtPrimaryKeyIndex::NodeBlock& ArtPrimaryKeyIndex::NodeBlock::operator=(
    NodeBlock&& other) noexcept {
    if (this != &other) {
        for (auto i = 0u; i < used; ++i) {
            nodes[i].~Node();
        }
        ::operator delete(nodes);
        nodes = other.nodes;
        used = other.used;
        other.nodes = nullptr;
        other.used = 0;
    }
    return *this;
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::Node::getChild(uint8_t byte) const {
    switch (kind) {
    case Kind::NODE4:
    case Kind::NODE16: {
        for (auto i = 0u; i < count; ++i) {
            if (small.keys[i] == byte) {
                return small.children[i];
            }
        }
        return nullptr;
    }
    case Kind::NODE48: {
        const auto pos = node48->childIndex[byte];
        return pos == EMPTY_MARKER ? nullptr : node48->children[pos];
    }
    case Kind::NODE256:
        return node256->children[byte];
    default:
        UNREACHABLE_CODE;
    }
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::Node::getOrInsertChild(ArtPrimaryKeyIndex& index,
    uint8_t byte) {
    if (auto* child = getChild(byte)) {
        return child;
    }
    auto* child = index.allocateNode();
    insertChild(index, byte, child);
    return child;
}

void ArtPrimaryKeyIndex::Node::insertChild(ArtPrimaryKeyIndex& index, uint8_t byte, Node* child) {
    switch (kind) {
    case Kind::NODE4:
        if (count == 4) {
            index.recordKindChange(*this, Kind::NODE16);
        }
        break;
    case Kind::NODE16:
        if (count == 16) {
            auto children = std::make_unique<Node48Children>();
            children->childIndex.fill(EMPTY_MARKER);
            for (auto i = 0u; i < count; ++i) {
                children->childIndex[small.keys[i]] = i;
                children->children[i] = small.children[i];
            }
            node48 = std::move(children);
            index.recordKindChange(*this, Kind::NODE48);
        }
        break;
    case Kind::NODE48:
        if (count == 48) {
            auto children = std::make_unique<Node256Children>();
            children->children.fill(nullptr);
            for (auto i = 0u; i < node48->childIndex.size(); ++i) {
                const auto pos = node48->childIndex[i];
                if (pos != EMPTY_MARKER) {
                    children->children[i] = node48->children[pos];
                }
            }
            node48.reset();
            node256 = std::move(children);
            index.recordKindChange(*this, Kind::NODE256);
        }
        break;
    case Kind::NODE256:
        break;
    default:
        UNREACHABLE_CODE;
    }

    switch (kind) {
    case Kind::NODE4:
    case Kind::NODE16: {
        small.keys[count] = byte;
        small.children[count++] = child;
        return;
    }
    case Kind::NODE48: {
        node48->childIndex[byte] = static_cast<uint8_t>(count);
        node48->children[count++] = child;
        return;
    }
    case Kind::NODE256:
        node256->children[byte] = child;
        ++count;
        return;
    default:
        UNREACHABLE_CODE;
    }
}

void ArtPrimaryKeyIndex::Node::removeChild(uint8_t byte) {
    switch (kind) {
    case Kind::NODE4:
    case Kind::NODE16: {
        for (auto i = 0u; i < count; ++i) {
            if (small.keys[i] != byte) {
                continue;
            }
            for (auto j = i + 1; j < count; ++j) {
                small.keys[j - 1] = small.keys[j];
                small.children[j - 1] = small.children[j];
            }
            small.children[count - 1] = nullptr;
            --count;
            return;
        }
        return;
    }
    case Kind::NODE48: {
        const auto removedPos = node48->childIndex[byte];
        if (removedPos == EMPTY_MARKER) {
            return;
        }
        const auto lastPos = count - 1;
        node48->childIndex[byte] = EMPTY_MARKER;
        if (removedPos != lastPos) {
            for (auto i = 0u; i < node48->childIndex.size(); ++i) {
                if (node48->childIndex[i] == lastPos) {
                    node48->childIndex[i] = removedPos;
                    break;
                }
            }
            node48->children[removedPos] = node48->children[lastPos];
        }
        node48->children[lastPos] = nullptr;
        --count;
        return;
    }
    case Kind::NODE256:
        if (node256->children[byte]) {
            node256->children[byte] = nullptr;
            --count;
        }
        return;
    default:
        UNREACHABLE_CODE;
    }
}

ArtKey ArtKey::encode(ValueVector* vector, uint64_t vectorPos) {
    if (vector->isNull(vectorPos)) {
        return ArtKey{};
    }
    std::vector<uint8_t> bytes;
    TypeUtils::visit(vector->dataType.getPhysicalType(), [&]<typename T>(T) {
        if constexpr (std::same_as<T, string_t>) {
            appendString(bytes, vector->getValue<string_t>(vectorPos).getAsStringView());
        } else if constexpr (std::same_as<T, int128_t>) {
            const auto value = vector->getValue<T>(vectorPos);
            appendInt128(bytes, value.high, value.low);
        } else if constexpr (std::same_as<T, uint128_t>) {
            const auto value = vector->getValue<T>(vectorPos);
            appendUInt128(bytes, value.high, value.low);
        } else if constexpr (std::same_as<T, bool>) {
            bytes.push_back(vector->getValue<T>(vectorPos) ? 1 : 0);
        } else if constexpr (std::integral<T>) {
            appendIntegral(bytes, vector->getValue<T>(vectorPos));
        } else if constexpr (std::floating_point<T>) {
            appendFloat(bytes, vector->getValue<T>(vectorPos));
        } else {
            UNREACHABLE_CODE;
        }
    });
    return ArtKey{std::move(bytes)};
}

std::shared_ptr<BufferWriter> ArtPrimaryKeyIndexStorageInfo::serialize() const {
    auto bufferWriter = std::make_shared<BufferWriter>();
    auto serializer = Serializer(bufferWriter);
    serializer.write<page_idx_t>(treePageRange.startPageIdx);
    serializer.write<page_idx_t>(treePageRange.numPages);
    serializer.write<uint64_t>(treeSize);
    return bufferWriter;
}

std::unique_ptr<IndexStorageInfo> ArtPrimaryKeyIndexStorageInfo::deserialize(
    std::unique_ptr<BufferReader> reader) {
    Deserializer deSer(std::move(reader));
    page_idx_t startPageIdx = INVALID_PAGE_IDX;
    page_idx_t numPages = 0;
    uint64_t treeSize = 0;
    deSer.deserializeValue(startPageIdx);
    deSer.deserializeValue(numPages);
    deSer.deserializeValue(treeSize);
    return std::make_unique<ArtPrimaryKeyIndexStorageInfo>(PageRange{startPageIdx, numPages},
        treeSize);
}

ArtPrimaryKeyIndex::ArtPrimaryKeyIndex(IndexInfo indexInfo,
    std::unique_ptr<IndexStorageInfo> storageInfo)
    : Index{std::move(indexInfo), std::move(storageInfo)} {
    const auto& artStorageInfo = this->storageInfo->constCast<ArtPrimaryKeyIndexStorageInfo>();
    if (artStorageInfo.treePageRange.startPageIdx != INVALID_PAGE_IDX) {
        diskTreePageRange = artStorageInfo.treePageRange;
        diskTreeSize = artStorageInfo.treeSize;
        diskBacked = true;
        return;
    }
    loadEntries(artStorageInfo);
}

ArtPrimaryKeyIndex::~ArtPrimaryKeyIndex() {
    if (!shouldPrintDestructorStats()) {
        clear();
        return;
    }
    const auto allocatedNodes = numAllocatedNodes;
    const auto numBlocks = nodeBlocks.size();
    const auto numNode4 = numNodesByKind[0];
    const auto numNode16 = numNodesByKind[1];
    const auto numNode48 = numNodesByKind[2];
    const auto numNode256 = numNodesByKind[3];
    const auto start = std::chrono::steady_clock::now();
    clear();
    const auto end = std::chrono::steady_clock::now();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::fprintf(stderr,
        "ART destructor index=%s table=%llu allocated_nodes=%llu arena_nodes=%llu blocks=%llu "
        "node4=%llu node16=%llu node48=%llu node256=%llu elapsed_ms=%lld\n",
        indexInfo.name.c_str(), static_cast<unsigned long long>(indexInfo.tableID),
        static_cast<unsigned long long>(allocatedNodes),
        static_cast<unsigned long long>(allocatedNodes - 1),
        static_cast<unsigned long long>(numBlocks), static_cast<unsigned long long>(numNode4),
        static_cast<unsigned long long>(numNode16), static_cast<unsigned long long>(numNode48),
        static_cast<unsigned long long>(numNode256), static_cast<long long>(elapsedMs));
}

void ArtPrimaryKeyIndex::clear() {
    nodeBlocks.clear();
    root.offset.reset();
    root.overflowOffsets.reset();
    root.prefix.clear();
    root.kind = Node::Kind::NODE4;
    root.count = 0;
    root.small = Node::SmallChildren();
    root.node48.reset();
    root.node256.reset();
    numAllocatedNodes = 1;
    numNodesByKind = {1, 0, 0, 0};
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::allocateNode() {
    if (nodeBlocks.empty() || nodeBlocks.back().used == NODE_BLOCK_CAPACITY) {
        nodeBlocks.emplace_back();
    }
    auto& block = nodeBlocks.back();
    auto* node = block.nodes + block.used++;
    new (node) Node();
    ++numAllocatedNodes;
    ++numNodesByKind[static_cast<uint8_t>(Node::Kind::NODE4)];
    return node;
}

void ArtPrimaryKeyIndex::recordKindChange(Node& node, Node::Kind newKind) {
    --numNodesByKind[static_cast<uint8_t>(node.kind)];
    node.kind = newKind;
    ++numNodesByKind[static_cast<uint8_t>(node.kind)];
}

static uint64_t matchPrefix(const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& key,
    uint64_t depth) {
    auto matched = 0u;
    while (matched < prefix.size() && depth + matched < key.size() &&
           prefix[matched] == key[depth + matched]) {
        ++matched;
    }
    return matched;
}

void ArtPrimaryKeyIndex::resetNodePayload(Node& node) {
    node.offset.reset();
    node.overflowOffsets.reset();
    node.prefix.clear();
    node.kind = ArtPrimaryKeyIndex::Node::Kind::NODE4;
    node.count = 0;
    node.small = ArtPrimaryKeyIndex::Node::SmallChildren();
    node.node48.reset();
    node.node256.reset();
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::findOrCreateLeaf(const std::vector<uint8_t>& key) {
    auto* node = &root;
    auto depth = 0u;
    while (true) {
        const auto prefixMatch = matchPrefix(node->prefix, key, depth);
        if (prefixMatch != node->prefix.size()) {
            const auto oldPrefix = std::move(node->prefix);
            const auto oldEdge = oldPrefix[prefixMatch];
            auto* oldChild = allocateNode();
            oldChild->offset = std::move(node->offset);
            oldChild->overflowOffsets = std::move(node->overflowOffsets);
            oldChild->prefix.assign(oldPrefix.begin() + prefixMatch + 1, oldPrefix.end());
            oldChild->kind = node->kind;
            oldChild->count = node->count;
            oldChild->small = node->small;
            oldChild->node48 = std::move(node->node48);
            oldChild->node256 = std::move(node->node256);

            resetNodePayload(*node);
            node->prefix.assign(oldPrefix.begin(), oldPrefix.begin() + prefixMatch);
            node->insertChild(*this, oldEdge, oldChild);
            depth += prefixMatch;
            if (depth == key.size()) {
                return node;
            }
            auto* newChild = allocateNode();
            newChild->prefix.assign(key.begin() + depth + 1, key.end());
            node->insertChild(*this, key[depth], newChild);
            return newChild;
        }
        depth += node->prefix.size();
        if (depth == key.size()) {
            return node;
        }
        const auto edge = key[depth++];
        if (auto* child = node->getChild(edge)) {
            node = child;
            continue;
        }
        auto* child = allocateNode();
        child->prefix.assign(key.begin() + depth, key.end());
        node->insertChild(*this, edge, child);
        return child;
    }
}

std::unique_ptr<Index::InsertState> ArtPrimaryKeyIndex::initInsertState(main::ClientContext*,
    visible_func isVisible) {
    return std::make_unique<InsertState>(std::move(isVisible));
}

static void validateIndexInfo(const IndexInfo& indexInfo) {
    if (!indexInfo.isBuiltin) {
        throw RuntimeException("ART indexes currently support only built-in indexes.");
    }
    if (indexInfo.columnIDs.size() != 1 || indexInfo.keyDataTypes.size() != 1) {
        throw RuntimeException("ART indexes currently support exactly one property.");
    }
    switch (indexInfo.keyDataTypes[0]) {
    case PhysicalTypeID::UINT8:
    case PhysicalTypeID::UINT16:
    case PhysicalTypeID::UINT32:
    case PhysicalTypeID::UINT64:
    case PhysicalTypeID::INT8:
    case PhysicalTypeID::INT16:
    case PhysicalTypeID::INT32:
    case PhysicalTypeID::INT64:
    case PhysicalTypeID::INT128:
    case PhysicalTypeID::UINT128:
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::DOUBLE:
        return;
    default:
        throw RuntimeException("ART indexes do not support this primary-key type.");
    }
}

std::unique_ptr<ArtPrimaryKeyIndex> ArtPrimaryKeyIndex::createNewIndex(IndexInfo indexInfo) {
    validateIndexInfo(indexInfo);
    return std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo),
        std::make_unique<ArtPrimaryKeyIndexStorageInfo>());
}

bool ArtPrimaryKeyIndex::insertInternal(const ArtKey& key, offset_t offset,
    visible_func isVisible) {
    DASSERT(!key.empty());
    auto* node = findOrCreateLeaf(key.getBytes());
    if (node->offset.has_value() && isVisible(node->offset.value())) {
        return false;
    }
    if (node->overflowOffsets) {
        for (const auto existingOffset : *node->overflowOffsets) {
            if (isVisible(existingOffset)) {
                return false;
            }
        }
    }
    node->offset = offset;
    node->overflowOffsets.reset();
    return true;
}

void ArtPrimaryKeyIndex::insertSecondaryInternal(const ArtKey& key, offset_t offset) {
    DASSERT(!key.empty());
    auto* node = findOrCreateLeaf(key.getBytes());
    if (!node->offset.has_value()) {
        node->offset = offset;
        return;
    }
    if (node->offset.value() == offset) {
        return;
    }
    if (!node->overflowOffsets) {
        node->overflowOffsets = std::make_unique<std::vector<offset_t>>();
    }
    if (std::find(node->overflowOffsets->begin(), node->overflowOffsets->end(), offset) ==
        node->overflowOffsets->end()) {
        node->overflowOffsets->push_back(offset);
    }
}

bool ArtPrimaryKeyIndex::lookup(const ArtKey& key, offset_t& result, visible_func isVisible) const {
    const auto* node = findLeaf(key);
    if (node == nullptr) {
        return false;
    }
    if (node->offset.has_value() && isVisible(node->offset.value())) {
        result = node->offset.value();
        return true;
    }
    if (node->overflowOffsets) {
        for (const auto offset : *node->overflowOffsets) {
            if (isVisible(offset)) {
                result = offset;
                return true;
            }
        }
    }
    return false;
}

struct DiskNodeHeader {
    std::vector<uint8_t> prefix;
    std::vector<offset_t> offsets;
    uint64_t numChildren = 0;
};

template<class READER>
static DiskNodeHeader readDiskNodeHeader(READER& reader) {
    DiskNodeHeader header;
    const auto prefixSize = readVarUint(reader);
    header.prefix.resize(prefixSize);
    if (prefixSize > 0) {
        reader.read(header.prefix.data(), prefixSize);
    }
    const auto numOffsets = readVarUint(reader);
    header.offsets.reserve(numOffsets);
    for (auto i = 0u; i < numOffsets; ++i) {
        header.offsets.push_back(readVarUint(reader));
    }
    header.numChildren = readVarUint(reader);
    return header;
}

template<class READER>
static void skipDiskTree(READER& reader) {
    const auto prefixSize = readVarUint(reader);
    reader.skip(prefixSize);
    const auto numOffsets = readVarUint(reader);
    for (auto i = 0u; i < numOffsets; ++i) {
        readVarUint(reader);
    }
    const auto numChildren = readVarUint(reader);
    for (auto i = 0u; i < numChildren; ++i) {
        uint8_t byte = 0;
        reader.read(&byte, 1);
        uint64_t childSize = 0;
        reader.read(reinterpret_cast<uint8_t*>(&childSize), sizeof(childSize));
        reader.skip(childSize);
    }
}

bool ArtPrimaryKeyIndex::lookupPrimaryKey(const transaction::Transaction*, ValueVector* keyVector,
    uint64_t vectorPos, offset_t& result, visible_func isVisible) {
    std::lock_guard lck{mutex};
    const auto key = ArtKey::encode(keyVector, vectorPos);
    if (!diskBacked) {
        return lookup(key, result, std::move(isVisible));
    }
    if (key.empty()) {
        return false;
    }
    DASSERT(diskFileHandle != nullptr);
    ArtPageRangeReader reader{*diskFileHandle, diskTreePageRange, diskTreeSize};
    auto depth = 0u;
    const auto& bytes = key.getBytes();
    while (true) {
        const auto header = readDiskNodeHeader(reader);
        const auto prefixMatch = matchPrefix(header.prefix, bytes, depth);
        if (prefixMatch != header.prefix.size()) {
            return false;
        }
        depth += header.prefix.size();
        if (depth == bytes.size()) {
            for (const auto offset : header.offsets) {
                if (isVisible(offset)) {
                    result = offset;
                    return true;
                }
            }
            return false;
        }
        const auto edge = bytes[depth++];
        auto found = false;
        for (auto i = 0u; i < header.numChildren; ++i) {
            uint8_t byte = 0;
            reader.read(&byte, 1);
            uint64_t childSize = 0;
            reader.read(reinterpret_cast<uint8_t*>(&childSize), sizeof(childSize));
            if (byte == edge) {
                found = true;
                break;
            }
            reader.skip(childSize);
        }
        if (!found) {
            return false;
        }
    }
}

const ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::findLeaf(const ArtKey& key) const {
    if (key.empty()) {
        return nullptr;
    }
    const auto* node = &root;
    auto depth = 0u;
    const auto& bytes = key.getBytes();
    while (true) {
        const auto prefixMatch = matchPrefix(node->prefix, bytes, depth);
        if (prefixMatch != node->prefix.size()) {
            return nullptr;
        }
        depth += node->prefix.size();
        if (depth == bytes.size()) {
            return node;
        }
        const auto* child = node->getChild(bytes[depth++]);
        if (child == nullptr) {
            return nullptr;
        }
        node = child;
    }
}

void ArtPrimaryKeyIndex::appendVisibleOffsets(const Node& node, std::vector<offset_t>& results,
    visible_func isVisible) const {
    if (node.offset.has_value() && isVisible(node.offset.value())) {
        results.push_back(node.offset.value());
    }
    if (!node.overflowOffsets) {
        return;
    }
    for (const auto offset : *node.overflowOffsets) {
        if (isVisible(offset)) {
            results.push_back(offset);
        }
    }
}

void ArtPrimaryKeyIndex::eraseOffsetFromLeaf(Node& node, offset_t offset) {
    if (node.offset.has_value() && node.offset.value() == offset) {
        if (node.overflowOffsets && !node.overflowOffsets->empty()) {
            node.offset = node.overflowOffsets->back();
            node.overflowOffsets->pop_back();
            if (node.overflowOffsets->empty()) {
                node.overflowOffsets.reset();
            }
            return;
        }
        node.offset.reset();
        return;
    }
    if (!node.overflowOffsets) {
        return;
    }
    auto it = std::find(node.overflowOffsets->begin(), node.overflowOffsets->end(), offset);
    if (it != node.overflowOffsets->end()) {
        node.overflowOffsets->erase(it);
    }
    if (node.overflowOffsets->empty()) {
        node.overflowOffsets.reset();
    }
}

bool ArtPrimaryKeyIndex::eraseInternal(Node& node, const std::vector<uint8_t>& key,
    uint64_t depth) {
    const auto prefixMatch = matchPrefix(node.prefix, key, depth);
    if (prefixMatch != node.prefix.size()) {
        return false;
    }
    depth += node.prefix.size();
    if (depth == key.size()) {
        node.offset.reset();
        node.overflowOffsets.reset();
        return node.empty();
    }
    const auto byte = key[depth];
    auto* child = node.getChild(byte);
    if (child == nullptr) {
        return false;
    }
    if (eraseInternal(*child, key, depth + 1)) {
        node.removeChild(byte);
    }
    return node.empty();
}

void ArtPrimaryKeyIndex::erase(const ArtKey& key) {
    if (!key.empty()) {
        eraseInternal(root, key.getBytes(), 0);
    }
}

bool ArtPrimaryKeyIndex::eraseOffsetInternal(Node& node, offset_t offset) {
    eraseOffsetFromLeaf(node, offset);
    switch (node.kind) {
    case Node::Kind::NODE4:
    case Node::Kind::NODE16: {
        for (auto i = 0u; i < node.count;) {
            if (eraseOffsetInternal(*node.small.children[i], offset)) {
                node.removeChild(node.small.keys[i]);
                continue;
            }
            ++i;
        }
        break;
    }
    case Node::Kind::NODE48:
        for (auto byte = 0u; byte < node.node48->childIndex.size(); ++byte) {
            const auto pos = node.node48->childIndex[byte];
            if (pos == Node::EMPTY_MARKER) {
                continue;
            }
            if (eraseOffsetInternal(*node.node48->children[pos], offset)) {
                node.removeChild(static_cast<uint8_t>(byte));
            }
        }
        break;
    case Node::Kind::NODE256:
        for (auto byte = 0u; byte < node.node256->children.size(); ++byte) {
            if (!node.node256->children[byte]) {
                continue;
            }
            if (eraseOffsetInternal(*node.node256->children[byte], offset)) {
                node.removeChild(static_cast<uint8_t>(byte));
            }
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
    return node.empty();
}

void ArtPrimaryKeyIndex::commitInsert(transaction::Transaction*, const ValueVector& nodeIDVector,
    const std::vector<ValueVector*>& indexVectors, Index::InsertState& insertState) {
    DASSERT(indexVectors.size() == 1);
    std::lock_guard lck{mutex};
    materializeDiskTree();
    auto& keyVector = *indexVectors[0];
    const auto& artInsertState = insertState.cast<InsertState>();
    for (auto i = 0u; i < nodeIDVector.state->getSelSize(); i++) {
        const auto nodeIDPos = nodeIDVector.state->getSelVector()[i];
        const auto offset = nodeIDVector.readNodeOffset(nodeIDPos);
        const auto keyPos = keyVector.state->getSelVector()[i];
        if (keyVector.isNull(keyPos) && indexInfo.isPrimary) {
            throw RuntimeException(ExceptionMessage::nullPKException());
        }
        const auto key = ArtKey::encode(&keyVector, keyPos);
        if (key.empty()) {
            continue;
        }
        if (!indexInfo.isPrimary) {
            insertSecondaryInternal(key, offset);
            continue;
        }
        if (!insertInternal(key, offset, artInsertState.isVisible)) {
            throw RuntimeException(
                ExceptionMessage::duplicatePKException(keyVector.getAsValue(keyPos)->toString()));
        }
    }
}

std::unique_ptr<Index::UpdateState> ArtPrimaryKeyIndex::initUpdateState(main::ClientContext*,
    column_id_t, visible_func) {
    return std::make_unique<UpdateState>();
}

void ArtPrimaryKeyIndex::update(transaction::Transaction*, const ValueVector& nodeIDVector,
    ValueVector& propertyVector, UpdateState&) {
    if (indexInfo.isPrimary) {
        UNREACHABLE_CODE;
    }
    std::lock_guard lck{mutex};
    materializeDiskTree();
    for (auto i = 0u; i < nodeIDVector.state->getSelSize(); i++) {
        const auto nodeIDPos = nodeIDVector.state->getSelVector()[i];
        const auto offset = nodeIDVector.readNodeOffset(nodeIDPos);
        eraseOffsetInternal(root, offset);
        const auto keyPos = propertyVector.state->getSelVector()[i];
        const auto key = ArtKey::encode(&propertyVector, keyPos);
        if (!key.empty()) {
            insertSecondaryInternal(key, offset);
        }
    }
}

void ArtPrimaryKeyIndex::delete_(transaction::Transaction*, const ValueVector& nodeIDVector,
    DeleteState&) {
    if (indexInfo.isPrimary) {
        return;
    }
    std::lock_guard lck{mutex};
    materializeDiskTree();
    for (auto i = 0u; i < nodeIDVector.state->getSelSize(); i++) {
        const auto nodeIDPos = nodeIDVector.state->getSelVector()[i];
        eraseOffsetInternal(root, nodeIDVector.readNodeOffset(nodeIDPos));
    }
}

bool ArtPrimaryKeyIndex::lookupAll(const transaction::Transaction*, ValueVector* keyVector,
    uint64_t vectorPos, std::vector<offset_t>& results, visible_func isVisible) {
    std::lock_guard lck{mutex};
    const auto key = ArtKey::encode(keyVector, vectorPos);
    if (diskBacked) {
        if (key.empty()) {
            return false;
        }
        DASSERT(diskFileHandle != nullptr);
        ArtPageRangeReader reader{*diskFileHandle, diskTreePageRange, diskTreeSize};
        auto depth = 0u;
        const auto& bytes = key.getBytes();
        while (true) {
            const auto header = readDiskNodeHeader(reader);
            const auto prefixMatch = matchPrefix(header.prefix, bytes, depth);
            if (prefixMatch != header.prefix.size()) {
                return false;
            }
            depth += header.prefix.size();
            if (depth == bytes.size()) {
                for (const auto offset : header.offsets) {
                    if (isVisible(offset)) {
                        results.push_back(offset);
                    }
                }
                return !results.empty();
            }
            const auto edge = bytes[depth++];
            auto found = false;
            for (auto i = 0u; i < header.numChildren; ++i) {
                uint8_t byte = 0;
                reader.read(&byte, 1);
                uint64_t childSize = 0;
                reader.read(reinterpret_cast<uint8_t*>(&childSize), sizeof(childSize));
                if (byte == edge) {
                    found = true;
                    break;
                }
                reader.skip(childSize);
            }
            if (!found) {
                return false;
            }
        }
    }
    const auto* node = findLeaf(key);
    if (node == nullptr) {
        return false;
    }
    appendVisibleOffsets(*node, results, std::move(isVisible));
    return !results.empty();
}

static int compareKeys(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
    const auto cmpSize = std::min(left.size(), right.size());
    for (auto i = 0u; i < cmpSize; ++i) {
        if (left[i] < right[i]) {
            return -1;
        }
        if (left[i] > right[i]) {
            return 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

static bool satisfiesLowerBound(const std::vector<uint8_t>& key, const ArtKey* lowerBound,
    bool lowerInclusive) {
    if (lowerBound == nullptr) {
        return true;
    }
    const auto cmp = compareKeys(key, lowerBound->getBytes());
    return lowerInclusive ? cmp >= 0 : cmp > 0;
}

static bool satisfiesUpperBound(const std::vector<uint8_t>& key, const ArtKey* upperBound,
    bool upperInclusive) {
    if (upperBound == nullptr) {
        return true;
    }
    const auto cmp = compareKeys(key, upperBound->getBytes());
    return upperInclusive ? cmp <= 0 : cmp < 0;
}

template<class READER>
static void collectDiskRange(READER& reader, std::vector<uint8_t>& key, const ArtKey* lowerBound,
    bool lowerInclusive, const ArtKey* upperBound, bool upperInclusive, idx_t maxResults,
    std::vector<offset_t>& results, visible_func isVisible) {
    const auto header = readDiskNodeHeader(reader);
    const auto keySizeBeforePrefix = key.size();
    key.insert(key.end(), header.prefix.begin(), header.prefix.end());
    if (results.size() >= maxResults) {
        key.resize(keySizeBeforePrefix);
        return;
    }
    if (!header.offsets.empty() && satisfiesLowerBound(key, lowerBound, lowerInclusive) &&
        satisfiesUpperBound(key, upperBound, upperInclusive)) {
        for (const auto offset : header.offsets) {
            if (isVisible(offset)) {
                results.push_back(offset);
                if (results.size() >= maxResults) {
                    key.resize(keySizeBeforePrefix);
                    return;
                }
            }
        }
    }
    for (auto i = 0u; i < header.numChildren; ++i) {
        uint8_t byte = 0;
        reader.read(&byte, 1);
        uint64_t childSize = 0;
        reader.read(reinterpret_cast<uint8_t*>(&childSize), sizeof(childSize));
        key.push_back(byte);
        if (satisfiesUpperBound(key, upperBound, true)) {
            collectDiskRange(reader, key, lowerBound, lowerInclusive, upperBound, upperInclusive,
                maxResults, results, isVisible);
        } else {
            reader.skip(childSize);
        }
        key.pop_back();
        if (results.size() >= maxResults) {
            key.resize(keySizeBeforePrefix);
            return;
        }
    }
    key.resize(keySizeBeforePrefix);
}

void ArtPrimaryKeyIndex::collectRange(const Node& node, std::vector<uint8_t>& key,
    const ArtKey* lowerBound, bool lowerInclusive, const ArtKey* upperBound, bool upperInclusive,
    idx_t maxResults, std::vector<offset_t>& results, visible_func isVisible) const {
    const auto keySizeBeforePrefix = key.size();
    key.insert(key.end(), node.prefix.begin(), node.prefix.end());
    if (results.size() >= maxResults) {
        key.resize(keySizeBeforePrefix);
        return;
    }
    if (node.hasOffsets() && satisfiesLowerBound(key, lowerBound, lowerInclusive) &&
        satisfiesUpperBound(key, upperBound, upperInclusive)) {
        if (node.offset.has_value() && isVisible(node.offset.value())) {
            results.push_back(node.offset.value());
            if (results.size() >= maxResults) {
                key.resize(keySizeBeforePrefix);
                return;
            }
        }
        if (node.overflowOffsets) {
            for (const auto offset : *node.overflowOffsets) {
                if (isVisible(offset)) {
                    results.push_back(offset);
                    if (results.size() >= maxResults) {
                        key.resize(keySizeBeforePrefix);
                        return;
                    }
                }
            }
        }
    }
    auto visitChild = [&](uint8_t byte, const Node& child) {
        key.push_back(byte);
        if (satisfiesUpperBound(key, upperBound, true)) {
            collectRange(child, key, lowerBound, lowerInclusive, upperBound, upperInclusive,
                maxResults, results, isVisible);
        }
        key.pop_back();
    };
    switch (node.kind) {
    case Node::Kind::NODE4:
    case Node::Kind::NODE16: {
        std::array<uint16_t, 16> childOrder{};
        for (auto i = 0u; i < node.count; ++i) {
            childOrder[i] = i;
        }
        std::sort(childOrder.begin(), childOrder.begin() + node.count,
            [&node](auto left, auto right) {
                return node.small.keys[left] < node.small.keys[right];
            });
        for (auto i = 0u; i < node.count; ++i) {
            const auto pos = childOrder[i];
            visitChild(node.small.keys[pos], *node.small.children[pos]);
            if (results.size() >= maxResults) {
                key.resize(keySizeBeforePrefix);
                return;
            }
        }
        break;
    }
    case Node::Kind::NODE48:
        for (auto byte = 0u; byte < node.node48->childIndex.size(); ++byte) {
            const auto pos = node.node48->childIndex[byte];
            if (pos == Node::EMPTY_MARKER) {
                continue;
            }
            visitChild(static_cast<uint8_t>(byte), *node.node48->children[pos]);
            if (results.size() >= maxResults) {
                key.resize(keySizeBeforePrefix);
                return;
            }
        }
        break;
    case Node::Kind::NODE256:
        for (auto byte = 0u; byte < node.node256->children.size(); ++byte) {
            if (!node.node256->children[byte]) {
                continue;
            }
            visitChild(static_cast<uint8_t>(byte), *node.node256->children[byte]);
            if (results.size() >= maxResults) {
                key.resize(keySizeBeforePrefix);
                return;
            }
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
    key.resize(keySizeBeforePrefix);
}

bool ArtPrimaryKeyIndex::scanPrimaryKeyRange(ValueVector* lowerBoundVector, uint64_t lowerBoundPos,
    bool lowerInclusive, ValueVector* upperBoundVector, uint64_t upperBoundPos, bool upperInclusive,
    idx_t maxResults, std::vector<offset_t>& results, visible_func isVisible) {
    std::lock_guard lck{mutex};
    auto lowerBound =
        lowerBoundVector == nullptr ? ArtKey{} : ArtKey::encode(lowerBoundVector, lowerBoundPos);
    auto upperBound =
        upperBoundVector == nullptr ? ArtKey{} : ArtKey::encode(upperBoundVector, upperBoundPos);
    const auto* lowerBoundPtr = lowerBoundVector == nullptr ? nullptr : &lowerBound;
    const auto* upperBoundPtr = upperBoundVector == nullptr ? nullptr : &upperBound;
    if ((lowerBoundVector != nullptr && lowerBound.empty()) ||
        (upperBoundVector != nullptr && upperBound.empty())) {
        return true;
    }
    std::vector<uint8_t> key;
    if (diskBacked) {
        DASSERT(diskFileHandle != nullptr);
        ArtPageRangeReader reader{*diskFileHandle, diskTreePageRange, diskTreeSize};
        collectDiskRange(reader, key, lowerBoundPtr, lowerInclusive, upperBoundPtr, upperInclusive,
            maxResults, results, std::move(isVisible));
        return true;
    }
    collectRange(root, key, lowerBoundPtr, lowerInclusive, upperBoundPtr, upperInclusive,
        maxResults, results, std::move(isVisible));
    return true;
}

void ArtPrimaryKeyIndex::discardPrimaryKey(ValueVector* keyVector) {
    std::lock_guard lck{mutex};
    materializeDiskTree();
    for (auto i = 0u; i < keyVector->state->getSelSize(); ++i) {
        const auto pos = keyVector->state->getSelVector()[i];
        erase(ArtKey::encode(keyVector, pos));
    }
}

void ArtPrimaryKeyIndex::collectEntries(const Node& node, std::vector<uint8_t>& key,
    std::vector<std::pair<std::vector<uint8_t>, offset_t>>& entries) const {
    const auto keySizeBeforePrefix = key.size();
    key.insert(key.end(), node.prefix.begin(), node.prefix.end());
    if (node.offset.has_value()) {
        entries.emplace_back(key, node.offset.value());
    }
    if (node.overflowOffsets) {
        for (const auto offset : *node.overflowOffsets) {
            entries.emplace_back(key, offset);
        }
    }
    switch (node.kind) {
    case Node::Kind::NODE4:
    case Node::Kind::NODE16:
        for (auto i = 0u; i < node.count; ++i) {
            key.push_back(node.small.keys[i]);
            collectEntries(*node.small.children[i], key, entries);
            key.pop_back();
        }
        break;
    case Node::Kind::NODE48:
        for (auto i = 0u; i < node.node48->childIndex.size(); ++i) {
            const auto pos = node.node48->childIndex[i];
            if (pos == Node::EMPTY_MARKER) {
                continue;
            }
            key.push_back(static_cast<uint8_t>(i));
            collectEntries(*node.node48->children[pos], key, entries);
            key.pop_back();
        }
        break;
    case Node::Kind::NODE256:
        for (auto i = 0u; i < node.node256->children.size(); ++i) {
            if (!node.node256->children[i]) {
                continue;
            }
            key.push_back(static_cast<uint8_t>(i));
            collectEntries(*node.node256->children[i], key, entries);
            key.pop_back();
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
    key.resize(keySizeBeforePrefix);
}

uint64_t ArtPrimaryKeyIndex::calculateSerializedTreeSize(const Node& node) const {
    uint64_t numOffsets = node.offset.has_value() ? 1 : 0;
    if (node.overflowOffsets) {
        numOffsets += node.overflowOffsets->size();
    }
    auto size = getVarUintSize(node.prefix.size()) + node.prefix.size() +
                getVarUintSize(numOffsets) + getVarUintSize(node.count);
    if (node.offset.has_value()) {
        size += getVarUintSize(node.offset.value());
    }
    if (node.overflowOffsets) {
        for (const auto offset : *node.overflowOffsets) {
            size += getVarUintSize(offset);
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
    writeVarUint(serializer, node.prefix.size());
    if (!node.prefix.empty()) {
        serializer.write(node.prefix.data(), node.prefix.size());
    }
    uint64_t numOffsets = node.offset.has_value() ? 1 : 0;
    if (node.overflowOffsets) {
        numOffsets += node.overflowOffsets->size();
    }
    writeVarUint(serializer, numOffsets);
    if (node.offset.has_value()) {
        writeVarUint(serializer, node.offset.value());
    }
    if (node.overflowOffsets) {
        for (const auto offset : *node.overflowOffsets) {
            writeVarUint(serializer, offset);
        }
    }
    writeVarUint(serializer, node.count);
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
    const auto prefixSize = readVarUint(reader);
    node.prefix.resize(prefixSize);
    if (prefixSize > 0) {
        reader.read(node.prefix.data(), prefixSize);
    }
    const auto numOffsets = readVarUint(reader);
    if (numOffsets > 0) {
        node.offset = readVarUint(reader);
    }
    if (numOffsets > 1) {
        node.overflowOffsets = std::make_unique<std::vector<offset_t>>();
        node.overflowOffsets->reserve(numOffsets - 1);
        for (auto i = 1u; i < numOffsets; ++i) {
            node.overflowOffsets->push_back(readVarUint(reader));
        }
    }
    const auto numChildren = readVarUint(reader);
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
    StorageManager* storageManager, IndexInfo indexInfo, std::span<uint8_t> treeBytes) {
    validateIndexInfo(indexInfo);
    auto* dataFH = storageManager->getDataFH();
    const auto numPages =
        static_cast<page_idx_t>((treeBytes.size() + LBUG_PAGE_SIZE - 1) / LBUG_PAGE_SIZE);
    auto pageRange = dataFH->getPageManager()->allocatePageRange(numPages);
    if (!treeBytes.empty()) {
        dataFH->writePagesToFile(treeBytes.data(), treeBytes.size(), pageRange.startPageIdx);
    }
    auto storageInfo = std::make_unique<ArtPrimaryKeyIndexStorageInfo>(pageRange, treeBytes.size());
    auto index = std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo), std::move(storageInfo));
    index->diskFileHandle = dataFH;
    index->diskTreePageRange = pageRange;
    index->diskTreeSize = treeBytes.size();
    index->diskBacked = true;
    return index;
}

} // namespace storage
} // namespace lbug
