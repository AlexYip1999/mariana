#pragma once

#include <cstddef>
#include <string_view>

namespace agent::core
{

/// Return a byte prefix without splitting a code point. Input is expected to be valid UTF-8.
inline std::size_t Utf8PrefixSize(std::string_view text, std::size_t max_bytes)
{
    if (text.size() <= max_bytes)
    {
        return text.size();
    }
    while (max_bytes > 0 && (static_cast<unsigned char>(text[max_bytes]) & 0xC0) == 0x80)
    {
        --max_bytes;
    }
    return max_bytes;
}

} // namespace agent::core
