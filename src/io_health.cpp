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

IoThresholds io_thresholds(bool rotational) {
    // A spinning disk needs 5-15 ms per random request and keeps up to 32
    // requests in its own queue (NCQ), so 20 ms averages and a queue depth
    // of 4 are its normal working range under load, not congestion.
    if (rotational) return {.degraded_ms = 250.0, .congested_ms = 60.0,
                            .congested_queue = 16.0, .saturated_queue = 4.0};
    return {.degraded_ms = 100.0, .congested_ms = 20.0, .congested_queue = 4.0,
            .saturated_queue = 1.0};
}

IoAssessment assess_io(const BlockDeviceInterval& device) {
    const bool active = device.read_iops != 0.0 || device.write_iops != 0.0 ||
                        device.discard_iops != 0.0 || device.flush_iops != 0.0;
    if (!active) return {};

    const double latency = std::max(
        {device.average_read_latency_ms, device.average_write_latency_ms,
         device.average_discard_latency_ms, device.average_flush_latency_ms});
    const IoThresholds limit = io_thresholds(device.rotational);

    // This is an absolute guardrail, not a learned expectation. It prevents a
    // chronically slow state from becoming statistically normal.
    if (latency >= limit.degraded_ms) {
        return {IoCondition::degraded,
                std::clamp(0.75 + (latency - limit.degraded_ms) / 1600.0, 0.75, 1.0),
                latency, "absolute_latency"};
    }

    // Utilization alone is not failure: a responsive device may be 100% busy.
    // Saturation additionally requires queued work. Stacked bio-based devices
    // (md, device-mapper) count time with any request in flight below them,
    // so their busy time says nothing about capacity.
    if (!device.stacked && device.busy_percent >= 90.0 &&
        device.average_queue_depth >= limit.saturated_queue) {
        return {IoCondition::saturated,
                std::clamp(0.60 + device.average_queue_depth / (40.0 * limit.saturated_queue),
                           0.60, 0.90),
                latency, "busy_with_queue"};
    }

    if ((!device.stacked && device.average_queue_depth >= limit.congested_queue) ||
        latency >= limit.congested_ms) {
        return {IoCondition::congested,
                std::clamp(0.35 + std::max(device.average_queue_depth /
                                               (10.0 * limit.congested_queue),
                                           latency / (20.0 * limit.congested_ms)),
                           0.35, 0.74),
                latency, latency >= limit.congested_ms ? "elevated_latency" : "queueing"};
    }

    return {IoCondition::healthy,
            std::clamp(device.busy_percent / 500.0, 0.01, 0.20), latency,
            "responsive"};
}

}  // namespace graywatch
