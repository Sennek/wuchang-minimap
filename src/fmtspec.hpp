#pragma once

//
// fmtspec - printf conversion specifiers, read at compile time. PURE, so
// tests/markers_test.cpp links it.
//
// A format string that moves out of a literal and into a table loses the compiler's own
// format check, and a translated one can carry a different specifier than the English it
// replaces. Both end in a crash inside Present. This is the check they lose, applied to
// the table instead: `matches<Args...>(fmt)` compares the specifiers with the argument
// types at the call site, and `same(a, b)` compares two strings' specifier sequences.
//

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace fspec
{
    // What a specifier (or its `*` width / precision) consumes.
    enum class Arg : unsigned char
    {
        Int,    // int-sized integer: d i u o x X c, and l on this platform
        Int64,  // ll, I64, z, j, t
        Double, // e E f F g G a A
        Str,    // s
        WStr,   // ls, S
        Ptr,    // p
        Bad,    // not a specifier this project may use (n, L, a cut-off string)
    };

    // One `%...` of a format string: its extent, and whether it consumes an argument.
    struct Spec
    {
        std::size_t begin = 0;
        std::size_t end = 0; // one past the conversion character
        int stars = 0;       // `*` width / precision, one int argument each
        Arg arg = Arg::Bad;
        bool literal = false; // `%%`
    };

    // The next specifier at or after `from`, or false when there is none.
    constexpr bool next(std::string_view f, std::size_t from, Spec& out)
    {
        std::size_t i = f.find('%', from);
        if (i == std::string_view::npos)
        {
            return false;
        }
        Spec s{};
        s.begin = i++;
        if (i < f.size() && f[i] == '%')
        {
            s.end = i + 1;
            s.literal = true;
            out = s;
            return true;
        }
        while (i < f.size() && (f[i] == '-' || f[i] == '+' || f[i] == ' ' || f[i] == '#' || f[i] == '0'))
        {
            ++i;
        }
        const auto width = [&] {
            if (i < f.size() && f[i] == '*')
            {
                ++s.stars;
                ++i;
                return;
            }
            while (i < f.size() && f[i] >= '0' && f[i] <= '9')
            {
                ++i;
            }
        };
        width();
        if (i < f.size() && f[i] == '.')
        {
            ++i;
            width();
        }
        // Length: hh h l ll z j t I I32 I64 L.
        int longs = 0;
        bool wide64 = false;
        bool bad_len = false;
        while (i < f.size())
        {
            const char c = f[i];
            if (c == 'h')
            {
                ++i;
            }
            else if (c == 'l')
            {
                ++longs;
                ++i;
            }
            else if (c == 'z' || c == 'j' || c == 't')
            {
                wide64 = true;
                ++i;
            }
            else if (c == 'I')
            {
                ++i;
                if (f.substr(i, 2) == "64")
                {
                    i += 2;
                }
                else if (f.substr(i, 2) == "32")
                {
                    i += 2;
                    longs = 0;
                    continue;
                }
                wide64 = true;
            }
            else if (c == 'L')
            {
                bad_len = true; // long double: nothing here passes one
                ++i;
            }
            else
            {
                break;
            }
        }
        if (i >= f.size())
        {
            s.end = f.size();
            s.arg = Arg::Bad;
            out = s;
            return true;
        }
        const char conv = f[i];
        s.end = i + 1;
        switch (conv)
        {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'c':
            s.arg = (longs >= 2 || wide64) ? Arg::Int64 : Arg::Int;
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            s.arg = Arg::Double;
            break;
        case 's':
            s.arg = longs == 1 ? Arg::WStr : Arg::Str;
            break;
        case 'S':
            s.arg = Arg::WStr;
            break;
        case 'p':
            s.arg = Arg::Ptr;
            break;
        default:
            s.arg = Arg::Bad;
            break;
        }
        if (bad_len)
        {
            s.arg = Arg::Bad;
        }
        out = s;
        return true;
    }

    // True when `a` and `b` carry the same specifiers, character for character, in the
    // same order. A `%%` consumes nothing and may sit anywhere; a lone `%` is a specifier
    // of its own (a bad one), so a stray percent in a translation is caught too.
    constexpr bool same(std::string_view a, std::string_view b)
    {
        // The next specifier that consumes an argument, skipping `%%`.
        const auto next_arg = [](std::string_view f, std::size_t& at, Spec& s) {
            while (next(f, at, s))
            {
                at = s.end;
                if (!s.literal)
                {
                    return true;
                }
            }
            return false;
        };
        Spec sa{};
        Spec sb{};
        std::size_t ia = 0;
        std::size_t ib = 0;
        for (;;)
        {
            const bool ha = next_arg(a, ia, sa);
            const bool hb = next_arg(b, ib, sb);
            if (ha != hb)
            {
                return false;
            }
            if (!ha)
            {
                return true;
            }
            if (a.substr(sa.begin, sa.end - sa.begin) != b.substr(sb.begin, sb.end - sb.begin))
            {
                return false;
            }
        }
    }

    // The argument kind a C++ type is passed as through `...`.
    template <typename T>
    constexpr Arg kind_of()
    {
        using U = std::remove_cvref_t<std::decay_t<T>>;
        if constexpr (std::is_same_v<U, bool>)
        {
            return Arg::Bad;
        }
        else if constexpr (std::is_integral_v<U>)
        {
            return sizeof(U) <= 4 ? Arg::Int : Arg::Int64;
        }
        else if constexpr (std::is_floating_point_v<U>)
        {
            return std::is_same_v<U, long double> ? Arg::Bad : Arg::Double;
        }
        else if constexpr (std::is_pointer_v<U>)
        {
            using P = std::remove_cv_t<std::remove_pointer_t<U>>;
            if constexpr (std::is_same_v<P, char>)
            {
                return Arg::Str;
            }
            else if constexpr (std::is_same_v<P, wchar_t>)
            {
                return Arg::WStr;
            }
            else
            {
                return Arg::Ptr;
            }
        }
        else
        {
            return Arg::Bad; // a class, an enum, nullptr: nothing printf can take
        }
    }

    // True when `f`'s specifiers consume exactly `kinds[0..n)`, in order. A string
    // (`Str` / `WStr`) is also a valid `%p`.
    constexpr bool consumes(std::string_view f, const Arg* kinds, std::size_t n)
    {
        std::size_t k = 0;
        Spec s{};
        std::size_t at = 0;
        while (next(f, at, s))
        {
            at = s.end;
            if (s.literal)
            {
                continue;
            }
            if (s.arg == Arg::Bad)
            {
                return false;
            }
            for (int st = 0; st < s.stars; ++st)
            {
                if (k >= n || kinds[k] != Arg::Int)
                {
                    return false;
                }
                ++k;
            }
            if (k >= n)
            {
                return false;
            }
            const Arg a = kinds[k++];
            const bool ok = a == s.arg || (s.arg == Arg::Ptr && (a == Arg::Str || a == Arg::WStr));
            if (!ok)
            {
                return false;
            }
        }
        return k == n;
    }

    template <typename... A>
    constexpr bool matches(std::string_view f)
    {
        constexpr Arg kinds[] = {kind_of<A>()..., Arg::Bad};
        return consumes(f, kinds, sizeof...(A));
    }
} // namespace fspec
