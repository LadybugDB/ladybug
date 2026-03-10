#include "binder/binder.h"
#include "common/exception/runtime.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace storage {

static void serializeAlterExtraInfo(Serializer& serializer, const BoundAlterInfo* alterInfo) {
    const auto* extraInfo = alterInfo->extraInfo.get();
    serializer.write(alterInfo->alterType);
    serializer.write(alterInfo->tableName);
    switch (alterInfo->alterType) {
    case AlterType::ADD_PROPERTY: {
        auto addInfo = extraInfo->constPtrCast<BoundExtraAddPropertyInfo>();
        addInfo->propertyDefinition.serialize(serializer);
    } break;
    case AlterType::DROP_PROPERTY: {
        auto dropInfo = extraInfo->constPtrCast<BoundExtraDropPropertyInfo>();
        serializer.write(dropInfo->propertyName);
    } break;
    case AlterType::RENAME_PROPERTY: {
        auto renamePropertyInfo = extraInfo->constPtrCast<BoundExtraRenamePropertyInfo>();
        serializer.write(renamePropertyInfo->newName);
        serializer.write(renamePropertyInfo->oldName);
    } break;
    case AlterType::COMMENT: {
        auto commentInfo = extraInfo->constPtrCast<BoundExtraCommentInfo>();
        serializer.write(commentInfo->comment);
    } break;
    case AlterType::RENAME: {
        auto renameTableInfo = extraInfo->constPtrCast<BoundExtraRenameTableInfo>();
        serializer.write(renameTableInfo->newName);
    } break;
    case AlterType::ADD_FROM_TO_CONNECTION:
    case AlterType::DROP_FROM_TO_CONNECTION: {
        auto connectionInfo = extraInfo->constPtrCast<BoundExtraAlterFromToConnection>();
        serializer.write(connectionInfo->fromTableID);
        serializer.write(connectionInfo->toTableID);
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

static decltype(auto) deserializeAlterRecord(Deserializer& deserializer) {
    auto alterType = AlterType::INVALID;
    std::string tableName;
    deserializer.deserializeValue(alterType);
    deserializer.deserializeValue(tableName);
    std::unique_ptr<BoundExtraAlterInfo> extraInfo;
    switch (alterType) {
    case AlterType::ADD_PROPERTY: {
        auto definition = PropertyDefinition::deserialize(deserializer);
        extraInfo = std::make_unique<BoundExtraAddPropertyInfo>(std::move(definition), nullptr);
    } break;
    case AlterType::DROP_PROPERTY: {
        std::string propertyName;
        deserializer.deserializeValue(propertyName);
        extraInfo = std::make_unique<BoundExtraDropPropertyInfo>(std::move(propertyName));
    } break;
    case AlterType::RENAME_PROPERTY: {
        std::string newName;
        std::string oldName;
        deserializer.deserializeValue(newName);
        deserializer.deserializeValue(oldName);
        extraInfo =
            std::make_unique<BoundExtraRenamePropertyInfo>(std::move(newName), std::move(oldName));
    } break;
    case AlterType::COMMENT: {
        std::string comment;
        deserializer.deserializeValue(comment);
        extraInfo = std::make_unique<BoundExtraCommentInfo>(std::move(comment));
    } break;
    case AlterType::RENAME: {
        std::string newName;
        deserializer.deserializeValue(newName);
        extraInfo = std::make_unique<BoundExtraRenameTableInfo>(std::move(newName));
    } break;
    case AlterType::ADD_FROM_TO_CONNECTION:
    case AlterType::DROP_FROM_TO_CONNECTION: {
        table_id_t fromTableID = INVALID_TABLE_ID;
        table_id_t toTableID = INVALID_TABLE_ID;
        deserializer.deserializeValue(fromTableID);
        deserializer.deserializeValue(toTableID);
        extraInfo = std::make_unique<BoundExtraAlterFromToConnection>(fromTableID, toTableID);
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
    return std::make_tuple(alterType, tableName, std::move(extraInfo));
}

void AlterTableEntryRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializeAlterExtraInfo(serializer, alterInfo);
}

std::unique_ptr<AlterTableEntryRecord> AlterTableEntryRecord::deserialize(
    Deserializer& deserializer) {
    auto [alterType, tableName, extraInfo] = deserializeAlterRecord(deserializer);
    auto retval = std::make_unique<AlterTableEntryRecord>();
    retval->ownedAlterInfo =
        std::make_unique<BoundAlterInfo>(alterType, tableName, std::move(extraInfo));
    return retval;
}

} // namespace storage
} // namespace lbug
