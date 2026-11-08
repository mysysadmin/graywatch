#include "graywatch/fuse_stats.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const char* mountinfo =
    "22 1 253:0 / / rw,relatime shared:1 - xfs /dev/mapper/rl-root rw\n"
    "80 22 0:49 / /quobyte rw,nosuid,nodev,relatime shared:40 - fuse.quobyte "
    "quobyte@registry rw,user_id=0,group_id=0,allow_other\n"
    "81 22 0:49 / /srv/bind rw,relatime shared:40 - fuse.quobyte quobyte@registry rw\n"
    "82 22 0:51 / /mnt/with\\040space rw,relatime - fuse sshfs rw\n"
    "83 22 8:17 / /mnt/ntfs rw,relatime - fuseblk /dev/sdb1 rw\n"
    "84 22 0:52 / /mnt/nfs rw,relatime - nfs4 server:/export rw\n";

}  // namespace

int main() {
    try {
        using graywatch::FuseCondition;

        // Mount parsing: FUSE only, bind mounts once, escapes decoded, kernel dev_t.
        const auto mounts = graywatch::parse_fuse_mounts(mountinfo);
        require(mounts.size() == 3, "expected three FUSE connections");
        require(mounts[0].mount_point == "/quobyte" && mounts[0].filesystem == "fuse.quobyte" &&
                    mounts[0].connection == 49 && mounts[0].device == "0:49",
                "fuse.quobyte mount parsed");
        require(mounts[1].mount_point == "/mnt/with space", "octal escape decoded");
        require(mounts[2].filesystem == "fuseblk" && mounts[2].connection == ((8ull << 20) | 17),
                "fuseblk connection is the kernel dev_t");

        // Reading fusectl: a readable connection and a missing one.
        const auto root = std::filesystem::temp_directory_path() /
                          ("graywatch-fuse-test-" + std::to_string(getpid()));
        std::filesystem::create_directories(root / "connections" / "49");
        { std::ofstream(root / "mountinfo") << mountinfo; }
        { std::ofstream(root / "connections" / "49" / "waiting") << "3\n"; }
        const auto samples =
            graywatch::read_fuse_samples(root / "mountinfo", root / "connections");
        std::filesystem::remove_all(root);
        require(samples.size() == 3, "one sample per mount");
        require(samples[0].access == graywatch::FuseAccess::ok && samples[0].waiting == 3,
                "waiting count read");
        require(samples[1].access == graywatch::FuseAccess::missing, "missing connection");

        // Tracker: idle, active, stalled after a frozen count, reset on change.
        graywatch::FuseTracker tracker(30.0);
        graywatch::FuseSample sample{.mount = mounts[0], .access = graywatch::FuseAccess::ok,
                                     .waiting = 0};
        require(tracker.observe(sample, 1.0).condition == FuseCondition::idle, "idle");
        sample.waiting = 1;
        auto assessment = tracker.observe(sample, 1.0);
        require(assessment.condition == FuseCondition::active && assessment.unchanged_seconds == 0,
                "first waiting sample is active");
        for (int second = 0; second < 29; ++second) assessment = tracker.observe(sample, 1.0);
        require(assessment.condition == FuseCondition::active, "not stalled before 30 s");
        assessment = tracker.observe(sample, 1.0);
        require(assessment.condition == FuseCondition::stalled && assessment.severity == 0.8 &&
                    assessment.unchanged_seconds == 30.0 && assessment.waiting_seconds == 30.0,
                "frozen count for 30 s is stalled");
        sample.waiting = 2;
        assessment = tracker.observe(sample, 1.0);
        require(assessment.condition == FuseCondition::active && assessment.unchanged_seconds == 0 &&
                    assessment.waiting_seconds == 31.0,
                "a changing count is progress");
        sample.waiting = 0;
        require(tracker.observe(sample, 1.0).condition == FuseCondition::idle, "back to idle");
        sample.access = graywatch::FuseAccess::permission_denied;
        assessment = tracker.observe(sample, 1.0);
        require(assessment.condition == FuseCondition::unavailable &&
                    assessment.reason == "permission_denied",
                "unreadable fusectl is unavailable");

        // Event JSON.
        sample.access = graywatch::FuseAccess::ok;
        sample.waiting = 1;
        graywatch::FuseTracker fresh;
        const graywatch::FuseEvent event{.timestamp = "2026-11-08T01:00:00.000Z",
                                         .interval_seconds = 1.0,
                                         .sample = sample,
                                         .assessment = fresh.observe(sample, 1.0)};
        const auto json = graywatch::to_json(event);
        for (const char* needle :
             {"\"resource\":\"fuse\"", "\"condition\":\"active\"", "\"path\":\"/quobyte\"",
              "\"connection\":49", "\"waiting_requests\":1", "\"readable\":true"}) {
            require(json.find(needle) != std::string::npos, needle);
        }
    } catch (const std::exception& error) {
        std::cerr << "fuse_stats_test: " << error.what() << '\n';
        return 1;
    }
}
