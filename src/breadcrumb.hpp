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

    // Loop thread, once, as early as possible. `dir` is the mod directory; the full path
    // is built here and never again, so `stage()` allocates nothing. Reads the previous
    // session's value first.
    void init(const wchar_t* dir, bool enabled);

    // ANY THREAD. Rewrites the file with `name`, a timestamp and the calling thread id,
    // then closes it. POD-only by construction - see the header comment.
    void stage(const char* name);

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
} // namespace crumb
