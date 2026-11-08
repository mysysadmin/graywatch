#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace graywatch {

namespace json {
class Value;
}

// `graywatchctl cluster HOST... [--user USER] [--timeout SECONDS]
// [--remote-command PATH] [--socket PATH] [--json]`: query graywatchd on each
// host over SSH in parallel and print a one-line summary per host. Returns 0
// when every host answered, 1 otherwise.
int run_cluster(int argc, char** argv, int first_argument);

// Busy share below which a healthy device is not worth showing in the summary.
inline constexpr double cluster_minimum_busy_percent = 1.0;

// The device the cluster summary shows from a `status --resource io` array:
// the most severe congested, saturated or degraded device; otherwise the
// busiest healthy device at or above cluster_minimum_busy_percent; otherwise
// none (nullptr).
const json::Value* notable_device(const json::Value& io);

// The FUSE column from a `status --resource fuse` array: "STALLED <mount> <s>"
// for the longest-stalled mount (+N more), "active w=N" with requests in
// flight, "idle", "no-access" when the daemon cannot read fusectl (it runs
// unprivileged), or "-" without FUSE mounts or an older daemon.
std::string fuse_summary(const json::Value& fuse);

// Lay out a table: each column is as wide as its widest cell, columns are
// separated by two spaces, and the last column is not padded. Columns listed
// in RIGHT_ALIGNED are right-aligned. A row may have fewer cells than the
// header; its last cell then runs to the end of the line.
std::string format_columns(const std::vector<std::vector<std::string>>& rows,
                           const std::vector<std::size_t>& right_aligned = {});

}  // namespace graywatch
