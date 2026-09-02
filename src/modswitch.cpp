#include "modswitch.hpp"

#include <Windows.h>

#include <cstdint>

#include "gamestate.hpp"
#include "mapdata.hpp"
#include "markers.hpp"
#include "mmstate.hpp"
#include "navmesh_dump.hpp"
#include "overlay.hpp"

namespace modswitch
{
    namespace
    {
        // How often the loop thread stat()s the config file while the mod is running
        // AND while it is off. One GetFileAttributesEx of a file that is already in the
        // OS cache is a few microseconds; the file is only parsed when the timestamp
        // actually moved.
        constexpr std::uint64_t kWatchPeriodMs = 1000;

        // How long a disable waits for the render thread to tear its own D3D12 objects
        // down before the hooks come out anyway. At 60 fps a frame is 17 ms; three
        // seconds is a game that is minimised or paused at a driver level.
        constexpr std::uint64_t kStopTimeoutMs = 3000;

        enum class State
        {
            Off,      // mod_enabled = 0 and the shutdown has completed
            Running,  // everything is up
            Stopping, // waiting for the render thread (or the timeout)
        };

        State g_state = State::Off;
        bool g_ever_started = false;
        std::uint64_t g_last_watch = 0;
        std::uint64_t g_config_mtime = 0;
        std::uint64_t g_stop_deadline = 0;

        void start_subsystems()
        {
            // ORDER MATTERS and is the same as the original dllmain:
            //   overlay  - config-driven assets + the DX12 hooks (no UObject work);
            //   markers  - the static DB and the found tracker, read on THIS thread,
            //              before anything can pump the live half;
            //   gamestate- registers the ProcessEvent game-thread pump (idempotent);
            //   navmesh  - opt-in, and it registers its own pump (idempotent).
            overlay::start();
            markers::on_unreal_init();
            gamestate::on_unreal_init();
            navmesh::on_unreal_init();
        }

        void enable(const wchar_t* why)
        {
            if (g_state == State::Running)
            {
                return;
            }
            // A flip back on re-reads the WHOLE file: the player has had the mod off,
            // and whatever else they edited in the meantime is what they meant.
            mm::load_config_file();
            mm::g_mod_active.store(true, std::memory_order_release);
            g_state = State::Running;
            mm::logf(L"master switch: mod_enabled = 1 ({}) - starting up", std::wstring{why});
            if (!g_ever_started)
            {
                g_ever_started = true;
                start_subsystems();
            }
            else
            {
                // A restart. gamestate / navmesh are already registered and guard
                // themselves; the overlay re-enables its hooks and the assets are read
                // again from disk.
                overlay::start();
                markers::reload();
            }
            mm::drain_log();
        }

        void begin_disable(const wchar_t* why)
        {
            if (g_state != State::Running)
            {
                return;
            }
            // Step 1: nothing new starts anywhere from this store on.
            mm::g_mod_active.store(false, std::memory_order_release);
            g_state = State::Stopping;
            g_stop_deadline = ::GetTickCount64() + kStopTimeoutMs;
            mm::logf(L"master switch: mod_enabled = 0 ({}) - the game-thread pump and the overlay are "
                     L"standing down",
                     std::wstring{why});
            overlay::request_stop();
            mm::drain_log();
        }

        void finish_disable(bool timed_out)
        {
            overlay::finish_stop(); // step 3a: the hooks come out
            mapdata::unload();      // step 3b: the chapter's height planes are freed
            g_state = State::Off;
            if (timed_out)
            {
                mm::logf(L"master switch: the render thread did not answer within {} ms (no Present is "
                         L"arriving) - the hooks were disabled anyway; ImGui and the D3D12 objects stay "
                         L"allocated until the mod is turned back on",
                         kStopTimeoutMs);
            }
            mm::logf(L"master switch: the mod is OFF. Nothing is hooked, nothing is scanned and no map is "
                     L"resident; set mod_enabled = 1 in {} to turn it back on (checked once a second).",
                     mm::config_path());
            mm::drain_log();
        }

        // The 1 Hz watcher. It reads ONLY `mod_enabled`, and only when the file's
        // timestamp moved - an edit to any other key is picked up by F5 while the mod
        // runs, and by the full reload a re-enable does.
        void watch(std::uint64_t now)
        {
            if (now - g_last_watch < kWatchPeriodMs)
            {
                return;
            }
            g_last_watch = now;
            const std::uint64_t mtime = mm::config_mtime();
            if (mtime == g_config_mtime)
            {
                return;
            }
            g_config_mtime = mtime;
            bool wanted = mm::config().mod_enabled;
            if (!mm::peek_mod_enabled(wanted))
            {
                return; // no mod_enabled line (or the file vanished): change nothing
            }
            if (wanted && g_state != State::Running)
            {
                enable(L"the config file changed");
            }
            else if (!wanted && g_state == State::Running)
            {
                begin_disable(L"the config file changed");
            }
        }
    } // namespace

    void on_unreal_init()
    {
        mm::set_loop_thread();
        mm::load_config_file();
        g_config_mtime = mm::config_mtime();
        g_last_watch = ::GetTickCount64();

        if (!mm::config().mod_enabled)
        {
            mm::g_mod_active.store(false, std::memory_order_release);
            g_state = State::Off;
            mm::logf(L"master switch: mod_enabled = 0 in {} - the mod is inert. No DX12 hook is installed, "
                     L"no game-thread callback is registered, nothing is scanned and no map is loaded. "
                     L"Set mod_enabled = 1 and save the file: the change is picked up within a second "
                     L"(F5 cannot be used while the mod is off - nothing is reading the keyboard).",
                     mm::config_path());
            mm::drain_log();
            return;
        }

        mm::g_mod_active.store(true, std::memory_order_release);
        g_state = State::Running;
        g_ever_started = true;
        start_subsystems();
    }

    void on_update()
    {
        const std::uint64_t now = ::GetTickCount64();

        if (g_state == State::Stopping)
        {
            const bool done = overlay::stop_complete();
            if (done || now >= g_stop_deadline)
            {
                finish_disable(!done);
            }
            // Nothing else runs during a stop; the log is drained by finish_disable.
            mm::drain_log();
            return;
        }

        if (g_state == State::Off)
        {
            // THE ONLY THING A DISABLED MOD DOES.
            watch(now);
            mm::drain_log();
            return;
        }

        // Running: the original dllmain body, plus the watcher.
        navmesh::on_update();
        overlay::on_update();
        // The chapter's map asset is loaded and unloaded here, on the loop thread:
        // gamestate names the chapter from the game thread with one atomic store, and
        // mapdata does the (multi-second, allocating) PNG work off it. See mapdata.hpp.
        mapdata::on_update();
        gamestate::on_update();
        markers::on_update();
        watch(now);
    }
} // namespace modswitch
