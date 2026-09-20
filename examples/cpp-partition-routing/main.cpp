// Distributed partition-routing wrapper example.
//
// Implements the hooks from the distributed API section of lbug.hpp
// (main/lbug_distributed.h) against nothing but the amalgamated header: it claims every
// partition of a HASH-partitioned table as remote, serves point and bulk writes from an
// in-memory sink, answers parent scans through one shared table function, and logs
// lifecycle notifications. See common/partition_routing_hook.h for the contract.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "lbug.hpp"

using namespace lbug;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::main;
using namespace lbug::transaction;

namespace {

// Remote store: every accepted row, in insert order, as (id, v) pairs.
struct RemoteRow {
    int64_t id;
    int64_t v;
};
std::vector<RemoteRow> g_remoteRows;
std::vector<std::string> g_events;

std::string refString(PartitionRef ref) {
    return std::to_string(ref.parentTableID) + ":" + std::to_string(ref.partitionIndex);
}

// --- Scan serving: one shared function (partitions collapse to a single entry only
// when they expose the same scan), columns built directly without a Binder.
binder::expression_vector scanColumns(const std::string& node) {
    binder::expression_vector columns;
    columns.push_back(std::make_shared<binder::VariableExpression>(
        LogicalType(LogicalTypeID::INT64), node + "._ID", "rowid"));
    columns.push_back(std::make_shared<binder::VariableExpression>(
        LogicalType(LogicalTypeID::INT64), node + ".id", "id"));
    columns.push_back(std::make_shared<binder::VariableExpression>(
        LogicalType(LogicalTypeID::INT64), node + ".v", "v"));
    return columns;
}

offset_t scanKernel(const TableFuncMorsel& morsel, const TableFuncInput&,
    DataChunk& output) {
    if (!morsel.hasMoreToOutput()) {
        return 0;
    }
    for (auto i = 0u; i < morsel.getMorselSize(); ++i) {
        const auto& row = g_remoteRows[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, (int64_t)(morsel.startOffset + i));
        output.getValueVectorMutable(1).setValue(i, row.id);
        output.getValueVectorMutable(2).setValue(i, row.v);
    }
    return morsel.getMorselSize();
}

TableFunction g_scanFunc;

void ensureScanFunc() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    g_scanFunc = TableFunction("partition_routing_example_scan", std::vector<LogicalTypeID>{});
    g_scanFunc.tableFunc = SimpleTableFunc::getTableFunc(scanKernel);
    g_scanFunc.bindFunc = [](ClientContext*, const TableFuncBindInput*) {
        return std::make_unique<TableFuncBindData>(scanColumns("r"), g_remoteRows.size());
    };
    g_scanFunc.initSharedStateFunc = SimpleTableFunc::initSharedState;
    g_scanFunc.initLocalStateFunc = TableFunction::initEmptyLocalState;
}

// --- Hook implementations.
bool locateHook(void*, PartitionRef ref, PartitionHandle* handleOut) {
    *handleOut = reinterpret_cast<void*>(0xC0FFEE);
    (void)ref;
    return true; // claim everything: fully-claimed parents collapse to one scan entry.
}

void createHook(void*, PartitionRef ref, PartitionHandle) {
    g_events.push_back("create:" + refString(ref));
}

void dropHook(void*, PartitionRef ref, PartitionHandle) {
    g_events.push_back("drop:" + refString(ref));
}

nodeID_t insertRowHook(void*, PartitionRef ref, PartitionHandle, Transaction*,
    const ValueVector* keyVector, std::span<ValueVector* const> columnVectors) {
    const auto idPos = columnVectors[0]->state->getSelVector()[0];
    const auto keySel = keyVector->state->getSelVector()[0];
    g_remoteRows.push_back({columnVectors[0]->getValue<int64_t>(idPos),
        keyVector->getValue<int64_t>(keySel)});
    return {static_cast<offset_t>(g_remoteRows.size() - 1), ref.parentTableID};
}

void insertChunkHook(void*, PartitionRef, PartitionHandle, Transaction*,
    const ValueVector* keyVector, std::span<ValueVector* const> columnVectors, uint64_t startRow,
    uint64_t numRows) {
    const auto& keySel = keyVector->state->getSelVector();
    const auto& idSel = columnVectors[0]->state->getSelVector();
    for (uint64_t j = 0; j < numRows; ++j) {
        g_remoteRows.push_back({columnVectors[0]->getValue<int64_t>(idSel[startRow + j]),
            keyVector->getValue<int64_t>(keySel[startRow + j])});
    }
}

bool bindScanHook(void*, PartitionRef, PartitionHandle, PartitionScanSpec* specOut) {
    ensureScanFunc();
    specOut->scanFunction = &g_scanFunc;
    specOut->createBindData = [](const std::string& nodeUniqueName) {
        return std::make_unique<TableFuncBindData>(
            scanColumns(nodeUniqueName), g_remoteRows.size());
    };
    return true;
}

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        std::exit(1);
    }
    std::cout << "ok: " << what << "\n";
}

} // namespace

int main() {
    static PartitionRoutingHooks hooks{};
    hooks.locate = locateHook;
    hooks.onPartitionCreate = createHook;
    hooks.onPartitionDrop = dropHook;
    hooks.bindScan = bindScanHook;
    hooks.insertRow = insertRowHook;
    hooks.insertChunk = insertChunkHook;
    setPartitionRoutingHooks(&hooks);

    std::filesystem::remove_all("partition_routing_example.db");
    auto database = std::make_unique<Database>("partition_routing_example.db");
    auto connection = std::make_unique<Connection>(database.get());

    connection->query(
        "CREATE NODE TABLE Remote(id INT64, v INT64, PRIMARY KEY(id)) PARTITION BY HASH(v) PARTITIONS 3;");
    check(g_events.size() == 3, "lifecycle saw 3 partition creates");

    connection->query("CREATE (:Remote {id: 1, v: 10});");
    connection->query("CREATE (:Remote {id: 2, v: 20});");
    check(g_remoteRows.size() == 2, "point writes routed remotely");

    FILE* csv = std::fopen("partition_routing_bulk.csv", "w");
    std::fputs("3,30\n4,40\n", csv);
    std::fclose(csv);
    connection->query("COPY Remote FROM 'partition_routing_bulk.csv';");
    check(g_remoteRows.size() == 4, "bulk writes routed remotely");

    auto result = connection->query("MATCH (r:Remote) RETURN r.id, r.v ORDER BY r.id;");
    check(result->toString() == "r.id|r.v\n1|10\n2|20\n3|30\n4|40\n",
        "parent scan reads remote rows in order");

    connection->query("DROP TABLE Remote;");
    check(g_events.size() == 6, "lifecycle saw 3 partition drops");

    setPartitionRoutingHooks(nullptr);
    connection.reset();
    database.reset();
    std::filesystem::remove_all("partition_routing_example.db");
    std::filesystem::remove("partition_routing_bulk.csv");
    std::cout << "ALL CHECKS PASSED\n";
    return 0;
}
