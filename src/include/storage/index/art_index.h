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

    std::unique_ptr<DeleteState> initDeleteState(const transaction::Transaction*, MemoryManager*,
        visible_func) override {
        return std::make_unique<DeleteState>();
    }
    void delete_(transaction::Transaction*, const common::ValueVector&, DeleteState&) override {
        // Visibility rules filter deleted rows. Physical removal is used only for rollback cleanup.
    }

    bool lookupPrimaryKey(const transaction::Transaction* transaction,
        common::ValueVector* keyVector, uint64_t vectorPos, common::offset_t& result,
        visible_func isVisible) override;
    bool scanPrimaryKeyRange(common::ValueVector* lowerBoundVector, uint64_t lowerBoundPos,
        bool lowerInclusive, common::ValueVector* upperBoundVector, uint64_t upperBoundPos,
        bool upperInclusive, common::idx_t maxResults, std::vector<common::offset_t>& results,
        visible_func isVisible) override;
    void discardPrimaryKey(common::ValueVector* keyVector) override;

    void checkpoint(main::ClientContext*, PageAllocator&) override;

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
        Kind kind = Kind::NODE4;
        uint16_t count = 0;
        union {
            SmallChildren small;
            Node48Children node48;
            Node256Children node256;
        };

        Node();
        ~Node();
        Node* getChild(uint8_t byte) const;
        Node* getOrInsertChild(uint8_t byte);
        void removeChild(uint8_t byte);
        void moveChildrenTo(std::vector<Node*>& children);
        bool empty() const { return !offset.has_value() && count == 0; }
    };

    bool insertInternal(const ArtKey& key, common::offset_t offset, visible_func isVisible);
    bool lookup(const ArtKey& key, common::offset_t& result, visible_func isVisible) const;
    bool eraseInternal(Node& node, const std::vector<uint8_t>& key, uint64_t depth);
    void erase(const ArtKey& key);
    static void deleteTree(Node* root);
    void collectRange(const Node& node, std::vector<uint8_t>& key, const ArtKey* lowerBound,
        bool lowerInclusive, const ArtKey* upperBound, bool upperInclusive,
        common::idx_t maxResults, std::vector<common::offset_t>& results,
        visible_func isVisible) const;
    void clear();
    void collectEntries(const Node& node, std::vector<uint8_t>& key,
        std::vector<std::pair<std::vector<uint8_t>, common::offset_t>>& entries) const;
    void loadEntries(const ArtPrimaryKeyIndexStorageInfo& storageInfo);

private:
    Node root;
    mutable std::mutex mutex;
};

} // namespace storage
} // namespace lbug
