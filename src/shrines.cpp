//
// shrines - the runtime unlock state. See shrines.hpp for the shape of the data.
//

#include "shrines.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mmstate.hpp"
#include "shrines_db.hpp"
#include "ue_min.hpp"
#include "spinlock.hpp"
#include "uereflect.hpp"

namespace shr
{
    namespace
    {
        using RC::Unreal::UObject;
        namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

        spin::Spinlock g_lock;
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

        // Both capitalisations of the leading letter are tried: a blueprint variable's
        // reflected name is whatever the designer typed.
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

        void set_unresolved(const char* why)
        {
            spin::SpinGuard guard(g_lock);
            g_state.valid = false;
            copy_into(g_state.route, sizeof(g_state.route), why);
        }

        //==============================================================================
        // The offline table
        //==============================================================================

        std::atomic<const std::vector<shdb::Shrine>*> g_table{nullptr};
        TableInfo g_table_info; // guarded by g_lock
    } // namespace

    State state()
    {
        spin::SpinGuard guard(g_lock);
        return g_state;
    }

    bool is_unlocked(const char* id)
    {
        if (id == nullptr || id[0] == '\0')
        {
            return false;
        }
        spin::SpinGuard guard(g_lock);
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
        spin::SpinGuard guard(g_lock);
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
                spin::SpinGuard guard(g_lock);
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
                spin::SpinGuard guard(g_lock);
                g_table_info = info;
            }
            mm::logf(L"shrines: {} rejected - {}", path, widen(rep.error));
            return;
        }
        info.rows = rep.rows;
        info.shrines = rep.shrines;
        info.named = rep.named;
        {
            spin::SpinGuard guard(g_lock);
            g_table_info = info;
        }
        // Leaked on reload: the render thread may be walking the old vector.
        g_table.store(list.release(), std::memory_order_release);
        mm::logf(L"shrines: table loaded - {} row(s), {} shrine(s), {} named", rep.rows, rep.shrines,
                 rep.named);
    }

    void drop_caches()
    {
        g_layouts.clear();
        g_manager.reset();
        g_last_poll = 0;
        g_last_find = 0;
        g_logged_props = false;
        set_unresolved("world changed - not read yet");
    }

    void game_thread_pump(std::uint64_t now)
    {
        // 1 Hz: the list changes only when the player lights a shrine.
        if (g_last_poll != 0 && now - g_last_poll < 1000)
        {
            return;
        }
        g_last_poll = now;

        if (g_manager.empty() || !uer::alive(g_manager))
        {
            g_manager.reset();
            // FindAllOf walks the whole object array, so it runs only while there is
            // no component at all, throttled to 5 s.
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
                // Name what IS there: a wrong spelling is the likeliest cause.
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
            spin::SpinGuard guard(g_lock);
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
            // UnlockedFirepoints also carries the `bossdoor_*` / `Task*` pseudo-points
            // that markers/shrines.json marks `"shrine": false`.
            std::wstring pseudo;
            int pseudo_n = 0;
            for (int i = 0; i < next.id_count; ++i)
            {
                if (shdb::is_shrine_id(table(), next.ids[i]))
                {
                    continue;
                }
                ++pseudo_n;
                if (!pseudo.empty())
                {
                    pseudo += L", ";
                }
                pseudo += widen(std::string{next.ids[i]});
            }
            mm::logf(L"shrines: {} unlocked non-shrine point(s){}{}", pseudo_n,
                     pseudo_n != 0 ? L": " : L"", pseudo);
        }
    }
} // namespace shr
