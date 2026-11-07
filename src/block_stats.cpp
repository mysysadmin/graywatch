#include "graywatch/block_stats.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace graywatch {
namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, last - first + 1);
}

bool counters_valid(const BlockDeviceSample& a, const BlockDeviceSample& b) {
    return b.reads_completed >= a.reads_completed &&
           b.sectors_read >= a.sectors_read && b.read_time_ms >= a.read_time_ms &&
           b.writes_completed >= a.writes_completed &&
           b.sectors_written >= a.sectors_written &&
           b.write_time_ms >= a.write_time_ms && b.io_time_ms >= a.io_time_ms &&
           b.weighted_io_time_ms >= a.weighted_io_time_ms &&
           b.discards_completed >= a.discards_completed &&
           b.discard_time_ms >= a.discard_time_ms &&
           b.flushes_completed >= a.flushes_completed &&
           b.flush_time_ms >= a.flush_time_ms;
}

}  // namespace

BlockDeviceSample parse_block_stat(std::string name, unsigned int major,
                                   unsigned int minor, std::string model,
                                   std::string_view line) {
    BlockDeviceSample sample{
        .name = std::move(name), .major = major, .minor = minor,
        .model = std::move(model),
    };
    std::istringstream input{std::string(line)};
    if (!(input >> sample.reads_completed >> sample.reads_merged >>
          sample.sectors_read >> sample.read_time_ms >> sample.writes_completed >>
          sample.writes_merged >> sample.sectors_written >> sample.write_time_ms >>
          sample.io_in_progress >> sample.io_time_ms >>
          sample.weighted_io_time_ms)) {
        throw std::runtime_error("invalid block stat line");
    }
    input >> sample.discards_completed >> sample.discards_merged >>
        sample.sectors_discarded >> sample.discard_time_ms >>
        sample.flushes_completed >> sample.flush_time_ms;
    return sample;
}

std::optional<BlockDeviceInterval> calculate_block_interval(
    const BlockDeviceSample& previous, const BlockDeviceSample& current,
    double elapsed_seconds) {
    if (elapsed_seconds <= 0.0 || previous.name != current.name ||
        previous.major != current.major || previous.minor != current.minor ||
        !counters_valid(previous, current)) {
        return std::nullopt;
    }
    const auto reads = current.reads_completed - previous.reads_completed;
    const auto writes = current.writes_completed - previous.writes_completed;
    const auto discards = current.discards_completed - previous.discards_completed;
    const auto flushes = current.flushes_completed - previous.flushes_completed;
    const auto read_ms = current.read_time_ms - previous.read_time_ms;
    const auto write_ms = current.write_time_ms - previous.write_time_ms;
    const auto discard_ms = current.discard_time_ms - previous.discard_time_ms;
    const auto flush_ms = current.flush_time_ms - previous.flush_time_ms;
    const auto io_ms = current.io_time_ms - previous.io_time_ms;
    const auto weighted_ms =
        current.weighted_io_time_ms - previous.weighted_io_time_ms;
    return BlockDeviceInterval{
        .name = current.name,
        .major = current.major,
        .minor = current.minor,
        .model = current.model,
        .read_iops = static_cast<double>(reads) / elapsed_seconds,
        .write_iops = static_cast<double>(writes) / elapsed_seconds,
        .discard_iops = static_cast<double>(discards) / elapsed_seconds,
        .flush_iops = static_cast<double>(flushes) / elapsed_seconds,
        .read_bytes_per_second =
            static_cast<double>(current.sectors_read - previous.sectors_read) * 512.0 /
            elapsed_seconds,
        .write_bytes_per_second =
            static_cast<double>(current.sectors_written - previous.sectors_written) *
            512.0 / elapsed_seconds,
        .average_read_latency_ms =
            reads == 0 ? 0.0 : static_cast<double>(read_ms) / reads,
        .average_write_latency_ms =
            writes == 0 ? 0.0 : static_cast<double>(write_ms) / writes,
        .average_discard_latency_ms =
            discards == 0 ? 0.0 : static_cast<double>(discard_ms) / discards,
        .average_flush_latency_ms =
            flushes == 0 ? 0.0 : static_cast<double>(flush_ms) / flushes,
        .busy_percent =
            std::min(100.0, static_cast<double>(io_ms) /
                                (elapsed_seconds * 1000.0) * 100.0),
        .average_queue_depth =
            static_cast<double>(weighted_ms) / (elapsed_seconds * 1000.0),
        .io_in_progress = current.io_in_progress,
    };
}

std::vector<BlockDeviceSample> read_block_devices(
    const std::filesystem::path& sys_block_root) {
    std::vector<BlockDeviceSample> samples;
    std::error_code error;
    std::filesystem::directory_iterator devices(sys_block_root, error);
    if (error) {
        return samples;
    }
    for (const auto& entry : devices) {
        const std::string name = entry.path().filename().string();
        const std::string dev = read_text(entry.path() / "dev");
        const auto separator = dev.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        try {
            const auto major = static_cast<unsigned int>(
                std::stoul(dev.substr(0, separator)));
            const auto minor = static_cast<unsigned int>(
                std::stoul(dev.substr(separator + 1)));
            const std::string stat = read_text(entry.path() / "stat");
            if (stat.empty()) {
                continue;
            }
            samples.push_back(parse_block_stat(
                name, major, minor, read_text(entry.path() / "device/model"), stat));
        } catch (const std::exception&) {
            continue;
        }
    }
    return samples;
}

std::vector<BlockDeviceInterval> calculate_block_intervals(
    const std::vector<BlockDeviceSample>& previous,
    const std::vector<BlockDeviceSample>& current, double elapsed_seconds) {
    using Key = std::pair<unsigned int, unsigned int>;
    std::map<Key, const BlockDeviceSample*> prior;
    for (const auto& sample : previous) {
        prior[{sample.major, sample.minor}] = &sample;
    }
    std::vector<BlockDeviceInterval> intervals;
    for (const auto& sample : current) {
        const auto found = prior.find({sample.major, sample.minor});
        if (found == prior.end()) {
            continue;
        }
        auto interval = calculate_block_interval(*found->second, sample,
                                                 elapsed_seconds);
        if (interval) {
            intervals.push_back(std::move(*interval));
        }
    }
    return intervals;
}

}  // namespace graywatch
