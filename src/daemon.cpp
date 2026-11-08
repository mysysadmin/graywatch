#include "graywatch/block_stats.hpp"
#include "graywatch/event.hpp"
#include "graywatch/fuse_stats.hpp"
#include "graywatch/io_health.hpp"
#include "graywatch/memory_stats.hpp"
#include "graywatch/network_stats.hpp"
#include "graywatch/psi.hpp"
#include "graywatch/task_stats.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <poll.h>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic_bool running{true};

void stop(int) {
    running = false;
}

struct Options {
    std::filesystem::path socket_path{"/run/graywatch/graywatchd.sock"};
    double interval_seconds{1.0};
    std::optional<std::filesystem::path> log_path;
    std::size_t top_threads{20};
    double task_interval_seconds{5.0};
    std::set<std::string> ignored_ib_ports;
};

struct Client {
    int fd{-1};
    std::string input;
    bool subscribed{};
    double minimum_severity{};
    std::string resource{"all"};
};

bool wants_event(const Client& client, std::string_view resource,
                 double severity) {
    return client.subscribed && severity >= client.minimum_severity &&
           (client.resource == "all" || client.resource == resource);
}

double positive_double(const char* text, std::string_view option) {
    std::size_t consumed{};
    const double value = std::stod(text, &consumed);
    if (text[consumed] != '\0' || value <= 0.0) {
        throw std::invalid_argument(std::string(option) + " must be positive");
    }
    return value;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--socket" && index + 1 < argc) {
            options.socket_path = argv[++index];
        } else if (argument == "--interval" && index + 1 < argc) {
            options.interval_seconds = positive_double(argv[++index], "--interval");
        } else if (argument == "--log" && index + 1 < argc) {
            options.log_path = argv[++index];
        } else if (argument == "--top" && index + 1 < argc) {
            std::size_t consumed{};
            const std::string value(argv[++index]);
            options.top_threads = std::stoul(value, &consumed);
            if (consumed != value.size() || options.top_threads == 0) {
                throw std::invalid_argument("--top must be positive");
            }
        } else if (argument == "--ignore-ib-port" && index + 1 < argc) {
            const std::string value(argv[++index]);
            const auto slash = value.find('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 == value.size()) {
                throw std::invalid_argument("--ignore-ib-port must be DEVICE/PORT");
            }
            options.ignored_ib_ports.insert(value);
        } else if (argument == "--task-interval" && index + 1 < argc) {
            options.task_interval_seconds =
                positive_double(argv[++index], "--task-interval");
        } else {
            throw std::invalid_argument("unknown or incomplete option: " +
                                        std::string(argument));
        }
    }
    return options;
}

int create_listener(const std::filesystem::path& path) {
    const std::string path_text = path.string();
    if (path_text.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Unix socket path is too long");
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_text.c_str(), path_text.size() + 1);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string message = std::strerror(errno);
        close(fd);
        throw std::runtime_error("bind " + path_text + ": " + message);
    }
    if (chmod(path_text.c_str(), 0660) < 0) {
        const std::string message = std::strerror(errno);
        close(fd);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw std::runtime_error("chmod " + path_text + ": " + message);
    }
    if (listen(fd, 16) < 0) {
        const std::string message = std::strerror(errno);
        close(fd);
        throw std::runtime_error("listen: " + message);
    }
    return fd;
}

bool send_line(int fd, std::string_view line) {
    std::string message(line);
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
            return false;
        }
    }
    return true;
}

void handle_command(Client& client, std::string_view command,
                    const std::optional<std::string>& current_event,
                    const std::vector<std::string>& current_io_events,
                    const std::vector<std::string>& current_network_events,
                    const std::optional<std::string>& current_memory_event,
                    const std::vector<std::string>& current_fuse_events) {
    if (command == "GET") {
        if (current_event) {
            (void)send_line(client.fd, *current_event);
        } else {
            (void)send_line(client.fd,
                            R"({"error":"state_not_ready"})");
        }
        return;
    }
    if (command == "GET io") {
        (void)send_line(client.fd, graywatch::json_array(current_io_events));
        return;
    }
    if (command == "GET memory") {
        (void)send_line(client.fd, current_memory_event
                                       ? *current_memory_event
                                       : R"({"error":"state_not_ready"})");
        return;
    }
    if (command == "GET network") {
        (void)send_line(client.fd, graywatch::json_array(current_network_events));
        return;
    }
    if (command == "GET fuse") {
        (void)send_line(client.fd, graywatch::json_array(current_fuse_events));
        return;
    }
    constexpr std::string_view prefix = "SUBSCRIBE ";
    if (command.starts_with(prefix)) {
        try {
            std::istringstream input{std::string(command.substr(prefix.size()))};
            double severity{};
            std::string resource{"all"};
            std::string extra;
            if (!(input >> severity) ||
                ((input >> resource) && resource != "all" && resource != "cpu" &&
                 resource != "io" && resource != "network" &&
                 resource != "memory" && resource != "fuse") ||
                (input >> extra) || severity < 0.0 || severity > 1.0) {
                throw std::invalid_argument("range");
            }
            client.subscribed = true;
            client.minimum_severity = severity;
            client.resource = resource;
            (void)send_line(client.fd,
                std::string(R"({"subscribed":true,"resource":")") +
                resource + R"("})");
        } catch (const std::exception&) {
            (void)send_line(client.fd,
                            R"({"error":"invalid_minimum_severity"})");
        }
        return;
    }
    (void)send_line(client.fd, R"({"error":"unknown_command"})");
}

graywatch::CpuPressureEvent make_event(
    const std::vector<graywatch::PsiInterval>& pressure,
    const std::vector<graywatch::TaskInterval>& tasks,
    std::size_t observed_threads, std::size_t top_threads, double elapsed,
    bool psi_available, bool attribution_available,
    double attribution_interval, double attribution_age) {
    double some{};
    double full{};
    for (const auto& interval : pressure) {
        if (interval.resource == "cpu" && interval.scope == "some") {
            some = interval.stall_percent;
        } else if (interval.resource == "cpu" && interval.scope == "full") {
            full = interval.stall_percent;
        }
    }
    const auto active_threads = std::count_if(
        tasks.begin(), tasks.end(), [](const auto& task) {
            return task.runtime_ns != 0 || task.runqueue_wait_ns != 0;
        });
    const auto waiting_threads = std::count_if(
        tasks.begin(), tasks.end(), [](const auto& task) {
            return task.runqueue_wait_ns != 0;
        });
    const auto blocked_threads = std::count_if(
        tasks.begin(), tasks.end(), [](const auto& task) {
            return task.state == 'D';
        });
    std::vector<graywatch::TaskInterval> affected;
    for (const auto& task : tasks) {
        if (affected.size() >= top_threads) {
            break;
        }
        if (task.state == 'D' || task.runqueue_wait_ns != 0) {
            affected.push_back(task);
        }
    }
    return {
        .timestamp = graywatch::current_timestamp_utc(),
        .interval_seconds = elapsed,
        .psi_available = psi_available,
        .severity = std::clamp(some / 100.0, 0.0, 1.0),
        .some_stall_percent = some,
        .full_stall_percent = full,
        .selection_kind = "system",
        .selection_value = "all",
        .attribution_available = attribution_available,
        .attribution_interval_seconds = attribution_interval,
        .attribution_age_seconds = attribution_age,
        .selected_observed_threads = observed_threads,
        .selected_active_threads = static_cast<std::size_t>(active_threads),
        .selected_waiting_threads = static_cast<std::size_t>(waiting_threads),
        .selected_blocked_threads = static_cast<std::size_t>(blocked_threads),
        .affected_threads = std::move(affected),
    };
}

std::vector<graywatch::PsiSample> read_available_psi() {
    try {
        return graywatch::read_system_psi();
    } catch (const std::exception&) {
        return {};
    }
}

std::pair<double, double> resource_psi(
    const std::vector<graywatch::PsiInterval>& pressure, std::string_view resource) {
    double some{};
    double full{};
    for (const auto& interval : pressure) {
        if (interval.resource != resource) continue;
        if (interval.scope == "some") some = interval.stall_percent;
        if (interval.scope == "full") full = interval.stall_percent;
    }
    return {some, full};
}

std::optional<graywatch::MemorySample> read_available_memory() {
    try {
        return graywatch::read_memory_sample();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string ib_port_key(const graywatch::IbPortInterval& port) {
    return port.device + "/" + std::to_string(port.port);
}

}  // namespace

int main(int argc, char** argv) {
    int listener{-1};
    std::optional<std::filesystem::path> bound_socket;
    try {
        const auto options = parse_options(argc, argv);
        if (std::filesystem::exists(options.socket_path)) {
            throw std::runtime_error("socket path already exists: " +
                                     options.socket_path.string());
        }
        listener = create_listener(options.socket_path);
        bound_socket = options.socket_path;
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        std::ofstream log;
        if (options.log_path) {
            log.open(*options.log_path, std::ios::app);
            if (!log) {
                throw std::runtime_error("cannot open event log: " +
                                         options.log_path->string());
            }
        }

        std::vector<Client> clients;
        std::optional<std::string> current_event;
        std::vector<std::string> current_io_events;
        std::vector<std::string> current_network_events;
        std::optional<std::string> current_memory_event;
        std::vector<std::string> current_fuse_events;
        graywatch::FuseTracker fuse_tracker;
        std::set<std::string> ib_ports_seen_active;
        auto previous = read_available_psi();
        auto previous_memory = read_available_memory();
        auto previous_tasks = graywatch::read_system_tasks();
        std::vector<graywatch::TaskInterval> latest_task_intervals;
        std::size_t observed_threads = previous_tasks.size();
        bool attribution_available = false;
        double attribution_elapsed{};
        auto previous_block = graywatch::read_block_devices();
        auto previous_ib = graywatch::read_ib_ports();
        auto previous_tcp = graywatch::read_tcp_sample();
        auto previous_time = std::chrono::steady_clock::now();
        auto previous_task_time = previous_time;
        auto latest_task_time = previous_time;
        auto next_task_sample = previous_time +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(options.task_interval_seconds));
        auto next_sample = previous_time +
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               std::chrono::duration<double>(options.interval_seconds));

        std::cout << "graywatchd listening on " << options.socket_path << '\n';
        while (running) {
            const auto now = std::chrono::steady_clock::now();
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::max(next_sample - now,
                         std::chrono::steady_clock::duration::zero()));
            std::vector<pollfd> descriptors{{listener, POLLIN, 0}};
            const std::size_t polled_clients = clients.size();
            for (const auto& client : clients) {
                descriptors.push_back({client.fd, POLLIN, 0});
            }
            const int result = poll(descriptors.data(), descriptors.size(),
                                    static_cast<int>(wait.count()));
            if (result < 0 && errno != EINTR) {
                throw std::runtime_error("poll: " +
                                         std::string(std::strerror(errno)));
            }

            if (!descriptors.empty() && (descriptors[0].revents & POLLIN)) {
                while (true) {
                    const int client_fd = accept4(listener, nullptr, nullptr,
                                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        throw std::runtime_error("accept: " +
                                                 std::string(std::strerror(errno)));
                    }
                    clients.push_back({
                        .fd = client_fd,
                        .input = {},
                        .subscribed = false,
                        .minimum_severity = 0.0,
                    });
                }
            }

            for (std::size_t index = 0; index < polled_clients; ++index) {
                const auto events = descriptors[index + 1].revents;
                if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    close(clients[index].fd);
                    clients[index].fd = -1;
                    continue;
                }
                if ((events & POLLIN) == 0) {
                    continue;
                }
                char buffer[1024];
                const ssize_t count = recv(clients[index].fd, buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    close(clients[index].fd);
                    clients[index].fd = -1;
                    continue;
                }
                clients[index].input.append(buffer, static_cast<std::size_t>(count));
                std::size_t newline{};
                while ((newline = clients[index].input.find('\n')) != std::string::npos) {
                    const std::string command = clients[index].input.substr(0, newline);
                    clients[index].input.erase(0, newline + 1);
                    handle_command(clients[index], command, current_event,
                                   current_io_events, current_network_events,
                                   current_memory_event,
                                   current_fuse_events);
                }
            }
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [](const Client& client) {
                                             return client.fd < 0;
                                         }),
                          clients.end());

            const auto sample_time = std::chrono::steady_clock::now();
            if (sample_time >= next_sample) {
                const auto current = read_available_psi();
                // PSI is re-evaluated each interval: an interval needs both
                // endpoints, and a read failure must not be reported as zero.
                const bool psi_available = !previous.empty() && !current.empty();
                const auto current_memory = read_available_memory();
                const auto current_block = graywatch::read_block_devices();
                const auto current_ib = graywatch::read_ib_ports();
                const auto current_tcp = graywatch::read_tcp_sample();
                const double elapsed = std::chrono::duration<double>(
                                           sample_time - previous_time)
                                           .count();
                const auto pressure = graywatch::calculate_psi_intervals(
                    previous, current, elapsed);
                if (sample_time >= next_task_sample) {
                    const auto current_tasks = graywatch::read_system_tasks();
                    attribution_elapsed = std::chrono::duration<double>(
                                              sample_time - previous_task_time)
                                              .count();
                    latest_task_intervals = graywatch::calculate_task_intervals(
                        previous_tasks, current_tasks);
                    previous_tasks = current_tasks;
                    observed_threads = current_tasks.size();
                    previous_task_time = sample_time;
                    latest_task_time = sample_time;
                    attribution_available = true;
                    next_task_sample = sample_time +
                        std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(
                                options.task_interval_seconds));
                }
                const double attribution_age = attribution_available
                    ? std::chrono::duration<double>(sample_time - latest_task_time).count()
                    : 0.0;
                const auto event = make_event(
                    pressure,
                    latest_task_intervals, observed_threads, options.top_threads,
                    elapsed, psi_available, attribution_available,
                    attribution_elapsed, attribution_age);
                current_event = graywatch::to_json(event);
                if (log) {
                    log << *current_event << '\n' << std::flush;
                }
                for (auto& client : clients) {
                    if (wants_event(client, "cpu", event.severity) &&
                        !send_line(client.fd, *current_event)) {
                        close(client.fd);
                        client.fd = -1;
                    }
                }
                current_memory_event.reset();
                if (previous_memory && current_memory) {
                    if (const auto memory = graywatch::calculate_memory_interval(
                            *previous_memory, *current_memory, elapsed)) {
                        const auto [memory_some, memory_full] =
                            resource_psi(pressure, "memory");
                        const graywatch::MemoryEvent memory_event{
                            .timestamp = graywatch::current_timestamp_utc(),
                            .interval_seconds = elapsed,
                            .interval = *memory,
                            .assessment = graywatch::assess_memory(
                                *memory, psi_available, memory_some, memory_full),
                            .psi_available = psi_available,
                            .psi_some_stall_percent = memory_some,
                            .psi_full_stall_percent = memory_full,
                        };
                        current_memory_event = graywatch::to_json(memory_event);
                        if (log) log << *current_memory_event << '\n' << std::flush;
                        for (auto& client : clients) {
                            if (wants_event(client, "memory",
                                            memory_event.assessment.severity) &&
                                !send_line(client.fd, *current_memory_event)) {
                                close(client.fd);
                                client.fd = -1;
                            }
                        }
                    }
                }
                const auto [io_some, io_full] = resource_psi(pressure, "io");
                current_io_events.clear();
                for (const auto& device : graywatch::calculate_block_intervals(
                         previous_block, current_block, elapsed)) {
                    const graywatch::IoDeviceEvent io_event{
                        .timestamp = graywatch::current_timestamp_utc(),
                        .interval_seconds = elapsed,
                        .device = device,
                        .assessment = graywatch::assess_io(device),
                        .psi_available = psi_available,
                        .psi_some_stall_percent = io_some,
                        .psi_full_stall_percent = io_full,
                    };
                    auto json = graywatch::to_json(io_event);
                    current_io_events.push_back(json);
                    if (log) log << json << '\n' << std::flush;
                    const double severity = io_event.assessment.severity;
                    for (auto& client : clients) {
                        if (wants_event(client, "io", severity) &&
                            !send_line(client.fd, json)) {
                            close(client.fd);
                            client.fd = -1;
                        }
                    }
                }
                current_network_events.clear();
                if (const auto tcp = graywatch::calculate_tcp_interval(
                        previous_tcp, current_tcp)) {
                    const graywatch::TcpEvent network_event{
                        .timestamp = graywatch::current_timestamp_utc(),
                        .interval_seconds = elapsed,
                        .interval = *tcp,
                    };
                    auto json = graywatch::to_json(network_event);
                    current_network_events.push_back(json);
                    if (log) log << json << '\n' << std::flush;
                    const bool error = tcp->input_errors != 0;
                    const bool unstable = tcp->retransmitted_segments >= 10 &&
                                          tcp->retransmit_percent >= 1.0;
                    const double severity = error ? 0.8 :
                        (unstable ? std::clamp(tcp->retransmit_percent / 10.0,
                                               0.4, 1.0) : 0.01);
                    for (auto& client : clients) {
                        if (wants_event(client, "network", severity) &&
                            !send_line(client.fd, json)) {
                            close(client.fd);
                            client.fd = -1;
                        }
                    }
                }
                for (const auto& port : graywatch::calculate_ib_intervals(
                         previous_ib, current_ib)) {
                    const auto key = ib_port_key(port);
                    if (options.ignored_ib_ports.contains(key)) continue;
                    if (port.state == "ACTIVE") ib_ports_seen_active.insert(key);
                    const graywatch::IbPortEvent network_event{
                        .timestamp = graywatch::current_timestamp_utc(),
                        .interval_seconds = elapsed,
                        .port = port,
                        .in_use = port.used_since_boot ||
                                  ib_ports_seen_active.contains(key),
                    };
                    auto json = graywatch::to_json(network_event);
                    current_network_events.push_back(json);
                    if (log) log << json << '\n' << std::flush;
                    const double severity = graywatch::assess_ib_port(
                        port, network_event.in_use).severity;
                    for (auto& client : clients) {
                        if (wants_event(client, "network", severity) &&
                            !send_line(client.fd, json)) {
                            close(client.fd);
                            client.fd = -1;
                        }
                    }
                }
                // FUSE: requests the daemon of each fuse mount has not answered.
                current_fuse_events.clear();
                for (const auto& sample : graywatch::read_fuse_samples()) {
                    const graywatch::FuseEvent fuse_event{
                        .timestamp = graywatch::current_timestamp_utc(),
                        .interval_seconds = elapsed,
                        .sample = sample,
                        .assessment = fuse_tracker.observe(sample, elapsed),
                    };
                    auto json = graywatch::to_json(fuse_event);
                    current_fuse_events.push_back(json);
                    if (log) log << json << '\n' << std::flush;
                    for (auto& client : clients) {
                        if (wants_event(client, "fuse", fuse_event.assessment.severity) &&
                            !send_line(client.fd, json)) {
                            close(client.fd);
                            client.fd = -1;
                        }
                    }
                }
                clients.erase(std::remove_if(clients.begin(), clients.end(),
                                             [](const Client& client) {
                                                 return client.fd < 0;
                                             }),
                              clients.end());
                previous = current;
                previous_memory = current_memory;
                previous_block = current_block;
                previous_ib = current_ib;
                previous_tcp = current_tcp;
                previous_time = sample_time;
                next_sample = sample_time +
                              std::chrono::duration_cast<
                                  std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(
                                      options.interval_seconds));
            }
        }

        for (const auto& client : clients) {
            close(client.fd);
        }
        close(listener);
        std::filesystem::remove(options.socket_path);
        return 0;
    } catch (const std::exception& error) {
        if (listener >= 0) {
            close(listener);
        }
        if (bound_socket) {
            std::error_code ignored;
            std::filesystem::remove(*bound_socket, ignored);
        }
        std::cerr << "graywatchd: " << error.what() << '\n';
        return 1;
    }
}
