#include "highlight.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "mem.hpp"
#include "mmstate.hpp"
#include "projection.hpp"
#include "ue_min.hpp"
#include "uereflect.hpp"

using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

namespace hl
{
    namespace
    {
        //==============================================================================
        // Tuning
        //==============================================================================

        constexpr std::uint64_t kResolvePeriodMs = 500; // re-find the camera manager
        constexpr std::uint64_t kCompassPeriodMs = 50;  // 20 Hz is plenty for a heading
        constexpr std::uint64_t kGetterPeriodMs = 33;   // the ProcessEvent fallback route
        // How far into CameraCachePrivate the POV block is looked for. FCameraCacheEntry
        // is a float timestamp plus FMinimalViewInfo, so the answer is single digits -
        // 192 is pure paranoia and still only 48 candidate offsets.
        constexpr int kMaxPovOffset = 192;
        // Consecutive insane reads before the pinned offset is thrown away. One is not
        // enough: a torn read across a game-thread write is possible in principle.
        constexpr int kMaxBadReads = 8;

        //==============================================================================
        // What the game thread reads out of the camera cache
        //==============================================================================
        //
        // FMinimalViewInfo's first fields in UE 5.1: FVector Location (3 doubles, LWC),
        // FRotator Rotation (pitch, yaw, roll - 3 doubles), float FOV. Read as one POD
        // so a single guarded copy covers the lot.

        struct PovRaw
        {
            double loc[3]{};
            double rot[3]{}; // pitch, yaw, roll
            float fov = 0.0f;
        };

        //==============================================================================
        // Published pose (seqlock, exactly like mm::Snapshot)
        //==============================================================================

        std::atomic<std::uint32_t> g_seq{0};
        Pose g_pose{};

        void publish(const Pose& pose)
        {
            const std::uint32_t start = g_seq.load(std::memory_order_relaxed);
            g_seq.store(start + 1, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(&g_pose, &pose, sizeof(Pose));
            std::atomic_thread_fence(std::memory_order_release);
            g_seq.store(start + 2, std::memory_order_release);
        }

        //==============================================================================
        // Cross-thread flags and diagnostics
        //==============================================================================

        std::atomic<bool> g_held{false};
        std::atomic<bool> g_compass{false};
        std::atomic<int> g_route{static_cast<int>(Route::None)};
        std::atomic<int> g_pov_offset_pub{-1};
        std::atomic<int> g_cache_offset_pub{-1};
        std::atomic<std::uint64_t> g_reads{0};
        std::atomic<std::uint64_t> g_fails{0};
        std::atomic<std::uint64_t> g_last_ms{0};
        std::atomic<bool> g_have_manager{false};

        //==============================================================================
        // Game-thread state
        //==============================================================================

        uer::LayoutCache g_layouts;
        uer::FuncCache g_funcs;
        uer::ObjRef g_pcm{};
        const void* g_world = nullptr;
        std::uint64_t g_last_resolve = 0;
        std::uint64_t g_last_read = 0;
        int g_cache_offset = -1; // CameraCachePrivate inside APlayerCameraManager
        int g_cache_size = 0;
        int g_pov_offset = -1; // POV inside CameraCachePrivate
        int g_bad_reads = 0;
        bool g_getter_route = false; // the cache was unusable; call the getters instead
        bool g_logged_route = false;

        void reset_layout_knowledge()
        {
            g_cache_offset = -1;
            g_cache_size = 0;
            g_pov_offset = -1;
            g_bad_reads = 0;
            g_getter_route = false;
            g_logged_route = false;
            g_route.store(static_cast<int>(Route::None), std::memory_order_relaxed);
            g_pov_offset_pub.store(-1, std::memory_order_relaxed);
            g_cache_offset_pub.store(-1, std::memory_order_relaxed);
        }

        //==============================================================================
        // Finding the camera manager
        //==============================================================================
        //
        // Two routes, both from context/wuchang-classes.md 2: the manager is the plain
        // engine `PlayerCameraManager` class here (no game subclass), and it also hangs
        // off the PlayerController as a plain object property.

        void resolve_manager()
        {
            UObject* pcm = UObjectGlobals::FindFirstOf(L"PlayerCameraManager");
            if (pcm != nullptr && !UObjectGlobals::IsValidObjectForFindXOf(pcm))
            {
                pcm = nullptr;
            }
            if (pcm == nullptr)
            {
                UObject* controller = UObjectGlobals::FindFirstOf(L"PlayerController");
                if (controller != nullptr && UObjectGlobals::IsValidObjectForFindXOf(controller))
                {
                    const uer::ClassLayout* layout = g_layouts.get(controller);
                    pcm = uer::read_object_prop(layout, controller, L"PlayerCameraManager");
                }
            }
            if (pcm == nullptr)
            {
                g_pcm.reset();
                g_have_manager.store(false, std::memory_order_relaxed);
                return;
            }
            uer::ObjRef ref{};
            if (!uer::capture(pcm, ref))
            {
                g_pcm.reset();
                g_have_manager.store(false, std::memory_order_relaxed);
                return;
            }
            const bool changed = ref.obj != g_pcm.obj;
            g_pcm = ref;
            g_have_manager.store(true, std::memory_order_relaxed);
            if (changed)
            {
                // A different manager object means a different world; everything learned
                // about the old one's class is still valid, but the pinned offset is
                // re-confirmed from scratch rather than trusted across the change.
                reset_layout_knowledge();
                mm::logf(L"highlight: camera manager acquired ({}, object index {})",
                         ref.obj->GetName(),
                         ref.index);
            }
        }

        //==============================================================================
        // The three blueprint getters (used ONCE for calibration, then never again)
        //==============================================================================

        struct RetFloat
        {
            float v = 0.0f;
        };

        bool getters(UObject* pcm, proj::Camera& out)
        {
            uer::FVec3 loc{};
            uer::FRot3 rot{};
            RetFloat fov{};
            if (!uer::call_getter(g_funcs, pcm, L"GetCameraLocation", loc))
            {
                return false;
            }
            if (!uer::call_getter(g_funcs, pcm, L"GetCameraRotation", rot))
            {
                return false;
            }
            if (!uer::call_getter(g_funcs, pcm, L"GetFOVAngle", fov))
            {
                return false;
            }
            out.x = loc.x;
            out.y = loc.y;
            out.z = loc.z;
            out.pitch = rot.pitch;
            out.yaw = rot.yaw;
            out.roll = rot.roll;
            out.fov_deg = static_cast<double>(fov.v);
            return proj::camera_sane(out);
        }

        proj::Camera from_raw(const PovRaw& raw)
        {
            proj::Camera c{};
            c.x = raw.loc[0];
            c.y = raw.loc[1];
            c.z = raw.loc[2];
            c.pitch = raw.rot[0];
            c.yaw = raw.rot[1];
            c.roll = raw.rot[2];
            c.fov_deg = static_cast<double>(raw.fov);
            return c;
        }

        bool read_raw_at(UObject* pcm, int pov_off, PovRaw& out)
        {
            const std::size_t off = static_cast<std::size_t>(g_cache_offset) + static_cast<std::size_t>(pov_off);
            return mem::read_at(pcm, off, out);
        }

        //==============================================================================
        // Calibration: WHERE inside the cache the POV block sits
        //==============================================================================

        bool near_enough(double a, double b, double tol)
        {
            return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tol;
        }

        // Returns the offset, or -1. `truth` is the getters' answer when it exists.
        int find_pov_offset(UObject* pcm, const proj::Camera* truth)
        {
            const int limit = (std::min)(kMaxPovOffset,
                                         (std::max)(0, g_cache_size - static_cast<int>(sizeof(PovRaw))));
            for (int off = 0; off <= limit; off += 4)
            {
                PovRaw raw{};
                const std::size_t at =
                    static_cast<std::size_t>(g_cache_offset) + static_cast<std::size_t>(off);
                if (!mem::read_at(pcm, at, raw))
                {
                    continue;
                }
                const proj::Camera c = from_raw(raw);
                if (!proj::camera_sane(c))
                {
                    continue;
                }
                if (truth == nullptr)
                {
                    // No getters: sanity alone. Weaker, and the F2 panel says so.
                    return off;
                }
                // The camera moves between the getter calls and this read only by a
                // frame's worth at most, and usually not at all (both happen inside one
                // ProcessEvent callback), so the tolerances can be tight enough that a
                // coincidental match is not credible.
                if (near_enough(c.x, truth->x, 2.0) && near_enough(c.y, truth->y, 2.0) &&
                    near_enough(c.z, truth->z, 2.0) && near_enough(c.pitch, truth->pitch, 0.5) &&
                    near_enough(c.yaw, truth->yaw, 0.5) && near_enough(c.fov_deg, truth->fov_deg, 0.5))
                {
                    return off;
                }
            }
            return -1;
        }

        // Learns the CameraCachePrivate property offset, then the POV offset inside it.
        void calibrate(UObject* pcm)
        {
            if (g_cache_offset < 0)
            {
                const uer::ClassLayout* layout = g_layouts.get(pcm);
                const uer::Prop* p = uer::find_prop(layout, L"CameraCachePrivate");
                if (p == nullptr || p->size < static_cast<int>(sizeof(PovRaw)))
                {
                    g_getter_route = true;
                    return;
                }
                g_cache_offset = static_cast<int>(p->offset);
                g_cache_size = p->size;
                g_cache_offset_pub.store(g_cache_offset, std::memory_order_relaxed);
            }

            proj::Camera truth{};
            const bool have_truth = getters(pcm, truth);
            const int off = find_pov_offset(pcm, have_truth ? &truth : nullptr);
            if (off < 0)
            {
                // The cache is there but nothing in it looks like a view. Fall back to
                // the getters, which at least produced something above.
                g_getter_route = have_truth;
                if (!g_logged_route)
                {
                    g_logged_route = true;
                    mm::logf(L"highlight: CameraCachePrivate is at +{} ({} bytes) but no POV block in it "
                             L"matched {} - {}",
                             g_cache_offset,
                             g_cache_size,
                             have_truth ? L"the camera getters" : L"a sane camera",
                             have_truth ? L"falling back to GetCameraLocation/Rotation/FOVAngle per read"
                                        : L"the highlight has no camera");
                }
                return;
            }
            g_pov_offset = off;
            g_bad_reads = 0;
            g_pov_offset_pub.store(off, std::memory_order_relaxed);
            g_route.store(static_cast<int>(have_truth ? Route::RawPinned : Route::RawSane),
                          std::memory_order_relaxed);
            mm::logf(L"highlight: camera POV pinned at CameraCachePrivate+{} (property +{}, {} bytes), "
                     L"validated against {}",
                     off,
                     g_cache_offset,
                     g_cache_size,
                     have_truth ? L"GetCameraLocation / GetCameraRotation / GetFOVAngle"
                                : L"sanity ranges only (the getters were unavailable)");
        }

        //==============================================================================
        // One camera read
        //==============================================================================

        void read_camera(std::uint64_t now)
        {
            UObject* pcm = g_pcm.obj;
            if (g_pov_offset < 0 && !g_getter_route)
            {
                calibrate(pcm);
            }

            proj::Camera cam{};
            bool ok = false;
            if (g_pov_offset >= 0)
            {
                PovRaw raw{};
                if (read_raw_at(pcm, g_pov_offset, raw))
                {
                    cam = from_raw(raw);
                    ok = proj::camera_sane(cam);
                }
                if (!ok)
                {
                    // Never latch a pinned offset that has stopped working: after a few
                    // consecutive rejects the discovery runs again (a patch could move
                    // the field, and a re-possession could hand us a different manager).
                    if (++g_bad_reads >= kMaxBadReads)
                    {
                        mm::logf(L"highlight: the pinned camera offset (+{}) produced {} insane reads in a "
                                 L"row - dropping it and re-calibrating",
                                 g_pov_offset,
                                 g_bad_reads);
                        g_pov_offset = -1;
                        g_pov_offset_pub.store(-1, std::memory_order_relaxed);
                        g_bad_reads = 0;
                        g_route.store(static_cast<int>(Route::None), std::memory_order_relaxed);
                    }
                }
                else
                {
                    g_bad_reads = 0;
                }
            }
            else if (g_getter_route)
            {
                ok = getters(pcm, cam);
                if (ok)
                {
                    g_route.store(static_cast<int>(Route::Getters), std::memory_order_relaxed);
                }
            }

            if (!ok)
            {
                g_fails.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            Pose pose{};
            pose.x = cam.x;
            pose.y = cam.y;
            pose.z = cam.z;
            pose.pitch = cam.pitch;
            pose.yaw = cam.yaw;
            pose.roll = cam.roll;
            pose.fov = cam.fov_deg;
            pose.stamp_ms = now;
            pose.valid = true;
            publish(pose);
            g_reads.fetch_add(1, std::memory_order_relaxed);
            g_last_ms.store(now, std::memory_order_relaxed);
        }
    } // namespace

    //==================================================================================
    // Public API
    //==================================================================================

    bool camera(Pose& out)
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const std::uint32_t before = g_seq.load(std::memory_order_acquire);
            if ((before & 1u) != 0u)
            {
                YieldProcessor();
                continue;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(&out, &g_pose, sizeof(Pose));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_seq.load(std::memory_order_acquire) == before)
            {
                return before != 0 && out.valid;
            }
        }
        return false;
    }

    Stats stats()
    {
        Stats s{};
        s.route = static_cast<Route>(g_route.load(std::memory_order_relaxed));
        s.pov_offset = g_pov_offset_pub.load(std::memory_order_relaxed);
        s.cache_offset = g_cache_offset_pub.load(std::memory_order_relaxed);
        s.reads = g_reads.load(std::memory_order_relaxed);
        s.fails = g_fails.load(std::memory_order_relaxed);
        s.last_ms = g_last_ms.load(std::memory_order_relaxed);
        s.have_manager = g_have_manager.load(std::memory_order_relaxed);
        s.demanded = g_held.load(std::memory_order_relaxed) || g_compass.load(std::memory_order_relaxed);
        return s;
    }

    void set_demand(bool held_now, bool compass)
    {
        g_held.store(held_now, std::memory_order_relaxed);
        g_compass.store(compass, std::memory_order_relaxed);
    }

    bool held()
    {
        return g_held.load(std::memory_order_relaxed);
    }

    void drop_caches()
    {
        g_pcm.reset();
        g_world = nullptr;
        g_funcs.clear();
        g_layouts.clear();
        g_last_resolve = 0;
        g_last_read = 0;
        reset_layout_knowledge();
        g_have_manager.store(false, std::memory_order_relaxed);
    }

    void game_thread_pump(std::uint64_t now, const void* world, const mm::Config& cfg)
    {
        const bool want_held = g_held.load(std::memory_order_relaxed) && cfg.highlight_enabled;
        const bool want_compass = g_compass.load(std::memory_order_relaxed) && cfg.compass_enabled;
        if (!want_held && !want_compass)
        {
            // Nothing on screen needs a camera: the reader costs one atomic load per
            // pump and touches no UObject at all.
            return;
        }

        if (world != nullptr && g_world != nullptr && world != g_world)
        {
            drop_caches();
        }
        g_world = world;

        if (!uer::alive(g_pcm))
        {
            g_pcm.reset();
            g_have_manager.store(false, std::memory_order_relaxed);
            if (now - g_last_resolve < kResolvePeriodMs)
            {
                return;
            }
            g_last_resolve = now;
            resolve_manager();
            if (!uer::alive(g_pcm))
            {
                return;
            }
        }

        // The rate: the full configured rate while the key is held (the labels have to
        // stay glued to the item while the camera swings), 20 Hz for the compass alone,
        // and never faster than kGetterPeriodMs on the ProcessEvent fallback route.
        std::uint64_t period = kCompassPeriodMs;
        if (want_held)
        {
            const int hz = (std::max)(5, (std::min)(240, cfg.highlight_camera_hz));
            period = static_cast<std::uint64_t>(1000 / hz);
        }
        if (g_getter_route && period < kGetterPeriodMs)
        {
            period = kGetterPeriodMs;
        }
        if (now - g_last_read < period)
        {
            return;
        }
        g_last_read = now;

        read_camera(now);
    }
} // namespace hl
