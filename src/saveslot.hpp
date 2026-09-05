#pragma once

//
// saveslot - "which save game is this?", so the collection tracker and the full map's
// waypoints are per-slot: one key per save, one file per key
// (`wuchang_minimap_found_<key>.txt`, `wuchang_minimap_waypoint_<key>.txt`).
//
// The save is written by the native `GameSaverNative` plugin, not UGameplayStatics, to
//
//     %LOCALAPPDATA%\Project_Plague\Saved\<SteamAccountID>\GameSlots\<slot>\<slot>.sav
//
// and the per-save identity inside it is `datasummery.uuid` (32 hex chars). Four routes,
// tried in order, each logging the answer:
//
//   1. `uuid`   - `Get Save Slot Value` on the GameMode's `GameSaveExe` component. Made
//                 only when the UFunction's reflected parameter list matches the
//                 prediction in saveslot.cpp; on mismatch the rung is refused.
//   2. `slot`   - `Impl_GameSettingsSaver_C::TickCountSavPath`, an FString containing
//                 `...\GameSlots\<slot>\...`. Raw property read, no ProcessEvent.
//   3. `file`   - newest `*.sav` under `%LOCALAPPDATA%\Project_Plague\Saved`, giving
//                 `<slot>` from the path.
//   4. `shared` - nothing answered; the global file.
//
// Routes 2 and 3 are the same key for the same save: the slot name alone. Route 2's
// `TickCountSavPath` is engine-relative (`GameSaved/GameSlots/<slot>`) and carries no
// account id, so the account id is no part of the key - one save, one file, whichever
// route answers first.
//
// `found_profile` (Player tier) overrides the ladder: `auto` runs it, `shared` pins the
// global file, anything else is the key verbatim.
//
// The top half of this header is pure (no Windows, no UE4SS) and tested offline in
// tests/markers_test.cpp.
//

#include <cstdint>
#include <string>
#include <string_view>

namespace slotid
{
    //==================================================================================
    // Pure: keys and filenames
    //==================================================================================

    // Longest key allowed in a filename. A uuid is 32 chars; a slot key like
    // `maingame0` is 9.
    inline constexpr std::size_t kMaxKeyLen = 48;

    inline bool key_char_ok(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    }

    // Filename-safe form of a route's answer: everything outside [A-Za-z0-9_-] becomes
    // '_', runs of '_' collapse, leading/trailing '_' drop, capped at kMaxKeyLen. Empty
    // result means "no key" - the caller falls back to the shared file.
    inline std::string sanitise_key(std::string_view raw)
    {
        std::string out;
        out.reserve(raw.size() < kMaxKeyLen ? raw.size() : kMaxKeyLen);
        bool last_us = true; // suppresses a leading '_'
        for (char c : raw)
        {
            if (out.size() >= kMaxKeyLen)
            {
                break;
            }
            if (key_char_ok(c))
            {
                out.push_back(c);
                last_us = false;
            }
            else if (!last_us)
            {
                out.push_back('_');
                last_us = true;
            }
        }
        while (!out.empty() && out.back() == '_')
        {
            out.pop_back();
        }
        return out;
    }

    // The two per-key filename families: `<prefix><key>.txt`.
    inline constexpr std::string_view kFoundPrefix = "wuchang_minimap_found_";
    inline constexpr std::string_view kWaypointPrefix = "wuchang_minimap_waypoint_";
    inline constexpr std::string_view kKeyedSuffix = ".txt";

    // The tracker file's name for a key. An empty key is the shared file.
    inline std::string found_filename(std::string_view key)
    {
        if (key.empty())
        {
            return "wuchang_minimap_found.txt";
        }
        return std::string{kFoundPrefix} + std::string{key} + std::string{kKeyedSuffix};
    }

    // The full map's waypoint file, named after the same key. An empty key is the shared file.
    inline std::string waypoint_filename(std::string_view key)
    {
        if (key.empty())
        {
            return "wuchang_minimap_waypoint.txt";
        }
        return std::string{kWaypointPrefix} + std::string{key} + std::string{kKeyedSuffix};
    }

    // The key inside `<prefix><key>.txt`, or "" when `name` is not that shape. The key
    // itself must be a sanitised one, so `found.txt.bak` and a shared file are both "".
    inline std::string key_in_filename(std::string_view name, std::string_view prefix)
    {
        if (name.size() <= prefix.size() + kKeyedSuffix.size())
        {
            return {};
        }
        if (name.compare(0, prefix.size(), prefix) != 0)
        {
            return {};
        }
        if (name.compare(name.size() - kKeyedSuffix.size(), kKeyedSuffix.size(), kKeyedSuffix) != 0)
        {
            return {};
        }
        const std::string_view key =
            name.substr(prefix.size(), name.size() - prefix.size() - kKeyedSuffix.size());
        if (key.size() > kMaxKeyLen)
        {
            return {};
        }
        for (char c : key)
        {
            if (!key_char_ok(c))
            {
                return {};
            }
        }
        return std::string{key};
    }

    // True when `candidate` names the same save as `canonical` under the pre-canonical
    // `<steam account id>_<slot>` spelling. The prefix is all digits, so a slot genuinely
    // named `ng_maingame0` is not mistaken for one.
    inline bool is_legacy_account_key(std::string_view canonical, std::string_view candidate)
    {
        if (canonical.empty() || candidate.size() <= canonical.size() + 1)
        {
            return false;
        }
        const std::size_t split = candidate.size() - canonical.size() - 1;
        if (candidate[split] != '_' || candidate.substr(split + 1) != canonical)
        {
            return false;
        }
        for (std::size_t i = 0; i < split; ++i)
        {
            if (candidate[i] < '0' || candidate[i] > '9')
            {
                return false;
            }
        }
        return true;
    }

    //==================================================================================
    // Pure: pulling a key out of a path
    //==================================================================================

    namespace detail
    {
        inline bool sep(char c)
        {
            return c == '\\' || c == '/';
        }

        // The path component that follows the (case-insensitive) component `what`, or
        // "" when there is none.
        inline std::string component_after(std::string_view path, std::string_view what)
        {
            std::size_t i = 0;
            std::string prev;
            std::string cur;
            const auto lower = [](std::string s) {
                for (char& c : s)
                {
                    if (c >= 'A' && c <= 'Z')
                    {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
                return s;
            };
            const std::string want = lower(std::string{what});
            for (; i <= path.size(); ++i)
            {
                const bool end = (i == path.size());
                if (end || sep(path[i]))
                {
                    if (!prev.empty() && lower(prev) == want && !cur.empty())
                    {
                        return cur;
                    }
                    if (!cur.empty())
                    {
                        prev = cur;
                        cur.clear();
                    }
                    continue;
                }
                cur.push_back(path[i]);
            }
            return {};
        }
    } // namespace detail

    // `...\Saved\36053875\GameSlots\maingame0\maingame0.sav` -> `maingame0`.
    inline std::string slot_from_path(std::string_view path)
    {
        return detail::component_after(path, "GameSlots");
    }

    // The key any path-shaped route produces: the slot name alone, or "" when the path
    // has no `GameSlots` component. Route 2 sees an engine-relative path with no account
    // id in it, so the account id is not part of the key.
    inline std::string key_from_sav_path(std::string_view path)
    {
        return sanitise_key(slot_from_path(path));
    }

    //==================================================================================
    // Runtime - implemented in saveslot.cpp
    //==================================================================================

    // Which rung of the ladder produced the key currently in force.
    enum class Route
    {
        None = 0,   // nothing has answered yet
        Uuid,       // 1. Get Save Slot Value -> datasummery/uuid
        SlotPath,   // 2. Impl_GameSettingsSaver_C::TickCountSavPath
        SavFile,    // 3. newest *.sav under %LOCALAPPDATA%
        Shared,     // 4. no key - the global file
        Forced,     // found_profile = <name>
    };

    const char* route_name(Route r);

    struct Status
    {
        char key[kMaxKeyLen + 1]{}; // "" = the shared file
        Route route = Route::None;
        // Rung outcome: "" until tried, otherwise the value produced or the refusal.
        char note[96]{};
    };

    // Any thread. A snapshot of the key in force.
    Status status();

    // Loop thread, once, before the game thread runs: route 3, so the first found-file
    // load already has a key.
    void on_unreal_init();

    // GAME THREAD ONLY. Runs routes 1 and 2 every few seconds until one answers, then
    // re-checks slowly to notice a slot switch.
    void game_thread_pump(std::uint64_t now, const void* world);

    // GAME THREAD ONLY. Drops the objects the routes cached; the key is re-resolved.
    void drop_caches();

    // Loop thread. Re-runs route 3 (used by F5 and by the slot-change watch).
    void rescan_files();
} // namespace slotid
