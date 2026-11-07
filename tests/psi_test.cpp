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
    } catch (const std::exception& error) {
        std::cerr << "psi_test: " << error.what() << '\n';
        return 1;
    }
}
