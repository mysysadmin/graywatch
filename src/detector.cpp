#include "graywatch/detector.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace graywatch {
namespace {

struct Summary {
    double mean{};
    double stddev{};
};

Summary summarize(const std::deque<double>& values) {
    if (values.empty()) {
        return {};
    }
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
    if (values.size() == 1) {
        return {.mean = mean, .stddev = 0.0};
    }
    double squared_error{};
    for (const double value : values) {
        const double difference = value - mean;
        squared_error += difference * difference;
    }
    return {
        .mean = mean,
        .stddev = std::sqrt(squared_error /
                           static_cast<double>(values.size() - 1)),
    };
}

}  // namespace

std::string_view to_string(HealthState state) {
    switch (state) {
        case HealthState::learning:
            return "LEARNING";
        case HealthState::normal:
            return "NORMAL";
        case HealthState::degraded:
            return "DEGRADED";
    }
    return "UNKNOWN";
}

RollingDetector::RollingDetector(DetectorConfig config) : config_(config) {
    if (config_.baseline_window == 0 || config_.minimum_baseline_samples == 0 ||
        config_.minimum_baseline_samples > config_.baseline_window ||
        config_.degrade_after == 0 || config_.recover_after == 0 ||
        config_.sigma_threshold <= 0.0 || config_.minimum_stddev <= 0.0) {
        throw std::invalid_argument("invalid detector configuration");
    }
}

Detection RollingDetector::observe(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument("observation must be finite and non-negative");
    }

    const HealthState previous_state = state_;
    auto summary = summarize(baseline_);
    double effective_stddev = std::max(summary.stddev, config_.minimum_stddev);
    double threshold = summary.mean + config_.sigma_threshold * effective_stddev;
    bool anomalous = baseline_.size() >= config_.minimum_baseline_samples &&
                     value > threshold;

    if (state_ == HealthState::learning) {
        add_to_baseline(value);
        if (baseline_.size() >= config_.minimum_baseline_samples) {
            state_ = HealthState::normal;
        }
    } else if (state_ == HealthState::normal) {
        if (anomalous) {
            ++anomaly_streak_;
            if (anomaly_streak_ >= config_.degrade_after) {
                state_ = HealthState::degraded;
                recovery_streak_ = 0;
            }
        } else {
            anomaly_streak_ = 0;
            add_to_baseline(value);
        }
    } else {
        if (anomalous) {
            recovery_streak_ = 0;
        } else {
            ++recovery_streak_;
            if (recovery_streak_ >= config_.recover_after) {
                state_ = HealthState::normal;
                anomaly_streak_ = 0;
                recovery_streak_ = 0;
                add_to_baseline(value);
            }
        }
    }

    summary = summarize(baseline_);
    effective_stddev = std::max(summary.stddev, config_.minimum_stddev);
    threshold = summary.mean + config_.sigma_threshold * effective_stddev;
    return {
        .state = state_,
        .previous_state = previous_state,
        .changed = state_ != previous_state,
        .anomalous = anomalous,
        .current = value,
        .baseline_mean = summary.mean,
        .baseline_stddev = summary.stddev,
        .threshold = threshold,
        .z_score = (value - summary.mean) / effective_stddev,
        .baseline_samples = baseline_.size(),
    };
}

HealthState RollingDetector::state() const noexcept {
    return state_;
}

void RollingDetector::add_to_baseline(double value) {
    baseline_.push_back(value);
    if (baseline_.size() > config_.baseline_window) {
        baseline_.pop_front();
    }
}

}  // namespace graywatch
