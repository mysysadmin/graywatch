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
