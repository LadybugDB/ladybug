#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/binder.h"
#include "function/table/bind_data.h"
#include "function/table/simple_table_function.h"
#include "main/client_context.h"
#include "storage/database_header.h"
#include "storage/index/hash_index.h"
#include "storage/page_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/list_chunk_data.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "storage/table/string_chunk_data.h"
#include "storage/table/struct_chunk_data.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::catalog;
using namespace lbug::storage;
using namespace lbug::main;

namespace lbug {
namespace function {

struct DiskSizeInfoBindData final : TableFuncBindData {
    const ClientContext* ctx;
    DiskSizeInfoBindData(binder::expression_vector columns, row_idx_t numRows,
        const ClientContext* ctx)
        : TableFuncBindData{std::move(columns), numRows}, ctx{ctx} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DiskSizeInfoBindData>(columns, numRows, ctx);
    }
};

static uint64_t countChunkDataPages(const ColumnChunkData& chunkData) {
    uint64_t pages = 0;
    auto metadata = chunkData.getResidencyState() == ResidencyState::ON_DISK ?
                        chunkData.getMetadata() :
                        chunkData.getMetadataToFlush();
    pages += metadata.getNumPages();

    if (chunkData.hasNullData()) {
        pages += countChunkDataPages(*chunkData.getNullData());
    }

    auto physicalType = chunkData.getDataType().getPhysicalType();
    switch (physicalType) {
    case PhysicalTypeID::STRUCT: {
        auto& structChunk = chunkData.cast<StructChunkData>();
        for (auto i = 0u; i < structChunk.getNumChildren(); i++) {
            pages += countChunkDataPages(structChunk.getChild(i));
        }
    } break;
    case PhysicalTypeID::STRING:
    case PhysicalTypeID::JSON: {
        auto& stringChunk = chunkData.cast<StringChunkData>();
        pages += countChunkDataPages(*stringChunk.getIndexColumnChunk());
        auto& dictionaryChunk = stringChunk.getDictionaryChunk();
        pages += countChunkDataPages(*dictionaryChunk.getStringDataChunk());
        pages += countChunkDataPages(*dictionaryChunk.getOffsetChunk());
    } break;
    case PhysicalTypeID::ARRAY:
    case PhysicalTypeID::LIST: {
        auto& listChunk = chunkData.cast<ListChunkData>();
        pages += countChunkDataPages(*listChunk.getOffsetColumnChunk());
        pages += countChunkDataPages(*listChunk.getSizeColumnChunk());
        pages += countChunkDataPages(*listChunk.getDataColumnChunk());
    } break;
    default:
        break;
    }
    return pages;
}

static uint64_t countChunkedGroupPages(ChunkedNodeGroup* chunkedGroup) {
    uint64_t pages = 0;
    auto numColumns = chunkedGroup->getNumColumns();
    for (auto i = 0u; i < numColumns; i++) {
        for (auto* segment : chunkedGroup->getColumnChunk(i).getSegments()) {
            pages += countChunkDataPages(*segment);
        }
    }
    if (chunkedGroup->getFormat() == NodeGroupDataFormat::CSR) {
        auto& chunkedCSRGroup = chunkedGroup->cast<ChunkedCSRNodeGroup>();
        for (auto* segment : chunkedCSRGroup.getCSRHeader().offset->getSegments()) {
            pages += countChunkDataPages(*segment);
        }
        for (auto* segment : chunkedCSRGroup.getCSRHeader().length->getSegments()) {
            pages += countChunkDataPages(*segment);
        }
    }
    return pages;
}

static uint64_t countNodeGroupPages(NodeGroup* nodeGroup) {
    uint64_t pages = 0;
    auto numChunks = nodeGroup->getNumChunkedGroups();
    for (auto chunkIdx = 0ul; chunkIdx < numChunks; chunkIdx++) {
        pages += countChunkedGroupPages(nodeGroup->getChunkedNodeGroup(chunkIdx));
    }
    if (nodeGroup->getFormat() == NodeGroupDataFormat::CSR) {
        auto& csrNodeGroup = nodeGroup->cast<CSRNodeGroup>();
        auto persistentChunk = csrNodeGroup.getPersistentChunkedGroup();
        if (persistentChunk) {
            pages += countChunkedGroupPages(persistentChunk);
        }
    }
    return pages;
}

struct DiskSizeEntry {
    std::string category;
    std::string name;
    uint64_t numPages;
    uint64_t sizeBytes;
};

static std::vector<DiskSizeEntry> collectDiskSizeInfo(const ClientContext* context) {
    std::vector<DiskSizeEntry> entries;
    auto storageManager = StorageManager::Get(*context);
    auto catalog = Catalog::Get(*context);
    auto dataFH = storageManager->getDataFH();

    // Handle in-memory databases
    if (storageManager->isInMemory()) {
        entries.push_back({"info", "in_memory_database", 0, 0});
        return entries;
    }

    auto pageManager = dataFH->getPageManager();

    // 1. Database header (always 1 page at index 0)
    entries.push_back({"header", "database_header", 1, LBUG_PAGE_SIZE});

    // 2. Get catalog and metadata page ranges from database header
    auto databaseHeader = DatabaseHeader::readDatabaseHeader(*dataFH->getFileInfo());
    if (databaseHeader.has_value()) {
        entries.push_back({"catalog", "catalog", databaseHeader->catalogPageRange.numPages,
            databaseHeader->catalogPageRange.numPages * LBUG_PAGE_SIZE});

        entries.push_back({"metadata", "metadata", databaseHeader->metadataPageRange.numPages,
            databaseHeader->metadataPageRange.numPages * LBUG_PAGE_SIZE});
    }

    // 3. Count table data pages
    auto nodeTableEntries =
        catalog->getNodeTableEntries(&transaction::DUMMY_CHECKPOINT_TRANSACTION);
    auto relGroupEntries = catalog->getRelGroupEntries(&transaction::DUMMY_CHECKPOINT_TRANSACTION);

    for (const auto tableEntry : nodeTableEntries) {
        auto& nodeTable = storageManager->getTable(tableEntry->getTableID())->cast<NodeTable>();
        uint64_t tablePages = 0;
        auto numNodeGroups = nodeTable.getNumNodeGroups();
        for (auto i = 0ul; i < numNodeGroups; i++) {
            tablePages += countNodeGroupPages(nodeTable.getNodeGroup(i));
        }
        entries.push_back(
            {"node_table", tableEntry->getName(), tablePages, tablePages * LBUG_PAGE_SIZE});

        for (const auto& indexHolder : nodeTable.getIndexes()) {
            if (!indexHolder.isLoaded()) {
                continue;
            }
            const auto* index = indexHolder.getIndex();
            const auto indexInfo = index->getIndexInfo();
            for (const auto& storageEntry : index->getStorageEntries()) {
                if (storageEntry.pageRange.startPageIdx == INVALID_PAGE_IDX ||
                    storageEntry.pageRange.numPages == 0) {
                    continue;
                }
                entries.push_back({"index",
                    tableEntry->getName() + "." + indexInfo.name + ":" + storageEntry.component,
                    storageEntry.pageRange.numPages,
                    storageEntry.pageRange.numPages * LBUG_PAGE_SIZE});
            }
        }
    }

    for (const auto entry : relGroupEntries) {
        auto& relGroupEntry = entry->cast<RelGroupCatalogEntry>();
        for (auto& info : relGroupEntry.getRelEntryInfos()) {
            auto& relTable = storageManager->getTable(info.oid)->cast<RelTable>();
            uint64_t tablePages = 0;

            for (auto direction : relTable.getStorageDirections()) {
                auto* directedRelTableData = relTable.getDirectedTableData(direction);
                auto numNodeGroups = directedRelTableData->getNumNodeGroups();
                for (auto i = 0ul; i < numNodeGroups; i++) {
                    tablePages += countNodeGroupPages(directedRelTableData->getNodeGroup(i));
                }
            }
            auto tableName = relGroupEntry.getName() + ":" +
                             catalog
                                 ->getTableCatalogEntry(&transaction::DUMMY_CHECKPOINT_TRANSACTION,
                                     info.nodePair.srcTableID)
                                 ->getName() +
                             "->" +
                             catalog
                                 ->getTableCatalogEntry(&transaction::DUMMY_CHECKPOINT_TRANSACTION,
                                     info.nodePair.dstTableID)
                                 ->getName();
            entries.push_back({"rel_table", tableName, tablePages, tablePages * LBUG_PAGE_SIZE});
        }
    }

    // 4. Free space (from FSM)
    auto freeEntries = pageManager->getFreeEntries(0, pageManager->getNumFreeEntries());
    uint64_t freePages = 0;
    for (const auto& freeEntry : freeEntries) {
        freePages += freeEntry.numPages;
    }
    entries.push_back({"free_space", "free_pages", freePages, freePages * LBUG_PAGE_SIZE});

    // 5. Calculate unaccounted pages (index slot data)
    auto totalFilePages = dataFH->getNumPages();
    uint64_t accountedPages = 1; // header
    if (databaseHeader.has_value()) {
        accountedPages +=
            databaseHeader->catalogPageRange.numPages + databaseHeader->metadataPageRange.numPages;
    }
    for (const auto& entry : entries) {
        if (entry.category == "node_table" || entry.category == "rel_table" ||
            entry.category == "index") {
            accountedPages += entry.numPages;
        }
    }
    accountedPages += freePages;

    if (totalFilePages > accountedPages) {
        uint64_t unaccountedPages = totalFilePages - accountedPages;
        entries.push_back({"unaccounted", "unaccounted_pages", unaccountedPages,
            unaccountedPages * LBUG_PAGE_SIZE});
    }

    // 6. Total file size (last row)
    entries.push_back({"total", "file_total", totalFilePages, totalFilePages * LBUG_PAGE_SIZE});

    return entries;
}

static offset_t internalTableFunc(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    const auto bindData = input.bindData->constPtrCast<DiskSizeInfoBindData>();
    auto entries = collectDiskSizeInfo(bindData->ctx);

    auto numEntriesToOutput = std::min(static_cast<uint64_t>(entries.size()) - morsel.startOffset,
        morsel.getMorselSize());

    for (row_idx_t i = 0; i < numEntriesToOutput; ++i) {
        const auto& entry = entries[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, entry.category);
        output.getValueVectorMutable(1).setValue(i, entry.name);
        output.getValueVectorMutable(2).setValue<uint64_t>(i, entry.numPages);
        output.getValueVectorMutable(3).setValue<uint64_t>(i, entry.sizeBytes);
    }
    return numEntriesToOutput;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const ClientContext* context,
    const TableFuncBindInput* input) {
    std::vector<std::string> columnNames = {"category", "name", "num_pages", "size_bytes"};
    std::vector<LogicalType> columnTypes;
    columnTypes.push_back(LogicalType::STRING());
    columnTypes.push_back(LogicalType::STRING());
    columnTypes.push_back(LogicalType::UINT64());
    columnTypes.push_back(LogicalType::UINT64());

    // Get number of entries to report
    auto entries = collectDiskSizeInfo(context);

    auto columns = input->binder->createVariables(columnNames, columnTypes);
    return std::make_unique<DiskSizeInfoBindData>(columns, entries.size(), context);
}

static function_set getDiskSizeInfoFunctionSet(const char* name) {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector<LogicalTypeID>{});
    function->tableFunc = SimpleTableFunc::getTableFunc(internalTableFunc);
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = SimpleTableFunc::initSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    functionSet.push_back(std::move(function));
    return functionSet;
}

function_set DiskSizeInfoFunction::getFunctionSet() {
    return getDiskSizeInfoFunctionSet(name);
}

function_set DiskInfoFunction::getFunctionSet() {
    return getDiskSizeInfoFunctionSet(name);
}

} // namespace function
} // namespace lbug
