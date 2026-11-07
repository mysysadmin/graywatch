#include "graywatch/psi.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const auto sample = graywatch::parse_psi_line(
            "io", "some avg10=1.25 avg60=0.50 avg300=0.10 total=987654");
        require(sample.resource == "io", "resource was not parsed");
        require(sample.scope == "some", "scope was not parsed");
        require(std::abs(sample.avg10 - 1.25) < 0.0001, "avg10 was not parsed");
        require(std::abs(sample.avg60 - 0.50) < 0.0001, "avg60 was not parsed");
        require(std::abs(sample.avg300 - 0.10) < 0.0001, "avg300 was not parsed");
        require(sample.total_us == 987654, "total was not parsed");

        bool rejected = false;
        try {
            (void)graywatch::parse_psi_line("cpu", "some avg10=1.0");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "invalid input was accepted");

        const std::vector<graywatch::PsiSample> previous{
            {.resource = "cpu", .scope = "some", .total_us = 1'000'000},
            {.resource = "io", .scope = "full", .total_us = 2'000'000},
        };
        const std::vector<graywatch::PsiSample> current{
            {.resource = "cpu", .scope = "some", .total_us = 1'250'000},
            {.resource = "io", .scope = "full", .total_us = 2'010'000},
            {.resource = "memory", .scope = "some", .total_us = 100},
        };
        const auto intervals =
            graywatch::calculate_psi_intervals(previous, current, 2.0);
        require(intervals.size() == 2, "unexpected interval count");
        require(intervals[0].stalled_us == 250'000, "incorrect stalled time");
        require(std::abs(intervals[0].stall_percent - 12.5) < 0.0001,
                "incorrect stall percentage");
        require(std::abs(intervals[1].stall_percent - 0.5) < 0.0001,
                "incorrect full-stall percentage");

        rejected = false;
        try {
            (void)graywatch::calculate_psi_intervals(current, previous, 2.0);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "backwards counter was accepted");

        rejected = false;
        try {
            (void)graywatch::calculate_psi_intervals(previous, current, 0.0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "zero duration was accepted");
    } catch (const std::exception& error) {
        std::cerr << "psi_test: " << error.what() << '\n';
        return 1;
    }
}
