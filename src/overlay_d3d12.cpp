//
// overlay_d3d12 - the D3D12 device objects, the swapchain hooks and the per-frame
// render entry point.
//
// Everything here runs on the RENDER thread except the slice request the loop thread
// leaves behind: the SRV heap, the map and height-slice textures, the ImGui backends,
// the Present / Present1 / ResizeBuffers / ExecuteCommandLists hooks and the hook
// address cache they are installed from.
//
// HOOK STRATEGY (the hudhook approach)
// ------------------------------------
// The game's IDXGISwapChain and ID3D12CommandQueue are not reachable from a UE4SS mod,
// and the swapchain's command queue cannot be queried back out of the swapchain. So:
//
//   1. create a throwaway D3D12 device + DIRECT command queue + a 64x64 swapchain on a
//      hidden window, purely to read their vtables;
//   2. MinHook the absolute addresses of IDXGISwapChain::Present (vtable slot 8),
//      ResizeBuffers (13), IDXGISwapChain1::Present1 (22) and
//      ID3D12CommandQueue::ExecuteCommandLists (10);
//   3. throw the dummy objects away and wait. The first real Present gives us the
//      swapchain; the first real ExecuteCommandLists gives us the queue.
//
// The dummy objects are created through *our own import table*, i.e. through whatever
// `dxgi.dll` is loaded in the process. On this machine that is **ReShade's** proxy, so
// our dummy swapchain is a ReShade wrapper with exactly the same vtable the game holds
// - which is the point: we hook the same slot the game calls, whoever owns it. The
// module that owns every hooked address is logged, so the log answers the coexistence
// question directly instead of us having to guess. A watchdog complains if no Present
// arrives within a few seconds, which is the signal that the swapchain is wrapped by
// something we did not go through (e.g. a DLSS-FG proxy).
//
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // SRV descriptor heap allocator
        //==============================================================================
        //
        // ImGui 1.92's DX12 backend allocates SRV descriptors through callbacks (it
        // needs one per texture now, not just one for the font atlas), so the heap and
        // its free list are ours. A fixed 64-slot heap is plenty: the font atlas plus
        // one map texture.



        void srv_alloc_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            if (!g_srv_heap.alloc(*cpu, *gpu))
            {
                cpu->ptr = 0;
                gpu->ptr = 0;
                // ONCE. ImGui asks for a descriptor when it (re)builds the font atlas,
                // which is not a per-frame event - but this is a render-thread callback
                // and a heap that is full stays full, so it says so once and then stops.
                static bool said = false;
                if (!said)
                {
                    said = true;
                    mm::log(L"SRV heap exhausted - raise srv_heap_size (RESTART) if the "
                            L"overlay is missing textures");
                }
            }
        }

        void srv_free_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            g_srv_heap.free(cpu);
        }

        //==============================================================================
        // Renderer state
        //==============================================================================



        //==============================================================================
        // The height-slice texture
        //==============================================================================
        //
        // A small dynamic RGBA8 texture the CPU slicer refills at slice_hz. Two of
        // them, because the GPU may still be sampling one while we write the next:
        // `in_flight_fence` is the fence value of the last frame that DREW this buffer,
        // so a buffer is only rewritten once GetCompletedValue() has passed it.
        //
        // The upload heap stays mapped for the buffer's whole life (a 512x512 RGBA
        // window is 1 MB, and Map/Unmap per update is pure overhead), and the copy is
        // recorded on the same command list Present already records for ImGui - so
        // there is no extra queue, no PSO and no root signature on ReShade's swapchain.




        // QueryPerformanceFrequency is a constant for the life of the process, and it
        // was being asked for on every slice and every map cut. Once, lazily.
        std::int64_t qpc_freq()
        {
            static const std::int64_t freq = [] {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                return static_cast<std::int64_t>(f.QuadPart);
            }();
            return freq;
        }



        // The window proc that was there before ours, and the one every message is

        // ATOMIC, because all three are written by the RENDER thread and read by other
        // threads: `g_imgui_ready` gates the WndProc hook's whole body on the GAME
        // thread, and the F2 panel / the loop thread read the other two. As plain bools
        // NOT TERMINAL FOR A SWAPCHAIN-LEVEL FAILURE. `g_failed` means "this mod cannot
        // draw and must stop trying": a hook that would not install, a device that is




        //==============================================================================
        // The slicer runs on the LOOP thread
        //==============================================================================
        //
        // `slice_region` is 1-4 ms of pure CPU that writes into a persistently mapped
        // upload heap. Nothing about it needs Present, and running it inside the frame
        // put a 1-4 ms spike into frame time twelve times a second. It now runs from
        // `overlay::on_update` on the UE4SS loop thread; `render()` only records the
        // CopyTextureRegion for a buffer the slicer has finished and stamps the fence of
        // the frame that sampled it.
        //
        // THE OWNERSHIP RULES, which are what keep this safe:
        //   * the RENDER thread owns creation and destruction of every D3D12 object,
        //     including the slice buffers, and publishes what it wants cut;
        //   * the LOOP thread only ever writes into `mapped` memory of a buffer that
        //     already exists, and only while the slicer is not paused;
        //   * a buffer may be written only when the fence of the last frame that
        //     sampled it has completed (`g_slice_in_flight`), exactly as before;
        //   * before creating or destroying any slice buffer the render thread PAUSES
        //     the slicer and waits, with a bound, for it to leave the critical section.
        //     If it cannot, it does not touch the buffers this frame and tries again.
        //   * `g_slice_gen` is bumped by every create/destroy; the slicer re-reads it
        //     after writing and throws the result away if it moved, so a cut can never
        //     be published against a buffer set that no longer exists.






        SliceView slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            return g_slice_view;
        }

        MapSliceView map_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            return g_mslice_view;
        }

        void clear_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            g_slice_view = SliceView{};
        }

        void clear_map_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            g_mslice_view = MapSliceView{};
        }

        // RENDER THREAD. Stop the slicer and wait for it to leave its critical section.
        // Returns false when it did not stop inside `budget_ms` - the caller must then
        // leave every slice buffer alone and try again on the next frame. The slicer's
        // critical section is a few milliseconds of arithmetic and never blocks, so a
        // timeout means something is very wrong and skipping is the safe answer.
        bool slicer_pause_begin(unsigned budget_ms)
        {
            g_slicer_pause.store(true); // seq_cst on purpose: it pairs with the loop's
                                        // "check, mark busy, check again" sequence
            const std::uint64_t deadline = ::GetTickCount64() + budget_ms;
            while (g_slicer_busy.load())
            {
                if (::GetTickCount64() > deadline)
                {
                    g_slicer_pause.store(false);
                    return false;
                }
                ::SwitchToThread();
            }
            return true;
        }

        void slicer_pause_end()
        {
            g_slicer_pause.store(false);
        }

        // Bumped by every create/destroy so a cut in flight can be discarded.
        void note_slice_buffers_changed()
        {
            g_slice_gen.fetch_add(1, std::memory_order_release);
        }

        //==============================================================================
        // The full map (step C1)
        //==============================================================================
        //
        // The same height-sliced asset the minimap draws, at map scale: north-up,
        // pannable, zoomable, with every marker on it. It has its own pair of dynamic
        // textures because its window is both bigger and DECIMATED - one texture pixel
        // covers `step` source pixels - and its own update policy: the minimap re-cuts
        // 12 times a second because the player is always moving, while the map only
        // re-cuts when something actually changed (pan out of the cut region, zoom,
        // floor slice, a big player move), capped at map_slice_hz.



        // The SAME object as g_swapchain, QueryInterface'd once and kept with a
        // reference held, because `GetCurrentBackBufferIndex()` lives only on
        // IDXGISwapChain3 and a QI per frame is a virtual call plus an AddRef/Release

        // RE-ADOPTION. Set when the swapchain or the device we latched onto has stopped
        // being usable - DXGI_ERROR_DEVICE_REMOVED / _RESET out of Present, a GetBuffer
        // or render-target failure, a command queue that turns out to belong to another

        // Drops the captured command queue AND the reference held on it. Only ever
        // called from the render thread's teardown, so no other thread can be inside
        // `o_ExecuteCommandLists(queue, ...)` with our pointer at the same time.
        void safe_release_queue()
        {
            ID3D12CommandQueue* queue = g_queue.exchange(nullptr, std::memory_order_acq_rel);
            if (queue != nullptr)
            {
                queue->Release();
            }
        }

        void request_readoption(const wchar_t* why)
        {
            if (!g_readopt.exchange(true, std::memory_order_release))
            {
                g_readopt_count.fetch_add(1, std::memory_order_relaxed);
                mm::logf(L"the overlay is releasing its D3D12 objects and will adopt the swapchain again "
                         L"on a later Present: {}",
                         why);
            }
        }

        // WHAT THE RENDER THREAD IS DOING, and which thread it is, for the loop

        // The stage names are ASCII literals and the log takes wide strings. An explicit
        // cast loop rather than `std::wstring(a.begin(), a.end())`, which warns (C4244)
        // and this repo is warning-free by policy.
        std::wstring stage_w(const char* s)
        {
            const char* p = s != nullptr ? s : "?";
            std::wstring out;
            for (; *p != '\0'; ++p)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
            }
            return out;
        }



        // Every hide/show transition is logged with its reason, so one line in the log

        void set_hide_reason(const wchar_t* text)
        {
            if (::wcscmp(g_hide_reason, text) == 0)
            {
                return; // unchanged - nothing to record, nothing to log
            }
            ::wcsncpy_s(g_hide_reason, text, std::size(g_hide_reason) - 1);
            const std::uint64_t now = ::GetTickCount64();
            const std::uint64_t held = g_reason_since_ms == 0 ? 0 : now - g_reason_since_ms;
            g_reason_since_ms = now;
            if (::wcscmp(g_reason_logged, text) == 0)
            {
                return; // flapping between two states we already reported
            }
            if (now - g_reason_log_ms < static_cast<std::uint64_t>(mm::cfg_cached().hide_reason_log_ms))
            {
                ++g_reason_suppressed;
                return;
            }
            const bool visible = ::wcscmp(text, L"visible") == 0;
            MM_LOGV(L"minimap {}: {} (previous state held {} ms{})",
                    visible ? L"SHOWN" : L"HIDDEN",
                    text,
                    held,
                    g_reason_suppressed != 0 ? std::format(L", {} change(s) suppressed",
                                                           g_reason_suppressed)
                                             : std::wstring{});
            ::wcsncpy_s(g_reason_logged, text, std::size(g_reason_logged) - 1);
            g_reason_log_ms = now;
            g_reason_suppressed = 0;
        }

        //==============================================================================
        // Original functions
        //==============================================================================



        //==============================================================================
        // Teardown of the swapchain-dependent objects
        //==============================================================================

        void release_render_targets()
        {
            for (UINT i = 0; i < kMaxBuffers; ++i)
            {
                safe_release(g_backbuffers[i]);
                g_rtv[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            safe_release(g_rtv_heap);
            g_rt_ready = false;
        }

        void wait_for_gpu()
        {
            if (g_fence == nullptr || g_fence_event == nullptr)
            {
                return;
            }
            if (g_fence->GetCompletedValue() >= g_fence_value)
            {
                return;
            }
            if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fence_value, g_fence_event)))
            {
                ::WaitForSingleObject(g_fence_event, 1000);
            }
        }

        // How many frames in flight the ImGui DX12 backend was initialised with. A

        // ONE ALLOCATOR PER BACK BUFFER, created for every buffer that has none. They
        // used to be created once, for the count seen at init: after a fullscreen toggle
        // that raised BufferCount from 2 to 3, `render()` called `frame.allocator->Reset()`
        // on a null pointer. `g_buffer_count` is already clamped to kMaxBuffers.
        bool ensure_frame_allocators()
        {
            if (g_device == nullptr)
            {
                return false;
            }
            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (g_frames[i].allocator != nullptr)
                {
                    continue;
                }
                if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&g_frames[i].allocator))))
                {
                    mm::logf(L"CreateCommandAllocator({}) failed", i);
                    return false;
                }
                g_frames[i].fence_value = 0;
            }
            return true;
        }

        bool create_render_targets(IDXGISwapChain* swapchain)
        {
            release_render_targets();

            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(swapchain->GetDesc(&desc)))
            {
                mm::log(L"GetDesc failed - cannot create render targets");
                return false;
            }
            g_buffer_count = (std::min)(desc.BufferCount, static_cast<UINT>(kMaxBuffers));
            if (g_buffer_count == 0)
            {
                g_buffer_count = 2;
            }
            g_format = desc.BufferDesc.Format;
            g_width = desc.BufferDesc.Width;
            g_height = desc.BufferDesc.Height;
            g_hwnd = desc.OutputWindow;

            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = g_buffer_count;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap))))
            {
                mm::log(L"CreateDescriptorHeap(RTV) failed");
                return false;
            }

            const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]))))
                {
                    mm::logf(L"GetBuffer({}) failed", i);
                    release_render_targets();
                    return false;
                }
                g_device->CreateRenderTargetView(g_backbuffers[i], nullptr, handle);
                g_rtv[i] = handle;
                handle.ptr += stride;
            }

            // DOES THE BACK BUFFER BELONG TO THE DEVICE WE TOOK OFF THE QUEUE? The
            // device comes from the first DIRECT command queue seen executing anywhere
            // in the process (IDXGISwapChain::GetDevice does not work through this
            // game's ReShade wrapper), and with frame generation or a second renderer
            // that queue need not belong to the presenting device. `ID3D12Resource::
            // GetDevice` on a back buffer answers authoritatively, and it is the one
            // link from the swapchain to a device that the wrapper does forward.
            // Recording our command list on a queue of a different device is an
            // immediate device removal, so this is a hard reject.
            if (g_backbuffers[0] != nullptr)
            {
                ID3D12Device* owner = nullptr;
                if (SUCCEEDED(g_backbuffers[0]->GetDevice(IID_PPV_ARGS(&owner))) && owner != nullptr)
                {
                    const bool same = owner == g_device;
                    owner->Release();
                    if (!same)
                    {
                        mm::log(L"the back buffers belong to a different ID3D12Device than the captured "
                                L"command queue - dropping the queue so another one can be captured");
                        g_bad_queue.store(g_queue.load(std::memory_order_acquire), std::memory_order_release);
                        release_render_targets();
                        request_readoption(L"the captured command queue belongs to another device");
                        return false;
                    }
                }
            }

            if (!ensure_frame_allocators())
            {
                release_render_targets();
                return false;
            }

            g_rt_ready = true;
            mm::logf(L"render targets: {} buffer(s), {}x{}, {}, hwnd 0x{:X}",
                     g_buffer_count,
                     g_width,
                     g_height,
                     format_name(g_format),
                     reinterpret_cast<std::uintptr_t>(g_hwnd));
            return true;
        }

        //==============================================================================
        // Per-frame work
        //==============================================================================

        void build_ui()
        {
            // `raw` is what the panel edits and what Save writes; `cfg` is the same
            // settings with every pixel key multiplied by the UI scale, and it is what
            // the HUD draws from. Keeping them apart is what stops a scaled value ever
            // being written back into the config file.
            const mm::Config& raw = mm::cfg_cached();
            mm::Config cfg = ui_scaled(raw, g_ui_scale);
            // The look, once per frame: every draw path below reads these two globals
            // instead of asking the config what colour it is.
            g_palette = raw.palette;
            g_plate = gly::theme_colors(raw.theme).plate;
            // The disc-drawing helpers take geometry, not the config, so the live
            // roundness is cached here once per frame (render thread only).
            mm::Snapshot snap{};
            const bool have = mm::read_snapshot(snap);

            const bool map_open = mm::g_map_open.load(std::memory_order_relaxed);
            const std::uint64_t frame_now = ::GetTickCount64();

            // THE HUD FADE. Its target is the gate result and nothing else: 0 is applied
            // instantly (and the gate stops the draw anyway, so hiding is immediate),
            // and only showing is eased - 150 ms, so a menu closing does not snap the
            // HUD back on. It is applied by scaling the opacity keys of the per-frame
            // config copy, which is why `cfg` is a mutable copy: nothing downstream has
            // to know the fade exists, and no scaled value can reach the config file.
            const bool gate_open = have && hud_gate(cfg, snap, have, frame_now) == nullptr;
            if (gate_open && !g_hud_gate_ever_open.load(std::memory_order_relaxed))
            {
                // The first frame anything of ours could be seen. The first-run tip on
                // the loop thread is waiting for exactly this (review B.2).
                g_hud_gate_ever_open.store(true, std::memory_order_release);
            }
            const float fade = hud_fade_step(gate_open && cfg.overlay_enabled, frame_now);
            cfg.opacity *= fade;
            cfg.compass_opacity *= fade;
            cfg.highlight_alpha_near *= fade;
            cfg.highlight_alpha_far *= fade;

            // The mouse cursor belongs to whoever is taking the input. Both conditions
            // are plain reads of the live flags - nothing here is remembered, so the
            // frame the map or the panel closes is the frame the game gets the cursor
            // back.
            ImGui::GetIO().MouseDrawCursor = map_open || mm::g_panel_open.load(std::memory_order_relaxed);

            // The pad, into ImGui's own nav (review B.8). Only while the panel is open,
            // and from the state the loop thread sampled - never a poll from here.
            feed_pad_nav(raw);

            // THE ONE MARKER PASS. The minimap, the full map, the compass pips and the
            // x-ray highlight all read the same published buffer; walking it once here
            // and letting each of them filter the result replaces three (four with the
            // map open) full scans plus their square roots.
            if (have)
            {
                build_frame_candidates(snap);
            }
            else
            {
                g_frame_cands.clear();
                g_frame_marker_total = 0;
                g_frame_bad_cat = 0;
            }

            if (mm::g_panel_open.load(std::memory_order_relaxed))
            {
                draw_panel(raw, snap, have);
                mm::g_panel_drew_frame.store(true, std::memory_order_relaxed);
            }
            else if (g_capture_row >= 0)
            {
                // The panel closed with a capture armed. Disarm it here rather than in
                // the close paths: this is the one place that runs on every frame, so
                // the keyboard can never stay swallowed with no panel on screen.
                arm_capture(-1);
            }

            if (map_open)
            {
                draw_full_map(raw, snap, have, g_ui_scale);
            }
            if (g_map_was_open && !mm::g_map_open.load(std::memory_order_relaxed))
            {
                // Closed (by the key, by the gate, or from inside the map): the next
                // open starts centred on the player again.
                g_mv_init = false;
            }
            g_map_was_open = mm::g_map_open.load(std::memory_order_relaxed);

            if (!cfg.overlay_enabled || !cfg.show_minimap)
            {
                set_hide_reason(L"disabled in the config");
            }
            else if (mm::g_map_open.load(std::memory_order_relaxed))
            {
                // The full map replaces the minimap while it is up - two views of the
                // same thing on one screen is just clutter, and the slicer would then be
                // cutting two windows a frame.
                set_hide_reason(L"the full map is open");
            }
            else
            {
                if (g_pf_minimap < 0)
                {
                    g_pf_minimap = mm::perf_register("minimap draw", perf::Thread::Render);
                }
                const mm::PerfScope scope(g_pf_minimap);
                draw_minimap(cfg, snap, have);
            }

            // The compass and the x-ray highlight ask the SAME gate the minimap does -
            // one evaluation, no second set of rules, no second latch - and additionally
            // stand down while the full map is open, because the map is a mode of its own
            // (it swallows the input and covers the scene they would be drawn over).
            const bool hud_ok = gate_open && !mm::g_map_open.load(std::memory_order_relaxed);
            draw_compass(cfg, snap, hud_ok);
            draw_highlight(cfg, snap, hud_ok);

            // The clipboard result comes back from the loop thread as text plus a flag;
            // turn it into a toast here, where toasts live.
            if (g_toast_pending_ready.exchange(false, std::memory_order_acquire))
            {
                char text[160]{};
                unsigned ms = 2500;
                {
                    spin::SpinGuard guard(g_toast_lock);
                    ::strncpy_s(text, sizeof(text), g_toast_pending, _TRUNCATE);
                    ms = g_toast_pending_ms;
                }
                toast_for(text, ms);
            }

            // Toasts last, so they sit over everything they are talking about.
            draw_toast();
        }

        bool ensure_initialised(IDXGISwapChain* swapchain)
        {
            if (g_failed)
            {
                return false;
            }
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            if (queue == nullptr)
            {
                return false; // no ExecuteCommandLists yet; try again next frame
            }
            if (g_imgui_ready)  // NOLINT: the happy path, checked every frame
            {
                return true;
            }

            // The device comes off the CAPTURED QUEUE, not off the swapchain.
            // IDXGISwapChain::GetDevice(ID3D12Device) fails on this game's swapchain -
            // it is a ReShade wrapper (all four hooked addresses live in the 5.6 MB
            // dxgi.dll ReShade drops next to the exe) and its GetDevice does not hand
            // out the D3D12 device. ID3D12CommandQueue::GetDevice always does.
            HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g_device));
            if (FAILED(hr) || g_device == nullptr)
            {
                mm::logf(L"queue->GetDevice failed (0x{:08X}); trying the swapchain", static_cast<unsigned>(hr));
                hr = swapchain->GetDevice(IID_PPV_ARGS(&g_device));
            }
            if (FAILED(hr) || g_device == nullptr)
            {
                mm::logf(L"no ID3D12Device reachable from either the queue or the swapchain (0x{:08X}) - overlay off",
                         static_cast<unsigned>(hr));
                g_failed = true;
                return false;
            }

            // NOT `g_failed`: a swapchain that will not hand out its buffers is a
            // swapchain-level failure (a resize in flight, a device that has just gone,
            // a wrapper being swapped), and those recover. `create_render_targets` has
            // already asked for a re-adoption where it knew the reason.
            if (!create_render_targets(swapchain))
            {
                request_readoption(L"the render targets could not be created");
                return false;
            }

            // One allocator per back buffer, and grown again after any resize that
            // raises BufferCount (see ensure_frame_allocators).
            if (!ensure_frame_allocators())
            {
                g_failed = true;
                return false;
            }
            if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr,
                                                   IID_PPV_ARGS(&g_cmd_list))))
            {
                mm::log(L"CreateCommandList failed");
                g_failed = true;
                return false;
            }
            g_cmd_list->Close();

            if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            {
                mm::log(L"CreateFence failed");
                g_failed = true;
                return false;
            }
            g_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_fence_event == nullptr)
            {
                mm::log(L"CreateEvent failed");
                g_failed = true;
                return false;
            }

            // RESTART-ONLY config key: the heap is created once, here.
            if (!g_srv_heap.create(g_device, mm::config().srv_heap_size))
            {
                mm::log(L"CreateDescriptorHeap(SRV) failed");
                g_failed = true;
                return false;
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // We draw our own software cursor for the panel; never let ImGui fight the
            // game over the OS cursor shape.
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            // [fix-ui] keyboard + gamepad navigation for the F2 panel (review B.8).
            ui_init_io(io);
            ImGui::StyleColorsDark();
            ImGui::GetStyle().WindowRounding = 4.0f;

            if (!ImGui_ImplWin32_Init(g_hwnd))
            {
                mm::log(L"ImGui_ImplWin32_Init failed");
                g_failed = true;
                return false;
            }

            ImGui_ImplDX12_InitInfo info{};
            info.Device = g_device;
            info.CommandQueue = queue;
            info.NumFramesInFlight = static_cast<int>(g_buffer_count);
            info.RTVFormat = g_format;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.SrvDescriptorHeap = g_srv_heap.heap();
            info.SrvDescriptorAllocFn = &srv_alloc_cb;
            info.SrvDescriptorFreeFn = &srv_free_cb;
            if (!ImGui_ImplDX12_Init(&info))
            {
                mm::log(L"ImGui_ImplDX12_Init failed");
                ImGui_ImplWin32_Shutdown();
                g_failed = true;
                return false;
            }

            // WHICH DEVICES THE GAME READS THROUGH WM_INPUT. This decides whether
            // swallowing a key as a window message is enough: a game that registers a
            // raw KEYBOARD also has to have the raw packet filtered (which the panel's
            // Esc path does), and one that registers only the mouse does not. It is one
            // call, once, and it turns "does UE read Esc through raw input?" from a
            // guess into a log line.
            {
                UINT count = 0;
                if (::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == 0 &&
                    count > 0 && count < 64)
                {
                    std::vector<RAWINPUTDEVICE> devs(count);
                    if (::GetRegisteredRawInputDevices(devs.data(), &count, sizeof(RAWINPUTDEVICE)) !=
                        static_cast<UINT>(-1))
                    {
                        std::wstring list;
                        for (UINT i = 0; i < count && i < devs.size(); ++i)
                        {
                            list += std::format(L"{}usage {:#x}/{:#x} flags {:#x}",
                                                list.empty() ? L"" : L", ",
                                                devs[i].usUsagePage,
                                                devs[i].usUsage,
                                                devs[i].dwFlags);
                        }
                        mm::logf(L"raw input: the game has {} device(s) registered ({}). Usage 1/6 is a "
                                 L"KEYBOARD - if it is in that list, keys reach the game through WM_INPUT "
                                 L"as well as WM_KEYDOWN.",
                                 count,
                                 list);
                    }
                }
                else
                {
                    mm::log(L"raw input: the game has no raw-input devices registered - every key and "
                            L"mouse move reaches it as a window message only");
                }
            }

            hook_wndproc();

            g_imgui_frames_in_flight = static_cast<int>(g_buffer_count);
            g_imgui_ready = true;
            crumb::stage(crumb::kImGuiUp);
            mm::logf(L"ImGui {} initialised on the game's swapchain: device {:p}, queue {:p}, {} frames in flight, "
                     L"RTV {}",
                     std::wstring(IMGUI_VERSION, IMGUI_VERSION + std::strlen(IMGUI_VERSION)),
                     static_cast<void*>(g_device),
                     static_cast<void*>(queue),
                     g_buffer_count,
                     format_name(g_format));
            slice_selftest();
            return true;
        }


        // NEVER GUESSES. This used to fall back to a rotating counter when
        // IDXGISwapChain3 was unavailable, which is worse than doing nothing: the index
        // decides which resource the PRESENT -> RENDER_TARGET barrier is issued on, and
        // a barrier declaring the wrong before-state on a resource that is not in it is
        // a device-removal-class error (and, with the wrong RTV, a frame drawn into the
        // buffer the display is scanning out). The interface is QI'd once at adoption
        // and cached; if it is not there, the caller skips the frame.
        UINT current_backbuffer_index(IDXGISwapChain* swapchain)
        {
            if (g_sc3 == nullptr)
            {
                if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&g_sc3))) || g_sc3 == nullptr)
                {
                    g_sc3 = nullptr;
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        mm::log(L"this swapchain does not expose IDXGISwapChain3, so the back-buffer "
                                L"index cannot be known - the overlay will not draw on it (guessing the "
                                L"index would put a resource barrier on the wrong buffer)");
                    }
                    return kNoBackbuffer;
                }
            }
            const UINT index = g_sc3->GetCurrentBackBufferIndex();
            return index < g_buffer_count ? index : kNoBackbuffer;
        }

        // A D3D12 swapchain hands out ID3D12Resource back buffers; a D3D11 one, or a
        // wrapper that does not forward, does not. That is the cheapest reliable test.
        bool is_d3d12_swapchain(IDXGISwapChain* sc)
        {
            ID3D12Resource* buffer = nullptr;
            const HRESULT hr = sc->GetBuffer(0, IID_PPV_ARGS(&buffer));
            safe_release(buffer);
            return SUCCEEDED(hr);
        }

        void log_candidate(IDXGISwapChain* sc, bool d3d12)
        {
            // One line per distinct swapchain, not per Present: the game's decoy 144x8
            // D3D11 swapchain presents every frame.
            static IDXGISwapChain* seen[8]{};
            for (IDXGISwapChain* s : seen)
            {
                if (s == sc)
                {
                    return;
                }
            }
            if (g_candidates_logged >= static_cast<int>(std::size(seen)))
            {
                return;
            }
            seen[g_candidates_logged] = sc;
            ++g_candidates_logged;
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            void** vtable = *reinterpret_cast<void***>(sc);
            mm::logf(L"Present candidate {:p}: {}x{} {} x{} buffers, hwnd 0x{:X}, vtable[8] {} -> {}",
                     static_cast<void*>(sc),
                     desc.BufferDesc.Width,
                     desc.BufferDesc.Height,
                     format_name(desc.BufferDesc.Format),
                     desc.BufferCount,
                     reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                     module_of(vtable[8]),
                     d3d12 ? L"D3D12, taking it" : L"not D3D12, ignored");
        }

        //==============================================================================
        // Render-side teardown for the master switch (RENDER THREAD ONLY)
        //==============================================================================
        //
        // Everything below is a D3D12 object or an ImGui context, and both may only be
        // touched from the thread that created them - which is whichever thread calls
        // Present. So the loop thread clears mm::g_mod_active and this runs inside the
        // next Present, before the hooks are taken out.
        //
        // It leaves the module in exactly the state it had before the first frame:
        // `start()` re-enables the hooks, ExecuteCommandLists re-captures the queue and
        // ensure_initialised() builds everything again.
        //
        // TWO CALLERS, ONE BODY. The master switch (shutdown_render) and a lost device /
        // replaced swapchain (the re-adoption path in render()) release exactly the same
        // objects; the only difference is that the master switch also answers the loop
        // thread's handshake with `g_render_stopped`, which a re-adoption must NOT touch
        // or the loop thread would believe a disable it never asked for had completed.

        void release_device_objects()
        {
            if (g_imgui_ready || g_device != nullptr)
            {
                crumb::stage(crumb::kTeardownBegin);
                // The slicer writes into mapped upload heaps we are about to release.
                // It is a few milliseconds of arithmetic and it re-checks the pause flag
                // on entry, so this always succeeds; if it somehow did not we would
                // rather leak the buffers than free memory under a live writer.
                const bool paused = slicer_pause_begin(1000);
                wait_for_gpu();
                if (!paused)
                {
                    mm::log(L"slice: the loop-thread slicer did not stand down in 1 s - "
                            L"the slice buffers are left allocated on purpose");
                }
                else
                {
                    destroy_all_map_textures();
                }
                slicer_pause_end();
                if (g_imgui_ready)
                {
                    unhook_wndproc();
                    ImGui_ImplDX12_Shutdown();
                    ImGui_ImplWin32_Shutdown();
                    ImGui::DestroyContext();
                    g_imgui_ready = false;
                    // The style went with the context: the next init must rebuild it.
                    g_ui_scale_applied = 0.0f;
                }
                release_render_targets();
                for (UINT i = 0; i < kMaxBuffers; ++i)
                {
                    safe_release(g_frames[i].allocator);
                    g_frames[i].fence_value = 0;
                }
                safe_release(g_cmd_list);
                safe_release(g_fence);
                if (g_fence_event != nullptr)
                {
                    ::CloseHandle(g_fence_event);
                    g_fence_event = nullptr;
                }
                g_fence_value = 0;
                g_srv_heap.destroy();
                safe_release(g_device);
                g_imgui_frames_in_flight = 0;
                mm::log(L"the render thread has released ImGui, the descriptor heaps, "
                        L"the slice buffers and the map textures");
            }
            g_swapchain = nullptr;
            // The cached IDXGISwapChain3 holds a reference on the swapchain we are
            // letting go of; keeping it would pin a dead object and, worse, answer
            // GetCurrentBackBufferIndex for a swapchain we no longer draw on.
            safe_release(g_sc3);
            g_candidates_logged = 0;
            // The queue was captured with a reference held (see hk_ExecuteCommandLists),
            // so dropping it means releasing it.
            safe_release_queue();
            g_failed = false;
            // Everything a re-adoption would have released is gone already.
            g_readopt.store(false, std::memory_order_release);
            crumb::stage(crumb::kTeardownEnd);
        }

        void shutdown_render()
        {
            release_device_objects();
            // THE MASTER SWITCH'S HANDSHAKE, and the one thing a re-adoption must not do.
            g_render_stopped.store(true, std::memory_order_release);
        }

        void render(IDXGISwapChain* swapchain)
        {
            // THE MASTER SWITCH, first statement. One relaxed atomic load per Present
            // while the mod is off - and the one Present that first sees it off does the
            // teardown, because this is the only thread allowed to.
            if (!mm::mod_active())
            {
                if (!g_render_stopped.load(std::memory_order_acquire))
                {
                    spin::SpinGuard guard(g_render_lock);
                    shutdown_render();
                }
                return;
            }

            g_present_count.fetch_add(1, std::memory_order_relaxed);
            g_render_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
            g_render_stage.store("prologue", std::memory_order_relaxed);
            if (g_failed || g_queue.load(std::memory_order_acquire) == nullptr)
            {
                return;
            }

            g_render_stage.store("waiting for the render lock", std::memory_order_relaxed);
            spin::SpinGuard guard(g_render_lock);
            g_render_stage.store("holding the render lock", std::memory_order_relaxed);

            // RE-ADOPTION, and it happens here because this is the only thread that may
            // touch a D3D12 object. Whatever asked for it (a removed device, a swapchain
            // that stopped handing out buffers, a queue from the wrong device) has left
            // this module holding objects that belong to something that no longer
            // exists; releasing them and starting over is what lets the overlay come
            // back by itself after a driver reset or a swapchain swap. `g_failed` is
            // deliberately NOT set: this path is recoverable, that flag is not.
            if (g_readopt.exchange(false, std::memory_order_acquire))
            {
                g_render_stage.store("re-adopting the swapchain", std::memory_order_relaxed);
                mm::perf_note_stall(L"a swapchain / device re-adoption", 2000);
                release_device_objects();
                return; // the next Present adopts whatever is there now
            }

            if (g_swapchain == nullptr)
            {
                const bool d3d12 = is_d3d12_swapchain(swapchain);
                log_candidate(swapchain, d3d12);
                if (!d3d12)
                {
                    return;
                }
                g_swapchain = swapchain;
                crumb::stage(crumb::kSwapchainChosen);
            }
            else if (swapchain != g_swapchain)
            {
                return; // another swapchain (frame generation / ReShade) - not ours
            }

            if (!ensure_initialised(swapchain))
            {
                return;
            }
            if (!g_rt_ready && !create_render_targets(swapchain))
            {
                // A GetBuffer / RTV failure is recoverable (a resize in flight, a device
                // that has gone): hand it to the re-adoption path instead of retrying
                // the same objects every frame for ever.
                request_readoption(L"the render targets could not be rebuilt");
                return;
            }
            // AFTER a resize that raised BufferCount, the ImGui backend still has one
            // set of per-frame buffers per OLD frame in flight and would reuse the
            // vertex, index and descriptor storage of a frame the GPU has not finished.
            // Re-initialising the DX12 backend is the only way to change that count; it
            // happens outside a frame (before NewFrame) and with the GPU idle.
            if (g_imgui_ready && g_imgui_frames_in_flight != 0 &&
                g_imgui_frames_in_flight < static_cast<int>(g_buffer_count))
            {
                mm::logf(L"the swapchain now has {} buffers, ImGui was initialised for {} frames in "
                         L"flight - re-initialising the DX12 backend",
                         g_buffer_count,
                         g_imgui_frames_in_flight);
                wait_for_gpu();
                ImGui_ImplDX12_Shutdown();
                ImGui_ImplDX12_InitInfo info{};
                info.Device = g_device;
                info.CommandQueue = g_queue.load(std::memory_order_acquire);
                info.NumFramesInFlight = static_cast<int>(g_buffer_count);
                info.RTVFormat = g_format;
                info.DSVFormat = DXGI_FORMAT_UNKNOWN;
                info.SrvDescriptorHeap = g_srv_heap.heap();
                info.SrvDescriptorAllocFn = &srv_alloc_cb;
                info.SrvDescriptorFreeFn = &srv_free_cb;
                if (!ImGui_ImplDX12_Init(&info))
                {
                    mm::log(L"ImGui_ImplDX12_Init failed on the re-init after a resize - overlay off");
                    g_failed = true;
                    return;
                }
                g_imgui_frames_in_flight = static_cast<int>(g_buffer_count);
            }

            const UINT index = current_backbuffer_index(swapchain);
            if (index == kNoBackbuffer)
            {
                // No index, no frame. Drawing without knowing which buffer is next means
                // barriering the wrong resource - see current_backbuffer_index.
                return;
            }
            FrameCtx& frame = g_frames[index];
            if (frame.allocator == nullptr)
            {
                // Cannot happen now that the allocators are grown with BufferCount; it
                // stays as the cheap guard that turns the old null-Reset() crash into a
                // dropped frame.
                request_readoption(L"a back buffer has no command allocator");
                return;
            }
            g_render_stage.store("waiting for this frame's fence", std::memory_order_relaxed);
            if (frame.fence_value != 0 && g_fence->GetCompletedValue() < frame.fence_value)
            {
                if (SUCCEEDED(g_fence->SetEventOnCompletion(frame.fence_value, g_fence_event)))
                {
                    ::WaitForSingleObject(g_fence_event, 500);
                }
            }

            // A reload (F5) rebuilds the whole texture set, so the old textures go
            // first - and they may only be released here, on the render thread, and
            // BEFORE the frame's draw lists are built, or this frame would reference
            // an SRV slot we just handed back.
            if (g_drop_textures.load(std::memory_order_acquire))
            {
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    g_drop_textures.store(false, std::memory_order_release);
                    // A full GPU flush plus ~340 MB of releases: a one-off, and one that
                    // must not become the peak every later frame is judged against.
                    mm::perf_note_stall(L"a map texture reload (F5)", 2000);
                    wait_for_gpu();
                    destroy_all_map_textures();
                    slicer_pause_end();
                    mm::log(L"map textures and slice buffers dropped for a reload");
                }
                // else: the request stays pending and the next frame retries. Never
                // release a resource the loop thread may still be writing into.
            }
            release_finished_uploads();

            if (g_pf_frame < 0)
            {
                g_pf_frame = mm::perf_register("render frame (ImGui)", perf::Thread::Render);
            }
            const std::uint64_t frame_t0 = mm::qpc_us();
            // The UI scale, decided from the CURRENT back buffer and applied before the
            // frame's draw lists exist. ResizeBuffers changes g_height, and a config
            // change comes through cfg_cached, so both re-enter here on their own.
            apply_ui_scale(wanted_ui_scale(mm::cfg_cached(), static_cast<float>(g_height)));
            // TWO SUB-COUNTERS, because the 358 ms peak this row showed after 30 minutes
            // had to be attributed to one side or the other. ImGui_ImplWin32_NewFrame is
            // the suspect: it reads and writes the CURSOR and the client rect of a window
            // owned by the GAME thread, and a cross-thread user32 call blocks until that
            // thread pumps messages - which it does not do while it is inside a
            // synchronous level load. build_ui() is our own drawing and touches no OS
            // handle at all. Whichever one carries the peak, the table now says so.
            if (g_pf_newframe < 0)
            {
                g_pf_newframe = mm::perf_register("render NewFrame (win32)", perf::Thread::Render);
                g_pf_buildui = mm::perf_register("render build_ui", perf::Thread::Render);
            }
            {
                const mm::PerfScope nf(g_pf_newframe);
                // The game thread's window messages, in order, on the one thread that
                // is allowed to touch the ImGui context.
                g_render_stage.store("imgui: replaying window messages", std::memory_order_relaxed);
                replay_imgui_messages();
                // CROSS-THREAD USER32, and the render lock is held across it. Everything
                // in here reads or writes the cursor and the client rect of a window
                // owned by the GAME thread, so it is the one place in the frame that can
                // wait on another thread - which is why `hk_ResizeBuffers` (the only
                // other taker of that lock, and a call that can arrive on the game
                // thread) acquires it with a bound instead of spinning for ever.
                g_render_stage.store("imgui: ImplWin32_NewFrame (user32)", std::memory_order_relaxed);
                ImGui_ImplWin32_NewFrame();
            }
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            {
                const mm::PerfScope bu(g_pf_buildui);
                g_render_stage.store("build_ui", std::memory_order_relaxed);
                build_ui();
            }
            ImGui::Render();
            // The game thread's swallow decision reads this instead of the context.
            g_imgui_want_keyboard.store(ImGui::GetIO().WantCaptureKeyboard, std::memory_order_relaxed);
            mm::perf_record(g_pf_frame, frame_t0);

            if (FAILED(frame.allocator->Reset()) || FAILED(g_cmd_list->Reset(frame.allocator, nullptr)))
            {
                return;
            }

            // One image per frame: a nine-layer chapter is resident after ~9 frames
            // instead of stalling a single one with ~100 MB of copies.
            std::unique_ptr<mapdata::PendingImage> pending = mapdata::take_pending();
            if (pending != nullptr)
            {
                begin_map_upload(*pending, g_cmd_list);
            }

            // The height-slice window the CPU filled during build_ui(). Recorded here,
            // i.e. BEFORE ImGui's draw call in the same command list, so the GPU sees
            // the copy complete before it samples the texture - no extra queue, no
            // second submission and no PSO of our own.
            record_slice_copy(g_cmd_list);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = g_backbuffers[index];
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_cmd_list->ResourceBarrier(1, &barrier);

            g_cmd_list->OMSetRenderTargets(1, &g_rtv[index], FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = {g_srv_heap.heap()};
            g_cmd_list->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list);

            // The screenshot copy, if one was asked for: it takes the back buffer from
            // RENDER_TARGET to PRESENT itself (via COPY_SOURCE), so it REPLACES the
            // closing barrier below rather than adding to it.
            const bool shot_recorded = record_shot_copy(g_cmd_list, g_backbuffers[index], index);
            if (!shot_recorded)
            {
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                g_cmd_list->ResourceBarrier(1, &barrier);
            }
            g_cmd_list->Close();

            g_render_stage.store("submitting the command list", std::memory_order_relaxed);
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            ID3D12CommandList* lists[] = {g_cmd_list};
            // The ORIGINAL, so our own submission does not re-enter the hook.
            o_ExecuteCommandLists(queue, 1, lists);
            ++g_fence_value;
            queue->Signal(g_fence, g_fence_value);
            frame.fence_value = g_fence_value;
            if (shot_recorded)
            {
                g_shot_fence = g_fence_value; // the readback may not be mapped before this
            }
            // The buffer this frame sampled may not be rewritten until the GPU is past
            // this fence.
            {
                const int shown = slice_view().shown;
                if (shown >= 0)
                {
                    g_slice_in_flight[shown].store(g_fence_value, std::memory_order_release);
                }
            }
            {
                const int shown = map_slice_view().shown;
                if (shown >= 0)
                {
                    g_mslice_in_flight[shown].store(g_fence_value, std::memory_order_release);
                }
            }

            if (g_map.upload != nullptr && g_map.upload_fence == 0)
            {
                g_map.upload_fence = g_fence_value;
            }
            else if (g_map.upload != nullptr && g_fence->GetCompletedValue() >= g_map.upload_fence)
            {
                safe_release(g_map.upload); // the copy has landed; give the 90 MB back
            }
            g_render_stage.store("between frames", std::memory_order_relaxed);
        }

        //==============================================================================
        // Hooks
        //==============================================================================

        // THE EXCEPTION BARRIER.
        //
        // Everything our frame does - std::format for a panel line, a vector that grows
        // while markers are collected, the ImGui context itself - can throw
        // std::bad_alloc, and a throw here would unwind THROUGH the MinHook trampoline
        // and into DXGI, i.e. through frames that were compiled with no idea our code
        // exists. That is a hard crash inside the graphics driver stack with a call
        // stack that names dxgi.dll and not this mod, which is the worst possible
        // diagnostic for the player who has to report it.
        //
        // So the whole frame is wrapped, once, at the hook boundary: a throw becomes one
        // log line and a dead overlay, and the game keeps presenting.
        //
        // NOT SEH. An access violation is deliberately left to crash: __try cannot live
        // in a function that needs C++ unwinding (MSVC C2712, lessons.md), so it would
        // take a second POD-only trampoline - and swallowing an AV in the middle of our
        // command-list recording would leave the list open and the back buffer stranded
        // between resource states, which the next frames turn into a device removal with
        // no evidence left. The crash breadcrumb plus CrashContext.runtime-xml is the
        // route that has actually diagnosed every fault in this project so far.
        void render_guarded(IDXGISwapChain* sc)
        {
            try
            {
                render(sc);
            }
            catch (const std::exception& e)
            {
                if (!g_failed.exchange(true))
                {
                    const char* what = e.what() != nullptr ? e.what() : "?";
                    mm::logf(L"an exception escaped the overlay's frame ({}) - the overlay is off for "
                             L"the rest of this session; the game is unaffected",
                             stage_w(what));
                }
            }
            catch (...)
            {
                if (!g_failed.exchange(true))
                {
                    mm::log(L"a non-standard exception escaped the overlay's frame - the overlay is off "
                            L"for the rest of this session; the game is unaffected");
                }
            }
        }

        // The screenshot readback: mapped, unpacked and turned into a DIB OUTSIDE the
        // render lock. It used to run inside render(), which holds that lock across the
        // whole frame - and `hk_ResizeBuffers` waits on the same lock from the GAME
        // thread, so a full-canvas unpack (two allocations plus a per-row conversion of
        // up to ~8 MB) sat directly in front of a resize. Same thread, same point in the
        // frame, no lock held: the readback resource and the fence are render-thread-only
        // objects and this is the render thread.
        void collect_guarded()
        {
            try
            {
                shot_collect();
            }
            catch (...)
            {
                mm::log(L"the screenshot readback threw - the map copy is dropped");
            }
        }

        // DID THE PRESENT ITSELF FAIL? The HRESULT used to be discarded, which is how a
        // TDR or a driver reset left the overlay silently dead for the rest of the
        // session: every later frame drew into resources belonging to a device that no
        // longer exists. Both removal codes mean "everything we hold is gone", so the
        // next Present starts over from an empty state.
        void note_present_result(HRESULT hr)
        {
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                request_readoption(hr == DXGI_ERROR_DEVICE_REMOVED ? L"Present returned DEVICE_REMOVED"
                                                                   : L"Present returned DEVICE_RESET");
            }
        }

        HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            // THE ORIGINAL IS ALWAYS CALLED, for every swapchain in the process - see
            // the comment on this hook's install: Present is one dxgi function shared by
            // every swapchain, D3D11 and D3D12 alike, and returning early for "not ours"
            // would stop somebody else's overlay from presenting at all.
            const HRESULT hr = o_Present(sc, sync, flags);
            if (sc == g_swapchain)
            {
                note_present_result(hr);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
        {
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            const HRESULT hr = o_Present1(sc, sync, flags, params);
            if (sc == g_swapchain)
            {
                note_present_result(hr);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                                   UINT flags)
        {
            if (!mm::mod_active())
            {
                return o_ResizeBuffers(sc, count, w, h, format, flags);
            }
            g_resize_count.fetch_add(1, std::memory_order_relaxed);
            // A resize means a device-level stall (a resolution or fullscreen change, or
            // the tail of a level load): the frames around it are wall-clock waits, not
            // this mod's cost, so they go to the stall columns of the F2 table.
            mm::perf_note_stall(L"a swapchain resize", 2000);
            // A BOUNDED ACQUIRE, NOT A SPIN. This call can arrive on the game thread,
            // and the render thread holds this same lock across
            // `ImGui_ImplWin32_NewFrame()`, which reads and writes the cursor and the
            // client rect of a window the GAME thread owns. Spinning here for ever is
            // therefore a two-thread deadlock with no diagnostic; a bound turns the
            // worst case into a named log line and a resize that may fail, which the
            // next Present recovers from by recreating its render targets.
            if (g_render_lock.try_lock_ms(2000))
            {
                // OURS OR NOT, TESTED BEFORE THE LINE IS FORMATTED. Every swapchain in
                // the process comes through this one function, and formatting a log line
                // for each of them (the game presents a decoy 144x8 D3D11 swapchain too)
                // put a wstring allocation and a log write in front of resizes that have
                // nothing to do with this mod.
                if (sc == g_swapchain)
                {
                    mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}) - releasing render targets",
                             count,
                             w,
                             h,
                             format_name(format));
                    // A throw in here would unwind into DXGI through the trampoline, the
                    // same hazard the Present barrier exists for - and this one runs on
                    // the GAME thread, where it would take the game down with it.
                    try
                    {
                        if (g_imgui_ready)
                        {
                            wait_for_gpu();
                        }
                        release_render_targets();
                    }
                    catch (...)
                    {
                        mm::log(L"an exception escaped the ResizeBuffers path - the overlay is off for "
                                L"the rest of this session; the resize itself still happens");
                        g_failed = true;
                    }
                }
                g_render_lock.unlock();
            }
            else if (sc == g_swapchain)
            {
                mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}): the render lock was still held after "
                         L"2000 ms, so our render targets were NOT released. The resize may fail and "
                         L"the next Present will rebuild them. Render thread {} was at '{}'.",
                         count,
                         w,
                         h,
                         format_name(format),
                         g_render_tid.load(std::memory_order_relaxed),
                         stage_w(g_render_stage.load(std::memory_order_relaxed)));
            }
            const HRESULT hr = o_ResizeBuffers(sc, count, w, h, format, flags);
            // The RTVs are recreated lazily on the next Present, once the swapchain has
            // its new buffers.
            if (FAILED(hr))
            {
                mm::logf(L"ResizeBuffers failed (0x{:08X})", static_cast<unsigned>(hr));
            }
            return hr;
        }

        void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                      ID3D12CommandList* const* lists)
        {
            if (mm::mod_active() && g_queue.load(std::memory_order_relaxed) == nullptr && queue != nullptr &&
                queue != g_bad_queue.load(std::memory_order_acquire))
            {
                const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
                if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    ID3D12CommandQueue* expected = nullptr;
                    if (g_queue.compare_exchange_strong(expected, queue))
                    {
                        // A REFERENCE IS HELD FROM HERE UNTIL THE TEARDOWN RELEASES IT.
                        // The pointer was borrowed before: the queue is the game's, and
                        // a game that destroys it (a device reset, a renderer swap) left
                        // this module submitting command lists to freed memory. One
                        // AddRef costs nothing and makes the pointer valid for as long
                        // as we hold it.
                        queue->AddRef();
                        mm::logf(L"captured the game's DIRECT command queue {:p} (priority {}, flags {})",
                                 static_cast<void*>(queue),
                                 desc.Priority,
                                 static_cast<unsigned>(desc.Flags));
                        // WHICH QUEUE, AND WHY IT MAY BE THE WRONG ONE. There is no way
                        // to ask this game's swapchain which queue presented it: it is a
                        // ReShade wrapper and IDXGISwapChain::GetDevice does not even
                        // forward, so "the queue that executed last before Present on
                        // our swapchain" is not derivable here - ExecuteCommandLists is
                        // hooked process-wide and every renderer in the process (the
                        // game, ReShade's own effects, a frame-generation runtime) uses
                        // it. What IS checkable is the device: create_render_targets
                        // compares the back buffer's ID3D12Device with the one this
                        // queue hands out and rejects the queue on a mismatch, which is
                        // the failure that would actually matter.
                    }
                }
            }
            o_ExecuteCommandLists(queue, count, lists);
        }

        //==============================================================================
        // Hook installation via a throwaway device + swapchain
        //==============================================================================

        //==============================================================================
        // THE HOOK-ADDRESS CACHE (and why the Steam overlay wants it)
        //==============================================================================
        //
        // Discovery creates a throwaway D3D12 device, a DIRECT command queue and a 64x64
        // swapchain on a hidden window, reads four vtable slots and destroys all three.
        // That is the hudhook recipe and it is what found the addresses in the first
        // place - but Steam's GameOverlayRenderer64 hooks the device-, queue- and
        // swapchain-creating entry points and re-targets its overlay onto what it sees
        // created. Ours are created LATER than the game's (this runs from
        // on_unreal_init, long after RHI init) and are then destroyed, which is a
        // textbook way to leave the Steam overlay pointed at a dead object - the
        // reported symptom, "the Steam FPS counter stopped rendering with the mod".
        //
        // The addresses, though, are a property of the DLL and not of the session: the
        // first launch writes them down as module + RVA, and every launch after that
        // hooks them directly and creates NOTHING. The cache is keyed to the module's
        // SizeOfImage, TimeDateStamp and CheckSum - all three baked into the file - so a
        // ReShade, driver or Windows update invalidates it and discovery runs once more.
        // If cached addresses ever produce no Present at all, the watchdog deletes the
        // file, so a stale cache costs one launch and heals itself.


        std::wstring hook_cache_path()
        {
            return mm::mod_dir() + L"\\wuchang_minimap_hookaddr.txt";
        }

        bool write_hook_cache(const ModuleId* ids)
        {
            std::wstring text = L"; WuchangMinimap - the DX12 hook addresses found on a previous launch.\n"
                                L"; Deleting this file forces a fresh discovery; it is rewritten by itself\n"
                                L"; whenever one of these modules changes. schema 1\n"
                                L"; <what> = <module> <rva> <SizeOfImage> <TimeDateStamp> <CheckSum>\n";
            for (int i = 0; i < kHookCount; ++i)
            {
                if (ids[i].name[0] == L'\0' || ids[i].rva == 0)
                {
                    return false;
                }
                text += std::format(L"{} = {} 0x{:X} 0x{:X} 0x{:08X} 0x{:08X}\n",
                                    kHookNames[i],
                                    ids[i].name,
                                    ids[i].rva,
                                    ids[i].size,
                                    ids[i].stamp,
                                    ids[i].sum);
            }
            HANDLE h = ::CreateFileW(hook_cache_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            std::string narrow;
            narrow.reserve(text.size());
            for (const wchar_t c : text)
            {
                narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            DWORD wrote = 0;
            const bool ok =
                ::WriteFile(h, narrow.data(), static_cast<DWORD>(narrow.size()), &wrote, nullptr) != 0;
            ::CloseHandle(h);
            return ok;
        }

        // A hex field ("0x1F" or "1F"). False on anything else, so a hand-edited or
        // truncated file is refused rather than half-read.
        bool parse_hex_field(std::string_view t, std::uint64_t& out)
        {
            if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
            {
                t.remove_prefix(2);
            }
            if (t.empty() || t.size() > 16)
            {
                return false;
            }
            std::uint64_t v = 0;
            for (const char c : t)
            {
                int d = -1;
                if (c >= '0' && c <= '9')
                {
                    d = c - '0';
                }
                else if (c >= 'a' && c <= 'f')
                {
                    d = c - 'a' + 10;
                }
                else if (c >= 'A' && c <= 'F')
                {
                    d = c - 'A' + 10;
                }
                if (d < 0)
                {
                    return false;
                }
                v = v * 16 + static_cast<std::uint64_t>(d);
            }
            out = v;
            return true;
        }

        // Resolves every entry against the module loaded RIGHT NOW. Any mismatch refuses
        // the WHOLE cache: a half-valid one would hook an address inside the wrong DLL,
        // which is a crash rather than a missing overlay.
        bool read_hook_cache(void** addr)
        {
            HANDLE h = ::CreateFileW(hook_cache_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            char buf[4096]{};
            DWORD got = 0;
            const bool read_ok = ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) != 0;
            ::CloseHandle(h);
            if (!read_ok || got == 0)
            {
                return false;
            }
            const std::string text{buf, buf + got};

            for (int i = 0; i < kHookCount; ++i)
            {
                addr[i] = nullptr;
            }
            int found = 0;
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t nl = text.find('\n', at);
                if (nl == std::string::npos)
                {
                    nl = text.size();
                }
                std::string_view line{text.data() + at, nl - at};
                at = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                {
                    line.remove_suffix(1);
                }
                if (line.empty() || line.front() == ';')
                {
                    continue;
                }
                const std::size_t eq = line.find(" = ");
                if (eq == std::string_view::npos)
                {
                    continue;
                }
                const std::string_view key = line.substr(0, eq);
                std::string_view rest = line.substr(eq + 3);

                int slot = -1;
                for (int i = 0; i < kHookCount; ++i)
                {
                    std::string want;
                    for (const wchar_t* p = kHookNames[i]; *p != L'\0'; ++p)
                    {
                        want.push_back(static_cast<char>(*p));
                    }
                    if (key == want)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0)
                {
                    continue;
                }

                std::string_view field[5];
                int n = 0;
                while (n < 5 && !rest.empty())
                {
                    const std::size_t sp = rest.find(' ');
                    field[n++] = rest.substr(0, sp);
                    rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
                }
                std::uint64_t rva = 0;
                std::uint64_t size = 0;
                std::uint64_t stamp = 0;
                std::uint64_t sum = 0;
                if (n != 5 || field[0].empty() || field[0].size() + 1 >= 64 ||
                    !parse_hex_field(field[1], rva) || !parse_hex_field(field[2], size) ||
                    !parse_hex_field(field[3], stamp) || !parse_hex_field(field[4], sum))
                {
                    mm::log(L"hook cache: a malformed line - falling back to discovery");
                    return false;
                }

                wchar_t wname[64]{};
                for (std::size_t k = 0; k < field[0].size(); ++k)
                {
                    wname[k] = static_cast<wchar_t>(field[0][k]);
                }
                const HMODULE mod = ::GetModuleHandleW(wname);
                ModuleId live{};
                if (mod == nullptr || !module_identity(mod, live))
                {
                    mm::logf(L"hook cache: '{}' is not loaded - falling back to discovery", wname);
                    return false;
                }
                if (live.size != static_cast<std::uint32_t>(size) ||
                    live.stamp != static_cast<std::uint32_t>(stamp) ||
                    live.sum != static_cast<std::uint32_t>(sum))
                {
                    mm::logf(L"hook cache: {} is a different build now (size 0x{:X} vs 0x{:X}, stamp "
                             L"0x{:08X} vs 0x{:08X}, sum 0x{:08X} vs 0x{:08X}) - falling back to discovery",
                             wname,
                             live.size,
                             static_cast<std::uint32_t>(size),
                             live.stamp,
                             static_cast<std::uint32_t>(stamp),
                             live.sum,
                             static_cast<std::uint32_t>(sum));
                    return false;
                }
                if (rva == 0 || rva >= live.size)
                {
                    return false;
                }
                addr[slot] = reinterpret_cast<std::uint8_t*>(mod) + rva;
                ++found;
            }
            if (found != kHookCount)
            {
                mm::logf(L"hook cache: {} of {} entries resolved - falling back to discovery",
                         found,
                         kHookCount);
                return false;
            }
            return true;
        }

        void delete_hook_cache(const wchar_t* why)
        {
            if (::DeleteFileW(hook_cache_path().c_str()) != 0)
            {
                mm::logf(L"hook cache: deleted ({}). The next launch rediscovers the addresses.", why);
            }
        }

        // Both routes end here: four MH_CreateHook calls, one MH_EnableHook, one report.
        bool create_and_enable(void** addr, const wchar_t* how)
        {
            const MH_STATUS s1 = MH_CreateHook(addr[0], reinterpret_cast<void*>(&hk_Present),
                                               reinterpret_cast<void**>(&o_Present));
            const MH_STATUS s2 = MH_CreateHook(addr[1], reinterpret_cast<void*>(&hk_ResizeBuffers),
                                               reinterpret_cast<void**>(&o_ResizeBuffers));
            const MH_STATUS s3 = MH_CreateHook(addr[2], reinterpret_cast<void*>(&hk_Present1),
                                               reinterpret_cast<void**>(&o_Present1));
            const MH_STATUS s4 = MH_CreateHook(addr[3], reinterpret_cast<void*>(&hk_ExecuteCommandLists),
                                               reinterpret_cast<void**>(&o_ExecuteCommandLists));

            // THE STATUSES ARE CHECKED BEFORE ANYTHING IS ENABLED. MH_EnableHook used to
            // run first, so a partial install (Present created, ExecuteCommandLists not)
            // left LIVE trampolines behind while this function reported failure - and
            // `g_hooks_created` then stayed false, so the master switch's re-enable took
            // the "install from scratch" branch and ran the dummy-device discovery
            // again on top of hooks that were already in place. Either both required
            // hooks exist or nothing of ours is installed at all.
            const MH_STATUS created[kHookCount] = {s1, s2, s3, s4};
            const bool required_ok = s1 == MH_OK && s4 == MH_OK;
            MH_STATUS en = MH_ERROR_NOT_CREATED;
            if (required_ok)
            {
                en = MH_EnableHook(MH_ALL_HOOKS);
            }
            if (!required_ok || en != MH_OK)
            {
                for (int i = 0; i < kHookCount; ++i)
                {
                    if (created[i] == MH_OK)
                    {
                        MH_DisableHook(addr[i]);
                        MH_RemoveHook(addr[i]);
                    }
                }
                mm::log(L"hooks: the required pair (Present, ExecuteCommandLists) did not install, so "
                        L"every trampoline that HAD been created was removed again - nothing of this "
                        L"mod is in the game's call path");
            }

            g_hook_report = std::format(L"{} | Present {} @ {} | ResizeBuffers {} @ {} | Present1 {} @ {} | "
                                        L"ExecuteCommandLists {} @ {} | enable {}",
                                        how,
                                        static_cast<int>(s1),
                                        module_of(addr[0]),
                                        static_cast<int>(s2),
                                        module_of(addr[1]),
                                        static_cast<int>(s3),
                                        module_of(addr[2]),
                                        static_cast<int>(s4),
                                        module_of(addr[3]),
                                        static_cast<int>(en));
            mm::logf(L"hooks: {}", g_hook_report);
            mm::logf(L"hook addresses: Present {:p}  ResizeBuffers {:p}  Present1 {:p}  ExecuteCommandLists {:p}",
                     addr[0],
                     addr[1],
                     addr[2],
                     addr[3]);
            log_overlay_modules();
            const bool ok = required_ok && en == MH_OK;
            if (!ok)
            {
                mm::log(L"at least one required hook did not install - the overlay will not draw");
            }
            return ok;
        }

        bool install_hooks_from_cache()
        {
            void* addr[kHookCount]{};
            if (!read_hook_cache(addr))
            {
                return false;
            }
            // WHO WAS ALREADY THERE, read before we write a byte.
            for (int i = 0; i < kHookCount; ++i)
            {
                mm::logf(L"hook cache: {} -> {} {}", kHookNames[i], module_of(addr[i]),
                         detour_report(addr[i]));
            }
            mm::log(L"hook cache: the addresses came out of wuchang_minimap_hookaddr.txt, so NO dummy "
                    L"device, queue, swapchain or window was created this launch - the Steam overlay has "
                    L"nothing of ours to re-target onto");
            g_hooks_from_cache = true;
            return create_and_enable(addr, L"from the address cache");
        }

        bool install_hooks_by_discovery()
        {
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                mm::logf(L"MH_Initialize failed: {}",
                         std::wstring(MH_StatusToString(init), MH_StatusToString(init) + std::strlen(MH_StatusToString(init))));
                return false;
            }

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WuchangMinimapDummyWnd";
            ::RegisterClassExW(&wc);
            HWND dummy_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"WuchangMinimap", WS_OVERLAPPEDWINDOW, 0, 0, 64,
                                                64, nullptr, nullptr, wc.hInstance, nullptr);
            if (dummy_hwnd == nullptr)
            {
                mm::log(L"could not create the dummy window for vtable discovery");
                ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
                return false;
            }

            ID3D12Device* device = nullptr;
            ID3D12CommandQueue* queue = nullptr;
            IDXGIFactory2* factory = nullptr;
            IDXGISwapChain1* swapchain = nullptr;
            bool ok = false;

            HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            if (SUCCEEDED(hr))
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
            }
            if (SUCCEEDED(hr))
            {
                hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            }
            if (SUCCEEDED(hr))
            {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = 64;
                sd.Height = 64;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.Scaling = DXGI_SCALING_STRETCH;
                sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                hr = factory->CreateSwapChainForHwnd(queue, dummy_hwnd, &sd, nullptr, nullptr, &swapchain);
            }

            if (SUCCEEDED(hr) && swapchain != nullptr && queue != nullptr)
            {
                void** sc_vtable = *reinterpret_cast<void***>(swapchain);
                void** q_vtable = *reinterpret_cast<void***>(queue);

                void* addr[kHookCount] = {
                    sc_vtable[8],  // IDXGISwapChain::Present
                    sc_vtable[13], // IDXGISwapChain::ResizeBuffers
                    sc_vtable[22], // IDXGISwapChain1::Present1
                    q_vtable[10],  // ID3D12CommandQueue::ExecuteCommandLists
                };

                // WHO WAS ALREADY THERE. Read before we write a byte: a jmp in front of
                // Present names the module that installed it, and MinHook relocates
                // those bytes into our trampoline - which is the proof that the other
                // overlay stays in the chain below us instead of being replaced.
                ModuleId ids[kHookCount]{};
                bool all_identified = true;
                for (int i = 0; i < kHookCount; ++i)
                {
                    mm::logf(L"hook discovery: {} -> {} {}",
                             kHookNames[i],
                             module_of(addr[i]),
                             detour_report(addr[i]));
                    all_identified = module_id_of(addr[i], ids[i]) && all_identified;
                }

                ok = create_and_enable(addr, L"by dummy-swapchain discovery");

                // The addresses belong to the DLLs, so the next launch can hook them
                // without creating (and then destroying) a device, a queue and a
                // swapchain that Steam's overlay may have re-targeted itself onto.
                if (ok && all_identified && write_hook_cache(ids))
                {
                    mm::logf(L"hook cache: written to {} - the next launch hooks these addresses directly "
                             L"and creates no dummy objects at all",
                             hook_cache_path());
                }
            }
            else
            {
                mm::logf(L"dummy device/swapchain creation failed (0x{:08X}) - no hooks installed",
                         static_cast<unsigned>(hr));
            }

            safe_release(swapchain);
            safe_release(factory);
            safe_release(queue);
            safe_release(device);
            ::DestroyWindow(dummy_hwnd);
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

            g_hooks_installed = ok;
            g_hook_install_ms = ::GetTickCount64();
            return ok;
        }

        // THE ONE ENTRY POINT. The cache first (it creates nothing), the dummy-swapchain
        // discovery as the fallback that also refreshes the cache.
        bool install_hooks()
        {
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                mm::logf(L"MH_Initialize failed: {}", static_cast<int>(init));
                return false;
            }
            if (install_hooks_from_cache())
            {
                g_hooks_installed = true;
                g_hook_install_ms = ::GetTickCount64();
                return true;
            }
            g_hooks_from_cache = false;
            return install_hooks_by_discovery();
        }
    } // namespace ovl
} // namespace overlay
