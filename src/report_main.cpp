#include "graywatch/report.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::cerr << "usage: graywatch-report ARCHIVE.tar.gz|DIRECTORY "
                 "[--format text|markdown|json] [--output PATH]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    auto format = graywatch::report::Format::text;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--format" && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (value == "text") format = graywatch::report::Format::text;
            else if (value == "markdown") format = graywatch::report::Format::markdown;
            else if (value == "json") format = graywatch::report::Format::json;
            else return usage();
        } else if (argument == "--output" && index + 1 < argc) {
            output = argv[++index];
        } else if (argument == "-h" || argument == "--help") {
            return usage();
        } else if (input.empty() && !argument.starts_with("-")) {
            input = argument;
        } else {
            return usage();
        }
    }
    if (input.empty()) return usage();
    try {
        const auto files = graywatch::report::load_bundle(input);
        if (!files.contains("events.jsonl") && !files.contains("system.txt")) {
            std::cerr << "graywatch-report: " << input
                      << " is not a Graywatch support archive\n";
            return 1;
        }
        const auto text = graywatch::report::render(graywatch::report::analyze(files), format);
        if (output.empty()) {
            std::cout << text;
        } else {
            std::ofstream file(output);
            file << text;
            if (!file) {
                std::cerr << "graywatch-report: cannot write " << output << '\n';
                return 1;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "graywatch-report: " << error.what() << '\n';
        return 1;
    }
}
