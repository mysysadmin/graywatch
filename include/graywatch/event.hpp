#pragma once

#include "graywatch/task_stats.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace graywatch {

struct CpuPressureEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    double severity{};
    double some_stall_percent{};
    double full_stall_percent{};
    std::string selection_kind;
    std::string selection_value;
    std::size_t selected_active_threads{};
    std::size_t selected_waiting_threads{};
    std::vector<TaskInterval> affected_threads;
};

std::string current_timestamp_utc();
std::string to_json(const CpuPressureEvent& event);

}  // namespace graywatch
