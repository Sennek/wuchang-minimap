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

        // The player's own pawn class, from the step-1 recon
        // (context/wuchang-classes.md). The controller is the fallback route.
        constexpr const wchar_t* kPlayerPawnClass = L"BP_CombatCharacter_Player_Final_C";
        constexpr const wchar_t* kControllerClass = L"DCSPlayerController_C";

        //==============================================================================
        // State (game thread only, except the atomics)
        //==============================================================================

        std::atomic<bool> g_registered{false};
        std::atomic<std::uint64_t> g_pump_calls{0};
        std::atomic<std::uint64_t> g_publishes{0};
        std::atomic<bool> g_report_pending{false};
        std::atomic<bool> g_pawn_found_logged{false};

        uer::LayoutCache g_layouts;
        uer::FuncCache g_funcs;

        UObject* g_pawn = nullptr;
        RC::Unreal::UClass* g_pawn_class = nullptr;
        UObject* g_controller = nullptr;
        RC::Unreal::UClass* g_controller_class = nullptr;

        std::uint64_t g_last_position = 0;
        std::uint64_t g_last_resolve = 0;
        std::uint64_t g_last_widgets = 0;

        bool g_menu_open = false;
        std::uint32_t g_widgets_seen = 0;
        std::uint32_t g_widgets_visible = 0;

        std::wstring g_pawn_full_name;
        std::wstring g_pawn_short_name;

        // A cached UObject* can outlive its object across a level transition. UE never
        // decommits the object pool, so a stale read cannot fault - but it can return
        // garbage, so the pointer is only trusted while its class pointer still matches
        // what it had when we found it. Combined with the 2 Hz re-resolve that keeps the
        // stale window to half a second.
        bool still_valid(UObject* obj, RC::Unreal::UClass* expected)
        {
            if (obj == nullptr || expected == nullptr)
            {
                return false;
            }
            if (!mem::readable(obj, 0x40))
            {
                return false;
            }
            return obj->GetClassPrivate() == expected;
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
        // Resolving the pawn and the controller
        //==============================================================================

        void resolve()
        {
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(kPlayerPawnClass, found);
            UObject* pawn = found.empty() ? nullptr : found.front();

            found.clear();
            UObjectGlobals::FindAllOf(kControllerClass, found);
            UObject* controller = found.empty() ? nullptr : found.front();
            if (controller == nullptr)
            {
                found.clear();
                UObjectGlobals::FindAllOf(L"PlayerController", found);
                controller = found.empty() ? nullptr : found.front();
            }

            // Fallback route for the pawn: AController::Pawn. This is also what covers a
            // build where the player class is renamed.
            if (pawn == nullptr && controller != nullptr)
            {
                const uer::ClassLayout* layout = g_layouts.get(controller);
                pawn = uer::read_object_prop(layout, controller, L"Pawn");
                if (pawn == nullptr)
                {
                    pawn = uer::read_object_prop(layout, controller, L"AcknowledgedPawn");
                }
            }

            g_controller = controller;
            g_controller_class = controller != nullptr ? controller->GetClassPrivate() : nullptr;

            if (pawn != g_pawn)
            {
                g_pawn = pawn;
                g_pawn_class = pawn != nullptr ? pawn->GetClassPrivate() : nullptr;
                g_pawn_full_name.clear();
                g_pawn_short_name.clear();
                if (pawn != nullptr)
                {
                    g_pawn_short_name = pawn->GetName();
                    g_pawn_full_name = pawn->GetFullName();
                    if (!g_pawn_found_logged.exchange(true))
                    {
                        mm::logf(L"player pawn found: {}", g_pawn_full_name);
                    }
                }
            }
        }

        //==============================================================================
        // Location / rotation
        //==============================================================================

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

        UObject* read_view_target(UObject* controller)
        {
            if (controller == nullptr)
            {
                return nullptr;
            }
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
                if (w == nullptr)
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

            ++depth;
            g_pump_calls.fetch_add(1, std::memory_order_relaxed);

            if (now - g_last_resolve >= kResolvePeriodMs || !still_valid(g_pawn, g_pawn_class) ||
                !still_valid(g_controller, g_controller_class))
            {
                g_last_resolve = now;
                resolve();
            }

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

            UObject* pawn = still_valid(g_pawn, g_pawn_class) ? g_pawn : nullptr;
            if (pawn != nullptr)
            {
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

                UObject* view = read_view_target(still_valid(g_controller, g_controller_class) ? g_controller : nullptr);
                snap.is_pawn_view = (view != nullptr && view == pawn);
            }

            mm::publish(snap);
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            g_report_pending.store(true, std::memory_order_relaxed);
            --depth;
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
                L"(position 10 Hz, pawn/controller resolve 2 Hz, widgets 2 Hz)");
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
        mm::logf(L"state: pawn {} pos {:.0f} {:.0f} {:.0f} yaw {:.0f} | pawn-view {} | menu {} | widgets {}/{} "
                 L"| {} publishes",
                 snap.has_pawn ? L"yes" : L"NO",
                 snap.x,
                 snap.y,
                 snap.z,
                 static_cast<double>(snap.yaw),
                 snap.is_pawn_view ? L"yes" : L"no",
                 snap.menu_open ? L"OPEN" : L"no",
                 snap.widgets_visible_in_viewport,
                 snap.widgets_seen,
                 g_publishes.load());
    }
} // namespace gamestate
