#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace graywatch::report {

// Text and Markdown reports wrap at this many columns.
inline constexpr std::size_t report_width = 80;

// Archive member base name -> contents.
using Files = std::map<std::string, std::string>;

// Read a support archive (.tar.gz, read through tar without extracting to
// disk) or a directory holding its extracted files. Throws on failure.
Files load_bundle(const std::filesystem::path& path);

enum class Severity { critical, warning, info };

struct Finding {
    Severity severity{Severity::info};
    std::string area;
    std::string title;
    std::vector<std::string> details;
    std::vector<std::string> next_steps;
};

struct TimelineEntry {
    std::string start;      // ISO timestamp
    std::string area;
    std::string text;
    std::size_t count{};
    std::string unit;       // "interval" or "snapshot"
    double peak_severity{-1.0};
};

struct Report {
    std::map<std::string, std::string> system;
    std::string psi_status;
    std::string window_start;
    std::string window_end;
    std::size_t intervals{};
    std::vector<Finding> findings;          // ranked, most severe first
    std::vector<std::string> conclusions;   // cross-signal assessment
    std::vector<TimelineEntry> timeline;    // sorted by start
    std::vector<std::string> overview;      // per-resource summary lines
    // One line per block device and request type. Storage servers have dozens
    // of disks: text and Markdown print these only when there are few, JSON
    // always.
    std::vector<std::string> device_overview;
    std::vector<std::string> coverage;      // limits of this collection
    // Platform settings worth comparing between hosts (MTU, NIC speed, block
    // scheduler and writeback settings, sysctls), name -> value.
    std::map<std::string, std::string> settings;
};

Report analyze(const Files& files);

enum class Format { text, markdown, json };
std::string render(const Report& report, Format format);

std::string_view to_string(Severity severity);

// One summary for archives collected on several hosts in the same window: a
// finding that at least half of the hosts share (same title) is listed once
// with the hosts that have it, the rest per host. Info findings are left out.
std::string render_cluster(const std::vector<Report>& reports);

}  // namespace graywatch::report
