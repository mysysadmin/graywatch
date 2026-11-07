#include "graywatch/detector.hpp"
#include "graywatch/psi.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

struct Options {
    bool watch{};
    double interval_seconds{1.0};
    unsigned int count{};
};

void usage() {
    std::cerr << "usage:\n"
                 "  graywatchctl psi [--watch] [--interval SECONDS] "
                 "[--count SAMPLES]\n"
                 "  graywatchctl health [--interval SECONDS] "
                 "[--count SAMPLES]\n";
}

double parse_positive_double(const char* text, std::string_view option) {
    std::size_t consumed{};
    const double value = std::stod(text, &consumed);
    if (text[consumed] != '\0' || value <= 0.0) {
        throw std::invalid_argument(std::string(option) + " must be positive");
    }
    return value;
}

unsigned int parse_positive_unsigned(const char* text, std::string_view option) {
    std::size_t consumed{};
    const auto value = std::stoul(text, &consumed);
    if (text[consumed] != '\0' || value == 0) {
        throw std::invalid_argument(std::string(option) + " must be positive");
    }
    return static_cast<unsigned int>(value);
}

Options parse_options(int argc, char** argv, bool health_command) {
    Options options;
    options.watch = health_command;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--watch" && !health_command) {
            options.watch = true;
        } else if (argument == "--interval" && index + 1 < argc) {
            options.interval_seconds =
                parse_positive_double(argv[++index], "--interval");
        } else if (argument == "--count" && index + 1 < argc) {
            options.count = parse_positive_unsigned(argv[++index], "--count");
        } else {
            throw std::invalid_argument("unknown or incomplete option: " +
                                        std::string(argument));
        }
    }
    if (options.count != 0 && !options.watch) {
        throw std::invalid_argument("--count requires --watch");
    }
    return options;
}

void print_snapshot(const std::vector<graywatch::PsiSample>& samples) {
    for (const auto& sample : samples) {
        std::cout << sample.resource << ' ' << sample.scope
                  << " avg10=" << sample.avg10
                  << " avg60=" << sample.avg60
                  << " avg300=" << sample.avg300
                  << " total_us=" << sample.total_us << '\n';
    }
}

void watch(const Options& options) {
    using Clock = std::chrono::steady_clock;
    auto previous = graywatch::read_system_psi();
    auto previous_time = Clock::now();
    unsigned int emitted{};

    const auto sleep_duration = std::chrono::duration<double>(options.interval_seconds);
    while (options.count == 0 || emitted < options.count) {
        std::this_thread::sleep_for(sleep_duration);
        const auto current_time = Clock::now();
        const auto current = graywatch::read_system_psi();
        const double elapsed =
            std::chrono::duration<double>(current_time - previous_time).count();

        std::cout << "interval_seconds=" << elapsed << '\n';
        for (const auto& interval :
             graywatch::calculate_psi_intervals(previous, current, elapsed)) {
            std::cout << interval.resource << ' ' << interval.scope
                      << " stall_percent=" << interval.stall_percent
                      << " stalled_us=" << interval.stalled_us << '\n';
        }
        std::cout << std::flush;
        previous = current;
        previous_time = current_time;
        ++emitted;
    }
}

void health(const Options& options) {
    using Clock = std::chrono::steady_clock;
    using Key = std::pair<std::string, std::string>;
    std::map<Key, graywatch::RollingDetector> detectors;
    auto previous = graywatch::read_system_psi();
    auto previous_time = Clock::now();
    unsigned int emitted{};

    const auto sleep_duration = std::chrono::duration<double>(options.interval_seconds);
    while (options.count == 0 || emitted < options.count) {
        std::this_thread::sleep_for(sleep_duration);
        const auto current_time = Clock::now();
        const auto current = graywatch::read_system_psi();
        const double elapsed =
            std::chrono::duration<double>(current_time - previous_time).count();

        for (const auto& interval :
             graywatch::calculate_psi_intervals(previous, current, elapsed)) {
            auto [position, inserted] = detectors.try_emplace(
                Key{interval.resource, interval.scope});
            (void)inserted;
            const auto detection = position->second.observe(interval.stall_percent);
            std::cout << interval.resource << ' ' << interval.scope
                      << " state=" << graywatch::to_string(detection.state)
                      << " current=" << detection.current
                      << " baseline_mean=" << detection.baseline_mean
                      << " baseline_stddev=" << detection.baseline_stddev
                      << " threshold=" << detection.threshold
                      << " samples=" << detection.baseline_samples;
            if (detection.anomalous) {
                std::cout << " anomalous=yes z_score=" << detection.z_score;
            }
            if (detection.changed) {
                std::cout << " transition="
                          << graywatch::to_string(detection.previous_state) << "->"
                          << graywatch::to_string(detection.state);
            }
            std::cout << '\n';
        }
        std::cout << "---\n" << std::flush;
        previous = current;
        previous_time = current_time;
        ++emitted;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    try {
        const std::string_view command(argv[1]);
        if (command != "psi" && command != "health") {
            throw std::invalid_argument("unknown command: " + std::string(command));
        }
        const bool health_command = command == "health";
        const auto options = parse_options(argc, argv, health_command);
        std::cout << std::fixed << std::setprecision(2);
        if (health_command) {
            health(options);
        } else if (options.watch) {
            watch(options);
        } else {
            print_snapshot(graywatch::read_system_psi());
        }
    } catch (const std::exception& error) {
        std::cerr << "graywatchctl: " << error.what() << '\n';
        usage();
        return 1;
    }
}
