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

        // Reported statistics are the ones the decision used: a first anomaly
        // is compared with, and reported against, the unchanged baseline.
        {
            graywatch::RollingDetector judged(config);
            for (const double value : {1.0, 1.0, 1.0, 1.0, 1.0}) (void)judged.observe(value);
            const auto normal = judged.observe(1.0);
            require(normal.baseline_samples == 5 && !normal.anomalous,
                    "reported baseline is not the compared one");
            const auto anomaly = judged.observe(8.0);
            require(anomaly.anomalous && anomaly.current > anomaly.threshold,
                    "anomaly not above its reported threshold");
            require(anomaly.anomaly_streak == 1, "anomaly streak not counted");
            require(judged.observe(8.0).anomaly_streak == 2, "anomaly streak not incremented");
        }

        // Absolute floor: with an all-zero baseline, 0.16% is unusual but too
        // small to matter; 5% is both unusual and above the floor.
        {
            auto floored_config = config;
            floored_config.minimum_anomaly = 1.0;
            graywatch::RollingDetector floored(floored_config);
            for (int sample = 0; sample < 5; ++sample) (void)floored.observe(0.0);
            const auto small = floored.observe(0.16);
            require(!small.anomalous, "activity below the floor was anomalous");
            require(small.floor_applied && small.threshold == 1.0,
                    "floor not reported as the threshold");
            const auto large = floored.observe(5.0);
            require(large.anomalous, "stall above the floor was not anomalous");

            graywatch::RollingDetector unfloored(config);
            for (int sample = 0; sample < 5; ++sample) (void)unfloored.observe(0.0);
            const auto noise = unfloored.observe(0.16);
            require(noise.anomalous && !noise.floor_applied,
                    "without a floor, a zero baseline should flag any activity");
        }

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

        rejected = false;
        try {
            auto negative_floor = config;
            negative_floor.minimum_anomaly = -1.0;
            (void)graywatch::RollingDetector(negative_floor);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "negative floor was accepted");
    } catch (const std::exception& error) {
        std::cerr << "detector_test: " << error.what() << '\n';
        return 1;
    }
}
