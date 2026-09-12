//==============================================================================
// overlay_dcomp.cpp - the overlay's own DirectComposition surface
//==============================================================================
//
// Everything between the overlay's finished D3D12 frame and the desktop compositor: a
// DIRECT queue the overlay submits its frames on, a D3D11On12 device over the game's
// ID3D12Device with a second DIRECT queue of its own, a DirectComposition desktop
// device with a target and a visual on the game's window, the surface the visual
// shows, the D3D11 wrappers of the mod's own render targets, and the SURFACE THREAD
// that copies a finished target into the surface.
//
// Why a surface and not a swapchain. A process carrying NVIDIA Streamline supports
// exactly one IDXGISwapChain; presenting a second one kills the game. A
// DirectComposition surface reaches the compositor with no swapchain and no Present at
// all. Every resource here is one this module created, so the question of whether the
// mod may write a buffer some frame-generation layer owns never arises.
//
// It creates no D3D12 device: the targets, the wrappers and both queues live on the
// game's own, which keeps ImGui, the map textures and the height slicer where they are.
//
// THREADS. `comp_create`, `comp_bind_targets`, `comp_unbind_targets`, `comp_release`,
// `comp_pick_target` and `comp_publish` belong to whoever holds `g_render_lock`. The
// SURFACE THREAD runs `surface_proc` and nothing else: it waits on the render fence,
// copies the published target into the surface and commits. It touches no UObject and
// takes no lock. The only object of the render path's it dereferences is `g_fence` -
// `GetCompletedValue` and `SetEventOnCompletion` on it, plus a `Signal` on its own copy
// fence - and it is stopped and joined before `g_fence` is released, or, if it will not
// stop, marked WEDGED, in which case `g_fence` and everything else it reads is leaked
// rather than freed under it.
//
// The three event handles - wake, ack and the fence event - are created once and never
// closed while the module is loaded. `comp_publish` and the quiesce signal them from the
// render thread while `comp_stop_thread` may run on the loop thread, and a closed handle
// value is recycled by the kernel: signalling a recycled handle would hit an unrelated
// object. Three handles for the life of the process is the cheaper answer.
//
// `comp_stop_thread` is the one entry point any thread may call, because stopping a
// thread of the mod's own is not a D3D12 release. Two threads do call it - the render
// thread inside `comp_release` and the loop thread on module unload - so the thread's
// whole lifecycle is one state word: the caller that wins the transition to `Stopping`
// joins, and the other waits for its answer. `comp_thread_stopped()` is the single
// licence to free anything the thread reads, and only a completed join grants it.

#include "overlay_internal.hpp"

#include <d3d11.h>
#include <d3d11on12.h>
#include <dcomp.h>

namespace overlay
{
    namespace ovl
    {
        namespace
        {
            // dcomp.dll and d3d11.dll are looked up, not imported: a static import would
            // make the whole mod fail to load if either were ever absent - a silent
            // load-time failure this project has paid for once.
            using DCompositionCreateDevice2Fn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
            using D3D11On12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*,
                                                             UINT, IUnknown* const*, UINT, UINT,
                                                             ID3D11Device**, ID3D11DeviceContext**,
                                                             D3D_FEATURE_LEVEL*);

            HMODULE g_dcomp_dll = nullptr;
            HMODULE g_d3d11_dll = nullptr;

            IDCompositionDesktopDevice* g_comp_device = nullptr;
            IDCompositionTarget* g_comp_target = nullptr;
            IDCompositionVisual2* g_comp_visual = nullptr;
            IDCompositionSurfaceFactory* g_surface_factory = nullptr;
            IDCompositionSurface* g_surface = nullptr;

            // The queue the overlay's own command lists are submitted on.
            ID3D12CommandQueue* g_comp_queue = nullptr;
            // D3D11On12's queue. A second one, so the surface thread's `Flush` never
            // races the render thread's `ExecuteCommandLists` for submission order on
            // one queue; the two are ordered by a CPU fence wait instead.
            ID3D12CommandQueue* g_queue11 = nullptr;
            ID3D11Device* g_dev11 = nullptr;
            ID3D11DeviceContext* g_ctx11 = nullptr;
            ID3D11On12Device* g_on12 = nullptr;
            ID3D11Resource* g_wrapped[kTargets]{};

            UINT g_surface_w = 0;
            UINT g_surface_h = 0;
            bool g_comp_logged = false;
            bool g_draw_logged = false;  // surface thread only
            bool g_wait_logged = false;  // surface thread only
            bool g_leak_logged = false;

            // The copy fence, signalled on `g_queue11` once the copy that read a target
            // has been submitted. A target is free again when its recorded value has
            // completed - asked with GetCompletedValue, never waited on by the frame.
            ID3D12Fence* g_copy_fence = nullptr;
            std::uint64_t g_copy_value = 0; // surface thread only
            std::atomic<std::uint64_t> g_target_copy[kTargets]{};
            // True from the moment the render thread publishes a target until the copy
            // that reads it has been submitted, or until a newer frame displaces it.
            std::atomic<bool> g_target_busy[kTargets]{};
            std::atomic<std::uint64_t> g_skipped{0};
            std::atomic<std::uint64_t> g_dropped{0};

            // The mailbox: latest wins, one slot, no lock. 0 is empty; otherwise the
            // render fence value shifted up with the target index in the low bits.
            constexpr int kMailboxIndexBits = 3;
            constexpr std::uint64_t kMailboxIndexMask = (1ull << kMailboxIndexBits) - 1ull;
            std::atomic<std::uint64_t> g_mailbox{0};

            // The surface thread's whole lifecycle, in one word, because two threads
            // may ask to stop it - the render thread inside `comp_release` and the loop
            // thread on the master switch's timeout or on module unload. `Stopping` is
            // the state that makes that safe: the caller that wins the transition joins,
            // and the other waits for the answer instead of racing past it into a
            // release. Only `None` licences freeing anything the thread reads.
            enum class SurfState
            {
                None,     // never created, or created, stopped and joined
                Running,  // the thread is in its wait loop
                Stopping, // one caller is inside the join right now
                Wedged,   // it would not exit within the join budget; permanent
            };
            std::atomic<SurfState> g_surf_state{SurfState::None};
            // Owned by whoever holds the state at `Running` or `Stopping`, so a plain
            // handle: the state transition is what makes exactly one thread close it.
            HANDLE g_surf_thread = nullptr;
            // Created once, never closed - see the prose block.
            HANDLE g_surf_wake = nullptr;  // auto-reset: a frame was published, or the state changed
            HANDLE g_surf_ack = nullptr;   // auto-reset: the surface thread has just parked
            HANDLE g_surf_fence_event = nullptr;
            std::atomic<bool> g_surf_stop{false};
            // The REQUEST to stand still, and the thread's own answer that it has. Only
            // the answer is ever believed: a quiesce that timed out leaves the request
            // set with the thread still running, and trusting the request would then let
            // the next one free the surface under a live BeginDraw.
            std::atomic<bool> g_surf_pause_req{false};
            std::atomic<bool> g_surf_parked{false};

            // The format the visual is composed in. BGRA because every composition
            // surface takes it, premultiplied because ImGui's DX12 blend state over the
            // frame's transparent-black clear already emits it.
            constexpr DXGI_FORMAT kCompFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            constexpr DWORD kQuiesceMs = 2000;
            constexpr DWORD kJoinMs = 2000;
            // How long the caller that did NOT win the stop waits for the one that did.
            // A join answers inside `kJoinMs`; past that the winner has already settled
            // the state one way or the other.
            constexpr DWORD kJoinWatchMs = kJoinMs + 500;
            constexpr DWORD kFenceWaitMs = 1000;
            constexpr DWORD kAckPollMs = 50;

            // SURFACE THREAD. One published frame: wait for the render fence, take the
            // texture DirectComposition offers, copy the target into it at the offset it
            // names, commit. The offset is asked for every frame because the texture
            // BeginDraw returns is not the same one between frames. False means the frame
            // was dropped and nothing reached the compositor.
            bool copy_frame(UINT index, std::uint64_t fence_value)
            {
                if (g_fence != nullptr && g_fence->GetCompletedValue() < fence_value)
                {
                    // Reset first: a wait that timed out earlier can leave this
                    // auto-reset event signalled by the fence that arrived late, and the
                    // next frame would then read it as its own completion.
                    ::ResetEvent(g_surf_fence_event);
                    if (FAILED(g_fence->SetEventOnCompletion(fence_value, g_surf_fence_event)))
                    {
                        return false;
                    }
                    if (::WaitForSingleObject(g_surf_fence_event, kFenceWaitMs) != WAIT_OBJECT_0)
                    {
                        // The GPU has not finished drawing this target. Copying it now
                        // would compose a half-drawn frame, so it is dropped instead.
                        if (!g_wait_logged)
                        {
                            g_wait_logged = true;
                            mm::logf(L"composition: the overlay's frame was not finished within {} ms, "
                                     L"so it is dropped rather than composed half-drawn",
                                     kFenceWaitMs);
                        }
                        return false;
                    }
                }
                ID3D11Texture2D* dest = nullptr;
                POINT off{};
                const HRESULT hr = g_surface->BeginDraw(nullptr, IID_PPV_ARGS(&dest), &off);
                if (FAILED(hr) || dest == nullptr)
                {
                    if (!g_draw_logged)
                    {
                        g_draw_logged = true;
                        mm::logf(L"composition: BeginDraw on the overlay's surface failed (0x{:08X}) - "
                                 L"the overlay stops reaching the compositor, the game is unaffected",
                                 static_cast<unsigned>(hr));
                    }
                    return false;
                }
                g_on12->AcquireWrappedResources(&g_wrapped[index], 1);
                const D3D11_BOX box{0, 0, 0, g_surface_w, g_surface_h, 1};
                g_ctx11->CopySubresourceRegion(dest, 0, static_cast<UINT>(off.x),
                                               static_cast<UINT>(off.y), 0, g_wrapped[index], 0, &box);
                g_on12->ReleaseWrappedResources(&g_wrapped[index], 1);
                g_ctx11->Flush();
                ++g_copy_value;
                g_queue11->Signal(g_copy_fence, g_copy_value);
                // Recorded before the target is handed back, so a render thread that sees
                // it free reads the value that frees it.
                g_target_copy[index].store(g_copy_value, std::memory_order_release);
                g_surface->EndDraw();
                // Commit blocks on the compositor when it is called faster than the
                // compositor consumes. That stall lands here and never on the game's
                // present, which is the whole reason this thread exists.
                g_comp_device->Commit();
                safe_release(dest);
                return true;
            }

            DWORD WINAPI surface_proc(LPVOID)
            {
                for (;;)
                {
                    ::WaitForSingleObject(g_surf_wake, INFINITE);
                    if (g_surf_stop.load(std::memory_order_acquire))
                    {
                        break;
                    }
                    if (g_surf_pause_req.load(std::memory_order_acquire))
                    {
                        // Parked is the fact; the ack event only wakes the waiter. The
                        // flag is set before the event, so whoever the event releases
                        // reads it true.
                        g_surf_parked.store(true, std::memory_order_release);
                        ::SetEvent(g_surf_ack);
                        while (g_surf_pause_req.load(std::memory_order_acquire)
                               && !g_surf_stop.load(std::memory_order_acquire))
                        {
                            ::WaitForSingleObject(g_surf_wake, 20);
                        }
                        g_surf_parked.store(false, std::memory_order_release);
                        if (g_surf_stop.load(std::memory_order_acquire))
                        {
                            break;
                        }
                        continue;
                    }
                    const std::uint64_t slot = g_mailbox.exchange(0, std::memory_order_acq_rel);
                    if (slot == 0)
                    {
                        continue;
                    }
                    const UINT index = static_cast<UINT>(slot & kMailboxIndexMask);
                    if (!copy_frame(index, slot >> kMailboxIndexBits))
                    {
                        g_dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_target_busy[index].store(false, std::memory_order_release);
                }
                return 0;
            }

            // Brings the surface thread to a stop inside its wait loop and waits until it
            // says it has. Everything the thread dereferences may be rebuilt only between
            // this and `resume_surface_thread`.
            bool quiesce_surface_thread()
            {
                const SurfState state = g_surf_state.load(std::memory_order_acquire);
                if (state == SurfState::None)
                {
                    return true; // never created, or stopped and joined: nothing reads anything
                }
                if (state != SurfState::Running)
                {
                    // Stopping or wedged. It may be inside a copy and it will not answer a
                    // park request, so nothing it reads may be released.
                    return false;
                }
                if (g_surf_parked.load(std::memory_order_acquire))
                {
                    return true;
                }
                g_surf_pause_req.store(true, std::memory_order_release);
                ::SetEvent(g_surf_wake);
                // The ack event is only an early-out: a quiesce that timed out earlier can
                // leave it signalled, so `g_surf_parked` is what decides.
                const std::uint64_t deadline = ::GetTickCount64() + kQuiesceMs;
                do
                {
                    ::WaitForSingleObject(g_surf_ack, kAckPollMs);
                    if (g_surf_parked.load(std::memory_order_acquire))
                    {
                        return true;
                    }
                } while (::GetTickCount64() < deadline);
                mm::logf(L"composition: the surface thread did not stand down within {} ms - the "
                         L"surface is left as it is and the overlay skips this rebuild",
                         kQuiesceMs);
                return false;
            }

            void resume_surface_thread()
            {
                if (g_surf_state.load(std::memory_order_acquire) != SurfState::Running)
                {
                    return;
                }
                g_surf_pause_req.store(false, std::memory_order_release);
                // The answer goes with the request. The thread clears it too, but not
                // until it wakes, and a quiesce in that gap would read a stale `parked`
                // as its own and free the surface under a thread about to run. This runs
                // under `g_render_lock`, which is the only place a rebuild happens.
                g_surf_parked.store(false, std::memory_order_release);
                ::SetEvent(g_surf_wake);
            }

            // The three handles the surface thread is driven by. Created once for the life
            // of the module and never closed, because the render thread signals them while
            // another thread may be stopping the thread, and a recycled handle value would
            // then be signalled instead.
            bool ensure_events()
            {
                if (g_surf_wake == nullptr)
                {
                    g_surf_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                }
                if (g_surf_ack == nullptr)
                {
                    g_surf_ack = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                }
                if (g_surf_fence_event == nullptr)
                {
                    g_surf_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                }
                return g_surf_wake != nullptr && g_surf_ack != nullptr
                       && g_surf_fence_event != nullptr;
            }

            // Teardown only, and only with the thread joined: waits until the last copy it
            // submitted has actually run, so the wrappers, the queue and the targets are
            // not released out from under work still on the GPU.
            void wait_for_copies()
            {
                if (g_copy_fence == nullptr || g_surf_fence_event == nullptr || g_copy_value == 0)
                {
                    return;
                }
                if (g_copy_fence->GetCompletedValue() >= g_copy_value)
                {
                    return;
                }
                ::ResetEvent(g_surf_fence_event);
                if (SUCCEEDED(g_copy_fence->SetEventOnCompletion(g_copy_value, g_surf_fence_event))
                    && ::WaitForSingleObject(g_surf_fence_event, kFenceWaitMs) != WAIT_OBJECT_0)
                {
                    mm::logf(L"composition: the last copy had still not run after {} ms, so the "
                             L"teardown stopped waiting for it",
                             kFenceWaitMs);
                }
            }
        } // namespace

        DXGI_FORMAT comp_format()
        {
            return kCompFormat;
        }

        ID3D12CommandQueue* comp_queue()
        {
            return g_comp_queue;
        }

        bool comp_ready()
        {
            return g_comp_device != nullptr && g_comp_visual != nullptr && g_on12 != nullptr
                   && g_comp_queue != nullptr;
        }

        bool comp_thread_stopped()
        {
            return g_surf_state.load(std::memory_order_acquire) == SurfState::None;
        }

        std::uint64_t comp_skipped_frames()
        {
            return g_skipped.load(std::memory_order_relaxed);
        }

        std::uint64_t comp_dropped_frames()
        {
            return g_dropped.load(std::memory_order_relaxed);
        }

        void comp_reset_counters()
        {
            g_skipped.store(0, std::memory_order_relaxed);
            g_dropped.store(0, std::memory_order_relaxed);
        }

        int comp_pick_target()
        {
            const std::uint64_t done = g_copy_fence != nullptr ? g_copy_fence->GetCompletedValue() : 0;
            for (int i = 0; i < static_cast<int>(kTargets); ++i)
            {
                if (g_target_busy[i].load(std::memory_order_acquire))
                {
                    continue;
                }
                if (g_target_copy[i].load(std::memory_order_acquire) <= done)
                {
                    return i;
                }
            }
            // The compositor is behind. The frame is dropped rather than waited for: the
            // thread inside the game's Present never stalls on anything of ours.
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }

        void comp_publish(int index, std::uint64_t fence_value)
        {
            if (g_surf_state.load(std::memory_order_acquire) != SurfState::Running || index < 0
                || index >= static_cast<int>(kTargets))
            {
                return;
            }
            g_target_busy[index].store(true, std::memory_order_release);
            const std::uint64_t slot =
                (fence_value << kMailboxIndexBits) | static_cast<std::uint64_t>(index);
            const std::uint64_t prev = g_mailbox.exchange(slot, std::memory_order_acq_rel);
            if (prev != 0)
            {
                // The surface thread never saw that frame, so its target was not copied
                // and is free the moment this one replaces it.
                g_target_busy[prev & kMailboxIndexMask].store(false, std::memory_order_release);
            }
            ::SetEvent(g_surf_wake);
        }

        void comp_stop_thread()
        {
            SurfState running = SurfState::Running;
            if (!g_surf_state.compare_exchange_strong(running, SurfState::Stopping,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
            {
                // Another thread is joining it, or it is already gone or wedged. Waiting
                // for the join to answer is the whole point: a caller that returned while
                // the thread might still be running would go on to free what it reads.
                const std::uint64_t deadline = ::GetTickCount64() + kJoinWatchMs;
                while (g_surf_state.load(std::memory_order_acquire) == SurfState::Stopping
                       && ::GetTickCount64() < deadline)
                {
                    ::Sleep(1);
                }
                return;
            }
            HANDLE thread = g_surf_thread;
            g_surf_stop.store(true, std::memory_order_release);
            // A parked thread polls the pause request, so it has to be cleared before the
            // stop can be seen.
            g_surf_pause_req.store(false, std::memory_order_release);
            ::SetEvent(g_surf_wake);
            if (::WaitForSingleObject(thread, kJoinMs) == WAIT_OBJECT_0)
            {
                ::CloseHandle(thread);
                g_surf_thread = nullptr;
                g_surf_stop.store(false, std::memory_order_release);
                g_surf_parked.store(false, std::memory_order_release);
                g_surf_state.store(SurfState::None, std::memory_order_release);
                return;
            }
            // WEDGED, and permanently. The thread is still running and may be inside a
            // copy, so its handle stays open, the stop flag stays set, and everything it
            // reads is leaked from here on rather than released under it.
            g_surf_state.store(SurfState::Wedged, std::memory_order_release);
            // Its procedure is code in main.dll. `uninstall_mod` would otherwise let the
            // loader unload the module out from under a thread executing it, which is a
            // crash in the game's address space rather than in ours - so the module is
            // pinned and never leaves the process.
            HMODULE self = nullptr;
            const BOOL pinned = ::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&surface_proc), &self);
            mm::logf(L"composition: the surface thread did not exit within {} ms. It is left running "
                     L"and every object it reads is leaked on purpose - the overlay does not come "
                     L"back this session. The mod's module is pinned ({}), so unloading the mod "
                     L"cannot take the running thread's code away; the game keeps rendering either "
                     L"way.",
                     kJoinMs,
                     pinned != FALSE ? L"held" : L"FAILED - do not unload this mod");
        }

        bool comp_unbind_targets()
        {
            if (!quiesce_surface_thread())
            {
                // Nothing is released under a thread that may still be reading it - the
                // render targets included, which is why the caller is told. The frame
                // then asks for a re-adoption, and that release either joins the thread
                // or marks it wedged.
                //
                // "May still be reading it" includes a join another thread is inside
                // right now: a park request would never be answered, and the thread is
                // not gone until that join says so.
                return false;
            }
            for (ID3D11Resource*& w : g_wrapped)
            {
                safe_release(w);
            }
            if (g_comp_visual != nullptr && g_surface != nullptr)
            {
                g_comp_visual->SetContent(nullptr);
                g_comp_device->Commit();
            }
            safe_release(g_surface);
            g_surface_w = 0;
            g_surface_h = 0;
            g_mailbox.store(0, std::memory_order_release);
            for (int i = 0; i < static_cast<int>(kTargets); ++i)
            {
                g_target_busy[i].store(false, std::memory_order_release);
                g_target_copy[i].store(0, std::memory_order_release);
            }
            return true;
        }

        void comp_release()
        {
            // The thread first: everything below is something it dereferences.
            comp_stop_thread();
            if (!comp_thread_stopped())
            {
                // It is still running. Releasing a single one of these would be a
                // use-after-free on a live thread, and that includes the composition
                // device itself, whose `Commit` the copy calls. So nothing is released
                // and nothing is nulled: the pointers stay valid for the thread that
                // holds them, `comp_ready()` stays true, and no later frame gets here
                // because the overlay is finished for this session.
                if (!g_leak_logged)
                {
                    g_leak_logged = true;
                    mm::log(L"composition: the wedged surface thread's device, queues, surface and "
                            L"wrapped targets are leaked deliberately - releasing them would free "
                            L"memory a running thread is reading");
                }
                return;
            }
            if (g_ctx11 != nullptr)
            {
                g_ctx11->Flush();
            }
            // The last copy may still be on the GPU; the wrappers, the queue and the
            // targets it reads may not go before it has run.
            wait_for_copies();
            (void)comp_unbind_targets();
            safe_release(g_surface_factory);
            safe_release(g_on12);
            safe_release(g_ctx11);
            safe_release(g_dev11);
            safe_release(g_copy_fence);
            safe_release(g_queue11);
            if (g_comp_target != nullptr)
            {
                g_comp_target->SetRoot(nullptr);
            }
            if (g_comp_device != nullptr)
            {
                g_comp_device->Commit();
            }
            safe_release(g_comp_visual);
            safe_release(g_comp_target);
            safe_release(g_comp_device);
            safe_release(g_comp_queue);
            g_copy_value = 0;
            comp_reset_counters();
            g_draw_logged = false;
            g_wait_logged = false;
            // `g_dcomp_dll` and `g_d3d11_dll` are deliberately not freed: both are system
            // modules that were already resident, and this module may be created again.
            // The three event handles are not closed either - see the prose block.
        }

        // Creates both queues, the D3D11On12 device, the composition device, target and
        // visual, and starts the surface thread. `device` is the game's; `hwnd` is the
        // game's window (this process's), which CreateTargetForHwnd requires. The surface
        // itself is `comp_bind_targets`'s, because its size is the render targets' size.
        bool comp_create(ID3D12Device* device, HWND hwnd)
        {
            if (device == nullptr || hwnd == nullptr)
            {
                mm::logf(L"composition: nothing to create from (device {:p}, hwnd 0x{:X})",
                         static_cast<void*>(device),
                         reinterpret_cast<std::uintptr_t>(hwnd));
                return false;
            }
            if (!comp_thread_stopped())
            {
                return false; // a live or wedged thread owns the objects a rebuild would replace
            }
            if (g_dcomp_dll == nullptr)
            {
                g_dcomp_dll = ::LoadLibraryW(L"dcomp.dll");
            }
            if (g_d3d11_dll == nullptr)
            {
                g_d3d11_dll = ::LoadLibraryW(L"d3d11.dll");
            }
            if (g_dcomp_dll == nullptr || g_d3d11_dll == nullptr)
            {
                mm::log(L"composition: dcomp.dll or d3d11.dll is not available on this system");
                return false;
            }
            auto create_device2 = reinterpret_cast<DCompositionCreateDevice2Fn>(
                reinterpret_cast<void*>(::GetProcAddress(g_dcomp_dll, "DCompositionCreateDevice2")));
            auto create_11on12 = reinterpret_cast<D3D11On12CreateDeviceFn>(
                reinterpret_cast<void*>(::GetProcAddress(g_d3d11_dll, "D3D11On12CreateDevice")));
            if (create_device2 == nullptr || create_11on12 == nullptr)
            {
                mm::log(L"composition: DCompositionCreateDevice2 or D3D11On12CreateDevice is missing - "
                        L"the overlay does not start. The rest of the mod keeps running.");
                return false;
            }
            if (!ensure_events())
            {
                mm::log(L"composition: the surface thread's events could not be created");
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_comp_queue));
            if (SUCCEEDED(hr))
            {
                hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue11));
            }
            if (FAILED(hr) || g_comp_queue == nullptr || g_queue11 == nullptr)
            {
                mm::logf(L"composition: the overlay's command queues could not be created (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copy_fence));
            if (FAILED(hr) || g_copy_fence == nullptr)
            {
                mm::logf(L"composition: the copy fence could not be created (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }

            IUnknown* queues[] = {g_queue11};
            hr = create_11on12(device, 0, nullptr, 0, queues, 1, 0, &g_dev11, &g_ctx11, nullptr);
            if (SUCCEEDED(hr) && g_dev11 != nullptr)
            {
                hr = g_dev11->QueryInterface(IID_PPV_ARGS(&g_on12));
            }
            if (FAILED(hr) || g_on12 == nullptr || g_ctx11 == nullptr)
            {
                mm::logf(L"composition: D3D11On12 over the game's device failed (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }

            // No IDXGIDevice is passed: a D3D12 device does not expose one, and the
            // parameter is optional - DirectComposition then uses its own.
            hr = create_device2(nullptr, IID_PPV_ARGS(&g_comp_device));
            if (FAILED(hr) || g_comp_device == nullptr)
            {
                mm::logf(L"composition: DCompositionCreateDevice2 failed (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            hr = g_comp_device->CreateSurfaceFactory(g_dev11, &g_surface_factory);
            if (FAILED(hr) || g_surface_factory == nullptr)
            {
                mm::logf(L"composition: CreateSurfaceFactory on the D3D11On12 device failed (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            // Topmost is where an overlay belongs, and the window must belong to this
            // process - it does, since the mod is loaded into the game. A window holds at
            // most one topmost target and one below it, so if something else in this
            // process took the topmost slot, the other one is the only honest answer:
            // drawn under it, but drawn.
            hr = g_comp_device->CreateTargetForHwnd(hwnd, TRUE, &g_comp_target);
            if (FAILED(hr) || g_comp_target == nullptr)
            {
                safe_release(g_comp_target);
                mm::logf(L"composition: the topmost target for hwnd 0x{:X} is taken (0x{:08X}) - "
                         L"asking for the one below it instead",
                         reinterpret_cast<std::uintptr_t>(hwnd),
                         static_cast<unsigned>(hr));
                hr = g_comp_device->CreateTargetForHwnd(hwnd, FALSE, &g_comp_target);
            }
            if (FAILED(hr) || g_comp_target == nullptr)
            {
                mm::logf(L"composition: CreateTargetForHwnd failed for hwnd 0x{:X} (0x{:08X})",
                         reinterpret_cast<std::uintptr_t>(hwnd),
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            hr = g_comp_device->CreateVisual(&g_comp_visual);
            if (FAILED(hr) || g_comp_visual == nullptr)
            {
                mm::logf(L"composition: CreateVisual failed (0x{:08X})", static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            if (FAILED(g_comp_target->SetRoot(g_comp_visual)))
            {
                mm::log(L"composition: the visual could not be attached to the window");
                comp_release();
                return false;
            }

            // Created parked: the surface and the wrapped resources do not exist yet, and
            // `comp_bind_targets` is what lets it run.
            g_surf_stop.store(false, std::memory_order_release);
            g_surf_parked.store(false, std::memory_order_release);
            g_surf_pause_req.store(true, std::memory_order_release);
            // Running BEFORE the thread exists: a stop landing in the gap would
            // otherwise find `None`, no-op, and leave the thread running through a
            // release. A failed create puts it back.
            g_surf_state.store(SurfState::Running, std::memory_order_release);
            g_surf_thread = ::CreateThread(nullptr, 0, &surface_proc, nullptr, 0, nullptr);
            if (g_surf_thread == nullptr)
            {
                g_surf_state.store(SurfState::None, std::memory_order_release);
                mm::log(L"composition: the surface thread could not be created");
                comp_release();
                return false;
            }
            return true;
        }

        // The render targets have been (re)created: make a surface of their size, wrap
        // them for D3D11On12 and let the surface thread run again. The previous surface
        // and wrappers are the caller's to have released, through `comp_unbind_targets`.
        bool comp_bind_targets(ID3D12Resource* const* targets, UINT width, UINT height)
        {
            if (!comp_ready() || g_surf_state.load(std::memory_order_acquire) != SurfState::Running
                || g_surface != nullptr || targets == nullptr
                || width == 0 || height == 0)
            {
                return false;
            }
            HRESULT hr = g_surface_factory->CreateSurface(width, height, kCompFormat,
                                                          DXGI_ALPHA_MODE_PREMULTIPLIED, &g_surface);
            if (FAILED(hr) || g_surface == nullptr)
            {
                mm::logf(L"composition: CreateSurface {}x{} failed (0x{:08X})",
                         width,
                         height,
                         static_cast<unsigned>(hr));
                (void)comp_unbind_targets();
                return false;
            }
            D3D11_RESOURCE_FLAGS rf{};
            rf.BindFlags = D3D11_BIND_RENDER_TARGET;
            for (UINT i = 0; i < kTargets; ++i)
            {
                // In and out state are the same, and the state the targets live in for
                // their whole life: the frame records no transition on them at all.
                hr = g_on12->CreateWrappedResource(targets[i], &rf,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   IID_PPV_ARGS(&g_wrapped[i]));
                if (FAILED(hr) || g_wrapped[i] == nullptr)
                {
                    mm::logf(L"composition: CreateWrappedResource({}) failed (0x{:08X})",
                             i,
                             static_cast<unsigned>(hr));
                    (void)comp_unbind_targets();
                    return false;
                }
            }
            g_surface_w = width;
            g_surface_h = height;
            if (FAILED(g_comp_visual->SetContent(g_surface)) || FAILED(g_comp_device->Commit()))
            {
                mm::log(L"composition: the surface could not be attached to the visual");
                (void)comp_unbind_targets();
                return false;
            }
            resume_surface_thread();
            if (!g_comp_logged)
            {
                g_comp_logged = true;
                mm::logf(L"composition: the overlay draws into {} target(s) of its own - {}x{} {}, "
                         L"premultiplied alpha - which a thread of the mod's own copies into a "
                         L"DirectComposition surface over the game's window. There is no second "
                         L"swapchain and no second Present anywhere, which is what makes this work "
                         L"under frame generation, capture and overlay layers.",
                         kTargets,
                         width,
                         height,
                         format_name(kCompFormat));
            }
            return true;
        }

    } // namespace ovl
} // namespace overlay
