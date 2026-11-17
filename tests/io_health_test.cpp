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

        // Saturation is capacity evidence: it never reaches the 0.75 at which
        // graywatchd requests an automatic collection.
        graywatch::BlockDeviceInterval busy{.name = "nvme0n1", .model = "test"};
        busy.read_iops = 5000.0;
        busy.busy_percent = 100.0;
        busy.average_queue_depth = 1500.0;
        busy.average_read_latency_ms = 1.1;
        result = graywatch::assess_io(busy);
        require(result.condition == graywatch::IoCondition::saturated &&
                    result.severity < 0.75,
                "saturation alone triggers a collection");

        // Slow discards delay no reader or writer.
        graywatch::BlockDeviceInterval trim{.name = "nvme1n1", .model = "test"};
        trim.discard_iops = 2.0;
        trim.average_discard_latency_ms = 400.0;
        trim.read_iops = 100.0;
        trim.average_read_latency_ms = 0.2;
        result = graywatch::assess_io(trim);
        require(result.condition == graywatch::IoCondition::healthy, "slow discard flagged");

        // A busy hard disk queueing at 70 ms is load, not a fault; 600 ms is.
        graywatch::BlockDeviceInterval hdd{.name = "sdz", .model = "test", .rotational = true};
        hdd.read_iops = 120.0;
        hdd.busy_percent = 80.0;
        hdd.average_queue_depth = 8.0;
        hdd.average_read_latency_ms = 70.0;
        result = graywatch::assess_io(hdd);
        require(result.condition == graywatch::IoCondition::healthy, "busy HDD flagged");
        hdd.average_read_latency_ms = 150.0;
        result = graywatch::assess_io(hdd);
        require(result.condition == graywatch::IoCondition::congested, "slow HDD not congested");
        hdd.average_read_latency_ms = 600.0;
        result = graywatch::assess_io(hdd);
        require(result.condition == graywatch::IoCondition::degraded, "HDD degradation missed");

        // md/dm busy time and queue depth add up their members' parallel work.
        graywatch::BlockDeviceInterval md{.name = "md0", .model = "", .stacked = true};
        md.write_iops = 300.0;
        md.busy_percent = 100.0;
        md.average_queue_depth = 12.0;
        md.average_write_latency_ms = 9.7;
        result = graywatch::assess_io(md);
        require(result.condition == graywatch::IoCondition::healthy, "busy md0 flagged");
    } catch (const std::exception& error) {
        std::cerr << "io_health_test: " << error.what() << '\n';
        return 1;
    }
}
