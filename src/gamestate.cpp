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

        // Defaults for the `reader_*` config keys; the live values are in `g_tune`.
        constexpr std::uint64_t kPositionPeriodMs = 100; // 10 Hz: pawn location + yaw
        constexpr std::uint64_t kResolvePeriodMs = 500;  // 2 Hz: FindAllOf for pawn / controller
        // Fast cadence of the discovery walk's adaptive schedule (scan::SweepSched): the
        // period doubles up to reader_widget_sweep_max_period_ms while nothing new is
        // discovered; a menu flip / teleport / world change / view-target change resets it.
        constexpr std::uint64_t kWidgetFullPeriodMs = 250;
        // Sanity cap on one FindAllOf pass - the fallback path only.
        constexpr std::size_t kMaxWidgets = 6000;
        constexpr std::size_t kMaxMenuRoots = 32;  // cache cap; the game only ever has 5-6

        // After ANY pawn / world change, no UFunction is called for this long: a blueprint
        // getter issued mid-LoadMap faults.
        constexpr std::uint64_t kTransitionCooldownMs = 2000;

        constexpr std::uint64_t kLogThrottleMs = 5000;
        // Floor for the "not a gameplay pawn" line when the class name changes.
        constexpr std::uint64_t kRejectFlipThrottleMs = 1000;

        // How often the streamed level set is walked to name the chapter.
        constexpr std::uint64_t kChapterPeriodMs = 1000;
        constexpr int kMaxLevelsScanned = 4096;

        // The player's own pawn class; the controller is the fallback route.
        constexpr const wchar_t* kPlayerPawnClass = L"BP_CombatCharacter_Player_Final_C";
        constexpr const wchar_t* kControllerClass = L"DCSPlayerController_C";

        // CLASS GATE. Only a pawn whose class name contains this is ever read: the Lobby
        // `DefaultPawn` and `SpectatorPawn` both sit inside Chapter 1's bounds.
        constexpr const wchar_t* kGameplayPawnSubstr = L"BP_CombatCharacter_Player";

        // State (game thread only, except the atomics)

        std::atomic<bool> g_registered{false};
        std::atomic<std::uint64_t> g_pump_calls{0};
        // THE GAME-THREAD LATCH. ProcessEvent also arrives from async-loading, audio and
        // worker threads; only the latched thread may touch the state below.
        std::atomic<unsigned long> g_pump_thread{0};
        std::atomic<bool> g_wrong_thread_logged{false};
        // What the game thread is doing, for the loop thread's stall watchdog. A relaxed
        // store of a string literal: no allocation, no lock. It names the STEP only.
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
        // The last world ever seen; drop_pawn does NOT clear it, so
        // controller_from_game_instance() still has a starting point after a drop.
        const void* g_world_token = nullptr;

        std::uint64_t g_last_position = 0;
        // ONE BUDGET PER THING RESOLVED: a shared timestamp would let the controller step
        // starve the pawn step.
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
        // How long the reader tolerates having no gameplay pawn before it drops its cached
        // PlayerController and re-resolves it, and before the one-shot diagnosis prints.
        constexpr std::uint64_t kNoPawnCtrlDropMs = 3000;
        constexpr std::uint64_t kNoPawnDiagnoseMs = 5000;
        std::uint64_t g_last_ctrl_drop = 0;
        bool g_pawnless_diag_done = false;

        bool g_menu_open = true; // safe default: "a menu is up" hides the minimap
        std::uint64_t g_menu_change_ms = 0;
        std::uint32_t g_widgets_seen = 0;
        std::uint32_t g_widgets_visible = 0;

        // Root widgets currently confirmed `IsInViewport()`. Rebuilt every pump: the
        // Visibility byte is a prefilter, `IsInViewport()` is the answer.
        std::vector<uer::ObjRef> g_menu_roots;

        // Every widget that has ever confirmed as an in-viewport `Visible` root this
        // session. A CANDIDATE list, never an answer: `g_menu_roots` is rebuilt from live
        // tests every pump. Entries leave only when the object dies or the world changes.
        std::vector<uer::ObjRef> g_menu_watch;

        // Adaptive cadence of the discovery sweep (pure arithmetic in scan_sched.hpp).
        scan::SweepSched g_sweep{};

        // The SLICED discovery walk over GUObjectArray; one wrap is a ROUND.
        //   * the SLICE (`widget_scan_pump`) does raw reads only and issues no
        //     ProcessEvent, so it can ride the fast path between position pumps.
        //   * the COMMIT (`commit_widget_round`) runs on the 10 Hz pump and is the only
        //     place that calls `IsInViewport()`. The answer is REBUILT, never merged.
        scan::Cursor g_wcursor{};
        bool g_wround_active = false;      // a round is walking right now
        bool g_wround_ready = false;       // a round has wrapped and awaits the commit
        std::uint64_t g_wslice_us = 0;     // QPC of the last slice
        std::uint32_t g_wseen_round = 0;   // UserWidget instances this round has seen
        std::uint32_t g_wcand_dropped = 0; // candidates the cap refused this round
        std::uint32_t g_wcand_round = 0;   // byte-Visible candidates this round produced
        // Candidates are ObjRefs, not raw pointers: a widget can die in the pump of delay,
        // and the commit issues a ProcessEvent at them. Committed on the next validated
        // pump, not at the end of the round - see scan::menu_open_from.
        std::vector<uer::ObjRef> g_wpending;
        // Published copies for the loop thread's state line; the originals are game-thread-only.
        std::atomic<std::uint32_t> g_wcand_round_pub{0};
        std::atomic<std::uint32_t> g_wpending_pub{0};
        std::atomic<std::uint32_t> g_wcand_dropped_pub{0};
        // "Is this UClass* a UUserWidget descendant?" - one super-chain name walk per class,
        // then a hash lookup per object. The cap must clear the whole GAME's class count.
        std::unordered_map<RC::Unreal::UClass*, unsigned char> g_wclass;
        constexpr std::size_t kWidgetClassCacheMax = 262144;
        // Class NAMES whose not-a-menu line has been printed. Never cleared, which is what
        // makes the diagnostic once-per-session.
        std::unordered_set<std::wstring> g_non_menu_logged;
        // Set if `FUObjectArray::GetNumElements()` cannot answer (UE4SS did not resolve
        // GUObjectArray on this build); the whole-array `FindAllOf` sweep is the fallback.
        bool g_wfallback = false;

        // How many known-menu-class candidates lead `g_wpending`. New known-class
        // candidates are inserted behind them (scan::candidate_insert_at), so the per-pump
        // commit cap can never push a likely menu root behind a burst of unknown widgets.
        int g_wknown_front = 0;

        // Menu root CLASS names confirmed this session (scan_sched.hpp). Everything else in
        // this file that remembers a menu is keyed by POINTER and dies with the world; this
        // survives, so the first menu after a level load is a known class again.
        scan::MenuRootNames g_menu_root_names{};

        //---- the UI-EVENT path -------------------------------------------------------
        // ProcessEvent itself says which object is doing something. A widget event names a
        // menu the instant the game touches it, ~2.5 s before the discovery walk would have
        // reached it. The callback only OFFERS a candidate: `IsInViewport()` on the next
        // 10 Hz pump is still the whole decision.

        // Widgets the event path has already offered, and when (GetTickCount64). Bounds the
        // cost of an event storm on one widget to a single hash lookup, and the re-offer
        // window keeps a widget that was parked when it was offered from being ignored
        // forever. Pointer-keyed, so drop_pawn clears it.
        std::unordered_map<UObject*, std::uint64_t> g_wevent_offered;
        constexpr std::size_t kEventOfferedMax = 256;
        constexpr std::uint64_t kEventReofferMs = 1000;

        // Event-sourced candidates accepted since the last 10 Hz pump. The cap is what keeps
        // a frame of widget churn from filling the pending list.
        int g_wevent_added = 0;
        constexpr int kEventCandidatesPerPump = 32;

        // Outer hops the walk from an arbitrary widget to its root UserWidget may take:
        // child -> WidgetTree -> UserWidget -> WidgetTree -> ... -> game instance. Three
        // levels of nesting is more than this game's UI uses.
        constexpr int kWidgetOuterHops = 6;

        // How the root that flipped the menu state was found, and how long after the
        // triggering UI event - the numbers the verbose flip line reports.
        const wchar_t* g_commit_route = L"sweep";
        std::uint64_t g_commit_lat_ms = 0;

        // Perf counter ids (perf.hpp). Namespace-scope rather than function statics: a
        // guarded static's first call would run the CRT thread-safe-init path here.
        int g_pf_position = -1;
        int g_pf_sweep = -1;
        int g_pf_wslice = -1;
        int g_pf_wcommit = -1;
        int g_pf_retest = -1;
        int g_pf_chapter = -1;

        // The level set last handed to markers::set_loaded_levels(). Cleared with the
        // marker module's caches, so the next enumeration republishes.
        std::vector<std::string> g_last_levels;

        // The root currently holding "a menu is open" true. Empty when no root is visible.
        std::wstring g_menu_holder;

        // Set whenever the menu state flips, a root leaves the viewport or the pawn
        // teleports: the next pump runs the full sweep.
        bool g_force_widget_sweep = true;

        // Previous pump's "is the view target the pawn?" answer; a change arms discovery.
        bool g_last_pawn_view = true;

        // Teleport detection. A shrine fast-travel keeps the same pawn object and UWorld, so
        // no transition test fires, yet every position-derived cache must be re-armed.
        double g_last_x = 0.0;
        double g_last_y = 0.0;
        double g_last_z = 0.0;
        bool g_have_last_pos = false;
        std::uint64_t g_teleport_ms = 0;
        // Movement inside one 100 ms pump that can only be a teleport, in uu. The player
        // sprints at ~700 uu/s, i.e. ~70 uu per pump.
        constexpr double kTeleportJumpUu = 3000.0;

        // mm::config() copies the whole Config under a spinlock, too much for a callback the
        // engine fires thousands of times a second. Everything below uses `g_tune`.

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

        // Chapter detection, from the streamed level set (chapterid.hpp).
        std::uint64_t g_last_chapter = 0;
        int g_chapter = chid::kNone;
        int g_chapter_levels = 0;
        // Which enumeration route worked: 0 = none yet, 1 = UWorld::Levels,
        // 2 = UWorld::StreamingLevels -> ULevelStreaming::LoadedLevel, 3 = FindAllOf.
        int g_chapter_route = 0;

        // "SAY IT ONCE, THEN RARELY" throttle. The FIRST occurrence always prints, then at
        // most one line per kPersistentLogMs carrying how many were suppressed.
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

        // Called whenever the pawn pointer, its class or its world stops matching what was
        // captured: every cached UFunction* and property offset came from the dead class.

        void drop_pawn(std::uint64_t now, const wchar_t* why)
        {
            const bool had_pawn = !g_pawn.empty();
            g_pawn.reset();
            g_pawn_is_gameplay = false;
            g_world = nullptr; // g_world_token deliberately keeps the old value
            g_pawn_class_name.clear();
            g_pawn_full_name.clear();
            g_pawn_short_name.clear();
            g_state_ok_since = 0;
            // THE MENU STATE MUST NOT OUTLIVE ITS EVIDENCE. With the watchlist and the root
            // cache emptied below, the only answer that cannot latch is CLOSED.
            g_menu_open = false;
            g_menu_change_ms = now;
            g_menu_roots.clear();
            g_menu_watch.clear();
            // The sliced discovery walk is keyed to that world too, and a recycled UClass*
            // address would answer from the wrong memo entry. The cursor restarts.
            g_wpending.clear();
            g_wknown_front = 0;
            g_wevent_offered.clear();
            g_wevent_added = 0;
            g_wclass.clear();
            g_wcursor = scan::Cursor{};
            g_wround_active = false;
            g_wround_ready = false;
            g_wseen_round = 0;
            g_wcand_dropped = 0;
            g_wcand_round = 0;
            g_widgets_seen = 0;
            g_widgets_visible = 0;
            g_controller.reset();
            g_menu_holder.clear();
            g_force_widget_sweep = true;
            g_have_last_pos = false;
            g_last_chapter = 0; // re-detect the chapter as soon as a pawn is back
            g_funcs.clear();
            g_layouts.clear();
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

        // Raw property reads only - no ProcessEvent, no allocation. A controller Pawn
        // pointer that no longer equals the cached object means the cached one is stale.
        UObject* pawn_from_controller()
        {
            if (!uer::alive(g_controller))
            {
                return nullptr;
            }
            const uer::ClassLayout* layout = g_layouts.get(g_controller.obj);
            // Three names, in order of how directly they mean "the pawn this controller
            // drives right now". A shrine fast travel can leave `Pawn` null for minutes.
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

        // The world's own idea of the player controller: UWorld -> OwningGameInstance ->
        // LocalPlayers[0] -> PlayerController. All raw property reads, no ProcessEvent.
        // Preferred over `FindFirstOf`, which can hand back one from a dying world.
        UObject* controller_from_game_instance(bool& from_stale_world)
        {
            // The live world if there is one, otherwise the last one seen.
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
            // g_layouts.get() dereferences immediately (GetClassPrivate) outside any SEH
            // guard, so LocalPlayers[0] is validated first.
            if (!mem::readable(local_player, 0x40))
            {
                return nullptr;
            }
            const uer::ClassLayout* ll = g_layouts.get(local_player);
            return uer::read_object_prop(ll, local_player, L"PlayerController");
        }

        void resolve_controller()
        {
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

        // One-shot diagnostic for "no gameplay pawn, forever": the controller and its three
        // pawn properties, every instance of the player class, and the world.
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
            // EVERY ROUTE IS TRIED, AND THE FIRST CANDIDATE THAT PASSES THE CLASS GATE WINS -
            // not the first candidate that exists. The controller's own pointers come first,
            // but they also hand out the Lobby `DefaultPawn`, so a wrong class falls through.
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
            // Last resort: the substring gate; `FindAllOf` matches subclasses.
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

            // THE WORLD CROSS-CHECK. The controller is resolved independently, so its world is
            // the reference; a token taken from the pawn itself could never fire.
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
                    // A new class name reports promptly, floored at kRejectFlipThrottleMs.
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
                // Every candidate was rejected by the class gate or the world check; both log.
                return;
            }

            g_pawn = ref;
            g_pawn_is_gameplay = true;
            g_pawn_class_name = cls;
            g_pawn_short_name = ref.obj->GetName();
            g_pawn_full_name = ref.obj->GetFullName();
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

        // Both routes are only reached with a pawn that passed uer::alive() in this same
        // pump, outside the cooldown; every ProcessEvent runs in an SEH guard.

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

        // APlayerCameraManager has NO GetViewTarget(); the view target lives on the
        // PlayerController. The camera manager's `ViewTarget` struct starts with the AActor*.

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

        // Only *root* widgets are ever IsInViewport() (5-6 of ~1700 instances), and every
        // menu adds exactly one root whose visibility is ESlateVisibility::Visible, so "a
        // menu is open" == "some in-viewport widget is Visible".
        // UWidget::Visibility is a reflected TEnumAsByte and Visible == 0, so the byte is
        // the prefilter. Only called with a validated gameplay pawn, outside the cooldown.

        void set_menu_open(bool open, std::uint64_t now, const wchar_t* why, const wchar_t* route,
                           std::uint64_t lat_ms)
        {
            if (open != g_menu_open)
            {
                g_menu_open = open;
                g_menu_change_ms = now;
                // A flip in either direction invalidates the root cache. The schedule is armed
                // here as well as the flag: set_menu_open runs at the END of the pump and the
                // flag is consumed near its START. Arming twice is idempotent.
                g_force_widget_sweep = true;
                scan::sweep_arm(g_sweep, now);
                // `route` is which of the three finders answered - the watchlist re-test, a
                // candidate the ProcessEvent callback offered, or the discovery walk - and
                // `lat_ms` is the age of the triggering UI event, 0 when there was none.
                MM_LOGV(L"menu state -> {} ({}; found via {}, {} ms after the triggering UI "
                        L"event); a full widget sweep is queued",
                        open ? L"OPEN" : L"closed",
                        why,
                        route,
                        lat_ms);
            }
        }

        // Defined with the SLICED walk below; the FindAllOf fallback needs the same gate.
        unsigned char widget_class_kind(UObject* obj);
        bool widget_may_be_menu(UObject* obj);
        void remember_menu_class(UObject* w);

        // Is this widget's reflected Visibility byte ESlateVisibility::Visible (0)? Raw read.
        bool widget_is_visible_byte(UObject* w, bool& has_byte)
        {
            const uer::ClassLayout* layout = g_layouts.get(w);
            std::uint8_t vis = 0xFF;
            has_byte = uer::read_prop(layout, w, L"Visibility", vis, 1);
            return has_byte && vis == 0;
        }

        // Put a confirmed in-viewport root on the watchlist. Returns true when it was not
        // already there: the "discovery found something" signal (scan::sweep_done).
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
                // THE CAP IS NOT "NOTHING NEW": a root was discovered and could not be recorded,
                // so discovery must stay fast - hence true. The game only ever has 5-6 roots.
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
                remember_menu_class(w);
                MM_LOGV(L"menu root discovered: {} (watchlist now {} widget(s); they are "
                        L"re-tested at 10 Hz, so this menu is caught within one pump from "
                        L"now on)",
                        w->GetName(),
                        g_menu_watch.size());
                return true;
            }
            return false;
        }

        // ESlateVisibility::Visible, by the reflected byte where the class has one and by the
        // getter where it does not. The FindAllOf sweep and the commit must agree on this.
        bool widget_says_visible(UObject* w)
        {
            bool has_byte = false;
            const bool byte_visible = widget_is_visible_byte(w, has_byte);
            if (has_byte)
            {
                return byte_visible;
            }
            // No reflected Visibility on this class: ask the getter instead.
            struct RetByte
            {
                std::uint8_t v = 0xFF;
            } ret{};
            return uer::call_getter(g_funcs, w, L"GetVisibility", ret) && ret.v == 0;
        }

        // One ProcessEvent, SEH-guarded in uer::call_getter. Asked only of widgets that
        // already said Visible.
        bool widget_in_viewport(UObject* w)
        {
            struct RetBool
            {
                bool v = false;
            } ret{};
            return uer::call_getter(g_funcs, w, L"IsInViewport", ret) && ret.v;
        }

        // THE PER-PUMP TEST (10 Hz), over the watchlist only.
        // The `Visibility` byte is a PREFILTER, never the answer: Wuchang takes
        // `WB_MenuMain_C` out of the viewport but leaves its Visibility at Visible, so the
        // byte alone latches "menu open" forever. `g_menu_roots` is REBUILT from this pass.
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
                    // Visible but not in the viewport = that menu is closed. The widget STAYS on
                    // the watchlist: it is how the next open is caught in one pump.
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
                // A root opening or closing may also have built an unseen menu root.
                g_force_widget_sweep = true;
            }
            return menu;
        }

        // THE FULL SWEEP IN ONE CALL - the FALLBACK path for a build where
        // `FUObjectArray::GetNumElements()` cannot answer. `FindAllOf` walks the whole
        // object array, ~25 ms on the game thread. REBUILDS the root cache from scratch.
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
                if (!widget_says_visible(w))
                {
                    continue; // not ESlateVisibility::Visible
                }
                if (widget_in_viewport(w))
                {
                    ++visible_in_viewport;
                    if (!menu)
                    {
                        holder = w->GetName();
                    }
                    menu = true;
                    // Watched from now on: re-tested every pump.
                    discovered_new = watch_menu_root(w) || discovered_new;
                    uer::ObjRef ref{};
                    if (uer::capture(w, ref))
                    {
                        confirmed.push_back(ref);
                    }
                }
            }

            // REBUILD, do not merge: `confirmed` is the complete live answer.
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

        // Is this object's class a `UUserWidget` descendant? Walk the super chain comparing
        // NAMES - there is no `UUserWidget::StaticClass()` to compare against - and memoise
        // per `UClass*`.
        //   0 = not a UUserWidget at all
        //   1 = a widget that MAY hold a menu
        //   2 = a widget whose class is on the not-a-menu deny-list, matched on the CLASS
        //       name (an object name is index-suffixed). See scan_sched.hpp.
        //   3 = 1, plus this class has already confirmed as a menu root this session, so a
        //       candidate of it leads the pending list. A PRIORITY, not an answer.
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
                    // Once per class per session, naming the reason.
                    if (g_non_menu_logged.insert(cname).second)
                    {
                        mm::logf(L"menu detector: '{}' is on the not-a-menu list ({}) - it can never "
                                 L"hide the minimap",
                                 cname,
                                 std::wstring(why, why + std::strlen(why)));
                    }
                }
                else if (scan::menu_root_known(g_menu_root_names, cname.c_str()))
                {
                    // The memo died with the last world; the NAME set did not.
                    kind = 3;
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

        // Could this widget hold a menu? A deny-listed widget is still walked and counted,
        // so the state line's `widgets N/M` numbers do not change.
        bool widget_may_be_menu(UObject* obj)
        {
            const unsigned char kind = widget_class_kind(obj);
            return kind == 1 || kind == 3;
        }

        // A class that has already held a menu this session. Its candidates lead the pending
        // list; the confirmation they must pass is unchanged.
        bool widget_class_is_known_menu(UObject* obj)
        {
            return widget_class_kind(obj) == 3;
        }

        // Record a confirmed root's class name, so the next world starts knowing it, and
        // promote the live memo entry to kind 3 at once.
        void remember_menu_class(UObject* w)
        {
            RC::Unreal::UClass* cls = w->GetClassPrivate();
            if (cls == nullptr)
            {
                return;
            }
            const std::wstring cname = static_cast<UObject*>(cls)->GetName();
            if (scan::remember_menu_root(g_menu_root_names, cname.c_str()))
            {
                MM_LOGV(L"menu root class remembered: {} ({} name(s) now survive a level "
                        L"transition)",
                        cname,
                        g_menu_root_names.count);
            }
            g_wclass[cls] = 3;
        }

        // THE ONE PLACE A CANDIDATE ENTERS `g_wpending`. Returns false when the list is full
        // or the object could not be captured. A known-menu class jumps the queue instead of
        // appending; nothing here decides anything, the commit's `IsInViewport()` does.
        bool push_widget_candidate(UObject* obj, bool known_class)
        {
            const int pending = static_cast<int>(g_wpending.size());
            if (pending >= scan::kWidgetCandidateMax)
            {
                ++g_wcand_dropped;
                return false;
            }
            uer::ObjRef ref{};
            if (!uer::capture(obj, ref))
            {
                return false;
            }
            const int at = scan::candidate_insert_at(pending, g_wknown_front, known_class);
            g_wpending.insert(g_wpending.begin() + static_cast<std::ptrdiff_t>(at), ref);
            if (known_class)
            {
                ++g_wknown_front;
            }
            ++g_wcand_round;
            return true;
        }

        // ONE SLICE. Raw reads only - no ProcessEvent - so it is safe on the fast path
        // between position pumps. Rejects cheapest first: FUObjectItem validity read through
        // the object ARRAY (safe on a freed allocation), the memoised class test,
        // IsValidObjectForFindXOf, then the Visibility byte.
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
                // ESlateVisibility::Visible == 0. A class with no reflected Visibility stays a
                // candidate so the commit can ask `GetVisibility()` instead.
                bool has_byte = false;
                const bool byte_visible = widget_is_visible_byte(obj, has_byte);
                if (has_byte && !byte_visible)
                {
                    continue;
                }
                push_widget_candidate(obj, widget_class_is_known_menu(obj));
            }
        }

        // Drive the walk: start a round when the schedule says a sweep is due, then take one
        // slice per `kWidgetSlicePeriodMs`. The slice rate is set here, never by the caller.
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
                // `g_wpending` is NOT cleared: a candidate from the previous round awaits commit.
                g_wseen_round = 0;
                g_wcand_dropped = 0;
                g_wcand_round = 0;
            }
            if (!scan::slice_due(now_us, g_wslice_us, scan::kWidgetSlicePeriodMs))
            {
                return;
            }
            g_wslice_us = now_us;
            // Re-read every slice: the array grows and can shrink, so the cursor is clamped.
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

        // THE COMMIT, ON EVERY VALIDATED PUMP - not per round, so a menu that opens and
        // closes inside one round is still confirmed.
        // The byte-`Visible` candidates the slices collected are the only widgets that pay
        // for an `IsInViewport()` ProcessEvent. IT CAN ONLY ADD (scan::menu_open_from).
        bool commit_widget_candidates(std::uint64_t now, std::wstring& holder, bool& discovered_new)
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
                // Re-read the byte rather than trusting the slice's.
                if (!widget_says_visible(w))
                {
                    continue;
                }
                if (!widget_in_viewport(w))
                {
                    continue;
                }
                if (!menu)
                {
                    holder = w->GetName();
                    // WHICH FINDER ANSWERED. A widget the ProcessEvent callback offered is
                    // still in the offer memo, and its entry is when the event fired.
                    const auto offered = g_wevent_offered.find(w);
                    if (offered != g_wevent_offered.end())
                    {
                        g_commit_route = L"event";
                        g_commit_lat_ms = now >= offered->second ? now - offered->second : 0;
                    }
                    else
                    {
                        g_commit_route = L"sweep";
                        g_commit_lat_ms = 0;
                    }
                }
                menu = true;
                // Watched from now on: re-tested every pump.
                discovered_new = watch_menu_root(w) || discovered_new;
                // Open NOW: `g_menu_roots` is the set of roots open this pump.
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
            g_wknown_front = g_wknown_front > take ? g_wknown_front - take : 0;
            return menu;
        }

        // THE UI-EVENT PATH, on EVERY ProcessEvent - thousands per second.
        //
        // RAW READS ONLY. It issues no ProcessEvent (it runs outside the re-entrancy guard)
        // and decides nothing: all it does is offer a candidate the next 10 Hz commit will
        // put through the same `IsInViewport()` test as any other. The deny-list still wins,
        // because a deny-listed class never reaches kind 1 or 3.
        //
        // The cost for the overwhelming majority of events - an actor or component, not a
        // widget - is ONE hash lookup on the class pointer. A widget event adds up to six
        // outer reads and one lookup in the offer memo.
        void note_ui_event(UObject* ctx, RC::Unreal::UFunction* fn, std::uint64_t now)
        {
            if (ctx == nullptr || fn == nullptr || g_wfallback)
            {
                return; // the FindAllOf fallback never commits `g_wpending`
            }
            if (g_wevent_added >= kEventCandidatesPerPump)
            {
                return; // this pump's budget is spent; the next one reopens it
            }
            unsigned char kind = widget_class_kind(ctx);
            if (kind == 0)
            {
                return; // not a UUserWidget: 99 % of events stop here
            }

            // UP TO THE ROOT. A widget's outer chain is
            // child -> WidgetTree -> the owning UserWidget -> ... -> the game instance, so
            // the topmost UserWidget in the chain is the root `IsInViewport()` can confirm.
            UObject* root = (kind == 1 || kind == 3) ? ctx : nullptr;
            UObject* at = ctx;
            for (int hop = 0; hop < kWidgetOuterHops; ++hop)
            {
                if (!mem::readable(at, 0x40))
                {
                    break;
                }
                UObject* outer = at->GetOuterPrivate();
                if (outer == nullptr || !mem::plausible_ptr(outer) || !mem::readable(outer, 0x40))
                {
                    break;
                }
                at = outer;
                kind = widget_class_kind(at);
                if (kind == 0)
                {
                    break; // the WidgetTree's owner ran out: this is the game instance
                }
                if (kind == 1 || kind == 3)
                {
                    root = at;
                }
            }
            if (root == nullptr)
            {
                return; // every widget in the chain is deny-listed
            }

            // Already known? The watchlist re-test answers those in one pump on its own.
            for (const uer::ObjRef& ref : g_menu_watch)
            {
                if (ref.obj == root)
                {
                    return;
                }
            }
            // Offered recently? The re-offer window bounds an event storm on one widget to a
            // hash lookup, and still lets a widget that was parked when it was offered be
            // re-offered once it is not.
            const auto seen = g_wevent_offered.find(root);
            if (seen != g_wevent_offered.end() && now >= seen->second &&
                now - seen->second < kEventReofferMs)
            {
                return;
            }
            // The prefilter the slice applies, so an event does not offer a parked widget.
            bool has_byte = false;
            const bool byte_visible = widget_is_visible_byte(root, has_byte);
            if (has_byte && !byte_visible)
            {
                return;
            }
            if (!push_widget_candidate(root, widget_class_kind(root) == 3))
            {
                return;
            }
            ++g_wevent_added;
            if (g_wevent_offered.size() >= kEventOfferedMax)
            {
                g_wevent_offered.clear(); // bounded; the window would have expired anyway
            }
            g_wevent_offered[root] = now;
            // Belt and braces: the commit runs every pump regardless, but a menu opening is
            // also exactly when the discovery walk should be back on its fast cadence.
            scan::sweep_arm(g_sweep, now);
        }

        // End-of-round bookkeeping: the counts the state line reports, and the one place an
        // over-full candidate list is reported.
        void finish_widget_round()
        {
            g_widgets_seen = g_wseen_round;
            if (g_wcand_dropped != 0)
            {
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

        // From the STREAMED LEVEL SET, never from the position: the five chapters' world
        // bounds overlap. chapterid.hpp holds the string logic and the tiered vote.
        // Three routes, tried in order, and the one that worked is logged once:
        //   1. `UWorld::Levels`          - TArray<ULevel*>, ~50 entries.
        //   2. `UWorld::StreamingLevels` - TArray<ULevelStreaming*>, ~834 entries, of
        //                                  which the loaded ones have a `LoadedLevel`.
        //   3. `FindAllOf(L"Level")`     - a whole GUObjectArray walk, 28-51 ms.

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

        // Validates `obj` as a live UObject and feeds its full name to the vote. Also
        // collects the SHORT name ("Chapter1_DGong_logic") for
        // markers::set_loaded_levels(). Names are ASCII.
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
                    // ULevelStreaming::LoadedLevel is null for a sublevel that is not streamed in.
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

        // ENUMERATE THE STREAMED LEVELS by whichever of the three routes works. Returns the
        // route used (0 = none named a level) and fills the chapter vote and the short
        // names of every level it saw.
        int enumerate_levels(UObject* world,
                             chid::Vote& vote,
                             int& counted,
                             std::vector<std::string>& levels)
        {
            int route = 0;
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
            return route;
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
            // The short names of every level this walk sees, for the marker sweep's absence
            // rule (markers.hpp).
            std::vector<std::string> levels;
            const int route = enumerate_levels(world, vote, counted, levels);

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

            // Published even when the chapter vote is undecided: the two answers are
            // independent. Only on an actual change - the enumeration order is the world's
            // own level order, so an element-wise compare is exact enough.
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
            // Diagnostics describe the reader, not the struct's defaults. has_pawn stays false,
            // so nothing is shown on the strength of them.
            snap.menu_open = g_menu_open;
            snap.menu_roots_cached = static_cast<std::uint32_t>(g_menu_roots.size());
            snap.menu_watch_count = static_cast<std::uint32_t>(g_menu_watch.size());
            snap.widget_sweep_period_ms = static_cast<std::uint32_t>(scan::sweep_period_ms(g_sweep, now));
            snap.widgets_visible_in_viewport = g_widgets_visible;
            mm::publish(snap);
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            g_report_pending.store(true, std::memory_order_relaxed);
        }

        // The pump, one named step per stage. Same order, same early returns, same
        // g_pump_stage labels - the stall watchdog reports the step it froze in.

        void pump_fast_slice(std::uint64_t now, int& depth)
        {
            // BETWEEN position pumps: the sliced GUObjectArray walks, nothing else. Both are
            // self-throttled on QueryPerformanceCounter, so a call out of turn costs one QPC
            // read and a compare. They run on the LAST validated state: same re-entrancy
            // guard, same cooldown, and only with a pawn standing as of the last position pump.
            if (g_state_ok_since != 0 && now >= g_cooldown_until)
            {
                const DepthGuard slice_guard{depth};
                const StageMark mark{"fast slice: markers"};
                markers::game_thread_pump(now, g_world);
                // The menu-discovery walk rides here too. Raw reads only - its ProcessEvent
                // half is the commit, on the validated 10 Hz pump below.
                const StageMark wmark{"fast slice: widgets"};
                widget_scan_pump(now, mm::qpc_us());
            }
            // The stage breadcrumb must not be left naming work that has already finished.
            g_pump_stage.store("between pumps", std::memory_order_relaxed);
        }

        void pump_controller(std::uint64_t now)
        {
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
        }

        // Three independent tests, cheapest first:
        //   a) GUObjectArray liveness (index -> FUObjectItem -> flags + back
        //      pointer), which is safe to read even after the object was freed;
        //   b) the controller's own Pawn pointer still names the same object;
        //   c) the pawn's UWorld* is still the world we captured it in.
        bool pump_validate_pawn(std::uint64_t now)
        {
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
                    g_world_token = world; // the recovery route's starting point
                }
            }
            if (!pawn_ok && !g_pawn.empty())
            {
                drop_pawn(now, L"the cached pawn is no longer a live object");
            }

            return pawn_ok;
        }

        // NEVER gated on the controller, the menu state or the widget sweep: the pawn owns
        // its 2 Hz budget (g_last_resolve_pawn), and the controller is only a fallback.
        bool pump_resolve_pawn(std::uint64_t now, bool pawn_ok)
        {
            if (!pawn_ok && now - g_last_resolve_pawn >= g_tune.resolve_ms)
            {
                g_last_resolve_pawn = now;
                resolve_pawn(now);
                pawn_ok = g_pawn_is_gameplay && uer::alive(g_pawn);
            }

            if (!pawn_ok)
            {
                if (g_no_pawn_since == 0)
                {
                    g_no_pawn_since = now;
                }
                // RE-RESOLVE THE CONTROLLER TOO. A fast travel can leave it alive-but-wrong,
                // reporting a null `Pawn` forever, and the pawn resolve only ever asks it.
                if (now - g_no_pawn_since >= kNoPawnCtrlDropMs &&
                    now - g_last_ctrl_drop >= kNoPawnCtrlDropMs)
                {
                    g_last_ctrl_drop = now;
                    // The drop runs every kNoPawnCtrlDropMs; the LINE about it does not.
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
                return false;
            }
            g_no_pawn_since = 0;
            // A pawn is standing again: the next stall gets its own first-occurrence lines.
            g_pawnless_diag_done = false;
            rare_reset(g_rare_ctrl_drop);
            rare_reset(g_rare_stuck);

            return true;
        }

        // From here on the pawn is validated: UFunctions are allowed.
        // THE MENU TEST HAS TWO HALVES AND BOTH RUN ON EVERY PUMP.
        //   a) the WATCHLIST re-test: `IsInViewport()` over every root ever confirmed,
        //      which rebuilds `g_menu_roots` from scratch;
        //   b) the COMMIT of what the sliced walk newly saw as byte-`Visible`, which can
        //      only ADD a root, and puts it on the watchlist at the same time.
        void pump_widgets(std::uint64_t now)
        {
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

            // The discovery walk's cadence. This schedule bounds ONLY the latency of "a menu
            // whose root has never been seen this session opened".
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
                // discovery route, ~25 ms of game thread per run.
                if (scan::sweep_due(g_sweep, now))
                {
                    if (g_pf_sweep < 0)
                    {
                        g_pf_sweep =
                            mm::perf_register("widget sweep (FindAllOf, fallback)", perf::Thread::Game);
                    }
                    const std::uint64_t sweep_t0 = mm::qpc_us();
                    g_commit_route = L"sweep";
                    g_commit_lat_ms = 0;
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
                    commit_menu = commit_widget_candidates(now, commit_holder, discovered_new);
                }
                g_wcand_round_pub.store(g_wcand_round, std::memory_order_relaxed);
                g_wpending_pub.store(static_cast<std::uint32_t>(g_wpending.size()),
                                     std::memory_order_relaxed);
                g_wcand_dropped_pub.store(g_wcand_dropped, std::memory_order_relaxed);
                if (g_wround_ready)
                {
                    // The round has wrapped: report its counts and let the schedule back off.
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
            // A root already on the watchlist needed no finding; otherwise the commit named
            // which of the two discovery routes produced it.
            const wchar_t* route = watch_menu ? L"the watchlist" : (commit_menu ? g_commit_route : L"-");
            const std::uint64_t lat_ms = watch_menu ? 0 : (commit_menu ? g_commit_lat_ms : 0);
            set_menu_open(menu,
                          now,
                          menu ? (watch_menu ? L"a watchlist root is in the viewport and Visible"
                                             : L"the discovery walk just confirmed a new in-viewport "
                                               L"Visible root")
                               : L"no root is in the viewport and Visible",
                          route,
                          lat_ms);
        }

        // Which chapter's map asset should be resident. 1 Hz, on the validated state.
        void pump_chapter(std::uint64_t now)
        {
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
        }

        // The snapshot: position, teleport detection, the view target, the grace timer.
        // Returns false when it published a hidden snapshot instead and the pump is done.
        bool pump_publish(std::uint64_t now)
        {
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

            // TELEPORT / RE-POSSESSION. A shrine fast-travel keeps the same pawn object in the
            // same UWorld, so no transition test above fires. Detect it from the position,
            // re-sweep the widgets, and publish the event so the render side drops its caches.
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
                        // Re-resolve rather than trust the cached pointer: a re-possession during
                        // the fade is named by FindAllOf.
                        UObject* before = g_pawn.obj;
                        resolve_pawn(now);
                        if (g_pawn.obj != before)
                        {
                            mm::log(L"the teleport re-possessed the pawn - snapshot deferred one pump");
                            g_state_ok_since = 0;
                            g_have_last_pos = false;
                            publish_hidden(now, true);
                            return false;
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

            // The view target leaving or returning to the pawn is the earliest signal that a
            // never-seen menu root may have been built. Only an ARM - no menu state from it.
            if (snap.is_pawn_view != g_last_pawn_view)
            {
                g_last_pawn_view = snap.is_pawn_view;
                g_force_widget_sweep = true;
            }

            // The grace timer the overlay gates on: how long a validated gameplay pawn
            // with a readable location has stood continuously.
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

            return true;
        }

        void pump(UObject* ctx, RC::Unreal::UFunction* fn)
        {
            // THE MASTER SWITCH, first statement (modswitch.hpp). UE4SS exports no Unregister
            // for the ProcessEvent callback, so a disabled mod bails here: one relaxed load.
            if (!mm::mod_active())
            {
                return;
            }

            // ProcessEvent fires thousands of times a second and every ProcessEvent WE issue
            // fires it again, so the re-entrancy guard comes first. thread_local: the engine
            // calls ProcessEvent from more than one thread.
            static thread_local int depth = 0;
            if (depth != 0)
            {
                return;
            }

            // The THREAD guard: everything below touches game-thread-only state.
            const unsigned long tid = ::GetCurrentThreadId();
            unsigned long owner = g_pump_thread.load(std::memory_order_relaxed);
            if (owner == 0)
            {
                g_pump_thread.store(tid, std::memory_order_relaxed);
                owner = tid;
            }
            if (owner != tid)
            {
                // Once, at verbose: a permanent property of the process, not an event.
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
            refresh_tunables(now);

            // THE UI-EVENT PATH, before the 10 Hz gate: ProcessEvent has just named an
            // object, and if it is a widget the menu it belongs to is worth offering NOW
            // rather than when the discovery walk next reaches it. Raw reads, no
            // ProcessEvent, no decision - see note_ui_event.
            note_ui_event(ctx, fn, now);

            if (now - g_last_position < g_tune.position_ms)
            {
                pump_fast_slice(now, depth);
                return;
            }
            g_last_position = now;
            // The UI-event path's per-pump budget reopens here rather than in pump_widgets:
            // it must keep working while there is no validated pawn to run one.
            g_wevent_added = 0;

            const DepthGuard guard{depth};
            g_pump_calls.fetch_add(1, std::memory_order_relaxed);

            // A transition is in progress: do nothing at all.
            if (now < g_cooldown_until)
            {
                g_state_ok_since = 0;
                publish_hidden(now, true);
                return;
            }

            pump_controller(now);

            const bool pawn_ok = pump_validate_pawn(now);
            if (now < g_cooldown_until)
            {
                g_state_ok_since = 0;
                publish_hidden(now, true);
                return;
            }

            if (!pump_resolve_pawn(now, pawn_ok))
            {
                return;
            }

            pump_widgets(now);
            pump_chapter(now);
            if (!pump_publish(now))
            {
                return;
            }

            // The marker sweep runs LAST, on the same validated state and inside the same
            // re-entrancy guard, after the publish so a slow slice cannot delay the position.
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
        // The marker/map data is dumped from ONE game build; a patch can move actor ids and
        // world coordinates, and the symptom is misplaced markers.
        mm::check_game_build();
        RC::Unreal::Hook::RegisterProcessEventPreCallback(
            [](UObject* ctx, RC::Unreal::UFunction* fn, void*) { pump(ctx, fn); });
        mm::log(L"game-state reader registered on the ProcessEvent game-thread pump "
                L"(position 10 Hz, pawn/controller resolve 2 Hz, menu test EVERY pump on the cached "
                L"in-viewport roots, menu roots also offered by the ProcessEvent context itself, "
                L"backed by a SLICED GUObjectArray walk - 8192 slots per slice, one round per "
                L"0.25-2 s, no FindAllOf; pawn validated through GUObjectArray every pump, class "
                L"gate 'BP_CombatCharacter_Player')");
    }

    void on_update()
    {
        // Loop thread, two cadences. The "is the pump alive?" check keeps its 10-second
        // window whatever the log level; the `state:` line is 10 s at verbose, 60 s at normal.
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
                 // `byte-Visible` is the population the candidate cap applies to, not the roots.
                 g_wcand_round_pub.load(std::memory_order_relaxed),
                 g_wpending_pub.load(std::memory_order_relaxed),
                 g_wcand_dropped_pub.load(std::memory_order_relaxed),
                 snap.transition ? L"TRANSITION | " : L"",
                 g_publishes.load());
    }
} // namespace gamestate
