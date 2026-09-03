#pragma once

//
// highlight - the GAME-THREAD half of the hold-key x-ray highlight: it reads the
// camera pose and publishes it. The drawing lives in overlay.cpp, because that is
// where ImGui, the swapchain size and the marker glyphs are.
//
// WHY THE CAMERA IS PUBLISHED HERE AND NOT IN mm::Snapshot
// --------------------------------------------------------
// `gamestate.cpp` (which fills the Snapshot) is owned by another workstream, and a
// Snapshot field nobody fills reads zero forever - a silent, invisible failure. So the
// pose gets its own seqlock in this module and its own clearly-marked call site inside
// the existing game-thread pump chain (markers::game_thread_pump, which already runs
// only while gamestate has a validated gameplay pawn outside the transition cooldown).
//
// HOW THE CAMERA IS READ (and why it is not just "read POV at +8")
// ---------------------------------------------------------------
// `APlayerCameraManager::CameraCachePrivate` is an FCameraCacheEntry - a float
// timestamp followed by an FMinimalViewInfo whose first fields are Location (3
// doubles), Rotation (3 doubles) and FOV (float). That layout is documented, not
// verified on this build, and lessons.md is explicit that recognising a non-reflected
// engine struct by an assumed field order is how a whole play session gets wasted.
//
// So the offset of the POV block inside the cache is DISCOVERED once:
//   1. call the three blueprint getters (GetCameraLocation / GetCameraRotation /
//      GetFOVAngle) - one ProcessEvent each, SEH-guarded like every other call in this
//      mod;
//   2. scan the first bytes of CameraCachePrivate for the offset whose 6 doubles and
//      following float match what the getters just said;
//   3. pin it and never call a getter again - from then on the read is 56 bytes at a
//      cached offset, cheap enough to run at the frame rate.
// If the getters are unavailable, step 2 falls back to accepting the first offset that
// is merely SANE (finite, in-world, pitch in range, FOV 5..170) and the F2 panel says
// so. If the pinned offset ever stops producing sane values it is dropped and the
// discovery runs again - there is no latched "we know where it is".
//
// THREADS
//   game thread   game_thread_pump(), drop_caches()  - reflection and raw reads only
//   loop thread   set_demand()                       - the hotkey sampler's answer
//   any thread    camera(), stats()                  - lock-free reads of the seqlock
//

#include <cstdint>

namespace mm
{
    struct Config;
}

namespace hl
{
    // Trivially copyable: handed over with the same seqlock pattern mm::Snapshot uses.
    struct Pose
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double pitch = 0.0; // degrees, UE FRotator
        double yaw = 0.0;
        double roll = 0.0;
        double fov = 0.0;             // horizontal, degrees
        std::uint64_t stamp_ms = 0;   // GetTickCount64() when it was read
        bool valid = false;
    };

    // How the pose was obtained, for the F2 panel and the log.
    enum class Route : int
    {
        None = 0,      // nothing read yet
        RawPinned = 1, // raw POD read at the calibrated CameraCachePrivate offset
        RawSane = 2,   // same, but the offset was accepted on sanity alone (no getters)
        Getters = 3,   // ProcessEvent getters every read (the cache was unreadable)
    };

    struct Stats
    {
        Route route = Route::None;
        int pov_offset = -1;     // bytes into CameraCachePrivate, -1 = not pinned
        int cache_offset = -1;   // bytes into APlayerCameraManager, -1 = no property
        std::uint64_t reads = 0; // successful publishes
        std::uint64_t fails = 0; // reads rejected by the sanity gate
        std::uint64_t last_ms = 0;
        bool have_manager = false;
        bool demanded = false; // something on screen currently wants the camera
    };

    // Any thread. False when nothing usable has ever been published.
    bool camera(Pose& out);
    Stats stats();

    // LOOP THREAD. `held` = the highlight is ON right now - the key / pad chord is down
    // in hold mode, or the toggle latch is set in toggle mode - read at the full camera
    // rate; `compass` = the compass wants a heading (read at 20 Hz). Both false = the
    // reader does nothing at all, which is the normal state.
    void set_demand(bool held, bool compass);
    bool held();

    // THE TOGGLE LATCH (highlight_mode = toggle). Set by the hotkey sampler on the loop
    // thread and cleared from live state - never remembered across a level transition or
    // a dropped pawn, because drop_caches() below clears it. Any thread may read it.
    bool xray_latched();
    // Returns the new state. Loop thread (the hotkey sampler) only.
    bool xray_latch_flip();
    // `why` is logged when the latch was actually on. Any thread.
    void xray_latch_clear(const wchar_t* why);

    // GAME THREAD ONLY, from the existing pump chain.
    // `now` is GetTickCount64 (used for the pose stamp and the resolve throttle);
    // `now_us` is QueryPerformanceCounter microseconds and is what paces the camera
    // read. The tick count only moves in ~15.6 ms steps, so pacing on it made
    // highlight_camera_hz above ~64 do nothing and jittered the cadence by a frame.
    void game_thread_pump(std::uint64_t now, std::uint64_t now_us, const void* world, const mm::Config& cfg);

    // GAME THREAD ONLY. Drops the camera manager and every cached offset - called
    // whenever the pawn / world goes, because the manager belonged to that world.
    void drop_caches();
} // namespace hl
