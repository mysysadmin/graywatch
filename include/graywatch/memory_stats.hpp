#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace graywatch {

// Point-in-time memory state from /proc/meminfo plus cumulative reclaim and
// fault counters from /proc/vmstat. Both files exist on every supported
// kernel, so memory evidence does not depend on PSI.
struct MemorySample {
    std::uint64_t total_kib{};
    std::uint64_t available_kib{};
    std::uint64_t swap_total_kib{};
    std::uint64_t swap_free_kib{};
    std::uint64_t dirty_kib{};
    std::uint64_t writeback_kib{};
    std::uint64_t major_faults{};
    std::uint64_t swap_in_pages{};
    std::uint64_t swap_out_pages{};
    std::uint64_t direct_reclaim_stalls{};
    std::uint64_t direct_scanned_pages{};
    std::uint64_t kswapd_scanned_pages{};
    std::uint64_t compaction_stalls{};
    std::uint64_t file_refaults{};
    std::uint64_t oom_kills{};
};

struct MemoryInterval {
    std::uint64_t total_kib{};
    std::uint64_t available_kib{};
    double available_percent{};
    std::uint64_t swap_total_kib{};
    std::uint64_t swap_used_kib{};
    std::uint64_t dirty_kib{};
    std::uint64_t writeback_kib{};
    double major_faults_per_second{};
    double swap_in_pages_per_second{};
    double swap_out_pages_per_second{};
    std::uint64_t direct_reclaim_stalls{};
    double direct_scanned_pages_per_second{};
    double kswapd_scanned_pages_per_second{};
    std::uint64_t compaction_stalls{};
    double file_refaults_per_second{};
    std::uint64_t oom_kills{};
};

enum class MemoryCondition { healthy, pressure, degraded };

struct MemoryAssessment {
    MemoryCondition condition{MemoryCondition::healthy};
    double severity{0.01};
    std::string_view reason{"no_pressure"};
};

void parse_meminfo(std::string_view text, MemorySample& sample);
void parse_vmstat(std::string_view text, MemorySample& sample);
MemorySample read_memory_sample(
    const std::filesystem::path& meminfo = "/proc/meminfo",
    const std::filesystem::path& vmstat = "/proc/vmstat");
std::optional<MemoryInterval> calculate_memory_interval(
    const MemorySample& previous, const MemorySample& current,
    double elapsed_seconds);

// Baseline-independent memory guardrails. PSI values are used when available
// (psi_available) and otherwise ignored; vmstat evidence always applies.
MemoryAssessment assess_memory(const MemoryInterval& interval,
                               bool psi_available, double psi_some_percent,
                               double psi_full_percent);
std::string_view to_string(MemoryCondition condition);

}  // namespace graywatch
