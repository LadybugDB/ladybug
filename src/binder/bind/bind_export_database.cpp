#include "binder/bound_export_database.h"
#include "binder/query/bound_regular_query.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/file_system/virtual_file_system.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "parser/parser.h"
#include "parser/port_db.h"
#include "parser/query/regular_query.h"
#include "storage/table/ice_disk_constants.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::parser;
using namespace lbug::catalog;
using namespace lbug::transaction;
using namespace lbug::storage;

namespace lbug {
namespace binder {

FileTypeInfo getFileType(case_insensitive_map_t<Value>& options) {
    auto fileTypeInfo =
        FileTypeInfo{FileType::PARQUET, PortDBConstants::DEFAULT_EXPORT_FORMAT_OPTION};
    if (options.contains(PortDBConstants::EXPORT_FORMAT_OPTION)) {
        auto value = options.at(PortDBConstants::EXPORT_FORMAT_OPTION);
        if (value.getDataType().getLogicalTypeID() != LogicalTypeID::STRING) {
            throw BinderException("The type of format option must be a string.");
        }
        auto valueStr = value.getValue<std::string>();
        StringUtils::toUpper(valueStr);
        fileTypeInfo = FileTypeInfo{FileTypeUtils::fromString(valueStr), valueStr};
        if (fileTypeInfo.fileType != FileType::PARQUET) {
            throw BinderException(
                "Export database only supports the parquet (icebug-disk CSR) format.");
        }
        options.erase(PortDBConstants::EXPORT_FORMAT_OPTION);
    }
    return fileTypeInfo;
}

void bindExportTableData(ExportedTableData& tableData, const std::string& query,
    main::ClientContext* context, Binder* binder) {
    auto parsedStatement = Parser::parseQuery(query);
    DASSERT(parsedStatement.size() == 1);
    auto parsedQuery = parsedStatement[0]->constPtrCast<RegularQuery>();
    context->setUseInternalCatalogEntry(true /* useInternalCatalogEntry */);
    auto boundQuery = binder->bindQuery(*parsedQuery);
    context->setUseInternalCatalogEntry(false /* useInternalCatalogEntry */);
    auto columns = boundQuery->getStatementResult()->getColumns();
    for (auto& column : columns) {
        auto columnName = column->hasAlias() ? column->getAlias() : column->toString();
        tableData.columnNames.push_back(columnName);
        tableData.columnTypes.push_back(column->getDataType().copy());
    }
    tableData.regularQuery = std::move(boundQuery);
}

static std::string getPropertyReturnList(const TableCatalogEntry& entry, const std::string& var) {
    std::string props;
    for (auto& property : entry.getProperties()) {
        if (property.getType() == LogicalType::INTERNAL_ID()) {
            continue;
        }
        props += std::format(",{}.{} as {}", var, StringUtils::quoteIdentifier(property.getName()),
            StringUtils::quoteIdentifier(property.getName()));
    }
    return props;
}

static std::string getExportNodeTableDataQuery(const TableCatalogEntry& entry) {
    // Order by storage offset so that row i in nodes_<table>.parquet holds the node
    // with dense offset i, matching the CSR target/ptr values in the rel tables.
    // Properties are aliased explicitly so parquet columns carry clean names.
    auto props = getPropertyReturnList(entry, "a");
    auto returns = props.empty() ? "a.*" : props.substr(1);
    return std::format("match (a:{}) return {} order by offset(id(a))",
        StringUtils::quoteIdentifier(entry.getName()), returns);
}

static std::string getExportRelIndicesQuery(const TableCatalogEntry& relGroupEntry,
    const NodeTableCatalogEntry& srcEntry, const NodeTableCatalogEntry& dstEntry) {
    // CSR indices table: dense target offsets (+ edge properties), ordered by
    // (source offset, target offset). The source offset is encoded by indptr.
    return std::format("match (a:{})-[r:{}]->(b:{}) return offset(id(b)) as target{} "
                       "order by offset(id(a)), offset(id(b));",
        StringUtils::quoteIdentifier(srcEntry.getName()),
        StringUtils::quoteIdentifier(relGroupEntry.getName()),
        StringUtils::quoteIdentifier(dstEntry.getName()),
        getPropertyReturnList(relGroupEntry, "r"));
}

static std::string getExportRelIndptrQuery(const TableCatalogEntry& relGroupEntry,
    const NodeTableCatalogEntry& srcEntry, const NodeTableCatalogEntry& dstEntry) {
    // One row per source node (OPTIONAL MATCH keeps zero-degree nodes), consumed by
    // the indptr export function which sorts by src and prefix-sums the degrees.
    return std::format(
        "match (a:{}) optional match (a)-[r:{}]->(b:{}) return offset(id(a)) as src, "
        "count(r) as degree;",
        StringUtils::quoteIdentifier(srcEntry.getName()),
        StringUtils::quoteIdentifier(relGroupEntry.getName()),
        StringUtils::quoteIdentifier(dstEntry.getName()));
}

static std::string getExportRelFlatQuery(const TableCatalogEntry& relGroupEntry,
    const NodeTableCatalogEntry& srcEntry, const NodeTableCatalogEntry& dstEntry) {
    // Flat relationship file for native re-import: source/target primary-key values
    // (COPY FROM resolves them back to node offsets) followed by edge properties.
    return std::format("match (a:{})-[r:{}]->(b:{}) return {}.{} as {}, {}.{} as {}{};",
        StringUtils::quoteIdentifier(srcEntry.getName()),
        StringUtils::quoteIdentifier(relGroupEntry.getName()),
        StringUtils::quoteIdentifier(dstEntry.getName()), "a",
        StringUtils::quoteIdentifier(srcEntry.getPrimaryKeyDefinition().getName()),
        StringUtils::quoteIdentifier("from"), "b",
        StringUtils::quoteIdentifier(dstEntry.getPrimaryKeyDefinition().getName()),
        StringUtils::quoteIdentifier("to"), getPropertyReturnList(relGroupEntry, "r"));
}

static std::vector<ExportedTableData> getExportInfo(const Catalog& catalog,
    main::ClientContext* context, Binder* binder, [[maybe_unused]] FileTypeInfo& fileTypeInfo) {
    auto transaction = Transaction::Get(*context);
    std::vector<ExportedTableData> exportData;
    for (auto entry : catalog.getNodeTableEntries(transaction, false /*useInternal*/)) {
        ExportedTableData tableData;
        tableData.tableName = entry->getName();
        tableData.fileName = std::format("nodes_{}.parquet", entry->getName());
        auto query = getExportNodeTableDataQuery(*entry);
        bindExportTableData(tableData, query, context, binder);
        exportData.push_back(std::move(tableData));
    }
    for (auto entry : catalog.getRelGroupEntries(transaction, false /* useInternal */)) {
        auto& relGroupEntry = entry->constCast<RelGroupCatalogEntry>();
        if (relGroupEntry.getRelEntryInfos().size() != 1) {
            throw BinderException(std::format(
                "Export database: rel table {} connects multiple node pairs, which is not "
                "supported for icebug-disk CSR export yet.",
                relGroupEntry.getName()));
        }
        for (auto& info : relGroupEntry.getRelEntryInfos()) {
            auto srcTableID = info.nodePair.srcTableID;
            auto dstTableID = info.nodePair.dstTableID;
            auto& srcEntry = catalog.getTableCatalogEntry(transaction, srcTableID)
                                 ->constCast<NodeTableCatalogEntry>();
            auto& dstEntry = catalog.getTableCatalogEntry(transaction, dstTableID)
                                 ->constCast<NodeTableCatalogEntry>();
            ExportedTableData indicesData;
            indicesData.tableName = entry->getName();
            indicesData.fileName = std::format("indices_{}.parquet", relGroupEntry.getName());
            bindExportTableData(indicesData,
                getExportRelIndicesQuery(relGroupEntry, srcEntry, dstEntry), context, binder);
            exportData.push_back(std::move(indicesData));
            ExportedTableData indptrData;
            indptrData.tableName = entry->getName();
            indptrData.fileName = std::format("indptr_{}.parquet", relGroupEntry.getName());
            indptrData.isIndptr = true;
            bindExportTableData(indptrData,
                getExportRelIndptrQuery(relGroupEntry, srcEntry, dstEntry), context, binder);
            exportData.push_back(std::move(indptrData));
            // Flat relationship file (from/to primary-key values + properties), used by
            // IMPORT DATABASE to ingest the relationship table into native storage. It is
            // ignored when the export is mounted as icebug-disk (which uses the CSR files).
            ExportedTableData flatRelData;
            flatRelData.tableName = entry->getName();
            flatRelData.fileName = std::format("rels_{}.parquet", relGroupEntry.getName());
            bindExportTableData(flatRelData,
                getExportRelFlatQuery(relGroupEntry, srcEntry, dstEntry), context, binder);
            exportData.push_back(std::move(flatRelData));
        }
    }

    // Note: indexes are not exported. Icebug-disk tables are immutable, so indexes
    // cannot be rebuilt on import; only the schema, nodes CSR, and rel CSR are kept.
    return exportData;
}

static bool schemaOnly(case_insensitive_map_t<Value>& parsedOptions,
    const parser::ExportDB& exportDB) {
    auto isSchemaOnlyOption = [](const std::pair<std::string, Value>& option) -> bool {
        if (option.first != PortDBConstants::SCHEMA_ONLY_OPTION) {
            return false;
        }
        if (option.second.getDataType() != LogicalType::BOOL()) {
            throw common::BinderException{std::format("The '{}' option must have a BOOL value.",
                PortDBConstants::SCHEMA_ONLY_OPTION)};
        }
        return option.second.getValue<bool>();
    };
    auto exportSchemaOnly =
        std::count_if(parsedOptions.begin(), parsedOptions.end(), isSchemaOnlyOption) != 0;
    if (exportSchemaOnly && exportDB.getParsingOptionsRef().size() != 1) {
        throw common::BinderException{std::format("When '{}' option is set to true in export "
                                                  "database, no other options are allowed.",
            PortDBConstants::SCHEMA_ONLY_OPTION)};
    }
    parsedOptions.erase(PortDBConstants::SCHEMA_ONLY_OPTION);
    return exportSchemaOnly;
}

std::unique_ptr<BoundStatement> Binder::bindExportDatabaseClause(const Statement& statement) {
    auto& exportDB = statement.constCast<ExportDB>();
    auto parsedOptions = bindParsingOptions(exportDB.getParsingOptionsRef());
    auto fileTypeInfo = getFileType(parsedOptions);
    switch (fileTypeInfo.fileType) {
    case FileType::CSV:
    case FileType::PARQUET:
        break;
    default:
        throw BinderException("Export database currently only supports csv and parquet files.");
    }
    auto exportSchemaOnly = schemaOnly(parsedOptions, exportDB);
    if (!exportSchemaOnly && fileTypeInfo.fileType != FileType::CSV && parsedOptions.size() != 0) {
        throw BinderException{"Export database does not support copy options with the parquet "
                              "(icebug-disk CSR) format."};
    }
    auto exportData =
        getExportInfo(*Catalog::Get(*clientContext), clientContext, this, fileTypeInfo);
    // Stamp every exported parquet file with the icebug-disk format version so that
    // IceDiskUtils::checkVersionCompatibility can validate it when the export is mounted.
    if (fileTypeInfo.fileType == FileType::PARQUET && !exportSchemaOnly) {
        parsedOptions.insert_or_assign(PortDBConstants::ICEBUG_DISK_VERSION_OPTION,
            Value{std::string{storage::IceDiskConstants::CURRENT_VERSION}});
    }
    auto boundFilePath = VirtualFileSystem::GetUnsafe(*clientContext)
                             ->expandPath(clientContext, exportDB.getFilePath());
    return std::make_unique<BoundExportDatabase>(boundFilePath, fileTypeInfo, std::move(exportData),
        std::move(parsedOptions), exportSchemaOnly);
}

} // namespace binder
} // namespace lbug
