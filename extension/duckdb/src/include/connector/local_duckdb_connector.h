#pragma once

#include "connector/duckdb_connector.h"

namespace lbug {
namespace duckdb_extension {

class LocalDuckDBConnector : public DuckDBConnector {
public:
    void connect(const std::string& dbPath, const std::string& catalogName,
        const std::string& schemaName, main::ClientContext* context) override;
};

} // namespace duckdb_extension
} // namespace lbug
