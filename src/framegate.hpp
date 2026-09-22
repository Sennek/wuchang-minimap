#pragma once

//
// framegate - how often the overlay's frame is allowed to happen, as pure arithmetic.
//
// The overlay is drawn from the Present hook, so without a gate it is rebuilt, recorded,
// submitted and composed once per game present - at whatever rate the game runs. Its
// content does not move at that rate: the pawn's location and yaw are published by the
// game thread at 10 Hz, and the HUD path integrates nothing per frame, so most of those
// frames redraw a picture that has not changed.
//
// The gate is a wall clock: a frame is due when a full period has passed since the last
// one drew. The period is not a multiple of the game's frame interval, so the pattern of
// drawn presents is irregular and the rate that results sits below the ceiling - at
// 12.2 ms presents a ceiling of 30 draws ~26.5 times a second. That is the shape measured
// on 2026-09-22, and the number it bought is the one a ceiling is worth.
//
// TIME UNITS: microseconds, from QueryPerformanceCounter. No Windows, no D3D12, no UE4SS.
//

#include <cstdint>

namespace fgate
{
    // 0 is uncapped - a frame per present, which is what the mod did before the ceiling
    // existed. The floor is what keeps the ceiling honest, and it carries two reasons.
    //
    // The pawn's location and yaw are published at 10 Hz, so a ceiling above that loses no
    // step of the minimap's motion - which is the whole argument for having a ceiling at
    // all, and it holds only while the floor is above the publish rate. 15 is not 10
    // because the clock is not snapped to a grid: when the game's frame interval lands just
    // under the period, a frame is skipped every time and the effective rate falls to about
    // half the ceiling. 15 therefore guarantees ~10 Hz at 20 fps and ~13.7 at 82.
    //
    // The second reason is the F2 panel, which is drawn through this gate and is where the
    // key is dragged: a slider you cannot see the effect of cannot be judged, and one you
    // cannot hold onto cannot be dragged back.
    constexpr int kUncapped = 0;
    constexpr int kHzMin = 15;
    constexpr int kHzMax = 1000;

    constexpr int clamp_hz(int v) noexcept
    {
        if (v <= kUncapped)
        {
            return kUncapped;
        }
        return v < kHzMin ? kHzMin : (v > kHzMax ? kHzMax : v);
    }

    // The period one frame owes the clock. 0 for an uncapped gate, which has none.
    constexpr std::uint64_t period_us(int hz) noexcept
    {
        return hz <= kUncapped ? 0ull : 1000000ull / static_cast<std::uint64_t>(hz);
    }

    // Does this present draw? `last_us` is when the last one did, 0 before any has - the
    // first present through a gate always draws, so nothing waits a period to appear.
    constexpr bool due(std::uint64_t now_us, std::uint64_t last_us, int hz) noexcept
    {
        if (hz <= kUncapped || last_us == 0)
        {
            return true;
        }
        return now_us - last_us >= period_us(hz);
    }
}
