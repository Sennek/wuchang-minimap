//
// shrines - the runtime unlock state. See shrines.hpp for why it is a list of strings
// and not a per-actor flag.
//

#include "shrines.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mem.hpp"
#include "mmstate.hpp"
#include "shrines_db.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

namespace shr
{
    namespace
    {
        using RC::Unreal::UObject;
        namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

        class Spin
        {
          public:
            void lock() noexcept
            {
                while (flag_.test_and_set(std::memory_order_acquire))
                {
                    ::YieldProcessor();
                }
            }
            void unlock() noexcept
            {
                flag_.clear(std::memory_order_release);
            }

          private:
            std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
        };

        class Guard
        {
          public:
            explicit Guard(Spin& s) noexcept : s_(s)
            {
                s_.lock();
            }
            ~Guard()
            {
                s_.unlock();
            }
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;

          private:
            Spin& s_;
        };

        Spin g_lock;
        State g_state; // guarded by g_lock

        uer::LayoutCache g_layouts;
        uer::ObjRef g_manager;
        std::uint64_t g_last_poll = 0;
        std::uint64_t g_last_find = 0;
        bool g_logged_props = false;

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

        void copy_into(char* dst, std::size_t cap, const std::string& src)
        {
            ::strncpy_s(dst, cap, src.c_str(), _TRUNCATE);
        }

        // The property names the research read out of the cooked package. Both
        // capitalisations of the leading letter are tried: a blueprint variable's
        // reflected name is whatever the designer typed, and the document's rendering of
        // it is not proof.
        const wchar_t* const kUnlockedNames[] = {L"UnlockedFirepoints", L"unlockedFirepoints"};
        const wchar_t* const kEverNames[] = {L"FirepointsEverUnlocked", L"firepointsEverUnlocked",
                                             L"AllFirepointsEverUnlocked"};
        const wchar_t* const kDeactivatedNames[] = {L"DeactivatedFirepoints", L"deactivatedFirepoints"};
        const wchar_t* const kCurrentNames[] = {L"rebornFirePointID", L"RebornFirePointID"};

        // Reads the first of `names` that yields a TArray<FString>. Returns the name that
        // answered, or nullptr.
        const wchar_t* read_first_array(const uer::ClassLayout* layout, const void* obj,
                                        const wchar_t* const* names, std::size_t count,
                                        std::vector<std::wstring>& out)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                if (uer::read_str_array_prop(layout, obj, names[i], out))
                {
                    return names[i];
                }
            }
            return nullptr;
        }

        bool find_manager()
        {
            std::vector<UObject*> objs;
            UObjectGlobals::FindAllOf(L"RebornManagerComponent_C", objs);
            if (objs.empty())
            {
                // The component might be reachable only by its native base; try that too.
                UObjectGlobals::FindAllOf(L"RebornManagerComponent", objs);
            }
            for (UObject* obj : objs)
            {
                if (obj != nullptr && uer::capture(obj, g_manager))
                {
                    return true;
                }
            }
            return false;
        }

        //==============================================================================
        // The offline table (loop thread loads, everyone reads)
        //==============================================================================

        std::atomic<const std::vector<shdb::Shrine>*> g_table{nullptr};
        TableInfo g_table_info; // guarded by g_lock

        //==============================================================================
        // Fast travel
        //==============================================================================

        TravelState g_travel; // guarded by g_lock
        std::atomic<bool> g_travel_pending{false};

        uer::FuncCache g_travel_funcs;
        uer::ObjRef g_lib;    // PlayerModelLibrary_C (its CDO)
        uer::ObjRef g_shrine; // any resident BP_RebornFire_C, the fallback receiver

        void set_travel(Travel phase, const char* id, const std::string& note)
        {
            Guard guard(g_lock);
            g_travel.phase = phase;
            if (id != nullptr)
            {
                copy_into(g_travel.id, sizeof(g_travel.id), id);
            }
            copy_into(g_travel.note, sizeof(g_travel.note), note);
        }

        // A function-library static lives on the CDO, and uer::capture deliberately
        // REFUSES a CDO (latching onto one is how a reader ends up reading an archetype
        // instead of an instance - lessons.md). A CDO is also the one object that cannot
        // die while its class is loaded, so it is remembered as a plain pointer plus its
        // class, re-checked before every use.
        UObject* g_lib_cdo = nullptr;
        RC::Unreal::UClass* g_lib_cdo_class = nullptr;

        bool find_library()
        {
            if (g_lib_cdo != nullptr && mem::readable(g_lib_cdo, 0x40) &&
                g_lib_cdo->GetClassPrivate() == g_lib_cdo_class)
            {
                return true;
            }
            g_lib_cdo = nullptr;
            g_lib_cdo_class = nullptr;
            std::vector<UObject*> objs;
            UObjectGlobals::FindAllOf(L"PlayerModelLibrary_C", objs);
            for (UObject* obj : objs)
            {
                if (obj == nullptr || !mem::readable(obj, 0x40))
                {
                    continue;
                }
                RC::Unreal::UClass* cls = obj->GetClassPrivate();
                if (cls == nullptr)
                {
                    continue;
                }
                g_lib_cdo = obj;
                g_lib_cdo_class = cls;
                return true;
            }
            return false;
        }

        // The FString parameter block we hand to ProcessEvent. UE's ProcessEvent
        // destroys the frame's LOCALS after the call but explicitly not its PARAMETERS
        // (`if (!Destruct->HasAnyPropertyFlags(CPF_Parm))`), so the caller owns this
        // memory and the engine's allocator is never handed a pointer of ours. The
        // buffer is static so it outlives the call under any tail behaviour.
        struct FStringParam
        {
            const wchar_t* data = nullptr;
            std::int32_t num = 0;
            std::int32_t max = 0;
        };

        wchar_t g_travel_id_w[shdb::kMaxIdLen + 1]{};

        // The self-check. Research section 2.2 predicts a single FString in, so the
        // reflected parameter list must be one or two 16-byte properties and the
        // parameter block must be big enough to hold them. Anything else means the
        // offline reading of the call graph was wrong about this function and, per
        // lessons.md, the call is NOT made.
        struct Sig
        {
            bool ok = false;
            int params = 0;
            int block = 0;
            std::string detail;
        };

        Sig check_travel_signature(UObject* obj, const wchar_t* fname)
        {
            Sig sig{};
            RC::Unreal::UFunction* fn = g_travel_funcs.get(obj, fname);
            if (fn == nullptr)
            {
                sig.detail = "no such UFunction";
                return sig;
            }
            const std::vector<uer::ParamInfo> params = uer::func_params(fn);
            sig.params = static_cast<int>(params.size());
            sig.block = uer::func_param_size(fn);
            bool all_16 = !params.empty();
            for (const uer::ParamInfo& p : params)
            {
                if (!sig.detail.empty())
                {
                    sig.detail += ", ";
                }
                sig.detail += uer::narrow_ascii(p.name) + "@" + std::to_string(p.offset) + "/" +
                              std::to_string(p.size);
                if (p.size != 16)
                {
                    all_16 = false;
                }
            }
            sig.ok = all_16 && sig.params >= 1 && sig.params <= 2 && sig.block >= 16 &&
                     params[0].offset == 0;
            return sig;
        }

        bool issue_travel_call(UObject* obj, const wchar_t* fname, const Sig& sig, const std::string& id,
                               std::string& note)
        {
            RC::Unreal::UFunction* fn = g_travel_funcs.get(obj, fname);
            if (fn == nullptr)
            {
                note = "the function went away";
                return false;
            }
            const std::size_t n = id.size() < shdb::kMaxIdLen ? id.size() : shdb::kMaxIdLen;
            for (std::size_t i = 0; i < n; ++i)
            {
                g_travel_id_w[i] = static_cast<wchar_t>(static_cast<unsigned char>(id[i]));
            }
            g_travel_id_w[n] = L'\0';
            std::vector<std::uint8_t> block(static_cast<std::size_t>(sig.block) + 16u, 0u);
            FStringParam p{};
            p.data = g_travel_id_w;
            p.num = static_cast<std::int32_t>(n + 1); // UE counts the terminating NUL
            p.max = p.num;
            std::memcpy(block.data(), &p, sizeof(p));
            if (!mem::guarded_call(&uer::process_event_trampoline, obj, fn, block.data()))
            {
                note = "ProcessEvent faulted";
                return false;
            }
            return true;
        }

        void run_travel(const std::string& id)
        {
            const mm::Config& cfg = mm::cfg_cached();
            if (!cfg.fast_travel_enabled)
            {
                set_travel(Travel::Refused, id.c_str(),
                           "fast_travel_enabled = 0 - turn it on in the Advanced settings");
                return;
            }
            // Only an UNLOCKED id: travelling to a locked one is untested and is exactly
            // the kind of call that can wedge the streaming state (research 2.2).
            if (!is_unlocked(id.c_str()))
            {
                set_travel(Travel::Refused, id.c_str(),
                           "that shrine is not in the save's unlocked list");
                return;
            }

            struct Route
            {
                const wchar_t* fname;
                bool library;
            };
            // The library facade first (it needs no shrine actor to be loaded, and only
            // ~3 of the 57 shrines ever are), then the menu's own path as the fallback.
            const Route routes[] = {
                {L"PlayerChuanSongFirePoint", true},
                {L"ChuanSong", false},
            };
            std::string tried;
            for (const Route& r : routes)
            {
                UObject* obj = nullptr;
                if (r.library)
                {
                    if (!find_library())
                    {
                        tried += "PlayerModelLibrary_C not loaded; ";
                        continue;
                    }
                    obj = g_lib_cdo;
                }
                else
                {
                    if (g_shrine.empty() || !uer::alive(g_shrine))
                    {
                        g_shrine.reset();
                        std::vector<UObject*> objs;
                        UObjectGlobals::FindAllOf(L"BP_RebornFire_C", objs);
                        for (UObject* o : objs)
                        {
                            if (o != nullptr && uer::capture(o, g_shrine))
                            {
                                break;
                            }
                        }
                    }
                    if (g_shrine.empty())
                    {
                        tried += "no resident BP_RebornFire_C; ";
                        continue;
                    }
                    obj = g_shrine.obj;
                }
                const Sig sig = check_travel_signature(obj, r.fname);
                mm::logf(L"fast travel: '{}' - {} param(s), block {} B: {} => {}", r.fname, sig.params,
                         sig.block, widen(sig.detail.empty() ? std::string{"(none)"} : sig.detail),
                         sig.ok ? L"matches the predicted signature" : L"REFUSED");
                if (!sig.ok)
                {
                    tried += uer::narrow_ascii(r.fname) + ": " + sig.detail + "; ";
                    continue;
                }
                std::string note;
                if (!issue_travel_call(obj, r.fname, sig, id, note))
                {
                    tried += uer::narrow_ascii(r.fname) + ": " + note + "; ";
                    continue;
                }
                set_travel(Travel::Done, id.c_str(), "called " + uer::narrow_ascii(r.fname));
                mm::logf(L"fast travel: called {}('{}')", r.fname, widen(id));
                return;
            }
            set_travel(Travel::Refused, id.c_str(), tried.empty() ? "no route available" : tried);
            mm::logf(L"fast travel to '{}' REFUSED: {}", widen(id), widen(tried));
        }

        void set_unresolved(const char* why)
        {
            Guard guard(g_lock);
            g_state.valid = false;
            copy_into(g_state.route, sizeof(g_state.route), why);
        }
    } // namespace

    State state()
    {
        Guard guard(g_lock);
        return g_state;
    }

    bool is_unlocked(const char* id)
    {
        if (id == nullptr || id[0] == '\0')
        {
            return false;
        }
        Guard guard(g_lock);
        if (!g_state.valid)
        {
            return false;
        }
        for (int i = 0; i < g_state.id_count; ++i)
        {
            if (::_stricmp(g_state.ids[i], id) == 0)
            {
                return true;
            }
        }
        return false;
    }

    const std::vector<shdb::Shrine>* table()
    {
        return g_table.load(std::memory_order_acquire);
    }

    TableInfo table_info()
    {
        Guard guard(g_lock);
        return g_table_info;
    }

    void load_table()
    {
        const std::wstring path = mm::mod_dir() + L"\\markers\\shrines.json";
        std::string text;
        TableInfo info{};
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            copy_into(info.error, sizeof(info.error), "markers\\shrines.json not found");
            {
                Guard guard(g_lock);
                g_table_info = info;
            }
            mm::logf(L"shrines: {} does not exist - the shrine list will be empty "
                     L"(run tools/markers/extract_shrines.py)",
                     path);
            return;
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(h, &size) != 0 && size.QuadPart > 0 && size.QuadPart < (8 << 20))
        {
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            if (::ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) == 0 ||
                read != text.size())
            {
                text.clear();
            }
        }
        ::CloseHandle(h);

        auto list = std::make_unique<std::vector<shdb::Shrine>>();
        shdb::Report rep{};
        if (!shdb::parse(text, *list, rep))
        {
            copy_into(info.error, sizeof(info.error), rep.error);
            {
                Guard guard(g_lock);
                g_table_info = info;
            }
            mm::logf(L"shrines: {} rejected - {}", path, widen(rep.error));
            return;
        }
        info.rows = rep.rows;
        info.shrines = rep.shrines;
        info.named = rep.named;
        {
            Guard guard(g_lock);
            g_table_info = info;
        }
        // Deliberately leaked on a reload, exactly like the marker DB: the render thread
        // may be walking the old vector and there is no safe point at which to free it.
        g_table.store(list.release(), std::memory_order_release);
        mm::logf(L"shrines: table loaded - {} row(s), {} shrine(s), {} named", rep.rows, rep.shrines,
                 rep.named);
    }

    void request_travel(const char* id)
    {
        if (id == nullptr || id[0] == '\0')
        {
            return;
        }
        set_travel(Travel::Requested, id, "queued for the game thread");
        g_travel_pending.store(true, std::memory_order_release);
    }

    TravelState travel_state()
    {
        Guard guard(g_lock);
        return g_travel;
    }

    void clear_travel()
    {
        Guard guard(g_lock);
        g_travel = TravelState{};
    }

    void drop_caches()
    {
        g_layouts.clear();
        g_manager.reset();
        g_travel_funcs.clear();
        g_lib.reset();
        g_shrine.reset();
        g_lib_cdo = nullptr;
        g_lib_cdo_class = nullptr;
        g_last_poll = 0;
        g_last_find = 0;
        g_logged_props = false;
        set_unresolved("world changed - not read yet");
    }

    void game_thread_pump(std::uint64_t now)
    {
        // A travel request runs before the poll and outside its 1 Hz gate: the player
        // pressed a button and is waiting for a loading screen.
        if (g_travel_pending.exchange(false, std::memory_order_acquire))
        {
            std::string id;
            {
                Guard guard(g_lock);
                id = g_travel.id;
                g_travel.phase = Travel::InFlight;
            }
            run_travel(id);
        }

        // 1 Hz: the list changes only when the player lights a shrine.
        if (g_last_poll != 0 && now - g_last_poll < 1000)
        {
            return;
        }
        g_last_poll = now;

        if (g_manager.empty() || !uer::alive(g_manager))
        {
            g_manager.reset();
            // FindAllOf is a whole-object-array walk, so it is throttled hard and only
            // runs while we have no component at all.
            if (g_last_find != 0 && now - g_last_find < 5000)
            {
                return;
            }
            g_last_find = now;
            if (!find_manager())
            {
                set_unresolved("no RebornManagerComponent instance");
                return;
            }
            mm::logf(L"shrines: RebornManagerComponent found");
        }

        const uer::ClassLayout* layout = g_layouts.get(g_manager.obj);
        std::vector<std::wstring> unlocked;
        std::vector<std::wstring> ever;
        std::vector<std::wstring> deact;
        const wchar_t* n_unlocked =
            read_first_array(layout, g_manager.obj, kUnlockedNames,
                             sizeof(kUnlockedNames) / sizeof(kUnlockedNames[0]), unlocked);
        if (n_unlocked == nullptr)
        {
            set_unresolved("UnlockedFirepoints not readable");
            if (!g_logged_props)
            {
                g_logged_props = true;
                // Name what IS there: a wrong spelling is the likeliest cause and the
                // panel cannot show a property list.
                std::string names;
                if (layout != nullptr)
                {
                    int shown = 0;
                    for (const auto& kv : layout->props)
                    {
                        if (kv.first.find(L"irepoint") == std::wstring::npos)
                        {
                            continue;
                        }
                        if (!names.empty())
                        {
                            names += ", ";
                        }
                        names += uer::narrow_ascii(kv.first);
                        if (++shown >= 12)
                        {
                            break;
                        }
                    }
                }
                mm::logf(L"shrines: UnlockedFirepoints is not a readable TArray<FString> on this "
                         L"component. Properties matching 'irepoint': {}",
                         widen(names.empty() ? std::string{"(none)"} : names));
            }
            return;
        }
        const wchar_t* n_ever = read_first_array(layout, g_manager.obj, kEverNames,
                                                 sizeof(kEverNames) / sizeof(kEverNames[0]), ever);
        const wchar_t* n_deact =
            read_first_array(layout, g_manager.obj, kDeactivatedNames,
                             sizeof(kDeactivatedNames) / sizeof(kDeactivatedNames[0]), deact);
        std::wstring current;
        for (const wchar_t* name : kCurrentNames)
        {
            if (uer::read_fstring_prop(layout, g_manager.obj, name, current))
            {
                break;
            }
        }

        State next{};
        next.valid = true;
        next.unlocked = static_cast<int>(unlocked.size());
        next.ever = static_cast<int>(ever.size());
        next.deactivated = static_cast<int>(deact.size());
        next.truncated = next.unlocked > kMaxIds;
        copy_into(next.current, sizeof(next.current), uer::narrow_ascii(current));
        const int n = next.unlocked < kMaxIds ? next.unlocked : kMaxIds;
        for (int i = 0; i < n; ++i)
        {
            copy_into(next.ids[i], kIdLen, uer::narrow_ascii(unlocked[static_cast<std::size_t>(i)]));
        }
        next.id_count = n;
        copy_into(next.route, sizeof(next.route), uer::narrow_ascii(n_unlocked));

        bool changed = false;
        {
            Guard guard(g_lock);
            changed = !g_state.valid || g_state.unlocked != next.unlocked ||
                      g_state.deactivated != next.deactivated ||
                      ::strcmp(g_state.current, next.current) != 0;
            g_state = next;
        }
        if (changed)
        {
            mm::logf(L"shrines: {} unlocked, {} ever, {} deactivated; at '{}' (via {}{}{}{})",
                     next.unlocked, next.ever, next.deactivated,
                     widen(std::string{next.current}), widen(uer::narrow_ascii(n_unlocked)),
                     n_ever == nullptr ? L" - no ever-list" : L"",
                     n_deact == nullptr ? L" - no deactivated-list" : L"",
                     next.truncated ? L" - TRUNCATED" : L"");
        }
    }
} // namespace shr
