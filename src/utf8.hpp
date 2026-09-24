#pragma once

//
// utf8 - byte buffers that hold UTF-8 and never hold half a character. PURE: no Windows,
// so tests/markers_test.cpp links it.
//
// Every fixed `char[N]` the overlay draws from is filled through here: a cut lands on a
// codepoint boundary, so a truncated label ends one character early instead of in a
// replacement glyph, and the text handed to ImGui is always valid UTF-8. The UTF-16 side
// is what the mod's wide log and the file API take; wchar_t is 16 bits on the one
// platform this builds for.
//
// Allocation-free except `to_wide`. Any thread.
//

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace utf8
{
    constexpr bool is_cont(char c)
    {
        return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
    }

    // The byte length a lead byte announces: 1 for ASCII, 2..4 for a lead, 0 for a
    // continuation byte or a byte no UTF-8 sequence starts with.
    constexpr std::size_t seq_len(char lead)
    {
        const unsigned char b = static_cast<unsigned char>(lead);
        if (b < 0x80u)
        {
            return 1;
        }
        if ((b & 0xE0u) == 0xC0u)
        {
            return 2;
        }
        if ((b & 0xF0u) == 0xE0u)
        {
            return 3;
        }
        if ((b & 0xF8u) == 0xF0u)
        {
            return 4;
        }
        return 0;
    }

    // The longest prefix of `s` of at most `max_bytes` that ends on a codepoint boundary.
    constexpr std::size_t fit(std::string_view s, std::size_t max_bytes)
    {
        if (s.size() <= max_bytes)
        {
            return s.size();
        }
        std::size_t n = max_bytes;
        while (n > 0 && is_cont(s[n]))
        {
            --n;
        }
        return n;
    }

    // Drops a sequence cut short at the end of `s` (length `len`), in place. Returns the
    // new length. What snprintf leaves when it truncates is exactly this shape.
    inline std::size_t trim_partial(char* s, std::size_t len)
    {
        std::size_t back = 0;
        while (back < 4 && back < len && is_cont(s[len - 1 - back]))
        {
            ++back;
        }
        if (back == len)
        {
            return len; // nothing but continuation bytes: not ours to judge
        }
        const std::size_t lead = len - 1 - back;
        const std::size_t want = seq_len(s[lead]);
        if (want > back + 1)
        {
            s[lead] = '\0';
            return lead;
        }
        return len;
    }

    // `src` into `dst[cap]`, NUL-terminated, cut on a codepoint boundary.
    inline void copy(char* dst, std::size_t cap, std::string_view src)
    {
        if (dst == nullptr || cap == 0)
        {
            return;
        }
        const std::size_t n = fit(src, cap - 1);
        if (n != 0)
        {
            std::memcpy(dst, src.data(), n);
        }
        dst[n] = '\0';
    }

    // vsnprintf that never leaves half a character behind when it truncates. Returns the
    // bytes written; a format error leaves `dst` empty.
    inline std::size_t vformat(char* dst, std::size_t cap, const char* fmt, std::va_list args)
    {
        if (dst == nullptr || cap == 0)
        {
            return 0;
        }
        const int want = std::vsnprintf(dst, cap, fmt != nullptr ? fmt : "", args);
        if (want < 0)
        {
            dst[0] = '\0';
            return 0;
        }
        if (static_cast<std::size_t>(want) < cap)
        {
            return static_cast<std::size_t>(want);
        }
        return trim_partial(dst, cap - 1);
    }

    inline std::size_t format(char* dst, std::size_t cap, const char* fmt, ...)
    {
        std::va_list args;
        va_start(args, fmt);
        const std::size_t n = vformat(dst, cap, fmt, args);
        va_end(args);
        return n;
    }

    // One codepoint off the front of `s` at `i`, advancing `i`. A malformed or cut
    // sequence reads as U+FFFD and consumes one byte, so a bad byte never eats a good one.
    constexpr char32_t next(std::string_view s, std::size_t& i)
    {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        const std::size_t n = seq_len(s[i]);
        if (n == 1)
        {
            ++i;
            return b;
        }
        if (n == 0 || i + n > s.size())
        {
            ++i;
            return 0xFFFD;
        }
        char32_t cp = b & (n == 2 ? 0x1Fu : n == 3 ? 0x0Fu : 0x07u);
        for (std::size_t k = 1; k < n; ++k)
        {
            if (!is_cont(s[i + k]))
            {
                ++i;
                return 0xFFFD;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        // Overlong forms and surrogates are not characters.
        constexpr char32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            ++i;
            return 0xFFFD;
        }
        i += n;
        return cp;
    }

    // Well-formed UTF-8 throughout: every sequence decodes. A malformed one is the only way
    // next() consumes a single byte and returns U+FFFD; a real U+FFFD takes three.
    constexpr bool valid(std::string_view s)
    {
        std::size_t i = 0;
        while (i < s.size())
        {
            const std::size_t at = i;
            if (next(s, i) == 0xFFFD && i - at == 1)
            {
                return false;
            }
        }
        return true;
    }

    // UTF-8 -> UTF-16, for the wide log and the wide file API.
    inline std::wstring to_wide(std::string_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        std::size_t i = 0;
        while (i < s.size())
        {
            const char32_t cp = next(s, i);
            if (cp >= 0x10000)
            {
                const char32_t v = cp - 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
            }
            else
            {
                out.push_back(static_cast<wchar_t>(cp));
            }
        }
        return out;
    }
} // namespace utf8
