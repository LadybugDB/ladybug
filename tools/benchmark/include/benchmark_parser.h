#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lbug {
namespace benchmark {

struct ParsedBenchmark {
    std::string name;
    std::string preRun;
    std::string query;
    std::string postRun;
    uint64_t numThreads = 4;
    uint64_t expectedNumTuples = 0;
    std::vector<std::string> expectedTuples;
    bool checkOutputOrder = false;
    bool compareResult = true;
};

class BenchmarkParser {
public:
    std::vector<std::unique_ptr<ParsedBenchmark>> parseBenchmarkFile(const std::string& path,
        bool checkOutputOrder = false);

private:
    void replaceVariables(std::string& str) const;

    std::unordered_map<std::string, std::string> variableMap = {
        {"LBUG_ROOT_DIRECTORY", LBUG_ROOT_DIRECTORY}};
};

} // namespace benchmark
} // namespace lbug
