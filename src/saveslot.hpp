#pragma once

//
// saveslot - "which save game is this?", so the collection tracker can be per-slot.
//
// WHY
// ---
// `wuchang_minimap_found.txt` was global, so starting a second character inherited the
// first one's collection and every marker was already grey. The fix is one key per save
// and one file per key: `wuchang_minimap_found_<key>.txt`.
//
// THE KEY, AND THE LADDER THAT PRODUCES IT
// ----------------------------------------
// `context/saveslot-and-teleport-research.md` established (from the save file itself)
// that the save is NOT a UGameplayStatics SaveGame - it is the native `GameSaverNative`
// plugin writing
//
//     %LOCALAPPDATA%\Project_Plague\Saved\<SteamAccountID>\GameSlots\<slot>\<slot>.sav
//
// and that the per-save identity inside it is `datasummery.uuid` (32 hex chars). Three
// routes can produce a key, tried in this order and each one LOGGED with the route that
// answered - a silent fallback is what makes a wrong found file impossible to diagnose:
//
//   1. `uuid`   - the game's own KV accessor (`Get Save Slot Value`) on the GameMode's
//                 `GameSaveExe` component. Research §1.2. The parameter arity is not
//                 recoverable offline, so the call is made ONLY when the UFunction's
//                 reflected parameter list matches what we predict (see saveslot.cpp);
//                 on any mismatch the rung is refused and the reason logged.
//   2. `slot`   - `Impl_GameSettingsSaver_C::TickCountSavPath`, a plain FString that
//                 contains `...\GameSlots\<slot>\...`. Raw property read, no
//                 ProcessEvent at all. Research §1.3.1.
//   3. `file`   - the newest `*.sav` under `%LOCALAPPDATA%\Project_Plague\Saved`, which
//                 gives `<accountid>_<slot>` straight from the path. Research §1.3.2
//                 without the zlib chain: the mod only needs a key that is DIFFERENT
//                 for two different saves, and the slot directory is exactly that - the
//                 uuid is nicer but not more discriminating, and inflating the save to
//                 get it would be 300 lines of decoder for no behavioural difference.
//   4. `shared` - nothing answered; the old global file is used, exactly as before.
//
// `found_profile` (Player tier) overrides the ladder: `auto` runs it, `shared` pins the
// global file, anything else is used verbatim as the key.
//
// This header's top half is PURE (no Windows, no UE4SS) so the filename rules and the
// path parsing are tested offline in tests/markers_test.cpp - the same split every
// other module here uses.
//

#include <cstdint>
#include <string>
#include <string_view>

namespace slotid
{
    //==================================================================================
    // Pure: keys and filenames
    //==================================================================================

    // The longest key we will ever put in a filename. A uuid is 32 chars; a slot key
    // like `36053875_maingame0` is 18.
    inline constexpr std::size_t kMaxKeyLen = 48;

    inline bool key_char_ok(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    }

    // Filename-safe form of whatever a route produced. Everything outside
    // [A-Za-z0-9_-] becomes '_', runs of '_' collapse, leading/trailing '_' are
    // dropped, and the result is capped at kMaxKeyLen. An empty result means "no key" -
    // the caller must then fall back to the shared file rather than writing
    // `wuchang_minimap_found_.txt`.
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

    // The tracker file's name for a key. An empty key is the shared (pre-0.9.4) file,
    // which is also what every fallback lands on.
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
    // Runtime (not pure) - implemented in saveslot.cpp
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
        // Every rung's outcome, for the F2 readout and the log: "" until it has been
        // tried, otherwise either the value it produced or why it refused.
        char note[96]{};
    };

    // Any thread. A snapshot of the key in force.
    Status status();

    // Loop thread, once, before the game thread ever runs: route 3 (the filesystem) so
    // the very first found-file load already has a key.
    void on_unreal_init();

    // GAME THREAD ONLY, from markers::game_thread_pump. Runs routes 1 and 2 at most
    // once every few seconds until one answers, then re-checks slowly so a slot switch
    // is noticed.
    void game_thread_pump(std::uint64_t now, const void* world);

    // GAME THREAD ONLY, from markers::drop_caches: the world changed, so the objects
    // the routes cached are dead and the key must be re-resolved from scratch.
    void drop_caches();

    // Loop thread. Re-runs route 3 (used by F5 and by the slot-change watch).
    void rescan_files();
} // namespace slotid
