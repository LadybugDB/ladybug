#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/type_utils.h"
#include "common/types/int128_t.h"
#include "common/types/string_t.h"
#include "common/types/uint128_t.h"
#include "storage/index/index.h"

namespace lbug {
namespace common {
struct BufferReader;
}
namespace storage {

class ArtKey {
public:
    ArtKey() = default;
    explicit ArtKey(std::vector<uint8_t> bytes) : bytes{std::move(bytes)} {}

    bool empty() const { return bytes.empty(); }
    const std::vector<uint8_t>& getBytes() const { return bytes; }

    static ArtKey encode(common::ValueVector* vector, uint64_t vectorPos);

private:
    std::vector<uint8_t> bytes;
};

struct ArtPrimaryKeyIndexStorageInfo final : IndexStorageInfo {
    std::vector<std::pair<std::vector<uint8_t>, common::offset_t>> entries;

    ArtPrimaryKeyIndexStorageInfo() = default;
    explicit ArtPrimaryKeyIndexStorageInfo(
        std::vector<std::pair<std::vector<uint8_t>, common::offset_t>> entries)
        : entries{std::move(entries)} {}
    DELETE_COPY_DEFAULT_MOVE(ArtPrimaryKeyIndexStorageInfo);

    std::shared_ptr<common::BufferWriter> serialize() const override;

    static std::unique_ptr<IndexStorageInfo> deserialize(
        std::unique_ptr<common::BufferReader> reader);
};

class ArtPrimaryKeyIndex final : public Index {
public:
    struct InsertState final : Index::InsertState {
        visible_func isVisible;

        explicit InsertState(visible_func isVisible) : isVisible{std::move(isVisible)} {}
    };

    explicit ArtPrimaryKeyIndex(IndexInfo indexInfo, std::unique_ptr<IndexStorageInfo> storageInfo);
    ~ArtPrimaryKeyIndex() override;

    static std::unique_ptr<ArtPrimaryKeyIndex> createNewIndex(IndexInfo indexInfo);

    std::unique_ptr<Index::InsertState> initInsertState(main::ClientContext*,
        visible_func isVisible) override;
    bool needCommitInsert() const override { return true; }
    void commitInsert(transaction::Transaction* transaction,
        const common::ValueVector& nodeIDVector,
        const std::vector<common::ValueVector*>& indexVectors,
        Index::InsertState& insertState) override;
    std::unique_ptr<UpdateState> initUpdateState(main::ClientContext* context,
        common::column_id_t columnID, visible_func isVisible) override;
    void update(transaction::Transaction* transaction, const common::ValueVector& nodeIDVector,
        common::ValueVector& propertyVector, UpdateState& updateState) override;

    std::unique_ptr<DeleteState> initDeleteState(const transaction::Transaction*, MemoryManager*,
        visible_func) override {
        return std::make_unique<DeleteState>();
    }
    void delete_(transaction::Transaction*, const common::ValueVector& nodeIDVector,
        DeleteState&) override;

    bool lookupPrimaryKey(const transaction::Transaction* transaction,
        common::ValueVector* keyVector, uint64_t vectorPos, common::offset_t& result,
        visible_func isVisible) override;
    bool lookupAll(const transaction::Transaction* transaction, common::ValueVector* keyVector,
        uint64_t vectorPos, std::vector<common::offset_t>& results,
        visible_func isVisible) override;
    bool scanPrimaryKeyRange(common::ValueVector* lowerBoundVector, uint64_t lowerBoundPos,
        bool lowerInclusive, common::ValueVector* upperBoundVector, uint64_t upperBoundPos,
        bool upperInclusive, common::idx_t maxResults, std::vector<common::offset_t>& results,
        visible_func isVisible) override;
    void discardPrimaryKey(common::ValueVector* keyVector) override;

    void checkpoint(main::ClientContext*, PageAllocator&) override;
    void serialize(common::Serializer& ser) const override;

    static LBUG_API std::unique_ptr<Index> load(main::ClientContext* context,
        StorageManager* storageManager, IndexInfo indexInfo, std::span<uint8_t> storageInfoBuffer);

    static IndexType getIndexType() {
        static const IndexType ART_INDEX_TYPE{"ART", IndexConstraintType::PRIMARY,
            IndexDefinitionType::BUILTIN, load};
        return ART_INDEX_TYPE;
    }

private:
    struct Node {
        static constexpr uint8_t EMPTY_MARKER = UINT8_MAX;

        enum class Kind : uint8_t { NODE4 = 0, NODE16 = 1, NODE48 = 2, NODE256 = 3 };
        struct SmallChildren {
            std::array<uint8_t, 16> keys{};
            std::array<Node*, 16> children{};
        };
        struct Node48Children {
            std::array<uint8_t, 256> childIndex{};
            std::array<Node*, 48> children{};
        };
        struct Node256Children {
            std::array<Node*, 256> children{};
        };

        std::optional<common::offset_t> offset;
        std::unique_ptr<std::vector<common::offset_t>> overflowOffsets;
        std::vector<uint8_t> prefix;
        Kind kind = Kind::NODE4;
        uint16_t count = 0;
        SmallChildren small;
        std::unique_ptr<Node48Children> node48;
        std::unique_ptr<Node256Children> node256;

        Node();
        Node* getChild(uint8_t byte) const;
        Node* getOrInsertChild(ArtPrimaryKeyIndex& index, uint8_t byte);
        void insertChild(ArtPrimaryKeyIndex& index, uint8_t byte, Node* child);
        void removeChild(uint8_t byte);
        bool hasOffsets() const {
            return offset.has_value() || (overflowOffsets && !overflowOffsets->empty());
        }
        bool empty() const {
            return !offset.has_value() && (!overflowOffsets || overflowOffsets->empty()) &&
                   count == 0;
        }
    };

    static constexpr uint64_t NODE_BLOCK_CAPACITY = 16 * 1024;

    struct NodeBlock {
        Node* nodes = nullptr;
        uint64_t used = 0;

        NodeBlock();
        ~NodeBlock();
        NodeBlock(const NodeBlock&) = delete;
        NodeBlock& operator=(const NodeBlock&) = delete;
        NodeBlock(NodeBlock&& other) noexcept;
        NodeBlock& operator=(NodeBlock&& other) noexcept;
    };

    bool insertInternal(const ArtKey& key, common::offset_t offset, visible_func isVisible);
    void insertSecondaryInternal(const ArtKey& key, common::offset_t offset);
    Node* findOrCreateLeaf(const std::vector<uint8_t>& key);
    bool lookup(const ArtKey& key, common::offset_t& result, visible_func isVisible) const;
    const Node* findLeaf(const ArtKey& key) const;
    void appendVisibleOffsets(const Node& node, std::vector<common::offset_t>& results,
        visible_func isVisible) const;
    bool eraseInternal(Node& node, const std::vector<uint8_t>& key, uint64_t depth);
    void erase(const ArtKey& key);
    static void eraseOffsetFromLeaf(Node& node, common::offset_t offset);
    static void resetNodePayload(Node& node);
    bool eraseOffsetInternal(Node& node, common::offset_t offset);
    Node* allocateNode();
    void recordKindChange(Node& node, Node::Kind newKind);
    void collectRange(const Node& node, std::vector<uint8_t>& key, const ArtKey* lowerBound,
        bool lowerInclusive, const ArtKey* upperBound, bool upperInclusive,
        common::idx_t maxResults, std::vector<common::offset_t>& results,
        visible_func isVisible) const;
    void clear();
    uint64_t calculateSerializedTreeSize(const Node& node) const;
    void serializeTree(const Node& node, common::Serializer& serializer) const;
    void loadTree(common::BufferReader& reader, Node& node);
    void collectEntries(const Node& node, std::vector<uint8_t>& key,
        std::vector<std::pair<std::vector<uint8_t>, common::offset_t>>& entries) const;
    void loadEntries(const ArtPrimaryKeyIndexStorageInfo& storageInfo);
    void loadEntries(common::BufferReader& reader);

private:
    Node root;
    std::vector<NodeBlock> nodeBlocks;
    uint64_t numAllocatedNodes = 1;
    std::array<uint64_t, 4> numNodesByKind{1, 0, 0, 0};
    mutable std::mutex mutex;
};

} // namespace storage
} // namespace lbug
