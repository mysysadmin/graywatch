#include "graywatch/event.hpp"
#include "graywatch/psi.hpp"

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
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic_bool running{true};

void stop(int) {
    running = false;
}

struct Options {
    std::filesystem::path socket_path{"/run/graywatchd.sock"};
    double interval_seconds{1.0};
    std::optional<std::filesystem::path> log_path;
};

struct Client {
    int fd{-1};
    std::string input;
    bool subscribed{};
    double minimum_severity{};
};

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
                    const std::optional<std::string>& current_event) {
    if (command == "GET") {
        if (current_event) {
            (void)send_line(client.fd, *current_event);
        } else {
            (void)send_line(client.fd,
                            R"({"error":"state_not_ready"})");
        }
        return;
    }
    constexpr std::string_view prefix = "SUBSCRIBE ";
    if (command.starts_with(prefix)) {
        try {
            std::size_t consumed{};
            const std::string value(command.substr(prefix.size()));
            const double severity = std::stod(value, &consumed);
            if (consumed != value.size() || severity < 0.0 || severity > 1.0) {
                throw std::invalid_argument("range");
            }
            client.subscribed = true;
            client.minimum_severity = severity;
            (void)send_line(client.fd, R"({"subscribed":true})");
        } catch (const std::exception&) {
            (void)send_line(client.fd,
                            R"({"error":"invalid_minimum_severity"})");
        }
        return;
    }
    (void)send_line(client.fd, R"({"error":"unknown_command"})");
}

graywatch::CpuPressureEvent make_event(
    const std::vector<graywatch::PsiInterval>& pressure, double elapsed) {
    double some{};
    double full{};
    for (const auto& interval : pressure) {
        if (interval.resource == "cpu" && interval.scope == "some") {
            some = interval.stall_percent;
        } else if (interval.resource == "cpu" && interval.scope == "full") {
            full = interval.stall_percent;
        }
    }
    return {
        .timestamp = graywatch::current_timestamp_utc(),
        .interval_seconds = elapsed,
        .severity = std::clamp(some / 100.0, 0.0, 1.0),
        .some_stall_percent = some,
        .full_stall_percent = full,
        .selection_kind = "system",
        .selection_value = "all",
    };
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
        auto previous = graywatch::read_system_psi();
        auto previous_time = std::chrono::steady_clock::now();
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
                    clients.push_back({.fd = client_fd});
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
                    handle_command(clients[index], command, current_event);
                }
            }
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [](const Client& client) {
                                             return client.fd < 0;
                                         }),
                          clients.end());

            const auto sample_time = std::chrono::steady_clock::now();
            if (sample_time >= next_sample) {
                const auto current = graywatch::read_system_psi();
                const double elapsed = std::chrono::duration<double>(
                                           sample_time - previous_time)
                                           .count();
                const auto event = make_event(
                    graywatch::calculate_psi_intervals(previous, current, elapsed),
                    elapsed);
                current_event = graywatch::to_json(event);
                if (log) {
                    log << *current_event << '\n' << std::flush;
                }
                for (auto& client : clients) {
                    if (client.subscribed && event.severity >= client.minimum_severity &&
                        !send_line(client.fd, *current_event)) {
                        close(client.fd);
                        client.fd = -1;
                    }
                }
                clients.erase(std::remove_if(clients.begin(), clients.end(),
                                             [](const Client& client) {
                                                 return client.fd < 0;
                                             }),
                              clients.end());
                previous = current;
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
