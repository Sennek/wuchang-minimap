#include "modswitch.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <vector>

#include "breadcrumb.hpp"
#include "gamestate.hpp"
#include "mapdata.hpp"
#include "markers.hpp"
#include "mmstate.hpp"
#include "navmesh_dump.hpp"
#include "recon.hpp"
#include "overlay.hpp"
#include "version.hpp"

namespace modswitch
{
    namespace
    {
        // How often the loop thread stat()s the config file, running or off. The file is
        // parsed only when the timestamp moved.
        constexpr std::uint64_t kWatchPeriodMs = 1000;

        // How long a disable waits for the render thread to tear its own D3D12 objects
        // down before the hooks come out anyway.
        constexpr std::uint64_t kStopTimeoutMs = 3000;

        // A module's FILEVERSION as "a.b.c.d", or an empty string. `nullptr` asks for the
        // running executable, which is how the game's own build is named.
        std::wstring file_version(const wchar_t* module_name)
        {
            wchar_t path[MAX_PATH]{};
            const HMODULE mod = module_name == nullptr ? nullptr : ::GetModuleHandleW(module_name);
            if (module_name != nullptr && mod == nullptr)
            {
                return {};
            }
            if (::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path))) == 0)
            {
                return {};
            }
            DWORD ignored = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(path, &ignored);
            if (size == 0)
            {
                return {};
            }
            std::vector<unsigned char> buf(size);
            if (::GetFileVersionInfoW(path, 0, size, buf.data()) == 0)
            {
                return {};
            }
            VS_FIXEDFILEINFO* info = nullptr;
            UINT len = 0;
            if (::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) == 0 ||
                info == nullptr || len < sizeof(VS_FIXEDFILEINFO))
            {
                return {};
            }
            return std::format(L"{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                               HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        }

        // The real build number: GetVersionEx lies to a process without a manifest entry
        // for the running OS, RtlGetVersion does not.
        std::wstring windows_build()
        {
            using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (ntdll != nullptr)
            {
                const auto fn = reinterpret_cast<RtlGetVersionFn>(
                    reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
                RTL_OSVERSIONINFOW vi{};
                vi.dwOSVersionInfoSize = sizeof(vi);
                if (fn != nullptr && fn(&vi) == 0)
                {
                    return std::format(L"{}.{}.{}", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
                }
            }
            return L"unknown";
        }

        void log_bug_report_header()
        {
            const std::wstring game = file_version(nullptr);
            const std::wstring ue4ss = file_version(L"UE4SS.dll");
            mm::logf(L"===== WuchangMinimap v{} - attach these lines to any bug report =====",
                     WUCHANG_MINIMAP_VERSION_W);
            mm::logf(L"  game exe {}, UE4SS {}, Windows {}",
                     game.empty() ? std::wstring{L"(no version info)"} : game,
                     ue4ss.empty() ? std::wstring{L"(not loaded / no version info)"} : ue4ss,
                     windows_build());
            const char* level = mm::log_level_name(mm::config().log_level);
            wchar_t level_w[16]{};
            ::MultiByteToWideChar(CP_UTF8, 0, level, -1, level_w, static_cast<int>(std::size(level_w)) - 1);
            mm::logf(L"  log level {} (log_level in the config; raise it to verbose or trace if you "
                     L"are asked to reproduce something)",
                     level_w);
            mm::logf(L"  config   {}", mm::config_path());
            mm::logf(L"  log      {} (plus .1 / .2 / .3, the three previous sessions)", mm::modlog_path());
            mm::logf(L"  crash breadcrumb {} | waypoint {}", mm::mod_dir() + crumb::file_name(),
                     mm::waypoint_path());
            mm::log(L"  SEND: wuchang_minimap.log, wuchang_minimap_last_stage.txt and your config file.");
        }

        enum class State
        {
            Off,      // mod_enabled = 0 and the shutdown has completed
            Running,  // everything is up
            Stopping, // waiting for the render thread (or the timeout)
        };

        State g_state = State::Off;
        // "Disable for this session" from the F2 panel: the same three-step stop as
        // mod_enabled = 0, writing nothing, so the mtime watch turns the mod back on
        // when the config is next saved.
        std::atomic<bool> g_session_disable{false};
        bool g_ever_started = false;
        std::uint64_t g_last_watch = 0;
        std::uint64_t g_config_mtime = 0;
        std::uint64_t g_stop_deadline = 0;

        void start_subsystems()
        {
            // Order matters:
            //   overlay  - config-driven assets + the DX12 hooks (no UObject work);
            //   markers  - the static DB and the found tracker, read on this thread,
            //              before anything can pump the live half;
            //   gamestate- registers the ProcessEvent game-thread pump (idempotent);
            //   navmesh  - opt-in, registers its own pump (idempotent).
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
            // A flip back on re-reads the whole file, picking up every other edit made
            // while the mod was off.
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
                // gamestate / navmesh are already registered and guard themselves; the
                // overlay re-enables its hooks and re-reads the assets from disk.
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
            // The found tracker's write is debounced by a couple of seconds, so it is
            // flushed before anything is torn down.
            markers::flush_found_tracker();
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

        // The 1 Hz watcher. Reads only `mod_enabled`, and only when the file's timestamp
        // moved; other keys are picked up by F5 or by the full reload a re-enable does.
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
                return; // no mod_enabled line, or the file vanished
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

    void request_session_disable()
    {
        g_session_disable.store(true, std::memory_order_release);
    }

    void on_unreal_init()
    {
        mm::set_loop_thread();
        mm::load_config_file();
        g_config_mtime = mm::config_mtime();
        g_last_watch = ::GetTickCount64();

        // The crash breadcrumb goes as early as possible: it reads what the previous
        // session left before overwriting it, and a non-terminal stage there is the only
        // evidence that survives a process that died with the log buffer unflushed. The
        // flush hook is wired first so every breadcrumb stage also flushes the log and
        // the two files cannot disagree.
        crumb::set_flush_hook(&mm::modlog_flush);
        crumb::init(mm::mod_dir().c_str(), mm::config().crash_breadcrumb);
        log_bug_report_header();
        if (crumb::previous_suspicious())
        {
            const char* prev = crumb::previous();
            mm::logf(L"WARNING: the previous session did not shut down cleanly - "
                     L"wuchang_minimap_last_stage.txt says '{}'. If the game crashed, that is the "
                     L"overlay stage it was in; send this line with the crash report.",
                     std::wstring(prev, prev + std::strlen(prev)));
        }
        crumb::stage(crumb::kDllLoaded);

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
        // The mod log is flushed in every state, not just Running, so a disabled or
        // stopping mod still writes its buffered tail.
        mm::modlog_tick(now);

        if (g_state == State::Stopping)
        {
            const bool done = overlay::stop_complete();
            if (done || now >= g_stop_deadline)
            {
                finish_disable(!done);
            }
            // Nothing else runs during a stop.
            mm::drain_log();
            return;
        }

        if (g_state == State::Off)
        {
            // The only thing a disabled mod does.
            g_session_disable.store(false, std::memory_order_relaxed);
            watch(now);
            mm::drain_log();
            return;
        }

        if (g_session_disable.exchange(false, std::memory_order_acquire))
        {
            begin_disable(L"disabled for this session from the F2 panel - the config file is "
                          L"unchanged, so saving it (or editing it) turns the mod back on");
            mm::drain_log();
            return;
        }

        navmesh::on_update();
        overlay::on_update();
        // The chapter's map asset is loaded and unloaded on the loop thread: gamestate
        // names the chapter from the game thread with one atomic store, and mapdata does
        // the multi-second, allocating PNG work off it.
        mapdata::on_update();
        gamestate::on_update();
        markers::on_update();
        recon::on_update();
        watch(now);
    }
} // namespace modswitch
