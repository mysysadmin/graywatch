#include "graywatch/detector.hpp"
#include "graywatch/event.hpp"
#include "graywatch/psi.hpp"
#include "graywatch/task_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <pwd.h>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

struct Options {
    bool watch{};
    double interval_seconds{1.0};
    unsigned int count{};
};

void usage() {
    std::cerr << "usage:\n"
                 "  graywatchctl psi [--watch] [--interval SECONDS] "
                 "[--count SAMPLES]\n"
                 "  graywatchctl health [--interval SECONDS] "
                 "[--count SAMPLES]\n"
                 "  graywatchctl tasks (-p PID | -u USER) "
                 "[--interval SECONDS] [--count SAMPLES] [--top THREADS] "
                 "[--json] [--log PATH]\n";
}

double parse_positive_double(const char* text, std::string_view option) {
    std::size_t consumed{};
    const double value = std::stod(text, &consumed);
    if (text[consumed] != '\0' || value <= 0.0) {
        throw std::invalid_argument(std::string(option) + " must be positive");
    }
    return value;
}

unsigned int parse_positive_unsigned(const char* text, std::string_view option) {
    std::size_t consumed{};
    const auto value = std::stoul(text, &consumed);
    if (text[consumed] != '\0' || value == 0) {
        throw std::invalid_argument(std::string(option) + " must be positive");
    }
    return static_cast<unsigned int>(value);
}

Options parse_options(int argc, char** argv, bool health_command) {
    Options options;
    options.watch = health_command;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--watch" && !health_command) {
            options.watch = true;
        } else if (argument == "--interval" && index + 1 < argc) {
            options.interval_seconds =
                parse_positive_double(argv[++index], "--interval");
        } else if (argument == "--count" && index + 1 < argc) {
            options.count = parse_positive_unsigned(argv[++index], "--count");
        } else {
            throw std::invalid_argument("unknown or incomplete option: " +
                                        std::string(argument));
        }
    }
    if (options.count != 0 && !options.watch) {
        throw std::invalid_argument("--count requires --watch");
    }
    return options;
}

struct TaskOptions {
    std::optional<int> pid;
    std::optional<unsigned int> uid;
    double interval_seconds{1.0};
    unsigned int count{};
    unsigned int top{20};
    bool json{};
    std::optional<std::string> log_path;
    std::string selection_kind;
    std::string selection_value;
};

TaskOptions parse_task_options(int argc, char** argv) {
    TaskOptions options;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "-p" && index + 1 < argc) {
            const std::string pid_text(argv[++index]);
            options.pid = static_cast<int>(
                parse_positive_unsigned(pid_text.c_str(), "-p"));
            options.selection_kind = "pid";
            options.selection_value = pid_text;
        } else if (argument == "-u" && index + 1 < argc) {
            const std::string user(argv[++index]);
            const passwd* entry = getpwnam(user.c_str());
            if (entry == nullptr) {
                throw std::invalid_argument("unknown user: " + user);
            }
            options.uid = entry->pw_uid;
            options.selection_kind = "user";
            options.selection_value = user;
        } else if (argument == "--interval" && index + 1 < argc) {
            options.interval_seconds =
                parse_positive_double(argv[++index], "--interval");
        } else if (argument == "--count" && index + 1 < argc) {
            options.count = parse_positive_unsigned(argv[++index], "--count");
        } else if (argument == "--top" && index + 1 < argc) {
            options.top = parse_positive_unsigned(argv[++index], "--top");
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--log" && index + 1 < argc) {
            options.log_path = argv[++index];
        } else {
            throw std::invalid_argument("unknown or incomplete option: " +
                                        std::string(argument));
        }
    }
    if (options.pid.has_value() == options.uid.has_value()) {
        throw std::invalid_argument("tasks requires exactly one of -p or -u");
    }
    return options;
}

void print_snapshot(const std::vector<graywatch::PsiSample>& samples) {
    for (const auto& sample : samples) {
        std::cout << sample.resource << ' ' << sample.scope
                  << " avg10=" << sample.avg10
                  << " avg60=" << sample.avg60
                  << " avg300=" << sample.avg300
                  << " total_us=" << sample.total_us << '\n';
    }
}

void watch(const Options& options) {
    using Clock = std::chrono::steady_clock;
    auto previous = graywatch::read_system_psi();
    auto previous_time = Clock::now();
    unsigned int emitted{};

    const auto sleep_duration = std::chrono::duration<double>(options.interval_seconds);
    while (options.count == 0 || emitted < options.count) {
        std::this_thread::sleep_for(sleep_duration);
        const auto current_time = Clock::now();
        const auto current = graywatch::read_system_psi();
        const double elapsed =
            std::chrono::duration<double>(current_time - previous_time).count();

        std::cout << "interval_seconds=" << elapsed << '\n';
        for (const auto& interval :
             graywatch::calculate_psi_intervals(previous, current, elapsed)) {
            std::cout << interval.resource << ' ' << interval.scope
                      << " stall_percent=" << interval.stall_percent
                      << " stalled_us=" << interval.stalled_us << '\n';
        }
        std::cout << std::flush;
        previous = current;
        previous_time = current_time;
        ++emitted;
    }
}

void health(const Options& options) {
    using Clock = std::chrono::steady_clock;
    using Key = std::pair<std::string, std::string>;
    std::map<Key, graywatch::RollingDetector> detectors;
    auto previous = graywatch::read_system_psi();
    auto previous_time = Clock::now();
    unsigned int emitted{};

    const auto sleep_duration = std::chrono::duration<double>(options.interval_seconds);
    while (options.count == 0 || emitted < options.count) {
        std::this_thread::sleep_for(sleep_duration);
        const auto current_time = Clock::now();
        const auto current = graywatch::read_system_psi();
        const double elapsed =
            std::chrono::duration<double>(current_time - previous_time).count();

        for (const auto& interval :
             graywatch::calculate_psi_intervals(previous, current, elapsed)) {
            auto [position, inserted] = detectors.try_emplace(
                Key{interval.resource, interval.scope});
            (void)inserted;
            const auto detection = position->second.observe(interval.stall_percent);
            std::cout << interval.resource << ' ' << interval.scope
                      << " state=" << graywatch::to_string(detection.state)
                      << " current=" << detection.current
                      << " baseline_mean=" << detection.baseline_mean
                      << " baseline_stddev=" << detection.baseline_stddev
                      << " threshold=" << detection.threshold
                      << " samples=" << detection.baseline_samples;
            if (detection.anomalous) {
                std::cout << " anomalous=yes z_score=" << detection.z_score;
            }
            if (detection.changed) {
                std::cout << " transition="
                          << graywatch::to_string(detection.previous_state) << "->"
                          << graywatch::to_string(detection.state);
            }
            std::cout << '\n';
        }
        std::cout << "---\n" << std::flush;
        previous = current;
        previous_time = current_time;
        ++emitted;
    }
}

std::vector<graywatch::TaskSample> read_selected_tasks(const TaskOptions& options) {
    if (options.pid) {
        return graywatch::read_process_tasks(*options.pid);
    }
    return graywatch::read_user_tasks(*options.uid);
}

void tasks(const TaskOptions& options) {
    auto previous = read_selected_tasks(options);
    auto previous_psi = graywatch::read_system_psi();
    auto previous_time = std::chrono::steady_clock::now();
    if (previous.empty()) {
        throw std::runtime_error("no readable tasks matched the selection");
    }
    unsigned int emitted{};
    std::ofstream log;
    if (options.log_path) {
        log.open(*options.log_path, std::ios::app);
        if (!log) {
            throw std::runtime_error("cannot open event log: " + *options.log_path);
        }
    }
    const auto sleep_duration = std::chrono::duration<double>(options.interval_seconds);
    while (options.count == 0 || emitted < options.count) {
        std::this_thread::sleep_for(sleep_duration);
        const auto current_time = std::chrono::steady_clock::now();
        const auto current = read_selected_tasks(options);
        const auto intervals = graywatch::calculate_task_intervals(previous, current);
        const auto current_psi = graywatch::read_system_psi();
        const double elapsed = std::chrono::duration<double>(
                                   current_time - previous_time)
                                   .count();
        const auto pressure = graywatch::calculate_psi_intervals(
            previous_psi, current_psi, elapsed);

        double cpu_some_percent{};
        double cpu_full_percent{};
        std::uint64_t cpu_some_us{};
        for (const auto& interval : pressure) {
            if (interval.resource == "cpu" && interval.scope == "some") {
                cpu_some_percent = interval.stall_percent;
                cpu_some_us = interval.stalled_us;
            } else if (interval.resource == "cpu" && interval.scope == "full") {
                cpu_full_percent = interval.stall_percent;
            }
        }

        const auto active_threads = std::count_if(
            intervals.begin(), intervals.end(), [](const auto& interval) {
                return interval.runtime_ns != 0 || interval.runqueue_wait_ns != 0;
            });
        const auto waiting_threads = std::count_if(
            intervals.begin(), intervals.end(), [](const auto& interval) {
                return interval.runqueue_wait_ns != 0;
            });

        std::vector<graywatch::TaskInterval> affected;
        for (const auto& interval : intervals) {
            if (affected.size() >= options.top) {
                break;
            }
            if (interval.runqueue_wait_ns != 0) {
                affected.push_back(interval);
            }
        }
        const graywatch::CpuPressureEvent event{
            .timestamp = graywatch::current_timestamp_utc(),
            .interval_seconds = elapsed,
            .severity = std::clamp(cpu_some_percent / 100.0, 0.0, 1.0),
            .some_stall_percent = cpu_some_percent,
            .full_stall_percent = cpu_full_percent,
            .selection_kind = options.selection_kind,
            .selection_value = options.selection_value,
            .selected_active_threads = static_cast<std::size_t>(active_threads),
            .selected_waiting_threads = static_cast<std::size_t>(waiting_threads),
            .affected_threads = std::move(affected),
        };
        const std::string event_json = graywatch::to_json(event);
        if (log) {
            log << event_json << '\n' << std::flush;
        }
        if (options.json) {
            std::cout << event_json << '\n' << std::flush;
            previous = current;
            previous_psi = current_psi;
            previous_time = current_time;
            ++emitted;
            continue;
        }

        std::cout << "SYSTEM_CPU condition="
                  << (cpu_some_us == 0 ? "NO_PRESSURE" : "PRESSURE")
                  << " severity=" << event.severity
                  << " some_stall_percent=" << event.some_stall_percent
                  << " full_stall_percent=" << event.full_stall_percent
                  << " selected_active_threads=" << event.selected_active_threads
                  << " selected_waiting_threads=" << event.selected_waiting_threads
                  << '\n';

        std::cout << "PID      TID      COMMAND                  CPU_MS    WAIT_MS  "
                     "RUNNABLE_WAIT%  SLICES\n";
        unsigned int shown{};
        for (const auto& interval : intervals) {
            if (shown >= options.top) {
                break;
            }
            if (interval.runtime_ns == 0 && interval.runqueue_wait_ns == 0) {
                continue;
            }
            std::cout << std::left << std::setw(9) << interval.pid
                      << std::setw(9) << interval.tid
                      << std::setw(25) << interval.command.substr(0, 24)
                      << std::right << std::setw(9)
                      << static_cast<double>(interval.runtime_ns) / 1'000'000.0
                      << std::setw(11)
                      << static_cast<double>(interval.runqueue_wait_ns) / 1'000'000.0
                      << std::setw(16) << interval.runnable_wait_percent
                      << std::setw(8) << interval.timeslices << '\n';
            ++shown;
        }
        if (shown == 0) {
            std::cout << "(no selected thread was runnable during this interval)\n";
        }
        std::cout << "---\n" << std::flush;
        previous = current;
        previous_psi = current_psi;
        previous_time = current_time;
        ++emitted;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    try {
        const std::string_view command(argv[1]);
        if (command != "psi" && command != "health" && command != "tasks") {
            throw std::invalid_argument("unknown command: " + std::string(command));
        }
        if (command == "tasks") {
            std::cout << std::fixed << std::setprecision(2);
            tasks(parse_task_options(argc, argv));
            return 0;
        }
        const bool health_command = command == "health";
        const auto options = parse_options(argc, argv, health_command);
        std::cout << std::fixed << std::setprecision(2);
        if (health_command) {
            health(options);
        } else if (options.watch) {
            watch(options);
        } else {
            print_snapshot(graywatch::read_system_psi());
        }
    } catch (const std::exception& error) {
        std::cerr << "graywatchctl: " << error.what() << '\n';
        usage();
        return 1;
    }
}
