#include "graywatch/json.hpp"
#include "graywatch/report.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

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
}

}  // namespace

int main() {
    try {
        test_json();
        test_findings();
        test_topology();
        test_ports_and_conclusions();
        test_kernel_log();
        test_bpf();
        test_render();
    } catch (const std::exception& error) {
        std::cerr << "report_test: " << error.what() << '\n';
        return 1;
    }
}
