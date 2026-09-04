//
// recon - see recon.hpp. Read-only reflection, one file out.
//

#include "recon.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "mem.hpp"
#include "mmstate.hpp"
#include "shrines.hpp"
#include "spinlock.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

namespace recon
{
    namespace
    {
        using RC::Unreal::UObject;
        namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

        spin::Spinlock g_lock;
        Status g_status;                 // guarded by g_lock
        std::vector<std::string> g_text; // guarded by g_lock; game thread fills, loop drains
        std::atomic<bool> g_requested{false};
        std::atomic<bool> g_ready{false};

        // Gathering side (game thread): appends to a local vector only - no logging, no
        // file access, no locale-touching formatting.
        struct Out
        {
            std::vector<std::string> lines;

            void add(std::string s)
            {
                if (lines.size() < 4096)
                {
                    lines.push_back(std::move(s));
                }
            }
        };

        std::string name_of(UObject* obj)
        {
            if (obj == nullptr || !mem::readable(obj, 0x40))
            {
                return "(unreadable)";
            }
            return uer::narrow_ascii(obj->GetName());
        }

        std::string class_of(UObject* obj)
        {
            if (obj == nullptr || !mem::readable(obj, 0x40))
            {
                return "(unreadable)";
            }
            RC::Unreal::UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr || !mem::readable(cls, 0x40))
            {
                return "(no class)";
            }
            return uer::narrow_ascii(static_cast<UObject*>(cls)->GetName());
        }

        // Prints the reflected parameter list, and a distinct line when the function
        // does not exist, so a wrong name reads differently from a wrong shape.
        void dump_function(Out& o, uer::FuncCache& funcs, UObject* obj, const char* obj_label,
                           const wchar_t* fname)
        {
            const std::string label = std::string{obj_label} + "::" + uer::narrow_ascii(fname);
            if (obj == nullptr)
            {
                o.add("    " + label + "  -> receiver not available");
                return;
            }
            RC::Unreal::UFunction* fn = funcs.get(obj, fname);
            if (fn == nullptr)
            {
                o.add("    " + label + "  -> NO SUCH UFunction on this class chain");
                return;
            }
            const std::vector<uer::ParamInfo> params = uer::func_params(fn);
            std::string line = "    " + label + "  block " + std::to_string(uer::func_param_size(fn)) +
                               " B, " + std::to_string(params.size()) + " param(s):";
            for (const uer::ParamInfo& p : params)
            {
                line += " " + uer::narrow_ascii(p.name) + "@" + std::to_string(p.offset) + "/" +
                        std::to_string(p.size);
            }
            o.add(line);
        }

        UObject* find_first(const wchar_t* class_name)
        {
            std::vector<UObject*> objs;
            UObjectGlobals::FindAllOf(class_name, objs);
            for (UObject* obj : objs)
            {
                if (obj != nullptr && mem::readable(obj, 0x40))
                {
                    return obj;
                }
            }
            return nullptr;
        }

        // Every property of a class chain, with offset and size.
        void dump_properties(Out& o, uer::LayoutCache& layouts, UObject* obj, const char* label)
        {
            const uer::ClassLayout* layout = layouts.get(obj);
            if (layout == nullptr)
            {
                o.add(std::string{"    "} + label + ": no class layout");
                return;
            }
            o.add("    " + std::string{label} + ": " + std::to_string(layout->props.size()) +
                  " propert(ies) over " + std::to_string(layout->walked) + " struct(s)");
            for (const auto& kv : layout->props)
            {
                o.add("      " + uer::narrow_ascii(kv.first) + " @" +
                      std::to_string(kv.second.offset) + " /" + std::to_string(kv.second.size));
            }
        }

        void dump_string_array(Out& o, const uer::ClassLayout* layout, const void* obj,
                               const wchar_t* prop)
        {
            std::vector<std::wstring> v;
            if (!uer::read_str_array_prop(layout, obj, prop, v))
            {
                o.add("      " + uer::narrow_ascii(prop) + " = <not a readable TArray<FString>>");
                return;
            }
            std::string line = "      " + uer::narrow_ascii(prop) + " [" + std::to_string(v.size()) + "] =";
            for (const std::wstring& s : v)
            {
                line += " " + uer::narrow_ascii(s);
                if (line.size() > 900)
                {
                    o.add(line);
                    line = "        ...";
                }
            }
            o.add(line);
        }

        void dump_string(Out& o, const uer::ClassLayout* layout, const void* obj, const wchar_t* prop)
        {
            std::wstring s;
            if (!uer::read_fstring_prop(layout, obj, prop, s))
            {
                o.add("      " + uer::narrow_ascii(prop) + " = <not a readable FString>");
                return;
            }
            o.add("      " + uer::narrow_ascii(prop) + " = \"" + uer::narrow_ascii(s) + "\"");
        }

        void gather(const void* world)
        {
            Out o;
            uer::LayoutCache layouts;
            uer::FuncCache funcs;

            o.add("WuchangMinimap recon dump - context/saveslot-and-teleport-research.md section 3");
            o.add("Read-only: reflection lookups and raw property reads. Nothing was called.");
            o.add("");

            //--------------------------------------------------------------------------
            // 1. the game mode and its components
            //--------------------------------------------------------------------------
            o.add("[1] GAME MODE AND ITS COMPONENTS");
            UObject* gm = nullptr;
            if (world != nullptr && mem::readable(world, 0x40))
            {
                const uer::ClassLayout* wl = layouts.get(static_cast<UObject*>(const_cast<void*>(world)));
                o.add("    world " + name_of(static_cast<UObject*>(const_cast<void*>(world))) + " (" +
                      class_of(static_cast<UObject*>(const_cast<void*>(world))) + ")");
                for (const wchar_t* prop : {L"AuthorityGameMode", L"GameMode"})
                {
                    UObject* cand = uer::read_object_prop(wl, world, prop);
                    if (cand != nullptr)
                    {
                        gm = cand;
                        o.add("    " + uer::narrow_ascii(prop) + " -> " + name_of(gm) + " (" +
                              class_of(gm) + ")");
                        break;
                    }
                }
            }
            if (gm == nullptr)
            {
                // By class name, so the rest of the dump still happens when the world
                // read fails.
                for (const wchar_t* cls : {L"DCSGameMode_Net_C", L"DCSGameMode_C", L"GameModeBase"})
                {
                    gm = find_first(cls);
                    if (gm != nullptr)
                    {
                        o.add("    world read failed; found by class " + uer::narrow_ascii(cls) + " -> " +
                              name_of(gm));
                        break;
                    }
                }
            }
            if (gm == nullptr)
            {
                o.add("    NO GAME MODE FOUND - items 1 and 2 below fall back to a class search");
            }
            else
            {
                const uer::ClassLayout* gl = layouts.get(gm);
                // AActor's component lists: both UPROPERTYs, raw-read as
                // TArray<UActorComponent*>.
                for (const wchar_t* prop : {L"BlueprintCreatedComponents", L"InstanceComponents"})
                {
                    const uer::Prop* p = uer::find_prop(gl, prop);
                    if (p == nullptr)
                    {
                        o.add("    " + uer::narrow_ascii(prop) + ": not a property of this class");
                        continue;
                    }
                    struct ArrHdr
                    {
                        void* data;
                        std::int32_t num;
                        std::int32_t max;
                    } hdr{};
                    if (!mem::read_at(gm, p->offset, hdr) || hdr.num < 0 || hdr.num > 512 ||
                        !mem::plausible_ptr(hdr.data))
                    {
                        o.add("    " + uer::narrow_ascii(prop) + ": unreadable array header");
                        continue;
                    }
                    o.add("    " + uer::narrow_ascii(prop) + " [" + std::to_string(hdr.num) + "]:");
                    for (std::int32_t i = 0; i < hdr.num; ++i)
                    {
                        void* raw = nullptr;
                        if (!mem::read_at(hdr.data, static_cast<std::size_t>(i) * sizeof(void*), raw) ||
                            !mem::plausible_ptr(raw))
                        {
                            continue;
                        }
                        auto* comp = static_cast<UObject*>(raw);
                        o.add("      " + name_of(comp) + "  (" + class_of(comp) + ")");
                    }
                }
            }
            o.add("");

            //--------------------------------------------------------------------------
            // 2. RebornManagerComponent_C
            //--------------------------------------------------------------------------
            o.add("[2] RebornManagerComponent - every property, then the firepoint state");
            UObject* mgr = find_first(L"RebornManagerComponent_C");
            if (mgr == nullptr)
            {
                mgr = find_first(L"RebornManagerComponent");
            }
            if (mgr == nullptr)
            {
                o.add("    NOT FOUND by either class name");
            }
            else
            {
                o.add("    instance " + name_of(mgr) + " (" + class_of(mgr) + ")");
                dump_properties(o, layouts, mgr, "properties");
                const uer::ClassLayout* ml = layouts.get(mgr);
                for (const wchar_t* prop : {L"UnlockedFirepoints", L"FirepointsEverUnlocked",
                                            L"DeactivatedFirepoints"})
                {
                    dump_string_array(o, ml, mgr, prop);
                }
                for (const wchar_t* prop : {L"rebornFirePointID", L"default_firepoint",
                                            L"rebornLevelChapter", L"rebornLevelMap",
                                            L"UnlockedFirepointsKey", L"EverUnlockedFirepointsKey",
                                            L"DeactivatedFirepointsKey"})
                {
                    dump_string(o, ml, mgr, prop);
                }
            }
            o.add("");

            //--------------------------------------------------------------------------
            // 3. the function signatures
            //--------------------------------------------------------------------------
            o.add("[3] REFLECTED FUNCTION SIGNATURES (name@offset/size per parameter)");
            UObject* saver = find_first(L"GameSaveExecutor");
            o.add("    GameSaveExecutor instance: " + (saver != nullptr ? name_of(saver) : "NOT FOUND"));
            for (const wchar_t* f : {L"Get Save Slot Value", L"GetSaveSlotValue", L"GetSaveSlotStrValue",
                                     L"GetSlotStrArray", L"GetSlotArrayValue", L"UpdateSaveSlotValue"})
            {
                dump_function(o, funcs, saver, "GameSaveExecutor", f);
            }
            UObject* lib = find_first(L"PlayerModelLibrary_C");
            o.add("    PlayerModelLibrary_C object: " + (lib != nullptr ? name_of(lib) : "NOT FOUND"));
            for (const wchar_t* f : {L"PlayerChuanSongFirePoint", L"IsFirePointUnlock",
                                     L"GetSavedFirepoints", L"isFirepointDeactive"})
            {
                dump_function(o, funcs, lib, "PlayerModelLibrary_C", f);
            }
            for (const wchar_t* f : {L"Is Firepoint Unlocked", L"IsFirepointUnlocked",
                                     L"GetCurrentFirepoint", L"SetRebornInfoFromFirePointID",
                                     L"GetSavedFirepoints", L"LoadFirepointInfoFromTable"})
            {
                dump_function(o, funcs, mgr, "RebornManagerComponent", f);
            }
            UObject* fire = find_first(L"BP_RebornFire_C");
            o.add("    BP_RebornFire_C instance: " + (fire != nullptr ? name_of(fire) : "NOT FOUND"));
            for (const wchar_t* f : {L"ChuanSong", L"GetFirePointID"})
            {
                dump_function(o, funcs, fire, "BP_RebornFire_C", f);
            }
            o.add("");

            //--------------------------------------------------------------------------
            // 4. the save-slot fallback strings
            //--------------------------------------------------------------------------
            o.add("[4] SAVE-SLOT FALLBACK STRINGS");
            for (const wchar_t* cls : {L"Impl_GameSettingsSaver_C", L"GameSettingsSaver_C",
                                       L"GameSaveExecutorNative"})
            {
                UObject* obj = find_first(cls);
                if (obj == nullptr)
                {
                    o.add("    " + uer::narrow_ascii(cls) + ": NOT FOUND");
                    continue;
                }
                o.add("    " + uer::narrow_ascii(cls) + " -> " + name_of(obj));
                const uer::ClassLayout* l = layouts.get(obj);
                for (const wchar_t* prop : {L"TickCountSavPath", L"UserName", L"NewGamePlusSavingSlot",
                                            L"LCachedMapName", L"SavePath", L"PlayerName"})
                {
                    if (uer::find_prop(l, prop) != nullptr)
                    {
                        dump_string(o, l, obj, prop);
                    }
                }
            }
            o.add("");
            o.add("[5] WHAT THE MOD CURRENTLY BELIEVES");
            const shr::State st = shr::state();
            o.add(std::string{"    shrines: "} + (st.valid ? "read" : "NOT read") + " via \"" +
                  st.route + "\"; " + std::to_string(st.unlocked) + " unlocked, at \"" + st.current +
                  "\"");
            o.add("    end of dump");

            {
                spin::SpinGuard guard(g_lock);
                g_text = std::move(o.lines);
                g_status.lines = static_cast<int>(g_text.size());
                g_status.pending = false;
            }
            g_ready.store(true, std::memory_order_release);
        }
    } // namespace

    void request()
    {
        {
            spin::SpinGuard guard(g_lock);
            g_status.pending = true;
            g_status.error[0] = '\0';
        }
        g_requested.store(true, std::memory_order_release);
    }

    Status status()
    {
        spin::SpinGuard guard(g_lock);
        return g_status;
    }

    void game_thread_pump(const void* world)
    {
        if (!g_requested.exchange(false, std::memory_order_acquire))
        {
            return;
        }
        gather(world);
    }

    void on_update()
    {
        if (!g_ready.exchange(false, std::memory_order_acquire))
        {
            return;
        }
        std::vector<std::string> lines;
        {
            spin::SpinGuard guard(g_lock);
            lines.swap(g_text);
        }
        SYSTEMTIME t{};
        ::GetLocalTime(&t);
        wchar_t stamp[32]{};
        ::swprintf_s(stamp, L"%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                     t.wSecond);
        const std::wstring path = mm::mod_dir() + L"\\wuchang_minimap_recon_" + stamp + L".txt";

        std::string blob;
        for (const std::string& line : lines)
        {
            blob += line;
            blob += "\r\n";
        }
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            spin::SpinGuard guard(g_lock);
            ::strncpy_s(g_status.error, sizeof(g_status.error), "could not create the dump file",
                        _TRUNCATE);
            mm::logf(L"recon: could not create {} (error {})", path,
                     static_cast<unsigned>(::GetLastError()));
            return;
        }
        DWORD written = 0;
        ::WriteFile(h, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr);
        ::CloseHandle(h);
        {
            spin::SpinGuard guard(g_lock);
            const std::string narrow = uer::narrow_ascii(path);
            ::strncpy_s(g_status.file, sizeof(g_status.file), narrow.c_str(), _TRUNCATE);
        }
        mm::logf(L"recon: wrote {} ({} line(s), {} bytes) - send this file back", path, lines.size(),
                 blob.size());
    }
} // namespace recon
