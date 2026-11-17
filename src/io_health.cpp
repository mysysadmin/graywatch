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

    // Discards are background space reclamation; slow ones (NVMe trims often
    // take tens of milliseconds) delay no reader or writer.
    const double latency = std::max(
        {device.average_read_latency_ms, device.average_write_latency_ms,
         device.average_flush_latency_ms});

    // Average latency includes the time requests queue in the kernel. A busy
    // spinning disk serves one seek at a time, so under load 50-100 ms is
    // normal queueing, not a fault; solid-state devices answer in well under
    // a millisecond and 20 ms already means trouble.
    const double degraded_ms = device.rotational ? 500.0 : 100.0;
    const double congested_ms = device.rotational ? 100.0 : 20.0;
    const double congested_queue = device.rotational ? 32.0 : 4.0;

    // This is an absolute guardrail, not a learned expectation. It prevents a
    // chronically slow state from becoming statistically normal.
    if (latency >= degraded_ms) {
        return {IoCondition::degraded,
                std::clamp(0.75 + (latency - degraded_ms) / (16.0 * degraded_ms), 0.75, 1.0),
                latency, "absolute_latency"};
    }

    // A stacked device (md, device-mapper) counts its members' parallel work
    // as its own busy time and queue: only its latency says anything.
    if (!device.stacked) {
        // Utilization alone is not failure: a responsive device may be 100%
        // busy. Saturation additionally requires queued work. It is capacity
        // evidence, not a fault, so it stays below the 0.75 severity at which
        // graywatchd requests an automatic collection.
        if (device.busy_percent >= 90.0 && device.average_queue_depth >= 1.0) {
            return {IoCondition::saturated,
                    std::clamp(0.60 + device.average_queue_depth / 400.0, 0.60, 0.70),
                    latency, "busy_with_queue"};
        }
        // Queue depth is derived from the time requests spent in flight; on a
        // device that is mostly idle it only reflects a few long requests
        // (discards, flushes), not waiting work.
        if (device.average_queue_depth >= congested_queue && device.busy_percent >= 50.0) {
            return {IoCondition::congested,
                    std::clamp(0.35 + device.average_queue_depth / (10.0 * congested_queue),
                               0.35, 0.74),
                    latency, "queueing"};
        }
    }

    if (latency >= congested_ms) {
        return {IoCondition::congested,
                std::clamp(0.35 + latency / (20.0 * congested_ms), 0.35, 0.74), latency,
                "elevated_latency"};
    }

    return {IoCondition::healthy,
            std::clamp(device.busy_percent / 500.0, 0.01, 0.20), latency,
            "responsive"};
}

}  // namespace graywatch
