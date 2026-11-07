#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace graywatch {

std::optional<unsigned int> parse_passwd_uid(std::string_view content,
                                             std::string_view user);
std::optional<unsigned int> resolve_local_uid(
    std::string_view user,
    const std::filesystem::path& passwd_path = "/etc/passwd");

}  // namespace graywatch
