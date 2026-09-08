//
// overlay_d3d12 - the D3D12 device objects, the swapchain hooks and the per-frame
// render entry point.
//
// Everything here runs on the RENDER thread except the slice request the loop thread
// leaves behind: the SRV heap, the map and height-slice textures, the ImGui backends
// and the Present / Present1 / ResizeBuffers hooks.
//
// HOOK STRATEGY (the hudhook approach)
// ------------------------------------
// The game's IDXGISwapChain is not reachable from a UE4SS mod, so:
//
//   1. create a throwaway D3D12 device + DIRECT command queue + a 64x64 swapchain on a
//      hidden window, purely to read the swapchain's vtable;
//   2. MinHook the absolute addresses of IDXGISwapChain::Present (vtable slot 8),
//      ResizeBuffers (13) and IDXGISwapChain1::Present1 (22);
//   3. throw the dummy objects away and wait. The first real Present gives us the
//      swapchain, which the overlay follows for its geometry and its frame tick. What
//      it draws on is a queue and a surface of its own - see overlay_dcomp.cpp.
//
// The dummy objects go through our own import table, i.e. through whatever `dxgi.dll`
// is loaded - on this machine ReShade's proxy, so the dummy swapchain is a ReShade
// wrapper with the same vtable the game holds and we hook the slot the game calls. The
// module owning every hooked address is logged. A watchdog complains when no Present
// arrives within a few seconds: the swapchain is then wrapped by something we did not
// go through (e.g. a DLSS-FG proxy).
//

#include "overlay_internal.hpp"

#include "imgui_caret.hpp"

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
                // Once: a full heap stays full, and this is a render-thread callback.
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

        // RENDER THREAD ONLY, like `g_swapchain` itself.
        //
        // `g_adopted_present_ms` is GetTickCount64() of the last Present on the adopted
        // swapchain; 0 means "no reference point yet" and disables the check below. A
        // game that RECREATES its swapchain (an HDR toggle, a fullscreen mode change)
        // leaves the adopted one presenting nothing while a new one presents every
        // frame, and the plain "not ours" filter would drop the new one for the rest of
        // the session. So a D3D12 swapchain presenting while ours has been silent for
        // kSwapchainSilentMs asks for the ordinary re-adoption.
        //
        // `g_imgui_rtv_format` is the RTV format ImGui's DX12 backend was initialised
        // for. Its pipeline state bakes that format in, so a back-buffer format change
        // needs the backend rebuilt exactly as a raised buffer count does.
        //
        // `g_display_logged` is whether the adapter / output / presenting trio has been
        // written for the current adoption: one set of lines per adopted swapchain, not
        // one per resize.
        static std::uint64_t g_adopted_present_ms = 0;
        static DXGI_FORMAT g_imgui_rtv_format = DXGI_FORMAT_UNKNOWN;
        static bool g_display_logged = false;
        constexpr std::uint64_t kSwapchainSilentMs = 2000;

        //==============================================================================
        // The height-slice texture
        //==============================================================================
        //
        // A small dynamic RGBA8 texture the CPU slicer refills at slice_hz. Two of them,
        // because the GPU may still be sampling one while we write the next:
        // `in_flight_fence` is the fence value of the last frame that drew this buffer,
        // and a buffer is only rewritten once GetCompletedValue() has passed it.
        //
        // The upload heap stays mapped for the buffer's whole life (a 512x512 RGBA
        // window is 1 MB, and Map/Unmap per update is pure overhead), and the copy is
        // recorded on the same command list Present already records for ImGui - so
        // there is no extra queue, no PSO and no root signature on ReShade's swapchain.

        // QueryPerformanceFrequency is constant for the life of the process: asked once,
        // lazily.
        std::int64_t qpc_freq()
        {
            static const std::int64_t freq = [] {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                return static_cast<std::int64_t>(f.QuadPart);
            }();
            return freq;
        }

        //==============================================================================
        // The slicer runs on the LOOP thread
        //==============================================================================
        //
        // `slice_region` is 1-4 ms of pure CPU that writes into a persistently mapped
        // upload heap and needs nothing from Present, so it runs from
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
        //     sampled it has completed (`g_slice_in_flight`);
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

        // Render thread. Stops the slicer and waits for it to leave its critical
        // section. False when it did not stop inside `budget_ms`: the caller must then
        // leave every slice buffer alone and try again next frame.
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
        // The full map
        //==============================================================================
        //
        // The same height-sliced asset the minimap draws, at map scale: north-up,
        // pannable, zoomable, with every marker on it. Its own pair of dynamic textures,
        // because its window is bigger and decimated - one texture pixel covers `step`
        // source pixels - and its own update policy: it re-cuts only when something
        // changed (pan out of the cut region, zoom, floor slice, a big player move),
        // capped at map_slice_hz, where the minimap re-cuts 12 times a second.

        // A colour space is how the swapchain's numbers reach the display, and it is the
        // only honest answer to "is HDR on?" - the game's own menu says what was asked
        // for, not what the display is doing.
        const wchar_t* colour_space_name(DXGI_COLOR_SPACE_TYPE cs)
        {
            switch (cs)
            {
            case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
                return L"sRGB / SDR";
            case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                return L"HDR10 (PQ, Rec.2020)";
            case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
                return L"scRGB (linear, HDR)";
            case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
                return L"HDR10 studio range";
            default:
                return L"another colour space";
            }
        }

        // RENDER THREAD, once per adoption. Three questions a bug report about this mod
        // always needs and can never answer from the outside: what the GPU and its driver
        // are, what the display is doing with the frames, and how the game is presenting
        // them. All of it read from objects this module already holds; nothing is asked of
        // the player and nothing is read out of the game's own settings, which describe
        // what was requested rather than what is happening.
        void log_display_environment(IDXGISwapChain* swapchain)
        {
            if (g_device == nullptr || swapchain == nullptr)
            {
                // Not silent: this used to be called before the device existed, and a
                // guard that returns without a word is how three lines went missing.
                mm::logf(L"  the display environment cannot be read yet (device {:p}, swapchain {:p})",
                         static_cast<void*>(g_device),
                         static_cast<void*>(swapchain));
                return;
            }

            // The adapter, by LUID, because that is the one identity a D3D12 device
            // exposes. CheckInterfaceSupport on IID_IDXGIDevice answers with the
            // user-mode driver's version - the number a driver release is known by.
            IDXGIFactory4* factory = nullptr;
            if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            {
                IDXGIAdapter1* adapter = nullptr;
                if (SUCCEEDED(factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))
                    && adapter != nullptr)
                {
                    DXGI_ADAPTER_DESC1 ad{};
                    LARGE_INTEGER umd{};
                    const bool have_desc = SUCCEEDED(adapter->GetDesc1(&ad));
                    const bool have_umd = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                    mm::logf(L"  adapter \"{}\" vendor 0x{:04X} device 0x{:04X}, {} MB dedicated video "
                             L"memory, user-mode driver {}.{}.{}.{}",
                             have_desc ? ad.Description : L"(unnamed)",
                             have_desc ? ad.VendorId : 0u,
                             have_desc ? ad.DeviceId : 0u,
                             have_desc ? static_cast<std::uint64_t>(ad.DedicatedVideoMemory / (1024 * 1024))
                                       : 0ull,
                             have_umd ? HIWORD(umd.HighPart) : 0,
                             have_umd ? LOWORD(umd.HighPart) : 0,
                             have_umd ? HIWORD(umd.LowPart) : 0,
                             have_umd ? LOWORD(umd.LowPart) : 0);
                    safe_release(adapter);
                }
                safe_release(factory);
            }

            // The output, for the colour space. GetContainingOutput has no answer while
            // the game is windowed on some drivers, which is itself worth saying.
            IDXGIOutput* output = nullptr;
            if (SUCCEEDED(swapchain->GetContainingOutput(&output)) && output != nullptr)
            {
                IDXGIOutput6* out6 = nullptr;
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out6))) && out6 != nullptr)
                {
                    DXGI_OUTPUT_DESC1 od{};
                    if (SUCCEEDED(out6->GetDesc1(&od)))
                    {
                        mm::logf(L"  output \"{}\" {}x{}, {} bit(s) per colour, {}, {} nit peak",
                                 od.DeviceName,
                                 od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                                 od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                                 od.BitsPerColor,
                                 colour_space_name(od.ColorSpace),
                                 static_cast<int>(od.MaxLuminance));
                    }
                    safe_release(out6);
                }
                safe_release(output);
            }
            else
            {
                mm::log(L"  the swapchain reports no containing output, which a windowed game can do");
            }

            // How the game presents: the flip model and whether it is synchronised. The
            // sync interval is the game's own argument to Present, recorded by the hook -
            // 0 means it is not waiting for the display.
            BOOL fullscreen = FALSE;
            const bool have_fs = SUCCEEDED(swapchain->GetFullscreenState(&fullscreen, nullptr));
            const unsigned sync = g_present_sync.load(std::memory_order_relaxed);
            const unsigned flags = g_present_flags.load(std::memory_order_relaxed);
            mm::logf(L"  presenting {}, sync interval {}{}, present flags 0x{:X}{}",
                     !have_fs ? L"in an unknown mode"
                              : (fullscreen ? L"in exclusive fullscreen" : L"windowed or borderless"),
                     sync,
                     sync == 0 ? L" (not waiting for the display)" : L"",
                     flags,
                     (flags & DXGI_PRESENT_ALLOW_TEARING) != 0 ? L" (tearing allowed)" : L"");
        }

        // Streamline's own answer to "is frame generation in this present path". The
        // interposer exports `slIsFeatureLoaded`, which reads state the host has already
        // set up - it creates nothing, proxies nothing and initialises nothing, which is
        // why it is the question asked rather than any of the interfaces NVIDIA asks
        // third parties to leave alone.
        //
        // Loaded is the state that matters, not enabled. DLSS-G takes the present path
        // when it loads and does not give it back when a player switches frame generation
        // off in the menu, which is why a player can truthfully say theirs is off while
        // the back buffers are not the game's.
        //
        // `Unknown` is its own answer and not a synonym for absent: with DLSS-G absent
        // this export does not return false, it returns an error. Nothing may be refused
        // on a question that was never answered.
        enum class FrameGen
        {
            Unknown,
            Absent,
            Loaded
        };

        FrameGen frame_generation_state()
        {
            HMODULE sl = ::GetModuleHandleW(L"sl.interposer.dll");
            if (sl == nullptr)
            {
                return FrameGen::Absent;
            }
            using slIsFeatureLoadedFn = int(__cdecl*)(unsigned, bool*);
            auto is_loaded = reinterpret_cast<slIsFeatureLoadedFn>(
                reinterpret_cast<void*>(::GetProcAddress(sl, "slIsFeatureLoaded")));
            if (is_loaded == nullptr)
            {
                return FrameGen::Unknown;
            }
            bool loaded = false;
            // Streamline's own id for DLSS-G.
            if (is_loaded(1000u, &loaded) != 0)
            {
                return FrameGen::Unknown;
            }
            return loaded ? FrameGen::Loaded : FrameGen::Absent;
        }

        // RENDER THREAD, once per adoption. Two questions, both of which earned their
        // place by discriminating between a machine this mod works on and one it does
        // not, or by deciding something in the code.
        //
        // Everything else that was asked here once - the back buffer's heap flags and
        // whether they carry SHARED, its resource flags, a Streamline proxy
        // QueryInterface on the swapchain, device and queue, GetDesc against GetDesc1,
        // and the swapchain's device against ours - answered identically on a machine
        // that works and one that removes the device three milliseconds later. They are
        // not asked any more; a log line that cannot come out two ways is noise in every
        // report that carries it.
        void log_present_path(IDXGISwapChain* swapchain)
        {
            switch (frame_generation_state())
            {
            case FrameGen::Absent:
                mm::log(L"  streamline: NVIDIA frame generation (DLSS-G) is not in this present path");
                break;
            case FrameGen::Loaded:
                mm::log(L"  streamline: NVIDIA frame generation (DLSS-G) is LOADED, so the game renders "
                        L"off-screen and the swapchain's back buffers belong to it, not to the game");
                break;
            case FrameGen::Unknown:
                mm::log(L"  streamline: sl.interposer.dll is present but did not answer whether DLSS-G "
                        L"is loaded, which is also what it does when DLSS-G is absent");
                break;
            }

            // Whether the display can compose an extra plane. Not about the failure - it
            // is what an overlay drawn beside the game's frames rather than into them
            // depends on, and this is the one place a reporter's machine can be asked.
            if (swapchain == nullptr)
            {
                return;
            }
            IDXGIOutput* output = nullptr;
            if (SUCCEEDED(swapchain->GetContainingOutput(&output)) && output != nullptr)
            {
                IDXGIOutput6* out6 = nullptr;
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out6))) && out6 != nullptr)
                {
                    UINT caps = 0;
                    if (SUCCEEDED(out6->CheckHardwareCompositionSupport(&caps)))
                    {
                        mm::logf(L"  hardware composition (MPO): 0x{:X}{}{}",
                                 caps,
                                 (caps & static_cast<UINT>(
                                              DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN)) != 0
                                     ? L" fullscreen"
                                     : L"",
                                 (caps & static_cast<UINT>(
                                              DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED)) != 0
                                     ? L" windowed"
                                     : L"");
                    }
                    safe_release(out6);
                }
                safe_release(output);
            }
        }

        void engage_terminal_off(const wchar_t* why);

        void request_readoption(const wchar_t* why)
        {
            if (g_readopt.exchange(true, std::memory_order_release))
            {
                return; // one is already pending; the render thread will service it
            }
            const std::uint64_t n = g_readopt_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n > kMaxReadoptions)
            {
                // Release and re-adopt, over and over, is not a recovery: each cycle costs
                // the game a stall and gets the overlay no further.
                g_readopt.store(false, std::memory_order_release);
                mm::logf(L"the overlay has re-adopted the swapchain {} times this session and this "
                         L"Present asked for another ({}), which is a cycle rather than a recovery",
                         n - 1,
                         why);
                engage_terminal_off(L"the overlay is off for the rest of this session: re-adopting the "
                                    L"swapchain stopped getting anywhere.");
                return;
            }
            mm::logf(L"the overlay is releasing its D3D12 objects and will adopt the swapchain again "
                     L"on a later Present (re-adoption {} of at most {} this session): {}",
                     n,
                     kMaxReadoptions,
                     why);
        }

        //==============================================================================
        // A REMOVED DEVICE IS TERMINAL
        //==============================================================================
        //
        // Every D3D12 object this module holds belongs to one device. Once that device is
        // removed they are all dead, and building them again is worse than doing nothing:
        // ImGui's DX12 backend uploads its font atlas behind a fence wait with no
        // timeout, so a re-init on a dead device wedges the render thread and takes the
        // game's own device-removed recovery down with it. So the state is one-way -
        // `g_device_removed` is set here, released from once inside the next Present, and
        // cleared only by `overlay::start()` or the master switch's `finish_stop()`.

        // GetDeviceRemovedReason answers with one of these, and the bare number is
        // unreadable in a bug report. The list is the whole DXGI table rather than the
        // five codes the documentation calls the removal family, because the one that
        // actually reached a player was DXGI_ERROR_ACCESS_DENIED - a reason a device
        // never gets removed for in any of the textbook cases, and the single most
        // informative word in the whole report when it does.
        const char* removed_reason_name(HRESULT hr)
        {
            switch (hr)
            {
            case S_OK:
                return "S_OK";
            case DXGI_ERROR_DEVICE_HUNG:
                return "DXGI_ERROR_DEVICE_HUNG";
            case DXGI_ERROR_DEVICE_REMOVED:
                return "DXGI_ERROR_DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_RESET:
                return "DXGI_ERROR_DEVICE_RESET";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL:
                return "DXGI_ERROR_INVALID_CALL";
            case DXGI_ERROR_ACCESS_DENIED:
                return "DXGI_ERROR_ACCESS_DENIED - the buffers are not this process's to write";
            case DXGI_ERROR_ACCESS_LOST:
                return "DXGI_ERROR_ACCESS_LOST";
            case DXGI_ERROR_NONEXCLUSIVE:
                return "DXGI_ERROR_NONEXCLUSIVE";
            case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
                return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
            case DXGI_ERROR_CANNOT_PROTECT_CONTENT:
                return "DXGI_ERROR_CANNOT_PROTECT_CONTENT";
            case DXGI_ERROR_UNSUPPORTED:
                return "DXGI_ERROR_UNSUPPORTED";
            case DXGI_ERROR_NOT_FOUND:
                return "DXGI_ERROR_NOT_FOUND";
            case DXGI_ERROR_WAS_STILL_DRAWING:
                return "DXGI_ERROR_WAS_STILL_DRAWING";
            case E_OUTOFMEMORY:
                return "E_OUTOFMEMORY";
            default:
                return "an HRESULT outside the removal family";
            }
        }

        // The terminal off state. `why` opens the line; everything after it is the same
        // whichever way the overlay got here.
        void engage_terminal_off(const wchar_t* why)
        {
            if (g_device_removed.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            mm::logf(L"{} Nothing of this mod is submitted again. Its objects are released inside the "
                     L"next Present IF the game presents again; a game that never does keeps them "
                     L"allocated until it exits, which is deliberate - releasing them is the render "
                     L"thread's job and no other thread may do it. If the game does not recover on its "
                     L"own, set overlay_hooks = 0 in config_wuchang_minimap.txt and restart - the mod "
                     L"then never touches DirectX.",
                     why);
        }

        void engage_device_removal()
        {
            engage_terminal_off(L"the overlay is off for the rest of this session: the D3D12 device it "
                                L"adopted has been removed.");
        }

        // ANY THREAD. The whole point is that it does not need the render thread: the
        // case this exists for is a device that dies while the overlay is idle, where the
        // next Present - the only place the render thread could ask - never arrives.
        void describe_device_state(char* out, std::size_t cap, unsigned budget_ms)
        {
            if (out == nullptr || cap == 0)
            {
                return;
            }
            out[0] = '\0';
            // A bound, not a spin. The thread that mutates `g_device` can be wedged while
            // holding this, and the callers are a Present in the game's own path and the
            // watchdog, which is the last thread still answering. Neither may wait for
            // ever to fill in a diagnostic.
            if (!g_device_lock.try_lock_ms(budget_ms))
            {
                ::_snprintf_s(out, cap, _TRUNCATE, "not asked: the device lock was still held after %u ms",
                              budget_ms);
                return;
            }
            const bool adopted = g_device != nullptr;
            const HRESULT reason = adopted ? g_device->GetDeviceRemovedReason() : S_OK;
            g_device_lock.unlock();
            if (!adopted)
            {
                ::_snprintf_s(out, cap, _TRUNCATE, "no device adopted");
                return;
            }
            ::_snprintf_s(out, cap, _TRUNCATE, "%s (0x%08X)", removed_reason_name(reason),
                          static_cast<unsigned>(reason));
        }

        bool device_alive(const wchar_t* what)
        {
            if (g_device_removed.load(std::memory_order_acquire))
            {
                return false;
            }
            if (g_device == nullptr)
            {
                return true; // nothing adopted yet, so nothing of ours can be dead
            }
            const HRESULT reason = g_device->GetDeviceRemovedReason();
            if (reason == S_OK)
            {
                return true;
            }
            mm::logf(L"the adopted D3D12 device {:p} reports {} (0x{:08X}) at {}",
                     static_cast<void*>(g_device),
                     stage_w(removed_reason_name(reason)),
                     static_cast<unsigned>(reason),
                     what);
            engage_device_removal();
            return false;
        }

        // The stage names are ASCII literals and the log takes wide strings. An explicit
        // cast loop rather than `std::wstring(a.begin(), a.end())`, which warns (C4244).
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

        // Every hide/show transition is logged with its reason, rate-limited by
        // hide_reason_log_ms.
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

        // One allocator per back buffer, created for every buffer that has none - a
        // fullscreen toggle can raise BufferCount after init. `g_buffer_count` is
        // already clamped to kMaxBuffers.
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

            // The game's swapchain is read for its geometry and then left alone. What the
            // overlay draws into is a surface of its own, composed over the same window -
            // see overlay_dcomp.cpp for why there is no second way of doing this.
            const bool surface = comp_ready() ? comp_resize(g_width, g_height)
                                              : comp_create(g_device, g_hwnd, g_width, g_height);
            if (!surface)
            {
                mm::log(L"the overlay has no surface to draw on, so it does not start. Nothing of the "
                        L"game is touched, and the rest of the mod - the tracker, the marker sweep, the "
                        L"waypoint files - keeps running.");
                return false;
            }
            IDXGISwapChain* target = comp_swapchain();
            g_buffer_count = comp_buffer_count();
            g_format = comp_format();
            // Our own queue, so nothing downstream has to ask whose surface this is.
            g_queue.store(comp_queue(), std::memory_order_release);

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
                if (FAILED(target->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]))))
                {
                    mm::logf(L"GetBuffer({}) failed", i);
                    release_render_targets();
                    return false;
                }
                g_device->CreateRenderTargetView(g_backbuffers[i], nullptr, handle);
                g_rtv[i] = handle;
                handle.ptr += stride;
            }


            if (!ensure_frame_allocators())
            {
                release_render_targets();
                return false;
            }

            g_rt_ready = true;
            mm::logf(L"render targets: the overlay's own {} buffer(s), {}x{}, {}, composed over hwnd "
                     L"0x{:X} whose own swapchain has flags 0x{:X} and swap effect {}. The game's back "
                     L"buffers are not touched.",
                     g_buffer_count,
                     g_width,
                     g_height,
                     format_name(g_format),
                     reinterpret_cast<std::uintptr_t>(g_hwnd),
                     static_cast<unsigned>(desc.Flags),
                     static_cast<int>(desc.SwapEffect));
            return true;
        }

        //==============================================================================
        // Per-frame work
        //==============================================================================

        void build_ui()
        {
            // `raw` is what the panel edits and what Save writes; `cfg` is the same
            // settings with every pixel key multiplied by the UI scale and is what the
            // HUD draws from. Keeping them apart stops a scaled value ever being written
            // back into the config file.
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

            // THE HUD FADE. Its target is the gate result and nothing else: 0 applies
            // instantly (and the gate stops the draw anyway), only showing is eased over
            // 150 ms. It is applied by scaling the opacity keys of the per-frame config
            // copy, so nothing downstream has to know the fade exists.
            const bool gate_open = have && hud_gate(cfg, snap, have, frame_now) == nullptr;
            if (gate_open && !g_hud_gate_ever_open.load(std::memory_order_relaxed))
            {
                // The first frame anything of ours could be seen; the loop thread's
                // first-run tip waits for it.
                g_hud_gate_ever_open.store(true, std::memory_order_release);
            }
            const float fade = hud_fade_step(gate_open, frame_now);
            cfg.opacity *= fade;
            cfg.compass_opacity *= fade;
            cfg.highlight_alpha_near *= fade;
            cfg.highlight_alpha_far *= fade;

            // The mouse cursor belongs to whoever is taking the input. Both conditions
            // are plain reads of the live flags, so the frame the map or the panel
            // closes is the frame the game gets the cursor back.
            ImGui::GetIO().MouseDrawCursor = map_open || mm::g_panel_open.load(std::memory_order_relaxed);

            // The pad, into ImGui's own nav. Only while the panel is open, and from the
            // state the loop thread sampled - never a poll from here.
            feed_pad_nav(raw);

            // THE ONE MARKER PASS. The minimap, the full map, the compass pips and the
            // x-ray highlight all read the same published buffer: it is walked once here
            // and each of them filters the result.
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

            // NEAREST UNFOUND (the waypoint_nearest_key hotkey). The pick lives here
            // because the frame's candidate list is what knows both distance and the
            // category mask in force. It has no radius: the whole chapter's published
            // buffer is a candidate.
            if (g_nearest_request.exchange(false, std::memory_order_acquire))
            {
                const FrameCand* best = nullptr;
                if (cfg.markers_enabled)
                {
                    for (const FrameCand& c : g_frame_cands)
                    {
                        if (c.found ||
                            !mdb::cat_enabled(cfg.markers_categories, static_cast<mdb::Cat>(c.cat)))
                        {
                            continue;
                        }
                        if (best == nullptr || c.d2_3d < best->d2_3d)
                        {
                            best = &c;
                        }
                    }
                }
                char note[160]{};
                if (best == nullptr)
                {
                    (void)std::snprintf(note, sizeof(note),
                                        cfg.markers_enabled
                                            ? "nothing unfound in the categories you have on"
                                            : "markers are turned off");
                }
                else
                {
                    mv::Waypoint wp{};
                    wp.set = true;
                    wp.x = best->m->x;
                    wp.y = best->m->y;
                    wp.z = best->m->z;
                    const char* name = mdb::display_label(static_cast<mdb::Cat>(best->cat), best->m->label);
                    if (mm::add_waypoint(wp))
                    {
                        (void)std::snprintf(note, sizeof(note), "waypoint: %s, %.0f m away", name,
                                            std::sqrt(static_cast<double>(best->d2_3d)) / 100.0);
                    }
                    else
                    {
                        (void)std::snprintf(note, sizeof(note), "%zu waypoints already - clear one first",
                                            mv::kMaxWaypoints);
                    }
                }
                toast_for(note, 3500);
            }

            if (mm::g_panel_open.load(std::memory_order_relaxed))
            {
                draw_panel(raw, snap, have);
                mm::g_panel_drew_frame.store(true, std::memory_order_relaxed);
            }
            else if (g_capture_row >= 0)
            {
                // The panel closed with a capture armed. Disarmed here rather than in the
                // close paths - this runs every frame, so the keyboard cannot stay
                // swallowed with no panel on screen.
                arm_capture(-1);
            }

            if (map_open)
            {
                draw_full_map(raw, snap, have, g_ui_scale);
            }
            if (g_map_was_open && !mm::g_map_open.load(std::memory_order_relaxed))
            {
                // Closed (by the key, by the pad chord, by the gate, or from inside the
                // map): the next open starts centred on the player again, with the map
                // mode's panels and search box reset. close_map has already done that
                // for its own routes; this is the edge the map never sees.
                //
                // The zoom the player left the map at becomes the zoom it opens with.
                // The view holds a DPI-scaled number (draw_full_map's `zscale`), so it is
                // un-scaled on the way back into the config, which is always literal.
                if (g_mv_init)
                {
                    const float zscale =
                        (raw.zoom_dpi_scaled && g_ui_scale > 0.0f) ? 1.0f / g_ui_scale : 1.0f;
                    mm::Config edit = mm::config();
                    edit.map_zoom = static_cast<float>(g_mv.uu_per_px) / zscale;
                    if (edit.map_zoom != raw.map_zoom)
                    {
                        mm::set_config(edit);
                        mm::g_save_config_soon = true;
                    }
                }
                g_mv_init = false;
                reset_map_mode();
            }
            g_map_was_open = mm::g_map_open.load(std::memory_order_relaxed);

            if (!cfg.show_minimap)
            {
                set_hide_reason(L"the minimap is switched off");
            }
            else if (mm::g_map_open.load(std::memory_order_relaxed))
            {
                // The full map replaces the minimap while it is up, so the slicer never
                // cuts two windows a frame.
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

            // The compass and the x-ray highlight ask the same gate the minimap does -
            // one evaluation, no second set of rules, no second latch - and additionally
            // stand down while the full map is open.
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

        // The D3D12 half of the start-up: the device, the composition surface and its
        // render targets, one allocator per back buffer, the command list and the fence.
        // Nothing here draws and nothing here is ImGui.
        //
        // Called on every Present until ImGui is up, so every object is created only
        // where it does not exist yet.
        bool ensure_device_objects(IDXGISwapChain* swapchain)
        {
            if (g_device == nullptr)
            {
                // ID3D12Resource::GetDevice on back buffer 0 is the one link from a
                // swapchain to its device that every wrapper forwards, where
                // IDXGISwapChain::GetDevice(ID3D12Device) fails on this game's ReShade
                // wrapper. GetBuffer only READS the buffer, so it answers even where
                // WRITING that buffer is denied - which is why this is the reliable link
                // and not merely the first one tried.
                //
                // Into a local, published under `g_device_lock` once it is whole: the
                // out-parameter of a call that is still running is not a pointer another
                // thread may dereference.
                ID3D12Device* device = nullptr;
                ID3D12Resource* buffer = nullptr;
                HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer));
                if (SUCCEEDED(hr) && buffer != nullptr)
                {
                    hr = buffer->GetDevice(IID_PPV_ARGS(&device));
                }
                safe_release(buffer);
                if (FAILED(hr) || device == nullptr)
                {
                    mm::logf(L"the swapchain's back buffer 0 named no ID3D12Device (0x{:08X}); "
                             L"asking the swapchain itself",
                             static_cast<unsigned>(hr));
                    hr = swapchain->GetDevice(IID_PPV_ARGS(&device));
                }
                if (FAILED(hr) || device == nullptr)
                {
                    mm::logf(L"no ID3D12Device reachable from either the back buffer or the swapchain "
                             L"(0x{:08X}) - overlay off",
                             static_cast<unsigned>(hr));
                    g_failed = true;
                    return false;
                }
                spin::SpinGuard guard(g_device_lock);
                g_device = device;
            }

            // Not `g_failed`: a swapchain that will not hand out its buffers is a
            // swapchain-level failure (a resize in flight, a device that has just gone,
            // a wrapper being swapped) and those recover. `create_render_targets` has
            // already asked for a re-adoption where it knew the reason.
            if (!g_rt_ready && !create_render_targets(swapchain))
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
            if (g_cmd_list == nullptr)
            {
                if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       g_frames[0].allocator, nullptr,
                                                       IID_PPV_ARGS(&g_cmd_list))))
                {
                    mm::log(L"CreateCommandList failed");
                    g_failed = true;
                    return false;
                }
                g_cmd_list->Close();
            }

            if (g_fence == nullptr &&
                FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            {
                mm::log(L"CreateFence failed");
                g_failed = true;
                return false;
            }
            if (g_fence_event == nullptr)
            {
                g_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
            if (g_fence_event == nullptr)
            {
                mm::log(L"CreateEvent failed");
                g_failed = true;
                return false;
            }
            return true;
        }


        // The ImGui half: our SRV heap, the context, both backends and the wndproc chain.
        // Only ever reached once the composition surface and its render targets exist.
        bool ensure_imgui(ID3D12CommandQueue* queue)
        {
            // RESTART-ONLY config key: the heap is created once, here.
            if (g_srv_heap.heap() == nullptr && !g_srv_heap.create(g_device, mm::config().srv_heap_size))
            {
                mm::log(L"CreateDescriptorHeap(SRV) failed");
                g_failed = true;
                return false;
            }

            // The last gate before ImGui exists at all: the device has been created,
            // the render targets are up and a command list has been submitted on it, so
            // if the adoption was going to remove the device it has happened by now.
            if (!device_alive(L"the ImGui context and DX12 backend creation"))
            {
                return false;
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // The panel draws its own software cursor; ImGui must not fight the game
            // over the OS cursor shape.
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            // Keyboard + gamepad navigation for the F2 panel.
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

            // Which devices the game reads through WM_INPUT, logged once. It decides
            // whether swallowing a key as a window message is enough: a game that
            // registers a raw KEYBOARD also needs the raw packet filtered (the panel's
            // Esc path), one that registers only the mouse does not.
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
            g_imgui_rtv_format = g_format;
            g_imgui_ready = true;
            crumb::stage(crumb::kImGuiUp);
            mm::logf(L"ImGui {} initialised on the overlay's own surface: device {:p}, queue {:p}, {} frames in flight, "
                     L"RTV {}",
                     std::wstring(IMGUI_VERSION, IMGUI_VERSION + std::strlen(IMGUI_VERSION)),
                     static_cast<void*>(g_device),
                     static_cast<void*>(queue),
                     g_buffer_count,
                     format_name(g_format));
            slice_selftest();
            return true;
        }

        bool ensure_initialised(IDXGISwapChain* swapchain)
        {
            if (g_failed)
            {
                return false;
            }
            if (g_imgui_ready)  // NOLINT: the happy path, checked every frame
            {
                return true;
            }
            // Nothing is created on a device that is not answering S_OK. The ImGui DX12
            // backend is the reason the check is here and not only at the Present that
            // reports the removal: its font-atlas upload waits on a fence with no
            // timeout, which on a dead device never completes.
            if (!device_alive(L"the overlay's first initialisation"))
            {
                return false;
            }
            if (!ensure_device_objects(swapchain))
            {
                return false;
            }
            // `create_render_targets`, inside the call above, is what fills `g_queue`
            // with the overlay's own composition queue.
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            if (queue == nullptr)
            {
                return false;
            }
            if (!g_display_logged)
            {
                // Here rather than at the adoption of the swapchain: the adapter is
                // reached through `g_device`, which `ensure_device_objects` is what
                // creates. Once per adoption, so a resize does not reprint it.
                g_display_logged = true;
                log_display_environment(swapchain);
                log_present_path(swapchain);
            }
            return ensure_imgui(queue);
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
        // touched from the thread that created them - whichever thread calls Present. So
        // the loop thread clears mm::g_mod_active and this runs inside the next Present,
        // before the hooks are taken out. It leaves the module in the state it had
        // before the first frame: `start()` re-enables the hooks and
        // ensure_initialised() builds everything again.
        //
        // Two callers, one body. The master switch (shutdown_render) and a lost device /
        // replaced swapchain (the re-adoption path in render()) release the same
        // objects; only the master switch also answers the loop thread's handshake with
        // `g_render_stopped`, which a re-adoption must not touch or the loop thread
        // believes a disable it never asked for has completed.
        //
        // The invariant is the LOCK, not a thread id: whoever holds `g_render_lock` may
        // release these objects. Present arrives on whichever thread the game or a
        // frame-generation proxy presents from, so the thread that built them and the one
        // that releases them need not be the same - only serialised.

        void release_device_objects()
        {
            if (g_imgui_ready || g_device != nullptr)
            {
                crumb::stage(crumb::kTeardownBegin);
                // The slicer writes into mapped upload heaps about to be released. On a
                // timeout the buffers are leaked rather than freed under a live writer.
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
                // After the render targets, because the RTVs are views onto its buffers,
                // and after wait_for_gpu above, because our queue is what they were
                // submitted on.
                comp_release();
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
                {
                    // Under the lock, so a thread asking the device for its removal
                    // reason sees either a live pointer or none - never a released one.
                    spin::SpinGuard guard(g_device_lock);
                    safe_release(g_device);
                }
                g_imgui_frames_in_flight = 0;
                g_imgui_rtv_format = DXGI_FORMAT_UNKNOWN;
                mm::log(L"the render thread has released ImGui, the descriptor heaps, "
                        L"the slice buffers and the map textures");
            }
            g_swapchain = nullptr;
            // Nothing is adopted, so there is no silence to measure until the next
            // Present on a newly adopted swapchain.
            g_adopted_present_ms = 0;
            // The cached IDXGISwapChain3 holds a reference on the swapchain being let
            // go; keeping it would pin a dead object and answer
            // GetCurrentBackBufferIndex for a swapchain we no longer draw on.
            g_candidates_logged = 0;
            // The queue is the composition module's, released by the `comp_release()`
            // above; this is only the overlay's handle on it.
            g_queue.store(nullptr, std::memory_order_release);
            // The display lines belong to an adoption, so the next one writes its own.
            g_display_logged = false;
            g_failed = false;
            // Everything a re-adoption would have released is gone already.
            g_readopt.store(false, std::memory_order_release);
            crumb::stage(crumb::kTeardownEnd);
        }

        void shutdown_render()
        {
            release_device_objects();
            // The master switch's handshake, and the one thing a re-adoption must skip.
            g_render_stopped.store(true, std::memory_order_release);
        }

        void render(IDXGISwapChain* swapchain)
        {
            // The master switch, first statement: one relaxed atomic load per Present
            // while the mod is off, and the first Present to see it off does the
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
            // TERMINAL. The adopted device is gone: release what is left of ours once,
            // on this thread because it is the only one allowed to, and then return from
            // every later Present before touching anything at all.
            if (g_device_removed.load(std::memory_order_acquire))
            {
                if (!g_removal_released.exchange(true, std::memory_order_acq_rel))
                {
                    g_render_stage.store("releasing after a device removal", std::memory_order_relaxed);
                    spin::SpinGuard guard(g_render_lock);
                    release_device_objects();
                }
                g_render_stage.store("off: the adopted device was removed", std::memory_order_relaxed);
                return;
            }
            g_render_stage.store("waiting for the render lock", std::memory_order_relaxed);
            spin::SpinGuard guard(g_render_lock);
            g_render_stage.store("holding the render lock", std::memory_order_relaxed);

            // A Present of the adopted swapchain failed on some thread. Asking the device
            // why is this thread's job, and it decides between the terminal state and the
            // re-adoption immediately below. It runs BEFORE the `g_failed` return: the
            // reason a device was removed is the single most useful line in a bug report,
            // and an unrelated earlier failure must not be what swallows it.
            handle_present_failure();
            // A start after a stop that never reached a Present: whatever survived it is
            // released before anything is built on it.
            if (g_verify_on_start.exchange(false, std::memory_order_acq_rel) && g_device != nullptr)
            {
                request_readoption(L"the overlay is starting again while the objects of the device it "
                                   L"had adopted are still allocated");
            }
            if (g_device_removed.load(std::memory_order_acquire))
            {
                return; // the next Present does the one release
            }
            if (g_failed)
            {
                return;
            }

            // RE-ADOPTION, here because this is the only thread that may touch a D3D12
            // object. Whatever asked for it (a removed device, a swapchain that stopped
            // handing out buffers, a queue from the wrong device) left this module
            // holding objects of something that no longer exists; releasing them and
            // starting over is what brings the overlay back after a driver reset or a
            // swapchain swap. `g_failed` is not set - this path is recoverable.
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
                // Another swapchain - frame generation, ReShade, the game's decoy D3D11
                // one - and normally not ours. It IS ours when the game has recreated
                // its swapchain: the adopted one then presents nothing while this one
                // presents every frame. Both tests have to hold before that is believed
                // - our swapchain silent for kSwapchainSilentMs AND this one a D3D12
                // swapchain - because the decoy presents right through a level load,
                // when the game's own swapchain is legitimately quiet.
                const std::uint64_t now = ::GetTickCount64();
                if (g_adopted_present_ms != 0 && now - g_adopted_present_ms > kSwapchainSilentMs &&
                    is_d3d12_swapchain(swapchain))
                {
                    // The ordinary re-adoption: release everything on this thread and
                    // let the next Present adopt whatever is presenting now.
                    g_adopted_present_ms = 0; // one attempt per adopted swapchain
                    request_readoption(L"the adopted swapchain has not presented for 2 s while another "
                                       L"D3D12 swapchain has - the game has replaced it");
                }
                return;
            }

            // This Present is on the adopted swapchain: the reference point the check
            // above measures silence against.
            g_adopted_present_ms = ::GetTickCount64();

            if (!ensure_initialised(swapchain))
            {
                return;
            }
            if (!g_rt_ready && !create_render_targets(swapchain))
            {
                // A GetBuffer / RTV failure is recoverable (a resize in flight, a device
                // that has gone), so it goes to the re-adoption path instead of retrying
                // the same objects every frame.
                request_readoption(L"the render targets could not be rebuilt");
                return;
            }
            // After a resize that raised BufferCount the ImGui backend still has one set
            // of per-frame buffers per OLD frame in flight and would reuse the vertex,
            // index and descriptor storage of a frame the GPU has not finished.
            // Re-initialising the DX12 backend is the only way to change that count; it
            // happens outside a frame (before NewFrame) and with the GPU idle. A changed
            // back-buffer FORMAT (an HDR toggle) needs the same rebuild: the backend's
            // pipeline state bakes the RTV format in, and drawing it against a render
            // target of another format is a debug-layer error and a wrong-looking frame.
            if (g_imgui_ready && g_imgui_frames_in_flight != 0 &&
                (g_imgui_frames_in_flight < static_cast<int>(g_buffer_count) ||
                 g_imgui_rtv_format != g_format))
            {
                mm::logf(L"the swapchain now has {} buffer(s) in {}, ImGui was initialised for {} frames "
                         L"in flight in {} - re-initialising the DX12 backend",
                         g_buffer_count,
                         format_name(g_format),
                         g_imgui_frames_in_flight,
                         format_name(g_imgui_rtv_format));
                // ImGui_ImplDX12_Init uploads the font atlas behind a fence wait with no
                // timeout, so it may only ever run on a device that answers S_OK.
                if (!device_alive(L"the ImGui DX12 backend re-init after a resize"))
                {
                    return;
                }
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
                g_imgui_rtv_format = g_format;
            }

            // The buffer being drawn is ours, so its index comes from our own swapchain.
            const UINT index = comp_swapchain()->GetCurrentBackBufferIndex();
            FrameCtx& frame = g_frames[index];
            if (frame.allocator == nullptr)
            {
                // A cheap guard: a missing allocator drops the frame instead of faulting
                // in Reset().
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

            // A reload (F5) rebuilds the whole texture set. The old textures may only be
            // released here, on the render thread and BEFORE the frame's draw lists are
            // built, or the frame references an SRV slot just handed back.
            if (g_drop_textures.load(std::memory_order_acquire))
            {
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    g_drop_textures.store(false, std::memory_order_release);
                    // A full GPU flush plus ~340 MB of releases: a one-off that must not
                    // become the peak every later frame is judged against.
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
            // The UI scale, decided from the current back buffer and applied before the
            // frame's draw lists exist. ResizeBuffers changes g_height and a config
            // change comes through cfg_cached, so both re-enter here on their own.
            apply_ui_scale(wanted_ui_scale(mm::cfg_cached(), static_cast<float>(g_height)));
            // Two sub-counters, because the two halves fail differently:
            // ImGui_ImplWin32_NewFrame reads and writes the cursor and the client rect
            // of a window owned by the GAME thread, and that cross-thread user32 call
            // blocks until that thread pumps messages - which it does not do inside a
            // synchronous level load. build_ui() is our own drawing and touches no OS
            // handle.
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
                // Cross-thread user32 with the render lock held: everything in here
                // reads or writes the cursor and the client rect of a window owned by
                // the GAME thread, so it is the one place in the frame that can wait on
                // another thread. That is why `hk_ResizeBuffers` - the only other taker
                // of that lock, and a call that can arrive on the game thread - acquires
                // it with a bound instead of spinning for ever.
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
            // tgate::text_active() rather than io.WantTextInput: that flag is a frame
            // behind the caret, and the frame a click into a text box lands in is
            // exactly the frame the loop thread is reading while the first letter of
            // the word goes down.
            g_imgui_want_text.store(tgate::text_active(), std::memory_order_relaxed);
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

            // The height-slice window the CPU filled during build_ui(), recorded BEFORE
            // ImGui's draw call in the same command list, so the GPU sees the copy
            // complete before it samples the texture - no extra queue, no second
            // submission, no PSO of our own.
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
            // Our surface carries the overlay and nothing else, so every frame starts
            // fully transparent and the compositor shows the game through it.
            const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            g_cmd_list->ClearRenderTargetView(g_rtv[index], transparent, 0, nullptr);
            ID3D12DescriptorHeap* heaps[] = {g_srv_heap.heap()};
            g_cmd_list->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list);

            // The screenshot copy, if one was asked for: it takes the back buffer from
            // RENDER_TARGET to PRESENT itself (via COPY_SOURCE), so it replaces the
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
            // The overlay's own queue, which nothing in this process hooks.
            // ExecuteCommandLists returns void: the only report a bad submission gives is
            // the fence Signal that follows it and the device's own removal reason, both
            // checked below.
            queue->ExecuteCommandLists(1, lists);
            ++g_fence_value;
            const HRESULT sig = queue->Signal(g_fence, g_fence_value);
            if (FAILED(sig))
            {
                static bool said = false;
                if (!said)
                {
                    said = true;
                    mm::logf(L"ID3D12CommandQueue::Signal failed (0x{:08X}) on queue {:p} right after the "
                             L"overlay's command list was submitted - the queue or its device is gone",
                             static_cast<unsigned>(sig),
                             static_cast<void*>(queue));
                }
            }
            frame.fence_value = g_fence_value;
            // Our surface is presented by us. The game's Present, which this hook is
            // inside, carries the game's frame and knows nothing about this one; the
            // compositor is what puts the two together.
            //
            // The ORIGINAL, exactly as the command-list submission above. Present is one
            // dxgi function shared by every swapchain in the process, ours included, so
            // calling it through the vtable would re-enter this hook on a thread that
            // already holds the render lock and wedge the render thread.
            const HRESULT cp = o_Present(comp_swapchain(), 0, 0);
            if (FAILED(cp))
            {
                static bool said = false;
                if (!said)
                {
                    said = true;
                    mm::logf(L"composition: presenting the overlay's own surface failed (0x{:08X})",
                             static_cast<unsigned>(cp));
                }
            }
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
            // A removal our own submission caused does not have to wait for a Present to
            // report it: the device answers directly, and this is the shortest path from
            // cause to log line.
            (void)device_alive(L"the end of a frame the overlay submitted");
        }

        //==============================================================================
        // Hooks
        //==============================================================================

        // THE EXCEPTION BARRIER.
        //
        // Anything in the frame can throw std::bad_alloc, and a throw would unwind
        // through the MinHook trampoline into DXGI - frames compiled with no idea this
        // code exists, i.e. a crash inside the driver stack whose call stack names
        // dxgi.dll. So the whole frame is wrapped once, at the hook boundary: a throw
        // becomes one log line and a dead overlay, and the game keeps presenting.
        //
        // Not SEH: an access violation is left to crash. `__try` cannot live in a
        // function needing C++ unwinding (MSVC C2712), and swallowing an AV mid
        // command-list recording leaves the list open and the back buffer stranded
        // between resource states, which later frames turn into a device removal with no
        // evidence. The crash breadcrumb plus CrashContext.runtime-xml is the diagnostic
        // route here.
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
        // render lock, because a full-canvas unpack is two allocations plus a per-row
        // conversion of up to ~8 MB and `hk_ResizeBuffers` waits on that same lock from
        // the GAME thread. Still the render thread, which is what owns the readback
        // resource and the fence.
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

        // Did the Present itself fail? A removal is reported for EVERY swapchain in the
        // process, because the swapchain that reports it first need not be the one we
        // adopted and the reason code is what a bug report is read for. Only a removal on
        // our own device is terminal; anything else is one log line.
        //
        // ANY THREAD - frame generation presents from its own. The device is asked for
        // its reason HERE rather than left to the render thread: the removal that matters
        // most is the one that stops the game presenting for good, and then no later
        // Present ever arrives for the render thread to ask in. Asking is all this does -
        // acting on the answer is still the render thread's, through `g_present_failed`
        // and the check at the top of render().
        void note_present_result(HRESULT hr, IDXGISwapChain* sc)
        {
            if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET)
            {
                return;
            }
            const bool ours = sc == g_swapchain;
            // Once per removal. It repeats on every Present of every swapchain from here
            // on, and the first line is the one worth having.
            if (!g_removal_logged.exchange(true, std::memory_order_acq_rel))
            {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (sc != nullptr)
                {
                    sc->GetDesc(&desc);
                }
                // A short budget: this runs inside the game's own Present, once per
                // removal, on a device that is already gone.
                char device_state[128]{};
                describe_device_state(device_state, sizeof(device_state), 50);
                mm::logf(L"DEVICE REMOVED: Present returned 0x{:08X} on {} swapchain {:p} ({}x{} {} "
                         L"x{} buffers, flags 0x{:X}, swap effect {}, hwnd 0x{:X}); our device {:p}, "
                         L"our queue {:p}, present count {}. GetDeviceRemovedReason() answers {}.",
                         static_cast<unsigned>(hr),
                         ours ? L"OUR adopted" : L"another",
                         static_cast<void*>(sc),
                         desc.BufferDesc.Width,
                         desc.BufferDesc.Height,
                         format_name(desc.BufferDesc.Format),
                         desc.BufferCount,
                         static_cast<unsigned>(desc.Flags),
                         static_cast<int>(desc.SwapEffect),
                         reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                         static_cast<void*>(g_device),
                         static_cast<void*>(g_queue.load(std::memory_order_acquire)),
                         g_present_count.load(std::memory_order_relaxed),
                         stage_w(device_state));
            }
            if (!ours)
            {
                return; // another renderer's device: reported, never acted on
            }
            // The render thread decides: it asks the device for its reason and either
            // engages the terminal state or asks for a re-adoption.
            g_present_failed.store(true, std::memory_order_release);
        }

        // RENDER THREAD, from render(). The other half of note_present_result: a Present
        // of the adopted swapchain failed with DEVICE_REMOVED / DEVICE_RESET and the
        // device has to be asked what happened, on the one thread that may ask it.
        void handle_present_failure()
        {
            if (!g_present_failed.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            // device_alive() logs the reason and engages the terminal state when the
            // device has stopped answering S_OK.
            if (!device_alive(L"a Present of the adopted swapchain that returned DEVICE_REMOVED"))
            {
                return;
            }
            // Nothing adopted yet, or a device that still answers S_OK: the recoverable
            // recoverable path - release and adopt again on a later Present.
            request_readoption(L"a Present of the adopted swapchain returned DEVICE_REMOVED or "
                               L"DEVICE_RESET while the device still answers S_OK");
        }

        // The game's own Present arguments, kept for `log_display_environment`. Recorded
        // for the adopted swapchain only, so a decoy or a frame-generation chain cannot
        // put its own pacing in the log.
        void note_present_args(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            if (sc == g_swapchain)
            {
                g_present_sync.store(sync, std::memory_order_relaxed);
                g_present_flags.store(flags, std::memory_order_relaxed);
            }
        }

        HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            // Our own composition surface. It is presented from inside a frame this hook
            // is already running, so treating it as a frame of the game's would re-enter
            // everything below on a thread that holds the render lock.
            if (comp_ready() && sc == comp_swapchain())
            {
                return o_Present(sc, sync, flags);
            }
            note_present_args(sc, sync, flags);
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            // The original is always called, for every swapchain in the process: Present
            // is one dxgi function shared by every swapchain, D3D11 and D3D12 alike, and
            // returning early for "not ours" would stop another overlay presenting.
            const HRESULT hr = o_Present(sc, sync, flags);
            if (sc == g_swapchain || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                note_present_result(hr, sc);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
        {
            note_present_args(sc, sync, flags);
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            const HRESULT hr = o_Present1(sc, sync, flags, params);
            if (sc == g_swapchain || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                note_present_result(hr, sc);
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
            // A bounded acquire, not a spin: this call can arrive on the game thread and
            // the render thread holds the same lock across `ImGui_ImplWin32_NewFrame()`,
            // which waits on that very thread. Spinning for ever is a two-thread
            // deadlock; the bound turns the worst case into a named log line and a
            // resize that may fail, which the next Present recovers from by recreating
            // its render targets.
            if (g_render_lock.try_lock_ms(2000))
            {
                // Ours or not, tested before the line is formatted: every swapchain in
                // the process comes through this function, the decoy 144x8 D3D11 one
                // included, and formatting for each would put a wstring allocation and a
                // log write in front of resizes that have nothing to do with this mod.
                if (sc == g_swapchain)
                {
                    mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}) - releasing render targets",
                             count,
                             w,
                             h,
                             format_name(format));
                    // A throw here would unwind into DXGI through the trampoline - the
                    // same hazard the Present barrier exists for, and on the GAME
                    // thread, where it takes the game down with it.
                    try
                    {
                        // The fence, not ImGui: a frame can be in flight on the GPU
                        // before `g_imgui_ready` is set, and releasing the back buffers
                        // out from under it is exactly the case this wait exists for.
                        if (g_fence != nullptr)
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

        //==============================================================================
        // Hook installation via a throwaway device + swapchain, on every launch
        //==============================================================================
        //
        // Discovery creates a throwaway D3D12 device, a DIRECT command queue and a 64x64
        // swapchain on a hidden window through our own import table, reads three vtable
        // slots and destroys all three. The addresses it finds are a property of the
        // DLLs, so they could be written down and reused - and reusing them is what makes
        // such a launch a different launch. On a machine stacking ReShade's dxgi proxy,
        // an addon, Streamline's interposer and the Steam overlay, these creation calls
        // are what drives every one of those layers through its own interposer before
        // MinHook writes a byte; hooking the addresses straight out of a file skips that
        // and intermittently costs the frame - a black screen from launch, with this mod
        // presenting happily and nothing in any log. Discovery costs ~60 ms once per
        // launch, which is not worth a file to skip.
        //
        // The price is that Steam's GameOverlayRenderer64 hooks the same creation entry
        // points and re-targets its overlay onto what it sees created, so its FPS counter
        // can end up pointing at objects we destroyed again.

        // wuchang_minimap_hookaddr.txt is where older builds cached the addresses.
        void remove_stale_hook_cache()
        {
            const std::wstring path = mm::mod_dir() + L"\\wuchang_minimap_hookaddr.txt";
            if (::DeleteFileW(path.c_str()) != 0)
            {
                mm::logf(L"deleted {} - an older build's address cache. The addresses are discovered "
                         L"fresh on every launch.",
                         path);
            }
        }

        // Three MH_CreateHook calls, one MH_EnableHook, one report.
        bool create_and_enable(void** addr, const wchar_t* how)
        {
            const MH_STATUS s1 = MH_CreateHook(addr[0], reinterpret_cast<void*>(&hk_Present),
                                               reinterpret_cast<void**>(&o_Present));
            const MH_STATUS s2 = MH_CreateHook(addr[1], reinterpret_cast<void*>(&hk_ResizeBuffers),
                                               reinterpret_cast<void**>(&o_ResizeBuffers));
            const MH_STATUS s3 = MH_CreateHook(addr[2], reinterpret_cast<void*>(&hk_Present1),
                                               reinterpret_cast<void**>(&o_Present1));

            // The statuses are checked before anything is enabled: either the required
            // hook exists or nothing of ours is installed at all. A partial install
            // would leave live trampolines behind while reporting failure, and the
            // master switch's re-enable would then run discovery on top of them.
            const MH_STATUS created[kHookCount] = {s1, s2, s3};
            const bool required_ok = s1 == MH_OK;
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
                mm::log(L"hooks: the required hook (Present) did not install, so every trampoline that "
                        L"HAD been created was removed again - nothing of this mod is in the game's "
                        L"call path");
            }

            g_hook_report = std::format(L"{} | Present {} @ {} | ResizeBuffers {} @ {} | Present1 {} @ {} | "
                                        L"enable {}",
                                        how,
                                        static_cast<int>(s1),
                                        module_of(addr[0]),
                                        static_cast<int>(s2),
                                        module_of(addr[1]),
                                        static_cast<int>(s3),
                                        module_of(addr[2]),
                                        static_cast<int>(en));
            mm::logf(L"hooks: {}", g_hook_report);
            mm::logf(L"hook addresses: Present {:p}  ResizeBuffers {:p}  Present1 {:p}",
                     addr[0],
                     addr[1],
                     addr[2]);
            log_overlay_modules();
            const bool ok = required_ok && en == MH_OK;
            if (!ok)
            {
                mm::log(L"at least one required hook did not install - the overlay will not draw");
            }
            return ok;
        }

        bool install_hooks()
        {
            remove_stale_hook_cache();
            const std::uint64_t began = ::GetTickCount64();
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

                void* addr[kHookCount] = {
                    sc_vtable[8],  // IDXGISwapChain::Present
                    sc_vtable[13], // IDXGISwapChain::ResizeBuffers
                    sc_vtable[22], // IDXGISwapChain1::Present1
                };

                // Who was already there, read before a byte is written: a jmp in front of
                // Present names the module that installed it, and MinHook relocates
                // those bytes into our trampoline, so that overlay stays in the chain.
                for (int i = 0; i < kHookCount; ++i)
                {
                    mm::logf(L"hook discovery: {} -> {} {}",
                             kHookNames[i],
                             module_of(addr[i]),
                             detour_report(addr[i]));
                }

                ok = create_and_enable(addr, L"by dummy-swapchain discovery");
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
            // What the throwaway objects cost, and the window in which Steam's overlay
            // could have re-targeted itself onto them.
            mm::logf(L"hook discovery: {} ms from the dummy window to the three hooks; the dummy device, "
                     L"queue, swapchain and window are destroyed again",
                     g_hook_install_ms - began);
            return ok;
        }

    } // namespace ovl
} // namespace overlay
