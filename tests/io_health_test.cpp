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
        graywatch::BlockDeviceInterval device{.name = "test", .model = "test"};
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

        // A spinning disk under random load: 25 ms averages, NCQ queue of 3,
        // 100% busy. Normal for an HDD, congested for an SSD.
        graywatch::BlockDeviceInterval disk{.name = "sdc", .model = "", .rotational = true};
        disk.read_iops = 80.0;
        disk.busy_percent = 100.0;
        disk.average_queue_depth = 3.0;
        disk.average_read_latency_ms = 25.0;
        require(graywatch::assess_io(disk).condition == graywatch::IoCondition::healthy,
                "loaded HDD flagged");
        disk.rotational = false;
        require(graywatch::assess_io(disk).condition == graywatch::IoCondition::saturated,
                "SSD limits changed");
        disk.rotational = true;
        disk.average_read_latency_ms = 80.0;
        require(graywatch::assess_io(disk).condition == graywatch::IoCondition::congested,
                "slow HDD not congested");
        disk.average_read_latency_ms = 300.0;
        require(graywatch::assess_io(disk).condition == graywatch::IoCondition::degraded,
                "very slow HDD not degraded");
        disk.average_read_latency_ms = 25.0;
        disk.average_queue_depth = 6.0;
        require(graywatch::assess_io(disk).condition == graywatch::IoCondition::saturated,
                "queued HDD not saturated");

        // md0 over SSDs: busy 100% with one request in flight is how md
        // accounts time, not a full device.
        graywatch::BlockDeviceInterval md{.name = "md0", .model = "", .rotational = false, .stacked = true};
        md.write_iops = 220.0;
        md.busy_percent = 100.0;
        md.average_queue_depth = 5.0;
        md.average_write_latency_ms = 4.6;
        require(graywatch::assess_io(md).condition == graywatch::IoCondition::healthy,
                "busy md device flagged");
        md.average_write_latency_ms = 40.0;
        require(graywatch::assess_io(md).condition == graywatch::IoCondition::congested,
                "slow md device not congested");
    } catch (const std::exception& error) {
        std::cerr << "io_health_test: " << error.what() << '\n';
        return 1;
    }
}
