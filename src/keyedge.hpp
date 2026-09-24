#pragma once

//
// keyedge - PURE. When one hotkey binding fires, decided from two witnesses of its key.
//
// The loop thread samples the key as a LEVEL (GetAsyncKeyState). That sees a press only if
// the key is still down when a sample comes, and the loop thread is UE4SS's, shared with every
// other mod: a press can begin and end while it is busy elsewhere. The window proc, on the
// game thread, sees every key-down MESSAGE, stamped with the input's own time. A press fires
// once - on the level's rising edge, or from a stamped message whose press the level never
// saw down.
//
// No Windows here. Times are GetTickCount milliseconds; a message stamp is its low 32 bits.
//

#include <cstdint>

namespace kedge
{
    // A contact bounce or a key repeat of the SAME binding.
    constexpr std::uint64_t kDebounceMs = 250;
    // How late a press known only from its message may still fire. Past this the player has
    // pressed again or given up, and an action arriving now would be a surprise.
    constexpr std::uint32_t kMaxPressAgeMs = 2000;

    // One binding's memory. Every binding has its own, so two actions never debounce each
    // other.
    struct Edge
    {
        bool down = false;          // the level at the last sample
        std::uint64_t fired_ms = 0; // when it last fired
        std::uint32_t down_ms = 0;  // the last sample that saw the level down
        std::uint32_t press_ms = 0; // the newest message stamp already considered
    };

    // The newest key-down message for the binding's key. `eligible` is false when the
    // binding may not take it (a modifier it needs was up, a text box has the caret); the
    // press is still consumed, so it cannot fire once that changes.
    struct Press
    {
        bool any = false;
        bool eligible = false;
        std::uint32_t ms = 0;
    };

    // `a` strictly later than `b` on a millisecond clock that wraps every 49.7 days.
    inline bool later(std::uint32_t a, std::uint32_t b)
    {
        return static_cast<std::int32_t>(a - b) > 0;
    }

    // True exactly once per press. `sample_ms` is read AFTER the level, so the message of a
    // press the level saw down is never stamped later than the sample that saw it. The level
    // is recorded whatever the answer, so a key held down through a gate closing cannot fire
    // when the gate opens again.
    inline bool fired(Edge& e, bool level, Press press, std::uint64_t sample_ms)
    {
        const std::uint32_t now32 = static_cast<std::uint32_t>(sample_ms);
        const bool rising = level && !e.down;
        // Only while the key is up: a held key's press belongs to the level, and a repeat
        // that slipped through must not read as a new one.
        const bool missed = !level && press.any && press.eligible && press.ms != e.press_ms &&
                            now32 - press.ms <= kMaxPressAgeMs &&
                            (e.down_ms == 0 || later(press.ms, e.down_ms));
        const bool fire =
            (rising || missed) && (e.fired_ms == 0 || sample_ms - e.fired_ms > kDebounceMs);
        if (fire)
        {
            e.fired_ms = sample_ms;
        }
        if (level)
        {
            e.down_ms = now32;
        }
        if (press.any)
        {
            e.press_ms = press.ms;
        }
        e.down = level;
        return fire;
    }
} // namespace kedge
