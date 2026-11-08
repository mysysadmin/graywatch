#include "graywatch/fuse_stats.hpp"

#include "graywatch/json.hpp"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace graywatch {
namespace {

// mountinfo escapes space, tab, newline and backslash as \ooo.
std::string unescape_octal(std::string_view text) {
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\\' && index + 3 < text.size()) {
            const auto digit = [](char c) { return c >= '0' && c <= '7'; };
            if (digit(text[index + 1]) && digit(text[index + 2]) && digit(text[index + 3])) {
                result.push_back(static_cast<char>((text[index + 1] - '0') * 64 +
                                                   (text[index + 2] - '0') * 8 +
                                                   (text[index + 3] - '0')));
                index += 3;
                continue;
            }
        }
        result.push_back(text[index]);
    }
    return result;
}

bool is_fuse(std::string_view filesystem) {
    return filesystem == "fuse" || filesystem == "fuseblk" || filesystem.starts_with("fuse.");
}

}  // namespace

std::vector<FuseMount> parse_fuse_mounts(std::string_view mountinfo) {
    std::vector<FuseMount> mounts;
    std::set<std::uint64_t> seen;
    std::istringstream lines{std::string(mountinfo)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::vector<std::string> before;
        std::string field;
        while (fields >> field && field != "-") before.push_back(field);
        std::string filesystem;
        if (before.size() < 5 || !(fields >> filesystem) || !is_fuse(filesystem)) continue;
        const std::string& device = before[2];
        const auto colon = device.find(':');
        if (colon == std::string::npos) continue;
        std::uint64_t major{};
        std::uint64_t minor{};
        try {
            major = std::stoull(device.substr(0, colon));
            minor = std::stoull(device.substr(colon + 1));
        } catch (const std::exception&) {
            continue;
        }
        const std::uint64_t connection = (major << 20) | minor;
        if (!seen.insert(connection).second) continue;
        mounts.push_back({
            .mount_point = unescape_octal(before[4]),
            .filesystem = filesystem,
            .device = device,
            .connection = connection,
        });
    }
    return mounts;
}

std::vector<FuseSample> read_fuse_samples(const std::filesystem::path& mountinfo,
                                          const std::filesystem::path& connections) {
    std::ifstream input(mountinfo);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::vector<FuseSample> samples;
    for (auto& mount : parse_fuse_mounts(text)) {
        FuseSample sample;
        const auto path = connections / std::to_string(mount.connection) / "waiting";
        sample.mount = std::move(mount);
        errno = 0;
        if (std::FILE* file = std::fopen(path.c_str(), "re")) {
            unsigned long long waiting{};
            if (std::fscanf(file, "%llu", &waiting) == 1) {
                sample.access = FuseAccess::ok;
                sample.waiting = waiting;
            }
            std::fclose(file);
        } else {
            sample.access = (errno == EACCES || errno == EPERM) ? FuseAccess::permission_denied
                                                                : FuseAccess::missing;
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

std::string_view to_string(FuseCondition condition) {
    switch (condition) {
        case FuseCondition::unavailable: return "unavailable";
        case FuseCondition::idle: return "idle";
        case FuseCondition::active: return "active";
        case FuseCondition::stalled: return "stalled";
    }
    return "unknown";
}

FuseTracker::FuseTracker(double stall_after_seconds) : stall_after_seconds_(stall_after_seconds) {}

FuseAssessment FuseTracker::observe(const FuseSample& sample, double elapsed_seconds) {
    const auto connection = sample.mount.connection;
    if (sample.access != FuseAccess::ok) {
        history_.erase(connection);
        return {.condition = FuseCondition::unavailable,
                .severity = 0.0,
                .reason = sample.access == FuseAccess::permission_denied ? "permission_denied"
                                                                         : "fusectl_missing",
                .waiting_seconds = 0.0,
                .unchanged_seconds = 0.0};
    }
    if (sample.waiting == 0) {
        history_.erase(connection);
        return {.condition = FuseCondition::idle,
                .severity = 0.0,
                .reason = "no_requests",
                .waiting_seconds = 0.0,
                .unchanged_seconds = 0.0};
    }
    auto [entry, inserted] = history_.try_emplace(connection);
    auto& history = entry->second;
    if (!inserted) {
        history.waiting_seconds += elapsed_seconds;
        history.unchanged_seconds =
            history.waiting == sample.waiting ? history.unchanged_seconds + elapsed_seconds : 0.0;
    }
    history.waiting = sample.waiting;
    const bool stalled = history.unchanged_seconds >= stall_after_seconds_;
    return {.condition = stalled ? FuseCondition::stalled : FuseCondition::active,
            .severity = stalled ? 0.8 : 0.01,
            .reason = stalled ? "no_progress" : "requests_in_flight",
            .waiting_seconds = history.waiting_seconds,
            .unchanged_seconds = history.unchanged_seconds};
}

std::string to_json(const FuseEvent& event) {
    const auto& sample = event.sample;
    const auto& assessment = event.assessment;
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\"schema_version\":" << FuseEvent::schema_version
           << ",\"event_type\":\"resource_health\",\"timestamp\":\""
           << json::escape(event.timestamp) << "\",\"resource\":\"fuse\",\"condition\":\""
           << to_string(assessment.condition) << "\",\"severity\":" << assessment.severity
           << ",\"interval_seconds\":" << event.interval_seconds << ",\"mount\":{\"path\":\""
           << json::escape(sample.mount.mount_point) << "\",\"filesystem\":\""
           << json::escape(sample.mount.filesystem) << "\",\"device\":\""
           << json::escape(sample.mount.device) << "\",\"connection\":" << sample.mount.connection
           << "},\"evidence\":{\"source\":\"fusectl\",\"assessment_reason\":\""
           << assessment.reason << "\",\"readable\":"
           << (sample.access == FuseAccess::ok ? "true" : "false")
           << ",\"waiting_requests\":" << sample.waiting
           << ",\"waiting_seconds\":" << assessment.waiting_seconds
           << ",\"unchanged_seconds\":" << assessment.unchanged_seconds
           << ",\"stall_after_seconds\":" << fuse_stall_after_seconds << "}}";
    return output.str();
}

}  // namespace graywatch
