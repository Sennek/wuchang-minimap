#pragma once

//
// breadcrumb - one line on disk saying which stage the overlay is in.
//
// WHY
// ---
// The navmesh dumper has had `navmesh/last_stage.txt` since its first in-game run and
// it is the single most useful diagnostic in this project (lessons.md: "give a scanning
// module a one-line crash breadcrumb file"). The overlay had nothing equivalent, and
// UE4SS's log is BUFFERED - when the process dies the last seconds of it can be gone,
// which is exactly the window a crash happens in. A file that is closed after every
// write cannot be lost.
//
// THE CONSTRAINTS THIS SHAPE COMES FROM (all from lessons.md)
//   * `stage()` is called from the loop thread, the render thread AND the game thread,
//     so it must be POD-only: no iostreams, no locale, no std::mutex, no allocation.
//     It is plain `CreateFileW` + `WriteFile` + `CloseHandle` over a path built once at
//     init and a stack buffer assembled by hand.
//   * It must be cheap enough not to matter. It is called on TRANSITIONS - a handful of
//     times per session - never per frame and never per pump.
//
// WHAT IT ANSWERS ON THE NEXT LAUNCH
// ----------------------------------
// `init()` reads whatever the previous session left behind before overwriting it. A
// terminal stage ("clean exit", "teardown end") means the mod shut down properly; any
// other value means the process died while the overlay was in that stage, and that is
// logged as a warning naming the stage and shown in the F2 Debug tab.
//

#include <cstdint>

namespace crumb
{
    // The stage names. Plain string literals rather than an enum so the file is readable
    // by a human with no reference to hand - the whole point is that a player can paste
    // one line into a bug report.
    inline constexpr const char* kDllLoaded = "dll loaded";
    inline constexpr const char* kHooksInstalled = "hooks installed";
    inline constexpr const char* kSwapchainChosen = "swapchain chosen";
    inline constexpr const char* kImGuiUp = "imgui up";
    inline constexpr const char* kFirstSlice = "first slice";
    inline constexpr const char* kChapterSwapStart = "chapter swap start";
    inline constexpr const char* kChapterSwapEnd = "chapter swap end";
    inline constexpr const char* kTeardownBegin = "teardown begin";
    inline constexpr const char* kTeardownEnd = "teardown end";
    inline constexpr const char* kCleanExit = "clean exit";
    // ALT+F4 / the window's close button / DLL_PROCESS_DETACH. A TERMINAL stage: the
    // player closed the game, which is not a crash, and the next launch must not report
    // one. Before this existed, quitting with ALT+F4 always produced
    // "last session ended at 'first slice' - it did NOT shut down cleanly", because none
    // of the mod's teardown paths run when the window is closed from the outside: the
    // render thread is never asked to tear ImGui down and `~WuchangMinimap()` (the only
    // writer of `kCleanExit`) is not called either.
    inline constexpr const char* kWindowClosed = "window closed";

    // Loop thread, once, as early as possible. `dir` is the mod directory; the full path
    // is built here and never again, so `stage()` allocates nothing. Reads the previous
    // session's value first.
    void init(const wchar_t* dir, bool enabled);

    // ANY THREAD. Rewrites the file with `name`, a timestamp and the calling thread id,
    // then closes it. POD-only by construction - see the header comment.
    void stage(const char* name);

    // ANY THREAD, and idempotent: writes the terminal `kWindowClosed` stage exactly once.
    // Called from the WndProc hook on WM_CLOSE / WM_DESTROY / WM_QUIT and from
    // DLL_PROCESS_DETACH, i.e. from the paths an ALT+F4 actually takes. Idempotency
    // matters because those fire in sequence and the last writer would otherwise be the
    // one racing the process teardown.
    void mark_closing();

    // ANY THREAD, POD-ONLY, APPEND-ONLY. One line into `wuchang_minimap_watchdog.txt`
    // next to the breadcrumb, written with a hand-built stack buffer through flat
    // `CreateFileW` / `WriteFile` with FILE_FLAG_WRITE_THROUGH and closed again.
    //
    // WHY IT CANNOT GO THROUGH mm::logf. This is the diagnostic for a FREEZE, and the
    // freeze we are chasing is one where the process heap may be the thing that is
    // wedged - `std::format` allocates, `mm::log` allocates, and both would then hang
    // the one thread still running instead of leaving evidence. So the watchdog writes
    // its line with no allocation at all, and only THEN tries the ordinary log.
    //
    // `render_ms` / `game_ms` are how long since the render thread last presented and
    // since the game thread last pumped; `render_stage` / `game_stage` are what each of
    // them was last seen doing; `note` is free text (both stages' owner thread ids).
    void watchdog(unsigned long render_ms, unsigned long game_ms, const char* render_stage,
                  const char* game_stage, const char* note);

    // A hook the breadcrumb calls right after every successful write, used to flush the
    // mod's own rolling log so the two always agree about the last thing that happened.
    // A plain function pointer keeps this file free of everything but the flat Win32 API
    // (see the header comment); nullptr disables it.
    void set_flush_hook(void (*hook)());

    // The stage the PREVIOUS session's file held, or "" when there was none. Loop thread
    // (it is set once by init and never written again).
    const char* previous();

    // True when `previous()` is a non-terminal stage, i.e. the last session did not shut
    // the overlay down cleanly. False when there was no previous file at all - a first
    // run is not a crash.
    bool previous_suspicious();

    // The name currently in the file (the last thing `stage()` wrote). Any thread; it is
    // a fixed buffer written before the file, so a torn read is impossible in practice
    // and harmless in principle.
    const char* current();

    // The file's name, with its leading backslash, so a caller can compose the full path
    // without a second copy of the literal. Used by the startup bug-report header, which
    // has to tell the player which files to attach.
    const wchar_t* file_name();
} // namespace crumb
