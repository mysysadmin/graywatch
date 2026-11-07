#include "graywatch/report.hpp"

#include "graywatch/json.hpp"
#include "graywatch/process.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace graywatch::report {
namespace {

using json::Value;

constexpr std::size_t maximum_file_bytes = 512u * 1024u * 1024u;

__attribute__((format(printf, 1, 2))) std::string format(const char* pattern, ...) {
    va_list arguments;
    va_start(arguments, pattern);
    va_list copy;
    va_copy(copy, arguments);
    const int size = std::vsnprintf(nullptr, 0, pattern, copy);
    va_end(copy);
    std::string output(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (size > 0) std::vsnprintf(output.data(), output.size() + 1, pattern, arguments);
    va_end(arguments);
    return output;
}

std::string plural(std::size_t count, std::string_view word) {
    std::string result = std::to_string(count) + " " + std::string(word);
    if (count != 1) result += word.ends_with('s') ? "es" : "s";
    return result;
}

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::string output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output += separator;
        output += values[index];
    }
    return output;
}

std::string human_bytes(double value) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    std::size_t unit = 0;
    while (std::fabs(value) >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return format("%.1f %s", value, units[unit]);
}

std::string clock_of(const std::string& timestamp) {
    return timestamp.size() >= 19 ? timestamp.substr(11, 8) : "?";
}

std::string counts_text(const std::map<std::string, std::size_t>& counts) {
    std::vector<std::pair<std::string, std::size_t>> ordered(counts.begin(), counts.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> parts;
    for (const auto& [name, count] : ordered) parts.push_back(name + "=" + std::to_string(count));
    return join(parts, ", ");
}

// "2026-11-07T21:15:25.642Z" -> seconds since the epoch.
std::optional<double> parse_utc(const std::string& text) {
    std::tm parts{};
    int fraction_digits = 0;
    double fraction = 0.0;
    if (text.size() < 20 ||
        std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &parts.tm_year,
                    &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min,
                    &parts.tm_sec) != 6) {
        return std::nullopt;
    }
    if (text.size() > 20 && text[19] == '.') {
        for (std::size_t index = 20; index < text.size() && std::isdigit(
                 static_cast<unsigned char>(text[index])); ++index) {
            fraction += (text[index] - '0') * std::pow(10.0, -(++fraction_digits));
        }
    }
    parts.tm_year -= 1900;
    parts.tm_mon -= 1;
    return static_cast<double>(timegm(&parts)) + fraction;
}

// `dmesg --time-format iso` prefix, e.g. 2026-11-07T11:04:24,607624-08:00.
std::optional<double> kernel_line_time(const std::string& line) {
    static const std::regex pattern(
        R"(^\[?(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)[,.]?\d*\s*([+-])(\d\d):?(\d\d)?)");
    std::smatch match;
    if (!std::regex_search(line, match, pattern)) return std::nullopt;
    std::tm parts{};
    parts.tm_year = std::stoi(match[1]) - 1900;
    parts.tm_mon = std::stoi(match[2]) - 1;
    parts.tm_mday = std::stoi(match[3]);
    parts.tm_hour = std::stoi(match[4]);
    parts.tm_min = std::stoi(match[5]);
    parts.tm_sec = std::stoi(match[6]);
    const int sign = match[7] == "-" ? -1 : 1;
    const int offset = sign * (std::stoi(match[8]) * 3600 +
                               (match[9].matched ? std::stoi(match[9]) * 60 : 0));
    return static_cast<double>(timegm(&parts)) - offset;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::lround(fraction * static_cast<double>(values.size() - 1)));
    return values[std::min(index, values.size() - 1)];
}

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double total{};
    for (const double value : values) total += value;
    return total / static_cast<double>(values.size());
}

// Merged log2 histogram from bpf events: upper bound in microseconds -> count.
struct Histogram {
    std::map<double, double> buckets;
    double count{};
    double total_ms{};
    double max_ms{};

    void add(const Value& event) {
        count += event.number("count", 0.0);
        total_ms += event.number("total_ms", 0.0);
        max_ms = std::max(max_ms, event.number("max_ms", 0.0));
        for (const auto& [bound, value] : event["histogram_us"].members()) {
            const double upper = std::strtod(bound.c_str(), nullptr);
            if (upper > 0) buckets[upper] += value.number(0.0);
        }
    }

    // Upper bound (ms) of the bucket containing the requested quantile.
    [[nodiscard]] double quantile_ms(double fraction) const {
        double total{};
        for (const auto& [bound, value] : buckets) total += value;
        if (total <= 0.0) return 0.0;
        const double target = std::ceil(total * fraction);
        double seen{};
        for (const auto& [bound, value] : buckets) {
            seen += value;
            if (seen >= target) return bound / 1000.0;
        }
        return buckets.rbegin()->first / 1000.0;
    }

    [[nodiscard]] double average_ms() const { return count > 0 ? total_ms / count : 0.0; }
};

}  // namespace

std::string_view to_string(Severity severity) {
    switch (severity) {
        case Severity::critical: return "critical";
        case Severity::warning: return "warning";
        case Severity::info: return "info";
    }
    return "info";
}

namespace {

// Graywatch's own processes sample /proc and maps; they are not the workload.
bool own_process(const std::string& command) {
    return command.starts_with("graywatch");
}

const std::set<std::string> quiet_conditions{"healthy", "idle", "inactive",
                                             "no_pressure", "unavailable"};

bool quiet(const Value& event) {
    return quiet_conditions.contains(event.string("condition"));
}

struct WaitHint {
    std::vector<std::string> needles;
    std::string category;
    std::string meaning;
};

const std::vector<WaitHint>& wait_hints() {
    static const std::vector<WaitHint> hints{
        {{"request_wait_answer", "fuse_"}, "fuse",
         "FUSE request outstanding: the userspace filesystem daemon (or the storage "
         "service behind it) has not answered"},
        {{"rpc_wait_bit_killable", "nfs_", "rpc_"}, "nfs",
         "NFS/SunRPC call outstanding: the NFS server or network path is slow"},
        {{"io_schedule", "blk_mq_get_tag", "wait_on_page_bit", "folio_wait_bit",
          "bit_wait_io", "__lock_page", "wait_on_buffer", "blkdev_"}, "block",
         "waiting for block I/O or page writeback to complete"},
        {{"jbd2", "xlog", "xfs_", "ext4_", "btrfs_"}, "filesystem",
         "waiting on local filesystem journal or metadata"},
        {{"rwsem", "mutex", "down_read", "down_write", "lock"}, "lock",
         "waiting on a kernel lock held by another task"},
        {{"throttle_direct_reclaim", "balance_dirty_pages", "shrink_"}, "memory",
         "throttled by memory reclaim or dirty-page writeback"},
    };
    return hints;
}

std::pair<std::string, std::string> classify_wait_channel(const std::string& channel) {
    if (channel.empty()) {
        return {"hidden", "wait channel not visible (collect as root to see it)"};
    }
    for (const auto& hint : wait_hints()) {
        for (const auto& needle : hint.needles) {
            if (channel.find(needle) != std::string::npos) return {hint.category, hint.meaning};
        }
    }
    return {"other", "kernel wait channel " + channel};
}

struct KernelPattern {
    std::string name;
    Severity severity;
    std::regex pattern;
    std::string advice;
};

const std::vector<KernelPattern>& kernel_patterns() {
    using std::regex;
    constexpr auto icase = regex::ECMAScript | regex::icase | regex::optimize;
    constexpr auto exact = regex::ECMAScript | regex::optimize;
    static const std::vector<KernelPattern> patterns{
        {"storage error", Severity::warning,
         regex(R"(I/O error|blk_update_request|critical (medium|target) error|Buffer I/O error|end_request)", icase),
         "Block device errors were logged. Check SMART/NVMe health (`smartctl -a`, "
         "`nvme smart-log`) and the controller/HBA."},
        {"nvme timeout", Severity::warning,
         regex(R"(nvme.*(timeout|resetting controller|I/O \d+ QID))", icase),
         "NVMe command timeouts or controller resets. Check firmware, PCIe AER errors "
         "(`dmesg | grep -i aer`) and the drive's error log."},
        {"hung task", Severity::critical, regex(R"(blocked for more than \d+ seconds)", exact),
         "The kernel's hung-task detector fired. The stack printed after each report in "
         "kernel.log shows what the task was waiting on."},
        {"oom", Severity::critical, regex(R"(Out of memory|oom-kill|Killed process \d+)", icase),
         "The OOM killer terminated processes. Check which process was killed and its "
         "cgroup memory limit."},
        {"page allocation failure", Severity::warning,
         regex(R"(page allocation failure)", icase),
         "Kernel allocations failed (often fragmentation for high-order pages)."},
        {"lockup", Severity::critical,
         regex(R"(soft lockup|hard LOCKUP|rcu_(sched|preempt) detected stall|rcu: INFO)", icase),
         "CPU lockup or RCU stall: a CPU stopped scheduling. This often points at a "
         "driver or firmware problem."},
        {"link flap", Severity::warning,
         regex(R"(Link is (Down|Up)|link (down|up)|port .*(DOWN|ACTIVE)|mlx\d_core.*(link|port))", icase),
         "Network or InfiniBand link state changes were logged. Check cables, "
         "transceivers and the switch port."},
        {"filesystem error", Severity::critical,
         regex(R"(EXT4-fs error|XFS .*(error|shutdown|corrupt)|BTRFS.*(error|corrupt))", icase),
         "Filesystem errors were logged. The filesystem may be remounted read-only."},
        {"hardware error", Severity::critical,
         regex(R"(Machine Check|mce: \[Hardware Error\]|EDAC .*error)", icase),
         "Hardware errors (MCE/EDAC) were logged. Engage the hardware vendor."},
        {"segfault", Severity::info, regex(R"(segfault at|general protection fault|traps:)", exact),
         "Userspace processes crashed during the logged period."},
    };
    return patterns;
}

std::vector<std::string> psi_help(const std::string& status) {
    if (status == "disabled_by_default") {
        return {
            "This kernel was built with PSI but turned off by default "
            "(CONFIG_PSI_DEFAULT_DISABLED=y, the RHEL/Rocky/Alma default). Enable it with "
            "a kernel boot parameter and a reboot:",
            "  RHEL family:   grubby --update-kernel=ALL --args=psi=1",
            "  Debian/Ubuntu: add psi=1 to GRUB_CMDLINE_LINUX in /etc/default/grub, then "
            "update-grub",
            "  then reboot, and verify with: cat /proc/pressure/cpu",
            "PSI cannot be enabled at runtime. Its overhead is small, but schedule the "
            "reboot through the customer's change process.",
        };
    }
    if (status == "disabled_by_cmdline") {
        return {"PSI was explicitly disabled with psi=0 on the kernel command line. Remove it "
                "(RHEL family: grubby --update-kernel=ALL --remove-args=psi) and reboot."};
    }
    if (status == "not_built") {
        return {"This kernel was built without CONFIG_PSI (it needs Linux 4.20 or later with "
                "CONFIG_PSI=y). Only a different kernel build can provide PSI."};
    }
    return {"/proc/pressure is absent and the kernel configuration was not readable. If the "
            "kernel is 4.20 or later, try booting with psi=1."};
}

struct Episode {
    std::string start;
    std::size_t count{};
    const Value* peak{};
};

std::vector<Episode> episodes(const std::vector<const Value*>& events,
                              const std::function<bool(const Value&)>& bad) {
    std::vector<Episode> runs;
    std::optional<Episode> current;
    for (const auto* event : events) {
        if (bad(*event)) {
            if (!current) current = Episode{event->string("timestamp"), 0, event};
            ++current->count;
            if (event->number("severity", 0.0) > current->peak->number("severity", 0.0)) {
                current->peak = event;
            }
        } else if (current) {
            runs.push_back(*current);
            current.reset();
        }
    }
    if (current) runs.push_back(*current);
    return runs;
}

struct DeviceInfo {
    std::string name;
    std::string type;
    std::set<std::string> backing;
};

std::map<std::string, DeviceInfo> block_topology(const std::string& text) {
    std::map<std::string, DeviceInfo> info;
    if (text.empty()) return info;
    Value tree;
    try {
        tree = json::parse(text);
    } catch (const std::exception&) {
        return info;
    }
    std::function<void(const Value&, std::vector<std::string>&)> visit =
        [&](const Value& node, std::vector<std::string>& ancestors) {
            std::string kname = node.string("kname");
            if (kname.empty()) kname = node.string("name");
            auto& entry = info[kname];
            entry.name = node.string("name");
            entry.type = node.string("type");
            // lsblk prints stacked devices as children of the devices they use,
            // so the root of each path is a base device this one is built on.
            if (!ancestors.empty()) entry.backing.insert(ancestors.front());
            ancestors.push_back(kname);
            for (const auto& child : node["children"].array()) visit(child, ancestors);
            ancestors.pop_back();
        };
    for (const auto& root : tree["blockdevices"].array()) {
        std::vector<std::string> ancestors;
        visit(root, ancestors);
    }
    return info;
}

class Analysis {
  public:
    explicit Analysis(const Files& files) : files_(files) {}

    Report run() {
        parse_inputs();
        check_coverage();
        check_cpu();
        check_memory();
        check_io();
        check_network();
        check_bpf();
        check_kernel_log();
        correlate();
        std::stable_sort(report_.findings.begin(), report_.findings.end(),
                         [](const Finding& a, const Finding& b) {
                             return static_cast<int>(a.severity) < static_cast<int>(b.severity);
                         });
        std::stable_sort(report_.timeline.begin(), report_.timeline.end(),
                         [](const auto& a, const auto& b) { return a.start < b.start; });
        return std::move(report_);
    }

  private:
    const std::string& file(const std::string& name) const {
        static const std::string empty;
        const auto found = files_.find(name);
        return found == files_.end() ? empty : found->second;
    }

    void add(Severity severity, std::string area, std::string title,
             std::vector<std::string> details, std::vector<std::string> steps) {
        report_.findings.push_back({severity, std::move(area), std::move(title),
                                    std::move(details), std::move(steps)});
    }

    void timeline(const std::string& start, std::string area, std::string text,
                  std::size_t count, double severity, std::string unit = "interval") {
        report_.timeline.push_back({start, std::move(area), std::move(text), count,
                                    std::move(unit), severity});
    }

    static std::vector<Value> parse_lines(const std::string& text, std::size_t& invalid) {
        std::vector<Value> values;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            try {
                auto value = json::parse(line);
                if (value.is_object()) values.push_back(std::move(value));
            } catch (const std::exception&) {
                ++invalid;
            }
        }
        return values;
    }

    void parse_inputs() {
        std::istringstream system(file("system.txt"));
        std::string line;
        while (std::getline(system, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos || separator == 0) continue;
            const std::string key = line.substr(0, separator);
            if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_") !=
                std::string::npos) {
                continue;
            }
            std::string value = line.substr(separator + 1);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            report_.system[key] = value;
        }
        events_ = parse_lines(file("events.jsonl"), invalid_lines_);
        for (const auto& event : events_) by_resource_[event.string("resource")].push_back(&event);
        bpf_events_ = parse_lines(file("bpf-events.jsonl"), invalid_bpf_lines_);
        for (const auto& event : bpf_events_) {
            if (event.string("event_type") == "bpf_status") bpf_status_ = &event;
            else bpf_by_collector_[event.string("collector")].push_back(&event);
        }
        topology_ = block_topology(file("lsblk.json"));
    }

    std::string system(const std::string& key, const std::string& fallback = "") const {
        const auto found = report_.system.find(key);
        return found == report_.system.end() ? fallback : found->second;
    }

    // -- coverage ----------------------------------------------------------

    void check_coverage() {
        const auto& cpu = by_resource_["cpu"];
        std::vector<double> times;
        for (const auto* event : cpu) {
            if (const auto time = parse_utc(event->string("timestamp"))) times.push_back(*time);
        }
        if (!times.empty()) {
            window_start_ = *std::min_element(times.begin(), times.end());
            report_.window_start = cpu.front()->string("timestamp");
            report_.window_end = cpu.back()->string("timestamp");
        }
        report_.intervals = cpu.size();
        const long requested = std::strtol(system("duration_seconds", "0").c_str(), nullptr, 10);
        if (events_.empty()) {
            report_.coverage.push_back("No interval events were recorded; the daemon may "
                                       "have failed. See daemon.stderr.");
        } else if (requested > 0 && static_cast<double>(cpu.size()) < requested * 0.8) {
            report_.coverage.push_back(format(
                "Only %zu one-second CPU intervals were recorded for a %lds window; sampling "
                "had gaps or the daemon stopped early.", cpu.size(), requested));
        }
        if (invalid_lines_) {
            report_.coverage.push_back(plural(invalid_lines_, "line") +
                                       " in events.jsonl could not be parsed.");
        }
        const std::string uid = system("uid");
        if (!uid.empty() && uid != "0") {
            report_.coverage.push_back("Collected without root (uid " + uid +
                                       "): threads of other users and their wait channels "
                                       "may be missing, and eBPF collectors cannot run.");
        }
        if (system("proc_mount").find("hidepid") != std::string::npos) {
            report_.coverage.push_back("/proc is mounted with hidepid; task attribution only "
                                       "covers visible processes.");
        }
        for (const std::string name : {"daemon.stderr", "collection-errors.txt", "bpf.stderr"}) {
            std::istringstream text(file(name));
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(text, line) && lines.size() < 5) {
                if (!line.empty()) lines.push_back(line);
            }
            if (!lines.empty()) report_.coverage.push_back(name + ": " + join(lines, " | "));
        }
        if (!system("ignored_ib_ports").empty()) {
            report_.coverage.push_back("InfiniBand ports excluded on request: " +
                                       system("ignored_ib_ports"));
        }

        bool psi_seen = false;
        for (const auto* event : cpu) psi_seen |= (*event)["evidence"].boolean("psi_available", false);
        std::string psi = system("psi_status");
        if (psi.empty()) psi = psi_seen ? "enabled" : "unknown";
        report_.psi_status = psi;
        if (psi != "enabled" && !psi_seen) {
            add(Severity::info, "coverage", "PSI (pressure stall information) is not available",
                {"Without PSI, CPU events report condition=unavailable. Memory health falls "
                 "back to reclaim, swap and OOM counters, and I/O health to block statistics; "
                 "both still work. System-wide stall percentages for CPU, memory and I/O are "
                 "missing."},
                psi_help(psi));
        }
    }

    // -- CPU / tasks ---------------------------------------------------------

    void check_cpu() {
        const auto& cpu = by_resource_["cpu"];
        if (cpu.empty()) return;
        std::vector<double> stalls;
        for (const auto* event : cpu) {
            const auto& evidence = (*event)["evidence"];
            if (evidence.boolean("psi_available", false)) {
                stalls.push_back(evidence.number("psi_some_stall_percent", 0.0));
            }
        }
        if (!stalls.empty()) {
            psi_cpu_ = true;
            const double average = mean(stalls);
            const double p95 = percentile(stalls, 0.95);
            const double maximum = *std::max_element(stalls.begin(), stalls.end());
            const auto heavy = static_cast<std::size_t>(std::count_if(
                stalls.begin(), stalls.end(), [](double value) { return value >= 25.0; }));
            report_.overview.push_back(format("CPU PSI some: mean %.1f%%, p95 %.1f%%, max %.1f%%.",
                                              average, p95, maximum));
            for (const auto& run : episodes(cpu, [](const Value& e) {
                     return e.number("severity", 0.0) >= 0.25;
                 })) {
                timeline(run.start, "cpu", "CPU stall >= 25%", run.count,
                         run.peak->number("severity", 0.0));
            }
            if (p95 >= 25.0 || heavy >= std::max<std::size_t>(3, stalls.size() / 10)) {
                add(average >= 50.0 ? Severity::critical : Severity::warning, "cpu",
                    "CPU contention: runnable work waited for CPU",
                    {format("PSI cpu some: mean %.1f%%, p95 %.1f%%, max %.1f%% of wall time with "
                            "at least one task waiting for a CPU.", average, p95, maximum),
                     format("%zu of %zu intervals were at or above 25%%.", heavy, stalls.size())},
                    {"Identify the CPU consumers listed under the top waiting threads and check "
                     "whether the node is oversubscribed or a cgroup has a CPU quota (cpu.max).",
                     "Correlate with application latency for the same timestamps."});
            }
        } else {
            report_.overview.push_back("CPU PSI: " + report_.psi_status + ".");
        }

        // Attribution snapshots repeat for several intervals; keep fresh ones.
        std::vector<const Value*> fresh;
        for (const auto* event : cpu) {
            const auto& selection = (*event)["selection"];
            if (selection.boolean("attribution_available", false) &&
                selection.number("attribution_age_seconds", 1.0) < 1e-6) {
                fresh.push_back(event);
            }
        }
        if (fresh.empty()) return;
        double interval{};
        double observed{};
        for (const auto* event : fresh) {
            interval += (*event)["selection"].number("attribution_interval_seconds", 5.0);
            observed = std::max(observed, (*event)["selection"].number("observed_threads", 0.0));
        }
        interval /= static_cast<double>(fresh.size());
        report_.overview.push_back(format("Task attribution: %zu snapshots, up to %.0f readable "
                                          "threads.", fresh.size(), observed));

        struct Blocked {
            std::size_t snapshots{};
            std::set<long> pids;
            std::string first;
            std::string last;
        };
        struct Waiter {
            double wait_ms{};
            double cpu_ms{};
            double worst_percent{};
            std::set<long> pids;
        };
        using Key = std::pair<std::string, std::string>;
        std::map<Key, Blocked> blocked;
        std::map<std::pair<long, Key>, std::vector<std::size_t>> series;
        std::map<std::string, Waiter> waits;
        for (std::size_t index = 0; index < fresh.size(); ++index) {
            const auto& event = *fresh[index];
            std::set<Key> seen;
            for (const auto& thread : event["affected_threads"].array()) {
                const std::string command = thread.string("command");
                if (own_process(command)) continue;
                const auto pid = static_cast<long>(thread.number("pid", 0.0));
                if (thread.string("state") == "D") {
                    const Key key{command, thread.string("wait_channel")};
                    auto& entry = blocked[key];
                    if (seen.insert(key).second) ++entry.snapshots;
                    entry.pids.insert(pid);
                    if (entry.first.empty()) entry.first = event.string("timestamp");
                    entry.last = event.string("timestamp");
                    series[{static_cast<long>(thread.number("tid", 0.0)), key}].push_back(index);
                }
                auto& waiter = waits[command];
                waiter.wait_ms += thread.number("runqueue_wait_ms", 0.0);
                waiter.cpu_ms += thread.number("cpu_ms", 0.0);
                waiter.worst_percent = std::max(waiter.worst_percent,
                                                thread.number("runnable_wait_percent", 0.0));
                waiter.pids.insert(pid);
            }
        }

        // Longest run of consecutive snapshots in which one thread stayed in D.
        std::map<Key, std::size_t> longest;
        for (const auto& [identity, indexes] : series) {
            std::size_t best = 1;
            std::size_t run = 1;
            for (std::size_t index = 1; index < indexes.size(); ++index) {
                run = indexes[index] == indexes[index - 1] + 1 ? run + 1 : 1;
                best = std::max(best, run);
            }
            longest[identity.second] = std::max(longest[identity.second], best);
        }

        if (!blocked.empty()) {
            std::vector<std::pair<Key, Blocked>> ordered(blocked.begin(), blocked.end());
            std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return a.second.snapshots > b.second.snapshots;
            });
            std::vector<std::string> details;
            double worst_stuck{};
            for (std::size_t index = 0; index < ordered.size() && index < 10; ++index) {
                const auto& [key, entry] = ordered[index];
                const auto [category, meaning] = classify_wait_channel(key.second);
                blocked_categories_[category] += entry.snapshots;
                const double stuck = static_cast<double>(longest[key]) * interval;
                worst_stuck = std::max(worst_stuck, stuck);
                details.push_back(format(
                    "%s (%s) in D state on '%s' in %zu of %zu snapshots between %s and %s; one "
                    "thread stayed blocked for at least %.0fs. Meaning: %s.",
                    key.first.c_str(), plural(entry.pids.size(), "process").c_str(),
                    key.second.empty() ? "?" : key.second.c_str(), entry.snapshots, fresh.size(),
                    clock_of(entry.first).c_str(), clock_of(entry.last).c_str(), stuck,
                    meaning.c_str()));
                if (index < 5) {
                    timeline(entry.first, "tasks",
                             key.first + " blocked on " + (key.second.empty() ? "?" : key.second),
                             entry.snapshots, -1.0, "snapshot");
                }
            }
            std::vector<std::string> steps;
            if (blocked_categories_.contains("fuse")) {
                steps.push_back("FUSE waits: check the FUSE client daemon for the affected mount "
                                "(its logs, CPU and connectivity to its storage backend). See "
                                "mounts.txt for fuse mounts and the eBPF FUSE findings for the "
                                "slow operations.");
            }
            if (blocked_categories_.contains("nfs")) {
                steps.push_back("NFS waits: check `nfsstat -c`, `/proc/self/mountstats` and the "
                                "server's responsiveness.");
            }
            if (blocked_categories_.contains("block")) {
                steps.push_back("Block I/O waits: compare with the I/O device findings for the "
                                "same time window.");
            }
            if (blocked_categories_.contains("lock")) {
                steps.push_back("Lock waits: the eBPF off-CPU stacks show the code path; capture "
                                "`cat /proc/PID/task/TID/stack` as root to find the lock holder.");
            }
            if (blocked_categories_.contains("hidden")) {
                steps.push_back("Re-run the collection as root to reveal hidden wait channels.");
            }
            steps.push_back("For any thread stuck for minutes, capture `echo w > "
                            "/proc/sysrq-trigger` (as root) to log all blocked task stacks.");
            add(worst_stuck >= 15 ? Severity::critical
                                  : (worst_stuck >= 2 * interval ? Severity::warning : Severity::info),
                "cpu", "Threads in uninterruptible sleep (D state)", std::move(details),
                std::move(steps));
        }

        std::vector<std::pair<std::string, Waiter>> ranked(waits.begin(), waits.end());
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.second.wait_ms > b.second.wait_ms;
        });
        std::vector<std::string> starved;
        for (std::size_t index = 0; index < ranked.size() && index < 8; ++index) {
            const auto& [command, waiter] = ranked[index];
            if (waiter.wait_ms <= 0) continue;
            if (index < 5) {
                report_.overview.push_back(format(
                    "  Top CPU waiter %s: %.0f ms run-queue wait, %.0f ms on CPU, worst %.0f%% of "
                    "runnable time waiting (%s).", command.c_str(), waiter.wait_ms, waiter.cpu_ms,
                    waiter.worst_percent, plural(waiter.pids.size(), "process").c_str()));
            }
            if (waiter.worst_percent >= 50.0 &&
                waiter.wait_ms >= 100.0 * static_cast<double>(fresh.size())) {
                starved.push_back(format("%s waited %.0f ms for CPU in total; in its worst snapshot "
                                         "a thread spent %.0f%% of its runnable time waiting.",
                                         command.c_str(), waiter.wait_ms, waiter.worst_percent));
            }
        }
        if (!starved.empty() && !psi_cpu_) {
            add(Severity::warning, "cpu", "CPU starvation in task attribution (PSI unavailable)",
                std::move(starved),
                {"Check CPU utilisation (`mpstat -P ALL 1`) and cgroup CPU limits for these "
                 "processes; the eBPF run-queue findings show which cgroups waited."});
        }
    }

    // -- memory ------------------------------------------------------------

    void check_memory() {
        const auto& memory = by_resource_["memory"];
        if (memory.empty()) {
            report_.coverage.push_back("No memory events (archive from an older Graywatch "
                                       "version).");
            return;
        }
        double oom{}, reclaim{}, compaction{}, swap_in{}, major{};
        double min_available = 100.0;
        double psi_some_max{}, psi_full_max{};
        bool psi = false;
        std::map<std::string, std::size_t> reasons;
        std::size_t bad = 0;
        for (const auto* event : memory) {
            const auto& evidence = (*event)["evidence"];
            const double seconds = event->number("interval_seconds", 1.0);
            oom += evidence.number("oom_kills", 0.0);
            reclaim += evidence.number("direct_reclaim_stalls", 0.0);
            compaction += evidence.number("compaction_stalls", 0.0);
            swap_in += evidence.number("swap_in_pages_per_second", 0.0) * seconds;
            major += evidence.number("major_faults_per_second", 0.0) * seconds;
            min_available = std::min(min_available, evidence.number("available_percent", 100.0));
            if (evidence.boolean("psi_available", false)) {
                psi = true;
                psi_some_max = std::max(psi_some_max, evidence.number("psi_some_stall_percent", 0.0));
                psi_full_max = std::max(psi_full_max, evidence.number("psi_full_stall_percent", 0.0));
            }
            if (!quiet(*event)) {
                ++bad;
                ++reasons[evidence.string("assessment_reason")];
            }
        }
        direct_reclaim_ = reclaim;
        const double total = memory.back()->operator[]("evidence").number("total_kib", 0.0) * 1024.0;
        report_.overview.push_back(format(
            "Memory %s: lowest available %.1f%%, %.0f direct reclaim stalls, %.0f swap-in pages, "
            "%.0f OOM kills.", human_bytes(total).c_str(), min_available, reclaim, swap_in, oom));
        for (const auto& run : episodes(memory, [](const Value& e) { return !quiet(e); })) {
            timeline(run.start, "memory",
                     "memory " + run.peak->string("condition") + " (" +
                         (*run.peak)["evidence"].string("assessment_reason") + ")",
                     run.count, run.peak->number("severity", 0.0));
        }
        std::vector<std::string> details{
            format("%s memory; lowest MemAvailable during the window: %.1f%%.",
                   human_bytes(total).c_str(), min_available)};
        if (psi) {
            details.push_back(format("PSI memory some: max %.1f%%, full: max %.1f%%.",
                                     psi_some_max, psi_full_max));
        }
        details.push_back(format("Direct reclaim stalls: %.0f, compaction stalls: %.0f, swap-in "
                                 "pages: %.0f, major faults: %.0f, OOM kills: %.0f.",
                                 reclaim, compaction, swap_in, major, oom));
        if (oom > 0) {
            add(Severity::critical, "memory", "OOM killer ran during the window", std::move(details),
                {"Find the killed process in kernel.log ('Killed process') and the cgroup that hit "
                 "its limit ('oom-kill:' line).",
                 "Check memory.max / memory.high of the affected cgroup and the workload's "
                 "working-set size."});
        } else if (bad > 0) {
            details.push_back("Reasons: " + counts_text(reasons));
            add(bad >= 3 || reasons.contains("memory_stall_full") ? Severity::warning : Severity::info,
                "memory", format("Memory pressure in %zu of %zu intervals", bad, memory.size()),
                std::move(details),
                {"Direct reclaim means allocating tasks (possibly the application) had to free "
                 "memory themselves, which adds latency. The eBPF reclaim findings name the "
                 "processes. Check for page-cache-heavy jobs and large dirty-page backlogs "
                 "(Dirty/Writeback in meminfo-*.txt).",
                 "If swap-in is non-zero, the working set does not fit in memory: identify the "
                 "largest consumers (`ps aux --sort=-rss | head`)."});
        }
    }

    // -- block I/O -----------------------------------------------------------

    std::string device_label(const std::string& name) const {
        const auto found = topology_.find(name);
        if (found == topology_.end()) return name;
        std::string label = name;
        if (!found->second.name.empty() && found->second.name != name) {
            label += " (" + found->second.name + ")";
        }
        std::vector<std::string> backing;
        for (const auto& device : found->second.backing) {
            if (device != name) backing.push_back(device);
        }
        if (!backing.empty()) label += " on " + join(backing, ", ");
        return label;
    }

    void check_io() {
        std::map<std::string, std::vector<const Value*>> devices;
        for (const auto* event : by_resource_["io"]) {
            devices[(*event)["device"].string("name")].push_back(event);
        }
        struct Row {
            std::vector<const Value*> bad;
            std::size_t active{};
            double p50{}, p95{}, maximum{}, busy{}, queue{};
        };
        std::map<std::string, Row> unhealthy;
        std::vector<std::string> idle;
        for (const auto& [name, events] : devices) {
            Row row;
            std::vector<double> latency;
            double read{}, written{};
            for (const auto* event : events) {
                if (event->string("condition") == "idle") continue;
                ++row.active;
                const auto& evidence = (*event)["evidence"];
                const double seconds = event->number("interval_seconds", 1.0);
                latency.push_back(evidence.number("maximum_average_latency_ms", 0.0));
                row.busy = std::max(row.busy, evidence.number("busy_percent", 0.0));
                row.queue = std::max(row.queue, evidence.number("average_queue_depth", 0.0));
                read += evidence.number("read_bytes_per_second", 0.0) * seconds;
                written += evidence.number("write_bytes_per_second", 0.0) * seconds;
                if (!quiet(*event)) row.bad.push_back(event);
            }
            if (row.active == 0) {
                idle.push_back(name);
                continue;
            }
            row.p50 = percentile(latency, 0.50);
            row.p95 = percentile(latency, 0.95);
            row.maximum = *std::max_element(latency.begin(), latency.end());
            report_.overview.push_back(format(
                "Disk %s: %zu active intervals, latency p50 %.2f / p95 %.2f / max %.2f ms, busy "
                "max %.0f%%, read %s, written %s.", device_label(name).c_str(), row.active,
                row.p50, row.p95, row.maximum, row.busy, human_bytes(read).c_str(),
                human_bytes(written).c_str()));
            for (const auto& run : episodes(events, [](const Value& e) { return !quiet(e); })) {
                timeline(run.start, "io",
                         name + " " + run.peak->string("condition") +
                             format(" (%.1f ms)", (*run.peak)["evidence"].number(
                                                      "maximum_average_latency_ms", 0.0)),
                         run.count, run.peak->number("severity", 0.0));
            }
            if (!row.bad.empty()) unhealthy[name] = std::move(row);
        }
        if (!idle.empty()) report_.overview.push_back("Idle devices: " + join(idle, ", ") + ".");

        std::vector<std::pair<std::string, const Row*>> ordered;
        for (const auto& [name, row] : unhealthy) {
            ordered.emplace_back(name, &row);
            unhealthy_io_.insert(name);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.second->bad.size() > b.second->bad.size();
        });
        for (const auto& [name, row] : ordered) {
            std::map<std::string, std::size_t> conditions;
            std::map<std::string, std::size_t> reasons;
            for (const auto* event : row->bad) {
                ++conditions[event->string("condition")];
                ++reasons[(*event)["evidence"].string("assessment_reason")];
            }
            const std::size_t degraded = conditions["degraded"];
            if (degraded == 0) conditions.erase("degraded");
            std::vector<std::string> steps;
            const auto found = topology_.find(name);
            if (found != topology_.end()) {
                std::vector<std::string> backing;
                bool all_healthy = true;
                for (const auto& device : found->second.backing) {
                    if (device == name) continue;
                    backing.push_back(device);
                    all_healthy &= !unhealthy.contains(device);
                }
                if (!backing.empty() && all_healthy) {
                    steps.push_back("The underlying device(s) " + join(backing, ", ") +
                                    " stayed healthy, so the delay is added in the stacking layer "
                                    "(device-mapper, LVM, multipath, dm-crypt, RAID). Inspect "
                                    "`dmsetup table` and `dmsetup status`.");
                } else if (!backing.empty()) {
                    steps.push_back("Backing device(s) " + join(backing, ", ") +
                                    " were also unhealthy; start with the physical device.");
                }
            }
            if (reasons.contains("busy_with_queue") || reasons.contains("queueing")) {
                steps.push_back("Queueing with a busy device means demand exceeds the device's "
                                "capacity. Identify the I/O sources (`iotop -o`, `pidstat -d 1`) "
                                "before suspecting hardware.");
            }
            if (reasons.contains("absolute_latency") || reasons.contains("elevated_latency")) {
                steps.push_back("High latency: check the device's health (`smartctl -a`, `nvme "
                                "smart-log`) and kernel.log storage errors. The eBPF block "
                                "findings separate queueing from device service time.");
            }
            std::vector<std::string> names;
            for (const auto& [condition, count] : conditions) names.push_back(condition);
            add(degraded >= 3 ? Severity::critical
                              : (degraded > 0 || row->bad.size() >= 3 ? Severity::warning
                                                                       : Severity::info),
                "io", "Block device " + device_label(name) + ": " + join(names, ", "),
                {format("%zu of %zu active intervals non-healthy: %s.", row->bad.size(),
                        row->active, counts_text(conditions).c_str()),
                 format("Average latency per interval: p50 %.2f ms, p95 %.2f ms, max %.2f ms. Max "
                        "busy %.0f%%, max average queue depth %.1f.", row->p50, row->p95,
                        row->maximum, row->busy, row->queue),
                 "Reasons: " + counts_text(reasons)},
                std::move(steps));
        }
    }

    // -- network -------------------------------------------------------------

    void check_network() {
        std::vector<const Value*> tcp;
        std::map<std::string, std::vector<const Value*>> ports;
        for (const auto* event : by_resource_["network"]) {
            const auto& interface = (*event)["interface"];
            if (interface.string("kind") == "tcp") {
                tcp.push_back(event);
            } else if (interface.string("kind") == "infiniband") {
                ports[interface.string("device") + "/" +
                      std::to_string(static_cast<long>(interface.number("port", 0.0)))]
                    .push_back(event);
            }
        }
        if (!tcp.empty()) {
            double segments{}, retransmits{}, errors{}, resets{};
            std::size_t bad = 0;
            for (const auto* event : tcp) {
                const auto& evidence = (*event)["evidence"];
                segments += evidence.number("out_segments", 0.0);
                retransmits += evidence.number("retransmitted_segments", 0.0);
                errors += evidence.number("input_errors", 0.0);
                resets += evidence.number("output_resets", 0.0);
                bad += quiet(*event) ? 0 : 1;
            }
            const double ratio = segments > 0 ? retransmits / segments * 100.0 : 0.0;
            report_.overview.push_back(format("TCP: %.0f segments out, %.2f%% retransmitted, %.0f "
                                              "input errors.", segments, ratio, errors));
            for (const auto& run : episodes(tcp, [](const Value& e) { return !quiet(e); })) {
                timeline(run.start, "tcp", "TCP " + run.peak->string("condition"), run.count,
                         run.peak->number("severity", 0.0));
            }
            if (bad > 0 || errors > 0) {
                tcp_bad_ = true;
                add(bad >= 3 || errors > 0 ? Severity::warning : Severity::info, "network",
                    format("TCP retransmissions or errors in %zu of %zu intervals", bad, tcp.size()),
                    {format("%.0f of %.0f segments retransmitted (%.2f%%), %.0f input errors, %.0f "
                            "resets sent.", retransmits, segments, ratio, errors, resets),
                     "These counters are system-wide; the eBPF TCP findings name the connections."},
                    {"Inspect the affected connections with `ss -ti dst PEER` (retrans, rto) and "
                     "check NIC counters with `ip -s link` and `ethtool -S IFACE`.",
                     "Input errors usually mean checksum or malformed segments: check the NIC, "
                     "cabling and MTU consistency."});
            }
        }

        std::vector<std::string> inactive;
        for (const auto& [name, events] : ports) {
            std::map<std::string, std::size_t> conditions;
            std::vector<std::string> states;
            std::map<std::string, double> totals;
            for (const auto* event : events) {
                ++conditions[event->string("condition")];
                const auto& evidence = (*event)["evidence"];
                const std::string state = evidence.string("state");
                if (states.empty() || states.back() != state) states.push_back(state);
                for (const std::string key : {"receive_errors", "transmit_discards",
                                              "symbol_errors", "link_downed", "transmit_wait"}) {
                    totals[key] += evidence.number(key, 0.0);
                }
            }
            const std::string rate = (*events.back())["evidence"].string("rate");
            std::vector<std::string> condition_parts;
            for (const auto& [condition, count] : conditions) {
                condition_parts.push_back(condition + "=" + std::to_string(count));
            }
            report_.overview.push_back("InfiniBand " + name + " (" + rate + "): " +
                                       join(condition_parts, ", ") + ".");
            for (const auto& run : episodes(events, [](const Value& e) { return !quiet(e); })) {
                timeline(run.start, "infiniband", name + " " + run.peak->string("condition"),
                         run.count, run.peak->number("severity", 0.0));
            }
            if (conditions.size() == 1 && conditions.contains("inactive")) {
                inactive.push_back(name);
                continue;
            }
            std::vector<std::string> details{"States during window: " + join(states, " -> ") +
                                             "; rate " + rate + "."};
            std::vector<std::string> counters;
            for (const auto& [key, value] : totals) {
                if (value > 0) counters.push_back(format("%s=%.0f", key.c_str(), value));
            }
            if (!counters.empty()) details.push_back("Counter increments: " + join(counters, ", ") + ".");
            if (conditions.contains("link_down")) {
                network_bad_ = true;
                add(Severity::critical, "network", "InfiniBand port " + name + " link down",
                    std::move(details),
                    {"The port carried traffic before (or flapped) and is now down. Check "
                     "`ibstat`, the cable/transceiver and the switch port. If the port is "
                     "intentionally unused, rerun with --ignore-ib-port " + name + "."});
            } else if (conditions.contains("degraded")) {
                network_bad_ = true;
                add(Severity::warning, "network", "InfiniBand port " + name + " errors",
                    std::move(details),
                    {"Symbol and receive errors point at the physical link (cable, transceiver, "
                     "port). Check `perfquery -x` on both ends and the switch-side counters."});
            } else if (conditions.contains("congested")) {
                const std::size_t busy = conditions["congested"];
                // A handful of wait ticks is normal flow control, not congestion.
                const bool significant = busy >= 3 && totals["transmit_wait"] >= 1000;
                network_bad_ |= significant;
                add(significant ? Severity::warning : Severity::info, "network",
                    format("InfiniBand port %s congested in %zu intervals", name.c_str(), busy),
                    std::move(details),
                    {"port_xmit_wait means the port had data to send but no credits from the "
                     "link partner: congestion in the fabric or a slow receiver. Check the switch "
                     "for hotspots."});
            }
        }
        if (!inactive.empty()) {
            add(Severity::info, "network", "Unused InfiniBand ports ignored",
                {join(inactive, ", ") + " down for the whole window and never carried traffic "
                 "since boot; not treated as a failure."},
                {"If the application is configured to use one of these ports, that "
                 "configuration (or the cabling) is the problem."});
        }
    }

    // -- eBPF ----------------------------------------------------------------

    static std::string stack_text(const Value& event, std::size_t frames = 6) {
        std::vector<std::string> names;
        for (const auto& frame : event["stack"].array()) {
            if (names.size() >= frames) break;
            names.push_back(frame.string());
        }
        return names.empty() ? "(no stack)" : join(names, " < ");
    }

    void check_bpf() {
        if (!bpf_status_) {
            if (files_.contains("bpf-events.jsonl") || files_.contains("bpf.stderr")) {
                report_.coverage.push_back("The eBPF helper produced no status line; see "
                                           "bpf.stderr.");
            } else {
                report_.coverage.push_back(
                    "No eBPF data (collected without root, without kernel BTF, with --no-bpf, "
                    "or by a bundle without graywatch-bpf). FUSE latency, queue/service split, "
                    "blocked-task stacks, per-cgroup run-queue latency, per-connection "
                    "retransmits and reclaim attribution are missing.");
            }
            return;
        }
        std::vector<std::string> disabled;
        for (const auto& collector : (*bpf_status_)["collectors"].array()) {
            if (!collector.boolean("enabled", false)) {
                disabled.push_back(collector.string("name") + " (" + collector.string("reason") + ")");
            }
        }
        if (!disabled.empty()) {
            report_.coverage.push_back("eBPF collectors not running: " + join(disabled, "; ") + ".");
        }
        if (invalid_bpf_lines_) {
            report_.coverage.push_back(plural(invalid_bpf_lines_, "line") +
                                       " in bpf-events.jsonl could not be parsed.");
        }
        check_bpf_fuse();
        check_bpf_block();
        check_bpf_offcpu();
        check_bpf_runqueue();
        check_bpf_tcp();
        check_bpf_reclaim();
    }

    void check_bpf_fuse() {
        struct Op {
            Histogram histogram;
            double errors{};
            double oldest_ms{};
            std::string oldest_at;
        };
        std::map<std::pair<std::string, std::string>, Op> operations;
        for (const auto* event : bpf_by_collector_["fuse"]) {
            std::string mount = event->string("mount");
            if (mount.empty()) mount = event->string("device");
            auto& op = operations[{mount, event->string("opcode")}];
            op.histogram.add(*event);
            op.errors += event->number("errors", 0.0);
            const double oldest = event->number("oldest_outstanding_ms", 0.0);
            if (oldest > op.oldest_ms) {
                op.oldest_ms = oldest;
                op.oldest_at = event->string("timestamp");
            }
        }
        if (operations.empty()) return;
        std::vector<std::string> slow;
        std::vector<std::string> stuck;
        std::vector<std::string> summary;
        double worst_p99{};
        double worst_stuck{};
        for (const auto& [key, op] : operations) {
            const auto& [mount, opcode] = key;
            const double p99 = op.histogram.quantile_ms(0.99);
            const std::string line = format(
                "%s %s: %.0f requests, average %.2f ms, p99 <= %.1f ms, max %.1f ms, %.0f errors.",
                mount.c_str(), opcode.c_str(), op.histogram.count, op.histogram.average_ms(), p99,
                op.histogram.max_ms, op.errors);
            if (op.histogram.count > 0) summary.push_back(line);
            if (op.histogram.count >= 1 && (p99 >= 100.0 || op.histogram.max_ms >= 1000.0)) {
                slow.push_back(line);
                worst_p99 = std::max(worst_p99, std::max(p99, op.histogram.max_ms));
            }
            if (op.oldest_ms >= 5000.0) {
                stuck.push_back(format("%s %s request outstanding for %.1f s at %s.",
                                       mount.c_str(), opcode.c_str(), op.oldest_ms / 1000.0,
                                       clock_of(op.oldest_at).c_str()));
                worst_stuck = std::max(worst_stuck, op.oldest_ms);
                timeline(op.oldest_at, "fuse", mount + " " + opcode + " outstanding", 1, -1.0,
                         "request");
            }
        }
        std::stable_sort(summary.begin(), summary.end());
        for (std::size_t index = 0; index < summary.size() && index < 8; ++index) {
            report_.overview.push_back("FUSE " + summary[index]);
        }
        if (!stuck.empty()) {
            fuse_slow_ = true;
            add(Severity::critical, "fuse", "FUSE requests stuck without an answer",
                std::move(stuck),
                {"The kernel sent these requests to the FUSE daemon and has not received a "
                 "reply. Check that the client daemon is running and responsive (its logs, CPU, "
                 "threads stuck in `D`) and whether it can reach its storage service.",
                 "Compare with the network and InfiniBand findings for the same time."});
        }
        if (!slow.empty()) {
            fuse_slow_ = true;
            add(worst_p99 >= 1000.0 ? Severity::critical : Severity::warning, "fuse",
                "Slow FUSE operations", std::move(slow),
                {"FUSE latency is the time from the kernel sending a request until the daemon "
                 "answers, which includes the daemon's own backend round trips. If local disks "
                 "are healthy, the delay is in the client daemon, its storage service, or the "
                 "network path between them.",
                 "Metadata operations (LOOKUP, GETATTR, OPEN) being slow while READ/WRITE are "
                 "fast points at metadata servers; the reverse points at data servers or "
                 "bandwidth."});
        }
    }

    void check_bpf_block() {
        struct Op {
            Histogram service;
            Histogram queue;
            double errors{};
            double direct{};
        };
        std::map<std::pair<std::string, std::string>, Op> operations;
        for (const auto* event : bpf_by_collector_["block"]) {
            auto& op = operations[{event->string("device"), event->string("operation")}];
            op.service.add((*event)["service"]);
            op.queue.add((*event)["queue"]);
            op.errors += event->number("errors", 0.0);
            op.direct += event->number("direct_issue", 0.0);
        }
        for (const auto& [key, op] : operations) {
            const auto& [device, operation] = key;
            const double service_p99 = op.service.quantile_ms(0.99);
            const double queue_p99 = op.queue.quantile_ms(0.99);
            report_.overview.push_back(format(
                "Block %s %s (eBPF): %.0f requests, service avg %.2f / p99 <= %.2f ms, queue avg "
                "%.2f / p99 <= %.2f ms over %.0f queued, %.0f errors.", device.c_str(),
                operation.c_str(), op.service.count, op.service.average_ms(), service_p99,
                op.queue.average_ms(), queue_p99, op.queue.count, op.errors));
            if (op.errors > 0) {
                add(Severity::critical, "io",
                    format("Block device %s returned %.0f %s errors", device.c_str(), op.errors,
                           operation.c_str()),
                    {"Errors were reported at request completion by the device driver."},
                    {"Check kernel.log for the matching error messages, the device's health "
                     "(`smartctl -a`, `nvme error-log`) and its cabling/controller."});
            }
            if (op.service.count < 10) continue;
            if (service_p99 >= 50.0) {
                add(service_p99 >= 500.0 ? Severity::critical : Severity::warning, "io",
                    format("Device %s is slow to service %s requests", device.c_str(),
                           operation.c_str()),
                    {format("Device service time (dispatch to completion) p99 <= %.1f ms, max "
                            "%.1f ms over %.0f requests.", service_p99, op.service.max_ms,
                            op.service.count)},
                    {"Slow service time is the device itself (or its transport): check its "
                     "health, firmware and, for network block devices, the path to the target."});
            } else if (op.queue.count >= 10 && queue_p99 >= 20.0 && queue_p99 >= 10.0 * service_p99) {
                add(Severity::warning, "io",
                    format("Requests to %s wait in the queue, not in the device", device.c_str()),
                    {format("%s queue time p99 <= %.1f ms while device service p99 <= %.2f ms.",
                            operation.c_str(), queue_p99, service_p99)},
                    {"The device answers quickly but requests queue before dispatch: the "
                     "offered load exceeds the device's queue depth or the I/O scheduler is "
                     "throttling (check `/sys/block/DEV/queue/scheduler`, io.max/io.latency "
                     "cgroup limits and the I/O sources with `iotop -o`)."});
            }
        }
    }

    void check_bpf_offcpu() {
        struct Group {
            std::string command;
            std::string stack;
            double count{};
            double total_ms{};
            double max_ms{};
        };
        std::map<std::pair<long, std::string>, Group> groups;
        const Value* longest_outstanding = nullptr;
        for (const auto* event : bpf_by_collector_["offcpu"]) {
            if (event->boolean("outstanding", false)) {
                if (!longest_outstanding ||
                    event->number("blocked_ms", 0.0) > longest_outstanding->number("blocked_ms", 0.0)) {
                    longest_outstanding = event;
                }
                continue;
            }
            if (own_process(event->string("command"))) continue;
            const std::string stack = stack_text(*event);
            auto& group = groups[{static_cast<long>(event->number("pid", 0.0)), stack}];
            group.command = event->string("command");
            group.stack = stack;
            group.count += event->number("count", 0.0);
            group.total_ms += event->number("total_ms", 0.0);
            group.max_ms = std::max(group.max_ms, event->number("max_ms", 0.0));
        }
        std::vector<Group> ranked;
        for (const auto& [key, group] : groups) ranked.push_back(group);
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const Group& a, const Group& b) { return a.total_ms > b.total_ms; });
        std::vector<std::string> details;
        double worst{};
        for (std::size_t index = 0; index < ranked.size() && index < 8; ++index) {
            const auto& group = ranked[index];
            if (group.total_ms < 1.0) break;
            worst = std::max(worst, group.max_ms);
            details.push_back(format("%s: %.0f blocks, %.0f ms total, longest %.0f ms, in %s.",
                                     group.command.c_str(), group.count, group.total_ms,
                                     group.max_ms, group.stack.c_str()));
        }
        if (longest_outstanding) {
            const double blocked = longest_outstanding->number("blocked_ms", 0.0);
            worst = std::max(worst, blocked);
            details.insert(details.begin(), format(
                "Still blocked at the end of an interval: %s (tid %.0f) for %.1f s in %s.",
                longest_outstanding->string("command").c_str(),
                longest_outstanding->number("tid", 0.0), blocked / 1000.0,
                stack_text(*longest_outstanding).c_str()));
        }
        if (details.empty()) return;
        add(worst >= 10000.0 ? Severity::critical
                             : (worst >= 1000.0 ? Severity::warning : Severity::info),
            "cpu", "Where tasks blocked in uninterruptible sleep (eBPF kernel stacks)",
            std::move(details),
            {"Read each stack from the left: the first frames name the kernel function the task "
             "was waiting in; the later frames show which subsystem called it (fuse_*, nfs_*, "
             "blk_*/submit_bio, xfs_/ext4_ journal, mutex/rwsem locks, or reclaim).",
             "A single long block matters more than many short ones; short blocks in "
             "io_schedule are normal disk I/O."});
    }

    void check_bpf_runqueue() {
        std::map<std::string, Histogram> cgroups;
        for (const auto* event : bpf_by_collector_["runqueue"]) {
            std::string cgroup = event->string("cgroup");
            if (cgroup.empty()) cgroup = format("id %.0f", event->number("cgroup_id", 0.0));
            cgroups[cgroup].add(*event);
        }
        std::vector<std::pair<std::string, const Histogram*>> ranked;
        for (const auto& [name, histogram] : cgroups) ranked.emplace_back(name, &histogram);
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.second->total_ms > b.second->total_ms;
        });
        std::vector<std::string> slow;
        for (std::size_t index = 0; index < ranked.size(); ++index) {
            const auto& [name, histogram] = ranked[index];
            const double p99 = histogram->quantile_ms(0.99);
            const std::string line = format(
                "%s: %.0f wakeups, average %.3f ms, p99 <= %.2f ms, max %.1f ms, %.0f ms total wait.",
                name.c_str(), histogram->count, histogram->average_ms(), p99, histogram->max_ms,
                histogram->total_ms);
            if (index < 5) report_.overview.push_back("Run-queue " + line);
            if (histogram->count >= 100 && p99 >= 10.0) slow.push_back(line);
        }
        if (!slow.empty()) {
            if (slow.size() > 8) slow.resize(8);
            add(Severity::warning, "cpu", "CPU run-queue latency per cgroup", std::move(slow),
                {"Run-queue latency is the time from wakeup until the task got a CPU. High values "
                 "for one cgroup with low values elsewhere suggest a CPU quota (cpu.max) or low "
                 "cpu.weight; high values everywhere mean the node is CPU-saturated.",
                 "Check `cat /sys/fs/cgroup/<cgroup>/cpu.max cpu.stat` (nr_throttled, "
                 "throttled_usec)."});
        }
    }

    void check_bpf_tcp() {
        std::map<std::string, double> connections;
        std::map<std::string, std::string> states;
        for (const auto* event : bpf_by_collector_["tcp_retransmit"]) {
            const std::string key = event->string("source") + " -> " + event->string("destination");
            connections[key] += event->number("retransmits", 0.0);
            states[key] = event->string("state");
        }
        if (connections.empty()) return;
        std::vector<std::pair<std::string, double>> ranked(connections.begin(), connections.end());
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        std::map<std::string, double> peers;
        for (const auto& [key, count] : ranked) {
            const auto arrow = key.find(" -> ");
            std::string peer = key.substr(arrow + 4);
            const auto colon = peer.rfind(':');
            if (colon != std::string::npos) peer = peer.substr(0, colon);
            peers[peer] += count;
        }
        std::vector<std::string> details;
        double total{};
        for (std::size_t index = 0; index < ranked.size(); ++index) {
            total += ranked[index].second;
            if (index < 8) {
                details.push_back(format("%s (%s): %.0f retransmits.", ranked[index].first.c_str(),
                                         states[ranked[index].first].c_str(), ranked[index].second));
            }
        }
        std::vector<std::pair<std::string, double>> peer_ranked(peers.begin(), peers.end());
        std::stable_sort(peer_ranked.begin(), peer_ranked.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        std::vector<std::string> peer_text;
        for (std::size_t index = 0; index < peer_ranked.size() && index < 5; ++index) {
            peer_text.push_back(format("%s=%.0f", peer_ranked[index].first.c_str(),
                                       peer_ranked[index].second));
        }
        details.push_back("By peer: " + join(peer_text, ", ") + ".");
        add(tcp_bad_ || total >= 100 ? Severity::warning : Severity::info, "network",
            format("TCP retransmits on %zu connections", ranked.size()), std::move(details),
            {"Retransmits concentrated on one peer point at that peer or the path to it; spread "
             "across all peers points at this host's NIC, driver or uplink.",
             "Check `ss -ti dst PEER` for rto and retrans, and `mtr PEER` for loss on the path."});
    }

    void check_bpf_reclaim() {
        struct Group {
            std::string kind;
            std::string command;
            std::string cgroup;
            double count{};
            double total_ms{};
            double max_ms{};
            double pages{};
        };
        std::map<std::tuple<std::string, long, std::string>, Group> groups;
        for (const auto* event : bpf_by_collector_["reclaim"]) {
            const std::string kind = event->string("kind");
            std::string cgroup = event->string("cgroup");
            if (cgroup.empty() && event->number("cgroup_id", 0.0) > 0) {
                cgroup = format("cgroup id %.0f (exited)", event->number("cgroup_id", 0.0));
            }
            auto& group = groups[{kind, static_cast<long>(event->number("pid", 0.0)), cgroup}];
            group.kind = kind;
            group.command = event->string("command");
            group.cgroup = cgroup;
            group.count += event->number("count", 0.0);
            group.total_ms += event->number("total_ms", 0.0);
            group.max_ms = std::max(group.max_ms, event->number("max_ms", 0.0));
            group.pages += event->number("reclaimed_pages", 0.0);
        }
        std::vector<Group> ranked;
        for (const auto& [key, group] : groups) ranked.push_back(group);
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const Group& a, const Group& b) { return a.total_ms > b.total_ms; });
        std::vector<std::string> details;
        double total{};
        double worst{};
        bool memcg = false;
        for (std::size_t index = 0; index < ranked.size(); ++index) {
            const auto& group = ranked[index];
            total += group.total_ms;
            worst = std::max(worst, group.max_ms);
            memcg |= group.kind == "memcg";
            if (index < 8) {
                details.push_back(format(
                    "%s reclaim by %s%s%s: %.0f stalls, %.1f ms total, longest %.1f ms, %.0f pages "
                    "reclaimed.", group.kind.c_str(), group.command.c_str(),
                    group.cgroup.empty() ? "" : " in ", group.cgroup.c_str(), group.count,
                    group.total_ms, group.max_ms, group.pages));
            }
        }
        if (details.empty()) return;
        reclaim_ms_ = total;
        std::vector<std::string> steps{
            "These processes paused to free memory themselves before their allocation could "
            "succeed; the stall time is latency added directly to them."};
        if (memcg) {
            steps.push_back("memcg reclaim means the cgroup reached memory.high or memory.max, "
                            "not that the node is out of memory: compare the cgroup's "
                            "memory.current with its limits.");
        }
        steps.push_back("Global direct reclaim means free memory fell below the watermarks: "
                        "check vm.min_free_kbytes, page-cache-heavy jobs and dirty-page backlog.");
        add(total >= 1000.0 || worst >= 100.0 ? Severity::warning : Severity::info, "memory",
            format("Memory reclaim stalls attributed to processes (%.0f ms total)", total),
            std::move(details), std::move(steps));
    }

    // -- kernel log ----------------------------------------------------------

    void check_kernel_log() {
        if (!files_.contains("kernel.log")) {
            report_.coverage.push_back("No kernel log in the archive (collected with "
                                       "--no-kernel-log or an older collector).");
            return;
        }
        const std::string& text = file("kernel.log");
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
            report_.coverage.push_back("Kernel log empty or unreadable (dmesg_restrict requires "
                                       "root).");
            return;
        }
        // Lines older than this are history, not evidence about the window.
        const bool have_window = window_start_.has_value();
        const double recent_since = have_window ? *window_start_ - 15 * 60 : 0.0;
        struct Hits {
            std::vector<std::string> recent;
            std::vector<std::string> old;
        };
        std::map<std::string, Hits> hits;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            for (const auto& pattern : kernel_patterns()) {
                if (!std::regex_search(line, pattern.pattern)) continue;
                const auto when = kernel_line_time(line);
                const bool old = have_window && when.has_value() && *when < recent_since;
                (old ? hits[pattern.name].old : hits[pattern.name].recent).push_back(line);
                break;
            }
        }
        for (const auto& pattern : kernel_patterns()) {
            const auto found = hits.find(pattern.name);
            if (found == hits.end()) continue;
            const auto& [recent, old] = found->second;
            if (!recent.empty()) {
                std::vector<std::string> details{"Most recent:"};
                for (std::size_t index = recent.size() > 3 ? recent.size() - 3 : 0;
                     index < recent.size(); ++index) {
                    details.push_back("  " + recent[index].substr(0, 200));
                }
                if (!old.empty()) {
                    details.push_back("Plus " + plural(old.size(), "older line") +
                                      " from more than 15 minutes before the window.");
                }
                std::vector<std::string> steps{pattern.advice};
                if (!have_window) {
                    steps.push_back("Kernel log timestamps could not be compared with the window; "
                                    "check that these lines belong to the incident.");
                }
                add(pattern.severity, "kernel log",
                    "Kernel log: " + pattern.name + " (" + plural(recent.size(), "line") + ")",
                    std::move(details), std::move(steps));
            } else {
                add(Severity::info, "kernel log",
                    "Kernel log: earlier " + pattern.name + " (" + plural(old.size(), "line") +
                        ", before the window)",
                    {"Most recent: " + old.back().substr(0, 200)},
                    {"History only: relevant if the incident started at that time. " +
                     pattern.advice});
            }
        }
    }

    // -- correlation -------------------------------------------------------

    void correlate() {
        auto& conclusions = report_.conclusions;
        const bool fuse = blocked_categories_.contains("fuse") || fuse_slow_;
        const bool nfs = blocked_categories_.contains("nfs");
        if (fuse || nfs) {
            const std::string layer = fuse ? "FUSE" : "NFS";
            if (unhealthy_io_.empty()) {
                conclusions.push_back(
                    "Threads waited on " + layer + " while every local block device stayed "
                    "healthy: the delay is above the local block layer, in the " + layer +
                    " client, its storage service or the network path to it.");
            }
            if (network_bad_ || tcp_bad_) {
                conclusions.push_back("Network/InfiniBand problems occurred during the same window "
                                      "as " + layer + " delays; the network path to the storage "
                                      "service is a likely contributor.");
            }
        }
        if (blocked_categories_.contains("block") && !unhealthy_io_.empty()) {
            conclusions.push_back("Threads waited on block I/O and " +
                                  join({unhealthy_io_.begin(), unhealthy_io_.end()}, ", ") +
                                  " showed degraded or congested service: start with the storage "
                                  "device findings.");
        }
        if (blocked_categories_.contains("block") && unhealthy_io_.empty()) {
            conclusions.push_back("Threads waited on block I/O but no device crossed the latency "
                                  "guardrails; look at short bursts in the eBPF block findings or "
                                  "at dirty-page throttling.");
        }
        const bool memory_significant = std::any_of(
            report_.findings.begin(), report_.findings.end(), [](const Finding& finding) {
                return finding.area == "memory" && finding.severity != Severity::info;
            });
        if (memory_significant &&
            (!unhealthy_io_.empty() || blocked_categories_.contains("memory"))) {
            conclusions.push_back("Memory reclaim coincided with I/O waits; page-cache pressure "
                                  "can turn into extra read and writeback I/O.");
        }
    }

    const Files& files_;
    Report report_;
    std::vector<Value> events_;
    std::vector<Value> bpf_events_;
    std::size_t invalid_lines_{};
    std::size_t invalid_bpf_lines_{};
    std::map<std::string, std::vector<const Value*>> by_resource_;
    std::map<std::string, std::vector<const Value*>> bpf_by_collector_;
    const Value* bpf_status_{};
    std::map<std::string, DeviceInfo> topology_;
    std::optional<double> window_start_;
    std::map<std::string, std::size_t> blocked_categories_;
    std::set<std::string> unhealthy_io_;
    bool psi_cpu_{};
    bool network_bad_{};
    bool tcp_bad_{};
    bool fuse_slow_{};
    double direct_reclaim_{};
    double reclaim_ms_{};
};

}  // namespace

Files load_bundle(const std::filesystem::path& path) {
    Files files;
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (!entry.is_regular_file() || entry.is_symlink()) continue;
            if (entry.file_size() > maximum_file_bytes) continue;
            std::ifstream input(entry.path(), std::ios::binary);
            std::ostringstream text;
            text << input.rdbuf();
            files[entry.path().filename().string()] = text.str();
        }
        return files;
    }
    if (!std::filesystem::is_regular_file(path, error)) {
        throw std::runtime_error("not a file or directory: " + path.string());
    }
    // Members are streamed through tar to stdout one at a time: nothing is
    // written to disk, so hostile member names cannot escape a directory.
    const auto listing = run_process({"tar", "-tzf", path.string()}, std::chrono::seconds(60));
    if (listing.exit_code != 0) {
        throw std::runtime_error("tar cannot list " + path.string() + ": " + listing.error);
    }
    std::istringstream names(listing.output);
    std::string member;
    while (std::getline(names, member)) {
        if (member.empty() || member.back() == '/') continue;
        const std::string base = std::filesystem::path(member).filename().string();
        if (base.empty() || files.contains(base)) continue;
        const auto content = run_process({"tar", "-xzOf", path.string(), member},
                                         std::chrono::seconds(120), maximum_file_bytes);
        if (content.exit_code == 0) files[base] = content.output;
    }
    return files;
}

Report analyze(const Files& files) {
    return Analysis(files).run();
}

std::string render(const Report& report, Format format_kind) {
    if (format_kind == Format::json) {
        std::ostringstream output;
        auto strings = [&](const std::vector<std::string>& values) {
            output << '[';
            for (std::size_t index = 0; index < values.size(); ++index) {
                output << (index ? "," : "") << '"' << json::escape(values[index]) << '"';
            }
            output << ']';
        };
        output << "{\"schema_version\":1,\"system\":{";
        bool first = true;
        for (const auto& [key, value] : report.system) {
            output << (first ? "" : ",") << '"' << json::escape(key) << "\":\""
                   << json::escape(value) << '"';
            first = false;
        }
        output << "},\"psi_status\":\"" << json::escape(report.psi_status)
               << "\",\"window_start\":\"" << json::escape(report.window_start)
               << "\",\"window_end\":\"" << json::escape(report.window_end)
               << "\",\"intervals\":" << report.intervals << ",\"findings\":[";
        for (std::size_t index = 0; index < report.findings.size(); ++index) {
            const auto& finding = report.findings[index];
            output << (index ? "," : "") << "{\"severity\":\"" << to_string(finding.severity)
                   << "\",\"area\":\"" << json::escape(finding.area) << "\",\"title\":\""
                   << json::escape(finding.title) << "\",\"details\":";
            strings(finding.details);
            output << ",\"next_steps\":";
            strings(finding.next_steps);
            output << '}';
        }
        output << "],\"conclusions\":";
        strings(report.conclusions);
        output << ",\"timeline\":[";
        for (std::size_t index = 0; index < report.timeline.size(); ++index) {
            const auto& entry = report.timeline[index];
            output << (index ? "," : "") << "{\"start\":\"" << json::escape(entry.start)
                   << "\",\"area\":\"" << json::escape(entry.area) << "\",\"text\":\""
                   << json::escape(entry.text) << "\",\"count\":" << entry.count
                   << ",\"unit\":\"" << entry.unit << '"';
            if (entry.peak_severity >= 0) output << ",\"peak_severity\":" << entry.peak_severity;
            output << '}';
        }
        output << "],\"overview\":";
        strings(report.overview);
        output << ",\"coverage\":";
        strings(report.coverage);
        output << "}\n";
        return output.str();
    }

    const bool markdown = format_kind == Format::markdown;
    std::ostringstream out;
    auto heading = [&](const std::string& text, int level) {
        if (markdown) {
            out << std::string(static_cast<std::size_t>(level), '#') << ' ' << text << "\n\n";
        } else {
            out << text << '\n' << std::string(text.size(), level == 1 ? '=' : '-') << "\n\n";
        }
    };
    auto bullet = [&](const std::string& text, int indent) {
        out << std::string(static_cast<std::size_t>(indent) * 2, ' ') << (markdown ? "- " : "* ")
            << text << '\n';
    };
    auto system = [&](const std::string& key, const char* fallback) {
        const auto found = report.system.find(key);
        return found == report.system.end() ? std::string(fallback) : found->second;
    };

    heading("Graywatch support summary: " + system("hostname", "unknown host"), 1);
    out << "Kernel " << system("kernel_release", "?") << " on "
        << system("os_PRETTY_NAME", "unknown OS") << ", " << system("cpus", "?")
        << " CPUs. Collected " << system("collected_utc", "?") << " for "
        << system("duration_seconds", "?") << "s as uid " << system("uid", "?") << ".\n\n";
    std::size_t actionable = 0;
    std::size_t critical = 0;
    for (const auto& finding : report.findings) {
        actionable += finding.severity != Severity::info;
        critical += finding.severity == Severity::critical;
    }
    if (actionable > 0) {
        std::string verdict(to_string(report.findings.front().severity));
        std::transform(verdict.begin(), verdict.end(), verdict.begin(), ::toupper);
        out << "Verdict: " << verdict << ". " << plural(actionable, "finding")
            << (actionable == 1 ? " needs" : " need") << " attention (" << critical
            << " critical).\n\n";
    } else {
        out << "Verdict: no degradation detected in this window. If the incident was not "
               "happening during collection, collect again while it is.\n\n";
    }
    if (!report.conclusions.empty()) {
        heading("Assessment", 2);
        for (const auto& line : report.conclusions) bullet(line, 0);
        out << '\n';
    }
    heading("Findings", 2);
    if (report.findings.empty()) out << "None.\n\n";
    for (std::size_t index = 0; index < report.findings.size(); ++index) {
        const auto& finding = report.findings[index];
        std::string level(to_string(finding.severity));
        std::transform(level.begin(), level.end(), level.begin(), ::toupper);
        const std::string title = format("%zu. [%s] ", index + 1, level.c_str()) + finding.title;
        out << (markdown ? "**" + title + "**" : title) << '\n';
        for (const auto& line : finding.details) bullet(line, 1);
        if (!finding.next_steps.empty()) {
            out << (markdown ? "  Next steps:\n" : "   Next steps:\n");
            for (const auto& line : finding.next_steps) bullet(line, 2);
        }
        out << '\n';
    }
    if (!report.timeline.empty()) {
        heading("Timeline (UTC)", 2);
        for (std::size_t index = 0; index < report.timeline.size() && index < 25; ++index) {
            const auto& entry = report.timeline[index];
            std::string text = format("%s %-10s ", clock_of(entry.start).c_str(), entry.area.c_str()) +
                               entry.text + " (" + plural(entry.count, entry.unit);
            if (entry.peak_severity >= 0) text += format(", peak severity %.2f", entry.peak_severity);
            bullet(text + ")", 0);
        }
        if (report.timeline.size() > 25) {
            bullet(format("... %zu more episodes", report.timeline.size() - 25), 0);
        }
        out << '\n';
    }
    heading("Resource overview", 2);
    for (const auto& line : report.overview) {
        if (line.starts_with("  ")) bullet(line.substr(2), 1);
        else bullet(line, 0);
    }
    out << '\n';
    heading("Collection coverage", 2);
    bullet("PSI status: " + report.psi_status + ".", 0);
    if (!report.window_start.empty()) {
        bullet("Sampled " + report.window_start + " to " + report.window_end + " (" +
                   std::to_string(report.intervals) + " intervals).", 0);
    }
    for (const auto& line : report.coverage) bullet(line, 0);
    out << "\nThresholds in this summary are triage heuristics. The daemon's per-interval "
           "condition in events.jsonl is the authoritative classification.\n";
    return out.str();
}

}  // namespace graywatch::report
