#include "processor/operator/scan/scan_multi_rel_tables.h"

#include "processor/execution_context.h"
#include "storage/local_storage/local_storage.h"
#include "storage/table/arrow_rel_table.h"
#include "storage/table/ice_disk_rel_table.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

bool DirectionInfo::needFlip(RelDataDirection relDataDirection) const {
    if (extendFromSource && relDataDirection == RelDataDirection::BWD) {
        return true;
    }
    if (!extendFromSource && relDataDirection == RelDataDirection::FWD) {
        return true;
    }
    return false;
}

bool RelTableCollectionScanner::scan(main::ClientContext* context, RelTableScanState& scanState,
    const std::vector<ValueVector*>& outVectors) {
    auto transaction = Transaction::Get(*context);
    auto initNextTable = [&]() -> bool {
        currentTableIdx = nextTableIdx;
        if (currentTableIdx == relInfos.size()) {
            return false;
        }
        auto& currentInfo = relInfos[currentTableIdx];
        currentInfo.initScanState(scanState, outVectors, context);
        currentInfo.table->initScanState(transaction, scanState, currentTableIdx == 0);
        nextTableIdx++;
        return true;
    };
    if (currentTableIdx == INVALID_IDX && !initNextTable()) {
        return false;
    }
    while (true) {
        auto& relInfo = relInfos[currentTableIdx];
        if (relInfo.table->scan(transaction, scanState)) {
            auto& selVector = scanState.outState->getSelVector();
            if (directionVector != nullptr) {
                for (auto i = 0u; i < selVector.getSelSize(); ++i) {
                    directionVector->setValue<bool>(selVector[i], directionValues[currentTableIdx]);
                }
            }
            if (selVector.getSelSize() > 0) {
                relInfo.castColumns();
                return true;
            }
        } else {
            if (!initNextTable()) {
                return false;
            }
        }
    }
}

void ScanMultiRelTable::refreshNbrMaskCache() {
    nbrEnabledMasks.clear();
    nbrSingleEnabledMask = nullptr;
    if (nbrNodeMaskMap == nullptr) {
        return;
    }
    for (auto& [tableID, mask] : nbrNodeMaskMap->getMasks()) {
        if (mask->isEnabled()) {
            nbrEnabledMasks.emplace_back(tableID, mask);
        }
    }
    if (nbrEnabledMasks.size() == 1) {
        nbrSingleEnabledMask = nbrEnabledMasks[0].second;
    }
}

common::sel_t ScanMultiRelTable::applyNbrNodeMask() {
    auto& selVector = scanState->outState->getSelVectorUnsafe();
    const auto selSize = selVector.getSelSize();
    if (nbrSingleEnabledMask == nullptr && nbrEnabledMasks.empty()) {
        return selSize;
    }
    auto* nbrVector = outVectors[0];
    auto buffer = selVector.getMutableBuffer();
    sel_t selectedSize = 0;
    if (nbrSingleEnabledMask != nullptr) {
        for (auto i = 0u; i < selSize; ++i) {
            auto pos = selVector[i];
            buffer[selectedSize] = pos;
            selectedSize +=
                nbrSingleEnabledMask->isMasked(nbrVector->getValue<nodeID_t>(pos).offset);
        }
    } else {
        for (auto i = 0u; i < selSize; ++i) {
            auto pos = selVector[i];
            auto nbrID = nbrVector->getValue<nodeID_t>(pos);
            buffer[selectedSize] = pos;
            auto keep = true;
            for (auto& [tableID, mask] : nbrEnabledMasks) {
                if (nbrID.tableID == tableID) {
                    keep = mask->isMasked(nbrID.offset);
                    break;
                }
            }
            selectedSize += keep;
        }
    }
    if (selectedSize == selSize) {
        return selSize;
    }
    selVector.setToFiltered(selectedSize);
    return selectedSize;
}

void ScanMultiRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    ScanTable::initLocalStateInternal(resultSet, context);
    refreshNbrMaskCache();
    auto clientContext = context->clientContext;
    boundNodeIDVector = resultSet->getValueVector(opInfo.nodeIDPos).get();
    auto nbrNodeIDVector = outVectors[0];

    // Check if any table in any scanner is an external rel table with a custom scan state.
    bool hasArrowTable = false;
    bool hasIceDiskTable = false;
    for (auto& [_, scanner] : scanners) {
        for (auto& relInfo : scanner.relInfos) {
            if (dynamic_cast<storage::ArrowRelTable*>(relInfo.table) != nullptr) {
                hasArrowTable = true;
                break;
            }
            if (dynamic_cast<storage::IceDiskRelTable*>(relInfo.table) != nullptr) {
                hasIceDiskTable = true;
                break;
            }
        }
        if (hasArrowTable || hasIceDiskTable) {
            break;
        }
    }

    // IceDisk scan state extends the common rel scan state and Arrow stores its per-table state
    // there, so one scan state can now cover IceDisk, Arrow, and native rel tables.
    if (hasIceDiskTable) {
        scanState =
            std::make_unique<storage::IceDiskRelTableScanState>(*MemoryManager::Get(*clientContext),
                boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    } else if (hasArrowTable) {
        scanState =
            std::make_unique<storage::ArrowRelTableScanState>(*MemoryManager::Get(*clientContext),
                boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    } else {
        scanState = std::make_unique<RelTableScanState>(*MemoryManager::Get(*clientContext),
            boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    }
    for (auto& [_, scanner] : scanners) {
        for (auto& relInfo : scanner.relInfos) {
            if (directionInfo.directionPos.isValid()) {
                scanner.directionVector =
                    resultSet->getValueVector(directionInfo.directionPos).get();
                scanner.directionValues.push_back(directionInfo.needFlip(relInfo.direction));
            }
        }
    }
    currentScanner = nullptr;
}

bool ScanMultiRelTable::getNextTuplesInternal(ExecutionContext* context) {
    while (true) {
        if (currentScanner != nullptr &&
            currentScanner->scan(context->clientContext, *scanState, outVectors)) {
            const auto filteredSize = applyNbrNodeMask();
            if (filteredSize == 0) {
                continue;
            }
            metrics->numOutputTuple.increase(filteredSize);
            return true;
        }
        if (!children[0]->getNextTuple(context)) {
            resetState();
            return false;
        }
        const auto currentIdx = boundNodeIDVector->state->getSelVector()[0];
        if (boundNodeIDVector->isNull(currentIdx)) {
            currentScanner = nullptr;
            continue;
        }
        auto nodeID = boundNodeIDVector->getValue<nodeID_t>(currentIdx);
        initCurrentScanner(nodeID);
    }
}

void ScanMultiRelTable::resetState() {
    currentScanner = nullptr;
    for (auto& [_, scanner] : scanners) {
        scanner.resetState();
    }
}

void ScanMultiRelTable::initCurrentScanner(const nodeID_t& nodeID) {
    if (scanners.contains(nodeID.tableID)) {
        currentScanner = &scanners.at(nodeID.tableID);
        currentScanner->resetState();
    } else {
        currentScanner = nullptr;
    }
}

} // namespace processor
} // namespace lbug
