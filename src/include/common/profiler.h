#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/metric.h"

namespace lbug {
namespace common {

class Profiler {

public:
    void startQueryTimer();

    double getQueryElapsedTimeMS() const;

    TimeMetric* registerTimeMetric(const std::string& key);

    NumericMetric* registerNumericMetric(const std::string& key);

    double sumAllTimeMetricsWithKey(const std::string& key);

    uint64_t sumAllNumericMetricsWithKey(const std::string& key);

private:
    void addMetric(const std::string& key, std::unique_ptr<Metric> metric);

public:
    Timer queryTimer;
    bool queryTimerStarted = false;
    mutable std::mutex mtx;
    bool enabled = false;
    std::unordered_map<std::string, std::vector<std::unique_ptr<Metric>>> metrics;
};

} // namespace common
} // namespace lbug
