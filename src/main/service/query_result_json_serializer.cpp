#include "main/service/query_result_json_serializer.h"

#include <cstdlib>

#include "processor/result/flat_tuple.h"
#include "yyjson.h"

namespace lbug {
namespace main {

std::string queryResultToJson(QueryResult& result) {
    auto* doc = yyjson_mut_doc_new(nullptr);
    auto* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (!result.isSuccess()) {
        yyjson_mut_obj_add_strcpy(doc, root, "error", result.getErrorMessage().c_str());
        auto* str = yyjson_mut_write(doc, 0, nullptr);
        std::string json(str ? str : "{}");
        free(str);
        yyjson_mut_doc_free(doc);
        return json;
    }

    // columns
    auto columnNames = result.getColumnNames();
    auto* colsArr = yyjson_mut_arr(doc);
    for (const auto& name : columnNames) {
        yyjson_mut_arr_add_strcpy(doc, colsArr, name.c_str());
    }
    yyjson_mut_obj_add_val(doc, root, "columns", colsArr);

    // rows
    auto* rowsArr = yyjson_mut_arr(doc);
    uint64_t numRows = 0;
    while (result.hasNext()) {
        auto tuple = result.getNext();
        auto* rowArr = yyjson_mut_arr(doc);
        for (uint32_t i = 0; i < columnNames.size(); ++i) {
            auto* val = tuple->getValue(i);
            if (val->isNull()) {
                yyjson_mut_arr_add_null(doc, rowArr);
            } else {
                auto valStr = val->toString();
                yyjson_mut_arr_add_strcpy(doc, rowArr, valStr.c_str());
            }
        }
        yyjson_mut_arr_add_val(rowsArr, rowArr);
        numRows++;
    }
    yyjson_mut_obj_add_val(doc, root, "rows", rowsArr);
    yyjson_mut_obj_add_uint(doc, root, "numRows", numRows);

    // timing
    auto* summary = result.getQuerySummary();
    if (summary) {
        yyjson_mut_obj_add_real(doc, root, "compilingTime", summary->getCompilingTime());
        yyjson_mut_obj_add_real(doc, root, "executionTime", summary->getExecutionTime());
    }

    auto* str = yyjson_mut_write(doc, 0, nullptr);
    std::string json(str ? str : "{}");
    free(str);
    yyjson_mut_doc_free(doc);
    return json;
}

} // namespace main
} // namespace lbug
