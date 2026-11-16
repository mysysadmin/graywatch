#include "graywatch/recorder.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path fresh_directory(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("graywatch-recorder-" + name + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::string> lines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) result.push_back(line);
    return result;
}

std::vector<std::string> names(const std::filesystem::path& directory) {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        result.push_back(entry.path().filename().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string io_event(const char* condition, double severity) {
    std::ostringstream json;
    json << R"({"schema_version":1,"event_type":"resource_health","timestamp":"t","resource":"io","condition":")"
         << condition << R"(","severity":)" << severity << R"(,"evidence":{"severity":0.0}})";
    return json.str();
}

void test_parsers() {
    require(graywatch::parse_duration("90") == 90s, "bare seconds");
    require(graywatch::parse_duration("30m") == 30min, "minutes");
    require(graywatch::parse_duration("7d") == std::chrono::hours(168), "days");
    require(graywatch::parse_size("2G") == (2ull << 30), "gigabytes");
    require(graywatch::parse_size("500M") == 500ull * 1024 * 1024, "megabytes");
    for (const char* bad : {"", "m", "-5m", "5x", "0", "1.5h"}) {
        bool threw = false;
        try {
            (void)graywatch::parse_duration(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, std::string("accepted duration ") + bad);
    }
    const auto event = io_event("congested", 0.5);
    require(graywatch::json_string_field(event, "condition") == "congested", "condition field");
    require(graywatch::json_number_field(event, "severity") == 0.5, "top-level severity");
}

void test_recorder_rotation() {
    const auto directory = fresh_directory("rotation");
    graywatch::Compressor compressor;
    {
        // 30 minutes kept in 5-minute segments.
        graywatch::EventRecorder recorder(directory, 30min, 1ull << 30, compressor);
        const auto start = std::chrono::system_clock::time_point(std::chrono::hours(500000));
        for (int minute = 0; minute < 60; ++minute) {
            recorder.write(R"({"minute":)" + std::to_string(minute) + "}",
                           start + std::chrono::minutes(minute));
            recorder.flush();
        }
    }
    compressor.wait_all();
    const auto files = names(directory);
    // Twelve segments were written. At the last rotation (minute 55) those
    // that ended more than 30 minutes earlier were deleted: minutes 20..59
    // remain, at least the 30 minutes asked for.
    require(files.size() == 8, "kept " + std::to_string(files.size()) + " segments");
    for (const auto& name : files) {
        require(name.starts_with("events-") && name.ends_with(".jsonl.gz"),
                "segment not compressed: " + name);
    }
    std::filesystem::remove_all(directory);
}

void test_recorder_size_limit() {
    const auto directory = fresh_directory("size");
    graywatch::Compressor compressor;
    {
        graywatch::EventRecorder recorder(directory, 7 * 24h, 4096, compressor);
        const auto start = std::chrono::system_clock::time_point(std::chrono::hours(500000));
        // Random text, which gzip cannot shrink much.
        std::uint32_t seed = 1;
        auto line = [&] {
            std::string text(1000, ' ');
            for (auto& character : text) {
                seed = seed * 1103515245u + 12345u;
                character = static_cast<char>(33 + (seed >> 16) % 94);
            }
            return text;
        };
        for (int hour = 0; hour < 10; ++hour) {
            for (int index = 0; index < 3; ++index) recorder.write(line(), start + std::chrono::hours(hour));
            recorder.flush();
            compressor.wait_all();
        }
    }
    compressor.wait_all();
    std::uintmax_t total{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) total += entry.file_size();
    require(names(directory).size() < 10, "size limit deleted nothing");
    // At most the limit plus the segment being written.
    require(total <= 4096 + 3100, "recording kept " + std::to_string(total) + " bytes");
    std::filesystem::remove_all(directory);
}

void test_change_log() {
    const auto directory = fresh_directory("changes");
    graywatch::Compressor compressor;
    graywatch::ChangeLog log(directory, 1ull << 20, 3, compressor);
    auto now = std::chrono::steady_clock::now();
    auto step = [&](const char* condition, double severity) {
        log.observe("io/sdb", "io", io_event(condition, severity), now, "ts");
        now += 1s;
    };
    // Single spikes are not a problem.
    for (int index = 0; index < 40; ++index) step(index % 10 == 0 ? "degraded" : "healthy", 0.8);
    require(lines(directory / "changes.jsonl").empty(), "spikes were logged");
    // Bad every other second is one problem, worst condition kept.
    for (int index = 0; index < 120; ++index) {
        step(index == 60 ? "degraded" : (index % 2 ? "congested" : "healthy"), index == 60 ? 0.9 : 0.5);
    }
    for (int index = 0; index < 40; ++index) step("idle", 0.0);
    const auto written = lines(directory / "changes.jsonl");
    require(written.size() == 2, "expected began and ended, got " + std::to_string(written.size()));
    require(written[0].find(R"("change":"began")") != std::string::npos &&
                written[0].find(R"("key":"io/sdb")") != std::string::npos,
            "began line: " + written[0]);
    require(written[1].find(R"("change":"ended")") != std::string::npos &&
                written[1].find(R"("worst_condition":"degraded")") != std::string::npos,
            "ended line: " + written[1]);
    std::filesystem::remove_all(directory);
}

void test_change_log_rotation() {
    const auto directory = fresh_directory("change-rotation");
    graywatch::Compressor compressor;
    {
        graywatch::ChangeLog log(directory, 200, 3, compressor);
        for (int index = 0; index < 50; ++index) {
            log.marker("daemon_started", "2026-11-16T20:22:" + std::to_string(10 + index) + "Z");
            compressor.wait_all();
        }
    }
    compressor.wait_all();
    std::size_t rotated{};
    for (const auto& name : names(directory)) rotated += name.starts_with("changes-");
    require(rotated <= 2, "kept " + std::to_string(rotated) + " rotated change logs");
    require(std::filesystem::exists(directory / "changes.jsonl"), "current change log missing");
    std::filesystem::remove_all(directory);
}

void test_capture_trigger() {
    const auto directory = fresh_directory("capture");
    const auto request = directory / "capture-request";
    const auto state = directory / "capture-last";
    auto now = std::chrono::steady_clock::now();
    auto wall = std::chrono::system_clock::now();
    {
        graywatch::CaptureTrigger trigger(request, state, 0.75, 60s, 1h);
        int fired = 0;
        for (int second = 0; second < 200; ++second) {
            // A dip below the threshold restarts the 60 s.
            const double severity = second == 30 ? 0.5 : 0.9;
            if (trigger.observe("io/sdc", io_event("degraded", severity), now, wall, "ts")) ++fired;
            now += 1s;
            wall += 1s;
        }
        require(fired == 1, "fired " + std::to_string(fired) + " times");
        require(lines(request).size() == 1 &&
                    lines(request)[0].find(R"("sustained_seconds":60)") != std::string::npos,
                "request content");
    }
    // The cooldown survives a restart.
    std::filesystem::remove(request);
    graywatch::CaptureTrigger restarted(request, state, 0.75, 60s, 1h);
    for (int second = 0; second < 120; ++second) {
        (void)restarted.observe("io/sdc", io_event("degraded", 0.9), now, wall, "ts");
        now += 1s;
        wall += 1s;
    }
    require(!std::filesystem::exists(request), "fired again within the cooldown");
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    try {
        test_parsers();
        test_recorder_rotation();
        test_recorder_size_limit();
        test_change_log();
        test_change_log_rotation();
        test_capture_trigger();
    } catch (const std::exception& error) {
        std::cerr << "recorder_test: " << error.what() << '\n';
        return 1;
    }
}
