#include "graywatch/network_stats.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        const graywatch::IbPortSample before{
            .device="mlx5_0", .port=1, .state="ACTIVE", .physical_state="LinkUp",
            .rate="100 Gb/sec", .receive_errors=2, .transmit_discards=3,
            .symbol_errors=4, .link_downed=5, .transmit_wait=10};
        auto after = before;
        after.transmit_discards = 5;
        after.transmit_wait = 17;
        const auto value = graywatch::calculate_ib_interval(before, after);
        if (!value || value->transmit_discards != 2 || value->transmit_wait != 7)
            throw std::runtime_error("counter delta is wrong");
        after.receive_errors = 1;
        if (graywatch::calculate_ib_interval(before, after))
            throw std::runtime_error("counter reset was accepted");
    } catch (const std::exception& error) {
        std::cerr << "network_stats_test: " << error.what() << '\n';
        return 1;
    }
}
