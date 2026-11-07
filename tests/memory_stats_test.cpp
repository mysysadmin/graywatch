#include "graywatch/memory_stats.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        graywatch::MemorySample before;
        graywatch::parse_meminfo(
            "MemTotal:       1000000 kB\nMemFree:          10000 kB\n"
            "MemAvailable:    400000 kB\nSwapTotal:       200000 kB\n"
            "SwapFree:        150000 kB\nDirty:              12 kB\n"
            "Writeback:           3 kB\n",
            before);
        graywatch::parse_vmstat(
            "pgmajfault 100\npswpin 10\npswpout 20\nallocstall_dma 0\n"
            "allocstall_normal 5\nallocstall_movable 7\npgscan_direct 1000\n"
            "pgscan_kswapd 2000\ncompact_stall 1\nworkingset_refault_anon 9\n"
            "workingset_refault_file 50\noom_kill 2\n",
            before);
        require(before.total_kib == 1000000 && before.available_kib == 400000,
                "meminfo parse is wrong");
        require(before.direct_reclaim_stalls == 12, "allocstall zones not summed");
        require(before.file_refaults == 50, "file refaults not parsed");

        graywatch::MemorySample legacy;
        graywatch::parse_vmstat("workingset_refault 33\n", legacy);
        require(legacy.file_refaults == 33, "legacy refault counter ignored");

        auto after = before;
        after.major_faults += 200;
        after.swap_in_pages += 300;
        const auto quiet = graywatch::calculate_memory_interval(before, before, 1.0);
        require(quiet && quiet->available_percent == 40.0 &&
                    quiet->swap_used_kib == 50000,
                "steady interval is wrong");
        require(graywatch::assess_memory(*quiet, false, 0, 0).condition ==
                    graywatch::MemoryCondition::healthy,
                "steady memory not healthy");

        const auto swapping = graywatch::calculate_memory_interval(before, after, 2.0);
        require(swapping && swapping->major_faults_per_second == 100.0 &&
                    swapping->swap_in_pages_per_second == 150.0,
                "rates are wrong");
        require(graywatch::assess_memory(*swapping, false, 0, 0).reason == "swap_in",
                "swap-in not detected without PSI");

        after.direct_reclaim_stalls += 4;
        auto reclaim = graywatch::calculate_memory_interval(before, after, 1.0);
        require(graywatch::assess_memory(*reclaim, false, 0, 0).reason ==
                    "direct_reclaim",
                "direct reclaim not detected");
        require(graywatch::assess_memory(*reclaim, true, 30.0, 0).reason ==
                    "memory_stall_some",
                "PSI some does not take precedence");
        require(graywatch::assess_memory(*reclaim, true, 30.0, 15.0).condition ==
                    graywatch::MemoryCondition::degraded,
                "PSI full not degraded");
        require(graywatch::assess_memory(*reclaim, false, 30.0, 15.0).reason ==
                    "direct_reclaim",
                "unavailable PSI values were used");

        after.oom_kills += 1;
        reclaim = graywatch::calculate_memory_interval(before, after, 1.0);
        require(graywatch::assess_memory(*reclaim, true, 0, 0).reason == "oom_kill",
                "OOM kill not degraded");

        auto low = before;
        low.available_kib = 10000;
        require(graywatch::assess_memory(
                    *graywatch::calculate_memory_interval(before, low, 1.0), false,
                    0, 0).reason == "low_available",
                "low available memory not detected");

        auto reset = before;
        reset.oom_kills = 0;
        require(!graywatch::calculate_memory_interval(before, reset, 1.0),
                "counter reset accepted");
    } catch (const std::exception& error) {
        std::cerr << "memory_stats_test: " << error.what() << '\n';
        return 1;
    }
}
