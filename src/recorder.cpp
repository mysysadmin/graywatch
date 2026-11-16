#include "graywatch/recorder.hpp"

#include "graywatch/json.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace graywatch {

namespace {

std::uint64_t parse_number(std::string_view digits, std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::invalid_argument("invalid value: " + std::string(text));
    }
    return value;
}

std::string utc_name(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%dT%H%M%SZ", &utc);
    return text;
}

// events-20261116T202000Z.jsonl[.gz] -> its start time.
std::optional<std::chrono::system_clock::time_point> segment_start(std::string_view name) {
    constexpr std::string_view prefix = "events-";
    if (!name.starts_with(prefix) || name.size() < prefix.size() + 16) return std::nullopt;
    const std::string stamp(name.substr(prefix.size(), 16));
    std::tm utc{};
    if (std::sscanf(stamp.c_str(), "%4d%2d%2dT%2d%2d%2dZ", &utc.tm_year, &utc.tm_mon,
                    &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6) {
        return std::nullopt;
    }
    utc.tm_year -= 1900;
    utc.tm_mon -= 1;
    return std::chrono::system_clock::from_time_t(timegm(&utc));
}

bool is_segment(std::string_view name) {
    return segment_start(name) &&
           (name.ends_with(".jsonl") || name.ends_with(".jsonl.gz"));
}

// Sorted by name, which is chronological for both file families.
std::vector<std::filesystem::path> files_with_prefix(const std::filesystem::path& directory,
                                                     std::string_view prefix) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && entry.is_regular_file(error)) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t written = ::write(fd, data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

// Replace a small file atomically, so a reader never sees half of it.
void replace_file(const std::filesystem::path& path, std::string_view content) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << content;
        if (!output) throw std::runtime_error("cannot write " + temporary);
    }
    std::filesystem::rename(temporary, path);
}

}  // namespace

std::chrono::seconds parse_duration(std::string_view text) {
    std::string_view digits = text;
    std::uint64_t unit = 1;
    if (!digits.empty()) {
        switch (digits.back()) {
            case 's': unit = 1; break;
            case 'm': unit = 60; break;
            case 'h': unit = 3600; break;
            case 'd': unit = 86400; break;
            default: unit = 0; break;
        }
        if (unit != 0) digits.remove_suffix(1);
        else unit = 1;
    }
    const auto value = parse_number(digits, text);
    if (value == 0) throw std::invalid_argument("duration must be positive: " + std::string(text));
    return std::chrono::seconds(value * unit);
}

std::uint64_t parse_size(std::string_view text) {
    std::string_view digits = text;
    std::uint64_t unit = 1;
    if (!digits.empty()) {
        switch (digits.back()) {
            case 'K': case 'k': unit = 1ull << 10; break;
            case 'M': case 'm': unit = 1ull << 20; break;
            case 'G': case 'g': unit = 1ull << 30; break;
            case 'T': case 't': unit = 1ull << 40; break;
            default: unit = 0; break;
        }
        if (unit != 0) digits.remove_suffix(1);
        else unit = 1;
    }
    const auto value = parse_number(digits, text);
    if (value == 0) throw std::invalid_argument("size must be positive: " + std::string(text));
    return value * unit;
}

std::string json_string_field(std::string_view json, std::string_view name) {
    const std::string key = "\"" + std::string(name) + "\":\"";
    const auto start = json.find(key);
    if (start == std::string_view::npos) return {};
    const auto begin = start + key.size();
    const auto end = json.find('"', begin);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(begin, end - begin));
}

double json_number_field(std::string_view json, std::string_view name) {
    const std::string key = "\"" + std::string(name) + "\":";
    const auto start = json.find(key);
    if (start == std::string_view::npos) return 0.0;
    const std::string rest(json.substr(start + key.size(), 32));
    return std::strtod(rest.c_str(), nullptr);
}

// --- Compressor -------------------------------------------------------------

Compressor::~Compressor() {
    wait_all();
}

void Compressor::compress(const std::filesystem::path& path) {
    const std::string file = path.string();
    std::vector<char*> argv{const_cast<char*>("gzip"), const_cast<char*>("-f"),
                            const_cast<char*>("-n"), const_cast<char*>("-q"),
                            const_cast<char*>(file.c_str()), nullptr};
    pid_t pid{};
    // Without gzip the file just stays uncompressed.
    if (posix_spawnp(&pid, "gzip", nullptr, nullptr, argv.data(), environ) == 0) {
        children_.push_back(pid);
    }
}

void Compressor::reap() {
    std::erase_if(children_, [](pid_t pid) {
        int status{};
        const pid_t result = waitpid(pid, &status, WNOHANG);
        return result == pid || (result < 0 && errno == ECHILD);
    });
}

void Compressor::wait_all() {
    for (const pid_t pid : children_) {
        int status{};
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    children_.clear();
}

// --- EventRecorder ----------------------------------------------------------

EventRecorder::EventRecorder(std::filesystem::path directory, std::chrono::seconds keep,
                             std::uint64_t max_bytes, Compressor& compressor)
    : directory_(std::move(directory)),
      keep_(keep),
      max_bytes_(max_bytes),
      // Short recordings rotate often so little more than `keep` is retained;
      // long ones in hours, to keep the number of files reasonable.
      segment_(keep <= std::chrono::hours(2) ? std::chrono::minutes(5) : std::chrono::hours(1)),
      compressor_(compressor) {
    std::filesystem::create_directories(directory_);
    // Segments left uncompressed by an earlier run (crash, reboot), except the
    // current one, which this run appends to.
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    const auto current_start = "events-" +
        utc_name(std::chrono::system_clock::time_point(since_epoch - since_epoch % segment_));
    for (const auto& file : files_with_prefix(directory_, "events-")) {
        const auto name = file.filename().string();
        if (file.extension() == ".jsonl" && is_segment(name) && !name.starts_with(current_start)) {
            compressor_.compress(file);
        }
    }
}

EventRecorder::~EventRecorder() {
    close_current();
}

void EventRecorder::open(std::chrono::system_clock::time_point now) {
    const auto since_epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    const auto start = std::chrono::system_clock::time_point(since_epoch - since_epoch % segment_);
    segment_end_ = start + segment_;
    // A segment compressed by an earlier run of this daemon (a restart within
    // the same segment) is continued in a new file, never overwritten. "_2" sorts
    // after the first part ('_' > '.'), so names stay chronological.
    const std::string base = "events-" + utc_name(start);
    current_ = directory_ / (base + ".jsonl");
    for (int part = 2; std::filesystem::exists(current_.string() + ".gz"); ++part) {
        current_ = directory_ / (base + "_" + std::to_string(part) + ".jsonl");
    }
    fd_ = ::open(current_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd_ < 0 && !write_failed_) {
        std::cerr << "graywatchd: cannot open " << current_ << ": " << std::strerror(errno) << '\n';
        write_failed_ = true;
    }
}

void EventRecorder::close_current() {
    flush();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        compressor_.compress(current_);
    }
}

void EventRecorder::write(std::string_view line, std::chrono::system_clock::time_point now) {
    if (fd_ < 0 || now >= segment_end_) {
        close_current();
        open(now);
        prune(now);
    }
    buffer_.append(line);
    buffer_.push_back('\n');
}

void EventRecorder::flush() {
    if (buffer_.empty()) return;
    if (fd_ >= 0) {
        if (write_all(fd_, buffer_)) {
            write_failed_ = false;
        } else if (!write_failed_) {
            // A full disk loses recording, never the daemon.
            std::cerr << "graywatchd: cannot write " << current_ << ": " << std::strerror(errno)
                      << '\n';
            write_failed_ = true;
        }
    }
    buffer_.clear();
}

void EventRecorder::prune(std::chrono::system_clock::time_point now) {
    auto files = files_with_prefix(directory_, "events-");
    std::erase_if(files, [](const auto& file) { return !is_segment(file.filename().string()); });
    std::error_code error;
    std::vector<std::filesystem::path> kept;
    for (const auto& file : files) {
        const auto start = segment_start(file.filename().string());
        if (file != current_ && *start + segment_ + keep_ < now) {
            std::filesystem::remove(file, error);
        } else {
            kept.push_back(file);
        }
    }
    std::uint64_t total{};
    for (const auto& file : kept) total += std::filesystem::file_size(file, error) * !error;
    for (const auto& file : kept) {
        if (total <= max_bytes_ || file == current_) break;
        const auto size = std::filesystem::file_size(file, error);
        if (std::filesystem::remove(file, error)) total -= std::min(total, size);
    }
}

// --- ChangeLog --------------------------------------------------------------

namespace {
// A key's problem begins when at least half of its last `window` intervals
// were non-normal, and ends when at most a tenth were. A device that is bad
// every other second is one long problem, not hundreds of short ones.
constexpr unsigned window_intervals = 30;
constexpr unsigned begin_count = window_intervals / 2;
constexpr unsigned end_count = window_intervals / 10;
}  // namespace

ChangeLog::ChangeLog(std::filesystem::path directory, std::uint64_t file_bytes, std::size_t files, Compressor& compressor)
    : directory_(std::move(directory)),
      path_(directory_ / "changes.jsonl"),
      file_bytes_(file_bytes),
      files_(std::max<std::size_t>(files, 1)),
      compressor_(compressor) {
    std::filesystem::create_directories(directory_);
}

ChangeLog::~ChangeLog() = default;

bool ChangeLog::normal(std::string_view condition) {
    return condition.empty() || condition == "healthy" || condition == "idle" ||
           condition == "no_pressure" || condition == "inactive" || condition == "active" ||
           // Missing data (PSI off, no permission) is a coverage gap, not a problem.
           condition == "unavailable";
}

void ChangeLog::append(const std::string& line) {
    {
        std::ofstream output(path_, std::ios::app);
        output << line << '\n';
        if (!output) return;
    }
    std::error_code error;
    if (std::filesystem::file_size(path_, error) <= file_bytes_ || error) return;
    const std::string base = "changes-" + utc_name(std::chrono::system_clock::now());
    auto rotated = directory_ / (base + ".jsonl");
    for (int part = 2; std::filesystem::exists(rotated) || std::filesystem::exists(rotated.string() + ".gz"); ++part) {
        rotated = directory_ / (base + "_" + std::to_string(part) + ".jsonl");
    }
    std::filesystem::rename(path_, rotated, error);
    if (error) return;
    compressor_.compress(rotated);
    auto old = files_with_prefix(directory_, "changes-");
    while (old.size() > files_ - 1) {
        std::filesystem::remove(old.front(), error);
        old.erase(old.begin());
    }
}

void ChangeLog::marker(std::string_view what, std::string_view timestamp) {
    std::ostringstream line;
    line << "{\"timestamp\":\"" << json::escape(timestamp) << "\",\"change\":\""
         << json::escape(what) << "\"}";
    append(line.str());
    if (what == "daemon_started") states_.clear();
}

void ChangeLog::observe(std::string_view key, std::string_view resource, std::string_view json,
                        std::chrono::steady_clock::time_point now, std::string_view timestamp) {
    const std::string condition = json_string_field(json, "condition");
    const double severity = json_number_field(json, "severity");
    const bool bad = !normal(condition);
    auto found = states_.find(key);
    if (found == states_.end()) found = states_.emplace(std::string(key), State{}).first;
    State& state = found->second;

    state.history = (state.history << 1) | (bad ? 1u : 0u);
    state.history &= (1ull << window_intervals) - 1;
    const auto count = static_cast<unsigned>(__builtin_popcountll(state.history));
    if (bad && (severity > state.worst_severity || state.worst_condition.empty())) {
        state.worst_severity = severity;
        state.worst_condition = condition;
        if (!state.problem) state.first_event = std::string(json);
    }

    std::ostringstream line;
    if (!state.problem && count >= begin_count) {
        state.problem = true;
        state.began = now;
        state.began_timestamp = std::string(timestamp);
        line << "{\"timestamp\":\"" << json::escape(timestamp) << "\",\"key\":\""
             << json::escape(key) << "\",\"resource\":\"" << json::escape(resource)
             << "\",\"change\":\"began\",\"condition\":\"" << json::escape(state.worst_condition)
             << "\",\"severity\":" << state.worst_severity << ",\"event\":"
             << (state.first_event.empty() ? std::string("null") : state.first_event) << '}';
        append(line.str());
        state.first_event.clear();
    } else if (state.problem && count <= end_count) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - state.began);
        line << "{\"timestamp\":\"" << json::escape(timestamp) << "\",\"key\":\""
             << json::escape(key) << "\",\"resource\":\"" << json::escape(resource)
             << "\",\"change\":\"ended\",\"began\":\"" << json::escape(state.began_timestamp)
             << "\",\"duration_seconds\":" << seconds.count() << ",\"worst_condition\":\""
             << json::escape(state.worst_condition) << "\",\"worst_severity\":"
             << state.worst_severity << '}';
        append(line.str());
        state = State{};
    } else if (!state.problem && count == 0) {
        // Forget an isolated spike, so the next problem reports its own worst.
        state.worst_condition.clear();
        state.worst_severity = 0.0;
        state.first_event.clear();
    }
}

// --- CaptureTrigger ---------------------------------------------------------

CaptureTrigger::CaptureTrigger(std::filesystem::path request,
                               std::optional<std::filesystem::path> state, double severity,
                               std::chrono::seconds after, std::chrono::seconds cooldown)
    : request_(std::move(request)),
      state_(std::move(state)),
      severity_(severity),
      after_(after),
      cooldown_(cooldown) {
    if (state_) {
        std::ifstream input(*state_);
        long long seconds{};
        if (input >> seconds) {
            last_ = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
        }
    }
}

std::optional<std::string> CaptureTrigger::observe(std::string_view key, std::string_view json,
                                                   std::chrono::steady_clock::time_point now,
                                                   std::chrono::system_clock::time_point wall,
                                                   std::string_view timestamp) {
    const double severity = json_number_field(json, "severity");
    if (severity < severity_) {
        if (const auto found = since_.find(key); found != since_.end()) since_.erase(found);
        return std::nullopt;
    }
    auto found = since_.find(key);
    if (found == since_.end()) found = since_.emplace(std::string(key), now).first;
    const auto sustained = std::chrono::duration_cast<std::chrono::seconds>(now - found->second);
    if (sustained < after_) return std::nullopt;
    if (last_ && wall - *last_ < cooldown_) return std::nullopt;

    std::ostringstream line;
    line << "{\"timestamp\":\"" << json::escape(timestamp) << "\",\"key\":\"" << json::escape(key)
         << "\",\"condition\":\"" << json::escape(json_string_field(json, "condition"))
         << "\",\"severity\":" << severity << ",\"sustained_seconds\":" << sustained.count()
         << "}\n";
    try {
        replace_file(request_, line.str());
    } catch (const std::exception& error) {
        std::cerr << "graywatchd: capture request: " << error.what() << '\n';
        return std::nullopt;
    }
    last_ = wall;
    if (state_) {
        try {
            replace_file(*state_, std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                     wall.time_since_epoch())
                                                     .count()) +
                                      "\n");
        } catch (const std::exception&) {
            // The cooldown then only lasts until the next restart.
        }
    }
    std::string result = line.str();
    result.pop_back();
    return result;
}

}  // namespace graywatch
