#pragma once

// Persistence for the long-running daemon: a rotating event recording (the
// flight recorder, or a full recording with a longer retention), a small log
// of problem periods, and the trigger that asks for an automatic collection.
// All of it is plain JSON lines; closed files are compressed with gzip.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace graywatch {

// "90s", "30m", "12h", "7d"; a bare number is seconds. Throws on bad input.
std::chrono::seconds parse_duration(std::string_view text);
// "500M", "2G", "64K"; a bare number is bytes. Throws on bad input.
std::uint64_t parse_size(std::string_view text);

// The value of the first "name":"..." or "name":number in a JSON line. The
// daemon's events put condition and severity before any nested object.
std::string json_string_field(std::string_view json, std::string_view name);
double json_number_field(std::string_view json, std::string_view name);

// Compresses closed files in the background and reaps the gzip children,
// so the sampling loop never waits for compression.
class Compressor {
public:
    ~Compressor();
    void compress(const std::filesystem::path& path);
    void reap();
    void wait_all();
    bool idle() const { return children_.empty(); }

private:
    std::vector<pid_t> children_;
};

// Every event line, in files of `segment` length named by their UTC start
// (events-20261116T202000Z.jsonl). Files older than `keep` are deleted, and
// the oldest ones also whenever the directory holds more than `max_bytes`.
class EventRecorder {
public:
    EventRecorder(std::filesystem::path directory, std::chrono::seconds keep,
                  std::uint64_t max_bytes, Compressor& compressor);
    ~EventRecorder();
    void write(std::string_view line, std::chrono::system_clock::time_point now);
    void flush();
    std::chrono::seconds segment() const { return segment_; }

private:
    void open(std::chrono::system_clock::time_point now);
    void close_current();
    void prune(std::chrono::system_clock::time_point now);

    std::filesystem::path directory_;
    std::chrono::seconds keep_;
    std::uint64_t max_bytes_;
    std::chrono::seconds segment_;
    Compressor& compressor_;
    int fd_{-1};
    std::filesystem::path current_;
    std::chrono::system_clock::time_point segment_end_{};
    std::string buffer_;
    bool write_failed_{};
};

// Problem periods per resource key (cpu, memory, io/sda, network/tcp,
// network/ib/mlx5_0/1, fuse/MOUNT). A "began" line is written once at least
// half of a key's last 30 intervals were non-normal, an "ended" line with
// duration and worst condition once at most three of them were. A device that
// is bad every other second is one long problem, not hundreds of short ones,
// and momentary spikes are not logged, so the file stays small. It rotates at
// `file_bytes` and keeps `files` files.
class ChangeLog {
public:
    ChangeLog(std::filesystem::path directory, std::uint64_t file_bytes, std::size_t files, Compressor& compressor);
    ~ChangeLog();
    void observe(std::string_view key, std::string_view resource,
                 std::string_view json, std::chrono::steady_clock::time_point now,
                 std::string_view timestamp);
    void marker(std::string_view what, std::string_view timestamp);

    static bool normal(std::string_view condition);

private:
    struct State {
        std::uint64_t history{};  // one bit per interval, 1 = non-normal
        bool problem{};
        std::chrono::steady_clock::time_point began{};
        std::string began_timestamp;
        std::string worst_condition;
        double worst_severity{};
        std::string first_event;  // the worst event before the problem began
    };
    void append(const std::string& line);

    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::uint64_t file_bytes_;
    std::size_t files_;
    Compressor& compressor_;
    std::map<std::string, State, std::less<>> states_;
};

// Asks for one automatic collection when a resource key stays at or above
// `severity` for `after`, at most once per `cooldown`. The request is a file
// that a root systemd path unit watches, so the daemon itself never needs
// privileges. The time of the last request survives restarts in `state`.
class CaptureTrigger {
public:
    CaptureTrigger(std::filesystem::path request, std::optional<std::filesystem::path> state,
                   double severity, std::chrono::seconds after, std::chrono::seconds cooldown);
    // Returns the request line when this observation fired a request.
    std::optional<std::string> observe(std::string_view key, std::string_view json,
                                       std::chrono::steady_clock::time_point now,
                                       std::chrono::system_clock::time_point wall,
                                       std::string_view timestamp);

private:
    std::filesystem::path request_;
    std::optional<std::filesystem::path> state_;
    double severity_;
    std::chrono::seconds after_;
    std::chrono::seconds cooldown_;
    std::optional<std::chrono::system_clock::time_point> last_;
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> since_;
};

}  // namespace graywatch
