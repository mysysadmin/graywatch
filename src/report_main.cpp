#include "graywatch/report.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int usage() {
    std::cerr << "usage: graywatch-report ARCHIVE.tar[.gz]|DIRECTORY "
                 "[--format text|markdown|json] [--output PATH]\n"
                 "       graywatch-report --cluster ARCHIVE|DIRECTORY... [--output PATH]\n";
    return 2;
}

}  // namespace

bool write(const std::string& output, const std::string& text) {
    if (output.empty()) {
        std::cout << text;
        return true;
    }
    std::ofstream file(output);
    file << text;
    if (!file) {
        std::cerr << "graywatch-report: cannot write " << output << '\n';
        return false;
    }
    return true;
}

// Archives from several hosts of one collection window: what they share.
int cluster(const std::vector<std::string>& inputs, const std::string& output) {
    std::vector<graywatch::report::Report> reports;
    for (const auto& input : inputs) {
        try {
            reports.push_back(graywatch::report::analyze(graywatch::report::load_bundle(input)));
        } catch (const std::exception& error) {
            std::cerr << "graywatch-report: " << input << ": " << error.what() << '\n';
            return 1;
        }
    }
    return write(output, graywatch::report::render_cluster(reports)) ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    bool cluster_mode = false;
    std::vector<std::string> inputs;
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
        } else if (argument == "--cluster") {
            cluster_mode = true;
        } else if (cluster_mode && !argument.starts_with("-")) {
            inputs.emplace_back(argument);
        } else if (argument == "-h" || argument == "--help") {
            return usage();
        } else if (input.empty() && !argument.starts_with("-")) {
            input = argument;
        } else {
            return usage();
        }
    }
    if (cluster_mode) {
        if (!input.empty()) inputs.insert(inputs.begin(), input);
        if (inputs.size() < 2) return usage();
        return cluster(inputs, output);
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
        return write(output, text) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "graywatch-report: " << error.what() << '\n';
        return 1;
    }
}
