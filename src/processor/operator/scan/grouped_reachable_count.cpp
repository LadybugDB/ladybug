#include "processor/operator/scan/grouped_reachable_count.h"

#include <limits>
#include <unordered_map>

#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void GroupedReachableCount::initLocalStateInternal(ResultSet* resultSet,
    ExecutionContext* context) {
    for (auto& pos : keyOutputPoss) {
        keyVectors.push_back(resultSet->getValueVector(pos).get());
    }
    if (hasCount) {
        countVector = resultSet->getValueVector(countOutputPos).get();
    }
    if (hasAvg) {
        avgVector = resultSet->getValueVector(avgOutputPos).get();
    }
    computed = false;
    emitIdx = 0;
    groups.clear();

    graph = std::make_unique<OnDiskGraph>(context->clientContext, graphEntry.copy());
    relInfos = graph->getRelInfos(srcTableID);
    for (auto& relInfo : relInfos) {
        auto scanState = graph->prepareRelScan(*relInfo.relGroupEntry, relInfo.relTableID,
            relInfo.dstTableID, /*relProperties=*/std::vector<std::string>{});
        scanStates.push_back(std::move(scanState));
    }
}

void GroupedReachableCount::appendToKey(std::string& out, const KeyValue& key,
    common::PhysicalTypeID physicalType) {
    if (key.isNull) {
        out.push_back('\x01');
        return;
    }
    out.push_back('\x00');
    switch (physicalType) {
    case PhysicalTypeID::BOOL:
    case PhysicalTypeID::INT8:
    case PhysicalTypeID::INT16:
    case PhysicalTypeID::INT32:
    case PhysicalTypeID::INT64:
    case PhysicalTypeID::UINT8:
    case PhysicalTypeID::UINT16:
    case PhysicalTypeID::UINT32:
    case PhysicalTypeID::UINT64: {
        out.append(reinterpret_cast<const char*>(&key.intVal), sizeof(key.intVal));
    } break;
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::DOUBLE: {
        out.append(reinterpret_cast<const char*>(&key.dblVal), sizeof(key.dblVal));
    } break;
    case PhysicalTypeID::STRING: {
        // Length-prefixed: a NUL-terminated encoding would let ("a\0", "b") and
        // ("a", "\0b") serialize identically and merge two distinct groups.
        auto len = key.strVal.size();
        out.append(reinterpret_cast<const char*>(&len), sizeof(len));
        out.append(key.strVal);
    } break;
    case PhysicalTypeID::INTERNAL_ID: {
        out.append(reinterpret_cast<const char*>(&key.nodeID.tableID), sizeof(table_id_t));
        out.append(reinterpret_cast<const char*>(&key.nodeID.offset), sizeof(offset_t));
    } break;
    default:
        // Unreachable: the optimizer only admits fixed-size numeric, string and internal-ID
        // keys. Throw rather than append nothing, which would silently alias this key onto
        // whatever follows it.
        throw RuntimeException(
            "Unsupported group key type in grouped reachable-count aggregation.");
    }
}

void GroupedReachableCount::initLookupState(ExecutionContext* context, Transaction* transaction) {
    auto* mm = storage::MemoryManager::Get(*context->clientContext);
    lookupChunkState = DataChunkState::getSingleValueDataChunkState();
    lookupIDVector =
        std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm, lookupChunkState);
    lookupIDVector->state = lookupChunkState;
    for (auto i = 0u; i < keyInfos.size(); ++i) {
        if (keyInfos[i].isInternalID) {
            continue;
        }
        lookupKeyVectors.push_back(
            std::make_unique<ValueVector>(keyInfos[i].logicalType.copy(), mm, lookupChunkState));
        lookupKeyVectors.back()->state = lookupChunkState;
        lookupOutVectors.push_back(lookupKeyVectors.back().get());
        lookupColumnIDs.push_back(keyInfos[i].columnID);
        lookupKeyIdxByColumn.push_back(i);
    }
    if (hasAvg) {
        lookupScoreVector = std::make_unique<ValueVector>(scoreType.copy(), mm, lookupChunkState);
        lookupScoreVector->state = lookupChunkState;
        lookupOutVectors.push_back(lookupScoreVector.get());
        lookupColumnIDs.push_back(scoreColumnID);
        lookupKeyIdxByColumn.push_back(keyInfos.size());
    }
    lookupScanState = std::make_unique<NodeTableScanState>(lookupIDVector.get(), lookupOutVectors,
        lookupChunkState);
    lookupScanState->setToTable(transaction, nodeTable, lookupColumnIDs, {});
}
void GroupedReachableCount::readNodeProperties(Transaction* transaction, offset_t offset,
    std::vector<KeyValue>& keys, bool& scoreIsNull, double& score) {
    // Reset the reused vectors to the state a freshly allocated one would have, so a value
    // left over from the previous node cannot be mistaken for this node's.
    lookupChunkState->getSelVectorUnsafe().setToUnfiltered(1);
    lookupIDVector->setNull(0, false);
    for (auto* vec : lookupOutVectors) {
        vec->resetAuxiliaryBuffer();
        vec->setNull(0, true);
    }
    nodeTable->initScanState(transaction, *lookupScanState, dstTableID, offset);
    lookupIDVector->setValue<nodeID_t>(0, nodeID_t{offset, dstTableID});
    if (!nodeTable->lookup(transaction, *lookupScanState)) {
        // Deleted or otherwise invisible: contributes no rows.
        keys.clear();
        return;
    }
    keys.resize(keyInfos.size());
    scoreIsNull = true;
    score = 0.0;
    for (auto c = 0u; c < lookupOutVectors.size(); ++c) {
        auto* vec = lookupOutVectors[c];
        auto keyIdx = lookupKeyIdxByColumn[c];
        if (keyIdx < keyInfos.size()) {
            auto& key = keys[keyIdx];
            if (keyInfos[keyIdx].isInternalID) {
                continue; // Filled by the caller from the offset.
            }
            key.isNull = vec->isNull(0);
            if (key.isNull) {
                continue;
            }
            switch (vec->dataType.getPhysicalType()) {
            case PhysicalTypeID::BOOL:
                key.intVal = vec->getValue<bool>(0) ? 1 : 0;
                break;
            case PhysicalTypeID::INT8:
                key.intVal = vec->getValue<int8_t>(0);
                break;
            case PhysicalTypeID::INT16:
                key.intVal = vec->getValue<int16_t>(0);
                break;
            case PhysicalTypeID::INT32:
                key.intVal = vec->getValue<int32_t>(0);
                break;
            case PhysicalTypeID::INT64:
                key.intVal = vec->getValue<int64_t>(0);
                break;
            case PhysicalTypeID::UINT8:
                key.intVal = static_cast<int64_t>(vec->getValue<uint8_t>(0));
                break;
            case PhysicalTypeID::UINT16:
                key.intVal = static_cast<int64_t>(vec->getValue<uint16_t>(0));
                break;
            case PhysicalTypeID::UINT32:
                key.intVal = static_cast<int64_t>(vec->getValue<uint32_t>(0));
                break;
            case PhysicalTypeID::UINT64:
                key.intVal = static_cast<int64_t>(vec->getValue<uint64_t>(0));
                break;
            case PhysicalTypeID::FLOAT:
                key.dblVal = vec->getValue<float>(0);
                break;
            case PhysicalTypeID::DOUBLE:
                key.dblVal = vec->getValue<double>(0);
                break;
            case PhysicalTypeID::STRING:
                key.strVal = vec->getValue<string_t>(0).getAsString();
                break;
            default:
                throw RuntimeException(
                    "Unsupported group key type in grouped reachable-count aggregation.");
            }
        } else {
            scoreIsNull = vec->isNull(0);
            if (scoreIsNull) {
                continue;
            }
            switch (vec->dataType.getPhysicalType()) {
            case PhysicalTypeID::INT8:
                score = vec->getValue<int8_t>(0);
                break;
            case PhysicalTypeID::INT16:
                score = vec->getValue<int16_t>(0);
                break;
            case PhysicalTypeID::INT32:
                score = vec->getValue<int32_t>(0);
                break;
            case PhysicalTypeID::INT64:
                score = static_cast<double>(vec->getValue<int64_t>(0));
                break;
            case PhysicalTypeID::UINT8:
                score = vec->getValue<uint8_t>(0);
                break;
            case PhysicalTypeID::UINT16:
                score = vec->getValue<uint16_t>(0);
                break;
            case PhysicalTypeID::UINT32:
                score = vec->getValue<uint32_t>(0);
                break;
            case PhysicalTypeID::UINT64:
                score = static_cast<double>(vec->getValue<uint64_t>(0));
                break;
            case PhysicalTypeID::FLOAT:
                score = vec->getValue<float>(0);
                break;
            case PhysicalTypeID::DOUBLE:
                score = vec->getValue<double>(0);
                break;
            default:
                throw RuntimeException(
                    "Unsupported AVG input type in grouped reachable-count aggregation.");
            }
        }
    }
}

void GroupedReachableCount::addWalk(uint64_t& target, uint64_t addend, uint64_t limit) {
    if (addend > limit - target) {
        throw RuntimeException(
            "The number of walks matched by the variable-length pattern exceeds the 64-bit "
            "counter range. Narrow the pattern's upper bound (e.g. *1..3 instead of *1..30) or "
            "restrict the set of source nodes.");
    }
    target += addend;
}

void GroupedReachableCount::compute(ExecutionContext* context) {
    DASSERT(direction == RelDataDirection::FWD);
    auto* clientContext = context->clientContext;
    auto* transaction = Transaction::Get(*clientContext);
    auto maxOffset = graph->getMaxOffset(transaction, srcTableID);
    initLookupState(context, transaction);

    // Per-destination walk counts: L_0[v] = 1 per visible source, L_{d+1} propagated over
    // forward edges. The walk count of `v` is sum_{d in [lo, up]} L_d[v].
    std::vector<uint64_t> cur(maxOffset, 0), next(maxOffset, 0), total(maxOffset, 0);
    for (offset_t offset = 0; offset < maxOffset; ++offset) {
        if (nodeTable->isVisible(transaction, offset)) {
            cur[offset] = 1;
            if (lowerBound == 0) {
                total[offset] = 1;
            }
        }
    }
    // uint32_t, not uint16_t: varLengthMaxDepth is user-settable without an upper clamp, so
    // upperBound can be UINT16_MAX and a uint16_t counter would wrap and loop forever.
    for (uint32_t depth = 1; depth <= upperBound; ++depth) {
        std::fill(next.begin(), next.end(), 0);
        for (offset_t offset = 0; offset < maxOffset; ++offset) {
            auto count = cur[offset];
            if (count == 0) {
                continue;
            }
            nodeID_t nodeID{offset, srcTableID};
            for (auto& scanState : scanStates) {
                for (auto chunk : graph->scanFwd(nodeID, *scanState)) {
                    chunk.forEach([&](auto neighbors, auto /*propertyVectors*/, auto i) {
                        auto nbr = neighbors[i];
                        if (nbr.tableID != dstTableID) {
                            return;
                        }
                        addWalk(next[nbr.offset], count, std::numeric_limits<uint64_t>::max());
                    });
                }
            }
        }
        if (depth >= lowerBound) {
            for (offset_t offset = 0; offset < maxOffset; ++offset) {
                addWalk(total[offset], next[offset], std::numeric_limits<uint64_t>::max());
            }
        }
        cur.swap(next);
    }

    std::unordered_map<std::string, size_t> groupIndex;
    std::vector<KeyValue> keys;
    for (offset_t offset = 0; offset < maxOffset; ++offset) {
        auto walkCount = total[offset];
        if (walkCount == 0) {
            continue;
        }
        bool scoreIsNull = true;
        double score = 0.0;
        readNodeProperties(transaction, offset, keys, scoreIsNull, score);
        if (keys.empty()) {
            continue; // Lookup failed (concurrently deleted); contributes no rows.
        }
        for (auto i = 0u; i < keyInfos.size(); ++i) {
            if (keyInfos[i].isInternalID) {
                keys[i].isNull = false;
                keys[i].nodeID = nodeID_t{offset, dstTableID};
            }
        }
        std::string serialized;
        for (auto i = 0u; i < keys.size(); ++i) {
            appendToKey(serialized, keys[i], keyInfos[i].logicalType.getPhysicalType());
        }
        auto it = groupIndex.find(serialized);
        size_t idx;
        if (it == groupIndex.end()) {
            idx = groups.size();
            groupIndex.emplace(std::move(serialized), idx);
            GroupAccumulator group;
            group.keys = std::move(keys);
            groups.push_back(std::move(group));
            keys.clear();
        } else {
            idx = it->second;
        }
        auto& group = groups[idx];
        // The COUNT is emitted as a signed 64-bit integer, so cap the accumulator there
        // rather than at the uint64 range.
        addWalk(group.count, walkCount, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
        if (hasAvg && !scoreIsNull) {
            group.avgSum += static_cast<double>(walkCount) * score;
            addWalk(group.avgCount, walkCount, std::numeric_limits<uint64_t>::max());
        }
    }
}

void GroupedReachableCount::emitRow(uint32_t pos, const GroupAccumulator& group) {
    for (auto i = 0u; i < keyVectors.size(); ++i) {
        auto& key = group.keys[i];
        auto* vec = keyVectors[i];
        if (key.isNull) {
            vec->setNull(pos, true);
            continue;
        }
        vec->setNull(pos, false);
        switch (keyInfos[i].logicalType.getPhysicalType()) {
        case PhysicalTypeID::BOOL:
            vec->setValue<bool>(pos, key.intVal != 0);
            break;
        case PhysicalTypeID::INT8:
            vec->setValue<int8_t>(pos, static_cast<int8_t>(key.intVal));
            break;
        case PhysicalTypeID::INT16:
            vec->setValue<int16_t>(pos, static_cast<int16_t>(key.intVal));
            break;
        case PhysicalTypeID::INT32:
            vec->setValue<int32_t>(pos, static_cast<int32_t>(key.intVal));
            break;
        case PhysicalTypeID::INT64:
            vec->setValue<int64_t>(pos, key.intVal);
            break;
        case PhysicalTypeID::UINT8:
            vec->setValue<uint8_t>(pos, static_cast<uint8_t>(key.intVal));
            break;
        case PhysicalTypeID::UINT16:
            vec->setValue<uint16_t>(pos, static_cast<uint16_t>(key.intVal));
            break;
        case PhysicalTypeID::UINT32:
            vec->setValue<uint32_t>(pos, static_cast<uint32_t>(key.intVal));
            break;
        case PhysicalTypeID::UINT64:
            vec->setValue<uint64_t>(pos, static_cast<uint64_t>(key.intVal));
            break;
        case PhysicalTypeID::FLOAT:
            vec->setValue<float>(pos, static_cast<float>(key.dblVal));
            break;
        case PhysicalTypeID::DOUBLE:
            vec->setValue<double>(pos, key.dblVal);
            break;
        case PhysicalTypeID::STRING:
            StringVector::addString(vec, pos, key.strVal.data(), key.strVal.size());
            break;
        case PhysicalTypeID::INTERNAL_ID:
            vec->setValue<nodeID_t>(pos, key.nodeID);
            break;
        default:
            throw RuntimeException(
                "Unsupported group key type in grouped reachable-count aggregation.");
        }
    }
    if (hasCount) {
        countVector->setNull(pos, false);
        countVector->setValue<int64_t>(pos, static_cast<int64_t>(group.count));
    }
    if (hasAvg) {
        if (group.avgCount == 0) {
            avgVector->setNull(pos, true);
        } else {
            avgVector->setNull(pos, false);
            avgVector->setValue<double>(pos, group.avgSum / static_cast<double>(group.avgCount));
        }
    }
}

bool GroupedReachableCount::getNextTuplesInternal(ExecutionContext* context) {
    if (!computed) {
        compute(context);
        computed = true;
    }
    if (emitIdx >= groups.size()) {
        return false;
    }
    auto numRows = std::min<uint64_t>(DEFAULT_VECTOR_CAPACITY, groups.size() - emitIdx);
    // All output vectors of the result chunk share one DataChunkState, so setting the
    // selection vector through any of them marks every column.
    keyVectors[0]->state->getSelVectorUnsafe().setToUnfiltered(numRows);
    for (auto i = 0u; i < numRows; ++i) {
        emitRow(i, groups[emitIdx + i]);
    }
    emitIdx += numRows;
    return true;
}

} // namespace processor
} // namespace lbug
