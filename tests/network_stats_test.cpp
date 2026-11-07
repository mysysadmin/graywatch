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
        if (!value->used_since_boot)
            throw std::runtime_error("port with link_downed history not in use");
        graywatch::IbPortSample unused{.device="mlx5_0", .port=2, .state="DOWN",
                                       .physical_state="Polling", .rate="100 Gb/sec"};
        const auto idle = graywatch::calculate_ib_interval(unused, unused);
        if (!idle || idle->used_since_boot)
            throw std::runtime_error("never-used port reported as in use");
        auto carried = unused;
        carried.receive_data = 7;
        if (!graywatch::calculate_ib_interval(carried, carried)->used_since_boot)
            throw std::runtime_error("port with traffic history not in use");
        after.receive_errors = 1;
        if (graywatch::calculate_ib_interval(before, after))
            throw std::runtime_error("counter reset was accepted");

        const auto tcp = graywatch::parse_tcp_snmp(
            "Ip: Forwarding DefaultTTL\nIp: 2 64\n"
            "Tcp: RtoAlgorithm OutSegs RetransSegs InErrs OutRsts\n"
            "Tcp: 1 1000 20 3 4\n");
        if (tcp.out_segments != 1000 || tcp.retransmitted_segments != 20 ||
            tcp.input_errors != 3 || tcp.output_resets != 4)
            throw std::runtime_error("TCP SNMP parse is wrong");
        auto tcp_after = tcp;
        tcp_after.out_segments += 200;
        tcp_after.retransmitted_segments += 4;
        const auto tcp_interval = graywatch::calculate_tcp_interval(tcp, tcp_after);
        if (!tcp_interval || tcp_interval->retransmit_percent != 2.0)
            throw std::runtime_error("TCP interval is wrong");
    } catch (const std::exception& error) {
        std::cerr << "network_stats_test: " << error.what() << '\n';
        return 1;
    }
}
