#pragma once

#include "graywatch/task_stats.hpp"
#include "graywatch/block_stats.hpp"
#include "graywatch/io_health.hpp"
#include "graywatch/memory_stats.hpp"
#include "graywatch/network_stats.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace graywatch {

struct CpuPressureEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    bool psi_available{true};
    double severity{};
    double some_stall_percent{};
    double full_stall_percent{};
    std::string selection_kind;
    std::string selection_value;
    bool attribution_available{};
    double attribution_interval_seconds{};
    double attribution_age_seconds{};
    std::size_t selected_observed_threads{};
    std::size_t selected_active_threads{};
    std::size_t selected_waiting_threads{};
    std::size_t selected_blocked_threads{};
    std::vector<TaskInterval> affected_threads;
};

struct IoDeviceEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    BlockDeviceInterval device;
    IoAssessment assessment;
    bool psi_available{};
    double psi_some_stall_percent{};
    double psi_full_stall_percent{};
};

struct IbPortEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    IbPortInterval port;
    // True when the port carried traffic since boot, has ever gone down, or
    // was ACTIVE at any point while the daemon observed it. A down port that
    // was never in use is reported as "inactive" rather than "link_down".
    bool in_use{true};
};

struct MemoryEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    MemoryInterval interval;
    MemoryAssessment assessment;
    bool psi_available{};
    double psi_some_stall_percent{};
    double psi_full_stall_percent{};
};

struct TcpEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    TcpInterval interval;
};

std::string current_timestamp_utc();
std::string to_json(const CpuPressureEvent& event);
std::string to_json(const IoDeviceEvent& event);
std::string to_json(const IbPortEvent& event);
std::string to_json(const TcpEvent& event);
std::string to_json(const MemoryEvent& event);
struct IbPortAssessment {
    std::string_view condition;
    double severity{};
};
IbPortAssessment assess_ib_port(const IbPortInterval& port, bool in_use);

std::string json_array(const std::vector<std::string>& values);

}  // namespace graywatch
