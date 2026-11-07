// graywatch-bpf: privileged eBPF evidence collector for Graywatch.
//
// Loads the collectors whose kernel hooks exist, then prints one JSON object
// per line: a bpf_status line first, then bpf_interval lines every interval.

#include "graywatch.h"
#include "graywatch.skel.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }

const char* const collector_names[GW_COLLECTORS] = {"fuse", "block", "offcpu",
                                                    "runqueue", "tcp_retransmit", "reclaim"};

struct Options {
    double interval{1.0};
    double duration{};                  // 0 = until interrupted
    std::set<std::string> collectors;   // empty = all
    std::size_t top{20};
    double blocked_report_ms{1000.0};
};

[[noreturn]] void usage() {
    std::cerr << "usage: graywatch-bpf [--interval SECONDS] [--duration SECONDS]\n"
                 "                     [--collectors fuse,block,offcpu,runqueue,tcp_retransmit,reclaim]\n"
                 "                     [--top N] [--blocked-report-ms MS]\n";
    std::exit(2);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (index + 1 >= argc) usage();
        const std::string value(argv[++index]);
        try {
            if (argument == "--interval") options.interval = std::stod(value);
            else if (argument == "--duration") options.duration = std::stod(value);
            else if (argument == "--top") options.top = std::stoul(value);
            else if (argument == "--blocked-report-ms") options.blocked_report_ms = std::stod(value);
            else if (argument == "--collectors") {
                std::istringstream names(value);
                std::string name;
                while (std::getline(names, name, ',')) {
                    if (std::find(std::begin(collector_names), std::end(collector_names), name) ==
                        std::end(collector_names)) {
                        std::cerr << "graywatch-bpf: unknown collector " << name << '\n';
                        usage();
                    }
                    options.collectors.insert(name);
                }
            } else {
                usage();
            }
        } catch (const std::exception&) {
            usage();
        }
    }
    if (options.interval <= 0 || options.duration < 0 || options.top == 0) usage();
    return options;
}

std::string timestamp_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    char text[40];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &utc);
    char result[48];
    std::snprintf(result, sizeof(result), "%s.%03lldZ", text, static_cast<long long>(ms));
    return result;
}

__u64 monotonic_ns() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<__u64>(now.tv_sec) * 1'000'000'000ull + static_cast<__u64>(now.tv_nsec);
}

std::string escape(std::string_view text) {
    std::string output;
    for (const unsigned char character : text) {
        if (character == '"' || character == '\\') {
            output += '\\';
            output += static_cast<char>(character);
        } else if (character < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
            output += buffer;
        } else {
            output += static_cast<char>(character);
        }
    }
    return output;
}

std::string comm_text(const char* comm) {
    return escape(std::string(comm, strnlen(comm, GW_COMM_LEN)));
}

std::string number(double value, int precision = 3) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", precision, value);
    return text;
}

// ---------------------------------------------------------------------------
// libbpf diagnostics are captured so failures can be explained per collector.

std::string libbpf_log;

int capture_libbpf(enum libbpf_print_level level, const char* format, va_list arguments) {
    if (level == LIBBPF_DEBUG) return 0;
    char line[1024];
    std::vsnprintf(line, sizeof(line), format, arguments);
    libbpf_log += line;
    if (libbpf_log.size() > 65536) libbpf_log.erase(0, libbpf_log.size() - 65536);
    return 0;
}

std::string last_log_line() {
    std::istringstream input(libbpf_log);
    std::string line;
    std::string last;
    while (std::getline(input, line)) {
        if (!line.empty()) last = line;
    }
    return last.substr(0, 200);
}

// ---------------------------------------------------------------------------
// Hook availability from kernel BTF.

class KernelBtf {
  public:
    KernelBtf() : vmlinux_(btf__load_vmlinux_btf()) {}
    ~KernelBtf() {
        if (fuse_) btf__free(fuse_);
        if (vmlinux_) btf__free(vmlinux_);
    }
    KernelBtf(const KernelBtf&) = delete;
    KernelBtf& operator=(const KernelBtf&) = delete;

    [[nodiscard]] bool available() const { return vmlinux_ != nullptr; }

    [[nodiscard]] bool tracepoint(const std::string& name) const {
        return vmlinux_ && btf__find_by_name_kind(vmlinux_, ("btf_trace_" + name).c_str(),
                                                  BTF_KIND_TYPEDEF) > 0;
    }

    [[nodiscard]] bool function(const std::string& name) const {
        return vmlinux_ && btf__find_by_name_kind(vmlinux_, name.c_str(), BTF_KIND_FUNC) > 0;
    }

    // FUSE may be built in (vmlinux BTF) or a module (/sys/kernel/btf/fuse).
    [[nodiscard]] bool fuse_function(const std::string& name) {
        if (function(name)) return true;
        if (!fuse_loaded_) {
            fuse_loaded_ = true;
            if (vmlinux_ && std::filesystem::exists("/sys/kernel/btf/fuse")) {
                fuse_ = btf__load_module_btf("fuse", vmlinux_);
            }
        }
        return fuse_ && btf__find_by_name_kind(fuse_, name.c_str(), BTF_KIND_FUNC) > 0;
    }

  private:
    struct btf* vmlinux_{};
    struct btf* fuse_{};
    bool fuse_loaded_{};
};

struct CollectorPlan {
    std::string name;
    bool wanted{};
    bool enabled{};
    std::string reason;    // why disabled, or which hook variant is used
    std::vector<std::string> programs;
    std::vector<std::string> optional_programs;
    std::vector<std::string> notes;
};

std::vector<CollectorPlan> plan_collectors(const Options& options, KernelBtf& btf) {
    std::vector<CollectorPlan> plans;
    auto wanted = [&](const char* name) {
        return options.collectors.empty() || options.collectors.contains(name);
    };
    auto add = [&](const char* name) -> CollectorPlan& {
        CollectorPlan plan;
        plan.name = name;
        plan.wanted = wanted(name);
        plans.push_back(std::move(plan));
        return plans.back();
    };
    auto require_tracepoints = [&](CollectorPlan& plan, const std::vector<std::string>& names) {
        for (const auto& name : names) {
            if (!btf.tracepoint(name)) {
                plan.reason = "tracepoint " + name + " not in kernel BTF";
                return false;
            }
        }
        return true;
    };

    auto& fuse = add("fuse");
    if (btf.fuse_function("__fuse_simple_request")) {
        fuse.programs = {"fuse_enter_idmap", "fuse_exit_idmap"};
        fuse.reason = "__fuse_simple_request";
    } else if (btf.fuse_function("fuse_simple_request")) {
        fuse.programs = {"fuse_enter", "fuse_exit"};
        fuse.reason = "fuse_simple_request";
    } else {
        fuse.reason = std::filesystem::exists("/sys/module/fuse")
            ? "fuse module has no BTF for fuse_simple_request"
            : "fuse module not loaded";
    }
    fuse.notes.push_back("synchronous requests only; readahead and writeback are background requests");

    auto& block = add("block");
    if (!btf.function("blk_mq_start_request")) {
        block.reason = "blk_mq_start_request not in kernel BTF";
    } else if (require_tracepoints(block, {"block_rq_complete"})) {
        block.programs = {"account_start", "account_done"};
        if (btf.tracepoint("block_rq_insert")) block.optional_programs = {"block_insert"};
        else block.notes.push_back("block_rq_insert missing: no queue time");
    }

    auto& offcpu = add("offcpu");
    if (require_tracepoints(offcpu, {"sched_switch", "sched_wakeup"})) {
        offcpu.programs = {"offcpu_switch", "offcpu_wakeup"};
    }

    auto& runqueue = add("runqueue");
    if (require_tracepoints(runqueue, {"sched_switch", "sched_wakeup", "sched_wakeup_new"})) {
        runqueue.programs = {"runqueue_switch", "runqueue_wakeup", "runqueue_wakeup_new"};
    }

    auto& tcp = add("tcp_retransmit");
    if (require_tracepoints(tcp, {"tcp_retransmit_skb"})) tcp.programs = {"tcp_retransmit"};

    auto& reclaim = add("reclaim");
    if (require_tracepoints(reclaim, {"mm_vmscan_direct_reclaim_begin",
                                      "mm_vmscan_direct_reclaim_end"})) {
        reclaim.programs = {"reclaim_direct_begin", "reclaim_direct_end"};
        if (btf.tracepoint("mm_vmscan_memcg_reclaim_begin") &&
            btf.tracepoint("mm_vmscan_memcg_reclaim_end")) {
            reclaim.optional_programs = {"reclaim_memcg_begin", "reclaim_memcg_end"};
        } else {
            reclaim.notes.push_back("memcg reclaim tracepoints missing");
        }
    }

    for (auto& plan : plans) {
        if (!plan.wanted) {
            plan.reason = "not requested";
            plan.programs.clear();
            plan.optional_programs.clear();
        }
        plan.enabled = !plan.programs.empty();
    }
    return plans;
}

// Open and load a skeleton with exactly the given programs enabled.
graywatch_bpf* load_with(const std::set<std::string>& programs) {
    graywatch_bpf* skeleton = graywatch_bpf__open();
    if (!skeleton) return nullptr;
    bpf_program* program = nullptr;
    bpf_object__for_each_program(program, skeleton->obj) {
        bpf_program__set_autoload(program, programs.contains(bpf_program__name(program)));
    }
    if (graywatch_bpf__load(skeleton) != 0) {
        graywatch_bpf__destroy(skeleton);
        return nullptr;
    }
    return skeleton;
}

std::set<std::string> programs_of(const std::vector<CollectorPlan>& plans) {
    std::set<std::string> programs;
    for (const auto& plan : plans) {
        if (!plan.enabled) continue;
        programs.insert(plan.programs.begin(), plan.programs.end());
        programs.insert(plan.optional_programs.begin(), plan.optional_programs.end());
    }
    return programs;
}

// ---------------------------------------------------------------------------
// Map access and deltas.

template <class Key, class Value>
std::vector<std::pair<Key, Value>> dump(int fd) {
    std::vector<std::pair<Key, Value>> entries;
    Key key{};
    Key next{};
    bool first = true;
    while (bpf_map_get_next_key(fd, first ? nullptr : &key, &next) == 0) {
        Value value{};
        if (bpf_map_lookup_elem(fd, &next, &value) == 0) entries.emplace_back(next, value);
        key = next;
        first = false;
    }
    return entries;
}

template <class Key>
void clear_map(int fd) {
    Key key{};
    while (bpf_map_get_next_key(fd, nullptr, &key) == 0) {
        if (bpf_map_delete_elem(fd, &key) != 0) break;
    }
}

template <class Key>
std::string key_bytes(const Key& key) {
    return std::string(reinterpret_cast<const char*>(&key), sizeof(Key));
}

gw_hist hist_delta(const gw_hist& current, const gw_hist& previous) {
    gw_hist delta{};
    delta.count = current.count - previous.count;
    delta.total_ns = current.total_ns - previous.total_ns;
    for (int index = 0; index < GW_BUCKETS; ++index) {
        delta.buckets[index] = current.buckets[index] - previous.buckets[index];
    }
    // The kernel keeps a cumulative maximum. A new record happened in this
    // interval; otherwise bound it by the upper edge of the highest occupied
    // bucket (never below the true interval maximum).
    if (current.max_ns > previous.max_ns) {
        delta.max_ns = current.max_ns;
    } else {
        for (int index = GW_BUCKETS - 1; index >= 0; --index) {
            if (delta.buckets[index]) {
                delta.max_ns = std::min<__u64>((1ull << (index + 1)) * 1000ull, current.max_ns);
                break;
            }
        }
    }
    return delta;
}

double quantile_upper_us(const gw_hist& hist, double fraction) {
    if (hist.count == 0) return 0;
    const auto target = static_cast<__u64>(std::ceil(static_cast<double>(hist.count) * fraction));
    __u64 seen{};
    for (int index = 0; index < GW_BUCKETS; ++index) {
        seen += hist.buckets[index];
        if (seen >= target) return static_cast<double>(1ull << (index + 1));
    }
    return static_cast<double>(1ull << GW_BUCKETS);
}

std::string hist_json(const gw_hist& hist) {
    std::string text = "\"count\":" + std::to_string(hist.count) +
                       ",\"total_ms\":" + number(static_cast<double>(hist.total_ns) / 1e6) +
                       ",\"average_ms\":" +
                       number(hist.count ? static_cast<double>(hist.total_ns) / hist.count / 1e6 : 0) +
                       ",\"max_ms\":" + number(static_cast<double>(hist.max_ns) / 1e6) +
                       ",\"p50_upper_us\":" + number(quantile_upper_us(hist, 0.50), 0) +
                       ",\"p99_upper_us\":" + number(quantile_upper_us(hist, 0.99), 0) +
                       ",\"histogram_us\":{";
    bool first = true;
    for (int index = 0; index < GW_BUCKETS; ++index) {
        if (!hist.buckets[index]) continue;
        text += std::string(first ? "" : ",") + "\"" + std::to_string(1ull << (index + 1)) +
                "\":" + std::to_string(hist.buckets[index]);
        first = false;
    }
    return text + "}";
}

// ---------------------------------------------------------------------------
// Name resolution.

class Symbols {
  public:
    Symbols() {
        std::ifstream input("/proc/kallsyms");
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            std::string address;
            std::string type;
            std::string name;
            if (!(fields >> address >> type >> name)) continue;
            if (type != "t" && type != "T") continue;
            const auto value = std::strtoull(address.c_str(), nullptr, 16);
            if (value == 0) continue;  // kptr_restrict hides addresses
            symbols_.emplace_back(value, name);
        }
        std::sort(symbols_.begin(), symbols_.end());
    }

    [[nodiscard]] bool available() const { return !symbols_.empty(); }

    [[nodiscard]] std::string name(__u64 address) const {
        auto found = std::upper_bound(symbols_.begin(), symbols_.end(),
                                      std::make_pair(address, std::string("\xff")));
        if (found == symbols_.begin()) return hex(address);
        --found;
        return found->second;
    }

  private:
    static std::string hex(__u64 address) {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    std::vector<std::pair<__u64, std::string>> symbols_;
};

// The captured stack starts inside the BPF program and the tracepoint
// machinery. Without bpf_jit_kallsyms those frames resolve to unrelated
// neighbouring symbols, so everything up to the scheduler entry is cut.
bool scheduler_entry(const std::string& name) {
    static const std::set<std::string> exact{"__schedule", "schedule",
                                             "schedule_preempt_disabled",
                                             "preempt_schedule_common", "schedule_idle"};
    return exact.contains(name) || name.starts_with("__traceiter_") ||
           name.starts_with("bpf_trace_run");
}

std::string stack_json(int stacks_fd, __s32 stack_id, const Symbols& symbols) {
    if (stack_id < 0) return "[]";
    __u64 frames[32] = {};
    __u32 id = static_cast<__u32>(stack_id);
    if (bpf_map_lookup_elem(stacks_fd, &id, frames) != 0) return "[]";
    std::vector<std::string> names;
    for (const __u64 frame : frames) {
        if (frame == 0) break;
        names.push_back(symbols.name(frame));
    }
    std::size_t begin = 0;
    for (std::size_t index = 0; index < names.size() && index < 12; ++index) {
        if (scheduler_entry(names[index])) begin = index + 1;
    }
    std::string text = "[";
    for (std::size_t index = begin; index < names.size(); ++index) {
        text += std::string(index == begin ? "" : ",") + "\"" + escape(names[index]) + "\"";
    }
    return text + "]";
}

std::string block_device(__u32 dev) {
    const auto major = dev >> 20;
    const auto minor = dev & ((1u << 20) - 1);
    std::error_code error;
    const auto path = std::filesystem::canonical(
        "/sys/dev/block/" + std::to_string(major) + ":" + std::to_string(minor), error);
    return error ? std::to_string(major) + ":" + std::to_string(minor) : path.filename().string();
}

// FUSE superblock dev_t -> mount point, from /proc/self/mountinfo.
std::map<__u32, std::string> fuse_mounts() {
    std::map<__u32, std::string> mounts;
    std::ifstream input("/proc/self/mountinfo");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string id, parent, device, root, point;
        if (!(fields >> id >> parent >> device >> root >> point)) continue;
        if (line.find(" - fuse") == std::string::npos) continue;
        unsigned int major{}, minor{};
        if (std::sscanf(device.c_str(), "%u:%u", &major, &minor) != 2) continue;
        mounts[(major << 20) | minor] = point;
    }
    return mounts;
}

const char* fuse_opcode(__u32 opcode) {
    static const std::map<__u32, const char*> names{
        {1, "LOOKUP"}, {2, "FORGET"}, {3, "GETATTR"}, {4, "SETATTR"}, {5, "READLINK"},
        {6, "SYMLINK"}, {8, "MKNOD"}, {9, "MKDIR"}, {10, "UNLINK"}, {11, "RMDIR"},
        {12, "RENAME"}, {13, "LINK"}, {14, "OPEN"}, {15, "READ"}, {16, "WRITE"},
        {17, "STATFS"}, {18, "RELEASE"}, {20, "FSYNC"}, {21, "SETXATTR"}, {22, "GETXATTR"},
        {23, "LISTXATTR"}, {24, "REMOVEXATTR"}, {25, "FLUSH"}, {26, "INIT"}, {27, "OPENDIR"},
        {28, "READDIR"}, {29, "RELEASEDIR"}, {30, "FSYNCDIR"}, {31, "GETLK"}, {32, "SETLK"},
        {33, "SETLKW"}, {34, "ACCESS"}, {35, "CREATE"}, {36, "INTERRUPT"}, {37, "BMAP"},
        {38, "DESTROY"}, {39, "IOCTL"}, {40, "POLL"}, {41, "NOTIFY_REPLY"},
        {42, "BATCH_FORGET"}, {43, "FALLOCATE"}, {44, "READDIRPLUS"}, {45, "RENAME2"},
        {46, "LSEEK"}, {47, "COPY_FILE_RANGE"}, {48, "SETUPMAPPING"}, {49, "REMOVEMAPPING"},
        {50, "SYNCFS"}, {51, "TMPFILE"}, {52, "STATX"}};
    const auto found = names.find(opcode);
    return found == names.end() ? "UNKNOWN" : found->second;
}

class Cgroups {
  public:
    std::string path(__u64 id) {
        auto found = paths_.find(id);
        if (found == paths_.end() && !scanned_this_interval_) {
            scan();
            found = paths_.find(id);
        }
        return found == paths_.end() ? "" : found->second;
    }
    void new_interval() { scanned_this_interval_ = false; }

  private:
    void scan() {
        scanned_this_interval_ = true;
        const std::filesystem::path root("/sys/fs/cgroup");
        std::error_code error;
        struct stat info {};
        if (stat(root.c_str(), &info) == 0) paths_[info.st_ino] = "/";
        for (std::filesystem::recursive_directory_iterator walk(root, error), end;
             !error && walk != end; walk.increment(error)) {
            if (walk.depth() > 8) {
                walk.disable_recursion_pending();
                continue;
            }
            if (!walk->is_directory(error)) continue;
            if (stat(walk->path().c_str(), &info) == 0) {
                paths_[info.st_ino] = "/" + std::filesystem::relative(walk->path(), root).string();
            }
        }
    }
    std::map<__u64, std::string> paths_;
    bool scanned_this_interval_{};
};

bool thread_in_state(__u32 tid, char wanted) {
    std::ifstream input("/proc/" + std::to_string(tid) + "/stat");
    std::string text;
    if (!std::getline(input, text)) return false;
    const auto close = text.rfind(')');
    return close != std::string::npos && close + 2 < text.size() && text[close + 2] == wanted;
}

bool thread_exists(__u32 tid) {
    return std::filesystem::exists("/proc/" + std::to_string(tid));
}

const char* tcp_state(__u32 state) {
    static const char* names[] = {"UNKNOWN", "ESTABLISHED", "SYN_SENT", "SYN_RECV",
                                  "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT", "CLOSE",
                                  "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING",
                                  "NEW_SYN_RECV"};
    return state < std::size(names) ? names[state] : "UNKNOWN";
}

std::string endpoint(__u16 family, const __u8* address, __u16 port) {
    // Dual-stack sockets carry IPv4 peers as ::ffff:a.b.c.d; print those as IPv4.
    static const __u8 mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (family != 2 && std::memcmp(address, mapped, sizeof(mapped)) == 0) {
        return endpoint(2, address + 12, port);
    }
    char text[INET6_ADDRSTRLEN] = "?";
    inet_ntop(family == 2 ? AF_INET : AF_INET6, address, text, sizeof(text));
    return family == 2 ? std::string(text) + ":" + std::to_string(port)
                       : "[" + std::string(text) + "]:" + std::to_string(port);
}

// ---------------------------------------------------------------------------
// Interval emission.

class Emitter {
  public:
    Emitter(graywatch_bpf* skeleton, const Options& options,
            const std::vector<CollectorPlan>& plans)
        : skeleton_(skeleton), options_(options) {
        for (const auto& plan : plans) enabled_[plan.name] = plan.enabled;
    }

    void interval(double elapsed) {
        timestamp_ = timestamp_utc();
        elapsed_ = elapsed;
        cgroups_.new_interval();
        if (enabled_["fuse"]) fuse();
        if (enabled_["block"]) block();
        if (enabled_["offcpu"]) offcpu();
        if (enabled_["runqueue"]) runqueue();
        if (enabled_["tcp_retransmit"]) tcp();
        if (enabled_["reclaim"]) reclaim();
        drops();
        std::cout << std::flush;
    }

    [[nodiscard]] bool symbols_available() const { return symbols_.available(); }

  private:
    std::string prefix(const char* collector) const {
        return "{\"schema_version\":1,\"event_type\":\"bpf_interval\",\"timestamp\":\"" +
               timestamp_ + "\",\"interval_seconds\":" + number(elapsed_, 6) +
               ",\"collector\":\"" + collector + "\"";
    }

    // Clear a cumulative map that is close to full; its next interval then
    // starts from zero instead of silently dropping new keys.
    template <class Key>
    bool rotate_if_full(int fd, std::size_t entries, std::size_t capacity,
                        std::map<std::string, std::string>& previous) {
        if (entries * 10 < capacity * 8) return false;
        clear_map<Key>(fd);
        previous.clear();
        return true;
    }

    void fuse() {
        const int fd = bpf_map__fd(skeleton_->maps.fuse_statistics);
        const auto mounts = fuse_mounts();
        auto mount_of = [&](__u32 dev) {
            const auto found = mounts.find(dev);
            return found == mounts.end()
                ? std::to_string(dev >> 20) + ":" + std::to_string(dev & 0xfffff)
                : found->second;
        };
        // Requests still waiting for a reply, including ones that never finish.
        struct Outstanding {
            std::size_t count{};
            double oldest_ms{};
        };
        std::map<std::pair<__u32, __u32>, Outstanding> outstanding;
        const int starts_fd = bpf_map__fd(skeleton_->maps.fuse_starts);
        const __u64 now = monotonic_ns();
        for (const auto& [tid, start] : dump<__u32, fuse_start>(starts_fd)) {
            if (!thread_exists(tid)) {
                bpf_map_delete_elem(starts_fd, &tid);
                continue;
            }
            auto& entry = outstanding[{start.dev, start.opcode}];
            ++entry.count;
            if (now > start.start_ns) {
                entry.oldest_ms = std::max(entry.oldest_ms,
                                           static_cast<double>(now - start.start_ns) / 1e6);
            }
        }
        const auto entries = dump<fuse_key, fuse_stats>(fd);
        std::set<std::pair<__u32, __u32>> written;
        for (const auto& [key, value] : entries) {
            const auto bytes = key_bytes(key);
            fuse_stats previous{};
            if (const auto found = fuse_previous_.find(bytes); found != fuse_previous_.end()) {
                std::memcpy(&previous, found->second.data(), sizeof(previous));
            }
            fuse_previous_[bytes] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            const auto delta = hist_delta(value.latency, previous.latency);
            const auto identity = std::make_pair(key.dev, key.opcode);
            const auto open = outstanding.find(identity);
            if (delta.count == 0 && open == outstanding.end()) continue;
            written.insert(identity);
            std::cout << prefix("fuse") << ",\"device\":\"" << (key.dev >> 20) << ':'
                      << (key.dev & 0xfffff) << "\",\"mount\":\"" << escape(mount_of(key.dev))
                      << "\",\"opcode\":\"" << fuse_opcode(key.opcode) << "\","
                      << hist_json(delta) << ",\"errors\":" << value.errors - previous.errors
                      << ",\"outstanding\":" << (open == outstanding.end() ? 0 : open->second.count)
                      << ",\"oldest_outstanding_ms\":"
                      << number(open == outstanding.end() ? 0 : open->second.oldest_ms) << "}\n";
        }
        for (const auto& [identity, open] : outstanding) {
            if (written.contains(identity)) continue;
            std::cout << prefix("fuse") << ",\"device\":\"" << (identity.first >> 20) << ':'
                      << (identity.first & 0xfffff) << "\",\"mount\":\""
                      << escape(mount_of(identity.first)) << "\",\"opcode\":\""
                      << fuse_opcode(identity.second) << "\"," << hist_json(gw_hist{})
                      << ",\"errors\":0,\"outstanding\":" << open.count
                      << ",\"oldest_outstanding_ms\":" << number(open.oldest_ms) << "}\n";
        }
        rotate_if_full<fuse_key>(fd, entries.size(), 1024, fuse_previous_);
    }

    void block() {
        const int fd = bpf_map__fd(skeleton_->maps.statistics);
        const auto entries = dump<io_key, io_stats>(fd);
        static const char* operations[] = {"read", "write", "flush", "discard"};
        for (const auto& [key, value] : entries) {
            const auto bytes = key_bytes(key);
            io_stats previous{};
            if (const auto found = block_previous_.find(bytes); found != block_previous_.end()) {
                std::memcpy(&previous, found->second.data(), sizeof(previous));
            }
            block_previous_[bytes] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            const auto service = hist_delta(value.service, previous.service);
            const auto queue = hist_delta(value.queue, previous.queue);
            const auto errors = value.errors - previous.errors;
            if (service.count == 0 && queue.count == 0 && errors == 0) continue;
            std::cout << prefix("block") << ",\"device\":\"" << escape(block_device(key.dev))
                      << "\",\"operation\":\""
                      << (key.operation < 4 ? operations[key.operation] : "other")
                      << "\",\"bytes\":" << value.bytes - previous.bytes << ",\"errors\":" << errors
                      << ",\"direct_issue\":" << value.direct_issue - previous.direct_issue
                      << ",\"service\":{" << hist_json(service) << "},\"queue\":{"
                      << hist_json(queue) << "}}\n";
        }
        rotate_if_full<io_key>(fd, entries.size(), 1024, block_previous_);
    }

    void offcpu() {
        const int fd = bpf_map__fd(skeleton_->maps.offcpu_statistics);
        const int stacks_fd = bpf_map__fd(skeleton_->maps.stacks);
        const auto entries = dump<offcpu_key, offcpu_stats>(fd);
        struct Row {
            offcpu_key key;
            gw_hist delta;
            std::string comm;
        };
        std::vector<Row> rows;
        for (const auto& [key, value] : entries) {
            const auto bytes = key_bytes(key);
            offcpu_stats previous{};
            if (const auto found = offcpu_previous_.find(bytes); found != offcpu_previous_.end()) {
                std::memcpy(&previous, found->second.data(), sizeof(previous));
            }
            offcpu_previous_[bytes] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            const auto delta = hist_delta(value.blocked, previous.blocked);
            if (delta.count) rows.push_back({key, delta, comm_text(value.comm)});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.delta.total_ns > b.delta.total_ns;
        });
        if (rows.size() > options_.top) rows.resize(options_.top);
        for (const auto& row : rows) {
            std::cout << prefix("offcpu") << ",\"outstanding\":false,\"pid\":" << row.key.tgid
                      << ",\"command\":\"" << row.comm << "\"," << hist_json(row.delta)
                      << ",\"stack\":" << stack_json(stacks_fd, row.key.stack_id, symbols_) << "}\n";
        }
        // Threads still blocked: the stalls that have not ended yet.
        const int blocked_fd = bpf_map__fd(skeleton_->maps.blocked);
        const __u64 now = monotonic_ns();
        std::vector<std::pair<__u32, blocked_task>> stuck;
        for (const auto& [tid, task] : dump<__u32, blocked_task>(blocked_fd)) {
            if (now <= task.start_ns ||
                static_cast<double>(now - task.start_ns) / 1e6 < options_.blocked_report_ms) {
                continue;
            }
            // Verify against /proc: an exited or already-running thread whose
            // wakeup was not observed leaves a stale entry.
            if (!thread_in_state(tid, 'D')) {
                bpf_map_delete_elem(blocked_fd, &tid);
                continue;
            }
            stuck.emplace_back(tid, task);
        }
        std::sort(stuck.begin(), stuck.end(), [](const auto& a, const auto& b) {
            return a.second.start_ns < b.second.start_ns;
        });
        for (std::size_t index = 0; index < stuck.size() && index < 10; ++index) {
            const auto& [tid, task] = stuck[index];
            std::cout << prefix("offcpu") << ",\"outstanding\":true,\"tid\":" << tid
                      << ",\"pid\":" << task.tgid << ",\"command\":\"" << comm_text(task.comm)
                      << "\",\"blocked_ms\":" << number(static_cast<double>(now - task.start_ns) / 1e6)
                      << ",\"stack\":" << stack_json(stacks_fd, task.stack_id, symbols_) << "}\n";
        }
        rotate_if_full<offcpu_key>(fd, entries.size(), 16384, offcpu_previous_);
    }

    void runqueue() {
        const int fd = bpf_map__fd(skeleton_->maps.runqueue_statistics);
        const auto entries = dump<__u64, gw_hist>(fd);
        std::vector<std::pair<__u64, gw_hist>> rows;
        for (const auto& [cgroup, value] : entries) {
            const auto bytes = key_bytes(cgroup);
            gw_hist previous{};
            if (const auto found = runqueue_previous_.find(bytes); found != runqueue_previous_.end()) {
                std::memcpy(&previous, found->second.data(), sizeof(previous));
            }
            runqueue_previous_[bytes] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            const auto delta = hist_delta(value, previous);
            if (delta.count) rows.emplace_back(cgroup, delta);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.total_ns > b.second.total_ns;
        });
        if (rows.size() > options_.top) rows.resize(options_.top);
        for (const auto& [cgroup, delta] : rows) {
            std::cout << prefix("runqueue") << ",\"cgroup_id\":" << cgroup << ",\"cgroup\":\""
                      << escape(cgroups_.path(cgroup)) << "\"," << hist_json(delta) << "}\n";
        }
        rotate_if_full<__u64>(fd, entries.size(), 8192, runqueue_previous_);
    }

    void tcp() {
        const int fd = bpf_map__fd(skeleton_->maps.tcp_statistics);
        const auto entries = dump<tcp_key, tcp_stats>(fd);
        std::vector<std::pair<tcp_key, tcp_stats>> rows;
        for (const auto& [key, value] : entries) {
            const auto bytes = key_bytes(key);
            tcp_stats previous{};
            if (const auto found = tcp_previous_.find(bytes); found != tcp_previous_.end()) {
                std::memcpy(&previous, found->second.data(), sizeof(previous));
            }
            tcp_previous_[bytes] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            tcp_stats delta = value;
            delta.retransmits = value.retransmits - previous.retransmits;
            if (delta.retransmits) rows.emplace_back(key, delta);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.retransmits > b.second.retransmits;
        });
        if (rows.size() > options_.top) rows.resize(options_.top);
        for (const auto& [key, value] : rows) {
            std::cout << prefix("tcp_retransmit") << ",\"family\":\""
                      << (key.family == 2 ? "ipv4" : "ipv6") << "\",\"source\":\""
                      << endpoint(key.family, key.saddr, key.sport) << "\",\"destination\":\""
                      << endpoint(key.family, key.daddr, key.dport) << "\",\"state\":\""
                      << tcp_state(value.state) << "\",\"retransmits\":" << value.retransmits
                      << "}\n";
        }
        rotate_if_full<tcp_key>(fd, entries.size(), 16384, tcp_previous_);
    }

    void reclaim() {
        const int fd = bpf_map__fd(skeleton_->maps.reclaim_statistics);
        const auto entries = dump<reclaim_key, reclaim_stats>(fd);
        for (const auto& [key, value] : entries) {
            const auto bytes = key_bytes(key);
            reclaim_stats previous{};
            if (const auto found = reclaim_previous_.find(bytes); found != reclaim_previous_.end()) {
                std::memcpy(&previous, found->second.data(), sizeof(previous));
            }
            reclaim_previous_[bytes] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            const auto delta = hist_delta(value.stall, previous.stall);
            if (delta.count == 0) continue;
            std::cout << prefix("reclaim") << ",\"kind\":\""
                      << (key.kind == GW_RECLAIM_MEMCG ? "memcg" : "direct") << "\",\"pid\":"
                      << key.tgid << ",\"command\":\"" << comm_text(value.comm)
                      << "\",\"cgroup_id\":" << key.cgroup_id << ",\"cgroup\":\""
                      << escape(cgroups_.path(key.cgroup_id)) << "\"," << hist_json(delta)
                      << ",\"reclaimed_pages\":"
                      << value.reclaimed_pages - previous.reclaimed_pages << "}\n";
        }
        rotate_if_full<reclaim_key>(fd, entries.size(), 4096, reclaim_previous_);
    }

    void drops() {
        const int fd = bpf_map__fd(skeleton_->maps.drops);
        const int cpus = libbpf_num_possible_cpus();
        if (cpus <= 0) return;
        std::vector<__u64> values(static_cast<std::size_t>(cpus));
        for (__u32 collector = 0; collector < GW_COLLECTORS; ++collector) {
            if (bpf_map_lookup_elem(fd, &collector, values.data()) != 0) continue;
            __u64 total{};
            for (const auto value : values) total += value;
            const auto delta = total - drops_previous_[collector];
            drops_previous_[collector] = total;
            if (delta) {
                std::cout << prefix(collector_names[collector]) << ",\"dropped_updates\":" << delta
                          << "}\n";
            }
        }
    }

    graywatch_bpf* skeleton_;
    const Options& options_;
    std::map<std::string, bool> enabled_;
    Symbols symbols_;
    Cgroups cgroups_;
    std::string timestamp_;
    double elapsed_{};
    std::map<std::string, std::string> fuse_previous_;
    std::map<std::string, std::string> block_previous_;
    std::map<std::string, std::string> offcpu_previous_;
    std::map<std::string, std::string> runqueue_previous_;
    std::map<std::string, std::string> tcp_previous_;
    std::map<std::string, std::string> reclaim_previous_;
    __u64 drops_previous_[GW_COLLECTORS]{};
};

void print_status(const std::vector<CollectorPlan>& plans, bool symbols) {
    std::cout << "{\"schema_version\":1,\"event_type\":\"bpf_status\",\"timestamp\":\""
              << timestamp_utc() << "\",\"kernel_symbols\":" << (symbols ? "true" : "false")
              << ",\"collectors\":[";
    for (std::size_t index = 0; index < plans.size(); ++index) {
        const auto& plan = plans[index];
        std::cout << (index ? "," : "") << "{\"name\":\"" << plan.name << "\",\"enabled\":"
                  << (plan.enabled ? "true" : "false");
        if (!plan.reason.empty()) {
            std::cout << (plan.enabled ? ",\"hook\":\"" : ",\"reason\":\"") << escape(plan.reason)
                      << '"';
        }
        if (!plan.notes.empty() && plan.enabled) {
            std::cout << ",\"notes\":[";
            for (std::size_t note = 0; note < plan.notes.size(); ++note) {
                std::cout << (note ? "," : "") << '"' << escape(plan.notes[note]) << '"';
            }
            std::cout << ']';
        }
        std::cout << '}';
    }
    std::cout << "]}\n" << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
    libbpf_set_print(capture_libbpf);
    if (geteuid() != 0) {
        std::cerr << "graywatch-bpf: must run as root (CAP_BPF and CAP_PERFMON)\n";
        return 1;
    }
    KernelBtf btf;
    if (!btf.available()) {
        std::cerr << "graywatch-bpf: kernel BTF (/sys/kernel/btf/vmlinux) unavailable\n";
        return 1;
    }
    auto plans = plan_collectors(options, btf);

    graywatch_bpf* skeleton = load_with(programs_of(plans));
    if (!skeleton) {
        // Find the collectors the verifier rejects by loading each alone.
        for (auto& plan : plans) {
            if (!plan.enabled) continue;
            libbpf_log.clear();
            std::set<std::string> programs(plan.programs.begin(), plan.programs.end());
            graywatch_bpf* trial = load_with(programs);
            if (!trial) {
                plan.enabled = false;
                plan.reason = "load failed: " + last_log_line();
                continue;
            }
            graywatch_bpf__destroy(trial);
            // Optional programs (e.g. queue time, memcg reclaim) may fail alone.
            programs.insert(plan.optional_programs.begin(), plan.optional_programs.end());
            if (!plan.optional_programs.empty() && !(trial = load_with(programs))) {
                plan.notes.push_back("optional hooks failed to load: " + last_log_line());
                plan.optional_programs.clear();
            } else if (trial) {
                graywatch_bpf__destroy(trial);
            }
        }
        skeleton = load_with(programs_of(plans));
        if (!skeleton) {
            std::cerr << "graywatch-bpf: no collector could be loaded: " << last_log_line() << '\n';
            return 1;
        }
    }

    std::vector<bpf_link*> links;
    for (auto& plan : plans) {
        if (!plan.enabled) continue;
        std::vector<bpf_link*> attached;
        bool failed = false;
        std::vector<std::string> names = plan.programs;
        names.insert(names.end(), plan.optional_programs.begin(), plan.optional_programs.end());
        for (const auto& name : names) {
            bpf_program* program = bpf_object__find_program_by_name(skeleton->obj, name.c_str());
            bpf_link* link = program ? bpf_program__attach(program) : nullptr;
            if (!link) {
                const bool optional = std::find(plan.optional_programs.begin(),
                                                plan.optional_programs.end(), name) !=
                                      plan.optional_programs.end();
                if (optional) {
                    plan.notes.push_back("could not attach " + name);
                    continue;
                }
                failed = true;
                plan.reason = "attach " + name + " failed: " + std::strerror(errno);
                break;
            }
            attached.push_back(link);
        }
        if (failed) {
            for (auto* link : attached) bpf_link__destroy(link);
            plan.enabled = false;
            continue;
        }
        links.insert(links.end(), attached.begin(), attached.end());
    }

    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    Emitter emitter(skeleton, options, plans);
    print_status(plans, emitter.symbols_available());

    const auto started = std::chrono::steady_clock::now();
    auto previous = started;
    auto next = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(options.interval));
    while (running) {
        while (running && std::chrono::steady_clock::now() < next) {
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
                next - std::chrono::steady_clock::now(), std::chrono::milliseconds(100)));
        }
        if (!running) break;
        const auto now = std::chrono::steady_clock::now();
        emitter.interval(std::chrono::duration<double>(now - previous).count());
        previous = now;
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(options.interval));
        if (options.duration > 0 &&
            std::chrono::duration<double>(now - started).count() >= options.duration) {
            break;
        }
    }
    for (auto* link : links) bpf_link__destroy(link);
    graywatch_bpf__destroy(skeleton);
    return 0;
}
