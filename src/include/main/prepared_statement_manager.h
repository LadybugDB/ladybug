#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

namespace lbug {
namespace main {

struct CachedPreparedStatement;

class CachedPreparedStatementManager {
public:
    CachedPreparedStatementManager();
    ~CachedPreparedStatementManager();

    std::string addStatement(std::unique_ptr<CachedPreparedStatement> statement);

    // Removes the cached statement registered under `name` (if any). Called when the owning
    // PreparedStatement is destroyed, so the parsed statement, logical plan, and cached
    // physical plan are freed instead of living until the connection is closed.
    void removeStatement(const std::string& name);

    bool containsStatement(const std::string& name) const { return statementMap.contains(name); }

    CachedPreparedStatement* getCachedStatement(const std::string& name) const;

private:
    std::mutex mtx;
    uint32_t currentIdx = 0;
    std::unordered_map<std::string, std::unique_ptr<CachedPreparedStatement>> statementMap;
};

} // namespace main
} // namespace lbug
