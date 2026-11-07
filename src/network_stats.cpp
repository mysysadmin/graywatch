#include "graywatch/network_stats.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace graywatch {
namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}

std::uint64_t read_counter(const std::filesystem::path& path) {
    const auto text = read_text(path);
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(),
                                              value);
    return error == std::errc{} && end == text.data() + text.size() ? value : 0;
}

std::string state_name(std::string value) {
    const auto separator = value.find(':');
    if (separator != std::string::npos) value.erase(0, separator + 1);
    const auto first = value.find_first_not_of(" \t");
    return first == std::string::npos ? "" : value.substr(first);
}

}  // namespace

TcpSample parse_tcp_snmp(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string header_line;
    while (std::getline(input, header_line)) {
        if (!header_line.starts_with("Tcp:")) continue;
        std::string value_line;
        if (!std::getline(input, value_line) || !value_line.starts_with("Tcp:")) break;
        std::istringstream headers(header_line);
        std::istringstream values(value_line);
        std::string prefix;
        headers >> prefix;
        values >> prefix;
        std::map<std::string, std::uint64_t> fields;
        std::string name;
        std::uint64_t value{};
        while (headers >> name && values >> value) fields[name] = value;
        return {
            .out_segments = fields["OutSegs"],
            .retransmitted_segments = fields["RetransSegs"],
            .input_errors = fields["InErrs"],
            .output_resets = fields["OutRsts"],
        };
    }
    return {};
}

TcpSample read_tcp_sample(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return parse_tcp_snmp(text.str());
}

std::optional<TcpInterval> calculate_tcp_interval(const TcpSample& previous,
                                                  const TcpSample& current) {
    if (current.out_segments < previous.out_segments ||
        current.retransmitted_segments < previous.retransmitted_segments ||
        current.input_errors < previous.input_errors ||
        current.output_resets < previous.output_resets) return std::nullopt;
    const auto out = current.out_segments - previous.out_segments;
    const auto retrans = current.retransmitted_segments -
                         previous.retransmitted_segments;
    return TcpInterval{
        .out_segments = out,
        .retransmitted_segments = retrans,
        .input_errors = current.input_errors - previous.input_errors,
        .output_resets = current.output_resets - previous.output_resets,
        .retransmit_percent = out == 0 ? 0.0 :
            static_cast<double>(retrans) / static_cast<double>(out) * 100.0,
    };
}

std::optional<IbPortInterval> calculate_ib_interval(
    const IbPortSample& previous, const IbPortSample& current) {
    if (previous.device != current.device || previous.port != current.port ||
        current.receive_errors < previous.receive_errors ||
        current.transmit_discards < previous.transmit_discards ||
        current.symbol_errors < previous.symbol_errors ||
        current.link_downed < previous.link_downed ||
        current.transmit_wait < previous.transmit_wait) return std::nullopt;
    return IbPortInterval{
        .device = current.device, .port = current.port, .state = current.state,
        .physical_state = current.physical_state, .rate = current.rate,
        .receive_errors = current.receive_errors - previous.receive_errors,
        .transmit_discards = current.transmit_discards - previous.transmit_discards,
        .symbol_errors = current.symbol_errors - previous.symbol_errors,
        .link_downed = current.link_downed - previous.link_downed,
        .transmit_wait = current.transmit_wait - previous.transmit_wait,
        .used_since_boot = current.receive_data != 0 || current.transmit_data != 0 ||
                           current.link_downed != 0,
    };
}

std::vector<IbPortSample> read_ib_ports(const std::filesystem::path& root) {
    std::vector<IbPortSample> result;
    std::error_code error;
    std::filesystem::directory_iterator devices(root, error);
    if (error) return result;
    for (const auto& device : devices) {
        std::filesystem::directory_iterator ports(device.path() / "ports", error);
        if (error) { error.clear(); continue; }
        for (const auto& port_path : ports) {
            unsigned int port{};
            const auto name = port_path.path().filename().string();
            const auto [end, parse_error] = std::from_chars(
                name.data(), name.data() + name.size(), port);
            if (parse_error != std::errc{} || end != name.data() + name.size()) continue;
            const auto counters = port_path.path() / "counters";
            result.push_back({
                .device = device.path().filename().string(), .port = port,
                .state = state_name(read_text(port_path.path() / "state")),
                .physical_state = state_name(read_text(port_path.path() / "phys_state")),
                .rate = read_text(port_path.path() / "rate"),
                .receive_errors = read_counter(counters / "port_rcv_errors"),
                .transmit_discards = read_counter(counters / "port_xmit_discards"),
                .symbol_errors = read_counter(counters / "symbol_error"),
                .link_downed = read_counter(counters / "link_downed"),
                .transmit_wait = read_counter(counters / "port_xmit_wait"),
                .receive_data = read_counter(counters / "port_rcv_data"),
                .transmit_data = read_counter(counters / "port_xmit_data"),
            });
        }
    }
    return result;
}

std::vector<IbPortInterval> calculate_ib_intervals(
    const std::vector<IbPortSample>& previous,
    const std::vector<IbPortSample>& current) {
    using Key = std::pair<std::string, unsigned int>;
    std::map<Key, const IbPortSample*> prior;
    for (const auto& sample : previous) prior[{sample.device, sample.port}] = &sample;
    std::vector<IbPortInterval> result;
    for (const auto& sample : current) {
        const auto found = prior.find({sample.device, sample.port});
        if (found == prior.end()) continue;
        auto interval = calculate_ib_interval(*found->second, sample);
        if (interval) result.push_back(std::move(*interval));
    }
    return result;
}

}  // namespace graywatch
