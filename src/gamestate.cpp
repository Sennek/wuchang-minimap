#include "gamestate.hpp"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "markers.hpp"
#include "mmstate.hpp"
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

        constexpr std::uint64_t kPositionPeriodMs = 100; // 10 Hz: pawn location + yaw
        constexpr std::uint64_t kResolvePeriodMs = 500;  // 2 Hz: FindAllOf for pawn / controller
        // The menu test runs at two rates, because "the minimap hides 2-3 s after I
        // open the inventory" was traced straight to it running at 2 Hz:
        //   * EVERY pump (10 Hz): re-read the reflected Visibility byte of the handful
        //     of root widgets we have already seen in the viewport. That is a few
        //     GUObjectArray checks and a few byte reads - no FindAllOf, no allocation,
        //     no ProcessEvent - and it is what makes hiding immediate.
        //   * every kWidgetFullPeriodMs: the full FindAllOf("UserWidget") sweep, which
        //     is the only thing that can DISCOVER a root the first time a given menu is
        //     opened in a session (a widget that has never been Visible has never been
        //     IsInViewport()-tested, so it cannot be in the cache yet).
        constexpr std::uint64_t kWidgetFullPeriodMs = 250;
        constexpr std::size_t kMaxWidgets = 6000;  // sanity cap on one FindAllOf pass
        constexpr std::size_t kMaxMenuRoots = 32;  // cache cap; the game only ever has 5-6

        // After ANY pawn / world change, no UFunction is called for this long. A level
        // transition destroys the old pawn, its world and everything cached off them;
        // the engine is mid-LoadMap and calling into a blueprint getter then is exactly
        // what produced the EXCEPTION_ACCESS_VIOLATION in read_location().
        constexpr std::uint64_t kTransitionCooldownMs = 2000;

        constexpr std::uint64_t kLogThrottleMs = 5000;

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
        std::atomic<std::uint64_t> g_publishes{0};
        std::atomic<bool> g_report_pending{false};

        uer::LayoutCache g_layouts;
        uer::FuncCache g_funcs;

        uer::ObjRef g_pawn{};
        uer::ObjRef g_controller{};
        bool g_pawn_is_gameplay = false;
        const void* g_world = nullptr; // the pawn's UWorld*, as a transition token

        std::uint64_t g_last_position = 0;
        std::uint64_t g_last_resolve = 0;
        std::uint64_t g_last_widgets = 0;
        std::uint64_t g_cooldown_until = 0;
        std::uint64_t g_state_ok_since = 0;

        std::uint64_t g_log_no_pawn = 0;
        std::uint64_t g_log_rejected = 0;
        std::wstring g_rejected_class;

        bool g_menu_open = true; // safe default: "a menu is up" hides the minimap
        std::uint64_t g_menu_change_ms = 0;
        std::uint32_t g_widgets_seen = 0;
        std::uint32_t g_widgets_visible = 0;

        // Root widgets that were once confirmed `IsInViewport()`. Re-tested every pump:
        // the Visibility byte as a cheap prefilter AND `IsInViewport()` as the actual
        // answer - see the comment on menu_from_cached_roots() for why the byte alone
        // latched the minimap off forever.
        std::vector<uer::ObjRef> g_menu_roots;

        // The root that is currently holding "a menu is open" true, for the F2 debug
        // block and the log. Empty when no root is visible.
        std::wstring g_menu_holder;

        // Set whenever the menu state flips, a root leaves the viewport or the pawn
        // teleports: the next pump runs the full FindAllOf sweep instead of waiting up
        // to kWidgetFullPeriodMs, so the cached-root list is re-validated and rebuilt
        // from live state.
        bool g_force_widget_sweep = true;

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

        std::wstring g_pawn_class_name;
        std::wstring g_pawn_full_name;
        std::wstring g_pawn_short_name;

        bool throttled(std::uint64_t& last, std::uint64_t now)
        {
            if (now - last < kLogThrottleMs)
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
            g_world = nullptr;
            g_pawn_class_name.clear();
            g_pawn_full_name.clear();
            g_pawn_short_name.clear();
            g_state_ok_since = 0;
            g_menu_open = true;
            g_menu_change_ms = now;
            g_menu_roots.clear(); // the widgets belonged to the world that just went
            g_menu_holder.clear();
            g_force_widget_sweep = true;
            g_have_last_pos = false;
            g_funcs.clear();
            g_layouts.clear();
            // The marker sweep caches class layouts, class classifications, per-object
            // ids and live actors - all of it keyed to the world that just went.
            markers::drop_caches();
            if (had_pawn)
            {
                g_cooldown_until = now + kTransitionCooldownMs;
                mm::logf(L"pawn dropped ({}): caches cleared, no UFunction calls for {} ms",
                         why,
                         kTransitionCooldownMs);
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
            UObject* pawn = uer::read_object_prop(layout, g_controller.obj, L"Pawn");
            if (pawn == nullptr)
            {
                pawn = uer::read_object_prop(layout, g_controller.obj, L"AcknowledgedPawn");
            }
            return pawn;
        }

        void resolve_controller()
        {
            UObject* controller = UObjectGlobals::FindFirstOf(kControllerClass);
            if (controller == nullptr)
            {
                controller = UObjectGlobals::FindFirstOf(L"PlayerController");
            }
            if (controller == nullptr || !UObjectGlobals::IsValidObjectForFindXOf(controller))
            {
                g_controller.reset();
                return;
            }
            uer::ObjRef ref{};
            if (uer::capture(controller, ref))
            {
                g_controller = ref;
            }
            else
            {
                g_controller.reset();
            }
        }

        void resolve_pawn(std::uint64_t now)
        {
            UObject* candidate = nullptr;

            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(kPlayerPawnClass, found);
            for (UObject* obj : found)
            {
                if (obj != nullptr && UObjectGlobals::IsValidObjectForFindXOf(obj))
                {
                    candidate = obj;
                    break;
                }
            }

            // Fallback route: AController::Pawn. It also covers a build where the
            // player class is renamed - but it is exactly the route that hands out the
            // Lobby DefaultPawn, so the class gate below is mandatory.
            if (candidate == nullptr)
            {
                candidate = pawn_from_controller();
            }

            if (candidate == nullptr)
            {
                if (throttled(g_log_no_pawn, now))
                {
                    mm::log(L"no player pawn in the world (main menu / loading) - reader idling");
                }
                return;
            }

            uer::ObjRef ref{};
            if (!uer::capture(candidate, ref))
            {
                return;
            }

            const std::wstring cls = uer::class_name(ref);
            if (cls.find(kGameplayPawnSubstr) == std::wstring::npos)
            {
                if (cls != g_rejected_class || throttled(g_log_rejected, now))
                {
                    g_rejected_class = cls;
                    g_log_rejected = now;
                    mm::logf(L"pawn candidate class '{}' is not a gameplay pawn "
                             L"(needs '{}') - reader idling, minimap stays hidden",
                             cls.empty() ? std::wstring{L"<unknown>"} : cls,
                             kGameplayPawnSubstr);
                }
                return;
            }

            g_pawn = ref;
            g_pawn_is_gameplay = true;
            g_pawn_class_name = cls;
            g_pawn_short_name = ref.obj->GetName();
            g_pawn_full_name = ref.obj->GetFullName();
            g_world = uer::world_of(ref);
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
                g_force_widget_sweep = true;
                mm::logf(L"menu state -> {} ({}); a full widget sweep is queued",
                         open ? L"OPEN" : L"closed",
                         why);
            }
        }

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

        void remember_menu_root(UObject* w)
        {
            for (const uer::ObjRef& ref : g_menu_roots)
            {
                if (ref.obj == w)
                {
                    return;
                }
            }
            if (g_menu_roots.size() >= kMaxMenuRoots)
            {
                return;
            }
            uer::ObjRef ref{};
            if (uer::capture(w, ref))
            {
                g_menu_roots.push_back(ref);
                mm::logf(L"menu root seen in the viewport: {} (now {} cached root(s); the "
                         L"menu test re-confirms these at 10 Hz)",
                         w->GetName(),
                         g_menu_roots.size());
            }
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
        bool menu_from_cached_roots(std::uint32_t& visible_count, std::wstring& holder)
        {
            bool menu = false;
            visible_count = 0;
            for (std::size_t i = 0; i < g_menu_roots.size();)
            {
                UObject* w = g_menu_roots[i].obj;
                if (!uer::alive(g_menu_roots[i]))
                {
                    g_menu_roots.erase(g_menu_roots.begin() + static_cast<std::ptrdiff_t>(i));
                    g_force_widget_sweep = true;
                    mm::logf(L"menu root dropped (the widget object died); {} cached root(s) left",
                             g_menu_roots.size());
                    continue;
                }
                bool has_byte = false;
                if (!widget_is_visible_byte(w, has_byte))
                {
                    ++i; // in the viewport, just not ESlateVisibility::Visible
                    continue;
                }
                if (!widget_in_viewport(w))
                {
                    // THE FIX. Visible but no longer in the viewport = the menu closed
                    // and the game never touched the widget's Visibility byte.
                    const std::wstring name = w->GetName();
                    g_menu_roots.erase(g_menu_roots.begin() + static_cast<std::ptrdiff_t>(i));
                    g_force_widget_sweep = true;
                    mm::logf(L"menu root dropped ({} is still Visible but no longer in the "
                             L"viewport - that menu closed); {} cached root(s) left",
                             name,
                             g_menu_roots.size());
                    continue;
                }
                ++visible_count;
                if (!menu)
                {
                    holder = w->GetName();
                }
                menu = true;
                ++i;
            }
            return menu;
        }

        // THE FULL SWEEP. Also returns "a menu is up", and REBUILDS the root cache from
        // scratch - a root that does not confirm in this pass is gone, so the cache can
        // never outlive the state it describes.
        bool update_widgets(std::wstring& holder)
        {
            std::vector<uer::ObjRef> confirmed;
            std::vector<UObject*> widgets;
            UObjectGlobals::FindAllOf(L"UserWidget", widgets);

            std::uint32_t seen = 0;
            std::uint32_t visible_in_viewport = 0;
            bool menu = false;

            const std::size_t count = widgets.size() < kMaxWidgets ? widgets.size() : kMaxWidgets;
            for (std::size_t i = 0; i < count; ++i)
            {
                UObject* w = widgets[i];
                if (w == nullptr || !UObjectGlobals::IsValidObjectForFindXOf(w))
                {
                    continue;
                }
                ++seen;
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
                    // Cache it: from now on this root is re-tested every pump, so the
                    // NEXT time this menu opens the minimap hides within ~100 ms.
                    remember_menu_root(w);
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
                mm::logf(L"menu root cache rebuilt by the sweep: {} -> {} root(s)",
                         g_menu_roots.size(),
                         confirmed.size());
            }
            g_menu_roots = std::move(confirmed);

            g_widgets_seen = seen;
            g_widgets_visible = visible_in_viewport;
            return menu;
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
            mm::publish(snap);
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            g_report_pending.store(true, std::memory_order_relaxed);
        }

        void pump()
        {
            // ProcessEvent fires thousands of times a second, and every ProcessEvent WE
            // issue fires it again - so the re-entrancy guard comes first, before any
            // work at all. thread_local, because the guard has to be per-thread: the
            // engine calls ProcessEvent from more than one thread.
            static thread_local int depth = 0;
            if (depth != 0)
            {
                return;
            }

            const std::uint64_t now = ::GetTickCount64();
            if (now - g_last_position < kPositionPeriodMs)
            {
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
            if (!uer::alive(g_controller))
            {
                g_controller.reset();
                if (now - g_last_resolve >= kResolvePeriodMs)
                {
                    g_last_resolve = now;
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
            if (!pawn_ok && now - g_last_resolve >= kResolvePeriodMs)
            {
                g_last_resolve = now;
                if (!uer::alive(g_controller))
                {
                    resolve_controller();
                }
                resolve_pawn(now);
                pawn_ok = g_pawn_is_gameplay && uer::alive(g_pawn);
            }

            if (!pawn_ok)
            {
                g_state_ok_since = 0;
                publish_hidden(now, false);
                return;
            }

            // ---- 4. from here on the pawn is validated: UFunctions are allowed -------
            //
            // The menu test: the cheap cached-root pass runs on EVERY pump, and the
            // full sweep only every kWidgetFullPeriodMs. Either one saying "a menu is
            // up" is enough, and the answer is published in this same snapshot - so
            // the overlay hides on the next frame, not seconds later.
            std::uint32_t roots_visible = 0;
            std::wstring holder;
            bool menu = menu_from_cached_roots(roots_visible, holder);
            const bool swept = g_force_widget_sweep || (now - g_last_widgets >= kWidgetFullPeriodMs);
            if (swept)
            {
                g_last_widgets = now;
                g_force_widget_sweep = false;
                std::wstring sweep_holder;
                const bool sweep_menu = update_widgets(sweep_holder);
                // After a sweep the sweep IS the answer. OR-ing the cached pass's older
                // answer over it - which is what the first version did - lets a root the
                // sweep has just dropped win, and that is the latch that hid the minimap
                // for the rest of run 2. Never OR a cached answer over a fresh sweep.
                menu = sweep_menu;
                roots_visible = g_widgets_visible;
                holder = sweep_holder;
            }
            else
            {
                g_widgets_visible = roots_visible;
            }
            g_menu_holder = holder;
            set_menu_open(menu,
                          now,
                          menu ? (swept ? L"the full sweep found an in-viewport Visible root"
                                        : L"a cached root is in the viewport and Visible")
                               : (swept ? L"the full sweep found no in-viewport Visible root"
                                        : L"no cached root is in the viewport and Visible"));

            mm::Snapshot snap{};
            snap.stamp_ms = now;
            snap.menu_open = g_menu_open;
            snap.widgets_seen = g_widgets_seen;
            snap.widgets_visible_in_viewport = g_widgets_visible;
            snap.menu_roots_cached = static_cast<std::uint32_t>(g_menu_roots.size());
            snap.menu_change_ms = g_menu_change_ms;
            snap.pawn_is_gameplay = true;
            copy_to(snap.menu_holder, std::size(snap.menu_holder), g_menu_holder);

            UObject* pawn = g_pawn.obj;
            bool via_function = false;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            float yaw = 0.0f;
            if (read_location(pawn, x, y, z, yaw, via_function))
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
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kTeleportJumpUu)
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
            // same re-entrancy guard: one FindAllOf per pump, cycling through the
            // marker class table. Putting it after the publish means a slow sweep can
            // never delay the position the overlay draws with.
            markers::game_thread_pump(now, g_world);
        }
    } // namespace

    void on_unreal_init()
    {
        if (g_registered.exchange(true))
        {
            return;
        }
        RC::Unreal::Hook::RegisterProcessEventPreCallback(
            [](UObject*, RC::Unreal::UFunction*, void*) { pump(); });
        mm::log(L"game-state reader registered on the ProcessEvent game-thread pump "
                L"(position 10 Hz, pawn/controller resolve 2 Hz, menu test EVERY pump on the cached "
                L"in-viewport roots + a full widget sweep at 4 Hz; pawn validated through "
                L"GUObjectArray every pump, class gate 'BP_CombatCharacter_Player')");
    }

    void on_update()
    {
        // Loop thread. One summary line every 10 s so the log shows the reader is alive
        // without drowning it.
        static std::uint64_t last_report = 0;
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
        mm::Snapshot snap{};
        if (!mm::read_snapshot(snap))
        {
            return;
        }
        mm::logf(L"state: pawn {} pos {:.0f} {:.0f} {:.0f} yaw {:.0f} ({}) | pawn-view {} | menu {} "
                 L"(last change {} ms ago, {} cached root(s)) | widgets {}/{} | {}{} publishes",
                 snap.has_pawn ? L"yes" : L"NO",
                 snap.x,
                 snap.y,
                 snap.z,
                 static_cast<double>(snap.yaw),
                 snap.loc_from_function ? L"K2_GetActorLocation" : L"RootComponent",
                 snap.is_pawn_view ? L"yes" : L"no",
                 snap.menu_open ? L"OPEN" : L"no",
                 snap.menu_change_ms == 0 ? 0ull : ::GetTickCount64() - snap.menu_change_ms,
                 snap.menu_roots_cached,
                 snap.widgets_visible_in_viewport,
                 snap.widgets_seen,
                 snap.transition ? L"TRANSITION | " : L"",
                 g_publishes.load());
    }
} // namespace gamestate
