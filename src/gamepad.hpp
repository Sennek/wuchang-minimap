#pragma once

//
// gamepad - XInput for the full map.
//
// THREADING (lessons.md): `XInputGetState` is polled from `on_update`, i.e. the UE4SS
// EVENT-LOOP thread, in exactly the same place the keyboard is sampled with
// GetAsyncKeyState - never from the game thread and never from Present. The render
// thread only reads lock-free atomics published here.
//
// XInput is loaded dynamically (xinput1_4 -> 1_3 -> 9_1_0) rather than linked, so a
// machine without the redistributable simply reports "no pad" instead of failing to
// load the mod. Polling a DISCONNECTED slot is expensive (it can take a millisecond),
// so slots that answered ERROR_DEVICE_NOT_CONNECTED are only re-probed once a second -
// the standard XInput hygiene rule.
//
// Buttons arrive two ways: `held` is the current mask, and `pressed` ACCUMULATES the
// rising edges seen since the consumer last took them, so a button tapped between two
// frames can never be missed and can never be seen twice (take_pressed() exchanges the
// accumulator for 0).
//

#include <cstdint>

namespace pad
{
    // The XInput button bits, spelled out so <Xinput.h> is not needed here.
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

    // LOOP THREAD ONLY. `enabled == false` publishes a disconnected state and costs
    // nothing. `deadzone` is a fraction of full stick deflection (0.05 .. 0.6).
    void poll(bool enabled, float deadzone);

    // Any thread.
    State state();

    // Any thread: the rising edges accumulated since the last call, then cleared.
    std::uint16_t take_pressed();

    // Any thread: drop any accumulated edges (used when the map opens, so a press that
    // happened while the map was closed cannot fire on the first frame).
    void clear_pressed();

    // For the F2 panel / the log: "xinput1_4.dll" or "not loaded".
    const wchar_t* module_name();
} // namespace pad
