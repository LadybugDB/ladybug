#include "processor/operator/simple/export_db.h"

#include <sstream>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/sequence_catalog_entry.h"
#include "common/file_system/virtual_file_system.h"
#include "common/string_utils.h"
#include "extension/extension_manager.h"
#include "function/scalar_macro_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include <format>

using namespace lbug::common;
using namespace lbug::transaction;
using namespace lbug::catalog;
using namespace lbug::main;

namespace lbug {
namespace processor {

using std::stringstream;

std::string ExportDBPrintInfo::toString() const {
    std::string result = "Export To: ";
    result += filePath;
    if (!options.empty()) {
        result += ",Options: ";
        auto it = options.begin();
        for (auto i = 0u; it != options.end(); ++it, ++i) {
            result += it->first + "=" + it->second.toString();
            if (i < options.size() - 1) {
                result += ", ";
            }
        }
    }
    return result;
}

static void writeStringStreamToFile(ClientContext* context, const std::string& ssString,
    const std::string& path) {
    const auto fileInfo = VirtualFileSystem::GetUnsafe(*context)->openFile(path,
        FileOpenFlags(FileFlags::WRITE | FileFlags::CREATE_IF_NOT_EXISTS), context);
    fileInfo->writeFile(reinterpret_cast<const uint8_t*>(ssString.c_str()), ssString.size(),
        0 /* offset */);
}

static void exportLoadedExtensions(stringstream& ss, const ClientContext* clientContext) {
    auto extensionCypher = extension::ExtensionManager::Get(*clientContext)->toCypher();
    if (!extensionCypher.empty()) {
        ss << extensionCypher << std::endl;
    }
}

// Attach an icebug-disk storage clause to a CREATE TABLE statement so the exported
// schema mounts the parquet files in place instead of re-ingesting them.
// The data files (nodes_<table>.parquet, indices_<rel>.parquet, indptr_<rel>.parquet)
// are written by sibling COPY TO pipelines into the same directory.
static std::string withIcebugStorage(const std::string& createStatement,
    const std::string& exportDir) {
    auto escapedDir = exportDir;
    StringUtils::replaceAll(escapedDir, "'", "\\'");
    auto clause = std::format(" WITH (storage = '{}', format = 'icebug-disk')", escapedDir);
    // Drop any pre-existing storage clause (e.g. re-exporting an icebug-disk table)
    // and the trailing ';', then attach the new clause.
    auto base = createStatement;
    if (auto withPos = base.find(" WITH ("); withPos != std::string::npos) {
        base = base.substr(0, withPos);
    }
    if (auto semiPos = base.rfind(';'); semiPos != std::string::npos) {
        base = base.substr(0, semiPos);
    }
    return base + clause + ";";
}

std::string getSchemaCypher(ClientContext* clientContext, const std::string& exportDir) {
    stringstream ss;
    exportLoadedExtensions(ss, clientContext);
    const auto catalog = Catalog::Get(*clientContext);
    auto transaction = Transaction::Get(*clientContext);
    ToCypherInfo toCypherInfo;
    for (const auto& nodeTableEntry :
        catalog->getNodeTableEntries(transaction, false /* useInternal */)) {
        ss << withIcebugStorage(nodeTableEntry->toCypher(toCypherInfo), exportDir) << std::endl;
    }
    RelGroupToCypherInfo relTableToCypherInfo{clientContext};
    for (const auto& entry : catalog->getRelGroupEntries(transaction, false /* useInternal */)) {
        ss << withIcebugStorage(entry->toCypher(relTableToCypherInfo), exportDir) << std::endl;
    }
    RelGroupToCypherInfo relGroupToCypherInfo{clientContext};
    for (const auto sequenceEntry : catalog->getSequenceEntries(transaction)) {
        ss << sequenceEntry->toCypher(relGroupToCypherInfo) << std::endl;
    }
    for (auto macroName : catalog->getMacroNames(transaction)) {
        ss << catalog->getScalarMacroFunction(transaction, macroName)->toCypher(macroName)
           << std::endl;
    }
    return ss.str();
}

std::string getIndexCypher(ClientContext* clientContext, const FileScanInfo& exportFileInfo) {
    stringstream ss;
    IndexToCypherInfo info{clientContext, exportFileInfo};
    auto transaction = Transaction::Get(*clientContext);
    auto catalog = Catalog::Get(*clientContext);
    for (auto entry : catalog->getIndexEntries(transaction)) {
        auto indexCypher = entry->toCypher(info);
        if (!indexCypher.empty()) {
            ss << indexCypher << std::endl;
        }
    }
    return ss.str();
}

void ExportDB::executeInternal(ExecutionContext* context) {
    const auto clientContext = context->clientContext;
    // The export layout is icebug-disk: sibling COPY TO pipelines write
    // nodes_<table>.parquet / indices_<rel>.parquet / indptr_<rel>.parquet, and the
    // schema mounts them in place, so no copy.cypher (data movement) is needed.
    // Secondary structures (HNSW, FTS, ...) are rebuilt from index.cypher on import.
    writeStringStreamToFile(clientContext,
        getSchemaCypher(clientContext, boundFileInfo.filePaths[0]),
        boundFileInfo.filePaths[0] + "/" + PortDBConstants::SCHEMA_FILE_NAME);
    writeStringStreamToFile(clientContext, getIndexCypher(clientContext, boundFileInfo),
        boundFileInfo.filePaths[0] + "/" + PortDBConstants::INDEX_FILE_NAME);
    appendMessage("Exported database successfully.", storage::MemoryManager::Get(*clientContext));
}

} // namespace processor
} // namespace lbug
