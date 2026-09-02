#include "gamestate.hpp"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

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
        constexpr std::uint64_t kWidgetPeriodMs = 500;   // 2 Hz: the menu test
        constexpr std::size_t kMaxWidgets = 6000;        // sanity cap on one FindAllOf pass

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
        std::uint32_t g_widgets_seen = 0;
        std::uint32_t g_widgets_visible = 0;

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
            g_funcs.clear();
            g_layouts.clear();
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

        void update_widgets()
        {
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
                const uer::ClassLayout* layout = g_layouts.get(w);
                std::uint8_t vis = 0xFF;
                if (uer::read_prop(layout, w, L"Visibility", vis, 1))
                {
                    if (vis != 0)
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
                    menu = true;
                }
            }

            g_widgets_seen = seen;
            g_widgets_visible = visible_in_viewport;
            g_menu_open = menu;
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
            if (now - g_last_widgets >= kWidgetPeriodMs)
            {
                g_last_widgets = now;
                update_widgets();
            }

            mm::Snapshot snap{};
            snap.stamp_ms = now;
            snap.menu_open = g_menu_open;
            snap.widgets_seen = g_widgets_seen;
            snap.widgets_visible_in_viewport = g_widgets_visible;
            snap.pawn_is_gameplay = true;

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
                L"(position 10 Hz, pawn/controller resolve 2 Hz, widgets 2 Hz; pawn validated "
                L"through GUObjectArray every pump, class gate 'BP_CombatCharacter_Player')");
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
        mm::logf(L"state: pawn {} pos {:.0f} {:.0f} {:.0f} yaw {:.0f} ({}) | pawn-view {} | menu {} | "
                 L"widgets {}/{} | {}{} publishes",
                 snap.has_pawn ? L"yes" : L"NO",
                 snap.x,
                 snap.y,
                 snap.z,
                 static_cast<double>(snap.yaw),
                 snap.loc_from_function ? L"K2_GetActorLocation" : L"RootComponent",
                 snap.is_pawn_view ? L"yes" : L"no",
                 snap.menu_open ? L"OPEN" : L"no",
                 snap.widgets_visible_in_viewport,
                 snap.widgets_seen,
                 snap.transition ? L"TRANSITION | " : L"",
                 g_publishes.load());
    }
} // namespace gamestate
