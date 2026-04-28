#pragma once

#include <string>

#include "common/api.h"
#include "main/option.h"

namespace lbug {
namespace common {
enum class LogicalTypeID : uint8_t;
} // namespace common

namespace main {

class ClientContext;
struct SystemConfig;

struct DBConfig {
    uint64_t bufferPoolSize;
    uint64_t maxNumThreads;
    bool enableCompression;
    bool readOnly;
    uint64_t maxDBSize;
    bool enableMultiWrites;
    bool autoCheckpoint;
    uint64_t checkpointThreshold;
    bool forceCheckpointOnClose;
    bool throwOnWalReplayFailure;
    bool enableChecksums;
    bool enableDefaultHashIndex;
    bool enableSpillingToDisk;
#if defined(__APPLE__)
    uint32_t threadQos;
#endif

    explicit DBConfig(const SystemConfig& systemConfig);

    static ConfigurationOption* getOptionByName(const std::string& optionName);
    LBUG_API static bool isDBPathInMemory(const std::string& dbPath);
};

} // namespace main
} // namespace lbug
