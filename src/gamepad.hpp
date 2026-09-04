#pragma once

//
// gamepad - XInput for the full map.
//
// Threading: `XInputGetState` is polled from `on_update` on the UE4SS event-loop
// thread, alongside the keyboard - never from the game thread, never from Present.
// Every other thread reads only the lock-free atomics published here.
//
// XInput is loaded dynamically (xinput1_4 -> 1_3 -> 9_1_0), so a machine without the
// redistributable reports "no pad" rather than failing to load the mod. Polling a
// disconnected slot costs up to a millisecond, so empty slots are re-probed once a
// second.
//
// `held` is the current button mask; `pressed` accumulates rising edges until a
// consumer takes them, so a tap between two frames is neither missed nor seen twice.
//

#include <cstdint>

namespace pad
{
    // XInput button bits, spelled out so <Xinput.h> is not needed.
    constexpr std::uint16_t kDpadUp = 0x0001;
    constexpr std::uint16_t kDpadDown = 0x0002;
    constexpr std::uint16_t kDpadLeft = 0x0004;
    constexpr std::uint16_t kDpadRight = 0x0008;
    constexpr std::uint16_t kStart = 0x0010;
    constexpr std::uint16_t kBack = 0x0020;
    constexpr std::uint16_t kLeftThumb = 0x0040;
    constexpr std::uint16_t kRightThumb = 0x0080;
    constexpr std::uint16_t kLeftShoulder = 0x0100;
    constexpr std::uint16_t kRightShoulder = 0x0200;
    constexpr std::uint16_t kA = 0x1000;
    constexpr std::uint16_t kB = 0x2000;
    constexpr std::uint16_t kX = 0x4000;
    constexpr std::uint16_t kY = 0x8000;

    struct State
    {
        bool connected = false;
        float lx = 0.0f; // left stick, -1..1, deadzone applied and rescaled
        float ly = 0.0f;
        float rx = 0.0f;
        float ry = 0.0f;
        float lt = 0.0f; // triggers, 0..1
        float rt = 0.0f;
        std::uint16_t held = 0;
    };

    // Loop thread only. `deadzone` is a fraction of full stick deflection (0.05 .. 0.6).
    void poll(bool enabled, float deadzone);

    // Any thread.
    State state();

    // Any thread: the rising edges accumulated since the last call, then cleared.
    std::uint16_t take_pressed();

    // Any thread: drop the accumulated edges.
    void clear_pressed();

    // For the F2 panel / the log: "xinput1_4.dll" or "not loaded".
    const wchar_t* module_name();
} // namespace pad
