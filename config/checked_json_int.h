// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace nxframe {

// Check the JSON storage type and range before any narrowing conversion.
inline bool checkedJsonInt(const nlohmann::json& value, int minimum, int maximum, int& out)
{
    if (value.is_number_unsigned()) {
        const auto v = value.get<uint64_t>();
        if (maximum < 0 || v > static_cast<uint64_t>(maximum) ||
            (minimum > 0 && v < static_cast<uint64_t>(minimum))) return false;
    } else if (value.is_number_integer()) {
        const auto v = value.get<int64_t>();
        if (v < minimum || v > maximum) return false;
    } else {
        return false;
    }
    out = value.get<int>();
    return true;
}

inline int optionalJsonInt(const nlohmann::json& object, const char* key,
                           int fallback, int minimum, int maximum,
                           const std::string& path)
{
    if (!object.contains(key)) return fallback;
    int value;
    if (!checkedJsonInt(object[key], minimum, maximum, value)) {
        throw std::invalid_argument(path + "." + key + " must be an integer between " +
                                    std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return value;
}

} // namespace nxframe
