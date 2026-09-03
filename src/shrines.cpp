//
// shrines - the runtime unlock state. See shrines.hpp for why it is a list of strings
// and not a per-actor flag.
//

#include "shrines.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "mem.hpp"
#include "mmstate.hpp"
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
