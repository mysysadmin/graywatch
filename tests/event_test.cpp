#include "graywatch/event.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const graywatch::CpuPressureEvent event{
            .timestamp = "2026-11-06T12:34:56.789Z",
            .interval_seconds = 1.0,
            .psi_available = true,
            .severity = 0.75,
            .some_stall_percent = 75.0,
            .full_stall_percent = 0.0,
            .selection_kind = "user",
            .selection_value = "test\"user",
            .attribution_available = true,
            .attribution_interval_seconds = 5.0,
            .attribution_age_seconds = 1.0,
            .selected_observed_threads = 4,
            .selected_active_threads = 2,
            .selected_waiting_threads = 1,
            .selected_blocked_threads = 1,
            .affected_threads = {{
                .pid = 10,
                .tid = 11,
                .uid = 1000,
                .command = "worker\\one",
                .state = 'D',
                .wait_channel = "request_wait_answer",
                .runtime_ns = 20'000'000,
                .runqueue_wait_ns = 5'000'000,
                .timeslices = 3,
                .runnable_wait_percent = 20.0,
            }},
        };
        const std::string json = graywatch::to_json(event);
        require(json.find("\"schema_version\":1") != std::string::npos,
                "schema version is absent");
        require(json.find("\"severity\":0.750000") != std::string::npos,
                "severity is absent");
        require(json.find("test\\\"user") != std::string::npos,
                "quote was not escaped");
        require(json.find("worker\\\\one") != std::string::npos,
                "backslash was not escaped");
        require(json.find("\"observed_threads\":4") != std::string::npos,
                "observed thread count is absent");
        require(json.find("\"attribution_age_seconds\":1.000000") !=
                    std::string::npos,
                "attribution age is absent");
        require(json.find("\"runnable_wait_percent\":20.000000") !=
                    std::string::npos,
                "thread evidence is absent");
        require(json.find("\"blocked_threads\":1") != std::string::npos,
                "blocked thread count is absent");
        require(json.find("request_wait_answer") != std::string::npos,
                "wait channel is absent");

        const graywatch::IoDeviceEvent io_event{
            .timestamp = "2026-11-06T12:34:56.789Z",
            .interval_seconds = 1.0,
            .device = {.name = "nvme0n1", .major = 259, .minor = 0,
                       .model = "Test NVMe", .read_iops = 100.0,
                       .write_iops = 20.0, .average_read_latency_ms = 0.2,
                       .busy_percent = 25.0, .average_queue_depth = 0.5,
                       .io_in_progress = 2},
            .assessment = {.condition = graywatch::IoCondition::healthy,
                           .severity = 0.05,
                           .maximum_average_latency_ms = 0.2,
                           .reason = "responsive"},
            .psi_available = false,
        };
        const auto io_json = graywatch::to_json(io_event);
        require(io_json.find("\"resource\":\"io\"") != std::string::npos,
                "I/O resource absent");
        require(io_json.find("\"name\":\"nvme0n1\"") != std::string::npos,
                "device absent");
        require(io_json.find("\"psi_available\":false") != std::string::npos,
                "PSI availability absent");
        require(io_json.find("\"condition\":\"healthy\"") != std::string::npos,
                "I/O assessment absent");
        require(graywatch::json_array({json, io_json}).front() == '[',
                "JSON array not produced");
    } catch (const std::exception& error) {
        std::cerr << "event_test: " << error.what() << '\n';
        return 1;
    }
}
