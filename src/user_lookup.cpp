#include "graywatch/user_lookup.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace graywatch {

std::optional<unsigned int> parse_passwd_uid(std::string_view content,
                                             std::string_view user) {
    unsigned int numeric{};
    const auto [numeric_end, numeric_error] = std::from_chars(
        user.data(), user.data() + user.size(), numeric);
    if (!user.empty() && numeric_error == std::errc{} &&
        numeric_end == user.data() + user.size()) return numeric;

    std::istringstream input{std::string(content)};
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find(':');
        if (first == std::string::npos ||
            std::string_view(line).substr(0, first) != user) continue;
        const auto second = line.find(':', first + 1);
        const auto third = second == std::string::npos ? std::string::npos :
                           line.find(':', second + 1);
        if (second == std::string::npos || third == std::string::npos) return {};
        const std::string_view uid(line.data() + second + 1,
                                   third - second - 1);
        unsigned int value{};
        const auto [end, error] = std::from_chars(
            uid.data(), uid.data() + uid.size(), value);
        return error == std::errc{} && end == uid.data() + uid.size()
            ? std::optional<unsigned int>(value) : std::nullopt;
    }
    return {};
}

std::optional<unsigned int> resolve_local_uid(
    std::string_view user, const std::filesystem::path& passwd_path) {
    std::ifstream input(passwd_path);
    std::ostringstream content;
    content << input.rdbuf();
    return parse_passwd_uid(content.str(), user);
}

}  // namespace graywatch
