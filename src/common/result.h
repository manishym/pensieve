#pragma once

#include <cerrno>
#include <expected>
#include <string>
#include <system_error>

namespace pensieve {

using Error = std::error_code;

template <typename T>
using Result = std::expected<T, Error>;

inline Error last_os_error() {
    return {errno, std::system_category()};
}

inline Error make_error(int code) {
    return {code, std::system_category()};
}

}  // namespace pensieve
