#include "graywatch/psi.hpp"

#include <iomanip>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "psi") {
        std::cerr << "usage: graywatchctl psi\n";
        return 2;
    }

    try {
        std::cout << std::fixed << std::setprecision(2);
        for (const auto& sample : graywatch::read_system_psi()) {
            std::cout << sample.resource << ' ' << sample.scope
                      << " avg10=" << sample.avg10
                      << " avg60=" << sample.avg60
                      << " avg300=" << sample.avg300
                      << " total_us=" << sample.total_us << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "graywatchctl: " << error.what() << '\n';
        return 1;
    }
}
