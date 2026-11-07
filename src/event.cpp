#include "graywatch/event.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace graywatch {
namespace {

std::string json_escape(std::string_view input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec << std::setfill(' ');
                } else {
                    output << character;
                }
        }
    }
    return output.str();
}

void json_string(std::ostringstream& output, std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

}  // namespace

std::string current_timestamp_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << milliseconds.count() << 'Z';
    return output.str();
}

std::string to_json(const CpuPressureEvent& event) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\"schema_version\":" << CpuPressureEvent::schema_version
           << ",\"event_type\":\"resource_pressure\",\"timestamp\":";
    json_string(output, event.timestamp);
    output << ",\"resource\":\"cpu\",\"condition\":\""
           << (!event.psi_available
                   ? "unavailable"
                   : (event.some_stall_percent == 0.0 ? "no_pressure" : "pressure"))
           << "\",\"severity\":" << event.severity
           << ",\"interval_seconds\":" << event.interval_seconds
           << ",\"evidence\":{\"psi_available\":"
           << (event.psi_available ? "true" : "false")
           << ",\"psi_some_stall_percent\":"
           << event.some_stall_percent
           << ",\"psi_full_stall_percent\":" << event.full_stall_percent
           << "},\"selection\":{\"kind\":";
    json_string(output, event.selection_kind);
    output << ",\"value\":";
    json_string(output, event.selection_value);
    output << ",\"attribution_available\":"
           << (event.attribution_available ? "true" : "false")
           << ",\"attribution_interval_seconds\":"
           << event.attribution_interval_seconds
           << ",\"attribution_age_seconds\":" << event.attribution_age_seconds
           << ",\"observed_threads\":" << event.selected_observed_threads
           << ",\"active_threads\":" << event.selected_active_threads
           << ",\"waiting_threads\":" << event.selected_waiting_threads
           << ",\"blocked_threads\":" << event.selected_blocked_threads
           << "},\"affected_threads\":[";

    bool first = true;
    for (const auto& thread : event.affected_threads) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << "{\"pid\":" << thread.pid << ",\"tid\":" << thread.tid
               << ",\"uid\":" << thread.uid << ",\"command\":";
        json_string(output, thread.command);
        output << ",\"state\":";
        json_string(output, std::string(1, thread.state));
        output << ",\"wait_channel\":";
        json_string(output, thread.wait_channel);
        output << ",\"cpu_ms\":"
               << static_cast<double>(thread.runtime_ns) / 1'000'000.0
               << ",\"runqueue_wait_ms\":"
               << static_cast<double>(thread.runqueue_wait_ns) / 1'000'000.0
               << ",\"runnable_wait_percent\":"
               << thread.runnable_wait_percent
               << ",\"timeslices\":" << thread.timeslices << '}';
    }
    output << "]}";
    return output.str();
}

std::string to_json(const IoDeviceEvent& event) {
    const auto& device = event.device;
    const auto& assessment = event.assessment;
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\"schema_version\":" << IoDeviceEvent::schema_version
           << ",\"event_type\":\"resource_health\",\"timestamp\":";
    json_string(output, event.timestamp);
    output << ",\"resource\":\"io\",\"condition\":\""
           << to_string(assessment.condition) << "\",\"severity\":"
           << assessment.severity
           << ",\"interval_seconds\":" << event.interval_seconds
           << ",\"device\":{\"name\":";
    json_string(output, device.name);
    output << ",\"major\":" << device.major << ",\"minor\":" << device.minor
           << ",\"model\":";
    json_string(output, device.model);
    output << "},\"evidence\":{\"source\":\"sysfs_block_stat\""
           << ",\"assessment_reason\":";
    json_string(output, assessment.reason);
    output << ",\"maximum_average_latency_ms\":"
           << assessment.maximum_average_latency_ms
           << ",\"read_iops\":" << device.read_iops
           << ",\"write_iops\":" << device.write_iops
           << ",\"discard_iops\":" << device.discard_iops
           << ",\"flush_iops\":" << device.flush_iops
           << ",\"read_bytes_per_second\":" << device.read_bytes_per_second
           << ",\"write_bytes_per_second\":" << device.write_bytes_per_second
           << ",\"average_read_latency_ms\":"
           << device.average_read_latency_ms
           << ",\"average_write_latency_ms\":"
           << device.average_write_latency_ms
           << ",\"average_discard_latency_ms\":"
           << device.average_discard_latency_ms
           << ",\"average_flush_latency_ms\":"
           << device.average_flush_latency_ms
           << ",\"busy_percent\":" << device.busy_percent
           << ",\"average_queue_depth\":" << device.average_queue_depth
           << ",\"io_in_progress\":" << device.io_in_progress
           << ",\"psi_available\":" << (event.psi_available ? "true" : "false")
           << ",\"psi_some_stall_percent\":" << event.psi_some_stall_percent
           << ",\"psi_full_stall_percent\":" << event.psi_full_stall_percent
           << "}}";
    return output.str();
}

IbPortAssessment assess_ib_port(const IbPortInterval& port, bool in_use) {
    const bool active = port.state == "ACTIVE";
    const std::uint64_t errors = port.receive_errors + port.transmit_discards +
                                 port.symbol_errors + port.link_downed;
    if (!active) {
        // A link that went down during the interval is a failure even if the
        // port had never carried traffic before.
        if (in_use || port.link_downed != 0) return {"link_down", 0.8};
        return {"inactive", 0.0};
    }
    if (errors != 0) {
        return {"degraded",
                std::clamp(0.8 + static_cast<double>(errors) / 100.0, 0.8, 1.0)};
    }
    if (port.transmit_wait != 0) return {"congested", 0.5};
    return {"healthy", 0.01};
}

std::string to_json(const IbPortEvent& event) {
    const auto& port = event.port;
    const auto [condition, severity] = assess_ib_port(port, event.in_use);
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\"schema_version\":" << IbPortEvent::schema_version
           << ",\"event_type\":\"resource_health\",\"timestamp\":";
    json_string(output, event.timestamp);
    output << ",\"resource\":\"network\",\"condition\":\"" << condition
           << "\",\"severity\":" << severity
           << ",\"interval_seconds\":" << event.interval_seconds
           << ",\"interface\":{";
    output << "\"kind\":\"infiniband\",\"device\":";
    json_string(output, port.device);
    output << ",\"port\":" << port.port << "},\"evidence\":{\"in_use\":"
           << (event.in_use ? "true" : "false") << ",\"state\":";
    json_string(output, port.state);
    output << ",\"physical_state\":";
    json_string(output, port.physical_state);
    output << ",\"rate\":";
    json_string(output, port.rate);
    output << ",\"receive_errors\":" << port.receive_errors
           << ",\"transmit_discards\":" << port.transmit_discards
           << ",\"symbol_errors\":" << port.symbol_errors
           << ",\"link_downed\":" << port.link_downed
           << ",\"transmit_wait\":" << port.transmit_wait << "}}";
    return output.str();
}

std::string to_json(const TcpEvent& event) {
    const auto& tcp = event.interval;
    const bool error = tcp.input_errors != 0;
    const bool unstable = tcp.retransmitted_segments >= 10 &&
                          tcp.retransmit_percent >= 1.0;
    const std::string_view condition = error ? "degraded" :
        (unstable ? "unstable" : "healthy");
    const double severity = error ? 0.8 :
        (unstable ? std::clamp(tcp.retransmit_percent / 10.0, 0.4, 1.0) : 0.01);
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\"schema_version\":" << TcpEvent::schema_version
           << ",\"event_type\":\"resource_health\",\"timestamp\":";
    json_string(output, event.timestamp);
    output << ",\"resource\":\"network\",\"condition\":\"" << condition
           << "\",\"severity\":" << severity
           << ",\"interval_seconds\":" << event.interval_seconds
           << ",\"interface\":{\"kind\":\"tcp\",\"device\":\"system\"}"
           << ",\"evidence\":{\"out_segments\":" << tcp.out_segments
           << ",\"retransmitted_segments\":" << tcp.retransmitted_segments
           << ",\"retransmit_percent\":" << tcp.retransmit_percent
           << ",\"input_errors\":" << tcp.input_errors
           << ",\"output_resets\":" << tcp.output_resets << "}}";
    return output.str();
}

std::string to_json(const MemoryEvent& event) {
    const auto& memory = event.interval;
    const auto& assessment = event.assessment;
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\"schema_version\":" << MemoryEvent::schema_version
           << ",\"event_type\":\"resource_health\",\"timestamp\":";
    json_string(output, event.timestamp);
    output << ",\"resource\":\"memory\",\"condition\":\""
           << to_string(assessment.condition) << "\",\"severity\":"
           << assessment.severity
           << ",\"interval_seconds\":" << event.interval_seconds
           << ",\"evidence\":{\"source\":\"proc_meminfo_vmstat\""
           << ",\"assessment_reason\":";
    json_string(output, assessment.reason);
    output << ",\"total_kib\":" << memory.total_kib
           << ",\"available_kib\":" << memory.available_kib
           << ",\"available_percent\":" << memory.available_percent
           << ",\"swap_total_kib\":" << memory.swap_total_kib
           << ",\"swap_used_kib\":" << memory.swap_used_kib
           << ",\"dirty_kib\":" << memory.dirty_kib
           << ",\"writeback_kib\":" << memory.writeback_kib
           << ",\"major_faults_per_second\":" << memory.major_faults_per_second
           << ",\"swap_in_pages_per_second\":" << memory.swap_in_pages_per_second
           << ",\"swap_out_pages_per_second\":" << memory.swap_out_pages_per_second
           << ",\"direct_reclaim_stalls\":" << memory.direct_reclaim_stalls
           << ",\"direct_scanned_pages_per_second\":"
           << memory.direct_scanned_pages_per_second
           << ",\"kswapd_scanned_pages_per_second\":"
           << memory.kswapd_scanned_pages_per_second
           << ",\"compaction_stalls\":" << memory.compaction_stalls
           << ",\"file_refaults_per_second\":" << memory.file_refaults_per_second
           << ",\"oom_kills\":" << memory.oom_kills
           << ",\"psi_available\":" << (event.psi_available ? "true" : "false")
           << ",\"psi_some_stall_percent\":" << event.psi_some_stall_percent
           << ",\"psi_full_stall_percent\":" << event.psi_full_stall_percent
           << "}}";
    return output.str();
}

std::string json_array(const std::vector<std::string>& values) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ',';
        output << values[index];
    }
    output << ']';
    return output.str();
}

}  // namespace graywatch
