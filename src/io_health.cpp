#include "graywatch/io_health.hpp"

#include <algorithm>

namespace graywatch {

std::string_view to_string(IoCondition condition) {
    switch (condition) {
        case IoCondition::idle: return "idle";
        case IoCondition::healthy: return "healthy";
        case IoCondition::congested: return "congested";
        case IoCondition::saturated: return "saturated";
        case IoCondition::degraded: return "degraded";
    }
    return "unknown";
}

IoAssessment assess_io(const BlockDeviceInterval& device) {
    const bool active = device.read_iops != 0.0 || device.write_iops != 0.0 ||
                        device.discard_iops != 0.0 || device.flush_iops != 0.0;
    if (!active) return {};

    const double latency = std::max(
        {device.average_read_latency_ms, device.average_write_latency_ms,
         device.average_discard_latency_ms, device.average_flush_latency_ms});

    // This is an absolute guardrail, not a learned expectation. It prevents a
    // chronically slow state from becoming statistically normal.
    if (latency >= 100.0) {
        return {IoCondition::degraded,
                std::clamp(0.75 + (latency - 100.0) / 1600.0, 0.75, 1.0),
                latency, "absolute_latency"};
    }

    // Utilization alone is not failure: a responsive device may be 100% busy.
    // Saturation additionally requires queued work.
    if (device.busy_percent >= 90.0 && device.average_queue_depth >= 1.0) {
        return {IoCondition::saturated,
                std::clamp(0.60 + device.average_queue_depth / 40.0, 0.60, 0.90),
                latency, "busy_with_queue"};
    }

    if (device.average_queue_depth >= 4.0 || latency >= 20.0) {
        return {IoCondition::congested,
                std::clamp(0.35 + std::max(device.average_queue_depth / 40.0,
                                           latency / 400.0),
                           0.35, 0.74),
                latency, latency >= 20.0 ? "elevated_latency" : "queueing"};
    }

    return {IoCondition::healthy,
            std::clamp(device.busy_percent / 500.0, 0.01, 0.20), latency,
            "responsive"};
}

}  // namespace graywatch
