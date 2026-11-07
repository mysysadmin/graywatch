#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace graywatch::report {

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
    std::vector<std::string> coverage;      // limits of this collection
};

Report analyze(const Files& files);

enum class Format { text, markdown, json };
std::string render(const Report& report, Format format);

std::string_view to_string(Severity severity);

}  // namespace graywatch::report
