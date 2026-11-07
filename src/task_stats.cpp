#include "graywatch/task_stats.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace graywatch {
namespace {

std::optional<int> numeric_name(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    int value{};
    const auto [end, error] =
        std::from_chars(name.data(), name.data() + name.size(), value);
    if (error != std::errc{} || end != name.data() + name.size() || value <= 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<unsigned int> process_uid(const std::filesystem::path& directory) {
    std::ifstream status(directory / "status");
    std::string key;
    while (status >> key) {
        if (key == "Uid:") {
            unsigned int uid{};
            if (status >> uid) {
                return uid;
            }
            return std::nullopt;
        }
        std::string remainder;
        std::getline(status, remainder);
    }
    return std::nullopt;
}

std::optional<TaskSample> read_task(int pid, int tid, unsigned int uid,
                                    const std::filesystem::path& task_directory) {
    std::ifstream comm(task_directory / "comm");
    std::ifstream schedstat(task_directory / "schedstat");
    std::string command;
    std::string line;
    if (!std::getline(comm, command) || !std::getline(schedstat, line)) {
        return std::nullopt;
    }
    try {
        auto sample = parse_task_schedstat(pid, tid, uid, std::move(command), line);
        std::ifstream status(task_directory / "status");
        std::string key;
        while (status >> key) {
            if (key == "State:") {
                status >> sample.state;
                break;
            }
            std::getline(status, line);
        }
        std::ifstream wchan(task_directory / "wchan");
        std::getline(wchan, sample.wait_channel);
        if (sample.wait_channel == "0") sample.wait_channel.clear();
        return sample;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

TaskSample parse_task_schedstat(int pid, int tid, unsigned int uid,
                                std::string command, std::string_view line) {
    std::istringstream input{std::string(line)};
    TaskSample sample{
        .pid = pid,
        .tid = tid,
        .uid = uid,
        .command = std::move(command),
        .state = '?',
        .wait_channel = {},
    };
    if (!(input >> sample.runtime_ns >> sample.runqueue_wait_ns >>
          sample.timeslices)) {
        throw std::runtime_error("invalid schedstat line");
    }
    return sample;
}

std::optional<TaskInterval> calculate_task_interval(const TaskSample& previous,
                                                    const TaskSample& current) {
    if (previous.pid != current.pid || previous.tid != current.tid ||
        current.runtime_ns < previous.runtime_ns ||
        current.runqueue_wait_ns < previous.runqueue_wait_ns ||
        current.timeslices < previous.timeslices) {
        return std::nullopt;
    }
    const auto runtime_ns = current.runtime_ns - previous.runtime_ns;
    const auto wait_ns = current.runqueue_wait_ns - previous.runqueue_wait_ns;
    const auto runnable_ns = runtime_ns + wait_ns;
    return TaskInterval{
        .pid = current.pid,
        .tid = current.tid,
        .uid = current.uid,
        .command = current.command,
        .state = current.state,
        .wait_channel = current.wait_channel,
        .runtime_ns = runtime_ns,
        .runqueue_wait_ns = wait_ns,
        .timeslices = current.timeslices - previous.timeslices,
        .runnable_wait_percent =
            runnable_ns == 0
                ? 0.0
                : static_cast<double>(wait_ns) /
                      static_cast<double>(runnable_ns) * 100.0,
    };
}

std::vector<TaskSample> read_process_tasks(int pid,
                                           const std::filesystem::path& proc_root) {
    std::vector<TaskSample> samples;
    const auto process_directory = proc_root / std::to_string(pid);
    const auto uid = process_uid(process_directory);
    if (!uid) {
        return samples;
    }

    std::error_code error;
    std::filesystem::directory_iterator tasks(process_directory / "task", error);
    if (error) {
        return samples;
    }
    for (const auto& entry : tasks) {
        const auto tid = numeric_name(entry.path());
        if (!tid) {
            continue;
        }
        auto sample = read_task(pid, *tid, *uid, entry.path());
        if (sample) {
            samples.push_back(std::move(*sample));
        }
    }
    return samples;
}

std::vector<TaskSample> read_user_tasks(unsigned int uid,
                                        const std::filesystem::path& proc_root) {
    std::vector<TaskSample> samples;
    std::error_code error;
    std::filesystem::directory_iterator processes(proc_root, error);
    if (error) {
        return samples;
    }
    for (const auto& entry : processes) {
        const auto pid = numeric_name(entry.path());
        if (!pid || process_uid(entry.path()) != uid) {
            continue;
        }
        auto process_samples = read_process_tasks(*pid, proc_root);
        samples.insert(samples.end(),
                       std::make_move_iterator(process_samples.begin()),
                       std::make_move_iterator(process_samples.end()));
    }
    return samples;
}

std::vector<TaskSample> read_system_tasks(const std::filesystem::path& proc_root) {
    std::vector<TaskSample> samples;
    std::error_code error;
    std::filesystem::directory_iterator processes(proc_root, error);
    if (error) {
        return samples;
    }
    for (const auto& entry : processes) {
        const auto pid = numeric_name(entry.path());
        if (!pid) {
            continue;
        }
        auto process_samples = read_process_tasks(*pid, proc_root);
        samples.insert(samples.end(),
                       std::make_move_iterator(process_samples.begin()),
                       std::make_move_iterator(process_samples.end()));
    }
    return samples;
}

std::vector<TaskInterval> calculate_task_intervals(
    const std::vector<TaskSample>& previous,
    const std::vector<TaskSample>& current) {
    using Key = std::pair<int, int>;
    std::map<Key, const TaskSample*> previous_by_id;
    for (const auto& sample : previous) {
        previous_by_id[{sample.pid, sample.tid}] = &sample;
    }

    std::vector<TaskInterval> intervals;
    for (const auto& sample : current) {
        const auto found = previous_by_id.find({sample.pid, sample.tid});
        if (found == previous_by_id.end()) {
            continue;
        }
        auto interval = calculate_task_interval(*found->second, sample);
        if (interval) {
            intervals.push_back(std::move(*interval));
        }
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const auto& left, const auto& right) {
                  if ((left.state == 'D') != (right.state == 'D')) {
                      return left.state == 'D';
                  }
                  if (left.runqueue_wait_ns != right.runqueue_wait_ns) {
                      return left.runqueue_wait_ns > right.runqueue_wait_ns;
                  }
                  return left.runtime_ns > right.runtime_ns;
              });
    return intervals;
}

}  // namespace graywatch
