#include "biolatency.h"
#include "biolatency.skel.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }

std::string operation_name(std::uint32_t operation) {
    switch (operation) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "flush";
        case 3: return "discard";
        default: return operation == 0xffffffff ? "error" : "other";
    }
}

std::string device_name(std::uint32_t major, std::uint32_t minor) {
    std::error_code error;
    const auto path = std::filesystem::canonical(
        "/sys/dev/block/" + std::to_string(major) + ":" + std::to_string(minor),
        error);
    return error ? "" : path.filename().string();
}

__u64 percentile_us(const __u64 *buckets, __u64 count, double fraction) {
    if (count == 0) return 0;
    const auto target = static_cast<__u64>(count * fraction + 0.999999);
    __u64 seen{};
    for (unsigned int index = 0; index < GRAYWATCH_LATENCY_BUCKETS; ++index) {
        seen += buckets[index];
        if (seen >= target) return __u64{1} << (index + 1);
    }
    return __u64{1} << GRAYWATCH_LATENCY_BUCKETS;
}

io_stats difference(const io_stats& current, const io_stats& previous) {
    io_stats result{};
    result.count = current.count - previous.count;
    result.bytes = current.bytes - previous.bytes;
    result.total_latency_ns = current.total_latency_ns - previous.total_latency_ns;
    result.errors = current.errors - previous.errors;
    for (unsigned int index = 0; index < GRAYWATCH_LATENCY_BUCKETS; ++index)
        result.buckets[index] = current.buckets[index] - previous.buckets[index];
    return result;
}
}  // namespace

int main(int argc, char **argv) {
    double interval = 1.0;
    unsigned int count{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--interval" && index + 1 < argc) interval = std::stod(argv[++index]);
        else if (argument == "--count" && index + 1 < argc)
            count = static_cast<unsigned int>(std::stoul(argv[++index]));
        else {
            std::cerr << "usage: graywatch-biolatency [--interval SECONDS] [--count N]\n";
            return 2;
        }
    }
    if (interval <= 0.0) return 2;
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
    biolatency_bpf *skeleton = biolatency_bpf__open_and_load();
    if (!skeleton) {
        std::cerr << "failed to open and load BPF program\n";
        return 1;
    }
    if (biolatency_bpf__attach(skeleton) != 0) {
        std::cerr << "failed to attach BPF program\n";
        biolatency_bpf__destroy(skeleton);
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const int map_fd = bpf_map__fd(skeleton->maps.statistics);
    std::map<std::pair<std::uint32_t, std::uint32_t>, io_stats> previous;
    unsigned int emitted{};
    while (running && (count == 0 || emitted < count)) {
        std::this_thread::sleep_for(std::chrono::duration<double>(interval));
        io_key key{};
        io_key next{};
        bool first = true;
        while (bpf_map_get_next_key(map_fd, first ? nullptr : &key, &next) == 0) {
            io_stats current{};
            if (bpf_map_lookup_elem(map_fd, &next, &current) == 0) {
                const auto identity = std::make_pair(next.dev, next.operation);
                const auto delta = difference(current, previous[identity]);
                previous[identity] = current;
                if (delta.count || delta.errors) {
                    const auto major = next.dev >> 20;
                    const auto minor = next.dev & ((1U << 20) - 1);
                    const double average_ms = delta.count == 0 ? 0.0 :
                        static_cast<double>(delta.total_latency_ns) / delta.count / 1e6;
                    std::cout << "{\"major\":" << major << ",\"minor\":" << minor
                              << ",\"device\":\"" << device_name(major, minor) << "\""
                              << ",\"operation\":\"" << operation_name(next.operation)
                              << "\",\"count\":" << delta.count
                              << ",\"bytes\":" << delta.bytes
                              << ",\"average_latency_ms\":" << average_ms
                              << ",\"p50_upper_us\":"
                              << percentile_us(delta.buckets, delta.count, 0.50)
                              << ",\"p95_upper_us\":"
                              << percentile_us(delta.buckets, delta.count, 0.95)
                              << ",\"p99_upper_us\":"
                              << percentile_us(delta.buckets, delta.count, 0.99)
                              << ",\"errors\":" << delta.errors << "}\n";
                }
            }
            key = next;
            first = false;
        }
        std::cout << std::flush;
        ++emitted;
    }
    biolatency_bpf__destroy(skeleton);
}
