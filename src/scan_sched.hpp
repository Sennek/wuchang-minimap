#pragma once

//
// scan_sched - the pure scheduling arithmetic behind the chunked GUObjectArray walk.
//
// The live marker sweep walks the object array once per round, in slices: each pump
// visits at most `chunk` consecutive slots, classifies them by `UClass*` against a
// memoised table, and the round is published when the cursor wraps.
//
// The arithmetic deciding "which slots this pump, has the round wrapped, is it time yet"
// is pure integer math with no engine types, so it lives here and is covered by
// tests/markers_test.cpp.
//
// TIME UNITS: microseconds. The pump samples QueryPerformanceCounter, not
// GetTickCount64: the slice period is on the order of one frame and the tick count only
// moves in ~15.6 ms steps.
//

#include <cstdint>

namespace scan
{
    //==================================================================================
    // Tunables and their clamps
    //==================================================================================

    // Objects visited per slice. 8192 slots is ~0.25-0.7 ms of walking; the cost is one
    // cache miss per object (the class pointer lives in the UObject itself, which the
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

    // The slots this pump should visit. `total` is the object array's CURRENT size, which
    // grows as levels stream in and can shrink after a GC compaction, so it is re-read
    // every pump and the cursor clamped against it.
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
    // array is classified and the draw buffer should be published. A slice that could not
    // be planned (total <= 0) also counts as a wrap.
    constexpr bool advance(Cursor& c, const Slice& s, int total) noexcept
    {
        if (total <= 0)
        {
            c.index = 0;
            c.visited = 0;
            ++c.round;
            return true;
        }
        // NO FORWARD PROGRESS IS STILL A ROUND. An empty slice would otherwise leave
        // c.index where it is and never wrap, so nothing would ever be published.
        if (s.count() <= 0)
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

    // Unsigned wrap-safe elapsed test. `last` == 0 means "never ran", which is always due.
    constexpr bool elapsed(std::uint64_t now_us, std::uint64_t last_us, std::uint64_t span_us) noexcept
    {
        return last_us == 0 || now_us < last_us || (now_us - last_us) >= span_us;
    }

    constexpr bool slice_due(std::uint64_t now_us, std::uint64_t last_slice_us, int period_ms) noexcept
    {
        return elapsed(now_us, last_slice_us, static_cast<std::uint64_t>(clamp_period_ms(period_ms)) * 1000ull);
    }

    // A round that finishes early does NOT immediately start the next one: the object
    // array walk is the most expensive thing this mod does on the game thread.
    constexpr bool round_due(std::uint64_t now_us, std::uint64_t round_start_us, int rounds_per_sec) noexcept
    {
        const int rps = rounds_per_sec < 1 ? 1 : (rounds_per_sec > 60 ? 60 : rounds_per_sec);
        return elapsed(now_us, round_start_us, static_cast<std::uint64_t>(1000000 / rps));
    }

    //==================================================================================
    // Diagnostics arithmetic (here so the F2 panel numbers are testable)
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
    // The discovery sweep is only ever needed to DISCOVER a menu root the reader has never
    // seen. gamestate.cpp keeps a watchlist of every widget that has ever confirmed as an
    // in-viewport `Visible` root (Wuchang constructs widgets lazily and then parks them
    // forever, so the object that held a menu open is the same object next time) and
    // re-tests that handful with the same authoritative test on every 10 Hz pump:
    //
    //   * a menu CLOSING              -> ~1 frame, the watchlist re-test a UI event on a
    //                                    watchlisted root asks for; <= 1 pump without one
    //   * a menu opening whose root
    //     is already on the watchlist -> the same
    //   * a menu opening whose root
    //     has never been seen         -> <= one sweep period + one round's walk time
    //
    // Only the third case depends on this schedule. It re-arms the FAST cadence for
    // `warm_ms` on every menu-state flip, teleport, world change, view-target change and
    // explicit force; once warm and quiet the period doubles per fruitless sweep up to
    // `slow_ms`, capped at `unknown_ms` while the watchlist is still empty.
    //
    // Nothing here is a latch: the sweep REBUILDS the open-root set and the watchlist only
    // supplies candidates.
    //
    // TIME UNITS: milliseconds (the reader's pump clock is GetTickCount64).

    struct SweepSched
    {
        // Tunables (from the config; clamped by the caller).
        std::uint64_t fast_ms = 250;  // cadence while armed
        std::uint64_t slow_ms = 2000; // cadence once warm and quiet
        std::uint64_t warm_ms = 2000; // how long an arm keeps the fast cadence

        // The cadence CAP while the watchlist is empty. An empty watchlist is the steady
        // state of ordinary gameplay, so it must not pin the cadence to fast_ms. The only
        // latency it bounds is the FIRST menu of a session, whose root has never been
        // seen; every other menu event is answered by the per-pump watchlist re-test.
        std::uint64_t unknown_ms = 1000;

        // State.
        std::uint64_t armed_until = 0; // fast cadence while now < armed_until
        std::uint64_t last_arm = 0;    // when the last arm happened, for the rate limit
        std::uint64_t next_at = 0;     // the sweep is due when now >= next_at
        int backoff = 0;               // doublings of fast_ms, 0 = none
        bool started = false;          // false until the first arm/complete
        bool nothing_known = false;    // the last completed sweep saw an empty watchlist
    };

    // The largest doubling ever applied. fast_ms << 8 is 64 s at the default, far past
    // slow_ms, so this only keeps the shift defined.
    constexpr int kSweepMaxBackoff = 8;

    // Re-arm the fast cadence and make a sweep due immediately. Called for every event
    // that can introduce an unseen menu root: start-up, a world or pawn change, a menu
    // state flip, a teleport, a root dropped from the cache.
    inline void sweep_arm(SweepSched& s, std::uint64_t now) noexcept
    {
        s.armed_until = now + s.warm_ms;
        s.next_at = now;
        s.last_arm = now;
        s.backoff = 0;
        s.started = true;
    }

    // The shortest gap between two arms on the UI-EVENT path. That path sees thousands of
    // calls a second, and an arm pins the walk to `fast_ms`: rounds then run back to back
    // and refill the candidate list faster than the per-pump commit drains it, which is how
    // an event storm turns into "no menu is ever confirmed".
    constexpr std::uint64_t kSweepArmMinGapMs = 250;

    // Arm unless one happened less than `min_gap_ms` ago; true when it armed. An arm never
    // disturbs a round already walking - `sweep_due` is consulted only between rounds - so
    // the limit bounds cost, never the answer.
    inline bool sweep_arm_limited(SweepSched& s, std::uint64_t now, std::uint64_t min_gap_ms) noexcept
    {
        // `started` is the "has ever been armed or completed" flag; a zero `last_arm` is a
        // real tick, not a sentinel, so the gap is measured against it either way.
        if (s.started && now >= s.last_arm && now - s.last_arm < min_gap_ms)
        {
            return false;
        }
        sweep_arm(s, now);
        return true;
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
        // An empty watchlist caps the period instead of pinning it to fast_ms. The cap is
        // never tighter than fast_ms, so a config with fast_ms above unknown_ms stands.
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
    //   discovered_new - it added a root the watchlist had never seen; stay fast.
    //   nothing_known  - the watchlist is EMPTY, so no menu can be detected cheaply. This
    //                    caps the cadence at unknown_ms rather than pinning it to fast_ms.
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
        // Both "stay fast" cases have just set backoff to 0, so this is fast_ms for them
        // and the doubled (then capped) period for everything else.
        s.next_at = now + sweep_period_ms(s, now);
    }

    //==================================================================================
    // The sliced widget walk the discovery sweep is built out of
    //==================================================================================
    //
    // The widget pass walks GUObjectArray in slices exactly like the marker scan -
    // `Cursor` / `next_slice` / `advance` above are shared verbatim - and a "sweep" is one
    // complete round of that walk. The round is started by the schedule above and
    // committed when the cursor wraps.
    //
    // 8192 slots per slice at ~8 ms spacing is ~1 M slots/s: a ~360 k-slot array is one
    // round per ~0.35 s at ~0.3-0.7 ms per slice. Same chunk as the marker scan - the
    // per-slot cost is one cache miss on the UObject's class pointer, so both walks have
    // identical economics.
    constexpr int kWidgetChunkDefault = 8192;
    constexpr int kWidgetSlicePeriodMs = 8;

    // How many byte-`Visible` widgets may be waiting for a commit at once. The candidate
    // population is every widget whose Visibility byte says `Visible`, not the ~5-6
    // in-viewport roots: this game leaves `Visibility` at `Visible` on widgets it has
    // removed from the viewport, and an open menu's child panels are `Visible` too.
    // Truncating cuts in object-array INDEX order, and a menu root is constructed lazily,
    // i.e. at a high index. 512 is a sanity cap; overflow is counted and logged.
    constexpr int kWidgetCandidateMax = 512;

    // How many `IsInViewport()` ProcessEvent calls a single 10 Hz pump may issue. The
    // remainder stays pending and is tested on the next pump, so a burst of candidates
    // costs latency rather than a frame.
    constexpr int kWidgetCommitPerPump = 128;

    // The menu answer, OR-ed from two sources that are BOTH fresh this pump. Neither is a
    // latch and neither waits for a sweep round: a pump on which no watchlisted root is in
    // the viewport and no candidate confirms publishes CLOSED, whatever the walk is doing
    // and even when the watchlist has just emptied because the root object died.
    //
    //   * the watchlist re-test - authoritative over every root ever confirmed, rebuilt
    //     from `IsInViewport()` on every pump;
    //   * this pump's commit of newly-seen candidates, which can only ADD a root.
    // Candidates are committed on the next validated pump rather than at the end of a
    // round: a menu that opens and closes inside a round would otherwise be byte-`Visible`
    // when the slice sees it and out of the viewport when the commit asks.
    constexpr bool menu_open_from(bool watchlist_open, bool commit_confirmed) noexcept
    {
        return watchlist_open || commit_confirmed;
    }


    //==================================================================================
    // In-viewport `Visible` roots that are not menus
    //==================================================================================
    //
    // "A menu is open" == "some in-viewport widget's Visibility is
    // ESlateVisibility::Visible". The gameplay HUD roots are all HitTestInvisible /
    // SelfHitTestInvisible so they never qualify, but transient combat furniture such as
    // the subtitle widget (`WB_ZiMu`, ZiMu = subtitles) is authored as plain `Visible`
    // and does.
    //
    // Hence a deny-list of class-name prefixes, matched case-insensitively. It fails safe
    // - an unknown root is still a menu - and every first-time root is logged by name.
    struct NonMenuRoot
    {
        const char* prefix;
        const char* why;
    };

    inline constexpr NonMenuRoot kNonMenuRoots[] = {
        {"WB_ZiMu", "subtitles (ZiMu = the game's own name for them)"},
        {"WB_Subtitle", "subtitles"},
        {"WB_Damage", "damage numbers"},
        {"WB_HurtNum", "damage numbers"},
        {"WB_InteractionTips", "the interaction prompt"},
        {"WB_Tips", "a tip / toast"},
        {"WB_Toast", "a toast"},
        {"WB_Tutorial", "a tutorial toast"},
        {"WB_Guide", "a tutorial toast"},
        {"WB_ShowAddItem", "the item-pickup toast"},
        {"WB_MainUI", "the gameplay HUD"},
        {"WB_HUD", "the gameplay HUD"},
        {"WB_AddressInfo", "the area-name banner"},
        {"WB_NPCBG", "the dialogue letterbox"},
        {"WB_GameSaving", "the autosave spinner"},
        {"WB_BossHp", "a boss health bar"},
        {"WB_BossBlood", "a boss health bar"},
        {"WB_AnimationSlot", "an animation wrapper, not a screen"},
    };

    // ASCII-only lowering. Widget class names in this game are ASCII (the CJK is in asset
    // paths and property names, never in a `WB_*_C` class name).
    template <class CharT>
    constexpr char ascii_lower(CharT c) noexcept
    {
        const unsigned v = static_cast<unsigned>(c);
        if (v > 127u)
        {
            return '\x01'; // never equal to any character of a prefix
        }
        const char ch = static_cast<char>(v);
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    template <class CharT>
    constexpr bool name_has_prefix_ci(const CharT* name, const char* prefix) noexcept
    {
        if (name == nullptr || prefix == nullptr || prefix[0] == '\0')
        {
            return false;
        }
        for (int i = 0; prefix[i] != '\0'; ++i)
        {
            if (name[i] == static_cast<CharT>(0))
            {
                return false;
            }
            if (ascii_lower(name[i]) != ascii_lower(prefix[i]))
            {
                return false;
            }
        }
        return true;
    }

    // Returns the REASON the class is not a menu, or nullptr when it could be one.
    template <class CharT>
    constexpr const char* builtin_non_menu_reason(const CharT* class_name) noexcept
    {
        if (class_name == nullptr)
        {
            return nullptr;
        }
        for (const NonMenuRoot& row : kNonMenuRoots)
        {
            if (name_has_prefix_ci(class_name, row.prefix))
            {
                return row.why;
            }
        }
        return nullptr;
    }

    // The player's own additions (`menu_ignore_roots`): the same prefix match over a
    // comma / semicolon / whitespace separated list.
    template <class CharT>
    inline bool extra_non_menu_match(const CharT* class_name, const char* list) noexcept
    {
        if (class_name == nullptr || list == nullptr)
        {
            return false;
        }
        int at = 0;
        while (list[at] != '\0')
        {
            while (list[at] == ',' || list[at] == ';' || list[at] == ' ' || list[at] == '\t')
            {
                ++at;
            }
            const int start = at;
            while (list[at] != '\0' && list[at] != ',' && list[at] != ';' && list[at] != ' ' &&
                   list[at] != '\t')
            {
                ++at;
            }
            const int len = at - start;
            if (len > 0)
            {
                bool all = true;
                for (int i = 0; i < len && all; ++i)
                {
                    if (class_name[i] == static_cast<CharT>(0) ||
                        ascii_lower(class_name[i]) != ascii_lower(list[start + i]))
                    {
                        all = false;
                    }
                }
                if (all)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // nullptr = this class may hold a menu.
    template <class CharT>
    inline const char* non_menu_root_reason(const CharT* class_name, const char* extra) noexcept
    {
        const char* why = builtin_non_menu_reason(class_name);
        if (why != nullptr)
        {
            return why;
        }
        return extra_non_menu_match(class_name, extra) ? "listed in menu_ignore_roots" : nullptr;
    }

    //==================================================================================
    // Menu root CLASS names, remembered across worlds
    //==================================================================================
    //
    // Every pointer-keyed cache in gamestate.cpp - the watchlist, the class memo, the
    // pending candidates - dies with the pawn's world, because a recycled address would
    // answer from the wrong entry. Class NAMES do not: `WB_MenuMain_C` holds the pause menu
    // in every chapter, so a name confirmed once is worth prioritising for the rest of the
    // session.
    //
    // This is a PRIORITY, never a latch: a candidate whose class is in the set still has to
    // pass the same `IsInViewport()` confirmation as any other. What it buys is position -
    // it leads the pending list, so the per-pump commit cap cannot push it behind a burst of
    // unknown candidates.
    //
    // The set is fixed-size and round-robin: the game has ~10 menu roots, and a set that
    // silently stopped learning would be worse than one that forgets its oldest entry.

    constexpr int kMenuRootNameChars = 64; // including the terminator
    constexpr int kMenuRootNamesMax = 24;

    struct MenuRootNames
    {
        char rows[kMenuRootNamesMax][kMenuRootNameChars]{};
        int count = 0;
        int next = 0; // the slot the next insertion evicts once the set is full
    };

    // Whole-string case-insensitive compare, not the prefix match the deny-list uses: a
    // remembered name is one exact class, not a family.
    template <class CharT>
    inline bool name_equals_ci(const CharT* name, const char* row) noexcept
    {
        if (name == nullptr || row == nullptr)
        {
            return false;
        }
        int i = 0;
        for (; row[i] != '\0'; ++i)
        {
            if (name[i] == static_cast<CharT>(0) || ascii_lower(name[i]) != ascii_lower(row[i]))
            {
                return false;
            }
        }
        return name[i] == static_cast<CharT>(0);
    }

    template <class CharT>
    inline bool menu_root_known(const MenuRootNames& s, const CharT* class_name) noexcept
    {
        if (class_name == nullptr)
        {
            return false;
        }
        for (int i = 0; i < s.count; ++i)
        {
            if (name_equals_ci(class_name, s.rows[i]))
            {
                return true;
            }
        }
        return false;
    }

    // True when the name was not already there. A name that does not fit a row, or that
    // carries a non-ASCII character, is simply not remembered - the set is an optimisation.
    template <class CharT>
    inline bool remember_menu_root(MenuRootNames& s, const CharT* class_name) noexcept
    {
        if (class_name == nullptr)
        {
            return false;
        }
        int n = 0;
        while (n < kMenuRootNameChars && class_name[n] != static_cast<CharT>(0))
        {
            if (static_cast<unsigned>(class_name[n]) > 127u)
            {
                return false;
            }
            ++n;
        }
        if (n <= 0 || n >= kMenuRootNameChars)
        {
            return false;
        }
        if (menu_root_known(s, class_name))
        {
            return false;
        }
        int slot = 0;
        if (s.count < kMenuRootNamesMax)
        {
            slot = s.count;
            ++s.count;
        }
        else
        {
            slot = s.next;
            s.next = (s.next + 1) % kMenuRootNamesMax;
        }
        for (int i = 0; i < n; ++i)
        {
            s.rows[slot][i] = ascii_lower(class_name[i]);
        }
        s.rows[slot][n] = '\0';
        return true;
    }

    // Where a newly seen candidate goes in the pending list. A widget whose class has
    // already confirmed as a menu root leads the list, behind the known ones already there;
    // everything else appends. `known_front` is how many known-class entries lead the list.
    constexpr int candidate_insert_at(int pending, int known_front, bool known_class) noexcept
    {
        if (pending <= 0)
        {
            return 0;
        }
        if (!known_class)
        {
            return pending;
        }
        if (known_front <= 0)
        {
            return 0;
        }
        return known_front < pending ? known_front : pending;
    }

    // Which entry a known-menu-class candidate DISPLACES when the pending list is already at
    // `kWidgetCandidateMax`. Returns the last unknown-class slot, or -1 when every entry is
    // itself a known class and the newcomer has to be refused. Without this the cap refuses
    // exactly the candidate most likely to be a menu root: the list fills, in object-array
    // index order, with byte-`Visible` junk long before a lazily constructed menu root.
    constexpr int candidate_evict_at(int pending, int known_front) noexcept
    {
        if (pending <= 0 || known_front >= pending)
        {
            return -1;
        }
        return pending - 1;
    }

    // The slots of `pending` a pump takes, given the per-pump cap.
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
