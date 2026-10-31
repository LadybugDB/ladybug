#include "processor/operator/scan/scan_multi_rel_tables.h"

#include "processor/execution_context.h"
#include "storage/local_storage/local_storage.h"
#include "storage/table/arrow_rel_table.h"
#include "storage/table/external_table_hooks.h"
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
    refreshNbrMasks(nbrNodeMaskMap.get());
}

common::sel_t ScanMultiRelTable::applyNbrNodeMask() {
    return applyNbrMaskFilter(scanState->outState->getSelVectorUnsafe(), outVectors[0]);
}

void ScanMultiRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    ScanTable::initLocalStateInternal(resultSet, context);
    refreshNbrMaskCache();
    auto clientContext = context->clientContext;
    boundNodeIDVector = resultSet->getValueVector(opInfo.nodeIDPos).get();
    auto nbrNodeIDVector = outVectors[0];

    // The scan state is shared across every table in every scanner of this multi-rel scan,
    // so it must serve every backend in the scan. Columnar backends (icebug-disk, Arrow, and
    // extension columnar tables whose state extends ColumnarRelTableScanState) all share that
    // state; native tables work with any state. An extension backend opts into mixed scans
    // with ColumnarTableHooks::supportsMixedColumnarScan (its createRelScanState must return
    // a ColumnarRelTableScanState subclass). A backend that cannot cover the in-core
    // columnar tables may only be scanned alongside its own tables and native tables;
    // mixing it with icebug-disk/Arrow tables is rejected with a clear error instead of
    // hitting an invalid downcast in the table's initScanState.
    bool hasArrowTable = false;
    bool hasIceDiskTable = false;
    storage::RelTable* hookStateOwner = nullptr;
    bool hookStateSupportsMixed = false;
    for (auto& [_, scanner] : scanners) {
        for (auto& relInfo : scanner.relInfos) {
            if (auto hooks = storage::ColumnarTableHookRegistry::getTableHooks(
                    relInfo.table->getStorageManager(), relInfo.table->getTableID())) {
                if (hooks->createRelScanState && hookStateOwner == nullptr) {
                    hookStateOwner = relInfo.table->ptrCast<storage::RelTable>();
                    hookStateSupportsMixed =
                        hooks->supportsMixedColumnarScan || hooks->scanStateRank >= 2;
                }
                continue;
            }
            if (dynamic_cast<storage::ArrowRelTable*>(relInfo.table) != nullptr) {
                hasArrowTable = true;
            } else if (dynamic_cast<storage::IceDiskRelTable*>(relInfo.table) != nullptr) {
                hasIceDiskTable = true;
            }
        }
    }

    // ColumnarRelTableScanState serves icebug-disk, Arrow, native, and mixed-capable
    // extension tables, so one shared state covers every combination below.
    const bool hookStateCoversAll =
        hookStateOwner != nullptr &&
        (hookStateSupportsMixed || (!hasIceDiskTable && !hasArrowTable));
    if (hookStateOwner != nullptr && !hookStateCoversAll) {
        throw common::RuntimeException(
            "Multi-rel scan mixes incompatible table backends: an extension-backed rel table "
            "cannot share a scan with icebug-disk or Arrow rel tables.");
    }
    if (hookStateCoversAll) {
        scanState = ColumnarTableHookRegistry::getTableHooks(hookStateOwner->getStorageManager(),
            hookStateOwner->getTableID())
                        ->createRelScanState(hookStateOwner, boundNodeIDVector, outVectors,
                            nbrNodeIDVector->state, MemoryManager::Get(*clientContext));
    } else if (hasIceDiskTable) {
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
            scanState->releaseScanPin();
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
