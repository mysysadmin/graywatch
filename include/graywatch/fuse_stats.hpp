#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace graywatch {

// A mounted FUSE filesystem and its kernel connection. The connection is the
// directory name under /sys/fs/fuse/connections: the superblock's kernel
// dev_t (major << 20 | minor), e.g. 49 for device 0:49.
struct FuseMount {
    std::string mount_point;
    std::string filesystem;  // e.g. fuse.quobyte, fuseblk
    std::string device;      // "major:minor" from mountinfo
    std::uint64_t connection{};
};

// FUSE mounts from a /proc/<pid>/mountinfo text, one entry per connection
// (bind mounts of the same filesystem are listed once).
std::vector<FuseMount> parse_fuse_mounts(std::string_view mountinfo);

enum class FuseAccess {
    ok,
    permission_denied,  // fusectl files are mode 0400, owned by the mount owner
    missing,            // fusectl not mounted, or the connection went away
};

struct FuseSample {
    FuseMount mount;
    FuseAccess access{FuseAccess::missing};
    // Requests sent to the FUSE daemon that have not been answered yet.
    std::uint64_t waiting{};
};

std::vector<FuseSample> read_fuse_samples(
    const std::filesystem::path& mountinfo = "/proc/self/mountinfo",
    const std::filesystem::path& connections = "/sys/fs/fuse/connections");

enum class FuseCondition { unavailable, idle, active, stalled };
std::string_view to_string(FuseCondition condition);

struct FuseAssessment {
    FuseCondition condition{FuseCondition::unavailable};
    double severity{};
    std::string_view reason{"not_sampled"};
    // How long the mount has continuously had waiting requests, and how long
    // the waiting count has stayed exactly the same, in seconds.
    double waiting_seconds{};
    double unchanged_seconds{};
};

// Seconds a non-zero waiting count must stay unchanged before a mount is
// `stalled`. Under load the count moves as requests complete and new ones
// start; a count frozen for this long means the daemon answered nothing.
inline constexpr double fuse_stall_after_seconds = 30.0;

// Classifies successive samples of each connection.
class FuseTracker {
  public:
    explicit FuseTracker(double stall_after_seconds = fuse_stall_after_seconds);
    FuseAssessment observe(const FuseSample& sample, double elapsed_seconds);

  private:
    struct History {
        std::uint64_t waiting{};
        double waiting_seconds{};
        double unchanged_seconds{};
    };
    double stall_after_seconds_;
    std::map<std::uint64_t, History> history_;
};

struct FuseEvent {
    static constexpr unsigned int schema_version = 1;

    std::string timestamp;
    double interval_seconds{};
    FuseSample sample;
    FuseAssessment assessment;
};

std::string to_json(const FuseEvent& event);

}  // namespace graywatch
