#include "storage/index/art_index.h"

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
void appendRaw(std::vector<uint8_t>& bytes, const T& value) {
    const auto* data = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(T));
}

template<typename T>
void appendFixed(std::vector<uint8_t>& bytes, T value) {
    appendRaw(bytes, value);
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

} // namespace

ArtPrimaryKeyIndex::Node::Node() {
    childIndex.fill(EMPTY_MARKER);
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::Node::getChild(uint8_t byte) const {
    switch (kind) {
    case Kind::NODE4:
    case Kind::NODE16: {
        for (auto i = 0u; i < count; ++i) {
            if (keys[i] == byte) {
                return smallChildren[i].get();
            }
        }
        return nullptr;
    }
    case Kind::NODE48: {
        const auto pos = childIndex[byte];
        return pos == EMPTY_MARKER ? nullptr : node48Children[pos].get();
    }
    case Kind::NODE256:
        return node256Children[byte].get();
    default:
        UNREACHABLE_CODE;
    }
}

ArtPrimaryKeyIndex::Node* ArtPrimaryKeyIndex::Node::getOrInsertChild(uint8_t byte) {
    if (auto* child = getChild(byte)) {
        return child;
    }
    switch (kind) {
    case Kind::NODE4:
        if (count == 4) {
            kind = Kind::NODE16;
        }
        break;
    case Kind::NODE16:
        if (count == 16) {
            childIndex.fill(EMPTY_MARKER);
            for (auto i = 0u; i < count; ++i) {
                childIndex[keys[i]] = i;
                node48Children[i] = std::move(smallChildren[i]);
            }
            kind = Kind::NODE48;
        }
        break;
    case Kind::NODE48:
        if (count == 48) {
            for (auto i = 0u; i < childIndex.size(); ++i) {
                const auto pos = childIndex[i];
                if (pos != EMPTY_MARKER) {
                    node256Children[i] = std::move(node48Children[pos]);
                }
            }
            kind = Kind::NODE256;
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
        keys[count] = byte;
        smallChildren[count] = std::make_unique<Node>();
        return smallChildren[count++].get();
    }
    case Kind::NODE48: {
        childIndex[byte] = static_cast<uint8_t>(count);
        node48Children[count] = std::make_unique<Node>();
        return node48Children[count++].get();
    }
    case Kind::NODE256:
        node256Children[byte] = std::make_unique<Node>();
        ++count;
        return node256Children[byte].get();
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
        } else if constexpr (std::same_as<T, int128_t> || std::same_as<T, uint128_t>) {
            const auto value = vector->getValue<T>(vectorPos);
            appendRaw(bytes, value.low);
            appendRaw(bytes, value.high);
        } else if constexpr (std::integral<T> || std::floating_point<T>) {
            appendFixed(bytes, vector->getValue<T>(vectorPos));
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

ArtPrimaryKeyIndex::~ArtPrimaryKeyIndex() = default;

std::unique_ptr<ArtPrimaryKeyIndex> ArtPrimaryKeyIndex::createNewIndex(IndexInfo indexInfo) {
    return std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo),
        std::make_unique<ArtPrimaryKeyIndexStorageInfo>());
}

bool ArtPrimaryKeyIndex::insertInternal(const ArtKey& key, offset_t offset,
    visible_func isVisible) {
    DASSERT(!key.empty());
    auto* node = &root;
    for (const auto byte : key.getBytes()) {
        node = node->getOrInsertChild(byte);
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

void ArtPrimaryKeyIndex::erase(const ArtKey& key) {
    if (key.empty()) {
        return;
    }
    auto* node = &root;
    for (const auto byte : key.getBytes()) {
        auto* child = node->getChild(byte);
        if (child == nullptr) {
            return;
        }
        node = child;
    }
    node->offset.reset();
}

void ArtPrimaryKeyIndex::commitInsert(transaction::Transaction*, const ValueVector& nodeIDVector,
    const std::vector<ValueVector*>& indexVectors, Index::InsertState& insertState) {
    DASSERT(indexVectors.size() == 1);
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
    const auto key = ArtKey::encode(keyVector, vectorPos);
    return lookup(key, result, std::move(isVisible));
}

void ArtPrimaryKeyIndex::discardPrimaryKey(ValueVector* keyVector) {
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
            key.push_back(node.keys[i]);
            collectEntries(*node.smallChildren[i], key, entries);
            key.pop_back();
        }
        break;
    case Node::Kind::NODE48:
        for (auto i = 0u; i < node.childIndex.size(); ++i) {
            const auto pos = node.childIndex[i];
            if (pos == Node::EMPTY_MARKER) {
                continue;
            }
            key.push_back(static_cast<uint8_t>(i));
            collectEntries(*node.node48Children[pos], key, entries);
            key.pop_back();
        }
        break;
    case Node::Kind::NODE256:
        for (auto i = 0u; i < node.node256Children.size(); ++i) {
            if (!node.node256Children[i]) {
                continue;
            }
            key.push_back(static_cast<uint8_t>(i));
            collectEntries(*node.node256Children[i], key, entries);
            key.pop_back();
        }
        break;
    default:
        UNREACHABLE_CODE;
    }
}

void ArtPrimaryKeyIndex::checkpoint(main::ClientContext*, PageAllocator&) {
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
    auto storageInfoBufferReader =
        std::make_unique<BufferReader>(storageInfoBuffer.data(), storageInfoBuffer.size());
    auto storageInfo =
        ArtPrimaryKeyIndexStorageInfo::deserialize(std::move(storageInfoBufferReader));
    return std::make_unique<ArtPrimaryKeyIndex>(std::move(indexInfo), std::move(storageInfo));
}

} // namespace storage
} // namespace lbug
