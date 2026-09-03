#pragma once

//
// scan_sched - the pure scheduling arithmetic behind the chunked GUObjectArray walk.
//
// WHY IT IS A SEPARATE HEADER
// ---------------------------
// The live marker sweep used to run one `UObjectGlobals::FindAllOf` per game-thread
// pump, once per marker class. Every `FindAllOf` walks the WHOLE object array, so a
// round cost ~10 full walks and each single walk was measured in-game at 28.30 ms
// average / 51.05 ms peak - two to three dropped frames, ten times a second.
//
// The replacement walks the object array ONCE per round, in slices: each pump visits
// at most `chunk` consecutive slots, classifies them by `UClass*` against a memoised
// table, and the round is published when the cursor wraps. That turns one 28 ms stall
// into ~50 slices of well under a millisecond.
//
// All of the arithmetic that decides "which slots this pump, has the round wrapped,
// is it time yet" is pure integer math with no engine types, so it lives here and is
// covered by tests/markers_test.cpp - the only place anything about this mod can be
// verified without burning a play session.
//
// TIME UNITS: microseconds everywhere. The pump samples QueryPerformanceCounter, not
// GetTickCount64, because the slice period is on the order of one frame and the tick
// count only moves in ~15.6 ms steps.
//

#include <cstdint>

namespace scan
{
    //==================================================================================
    // Tunables and their clamps
    //==================================================================================

    // Objects visited per slice. 8192 slots is ~0.25-0.7 ms of walking (the cost is one
    // cache miss per object: the class pointer lives in the UObject itself, which the
    // sequential item array does not prefetch).
    constexpr int kChunkDefault = 8192;
    constexpr int kChunkMin = 512;
    constexpr int kChunkMax = 131072;

    // Minimum spacing between slices. The pump is called from the ProcessEvent
    // pre-callback, which fires thousands of times a second, so this - not the caller -
    // sets the scan rate. 8 ms is ~125 slices/s, i.e. ~1 M objects/s.
    constexpr int kPeriodDefaultMs = 8;
    constexpr int kPeriodMinMs = 1;
    constexpr int kPeriodMaxMs = 500;

    constexpr int clamp_chunk(int v) noexcept
    {
        return v < kChunkMin ? kChunkMin : (v > kChunkMax ? kChunkMax : v);
    }

    constexpr int clamp_period_ms(int v) noexcept
    {
        return v < kPeriodMinMs ? kPeriodMinMs : (v > kPeriodMaxMs ? kPeriodMaxMs : v);
    }

    //==================================================================================
    // The cursor
    //==================================================================================

    struct Cursor
    {
        int index = 0;           // next slot to visit
        int visited = 0;         // slots visited so far in the current round
        std::uint64_t round = 0; // completed rounds
    };

    struct Slice
    {
        int begin = 0;
        int end = 0; // exclusive

        constexpr int count() const noexcept
        {
            return end - begin;
        }
        constexpr bool empty() const noexcept
        {
            return end <= begin;
        }
    };

    // The slots this pump should visit. `total` is the object array's CURRENT size,
    // which grows as levels stream in and can shrink after a GC compaction - so it is
    // re-read every pump and the cursor is clamped against it rather than remembered.
    constexpr Slice next_slice(const Cursor& c, int total, int chunk) noexcept
    {
        Slice s{};
        if (total <= 0 || chunk <= 0)
        {
            return s;
        }
        s.begin = (c.index < 0 || c.index >= total) ? 0 : c.index;
        const int room = total - s.begin;
        s.end = s.begin + (chunk < room ? chunk : room);
        return s;
    }

    // Advance past a visited slice. Returns true when the round wrapped, i.e. the whole
    // array has now been classified and the draw buffer should be published.
    //
    // A slice that could not be planned (total <= 0) also counts as a wrap: with no
    // objects there is nothing to find, and pretending the round never ends would stall
    // publishing forever.
    constexpr bool advance(Cursor& c, const Slice& s, int total) noexcept
    {
        if (total <= 0)
        {
            c.index = 0;
            c.visited = 0;
            ++c.round;
            return true;
        }
        c.visited += s.count();
        c.index = s.end;
        if (c.index >= total)
        {
            c.index = 0;
            c.visited = 0;
            ++c.round;
            return true;
        }
        return false;
    }

    //==================================================================================
    // Rate gates
    //==================================================================================

    // Unsigned wrap-safe elapsed test. `last` == 0 means "never ran", which is always
    // due - QPC never legitimately reads 0 for our purposes because the pump stores
    // `now_us | 1`-free values only after the first call.
    constexpr bool elapsed(std::uint64_t now_us, std::uint64_t last_us, std::uint64_t span_us) noexcept
    {
        return last_us == 0 || now_us < last_us || (now_us - last_us) >= span_us;
    }

    constexpr bool slice_due(std::uint64_t now_us, std::uint64_t last_slice_us, int period_ms) noexcept
    {
        return elapsed(now_us, last_slice_us, static_cast<std::uint64_t>(clamp_period_ms(period_ms)) * 1000ull);
    }

    // A round that finished early does NOT immediately start the next one: refreshing
    // the marker set more often than `rounds_per_sec` buys nothing and the object array
    // walk is the single most expensive thing this mod does on the game thread.
    constexpr bool round_due(std::uint64_t now_us, std::uint64_t round_start_us, int rounds_per_sec) noexcept
    {
        const int rps = rounds_per_sec < 1 ? 1 : (rounds_per_sec > 60 ? 60 : rounds_per_sec);
        return elapsed(now_us, round_start_us, static_cast<std::uint64_t>(1000000 / rps));
    }

    //==================================================================================
    // Diagnostics arithmetic (kept here so the F2 panel numbers are testable too)
    //==================================================================================

    struct RoundStats
    {
        double total_ms = 0.0; // summed slice time of the round
        double peak_ms = 0.0;  // worst single slice of the round
        int slices = 0;
        int objects = 0;

        constexpr double avg_ms() const noexcept
        {
            return slices > 0 ? total_ms / static_cast<double>(slices) : 0.0;
        }
    };

    inline void note_slice(RoundStats& r, double slice_ms, int objects) noexcept
    {
        r.total_ms += slice_ms;
        if (slice_ms > r.peak_ms)
        {
            r.peak_ms = slice_ms;
        }
        ++r.slices;
        r.objects += objects;
    }
} // namespace scan

namespace scan
{
    //==================================================================================
    // The adaptive schedule of the menu-widget discovery sweep
    //==================================================================================
    //
    // The discovery pass is a whole-object-array walk and it ran every 250 ms purely so
    // a menu opening would be noticed quickly. As one `FindAllOf("UserWidget")` call
    // that measured 28.30 ms / 51.05 ms peak when the schedule was written and 25.471 ms
    // average / 62.554 ms peak in the 2026-09-03 run - a tenth of the game thread, and a
    // dropped frame every time it fired. It is now a SLICED GUObjectArray walk (see
    // kWidgetChunkDefault below), so the burst is gone; this schedule is what keeps the
    // per-second total small on top of that.
    //
    // It can be backed off because the sweep is only ever needed to DISCOVER a menu root
    // the reader has never seen. gamestate.cpp keeps a watchlist of every widget that
    // has ever confirmed as an in-viewport `Visible` root (Wuchang constructs its widgets
    // lazily and then parks them forever, so the object that held a menu open is the same
    // object the next time that menu opens) and re-tests that handful with the SAME
    // authoritative test on every 10 Hz pump. So:
    //
    //   * a menu CLOSING              -> <= 1 pump  (~100 ms), watchlist re-test
    //   * a menu opening whose root
    //     is already on the watchlist -> <= 1 pump  (~100 ms), watchlist re-test
    //   * a menu opening whose root
    //     has never been seen         -> <= one sweep period + one round's walk time
    //
    // Only the third case depends on this schedule, and it is bounded by re-arming the
    // FAST cadence on every menu-state flip, teleport, world change, view-target change
    // and explicit force, for `warm_ms`. Once warm and quiet the period doubles per
    // fruitless sweep up to `slow_ms`, capped at `unknown_ms` while the watchlist is
    // still empty.
    //
    // Nothing here is a latch: the sweep still REBUILDS the open-root set, and the
    // watchlist only ever supplies candidates - the answer is derived live every pump.
    //
    // TIME UNITS: milliseconds (the reader's pump clock is GetTickCount64).

    struct SweepSched
    {
        // Tunables (from the config; clamped by the caller).
        std::uint64_t fast_ms = 250;  // cadence while armed
        std::uint64_t slow_ms = 2000; // cadence once warm and quiet
        std::uint64_t warm_ms = 2000; // how long an arm keeps the fast cadence

        // The cadence CAP while the watchlist is empty. `nothing_known` used to pin the
        // schedule to `fast_ms` outright, on the reasoning that with nothing to re-test
        // cheaply the discovery pass is the only latency bound there is. In-game that
        // reasoning was inverted by the data: a player who has not opened a menu yet has
        // an EMPTY watchlist, which is the steady state of ordinary gameplay - so the
        // 2026-09-03 run 2 log reads `sweep every 250 ms (watchlist 0)` on every single
        // state line for the whole session, and the discovery pass never backed off once
        // (25.5 ms average / 62.6 ms peak on the game thread at 3 Hz = a tenth of the
        // game thread and a dropped frame three times a second). An empty watchlist now
        // buys a bounded cadence rather than the fastest one; the only latency it bounds
        // is "the FIRST menu of a session, whose root has never been seen", and every
        // menu the player notices - a close, a re-open, a known menu - is answered by the
        // per-pump watchlist re-test one pump (~100 ms) after it happens.
        std::uint64_t unknown_ms = 1000;

        // State.
        std::uint64_t armed_until = 0; // fast cadence while now < armed_until
        std::uint64_t next_at = 0;     // the sweep is due when now >= next_at
        int backoff = 0;               // doublings of fast_ms, 0 = none
        bool started = false;          // false until the first arm/complete
        bool nothing_known = false;    // the last completed sweep saw an empty watchlist
    };

    // The largest doubling we will ever apply. fast_ms << 8 is 64 s at the default, far
    // past slow_ms, so this only exists to keep the shift defined.
    constexpr int kSweepMaxBackoff = 8;

    // Re-arm the fast cadence and make a sweep due immediately. Called for every event
    // that can have introduced a menu root we have never seen: start-up, a world or pawn
    // change, a menu state flip, a teleport, a root dropped from the cache.
    inline void sweep_arm(SweepSched& s, std::uint64_t now) noexcept
    {
        s.armed_until = now + s.warm_ms;
        s.next_at = now;
        s.backoff = 0;
        s.started = true;
    }

    inline bool sweep_armed(const SweepSched& s, std::uint64_t now) noexcept
    {
        return now < s.armed_until;
    }

    // The cadence that applies right now, before any doubling is decided.
    inline std::uint64_t sweep_period_ms(const SweepSched& s, std::uint64_t now) noexcept
    {
        if (sweep_armed(s, now) || s.backoff <= 0)
        {
            return s.fast_ms;
        }
        const int shift = s.backoff > kSweepMaxBackoff ? kSweepMaxBackoff : s.backoff;
        std::uint64_t p = s.fast_ms << shift;
        if (p > s.slow_ms)
        {
            p = s.slow_ms;
        }
        // An empty watchlist caps the period instead of pinning it to fast_ms. The cap
        // can never be tighter than fast_ms itself, so a config that sets fast_ms above
        // unknown_ms is still honoured rather than silently sped up.
        if (s.nothing_known && p > s.unknown_ms)
        {
            p = s.unknown_ms < s.fast_ms ? s.fast_ms : s.unknown_ms;
        }
        return p;
    }

    inline bool sweep_due(const SweepSched& s, std::uint64_t now) noexcept
    {
        return !s.started || now >= s.next_at;
    }

    // Record that a sweep has just run.
    //   discovered_new - it added a root the watchlist had never seen; something is
    //                    changing, so stay fast.
    //   nothing_known  - the watchlist is EMPTY, so no menu could be detected cheaply.
    //                    That no longer pins the cadence (see SweepSched::unknown_ms) -
    //                    it caps it, so the backoff still runs and an idle reader with
    //                    no menu ever opened settles at unknown_ms instead of fast_ms.
    inline void sweep_done(SweepSched& s, std::uint64_t now, bool discovered_new,
                           bool nothing_known) noexcept
    {
        s.started = true;
        s.nothing_known = nothing_known;
        if (discovered_new || sweep_armed(s, now))
        {
            s.backoff = 0;
        }
        else if (s.backoff < kSweepMaxBackoff)
        {
            ++s.backoff;
        }
        // Both "stay fast" cases have just set backoff to 0, so this is fast_ms for
        // them and the doubled (then capped) period for everything else.
        s.next_at = now + sweep_period_ms(s, now);
    }

    //==================================================================================
    // The SLICED widget walk that the discovery sweep is built out of now
    //==================================================================================
    //
    // The sweep itself used to be one `UObjectGlobals::FindAllOf(L"UserWidget")` call,
    // i.e. a whole-object-array walk in a single game-thread pump - and the in-game
    // number for it was 25.471 ms average / 62.554 ms peak (F2 -> Debug, 2026-09-03).
    // That is the same mistake the marker sweep already had to unlearn: the total work
    // is fine, the BURST is not.
    //
    // So the widget pass now walks GUObjectArray in slices exactly like the marker
    // scan - `Cursor` / `next_slice` / `advance` above are shared verbatim - and a
    // "sweep" is one complete round of that walk rather than one call. The round is
    // started by the schedule above and committed when the cursor wraps.
    //
    // Budget: 8192 slots per slice at ~8 ms spacing is ~1 M slots/s, so a ~360 k-slot
    // array is one round per ~0.35 s at ~0.3-0.7 ms per slice - a number that cannot be
    // seen in a frame time. The chunk is deliberately the same as the marker scan's
    // default: the per-slot cost is one cache miss on the UObject's class pointer, so
    // the two walks have identical economics and one measured chunk size serves both.
    constexpr int kWidgetChunkDefault = 8192;
    constexpr int kWidgetSlicePeriodMs = 8;

    // How many byte-`Visible` widgets may be waiting for a commit at once.
    //
    // THIS WAS 64 AND THAT IS WHY MENUS STOPPED BEING DETECTED. The number was taken
    // from "the game only ever has 5-6 in-viewport roots out of ~1 700 instances" - but
    // the candidate list is not the in-viewport roots, it is every widget whose
    // **Visibility byte** says `Visible`, which is a completely different population:
    // this game leaves `Visibility` at `Visible` on widgets it has REMOVED from the
    // viewport (that is the whole reason `IsInViewport()` is the authoritative test), and
    // an open menu's child panels are `Visible` too. A cap sized from the in-viewport
    // count therefore truncated the candidate list in object-array INDEX order - and a
    // menu root is constructed lazily, i.e. LATE, i.e. at a high index. So the one widget
    // the pass existed to find was the most likely one to be cut.
    //
    // 512 is a sanity cap, not a working limit, and overflow is still counted and logged.
    constexpr int kWidgetCandidateMax = 512;

    // How many `IsInViewport()` ProcessEvent calls a single 10 Hz pump may issue. The
    // remainder stays pending and is tested on the next pump, so a burst of candidates
    // costs latency rather than a frame.
    constexpr int kWidgetCommitPerPump = 128;

    // THE MENU ANSWER, and the reason it is a function.
    //
    // The 2026-09-03 rework made "a sweep" mean one complete ROUND of the sliced walk and
    // committed the round's candidates only when the cursor wrapped - which put up to a
    // whole round (~350 ms on the fast path, seconds if the reader is only slicing at
    // 10 Hz) between reading a widget's Visibility byte and asking it `IsInViewport()`.
    // A menu that opens and closes inside that window is byte-`Visible` when the slice
    // sees it and out of the viewport when the commit asks - so it is never confirmed,
    // never joins the watchlist, and every later opening of that same menu is missed as
    // well. The old whole-array `FindAllOf` sweep did both reads in the same instant and
    // could not have this failure.
    //
    // So candidates are committed on the very next validated pump instead of at the end
    // of the round, and the answer has two sources that are BOTH fresh this pump:
    //   * the watchlist re-test - the complete, authoritative answer over every root ever
    //     confirmed, rebuilt from `IsInViewport()` on every pump;
    //   * this pump's commit of newly-seen candidates, which can only ADD a root (and
    //     adds it to the watchlist at the same time).
    // Neither is a cached value, so OR-ing them is not the latch `lessons.md` condemns -
    // that rule is about OR-ing a STALE answer over a fresh one, and there is no stale
    // answer left in this design.
    constexpr bool menu_open_from(bool watchlist_open, bool commit_confirmed) noexcept
    {
        return watchlist_open || commit_confirmed;
    }

    // The slots of `pending` a pump takes, given the per-pump cap. Pure so the latency
    // contract can be arithmetic in a test rather than a claim in a comment.
    constexpr int commit_batch(int pending, int cap) noexcept
    {
        if (pending <= 0 || cap <= 0)
        {
            return 0;
        }
        return pending < cap ? pending : cap;
    }

    // Worst-case pumps needed to drain `pending` at `cap` per pump.
    constexpr int commit_pumps_needed(int pending, int cap) noexcept
    {
        if (pending <= 0)
        {
            return 0;
        }
        if (cap <= 0)
        {
            return -1; // never
        }
        return (pending + cap - 1) / cap;
    }
} // namespace scan
