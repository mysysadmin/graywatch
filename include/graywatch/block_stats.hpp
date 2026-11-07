#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace graywatch {

struct BlockDeviceSample {
    std::string name;
    unsigned int major{};
    unsigned int minor{};
    std::string model;
    std::uint64_t reads_completed{};
    std::uint64_t reads_merged{};
    std::uint64_t sectors_read{};
    std::uint64_t read_time_ms{};
    std::uint64_t writes_completed{};
    std::uint64_t writes_merged{};
    std::uint64_t sectors_written{};
    std::uint64_t write_time_ms{};
    std::uint64_t io_in_progress{};
    std::uint64_t io_time_ms{};
    std::uint64_t weighted_io_time_ms{};
    std::uint64_t discards_completed{};
    std::uint64_t discards_merged{};
    std::uint64_t sectors_discarded{};
    std::uint64_t discard_time_ms{};
    std::uint64_t flushes_completed{};
    std::uint64_t flush_time_ms{};
};

struct BlockDeviceInterval {
    std::string name;
    unsigned int major{};
    unsigned int minor{};
    std::string model;
    double read_iops{};
    double write_iops{};
    double discard_iops{};
    double flush_iops{};
    double read_bytes_per_second{};
    double write_bytes_per_second{};
    double average_read_latency_ms{};
    double average_write_latency_ms{};
    double average_discard_latency_ms{};
    double average_flush_latency_ms{};
    double busy_percent{};
    double average_queue_depth{};
    std::uint64_t io_in_progress{};
};

BlockDeviceSample parse_block_stat(std::string name, unsigned int major,
                                   unsigned int minor, std::string model,
                                   std::string_view line);
std::optional<BlockDeviceInterval> calculate_block_interval(
    const BlockDeviceSample& previous, const BlockDeviceSample& current,
    double elapsed_seconds);
std::vector<BlockDeviceSample> read_block_devices(
    const std::filesystem::path& sys_block_root = "/sys/block");
std::vector<BlockDeviceInterval> calculate_block_intervals(
    const std::vector<BlockDeviceSample>& previous,
    const std::vector<BlockDeviceSample>& current, double elapsed_seconds);

}  // namespace graywatch
