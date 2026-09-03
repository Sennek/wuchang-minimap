#include "gamestate.hpp"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chapterid.hpp"
#include "mapdata.hpp"
#include "markers.hpp"
#include "markers_db.hpp"
#include "mem.hpp"
#include "mmstate.hpp"
#include "scan_sched.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

namespace gamestate
{
    namespace
    {
        //==============================================================================
        // Tuning
        //==============================================================================

        // THE DEFAULTS. Every one of these is a config key now (`reader_*` in
        // config_wuchang_minimap.txt) and the live values live in `g_tune` below, which
        // is refreshed from the config twice a second. The constants stay as the
        // documented defaults and as the values used before the first refresh.
        constexpr std::uint64_t kPositionPeriodMs = 100; // 10 Hz: pawn location + yaw
        constexpr std::uint64_t kResolvePeriodMs = 500;  // 2 Hz: FindAllOf for pawn / controller
        // The menu test runs at two rates, because "the minimap hides 2-3 s after I
        // open the inventory" was traced straight to it running at 2 Hz:
        //   * EVERY pump (10 Hz): re-read the reflected Visibility byte of the handful
        //     of root widgets we have already seen in the viewport. That is a few
        //     GUObjectArray checks and a few byte reads - no FindAllOf, no allocation,
        //     no ProcessEvent - and it is what makes hiding immediate.
        //   * every kWidgetFullPeriodMs AT MOST: one round of the DISCOVERY walk, which
        //     is the only thing that can find a root the first time a given menu is
        //     opened in a session (a widget that has never been Visible has never been
        //     IsInViewport()-tested, so it cannot be on the watchlist yet).
        //
        // That round used to be a single FindAllOf("UserWidget") - a whole-object-array
        // walk, measured in-game at 25.471 ms average / 62.554 ms peak on the game thread
        // (F2 -> Debug, 2026-09-03) - and it is now a SLICED GUObjectArray walk of 8192
        // slots per slice (widget_scan_pump / commit_widget_round below). This value is
        // the fast cadence of its ADAPTIVE schedule (scan::SweepSched): the period doubles
        // up to reader_widget_sweep_max_period_ms while nothing new is discovered, and any
        // menu-state flip / teleport / world change / view-target change puts it back to
        // fast. What bounds the latency instead is the per-pump pass, which re-tests EVERY
        // root ever seen - see menu_from_cached_roots().
        constexpr std::uint64_t kWidgetFullPeriodMs = 250;
        // Sanity cap on one FindAllOf pass - the fallback path only (the sliced walk is
        // bounded by the object array itself and by scan::kWidgetCandidateMax).
        constexpr std::size_t kMaxWidgets = 6000;
        constexpr std::size_t kMaxMenuRoots = 32;  // cache cap; the game only ever has 5-6

        // After ANY pawn / world change, no UFunction is called for this long. A level
        // transition destroys the old pawn, its world and everything cached off them;
        // the engine is mid-LoadMap and calling into a blueprint getter then is exactly
        // what produced the EXCEPTION_ACCESS_VIOLATION in read_location().
        constexpr std::uint64_t kTransitionCooldownMs = 2000;

        constexpr std::uint64_t kLogThrottleMs = 5000;
        // The floor for the "this class is not a gameplay pawn" line when the class NAME
        // changes from one rejection to the next - see resolve_pawn.
        constexpr std::uint64_t kRejectFlipThrottleMs = 1000;

        // How often the streamed level set is walked to name the chapter. A chapter can
        // only change across a loading screen, so this is slow on purpose: it is ~50
        // GetFullName() calls, and the answer is only consumed by an asset swap that
        // itself takes seconds.
        constexpr std::uint64_t kChapterPeriodMs = 1000;
        constexpr int kMaxLevelsScanned = 4096;

        // The player's own pawn class, from the step-1 recon
        // (context/wuchang-classes.md). The controller is the fallback route.
        constexpr const wchar_t* kPlayerPawnClass = L"BP_CombatCharacter_Player_Final_C";
        constexpr const wchar_t* kControllerClass = L"DCSPlayerController_C";

        // CLASS GATE. Only a pawn whose class name contains this is ever read: the
        // Lobby / main menu pawn is a `DefaultPawn` and spectators are
        // `SpectatorPawn`, and reading either of those is what made the minimap appear
        // at the main menu (their location falls inside Chapter 1's bounds).
        constexpr const wchar_t* kGameplayPawnSubstr = L"BP_CombatCharacter_Player";

        //==============================================================================
        // State (game thread only, except the atomics)
        //==============================================================================

        std::atomic<bool> g_registered{false};
        std::atomic<std::uint64_t> g_pump_calls{0};
        // FIX (A.1): THE GAME-THREAD LATCH. The pump had a thread_local re-entrancy
        // guard but nothing that compared the CALLING thread against the one that owns
        // its state (contrast mm::set_loop_thread / the overlay's render-thread latch).
        // The engine issues ProcessEvent from async-loading, audio and worker threads
        // too, and every one of those entered with depth == 0 and mutated g_pawn,
        // g_menu_watch, g_wcursor and the two unordered_maps concurrently with the game
        // thread. Prevents: a data race on heap-allocated containers, i.e. the
        // corrupted-free-list HARD DUMPLESS HANG signature in lessons.md. The id is
        // latched by the first call that gets past the re-entrancy guard - the pump is
        // registered from the game thread's own init and the next ProcessEvent is its.
        std::atomic<unsigned long> g_pump_thread{0};
        std::atomic<bool> g_wrong_thread_logged{false};
        // WHAT THE GAME THREAD IS DOING, for the loop thread's stall watchdog. A
        // relaxed store of a pointer to a string literal - no allocation, no lock, and
        // nothing that can itself stall. It is deliberately coarse: the point is to name
        // the STEP a freeze happened in, not to profile.
        std::atomic<const char*> g_pump_stage{"start"};

        struct StageMark
        {
            explicit StageMark(const char* s) noexcept
            {
                g_pump_stage.store(s, std::memory_order_relaxed);
            }
        };
        std::atomic<std::uint64_t> g_publishes{0};
        std::atomic<bool> g_report_pending{false};

        uer::LayoutCache g_layouts;
        uer::FuncCache g_funcs;

        uer::ObjRef g_pawn{};
        uer::ObjRef g_controller{};
        bool g_pawn_is_gameplay = false;
        const void* g_world = nullptr; // the pawn's UWorld*, as a transition token
        // FIX (A.22): THE LAST WORLD WE EVER SAW, which drop_pawn does NOT clear.
        // `g_world` has to go to nullptr on a drop (it is the level-transition token and
        // a stale one would defeat the world-change test), but
        // controller_from_game_instance() starts from a world - so with only one variable
        // the recovery route the log claims to take (UWorld -> OwningGameInstance ->
        // LocalPlayers[0] -> PlayerController) could never run after a drop and every
        // recovery fell through to FindFirstOf, which is what returned a controller from a
        // dying world and stalled the reader for 86 s. Prevents: that stall.
        const void* g_world_token = nullptr;

        std::uint64_t g_last_position = 0;
        // ONE BUDGET PER THING RESOLVED. These used to be a single `g_last_resolve`
        // shared by the controller step and the pawn step, and that is what left the
        // overlay reading `pawn NO` forever after a fast travel taken from the shrine
        // menu (2026-09-03 run 1, 15:46:01 onwards): the controller step runs FIRST and
        // stamps the shared timestamp, so whenever the controller cannot be re-captured
        // the pawn step's `now - g_last_resolve >= resolve_ms` was false on every single
        // pump and `resolve_pawn()` was never called again - silently, because none of
        // its log lines can fire from a function that is not entered. Two independent
        // timestamps mean the pawn is re-acquired at 2 Hz whatever the controller does,
        // which is the honest dependency: `FindAllOf(BP_CombatCharacter_Player_Final_C)`
        // is the primary route and needs no controller at all.
        std::uint64_t g_last_resolve_pawn = 0;
        std::uint64_t g_last_resolve_ctrl = 0;
        std::uint64_t g_cooldown_until = 0;
        std::uint64_t g_state_ok_since = 0;

        std::uint64_t g_log_no_pawn = 0;
        std::uint64_t g_log_rejected = 0;
        std::uint64_t g_log_wrong_world = 0;
        std::uint64_t g_log_capture_failed = 0;
        std::uint64_t g_log_no_controller = 0;
        std::uint64_t g_no_pawn_since = 0;
        std::wstring g_rejected_class;
        // How long the reader tolerates having no gameplay pawn before it stops trusting
        // its cached PlayerController and re-resolves that too (a fast travel can leave
        // an alive-but-wrong controller whose `Pawn` is null forever), and before the
        // one-shot diagnosis is printed. Both are constants rather than config keys: they
        // are recovery timings, not preferences.
        constexpr std::uint64_t kNoPawnCtrlDropMs = 3000;
        constexpr std::uint64_t kNoPawnDiagnoseMs = 5000;
        std::uint64_t g_last_ctrl_drop = 0;
        bool g_pawnless_diag_done = false;

        bool g_menu_open = true; // safe default: "a menu is up" hides the minimap
        std::uint64_t g_menu_change_ms = 0;
        std::uint32_t g_widgets_seen = 0;
        std::uint32_t g_widgets_visible = 0;

        // Root widgets that were once confirmed `IsInViewport()`. Re-tested every pump:
        // the Visibility byte as a cheap prefilter AND `IsInViewport()` as the actual
        // answer - see the comment on menu_from_cached_roots() for why the byte alone
        // latched the minimap off forever.
        std::vector<uer::ObjRef> g_menu_roots;

        // EVERY widget that has ever confirmed as an in-viewport `Visible` root this
        // session. Wuchang constructs its widgets lazily and then parks them forever
        // (lessons.md: instance counts only grow, 763 -> 1696 as menus are first
        // opened), so the object that held a menu open IS the object the next time that
        // menu opens. Re-testing this handful with the authoritative test on every pump
        // is what lets the discovery sweep back off - see scan::SweepSched.
        //
        // It is a CANDIDATE list, never an answer: `g_menu_roots` (the open roots) is
        // rebuilt from live tests every pump, so nothing here can latch. Entries leave
        // only when the object dies or the world changes.
        std::vector<uer::ObjRef> g_menu_watch;

        // The adaptive cadence of the discovery sweep (pure arithmetic in
        // scan_sched.hpp, tested offline).
        scan::SweepSched g_sweep{};

        //------------------------------------------------------------------------------
        // The SLICED discovery walk (replaces FindAllOf("UserWidget"))
        //------------------------------------------------------------------------------
        //
        // The discovery pass was one `FindAllOf(L"UserWidget")` per sweep, and the F2
        // perf table measured it in-game at **25.471 ms average / 62.554 ms peak** on the
        // game thread at 3 Hz - a dropped frame three times a second. It is exactly the
        // failure the marker sweep already fixed: `FindAllOf` walks the whole object
        // array in one call, so the cure is to walk GUObjectArray ourselves in slices and
        // call the ROUND the sweep.
        //
        // Two halves, split by what is safe where:
        //   * the SLICE (`widget_scan_pump`) does raw reads only - FUObjectItem validity,
        //     the class pointer, a memoised "is this a UUserWidget descendant?" and the
        //     reflected Visibility byte - so it can ride the between-position-pumps fast
        //     path next to the marker slice, hundreds of times a second, at ~0.3-0.7 ms.
        //     Not one ProcessEvent is issued from it.
        //   * the COMMIT (`commit_widget_round`) runs on the 10 Hz pump, where the pawn
        //     has just been validated in this very call, and is the only place that calls
        //     `IsInViewport()` - over the handful of byte-`Visible` candidates the round
        //     collected, which is 5-6 widgets out of ~900.
        //
        // The answer is still REBUILT from the commit, never merged, so none of the three
        // latches this file has had can come back.
        scan::Cursor g_wcursor{};
        bool g_wround_active = false;      // a round is walking right now
        bool g_wround_ready = false;       // a round has wrapped and awaits the commit
        std::uint64_t g_wslice_us = 0;     // QPC of the last slice
        std::uint32_t g_wseen_round = 0;   // UserWidget instances this round has seen
        std::uint32_t g_wcand_dropped = 0; // candidates the cap refused this round
        std::uint32_t g_wcand_round = 0;   // byte-Visible candidates this round produced
        // Candidates are held as ObjRefs, not raw pointers: even one pump of delay is
        // enough for a widget to die, and the commit issues a ProcessEvent at them.
        //
        // THEY ARE COMMITTED ON THE NEXT VALIDATED PUMP, NOT AT THE END OF THE ROUND.
        // Waiting for the round put up to ~350 ms (and, when only the 10 Hz pump is
        // slicing, several seconds) between reading a widget's Visibility byte and asking
        // it `IsInViewport()` - and a menu that opens and closes inside that window is
        // byte-Visible for the slice and out of the viewport for the commit, so it is
        // never confirmed and never joins the watchlist, which then misses every later
        // opening of the same menu too. See scan::menu_open_from.
        std::vector<uer::ObjRef> g_wpending;
        // The same three numbers, published for the loop thread's 10 s state line. The
        // vector and the counters above are game-thread-only, so the log reads copies.
        std::atomic<std::uint32_t> g_wcand_round_pub{0};
        std::atomic<std::uint32_t> g_wpending_pub{0};
        std::atomic<std::uint32_t> g_wcand_dropped_pub{0};
        // "Is this UClass* a UUserWidget descendant?" - one super-chain name walk per
        // class per level, then a hash lookup per object. The cap has to clear the whole
        // GAME's class count, not the widget classes': this walk asks about every class
        // that owns an object, so a small cap would be hit mid-round and would throw away
        // exactly the negative answers that make the walk cheap (lessons.md).
        std::unordered_map<RC::Unreal::UClass*, unsigned char> g_wclass;
        constexpr std::size_t kWidgetClassCacheMax = 262144;
        // Class NAMES whose not-a-menu line has already been printed. Separate from
        // g_wclass on purpose: that map is a performance cache and is dropped on every
        // level transition and on any pawn change, so riding on it turned a
        // once-per-class diagnostic into the same thirteen lines seven times over in a
        // 41-minute session. This set is never cleared - it is a few dozen short
        // strings and it is what makes "once per session" true.
        std::unordered_set<std::wstring> g_non_menu_logged;
        // Set once if `FUObjectArray::GetNumElements()` cannot answer - i.e. UE4SS did not
        // resolve GUObjectArray on this build. The old whole-array `FindAllOf` sweep stays
        // in the file as that fallback, so a menu is still detected (expensively) rather
        // than never.
        bool g_wfallback = false;

        // Perf counter ids (perf.hpp). Namespace-scope ints rather than function
        // statics: a guarded static's first call would run the CRT's thread-safe-init
        // path on the game thread.
        int g_pf_position = -1;
        int g_pf_sweep = -1;
        int g_pf_wslice = -1;
        int g_pf_wcommit = -1;
        int g_pf_retest = -1;
        int g_pf_chapter = -1;

        // The level set last handed to markers::set_loaded_levels(). Cleared whenever
        // the marker module's caches are dropped, so the next enumeration always
        // republishes into an empty g_levels.
        std::vector<std::string> g_last_levels;

        // The root that is currently holding "a menu is open" true, for the F2 debug
        // block and the log. Empty when no root is visible.
        std::wstring g_menu_holder;

        // Set whenever the menu state flips, a root leaves the viewport or the pawn
        // teleports: the next pump runs the full FindAllOf sweep instead of waiting up
        // to kWidgetFullPeriodMs, so the cached-root list is re-validated and rebuilt
        // from live state.
        bool g_force_widget_sweep = true;

        // The previous pump's "is the view target the pawn?" answer. A change in it arms
        // the discovery walk - see the comment at the read site.
        bool g_last_pawn_view = true;

        // Teleport detection. A shrine fast-travel keeps the same pawn object and the
        // same UWorld, so none of the transition tests fire - but every position-derived
        // cache (the height-slice window, the smoothed feet Z) must be re-armed, and the
        // widget set changes as the fast-travel menu tears down.
        double g_last_x = 0.0;
        double g_last_y = 0.0;
        double g_last_z = 0.0;
        bool g_have_last_pos = false;
        std::uint64_t g_teleport_ms = 0;
        // uu of movement inside one 100 ms pump that can only be a teleport (the player
        // sprints at ~700 uu/s, i.e. ~70 uu per pump).
        constexpr double kTeleportJumpUu = 3000.0;

        //==============================================================================
        // The live copies of the constants above (game thread)
        //==============================================================================
        //
        // mm::config() copies the whole Config under a spinlock, which is far too much
        // for a callback the engine fires thousands of times a second - so it is read at
        // most every kTunePeriodMs and unpacked into these scalars. Everything below
        // uses `g_tune`, never the constants.

        struct Tunables
        {
            std::uint64_t position_ms = kPositionPeriodMs;
            std::uint64_t resolve_ms = kResolvePeriodMs;
            std::uint64_t widget_sweep_ms = kWidgetFullPeriodMs;
            std::uint64_t cooldown_ms = kTransitionCooldownMs;
            std::uint64_t log_throttle_ms = kLogThrottleMs;
            std::uint64_t chapter_ms = kChapterPeriodMs;
            std::size_t max_widgets = kMaxWidgets;
            std::size_t max_menu_roots = kMaxMenuRoots;
            int max_levels = kMaxLevelsScanned;
            double teleport_uu = kTeleportJumpUu;
        };

        constexpr std::uint64_t kTunePeriodMs = 500;

        Tunables g_tune{};
        std::uint64_t g_tune_ms = 0;

        void refresh_tunables(std::uint64_t now)
        {
            if (g_tune_ms != 0 && now - g_tune_ms < kTunePeriodMs)
            {
                return;
            }
            g_tune_ms = now;
            const mm::Config& cfg = mm::cfg_cached();
            g_tune.position_ms = static_cast<std::uint64_t>(cfg.reader_position_period_ms);
            g_tune.resolve_ms = static_cast<std::uint64_t>(cfg.reader_resolve_period_ms);
            g_tune.widget_sweep_ms = static_cast<std::uint64_t>(cfg.reader_widget_sweep_period_ms);
            g_sweep.fast_ms = g_tune.widget_sweep_ms;
            g_sweep.slow_ms = static_cast<std::uint64_t>(cfg.reader_widget_sweep_max_period_ms);
            g_sweep.warm_ms = static_cast<std::uint64_t>(cfg.reader_widget_sweep_warm_ms);
            g_tune.cooldown_ms = static_cast<std::uint64_t>(cfg.reader_transition_cooldown_ms);
            g_tune.log_throttle_ms = static_cast<std::uint64_t>(cfg.reader_log_throttle_ms);
            g_tune.chapter_ms = static_cast<std::uint64_t>(cfg.reader_chapter_period_ms);
            g_tune.teleport_uu = cfg.reader_teleport_jump_uu;
        }

        std::wstring g_pawn_class_name;
        std::wstring g_pawn_full_name;
        std::wstring g_pawn_short_name;

        // Chapter detection (see chapterid.hpp for why it is the streamed level set and
        // not the player's position).
        std::uint64_t g_last_chapter = 0;
        int g_chapter = chid::kNone;
        int g_chapter_levels = 0;
        // Which enumeration route worked, so one log line answers "how did it find the
        // levels" instead of a debugging session: 0 = none yet, 1 = UWorld::Levels,
        // 2 = UWorld::StreamingLevels -> ULevelStreaming::LoadedLevel, 3 = FindAllOf.
        int g_chapter_route = 0;

        // A "SAY IT ONCE, THEN RARELY" THROTTLE, for a condition that can persist for
        // minutes. `reader_log_throttle_ms` (5 s) is right for a condition that flickers;
        // it is wrong for one that simply stays true - "no gameplay pawn" printed 41
        // lines in run 5 saying the same thing. Here the FIRST occurrence always prints,
        // and after that at most one line per kPersistentLogMs, carrying how many were
        // suppressed in between. `rare_reset` is called when the condition clears, so the
        // next occurrence is a first occurrence again.
        constexpr std::uint64_t kPersistentLogMs = 30000;

        struct Rare
        {
            std::uint64_t last = 0;
            std::uint64_t suppressed = 0;
            bool seen = false;
        };

        bool rare(Rare& r, std::uint64_t now)
        {
            if (!r.seen)
            {
                r.seen = true;
                r.last = now;
                return true;
            }
            if (now - r.last < kPersistentLogMs)
            {
                ++r.suppressed;
                return false;
            }
            r.last = now;
            return true;
        }

        // "" or ", N more suppressed" - and it clears the counter, so it must be called
        // exactly once per line that `rare()` let through.
        std::wstring rare_note(Rare& r)
        {
            if (r.suppressed == 0)
            {
                return {};
            }
            const std::wstring note = std::format(L", {} more suppressed", r.suppressed);
            r.suppressed = 0;
            return note;
        }

        void rare_reset(Rare& r)
        {
            r = Rare{};
        }

        // The two conditions that can hold for a whole loading screen, and one that -
        // if it ever fires - holds for the whole session.
        Rare g_rare_ctrl_drop;
        Rare g_rare_stuck;
        Rare g_rare_wcap;
        Rare g_rare_rootcap;

        bool throttled(std::uint64_t& last, std::uint64_t now)
        {
            if (now - last < g_tune.log_throttle_ms)
            {
                return false;
            }
            last = now;
            return true;
        }

        void copy_to(wchar_t* dst, std::size_t cap, const std::wstring& src)
        {
            if (cap == 0)
            {
                return;
            }
            const std::size_t n = (src.size() < cap - 1) ? src.size() : cap - 1;
            std::memcpy(dst, src.data(), n * sizeof(wchar_t));
            dst[n] = L'\0';
        }

        //==============================================================================
        // Dropping everything that is keyed to the pawn
        //==============================================================================
        //
        // Called whenever the pawn pointer, its class or its world stops matching what
        // we captured. Every cached UFunction* and property offset was looked up on the
        // dead object's class, so all of it goes, and no UFunction is issued again for
        // kTransitionCooldownMs.

        void drop_pawn(std::uint64_t now, const wchar_t* why)
        {
            const bool had_pawn = !g_pawn.empty();
            g_pawn.reset();
            g_pawn_is_gameplay = false;
            g_world = nullptr; // g_world_token deliberately keeps the old value (A.22)
            g_pawn_class_name.clear();
            g_pawn_full_name.clear();
            g_pawn_short_name.clear();
            g_state_ok_since = 0;
            // THE MENU STATE IS NOT ALLOWED TO OUTLIVE ITS EVIDENCE. This used to latch
            // `true` here as a "safe default", and with the watchlist and the root cache
            // both emptied on the same line there was nothing left that could ever flip
            // it back: run 1's log read `menu OPEN (last change 138734 ms ago, 0 cached
            // root(s))` for the rest of the session. A menu is open because a widget
            // says so; with zero widgets the only honest answer is CLOSED. Nothing is
            // shown on the strength of it either - the pawn gate and the grace timer are
            // what keep the overlay off at the main menu and across a load.
            g_menu_open = false;
            g_menu_change_ms = now;
            g_menu_roots.clear(); // the widgets belonged to the world that just went
            g_menu_watch.clear();
            // The sliced discovery walk keyed everything it holds to that world too: the
            // candidate ObjRefs, the round's counts and the UClass* memo (classes are
            // unloaded with their packages, and a recycled UClass* address would answer
            // from the wrong entry). The cursor restarts rather than resuming mid-array.
            g_wpending.clear();
            g_wclass.clear();
            g_wcursor = scan::Cursor{};
            g_wround_active = false;
            g_wround_ready = false;
            g_wseen_round = 0;
            g_wcand_dropped = 0;
            g_wcand_round = 0;
            g_widgets_seen = 0;
            g_widgets_visible = 0;
            // The controller belonged to that world too. Resetting it here (rather than
            // waiting for uer::alive() to notice) is what re-arms its own resolve.
            g_controller.reset();
            g_menu_holder.clear();
            g_force_widget_sweep = true;
            g_have_last_pos = false;
            g_last_chapter = 0; // re-detect the chapter as soon as a pawn is back
            g_funcs.clear();
            g_layouts.clear();
            // The marker sweep caches class layouts, class classifications, per-object
            // ids and live actors - all of it keyed to the world that just went.
            markers::drop_caches();
            g_last_levels.clear();
            if (had_pawn)
            {
                g_cooldown_until = now + g_tune.cooldown_ms;
                mm::logf(L"pawn dropped ({}): caches cleared, no UFunction calls for {} ms",
                         why,
                         g_tune.cooldown_ms);
            }
        }

        //==============================================================================
        // Resolving the controller and the pawn
        //==============================================================================

        // Raw property read only - no ProcessEvent, no allocation. This is the cheap
        // per-pump cross-check: if the controller's own Pawn pointer no longer equals
        // the object we cached, our pointer is stale even if GUObjectArray has not
        // caught up yet.
        UObject* pawn_from_controller()
        {
            if (!uer::alive(g_controller))
            {
                return nullptr;
            }
            const uer::ClassLayout* layout = g_layouts.get(g_controller.obj);
            // Three names, in order of how directly they mean "the pawn this controller
            // is driving right now". `AcknowledgedPawn` is the one that survives a
            // re-possession the client has not been told about yet, and `Character` is
            // what a game that subclasses ACharacter often keeps its own pointer in - a
            // shrine fast travel left `Pawn` null for 86 s in the user's round-4 session
            // while the controller itself was alive the whole time.
            UObject* pawn = uer::read_object_prop(layout, g_controller.obj, L"Pawn");
            if (pawn == nullptr)
            {
                pawn = uer::read_object_prop(layout, g_controller.obj, L"AcknowledgedPawn");
            }
            if (pawn == nullptr)
            {
                pawn = uer::read_object_prop(layout, g_controller.obj, L"Character");
            }
            return pawn;
        }

        // THE WORLD'S OWN IDEA OF THE PLAYER CONTROLLER: UWorld -> OwningGameInstance ->
        // LocalPlayers[0] -> PlayerController. All raw property reads, no ProcessEvent.
        //
        // WHY IT IS PREFERRED OVER FindFirstOf. `FindFirstOf(L"DCSPlayerController_C")`
        // returns whichever instance the object array holds first, which after a travel
        // can be one belonging to a world that is being torn down - it passes every
        // liveness test (the allocation is still there and still says it is valid) and
        // then reports a null `Pawn` forever. The GameInstance chain is the only route
        // that answers "the controller the game is driving THIS world with".
        UObject* controller_from_game_instance(bool& from_stale_world)
        {
            // The live world if there is one, otherwise the last one we saw (A.22): after
            // a drop that is the only route that can name the controller the game is
            // driving, and every read below is guarded.
            from_stale_world = false;
            const void* start = g_world;
            if (start == nullptr)
            {
                start = g_world_token;
                from_stale_world = start != nullptr;
            }
            if (start == nullptr)
            {
                return nullptr;
            }
            UObject* world = static_cast<UObject*>(const_cast<void*>(start));
            if (!mem::readable(world, 0x40))
            {
                return nullptr;
            }
            const uer::ClassLayout* wl = g_layouts.get(world);
            UObject* gi = uer::read_object_prop(wl, world, L"OwningGameInstance");
            if (gi == nullptr)
            {
                return nullptr;
            }
            const uer::ClassLayout* gl = g_layouts.get(gi);
            // TArray<T> is { T* Data; int32 Num; int32 Max } - 16 bytes, which is also
            // what FArrayProperty reports, so the read is size-checked.
            struct ArrRaw
            {
                void* data = nullptr;
                std::int32_t num = 0;
                std::int32_t max = 0;
            };
            ArrRaw arr{};
            if (!uer::read_prop(gl, gi, L"LocalPlayers", arr, static_cast<int>(sizeof(ArrRaw))))
            {
                return nullptr;
            }
            if (arr.num <= 0 || arr.num > 8 || arr.max < arr.num || !mem::plausible_ptr(arr.data) ||
                !mem::readable(arr.data, sizeof(void*)))
            {
                return nullptr;
            }
            void* raw = nullptr;
            if (!mem::read_at(arr.data, 0, raw) || !mem::plausible_ptr(raw))
            {
                return nullptr;
            }
            UObject* local_player = static_cast<UObject*>(raw);
            // FIX (A.9): LocalPlayers[0] came out of a raw read validated only by
            // plausible_ptr, and g_layouts.get() dereferences it immediately
            // (GetClassPrivate) OUTSIDE any SEH guard. Prevents: an access violation on a
            // half-torn-down GameInstance during a level transition.
            if (!mem::readable(local_player, 0x40))
            {
                return nullptr;
            }
            const uer::ClassLayout* ll = g_layouts.get(local_player);
            return uer::read_object_prop(ll, local_player, L"PlayerController");
        }

        void resolve_controller()
        {
            // The world's own answer first; FindFirstOf is the fallback, because it can
            // hand back a controller from a world that has already gone. WHICH ROUTE
            // ANSWERED IS LOGGED (A.22) - the old line named the GameInstance chain while
            // the code could only ever have used FindFirstOf.
            bool stale_world = false;
            const wchar_t* route = L"UWorld -> OwningGameInstance -> LocalPlayers[0]";
            UObject* controller = controller_from_game_instance(stale_world);
            if (stale_world && controller != nullptr)
            {
                route = L"the LAST KNOWN world -> OwningGameInstance -> LocalPlayers[0]";
            }
            if (controller == nullptr)
            {
                route = L"FindFirstOf(DCSPlayerController_C)";
                controller = UObjectGlobals::FindFirstOf(kControllerClass);
            }
            if (controller == nullptr)
            {
                route = L"FindFirstOf(PlayerController)";
                controller = UObjectGlobals::FindFirstOf(L"PlayerController");
            }
            if (controller == nullptr || !UObjectGlobals::IsValidObjectForFindXOf(controller))
            {
                g_controller.reset();
                if (throttled(g_log_no_controller, ::GetTickCount64()))
                {
                    mm::logf(L"no player controller in the world (needs '{}' or any 'PlayerController') - "
                             L"the pawn is resolved by class instead",
                             kControllerClass);
                }
                return;
            }
            uer::ObjRef ref{};
            if (uer::capture(controller, ref))
            {
                g_controller = ref;
                // VERBOSE: it is one line per controller resolve, and the route is the
                // whole point of the fix.
                MM_LOGV(L"player controller resolved through {}", route);
            }
            else
            {
                g_controller.reset();
                if (throttled(g_log_no_controller, ::GetTickCount64()))
                {
                    mm::log(L"player controller found but not capturable (GUObjectArray says it is not a "
                            L"live object) - the pawn is resolved by class instead");
                }
            }
        }

        // ONE-SHOT DIAGNOSTIC FOR "NO GAMEPLAY PAWN, FOREVER".
        //
        // The user's round-4 report is `gameplay pawn NO ... held 86 s` after a shrine
        // fast travel, with `teleport never this session` (so the position-jump detector
        // never saw the travel either) and the controller alive throughout. Nothing in the
        // log said WHICH of the three routes was empty, so this prints all of them once:
        // the controller and what its three pawn properties hold, every instance of the
        // player class with its outer chain, and the world.
        void log_pawnless_diagnosis()
        {
            const bool ctrl_alive = uer::alive(g_controller);
            mm::logf(L"no-pawn diagnosis: controller {} ({}), world {}",
                     ctrl_alive ? g_controller.obj->GetFullName() : std::wstring{L"NOT resolved"},
                     ctrl_alive ? uer::class_name(g_controller) : std::wstring{L"-"},
                     g_world == nullptr ? std::wstring{L"none"} : std::format(L"{}", g_world));
            if (ctrl_alive)
            {
                const uer::ClassLayout* layout = g_layouts.get(g_controller.obj);
                for (const wchar_t* name : {L"Pawn", L"AcknowledgedPawn", L"Character"})
                {
                    UObject* p = uer::read_object_prop(layout, g_controller.obj, name);
                    uer::ObjRef ref{};
                    if (p != nullptr && uer::capture(p, ref))
                    {
                        mm::logf(L"no-pawn diagnosis:   {} -> {} (class {})",
                                 name,
                                 ref.obj->GetFullName(),
                                 uer::class_name(ref));
                    }
                    else
                    {
                        mm::logf(L"no-pawn diagnosis:   {} -> {}",
                                 name,
                                 p == nullptr ? L"null" : L"not a live object");
                    }
                }
                const uer::ClassLayout* cl = g_layouts.get(g_controller.obj);
                mm::logf(L"no-pawn diagnosis:   the controller class has {} readable propert(ies)",
                         cl != nullptr ? cl->props.size() : 0u);
            }
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(kPlayerPawnClass, found);
            mm::logf(L"no-pawn diagnosis: FindAllOf('{}') -> {} instance(s)",
                     kPlayerPawnClass,
                     found.size());
            int listed = 0;
            for (UObject* obj : found)
            {
                if (obj == nullptr || listed >= 8)
                {
                    continue;
                }
                ++listed;
                uer::ObjRef ref{};
                const bool ok = uer::capture(obj, ref);
                mm::logf(L"no-pawn diagnosis:   {} ({}, {})",
                         mem::readable(obj, 0x40) ? obj->GetFullName() : std::wstring{L"<unreadable>"},
                         ok ? L"capturable" : L"NOT capturable",
                         UObjectGlobals::IsValidObjectForFindXOf(obj) ? L"valid for FindXOf"
                                                                      : L"rejected by IsValidObjectForFindXOf");
            }
            if (found.empty())
            {
                mm::log(L"no-pawn diagnosis:   (the player class has no instance at all - the pawn was "
                        L"destroyed, or the class was renamed on this build)");
            }
        }

        void resolve_pawn(std::uint64_t now)
        {
            // EVERY ROUTE IS TRIED, AND THE FIRST CANDIDATE THAT PASSES THE CLASS GATE
            // WINS - not the first candidate that exists.
            //
            // The controller's own pointers come first: `FindAllOf` returns whichever
            // instance the object array holds first, which after a travel can be a pawn
            // belonging to a world that is being torn down, while the controller names the
            // pawn it is actually driving. But the controller is also the route that hands
            // out the Lobby `DefaultPawn`, and the old code RETURNED on the first
            // candidate whose class was wrong - so preferring the controller without
            // trying the next route would have swapped one stall for another.
            UObject* candidates[3] = {nullptr, nullptr, nullptr};
            candidates[0] = pawn_from_controller();
            {
                std::vector<UObject*> found;
                UObjectGlobals::FindAllOf(kPlayerPawnClass, found);
                for (UObject* obj : found)
                {
                    if (obj != nullptr && UObjectGlobals::IsValidObjectForFindXOf(obj))
                    {
                        candidates[1] = obj;
                        break;
                    }
                }
            }
            // Last resort: the substring gate, in case the exact class name moved on this
            // build. `FindAllOf` matches subclasses, so the base name is enough.
            if (candidates[0] == nullptr && candidates[1] == nullptr)
            {
                std::vector<UObject*> found;
                UObjectGlobals::FindAllOf(kGameplayPawnSubstr, found);
                for (UObject* obj : found)
                {
                    if (obj != nullptr && UObjectGlobals::IsValidObjectForFindXOf(obj))
                    {
                        candidates[2] = obj;
                        break;
                    }
                }
            }

            // FIX (A.23): THE WORLD CROSS-CHECK. resolve_pawn adopted the first
            // class-matching candidate whatever world it belonged to, and then set
            // `g_world` FROM THAT PAWN - so the world-change test in the pump compared the
            // pawn's world against a token derived from the same pawn and could never
            // fire. The controller is resolved independently (and preferentially from the
            // world's own GameInstance chain), so its world is the reference: a pawn from a
            // world that is being torn down is rejected, and the token comes from the
            // controller rather than from the pawn. Prevents: adopting a dying world's pawn
            // and never noticing the level changed.
            const void* ref_world = uer::alive(g_controller) ? uer::world_of(g_controller) : nullptr;

            uer::ObjRef ref{};
            std::wstring cls;
            const void* cand_world = nullptr;
            bool have = false;
            bool any_candidate = false;
            bool any_capturable = false;
            for (UObject* candidate : candidates)
            {
                if (candidate == nullptr)
                {
                    continue;
                }
                any_candidate = true;
                uer::ObjRef r{};
                if (!uer::capture(candidate, r))
                {
                    continue;
                }
                any_capturable = true;
                const std::wstring c = uer::class_name(r);
                if (c.find(kGameplayPawnSubstr) == std::wstring::npos)
                {
                    // A CHANGED CLASS NAME IS NOT A LICENCE TO LOG EVERY PUMP. This was
                    // `c != g_rejected_class || throttled(...)`, so two candidate classes
                    // alternating (the Lobby DefaultPawn and a spectator, which is exactly
                    // what a loading screen produces) wrote a line at the full 2 Hz resolve
                    // rate. A new class still reports promptly - just not ten times a
                    // second.
                    const bool changed = c != g_rejected_class;
                    if (now - g_log_rejected >= (changed ? kRejectFlipThrottleMs
                                                         : g_tune.log_throttle_ms))
                    {
                        g_rejected_class = c;
                        g_log_rejected = now;
                        mm::logf(L"pawn candidate class '{}' is not a gameplay pawn (needs '{}') - "
                                 L"trying the next route",
                                 c.empty() ? std::wstring{L"<unknown>"} : c,
                                 kGameplayPawnSubstr);
                    }
                    continue;
                }
                const void* w = uer::world_of(r);
                if (ref_world != nullptr && w != nullptr && w != ref_world)
                {
                    if (throttled(g_log_wrong_world, now))
                    {
                        mm::logf(L"pawn candidate '{}' belongs to another UWorld than the player "
                                 L"controller ({} vs {}) - it is a leftover from a level that is "
                                 L"being torn down; trying the next route",
                                 c,
                                 w,
                                 ref_world);
                    }
                    continue;
                }
                ref = r;
                cls = c;
                cand_world = w;
                have = true;
                break;
            }

            if (!any_candidate)
            {
                if (throttled(g_log_no_pawn, now))
                {
                    mm::log(L"no player pawn in the world (main menu / loading) - reader idling");
                }
                return;
            }
            if (!any_capturable)
            {
                if (throttled(g_log_capture_failed, now))
                {
                    mm::log(L"pawn candidate found but not capturable (GUObjectArray says it is not a live "
                            L"object yet) - retrying");
                }
                return;
            }
            if (!have)
            {
                // Every candidate was rejected by the class gate or by the world check;
                // both log their own line.
                return;
            }

            g_pawn = ref;
            g_pawn_is_gameplay = true;
            g_pawn_class_name = cls;
            g_pawn_short_name = ref.obj->GetName();
            g_pawn_full_name = ref.obj->GetFullName();
            // The controller's world when there is one (A.23): a token taken from the pawn
            // itself cannot detect that the pawn's world changed.
            g_world = ref_world != nullptr ? ref_world : cand_world;
            if (g_world != nullptr)
            {
                g_world_token = g_world;
            }
            g_funcs.clear();
            g_layouts.clear();
            g_rejected_class.clear();
            mm::logf(L"gameplay pawn acquired: {} (class {}, object index {})",
                     g_pawn_full_name,
                     g_pawn_class_name,
                     g_pawn.index);
        }

        //==============================================================================
        // Location / rotation
        //==============================================================================
        //
        // Both routes are only ever reached with a pawn that passed uer::alive() in this
        // same pump, outside the transition cooldown, and every ProcessEvent inside
        // uer::call_getter runs in an SEH guard.

        bool read_location(UObject* pawn, double& x, double& y, double& z, float& yaw, bool& via_function)
        {
            uer::FVec3 loc{};
            uer::FRot3 rot{};
            const bool got_loc = uer::call_getter(g_funcs, pawn, L"K2_GetActorLocation", loc);
            const bool got_rot = uer::call_getter(g_funcs, pawn, L"K2_GetActorRotation", rot);
            if (got_loc && std::isfinite(loc.x) && std::isfinite(loc.y) && std::isfinite(loc.z))
            {
                x = loc.x;
                y = loc.y;
                z = loc.z;
                yaw = got_rot && std::isfinite(rot.yaw) ? static_cast<float>(rot.yaw) : 0.0f;
                via_function = true;
                return true;
            }

            // Fallback: the root component's relative transform. For a pawn whose root
            // capsule has no attach parent that IS the world transform.
            const uer::ClassLayout* pawn_layout = g_layouts.get(pawn);
            UObject* root = uer::read_object_prop(pawn_layout, pawn, L"RootComponent");
            if (root == nullptr)
            {
                return false;
            }
            const uer::ClassLayout* root_layout = g_layouts.get(root);
            uer::FVec3 rel{};
            uer::FRot3 rrot{};
            if (!uer::read_prop(root_layout, root, L"RelativeLocation", rel, static_cast<int>(sizeof(uer::FVec3))))
            {
                return false;
            }
            if (!std::isfinite(rel.x) || !std::isfinite(rel.y) || !std::isfinite(rel.z))
            {
                return false;
            }
            x = rel.x;
            y = rel.y;
            z = rel.z;
            yaw = 0.0f;
            if (uer::read_prop(root_layout, root, L"RelativeRotation", rrot, static_cast<int>(sizeof(uer::FRot3))) &&
                std::isfinite(rrot.yaw))
            {
                yaw = static_cast<float>(rrot.yaw);
            }
            via_function = false;
            return true;
        }

        //==============================================================================
        // View target
        //==============================================================================
        //
        // lessons.md: APlayerCameraManager has NO GetViewTarget(); the view target lives
        // on the PlayerController. The camera manager does expose a readable `ViewTarget`
        // struct property whose first field is the AActor*, which is the fallback here.
        // Comparison is by pointer identity - the Lua `==` trap does not apply in C++.

        UObject* read_view_target()
        {
            if (!uer::alive(g_controller))
            {
                return nullptr;
            }
            UObject* controller = g_controller.obj;
            struct RetActor
            {
                void* actor = nullptr;
            } ret{};
            if (uer::call_getter(g_funcs, controller, L"GetViewTarget", ret) && mem::plausible_ptr(ret.actor))
            {
                return static_cast<UObject*>(ret.actor);
            }

            const uer::ClassLayout* layout = g_layouts.get(controller);
            UObject* pcm = uer::read_object_prop(layout, controller, L"PlayerCameraManager");
            if (pcm == nullptr)
            {
                return nullptr;
            }
            const uer::ClassLayout* pcm_layout = g_layouts.get(pcm);
            const uer::Prop* vt = uer::find_prop(pcm_layout, L"ViewTarget");
            if (vt == nullptr)
            {
                return nullptr;
            }
            void* target = nullptr;
            if (!mem::read_at(pcm, vt->offset, target) || !mem::plausible_ptr(target))
            {
                return nullptr;
            }
            return static_cast<UObject*>(target);
        }

        //==============================================================================
        // The menu test
        //==============================================================================
        //
        // From the step-1 recon: only *root* widgets are ever IsInViewport() (5-6 of
        // ~1700 instances), the gameplay HUD roots are all HitTestInvisible /
        // SelfHitTestInvisible, and every menu adds exactly one root whose visibility is
        // ESlateVisibility::Visible. So "a menu is open" == "some in-viewport widget is
        // Visible".
        //
        // Cost control: UWidget::Visibility is a reflected TEnumAsByte, so the byte is
        // read straight out of every instance and only the handful that say Visible pay
        // for a ProcessEvent call to IsInViewport(). ESlateVisibility::Visible == 0.
        //
        // Only ever called with a validated gameplay pawn and outside the cooldown: the
        // ProcessEvent calls in here are as dangerous during a level transition as the
        // location read was.

        void set_menu_open(bool open, std::uint64_t now, const wchar_t* why)
        {
            if (open != g_menu_open)
            {
                g_menu_open = open;
                g_menu_change_ms = now;
                // A flip in either direction invalidates the root cache: opening a menu
                // may have added a root we have never seen, and closing one leaves a
                // root behind that must be re-confirmed against the live viewport.
                //
                // ARM THE SCHEDULE HERE, NOT ONLY THE FLAG. set_menu_open runs at the END
                // of the pump and the flag is consumed near its START, so a flip used to
                // reach the discovery walk one whole pump late - and the walk is the only
                // thing that can find the root of a menu never seen before. The flag stays
                // set as well; arming twice is idempotent.
                g_force_widget_sweep = true;
                scan::sweep_arm(g_sweep, now);
                // VERBOSE. It is a per-menu-press transition (54 lines in run 5) and
                // the state it reports is in the snapshot the F2 panel prints live.
                MM_LOGV(L"menu state -> {} ({}); a full widget sweep is queued",
                        open ? L"OPEN" : L"closed",
                        why);
            }
        }

        // Forward declarations: the deny-list gate and the class memo behind it live
        // with the SLICED walk further down, but the FindAllOf fallback above needs the
        // same gate - one list, one answer, whichever path found the widget.
        unsigned char widget_class_kind(UObject* obj);
        bool widget_may_be_menu(UObject* obj);

        // Is this widget's reflected Visibility byte ESlateVisibility::Visible (0)?
        // Raw read at a cached offset: no ProcessEvent, so this is safe and cheap
        // enough to do on every pump.
        bool widget_is_visible_byte(UObject* w, bool& has_byte)
        {
            const uer::ClassLayout* layout = g_layouts.get(w);
            std::uint8_t vis = 0xFF;
            has_byte = uer::read_prop(layout, w, L"Visibility", vis, 1);
            return has_byte && vis == 0;
        }

        // Put a confirmed in-viewport root on the WATCHLIST. Returns true when it was
        // not already there - that is the "the sweep discovered something" signal that
        // keeps the discovery cadence fast (scan::sweep_done).
        bool watch_menu_root(UObject* w)
        {
            for (const uer::ObjRef& ref : g_menu_watch)
            {
                if (ref.obj == w)
                {
                    return false;
                }
            }
            if (g_menu_watch.size() >= g_tune.max_menu_roots)
            {
                // THE CAP IS NOT "NOTHING NEW". Returning false here told the schedule the
                // sweep had discovered nothing and let it back off - while the truth is
                // that a root WAS discovered and could not be recorded, so discovery has
                // to stay fast. The game only ever has 5-6 roots, so a full 32-entry
                // watchlist is a bug, and it now says so.
                if (rare(g_rare_rootcap, ::GetTickCount64()))
                {
                    mm::logf(L"menu watchlist is full at {} root(s) - a newly confirmed root "
                             L"cannot be recorded, so the discovery walk stays on its fast "
                             L"cadence (the game only ever has 5-6: raise "
                             L"reader_max_menu_roots and report this){}",
                             g_tune.max_menu_roots,
                             rare_note(g_rare_rootcap));
                }
                return true;
            }
            uer::ObjRef ref{};
            if (uer::capture(w, ref))
            {
                g_menu_watch.push_back(ref);
                // VERBOSE: the watchlist churns with every menu the player opens.
                MM_LOGV(L"menu root discovered: {} (watchlist now {} widget(s); they are "
                        L"re-tested at 10 Hz, so this menu is caught within one pump from "
                        L"now on)",
                        w->GetName(),
                        g_menu_watch.size());
                return true;
            }
            return false;
        }

        // Does this widget answer IsInViewport() == true right now? One ProcessEvent,
        // SEH-guarded inside uer::call_getter. Only ever asked of a handful of widgets.
        bool widget_in_viewport(UObject* w)
        {
            struct RetBool
            {
                bool v = false;
            } ret{};
            return uer::call_getter(g_funcs, w, L"IsInViewport", ret) && ret.v;
        }

        // THE PER-PUMP TEST (10 Hz), over the cached roots only.
        //
        // BUG THIS FIXES (2026-09-02 run 2, `context/ue4ss-overlay-ingame-run2.log`):
        // the first version answered from the reflected `Visibility` byte alone. When
        // the inventory closes, Wuchang takes `WB_MenuMain_C` OUT OF THE VIEWPORT but
        // leaves its own Visibility at ESlateVisibility::Visible - so the byte said
        // "Visible" forever, `menu` was OR-ed over the sweep's correct answer, and the
        // minimap never came back:
        //     menu OPEN (last change 109562 ms ago, 1 cached root(s)) | widgets 0/882
        // i.e. the authoritative sweep saw ZERO in-viewport Visible widgets while the
        // one cached root held the overlay hidden for the rest of the session.
        //
        // So the byte is only a prefilter now and `IsInViewport()` is the answer, which
        // makes this pass exactly as correct as the full sweep - and a root that is no
        // longer in the viewport is DROPPED from the cache (the sweep re-discovers it
        // the next time that menu opens). There is no latch left: the value returned is
        // derived from live state on every single pump.
        // It runs over the WATCHLIST (every root ever seen), not only over the roots that
        // were open last pump, so re-opening a menu is caught here too - which is what
        // lets the FindAllOf discovery sweep back off to 2 s. `g_menu_roots` (the roots
        // that are open right now) is REBUILT from this pass, so it cannot latch.
        bool menu_from_cached_roots(std::uint32_t& visible_count, std::wstring& holder)
        {
            bool menu = false;
            visible_count = 0;
            const std::size_t was_open = g_menu_roots.size();
            g_menu_roots.clear();
            for (std::size_t i = 0; i < g_menu_watch.size();)
            {
                UObject* w = g_menu_watch[i].obj;
                if (!uer::alive(g_menu_watch[i]))
                {
                    g_menu_watch.erase(g_menu_watch.begin() + static_cast<std::ptrdiff_t>(i));
                    g_force_widget_sweep = true;
                    MM_LOGV(L"menu root dropped (the widget object died); watchlist now {}",
                            g_menu_watch.size());
                    continue;
                }
                bool has_byte = false;
                if (!widget_is_visible_byte(w, has_byte))
                {
                    ++i; // parked, just not ESlateVisibility::Visible
                    continue;
                }
                if (!widget_in_viewport(w))
                {
                    // Visible but not in the viewport = that menu is closed. The widget
                    // STAYS on the watchlist (Wuchang never destroys it and it is how the
                    // next open is caught in one pump) - it simply does not count now.
                    ++i;
                    continue;
                }
                g_menu_roots.push_back(g_menu_watch[i]);
                ++visible_count;
                if (!menu)
                {
                    holder = w->GetName();
                }
                menu = true;
                ++i;
            }
            if (was_open != g_menu_roots.size())
            {
                // A root opening or closing is a state change: re-arm the fast discovery
                // cadence, because whatever the player just did may also have built a
                // menu root we have never seen.
                g_force_widget_sweep = true;
            }
            return menu;
        }

        // THE FULL SWEEP, IN ONE CALL - the FALLBACK path now.
        //
        // This is what the discovery pass used to be on every sweep, and the F2 perf
        // table's in-game reading of it was 25.471 ms average / 62.554 ms peak on the
        // game thread. `FindAllOf` walks the whole object array in a single call, so
        // there is no chunk size or rate that makes the burst acceptable - the sliced
        // walk below replaces it. It survives here for exactly one case: a UE4SS build
        // where `FUObjectArray::GetNumElements()` cannot answer, where a 25 ms sweep at
        // 0.5-1 Hz still beats never detecting a menu at all (`g_wfallback`).
        //
        // Also returns "a menu is up", and REBUILDS the root cache from scratch - a root
        // that does not confirm in this pass is gone, so the cache can never outlive the
        // state it describes.
        bool update_widgets(std::wstring& holder, bool& discovered_new)
        {
            discovered_new = false;
            std::vector<uer::ObjRef> confirmed;
            std::vector<UObject*> widgets;
            UObjectGlobals::FindAllOf(L"UserWidget", widgets);

            std::uint32_t seen = 0;
            std::uint32_t visible_in_viewport = 0;
            bool menu = false;

            const std::size_t count = widgets.size() < g_tune.max_widgets ? widgets.size() : g_tune.max_widgets;
            for (std::size_t i = 0; i < count; ++i)
            {
                UObject* w = widgets[i];
                if (w == nullptr || !UObjectGlobals::IsValidObjectForFindXOf(w))
                {
                    continue;
                }
                ++seen;
                if (!widget_may_be_menu(w))
                {
                    continue; // the same deny-list the sliced walk uses
                }
                bool has_byte = false;
                const bool byte_visible = widget_is_visible_byte(w, has_byte);
                if (has_byte)
                {
                    if (!byte_visible)
                    {
                        continue; // not ESlateVisibility::Visible
                    }
                }
                else
                {
                    // No reflected Visibility on this class: ask the getter instead.
                    struct RetByte
                    {
                        std::uint8_t v = 0xFF;
                    } ret{};
                    if (!uer::call_getter(g_funcs, w, L"GetVisibility", ret) || ret.v != 0)
                    {
                        continue;
                    }
                }

                struct RetBool
                {
                    bool v = false;
                } in_viewport{};
                if (uer::call_getter(g_funcs, w, L"IsInViewport", in_viewport) && in_viewport.v)
                {
                    ++visible_in_viewport;
                    if (!menu)
                    {
                        holder = w->GetName();
                    }
                    menu = true;
                    // Watch it: from now on this root is re-tested every pump, so the
                    // NEXT time this menu opens the minimap hides within ~100 ms and the
                    // discovery sweep no longer has to run often for its sake.
                    discovered_new = watch_menu_root(w) || discovered_new;
                    uer::ObjRef ref{};
                    if (uer::capture(w, ref))
                    {
                        confirmed.push_back(ref);
                    }
                }
            }

            // REBUILD, do not merge: `confirmed` is the complete live answer. Anything
            // that was in the cache and is not in here has left the viewport.
            if (confirmed.size() != g_menu_roots.size())
            {
                MM_LOGV(L"menu root cache rebuilt by the sweep: {} -> {} root(s)",
                        g_menu_roots.size(),
                        confirmed.size());
            }
            g_menu_roots = std::move(confirmed);

            g_widgets_seen = seen;
            g_widgets_visible = visible_in_viewport;
            return menu;
        }

        //==============================================================================
        // THE SLICED DISCOVERY WALK
        //==============================================================================

        // Is this object's class a `UUserWidget` descendant? Same job `FindAllOf(
        // L"UserWidget")` did for us (it matches subclasses), by the same means the
        // marker classifier uses: walk the super chain comparing NAMES, memoise the
        // answer per `UClass*`. Names rather than a `UClass*` compare because we have no
        // `UUserWidget::StaticClass()` to compare against - `ue_min.hpp` declares only
        // what UE4SS exports.
        //   0 = not a UUserWidget at all
        //   1 = a widget that MAY hold a menu
        //   2 = a widget whose class is on the not-a-menu deny-list
        //
        // The deny-list answer is memoised in the same map as the widget answer, keyed
        // by `UClass*`, so it costs one `GetName()` per CLASS per session rather than
        // one per instance per round - and the class name is what the list matches on
        // (an object name is index-suffixed; `WB_ZiMu_C_2147458145` is an instance of
        // `WB_ZiMu_C`). See scan_sched.hpp for why the list exists and why it is a list
        // rather than a new rule.
        unsigned char widget_class_kind(UObject* obj)
        {
            RC::Unreal::UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return 0;
            }
            const auto it = g_wclass.find(cls);
            if (it != g_wclass.end())
            {
                return it->second;
            }
            bool is_widget = false;
            auto* current = static_cast<RC::Unreal::UStruct*>(cls);
            for (int depth = 0; current != nullptr && depth < 48 && !is_widget; ++depth)
            {
                if (!mem::readable(current, 0x40))
                {
                    break;
                }
                if (static_cast<UObject*>(current)->GetName() == L"UserWidget")
                {
                    is_widget = true;
                }
                current = current->GetSuperStruct();
            }
            unsigned char kind = is_widget ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0);
            if (kind == 1)
            {
                const std::wstring cname = static_cast<UObject*>(cls)->GetName();
                const char* why =
                    scan::non_menu_root_reason(cname.c_str(), mm::cfg_cached().menu_ignore_roots);
                if (why != nullptr)
                {
                    kind = 2;
                    // Once per class per SESSION, and it names the reason: this is the
                    // line that says the deny-list did something, so a minimap that
                    // stops hiding on a real menu can be traced to an over-broad entry.
                    if (g_non_menu_logged.insert(cname).second)
                    {
                        mm::logf(L"menu detector: '{}' is on the not-a-menu list ({}) - it can never "
                                 L"hide the minimap",
                                 cname,
                                 std::wstring(why, why + std::strlen(why)));
                    }
                }
            }
            if (g_wclass.size() > kWidgetClassCacheMax)
            {
                g_wclass.clear();
            }
            g_wclass.emplace(cls, kind);
            return kind;
        }

        bool class_is_user_widget(UObject* obj)
        {
            return widget_class_kind(obj) != 0;
        }

        // Could this widget hold a menu? A widget on the deny-list is still walked and
        // still counted, so the state line's `widgets N/M` numbers do not change - it
        // simply never becomes a candidate.
        bool widget_may_be_menu(UObject* obj)
        {
            return widget_class_kind(obj) == 1;
        }

        // ONE SLICE. Raw reads only - no ProcessEvent - so this is safe on the fast path
        // between position pumps, which is where it gets the hundreds of calls a second
        // that keep a round short.
        //
        // Rejects, cheapest first (the same order and the same reasoning as the marker
        // slice): the FUObjectItem validity flags read through the object ARRAY rather
        // than through the object, so a freed allocation is safe to look at; then the
        // memoised class test, which is the overwhelming case; then
        // IsValidObjectForFindXOf, which is precisely what FindAllOf used to filter out
        // for us (CDOs and archetypes); and only then the Visibility byte.
        void widget_scan_slice(const scan::Slice& slice)
        {
            for (int i = slice.begin; i < slice.end; ++i)
            {
                RC::Unreal::FUObjectItem* item = RC::Unreal::FUObjectArray::IndexToObject(i);
                if (item == nullptr || !item->IsValid(false))
                {
                    continue;
                }
                UObject* obj = item->GetUObject();
                if (obj == nullptr)
                {
                    continue;
                }
                if (!class_is_user_widget(obj))
                {
                    continue;
                }
                if (!UObjectGlobals::IsValidObjectForFindXOf(obj) || !mem::readable(obj, 0x40))
                {
                    continue;
                }
                ++g_wseen_round;
                if (!widget_may_be_menu(obj))
                {
                    continue; // subtitles, damage numbers, toasts, the HUD - see scan_sched.hpp
                }
                // THE PREFILTER. `UWidget::Visibility` is a reflected TEnumAsByte and
                // ESlateVisibility::Visible == 0, so ~900 widget instances cost one
                // guarded byte read each and only the handful that say Visible are worth
                // a ProcessEvent later. A class with no reflected Visibility at all is
                // kept as a candidate so the commit can ask `GetVisibility()` instead -
                // that is what the one-call sweep did too.
                bool has_byte = false;
                const bool byte_visible = widget_is_visible_byte(obj, has_byte);
                if (has_byte && !byte_visible)
                {
                    continue;
                }
                if (g_wpending.size() >= static_cast<std::size_t>(scan::kWidgetCandidateMax))
                {
                    ++g_wcand_dropped;
                    continue;
                }
                uer::ObjRef ref{};
                if (uer::capture(obj, ref))
                {
                    ++g_wcand_round;
                    g_wpending.push_back(ref);
                }
            }
        }

        // Drive the walk: start a round when the schedule says a sweep is due, then take
        // one slice per `kWidgetSlicePeriodMs`. Called from the fast path AND once at the
        // end of the 10 Hz pump, so the slice rate is set here and not by the caller
        // (lessons.md: "a throttle in the CALLER can be the reason the callee cannot be
        // optimised").
        void widget_scan_pump(std::uint64_t now, std::uint64_t now_us)
        {
            if (g_wfallback || g_wround_ready)
            {
                return; // no GUObjectArray, or a finished round is waiting to be committed
            }
            if (!g_wround_active)
            {
                if (!scan::sweep_due(g_sweep, now))
                {
                    return;
                }
                if (RC::Unreal::FUObjectArray::GetNumElements() <= 0)
                {
                    g_wfallback = true;
                    mm::log(L"widget scan: GUObjectArray reports no elements - falling back to "
                            L"FindAllOf(\"UserWidget\") for menu discovery (expensive: ~25 ms per "
                            L"sweep on the game thread)");
                    return;
                }
                g_wround_active = true;
                g_wcursor = scan::Cursor{};
                // `g_wpending` is deliberately NOT cleared: a candidate this round's
                // predecessor found in its last slice is still waiting for its commit,
                // and throwing it away is exactly the discovery hole this rework closes.
                g_wseen_round = 0;
                g_wcand_dropped = 0;
                g_wcand_round = 0;
            }
            if (!scan::slice_due(now_us, g_wslice_us, scan::kWidgetSlicePeriodMs))
            {
                return;
            }
            g_wslice_us = now_us;
            // Re-read every slice: the array grows as levels stream in and can shrink
            // after a GC compaction, so the cursor is clamped against the CURRENT size
            // rather than against a remembered one.
            const int total = RC::Unreal::FUObjectArray::GetNumElements();
            const scan::Slice s = scan::next_slice(g_wcursor, total, scan::kWidgetChunkDefault);
            if (g_pf_wslice < 0)
            {
                g_pf_wslice = mm::perf_register("widget scan slice", perf::Thread::Game);
            }
            const std::uint64_t t0 = mm::qpc_us();
            if (!s.empty())
            {
                widget_scan_slice(s);
            }
            mm::perf_record(g_pf_wslice, t0);
            if (scan::advance(g_wcursor, s, total))
            {
                g_wround_active = false;
                g_wround_ready = true;
            }
        }

        // THE COMMIT, ON EVERY VALIDATED PUMP.
        //
        // The byte-`Visible` candidates the slices have collected since the last pump are
        // the only widgets that pay for an `IsInViewport()` ProcessEvent, and this runs on
        // the 10 Hz pump where the pawn was validated in the same call - the ProcessEvent
        // calls in here are as dangerous during a level transition as the location read.
        //
        // WHY NOT AT THE END OF THE ROUND (which is what broke menu detection): the round
        // takes ~350 ms on the fast path and can take seconds when only the 10 Hz pump is
        // slicing, so a menu that opens and closes inside one round is byte-Visible for
        // the slice and out of the viewport by the time the commit asks - never confirmed,
        // never added to the watchlist, and therefore missed on every later opening of the
        // same menu as well. Committing per pump bounds that gap at one pump.
        //
        // IT CAN ONLY ADD. A confirmed root goes on the watchlist AND into `g_menu_roots`,
        // and the watchlist re-test (menu_from_cached_roots, which rebuilds `g_menu_roots`
        // from live `IsInViewport()` calls on every pump) remains the complete answer for
        // everything already known. Both halves are fresh in the same pump, so there is no
        // cached value to latch - see scan::menu_open_from.
        bool commit_widget_candidates(std::wstring& holder, bool& discovered_new)
        {
            discovered_new = false;
            bool menu = false;
            if (g_wpending.empty())
            {
                return false;
            }
            const int take = scan::commit_batch(static_cast<int>(g_wpending.size()),
                                                scan::kWidgetCommitPerPump);
            for (int i = 0; i < take; ++i)
            {
                const uer::ObjRef& ref = g_wpending[static_cast<std::size_t>(i)];
                if (!uer::alive(ref))
                {
                    continue; // captured a pump ago, dead by now
                }
                UObject* w = ref.obj;
                if (!widget_may_be_menu(w))
                {
                    continue; // the config's list can grow between the slice and here
                }
                // Re-read the byte rather than trusting the slice's: the authoritative
                // answer is always the fresh one, however short the gap.
                bool has_byte = false;
                const bool byte_visible = widget_is_visible_byte(w, has_byte);
                if (has_byte)
                {
                    if (!byte_visible)
                    {
                        continue;
                    }
                }
                else
                {
                    struct RetByte
                    {
                        std::uint8_t v = 0xFF;
                    } ret{};
                    if (!uer::call_getter(g_funcs, w, L"GetVisibility", ret) || ret.v != 0)
                    {
                        continue;
                    }
                }
                if (!widget_in_viewport(w))
                {
                    continue;
                }
                if (!menu)
                {
                    holder = w->GetName();
                }
                menu = true;
                // Watch it: from now on this root is re-tested every pump, so the NEXT
                // time this menu opens the minimap hides within ~100 ms and the discovery
                // walk no longer has to run often for its sake.
                discovered_new = watch_menu_root(w) || discovered_new;
                // And count it as open NOW - `g_menu_roots` is the set of roots open this
                // pump, and this one was just proved to be in the viewport.
                bool already = false;
                for (const uer::ObjRef& open : g_menu_roots)
                {
                    already = already || open.obj == w;
                }
                if (!already)
                {
                    g_menu_roots.push_back(ref);
                    ++g_widgets_visible;
                }
            }
            g_wpending.erase(g_wpending.begin(), g_wpending.begin() + static_cast<std::ptrdiff_t>(take));
            return menu;
        }

        // End-of-round bookkeeping: the counts the state line reports, and the one place
        // an over-full candidate list is shouted about. The ANSWER does not come from here
        // any more (see commit_widget_candidates).
        void finish_widget_round()
        {
            g_widgets_seen = g_wseen_round;
            if (g_wcand_dropped != 0)
            {
                // Never silently truncate the answer: if this ever fires the cap is wrong
                // for this game, and the number says by how much. This is the counter that
                // would have named the 64-candidate cap as the reason menus stopped being
                // detected. But the sweep runs up to four times a second, and a cap that
                // is wrong is wrong for the whole session - so it is the first occurrence
                // and then once per 30 s, never once per round.
                if (rare(g_rare_wcap, ::GetTickCount64()))
                {
                    mm::logf(L"widget scan: {} byte-Visible widget(s) exceeded the {}-candidate "
                             L"cap this round and were not tested (a menu root is constructed "
                             L"late, i.e. at a HIGH object-array index, so it is the most likely "
                             L"one to be cut){}",
                             g_wcand_dropped,
                             scan::kWidgetCandidateMax,
                             rare_note(g_rare_wcap));
                }
            }
        }

        //==============================================================================
        // Which chapter is the player in?
        //==============================================================================
        //
        // From the STREAMED LEVEL SET, never from the position: the five chapters'
        // world bounds overlap (chapter 4 covers nearly all of chapter 1), which is the
        // "chapter identification by bounds only" hole the overlay MVP left open.
        // chapterid.hpp holds the string logic and the tiered vote; this is only the
        // enumeration, and it is raw reads plus one GetFullName() per LOADED level.
        //
        // Three routes, tried in order, because none of the three property names can be
        // verified without launching the game and a wrong guess must degrade rather
        // than fail:
        //
        //   1. `UWorld::Levels`         - TArray<ULevel*> of the levels currently in
        //                                 the world. Exactly the question, ~50 entries.
        //   2. `UWorld::StreamingLevels`- TArray<ULevelStreaming*>, ~834 entries, of
        //                                 which the loaded ones have a `LoadedLevel`.
        //   3. `FindAllOf(L"Level")`    - a whole GUObjectArray walk (28-51 ms,
        //                                 lessons.md), so it is the last resort and it
        //                                 still only runs at kChapterPeriodMs.
        //
        // The route that worked is remembered and logged once.

        // TArray<T> is { T* Data; int32 Num; int32 Max; } - 16 bytes, which is also what
        // FArrayProperty reports as its element size, so the read is size-checked.
        struct TArrayRaw
        {
            void* data = nullptr;
            std::int32_t num = 0;
            std::int32_t max = 0;
        };

        bool read_array_prop(const uer::ClassLayout* layout, const void* obj, const wchar_t* name,
                             TArrayRaw& out)
        {
            if (!uer::read_prop(layout, obj, name, out, static_cast<int>(sizeof(TArrayRaw))))
            {
                return false;
            }
            if (out.num < 0 || out.num > g_tune.max_levels || out.max < out.num)
            {
                return false;
            }
            if (out.num > 0 && (!mem::plausible_ptr(out.data) ||
                                !mem::readable(out.data, static_cast<std::size_t>(out.num) * sizeof(void*))))
            {
                return false;
            }
            return true;
        }

        // Validates `obj` as a live UObject and feeds its full name to the vote. The
        // capture() call is the same GUObjectArray liveness test the pawn uses, so a
        // level that is being torn down as we walk the array cannot be dereferenced.
        // The enumeration has the level's full name in hand anyway, so it also collects
        // the SHORT name ("Chapter1_DGong_logic") for markers::set_loaded_levels(). That
        // set is what lets the marker sweep treat absence as evidence of a collect - see
        // markers.hpp. Every name we parse is ASCII.
        void vote_on_object(UObject* obj, chid::Vote& vote, int& counted, std::vector<std::string>* levels)
        {
            if (obj == nullptr || !mem::plausible_ptr(obj))
            {
                return;
            }
            uer::ObjRef ref{};
            if (!uer::capture(obj, ref))
            {
                return;
            }
            const std::wstring full = obj->GetFullName();
            if (full.empty())
            {
                return;
            }
            vote.add(std::wstring_view{full});
            ++counted;
            if (levels != nullptr && levels->size() < 4096)
            {
                std::string narrow;
                narrow.reserve(full.size());
                for (const wchar_t c : full)
                {
                    narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
                }
                std::string level = mdb::level_from_full_name(narrow);
                if (!level.empty())
                {
                    levels->push_back(std::move(level));
                }
            }
        }

        // Routes 1 and 2 differ only in whether the array element IS the level or has
        // to be dereferenced through `LoadedLevel`.
        bool vote_from_array(UObject* world, const uer::ClassLayout* layout, const wchar_t* prop,
                             bool via_loaded_level, chid::Vote& vote, int& counted,
                             std::vector<std::string>* levels)
        {
            TArrayRaw arr{};
            if (!read_array_prop(layout, world, prop, arr) || arr.num == 0)
            {
                return false;
            }
            for (std::int32_t i = 0; i < arr.num; ++i)
            {
                void* raw = nullptr;
                if (!mem::read_at(arr.data, static_cast<std::size_t>(i) * sizeof(void*), raw))
                {
                    break;
                }
                if (!mem::plausible_ptr(raw))
                {
                    continue;
                }
                UObject* level = static_cast<UObject*>(raw);
                if (via_loaded_level)
                {
                    // ULevelStreaming::LoadedLevel is null for the ~780 sublevels that
                    // are not streamed in, which is exactly the filter we want.
                    const uer::ClassLayout* sl = g_layouts.get(level);
                    level = uer::read_object_prop(sl, level, L"LoadedLevel");
                    if (level == nullptr)
                    {
                        continue;
                    }
                }
                vote_on_object(level, vote, counted, levels);
            }
            return counted > 0;
        }

        void refresh_chapter(std::uint64_t now)
        {
            if (g_world == nullptr)
            {
                return;
            }
            UObject* world = static_cast<UObject*>(const_cast<void*>(g_world));
            if (!mem::readable(world, 0x40))
            {
                return;
            }

            chid::Vote vote{};
            int counted = 0;
            int route = 0;
            // The short names of every level this walk sees, for the marker sweep's
            // absence rule (markers.hpp). Built here because this is the one place that
            // already pays for the enumeration and the GetFullName() calls.
            std::vector<std::string> levels;
            const uer::ClassLayout* layout = g_layouts.get(world);
            if (vote_from_array(world, layout, L"Levels", false, vote, counted, &levels))
            {
                route = 1;
            }
            else if (vote_from_array(world, layout, L"StreamingLevels", true, vote, counted, &levels))
            {
                route = 2;
            }
            else
            {
                // Last resort: a full GUObjectArray walk. It is the expensive one, so it
                // only ever runs when neither UWorld array could be read at all.
                std::vector<UObject*> found;
                UObjectGlobals::FindAllOf(L"Level", found);
                for (UObject* level : found)
                {
                    if (counted >= g_tune.max_levels)
                    {
                        break;
                    }
                    vote_on_object(level, vote, counted, &levels);
                }
                route = counted > 0 ? 3 : 0;
            }

            if (route != g_chapter_route)
            {
                static const wchar_t* const kRouteName[] = {L"(none)", L"UWorld::Levels",
                                                            L"UWorld::StreamingLevels -> LoadedLevel",
                                                            L"FindAllOf(\"Level\")"};
                mm::logf(L"chapter: enumerating the streamed levels through {} ({} level(s) named)",
                         kRouteName[route < 0 || route > 3 ? 0 : route],
                         counted);
                g_chapter_route = route;
            }

            // Publish the loaded-level set even when the chapter vote came out
            // undecided: the two answers are independent, and the absence rule must not
            // go blind just because a level name did not name a chapter.
            //
            // ...but only when it actually CHANGED. The streamed set is stable for
            // minutes at a time, and set_loaded_levels rebuilds an
            // unordered_map<string,uint64> of ~50 entries from it. The enumeration order
            // is the world's own level order, which is stable while the set is, so an
            // element-wise compare is both exact enough and cheaper than the rebuild it
            // avoids. A false "changed" costs one rebuild; a false "unchanged" is
            // impossible, because any added, removed or renamed level shows up here.
            if (levels != g_last_levels)
            {
                g_last_levels = levels;
                markers::set_loaded_levels(levels);
            }

            g_chapter_levels = counted;
            const int detected = vote.best();
            if (detected == chid::kNone)
            {
                return; // never overwrite a good answer with "I could not tell"
            }
            if (detected != g_chapter)
            {
                mm::logf(L"chapter: {} -> {} (tier {}, {} of {} named level(s) agree)",
                         g_chapter == chid::kNone  ? std::wstring{L"?"}
                         : g_chapter == chid::kDlc ? std::wstring{L"DLC"}
                                                   : std::to_wstring(g_chapter),
                         detected == chid::kDlc ? std::wstring{L"DLC"} : std::to_wstring(detected),
                         vote.best_tier(),
                         vote.best_count(),
                         counted);
                g_chapter = detected;
            }
            mapdata::set_detected_chapter(detected);
            (void)now;
        }

        //==============================================================================
        // The pump
        //==============================================================================

        struct DepthGuard
        {
            int& d;
            explicit DepthGuard(int& v) : d(v)
            {
                ++d;
            }
            ~DepthGuard()
            {
                --d;
            }
        };

        void publish_hidden(std::uint64_t now, bool transition)
        {
            mm::Snapshot snap{}; // defaults are "hidden": no pawn, menu_open = true
            snap.stamp_ms = now;
            snap.transition = transition;
            snap.widgets_seen = g_widgets_seen;
            snap.menu_change_ms = g_menu_change_ms;
            // DIAGNOSTICS MUST DESCRIBE THE READER, NOT THE STRUCT'S DEFAULTS. The
            // hidden snapshot used to leave these at zero, so the 10 s state line read
            // `menu OPEN ... 0 cached root(s) | sweep every 0 ms (watchlist 0)` while the
            // reader was in fact simply pawn-less - three numbers that all pointed at the
            // widget code and none of which were about it. has_pawn is still false, so
            // nothing is shown on the strength of any of them.
            snap.menu_open = g_menu_open;
            snap.menu_roots_cached = static_cast<std::uint32_t>(g_menu_roots.size());
            snap.menu_watch_count = static_cast<std::uint32_t>(g_menu_watch.size());
            snap.widget_sweep_period_ms = static_cast<std::uint32_t>(scan::sweep_period_ms(g_sweep, now));
            snap.widgets_visible_in_viewport = g_widgets_visible;
            mm::publish(snap);
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            g_report_pending.store(true, std::memory_order_relaxed);
        }

        void pump()
        {
            // THE MASTER SWITCH, first statement (modswitch.hpp). UE4SS exports a
            // Register for the ProcessEvent pre-callback and no Unregister, so a
            // disabled mod cannot take this callback out of the engine's path - what it
            // can do is make it cost one relaxed atomic load and nothing else: no
            // config copy (that is a spinlock), no allocation, no reflection, no read
            // of any engine memory.
            if (!mm::mod_active())
            {
                return;
            }

            // ProcessEvent fires thousands of times a second, and every ProcessEvent WE
            // issue fires it again - so the re-entrancy guard comes first, before any
            // work at all. thread_local, because the guard has to be per-thread: the
            // engine calls ProcessEvent from more than one thread.
            static thread_local int depth = 0;
            if (depth != 0)
            {
                return;
            }

            // ...and then the THREAD guard (A.1). Everything below this line touches
            // game-thread-only state.
            const unsigned long tid = ::GetCurrentThreadId();
            unsigned long owner = g_pump_thread.load(std::memory_order_relaxed);
            if (owner == 0)
            {
                g_pump_thread.store(tid, std::memory_order_relaxed);
                owner = tid;
            }
            if (owner != tid)
            {
                // Once, at verbose: it is a permanent property of the process, not an
                // event, and it must not be able to flood the log from a worker thread.
                if (!g_wrong_thread_logged.exchange(true, std::memory_order_relaxed))
                {
                    MM_LOGV(L"ProcessEvent reached the reader on thread {} as well; the reader "
                            L"belongs to thread {} and every other thread returns immediately "
                            L"(its state is not synchronised)",
                            tid,
                            owner);
                }
                return;
            }

            const std::uint64_t now = ::GetTickCount64();
            // Twice a second at most; every other call is a compare (see refresh_tunables).
            refresh_tunables(now);
            if (now - g_last_position < g_tune.position_ms)
            {
                // BETWEEN position pumps: run the marker scan slice and nothing else.
                //
                // The marker sweep walks GUObjectArray a slice at a time and needs many
                // small slices per second to keep a full pass inside ~1 s. Gating it on
                // this 10 Hz pump was what forced the old design to do a whole
                // FindAllOf (28 ms, 2-3 dropped frames) per pump. It is self-throttled
                // on QueryPerformanceCounter, so calling it from every ProcessEvent
                // costs one QPC read and a compare when it is not its turn.
                //
                // It runs on the LAST validated state: same re-entrancy guard, same
                // transition cooldown, and only while a gameplay pawn was standing as
                // of the most recent position pump (at most 100 ms ago).
                if (g_state_ok_since != 0 && now >= g_cooldown_until)
                {
                    const DepthGuard slice_guard{depth};
                    const StageMark mark{"fast slice: markers"};
                    markers::game_thread_pump(now, g_world);
                    // The menu-discovery walk rides here for the same reason: it is a
                    // sliced GUObjectArray pass that needs many small slices per second,
                    // and it is self-throttled on QPC. Raw reads only - the ProcessEvent
                    // half of it is the commit, on the validated 10 Hz pump below.
                    const StageMark wmark{"fast slice: widgets"};
                    widget_scan_pump(now, mm::qpc_us());
                }
                // The stage is a breadcrumb for the stall watchdog, so it must not be left
                // naming work that has already finished - a freeze between pumps used to
                // be reported as a hang in the widget slice.
                g_pump_stage.store("between pumps", std::memory_order_relaxed);
                return;
            }
            g_last_position = now;

            const DepthGuard guard{depth};
            g_pump_calls.fetch_add(1, std::memory_order_relaxed);

            // ---- 0. a transition is in progress: do nothing at all ------------------
            if (now < g_cooldown_until)
            {
                g_state_ok_since = 0;
                publish_hidden(now, true);
                return;
            }

            // ---- 1. the controller ---------------------------------------------------
            g_pump_stage.store("controller", std::memory_order_relaxed);
            if (!uer::alive(g_controller))
            {
                g_controller.reset();
                if (now - g_last_resolve_ctrl >= g_tune.resolve_ms)
                {
                    g_last_resolve_ctrl = now;
                    resolve_controller();
                }
            }

            // ---- 2. validate the pawn EVERY pump ------------------------------------
            //
            // Three independent tests, cheapest first:
            //   a) GUObjectArray liveness (index -> FUObjectItem -> flags + back
            //      pointer), which is safe to read even after the object was freed;
            //   b) the controller's own Pawn pointer still names the same object;
            //   c) the pawn's UWorld* is still the world we captured it in.
            g_pump_stage.store("pawn validate", std::memory_order_relaxed);
            bool pawn_ok = g_pawn_is_gameplay && uer::alive(g_pawn);
            if (pawn_ok)
            {
                UObject* from_controller = pawn_from_controller();
                if (from_controller != nullptr && from_controller != g_pawn.obj)
                {
                    drop_pawn(now, L"the controller reports a different pawn");
                    pawn_ok = false;
                }
            }
            if (pawn_ok)
            {
                const void* world = uer::world_of(g_pawn);
                if (world == nullptr)
                {
                    drop_pawn(now, L"the pawn has no world");
                    pawn_ok = false;
                }
                else if (g_world != nullptr && world != g_world)
                {
                    drop_pawn(now, L"the world changed (level transition)");
                    pawn_ok = false;
                }
                else
                {
                    g_world = world;
                    g_world_token = world; // the recovery route's starting point (A.22)
                }
            }
            if (!pawn_ok && !g_pawn.empty())
            {
                drop_pawn(now, L"the cached pawn is no longer a live object");
            }
            if (now < g_cooldown_until)
            {
                g_state_ok_since = 0;
                publish_hidden(now, true);
                return;
            }

            // ---- 3. (re-)acquire a gameplay pawn ------------------------------------
            //
            // NEVER gated on the controller, on the menu state or on the widget sweep.
            // The pawn is the thing everything else hangs off, so its 2 Hz budget is its
            // own (see the comment on g_last_resolve_pawn) and the controller is only a
            // fallback route for finding it.
            if (!pawn_ok && now - g_last_resolve_pawn >= g_tune.resolve_ms)
            {
                g_last_resolve_pawn = now;
                resolve_pawn(now);
                pawn_ok = g_pawn_is_gameplay && uer::alive(g_pawn);
            }

            if (!pawn_ok)
            {
                // ONE LINE THAT SAYS THE READER IS STUCK. Run 1's log went two full
                // minutes printing `pawn NO` with nothing to say why, because every
                // diagnostic lived inside a resolve_pawn() that was never entered.
                if (g_no_pawn_since == 0)
                {
                    g_no_pawn_since = now;
                }
                // RE-RESOLVE THE CONTROLLER TOO, not just the pawn. A shrine fast travel
                // can leave the controller we cached alive-but-wrong: it still passes
                // every liveness test and reports a null `Pawn` for the rest of the
                // session, which is exactly the user's 86-second "no player pawn". The
                // pawn resolve above only ever asks that controller, so if the controller
                // is the stale half nothing can ever recover - after three seconds
                // without a pawn it is dropped and re-resolved from the world's own
                // GameInstance chain.
                if (now - g_no_pawn_since >= kNoPawnCtrlDropMs &&
                    now - g_last_ctrl_drop >= kNoPawnCtrlDropMs)
                {
                    g_last_ctrl_drop = now;
                    // The DROP happens every three seconds because that is the recovery
                    // timing; the LINE about it does not, or a two-minute loading screen
                    // writes forty of them.
                    if (rare(g_rare_ctrl_drop, now))
                    {
                        mm::logf(L"no gameplay pawn for {} ms - dropping the cached player controller "
                                 L"and re-resolving it from UWorld -> OwningGameInstance -> "
                                 L"LocalPlayers[0] -> PlayerController{}",
                                 now - g_no_pawn_since,
                                 rare_note(g_rare_ctrl_drop));
                    }
                    g_controller.reset();
                    g_funcs.clear();
                    resolve_controller();
                }
                // ...and once, after five seconds, print everything the next session
                // would otherwise have to be spent finding out.
                if (now - g_no_pawn_since >= kNoPawnDiagnoseMs && !g_pawnless_diag_done)
                {
                    g_pawnless_diag_done = true;
                    log_pawnless_diagnosis();
                }
                if (now - g_no_pawn_since >= 10000 && rare(g_rare_stuck, now))
                {
                    mm::logf(L"no gameplay pawn for {} ms (controller {}, resolve every {} ms) - "
                             L"the reader is retrying the controller's Pawn / AcknowledgedPawn / "
                             L"Character and FindAllOf('{}'){}",
                             now - g_no_pawn_since,
                             uer::alive(g_controller) ? L"alive" : L"NOT resolved",
                             g_tune.resolve_ms,
                             kPlayerPawnClass,
                             rare_note(g_rare_stuck));
                }
                g_state_ok_since = 0;
                publish_hidden(now, false);
                return;
            }
            g_no_pawn_since = 0;
            // A pawn is standing again, so the next stall gets its own diagnosis rather
            // than being silent because an earlier one used the one shot up - and its own
            // first-occurrence log lines, for the same reason.
            g_pawnless_diag_done = false;
            rare_reset(g_rare_ctrl_drop);
            rare_reset(g_rare_stuck);

            // ---- 4. from here on the pawn is validated: UFunctions are allowed -------
            //
            // THE MENU TEST HAS TWO HALVES AND BOTH RUN ON EVERY PUMP.
            //   a) the WATCHLIST re-test: `IsInViewport()` over every root ever confirmed
            //      this session, which is the complete authoritative answer for everything
            //      already known and rebuilds `g_menu_roots` from scratch;
            //   b) the COMMIT of whatever the sliced discovery walk has newly seen as
            //      byte-`Visible`, which can only ADD a root - and adds it to the
            //      watchlist at the same time, so (a) owns it from the next pump on.
            // Both are live `IsInViewport()` calls made in THIS pump, so OR-ing them is
            // not the cached-over-fresh latch lessons.md condemns; there is no cached
            // answer left anywhere in this path (scan::menu_open_from).
            std::uint32_t roots_visible = 0;
            std::wstring holder;
            bool watch_menu = false;
            {
                if (g_pf_retest < 0)
                {
                    g_pf_retest = mm::perf_register("menu root re-test", perf::Thread::Game);
                }
                const mm::PerfScope scope(g_pf_retest);
                watch_menu = menu_from_cached_roots(roots_visible, holder);
            }
            g_widgets_visible = roots_visible;

            // The discovery walk's cadence. Every event that can have created a menu
            // root we have never seen re-arms the fast cadence (and makes a round due
            // now); a quiet, warm reader lets the period double up to
            // reader_widget_sweep_max_period_ms. The latency this schedule bounds is ONLY
            // "a menu whose root has never been seen this session opened" - everything
            // else is answered by the watchlist pass above, one pump after it happens.
            if (g_force_widget_sweep)
            {
                g_force_widget_sweep = false;
                scan::sweep_arm(g_sweep, now);
            }

            bool commit_menu = false;
            std::wstring commit_holder;
            bool discovered_new = false;
            if (g_wfallback)
            {
                // No GUObjectArray on this build: the whole-array FindAllOf is the only
                // discovery route left, and it is still scheduled rather than per pump
                // because it costs ~25 ms of game thread every time it runs.
                if (scan::sweep_due(g_sweep, now))
                {
                    if (g_pf_sweep < 0)
                    {
                        g_pf_sweep =
                            mm::perf_register("widget sweep (FindAllOf, fallback)", perf::Thread::Game);
                    }
                    const std::uint64_t sweep_t0 = mm::qpc_us();
                    commit_menu = update_widgets(commit_holder, discovered_new);
                    mm::perf_record(g_pf_sweep, sweep_t0);
                    scan::sweep_done(g_sweep, now, discovered_new, g_menu_watch.empty());
                }
            }
            else
            {
                widget_scan_pump(now, mm::qpc_us());
                if (g_pf_wcommit < 0)
                {
                    g_pf_wcommit = mm::perf_register("widget round commit", perf::Thread::Game);
                }
                {
                    const mm::PerfScope commit_scope(g_pf_wcommit);
                    commit_menu = commit_widget_candidates(commit_holder, discovered_new);
                }
                // (`roots_visible` is not written back from g_widgets_visible here: it was
                // a dead store - the snapshot and the state line both read
                // g_widgets_visible, which the commit has just updated.)
                g_wcand_round_pub.store(g_wcand_round, std::memory_order_relaxed);
                g_wpending_pub.store(static_cast<std::uint32_t>(g_wpending.size()),
                                     std::memory_order_relaxed);
                g_wcand_dropped_pub.store(g_wcand_dropped, std::memory_order_relaxed);
                if (g_wround_ready)
                {
                    // The round has wrapped: report its counts and let the schedule back
                    // off. The ANSWER did not wait for this (see above), which is the
                    // whole point of the rework.
                    g_wround_ready = false;
                    finish_widget_round();
                    scan::sweep_done(g_sweep, now, discovered_new, g_menu_watch.empty());
                }
            }
            const bool menu = scan::menu_open_from(watch_menu, commit_menu);
            if (!watch_menu && commit_menu)
            {
                holder = commit_holder;
            }
            g_menu_holder = holder;
            set_menu_open(menu,
                          now,
                          menu ? (watch_menu ? L"a watchlist root is in the viewport and Visible"
                                             : L"the discovery walk just confirmed a new in-viewport "
                                               L"Visible root")
                               : L"no root is in the viewport and Visible");

            // Which chapter's map asset should be resident. Slow (1 Hz) and cheap, and
            // it runs on the same validated state the rest of the pump uses.
            if (now - g_last_chapter >= g_tune.chapter_ms)
            {
                g_last_chapter = now;
                if (g_pf_chapter < 0)
                {
                    g_pf_chapter = mm::perf_register("refresh_chapter", perf::Thread::Game);
                }
                const mm::PerfScope scope(g_pf_chapter);
                refresh_chapter(now);
            }

            mm::Snapshot snap{};
            snap.stamp_ms = now;
            snap.menu_open = g_menu_open;
            snap.widgets_seen = g_widgets_seen;
            snap.widgets_visible_in_viewport = g_widgets_visible;
            snap.menu_roots_cached = static_cast<std::uint32_t>(g_menu_roots.size());
            snap.menu_watch_count = static_cast<std::uint32_t>(g_menu_watch.size());
            snap.widget_sweep_period_ms =
                static_cast<std::uint32_t>(scan::sweep_period_ms(g_sweep, now));
            snap.menu_change_ms = g_menu_change_ms;
            snap.pawn_is_gameplay = true;
            copy_to(snap.menu_holder, std::size(snap.menu_holder), g_menu_holder);

            UObject* pawn = g_pawn.obj;
            bool via_function = false;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            float yaw = 0.0f;
            if (g_pf_position < 0)
            {
                g_pf_position = mm::perf_register("position pump", perf::Thread::Game);
            }
            const std::uint64_t pos_t0 = mm::qpc_us();
            const bool got_location = read_location(pawn, x, y, z, yaw, via_function);
            mm::perf_record(g_pf_position, pos_t0);
            if (got_location)
            {
                snap.has_pawn = true;
                snap.x = x;
                snap.y = y;
                snap.z = z;
                snap.yaw = yaw;
                snap.loc_from_function = via_function;
            }
            copy_to(snap.pawn_name, std::size(snap.pawn_name), g_pawn_short_name);
            copy_to(snap.level_name, std::size(snap.level_name), g_pawn_full_name);

            // TELEPORT / RE-POSSESSION. A shrine fast-travel keeps the same pawn object
            // in the same UWorld, so none of the transition tests above fire and nothing
            // is re-armed - which is the second way the overlay could get stuck. Detect
            // it from the position itself, re-run the widget sweep (the fast-travel menu
            // is tearing down as we land), confirm the world's idea of the player pawn
            // still names our object, and publish the event so the render side can drop
            // its position-derived caches.
            if (snap.has_pawn)
            {
                if (g_have_last_pos)
                {
                    const double dx = snap.x - g_last_x;
                    const double dy = snap.y - g_last_y;
                    const double dz = snap.z - g_last_z;
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) > g_tune.teleport_uu)
                    {
                        g_teleport_ms = now;
                        g_force_widget_sweep = true;
                        mm::logf(L"teleport detected: {:.0f} {:.0f} {:.0f} -> {:.0f} {:.0f} {:.0f}; "
                                 L"re-resolving the pawn and re-sweeping the widgets",
                                 g_last_x,
                                 g_last_y,
                                 g_last_z,
                                 snap.x,
                                 snap.y,
                                 snap.z);
                        // Re-resolve rather than trust the cached pointer: if the game
                        // re-possessed a new pawn during the fade, FindAllOf names it and
                        // we switch; if it did not, this is a no-op that costs one
                        // FindAllOf. Either way the caches keyed to the class are kept
                        // only when the object is unchanged.
                        UObject* before = g_pawn.obj;
                        resolve_pawn(now);
                        if (g_pawn.obj != before)
                        {
                            mm::log(L"the teleport re-possessed the pawn - snapshot deferred one pump");
                            g_state_ok_since = 0;
                            g_have_last_pos = false;
                            publish_hidden(now, true);
                            return;
                        }
                    }
                }
                g_last_x = snap.x;
                g_last_y = snap.y;
                g_last_z = snap.z;
                g_have_last_pos = true;
            }
            snap.teleport_ms = g_teleport_ms;

            // Re-read EVERY pump: during a fade the view target is the cutscene camera
            // and it must be re-read, never remembered, or `is_pawn_view` latches false.
            UObject* view = read_view_target();
            snap.is_pawn_view = (view != nullptr && view == pawn);

            // A FREE DISCOVERY TRIGGER. The view target leaving (or returning to) the
            // pawn is the game switching to a menu / cutscene camera, and it is already
            // read every pump for the overlay's own gate - so it costs nothing to arm the
            // discovery walk off it. In the 2026-09-03 run it fired 1.1 s BEFORE the sweep
            // found the shrine menu's root (`view target is not the pawn` at 16:40:52.9,
            // `menu root discovered: WB_PlumeArchive_Main_C` at 16:40:54.0), so on this
            // game it is the earliest signal available that a never-seen menu root may
            // have just been built. It is only ever an ARM: no menu state is derived from
            // it, and a menu that does not move the camera is still found by the walk.
            if (snap.is_pawn_view != g_last_pawn_view)
            {
                g_last_pawn_view = snap.is_pawn_view;
                g_force_widget_sweep = true;
            }

            // The grace timer the overlay gates on: how long we have continuously had a
            // validated gameplay pawn with a readable location.
            if (snap.has_pawn)
            {
                if (g_state_ok_since == 0)
                {
                    g_state_ok_since = now;
                }
            }
            else
            {
                g_state_ok_since = 0;
            }
            snap.state_ok_since_ms = g_state_ok_since;

            mm::publish(snap);
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            g_report_pending.store(true, std::memory_order_relaxed);

            // The marker sweep runs LAST, on the same validated state and inside the
            // same re-entrancy guard: one slice of the chunked GUObjectArray walk.
            // Putting it after the publish means a slow slice can never delay the
            // position the overlay draws with. Most slices are taken by the fast path
            // above, between position pumps.
            markers::game_thread_pump(now, g_world);
        }
    } // namespace

    std::uint64_t pump_calls()
    {
        return g_pump_calls.load(std::memory_order_relaxed);
    }

    const char* pump_stage()
    {
        const char* s = g_pump_stage.load(std::memory_order_relaxed);
        return s != nullptr ? s : "?";
    }

    void on_unreal_init()
    {
        if (g_registered.exchange(true))
        {
            return;
        }
        // FIX (A.20): the marker/map data was dumped from ONE game build. If the game
        // has been patched since, actor ids and world coordinates can have moved and the
        // symptom is markers in the wrong place - so the mismatch is named here, once,
        // rather than guessed at from a bug report.
        mm::check_game_build();
        RC::Unreal::Hook::RegisterProcessEventPreCallback(
            [](UObject*, RC::Unreal::UFunction*, void*) { pump(); });
        mm::log(L"game-state reader registered on the ProcessEvent game-thread pump "
                L"(position 10 Hz, pawn/controller resolve 2 Hz, menu test EVERY pump on the cached "
                L"in-viewport roots + a SLICED GUObjectArray walk for menu discovery - 8192 slots "
                L"per slice, one round per 0.25-2 s, no FindAllOf; pawn validated through "
                L"GUObjectArray every pump, class gate 'BP_CombatCharacter_Player')");
    }

    void on_update()
    {
        // Loop thread. One summary line every 10 s so the log shows the reader is alive
        // without drowning it.
        // TWO CADENCES, ONE FUNCTION.
        //
        // The "is the pump alive?" check keeps its 10-second window whatever the log
        // level - it is the only thing that notices the game-thread reader has stopped,
        // and it is silent unless something is wrong.
        //
        // The `state:` line itself is the running commentary: at `verbose` it keeps the
        // 10-second cadence it has always had, and at `normal` it is the periodic HEALTH
        // SUMMARY, once a minute. Run 5 spent 236 of its 2600 lines on it.
        static std::uint64_t last_report = 0;
        static std::uint64_t last_state = 0;
        const std::uint64_t now = ::GetTickCount64();
        if (now - last_report < 10000)
        {
            return;
        }
        last_report = now;
        if (!g_report_pending.exchange(false))
        {
            mm::log(L"game-state reader: no snapshot in the last 10 s (the ProcessEvent pump is not firing)");
            return;
        }
        const std::uint64_t state_period = mm::log_enabled(mm::LogLv::Verbose) ? 10000 : 60000;
        if (last_state != 0 && now - last_state < state_period)
        {
            return;
        }
        last_state = now;
        mm::Snapshot snap{};
        if (!mm::read_snapshot(snap))
        {
            return;
        }
        mm::logf(L"state: pawn {} pos {:.0f} {:.0f} {:.0f} yaw {:.0f} ({}) | chapter {} ({} level(s), "
                 L"map \"{}\") | pawn-view {} | menu {} "
                 L"(last change {} ms ago, {} cached root(s)) | widgets {}/{} | "
                 L"sweep every {} ms (watchlist {}) | byte-Visible {} last round, {} awaiting "
                 L"a commit, {} over the cap | {}{} publishes",
                 snap.has_pawn ? L"yes" : L"NO",
                 snap.x,
                 snap.y,
                 snap.z,
                 static_cast<double>(snap.yaw),
                 snap.loc_from_function ? L"K2_GetActorLocation" : L"RootComponent",
                 g_chapter == chid::kNone  ? std::wstring{L"?"}
                 : g_chapter == chid::kDlc ? std::wstring{L"DLC"}
                                           : std::to_wstring(g_chapter),
                 g_chapter_levels,
                 [] {
                     const std::string key = mapdata::active_chapter_key();
                     return std::wstring{key.begin(), key.end()};
                 }(),
                 snap.is_pawn_view ? L"yes" : L"no",
                 snap.menu_open ? L"OPEN" : L"no",
                 snap.menu_change_ms == 0 ? 0ull : ::GetTickCount64() - snap.menu_change_ms,
                 snap.menu_roots_cached,
                 snap.widgets_visible_in_viewport,
                 snap.widgets_seen,
                 snap.widget_sweep_period_ms,
                 snap.menu_watch_count,
                 // THE THREE NUMBERS THAT WOULD HAVE NAMED THE CAP. `byte-Visible` is the
                 // population the candidate cap applies to, and it is NOT the 5-6
                 // in-viewport roots the cap was sized from; `over the cap` is how many
                 // were never tested.
                 g_wcand_round_pub.load(std::memory_order_relaxed),
                 g_wpending_pub.load(std::memory_order_relaxed),
                 g_wcand_dropped_pub.load(std::memory_order_relaxed),
                 snap.transition ? L"TRANSITION | " : L"",
                 g_publishes.load());
    }
} // namespace gamestate
