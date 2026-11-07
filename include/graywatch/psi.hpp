#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace graywatch {

struct PsiSample {
    std::string resource;
    std::string scope;
    double avg10{};
    double avg60{};
    double avg300{};
    std::uint64_t total_us{};
};

struct PsiInterval {
    std::string resource;
    std::string scope;
    std::uint64_t stalled_us{};
    double elapsed_seconds{};
    double stall_percent{};
};

PsiSample parse_psi_line(std::string_view resource, std::string_view line);
std::vector<PsiSample> read_psi_file(std::string_view resource,
                                     const std::filesystem::path& path);
std::vector<PsiSample> read_system_psi(
    const std::filesystem::path& pressure_root = "/proc/pressure");
std::vector<PsiInterval> calculate_psi_intervals(
    const std::vector<PsiSample>& previous,
    const std::vector<PsiSample>& current,
    double elapsed_seconds);

}  // namespace graywatch
