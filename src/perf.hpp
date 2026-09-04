#pragma once

//
// perf - one lock-free counter per periodic activity, printed by the F2 debug block
// as name / rate / average / peak / last / thread.
//
// Constraints:
//   * no std::mutex (it faults against this process's MSVCP140 on the game thread) -
//     plain relaxed atomics only;
//   * no allocation and no blocking: the table is a fixed array of PODs and recording
//     is a handful of relaxed loads and stores, no CAS loop, no retry;
//   * no Windows, D3D12 or UE4SS here, so tests cover the arithmetic offline.
//
// Recording is racy: two threads writing the SAME counter can lose a sample. Each
// counter has exactly one writer (the `thread` field), and nothing here feeds a
// decision.
//

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace perf
{
    // Which thread owns an activity - the reason a counter needs no synchronisation.
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

    // Table capacity. Registration past this is dropped; a fixed array is what keeps
    // recording allocation-free.
    constexpr int kMaxCounters = 32;

    // The rolling window an average and a rate are taken over.
    constexpr std::uint64_t kWindowMs = 2000;

    // Plain scalars, not atomics: every element has a single writer. The F2 panel may
    // read a half-updated row, which costs one frame of a stale field.
    struct Counter
    {
        const char* name = nullptr;
        Thread thread = Thread::Unknown;

        std::uint64_t calls = 0;    // invocations since the mod loaded
        double last_ms = 0.0;       // the most recent invocation
        double peak_ms = 0.0;       // the worst since the mod loaded, stalls included
        double avg_ms = 0.0;        // mean over the last completed window
        double rate_hz = 0.0;       // invocations per second over the last window

        // A sample taken during a stall (level load, swapchain resize, a one-off
        // blocking job) is wall-clock time waiting for the game, not this activity's
        // cost. It still updates `peak_ms` and `last_ms`, but is counted separately so
        // it cannot hide later regressions behind it.
        double peak_calm_ms = 0.0;  // the worst sample taken outside a stall
        std::uint64_t stalls = 0;   // samples taken during one
        double peak_stall_ms = 0.0; // the worst of those

        // The window being accumulated right now.
        std::uint64_t win_start_ms = 0;
        std::uint64_t win_calls = 0;
        double win_total_ms = 0.0;
    };

    // `count` only grows, at registration time, and is atomic because it publishes the
    // row: the F2 panel walks `[0, count)` on the render thread while a registration
    // can be running on the game thread.
    struct Table
    {
        Counter c[kMaxCounters]{};
        std::atomic<int> count{0};
    };

    // Registers a counter and returns its index, or -1 when the table is full.
    // `name` must have static storage - the table keeps the pointer.
    inline int register_counter(Table& t, const char* name, Thread thread)
    {
        if (name == nullptr)
        {
            return -1;
        }
        const int have = t.count.load(std::memory_order_acquire);
        // The same name hands back the same counter, so re-registering after a reload
        // cannot duplicate a row. Compared by CONTENT: one literal in two translation
        // units is two addresses. The lookup runs before the cap, so an already
        // registered activity keeps working once the table is full.
        for (int i = 0; i < have; ++i)
        {
            if (t.c[i].name != nullptr && std::strcmp(t.c[i].name, name) == 0)
            {
                return i;
            }
        }
        if (have >= kMaxCounters)
        {
            return -1;
        }
        const int id = have;
        t.c[id] = Counter{};
        t.c[id].name = name;
        t.c[id].thread = thread;
        // Release: everything above is visible to any reader that sees this count.
        t.count.store(id + 1, std::memory_order_release);
        return id;
    }

    // Records one invocation. `now_ms` is any monotonic millisecond clock and only
    // decides when a window closes. `calm == false` routes the sample to the stall
    // columns instead of the displayed peak.
    inline void record(Table& t, int id, double ms, std::uint64_t now_ms, bool calm = true)
    {
        if (id < 0 || id >= t.count.load(std::memory_order_acquire))
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

    // True once the current window is several windows stale, so a stopped activity
    // does not keep showing its last rate.
    inline bool idle(const Counter& c, std::uint64_t now_ms)
    {
        if (c.calls == 0)
        {
            return true;
        }
        const std::uint64_t since = now_ms >= c.win_start_ms ? now_ms - c.win_start_ms : 0;
        return since > kWindowMs * 3;
    }

    // Clears every peak and the stall count.
    inline void reset_peaks(Table& t)
    {
        const int have = t.count.load(std::memory_order_acquire);
        for (int i = 0; i < have; ++i)
        {
            t.c[i].peak_ms = 0.0;
            t.c[i].peak_calm_ms = 0.0;
            t.c[i].peak_stall_ms = 0.0;
            t.c[i].stalls = 0;
        }
    }
} // namespace perf
