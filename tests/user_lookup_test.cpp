#include "graywatch/user_lookup.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        constexpr auto passwd =
            "root:x:0:0:root:/root:/bin/sh\n"
            "postgres:x:26:26:PostgreSQL:/var/lib/postgres:/bin/sh\n";
        if (graywatch::parse_passwd_uid(passwd, "postgres") != 26)
            throw std::runtime_error("named user was not resolved");
        if (graywatch::parse_passwd_uid(passwd, "1001") != 1001)
            throw std::runtime_error("numeric UID was not resolved");
        if (graywatch::parse_passwd_uid(passwd, "missing"))
            throw std::runtime_error("missing user was resolved");
    } catch (const std::exception& error) {
        std::cerr << "user_lookup_test: " << error.what() << '\n';
        return 1;
    }
}
