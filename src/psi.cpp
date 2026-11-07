#include "graywatch/psi.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace graywatch {
namespace {

std::string_view value_after_equals(std::string_view token) {
    const auto position = token.find('=');
    if (position == std::string_view::npos || position + 1 == token.size()) {
        throw std::runtime_error("invalid PSI field: " + std::string(token));
    }
    return token.substr(position + 1);
}

}  // namespace

PsiSample parse_psi_line(std::string_view resource, std::string_view line) {
    std::istringstream input{std::string(line)};
    PsiSample sample;
    sample.resource = resource;

    std::string avg10;
    std::string avg60;
    std::string avg300;
    std::string total;
    if (!(input >> sample.scope >> avg10 >> avg60 >> avg300 >> total)) {
        throw std::runtime_error("incomplete PSI line: " + std::string(line));
    }

    sample.avg10 = std::stod(std::string(value_after_equals(avg10)));
    sample.avg60 = std::stod(std::string(value_after_equals(avg60)));
    sample.avg300 = std::stod(std::string(value_after_equals(avg300)));
    sample.total_us = std::stoull(std::string(value_after_equals(total)));
    return sample;
}

std::vector<PsiSample> read_psi_file(std::string_view resource,
                                     const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }

    std::vector<PsiSample> samples;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            samples.push_back(parse_psi_line(resource, line));
        }
    }
    return samples;
}

std::vector<PsiSample> read_system_psi(const std::filesystem::path& pressure_root) {
    std::vector<PsiSample> result;
    for (const auto* resource : {"cpu", "memory", "io"}) {
        auto samples = read_psi_file(resource, pressure_root / resource);
        result.insert(result.end(), samples.begin(), samples.end());
    }
    return result;
}

}  // namespace graywatch
