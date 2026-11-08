#include "graywatch/cluster.hpp"
#include "graywatch/json.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string device(const std::string& name, const std::string& condition, double severity,
                   double busy) {
    return R"({"condition":")" + condition + R"(","severity":)" + std::to_string(severity) +
           R"(,"device":{"name":")" + name + R"("},"evidence":{"busy_percent":)" +
           std::to_string(busy) + "}}";
}

std::string pick(const std::string& devices) {
    const auto io = graywatch::json::parse("[" + devices + "]");
    const auto* chosen = graywatch::notable_device(io);
    return chosen ? (*chosen)["device"].string("name") : "-";
}

}  // namespace

int main() {
    try {
        // Idle devices and near-idle healthy devices (root fs flushes) are not shown.
        require(pick(device("sda", "idle", 0, 0) + "," + device("dm-0", "healthy", 0, 0.02)) == "-",
                "near-idle device shown as busiest");
        // The busiest healthy device at or above 1% is shown.
        require(pick(device("dm-0", "healthy", 0, 0.5) + "," + device("nvme0n1", "healthy", 0.1, 40) +
                     "," + device("nvme1n1", "healthy", 0.1, 12)) == "nvme0n1",
                "busiest healthy device not chosen");
        // Any unhealthy device wins over a busier healthy one, most severe first.
        require(pick(device("nvme0n1", "healthy", 0.1, 99) + "," +
                     device("nvme1n1", "congested", 0.4, 30) + "," +
                     device("nvme2n1", "degraded", 0.85, 5)) == "nvme2n1",
                "most severe unhealthy device not chosen");
        require(pick("") == "-", "empty device list");
    } catch (const std::exception& error) {
        std::cerr << "cluster_test: " << error.what() << '\n';
        return 1;
    }
}
