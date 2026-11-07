#pragma once

#include "graywatch/block_stats.hpp"

#include <string_view>

namespace graywatch {

enum class IoCondition { idle, healthy, congested, saturated, degraded };

struct IoAssessment {
    IoCondition condition{IoCondition::idle};
    double severity{};
    double maximum_average_latency_ms{};
    std::string_view reason{"no_io"};
};

IoAssessment assess_io(const BlockDeviceInterval& device);
std::string_view to_string(IoCondition condition);

}  // namespace graywatch
