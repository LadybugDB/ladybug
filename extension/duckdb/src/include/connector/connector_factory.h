#pragma once

#include <memory>
#include <string>

namespace lbug {
namespace duckdb_extension {

class DuckDBConnector;

class DuckDBConnectorFactory {
public:
    static std::unique_ptr<DuckDBConnector> getDuckDBConnector(const std::string& dbPath);
};

} // namespace duckdb_extension
} // namespace lbug
