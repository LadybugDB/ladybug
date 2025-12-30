#include "storage/table/arrow_node_table.h"

#include "common/arrow/arrow_converter.h"
#include "common/system_config.h"
#include "common/types/types.h"
#include "storage/storage_manager.h"
#include "storage/table/arrow_table_support.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

ArrowNodeTable::ArrowNodeTable(const StorageManager* storageManager,
    const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
    ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays, std::string arrowId)
    : ColumnarNodeTableBase{storageManager, nodeTableEntry, memoryManager},
      schema{std::move(schema)}, arrays{std::move(arrays)}, totalRows{0},
      arrowId{std::move(arrowId)} {
    // Note: release may be nullptr if schema is managed by registry
    if (!this->schema.format) {
        throw common::RuntimeException("Arrow schema format cannot be null");
    }
    // Calculate total rows from arrays
    if (!this->arrays.empty()) {
        totalRows = this->arrays[0].length;
    }
}

ArrowNodeTable::~ArrowNodeTable() {
    // Unregister Arrow data from the global registry when table is destroyed
    // This handles the case where DROP TABLE is called instead of explicit unregister
    if (!arrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(arrowId);
    }
}

void ArrowNodeTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();

    // Note: We don't copy the schema/arrays as they are wrappers with release callbacks
    arrowScanState.initialized = false;
    arrowScanState.scanCompleted = false;
    arrowScanState.dataRead = false;
    arrowScanState.nextRowToDistribute = 0;
    arrowScanState.totalRows = totalRows;
    arrowScanState.allData.clear();

    // Each scan state needs to be able to read data independently for parallel scanning
    arrowScanState.initialized = true;
}

bool ArrowNodeTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();

    // Read all data from Arrow table if not already done
    if (!arrowScanState.dataRead) {
        readArrowTableData(arrowScanState);
        arrowScanState.dataRead = true;
    }

    // Check if we've distributed all rows
    if (arrowScanState.nextRowToDistribute >= arrowScanState.totalRows) {
        arrowScanState.scanCompleted = true;
        return false;
    }

    // Distribute one row at a time (like ParquetNodeTable does)
    size_t rowIndex = arrowScanState.nextRowToDistribute++;

    // Check if the row index is valid
    if (rowIndex >= arrowScanState.allData.size()) {
        arrowScanState.scanCompleted = true;
        return false;
    }

    // Copy one row to output vectors
    auto numColumns =
        std::min(scanState.outputVectors.size(), arrowScanState.allData[rowIndex].size());
    for (size_t col = 0; col < numColumns; ++col) {
        // Ensure output vector exists
        if (col >= scanState.outputVectors.size() || !scanState.outputVectors[col]) {
            continue;
        }

        auto& dstVector = *scanState.outputVectors[col];

        // Ensure value exists for this column
        if (col >= arrowScanState.allData[rowIndex].size() ||
            !arrowScanState.allData[rowIndex][col]) {
            dstVector.setNull(0, true);
            continue;
        }

        auto& value = *arrowScanState.allData[rowIndex][col];

        if (value.isNull()) {
            dstVector.setNull(0, true);
        } else {
            dstVector.copyFromValue(0, value);
        }
    }

    // Set node ID for this row
    auto tableID = this->getTableID();
    auto& nodeID = scanState.nodeIDVector->getValue<common::nodeID_t>(0);
    nodeID.tableID = tableID;
    nodeID.offset = rowIndex;

    scanState.outState->getSelVectorUnsafe().setSelSize(1);

    return true;
}

common::node_group_idx_t ArrowNodeTable::getNumBatches(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    // For simplicity, we treat the entire Arrow table as a single batch
    return 1;
}

common::row_idx_t ArrowNodeTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalRows;
}

void ArrowNodeTable::readArrowTableData(ArrowNodeTableScanState& scanState) const {
    if (arrays.empty() || !arrays[0].release) {
        scanState.totalRows = 0;
        return;
    }

    const ArrowArray& structArray = arrays[0];
    size_t numColumns = structArray.n_children;
    size_t numRows = totalRows;

    scanState.allData.resize(numRows);
    for (size_t row = 0; row < numRows; ++row) {
        scanState.allData[row].resize(numColumns);
    }

    for (size_t col = 0; col < numColumns; ++col) {
        if (!structArray.children || !structArray.children[col] ||
            !structArray.children[col]->release || !schema.children[col]) {
            for (size_t row = 0; row < numRows; ++row) {
                scanState.allData[row][col] =
                    std::make_unique<common::Value>(common::Value::createNullValue());
            }
            continue;
        }

        const ArrowArray& columnArray = *structArray.children[col];
        const ArrowSchema* columnSchema = schema.children[col];

        common::LogicalType logicalType = common::ArrowConverter::fromArrowSchema(columnSchema);
        common::ValueVector valueVector(std::move(logicalType));
        valueVector.state = std::make_shared<common::DataChunkState>();
        valueVector.state->getSelVectorUnsafe().setSelSize(numRows);

        try {
            common::ArrowConverter::fromArrowArray(columnSchema, &columnArray, valueVector);
        } catch (...) {
            for (size_t row = 0; row < numRows; ++row) {
                scanState.allData[row][col] =
                    std::make_unique<common::Value>(common::Value::createNullValue());
            }
            continue;
        }

        for (size_t row = 0; row < numRows; ++row) {
            if (valueVector.isNull(row)) {
                scanState.allData[row][col] =
                    std::make_unique<common::Value>(common::Value::createNullValue());
            } else {
                scanState.allData[row][col] = valueVector.getAsValue(row);
            }
        }
    }

    scanState.totalRows = numRows;
}

} // namespace storage
} // namespace lbug