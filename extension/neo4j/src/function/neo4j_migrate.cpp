#include "function/neo4j_migrate.h"

#include "binder/ddl/property_definition.h"
#include "binder/expression/literal_expression.h"
#include "common/enums/table_type.h"
#include "common/exception/runtime.h"
#include "common/types/value/nested.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/table/table_function.h"
#include "httplib.h"
#include "yyjson.h"
#include <format>

namespace lbug {
namespace neo4j_extension {

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::function;

struct Neo4jMigrateBindData final : TableFuncBindData {
    std::shared_ptr<httplib::Client> client;
    std::vector<std::string> nodesToImport;
    std::vector<std::string> relsToImport;

    Neo4jMigrateBindData(std::shared_ptr<httplib::Client> client,
        std::vector<std::string> nodesToImport, std::vector<std::string> relsToImport)
        : TableFuncBindData{binder::expression_vector{}, 0 /* maxOffset */},
          client{std::move(client)}, nodesToImport{std::move(nodesToImport)},
          relsToImport{std::move(relsToImport)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<Neo4jMigrateBindData>(*this);
    }
};

static std::string executeNeo4jQuery(httplib::Client& cli, std::string neo4jQuery) {
    std::string requestBody =
        std::format("{{\"statements\":[{{\"statement\":\"{}\"}}]}}", neo4jQuery);
    httplib::Request req;
    req.method = "POST";
    req.path = "/db/neo4j/tx/commit";
    req.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}};
    req.body.assign(reinterpret_cast<const char*>(requestBody.c_str()), requestBody.length());
    auto res = cli.send(req);
    if (!res) {
        throw common::RuntimeException{"Failed to connect to Neo4j. Please check whether it "
                                       "is a valid connection url."};
    }
    if (res->status != 200) {
        throw common::RuntimeException{
            std::format("Failed to connect to neo4j. Server returned: {}, Response: {}.",
                res->status, res->body)};
    }
    auto doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
    auto errorsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "errors");
    if (yyjson_arr_size(errorsArr) > 0) {
        auto errorObj = yyjson_arr_get(errorsArr, 0);
        auto message = yyjson_obj_get(errorObj, "message");
        throw common::RuntimeException{
            std::format("Failed to execute query '{}' in Neo4j. Error: {}.", neo4jQuery,
                yyjson_get_str(message))};
    }
    auto resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
    if (yyjson_arr_size(resultsArr) != 1) {
        throw common::RuntimeException{"Neo4j returned multiple results."};
    }
    auto firstResult = yyjson_arr_get(resultsArr, 0);
    yyjson_obj_get(firstResult, "data");
    char* jsonStr = yyjson_write(doc, 0, nullptr);
    std::string result(jsonStr);
    free(jsonStr);
    yyjson_doc_free(doc);
    return result;
}

static void validateConnectionString(httplib::Client& cli) {
    executeNeo4jQuery(cli, "RETURN 1");
}

static std::unordered_set<std::string> getLabelsInNeo4j(httplib::Client& cli,
    common::TableType tableType) {
    std::unordered_set<std::string> labels;
    std::string query;
    switch (tableType) {
    case TableType::NODE: {
        query = "CALL db.labels();";
    } break;
    case TableType::REL: {
        query = "CALL db.relationshipTypes();";
    } break;
    default:
        KU_UNREACHABLE;
    }
    auto jsonStr = executeNeo4jQuery(cli, query);
    auto doc = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    auto resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
    auto firstResult = yyjson_arr_get(resultsArr, 0);
    auto dataArr = yyjson_obj_get(firstResult, "data");
    size_t dataIdx, dataMax;
    yyjson_val* dataItem;
    yyjson_arr_foreach(dataArr, dataIdx, dataMax, dataItem) {
        auto rowArr = yyjson_obj_get(dataItem, "row");
        size_t rowIdx, rowMax;
        yyjson_val* rowItem;
        yyjson_arr_foreach(rowArr, rowIdx, rowMax, rowItem) {
            labels.emplace(yyjson_get_str(rowItem));
        }
    }
    yyjson_doc_free(doc);
    return labels;
}

static void addLabel(httplib::Client& cli, common::TableType tableType, std::string& label,
    std::vector<std::string>& labels) {

    if (tableType == common::TableType::NODE) {
        auto resStr = executeNeo4jQuery(cli,
            std::format("match (n:{}) where size(labels(n)) > 1 return labels(n) limit 1", label));
        auto doc = yyjson_read(resStr.c_str(), resStr.size(), 0);
        auto resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
        auto firstResult = yyjson_arr_get(resultsArr, 0);
        auto dataArr = yyjson_obj_get(firstResult, "data");
        if (yyjson_arr_size(dataArr) > 0) {
            auto rowArr = yyjson_arr_get(dataArr, 0);
            throw common::RuntimeException{std::format(
                "Importing nodes with multi-labels is not supported right now. Found: {}",
                yyjson_get_str(yyjson_arr_get(rowArr, 0)))};
        }
        yyjson_doc_free(doc);
    }
    labels.push_back(std::move(label));
}

static std::vector<std::string> getNodeOrRels(httplib::Client& cli, common::TableType tableType,
    std::shared_ptr<binder::Expression> expression) {
    auto labelsInNeo4j = getLabelsInNeo4j(cli, tableType);
    std::vector<std::string> labels;
    auto labelVals = expression->constPtrCast<binder::LiteralExpression>()->getValue();

    if (labelVals.getChildrenSize() == 1 &&
        NestedVal::getChildVal(&labelVals, 0)->toString() == "*") {
        for (auto label : labelsInNeo4j) {
            addLabel(cli, tableType, label, labels);
        }
        return labels;
    }

    for (auto i = 0u; i < labelVals.getChildrenSize(); i++) {
        auto label = NestedVal::getChildVal(&labelVals, i)->toString();
        if (label == "*") {
            throw common::RuntimeException{"* cannot be specified with other labels"};
        } else if (!labelsInNeo4j.contains(label)) {
            throw common::RuntimeException{std::format("{} '{}' does not exist in neo4j.",
                TableTypeUtils::toString(tableType), label)};
        }
        addLabel(cli, tableType, label, labels);
    }
    return labels;
}

static std::unique_ptr<TableFuncBindData> bindFunc(ClientContext* /*context*/,
    const TableFuncBindInput* input) {
    auto url = input->getLiteralVal<std::string>(0);
    auto userName = input->getLiteralVal<std::string>(1);
    auto password = input->getLiteralVal<std::string>(2);
    auto cli = std::make_shared<httplib::Client>(url);
    cli->set_basic_auth(userName, password);
    cli->set_connection_timeout(std::chrono::seconds(1000));
    cli->set_read_timeout(std::chrono::seconds(1000));
    validateConnectionString(*cli);
    auto nodes = getNodeOrRels(*cli, TableType::NODE, input->getParam(3));
    auto rels = getNodeOrRels(*cli, TableType::REL, input->getParam(4));
    return std::make_unique<Neo4jMigrateBindData>(std::move(cli), std::move(nodes),
        std::move(rels));
}

void exportNeo4jNodeToCSV(std::string nodeName, httplib::Client& cli) {
    auto query = std::format("MATCH (p:{}) "
                             "with collect(p) as n "
                             "CALL apoc.export.csv.data(n, [], \\\"/tmp/{}.csv\\\", null) "
                             "YIELD file, source, format, nodes, relationships, "
                             "properties, time, rows, batchSize, batches, done, data "
                             "RETURN file, source, format, nodes, relationships, "
                             "properties, time, rows, batchSize, batches, done, data",
        nodeName, nodeName);
    executeNeo4jQuery(cli, query);
}

void exportNeo4jRelToCSV(std::string relName, std::pair<std::string, std::string> nodePairs,
    httplib::Client& cli) {
    auto query = std::format("MATCH (:{})-[e:{}]->(:{}) "
                             "with collect(e) as rels "
                             "CALL apoc.export.csv.data([], rels, \\\"/tmp/{}_{}_{}.csv\\\", null) "
                             "YIELD file, source, format, nodes, relationships, "
                             "properties, time, rows, batchSize, batches, done, data "
                             "RETURN file, source, format, nodes, relationships, "
                             "properties, time, rows, batchSize, batches, done, data",
        nodePairs.first, relName, nodePairs.second, nodePairs.first, relName, nodePairs.second);
    executeNeo4jQuery(cli, query);
}

LogicalType convertFromNeo4jTypeStr(const std::string& neo4jTypeStr) {
    if (neo4jTypeStr == "Long") {
        return LogicalType{LogicalTypeID::INT64};
    } else if (neo4jTypeStr == "Integer") {
        return LogicalType{LogicalTypeID::INT32};
    } else if (neo4jTypeStr == "Date") {
        return LogicalType{LogicalTypeID::DATE};
    } else if (neo4jTypeStr == "DateTime") {
        return LogicalType{LogicalTypeID::TIMESTAMP};
    } else if (neo4jTypeStr == "Boolean") {
        return LogicalType{LogicalTypeID::BOOL};
    } else if (neo4jTypeStr == "Double") {
        return LogicalType{LogicalTypeID::DOUBLE};
    } else if (neo4jTypeStr == "Float") {
        return LogicalType{LogicalTypeID::FLOAT};
    } else if (neo4jTypeStr == "LongArray") {
        return LogicalType::LIST(LogicalType::INT64());
    } else if (neo4jTypeStr == "DoubleArray") {
        return LogicalType::LIST(LogicalType::DOUBLE());
    } else if (neo4jTypeStr == "StringArray") {
        return LogicalType::LIST(LogicalType::STRING());
    } else {
        return LogicalType{LogicalTypeID::STRING};
    }
}

static LogicalType inferLbugType(yyjson_val* typesArr) {
    auto firstTypeStr = yyjson_get_str(yyjson_arr_get(typesArr, 0));
    auto kuType = convertFromNeo4jTypeStr(firstTypeStr);
    auto size = yyjson_arr_size(typesArr);
    for (auto i = 1u; i < size; i++) {
        auto typeStr = yyjson_get_str(yyjson_arr_get(typesArr, i));
        kuType = LogicalTypeUtils::combineTypes(convertFromNeo4jTypeStr(typeStr), kuType);
    }
    return kuType;
}

std::pair<std::string, std::string> getCreateNodeTableQuery(httplib::Client& cli,
    const std::string& nodeName, std::vector<std::string>& outputTables) {
    auto neo4jQuery = std::format(
        "call db.schema.nodeTypeProperties() yield nodeType, propertyName,propertyTypes where "
        "nodeType = ':`{}`' return propertyName,propertyTypes",
        nodeName);
    auto jsonStr = executeNeo4jQuery(cli, neo4jQuery);
    auto doc = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    auto resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
    auto firstResult = yyjson_arr_get(resultsArr, 0);
    auto dataArr = yyjson_obj_get(firstResult, "data");
    std::vector<binder::ColumnDefinition> propertyDefinitions;
    size_t dataIdx, dataMax;
    yyjson_val* dataItem;
    yyjson_arr_foreach(dataArr, dataIdx, dataMax, dataItem) {
        auto rowArr = yyjson_obj_get(dataItem, "row");
        auto propertyVal = yyjson_arr_get(rowArr, 0);
        if (yyjson_get_type(propertyVal) == YYJSON_TYPE_NULL) {
            continue;
        }
        auto property = yyjson_get_str(propertyVal);
        auto typesArr = yyjson_arr_get(rowArr, 1);
        auto kuType = inferLbugType(typesArr);

        char* typesJsonStr = yyjson_val_write(typesArr, 0, nullptr);
        auto newNode = std::format("['{}','{}','{}','{}']", nodeName, property, kuType.toString(),
            typesJsonStr);
        free(typesJsonStr);
        outputTables.emplace_back(std::move(newNode));

        propertyDefinitions.emplace_back(property, kuType.copy());
    }
    yyjson_doc_free(doc);
    std::sort(propertyDefinitions.begin(), propertyDefinitions.end(),
        [](const binder::ColumnDefinition& left, const binder::ColumnDefinition& right) {
            return left.name < right.name;
        });
    std::string ddlProperties = "";
    std::string propertiesToCopy = "";
    for (auto& propertyDefinition : propertyDefinitions) {
        ddlProperties += propertyDefinition.name;
        ddlProperties += (" " + propertyDefinition.type.toString() + ",");
        propertiesToCopy += ", " + propertyDefinition.name;
    }
    return {std::format("CREATE NODE TABLE `{}` (`_id_` int64, {} PRIMARY KEY(_id_));", nodeName,
                ddlProperties),
        std::format(
            "COPY `{}` FROM (LOAD WITH HEADERS(_id STRING, _labels STRING, {} _start STRING, "
            "_end STRING, _type STRING) FROM '/tmp/{}.csv'(sample_size "
            "= 0, header=true) RETURN _id{});",
            nodeName, ddlProperties, nodeName, propertiesToCopy)};
}

std::vector<std::string> getRelProperties(httplib::Client& cli, std::string srcLabel,
    std::string dstLabel, std::string relLabel) {
    auto neo4jQuery =
        std::format("MATCH (:{})-[e:{}]->(:{}) UNWIND keys(e) AS key return distinct key", srcLabel,
            relLabel, dstLabel);
    auto jsonStr = executeNeo4jQuery(cli, neo4jQuery);
    auto doc = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    auto resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
    auto firstResult = yyjson_arr_get(resultsArr, 0);
    auto dataArr = yyjson_obj_get(firstResult, "data");
    std::vector<std::string> relProperties;
    size_t dataIdx, dataMax;
    yyjson_val* dataItem;
    yyjson_arr_foreach(dataArr, dataIdx, dataMax, dataItem) {
        auto rowArr = yyjson_obj_get(dataItem, "row");
        auto firstVal = yyjson_arr_get(rowArr, 0);
        if (yyjson_get_type(firstVal) == YYJSON_TYPE_NULL) {
            continue;
        }
        relProperties.push_back(yyjson_get_str(firstVal));
    }
    yyjson_doc_free(doc);
    std::sort(relProperties.begin(), relProperties.end(),
        [](std::string& left, std::string& right) { return left < right; });
    return relProperties;
}

std::string getCreateRelTableQuery(httplib::Client& cli, const std::string& relName,
    const std::vector<std::string>& nodeLabelsToImport, std::vector<std::string>& outputTables) {
    auto neo4jQuery = std::format(
        "call db.schema.relTypeProperties() yield relType, propertyName,propertyTypes where "
        "relType = ':`{}`' and propertyName is not null and  propertyTypes is not null return "
        "propertyName,propertyTypes",
        relName);
    auto jsonStr = executeNeo4jQuery(cli, neo4jQuery);
    auto doc = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);

    std::unordered_map<std::string, std::string> propertyTypes;
    std::unordered_map<std::string, std::string> originalTypes;
    std::vector<binder::ColumnDefinition> propertyDefinitions;
    auto resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
    auto firstResult = yyjson_arr_get(resultsArr, 0);
    auto dataArr = yyjson_obj_get(firstResult, "data");
    size_t dataIdx, dataMax;
    yyjson_val* dataItem;
    yyjson_arr_foreach(dataArr, dataIdx, dataMax, dataItem) {
        auto rowArr = yyjson_obj_get(dataItem, "row");
        auto property = yyjson_get_str(yyjson_arr_get(rowArr, 0));
        auto typesArr = yyjson_arr_get(rowArr, 1);
        auto kuType = inferLbugType(typesArr);
        propertyTypes.emplace(property, kuType.toString());
        char* typesJsonStr = yyjson_val_write(typesArr, 0, nullptr);
        originalTypes.emplace(property, typesJsonStr);
        free(typesJsonStr);
        propertyDefinitions.emplace_back(property, kuType.copy());
    }
    yyjson_doc_free(doc);
    std::sort(propertyDefinitions.begin(), propertyDefinitions.end(),
        [](const binder::ColumnDefinition& left, const binder::ColumnDefinition& right) {
            return left.name < right.name;
        });
    std::string ddlProperties = ",";
    for (auto& propertyDefinition : propertyDefinitions) {
        ddlProperties += propertyDefinition.name;
        ddlProperties += (" " + propertyDefinition.type.toString() + ",");
    }

    neo4jQuery = std::format("MATCH (a)-[:{}]->(b) RETURN distinct labels(a), labels(b);", relName);
    jsonStr = executeNeo4jQuery(cli, neo4jQuery);
    doc = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    resultsArr = yyjson_obj_get(yyjson_doc_get_root(doc), "results");
    firstResult = yyjson_arr_get(resultsArr, 0);
    dataArr = yyjson_obj_get(firstResult, "data");
    std::vector<std::pair<std::string, std::string>> nodePairs;
    std::string srcLabel, dstLabel;
    std::string nodePairsString;
    std::string copyQuery;
    yyjson_arr_foreach(dataArr, dataIdx, dataMax, dataItem) {
        auto rowArr = yyjson_obj_get(dataItem, "row");
        if (yyjson_arr_size(rowArr) == 0) {
            throw common::RuntimeException{"Error occurred while parsing neo4j result."};
        }
        auto firstElem = yyjson_arr_get(rowArr, 0);
        auto secondElem = yyjson_arr_get(rowArr, 1);
        if (yyjson_arr_size(firstElem) == 0 || yyjson_arr_size(secondElem) == 0) {
            continue;
        }
        srcLabel = yyjson_get_str(yyjson_arr_get(firstElem, 0));
        dstLabel = yyjson_get_str(yyjson_arr_get(secondElem, 0));
        if (std::find(nodeLabelsToImport.begin(), nodeLabelsToImport.end(), srcLabel) ==
            nodeLabelsToImport.end()) {
            throw common::RuntimeException{std::format(
                "The source node label '{}' of '{}' must be present in the nodes import list.",
                srcLabel, relName)};
        }
        if (std::find(nodeLabelsToImport.begin(), nodeLabelsToImport.end(), dstLabel) ==
            nodeLabelsToImport.end()) {
            throw common::RuntimeException{std::format(
                "The destination node label '{}' of '{}' must be present in the nodes import list.",
                dstLabel, relName)};
        }
        nodePairs.emplace_back(srcLabel, dstLabel);
        nodePairsString += std::format("FROM {} TO {},", srcLabel, dstLabel);
        exportNeo4jRelToCSV(relName, {srcLabel, dstLabel}, cli);
        auto relProperties = getRelProperties(cli, srcLabel, dstLabel, relName);

        std::string propertiesToCopy = "";
        std::string loadFromHeaders = "";
        for (auto i = 0u; i < relProperties.size(); i++) {
            propertiesToCopy += relProperties[i];
            loadFromHeaders +=
                std::format(", {} {}", relProperties[i], propertyTypes.at(relProperties[i]));
            if (i != relProperties.size() - 1) {
                propertiesToCopy += ",";
            }
            auto newRel = std::format("['{}_{}_{}','{}','{}','{}']", relName, srcLabel, dstLabel,
                relProperties[i], propertyTypes.at(relProperties[i]),
                originalTypes.at(relProperties[i]));
            outputTables.emplace_back(std::move(newRel));
        }
        copyQuery += std::format("COPY `{}`({}) FROM (LOAD WITH HEADERS(_id STRING, _labels "
                                 "STRING, _start INT64, _end INT64, _type STRING{}) FROM "
                                 "'/tmp/{}_{}_{}.csv'(sample_size=0, header=true) "
                                 "RETURN `_start`, `_end`{} {}) (from = \"{}\", to = \"{}\");",
            relName, propertiesToCopy, loadFromHeaders, srcLabel, relName, dstLabel,
            propertiesToCopy.empty() ? "" : ", ", propertiesToCopy, srcLabel, dstLabel);
    }
    yyjson_doc_free(doc);
    if (nodePairsString.empty()) {
        return "";
    }
    return std::format("CREATE REL TABLE `{}` ({} {});", relName,
               nodePairsString.substr(0, nodePairsString.size() - 1),
               ddlProperties.substr(0, ddlProperties.length() - 1)) +
           copyQuery;
}

std::string migrateQuery(ClientContext& /*context*/, const TableFuncBindData& bindData) {
    std::string result;
    std::vector<std::string> outputTables;
    auto neo4jMigrateBindData = bindData.constPtrCast<Neo4jMigrateBindData>();
    for (auto node : neo4jMigrateBindData->nodesToImport) {
        exportNeo4jNodeToCSV(node, *neo4jMigrateBindData->client);
        auto [ddl, copyQuery] =
            getCreateNodeTableQuery(*neo4jMigrateBindData->client, node, outputTables);
        result += ddl;
        result += copyQuery;
    }
    for (auto rel : neo4jMigrateBindData->relsToImport) {
        result += getCreateRelTableQuery(*neo4jMigrateBindData->client, rel,
            neo4jMigrateBindData->nodesToImport, outputTables);
    }
    std::string outputQuery;
    outputQuery.append("UNWIND [");
    for (auto i = 0u; i < outputTables.size(); i++) {
        outputQuery.append(outputTables[i]);
        if (i != outputTables.size() - 1) {
            outputQuery.append(",");
        }
    }
    outputQuery.append("] as row RETURN row[1] as lbug_table, row[2] as lbug_property, row[3] as "
                       "lbug_type, row[4] as neo4j_types;");
    result += outputQuery;
    return result;
}

static std::vector<common::LogicalType> inferInputTypes(
    const binder::expression_vector& /*params*/) {
    std::vector<common::LogicalType> inputTypes;
    inputTypes.push_back(common::LogicalType::STRING());
    inputTypes.push_back(common::LogicalType::STRING());
    inputTypes.push_back(common::LogicalType::STRING());
    inputTypes.push_back(common::LogicalType::LIST(common::LogicalType::STRING()));
    inputTypes.push_back(common::LogicalType::LIST(common::LogicalType::STRING()));
    return inputTypes;
}

function_set Neo4jMigrateFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::STRING, LogicalTypeID::STRING,
            LogicalTypeID::LIST, LogicalTypeID::LIST});
    func->tableFunc = TableFunction::emptyTableFunc;
    func->bindFunc = bindFunc;
    func->initSharedStateFunc = SimpleTableFunc::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->rewriteFunc = migrateQuery;
    func->canParallelFunc = [] { return false; };
    func->inferInputTypes = inferInputTypes;
    functionSet.push_back(std::move(func));
    return functionSet;
}

} // namespace neo4j_extension
} // namespace lbug
