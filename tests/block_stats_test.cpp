#include "graywatch/block_stats.hpp"

#include <cmath>
#include <iostream>
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
    } catch (const std::exception& error) {
        std::cerr << "block_stats_test: " << error.what() << '\n';
        return 1;
    }
}
