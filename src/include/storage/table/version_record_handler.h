#pragma once

#include "common/types/types.h"
#include "transaction/transaction.h"

namespace lbug {

namespace catalog {
class Catalog;
} // namespace catalog

namespace storage {

class ChunkedNodeGroup;

using version_record_handler_op_t = void (
    ChunkedNodeGroup::*)(common::row_idx_t, common::row_idx_t, common::transaction_t);

// Note: these handlers are not safe to use in multi-threaded contexts without external locking
class VersionRecordHandler {
public:
    explicit VersionRecordHandler(catalog::Catalog* ownerCatalog) : ownerCatalog{ownerCatalog} {}
    virtual ~VersionRecordHandler() = default;

    catalog::Catalog* getOwnerCatalog() const { return ownerCatalog; }

    virtual void applyFuncToChunkedGroups(version_record_handler_op_t func,
        common::node_group_idx_t nodeGroupIdx, common::row_idx_t startRow,
        common::row_idx_t numRows, common::transaction_t commitTS) const = 0;

    virtual void rollbackInsert(main::ClientContext* context, common::node_group_idx_t nodeGroupIdx,
        common::row_idx_t startRow, common::row_idx_t numRows) const;

private:
    catalog::Catalog* ownerCatalog;
};

} // namespace storage
} // namespace lbug
