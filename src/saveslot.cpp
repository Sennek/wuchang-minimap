//
// saveslot - the runtime half. See saveslot.hpp for the ladder.
//
// THREADS
//   loop thread   on_unreal_init(), rescan_files()  - filesystem only (route 3)
//   game thread   game_thread_pump(), drop_caches() - reflection and raw reads only
//   any thread    status()                          - spinlocked copy
//

#include "saveslot.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "mem.hpp"
#include "mmstate.hpp"
#include "ue_min.hpp"
#include "spinlock.hpp"
#include "uereflect.hpp"

namespace slotid
{
    namespace
    {
        using RC::Unreal::UObject;
        namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

        spin::Spinlock g_lock;
        Status g_status; // guarded by g_lock

        void set_status(const std::string& key, Route route, const std::string& note)
        {
            spin::SpinGuard guard(g_lock);
            ::strncpy_s(g_status.key, sizeof(g_status.key), key.c_str(), _TRUNCATE);
            g_status.route = route;
            ::strncpy_s(g_status.note, sizeof(g_status.note), note.c_str(), _TRUNCATE);
        }

        std::wstring widen(const std::string& s)
        {
            std::wstring out;
            out.reserve(s.size());
            for (char c : s)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return out;
        }

        //==============================================================================
        // Route 3 - the filesystem (loop thread)
        //==============================================================================
        //
        // %LOCALAPPDATA%\Project_Plague\Saved\<account>\GameSlots\<slot>\<slot>.sav.
        // The newest one is the slot in play, and can lag one autosave behind a slot
        // switch; a slot switch goes through a full level reload, which re-runs the
        // game-thread routes.

        std::wstring saved_root()
        {
            wchar_t buf[MAX_PATH]{};
            DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return {};
            }
            return std::wstring{buf} + L"\\Project_Plague\\Saved";
        }

        std::vector<std::wstring> subdirs(const std::wstring& dir, std::size_t cap)
        {
            std::vector<std::wstring> out;
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE)
            {
                return out;
            }
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    continue;
                }
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..")
                {
                    continue;
                }
                out.push_back(name);
            } while (::FindNextFileW(h, &fd) != 0 && out.size() < cap);
            ::FindClose(h);
            return out;
        }

        bool newest_sav(std::wstring& out_path)
        {
            const std::wstring root = saved_root();
            if (root.empty())
            {
                return false;
            }
            ULARGE_INTEGER best{};
            best.QuadPart = 0;
            bool any = false;
            for (const std::wstring& account : subdirs(root, 32))
            {
                const std::wstring slots = root + L"\\" + account + L"\\GameSlots";
                for (const std::wstring& slot : subdirs(slots, 64))
                {
                    const std::wstring dir = slots + L"\\" + slot;
                    WIN32_FIND_DATAW fd{};
                    HANDLE h = ::FindFirstFileW((dir + L"\\*.sav").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                    {
                        continue;
                    }
                    do
                    {
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        {
                            continue;
                        }
                        ULARGE_INTEGER t{};
                        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                        if (t.QuadPart > best.QuadPart)
                        {
                            best = t;
                            out_path = dir + L"\\" + fd.cFileName;
                            any = true;
                        }
                    } while (::FindNextFileW(h, &fd) != 0);
                    ::FindClose(h);
                }
            }
            return any;
        }

        //==============================================================================
        // Routes 1 and 2 - reflection (game thread)
        //==============================================================================

        uer::LayoutCache g_layouts;
        uer::FuncCache g_funcs;
        uer::ObjRef g_save_exec;    // GameSaveExecutor (route 1)
        uer::ObjRef g_settings;     // Impl_GameSettingsSaver_C / GameSettingsSaver_C (route 2)
        bool g_uuid_sig_logged = false;
        bool g_resolved_by_engine = false;
        std::uint64_t g_last_try_ms = 0;

        // Captures the first live instance of `class_name`. FindAllOf is a whole-object-
        // array walk (28-51 ms here), so callers must be throttled.
        bool find_one(const wchar_t* class_name, uer::ObjRef& out)
        {
            std::vector<UObject*> objs;
            UObjectGlobals::FindAllOf(class_name, objs);
            for (UObject* obj : objs)
            {
                if (obj == nullptr)
                {
                    continue;
                }
                if (uer::capture(obj, out))
                {
                    return true;
                }
            }
            return false;
        }

        // Route 1's signature self-check. The accepted shapes are
        //     (FString key, FString ReturnValue)                   2 params
        //     (FString section, FString key, FString ReturnValue)  3 params
        // every one a 16-byte FString. On anything else the call is not made.
        struct UuidSig
        {
            bool ok = false;
            int params = 0;
            int block = 0;
            std::string detail;
        };

        UuidSig check_uuid_signature(UObject* obj, const wchar_t* fname)
        {
            UuidSig sig{};
            RC::Unreal::UFunction* fn = g_funcs.get(obj, fname);
            if (fn == nullptr)
            {
                sig.detail = "no such UFunction";
                return sig;
            }
            const std::vector<uer::ParamInfo> params = uer::func_params(fn);
            sig.params = static_cast<int>(params.size());
            sig.block = uer::func_param_size(fn);
            std::string detail;
            bool all_strings = !params.empty();
            for (const uer::ParamInfo& p : params)
            {
                if (!detail.empty())
                {
                    detail += ", ";
                }
                detail += uer::narrow_ascii(p.name) + "@" + std::to_string(p.offset) + "/" +
                          std::to_string(p.size);
                if (p.size != 16)
                {
                    all_strings = false;
                }
            }
            sig.detail = detail;
            sig.ok = all_strings && (sig.params == 2 || sig.params == 3) && sig.block >= 32;
            return sig;
        }

        // Candidate spellings: `GameSaveExecutor` is native (/Script/GameSaverNative),
        // so its C++ name has no spaces while blueprint call nodes name it with them.
        const wchar_t* const kUuidFuncNames[] = {
            L"Get Save Slot Value",
            L"GetSaveSlotValue",
            L"GetSaveSlotStrValue",
        };

        bool try_route_uuid(std::string& key_out, std::string& note_out)
        {
            if (g_save_exec.empty() && !find_one(L"GameSaveExecutor", g_save_exec))
            {
                note_out = "no GameSaveExecutor instance";
                return false;
            }
            if (!uer::alive(g_save_exec))
            {
                g_save_exec.reset();
                note_out = "GameSaveExecutor went away";
                return false;
            }
            for (const wchar_t* fname : kUuidFuncNames)
            {
                const UuidSig sig = check_uuid_signature(g_save_exec.obj, fname);
                if (sig.params == 0 && !sig.ok)
                {
                    continue; // no such spelling
                }
                if (!g_uuid_sig_logged)
                {
                    g_uuid_sig_logged = true;
                    mm::logf(L"saveslot: '{}' found on GameSaveExecutor - {} param(s), block {} B: {}",
                             fname, sig.params, sig.block, widen(sig.detail));
                }
                if (!sig.ok)
                {
                    note_out = "signature mismatch: " + sig.detail;
                    return false;
                }
                // Gated behind saveslot_uuid_call: an FString passed by value into
                // ProcessEvent is destroyed by the engine with FMemory, which would be
                // handed a CRT-allocated pointer. The signature check cannot rule that
                // out; the recon dump has to show the parameter is a const-ref/out pair.
                const mm::Config& cfg = mm::cfg_cached();
                if (!cfg.saveslot_uuid_call)
                {
                    note_out = "signature OK, call disabled (saveslot_uuid_call = 0)";
                    return false;
                }
                std::vector<std::uint8_t> block(static_cast<std::size_t>(sig.block) + 16u, 0u);
                // Empty input FStrings are the only shape safe for the engine to free.
                RC::Unreal::UFunction* fn = g_funcs.get(g_save_exec.obj, fname);
                if (fn == nullptr || !mem::guarded_call(&uer::process_event_trampoline, g_save_exec.obj,
                                                        fn, block.data()))
                {
                    note_out = "ProcessEvent faulted";
                    return false;
                }
                std::wstring value;
                const std::size_t ret_off = static_cast<std::size_t>(sig.params - 1) * 16u;
                if (!uer::read_fstring_at(block.data() + ret_off, value) || value.empty())
                {
                    note_out = "call returned nothing";
                    return false;
                }
                key_out = sanitise_key(uer::narrow_ascii(value));
                note_out = "uuid " + key_out;
                return !key_out.empty();
            }
            note_out = "no known Get-Save-Slot-Value spelling on GameSaveExecutor";
            return false;
        }

        const wchar_t* const kSettingsClasses[] = {
            L"Impl_GameSettingsSaver_C",
            L"GameSettingsSaver_C",
        };

        bool try_route_slot_path(std::string& key_out, std::string& note_out)
        {
            if (g_settings.empty())
            {
                for (const wchar_t* cls : kSettingsClasses)
                {
                    if (find_one(cls, g_settings))
                    {
                        mm::logf(L"saveslot: settings saver found ({})", cls);
                        break;
                    }
                }
            }
            if (g_settings.empty())
            {
                note_out = "no GameSettingsSaver instance";
                return false;
            }
            if (!uer::alive(g_settings))
            {
                g_settings.reset();
                note_out = "GameSettingsSaver went away";
                return false;
            }
            const uer::ClassLayout* layout = g_layouts.get(g_settings.obj);
            std::wstring path;
            if (!uer::read_fstring_prop(layout, g_settings.obj, L"TickCountSavPath", path) || path.empty())
            {
                note_out = "TickCountSavPath unreadable or empty";
                return false;
            }
            const std::string narrow = uer::narrow_ascii(path);
            const std::string key = key_from_sav_path(narrow);
            if (key.empty())
            {
                note_out = "TickCountSavPath has no GameSlots component: " + narrow;
                return false;
            }
            key_out = key;
            note_out = "slot path " + narrow;
            return true;
        }
    } // namespace

    const char* route_name(Route r)
    {
        switch (r)
        {
        case Route::Uuid:
            return "uuid";
        case Route::SlotPath:
            return "slot path";
        case Route::SavFile:
            return "sav file";
        case Route::Shared:
            return "shared";
        case Route::Forced:
            return "forced";
        case Route::None:
        default:
            return "unresolved";
        }
    }

    Status status()
    {
        spin::SpinGuard guard(g_lock);
        return g_status;
    }

    void rescan_files()
    {
        const mm::Config& cfg = mm::cfg_cached();
        // An explicit profile wins over every route; `shared` pins the global file.
        const std::string profile = cfg.found_profile;
        if (profile == "shared")
        {
            set_status(std::string{}, Route::Forced, "found_profile = shared");
            return;
        }
        if (!profile.empty() && profile != "auto")
        {
            const std::string key = sanitise_key(profile);
            set_status(key, Route::Forced, "found_profile = " + profile);
            return;
        }
        if (g_resolved_by_engine)
        {
            return; // a game-thread route already answered
        }
        std::wstring path;
        if (!newest_sav(path))
        {
            set_status(std::string{}, Route::Shared, "no .sav found under %LOCALAPPDATA%");
            return;
        }
        const std::string narrow = uer::narrow_ascii(path);
        const std::string key = key_from_sav_path(narrow);
        if (key.empty())
        {
            set_status(std::string{}, Route::Shared, "newest .sav has no slot component");
            return;
        }
        set_status(key, Route::SavFile, "newest .sav " + narrow);
    }

    void on_unreal_init()
    {
        rescan_files();
        const Status s = status();
        mm::logf(L"saveslot: profile key '{}' via {} ({})", widen(s.key), widen(route_name(s.route)),
                 widen(s.note));
    }

    void drop_caches()
    {
        g_layouts.clear();
        g_funcs.clear();
        g_save_exec.reset();
        g_settings.reset();
        g_resolved_by_engine = false;
        g_last_try_ms = 0;
        g_uuid_sig_logged = false;
    }

    void game_thread_pump(std::uint64_t now, const void*)
    {
        const mm::Config& cfg = mm::cfg_cached();
        if (std::strcmp(cfg.found_profile, "auto") != 0)
        {
            return;
        }
        // Once a route has answered, stop: FindAllOf is a whole-array walk and the
        // answer only changes with the world (drop_caches re-arms it).
        if (g_resolved_by_engine)
        {
            return;
        }
        if (g_last_try_ms != 0 && now - g_last_try_ms < 5000)
        {
            return;
        }
        g_last_try_ms = now;

        std::string key;
        std::string note;
        if (try_route_uuid(key, note) && !key.empty())
        {
            g_resolved_by_engine = true;
            set_status(key, Route::Uuid, note);
            mm::logf(L"saveslot: key '{}' via uuid ({})", widen(key), widen(note));
            return;
        }
        std::string uuid_note = note;

        if (try_route_slot_path(key, note) && !key.empty())
        {
            g_resolved_by_engine = true;
            set_status(key, Route::SlotPath, note);
            mm::logf(L"saveslot: key '{}' via slot path ({}); uuid route said: {}", widen(key),
                     widen(note), widen(uuid_note));
            return;
        }
        // Neither engine route answered; route 3's answer stays in force.
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            mm::logf(L"saveslot: no engine route yet - uuid: {}; slot path: {}", widen(uuid_note),
                     widen(note));
        }
    }
} // namespace slotid
