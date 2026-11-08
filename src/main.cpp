#include "graywatch/cluster.hpp"
#include "graywatch/detector.hpp"
#include "graywatch/event.hpp"
#include "graywatch/psi.hpp"
#include "graywatch/task_stats.hpp"
#include "graywatch/user_lookup.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string upper_case(std::string_view text) {
    std::string result(text);
    for (auto& character : result) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    }
    return result;
}

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
                 "[--json] [--log PATH]\n"
                 "  graywatchctl status [--socket PATH] [--resource cpu|io|memory|network]\n"
                 "  graywatchctl subscribe [--socket PATH] "
                 "[--resource cpu|io|memory|network|all] "
                 "[--min-severity 0..1] [--count EVENTS]\n"
                 "  graywatchctl cluster HOST... [--user USER] [--timeout SECONDS] "
                 "[--remote-command PATH] [--socket PATH] [--json]\n";
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
            const auto uid = graywatch::resolve_local_uid(user);
            if (!uid) {
                throw std::invalid_argument("unknown user: " + user);
            }
            options.uid = *uid;
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

struct DaemonOptions {
    std::filesystem::path socket_path{"/run/graywatch/graywatchd.sock"};
    double minimum_severity{};
    unsigned int count{};
    std::string resource{"cpu"};
};

DaemonOptions parse_daemon_options(int argc, char** argv, bool subscribe) {
    DaemonOptions options;
    if (subscribe) options.resource = "all";
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--socket" && index + 1 < argc) {
            options.socket_path = argv[++index];
        } else if (argument == "--resource" && index + 1 < argc) {
            options.resource = argv[++index];
            if (options.resource != "cpu" && options.resource != "io" &&
                options.resource != "network" && options.resource != "memory" &&
                (!subscribe || options.resource != "all")) {
                throw std::invalid_argument(
                    "--resource must be cpu, io, memory, network, or all");
            }
        } else if (subscribe && argument == "--min-severity" &&
                   index + 1 < argc) {
            const std::string value(argv[++index]);
            std::size_t consumed{};
            options.minimum_severity = std::stod(value, &consumed);
            if (consumed != value.size() || options.minimum_severity < 0.0 ||
                options.minimum_severity > 1.0) {
                throw std::invalid_argument(
                    "--min-severity must be between 0 and 1");
            }
        } else if (subscribe && argument == "--count" && index + 1 < argc) {
            options.count = parse_positive_unsigned(argv[++index], "--count");
        } else {
            throw std::invalid_argument("unknown or incomplete option: " +
                                        std::string(argument));
        }
    }
    return options;
}

int connect_daemon(const std::filesystem::path& path) {
    const std::string path_text = path.string();
    if (path_text.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Unix socket path is too long");
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_text.c_str(), path_text.size() + 1);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string message = std::strerror(errno);
        close(fd);
        throw std::runtime_error("connect " + path_text + ": " + message);
    }
    return fd;
}

void send_command(int fd, std::string_view command) {
    std::string message(command);
    message.push_back('\n');
    std::size_t sent{};
    while (sent < message.size()) {
        const ssize_t result = send(fd, message.data() + sent,
                                    message.size() - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("send to daemon failed");
        }
    }
}

std::optional<std::string> receive_line(int fd, std::string& pending) {
    while (true) {
        const auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            return line;
        }
        char buffer[4096];
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            pending.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0) {
            return std::nullopt;
        } else if (errno != EINTR) {
            throw std::runtime_error("receive from daemon failed");
        }
    }
}

void daemon_status(const DaemonOptions& options) {
    const int fd = connect_daemon(options.socket_path);
    try {
        send_command(fd, options.resource == "cpu" ? std::string("GET")
                                                   : "GET " + options.resource);
        std::string pending;
        const auto line = receive_line(fd, pending);
        if (!line) {
            throw std::runtime_error("daemon closed without a response");
        }
        std::cout << *line << '\n';
        close(fd);
    } catch (...) {
        close(fd);
        throw;
    }
}

void daemon_subscribe(const DaemonOptions& options) {
    const int fd = connect_daemon(options.socket_path);
    try {
        send_command(fd, "SUBSCRIBE " + std::to_string(options.minimum_severity) +
                         " " + options.resource);
        std::string pending;
        const auto acknowledgement = receive_line(fd, pending);
        if (!acknowledgement || acknowledgement->find("\"subscribed\":true") ==
                                    std::string::npos) {
            throw std::runtime_error("subscription was rejected");
        }
        unsigned int received{};
        while (options.count == 0 || received < options.count) {
            const auto line = receive_line(fd, pending);
            if (!line) {
                throw std::runtime_error("daemon closed the subscription");
            }
            std::cout << *line << '\n' << std::flush;
            ++received;
        }
        close(fd);
    } catch (...) {
        close(fd);
        throw;
    }
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

std::vector<graywatch::PsiSample> read_optional_psi() {
    try {
        return graywatch::read_system_psi();
    } catch (const std::exception&) {
        return {};
    }
}

void tasks(const TaskOptions& options) {
    auto previous = read_selected_tasks(options);
    auto previous_psi = read_optional_psi();
    const bool psi_available = !previous_psi.empty();
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
        const auto current_psi = read_optional_psi();
        const double elapsed = std::chrono::duration<double>(
                                   current_time - previous_time)
                                   .count();
        const auto pressure = graywatch::calculate_psi_intervals(
            previous_psi, current_psi, elapsed);

        double cpu_some_percent{};
        double cpu_full_percent{};
        for (const auto& interval : pressure) {
            if (interval.resource == "cpu" && interval.scope == "some") {
                cpu_some_percent = interval.stall_percent;
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
        const auto blocked_threads = std::count_if(
            intervals.begin(), intervals.end(), [](const auto& interval) {
                return interval.state == 'D';
            });

        std::vector<graywatch::TaskInterval> affected;
        for (const auto& interval : intervals) {
            if (affected.size() >= options.top) {
                break;
            }
            if (interval.state == 'D' || interval.runqueue_wait_ns != 0) {
                affected.push_back(interval);
            }
        }
        const graywatch::CpuPressureEvent event{
            .timestamp = graywatch::current_timestamp_utc(),
            .interval_seconds = elapsed,
            .psi_available = psi_available,
            .severity = std::clamp(cpu_some_percent / 100.0, 0.0, 1.0),
            .some_stall_percent = cpu_some_percent,
            .full_stall_percent = cpu_full_percent,
            .selection_kind = options.selection_kind,
            .selection_value = options.selection_value,
            .attribution_available = true,
            .attribution_interval_seconds = elapsed,
            .attribution_age_seconds = 0.0,
            .selected_observed_threads = current.size(),
            .selected_active_threads = static_cast<std::size_t>(active_threads),
            .selected_waiting_threads = static_cast<std::size_t>(waiting_threads),
            .selected_blocked_threads = static_cast<std::size_t>(blocked_threads),
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
                  << upper_case(graywatch::cpu_pressure_condition(psi_available, event.some_stall_percent))
                  << " severity=" << event.severity
                  << " some_stall_percent=" << event.some_stall_percent
                  << " full_stall_percent=" << event.full_stall_percent
                  << " selected_active_threads=" << event.selected_active_threads
                  << " selected_waiting_threads=" << event.selected_waiting_threads
                  << " selected_blocked_threads=" << event.selected_blocked_threads
                  << '\n';

        std::cout << "PID      TID      S WCHAN                    COMMAND          "
                     "CPU_MS    WAIT_MS  RUNNABLE_WAIT%  SLICES\n";
        unsigned int shown{};
        for (const auto& interval : intervals) {
            if (shown >= options.top) {
                break;
            }
            if (interval.state != 'D' && interval.runtime_ns == 0 &&
                interval.runqueue_wait_ns == 0) {
                continue;
            }
            std::cout << std::left << std::setw(9) << interval.pid
                      << std::setw(9) << interval.tid
                      << std::setw(2) << interval.state
                      << std::setw(25) << interval.wait_channel.substr(0, 24)
                      << std::setw(17) << interval.command.substr(0, 16)
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
        if (command == "status") {
            daemon_status(parse_daemon_options(argc, argv, false));
            return 0;
        }
        if (command == "subscribe") {
            daemon_subscribe(parse_daemon_options(argc, argv, true));
            return 0;
        }
        if (command == "cluster") {
            return graywatch::run_cluster(argc, argv, 2);
        }
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
