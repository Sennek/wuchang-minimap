#pragma once

//
// lang_strings - every word the overlay draws, one table per culture. PURE: no Windows,
// no UE4SS, no D3D12, so tests/markers_test.cpp links it and proves the tables.
//
// `lang/en.inc` is the English table and DEFINES the ids: each `LANG_S(Id, "text")` line
// is one enumerator of `S` and one English string, in that order. Every other culture's
// file, `lang/<code>.inc`, is a sparse list of `LANG_T(Id, "text")` in any order; an id it
// does not list falls through the culture's chain (lang.hpp) and lands on English, so a
// table may be empty and a missing string is never blank.
//
// Compiled in, not data: a translated format whose `%` specifiers differ from the
// English is a crash inside Present, and a table in the binary is one markers_test can
// read whole (every id valid, no duplicates, the same specifiers in the same order).
// `fmt<Id>(args...)` checks the call site's arguments against the English specifiers at
// compile time, so the two ends are held together by the build.
//
// `tr` is an array index per link of the chain, three at most: cheap enough per frame,
// no allocation. The returned text is static and lives for the process.
//

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "fmtspec.hpp"
#include "lang.hpp"
#include "utf8.hpp"

namespace lang
{
    enum class S : std::uint16_t
    {
#define LANG_S(id, text) id,
#include "lang/en.inc"
#undef LANG_S
        Count,
    };
    constexpr std::size_t kStrCount = static_cast<std::size_t>(S::Count);

    using Table = std::array<const char*, kStrCount>;

    // One line of a translation file.
    struct Entry
    {
        S id = S::Count;
        const char* text = nullptr;
    };

    namespace detail
    {
        inline constexpr Table kEn = {
#define LANG_S(id, text) text,
#include "lang/en.inc"
#undef LANG_S
        };

        // Each list opens with a sentinel so an empty file is still a valid array; the
        // sentinel's id is Count, which `dense` and `listed` skip.
#define LANG_T(id, text) {S::id, text},
        inline constexpr Entry kDe[] = {
            {S::Count, nullptr},
#include "lang/de.inc"
        };
        inline constexpr Entry kEs[] = {
            {S::Count, nullptr},
#include "lang/es.inc"
        };
        inline constexpr Entry kFr[] = {
            {S::Count, nullptr},
#include "lang/fr.inc"
        };
        inline constexpr Entry kIt[] = {
            {S::Count, nullptr},
#include "lang/it.inc"
        };
        inline constexpr Entry kJa[] = {
            {S::Count, nullptr},
#include "lang/ja.inc"
        };
        inline constexpr Entry kKo[] = {
            {S::Count, nullptr},
#include "lang/ko.inc"
        };
        inline constexpr Entry kPt[] = {
            {S::Count, nullptr},
#include "lang/pt.inc"
        };
        inline constexpr Entry kRu[] = {
            {S::Count, nullptr},
#include "lang/ru.inc"
        };
        inline constexpr Entry kZh[] = {
            {S::Count, nullptr},
#include "lang/zh.inc"
        };
        inline constexpr Entry kZhHant[] = {
            {S::Count, nullptr},
#include "lang/zh-Hant.inc"
        };
#undef LANG_T

        // The sparse list as an array indexed by id; an id the list omits stays null.
        template <std::size_t N>
        constexpr Table dense(const Entry (&list)[N])
        {
            Table t{};
            for (const Entry& e : list)
            {
                if (e.id != S::Count)
                {
                    t[static_cast<std::size_t>(e.id)] = e.text;
                }
            }
            return t;
        }

        // Indexed by Culture.
        inline constexpr Table kTables[kCultureCount] = {
            kEn,       dense(kDe), dense(kEs), dense(kFr), dense(kIt), dense(kJa),
            dense(kKo), dense(kPt), dense(kRu), dense(kZh), dense(kZhHant),
        };

        template <std::size_t N>
        constexpr std::span<const Entry> listed(const Entry (&list)[N])
        {
            return std::span<const Entry>{list}.subspan(1);
        }

        inline constexpr std::span<const Entry> kEntries[kCultureCount] = {
            {},          listed(kDe), listed(kEs), listed(kFr), listed(kIt), listed(kJa),
            listed(kKo), listed(kPt), listed(kRu), listed(kZh), listed(kZhHant),
        };
    } // namespace detail

    // The English text of `id` - what every chain ends in, and what `fmt` checks against.
    constexpr const char* english(S id)
    {
        const std::size_t i = static_cast<std::size_t>(id);
        return i < kStrCount ? detail::kEn[i] : "";
    }

    // `id` in culture `c`, falling through `chain(c)` to English.
    constexpr const char* tr(S id, Culture c)
    {
        const std::size_t i = static_cast<std::size_t>(id);
        if (i >= kStrCount)
        {
            return "";
        }
        const Chain ch = chain(c);
        for (int k = 0; k < ch.n; ++k)
        {
            if (const char* s = detail::kTables[static_cast<std::size_t>(ch.c[k])][i]; s != nullptr)
            {
                return s;
            }
        }
        return detail::kEn[i];
    }

    // `id` in the active culture.
    inline const char* tr(S id)
    {
        return tr(id, active());
    }

    // What culture `c`'s own file lists, for the tests: ids, duplicates, specifiers.
    constexpr std::span<const Entry> entries(Culture c)
    {
        const int i = static_cast<int>(c);
        return (i >= 0 && i < kCultureCount) ? detail::kEntries[i] : std::span<const Entry>{};
    }

    // `c`'s own text for `id`, or null where its file is silent.
    constexpr const char* own(S id, Culture c)
    {
        const std::size_t i = static_cast<std::size_t>(id);
        const int ci = static_cast<int>(c);
        return (i < kStrCount && ci >= 0 && ci < kCultureCount) ? detail::kTables[ci][i] : nullptr;
    }

    // A formatted line, held by value for the statement that draws it. The copy
    // constructor is user-provided on purpose: it makes the type non-trivial, so passing
    // one through `...` instead of its c_str() is C4840, which xmake.lua makes an error.
    template <std::size_t N>
    struct Text
    {
        char s[N]{};

        template <typename... A>
        explicit Text(const char* format, A... args)
        {
            (void)utf8::format(s, N, format, args...);
        }
        Text(const Text& other)
        {
            std::memcpy(s, other.s, N);
        }
        Text& operator=(const Text&) = delete;

        const char* c_str() const
        {
            return s;
        }
    };

    // `id` formatted with `args` in the active culture, truncated on a character
    // boundary at N bytes. The arguments are checked against the English specifiers at
    // compile time; markers_test holds every translation to the same specifiers.
    template <S Id, std::size_t N = 256, typename... A>
    Text<N> fmt(A... args)
    {
        static_assert(fspec::matches<A...>(english(Id)),
                      "the arguments do not match the English text's printf specifiers");
        return Text<N>(tr(Id), args...);
    }
} // namespace lang
