#pragma once

#include "main/lbug.h"
#include "pybind_include.h"

using lbug::common::LogicalType;
using lbug::common::Value;

struct NPArrayWrapper {

public:
    NPArrayWrapper(const LogicalType& type, uint64_t numFlatTuple);

    void appendElement(Value* value);

private:
    py::dtype convertToArrayType(const LogicalType& type);

public:
    py::array data;
    uint8_t* dataBuffer;
    py::array mask;
    LogicalType type;
    uint64_t numElements;
};

class QueryResultConverter {
public:
    explicit QueryResultConverter(lbug::main::QueryResult* queryResult);

    py::object toDF();

private:
    lbug::main::QueryResult* queryResult;
    std::vector<std::unique_ptr<NPArrayWrapper>> columns;
};
