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

        // How long a disable waits for a Present to pick the teardown up. Past this with
        // nothing started, no Present is arriving and the hooks come out anyway.
        constexpr std::uint64_t kStopTimeoutMs = 3000;

        // How long a teardown that HAS started is given. It is generous on purpose: the
        // render side's own bounded waits - the slicer, the GPU fence, the surface
        // thread's join, the last copy - sum to five seconds before a single
        // DirectComposition call has been made, and those calls have no bound at all.
        // Past this the stop is called wedged and nothing is taken away from the thread.
        constexpr std::uint64_t kStopWedgeMs = 10000;

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

        // The size of a loaded module's file on disk. UE4SS carries no version resource, and the
        // game's executable changes between patches while its version resource does not, so the
        // byte count is the only thing in reach that tells two builds apart in a bug report.
        std::wstring module_bytes(const wchar_t* module_name)
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
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(path, GetFileExInfoStandard, &fad) == 0)
            {
                return {};
            }
            const std::uint64_t bytes =
                (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            return std::format(L"{} bytes", bytes);
        }

        // The PE TimeDateStamp of a loaded module, read from the mapped image. Two game patches
        // can share a version resource but not a link timestamp, so this is what names a build.
        std::uint32_t module_stamp(const wchar_t* module_name)
        {
            const HMODULE mod = module_name == nullptr ? ::GetModuleHandleW(nullptr)
                                                       : ::GetModuleHandleW(module_name);
            if (mod == nullptr)
            {
                return 0;
            }
            const auto* base = reinterpret_cast<const unsigned char*>(mod);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return 0;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return 0;
            }
            return nt->FileHeader.TimeDateStamp;
        }

        // Where the GAME keeps its own log and crash dumps. Named here because a crash report
        // needs them and asking costs a round trip; it is the path saveslot already resolves for
        // the saves.
        std::wstring game_saved_dir()
        {
            const std::wstring local = mm::local_app_data();
            if (local.empty())
            {
                return {};
            }
            return local + L"\\Project_Plague\\Saved";
        }

        void log_bug_report_header()
        {
            const std::wstring game = file_version(nullptr);
            const std::wstring game_size = module_bytes(nullptr);
            const std::wstring ue4ss_size = module_bytes(L"UE4SS.dll");
            mm::logf(L"===== WuchangMinimap v{} - attach these lines to any bug report =====",
                     WUCHANG_MINIMAP_VERSION_W);
            mm::logf(L"  game exe {} ({}, PE stamp 0x{:08X}), UE4SS.dll {}, Windows {}",
                     game.empty() ? std::wstring{L"(no version info)"} : game,
                     game_size.empty() ? std::wstring{L"size unknown"} : game_size,
                     module_stamp(nullptr),
                     ue4ss_size.empty() ? std::wstring{L"not loaded"} : ue4ss_size,
                     windows_build());
            const char* level = mm::log_level_name(mm::config().log_level);
            wchar_t level_w[16]{};
            ::MultiByteToWideChar(CP_UTF8, 0, level, -1, level_w, static_cast<int>(std::size(level_w)) - 1);
            mm::logf(L"  log level {} (log_level in the config; raise it to verbose or trace if you "
                     L"are asked to reproduce something)",
                     level_w);
            mm::logf(L"  config   {}", mm::config_path());
            mm::logf(L"  state    {}", mm::state_dir());
            mm::logf(L"  log      {} (plus .1 / .2 / .3, the three previous sessions)", mm::modlog_path());
            unsigned moved = 0;
            unsigned move_failed = 0;
            mm::state_dir_migration(moved, move_failed);
            if (moved != 0 || move_failed != 0)
            {
                mm::logf(L"  moved {} state file(s) from {} to {}, {} failed",
                         moved,
                         mm::mod_dir(),
                         mm::state_dir(),
                         move_failed);
            }
            mm::logf(L"  crash breadcrumb {} | waypoint {}", mm::state_dir() + crumb::file_name(),
                     mm::waypoint_path());
            const std::wstring saved = game_saved_dir();
            if (!saved.empty())
            {
                mm::logf(L"  the GAME's own logs and crash dumps, which are not this mod's: {}\\Logs and "
                         L"{}\\Crashes",
                         saved,
                         saved);
            }
            mm::log(L"  SEND: wuchang_minimap.log, wuchang_minimap_last_stage.txt and your config file. "
                    L"If the GAME crashed rather than the overlay, add its own Logs and Crashes folders "
                    L"named above.");
        }

        enum class State
        {
            Off,      // mod_enabled = 0 and the shutdown has completed
            Running,  // everything is up
            Stopping, // waiting for the render thread (or the timeout)
        };

        State g_state = State::Off;
        std::atomic<bool> g_session_disable{false};
        bool g_ever_started = false;
        std::uint64_t g_last_watch = 0;
        std::uint64_t g_config_mtime = 0;
        std::uint64_t g_stop_began = 0;    // when the disable was requested
        std::uint64_t g_stop_deadline = 0; // the next moment the stop has something to do or say
        std::uint64_t g_stop_wedge_at = 0; // when a teardown in progress is called wedged; 0 once it was

        void start_subsystems()
        {
            // Order matters:
            //   overlay  - config-driven assets + the DX12 hooks (no UObject work);
            //   markers  - the static DB + found tracker, read on this thread before anything
            //              can pump the live half;
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
            // A flip back on re-reads the whole file, taking every edit made while it was off.
            mm::load_config_file();
            mm::g_mod_active.store(true, std::memory_order_release);
            g_state = State::Running;
            // Nothing measured across the off period means anything: gamestate's on_update did not
            // run, so its watchdog window is stale by however long the mod was off.
            gamestate::reset_watchdog();
            mm::logf(L"master switch: mod_enabled = 1 ({}) - starting up", std::wstring{why});
            if (!g_ever_started)
            {
                g_ever_started = true;
                start_subsystems();
            }
            else
            {
                // gamestate / navmesh are registered and self-guard; the overlay re-enables its
                // hooks and re-reads the assets from disk.
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
            // Step 1.
            mm::g_mod_active.store(false, std::memory_order_release);
            g_state = State::Stopping;
            g_stop_began = ::GetTickCount64();
            g_stop_deadline = g_stop_began + kStopTimeoutMs;
            g_stop_wedge_at = g_stop_began + kStopWedgeMs;
            mm::logf(L"master switch: mod_enabled = 0 ({}) - the game-thread pump and the overlay are "
                     L"standing down",
                     std::wstring{why});
            overlay::request_stop();
            mm::drain_log();
        }

        // False when the hooks could not come out because a thread is still inside the
        // detour: nothing was changed and the stop has to go on waiting.
        bool finish_disable(bool never_started)
        {
            if (!overlay::finish_stop()) // step 3a: the hooks come out
            {
                return false;
            }
            // The found tracker's write is debounced a couple of seconds; flushed before teardown.
            markers::flush_found_tracker();
            mapdata::unload();      // step 3b: the chapter's height planes are freed
            g_state = State::Off;
            if (never_started)
            {
                // No Present ever reached the teardown, so no thread is inside the detour
                // and the surface thread is still alive. Stopping a thread of the mod's
                // own is allowed from here; releasing a D3D12 object still is not.
                overlay::stop_surface_thread();
                mm::logf(L"master switch: no Present reached the overlay's teardown within {} ms, so the "
                         L"render thread never started it - the hooks came out anyway; ImGui and the "
                         L"D3D12 objects stay allocated until the mod is turned back on",
                         kStopTimeoutMs);
            }
            mm::logf(L"master switch: the mod is OFF. Nothing is hooked, nothing is scanned and no map is "
                     L"resident; set mod_enabled = 1 in {} to turn it back on (checked once a second).",
                     mm::config_path());
            mm::drain_log();
            // Last thing the stop does, so the breadcrumb names the state the mod stays in: any
            // earlier, step 3b's chapter unload overwrites it and a crash while the mod is off is
            // reported against a chapter swap.
            crumb::stage(crumb::kModOff);
            return true;
        }

        // A teardown that HAS started and has not finished. Nothing may be taken away from
        // a thread that is inside the detour and inside DirectComposition, so this waits
        // and says so - once a second, then once for the wedge, then silently for ever,
        // because a render thread that finishes late still finishes cleanly.
        void report_slow_stop(std::uint64_t now)
        {
            g_stop_deadline = now + 1000;
            if (g_stop_wedge_at == 0)
            {
                return; // already called wedged; there is nothing new to say
            }
            if (now < g_stop_wedge_at)
            {
                mm::logf(L"master switch: the render thread is still inside its teardown after {} ms. "
                         L"The hooks stay installed and nothing is released - it is in there. Waiting "
                         L"up to {} ms in all.",
                         now - g_stop_began,
                         kStopWedgeMs);
                return;
            }
            g_stop_wedge_at = 0;
            mm::logf(L"master switch: WEDGED - the render thread has been inside the overlay's teardown "
                     L"for {} ms. It is a thread of the GAME's standing inside this mod's detour, so "
                     L"the hooks are NOT disabled, no D3D12 object is released and the surface thread "
                     L"is not touched: taking any of that away would fault that thread rather than free "
                     L"it. The mod stays in this state and still finishes the stop properly if the "
                     L"thread ever comes back. Send wuchang_minimap.log and "
                     L"wuchang_minimap_last_stage.txt.",
                     now - g_stop_began);
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

        // The crash breadcrumb goes as early as possible: it reads the previous session's value
        // before overwriting it, and a non-terminal stage is the only evidence surviving a process
        // that died with the log buffer unflushed. The flush hook is wired first so every stage
        // flushes the log too, and the two files cannot disagree.
        crumb::set_flush_hook(&mm::modlog_flush);
        crumb::init(mm::state_dir().c_str(), mm::config().crash_breadcrumb);
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
            const overlay::StopPhase phase = overlay::stop_phase();
            bool finished = false;
            if (phase == overlay::StopPhase::Done)
            {
                finished = finish_disable(false);
            }
            else if (now >= g_stop_deadline)
            {
                // The two meanings of a timeout, which is the whole reason the phase
                // exists: nothing started, so no Present is arriving and the hooks can
                // come out; or it started and is still running, and nothing it holds may
                // be pulled out from under it.
                finished = phase == overlay::StopPhase::NotStarted && finish_disable(true);
            }
            if (!finished && now >= g_stop_deadline)
            {
                // Either a teardown in progress, or one whose thread is still inside the
                // detour with the render lock. Same answer to both: wait, say so, and
                // take nothing away.
                report_slow_stop(now);
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
