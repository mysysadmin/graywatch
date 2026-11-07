#include "graywatch/io_health.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}  // namespace

int main() {
    try {
        graywatch::BlockDeviceInterval device{.name = "test"};
        auto result = graywatch::assess_io(device);
        require(result.condition == graywatch::IoCondition::idle, "idle misclassified");

        device.read_iops = 100.0;
        device.busy_percent = 100.0;
        device.average_queue_depth = 0.2;
        device.average_read_latency_ms = 0.5;
        result = graywatch::assess_io(device);
        require(result.condition == graywatch::IoCondition::healthy,
                "busy responsive device misclassified");

        device.average_queue_depth = 2.0;
        result = graywatch::assess_io(device);
        require(result.condition == graywatch::IoCondition::saturated,
                "saturation not detected");

        device.busy_percent = 20.0;
        device.average_queue_depth = 0.1;
        device.average_read_latency_ms = 30.0;
        result = graywatch::assess_io(device);
        require(result.condition == graywatch::IoCondition::congested,
                "elevated latency not detected");

        device.average_read_latency_ms = 250.0;
        result = graywatch::assess_io(device);
        require(result.condition == graywatch::IoCondition::degraded,
                "absolute degradation not detected");
        require(result.severity >= 0.75, "degradation severity too low");
    } catch (const std::exception& error) {
        std::cerr << "io_health_test: " << error.what() << '\n';
        return 1;
    }
}
