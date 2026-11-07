#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace graywatch {

struct ProcessResult {
    int exit_code{-1};       // exit status, or -1 when killed or not started
    bool timed_out{};
    bool truncated{};        // output exceeded the byte limit
    std::string output;      // standard output
    std::string error;       // standard error (bounded the same way)
};

// Run argv[0] (looked up in PATH) without a shell, capturing stdout/stderr.
// The child is killed when the timeout expires. Never throws for a failing
// child; throws std::runtime_error only when fork or pipe creation fails.
ProcessResult run_process(const std::vector<std::string>& argv,
                          std::chrono::milliseconds timeout,
                          std::size_t maximum_bytes = 256 * 1024 * 1024);

}  // namespace graywatch
