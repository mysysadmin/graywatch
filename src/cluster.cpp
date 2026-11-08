#include "graywatch/cluster.hpp"

#include "graywatch/json.hpp"
#include "graywatch/process.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace graywatch {
namespace {

struct Options {
    std::vector<std::string> hosts;
    std::string user;
    std::string remote_command{"/usr/bin/graywatchctl"};
    std::string socket;
    double timeout_seconds{8.0};
    bool json_output{};
};

struct Row {
    std::string host;
    bool reachable{};
    std::string error;
    json::Value cpu;
    json::Value io;
    json::Value memory;
    json::Value network;
    json::Value fuse;
};

std::string shell_quote(const std::string& text) {
    std::string quoted = "'";
    for (const char character : text) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }
    return quoted + "'";
}

Options parse(int argc, char** argv, int first) {
    Options options;
    for (int index = first; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--user" && index + 1 < argc) {
            options.user = argv[++index];
        } else if (argument == "--timeout" && index + 1 < argc) {
            options.timeout_seconds = std::stod(argv[++index]);
            if (options.timeout_seconds <= 0) throw std::invalid_argument("--timeout must be positive");
        } else if (argument == "--remote-command" && index + 1 < argc) {
            options.remote_command = argv[++index];
        } else if (argument == "--socket" && index + 1 < argc) {
            options.socket = argv[++index];
        } else if (argument == "--json") {
            options.json_output = true;
        } else if (!argument.starts_with("-")) {
            options.hosts.push_back(argument);
        } else {
            throw std::invalid_argument("unknown or incomplete option: " + argument);
        }
    }
    if (options.hosts.empty()) throw std::invalid_argument("cluster needs at least one host");
    return options;
}

Row inspect(const Options& options, const std::string& host) {
    Row row;
    row.host = host;
    // One SSH round trip per host. The remote command runs in the remote
    // user's shell, so every interpolated value is single-quoted.
    std::string status = shell_quote(options.remote_command) + " status";
    if (!options.socket.empty()) status += " --socket " + shell_quote(options.socket);
    std::string remote;
    for (const char* resource : {"cpu", "io", "memory", "network", "fuse"}) {
        if (!remote.empty()) remote += "; ";
        remote += status + " --resource " + resource + " || echo null";
    }
    const int connect_timeout = std::max(1, static_cast<int>(options.timeout_seconds));
    std::vector<std::string> argv{
        "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=" + std::to_string(connect_timeout),
        "--", options.user.empty() ? host : options.user + "@" + host, remote};
    const auto result = run_process(
        argv, std::chrono::milliseconds(static_cast<long>(options.timeout_seconds * 1000) + 2000),
        16 * 1024 * 1024);
    if (result.timed_out || result.exit_code != 0) {
        std::string error = result.timed_out ? "timed out" : result.error;
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
        row.error = error.empty() ? "ssh exited with " + std::to_string(result.exit_code) : error;
        return row;
    }
    std::istringstream lines(result.output);
    std::string line;
    std::vector<json::Value> values;
    while (std::getline(lines, line) && values.size() < 5) {
        if (line.empty()) continue;
        try {
            values.push_back(json::parse(line));
        } catch (const std::exception&) {
            values.emplace_back();
        }
    }
    values.resize(5);
    row.cpu = values[0];
    row.io = values[1];
    row.memory = values[2];
    row.network = values[3];
    row.fuse = values[4];
    row.reachable = row.cpu.is_object() && !row.cpu.contains("error");
    if (!row.reachable) row.error = "graywatchd status unavailable on host";
    return row;
}

std::string fixed(double value, int precision) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", precision, value);
    return text;
}

const json::Value* top_thread(const json::Value& cpu) {
    const json::Value* best = nullptr;
    for (const auto& thread : cpu["affected_threads"].array()) {
        if (!best || thread.number("runqueue_wait_ms", 0) > best->number("runqueue_wait_ms", 0)) {
            best = &thread;
        }
    }
    return best;
}

std::vector<const json::Value*> network_issues(const json::Value& network) {
    std::vector<const json::Value*> issues;
    for (const auto& item : network.array()) {
        const auto& condition = item.string("condition");
        if (condition != "healthy" && condition != "inactive") issues.push_back(&item);
    }
    return issues;
}

std::string compact_network(const std::vector<const json::Value*>& issues) {
    if (issues.empty()) return "healthy";
    const auto& interface = (*issues.front())["interface"];
    std::string identity = interface.string("kind") == "infiniband"
        ? interface.string("device") + "/" + fixed(interface.number("port", 0), 0)
        : interface.string("kind");
    std::string text = identity + ":" + issues.front()->string("condition");
    if (issues.size() > 1) text += " +" + std::to_string(issues.size() - 1);
    return text;
}

std::string pad(std::string text, std::size_t width, bool right = false) {
    if (text.size() >= width) return text;
    return right ? std::string(width - text.size(), ' ') + text
                 : text + std::string(width - text.size(), ' ');
}

void print_table(const std::vector<Row>& rows) {
    std::cout << pad("HOST", 22) << ' ' << pad("CPU/PSI", 20) << ' ' << pad("WAIT", 6, true)
              << ' ' << pad("TOP THREAD", 26) << ' ' << pad("MEMORY", 22) << ' '
              << pad("DEVICE", 34) << ' ' << pad("FUSE", 24) << " NETWORK\n";
    for (const auto& row : rows) {
        if (!row.reachable) {
            std::cout << pad(row.host, 22) << ' ' << pad("UNREACHABLE", 20) << ' '
                      << pad("-", 6, true) << ' ' << row.error << '\n';
            continue;
        }
        const bool psi = row.cpu["evidence"].boolean("psi_available", false);
        const std::string cpu = row.cpu.string("condition") + "/" + (psi ? "psi" : "no-psi");
        const auto* thread = top_thread(row.cpu);
        const std::string thread_text = thread
            ? thread->string("command") + ":" + fixed(thread->number("runqueue_wait_ms", 0), 1) + "ms"
            : "-";
        std::string memory = "-";
        if (row.memory.is_object() && !row.memory.contains("error")) {
            const auto& evidence = row.memory["evidence"];
            memory = row.memory.string("condition") + " avail=" +
                     fixed(evidence.number("available_percent", 0), 0) + "%";
        }
        std::string device = "-";
        if (const auto* notable = notable_device(row.io)) {
            const auto& evidence = (*notable)["evidence"];
            const auto& condition = notable->string("condition");
            device = (*notable)["device"].string("name") + ":" +
                     (condition == "healthy" ? std::string() : condition + " ") +
                     fixed(evidence.number("busy_percent", 0), 1) + "% r=" +
                     fixed(evidence.number("average_read_latency_ms", 0), 2) + "ms w=" +
                     fixed(evidence.number("average_write_latency_ms", 0), 2) + "ms";
        }
        std::cout << pad(row.host, 22) << ' ' << pad(cpu, 20) << ' '
                  << pad(fixed(row.cpu["selection"].number("waiting_threads", 0), 0), 6, true)
                  << ' ' << pad(thread_text, 26) << ' ' << pad(memory, 22) << ' '
                  << pad(device, 34) << ' ' << pad(fuse_summary(row.fuse), 24) << ' '
                  << compact_network(network_issues(row.network))
                  << '\n';
    }
}

void print_json(const std::vector<Row>& rows) {
    // Re-emit each host's raw status documents; consumers get full evidence.
    auto raw = [](const json::Value& value) -> std::string {
        // Values came from graywatchd JSON; serialize the few shapes we keep.
        std::function<void(std::ostringstream&, const json::Value&)> write =
            [&](std::ostringstream& output, const json::Value& item) {
                switch (item.type()) {
                    case json::Value::Type::null: output << "null"; break;
                    case json::Value::Type::boolean: output << (item.boolean() ? "true" : "false"); break;
                    case json::Value::Type::number: output << fixed(item.number(), 6); break;
                    case json::Value::Type::string: output << '"' << json::escape(item.string()) << '"'; break;
                    case json::Value::Type::array: {
                        output << '[';
                        bool first = true;
                        for (const auto& element : item.array()) {
                            if (!first) output << ',';
                            first = false;
                            write(output, element);
                        }
                        output << ']';
                        break;
                    }
                    case json::Value::Type::object: {
                        output << '{';
                        bool first = true;
                        for (const auto& [key, element] : item.members()) {
                            if (!first) output << ',';
                            first = false;
                            output << '"' << json::escape(key) << "\":";
                            write(output, element);
                        }
                        output << '}';
                        break;
                    }
                }
            };
        std::ostringstream output;
        write(output, value);
        return output.str();
    };
    std::cout << "[\n";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        std::cout << "  {\"host\":\"" << json::escape(row.host) << "\",\"reachable\":"
                  << (row.reachable ? "true" : "false");
        if (!row.reachable) {
            std::cout << ",\"error\":\"" << json::escape(row.error) << "\"}";
        } else {
            std::cout << ",\"cpu\":" << raw(row.cpu) << ",\"io\":" << raw(row.io)
                      << ",\"memory\":" << raw(row.memory) << ",\"network\":" << raw(row.network)
                      << ",\"fuse\":" << raw(row.fuse)
                      << '}';
        }
        std::cout << (index + 1 < rows.size() ? ",\n" : "\n");
    }
    std::cout << "]\n";
}

}  // namespace

std::string fuse_summary(const json::Value& fuse) {
    if (!fuse.is_array() || fuse.array().empty()) return "-";
    const json::Value* stalled = nullptr;
    std::size_t stalled_count{};
    std::size_t readable{};
    double waiting{};
    for (const auto& mount : fuse.array()) {
        const auto& condition = mount.string("condition");
        if (condition == "unavailable") continue;
        ++readable;
        waiting += mount["evidence"].number("waiting_requests", 0);
        if (condition == "stalled") {
            ++stalled_count;
            if (!stalled || mount["evidence"].number("unchanged_seconds", 0) >
                                (*stalled)["evidence"].number("unchanged_seconds", 0)) {
                stalled = &mount;
            }
        }
    }
    if (stalled) {
        std::string text = "STALLED " + (*stalled)["mount"].string("path") + " " +
                           fixed((*stalled)["evidence"].number("unchanged_seconds", 0), 0) + "s";
        if (stalled_count > 1) text += " +" + std::to_string(stalled_count - 1);
        return text;
    }
    if (readable == 0) return "no-access";
    return waiting > 0 ? "active w=" + fixed(waiting, 0) : "idle";
}

const json::Value* notable_device(const json::Value& io) {
    const json::Value* problem = nullptr;
    const json::Value* busiest = nullptr;
    for (const auto& device : io.array()) {
        const auto& condition = device.string("condition");
        if (condition == "idle") continue;
        if (condition != "healthy") {
            if (!problem || device.number("severity", 0) > problem->number("severity", 0)) {
                problem = &device;
            }
            continue;
        }
        const double busy = device["evidence"].number("busy_percent", 0);
        if (busy >= cluster_minimum_busy_percent &&
            (!busiest || busy > (*busiest)["evidence"].number("busy_percent", 0))) {
            busiest = &device;
        }
    }
    return problem ? problem : busiest;
}

int run_cluster(int argc, char** argv, int first_argument) {
    const auto options = parse(argc, argv, first_argument);
    std::vector<Row> rows(options.hosts.size());
    std::atomic<std::size_t> next{0};
    const std::size_t workers = std::min<std::size_t>(options.hosts.size(), 16);
    std::vector<std::thread> threads;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&] {
            for (std::size_t index = next++; index < options.hosts.size(); index = next++) {
                try {
                    rows[index] = inspect(options, options.hosts[index]);
                } catch (const std::exception& error) {
                    rows[index] = Row{};
                    rows[index].host = options.hosts[index];
                    rows[index].error = error.what();
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (options.json_output) print_json(rows);
    else print_table(rows);
    return std::all_of(rows.begin(), rows.end(), [](const Row& row) { return row.reachable; }) ? 0 : 1;
}

}  // namespace graywatch
