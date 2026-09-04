#pragma once

//
// textmatch - case-insensitive ASCII substring search, the full map's marker name
// filter. Header-only and pure, so tests/markers_test.cpp links it.
//

#include <cstddef>
#include <string_view>

namespace txt
{
    inline char lower_ascii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    // True for an empty needle: an empty search box filters nothing out.
    inline bool contains_ci(std::string_view hay, std::string_view needle)
    {
        if (needle.empty())
        {
            return true;
        }
        if (needle.size() > hay.size())
        {
            return false;
        }
        const std::size_t last = hay.size() - needle.size();
        for (std::size_t i = 0; i <= last; ++i)
        {
            std::size_t j = 0;
            while (j < needle.size() && lower_ascii(hay[i + j]) == lower_ascii(needle[j]))
            {
                ++j;
            }
            if (j == needle.size())
            {
                return true;
            }
        }
        return false;
    }
} // namespace txt
