#include "graywatch/task_stats.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const auto parsed = graywatch::parse_task_schedstat(
            100, 101, 1000, "worker", "25000000 5000000 42");
        require(parsed.runtime_ns == 25'000'000, "runtime was not parsed");
        require(parsed.runqueue_wait_ns == 5'000'000, "wait was not parsed");
        require(parsed.timeslices == 42, "timeslices were not parsed");

        const graywatch::TaskSample previous{
            .pid = 100, .tid = 101, .uid = 1000, .command = "old",
            .runtime_ns = 10'000'000, .runqueue_wait_ns = 2'000'000,
            .timeslices = 10,
        };
        const graywatch::TaskSample current{
            .pid = 100, .tid = 101, .uid = 1000, .command = "worker",
            .runtime_ns = 18'000'000, .runqueue_wait_ns = 4'000'000,
            .timeslices = 13,
        };
        const auto interval = graywatch::calculate_task_interval(previous, current);
        require(interval.has_value(), "valid interval was rejected");
        require(interval->runtime_ns == 8'000'000, "runtime delta is wrong");
        require(interval->runqueue_wait_ns == 2'000'000, "wait delta is wrong");
        require(std::abs(interval->runnable_wait_percent - 20.0) < 0.0001,
                "runnable wait percentage is wrong");

        auto reset = current;
        reset.runtime_ns = 1;
        require(!graywatch::calculate_task_interval(current, reset),
                "counter reset was accepted");

        const graywatch::TaskSample other{
            .pid = 200, .tid = 200, .uid = 1000, .command = "other",
            .runtime_ns = 5'000'000, .runqueue_wait_ns = 8'000'000,
            .timeslices = 5,
        };
        const auto intervals = graywatch::calculate_task_intervals(
            {previous, {.pid = 200, .tid = 200, .uid = 1000,
                        .command = "other", .runtime_ns = 1'000'000,
                        .runqueue_wait_ns = 1'000'000, .timeslices = 1}},
            {current, other});
        require(intervals.size() == 2, "interval matching failed");
        require(intervals.front().pid == 200, "intervals were not wait-sorted");
    } catch (const std::exception& error) {
        std::cerr << "task_stats_test: " << error.what() << '\n';
        return 1;
    }
}
