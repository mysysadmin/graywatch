#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace graywatch {

struct IbPortSample {
    std::string device;
    unsigned int port{};
    std::string state;
    std::string physical_state;
    std::string rate;
    std::uint64_t receive_errors{};
    std::uint64_t transmit_discards{};
    std::uint64_t symbol_errors{};
    std::uint64_t link_downed{};
    std::uint64_t transmit_wait{};
};

struct IbPortInterval {
    std::string device;
    unsigned int port{};
    std::string state;
    std::string physical_state;
    std::string rate;
    std::uint64_t receive_errors{};
    std::uint64_t transmit_discards{};
    std::uint64_t symbol_errors{};
    std::uint64_t link_downed{};
    std::uint64_t transmit_wait{};
};

std::optional<IbPortInterval> calculate_ib_interval(
    const IbPortSample& previous, const IbPortSample& current);
std::vector<IbPortSample> read_ib_ports(
    const std::filesystem::path& root = "/sys/class/infiniband");
std::vector<IbPortInterval> calculate_ib_intervals(
    const std::vector<IbPortSample>& previous,
    const std::vector<IbPortSample>& current);

}  // namespace graywatch
