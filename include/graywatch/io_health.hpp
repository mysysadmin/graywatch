#pragma once

#include "graywatch/block_stats.hpp"

#include <string_view>

namespace graywatch {

enum class IoCondition { idle, healthy, congested, saturated, degraded };

struct IoAssessment {
    IoCondition condition{IoCondition::idle};
    double severity{};
    double maximum_average_latency_ms{};
    std::string_view reason{"no_io"};
};

// Latency and queue limits for one interval. Rotational disks get wider
// limits than solid-state devices.
struct IoThresholds {
    double degraded_ms{};
    double congested_ms{};
    double congested_queue{};
    double saturated_queue{};
};
IoThresholds io_thresholds(bool rotational);

// Uses device.rotational and device.stacked to pick the limits.
IoAssessment assess_io(const BlockDeviceInterval& device);
std::string_view to_string(IoCondition condition);

}  // namespace graywatch
