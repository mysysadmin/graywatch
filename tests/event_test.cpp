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
            .severity = 0.75,
            .some_stall_percent = 75.0,
            .full_stall_percent = 0.0,
            .selection_kind = "user",
            .selection_value = "test\"user",
            .selected_active_threads = 2,
            .selected_waiting_threads = 1,
            .affected_threads = {{
                .pid = 10,
                .tid = 11,
                .uid = 1000,
                .command = "worker\\one",
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
        require(json.find("\"runnable_wait_percent\":20.000000") !=
                    std::string::npos,
                "thread evidence is absent");
    } catch (const std::exception& error) {
        std::cerr << "event_test: " << error.what() << '\n';
        return 1;
    }
}
