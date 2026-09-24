#pragma once

//
// lang - which language the overlay speaks. PURE: no Windows, no UE4SS, no D3D12, so
// tests/markers_test.cpp links it.
//
// A culture is one of the eleven the game ships, named by its locres folder
// (`Content/Localization/MMGame/<code>`). This module is the one place a culture string is
// parsed, the one place the culture in force is decided from the config, the game and
// Windows, the one place the fallback chain is decided, and the one place a culture's
// fonts are named: the string table (lang_strings.hpp), the marker-name loader, langsel
// and the overlay's font all ask it, and none of them knows the rule.
//
// The active culture is process-wide. The loop thread sets it; the render thread reads it
// once per string, relaxed - a switch mid-frame costs one frame of mixed languages.
//

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lang
{
    // En first: it is every chain's last link and the table the string ids are declared
    // against. The order is the tables' index and nothing else.
    enum class Culture : std::uint8_t
    {
        En = 0,
        De,
        Es,
        Fr,
        It,
        Ja,
        Ko,
        Pt,
        Ru,
        Zh,     // Simplified Chinese, the game's native culture
        ZhHant, // Traditional Chinese
        Count,
    };
    constexpr int kCultureCount = static_cast<int>(Culture::Count);

    // The locres folder name: "en", "zh", "zh-Hant".
    constexpr std::string_view code(Culture c)
    {
        constexpr std::string_view kCodes[kCultureCount] = {"en", "de", "es", "fr", "it", "ja",
                                                            "ko", "pt", "ru", "zh", "zh-Hant"};
        const int i = static_cast<int>(c);
        return (i >= 0 && i < kCultureCount) ? kCodes[i] : std::string_view{"en"};
    }

    namespace detail
    {
        constexpr char fold(char c)
        {
            if (c >= 'A' && c <= 'Z')
            {
                return static_cast<char>(c - 'A' + 'a');
            }
            return c == '_' ? '-' : c;
        }

        constexpr bool is_space(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"';
        }

        constexpr std::string_view trim(std::string_view v)
        {
            while (!v.empty() && is_space(v.front()))
            {
                v.remove_prefix(1);
            }
            while (!v.empty() && is_space(v.back()))
            {
                v.remove_suffix(1);
            }
            return v;
        }

        // `text` folded equals `want`, or starts with `want` followed by a '-' subtag.
        constexpr bool tag_is(std::string_view text, std::string_view want)
        {
            if (text.size() < want.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < want.size(); ++i)
            {
                if (fold(text[i]) != want[i])
                {
                    return false;
                }
            }
            return text.size() == want.size() || fold(text[want.size()]) == '-';
        }
    } // namespace detail

    // A culture as the game or a player might spell it: the locres codes, plus the
    // regional and script forms UE writes to GameUserSettings.ini `Language=` - `zh-CN`,
    // `zh-Hans`, `zh_TW`, `ja-JP`, `pt-BR`, `es-419` - case-insensitive, `_` for `-`.
    // Chinese is decided by script first, then by region: Hant, TW, HK and MO are
    // Traditional, everything else Simplified. False for anything else.
    constexpr bool parse_culture(std::string_view text, Culture& out)
    {
        const std::string_view t = detail::trim(text);
        if (detail::tag_is(t, "zh"))
        {
            const bool trad = detail::tag_is(t, "zh-hant") || detail::tag_is(t, "zh-tw") ||
                              detail::tag_is(t, "zh-hk") || detail::tag_is(t, "zh-mo");
            out = trad ? Culture::ZhHant : Culture::Zh;
            return true;
        }
        constexpr struct
        {
            std::string_view tag;
            Culture culture;
        } kTags[] = {
            {"en", Culture::En}, {"de", Culture::De}, {"es", Culture::Es}, {"fr", Culture::Fr},
            {"it", Culture::It}, {"ja", Culture::Ja}, {"ko", Culture::Ko}, {"pt", Culture::Pt},
            {"ru", Culture::Ru},
        };
        for (const auto& k : kTags)
        {
            if (detail::tag_is(t, k.tag))
            {
                out = k.culture;
                return true;
            }
        }
        return false;
    }

    // The config override `language = auto | <culture>`. True with `is_auto` set for
    // `auto` and for an empty value; true with `out` set for a culture; false for anything
    // else, which the caller reports and treats as `auto`.
    constexpr bool parse_override(std::string_view text, bool& is_auto, Culture& out)
    {
        const std::string_view t = detail::trim(text);
        if (t.empty() || detail::tag_is(t, "auto"))
        {
            is_auto = true;
            return true;
        }
        is_auto = false;
        return parse_culture(t, out);
    }

    // Who decided the culture in force.
    enum class Source : std::uint8_t
    {
        Config,  // `language = <culture>`
        Game,    // the game's own GameUserSettings.ini `Language=`
        Windows, // the Windows display language, when the game names none
        Default, // nobody said anything usable: English
    };

    struct Decision
    {
        Culture culture = Culture::En;
        Source source = Source::Default;
    };

    // THE LANGUAGE DECISION. The override when it names a culture. Else the game's own
    // `Language=` - and English when the game names one the mod has no table for, because
    // the overlay follows the game rather than second-guessing it. Only when the game names
    // none at all (no ini yet, a first launch, a build that keeps it elsewhere) is the
    // Windows display language asked; else English. An override that is not a culture reads
    // as `auto` here; the config loader has already reported it.
    constexpr Decision decide(std::string_view override_text, std::string_view game_language,
                              std::string_view os_locale)
    {
        bool is_auto = true;
        Culture c = Culture::En;
        if (parse_override(override_text, is_auto, c) && !is_auto)
        {
            return Decision{c, Source::Config};
        }
        if (!detail::trim(game_language).empty())
        {
            return Decision{parse_culture(game_language, c) ? c : Culture::En, Source::Game};
        }
        if (parse_culture(os_locale, c))
        {
            return Decision{c, Source::Windows};
        }
        return Decision{Culture::En, Source::Default};
    }

    namespace detail
    {
        constexpr char lower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        constexpr bool ieq(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (lower(a[i]) != lower(b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        constexpr std::string_view trim_blank(std::string_view v)
        {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r'))
            {
                v.remove_prefix(1);
            }
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
            {
                v.remove_suffix(1);
            }
            return v;
        }
    } // namespace detail

    // `[Internationalization]` `Language=` out of GameUserSettings.ini's bytes, or empty when
    // the section or the key is absent. UE writes the file as ASCII, or as UTF-16LE behind a
    // BOM once any value in it is not ANSI; both are read, and only ASCII survives, which is
    // all a culture tag is. Sections and keys compare case-insensitively, the way UE's own
    // config reader does.
    inline std::string ini_language(std::string_view bytes)
    {
        std::string text;
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE)
        {
            text.reserve(bytes.size() / 2);
            for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                const unsigned u = static_cast<unsigned char>(bytes[i]) |
                                   (static_cast<unsigned>(static_cast<unsigned char>(bytes[i + 1])) << 8);
                text.push_back(u < 0x80u ? static_cast<char>(u) : '?');
            }
        }
        else
        {
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.remove_prefix(3);
            }
            text.assign(bytes);
        }

        const std::string_view all{text};
        bool in_section = false;
        std::size_t at = 0;
        while (at <= all.size())
        {
            std::size_t end = all.find('\n', at);
            if (end == std::string_view::npos)
            {
                end = all.size();
            }
            const std::string_view line = detail::trim_blank(all.substr(at, end - at));
            at = end + 1;
            if (line.empty() || line.front() == ';' || line.front() == '#')
            {
                continue;
            }
            if (line.front() == '[')
            {
                const std::size_t close = line.find(']');
                in_section = close != std::string_view::npos &&
                             detail::ieq(detail::trim_blank(line.substr(1, close - 1)), "Internationalization");
                continue;
            }
            const std::size_t eq = line.find('=');
            if (in_section && eq != std::string_view::npos &&
                detail::ieq(detail::trim_blank(line.substr(0, eq)), "Language"))
            {
                return std::string{detail::trim_blank(line.substr(eq + 1))};
            }
        }
        return {};
    }

    // A Windows font, by file name in the Fonts folder and face index inside a collection.
    struct FontFile
    {
        std::string_view file;
        int face = 0;
    };

    // What every UI font is backed by: Latin with its accents, Cyrillic, the punctuation
    // the tables use. Merged under any base that is not this font itself.
    inline constexpr FontFile kTextFont{"segoeui.ttf", 0};

    namespace detail
    {
        inline constexpr FontFile kZhFonts[] = {{"msyh.ttc", 0}};
        inline constexpr FontFile kZhHantFonts[] = {{"msjh.ttc", 0}};
        // Yu Gothic stops at JIS: the game's own Japanese names use a Han character it lacks
        // (U+83B9, measured by markers_test), which Microsoft YaHei has.
        inline constexpr FontFile kJaFonts[] = {{"YuGothM.ttc", 0}, {"msyh.ttc", 0}};
        inline constexpr FontFile kKoFonts[] = {{"malgun.ttf", 0}};
    } // namespace detail

    // The script fonts culture `c` needs beyond kTextFont, merged under it in this order.
    // Han is drawn differently per locale, so each CJK culture leads with its own face.
    // Empty for the rest.
    constexpr std::span<const FontFile> script_fonts(Culture c)
    {
        switch (c)
        {
        case Culture::Zh:
            return detail::kZhFonts;
        case Culture::ZhHant:
            return detail::kZhHantFonts;
        case Culture::Ja:
            return detail::kJaFonts;
        case Culture::Ko:
            return detail::kKoFonts;
        default:
            return {};
        }
    }

    // Each culture's name in its own language, as the language list shows it in every
    // culture. Not a translation: the same in every table.
    constexpr std::string_view endonym(Culture c)
    {
        constexpr std::string_view kNames[kCultureCount] = {
            "English",  "Deutsch", "Español",   "Français", "Italiano", "日本語",
            "한국어",   "Português", "Русский", "简体中文", "繁體中文",
        };
        const int i = static_cast<int>(c);
        return (i >= 0 && i < kCultureCount) ? kNames[i] : std::string_view{"English"};
    }

    // What the language list needs beyond the culture's own fonts to draw every endonym:
    // Microsoft YaHei carries the Han of all three CJK names, Malgun Gothic the Hangul. No
    // one Windows font has both (measured), and together they are 33 MB, so the overlay
    // merges them only once the list is about to be read.
    inline constexpr FontFile kEndonymFonts[] = {{"msyh.ttc", 0}, {"malgun.ttf", 0}};

    // THE FALLBACK CHAIN, most specific first and always ending in English: zh-Hant ->
    // zh -> en, every other culture -> en. Traditional falls back to Simplified because
    // the game's own zh-Hant locres lacks keys its zh one has.
    struct Chain
    {
        Culture c[3]{};
        int n = 0;
    };

    constexpr Chain chain(Culture c)
    {
        if (c == Culture::ZhHant)
        {
            return Chain{{Culture::ZhHant, Culture::Zh, Culture::En}, 3};
        }
        if (c == Culture::En || static_cast<int>(c) < 0 || static_cast<int>(c) >= kCultureCount)
        {
            return Chain{{Culture::En}, 1};
        }
        return Chain{{c, Culture::En}, 2};
    }

    // The chain as the marker loader takes it: the culture codes to look up in a record's
    // `names`, in order, WITHOUT English - `name` is the English and the loader's own
    // last resort. Empty for English.
    namespace detail
    {
        struct NameCodes
        {
            std::string_view code[2]{};
            std::size_t n = 0;
        };

        // Row i is chain(Culture(i)) minus its English tail, spelled as locres codes -
        // derived, so the chain keeps its one home above.
        inline constexpr auto kNameCodes = [] {
            std::array<NameCodes, kCultureCount> rows{};
            for (int i = 0; i < kCultureCount; ++i)
            {
                const Chain ch = chain(static_cast<Culture>(i));
                for (int k = 0; k + 1 < ch.n; ++k)
                {
                    rows[static_cast<std::size_t>(i)].code[k] = code(ch.c[k]);
                }
                rows[static_cast<std::size_t>(i)].n = static_cast<std::size_t>(ch.n - 1);
            }
            return rows;
        }();
    } // namespace detail

    constexpr std::span<const std::string_view> name_codes(Culture c)
    {
        const int i = static_cast<int>(c);
        if (i < 0 || i >= kCultureCount)
        {
            return {};
        }
        const detail::NameCodes& row = detail::kNameCodes[static_cast<std::size_t>(i)];
        return std::span<const std::string_view>{row.code, row.n};
    }

    // Process-wide. Defaults to English until the loop thread knows better.
    inline std::atomic<Culture> g_active{Culture::En};

    inline void set_active(Culture c)
    {
        g_active.store(static_cast<int>(c) >= 0 && static_cast<int>(c) < kCultureCount ? c : Culture::En,
                       std::memory_order_relaxed);
    }

    inline Culture active()
    {
        return g_active.load(std::memory_order_relaxed);
    }
} // namespace lang
