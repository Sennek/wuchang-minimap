//
// langsel - see langsel.hpp. Loop thread only.
//

#include "langsel.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>

#include "atomicfile.hpp"
#include "lang.hpp"
#include "mmstate.hpp"

namespace lsel
{
    namespace
    {
        constexpr std::uint64_t kIniPollMs = 1000;
        constexpr unsigned long long kIniCapBytes = 1ull << 20;

        // What the last decision read and answered.
        char g_override[sizeof(mm::Config::language)]{};
        std::uint64_t g_ini_stamp = 0; // 0 = absent, unreadable, or not looked at yet
        std::uint64_t g_last_poll = 0;
        bool g_decided = false;
        lang::Decision g_last{};
        std::atomic<lang::Culture> g_auto{lang::Culture::En};

        std::wstring widen(std::string_view s)
        {
            return std::wstring(s.begin(), s.end());
        }

        // The game keeps its user settings beside its saves; the path has no per-user part
        // beyond %LOCALAPPDATA%. Empty when there is no LOCALAPPDATA.
        const std::wstring& ini_path()
        {
            static const std::wstring path = [] {
                const std::wstring local = mm::local_app_data();
                return local.empty() ? std::wstring{}
                                     : local + L"\\Project_Plague\\Saved\\Config\\Windows\\GameUserSettings.ini";
            }();
            return path;
        }

        // Last write time and size mixed into one word; 0 when the file is not there.
        std::uint64_t file_stamp(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (path.empty() || ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) == 0)
            {
                return 0;
            }
            const std::uint64_t t = (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                    fad.ftLastWriteTime.dwLowDateTime;
            const std::uint64_t n = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            return (t ^ (n * 0x9E3779B97F4A7C15ull)) | 1u;
        }

        // `Language=` as the game last wrote it, or empty. `readable` is false when the file is
        // there but could not be read - the game holding it open mid-write - so the next poll
        // tries again instead of settling on the empty answer.
        std::string game_language(bool& readable)
        {
            readable = true;
            std::string bytes;
            const mmfile::ReadInfo info = mmfile::read_whole_file(ini_path(), bytes, kIniCapBytes);
            if (info.status == mmfile::ReadStatus::Ok)
            {
                return lang::ini_language(bytes);
            }
            readable = info.status == mmfile::ReadStatus::NotFound;
            return {};
        }

        // The Windows display language as a locale name ("ru-RU", "zh-TW"), or empty.
        std::string windows_language()
        {
            wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
            const LCID lcid = MAKELCID(::GetUserDefaultUILanguage(), SORT_DEFAULT);
            if (::LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) == 0)
            {
                return {};
            }
            std::string out;
            for (const wchar_t* p = name; *p != 0; ++p)
            {
                out.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
            }
            return out;
        }

        void log_decision(const lang::Decision& d, std::string_view game, std::string_view os)
        {
            const std::wstring code = widen(lang::code(d.culture));
            switch (d.source)
            {
            case lang::Source::Config:
                mm::logf(L"language: {} - set by `language = {}` in the config", code, code);
                break;
            case lang::Source::Game:
            {
                lang::Culture parsed = lang::Culture::En;
                if (lang::parse_culture(game, parsed))
                {
                    mm::logf(L"language: {} - the game's own (Language={} in {})", code, widen(game),
                             ini_path());
                }
                else
                {
                    mm::logf(L"language: {} - the game's Language={} in {} is not a language this mod "
                             L"has a table for",
                             code, widen(game), ini_path());
                }
                break;
            }
            case lang::Source::Windows:
                mm::logf(L"language: {} - the game names no language yet (no Language= in {}), so the "
                         L"Windows display language {}",
                         code, ini_path(), widen(os));
                break;
            case lang::Source::Default:
            default:
                mm::logf(L"language: {} - neither the config, the game ({}) nor Windows ('{}') names one "
                         L"this mod has",
                         code, ini_path(), widen(os));
                break;
            }
        }
    } // namespace

    bool refresh()
    {
        const mm::Config& cfg = mm::cfg_cached();
        std::memcpy(g_override, cfg.language, sizeof(g_override));

        bool readable = true;
        const std::uint64_t stamp = file_stamp(ini_path());
        const std::string game = game_language(readable);
        g_ini_stamp = readable ? stamp : 0;
        const std::string os = windows_language();

        const lang::Decision d = lang::decide(std::string_view{g_override, ::strnlen(g_override, sizeof(g_override))},
                                              game, os);
        const bool moved = !g_decided || d.culture != g_last.culture;
        if (moved || d.source != g_last.source)
        {
            log_decision(d, game, os);
        }
        g_decided = true;
        g_last = d;
        g_auto.store(lang::decide("auto", game, os).culture, std::memory_order_relaxed);
        lang::set_active(d.culture);
        return moved;
    }

    lang::Culture auto_culture()
    {
        return g_auto.load(std::memory_order_relaxed);
    }

    bool on_update(std::uint64_t now)
    {
        const mm::Config& cfg = mm::cfg_cached();
        bool due = std::memcmp(cfg.language, g_override, sizeof(g_override)) != 0;
        if (now - g_last_poll >= kIniPollMs)
        {
            g_last_poll = now;
            due = due || file_stamp(ini_path()) != g_ini_stamp;
        }
        return due && refresh();
    }
} // namespace lsel
