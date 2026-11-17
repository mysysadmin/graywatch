#include "graywatch/json.hpp"
#include "graywatch/report.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

using graywatch::report::Files;
using graywatch::report::Finding;
using graywatch::report::Report;
using graywatch::report::Severity;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string ts(int second, const char* milliseconds = "000") {
    char text[40];
    std::snprintf(text, sizeof(text), "2026-11-07T21:%02d:%02d.%sZ", second / 60, second % 60,
                  milliseconds);
    return text;
}

std::string cpu_event(int second, bool blocked) {
    const bool fresh = second % 5 == 0;
    std::string threads;
    if (blocked) {
        threads += R"({"pid":4242,"tid":4243,"uid":1000,"command":"app","state":"D",)"
                   R"("wait_channel":"request_wait_answer","cpu_ms":1.0,"runqueue_wait_ms":0.0,)"
                   R"("runnable_wait_percent":0.0,"timeslices":1},)";
    }
    threads += R"({"pid":77,"tid":77,"uid":0,"command":"kworker","state":"S",)"
               R"("wait_channel":"","cpu_ms":10.0,"runqueue_wait_ms":2.0,)"
               R"("runnable_wait_percent":16.0,"timeslices":4})";
    return R"({"schema_version":1,"event_type":"resource_pressure","timestamp":")" +
           ts(second, "642") + R"(","resource":"cpu","condition":"unavailable","severity":0.0,)"
           R"("interval_seconds":1.0,"evidence":{"psi_available":false,)"
           R"("psi_some_stall_percent":0.0,"psi_full_stall_percent":0.0},)"
           R"("selection":{"kind":"system","value":"all","attribution_available":)" +
           std::string(second >= 5 ? "true" : "false") +
           R"(,"attribution_interval_seconds":5.0,"attribution_age_seconds":)" +
           std::to_string(fresh ? 0 : second % 5) +
           R"(,"observed_threads":900},"affected_threads":[)" + threads + "]}";
}

std::string io_event(int second, const std::string& name, double latency) {
    const bool degraded = latency >= 100.0;
    return R"({"schema_version":1,"resource":"io","timestamp":")" + ts(second) +
           R"(","condition":")" + (degraded ? "degraded" : "healthy") + R"(","severity":)" +
           (degraded ? "0.8" : "0.02") + R"(,"interval_seconds":1.0,"device":{"name":")" + name +
           R"("},"evidence":{"assessment_reason":")" +
           (degraded ? "absolute_latency" : "responsive") +
           R"(","maximum_average_latency_ms":)" + std::to_string(latency) +
           R"(,"read_bytes_per_second":4096.0,"write_bytes_per_second":0.0,)"
           R"("busy_percent":10.0,"average_queue_depth":0.1}})";
}

std::string memory_event(int second, bool oom) {
    return R"({"resource":"memory","timestamp":")" + ts(second) + R"(","condition":")" +
           (oom ? "degraded" : "healthy") + R"(","severity":)" + (oom ? "0.95" : "0.01") +
           R"(,"interval_seconds":1.0,"evidence":{"assessment_reason":")" +
           (oom ? "oom_kill" : "no_pressure") +
           R"(","total_kib":1048576,"available_percent":50.0,"oom_kills":)" +
           (oom ? "1" : "0") + "}}";
}

std::string ib_event(int second, int port, const char* state, const char* condition) {
    return R"({"resource":"network","timestamp":")" + ts(second) + R"(","condition":")" +
           condition + R"(","severity":0.0,"interface":{"kind":"infiniband","device":"mlx4_0",)"
           R"("port":)" + std::to_string(port) + R"(},"evidence":{"state":")" + state +
           R"(","rate":"56 Gb/sec"}})";
}

Files bundle(bool include_dm = true) {
    std::string events;
    for (int second = 1; second <= 40; ++second) {
        events += cpu_event(second, second >= 10 && second <= 35) + "\n";
        events += memory_event(second, second == 20) + "\n";
        events += io_event(second, "nvme0n1", 0.3) + "\n";
        if (include_dm) {
            events += io_event(second, "dm-0", second >= 12 && second <= 16 ? 250.0 : 0.4) + "\n";
        }
        events += ib_event(second, 1, "ACTIVE", "healthy") + "\n";
        events += ib_event(second, 2, "DOWN", "inactive") + "\n";
    }
    events += "not json\n";
    return {
        {"events.jsonl", events},
        {"system.txt", "collected_utc=2026-11-07T21:00:00Z\nduration_seconds=40\nhostname=nodea\n"
                       "uid=0\nkernel_release=5.14.0\ncpus=32\npsi_status=disabled_by_default\n"
                       "os_PRETTY_NAME=\"Rocky Linux 9.6\"\n"},
        {"lsblk.json", R"({"blockdevices":[{"name":"nvme0n1","kname":"nvme0n1","type":"disk",)"
                       R"("children":[{"name":"vg-data","kname":"dm-0","type":"lvm"}]}]})"},
        {"kernel.log", "2026-11-01T08:00:00,000000+01:00 Out of memory: Killed process 12 (old)\n"
                       "2026-11-07T20:59:00,000000+00:00 INFO: task app:4243 blocked for more "
                       "than 122 seconds.\n"
                       "2026-11-07T22:00:20,5+01:00 Out of memory: Killed process 999 (hog)\n"},
        {"daemon.stderr", ""},
    };
}

const Finding* find(const Report& report, const std::string& needle) {
    for (const auto& finding : report.findings) {
        if (finding.title.find(needle) != std::string::npos) return &finding;
    }
    return nullptr;
}

bool any_contains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
        return line.find(needle) != std::string::npos;
    });
}

void test_json() {
    const auto value = graywatch::json::parse(
        R"({"a":[1,2.5e1,-3],"b":"x\"\u00e9\ud83d\ude00","c":{"d":true,"e":null}})");
    require(value["a"].array().size() == 3 && value["a"].array()[1].number() == 25.0,
            "array/number parse");
    require(value.string("b") == "x\"\xc3\xa9\xf0\x9f\x98\x80", "string escapes");
    require(value["c"].boolean("d", false) && value["c"]["e"].is_null(), "nested object");
    require(value["missing"]["deeper"].number(7.0) == 7.0, "missing lookup default");
    require(value["c"].members().size() == 2, "members");
    for (const char* bad : {"{", "[1,]", "{\"a\":1} x", "\"\\q\"", "01x", "{\"a\" 1}"}) {
        bool threw = false;
        try {
            (void)graywatch::json::parse(bad);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, std::string("accepted malformed JSON: ") + bad);
    }
    std::string deep(100, '[');
    deep += std::string(100, ']');
    bool threw = false;
    try {
        (void)graywatch::json::parse(deep);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "unbounded nesting accepted");
    require(graywatch::json::escape("a\"b\n\x01") == "a\\\"b\\n\\u0001", "escape");
}

void test_findings() {
    const auto report = graywatch::report::analyze(bundle());
    require(report.findings.front().severity == Severity::critical, "most severe first");
    require(find(report, "OOM killer ran during the window") != nullptr, "OOM finding");
    const auto* blocked = find(report, "uninterruptible");
    require(blocked && blocked->severity == Severity::critical, "D-state finding is critical");
    require(any_contains(blocked->next_steps, "FUSE"), "FUSE next step");
    const auto* psi = find(report, "PSI");
    require(psi && any_contains(psi->next_steps, "grubby --update-kernel=ALL --args=psi=1"),
            "PSI enable instructions");
    require(any_contains(report.coverage, "1 line in events.jsonl"), "invalid line counted");
    require(any_contains(report.coverage, "No eBPF data"), "missing eBPF coverage note");
}

void test_topology() {
    const auto report = graywatch::report::analyze(bundle());
    std::vector<const Finding*> devices;
    for (const auto& finding : report.findings) {
        if (finding.area == "io") devices.push_back(&finding);
    }
    require(devices.size() == 1, "one device finding");
    require(devices[0]->title.find("dm-0 (vg-data) on nvme0n1") != std::string::npos,
            "stacked device label: " + devices[0]->title);
    require(any_contains(devices[0]->next_steps, "stacking layer"), "stacking layer hint");
}

void test_ports_and_conclusions() {
    auto report = graywatch::report::analyze(bundle());
    const auto* ports = find(report, "InfiniBand");
    require(ports && ports->severity == Severity::info &&
                ports->title == "Unused InfiniBand ports ignored",
            "inactive port is informational");
    require(!any_contains(report.conclusions, "above the local block layer"),
            "FUSE conclusion despite unhealthy disk");
    report = graywatch::report::analyze(bundle(false));
    require(any_contains(report.conclusions, "above the local block layer"),
            "FUSE conclusion with healthy disks");
}

void test_kernel_log() {
    auto report = graywatch::report::analyze(bundle());
    const auto* oom = find(report, "Kernel log: oom");
    require(oom && oom->severity == Severity::critical, "recent OOM critical");
    require(any_contains(oom->details, "1 older line"), "older OOM line counted");
    require(find(report, "Kernel log: hung task") != nullptr, "hung task matched");
    auto files = bundle();
    files["kernel.log"] = "2026-10-01T08:00:00,000000+00:00 Out of memory: Killed process 12\n";
    report = graywatch::report::analyze(files);
    const auto* history = find(report, "Kernel log");
    require(history && history->severity == Severity::info &&
                history->title == "Kernel log: earlier oom (1 line, before the window)",
            "old kernel line is history");

    const std::string up =
        "2026-11-07T21:00:05,000000+00:00 bnxt_en 0000:13:00.0 ens3f0np0: NIC Link is Up, "
        "10000 Mbps\n";
    files["kernel.log"] = up;
    report = graywatch::report::analyze(files);
    const auto* boot = find(report, "Kernel log: links came up");
    require(boot && boot->severity == Severity::info, "link up only is not a flap");
    require(find(report, "Kernel log: link flap") == nullptr, "no flap without link down");
    files["kernel.log"] =
        "2026-11-07T21:00:03,000000+00:00 bnxt_en 0000:13:00.0 ens3f0np0: NIC Link is Down\n" + up;
    report = graywatch::report::analyze(files);
    const auto* flap = find(report, "Kernel log: link flap");
    require(flap && flap->severity == Severity::warning, "link down then up is a flap");

    // Platform faults that are not storage-device errors.
    const std::string at = "2026-11-07T21:00:10,000000+00:00 ";
    files["kernel.log"] =
        at + "sd 0:0:12:0: timing out command, waited 180s\n" +
        at + "mpt3sas_cm0: sending diag reset !!\n" +
        at + "pcieport 0000:00:03.0: AER: Corrected error received: 0000:05:00.0\n" +
        at + "nf_conntrack: nf_conntrack: table full, dropping packet\n" +
        at + "NETDEV WATCHDOG: ens3f0 (ixgbe): transmit queue 4 timed out\n" +
        at + "bond0: (slave ens3f1): link status definitely down, disabling slave\n" +
        at + "mlx5_core 0000:05:00.0: cmd_work_handler:877: timeout. Will cause a leak\n" +
        at + "mlx5_core 0000:05:00.0 mlx5_0: Port: 1 Link ACTIVE\n";
    report = graywatch::report::analyze(files);
    for (const auto* title : {"Kernel log: SCSI/SAS controller (2 lines)", "Kernel log: PCIe error",
                              "Kernel log: conntrack table full", "Kernel log: NIC transmit timeout",
                              "Kernel log: bond member down", "Kernel log: mlx5 error (1 line)"}) {
        require(find(report, title) != nullptr, std::string("kernel pattern: ") + title);
    }
    require(find(report, "Kernel log: links came up") != nullptr,
            "mlx5 port up is a link event, not an mlx5 error");
}

void test_bpf() {
    auto files = bundle(false);
    files["bpf-events.jsonl"] =
        R"({"schema_version":1,"event_type":"bpf_status","collectors":[{"name":"fuse","enabled":true},)"
        R"({"name":"runqueue","enabled":false,"reason":"tracepoint missing"}]})" "\n"
        R"({"event_type":"bpf_interval","collector":"fuse","timestamp":"2026-11-07T21:00:12.000Z",)"
        R"("mount":"/mnt/quobyte","opcode":"GETATTR","count":10,"total_ms":3000,"max_ms":1500,)"
        R"("histogram_us":{"1024":8,"2097152":2},"errors":0,"outstanding":1,"oldest_outstanding_ms":7000})" "\n"
        R"({"event_type":"bpf_interval","collector":"block","timestamp":"2026-11-07T21:00:12.000Z",)"
        R"("device":"nvme0n1","operation":"read","errors":0,"direct_issue":0,)"
        R"("service":{"count":100,"total_ms":20,"max_ms":0.5,"histogram_us":{"256":100}},)"
        R"("queue":{"count":100,"total_ms":5000,"max_ms":90,"histogram_us":{"65536":100}}})" "\n"
        R"({"event_type":"bpf_interval","collector":"offcpu","outstanding":true,"tid":4243,"pid":4242,)"
        R"("command":"app","blocked_ms":12000,"stack":["request_wait_answer","fuse_simple_request","fuse_do_getattr"]})" "\n"
        R"({"event_type":"bpf_interval","collector":"tcp_retransmit","source":"10.0.0.1:40000",)"
        R"("destination":"10.0.0.9:7861","state":"ESTABLISHED","retransmits":150})" "\n"
        R"({"event_type":"bpf_interval","collector":"reclaim","kind":"memcg","pid":5,"command":"java",)"
        R"("cgroup":"/app.slice","count":20,"total_ms":2000,"max_ms":300,"reclaimed_pages":1000})" "\n";
    const auto report = graywatch::report::analyze(files);
    const auto* stuck = find(report, "FUSE requests stuck");
    require(stuck && stuck->severity == Severity::critical, "stuck FUSE request critical");
    const auto* slow = find(report, "Slow FUSE operations");
    require(slow && slow->severity == Severity::critical, "slow FUSE p99 critical");
    require(any_contains(slow->details, "p99 <= 2097.2 ms"), "histogram percentile merged");
    require(find(report, "wait in the queue, not in the device") != nullptr, "queue vs service");
    const auto* offcpu = find(report, "eBPF kernel stacks");
    require(offcpu && offcpu->severity == Severity::critical &&
                any_contains(offcpu->details, "request_wait_answer < fuse_simple_request"),
            "outstanding D-state stack");
    require(find(report, "TCP retransmits on 1 connections") != nullptr, "TCP connections");
    const auto* reclaim = find(report, "Memory reclaim stalls attributed");
    require(reclaim && reclaim->severity == Severity::warning &&
                any_contains(reclaim->next_steps, "memory.high"),
            "memcg reclaim");
    require(any_contains(report.coverage, "runqueue (tracepoint missing)"), "disabled collector");
    require(any_contains(report.conclusions, "above the local block layer"),
            "eBPF FUSE slowness drives conclusion");
}

// Text and Markdown fit in 80 columns; only a single over-long word (a path)
// may exceed it, alone on its line.
void require_width(const Report& report) {
    using graywatch::report::Format;
    for (const auto format : {Format::text, Format::markdown}) {
        std::istringstream lines(graywatch::report::render(report, format));
        std::string line;
        while (std::getline(lines, line)) {
            if (line.size() <= graywatch::report::report_width) continue;
            const auto start = line.find_first_not_of(" *-");
            require(start != std::string::npos &&
                        line.find(' ', start) == std::string::npos,
                    "line wider than 80 columns: " + line);
        }
    }
}

// The fio-on-Quobyte incident: a CREATE never answered while disks were busy,
// InfiniBand congested, a disk-health tool's pass-through commands failed and
// a storage thread kept re-blocking on short reads.
void test_incident_triage() {
    auto files = bundle(true);  // dm-0 degraded: a critical disk finding exists
    std::string events = files["events.jsonl"];
    for (int second = 1; second <= 40; ++second) {
        events += R"({"resource":"network","timestamp":")" + ts(second) +
                  R"(","condition":"congested","severity":0.5,"interface":{"kind":"infiniband",)"
                  R"("device":"mlx4_0","port":1},"evidence":{"state":"ACTIVE","rate":"56 Gb/sec",)"
                  R"("transmit_wait":2000}})" "\n";
    }
    files["events.jsonl"] = events;
    files["bpf-events.jsonl"] =
        R"({"schema_version":1,"event_type":"bpf_status","collectors":[{"name":"fuse","enabled":true},)"
        R"({"name":"block","enabled":true},{"name":"offcpu","enabled":true}]})" "\n"
        R"({"event_type":"bpf_interval","collector":"fuse","timestamp":"2026-11-07T21:00:40.000Z",)"
        R"("mount":"/quobyte","opcode":"CREATE","count":0,"total_ms":0,"max_ms":0,"histogram_us":{},)"
        R"("errors":0,"outstanding":1,"oldest_outstanding_ms":482400,"outstanding_before_start":true})" "\n"
        R"({"event_type":"bpf_interval","collector":"block","timestamp":"2026-11-07T21:00:20.000Z",)"
        R"("device":"sda","operation":"drv_in","errors":6,"direct_issue":6,)"
        R"("service":{"count":6,"total_ms":2,"max_ms":0.8,"histogram_us":{"1024":6}},)"
        R"("queue":{"count":0,"total_ms":0,"max_ms":0,"histogram_us":{}}})" "\n"
        R"({"event_type":"bpf_interval","collector":"block","timestamp":"2026-11-07T21:00:21.000Z",)"
        R"("device":"sdb","operation":"other","errors":2,"direct_issue":2,)"
        R"("service":{"count":2,"total_ms":1,"max_ms":0.8,"histogram_us":{"1024":2}},)"
        R"("queue":{"count":0,"total_ms":0,"max_ms":0,"histogram_us":{}}})" "\n"
        R"({"event_type":"bpf_interval","collector":"offcpu","outstanding":false,"pid":4242,)"
        R"("command":"app","count":50000,"total_ms":90000,"max_ms":12,"stack":["io_schedule"]})" "\n";
    const auto report = graywatch::report::analyze(files);

    // 1. Pass-through failures are not disk errors.
    const auto* passthrough = find(report, "Pass-through commands to sda failed 6 times");
    require(passthrough && passthrough->severity == Severity::info, "pass-through errors are info");
    require(find(report, "Block device sda returned") == nullptr, "no critical sda error");
    const auto* unclassified = find(report, "errors on other requests");
    require(unclassified && unclassified->severity == Severity::warning,
            "unclassified request errors are a warning");

    // 2. Continuous short waits are busy, not stuck.
    const auto* blocked = find(report, "Threads in uninterruptible sleep");
    require(blocked && blocked->severity == Severity::info &&
                any_contains(blocked->details, "busy rather than stuck") &&
                any_contains(blocked->details, "the longest 12 ms"),
            "eBPF qualifies D-state snapshots");

    // 3. The unanswered request is reported with the platform evidence from
    //    the same window next to it, not above it; load-only network evidence
    //    is context.
    const auto* hung = find(report, "FUSE requests stuck without an answer");
    require(hung && hung->severity == Severity::critical, "hung request is critical");
    require(any_contains(hung->details, "already waiting when the collection"),
            "pre-existing request flagged");
    require(report.findings.front().area != "fuse",
            "platform findings of the same severity are not ranked below the hung request");
    require(!report.conclusions.empty() &&
                report.conclusions.front().starts_with(
                    "A FUSE CREATE request on /quobyte waited 482 s") &&
                report.conclusions.front().find("local block devices were degraded") !=
                    std::string::npos,
            "assessment names the platform evidence next to the stuck request");
    require(any_contains(report.coverage, "cannot clear the storage servers"),
            "one host cannot clear the storage servers");
    require(any_contains(report.conclusions, "common during heavy transfers"),
            "congestion is context");
    require(!any_contains(report.conclusions, "likely contributor"),
            "congestion alone is not called a likely cause");
    require_width(report);
}

// The daemon's fusectl view: a mount whose waiting count stays frozen.
void test_daemon_fuse() {
    auto files = bundle(false);
    std::string events = files["events.jsonl"];
    for (int second = 1; second <= 40; ++second) {
        const bool stalled = second >= 31;
        events += R"({"resource":"fuse","timestamp":")" + ts(second) + R"(","condition":")" +
                  (stalled ? "stalled" : "active") + R"(","severity":)" + (stalled ? "0.8" : "0.01") +
                  R"(,"mount":{"path":"/quobyte","filesystem":"fuse.quobyte","device":"0:49",)"
                  R"("connection":49},"evidence":{"waiting_requests":1,"unchanged_seconds":)" +
                  std::to_string(second - 1) + "}}\n";
        events += R"({"resource":"fuse","timestamp":")" + ts(second) +
                  R"(","condition":"unavailable","severity":0,"mount":{"path":"/mnt/other"},)"
                  R"("evidence":{"waiting_requests":0}})" "\n";
    }
    files["events.jsonl"] = events;
    const auto report = graywatch::report::analyze(files);
    const auto* stalled = find(report, "FUSE mount /quobyte made no progress");
    require(stalled && stalled->severity == Severity::critical &&
                any_contains(stalled->details, "no change for 39 s"),
            "daemon FUSE stall reported");
    require(any_contains(report.conclusions, "The FUSE mount /quobyte answered nothing for 39 s"),
            "daemon FUSE stall leads the assessment");
    require(any_contains(report.coverage, "/mnt/other: the daemon could not read"),
            "unreadable fusectl noted");
    require(!any_contains(stalled->details, "background requests"), "no eBPF, no guess");

    // eBPF ran and saw no synchronous request waiting: background requests.
    files["bpf-events.jsonl"] =
        R"({"event_type":"bpf_status","collectors":[{"name":"fuse","enabled":true}]})" "\n";
    const auto with_bpf = graywatch::report::analyze(files);
    const auto* background = find(with_bpf, "FUSE mount /quobyte: background requests waiting");
    require(background && background->severity == Severity::warning &&
                any_contains(background->details, "background requests") &&
                !any_contains(background->details, "eBPF FUSE findings name"),
            "stall without a synchronous wait is a background warning");
    require(!any_contains(with_bpf.conclusions, "answered nothing"),
            "a background stall does not lead the assessment");
}

// Limits per FUSE operation: listings and lock waits are not judged like
// lookups.
void test_fuse_limits() {
    auto files = bundle(false);
    const auto interval = [](const char* opcode, const char* histogram, double max_ms,
                             double oldest_ms) {
        return std::string(R"({"event_type":"bpf_interval","collector":"fuse",)"
                           R"("timestamp":"2026-11-07T21:00:12.000Z","mount":"/quobyte",)"
                           R"("opcode":")") + opcode + R"(","count":10,"total_ms":1420,"max_ms":)" +
               std::to_string(max_ms) + R"(,"histogram_us":)" + histogram +
               R"(,"errors":0,"outstanding":0,"oldest_outstanding_ms":)" +
               std::to_string(oldest_ms) + "}\n";
    };
    files["bpf-events.jsonl"] =
        R"({"event_type":"bpf_status","collectors":[{"name":"fuse","enabled":true}]})" "\n" +
        interval("READDIRPLUS", R"({"262144":10})", 240.0, 0.0) +
        interval("SETLKW", R"({})", 0.0, 60000.0) +
        interval("FSYNC", R"({"2097152":10})", 1900.0, 12000.0);
    const auto report = graywatch::report::analyze(files);
    const auto* slow = find(report, "Slow FUSE operations");
    require(slow && !any_contains(slow->details, "READDIRPLUS") &&
                any_contains(slow->details, "FSYNC"),
            "a 262 ms listing is normal, a 2 s fsync is slow");
    require(find(report, "FUSE requests stuck") == nullptr,
            "a 12 s fsync and a 60 s lock wait are not stuck");
    const auto* locks = find(report, "Long FUSE lock waits");
    require(locks && locks->severity == Severity::info, "lock waits are info");
}

// Retransmits concentrated on one peer are a fault on that peer or its path.
void test_retransmit_peer() {
    auto files = bundle(false);
    files["bpf-events.jsonl"] =
        R"({"event_type":"bpf_status","collectors":[{"name":"tcp_retransmit","enabled":true}]})" "\n"
        R"({"event_type":"bpf_interval","collector":"tcp_retransmit","source":"10.0.0.1:7863",)"
        R"("destination":"10.0.0.26:40000","state":"ESTABLISHED","retransmits":9000})" "\n"
        R"({"event_type":"bpf_interval","collector":"tcp_retransmit","source":"10.0.0.1:7863",)"
        R"("destination":"10.0.0.27:40000","state":"ESTABLISHED","retransmits":300})" "\n";
    const auto report = graywatch::report::analyze(files);
    const auto* peer = find(report, "TCP retransmits concentrated on 10.0.0.26");
    require(peer && peer->severity == Severity::warning &&
                any_contains(peer->details, "97% of all retransmits"),
            "concentrated retransmits name the peer");
}

void test_device_grouping() {
    auto files = bundle(false);
    std::string events = files["events.jsonl"];
    std::string bpf =
        R"({"event_type":"bpf_status","collectors":[{"name":"block","enabled":true}]})" "\n";
    for (int disk = 0; disk < 12; ++disk) {
        const std::string name = std::string("sd") + static_cast<char>('b' + disk);
        for (int second = 1; second <= 40; ++second) {
            const bool slow = second >= 12 && second <= 16;
            events += io_event(second, name, slow ? 150.0 + disk * 10.0 : 5.0) + "\n";
        }
        bpf += R"({"event_type":"bpf_interval","collector":"block","timestamp":")" + ts(20) +
               R"(","device":")" + name + R"(","operation":"flush","errors":0,"direct_issue":0,)"
               R"("service":{"count":100,"total_ms":3000,"max_ms":)" +
               std::to_string(100 + disk) + R"(,"histogram_us":{"65536":100}},)"
               R"("queue":{"count":100,"total_ms":1,"max_ms":0.1,"histogram_us":{"16":100}}})" "\n";
    }
    files["events.jsonl"] = events;
    files["bpf-events.jsonl"] = bpf;
    const auto report = graywatch::report::analyze(files);
    const auto* group = find(report, "12 of 13 active block devices non-healthy");
    require(group && group->severity == Severity::critical, "devices grouped above the limit");
    require(find(report, "Block device sdb:") == nullptr, "no per-device findings when grouped");
    require(any_contains(group->details, "Worst by p95") &&
                any_contains(group->details, "  sdm: p50"),
            "worst device listed first");
    const auto* flush = find(report, "12 devices are slow to service flush requests");
    require(flush && flush->severity == Severity::warning, "eBPF slow devices grouped");
    require(find(report, "Device sdb is slow") == nullptr, "no per-device eBPF findings");
    require_width(report);
}

void test_render() {
    const auto report = graywatch::report::analyze(bundle());
    using graywatch::report::Format;
    const auto text = graywatch::report::render(report, Format::text);
    require(text.find("Verdict: CRITICAL") != std::string::npos, "verdict");
    require(text.find("Timeline (UTC)") != std::string::npos, "timeline");
    require(graywatch::report::render(report, Format::markdown).find("## Findings") !=
                std::string::npos,
            "markdown heading");
    const auto json = graywatch::json::parse(graywatch::report::render(report, Format::json));
    require(json["findings"].array().size() == report.findings.size(), "json findings");
    require(json["system"].string("hostname") == "nodea", "json system");

    require_width(report);
}

void test_history() {
    auto files = bundle(false);
    std::string changes = R"({"timestamp":"2026-11-07T19:00:00.000Z","change":"daemon_started"})" "\n";
    for (int disk = 0; disk < 5; ++disk) {
        changes += std::string(R"({"timestamp":"2026-11-07T20:1)") + std::to_string(disk) +
                   R"(:00.000Z","key":"io/sd)" + static_cast<char>('b' + disk) +
                   R"(","resource":"io","change":"began","condition":"congested","severity":0.5,"event":null})" "\n";
    }
    changes += R"({"timestamp":"2026-11-07T20:30:00.000Z","key":"fuse//quobyte","resource":"fuse","change":"began","condition":"stalled","severity":0.8,"event":null})" "\n"
               R"({"timestamp":"2026-11-07T20:35:00.000Z","key":"fuse//quobyte","resource":"fuse","change":"ended","began":"2026-11-07T20:30:00.000Z","duration_seconds":300,"worst_condition":"stalled","worst_severity":0.8})" "\n"
               R"({"timestamp":"2026-11-07T20:59:00.000Z","change":"capture_requested"})" "\n";
    files["changes.jsonl"] = changes;
    files["trigger.json"] =
        R"({"timestamp":"2026-11-07T20:59:00.000Z","key":"io/sdb","condition":"degraded","severity":0.93,"sustained_seconds":60})" "\n";
    const auto report = graywatch::report::analyze(files);
    const auto* history = find(report, "Recorded before the collection: 6 problem periods");
    require(history != nullptr, "history finding");
    require(history->title.find("ongoing ones since") != std::string::npos, "ongoing span");
    require(any_contains(history->details, "io: 5 problem periods on 5 devices") &&
                any_contains(history->details, "5 still ongoing"),
            "devices summarized");
    require(any_contains(history->details, "fuse//quobyte stalled from 20:30:00 UTC for 5 min"),
            "single period listed");
    require(!report.conclusions.empty() &&
                report.conclusions.front().starts_with("Automatic collection: graywatchd asked"),
            "trigger leads the assessment");
    require_width(report);
}

void test_load_bundle() {
    // A support archive is read the same whether it is still compressed or
    // was unpacked to a plain .tar on the way.
    const auto root = std::filesystem::temp_directory_path() /
                      ("graywatch-report-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bundle");
    std::ofstream(root / "bundle" / "system.txt") << "kernel_release=7.0.0\n";
    const std::string dir = root.string();
    require(std::system(("tar -C " + dir + "/bundle -czf " + dir + "/a.tar.gz . && "
                         "tar -C " + dir + "/bundle -cf " + dir + "/a.tar .").c_str()) == 0,
            "tar failed");
    for (const char* name : {"a.tar.gz", "a.tar"}) {
        const auto files = graywatch::report::load_bundle(root / name);
        require(files.contains("system.txt") &&
                    files.at("system.txt") == "kernel_release=7.0.0\n",
                std::string("bundle not read from ") + name);
    }
    std::filesystem::remove_all(root);
}

}  // namespace

// Findings several hosts share are listed once.
void test_cluster() {
    using graywatch::report::Report;
    const auto host = [](const char* name, std::vector<Finding> findings) {
        Report report;
        report.system["hostname"] = name;
        report.findings = std::move(findings);
        return report;
    };
    const Finding peer{Severity::warning, "network", "TCP retransmits concentrated on 10.0.0.26",
                       {}, {}};
    auto fs1 = host("fs1", {peer, {Severity::warning, "io", "Block device sdl: saturated", {}, {}}});
    auto fs2 = host("fs2", {peer, {Severity::info, "cpu", "Threads in D state", {}, {}}});
    fs1.settings = {{"nic ib0 mtu", "4092"}, {"hard disk scheduler", "mq-deadline"}};
    fs2.settings = {{"nic ib0 mtu", "2044"}, {"hard disk scheduler", "mq-deadline"}};
    const auto text = graywatch::report::render_cluster({fs1, fs2});
    require(text.find("* nic ib0 mtu: 2044 on fs2; 4092 on fs1") != std::string::npos &&
                text.find("hard disk scheduler:") == std::string::npos,
            "settings that differ between hosts");
    require(text.find("[WARNING] TCP retransmits concentrated on 10.0.0.26 (all hosts)") !=
                std::string::npos,
            "shared finding listed once");
    require(text.find("* fs1:\n  * [WARNING] Block device sdl: saturated") != std::string::npos,
            "own finding per host");
    require(text.find("fs2: nothing beyond the shared findings") != std::string::npos &&
                text.find("Threads in D state") == std::string::npos,
            "info findings left out");
}

// Platform pointers from the collector's snapshots before and after the window.
void test_platform() {
    auto files = bundle(false);
    files["platform.txt"] =
        "pci 0000:05:00.0 ens1f0 driver=mlx5_core current_speed=16.0_GT/s_PCIe "
        "max_speed=16.0_GT/s_PCIe current_width=8 max_width=16 numa_node=0\n"
        "nic ens1f0 mtu=9000 operstate=up master=bond0\n"
        "nic ens1f1 mtu=1500 operstate=up master=bond0\n"
        "numa 0 cpus=0-3\nnuma 1 cpus=4-7\n"
        "block sda rotational=1 scheduler=mq-deadline wbt_lat_usec=75000 nr_requests=256 "
        "read_ahead_kb=128 max_sectors_kb=1280 write_cache=write_back\n"
        "block sdb rotational=1 scheduler=mq-deadline wbt_lat_usec=75000 nr_requests=256 "
        "read_ahead_kb=128 max_sectors_kb=1280 write_cache=write_back\n"
        "block sdc rotational=1 scheduler=none wbt_lat_usec=75000 nr_requests=256 "
        "read_ahead_kb=128 max_sectors_kb=1280 write_cache=write_back\n"
        "sysctl net.core.netdev_max_backlog 1000\n";
    const auto links = [](int crc, int missed) {
        return R"([{"ifname":"ens1f0","stats64":{"rx":{"bytes":1,"packets":1,"crc_errors":)" +
               std::to_string(crc) + R"(,"missed_errors":)" + std::to_string(missed) +
               R"(,"dropped":0},"tx":{"errors":0}}}])";
    };
    files["links-initial.json"] = links(0, 0);
    files["links-final.json"] = links(12, 500);
    files["ethtool-stats-initial.txt"] =
        "== ens1f0\nNIC statistics:\n     rx_pause_ctrl_phy: 0\n     rx0_packets: 5\n";
    files["ethtool-stats-final.txt"] =
        "== ens1f0\nNIC statistics:\n     rx_pause_ctrl_phy: 40000\n     rx0_packets: 900\n";
    files["ethtool-settings.txt"] =
        "== ens1f0\nSettings for ens1f0:\n\tSupported link modes:   10000baseKR/Full\n"
        "\t                        25000baseCR/Full\n"
        "\tLink partner advertised link modes:  10000baseKR/Full\n"
        "\tSpeed: 10000Mb/s\n\tDuplex: Full\n";
    files["softnet-initial.txt"] = "00000010 00000000 00000001\n";
    files["softnet-final.txt"] = "00000fff 0000002a 00000009\n";
    files["interrupts-initial.txt"] =
        "           CPU0       CPU1       CPU2       CPU3       CPU4       CPU5       CPU6       CPU7\n"
        " 80:          0          0          0          0          0          0          0          0  "
        "IR-PCI-MSIX-0000:05:00.0   0-edge      mlx5_comp0@pci:0000:05:00.0\n"
        " 81:          0          0          0          0          0          0          0          0  "
        "IR-PCI-MSIX-0000:05:00.0   1-edge      mlx5_comp1@pci:0000:05:00.0\n";
    files["interrupts-final.txt"] =
        "           CPU0       CPU1       CPU2       CPU3       CPU4       CPU5       CPU6       CPU7\n"
        " 80:          0          0          0          0     900000          0          0          0  "
        "IR-PCI-MSIX-0000:05:00.0   0-edge      mlx5_comp0@pci:0000:05:00.0\n"
        " 81:          0          0          0          0       1000          0          0          0  "
        "IR-PCI-MSIX-0000:05:00.0   1-edge      mlx5_comp1@pci:0000:05:00.0\n";
    files["stat-initial.txt"] = "cpu 100 0 100 1000 0 0 0 0 0 0\n";
    files["stat-final.txt"] = "cpu 200 0 200 1500 0 0 0 120 0 0\n";
    files["throttle-initial.txt"] =
        "/sys/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count 3\n";
    files["throttle-final.txt"] =
        "/sys/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count 7\n";
    files["bonding.txt"] =
        "== bond0\nSlave Interface: ens1f0\nMII Status: up\nLink Failure Count: 0\n"
        "Slave Interface: ens1f1\nMII Status: down\nLink Failure Count: 3\n";
    const auto report = graywatch::report::analyze(files);
    const auto* nic = find(report, "NIC ens1f0 counted errors");
    require(nic && nic->severity == Severity::warning && any_contains(nic->details, "crc_errors +12") &&
                any_contains(nic->details, "missed_errors +500") &&
                !any_contains(nic->details, "rx0_packets"),
            "NIC errors and drops, per-queue counters skipped");
    require(find(report, "NIC ens1f0 sent or received pause frames") != nullptr, "pause frames");
    const auto* speed = find(report, "NIC ens1f0 runs at 10000 Mb/s of 25000 Mb/s");
    require(speed && any_contains(speed->details, "advertises up to 10000"), "link speed");
    require(find(report, "NICs use different MTUs") != nullptr, "MTU mismatch");
    require(find(report, "receive backlog") != nullptr, "softnet drops");
    require(find(report, "Interrupts of NIC ens1f0 concentrated on CPU 4") != nullptr,
            "interrupt concentration via PCI address");
    require(find(report, "NIC ens1f0 interrupts are handled on another NUMA node") != nullptr,
            "NUMA placement");
    require(find(report, "PCIe links narrower") != nullptr, "PCIe width");
    require(find(report, "thermally throttled") != nullptr, "throttling");
    require(find(report, "The hypervisor took 15% of the CPU time") != nullptr, "steal");
    require(find(report, "Bond bond0 has members down: ens1f1") != nullptr, "bond member");
    const auto* odd = find(report, "Block devices of the same kind");
    require(odd && any_contains(odd->details, "hard disk scheduler: 2 use mq-deadline; sdc=none"),
            "block setting outlier");
    require(report.settings.at("nic ens1f0 mtu") == "9000" &&
                report.settings.at("hard disk scheduler") == "mq-deadline (most)",
            "settings recorded for the cluster summary");
    require_width(report);
}

int main() {
    try {
        test_json();
        test_findings();
        test_topology();
        test_ports_and_conclusions();
        test_kernel_log();
        test_bpf();
        test_incident_triage();
        test_daemon_fuse();
        test_fuse_limits();
        test_retransmit_peer();
        test_device_grouping();
        test_render();
        test_load_bundle();
        test_history();
        test_cluster();
        test_platform();
    } catch (const std::exception& error) {
        std::cerr << "report_test: " << error.what() << '\n';
        return 1;
    }
}
