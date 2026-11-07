#include "graywatch/psi.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
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
    std::cerr << "usage: graywatchctl psi [--watch] [--interval SECONDS] "
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

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--watch") {
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) != "psi") {
        usage();
        return 2;
    }

    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::fixed << std::setprecision(2);
        if (options.watch) {
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
