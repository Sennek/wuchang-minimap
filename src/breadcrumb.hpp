#pragma once

//
// breadcrumb - one line on disk naming the stage the overlay is in, rewritten and
// closed on every transition so a dying process cannot lose it.
//
// `stage()` runs on the loop, render and game threads, so everything here is POD-only:
// flat CreateFileW / WriteFile / CloseHandle, hand-built stack buffers, no allocation,
// no iostreams, no locale, no mutex.
//

#include <cstdint>

namespace crumb
{
    // Stage names. String literals, not an enum, so the file reads as plain English in
    // a bug report.
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
    // ALT+F4 / close button / DLL_PROCESS_DETACH. Terminal: none of the mod's teardown
    // paths run when the window closes from outside, so this is the only clean marker.
    inline constexpr const char* kWindowClosed = "window closed";
    // The master switch is off and everything is released. Without it the file keeps
    // whatever the last transition happened to be - a chapter unload, most often - and a
    // crash in that window is reported against a stage the mod is not in any more.
    inline constexpr const char* kModOff = "mod off";

    // Loop thread, once. Builds the paths under mod directory `dir` and reads the
    // previous session's value before overwriting it.
    void init(const wchar_t* dir, bool enabled);

    // Any thread. Rewrites the file with `name`, a timestamp and the calling thread id.
    //
    // The five start-up stages are a ladder - dll loaded, hooks installed, swapchain
    // chosen, imgui up, first slice - and a rung is never written over a higher one. The
    // loop thread and the render thread climb it at the same time: on a fast machine
    // `install_hooks` returns after the render thread has already reached `imgui up`, and
    // a crash report naming a stage the mod had already left is worse than useless. A
    // teardown lets the ladder be climbed again; every other stage always writes.
    void stage(const char* name);

    // Any thread. Writes the terminal `kWindowClosed` stage exactly once; WM_CLOSE,
    // WM_DESTROY, WM_QUIT and DLL_PROCESS_DETACH all call it in sequence.
    void mark_closing();

    // Any thread, append-only, allocation-free: the freeze this diagnoses can be a
    // wedged heap, so it must not go through mm::logf.
    //
    // `render_ms` / `game_ms`: milliseconds since the render thread last presented and
    // the game thread last pumped. `render_stage` / `game_stage`: what each was last
    // doing. `note`: free text.
    void watchdog(unsigned long render_ms, unsigned long game_ms, const char* render_stage,
                  const char* game_stage, const char* note);

    // Called after every successful write, to flush the mod's rolling log in step with
    // the breadcrumb. nullptr disables it.
    void set_flush_hook(void (*hook)());

    // The previous session's stage, or "" when there was none. Set once by init().
    const char* previous();

    // True when `previous()` is a non-terminal stage. False when there was no previous
    // file: a first run is not a crash.
    bool previous_suspicious();

    // The last name `stage()` wrote. Any thread; a fixed buffer, torn reads harmless.
    const char* current();

    // The file's name, leading backslash included.
    const wchar_t* file_name();
} // namespace crumb
