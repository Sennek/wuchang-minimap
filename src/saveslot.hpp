#pragma once

//
// saveslot - "which save game is this?", so the collection tracker is per-slot: one key
// per save, one file per key (`wuchang_minimap_found_<key>.txt`).
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
//                 `<accountid>_<slot>` from the path.
//   4. `shared` - nothing answered; the global file.
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
    // `36053875_maingame0` is 18.
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

    // The tracker file's name for a key. An empty key is the shared file.
    inline std::string found_filename(std::string_view key)
    {
        if (key.empty())
        {
            return "wuchang_minimap_found.txt";
        }
        return "wuchang_minimap_found_" + std::string{key} + ".txt";
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

    // `...\Saved\36053875\GameSlots\...` -> `36053875`.
    inline std::string account_from_path(std::string_view path)
    {
        return detail::component_after(path, "Saved");
    }

    // The full route-3 key: `<accountid>_<slot>`, or just the slot when the account
    // component is not in the path, or "" when neither is.
    inline std::string key_from_sav_path(std::string_view path)
    {
        const std::string slot = slot_from_path(path);
        if (slot.empty())
        {
            return {};
        }
        const std::string acct = account_from_path(path);
        return sanitise_key(acct.empty() ? slot : acct + "_" + slot);
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
