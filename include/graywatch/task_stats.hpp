#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace graywatch {

struct TaskSample {
    int pid{};
    int tid{};
    unsigned int uid{};
    std::string command;
    std::uint64_t runtime_ns{};
    std::uint64_t runqueue_wait_ns{};
    std::uint64_t timeslices{};
};

struct TaskInterval {
    int pid{};
    int tid{};
    unsigned int uid{};
    std::string command;
    std::uint64_t runtime_ns{};
    std::uint64_t runqueue_wait_ns{};
    std::uint64_t timeslices{};
    double runnable_wait_percent{};
};

TaskSample parse_task_schedstat(int pid, int tid, unsigned int uid,
                                std::string command, std::string_view line);
std::optional<TaskInterval> calculate_task_interval(const TaskSample& previous,
                                                    const TaskSample& current);
std::vector<TaskSample> read_process_tasks(
    int pid, const std::filesystem::path& proc_root = "/proc");
std::vector<TaskSample> read_user_tasks(
    unsigned int uid, const std::filesystem::path& proc_root = "/proc");
std::vector<TaskInterval> calculate_task_intervals(
    const std::vector<TaskSample>& previous,
    const std::vector<TaskSample>& current);

}  // namespace graywatch
