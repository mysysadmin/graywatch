#pragma once

#include <cstddef>
#include <deque>
#include <string_view>

namespace graywatch {

enum class HealthState {
    learning,
    normal,
    degraded,
};

std::string_view to_string(HealthState state);

struct DetectorConfig {
    std::size_t baseline_window{60};
    std::size_t minimum_baseline_samples{10};
    std::size_t degrade_after{3};
    std::size_t recover_after{3};
    double sigma_threshold{3.0};
    double minimum_stddev{0.01};
    // Absolute floor, in the same unit as the observations: a value is only
    // anomalous when it also exceeds this, so a quiet baseline (all zeros) does
    // not turn trivial activity into an anomaly. 0 disables the floor.
    double minimum_anomaly{0.0};
};

// The outcome of one observation. baseline_mean, baseline_stddev, threshold,
// z_score and baseline_samples describe the baseline the value was compared
// against, i.e. before the value itself was (possibly) added to it.
struct Detection {
    HealthState state{HealthState::learning};
    HealthState previous_state{HealthState::learning};
    bool changed{};
    bool anomalous{};
    double current{};
    double baseline_mean{};
    double baseline_stddev{};
    double threshold{};      // max(mean + sigma * max(stddev, minimum_stddev), minimum_anomaly)
    bool floor_applied{};    // minimum_anomaly, not the baseline, set the threshold
    double z_score{};
    std::size_t baseline_samples{};  // size of the baseline the sample was compared with
    std::size_t anomaly_streak{};    // consecutive anomalies while NORMAL
    std::size_t recovery_streak{};   // consecutive normal samples while DEGRADED
};

class RollingDetector {
  public:
    explicit RollingDetector(DetectorConfig config = {});
    Detection observe(double value);
    [[nodiscard]] HealthState state() const noexcept;

  private:
    void add_to_baseline(double value);

    DetectorConfig config_;
    std::deque<double> baseline_;
    HealthState state_{HealthState::learning};
    std::size_t anomaly_streak_{};
    std::size_t recovery_streak_{};
};

}  // namespace graywatch
