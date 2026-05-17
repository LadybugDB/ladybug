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
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/types/value/value.h"
#include "common/vector/value_vector.h"
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

} // namespace

ArtPrimaryKeyIndex::Node::Node() {
    new (&small) SmallChildren();
}

ArtPrimaryKeyIndex::NodeBlock::NodeBlock()
    : nodes{static_cast<Node*>(::operator new(sizeof(Node) * NODE_BLOCK_CAPACITY))} {}

ArtPrimaryKeyIndex::NodeBlock::~NodeBlock() {
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
        const auto pos = node48.childIndex[byte];
        return pos == EMPTY_MARKER ? nullptr : node48.children[pos];
    }
    case Kind::NODE256:
        return node256.children[byte];
    default:
        UNREACHABLE_CODE;
    }
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::Node::getOrInsertChild(ArtPrimaryKeyIndex& index,
    uint8_t byte) {
    if (auto* child = getChild(byte)) {
        return child;
    }
    switch (kind) {
    case Kind::NODE4:
        if (count == 4) {
            index.recordKindChange(*this, Kind::NODE16);
        }
        break;
    case Kind::NODE16:
        if (count == 16) {
            Node48Children children;
            children.childIndex.fill(EMPTY_MARKER);
            for (auto i = 0u; i < count; ++i) {
                children.childIndex[small.keys[i]] = i;
                children.children[i] = small.children[i];
            }
            new (&node48) Node48Children(children);
            index.recordKindChange(*this, Kind::NODE48);
        }
        break;
    case Kind::NODE48:
        if (count == 48) {
            Node256Children children;
            children.children.fill(nullptr);
            for (auto i = 0u; i < node48.childIndex.size(); ++i) {
                const auto pos = node48.childIndex[i];
                if (pos != EMPTY_MARKER) {
                    children.children[i] = node48.children[pos];
                }
            }
            new (&node256) Node256Children(children);
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
        small.children[count] = index.allocateNode();
        return small.children[count++];
    }
    case Kind::NODE48: {
        node48.childIndex[byte] = static_cast<uint8_t>(count);
        node48.children[count] = index.allocateNode();
        return node48.children[count++];
    }
    case Kind::NODE256:
        node256.children[byte] = index.allocateNode();
        ++count;
        return node256.children[byte];
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
        const auto removedPos = node48.childIndex[byte];
        if (removedPos == EMPTY_MARKER) {
            return;
        }
        const auto lastPos = count - 1;
        node48.childIndex[byte] = EMPTY_MARKER;
        if (removedPos != lastPos) {
            for (auto i = 0u; i < node48.childIndex.size(); ++i) {
                if (node48.childIndex[i] == lastPos) {
                    node48.childIndex[i] = removedPos;
                    break;
                }
            }
            node48.children[removedPos] = node48.children[lastPos];
        }
        node48.children[lastPos] = nullptr;
        --count;
        return;
    }
    case Kind::NODE256:
        if (node256.children[byte]) {
            node256.children[byte] = nullptr;
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
    serializer.write<uint64_t>(entries.size());
    for (const auto& [key, offset] : entries) {
        serializer.write<uint64_t>(key.size());
        if (!key.empty()) {
            serializer.write(key.data(), key.size());
        }
        serializer.write<offset_t>(offset);
    }
    return bufferWriter;
}

std::unique_ptr<IndexStorageInfo> ArtPrimaryKeyIndexStorageInfo::deserialize(
    std::unique_ptr<BufferReader> reader) {
    Deserializer deSer(std::move(reader));
    uint64_t numEntries = 0;
    deSer.deserializeValue(numEntries);
    std::vector<std::pair<std::vector<uint8_t>, offset_t>> entries;
    entries.reserve(numEntries);
    for (auto i = 0u; i < numEntries; ++i) {
        uint64_t keySize = 0;
        deSer.deserializeValue(keySize);
        std::vector<uint8_t> key(keySize);
        if (keySize > 0) {
            deSer.read(key.data(), keySize);
        }
        offset_t offset = INVALID_OFFSET;
        deSer.deserializeValue(offset);
        entries.emplace_back(std::move(key), offset);
    }
    return std::make_unique<ArtPrimaryKeyIndexStorageInfo>(std::move(entries));
}

ArtPrimaryKeyIndex::ArtPrimaryKeyIndex(IndexInfo indexInfo,
    std::unique_ptr<IndexStorageInfo> storageInfo)
    : Index{std::move(indexInfo), std::move(storageInfo)} {
    loadEntries(this->storageInfo->constCast<ArtPrimaryKeyIndexStorageInfo>());
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
    root.kind = Node::Kind::NODE4;
    root.count = 0;
    new (&root.small) Node::SmallChildren();
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

std::unique_ptr<Index::InsertState> ArtPrimaryKeyIndex::initInsertState(main::ClientContext*,
    visible_func isVisible) {
    return std::make_unique<InsertState>(std::move(isVisible));
}

static void validateIndexInfo(const IndexInfo& indexInfo) {
    if (!indexInfo.isPrimary || !indexInfo.isBuiltin) {
        throw RuntimeException("ART indexes currently support only built-in primary-key indexes.");
    }
    if (indexInfo.columnIDs.size() != 1 || indexInfo.keyDataTypes.size() != 1) {
        throw RuntimeException("ART indexes currently support exactly one primary-key property.");
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
    auto* node = &root;
    for (const auto byte : key.getBytes()) {
        node = node->getOrInsertChild(*this, byte);
    }
    if (node->offset.has_value() && isVisible(node->offset.value())) {
        return false;
    }
    node->offset = offset;
    return true;
}

bool ArtPrimaryKeyIndex::lookup(const ArtKey& key, offset_t& result, visible_func isVisible) const {
    if (key.empty()) {
        return false;
    }
    const auto* node = &root;
    for (const auto byte : key.getBytes()) {
        const auto* child = node->getChild(byte);
        if (child == nullptr) {
            return false;
        }
        node = child;
    }
    if (!node->offset.has_value() || !isVisible(node->offset.value())) {
        return false;
    }
    result = node->offset.value();
    return true;
}

bool ArtPrimaryKeyIndex::eraseInternal(Node& node, const std::vector<uint8_t>& key,
    uint64_t depth) {
    if (depth == key.size()) {
        node.offset.reset();
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

void ArtPrimaryKeyIndex::commitInsert(transaction::Transaction*, const ValueVector& nodeIDVector,
    const std::vector<ValueVector*>& indexVectors, Index::InsertState& insertState) {
    DASSERT(indexVectors.size() == 1);
    std::lock_guard lck{mutex};
    auto& keyVector = *indexVectors[0];
    const auto& artInsertState = insertState.cast<InsertState>();
    for (auto i = 0u; i < nodeIDVector.state->getSelSize(); i++) {
        const auto nodeIDPos = nodeIDVector.state->getSelVector()[i];
        const auto offset = nodeIDVector.readNodeOffset(nodeIDPos);
        const auto keyPos = keyVector.state->getSelVector()[i];
        if (keyVector.isNull(keyPos)) {
            throw RuntimeException(ExceptionMessage::nullPKException());
        }
        const auto key = ArtKey::encode(&keyVector, keyPos);
        if (!insertInternal(key, offset, artInsertState.isVisible)) {
            throw RuntimeException(
                ExceptionMessage::duplicatePKException(keyVector.getAsValue(keyPos)->toString()));
        }
    }
}

bool ArtPrimaryKeyIndex::lookupPrimaryKey(const transaction::Transaction*, ValueVector* keyVector,
    uint64_t vectorPos, offset_t& result, visible_func isVisible) {
    std::lock_guard lck{mutex};
    const auto key = ArtKey::encode(keyVector, vectorPos);
    return lookup(key, result, std::move(isVisible));
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

void ArtPrimaryKeyIndex::collectRange(const Node& node, std::vector<uint8_t>& key,
    const ArtKey* lowerBound, bool lowerInclusive, const ArtKey* upperBound, bool upperInclusive,
    idx_t maxResults, std::vector<offset_t>& results, visible_func isVisible) const {
    if (results.size() >= maxResults) {
        return;
    }
    if (node.offset.has_value() && satisfiesLowerBound(key, lowerBound, lowerInclusive) &&
        satisfiesUpperBound(key, upperBound, upperInclusive) && isVisible(node.offset.value())) {
        results.push_back(node.offset.value());
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
                return;
            }
        }
        break;
    }
    case Node::Kind::NODE48:
        for (auto byte = 0u; byte < node.node48.childIndex.size(); ++byte) {
            const auto pos = node.node48.childIndex[byte];
            if (pos == Node::EMPTY_MARKER) {
                continue;
            }
            visitChild(static_cast<uint8_t>(byte), *node.node48.children[pos]);
            if (results.size() >= maxResults) {
                return;
            }
        }
        break;
    case Node::Kind::NODE256:
        for (auto byte = 0u; byte < node.node256.children.size(); ++byte) {
            if (!node.node256.children[byte]) {
                continue;
            }
            visitChild(static_cast<uint8_t>(byte), *node.node256.children[byte]);
            if (results.size() >= maxResults) {
                return;
            }
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
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
    collectRange(root, key, lowerBoundPtr, lowerInclusive, upperBoundPtr, upperInclusive,
        maxResults, results, std::move(isVisible));
    return true;
}

void ArtPrimaryKeyIndex::discardPrimaryKey(ValueVector* keyVector) {
    std::lock_guard lck{mutex};
    for (auto i = 0u; i < keyVector->state->getSelSize(); ++i) {
        const auto pos = keyVector->state->getSelVector()[i];
        erase(ArtKey::encode(keyVector, pos));
    }
}

void ArtPrimaryKeyIndex::collectEntries(const Node& node, std::vector<uint8_t>& key,
    std::vector<std::pair<std::vector<uint8_t>, offset_t>>& entries) const {
    if (node.offset.has_value()) {
        entries.emplace_back(key, node.offset.value());
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
        for (auto i = 0u; i < node.node48.childIndex.size(); ++i) {
            const auto pos = node.node48.childIndex[i];
            if (pos == Node::EMPTY_MARKER) {
                continue;
            }
            key.push_back(static_cast<uint8_t>(i));
            collectEntries(*node.node48.children[pos], key, entries);
            key.pop_back();
        }
        break;
    case Node::Kind::NODE256:
        for (auto i = 0u; i < node.node256.children.size(); ++i) {
            if (!node.node256.children[i]) {
                continue;
            }
            key.push_back(static_cast<uint8_t>(i));
            collectEntries(*node.node256.children[i], key, entries);
            key.pop_back();
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
}

void ArtPrimaryKeyIndex::checkpoint(main::ClientContext*, PageAllocator&) {
    std::lock_guard lck{mutex};
    std::vector<std::pair<std::vector<uint8_t>, offset_t>> entries;
    std::vector<uint8_t> key;
    collectEntries(root, key, entries);
    storageInfo = std::make_unique<ArtPrimaryKeyIndexStorageInfo>(std::move(entries));
}

void ArtPrimaryKeyIndex::loadEntries(const ArtPrimaryKeyIndexStorageInfo& storageInfo) {
    static constexpr auto alwaysVisible = [](offset_t) { return true; };
    for (const auto& [keyBytes, offset] : storageInfo.entries) {
        insertInternal(ArtKey{keyBytes}, offset, alwaysVisible);
    }
}

std::unique_ptr<Index> ArtPrimaryKeyIndex::load(main::ClientContext*, StorageManager*,
    IndexInfo indexInfo, std::span<uint8_t> storageInfoBuffer) {
    validateIndexInfo(indexInfo);
    auto storageInfoBufferReader =
        std::make_unique<BufferReader>(storageInfoBuffer.data(), storageInfoBuffer.size());
    auto storageInfo =
        ArtPrimaryKeyIndexStorageInfo::deserialize(std::move(storageInfoBufferReader));
    return std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo), std::move(storageInfo));
}

} // namespace storage
} // namespace lbug
