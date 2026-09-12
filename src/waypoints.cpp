//
// waypoints - the player's own pins: the set the render thread reads, the file they
// live in, and the save slot that file belongs to.
//
// LOOP THREAD for everything that touches the disk; `waypoints()` and the four editors
// are called from any thread and take `g_wp_lock` for the length of a copy. The set is
// small and fixed (`mv::kMaxWaypoints`), so the lock is never held across I/O.
//
// The file is `wuchang_minimap_waypoint[_<slot>].txt` beside the mod's other state.
// A save slot arriving late is why there is a migration at all: waypoints dropped
// before the slot was known live in the shared file, and are seeded into the slot's own
// the first time it is identified.
//

#include "mmstate.hpp"

#include "atomicfile.hpp"
#include "saveslot.hpp"
#include "spinlock.hpp"

#include <Windows.h>

#include <format>
#include <string>
#include <string_view>

namespace mm
{
    namespace
    {
        spin::Spinlock g_wp_lock;
        mv::WaypointSet g_wp{};
    } // namespace

    // Raised by every editor below; the loop thread consumes it, debounced, and writes.
    std::atomic<bool> g_waypoint_dirty{false};

    mv::WaypointSet waypoints()
    {
        spin::SpinGuard guard(g_wp_lock);
        return g_wp;
    }

    void set_waypoints(const mv::WaypointSet& set)
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            g_wp = set;
            if (g_wp.count > mv::kMaxWaypoints)
            {
                g_wp.count = mv::kMaxWaypoints;
            }
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    bool add_waypoint(const mv::Waypoint& wp)
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            if (g_wp.count >= mv::kMaxWaypoints)
            {
                return false;
            }
            g_wp.items[g_wp.count] = wp;
            g_wp.items[g_wp.count].set = true;
            ++g_wp.count;
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
        return true;
    }

    void remove_waypoint(std::size_t index)
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            if (index >= g_wp.count)
            {
                return;
            }
            for (std::size_t i = index + 1; i < g_wp.count; ++i)
            {
                g_wp.items[i - 1] = g_wp.items[i];
            }
            --g_wp.count;
            g_wp.items[g_wp.count] = mv::Waypoint{};
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    void clear_waypoints()
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            g_wp = mv::WaypointSet{};
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    namespace
    {
        std::string g_wp_key;        // "" = the shared file
        bool g_wp_key_valid = false; // has a key ever been taken from slotid?

        std::wstring waypoint_path_for(const std::string& key)
        {
            const std::string name = slotid::waypoint_filename(key);
            std::wstring wide;
            wide.reserve(name.size());
            for (char c : name)
            {
                wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return state_dir() + L"\\" + wide;
        }

        bool wp_file_exists(const std::wstring& path)
        {
            return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // The waypoint half of the found tracker's legacy reconcile: builds before the
        // save key became the slot name alone also wrote `<steam account id>_<slot>`
        // waypoint files. Each one's waypoints are added to the canonical set (places
        // already in it are not duplicated, and the set stops at mv::kMaxWaypoints) and
        // the legacy file is removed. Loop thread.
        void reconcile_legacy_waypoints(const std::string& key)
        {
            if (key.empty())
            {
                return;
            }
            const std::wstring dir = state_dir();
            std::wstring wpattern;
            for (char c : std::string{slotid::kWaypointPrefix} + "*_" + key + ".txt")
            {
                wpattern.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            std::vector<std::string> legacy_names;
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((dir + L"\\" + wpattern).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        continue;
                    }
                    std::string name;
                    for (const wchar_t* p = fd.cFileName; *p != 0; ++p)
                    {
                        name.push_back(*p < 128 ? static_cast<char>(*p) : '?');
                    }
                    const std::string cand = slotid::key_in_filename(name, slotid::kWaypointPrefix);
                    if (slotid::is_legacy_account_key(key, cand))
                    {
                        legacy_names.push_back(name);
                    }
                } while (::FindNextFileW(h, &fd) != 0 && legacy_names.size() < 16);
                ::FindClose(h);
            }
            if (legacy_names.empty())
            {
                return;
            }
            const std::wstring dst = waypoint_path_for(key);
            mv::WaypointSet set{};
            std::string text;
            if (wp_file_exists(dst) && read_whole_file(dst, text))
            {
                mv::waypoints_parse(text, set);
            }
            for (const std::string& name : legacy_names)
            {
                std::wstring src = dir + L"\\";
                for (char c : name)
                {
                    src.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                }
                std::string legacy_text;
                mv::WaypointSet legacy{};
                if (!read_whole_file(src, legacy_text) || !mv::waypoints_parse(legacy_text, legacy))
                {
                    logf(L"waypoint: the legacy file {} could not be read - it is LEFT in place", src);
                    continue;
                }
                std::size_t added = 0;
                for (std::size_t i = 0; i < legacy.count; ++i)
                {
                    const mv::Waypoint& w = legacy.items[i];
                    if (set.count >= mv::kMaxWaypoints)
                    {
                        break;
                    }
                    const mv::WaypointToggleResult hit =
                        mv::waypoint_toggle_at(set, w.x, w.y, w.z, mv::kWaypointSamePlace);
                    if (hit.action != mv::WaypointToggle::Add)
                    {
                        continue;
                    }
                    set.items[set.count++] = w;
                    ++added;
                }
                if (!write_whole_file(dst, mv::waypoints_serialize(set)))
                {
                    logf(L"waypoint: could not merge the legacy file {} into {} (error {}) - both files "
                         L"are left alone",
                         src, dst, static_cast<unsigned>(::GetLastError()));
                    return;
                }
                logf(L"waypoint: merged the legacy file {} ({} waypoint(s), {} of them new) into {} and "
                     L"removed it",
                     src, legacy.count, added, dst);
                ::DeleteFileW(src.c_str());
            }
        }

        // First sight of a slot with no waypoint file of its own: seed it from the shared
        // one, as the found tracker does. The copy is itself the "already seeded" mark -
        // once the file exists this is a no-op.
        void seed_waypoints_from_shared(const std::string& key)
        {
            if (key.empty())
            {
                return;
            }
            const std::wstring dst = waypoint_path_for(key);
            if (wp_file_exists(dst))
            {
                return;
            }
            const std::wstring src = waypoint_path_for(std::string{});
            std::string text;
            if (!wp_file_exists(src) || !read_whole_file(src, text) || text.empty())
            {
                return;
            }
            if (write_whole_file(dst, text))
            {
                logf(L"waypoint: first sight of save slot '{}' - copied the shared waypoints "
                     L"({} bytes) into {}",
                     std::wstring(key.begin(), key.end()), text.size(), dst);
            }
            else
            {
                logf(L"waypoint: could not seed {} from the shared file (error {})", dst,
                     static_cast<unsigned>(::GetLastError()));
            }
        }

        // Takes whatever key slotid has resolved. Loop thread. True when the file changed
        // and the caller must reload it.
        bool adopt_waypoint_key()
        {
            const slotid::Status st = slotid::status();
            const std::string key{st.key};
            if (g_wp_key_valid && key == g_wp_key)
            {
                return false;
            }
            const bool first = !g_wp_key_valid;
            g_wp_key = key;
            g_wp_key_valid = true;
            reconcile_legacy_waypoints(key);
            seed_waypoints_from_shared(key);
            logf(L"waypoint: profile {} '{}' -> {}", first ? L"=" : L"changed to",
                 std::wstring(key.begin(), key.end()), waypoint_path_for(key));
            return true;
        }
    } // namespace

    // The file the waypoints are READ from: the slot's own, falling back to the shared one
    // while the slot has none of its own.
    std::wstring waypoint_path()
    {
        const std::wstring path = waypoint_path_for(g_wp_key);
        if (!g_wp_key.empty() && !wp_file_exists(path))
        {
            const std::wstring shared = waypoint_path_for(std::string{});
            if (wp_file_exists(shared))
            {
                return shared;
            }
        }
        return path;
    }

    // The save-slot watch, mirroring the found tracker's: a pending write goes to the OLD
    // file first, because those waypoints belong to the save that was loaded when they
    // were dropped. Loop thread, 1 Hz.
    void waypoint_slot_poll()
    {
        static std::uint64_t last_check = 0;
        const std::uint64_t now = ::GetTickCount64();
        if (now - last_check < 1000)
        {
            return;
        }
        last_check = now;
        if (g_wp_key_valid && std::string{slotid::status().key} == g_wp_key)
        {
            return;
        }
        if (g_wp_key_valid && g_waypoint_dirty.load(std::memory_order_acquire))
        {
            save_waypoint_file();
        }
        if (adopt_waypoint_key())
        {
            // Clears the dirty flag: the pending set belonged to the previous file.
            load_waypoint_file();
        }
    }

    void load_waypoint_file()
    {
        if (!g_wp_key_valid)
        {
            adopt_waypoint_key();
        }
        const std::wstring path = waypoint_path();
        std::string text;
        mv::WaypointSet set{};
        if (read_whole_file(path, text))
        {
            if (!mv::waypoints_parse(text, set))
            {
                logf(L"waypoint: {} exists but carries no usable coordinates - ignored", path);
                set = mv::WaypointSet{};
            }
            else if (set.count != 0)
            {
                logf(L"waypoint: loaded {} waypoint(s) from {}", set.count, path);
            }
        }
        {
            spin::SpinGuard guard(g_wp_lock);
            g_wp = set;
        }
        // What was just read is what the file says, so nothing is pending.
        g_waypoint_dirty.store(false, std::memory_order_release);
    }

    void save_waypoint_file()
    {
        const mv::WaypointSet set = waypoints();
        // The slot's own file, never the shared one waypoint_path() may fall back to.
        const std::wstring path = waypoint_path_for(g_wp_key);
        if (!write_whole_file(path, mv::waypoints_serialize(set)))
        {
            logf(L"waypoint: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
            return;
        }
        logf(L"waypoint: saved {} waypoint(s)", set.count);
    }
} // namespace mm
