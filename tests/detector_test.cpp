#include "graywatch/detector.hpp"

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
        graywatch::DetectorConfig config{
            .baseline_window = 8,
            .minimum_baseline_samples = 5,
            .degrade_after = 3,
            .recover_after = 2,
            .sigma_threshold = 3.0,
            .minimum_stddev = 0.01,
        };
        graywatch::RollingDetector detector(config);

        for (const double value : {1.00, 1.02, 0.98, 1.01}) {
            const auto result = detector.observe(value);
            require(result.state == graywatch::HealthState::learning,
                    "detector left learning too early");
        }
        auto result = detector.observe(0.99);
        require(result.state == graywatch::HealthState::normal,
                "detector did not finish learning");
        require(result.changed, "learning-to-normal transition was not reported");

        result = detector.observe(8.0);
        require(result.state == graywatch::HealthState::normal && result.anomalous,
                "first anomaly changed state");
        result = detector.observe(8.0);
        require(result.state == graywatch::HealthState::normal,
                "second anomaly changed state");
        result = detector.observe(8.0);
        require(result.state == graywatch::HealthState::degraded,
                "persistent anomaly was not degraded");
        require(result.changed, "degradation transition was not reported");
        require(result.baseline_samples == 5,
                "anomalies contaminated the baseline");

        result = detector.observe(1.0);
        require(result.state == graywatch::HealthState::degraded,
                "detector recovered too early");
        result = detector.observe(1.01);
        require(result.state == graywatch::HealthState::normal,
                "detector did not recover");
        require(result.changed, "recovery transition was not reported");

        bool rejected = false;
        try {
            (void)detector.observe(-1.0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "negative observation was accepted");

        rejected = false;
        try {
            (void)graywatch::RollingDetector(graywatch::DetectorConfig{
                .baseline_window = 2,
                .minimum_baseline_samples = 3,
            });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "invalid configuration was accepted");
    } catch (const std::exception& error) {
        std::cerr << "detector_test: " << error.what() << '\n';
        return 1;
    }
}
