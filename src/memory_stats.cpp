#include "graywatch/memory_stats.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace graywatch {
namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

double rate(std::uint64_t previous, std::uint64_t current, double elapsed) {
    return static_cast<double>(current - previous) / elapsed;
}

}  // namespace

void parse_meminfo(std::string_view text, MemorySample& sample) {
    std::istringstream input{std::string(text)};
    std::string name;
    std::uint64_t value{};
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        if (!(fields >> name >> value)) continue;
        if (name == "MemTotal:") sample.total_kib = value;
        else if (name == "MemAvailable:") sample.available_kib = value;
        else if (name == "SwapTotal:") sample.swap_total_kib = value;
        else if (name == "SwapFree:") sample.swap_free_kib = value;
        else if (name == "Dirty:") sample.dirty_kib = value;
        else if (name == "Writeback:") sample.writeback_kib = value;
    }
}

void parse_vmstat(std::string_view text, MemorySample& sample) {
    std::istringstream input{std::string(text)};
    std::string name;
    std::uint64_t value{};
    std::uint64_t legacy_refaults{};
    bool split_refaults{};
    while (input >> name >> value) {
        if (name == "pgmajfault") sample.major_faults = value;
        else if (name == "pswpin") sample.swap_in_pages = value;
        else if (name == "pswpout") sample.swap_out_pages = value;
        // Kernels split allocstall by zone (dma, dma32, normal, movable, ...).
        else if (name.starts_with("allocstall")) sample.direct_reclaim_stalls += value;
        else if (name == "pgscan_direct") sample.direct_scanned_pages = value;
        else if (name == "pgscan_kswapd") sample.kswapd_scanned_pages = value;
        else if (name == "compact_stall") sample.compaction_stalls = value;
        else if (name == "oom_kill") sample.oom_kills = value;
        else if (name == "workingset_refault_file") {
            sample.file_refaults = value;
            split_refaults = true;
        } else if (name == "workingset_refault") {
            legacy_refaults = value;
        }
    }
    if (!split_refaults) sample.file_refaults = legacy_refaults;
}

MemorySample read_memory_sample(const std::filesystem::path& meminfo,
                                const std::filesystem::path& vmstat) {
    MemorySample sample;
    parse_meminfo(read_file(meminfo), sample);
    parse_vmstat(read_file(vmstat), sample);
    return sample;
}

std::optional<MemoryInterval> calculate_memory_interval(
    const MemorySample& previous, const MemorySample& current,
    double elapsed_seconds) {
    if (elapsed_seconds <= 0.0 || current.total_kib == 0 ||
        current.major_faults < previous.major_faults ||
        current.swap_in_pages < previous.swap_in_pages ||
        current.swap_out_pages < previous.swap_out_pages ||
        current.direct_reclaim_stalls < previous.direct_reclaim_stalls ||
        current.direct_scanned_pages < previous.direct_scanned_pages ||
        current.kswapd_scanned_pages < previous.kswapd_scanned_pages ||
        current.compaction_stalls < previous.compaction_stalls ||
        current.file_refaults < previous.file_refaults ||
        current.oom_kills < previous.oom_kills) {
        return std::nullopt;
    }
    return MemoryInterval{
        .total_kib = current.total_kib,
        .available_kib = current.available_kib,
        .available_percent = static_cast<double>(current.available_kib) /
                             static_cast<double>(current.total_kib) * 100.0,
        .swap_total_kib = current.swap_total_kib,
        .swap_used_kib = current.swap_total_kib >= current.swap_free_kib
            ? current.swap_total_kib - current.swap_free_kib : 0,
        .dirty_kib = current.dirty_kib,
        .writeback_kib = current.writeback_kib,
        .major_faults_per_second =
            rate(previous.major_faults, current.major_faults, elapsed_seconds),
        .swap_in_pages_per_second =
            rate(previous.swap_in_pages, current.swap_in_pages, elapsed_seconds),
        .swap_out_pages_per_second =
            rate(previous.swap_out_pages, current.swap_out_pages, elapsed_seconds),
        .direct_reclaim_stalls =
            current.direct_reclaim_stalls - previous.direct_reclaim_stalls,
        .direct_scanned_pages_per_second = rate(
            previous.direct_scanned_pages, current.direct_scanned_pages,
            elapsed_seconds),
        .kswapd_scanned_pages_per_second = rate(
            previous.kswapd_scanned_pages, current.kswapd_scanned_pages,
            elapsed_seconds),
        .compaction_stalls = current.compaction_stalls - previous.compaction_stalls,
        .file_refaults_per_second =
            rate(previous.file_refaults, current.file_refaults, elapsed_seconds),
        .oom_kills = current.oom_kills - previous.oom_kills,
    };
}

MemoryAssessment assess_memory(const MemoryInterval& interval,
                               bool psi_available, double psi_some_percent,
                               double psi_full_percent) {
    // An OOM kill is unambiguous: the kernel terminated a process.
    if (interval.oom_kills != 0) {
        return {MemoryCondition::degraded, 0.95, "oom_kill"};
    }
    // memory "full": all non-idle tasks were stalled on memory simultaneously.
    if (psi_available && psi_full_percent >= 10.0) {
        return {MemoryCondition::degraded,
                std::clamp(0.75 + psi_full_percent / 400.0, 0.75, 1.0),
                "memory_stall_full"};
    }
    if (psi_available && psi_some_percent >= 10.0) {
        return {MemoryCondition::pressure,
                std::clamp(0.40 + psi_some_percent / 200.0, 0.40, 0.74),
                "memory_stall_some"};
    }
    // Without PSI, direct reclaim is the strongest portable stall signal: an
    // allocating task had to reclaim memory itself instead of kswapd doing it
    // in the background.
    if (interval.direct_reclaim_stalls != 0) {
        return {MemoryCondition::pressure,
                std::clamp(0.40 + static_cast<double>(interval.direct_reclaim_stalls) /
                                      1000.0,
                           0.40, 0.70),
                "direct_reclaim"};
    }
    if (interval.swap_in_pages_per_second >= 100.0) {
        return {MemoryCondition::pressure, 0.45, "swap_in"};
    }
    if (interval.available_percent < 5.0) {
        return {MemoryCondition::pressure, 0.40, "low_available"};
    }
    const double some = psi_available ? psi_some_percent : 0.0;
    return {MemoryCondition::healthy, std::clamp(some / 100.0, 0.01, 0.20),
            "no_pressure"};
}

std::string_view to_string(MemoryCondition condition) {
    switch (condition) {
        case MemoryCondition::healthy: return "healthy";
        case MemoryCondition::pressure: return "pressure";
        case MemoryCondition::degraded: return "degraded";
    }
    return "unknown";
}

}  // namespace graywatch
