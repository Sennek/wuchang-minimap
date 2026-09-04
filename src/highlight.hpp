#pragma once

//
// highlight - game-thread half of the hold-key x-ray highlight: reads the camera pose
// and publishes it. Drawing lives in overlay.cpp.
//
// The POV block offset inside `APlayerCameraManager::CameraCachePrivate` (an
// FCameraCacheEntry: float timestamp + FMinimalViewInfo) is discovered at runtime:
// call GetCameraLocation / GetCameraRotation / GetFOVAngle once, scan the cache for
// the offset whose 6 doubles + float match, pin it and read raw from then on. Without
// the getters the first merely-sane offset (finite, in-world, pitch in range, FOV
// 5..170) is accepted instead. A pinned offset that stops producing sane values is
// dropped and rediscovered.
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
        Getters = 3,   // ProcessEvent getters every read (the cache is unreadable)
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

    // LOOP THREAD. `held` = highlight is on, read at the full camera rate; `compass` =
    // the compass wants a heading, read at 20 Hz. Both false = the reader does nothing.
    void set_demand(bool held, bool compass);
    bool held();

    // Toggle latch (highlight_mode = toggle). Any thread may read it; only the hotkey
    // sampler may set it, and drop_caches() clears it.
    bool xray_latched();
    // Returns the new state. Loop thread (the hotkey sampler) only.
    bool xray_latch_flip();
    // `why` is logged when the latch was actually on. Any thread.
    void xray_latch_clear(const wchar_t* why);

    // GAME THREAD ONLY, from the pump chain. `now` is GetTickCount64 (pose stamp and
    // resolve throttle); `now_us` is QPC microseconds and paces the camera read.
    void game_thread_pump(std::uint64_t now, std::uint64_t now_us, const void* world, const mm::Config& cfg);

    // GAME THREAD ONLY. Drops the camera manager and every cached offset; the manager
    // belongs to a world.
    void drop_caches();
} // namespace hl
