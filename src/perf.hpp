#pragma once

//
// perf - one lock-free counter per periodic activity.
//
// WHY
// ---
// This mod does periodic work on three threads (the UE4SS loop thread, the game
// thread inside the ProcessEvent pre-callback, and whichever thread calls Present)
// and the review of v0.9.1 found two costs that had been invisible for weeks: the
// 4 Hz widget sweep at 28-51 ms a call, and `publish_round`, which was never timed
// at all. Both were found by reading code, not by looking at a number.
//
// So every periodic activity registers a counter here and records how long each
// invocation took. The F2 debug block prints the table: name, rate, average, peak,
// last, thread. One screenshot then answers "what is this mod costing".
//
// THE CONSTRAINTS, from lessons.md
// --------------------------------
//   * NO std::mutex anywhere - it faults against this process's MSVCP140 the first
//     time the game thread touches it. This header uses plain relaxed atomics only.
//   * The game thread must not allocate or block. Registration allocates nothing
//     (the table is a fixed array of PODs) and recording is a handful of relaxed
//     atomic reads and writes with no CAS loop and no retry.
//   * The pure arithmetic lives here, in a header with no Windows, no D3D12 and no
//     UE4SS, so tests/markers_test.cpp covers it with the game closed.
//
// ACCURACY
// --------
// Recording is deliberately racy in one specific way: two threads recording into
// the SAME counter can interleave and lose a sample. That never happens in practice
// because each counter is owned by exactly one thread (the `thread` field says
// which), and even if it did, a diagnostic that loses one sample in a million is
// still a diagnostic. Nothing here is ever used to make a decision.
//

#include <cstddef>
#include <cstdint>

namespace perf
{
    // Which thread an activity belongs to. Printed in the table, and the reason a
    // counter needs no synchronisation: one writer each.
    enum class Thread : int
    {
        Unknown = 0,
        Loop = 1,   // UE4SS event loop (on_update)
        Game = 2,   // ProcessEvent pre-callback
        Render = 3, // the hooked Present
    };

    inline const char* thread_name(Thread t)
    {
        switch (t)
        {
        case Thread::Loop:
            return "loop";
        case Thread::Game:
            return "game";
        case Thread::Render:
            return "render";
        case Thread::Unknown:
        default:
            return "?";
        }
    }

    // The most counters the table can hold. Registration past this is dropped rather
    // than growing the table - a fixed array is what keeps recording allocation-free.
    constexpr int kMaxCounters = 32;

    // The window an average is taken over. A rolling reset (rather than a lifetime
    // mean) is what makes the table respond when something gets slower.
    constexpr std::uint64_t kWindowMs = 2000;

    // ONE COUNTER. It is a plain struct of scalars, not atomics, because the array
    // that holds them is the only thing shared and every element has a single writer.
    // The reader (the F2 panel, on the render thread) may see a half-updated row; the
    // numbers are refreshed many times a second, so a stale field for one frame is
    // not worth a lock.
    struct Counter
    {
        const char* name = nullptr;
        Thread thread = Thread::Unknown;

        std::uint64_t calls = 0;    // invocations since the mod loaded
        double last_ms = 0.0;       // the most recent invocation
        double peak_ms = 0.0;       // the worst since the mod loaded, stalls included
        double avg_ms = 0.0;        // mean over the last completed window
        double rate_hz = 0.0;       // invocations per second over the last window

        // THE PEAK THAT MEANS SOMETHING. A sample taken while the process was loading a
        // level, resizing the swapchain or doing a one-off blocking job (a reload, a
        // clipboard copy) is wall-clock time spent waiting for the game, not the cost of
        // this activity - and one such sample hides every later regression behind it,
        // exactly the way the "reset peaks" button exists to work around. So a stalled
        // sample still updates `peak_ms` and `last_ms` (nothing is hidden) but is counted
        // separately instead of setting the peak the table shows.
        double peak_calm_ms = 0.0;  // the worst sample taken outside a stall
        std::uint64_t stalls = 0;   // samples taken during one
        double peak_stall_ms = 0.0; // the worst of those

        // The window being accumulated right now.
        std::uint64_t win_start_ms = 0;
        std::uint64_t win_calls = 0;
        double win_total_ms = 0.0;
    };

    // The table. `count` only ever grows, and only at registration time.
    struct Table
    {
        Counter c[kMaxCounters]{};
        int count = 0;
    };

    // Registers a counter and returns its index, or -1 when the table is full.
    // `name` must have static storage - the table keeps the pointer.
    inline int register_counter(Table& t, const char* name, Thread thread)
    {
        if (name == nullptr || t.count >= kMaxCounters)
        {
            return -1;
        }
        // Registering the same name twice hands back the same counter, so a static
        // local `id` in each call site cannot be duplicated by a reload.
        for (int i = 0; i < t.count; ++i)
        {
            if (t.c[i].name == name)
            {
                return i;
            }
        }
        const int id = t.count;
        t.c[id] = Counter{};
        t.c[id].name = name;
        t.c[id].thread = thread;
        t.count = id + 1;
        return id;
    }

    // Records one invocation. `now_ms` is any monotonic millisecond clock; it only
    // ever decides when a window closes. `calm` is false when the process was known to
    // be stalled - loading, resizing, or doing a one-off blocking job - which routes the
    // sample to the stall columns instead of to the peak the table shows.
    inline void record(Table& t, int id, double ms, std::uint64_t now_ms, bool calm = true)
    {
        if (id < 0 || id >= t.count)
        {
            return;
        }
        Counter& c = t.c[id];
        ++c.calls;
        c.last_ms = ms;
        if (ms > c.peak_ms)
        {
            c.peak_ms = ms;
        }
        if (calm)
        {
            if (ms > c.peak_calm_ms)
            {
                c.peak_calm_ms = ms;
            }
        }
        else
        {
            ++c.stalls;
            if (ms > c.peak_stall_ms)
            {
                c.peak_stall_ms = ms;
            }
        }
        if (c.win_start_ms == 0)
        {
            c.win_start_ms = now_ms;
        }
        ++c.win_calls;
        c.win_total_ms += ms;

        const std::uint64_t elapsed = now_ms >= c.win_start_ms ? now_ms - c.win_start_ms : 0;
        if (elapsed >= kWindowMs)
        {
            c.avg_ms = c.win_calls > 0 ? c.win_total_ms / static_cast<double>(c.win_calls) : 0.0;
            c.rate_hz = elapsed > 0 ? static_cast<double>(c.win_calls) * 1000.0 / static_cast<double>(elapsed)
                                    : 0.0;
            c.win_start_ms = now_ms;
            c.win_calls = 0;
            c.win_total_ms = 0.0;
        }
    }

    // An activity that has stopped being called keeps showing its last window's rate
    // forever, which reads as "this is still running". Anything whose window is more
    // than a couple of windows stale is idle, and the table says so instead.
    inline bool idle(const Counter& c, std::uint64_t now_ms)
    {
        if (c.calls == 0)
        {
            return true;
        }
        const std::uint64_t since = now_ms >= c.win_start_ms ? now_ms - c.win_start_ms : 0;
        return since > kWindowMs * 3;
    }

    // The peak is the number that matters and it must be resettable, or one hitch
    // during loading hides every later regression behind it.
    inline void reset_peaks(Table& t)
    {
        for (int i = 0; i < t.count; ++i)
        {
            t.c[i].peak_ms = 0.0;
            t.c[i].peak_calm_ms = 0.0;
            t.c[i].peak_stall_ms = 0.0;
            t.c[i].stalls = 0;
        }
    }
} // namespace perf
