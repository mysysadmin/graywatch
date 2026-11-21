#include "graywatch/block_stats.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
    try {
        const auto before = graywatch::parse_block_stat(
            "nvme0n1", 259, 0, "Test NVMe",
            "100 2 1000 200 200 3 2000 500 4 600 700 10 1 100 40 5 25");
        const auto after = graywatch::parse_block_stat(
            "nvme0n1", 259, 0, "Test NVMe",
            "120 2 1400 240 210 3 2400 550 2 700 850 12 1 120 50 7 29");
        const auto value = graywatch::calculate_block_interval(before, after, 2.0);
        require(value.has_value(), "valid block interval rejected");
        require(std::abs(value->read_iops - 10.0) < 0.0001, "read iops wrong");
        require(std::abs(value->write_iops - 5.0) < 0.0001, "write iops wrong");
        require(std::abs(value->average_read_latency_ms - 2.0) < 0.0001,
                "read latency wrong");
        require(std::abs(value->average_write_latency_ms - 5.0) < 0.0001,
                "write latency wrong");
        require(std::abs(value->busy_percent - 5.0) < 0.0001, "busy wrong");
        require(std::abs(value->average_queue_depth - 0.075) < 0.0001,
                "queue depth wrong");
        require(value->io_in_progress == 2, "in-flight count wrong");
        auto reset = after;
        reset.reads_completed = 1;
        require(!graywatch::calculate_block_interval(after, reset, 1.0),
                "counter reset accepted");

        // Device type from a fake /sys/block: sdc spins and has a device/
        // link, md0 is stacked, nvme0n1 is a solid-state disk.
        const auto root = std::filesystem::temp_directory_path() /
                          ("graywatch-block-" + std::to_string(::getpid()));
        const auto write = [](const std::filesystem::path& path, const std::string& text) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path) << text << '\n';
        };
        const std::string stat = "1 0 8 1 1 0 8 1 0 1 2";
        write(root / "sdc/dev", "8:32");
        write(root / "sdc/stat", stat);
        write(root / "sdc/queue/rotational", "1");
        write(root / "sdc/device/model", "HDD");
        write(root / "md0/dev", "9:0");
        write(root / "md0/stat", stat);
        write(root / "md0/queue/rotational", "0");
        write(root / "nvme0n1/dev", "259:0");
        write(root / "nvme0n1/stat", stat);
        write(root / "nvme0n1/queue/rotational", "0");
        write(root / "nvme0n1/device/model", "NVMe");
        int seen = 0;
        for (const auto& sample : graywatch::read_block_devices(root)) {
            ++seen;
            if (sample.name == "sdc") require(sample.rotational && !sample.stacked, "sdc type");
            if (sample.name == "md0") require(!sample.rotational && sample.stacked, "md0 type");
            if (sample.name == "nvme0n1") {
                require(!sample.rotational && !sample.stacked, "nvme0n1 type");
            }
        }
        std::filesystem::remove_all(root);
        require(seen == 3, "fake sysfs devices read");
        graywatch::BlockDeviceSample disk_before;
        disk_before.name = "sdc";
        disk_before.rotational = true;
        auto disk_after = disk_before;
        disk_after.reads_completed = 1;
        const auto interval = graywatch::calculate_block_interval(disk_before, disk_after, 1.0);
        require(interval && interval->rotational, "interval keeps device type");
    } catch (const std::exception& error) {
        std::cerr << "block_stats_test: " << error.what() << '\n';
        return 1;
    }
}
