//
// typing_gate - the loop thread's half of "a text box of ours has the caret".
//
// The mod samples hotkeys with GetAsyncKeyState on the loop thread, so a letter typed into
// the map's search box or the panel's import path reaches the bindings unless something
// vetoes it. Only ImGui knows a caret is up (imgui_caret.hpp, render thread) and its answer
// crosses threads once per rendered frame - this gate makes that answer safe to read at 60 Hz.
//

#pragma once

#include <cstdint>

namespace tgate
{
    // How long the loop thread keeps believing the last "a box has the caret" it was
    // told. It has to outlast the gap between two published frames at any frame rate a
    // player would type at, and it is what a player waits out after clicking away from a
    // box before a letter is a binding again - so a fraction of a second, not seconds.
    constexpr std::uint64_t kTypingHoldMs = 500;

    // The published flag as a level with a tail: between two frames it says nothing at all -
    // a dropped frame, a stall, or a Present that returned early leaves the last value
    // standing, and a word typed across such a gap would hand its letters back to the
    // bindings mid-word. Latching the last `true` for hold_ms closes that window.
    struct Latch
    {
        std::uint64_t until_ms = 0;
    };

    inline bool typing(Latch& l, bool want_now, std::uint64_t now_ms, std::uint64_t hold_ms = kTypingHoldMs)
    {
        if (want_now)
        {
            l.until_ms = now_ms + hold_ms;
            return true;
        }
        return l.until_ms != 0 && now_ms < l.until_ms;
    }
} // namespace tgate
