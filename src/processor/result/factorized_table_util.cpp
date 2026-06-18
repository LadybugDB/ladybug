#include "processor/result/factorized_table_util.h"

using namespace lbug::storage;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::planner;

namespace lbug {
namespace processor {

FactorizedTableSchema FactorizedTableUtils::createFTableSchema(const expression_vector& exprs,
    const Schema& schema) {
    auto tableSchema = FactorizedTableSchema();
    std::unordered_set<common::idx_t> groupIDSet;
    for (auto& e : exprs) {
        auto groupPos = schema.getExpressionPos(*e).first;
        auto group = schema.getGroup(groupPos);
        if (group->isFlat()) {
            auto column =
                ColumnSchema(false, groupPos, LogicalTypeUtils::getRowLayoutSize(e->getDataType()));
            tableSchema.appendColumn(std::move(column));
        } else {
            auto column = ColumnSchema(true, groupPos, (uint32_t)sizeof(overflow_value_t));
            tableSchema.appendColumn(std::move(column));
        }
    }
    return tableSchema;
}

FactorizedTableSchema FactorizedTableUtils::createFlatTableSchema(
    std::vector<LogicalType> columnTypes) {
    auto tableSchema = FactorizedTableSchema();
    for (auto& type : columnTypes) {
        auto column = ColumnSchema(false /* isUnFlat */, 0 /* groupID */,
            LogicalTypeUtils::getRowLayoutSize(type));
        tableSchema.appendColumn(std::move(column));
    }
    return tableSchema;
}

void FactorizedTableUtils::appendStringToTable(FactorizedTable* factorizedTable,
    const std::string& outputMsg, MemoryManager* memoryManager) {
    auto outputMsgVector = std::make_shared<ValueVector>(LogicalTypeID::STRING, memoryManager);
    outputMsgVector->state = DataChunkState::getSingleValueDataChunkState();
    auto outputStr = string_t();
    outputStr.overflowPtr =
        reinterpret_cast<uint64_t>(StringVector::getInMemOverflowBuffer(outputMsgVector.get())
                                       ->allocateSpace(outputMsg.length()));
    outputStr.set(outputMsg);
    outputMsgVector->setValue(0, outputStr);
    factorizedTable->append(std::vector<ValueVector*>{outputMsgVector.get()});
}

void FactorizedTableUtils::appendNodeCopyResultToTable(FactorizedTable* factorizedTable,
    const std::string& outputMsg, int64_t skippedDuplicatePKCount,
    const std::vector<std::string>& skippedDuplicatePKs, MemoryManager* memoryManager) {
    auto outputMsgVector = std::make_shared<ValueVector>(LogicalTypeID::STRING, memoryManager);
    auto skippedCountVector = std::make_shared<ValueVector>(LogicalTypeID::INT64, memoryManager);
    auto skippedPKVector =
        std::make_shared<ValueVector>(LogicalType::LIST(LogicalType::STRING()), memoryManager);

    outputMsgVector->state = DataChunkState::getSingleValueDataChunkState();
    skippedCountVector->state = DataChunkState::getSingleValueDataChunkState();
    skippedPKVector->state = DataChunkState::getSingleValueDataChunkState();

    auto outputStr = string_t();
    outputStr.overflowPtr =
        reinterpret_cast<uint64_t>(StringVector::getInMemOverflowBuffer(outputMsgVector.get())
                                       ->allocateSpace(outputMsg.length()));
    outputStr.set(outputMsg);
    outputMsgVector->setValue(0, outputStr);
    skippedCountVector->setValue<int64_t>(0, skippedDuplicatePKCount);

    auto listEntry = ListVector::addList(skippedPKVector.get(), skippedDuplicatePKs.size());
    skippedPKVector->setValue<list_entry_t>(0, listEntry);
    auto dataVector = ListVector::getDataVector(skippedPKVector.get());
    for (auto i = 0u; i < skippedDuplicatePKs.size(); ++i) {
        dataVector->setValue<std::string>(listEntry.offset + i, skippedDuplicatePKs[i]);
    }

    factorizedTable->append(std::vector<ValueVector*>{outputMsgVector.get(),
        skippedCountVector.get(), skippedPKVector.get()});
}

std::shared_ptr<FactorizedTable> FactorizedTableUtils::getFactorizedTableForOutputMsg(
    const std::string& outputMsg, MemoryManager* memoryManager) {
    auto table = getSingleStringColumnFTable(memoryManager);
    appendStringToTable(table.get(), outputMsg, memoryManager);
    return table;
}

std::shared_ptr<FactorizedTable> FactorizedTableUtils::getSingleStringColumnFTable(
    MemoryManager* mm) {
    std::vector<LogicalType> typeVec;
    typeVec.push_back(LogicalType::STRING());
    auto fTableSchema = createFlatTableSchema(std::move(typeVec));
    return std::make_shared<FactorizedTable>(mm, std::move(fTableSchema));
}

std::shared_ptr<FactorizedTable> FactorizedTableUtils::getNodeCopyResultFTable(MemoryManager* mm) {
    std::vector<LogicalType> typeVec;
    typeVec.push_back(LogicalType::STRING());
    typeVec.push_back(LogicalType::INT64());
    typeVec.push_back(LogicalType::LIST(LogicalType::STRING()));
    auto fTableSchema = createFlatTableSchema(std::move(typeVec));
    return std::make_shared<FactorizedTable>(mm, std::move(fTableSchema));
}

} // namespace processor
} // namespace lbug
