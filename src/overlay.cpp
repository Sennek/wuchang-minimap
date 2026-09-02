#include "overlay.hpp"

//
// overlay - the DX12 + Dear ImGui side of WuchangMinimap.
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
// THREADING
// ---------
// Present runs on the RHI / present thread, never the game thread. Nothing in this file
// touches a UObject; the game state arrives through mm::read_snapshot(), which is a
// seqlock. Config file I/O, maps.json parsing and the PNG decode all happen on the
// UE4SS loop thread (on_update) - Present only ever creates D3D12 objects and draws.
//

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <MinHook.h>

#include "compass.hpp"
#include "gamepad.hpp"
#include "highlight.hpp"
#include "mapdata.hpp"
#include "mapview.hpp"
#include "markers.hpp"
#include "mmstate.hpp"
#include "projection.hpp"
#include "version.hpp"

// imgui_impl_win32.h deliberately hides this behind `#if 0` so the header does not
// depend on <windows.h>; the backend expects you to copy the declaration yourself.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr int kSrvHeapSize = 64;
        constexpr int kMaxBuffers = 8;
        // The DEFAULT roundness of the minimap disc and its rings; `minimap_circle_segments`
        // overrides it. The drawing helpers below are handed geometry, not the config, so
        // the live value is cached here by build_ui() once per frame (render thread only).
        constexpr int kCircleSegments = 72;
        int g_circle_segments = kCircleSegments;

        //==============================================================================
        // Spinlock (no std::mutex anywhere in this mod - see lessons.md)
        //==============================================================================

        class Spinlock
        {
          public:
            void lock() noexcept
            {
                for (int spin = 0; flag_.test_and_set(std::memory_order_acquire); ++spin)
                {
                    if ((spin & 0x3F) == 0x3F)
                    {
                        ::SwitchToThread();
                    }
                    else
                    {
                        YieldProcessor();
                    }
                }
            }
            void unlock() noexcept
            {
                flag_.clear(std::memory_order_release);
            }

          private:
            std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
        };

        class SpinGuard
        {
          public:
            explicit SpinGuard(Spinlock& l) noexcept : lock_(l)
            {
                lock_.lock();
            }
            ~SpinGuard()
            {
                lock_.unlock();
            }
            SpinGuard(const SpinGuard&) = delete;
            SpinGuard& operator=(const SpinGuard&) = delete;

          private:
            Spinlock& lock_;
        };

        //==============================================================================
        // Small helpers
        //==============================================================================

        std::wstring module_of(const void* addr)
        {
            HMODULE mod = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(addr),
                                     &mod) == 0 ||
                mod == nullptr)
            {
                return L"<unknown>";
            }
            wchar_t path[MAX_PATH * 2]{};
            if (::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path))) == 0)
            {
                return L"<unknown>";
            }
            std::wstring s{path};
            const auto slash = s.find_last_of(L'\\');
            const std::wstring name = slash == std::wstring::npos ? s : s.substr(slash + 1);
            return std::format(L"{}+0x{:X}",
                               name,
                               reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod));
        }

        const wchar_t* format_name(DXGI_FORMAT f)
        {
            switch (f)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                return L"R8G8B8A8_UNORM";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return L"R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return L"B8G8R8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return L"B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return L"R10G10B10A2_UNORM";
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return L"R16G16B16A16_FLOAT";
            default:
                return L"<other>";
            }
        }

        template <typename T>
        void safe_release(T*& p)
        {
            if (p != nullptr)
            {
                p->Release();
                p = nullptr;
            }
        }

        //==============================================================================
        // SRV descriptor heap allocator
        //==============================================================================
        //
        // ImGui 1.92's DX12 backend allocates SRV descriptors through callbacks (it
        // needs one per texture now, not just one for the font atlas), so the heap and
        // its free list are ours. A fixed 64-slot heap is plenty: the font atlas plus
        // one map texture.

        class SrvHeap
        {
          public:
            bool create(ID3D12Device* device, int count)
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc{};
                desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                desc.NumDescriptors = static_cast<UINT>(count);
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_))))
                {
                    return false;
                }
                stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
                gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
                free_.clear();
                free_.reserve(static_cast<std::size_t>(count));
                for (int i = count - 1; i >= 0; --i)
                {
                    free_.push_back(i);
                }
                return true;
            }

            void destroy()
            {
                safe_release(heap_);
                free_.clear();
            }

            ID3D12DescriptorHeap* heap() const
            {
                return heap_;
            }

            bool alloc(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
            {
                if (free_.empty())
                {
                    return false;
                }
                const int index = free_.back();
                free_.pop_back();
                cpu.ptr = cpu_start_.ptr + static_cast<SIZE_T>(index) * stride_;
                gpu.ptr = gpu_start_.ptr + static_cast<UINT64>(index) * stride_;
                return true;
            }

            void free(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
            {
                if (heap_ == nullptr || stride_ == 0 || cpu.ptr < cpu_start_.ptr)
                {
                    return;
                }
                const int index = static_cast<int>((cpu.ptr - cpu_start_.ptr) / stride_);
                free_.push_back(index);
            }

          private:
            ID3D12DescriptorHeap* heap_ = nullptr;
            UINT stride_ = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
            std::vector<int> free_;
        };

        SrvHeap g_srv_heap;

        void srv_alloc_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            if (!g_srv_heap.alloc(*cpu, *gpu))
            {
                cpu->ptr = 0;
                gpu->ptr = 0;
                mm::log(L"SRV heap exhausted");
            }
        }

        void srv_free_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            g_srv_heap.free(cpu);
        }

        //==============================================================================
        // Renderer state
        //==============================================================================

        struct FrameCtx
        {
            ID3D12CommandAllocator* allocator = nullptr;
            UINT64 fence_value = 0;
        };

        // One texture: either the chapter composite (RGBA8) or one floor layer (R8
        // coverage mask, swizzled to RGBA in the SRV so ImGui's shader can tint it).
        struct MapTexture
        {
            ID3D12Resource* tex = nullptr;
            ID3D12Resource* upload = nullptr;
            UINT64 upload_fence = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
            int width = 0;
            int height = 0;
            int channels = 4;
            std::size_t bytes = 0;
            bool ready = false;
            std::string chapter;
        };

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

        struct SliceBuf
        {
            ID3D12Resource* tex = nullptr;
            ID3D12Resource* upload = nullptr;
            std::uint8_t* mapped = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows = 0;
            // The minimap's buffers are square (w == h); the full map's match the map
            // rectangle's aspect, so nothing is cut that is never sampled.
            int w = 0;
            int h = 0;
            bool needs_copy = false;      // filled by the CPU, copy not recorded yet
            bool in_copy_dest = true;     // resource state tracking for the barriers
            UINT64 in_flight_fence = 0;   // last frame that sampled it
        };

        // Per-pixel scratch for the PLANE-MAJOR slice pass, plus the destination ->
        // source index tables. One set per slicer (the minimap and the full map run at
        // different sizes and must not resize each other's buffers every frame).
        struct SliceScratch
        {
            std::vector<std::uint8_t> state;
            std::vector<float> best_ad;
            std::vector<float> best_d;
            std::vector<int> col_x;
            std::vector<int> row_y;

            void clear()
            {
                state.clear();
                best_ad.clear();
                best_d.clear();
                col_x.clear();
                row_y.clear();
            }
        };

        struct SliceCounts
        {
            std::uint32_t opaque = 0;
            std::uint32_t dim = 0;
            std::uint32_t faint = 0;
            int surfaces = 0;
        };

        Spinlock g_render_lock;

        ID3D12Device* g_device = nullptr;
        std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
        ID3D12GraphicsCommandList* g_cmd_list = nullptr;
        ID3D12DescriptorHeap* g_rtv_heap = nullptr;
        ID3D12Resource* g_backbuffers[kMaxBuffers]{};
        D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kMaxBuffers]{};
        FrameCtx g_frames[kMaxBuffers]{};
        ID3D12Fence* g_fence = nullptr;
        HANDLE g_fence_event = nullptr;
        UINT64 g_fence_value = 0;
        UINT g_buffer_count = 0;
        DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
        UINT g_width = 0;
        UINT g_height = 0;
        HWND g_hwnd = nullptr;
        WNDPROC g_prev_wndproc = nullptr;

        bool g_imgui_ready = false;
        bool g_rt_ready = false;
        bool g_failed = false;

        // The chapter composite - only loaded when fallback_use_composite = 1, or when
        // the height maps failed to load at all.
        MapTexture g_map;

        constexpr int kSliceBufs = 2;
        // Defaults for `slice_min_px` / `slice_max_px`, which is what slice_size_for()
        // actually reads.
        constexpr int kSliceMinPx = 128;
        constexpr int kSliceMaxPx = 1024;
        SliceBuf g_slice[kSliceBufs];
        int g_slice_next = 0;  // the buffer the next update writes
        int g_slice_shown = -1; // the buffer the minimap is drawing
        int g_slice_size = 0;   // side of the currently allocated buffers, px
        std::uint64_t g_slice_last_ms = 0;
        double g_slice_ms = 0.0;     // cost of the last slice, ms (EMA)
        double g_slice_ms_peak = 0.0;
        std::uint64_t g_slice_updates = 0;
        std::uint64_t g_slice_skipped = 0;
        // The window the SHOWN buffer covers, as a world->uv mapping.
        double g_slice_min_y = 0.0;
        double g_slice_max_x = 0.0;
        double g_slice_px_per_uu = 0.0;
        // Feet Z, EMA-smoothed so a jump or a step does not snap the whole picture.
        float g_feet_z = 0.0f;
        bool g_feet_z_valid = false;
        // Slice statistics, for the F2 debug block.
        std::uint32_t g_slice_opaque = 0;
        std::uint32_t g_slice_dim = 0;
        std::uint32_t g_slice_faint = 0;
        int g_slice_surfaces = 0; // height planes the slicer is reading
        SliceScratch g_slice_scratch;

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

        constexpr int kMapSliceBufs = 2;
        SliceBuf g_mslice[kMapSliceBufs];
        int g_mslice_next = 0;
        int g_mslice_shown = -1;
        SliceScratch g_mslice_scratch;
        SliceCounts g_mslice_counts{};
        double g_mslice_ms = 0.0;
        double g_mslice_ms_peak = 0.0;
        std::uint64_t g_mslice_updates = 0;
        std::uint64_t g_mslice_skipped = 0;
        std::uint64_t g_mslice_last_ms = 0;
        // The WORLD rectangle the shown buffer covers. X is north/south (screen up is
        // +X), Y is west/east - the same axes as everywhere else in this mod.
        double g_mr_x0 = 0.0; // south edge
        double g_mr_x1 = 0.0; // north edge
        double g_mr_y0 = 0.0; // west edge
        double g_mr_y1 = 0.0; // east edge
        bool g_mr_valid = false;
        double g_mr_zoom = 0.0;
        float g_mr_feet = 0.0f;
        double g_mr_px = 0.0; // the player position the cut was made at
        double g_mr_py = 0.0;
        std::string g_mr_chapter;

        // The view itself. Render thread only.
        mv::View g_mv{};
        bool g_mv_init = false;
        float g_map_floor_off = 0.0f; // uu added to feet Z by the floor adjustment
        bool g_map_was_open = false;
        // Set by the loop thread when the recentre key is pressed; consumed by the map.
        std::atomic<bool> g_map_recenter{false};
        // Manual found toggles are applied by the LOOP thread (it owns the master set
        // and the file), so the draw buffer only agrees a round later. These overrides
        // make the click feel instant and are dropped the moment the published buffer
        // says the same thing.
        std::vector<std::pair<std::string, bool>> g_found_override;
        // What the map is doing, for the F2 debug block and the map's own footer.
        int g_map_markers_drawn = 0;
        int g_map_markers_total = 0;

        // The swapchain we render on. Present can be called for more than one
        // swapchain (ReShade wraps its own, DLSS frame generation adds another), so the
        // first one that proves to be D3D12 wins and every other Present is ignored.
        IDXGISwapChain* g_swapchain = nullptr;
        int g_candidates_logged = 0;

        std::atomic<std::uint64_t> g_present_count{0};
        std::atomic<std::uint64_t> g_resize_count{0};
        // Set by the loop thread on an F5 reload; consumed on the render thread,
        // which is the only place a D3D12 resource may be released.
        std::atomic<bool> g_drop_textures{false};
        std::atomic<bool> g_hooks_installed{false};
        // Master-switch state. `g_hooks_created` is set once the MinHook trampolines
        // exist: a re-enable then only has to MH_EnableHook them, so no address can
        // ever be hooked twice. `g_render_stopped` is the render thread's answer to
        // "you have been switched off" (see shutdown_render()).
        bool g_hooks_created = false;                // loop thread only
        std::atomic<bool> g_render_stopped{true};    // render -> loop
        std::atomic<bool> g_watchdog_reported{false};
        std::uint64_t g_hook_install_ms = 0;

        // Written by the render thread, read by the F2 panel (same thread), so no
        // synchronisation needed - but it is also logged once from the loop thread, so
        // keep it a fixed buffer rather than a std::string being reallocated.
        wchar_t g_hide_reason[96] = L"not evaluated yet";
        std::wstring g_hook_report = L"not installed";

        // Every hide/show transition is logged with its reason, so one line in the log
        // pins "why did the minimap vanish" without a screenshot. Rate-limited: a
        // reason that has not changed is never logged again, and even a changing reason
        // is logged at most once per kReasonLogMs (a flapping condition must not be
        // able to flood the log the way "the settings panel rendered" once did).
        constexpr std::uint64_t kReasonLogMs = 2000; // the default of `hide_reason_log_ms`
        wchar_t g_reason_logged[96] = L"";
        std::uint64_t g_reason_log_ms = 0;
        std::uint64_t g_reason_since_ms = 0;
        std::uint64_t g_reason_suppressed = 0;

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
            if (now - g_reason_log_ms < static_cast<std::uint64_t>(mm::config().hide_reason_log_ms))
            {
                ++g_reason_suppressed;
                return;
            }
            const bool visible = ::wcscmp(text, L"visible") == 0;
            mm::logf(L"minimap {}: {} (previous state held {} ms{})",
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

        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
        using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

        PresentFn o_Present = nullptr;
        Present1Fn o_Present1 = nullptr;
        ResizeBuffersFn o_ResizeBuffers = nullptr;
        ExecuteCommandListsFn o_ExecuteCommandLists = nullptr;

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
        // WndProc hook
        //==============================================================================

        bool is_mouse_message(UINT msg)
        {
            return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE ||
                   msg == WM_NCMOUSEMOVE;
        }

        bool is_keyboard_message(UINT msg)
        {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                   msg == WM_CHAR || msg == WM_SETCURSOR;
        }

        LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            if (g_imgui_ready && ImGui::GetCurrentContext() != nullptr)
            {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

                // Input is only ever taken away from the game while the F2 panel or
                // the full map is up. With both closed the minimap is a pure overlay
                // and every message goes straight through, so gameplay input is
                // untouched - and because the test is a plain read of the two flags,
                // closing either one hands the input back on the very next message.
                // NOTHING IS LATCHED HERE (lessons.md).
                if (mm::g_map_open.load(std::memory_order_relaxed))
                {
                    // The map owns the whole keyboard and mouse: WASD pans it, and a
                    // click on a marker must not also swing the camera. io.WantCapture*
                    // is not enough - it is only true over an ImGui window, and the
                    // canvas deliberately reads raw keys rather than focusing a widget.
                    // The map's own toggle key is sampled with GetAsyncKeyState on the
                    // loop thread, so it still closes the map from here.
                    if (is_mouse_message(msg) || is_keyboard_message(msg))
                    {
                        return 1;
                    }
                }
                if (mm::g_panel_open.load(std::memory_order_relaxed))
                {
                    const ImGuiIO& io = ImGui::GetIO();
                    if ((io.WantCaptureMouse && is_mouse_message(msg)) ||
                        (io.WantCaptureKeyboard && is_keyboard_message(msg)))
                    {
                        return 1;
                    }
                }
            }
            if (g_prev_wndproc == nullptr)
            {
                return ::DefWindowProcW(hwnd, msg, wparam, lparam);
            }
            return ::CallWindowProcW(g_prev_wndproc, hwnd, msg, wparam, lparam);
        }

        //==============================================================================
        // Map texture upload
        //==============================================================================

        void destroy_texture(MapTexture& t)
        {
            if (t.srv_cpu.ptr != 0)
            {
                g_srv_heap.free(t.srv_cpu);
                t.srv_cpu = D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            safe_release(t.tex);
            safe_release(t.upload);
            t = MapTexture{};
        }

        void destroy_slice_set(SliceBuf* bufs, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (b.upload != nullptr && b.mapped != nullptr)
                {
                    b.upload->Unmap(0, nullptr);
                }
                if (b.srv_cpu.ptr != 0)
                {
                    g_srv_heap.free(b.srv_cpu);
                }
                safe_release(b.tex);
                safe_release(b.upload);
                b = SliceBuf{};
            }
        }

        void destroy_slice_buffers()
        {
            destroy_slice_set(g_slice, kSliceBufs);
            g_slice_size = 0;
            g_slice_next = 0;
            g_slice_shown = -1;
            g_slice_last_ms = 0;
        }

        void destroy_map_slice_buffers()
        {
            destroy_slice_set(g_mslice, kMapSliceBufs);
            g_mslice_next = 0;
            g_mslice_shown = -1;
            g_mslice_last_ms = 0;
            g_mr_valid = false;
        }

        void destroy_all_map_textures()
        {
            destroy_texture(g_map);
            destroy_slice_buffers();
            destroy_map_slice_buffers();
            g_feet_z_valid = false;
            g_slice_scratch.clear();
            g_mslice_scratch.clear();
        }

        // An upload buffer only has to live until the GPU has run the copy. Releasing
        // them matters now that a chapter uploads ten pictures: keeping them would park
        // ~100 MB of CPU-visible memory for the whole session.
        void release_finished_uploads()
        {
            if (g_fence == nullptr)
            {
                return;
            }
            const UINT64 done = g_fence->GetCompletedValue();
            const auto sweep = [done](MapTexture& t) {
                if (t.upload != nullptr && t.upload_fence != 0 && done >= t.upload_fence)
                {
                    safe_release(t.upload);
                }
            };
            sweep(g_map);
        }

        // Creates the texture and its upload buffer, and records the copy into
        // `list`. Called with the render lock held, from inside a frame.
        bool begin_map_upload(const mapdata::PendingImage& img, ID3D12GraphicsCommandList* list)
        {
            MapTexture& target = g_map;
            const DXGI_FORMAT format = img.channels == 1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            destroy_texture(target);

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(img.width);
            desc.Height = static_cast<UINT>(img.height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&target.tex))))
            {
                mm::logf(L"map texture: CreateCommittedResource({}x{}) failed", img.width, img.height);
                return false;
            }

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total = 0;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.Format = DXGI_FORMAT_UNKNOWN;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&target.upload))))
            {
                mm::logf(L"map texture: upload buffer of {} MB failed", total / (1024 * 1024));
                destroy_texture(target);
                return false;
            }

            void* mapped = nullptr;
            D3D12_RANGE none{0, 0};
            if (FAILED(target.upload->Map(0, &none, &mapped)) || mapped == nullptr)
            {
                mm::log(L"map texture: Map() of the upload buffer failed");
                destroy_texture(target);
                return false;
            }
            const std::size_t src_pitch = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.channels);
            for (UINT row = 0; row < num_rows; ++row)
            {
                std::memcpy(static_cast<std::uint8_t*>(mapped) + layout.Offset + row * layout.Footprint.RowPitch,
                            img.pixels.data() + row * src_pitch,
                            src_pitch);
            }
            target.upload->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = target.tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layout;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.tex;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);

            if (!g_srv_heap.alloc(target.srv_cpu, target.srv_gpu))
            {
                mm::log(L"map texture: no free SRV descriptor");
                destroy_texture(target);
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            // A layer is a single-channel coverage mask, so R is mapped to all four
            // components: ImGui's pixel shader then computes tint * (r, r, r, r), i.e.
            // "the floor colour, at this pixel's coverage". That is what lets the
            // runtime recolour a floor (bright for the current one, dim for the storey
            // below) and what makes an R8 texture - a quarter of the memory of RGBA8 -
            // enough for a full-resolution layer.
            srv.Shader4ComponentMapping =
                img.channels == 1 ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0)
                                  : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(target.tex, &srv, target.srv_cpu);

            target.width = img.width;
            target.height = img.height;
            target.channels = img.channels;
            target.bytes = static_cast<std::size_t>(total);
            target.chapter = img.chapter_key;
            target.ready = true;
            // The fence this frame will signal at the end of Present; once the GPU has
            // passed it, release_finished_uploads() frees the staging buffer.
            target.upload_fence = g_fence_value + 1;

            mm::logf(L"map texture: composite {}x{} uploaded (RGBA8, {} MB), chapter \"{}\"",
                     img.width,
                     img.height,
                     total / (1024 * 1024),
                     std::wstring(img.chapter_key.begin(), img.chapter_key.end()));
            return true;
        }

        //==============================================================================
        // Drawing: the minimap
        //==============================================================================

        // World -> texture uv for ONE image. The composite and every floor layer have
        // their own bounds (layers are cropped to their own footprint), so the mapping
        // travels with the picture instead of with the chapter.
        struct UvMap
        {
            double min_y = 0.0;
            double max_x = 0.0;
            double px_per_uu = 0.0;
            int width = 0;
            int height = 0;

            void to_uv(double wx, double wy, float& u, float& v) const
            {
                const double upx = (wy - min_y) * px_per_uu;
                const double vpx = (max_x - wx) * px_per_uu;
                u = width > 0 ? static_cast<float>(upx / width) : 0.0f;
                v = height > 0 ? static_cast<float>(vpx / height) : 0.0f;
            }
        };

        UvMap uv_of(const mapdata::Chapter& c)
        {
            return UvMap{c.min_y, c.max_x, c.px_per_uu, c.image_width, c.image_height};
        }

        struct MiniGeom
        {
            ImVec2 center{};
            float half = 0.0f;
            float cos_yaw = 1.0f;
            float sin_yaw = 0.0f;
            float zoom = 1.0f;
            UvMap uv{};
            double px = 0.0;
            double py = 0.0;
        };

        // Screen offset (dx, dy) in minimap pixels -> texture uv.
        //
        // Screen up is the player's forward when rotate_with_player is on, and world +X
        // (north) otherwise; screen right is the corresponding right vector. In UE, for
        // yaw a, forward = (cos a, sin a) and right = (-sin a, cos a). So
        //     world = player + forward * (-dy * zoom) + right * (dx * zoom)
        // which for yaw = 0 reduces to (X - dy*zoom, Y + dx*zoom): screen right -> +Y
        // (east -> right in the image) and screen up -> +X (north -> up). That is
        // exactly render.py / build_map.py's north-up convention.
        ImVec2 uv_at(const MiniGeom& g, float dx, float dy)
        {
            const double fx = g.cos_yaw;
            const double fy = g.sin_yaw;
            const double rx = -g.sin_yaw;
            const double ry = g.cos_yaw;
            const double wx = g.px + fx * (-dy * g.zoom) + rx * (dx * g.zoom);
            const double wy = g.py + fy * (-dy * g.zoom) + ry * (dx * g.zoom);
            float u = 0.0f;
            float v = 0.0f;
            g.uv.to_uv(wx, wy, u, v);
            return ImVec2{u, v};
        }

        void add_image_circle(ImDrawList* dl, ImTextureRef tex, const MiniGeom& g, ImU32 col)
        {
            dl->PushTexture(tex);
            dl->PrimReserve(g_circle_segments * 3, g_circle_segments + 1);
            const unsigned int base = dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(g.center, uv_at(g, 0.0f, 0.0f), col);
            for (int i = 0; i < g_circle_segments; ++i)
            {
                const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(g_circle_segments);
                const float dx = std::cos(a) * g.half;
                const float dy = std::sin(a) * g.half;
                dl->PrimWriteVtx(ImVec2{g.center.x + dx, g.center.y + dy}, uv_at(g, dx, dy), col);
            }
            for (int i = 0; i < g_circle_segments; ++i)
            {
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + ((i + 1) % g_circle_segments)));
            }
            dl->PopTexture();
        }

        void add_player_arrow(ImDrawList* dl, ImVec2 c, float angle_deg, float size)
        {
            // angle measured clockwise from screen up.
            const float a = angle_deg * kPi / 180.0f;
            const float dirx = std::sin(a);
            const float diry = -std::cos(a);
            const float perpx = std::cos(a);
            const float perpy = std::sin(a);
            const ImVec2 tip{c.x + dirx * size, c.y + diry * size};
            const ImVec2 l{c.x - dirx * size * 0.62f + perpx * size * 0.60f,
                           c.y - diry * size * 0.62f + perpy * size * 0.60f};
            const ImVec2 r{c.x - dirx * size * 0.62f - perpx * size * 0.60f,
                           c.y - diry * size * 0.62f - perpy * size * 0.60f};
            dl->AddTriangleFilled(tip, l, r, IM_COL32(255, 226, 92, 255));
            dl->AddTriangle(tip, l, r, IM_COL32(30, 26, 10, 220), 1.6f);
        }

        struct MiniDebug
        {
            bool visible = false;
            float u = 0.0f;
            float v = 0.0f;
            std::string chapter;
            float side = 0.0f;
        };

        MiniDebug g_last_mini{};

        //==============================================================================
        // The height slicer
        //==============================================================================
        //
        // WHY THIS REPLACED THE ORDINAL LAYERS (2026-09-02, round 3 of the in-world
        // feedback). The map used to ship one pre-rendered texture per per-pixel
        // surface ORDINAL plus a 640-uu grid of surface bands, and the runtime guessed
        // which ordinal the player's storey was from that grid. The guess is genuinely
        // ambiguous - a band names up to three ordinals - so a temple interior drew
        // several layers blended together and read as noise, exactly the failure mode
        // `CURRENT.md` had written down as the trigger for this rewrite.
        //
        // Now the asset carries the ACTUAL Z of up to four stacked surfaces per pixel
        // (mapdata::HeightMaps) and this code answers the question exactly, per pixel:
        //
        //     |Z - feetZ| <= floor_z_tolerance          -> the floor I am on, opaque
        //     nearest surface below within floor_fade_uu -> dim  (adjacent_floor_opacity)
        //     nearest surface above within floor_fade_uu -> faint (x 0.6)
        //     nothing                                    -> transparent
        //
        // and shades each pixel by (surfaceZ - feetZ) so slopes and staircases inside
        // one storey read as a gentle gradient instead of a flat silhouette. There are
        // no floor ranks, no bands and no per-position grid lookup left - the only
        // hysteresis is the EMA on feetZ.
        //
        // It runs on the CPU, at slice_hz, over only the window the minimap can show
        // (a ~512x512 source region), and uploads that window into a small dynamic
        // texture. That buys the shader path's exact semantics without a custom root
        // signature / PSO / D3DCompile on a ReShade-wrapped DX12 swapchain - see
        // CURRENT.md § Decisions. The shader path stays documented as a later
        // optimisation: it would move this loop to the GPU and drop the 512x512 upload.

        void slice_selftest();

        // Creates `count` dynamic RGBA textures of w x h with a persistently mapped
        // upload heap each. Shared by the minimap (square) and the full map
        // (rectangular): the resources, the barriers and the copy are identical, only
        // the size and the update policy differ.
        bool create_slice_set(SliceBuf* bufs, int count, int w, int h, const wchar_t* what)
        {
            destroy_slice_set(bufs, count);
            if (g_device == nullptr || w <= 0 || h <= 0)
            {
                return false;
            }

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(w);
            desc.Height = static_cast<UINT>(h);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total = 0;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.Format = DXGI_FORMAT_UNKNOWN;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&b.tex))))
                {
                    mm::logf(L"slice ({}): CreateCommittedResource({}x{} RGBA) failed", what, w, h);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&b.upload))))
                {
                    mm::logf(L"slice ({}): upload buffer of {} KB failed", what, total / 1024);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                void* mapped = nullptr;
                D3D12_RANGE none{0, 0};
                if (FAILED(b.upload->Map(0, &none, &mapped)) || mapped == nullptr)
                {
                    mm::logf(L"slice ({}): Map() of the upload buffer failed", what);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                b.mapped = static_cast<std::uint8_t*>(mapped);
                if (!g_srv_heap.alloc(b.srv_cpu, b.srv_gpu))
                {
                    mm::logf(L"slice ({}): no free SRV descriptor", what);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(b.tex, &srv, b.srv_cpu);

                b.footprint = layout;
                b.rows = num_rows;
                b.w = w;
                b.h = h;
                b.in_copy_dest = true;
                b.in_flight_fence = 0;
                b.needs_copy = false;
            }
            mm::logf(L"slice ({}): {} dynamic texture(s) of {}x{} RGBA created ({} KB each, {} KB of "
                     L"mapped upload memory)",
                     what,
                     count,
                     w,
                     h,
                     total / 1024,
                     (total * static_cast<UINT64>(count)) / 1024);
            return true;
        }

        bool create_slice_buffers(int size)
        {
            if (!create_slice_set(g_slice, kSliceBufs, size, size, L"minimap"))
            {
                destroy_slice_buffers();
                return false;
            }
            g_slice_size = size;
            g_slice_next = 0;
            g_slice_shown = -1;
            return true;
        }

        // How many source pixels the minimap can show, including the rotation corners
        // and a margin so the CLAMP sampler never smears an edge into view.
        int slice_size_for(const mm::Config& cfg, const mapdata::HeightMaps& hm, float half_px)
        {
            const double radius_uu = static_cast<double>(half_px) * static_cast<double>(cfg.zoom_uu_per_px) *
                                     1.4143; // the diagonal of the square the disc rotates in
            double want = 2.0 * radius_uu * hm.px_per_uu + 16.0;
            int size = static_cast<int>(std::ceil(want / 128.0)) * 128;
            if (size < cfg.slice_min_px)
            {
                size = cfg.slice_min_px;
            }
            if (size > cfg.slice_max_px)
            {
                size = cfg.slice_max_px;
            }
            return size;
        }

        // One pixel's colour, from the up-to-four surface Z values under it.
        //
        // `state` 3 = the floor the player is on, 2 = the nearest surface below,
        // 1 = the nearest above, 0 = nothing. The gradient is the SAME rule offline
        // (tools/navmesh/slice_preview.py) so a reported spot can be reproduced
        // without the game:
        //     lum = 1 + gradient_strength * clamp((surfaceZ - feetZ) / span, -1, +1)
        // with span = tolerance for the current floor and fade for the dim ones.
        struct SliceStyle
        {
            float base_r = 214.0f;
            float base_g = 208.0f;
            float base_b = 196.0f;
            float strength = 0.18f;
            float tol = 200.0f;
            float fade = 800.0f;
            float a_dim = 0.25f;
            float a_faint = 0.15f;
        };

        // Fills `dst` (size*size RGBA8, row pitch `pitch`) with the window whose
        // top-left source pixel is (x0, y0).
        //
        // PLANE-MAJOR, deliberately. The obvious pixel-major loop reads all eight
        // planes at one pixel before moving on - eight addresses ~43 MB apart, i.e. one
        // cache miss per plane per pixel, ~2.1 M misses for a 512x512 window. Walking
        // one plane's window to completion instead touches 512 contiguous uint16 per
        // row, so the whole pass is ~65 k cache lines: the same arithmetic, ~30x fewer
        // misses. The per-pixel decision state lives in ~1.3 MB of scratch, which fits
        // in L2/L3.
        // `sx0` / `sy0` are the source pixel of the destination's top-left CORNER and
        // `src_step` is how many source pixels one destination pixel advances - 1.0 for
        // the minimap (which shows the asset at its own resolution) and > 1 for the full
        // map, which decimates. Sampling is nearest, at the destination pixel's centre;
        // at a decimating step a 1-px corridor can drop out, but at that zoom it is
        // sub-pixel anyway, and the alternative (scanning every source pixel of the
        // region) is ~30x the work for a picture nobody can resolve.
        void slice_region(const mapdata::HeightMaps& hm, double sx0, double sy0, double src_step, int w, int h,
                          std::uint8_t* dst, UINT pitch, float feet, const SliceStyle& st, SliceScratch& sc,
                          SliceCounts& counts)
        {
            counts = SliceCounts{};

            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            if (sc.state.size() != n)
            {
                sc.state.assign(n, 0);
                sc.best_ad.assign(n, 0.0f);
                sc.best_d.assign(n, 0.0f);
            }
            else
            {
                std::memset(sc.state.data(), 0, n);
            }
            std::uint8_t* state = sc.state.data();
            float* best_ad = sc.best_ad.data();
            float* best_d = sc.best_d.data();

            // Destination -> source index, computed once instead of once per plane. It
            // is also where the bounds check lives: -1 means "outside the asset", which
            // comes out transparent.
            if (sc.col_x.size() != static_cast<std::size_t>(w))
            {
                sc.col_x.resize(static_cast<std::size_t>(w));
            }
            if (sc.row_y.size() != static_cast<std::size_t>(h))
            {
                sc.row_y.resize(static_cast<std::size_t>(h));
            }
            int* col_x = sc.col_x.data();
            int* row_y = sc.row_y.data();
            for (int col = 0; col < w; ++col)
            {
                const int i = static_cast<int>(std::floor(sx0 + (static_cast<double>(col) + 0.5) * src_step));
                col_x[col] = (i >= 0 && i < hm.width) ? i : -1;
            }
            for (int row = 0; row < h; ++row)
            {
                const int i = static_cast<int>(std::floor(sy0 + (static_cast<double>(row) + 0.5) * src_step));
                row_y[row] = (i >= 0 && i < hm.height) ? i : -1;
            }

            const float z0 = hm.z_min;
            const float step = hm.z_step();
            const int planes = hm.count < mapdata::kMaxSurfaces ? hm.count : mapdata::kMaxSurfaces;
            counts.surfaces = planes;

            for (int k = 0; k < planes; ++k)
            {
                const std::uint16_t* plane = hm.plane[k].data();
                if (hm.plane[k].empty())
                {
                    continue;
                }
                for (int row = 0; row < h; ++row)
                {
                    const int sy = row_y[row];
                    if (sy < 0)
                    {
                        continue;
                    }
                    const std::uint16_t* src =
                        plane + static_cast<std::size_t>(sy) * static_cast<std::size_t>(hm.width);
                    const std::size_t out_base = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                    for (int col = 0; col < w; ++col)
                    {
                        const int sx = col_x[col];
                        if (sx < 0)
                        {
                            continue;
                        }
                        const std::uint16_t code = src[sx];
                        if (code == 0)
                        {
                            continue; // no surface in this slot here
                        }
                        const float z = z0 + (static_cast<float>(code) - 1.0f) * step;
                        const float d = z - feet;
                        const float ad = d < 0.0f ? -d : d;
                        std::uint8_t cand = 0;
                        if (ad <= st.tol)
                        {
                            cand = 3; // the floor I am standing on
                        }
                        else if (ad <= st.fade)
                        {
                            cand = d < 0.0f ? 2 : 1; // below / above, dimmed
                        }
                        else
                        {
                            continue;
                        }
                        const std::size_t i = out_base + static_cast<std::size_t>(col);
                        // Class first, then "nearest": exactly the offline rule in
                        // tools/navmesh/slice_preview.py (slice_window + shade).
                        if (cand > state[i] || (cand == state[i] && ad < best_ad[i]))
                        {
                            state[i] = cand;
                            best_ad[i] = ad;
                            best_d[i] = d;
                        }
                    }
                }
            }

            const std::uint8_t a_dim = static_cast<std::uint8_t>(st.a_dim * 255.0f + 0.5f);
            const std::uint8_t a_faint = static_cast<std::uint8_t>(st.a_faint * 255.0f + 0.5f);
            for (int row = 0; row < h; ++row)
            {
                std::uint8_t* out = dst + static_cast<std::size_t>(row) * pitch;
                const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                for (int col = 0; col < w; ++col)
                {
                    std::uint8_t* px = out + static_cast<std::size_t>(col) * 4;
                    const std::size_t i = base + static_cast<std::size_t>(col);
                    const std::uint8_t cls = state[i];
                    if (cls == 0)
                    {
                        px[0] = px[1] = px[2] = px[3] = 0;
                        continue;
                    }
                    // lum = 1 + strength * clamp(d / span, -1, +1), span = tol for my own
                    // floor and fade for the dimmed ones - so a ramp or a staircase
                    // inside one storey reads as a gentle gradient, and the dimmed
                    // storeys are shaded by how far away they are.
                    const float span = cls == 3 ? st.tol : st.fade;
                    float t = span > 0.0f ? best_d[i] / span : 0.0f;
                    t = t < -1.0f ? -1.0f : (t > 1.0f ? 1.0f : t);
                    const float lum = 1.0f + st.strength * t;
                    const auto ch = [lum](float v) {
                        const float x = v * lum + 0.5f;
                        return static_cast<std::uint8_t>(x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x));
                    };
                    px[0] = ch(st.base_r);
                    px[1] = ch(st.base_g);
                    px[2] = ch(st.base_b);
                    if (cls == 3)
                    {
                        px[3] = 255;
                        ++counts.opaque;
                    }
                    else if (cls == 2)
                    {
                        px[3] = a_dim;
                        ++counts.dim;
                    }
                    else
                    {
                        px[3] = a_faint;
                        ++counts.faint;
                    }
                }
            }
        }

        // The minimap's window: the asset at 1:1, at an integral source origin.
        void slice_window(const mapdata::HeightMaps& hm, int x0, int y0, int size, std::uint8_t* dst, UINT pitch,
                          float feet, const SliceStyle& st)
        {
            SliceCounts counts{};
            slice_region(hm, static_cast<double>(x0), static_cast<double>(y0), 1.0, size, size, dst, pitch, feet,
                         st, g_slice_scratch, counts);
            g_slice_opaque = counts.opaque;
            g_slice_dim = counts.dim;
            g_slice_faint = counts.faint;
            g_slice_surfaces = counts.surfaces;
        }

        // Re-slices into the next buffer if it is time and that buffer is free.
        // Returns true when a buffer is available to draw (this frame's or the previous
        // one's - the window has margin, so a skipped update is invisible).
        bool update_slice(const mm::Config& cfg, const mapdata::Chapter& ch, const mm::Snapshot& snap,
                          float half_px, std::uint64_t now)
        {
            if (!ch.has_heights())
            {
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;

            // ---- feet Z, EMA-smoothed ------------------------------------------------
            const float raw_feet = static_cast<float>(snap.z) - cfg.player_z_offset;
            static std::uint64_t last_teleport = 0;
            const bool teleported = snap.teleport_ms != 0 && snap.teleport_ms != last_teleport;
            if (teleported)
            {
                last_teleport = snap.teleport_ms;
            }
            if (!g_feet_z_valid || teleported)
            {
                g_feet_z = raw_feet;
                g_feet_z_valid = true;
                g_slice_last_ms = 0; // a teleport must re-slice on this very frame
            }
            else
            {
                const float tau = cfg.feet_z_smooth_ms > 1 ? static_cast<float>(cfg.feet_z_smooth_ms) : 1.0f;
                // One frame is ~16 ms; the exact dt does not matter for a 100 ms EMA.
                const float a = 16.0f / tau;
                g_feet_z += (raw_feet - g_feet_z) * (a > 1.0f ? 1.0f : a);
            }

            // ---- (re)allocate when the needed window size changes --------------------
            const int want = slice_size_for(cfg, hm, half_px);
            if (want != g_slice_size)
            {
                wait_for_gpu(); // the old buffers may still be in flight
                if (!create_slice_buffers(want))
                {
                    return false;
                }
            }

            const int period = cfg.slice_hz > 0 ? 1000 / cfg.slice_hz : 80;
            if (g_slice_shown >= 0 && now - g_slice_last_ms < static_cast<std::uint64_t>(period))
            {
                return true; // the previous window is still good enough
            }

            SliceBuf& b = g_slice[g_slice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0)
            {
                return g_slice_shown >= 0;
            }
            if (b.in_flight_fence != 0 && g_fence != nullptr &&
                g_fence->GetCompletedValue() < b.in_flight_fence)
            {
                // The GPU is still sampling this one. Never stall Present for the map:
                // keep showing the other buffer and try again next frame.
                ++g_slice_skipped;
                return g_slice_shown >= 0;
            }

            double pxc = 0.0;
            double pyc = 0.0;
            hm.to_px(snap.x, snap.y, pxc, pyc);
            const int x0 = static_cast<int>(std::lround(pxc)) - b.w / 2;
            const int y0 = static_cast<int>(std::lround(pyc)) - b.w / 2;

            SliceStyle st{};
            st.base_r = cfg.floor_base_r;
            st.base_g = cfg.floor_base_g;
            st.base_b = cfg.floor_base_b;
            st.strength = cfg.floor_gradient_strength;
            st.tol = cfg.floor_z_tolerance;
            st.fade = cfg.floor_fade_uu;
            st.a_dim = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity : 0.0f;
            st.a_faint = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity * 0.6f : 0.0f;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            LARGE_INTEGER freq{};
            ::QueryPerformanceFrequency(&freq);
            ::QueryPerformanceCounter(&t0);
            slice_window(hm, x0, y0, b.w, b.mapped + b.footprint.Offset, b.footprint.Footprint.RowPitch,
                         g_feet_z, st);
            ::QueryPerformanceCounter(&t1);
            if (freq.QuadPart > 0)
            {
                const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                  static_cast<double>(freq.QuadPart);
                g_slice_ms = g_slice_ms == 0.0 ? ms : g_slice_ms * 0.8 + ms * 0.2;
                if (ms > g_slice_ms_peak)
                {
                    g_slice_ms_peak = ms;
                }
            }

            b.needs_copy = true;
            g_slice_shown = g_slice_next;
            g_slice_next = (g_slice_next + 1) % kSliceBufs;
            g_slice_last_ms = now;
            ++g_slice_updates;
            // The window's own world -> pixel mapping (see mapdata::HeightMaps::to_px):
            //   px_local = px - x0 = (Y - (min_y + x0/s)) * s
            //   py_local = py - y0 = ((max_x - y0/s) - X) * s
            g_slice_px_per_uu = hm.px_per_uu;
            g_slice_min_y = hm.min_y + static_cast<double>(x0) / hm.px_per_uu;
            g_slice_max_x = hm.max_x - static_cast<double>(y0) / hm.px_per_uu;
            return true;
        }

        // MAIN-MENU SELF-TEST. The slicer only ever runs inside draw_minimap, which is
        // gated on a gameplay pawn - so at the main menu neither the D3D12 resources nor
        // the CPU loop is exercised, and a verification run there could only say "it did
        // not crash". This allocates the buffers and slices one window at the chapter's
        // centre so a Lobby log line proves the whole path: texture + mapped upload heap
        // created, the loop ran, and what it cost. It also removes the first-frame hitch
        // in-world, since the buffers already exist.
        void slice_selftest()
        {
            const mm::Config cfg = mm::config();
            std::vector<mapdata::Chapter> list = mapdata::chapters();
            const mapdata::Chapter* ch = nullptr;
            for (const mapdata::Chapter& c : list)
            {
                if (c.has_heights())
                {
                    ch = &c;
                    break;
                }
            }
            // Size it for the shipping window so the in-world path needs no realloc.
            const float side = (std::max)(72.0f, cfg.size_frac * static_cast<float>(g_height));
            const int size = ch != nullptr ? slice_size_for(cfg, *ch->heights, side * 0.5f) : 512;
            if (!create_slice_buffers(size))
            {
                return;
            }
            if (ch == nullptr)
            {
                mm::log(L"slice: self-test skipped - no chapter with height maps is loaded");
                return;
            }

            const mapdata::HeightMaps& hm = *ch->heights;
            SliceBuf& b = g_slice[0];
            SliceStyle st{};
            st.base_r = cfg.floor_base_r;
            st.base_g = cfg.floor_base_g;
            st.base_b = cfg.floor_base_b;
            st.strength = cfg.floor_gradient_strength;
            st.tol = cfg.floor_z_tolerance;
            st.fade = cfg.floor_fade_uu;
            st.a_dim = cfg.adjacent_floor_opacity;
            st.a_faint = cfg.adjacent_floor_opacity * 0.6f;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            LARGE_INTEGER freq{};
            ::QueryPerformanceFrequency(&freq);
            ::QueryPerformanceCounter(&t0);
            // Slice a window that actually HAS geometry in it, at a feet Z taken from
            // that geometry - a window over empty map at the mid-Z of the chapter comes
            // out fully transparent and proves nothing about the colour path.
            const int wx0 = (hm.width - b.w) / 2;
            const int wy0 = (hm.height - b.w) / 2;
            float probe_z = (hm.z_min + hm.z_max) * 0.5f;
            int sx0 = wx0;
            int sy0 = wy0;
            {
                const std::vector<std::uint16_t>& p0 = hm.plane[0];
                for (std::size_t i = 0; i < p0.size(); i += 97) // a coarse stride is plenty
                {
                    if (p0[i] != 0)
                    {
                        const int px = static_cast<int>(i % static_cast<std::size_t>(hm.width));
                        const int py = static_cast<int>(i / static_cast<std::size_t>(hm.width));
                        probe_z = hm.decode(p0[i]);
                        sx0 = px - b.w / 2;
                        sy0 = py - b.w / 2;
                        break;
                    }
                }
            }
            slice_window(hm, sx0, sy0, b.w, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, probe_z, st);
            ::QueryPerformanceCounter(&t1);
            const double ms = freq.QuadPart > 0 ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                                      static_cast<double>(freq.QuadPart)
                                                : 0.0;
            // Deliberately NOT marked needs_copy: nothing may be drawn at the main menu.
            mm::logf(L"slice: self-test sliced a {}x{} window of \"{}\" at source ({}, {}), feet Z "
                     L"{:.0f}, over {} surface(s) in {:.2f} ms (opaque {}, dim {}, faint {}) - the CPU "
                     L"path and the dynamic texture both work",
                     b.w,
                     b.w,
                     std::wstring(ch->key.begin(), ch->key.end()),
                     sx0 + b.w / 2,
                     sy0 + b.w / 2,
                     static_cast<double>(probe_z),
                     hm.count,
                     ms,
                     g_slice_opaque,
                     g_slice_dim,
                     g_slice_faint);
            g_slice_opaque = 0;
            g_slice_dim = 0;
            g_slice_faint = 0;
        }

        // Render thread, inside a frame, after the command list has been reset: record
        // the copy for whichever buffer the CPU just filled.
        void record_slice_copies(ID3D12GraphicsCommandList* list, SliceBuf* bufs, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (!b.needs_copy || b.tex == nullptr)
                {
                    continue;
                }
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = b.tex;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (!b.in_copy_dest)
                {
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    list->ResourceBarrier(1, &barrier);
                }

                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = b.tex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = b.upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = b.footprint;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &barrier);
                b.in_copy_dest = false;
                b.needs_copy = false;
            }
        }

        void record_slice_copy(ID3D12GraphicsCommandList* list)
        {
            record_slice_copies(list, g_slice, kSliceBufs);
            record_slice_copies(list, g_mslice, kMapSliceBufs);
        }

        //==============================================================================
        // Drawing one image (the composite or one floor layer)
        //==============================================================================

        void draw_srv(ImDrawList* dl, D3D12_GPU_DESCRIPTOR_HANDLE srv, const UvMap& uv, MiniGeom g, ImU32 col,
                      bool round, float x0, float y0, float side)
        {
            if (srv.ptr == 0)
            {
                return;
            }
            g.uv = uv;
            const ImTextureRef tex{static_cast<ImTextureID>(srv.ptr)};
            if (round)
            {
                add_image_circle(dl, tex, g, col);
                return;
            }
            const ImVec2 p0{x0, y0};
            const ImVec2 p1{x0 + side, y0 + side};
            dl->AddImageQuad(tex,
                             p0,
                             ImVec2{p1.x, p0.y},
                             p1,
                             ImVec2{p0.x, p1.y},
                             uv_at(g, -g.half, -g.half),
                             uv_at(g, g.half, -g.half),
                             uv_at(g, g.half, g.half),
                             uv_at(g, -g.half, g.half),
                             col);
        }

        void draw_image(ImDrawList* dl, const MapTexture& t, const UvMap& uv, const MiniGeom& g, ImU32 col,
                        bool round, float x0, float y0, float side)
        {
            if (!t.ready)
            {
                return;
            }
            draw_srv(dl, t.srv_gpu, uv, g, col, round, x0, y0, side);
        }

        // mm::key_name is wide (the log is wide); ImGui is UTF-8. Key names are pure
        // ASCII, so this is a cast per character - but it has to be an EXPLICIT one:
        // std::string(w.begin(), w.end()) compiles and warns (C4244), and this mod
        // ships warning-free.
        std::string key_name_ascii(int vk)
        {
            const std::wstring wide = mm::key_name(vk);
            std::string out;
            out.reserve(wide.size());
            for (const wchar_t c : wide)
            {
                out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return out;
        }

        std::string wide_to_ascii(const std::wstring& wide)
        {
            std::string out;
            out.reserve(wide.size());
            for (const wchar_t c : wide)
            {
                out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return out;
        }

        // THE KEY HINTS. Built from the CONFIG, never from the defaults, so a rebound key
        // is what the player is told - both in the F2 panel and along the bottom of the
        // full map. One builder, so the two can never drift apart.
        std::string bindings_hint(const mm::Config& cfg)
        {
            std::string s = std::format("{} panel   {} full map ({} recentres)   {} reload",
                                        key_name_ascii(cfg.panel_key),
                                        key_name_ascii(cfg.map_key),
                                        key_name_ascii(cfg.map_recenter_key),
                                        key_name_ascii(cfg.reload_key));
            if (cfg.highlight_enabled)
            {
                s += std::format("   hold {} x-ray", key_name_ascii(cfg.highlight_key));
                if (cfg.highlight_gamepad &&
                    (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt))
                {
                    s += " (pad " +
                         wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt,
                                                          cfg.highlight_pad_rt)) +
                         ")";
                }
            }
            return s;
        }

        //==============================================================================
        // Drawing: markers
        //==============================================================================
        //
        // Glyphs are drawn with ImDrawList primitives, not from an image atlas: the mod
        // ships no marker art, there is no texture to keep in sync with the categories,
        // and a vector glyph stays sharp at any minimap size. Each category gets a
        // shape AND a colour, because a dimmed "found" marker loses most of its colour
        // contrast and the shape is what still tells it apart.
        //
        // World -> minimap pixels is the inverse of uv_at(): for yaw a,
        // forward = (cos a, sin a) and right = (-sin a, cos a), so
        //     wdx = -s*(dx*z) - c*(dy*z)
        //     wdy =  c*(dx*z) - s*(dy*z)
        // whose inverse (the matrix is a rotation, det = 1) is
        //     dx = (-s*wdx + c*wdy) / z
        //     dy = (-c*wdx - s*wdy) / z
        // At yaw 0 that is dx = wdy/z (east to the right) and dy = -wdx/z (north up),
        // i.e. exactly build_map.py's north-up convention.

        // Defined with the rest of the full map, below - the minimap draws the same
        // glyph, edge-clamped, so the two views agree on what a waypoint looks like.
        void draw_waypoint_glyph(ImDrawList* dl, ImVec2 p, float r, int alpha);

        ImU32 marker_color(mdb::Cat cat, int alpha)
        {
            switch (cat)
            {
            case mdb::Cat::Shrine:
                return IM_COL32(255, 186, 72, alpha);
            case mdb::Cat::Chest:
                return IM_COL32(255, 226, 120, alpha);
            case mdb::Cat::Pickup:
                return IM_COL32(120, 220, 255, alpha);
            case mdb::Cat::Boss:
                return IM_COL32(255, 86, 86, alpha);
            case mdb::Cat::Elite:
                return IM_COL32(255, 140, 80, alpha);
            case mdb::Cat::Enemy:
                return IM_COL32(232, 96, 96, alpha);
            case mdb::Cat::Npc:
                return IM_COL32(140, 235, 140, alpha);
            case mdb::Cat::Merchant:
                return IM_COL32(120, 230, 210, alpha);
            case mdb::Cat::Door:
                return IM_COL32(172, 194, 224, alpha);
            case mdb::Cat::Ladder:
            case mdb::Cat::Lift:
                return IM_COL32(206, 184, 142, alpha);
            case mdb::Cat::FogGate:
                return IM_COL32(198, 150, 255, alpha);
            case mdb::Cat::Hidden:
                return IM_COL32(255, 130, 220, alpha);
            case mdb::Cat::Other:
            default:
                return IM_COL32(196, 196, 196, alpha);
            }
        }

        void draw_marker_glyph(ImDrawList* dl, mdb::Cat cat, ImVec2 p, float r, ImU32 col, ImU32 edge)
        {
            const auto tri = [&](float scale) {
                const ImVec2 a{p.x, p.y - r * scale};
                const ImVec2 b{p.x - r * scale * 0.92f, p.y + r * scale * 0.72f};
                const ImVec2 c{p.x + r * scale * 0.92f, p.y + r * scale * 0.72f};
                dl->AddTriangleFilled(a, b, c, col);
                dl->AddTriangle(a, b, c, edge, 1.2f);
            };
            const auto rect = [&](float w, float h) {
                const ImVec2 a{p.x - r * w, p.y - r * h};
                const ImVec2 b{p.x + r * w, p.y + r * h};
                dl->AddRectFilled(a, b, col, 1.5f);
                dl->AddRect(a, b, edge, 1.5f, 0, 1.2f);
            };

            switch (cat)
            {
            case mdb::Cat::Shrine:
                // A diamond: AddNgon starts at angle 0, so a 4-gon has its vertices on
                // the axes.
                dl->AddNgonFilled(p, r * 1.15f, col, 4);
                dl->AddNgon(p, r * 1.15f, edge, 4, 1.4f);
                dl->AddCircleFilled(p, r * 0.32f, edge, 8);
                break;
            case mdb::Cat::Chest:
                rect(0.95f, 0.75f);
                dl->AddLine(ImVec2{p.x - r * 0.95f, p.y}, ImVec2{p.x + r * 0.95f, p.y}, edge, 1.2f);
                break;
            case mdb::Cat::Pickup:
                dl->AddCircleFilled(p, r * 0.72f, col, 10);
                dl->AddCircle(p, r * 0.72f, edge, 10, 1.2f);
                break;
            case mdb::Cat::Boss:
                tri(1.5f);
                break;
            case mdb::Cat::Elite:
                tri(1.15f);
                break;
            case mdb::Cat::Enemy:
                tri(0.85f);
                break;
            case mdb::Cat::Npc:
                dl->AddCircleFilled(p, r * 0.7f, col, 12);
                dl->AddCircle(p, r * 0.95f, col, 12, 1.3f);
                break;
            case mdb::Cat::Merchant:
                dl->AddCircleFilled(p, r * 0.8f, col, 12);
                dl->AddCircleFilled(p, r * 0.3f, edge, 8);
                break;
            case mdb::Cat::Door:
                rect(0.55f, 0.95f);
                break;
            case mdb::Cat::Ladder:
                dl->AddLine(ImVec2{p.x - r * 0.5f, p.y - r}, ImVec2{p.x - r * 0.5f, p.y + r}, col, 1.6f);
                dl->AddLine(ImVec2{p.x + r * 0.5f, p.y - r}, ImVec2{p.x + r * 0.5f, p.y + r}, col, 1.6f);
                for (int i = -1; i <= 1; ++i)
                {
                    const float y = p.y + static_cast<float>(i) * r * 0.55f;
                    dl->AddLine(ImVec2{p.x - r * 0.5f, y}, ImVec2{p.x + r * 0.5f, y}, col, 1.2f);
                }
                break;
            case mdb::Cat::Lift:
                rect(0.85f, 0.5f);
                dl->AddTriangleFilled(ImVec2{p.x, p.y - r * 1.35f},
                                      ImVec2{p.x - r * 0.5f, p.y - r * 0.6f},
                                      ImVec2{p.x + r * 0.5f, p.y - r * 0.6f},
                                      col);
                break;
            case mdb::Cat::FogGate:
                dl->AddCircle(p, r, col, 14, 2.0f);
                dl->AddLine(ImVec2{p.x - r * 0.7f, p.y}, ImVec2{p.x + r * 0.7f, p.y}, col, 1.4f);
                break;
            case mdb::Cat::Hidden:
                dl->AddNgon(p, r * 1.1f, col, 4, 1.8f);
                break;
            case mdb::Cat::Other:
            default:
                dl->AddCircleFilled(p, r * 0.5f, col, 8);
                break;
            }
        }

        struct MarkerDrawStats
        {
            int total = 0;
            int drawn = 0;
            int clamped = 0;
            int filtered = 0;
            std::string nearest;
            float nearest_uu = 0.0f;
        };

        MarkerDrawStats g_marker_draw{};

        void draw_markers(const mm::Config& cfg, const MiniGeom& g, bool round, float x0, float y0, float side,
                          ImDrawList* dl)
        {
            g_marker_draw = MarkerDrawStats{};
            if (!cfg.markers_enabled)
            {
                return;
            }
            const markers::View v = markers::view();
            g_marker_draw.total = static_cast<int>(v.count);
            if (v.data == nullptr || v.count == 0)
            {
                return;
            }

            const double c = g.cos_yaw;
            const double s = g.sin_yaw;
            const double z = g.zoom > 0.0001f ? static_cast<double>(g.zoom) : 1.0;
            const float r = cfg.markers_size;
            const float limit = (std::max)(4.0f, g.half - r - 2.0f);

            struct Cand
            {
                float dx = 0.0f;
                float dy = 0.0f;
                float d2 = 0.0f;
                std::uint8_t cat = 0;
                bool found = false;
                bool clamped = false;
                const char* id = nullptr;
            };
            // Render thread only, and reused frame to frame so a full minimap never
            // allocates during Present.
            static std::vector<Cand> cands;
            cands.clear();

            for (std::size_t i = 0; i < v.count; ++i)
            {
                const markers::DrawMarker& m = v.data[i];
                const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                if (static_cast<int>(m.cat) >= mdb::kCatCount || !mdb::cat_enabled(cfg.markers_categories, cat))
                {
                    ++g_marker_draw.filtered;
                    continue;
                }
                const bool found = (m.flags & markers::kFlagFound) != 0;
                if (found && cfg.markers_hide_found)
                {
                    ++g_marker_draw.filtered;
                    continue;
                }

                const double wdx = m.x - g.px;
                const double wdy = m.y - g.py;
                double dx = (-s * wdx + c * wdy) / z;
                double dy = (-c * wdx - s * wdy) / z;

                bool clamped = false;
                if (round)
                {
                    const double d = std::sqrt(dx * dx + dy * dy);
                    if (d > limit)
                    {
                        if (!cfg.markers_clamp_to_edge || d <= 0.0001)
                        {
                            continue;
                        }
                        dx = dx * limit / d;
                        dy = dy * limit / d;
                        clamped = true;
                    }
                }
                else if (std::abs(dx) > limit || std::abs(dy) > limit)
                {
                    if (!cfg.markers_clamp_to_edge)
                    {
                        continue;
                    }
                    const double scale = limit / (std::max)(std::abs(dx), std::abs(dy));
                    dx *= scale;
                    dy *= scale;
                    clamped = true;
                }

                Cand cand{};
                cand.dx = static_cast<float>(dx);
                cand.dy = static_cast<float>(dy);
                cand.d2 = static_cast<float>(wdx * wdx + wdy * wdy);
                cand.cat = m.cat;
                cand.found = found;
                cand.clamped = clamped;
                cand.id = m.id;
                cands.push_back(cand);
            }

            // Nearest first, so the cap drops the far ones and the near ones draw last
            // (on top).
            const std::size_t cap = cfg.markers_max_draw > 0
                                        ? static_cast<std::size_t>(cfg.markers_max_draw)
                                        : cands.size();
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 > b.d2; });

            const float op = cfg.opacity;
            for (const Cand& cand : cands)
            {
                const float a = op * (cand.found ? cfg.markers_found_alpha : 1.0f);
                if (a <= 0.01f)
                {
                    continue;
                }
                const int alpha = static_cast<int>((std::min)(1.0f, a) * 255.0f + 0.5f);
                const ImU32 col = marker_color(static_cast<mdb::Cat>(cand.cat), alpha);
                const ImU32 edge = IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f));
                const ImVec2 p{g.center.x + cand.dx, g.center.y + cand.dy};
                draw_marker_glyph(dl, static_cast<mdb::Cat>(cand.cat), p, cand.clamped ? r * 0.72f : r, col, edge);
                ++g_marker_draw.drawn;
                g_marker_draw.clamped += cand.clamped ? 1 : 0;
            }
            if (!cands.empty())
            {
                // cands is sorted far -> near, so the last one is the nearest.
                const Cand& near_one = cands.back();
                g_marker_draw.nearest = near_one.id != nullptr ? near_one.id : "";
                g_marker_draw.nearest_uu = std::sqrt(near_one.d2);
            }
            (void)x0;
            (void)y0;
            (void)side;
        }

        //==============================================================================
        // THE ONE HUD GATE
        //==============================================================================
        //
        // "May anything of ours be on screen right now?" - the minimap, the compass and
        // the x-ray highlight all ask exactly this, so it is evaluated exactly once, in
        // one place, from live state only. It returns nullptr when everything of ours may
        // draw, or the reason it may not, and the caller decides what to do with that:
        // the minimap feeds it to set_hide_reason() (which owns the "hidden because"
        // readout and the transition log lines), the compass and the highlight simply do
        // not draw. Adding a second set of show/hide rules for the compass is exactly the
        // shape of bug lessons.md warns about, so there isn't one.
        //
        // Nothing in here is remembered between frames. Every condition is recomputed
        // from the snapshot the game thread published, which is what makes hiding
        // immediate and makes "it got stuck hidden" impossible.
        const wchar_t* hud_gate(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state,
                                std::uint64_t now)
        {
            if (!have_state)
            {
                return L"no game-state snapshot yet";
            }
            if (snap.stamp_ms == 0 || now - snap.stamp_ms > static_cast<std::uint64_t>(cfg.state_stale_ms))
            {
                return L"game state is stale (game thread not pumping)";
            }
            if (!snap.transition && !snap.has_pawn)
            {
                return L"no player pawn";
            }
            if (snap.transition)
            {
                return L"level transition in progress (reader idling)";
            }
            // The class gate lives on the game thread; this is its render-side echo.
            if (!snap.pawn_is_gameplay)
            {
                return L"the pawn is not a gameplay pawn (Lobby / spectator)";
            }
            // A fresh gameplay pawn must have been valid for a while before anything is
            // drawn: without this the first snapshot after a load can flash the minimap.
            if (snap.state_ok_since_ms == 0)
            {
                return L"waiting for a valid gameplay state";
            }
            if (now - snap.state_ok_since_ms < static_cast<std::uint64_t>(cfg.min_visible_after_state_ok_ms))
            {
                return L"gameplay state is too fresh (grace period)";
            }
            // HIDING IS IMMEDIATE: the game thread re-tests the cached in-viewport menu
            // roots on every pump (10 Hz), so this is true within ~100 ms of the
            // inventory opening.
            if (cfg.hide_in_menus && snap.menu_open)
            {
                return L"a menu is open";
            }
            // Showing again waits only menu_close_show_delay_ms - a menu closing is not
            // a level transition, so it must not pay min_visible_after_state_ok_ms.
            if (cfg.hide_in_menus && snap.menu_change_ms != 0 &&
                now - snap.menu_change_ms < static_cast<std::uint64_t>(cfg.menu_close_show_delay_ms))
            {
                return L"the menu just closed (short show delay)";
            }
            if (cfg.require_pawn_view && !snap.is_pawn_view)
            {
                return L"view target is not the pawn (menu / cutscene / Lobby)";
            }
            return nullptr;
        }

        void draw_minimap(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            g_last_mini = MiniDebug{};

            const std::uint64_t now = ::GetTickCount64();
            if (const wchar_t* blocked = hud_gate(cfg, snap, have_state, now); blocked != nullptr)
            {
                set_hide_reason(blocked);
                return;
            }
            const mapdata::Chapter* chapter_ptr = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (chapter_ptr == nullptr)
            {
                set_hide_reason(L"player is outside every mapped chapter");
                return;
            }
            const mapdata::Chapter& chapter = *chapter_ptr;
            const bool composite_ready = g_map.ready && chapter.key == g_map.chapter;
            if (!chapter.has_heights() && !composite_ready)
            {
                set_hide_reason(L"no height maps and no composite texture loaded");
                return;
            }

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float screen_w = vp->Size.x;
            const float screen_h = vp->Size.y;
            const float side = (std::max)(cfg.minimap_min_px,
                                          (std::min)(cfg.size_frac * screen_h,
                                                     (std::min)(screen_w, screen_h) * 0.9f));

            float x0 = cfg.offset_x;
            float y0 = cfg.offset_y;
            if (cfg.anchor == mm::Anchor::TopRight || cfg.anchor == mm::Anchor::BottomRight)
            {
                x0 = screen_w - cfg.offset_x - side;
            }
            if (cfg.anchor == mm::Anchor::BottomLeft || cfg.anchor == mm::Anchor::BottomRight)
            {
                y0 = screen_h - cfg.offset_y - side;
            }
            x0 += vp->Pos.x;
            y0 += vp->Pos.y;

            MiniGeom g{};
            g.half = side * 0.5f;
            g.center = ImVec2{x0 + g.half, y0 + g.half};
            g.zoom = cfg.zoom_uu_per_px;
            g.uv = uv_of(chapter);
            g.px = snap.x;
            g.py = snap.y;
            const float eff_yaw = cfg.rotate_with_player ? snap.yaw : 0.0f;
            g.cos_yaw = std::cos(eff_yaw * kPi / 180.0f);
            g.sin_yaw = std::sin(eff_yaw * kPi / 180.0f);

            const float op = cfg.opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };
            // A darker, more opaque disc than the MVP had: the first in-world
            // screenshot showed a light grey map over a light grey scene, and the
            // walkable fill needs something dark to sit on.
            const auto ch = [](float v) { return static_cast<int>(v + 0.5f); };
            const ImU32 backdrop = IM_COL32(ch(cfg.minimap_backdrop_r), ch(cfg.minimap_backdrop_g),
                                            ch(cfg.minimap_backdrop_b), alpha(cfg.minimap_backdrop));
            const ImU32 frame = IM_COL32(ch(cfg.minimap_frame_r), ch(cfg.minimap_frame_g), ch(cfg.minimap_frame_b),
                                         alpha(cfg.minimap_frame_alpha));
            const ImU32 inner_ring = IM_COL32(0, 0, 0, alpha(0.55f));

            // The slice texture already carries the floor colour, the height gradient
            // and the per-pixel alpha, so the only tint left is the global opacity.
            const ImU32 tint_slice = IM_COL32(255, 255, 255, alpha(1.0f));
            const ImU32 tint_composite = IM_COL32(255, 255, 255, alpha(cfg.minimap_composite_alpha));

            ImDrawList* dl = ImGui::GetForegroundDrawList();

            if (cfg.round)
            {
                dl->AddCircleFilled(g.center, g.half, backdrop, g_circle_segments);
            }
            else
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, backdrop, 4.0f);
            }

            // ONE textured quad: the height slice. `update_slice` re-cuts the window
            // around the player at slice_hz and returns true while a window is
            // available to draw (its own or the previous one's - the window carries
            // margin, so a skipped update is invisible).
            const bool slice_ok = update_slice(cfg, chapter, snap, g.half, now);
            if (slice_ok && g_slice_shown >= 0)
            {
                const SliceBuf& b = g_slice[g_slice_shown];
                const UvMap window{g_slice_min_y, g_slice_max_x, g_slice_px_per_uu, b.w, b.h};
                draw_srv(dl, b.srv_gpu, window, g, tint_slice, cfg.round, x0, y0, side);
            }
            else if (composite_ready)
            {
                // No height maps: the Z-shaded composite, which merges every storey.
                draw_image(dl, g_map, uv_of(chapter), g, tint_composite, cfg.round, x0, y0, side);
            }
            else
            {
                set_hide_reason(L"the height slicer has no window yet");
                return;
            }

            if (cfg.round)
            {
                dl->AddCircle(g.center, g.half - 1.0f, inner_ring, g_circle_segments, 2.0f);
                dl->AddCircle(g.center, g.half, frame, g_circle_segments, 2.0f);
            }
            else
            {
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, frame, 4.0f, 0, 2.0f);
            }

            // Markers go over the map and under the frame ring's highlight and the
            // player arrow, so the arrow is never hidden by a glyph standing on it.
            draw_markers(cfg, g, cfg.round, x0, y0, side, dl);

            // North marker, only meaningful when the map itself is rotating.
            if (cfg.rotate_with_player)
            {
                const float na = -eff_yaw * kPi / 180.0f;
                const ImVec2 np{g.center.x + std::sin(na) * (g.half - 12.0f),
                                g.center.y - std::cos(na) * (g.half - 12.0f)};
                dl->AddCircleFilled(np, 3.5f, IM_COL32(230, 90, 80, 235), 12);
            }

            // The waypoint, edge-clamped with its distance. It is the one marker that
            // must never be culled: the whole point of setting one is to be told which
            // way to walk while it is off the map.
            {
                const mv::Waypoint wp = mm::waypoint();
                if (wp.set)
                {
                    const double wdx = wp.x - g.px;
                    const double wdy = wp.y - g.py;
                    const double zz = g.zoom > 0.0001f ? static_cast<double>(g.zoom) : 1.0;
                    double dx = (-g.sin_yaw * wdx + g.cos_yaw * wdy) / zz;
                    double dy = (-g.cos_yaw * wdx - g.sin_yaw * wdy) / zz;
                    const float wr = (std::max)(5.0f, cfg.markers_size * cfg.waypoint_size_scale);
                    const float lim = (std::max)(4.0f, g.half - wr - 3.0f);
                    bool clamped = false;
                    if (cfg.round)
                    {
                        const double d = std::sqrt(dx * dx + dy * dy);
                        if (d > lim && d > 0.0001)
                        {
                            dx = dx * lim / d;
                            dy = dy * lim / d;
                            clamped = true;
                        }
                    }
                    else if (std::abs(dx) > lim || std::abs(dy) > lim)
                    {
                        const double sc = lim / (std::max)(std::abs(dx), std::abs(dy));
                        dx *= sc;
                        dy *= sc;
                        clamped = true;
                    }
                    const ImVec2 wp_pos{g.center.x + static_cast<float>(dx), g.center.y + static_cast<float>(dy)};
                    draw_waypoint_glyph(dl, wp_pos, clamped ? wr * 0.85f : wr, alpha(1.0f));
                    const double dist_m = std::sqrt(wdx * wdx + wdy * wdy) / 100.0;
                    const std::string label =
                        dist_m >= 1000.0 ? std::format("{:.1f} km", dist_m / 1000.0) : std::format("{:.0f} m", dist_m);
                    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    const ImVec2 tp{wp_pos.x - ts.x * 0.5f, wp_pos.y + wr * 1.6f};
                    dl->AddRectFilled(ImVec2{tp.x - 3.0f, tp.y - 1.0f}, ImVec2{tp.x + ts.x + 3.0f, tp.y + ts.y + 1.0f},
                                      IM_COL32(8, 10, 14, alpha(0.7f)), 3.0f);
                    dl->AddText(tp, IM_COL32(255, 190, 235, alpha(1.0f)), label.c_str());
                }
            }

            add_player_arrow(dl, g.center, snap.yaw - eff_yaw,
                             (std::max)(cfg.minimap_arrow_min_px, side * cfg.minimap_arrow_frac));

            set_hide_reason(L"visible");
            g.uv = uv_of(chapter);
            const ImVec2 uv = uv_at(g, 0.0f, 0.0f);
            g_last_mini.visible = true;
            g_last_mini.u = uv.x;
            g_last_mini.v = uv.y;
            g_last_mini.chapter = chapter.key;
            g_last_mini.side = side;
        }

        //==============================================================================
        // Drawing: the HOLD-KEY X-RAY HIGHLIGHT (master plan step 7, v1)
        //==============================================================================
        //
        // While the highlight key (or the pad chord) is held, every marker of an enabled
        // category within highlight_radius of the PLAYER is drawn at its projected screen
        // position: category glyph, name, distance in metres, alpha fading with distance.
        // It is "through walls" for free - the overlay is composited over the finished
        // frame, so there is no occlusion test, no CustomDepth, no material, and nothing
        // that can go wrong with the game's render state.
        //
        // WHAT IT NEEDS AND WHAT IT DOES WITHOUT
        //   * the camera pose comes from hl::camera() (game thread, see highlight.cpp)
        //     and is REQUIRED: no fresh pose, no highlight. Falling back to the pawn's
        //     own position and yaw would put every label a spring-arm's length away from
        //     the truth, which is worse than drawing nothing;
        //   * the screen size comes from the ImGui viewport, which is the swapchain's -
        //     the same rectangle UE built its projection matrix for;
        //   * the radius test is against the player, the projection against the camera,
        //     and the distance shown is the player's. That is what "30 m away" means to
        //     someone deciding whether to walk over.

        struct HighlightDebug
        {
            bool active = false;
            bool have_camera = false;
            int considered = 0;
            int drawn = 0;
            int on_screen = 0;
            int edge = 0;
            std::uint64_t cam_age_ms = 0;
        };

        HighlightDebug g_hl_debug{};

        // A small outward-pointing triangle at `p`, aimed along (dx, dy) in screen space.
        void add_edge_arrow(ImDrawList* dl, ImVec2 p, float dx, float dy, float r, ImU32 col, ImU32 edge)
        {
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f)
            {
                return;
            }
            const float ux = dx / len;
            const float uy = dy / len;
            const ImVec2 tip{p.x + ux * r, p.y + uy * r};
            const ImVec2 a{p.x - ux * r * 0.6f - uy * r * 0.75f, p.y - uy * r * 0.6f + ux * r * 0.75f};
            const ImVec2 b{p.x - ux * r * 0.6f + uy * r * 0.75f, p.y - uy * r * 0.6f - ux * r * 0.75f};
            dl->AddTriangleFilled(tip, a, b, col);
            dl->AddTriangle(tip, a, b, edge, 1.2f);
        }

        void draw_label(ImDrawList* dl, ImVec2 at, const std::string& text, ImU32 col, int alpha)
        {
            if (text.empty())
            {
                return;
            }
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            const ImVec2 tp{at.x - ts.x * 0.5f, at.y};
            dl->AddRectFilled(ImVec2{tp.x - 4.0f, tp.y - 1.0f}, ImVec2{tp.x + ts.x + 4.0f, tp.y + ts.y + 1.0f},
                              IM_COL32(8, 10, 14, static_cast<int>(alpha * 0.62f)), 3.0f);
            dl->AddText(tp, col, text.c_str());
        }

        void draw_highlight(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_hl_debug = HighlightDebug{};
            if (!cfg.enabled || !cfg.highlight_enabled || !gate_ok)
            {
                return;
            }
            if (!hl::held())
            {
                return;
            }
            g_hl_debug.active = true;

            hl::Pose pose{};
            if (!hl::camera(pose))
            {
                return; // the reader has not produced a pose yet
            }
            const std::uint64_t now = ::GetTickCount64();
            g_hl_debug.cam_age_ms = pose.stamp_ms == 0 ? 0 : now - pose.stamp_ms;
            // A pose older than a few frames is a camera that has stopped being read (the
            // game thread stalled, or the pawn went) - draw with it and the labels lag
            // visibly behind the scene, which reads as a bug. 250 ms is ~15 frames.
            if (pose.stamp_ms == 0 || g_hl_debug.cam_age_ms > 250)
            {
                return;
            }
            g_hl_debug.have_camera = true;

            proj::Camera cam{};
            cam.x = pose.x;
            cam.y = pose.y;
            cam.z = pose.z;
            cam.pitch = pose.pitch;
            cam.yaw = pose.yaw;
            cam.roll = pose.roll;
            cam.fov_deg = pose.fov;
            if (!proj::camera_sane(cam))
            {
                return;
            }

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const double screen_w = static_cast<double>(vp->Size.x);
            const double screen_h = static_cast<double>(vp->Size.y);
            if (!(screen_w > 1.0) || !(screen_h > 1.0))
            {
                return;
            }

            const markers::View v = markers::view();
            if (v.data == nullptr || v.count == 0)
            {
                return;
            }

            struct Cand
            {
                double dist = 0.0; // from the player, uu
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Cand> cands; // render thread only, reused every frame
            cands.clear();

            const double radius = static_cast<double>(cfg.highlight_radius);
            for (std::size_t i = 0; i < v.count; ++i)
            {
                const markers::DrawMarker& m = v.data[i];
                if (static_cast<int>(m.cat) >= mdb::kCatCount ||
                    !mdb::cat_enabled(cfg.highlight_categories, static_cast<mdb::Cat>(m.cat)))
                {
                    continue;
                }
                if (!cfg.highlight_show_found && (m.flags & markers::kFlagFound) != 0)
                {
                    continue; // the point of the feature is what is still UNcollected
                }
                const double dx = m.x - snap.x;
                const double dy = m.y - snap.y;
                const double dz = m.z - snap.z;
                const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > radius)
                {
                    continue;
                }
                cands.push_back(Cand{d, &m});
            }
            g_hl_debug.considered = static_cast<int>(cands.size());

            const std::size_t cap = static_cast<std::size_t>((std::max)(1, cfg.highlight_max_draw));
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
                cands.resize(cap);
            }
            // Far to near, so the nearest label ends up on top of the pile.
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist > b.dist; });

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float r = cfg.highlight_size;
            const float pad = r * 2.4f;

            for (const Cand& cand : cands)
            {
                const markers::DrawMarker& m = *cand.m;
                const proj::Result pr = proj::project(cam, m.x, m.y, m.z, screen_w, screen_h);
                if (!pr.valid)
                {
                    continue;
                }

                // Fade with distance: fully lit at the camera, highlight_alpha_far at the
                // radius. Linear - a squared falloff makes everything past half the radius
                // look identical.
                const double t = radius > 1.0 ? (cand.dist / radius) : 0.0;
                const double a = static_cast<double>(cfg.highlight_alpha_near) +
                                 (static_cast<double>(cfg.highlight_alpha_far) -
                                  static_cast<double>(cfg.highlight_alpha_near)) *
                                     (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
                const int alpha = static_cast<int>((std::min)(1.0, (std::max)(0.0, a)) * 255.0 + 0.5);
                if (alpha <= 4)
                {
                    continue;
                }
                const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                const ImU32 col = marker_color(cat, alpha);
                const ImU32 edge = IM_COL32(10, 12, 16, static_cast<int>(alpha * 0.9f));
                const bool found = (m.flags & markers::kFlagFound) != 0;

                if (pr.on_screen && !pr.behind)
                {
                    const ImVec2 p{vp->Pos.x + static_cast<float>(pr.sx), vp->Pos.y + static_cast<float>(pr.sy)};
                    draw_marker_glyph(dl, cat, p, found ? r * 0.75f : r, col, edge);
                    if (cfg.highlight_labels)
                    {
                        const char* name = m.label[0] != '\0' ? m.label : mdb::cat_label(cat);
                        const double metres = cand.dist / 100.0;
                        const std::string text = std::format("{}  {:.0f} m{}", name, metres, found ? "  (found)" : "");
                        draw_label(dl, ImVec2{p.x, p.y + r + 3.0f}, text, col, alpha);
                    }
                    ++g_hl_debug.on_screen;
                    ++g_hl_debug.drawn;
                    continue;
                }

                if (!cfg.highlight_edge_arrows)
                {
                    continue;
                }
                // Off screen (or behind): put an arrow on the rim pointing the way to
                // turn. proj::project() already handed us a direction rather than a
                // mirrored position, so this is just a projection onto the border box.
                double nx = pr.ndc_x;
                double ny = pr.ndc_y;
                const double mag = (std::max)(std::fabs(nx), std::fabs(ny));
                if (mag < 1e-6)
                {
                    continue;
                }
                nx /= mag;
                ny /= mag;
                float sx = static_cast<float>((nx * 0.5 + 0.5) * screen_w);
                float sy = static_cast<float>((0.5 - ny * 0.5) * screen_h);
                sx = (std::max)(pad, (std::min)(static_cast<float>(screen_w) - pad, sx));
                sy = (std::max)(pad, (std::min)(static_cast<float>(screen_h) - pad, sy));
                const ImVec2 p{vp->Pos.x + sx, vp->Pos.y + sy};
                const int dim = static_cast<int>(alpha * 0.8f);
                add_edge_arrow(dl, p, static_cast<float>(nx), static_cast<float>(-ny), r * 1.15f,
                               marker_color(cat, dim), IM_COL32(10, 12, 16, dim));
                ++g_hl_debug.edge;
                ++g_hl_debug.drawn;
            }
        }

        //==============================================================================
        // Drawing: the COMPASS STRIP
        //==============================================================================
        //
        // A heading strip across the top of the screen. The arithmetic (wrap, bearing,
        // strip position, tick layout) is in src/compass.cpp, where markers_test can
        // reach it; this is the drawing and nothing else.
        //
        // The heading is the CAMERA's yaw when a fresh pose exists, because that is what
        // the player is looking along, and the pawn's yaw otherwise - so the compass
        // still works with highlight_enabled = 0 and during the camera reader's warm-up.
        // Which one is in use is printed in the F2 debug block.

        struct CompassDebug
        {
            bool visible = false;
            bool from_camera = false;
            double heading = 0.0;
            int pips = 0;
        };

        CompassDebug g_compass_debug{};

        void draw_compass(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_compass_debug = CompassDebug{};
            if (!cfg.enabled || !cfg.compass_enabled || !gate_ok)
            {
                return;
            }

            const std::uint64_t now = ::GetTickCount64();
            double heading = static_cast<double>(snap.yaw);
            bool from_camera = false;
            hl::Pose pose{};
            if (hl::camera(pose) && pose.stamp_ms != 0 && now - pose.stamp_ms <= 1000)
            {
                heading = pose.yaw;
                from_camera = true;
            }
            g_compass_debug.heading = cmp::wrap360(heading);
            g_compass_debug.from_camera = from_camera;

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float screen_w = vp->Size.x;
            const float width = (std::max)(120.0f, cfg.compass_width * screen_w);
            const float height = cfg.compass_height;
            const float x0 = vp->Pos.x + (screen_w - width) * 0.5f;
            const float y0 = vp->Pos.y + cfg.compass_offset_y;
            const float y1 = y0 + height;

            const float op = cfg.compass_opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, IM_COL32(6, 9, 13, alpha(0.72f)), 4.0f);
            dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, IM_COL32(150, 158, 168, alpha(0.55f)), 4.0f, 0,
                        1.2f);

            cmp::Strip strip{};
            strip.x0 = static_cast<double>(x0);
            strip.width = static_cast<double>(width);
            strip.heading = heading;
            strip.span = static_cast<double>(cfg.compass_span_deg);

            // Ticks. A cardinal gets the full height and its letter, an intercardinal a
            // shorter line and its two-letter label, everything else a stub.
            cmp::Tick ticks[128]{};
            const int n = cmp::ticks(strip, ticks, static_cast<int>(std::size(ticks)),
                                     static_cast<double>(cfg.compass_tick_step_deg));
            for (int i = 0; i < n; ++i)
            {
                const cmp::Tick& t = ticks[i];
                const float x = static_cast<float>(t.x);
                const float len = t.rank == 2 ? height * 0.5f : (t.rank == 1 ? height * 0.34f : height * 0.22f);
                const int a = t.rank == 2 ? alpha(0.95f) : (t.rank == 1 ? alpha(0.75f) : alpha(0.45f));
                dl->AddLine(ImVec2{x, y1 - len}, ImVec2{x, y1 - 2.0f}, IM_COL32(226, 230, 236, a),
                            t.rank == 2 ? 2.0f : 1.2f);
                if (t.label[0] != '\0')
                {
                    const ImVec2 ts = ImGui::CalcTextSize(t.label);
                    dl->AddText(ImVec2{x - ts.x * 0.5f, y0 + 1.0f},
                                IM_COL32(240, 242, 246, t.rank == 2 ? alpha(1.0f) : alpha(0.8f)), t.label);
                }
            }

            // The centre reticle: what the player is actually facing.
            {
                const float cx = x0 + width * 0.5f;
                dl->AddTriangleFilled(ImVec2{cx, y1 - 1.0f}, ImVec2{cx - 5.0f, y1 + 7.0f},
                                      ImVec2{cx + 5.0f, y1 + 7.0f}, IM_COL32(255, 236, 180, alpha(0.95f)));
            }

            if (!snap.has_pawn)
            {
                g_compass_debug.visible = true;
                return;
            }

            // Marker pips. Nearest first so the cap keeps what matters, and only inside
            // the strip's span - an off-strip pip clamped to the edge would pile up into
            // a solid block at both ends.
            const markers::View v = markers::view();
            if (v.data != nullptr && v.count != 0)
            {
                struct Pip
                {
                    double dist = 0.0;
                    double bearing = 0.0;
                    std::uint8_t cat = 0;
                    bool found = false;
                };
                static std::vector<Pip> pips; // render thread only
                pips.clear();
                const double max_d = static_cast<double>(cfg.compass_marker_distance);
                for (std::size_t i = 0; i < v.count; ++i)
                {
                    const markers::DrawMarker& m = v.data[i];
                    if (static_cast<int>(m.cat) >= mdb::kCatCount ||
                        !mdb::cat_enabled(cfg.compass_categories, static_cast<mdb::Cat>(m.cat)))
                    {
                        continue;
                    }
                    const double dx = m.x - snap.x;
                    const double dy = m.y - snap.y;
                    const double d = std::sqrt(dx * dx + dy * dy);
                    if (d > max_d)
                    {
                        continue;
                    }
                    Pip p{};
                    p.dist = d;
                    p.bearing = cmp::bearing_deg(snap.x, snap.y, m.x, m.y);
                    p.cat = m.cat;
                    p.found = (m.flags & markers::kFlagFound) != 0;
                    pips.push_back(p);
                }
                const std::size_t kMaxPips = static_cast<std::size_t>(cfg.compass_max_pips);
                if (pips.size() > kMaxPips)
                {
                    std::partial_sort(pips.begin(), pips.begin() + kMaxPips, pips.end(),
                                      [](const Pip& a, const Pip& b) { return a.dist < b.dist; });
                    pips.resize(kMaxPips);
                }
                std::sort(pips.begin(), pips.end(), [](const Pip& a, const Pip& b) { return a.dist > b.dist; });
                for (const Pip& p : pips)
                {
                    double x = 0.0;
                    double rel = 0.0;
                    if (!cmp::strip_x(strip, p.bearing, x, rel))
                    {
                        continue;
                    }
                    const int a = p.found ? alpha(0.35f) : alpha(1.0f);
                    const ImU32 col = marker_color(static_cast<mdb::Cat>(p.cat), a);
                    const ImVec2 at{static_cast<float>(x), y1 - height * 0.30f};
                    draw_marker_glyph(dl, static_cast<mdb::Cat>(p.cat), at, height * 0.22f, col,
                                      IM_COL32(10, 12, 16, a));
                    ++g_compass_debug.pips;
                }
            }

            // The waypoint is never culled: being told which way to walk while it is off
            // the strip is the whole point, so it clamps to the edge instead.
            const mv::Waypoint wp = mm::waypoint();
            if (cfg.compass_show_waypoint && wp.set)
            {
                double x = 0.0;
                double rel = 0.0;
                const bool inside = cmp::strip_x(strip, cmp::bearing_deg(snap.x, snap.y, wp.x, wp.y), x, rel);
                const float wx = static_cast<float>(x);
                const ImVec2 at{wx, y1 - height * 0.30f};
                draw_waypoint_glyph(dl, at, height * 0.24f, alpha(1.0f));
                if (!inside)
                {
                    add_edge_arrow(dl, ImVec2{wx + (rel < 0.0 ? -8.0f : 8.0f), at.y}, rel < 0.0 ? -1.0f : 1.0f,
                                   0.0f, 5.0f, IM_COL32(255, 190, 235, alpha(0.9f)),
                                   IM_COL32(10, 12, 16, alpha(0.9f)));
                }
                const double dxw = wp.x - snap.x;
                const double dyw = wp.y - snap.y;
                const double metres = std::sqrt(dxw * dxw + dyw * dyw) / 100.0;
                const std::string text =
                    metres >= 1000.0 ? std::format("{:.1f} km", metres / 1000.0) : std::format("{:.0f} m", metres);
                draw_label(dl, ImVec2{wx, y1 + 9.0f}, text, IM_COL32(255, 190, 235, alpha(1.0f)), alpha(1.0f));
                ++g_compass_debug.pips;
            }

            g_compass_debug.visible = true;
        }

        //==============================================================================
        // Drawing: the FULL MAP (step C1)
        //==============================================================================
        //
        // Toggled by `map_key` (M). While it is open the minimap is hidden, a dark
        // backdrop covers the scene and one ImGui window holds the map: north-up,
        // pannable and zoomable, drawn from the SAME height-sliced asset the minimap
        // uses, with every marker on it.
        //
        // WHAT IS SHARED WITH THE MINIMAP, DELIBERATELY
        //   * the asset (mapdata::HeightMaps) - one copy in RAM, ~327 MB, and the map
        //     adds nothing to it: it cuts a second small dynamic texture out of the same
        //     planes rather than keeping a picture of its own;
        //   * the height-slice rule and its config (floor_z_tolerance / floor_fade_uu /
        //     the gradient / the base colour), so the two never disagree about what a
        //     floor is - the only addition is a floor OFFSET the player can nudge;
        //   * the marker draw buffer, the category filter mask and the glyphs.
        //
        // WHAT IS DIFFERENT
        //   * the cut is DECIMATED (one texture pixel covers `step` source pixels) and
        //     covers the visible viewport plus a 30 % margin, not the whole chapter;
        //   * it only re-cuts when something changed - a pan that leaves the cut region,
        //     a zoom, a floor change, or a big player move - capped at map_slice_hz. An
        //     idle open map costs nothing per frame beyond the draw;
        //   * it is always north-up. A rotating full map is unreadable and every
        //     reference implementation avoids it.
        //
        // NO LATCHES (lessons.md). The map closes itself the moment the state that
        // allows it stops being true - a menu opening, the pawn going away, a level
        // transition - and closing gives the mouse and the keyboard straight back to the
        // game on the same frame, because the swallow condition IS `g_map_open`.

        // The published found flag with the render side's own pending toggle applied.
        // An override is dropped as soon as the published buffer says the same thing,
        // so this can never latch: at worst it holds for one marker round (~1 s).
        bool marker_found_now(const markers::DrawMarker& m)
        {
            const bool published = (m.flags & markers::kFlagFound) != 0;
            for (std::size_t i = 0; i < g_found_override.size(); ++i)
            {
                if (g_found_override[i].first != m.id)
                {
                    continue;
                }
                if (g_found_override[i].second == published)
                {
                    g_found_override.erase(g_found_override.begin() + static_cast<std::ptrdiff_t>(i));
                    return published;
                }
                return g_found_override[i].second;
            }
            return published;
        }

        void toggle_found(const markers::DrawMarker& m)
        {
            if (m.id[0] == '\0')
            {
                return;
            }
            const bool want = !marker_found_now(m);
            for (auto& kv : g_found_override)
            {
                if (kv.first == m.id)
                {
                    kv.second = want;
                    markers::request_toggle_found(m.id, want);
                    return;
                }
            }
            if (g_found_override.size() < 512)
            {
                g_found_override.emplace_back(std::string{m.id}, want);
            }
            markers::request_toggle_found(m.id, want);
        }

        // Cuts the visible region (plus a margin) into the map's own dynamic texture,
        // if anything changed and the next buffer is free. Returns true while a buffer
        // is available to draw.
        bool update_map_slice(const mm::Config& cfg, const mapdata::Chapter& ch, const mv::Rect& canvas,
                              float feet, std::uint64_t now)
        {
            if (!ch.has_heights() || canvas.w() < 8.0f || canvas.h() < 8.0f)
            {
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                return false;
            }

            // 30 % of margin around the viewport: a drag can move ~15 % of the canvas
            // in either direction before the cut has to be redone, which at 6 Hz is
            // most of a fast drag.
            const double kMargin = static_cast<double>(cfg.map_slice_margin);
            const double zoom = g_mv.uu_per_px;
            const double want_w_uu = static_cast<double>(canvas.w()) * zoom * kMargin;
            const double want_h_uu = static_cast<double>(canvas.h()) * zoom * kMargin;

            int tw = static_cast<int>(std::lround(static_cast<double>(canvas.w()) * kMargin));
            if (tw > cfg.map_slice_px)
            {
                tw = cfg.map_slice_px;
            }
            tw = (tw / 8) * 8;
            if (tw < 64)
            {
                tw = 64;
            }
            // One step for both axes (a non-square pixel would shear the picture), so
            // the height follows from it rather than from the aspect ratio directly.
            const double step = (want_w_uu * hm.px_per_uu) / static_cast<double>(tw);
            int th = static_cast<int>(std::lround(want_h_uu * hm.px_per_uu / step));
            th = (th / 8) * 8;
            if (th < 64)
            {
                th = 64;
            }
            if (th > cfg.map_slice_px * 2)
            {
                th = (cfg.map_slice_px * 2 / 8) * 8;
            }

            const bool resized = g_mslice[0].w != tw || g_mslice[0].h != th || g_mslice[0].tex == nullptr;
            if (resized)
            {
                // A failing allocation must not retry (and log) once per frame - the
                // same "back a failing blind path off" rule the navmesh scan learned.
                static std::uint64_t create_failed_ms = 0;
                if (create_failed_ms != 0 && now - create_failed_ms < 5000)
                {
                    return false;
                }
                wait_for_gpu(); // the old buffers may still be in flight
                if (!create_slice_set(g_mslice, kMapSliceBufs, tw, th, L"full map"))
                {
                    destroy_map_slice_buffers();
                    create_failed_ms = now;
                    return false;
                }
                create_failed_ms = 0;
                g_mslice_next = 0;
                g_mslice_shown = -1;
                g_mr_valid = false;
            }

            // The world rectangle this cut will cover, derived from the texture so the
            // drawn quad matches the pixels exactly.
            const double half_w = static_cast<double>(tw) * step / hm.px_per_uu * 0.5;
            const double half_h = static_cast<double>(th) * step / hm.px_per_uu * 0.5;

            // Does the visible viewport still sit inside the region we already cut?
            const double view_half_x = static_cast<double>(canvas.h()) * zoom * 0.5;
            const double view_half_y = static_cast<double>(canvas.w()) * zoom * 0.5;
            const bool inside = g_mr_valid && g_mv.cx - view_half_x >= g_mr_x0 && g_mv.cx + view_half_x <= g_mr_x1 &&
                                g_mv.cy - view_half_y >= g_mr_y0 && g_mv.cy + view_half_y <= g_mr_y1;
            const bool feet_moved = !g_mr_valid || std::abs(feet - g_mr_feet) > 20.0f;
            const bool zoomed = !g_mr_valid || zoom != g_mr_zoom;
            const bool chapter_changed = g_mr_chapter != ch.key;
            const bool urgent = !inside || zoomed || chapter_changed;
            if (!urgent && !feet_moved && !resized)
            {
                return g_mslice_shown >= 0;
            }

            // The rate cap. An urgent cut (the view has left the region, or the zoom
            // changed) still has to wait for the buffer, but not for the clock: showing
            // an empty edge is worse than one extra cut.
            const int period = cfg.map_slice_hz > 0 ? 1000 / cfg.map_slice_hz : 166;
            if (!urgent && g_mslice_shown >= 0 &&
                now - g_mslice_last_ms < static_cast<std::uint64_t>(period))
            {
                return true;
            }

            SliceBuf& b = g_mslice[g_mslice_next];
            if (b.tex == nullptr || b.mapped == nullptr)
            {
                return g_mslice_shown >= 0;
            }
            if (b.in_flight_fence != 0 && g_fence != nullptr && g_fence->GetCompletedValue() < b.in_flight_fence)
            {
                ++g_mslice_skipped;
                return g_mslice_shown >= 0; // never stall Present for the map
            }

            SliceStyle st{};
            st.base_r = cfg.floor_base_r;
            st.base_g = cfg.floor_base_g;
            st.base_b = cfg.floor_base_b;
            st.strength = cfg.floor_gradient_strength;
            st.tol = cfg.floor_z_tolerance;
            st.fade = cfg.floor_fade_uu;
            st.a_dim = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity : 0.0f;
            st.a_faint = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity * 0.6f : 0.0f;
            if (cfg.map_show_all_floors)
            {
                // "Show everything": no surface is ever out of range, so the whole
                // chapter's walkable area is on screen with the current storey still
                // picked out at full opacity.
                st.fade = 1.0e9f;
                st.a_dim = 0.34f;
                st.a_faint = 0.24f;
            }

            const double x1 = g_mv.cx + half_h; // north edge
            const double y0 = g_mv.cy - half_w; // west edge
            const double src_x0 = (y0 - hm.min_y) * hm.px_per_uu;
            const double src_y0 = (hm.max_x - x1) * hm.px_per_uu;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            LARGE_INTEGER freq{};
            ::QueryPerformanceFrequency(&freq);
            ::QueryPerformanceCounter(&t0);
            slice_region(hm, src_x0, src_y0, step, b.w, b.h, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, feet, st, g_mslice_scratch, g_mslice_counts);
            ::QueryPerformanceCounter(&t1);
            if (freq.QuadPart > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
                g_mslice_ms = g_mslice_ms == 0.0 ? ms : g_mslice_ms * 0.7 + ms * 0.3;
                if (ms > g_mslice_ms_peak)
                {
                    g_mslice_ms_peak = ms;
                }
            }

            b.needs_copy = true;
            g_mslice_shown = g_mslice_next;
            g_mslice_next = (g_mslice_next + 1) % kMapSliceBufs;
            g_mslice_last_ms = now;
            ++g_mslice_updates;
            g_mr_x0 = g_mv.cx - half_h;
            g_mr_x1 = x1;
            g_mr_y0 = y0;
            g_mr_y1 = g_mv.cy + half_w;
            g_mr_zoom = zoom;
            g_mr_feet = feet;
            g_mr_chapter = ch.key;
            g_mr_valid = true;
            return true;
        }

        void draw_waypoint_glyph(ImDrawList* dl, ImVec2 p, float r, int alpha)
        {
            const ImU32 col = IM_COL32(255, 92, 210, alpha);
            const ImU32 edge = IM_COL32(20, 8, 18, static_cast<int>(alpha * 0.9f));
            const ImVec2 tip{p.x, p.y + r * 1.5f};
            const ImVec2 l{p.x - r * 0.75f, p.y + r * 0.25f};
            const ImVec2 rr{p.x + r * 0.75f, p.y + r * 0.25f};
            dl->AddTriangleFilled(l, rr, tip, col);
            dl->AddCircleFilled(ImVec2{p.x, p.y - r * 0.15f}, r * 0.85f, col, 14);
            dl->AddCircle(ImVec2{p.x, p.y - r * 0.15f}, r * 0.85f, edge, 14, 1.4f);
            dl->AddCircleFilled(ImVec2{p.x, p.y - r * 0.15f}, r * 0.3f, edge, 8);
        }

        // Closes the map and says why, exactly once per transition.
        void close_map(const wchar_t* why)
        {
            if (!mm::g_map_open.exchange(false))
            {
                return;
            }
            mm::logf(L"full map closed: {}", why);
        }

        void draw_full_map(mm::Config cfg, const mm::Snapshot& snap, bool have_state)
        {
            const mm::Config before = cfg;
            const std::uint64_t now = ::GetTickCount64();
            ImGuiIO& io = ImGui::GetIO();
            const ImGuiViewport* vp = ImGui::GetMainViewport();

            //--------------------------------------------------------------------------
            // The gate. Every condition is re-evaluated from the live snapshot on every
            // frame and closes the map outright - there is nothing here that can latch.
            //--------------------------------------------------------------------------
            if (!have_state || snap.stamp_ms == 0 ||
                now - snap.stamp_ms > static_cast<std::uint64_t>(cfg.state_stale_ms))
            {
                close_map(L"no fresh game-state snapshot");
                return;
            }
            if (snap.transition)
            {
                close_map(L"a level transition started");
                return;
            }
            if (!snap.has_pawn || !snap.pawn_is_gameplay)
            {
                close_map(L"there is no gameplay pawn");
                return;
            }
            if (cfg.hide_in_menus && snap.menu_open)
            {
                close_map(L"a game menu opened");
                return;
            }

            //--------------------------------------------------------------------------
            // Geometry and the backdrop
            //--------------------------------------------------------------------------
            const float margin = cfg.map_margin * vp->Size.y;
            const mv::Rect frame{vp->Pos.x + margin,
                                 vp->Pos.y + margin,
                                 vp->Pos.x + vp->Size.x - margin,
                                 vp->Pos.y + vp->Size.y - margin};

            ImDrawList* back = ImGui::GetBackgroundDrawList();
            back->AddRectFilled(vp->Pos,
                                ImVec2{vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y},
                                IM_COL32(3, 5, 8, static_cast<int>(cfg.map_backdrop * 255.0f + 0.5f)));

            const mapdata::Chapter* chapter_ptr = mapdata::chapter_ptr_for(snap.x, snap.y);

            //--------------------------------------------------------------------------
            // First frame after opening: centre on the player, reset the zoom and the
            // floor offset, and drop any gamepad edges from while it was closed.
            //--------------------------------------------------------------------------
            if (!g_mv_init)
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_mv.uu_per_px = mv::clamp_zoom(static_cast<double>(cfg.map_zoom),
                                                static_cast<double>(cfg.map_zoom_min),
                                                static_cast<double>(cfg.map_zoom_max));
                g_map_floor_off = 0.0f;
                g_mr_valid = false;
                g_mv_init = true;
                pad::clear_pressed();
                g_map_recenter.store(false, std::memory_order_relaxed);
            }

            ImGui::SetNextWindowPos(ImVec2{frame.x0, frame.y0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{frame.w(), frame.h()}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.97f);
            constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                ImGuiWindowFlags_NoSavedSettings;
            if (!ImGui::Begin("##wuchang_full_map", nullptr, kFlags))
            {
                ImGui::End();
                return;
            }

            //--------------------------------------------------------------------------
            // Header + the category filter (the SAME mask the minimap and the F2 panel
            // use, so a filter toggled here is toggled everywhere)
            //--------------------------------------------------------------------------
            ImGui::Text("Wuchang map");
            ImGui::SameLine();
            ImGui::TextDisabled("%s   |   %.0f uu/px   |   floor %+0.0f uu   |   X %.0f  Y %.0f",
                                chapter_ptr != nullptr ? chapter_ptr->key.c_str() : "no chapter here",
                                g_mv.uu_per_px,
                                static_cast<double>(g_map_floor_off),
                                snap.x,
                                snap.y);
            ImGui::SameLine((std::max)(200.0f, ImGui::GetWindowWidth() - 170.0f));
            if (ImGui::SmallButton("Recentre"))
            {
                g_map_recenter.store(true, std::memory_order_relaxed);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Close"))
            {
                close_map(L"the Close button");
            }

            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(i);
                const bool on = mdb::cat_enabled(cfg.markers_categories, cat);
                const ImU32 col = marker_color(cat, on ? 210 : 60);
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Button, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, marker_color(cat, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, marker_color(cat, 255));
                ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(12, 12, 12, 255) : IM_COL32(220, 220, 220, 190));
                if (ImGui::SmallButton(mdb::cat_label(cat)))
                {
                    cfg.markers_categories ^= mdb::cat_bit(cat);
                }
                ImGui::PopStyleColor(4);
                ImGui::PopID();
                if (i + 1 < mdb::kCatCount)
                {
                    ImGui::SameLine();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("all"))
            {
                cfg.markers_categories = mdb::kAllCats;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("none"))
            {
                cfg.markers_categories = 0u;
            }

            //--------------------------------------------------------------------------
            // The canvas
            //--------------------------------------------------------------------------
            const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const ImVec2 csize{(std::max)(64.0f, avail.x), (std::max)(64.0f, avail.y - footer_h)};
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##canvas", csize, ImGuiButtonFlags_MouseButtonLeft);
            const bool canvas_hovered = ImGui::IsItemHovered();
            const bool canvas_active = ImGui::IsItemActive();
            const mv::Rect canvas{cpos.x, cpos.y, cpos.x + csize.x, cpos.y + csize.y};

            const double zmin = static_cast<double>(cfg.map_zoom_min);
            const double zmax = static_cast<double>(cfg.map_zoom_max);
            g_mv.uu_per_px = mv::clamp_zoom(g_mv.uu_per_px, zmin, zmax);

            //--------------------------------------------------------------------------
            // Input: mouse
            //--------------------------------------------------------------------------
            static float drag_px = 0.0f;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canvas_hovered)
            {
                drag_px = 0.0f;
            }
            if (canvas_active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const ImVec2 d = io.MouseDelta;
                drag_px += std::abs(d.x) + std::abs(d.y);
                // Keep the world point under the cursor under the cursor.
                g_mv.cx += static_cast<double>(d.y) * g_mv.uu_per_px;
                g_mv.cy -= static_cast<double>(d.x) * g_mv.uu_per_px;
            }

            const auto zoom_about = [&](float sx, float sy, double notches) {
                double wx = 0.0;
                double wy = 0.0;
                mv::screen_to_world(g_mv, canvas, sx, sy, wx, wy);
                g_mv.uu_per_px =
                    mv::zoom_by(g_mv.uu_per_px, notches, static_cast<double>(cfg.map_zoom_factor), zmin, zmax);
                // Solve the inverse for the centre so that (wx, wy) lands on (sx, sy)
                // again: it is the same transform, read the other way round.
                g_mv.cx = wx + (static_cast<double>(sy) - static_cast<double>(canvas.cy())) * g_mv.uu_per_px;
                g_mv.cy = wy - (static_cast<double>(sx) - static_cast<double>(canvas.cx())) * g_mv.uu_per_px;
            };

            if (canvas_hovered && io.MouseWheel != 0.0f)
            {
                if (io.KeyCtrl)
                {
                    g_map_floor_off += io.MouseWheel * cfg.map_floor_step;
                }
                else
                {
                    zoom_about(io.MousePos.x, io.MousePos.y, static_cast<double>(io.MouseWheel));
                }
            }

            //--------------------------------------------------------------------------
            // Input: keyboard. ImGui sees these because the WndProc hook feeds it every
            // message BEFORE deciding to swallow it.
            //--------------------------------------------------------------------------
            const float dt = io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : 1.0f / 60.0f;
            const double pan_uu = static_cast<double>(cfg.map_pan_speed) * static_cast<double>(dt) * g_mv.uu_per_px;
            const auto down = [](ImGuiKey a, ImGuiKey b) { return ImGui::IsKeyDown(a) || ImGui::IsKeyDown(b); };
            if (down(ImGuiKey_W, ImGuiKey_UpArrow))
            {
                g_mv.cx += pan_uu; // screen up is world +X (north)
            }
            if (down(ImGuiKey_S, ImGuiKey_DownArrow))
            {
                g_mv.cx -= pan_uu;
            }
            if (down(ImGuiKey_D, ImGuiKey_RightArrow))
            {
                g_mv.cy += pan_uu; // screen right is world +Y (east)
            }
            if (down(ImGuiKey_A, ImGuiKey_LeftArrow))
            {
                g_mv.cy -= pan_uu;
            }
            if (down(ImGuiKey_Equal, ImGuiKey_KeypadAdd))
            {
                zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(dt) * 6.0);
            }
            if (down(ImGuiKey_Minus, ImGuiKey_KeypadSubtract))
            {
                zoom_about(canvas.cx(), canvas.cy(), -static_cast<double>(dt) * 6.0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, true) || ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
            {
                g_map_floor_off += cfg.map_floor_step;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Q, true) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
            {
                g_map_floor_off -= cfg.map_floor_step;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                close_map(L"Escape");
            }
            // Keyboard equivalents of the two mouse actions, at the view centre. They
            // exist because the mouse cursor is the one part of this that depends on
            // what the game does with the cursor while we hold the input - with these
            // (and the gamepad) the map is fully usable if it turns out the cursor
            // cannot be moved freely.
            bool key_waypoint = ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            bool key_toggle = ImGui::IsKeyPressed(ImGuiKey_F, false);

            //--------------------------------------------------------------------------
            // Input: gamepad. The state is polled on the LOOP thread (gamepad.cpp) - as
            // with the keyboard, nothing here touches XInput or the game thread.
            //--------------------------------------------------------------------------
            bool pad_waypoint = false;
            bool pad_toggle = false;
            const pad::State gp = pad::state();
            if (cfg.map_gamepad && gp.connected)
            {
                if (gp.lx != 0.0f || gp.ly != 0.0f)
                {
                    g_mv.cx += static_cast<double>(gp.ly) * pan_uu;
                    g_mv.cy += static_cast<double>(gp.lx) * pan_uu;
                }
                const float trig = gp.rt - gp.lt;
                if (trig > 0.02f || trig < -0.02f)
                {
                    zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(trig * dt) * 8.0);
                }
                if (gp.ry > 0.02f || gp.ry < -0.02f)
                {
                    zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(gp.ry * dt) * 6.0);
                }
                const std::uint16_t pressed = pad::take_pressed();
                if ((pressed & pad::kRightShoulder) != 0)
                {
                    g_map_floor_off += cfg.map_floor_step;
                }
                if ((pressed & pad::kLeftShoulder) != 0)
                {
                    g_map_floor_off -= cfg.map_floor_step;
                }
                if ((pressed & pad::kY) != 0)
                {
                    g_map_recenter.store(true, std::memory_order_relaxed);
                }
                if ((pressed & pad::kA) != 0)
                {
                    pad_waypoint = true;
                }
                if ((pressed & pad::kX) != 0)
                {
                    pad_toggle = true;
                }
                if ((pressed & pad::kB) != 0)
                {
                    close_map(L"the gamepad B button");
                }
            }

            if (g_map_recenter.exchange(false, std::memory_order_relaxed))
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_map_floor_off = 0.0f;
            }
            g_map_floor_off = (std::max)(-20000.0f, (std::min)(20000.0f, g_map_floor_off));

            //--------------------------------------------------------------------------
            // The map picture
            //--------------------------------------------------------------------------
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1}, true);
            dl->AddRectFilled(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1},
                              IM_COL32(9, 12, 16, 255));

            const float feet = static_cast<float>(snap.z) - cfg.player_z_offset + g_map_floor_off;
            bool have_picture = false;
            if (chapter_ptr != nullptr && update_map_slice(cfg, *chapter_ptr, canvas, feet, now) &&
                g_mslice_shown >= 0)
            {
                const SliceBuf& b = g_mslice[g_mslice_shown];
                float sx0 = 0.0f;
                float sy0 = 0.0f;
                float sx1 = 0.0f;
                float sy1 = 0.0f;
                // Top-left of the cut region is its NORTH-WEST corner (max X, min Y).
                mv::world_to_screen(g_mv, canvas, g_mr_x1, g_mr_y0, sx0, sy0);
                mv::world_to_screen(g_mv, canvas, g_mr_x0, g_mr_y1, sx1, sy1);
                const ImTextureRef tex{static_cast<ImTextureID>(b.srv_gpu.ptr)};
                dl->AddImage(tex, ImVec2{sx0, sy0}, ImVec2{sx1, sy1}, ImVec2{0.0f, 0.0f}, ImVec2{1.0f, 1.0f},
                             IM_COL32(255, 255, 255, 255));
                have_picture = true;
            }

            //--------------------------------------------------------------------------
            // Markers
            //--------------------------------------------------------------------------
            const markers::View mv_all = markers::view();
            g_map_markers_drawn = 0;
            g_map_markers_total = static_cast<int>(mv_all.count);
            const markers::DrawMarker* hover = nullptr;
            const markers::DrawMarker* centre_marker = nullptr;
            float hover_d2 = 0.0f;
            float centre_d2 = 0.0f;
            const float mr = cfg.map_marker_size;
            const float pick_r = (std::max)(8.0f, mr * 1.6f);
            const float kCentrePickR = (std::max)(48.0f, mr * 5.0f);

            if (cfg.markers_enabled && mv_all.data != nullptr)
            {
                const int cap = cfg.map_markers_max_draw > 0 ? cfg.map_markers_max_draw
                                                            : static_cast<int>(mv_all.count);
                for (std::size_t i = 0; i < mv_all.count && g_map_markers_drawn < cap; ++i)
                {
                    const markers::DrawMarker& m = mv_all.data[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                    if (static_cast<int>(m.cat) >= mdb::kCatCount ||
                        !mdb::cat_enabled(cfg.markers_categories, cat))
                    {
                        continue;
                    }
                    const bool found = marker_found_now(m);
                    if (found && cfg.markers_hide_found)
                    {
                        continue;
                    }
                    float sx = 0.0f;
                    float sy = 0.0f;
                    mv::world_to_screen(g_mv, canvas, m.x, m.y, sx, sy);
                    if (sx < canvas.x0 - mr || sx > canvas.x1 + mr || sy < canvas.y0 - mr || sy > canvas.y1 + mr)
                    {
                        continue;
                    }
                    const int alpha = static_cast<int>((found ? cfg.markers_found_alpha : 1.0f) * 255.0f + 0.5f);
                    draw_marker_glyph(dl, cat, ImVec2{sx, sy}, mr, marker_color(cat, alpha),
                                      IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f)));
                    ++g_map_markers_drawn;

                    const float mdx = sx - io.MousePos.x;
                    const float mdy = sy - io.MousePos.y;
                    const float d2 = mdx * mdx + mdy * mdy;
                    if (canvas_hovered && d2 <= pick_r * pick_r && (hover == nullptr || d2 < hover_d2))
                    {
                        hover = &m;
                        hover_d2 = d2;
                    }
                    // The keyboard / gamepad "toggle found" acts on the marker
                    // nearest the CENTRE of the view - but only within the same radius
                    // a mouse would have to be in, so it can never reach a marker on
                    // the far side of the screen.
                    const float cdx = sx - canvas.cx();
                    const float cdy = sy - canvas.cy();
                    const float cd2 = cdx * cdx + cdy * cdy;
                    if (cd2 <= kCentrePickR * kCentrePickR && (centre_marker == nullptr || cd2 < centre_d2))
                    {
                        centre_marker = &m;
                        centre_d2 = cd2;
                    }
                }
            }

            //--------------------------------------------------------------------------
            // The waypoint and the player
            //--------------------------------------------------------------------------
            const mv::Waypoint wp = mm::waypoint();
            if (wp.set)
            {
                float sx = 0.0f;
                float sy = 0.0f;
                mv::world_to_screen(g_mv, canvas, wp.x, wp.y, sx, sy);
                if (canvas.contains(sx, sy))
                {
                    draw_waypoint_glyph(dl, ImVec2{sx, sy}, mr * 1.1f, 255);
                }
            }
            {
                float sx = 0.0f;
                float sy = 0.0f;
                mv::world_to_screen(g_mv, canvas, snap.x, snap.y, sx, sy);
                if (canvas.contains(sx, sy))
                {
                    // The view cone first, so the arrow sits on top of it.
                    const float a = snap.yaw * kPi / 180.0f;
                    const float len = 46.0f;
                    const float half = 32.0f * kPi / 180.0f;
                    const ImVec2 c{sx, sy};
                    const auto dir = [&](float ang) {
                        return ImVec2{c.x + std::sin(ang) * len, c.y - std::cos(ang) * len};
                    };
                    dl->AddTriangleFilled(c, dir(a - half), dir(a + half), IM_COL32(255, 226, 92, 46));
                    add_player_arrow(dl, c, snap.yaw, 11.0f);
                }
            }

            dl->PopClipRect();
            dl->AddRect(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1},
                        IM_COL32(120, 130, 145, 160), 0.0f, 0, 1.5f);

            //--------------------------------------------------------------------------
            // Clicks (after the draw, so `hover` is known)
            //--------------------------------------------------------------------------
            if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && drag_px < 5.0f &&
                hover != nullptr)
            {
                toggle_found(*hover);
            }
            if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                mv::Waypoint set{};
                set.set = true;
                mv::screen_to_world(g_mv, canvas, io.MousePos.x, io.MousePos.y, set.x, set.y);
                set.z = static_cast<double>(feet);
                mm::set_waypoint(set);
            }
            if (pad_waypoint || key_waypoint)
            {
                mv::Waypoint set{};
                set.set = true;
                set.x = g_mv.cx;
                set.y = g_mv.cy;
                set.z = static_cast<double>(feet);
                mm::set_waypoint(set);
            }
            if ((pad_toggle || key_toggle) && centre_marker != nullptr)
            {
                toggle_found(*centre_marker);
            }

            //--------------------------------------------------------------------------
            // Hover tooltip
            //--------------------------------------------------------------------------
            if (hover != nullptr)
            {
                ImGui::BeginTooltip();
                const mdb::Cat cat = static_cast<mdb::Cat>(hover->cat);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(marker_color(cat, 255)), "%s",
                                   hover->label[0] != '\0' ? hover->label : mdb::cat_label(cat));
                ImGui::Text("category: %s", mdb::cat_name(cat));
                ImGui::TextDisabled("%s", hover->id);
                const double ddx = hover->x - snap.x;
                const double ddy = hover->y - snap.y;
                const double ddz = hover->z - snap.z;
                ImGui::Text("%s   %.0f m away, %+0.0f m up",
                            marker_found_now(*hover) ? "FOUND" : "not found",
                            std::sqrt(ddx * ddx + ddy * ddy) / 100.0,
                            ddz / 100.0);
                ImGui::TextDisabled("left-click toggles found");
                ImGui::EndTooltip();
            }

            //--------------------------------------------------------------------------
            // Footer
            //--------------------------------------------------------------------------
            if (!have_picture)
            {
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f},
                                   chapter_ptr == nullptr
                                       ? "no chapter covers this position - markers only"
                                       : "no height maps for this chapter - markers only");
            }
            else
            {
                ImGui::TextDisabled("drag / WASD / arrows pan   wheel or +- zoom   ctrl+wheel or Q/E floor   "
                                    "%s recentre   right-click or Space waypoint   click a marker or F "
                                    "toggles found   %s or Esc closes",
                                    key_name_ascii(cfg.map_recenter_key).c_str(),
                                    key_name_ascii(cfg.map_key).c_str());
            }
            // The same key hints the F2 panel shows, from the same builder and the same
            // config - so the map's footer cannot go stale when a key is rebound.
            ImGui::TextDisabled("%s", bindings_hint(cfg).c_str());
            ImGui::TextDisabled("%d of %d marker(s)   cut %dx%d @ %.2f ms%s", g_map_markers_drawn,
                                g_map_markers_total, g_mslice[0].w, g_mslice[0].h, g_mslice_ms,
                                cfg.map_gamepad && gp.connected
                                    ? "   pad: stick pan, triggers zoom, LB/RB floor, A waypoint, X found, "
                                      "Y recentre, B close"
                                    : "");

            ImGui::End();

            if (std::memcmp(&before, &cfg, sizeof(mm::Config)) != 0)
            {
                mm::set_config(cfg);
            }
        }

        //==============================================================================
        // Drawing: the F2 panel
        //==============================================================================

        void draw_panel(mm::Config cfg, const mm::Snapshot& snap, bool have_state)
        {
            bool open = true;
            ImGui::SetNextWindowSize(ImVec2{460.0f, 0.0f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2{ImGui::GetMainViewport()->Pos.x + 60.0f,
                                           ImGui::GetMainViewport()->Pos.y + 60.0f},
                                    ImGuiCond_FirstUseEver);
            if (!ImGui::Begin("Wuchang Minimap  v" WUCHANG_MINIMAP_VERSION
                              "###wuchang_minimap_panel",
                              &open, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::End();
                if (!open)
                {
                    mm::g_panel_open = false;
                }
                return;
            }

            const mm::Config before = cfg;

            ImGui::TextColored(ImVec4{0.62f, 0.68f, 0.78f, 1.0f},
                               "WuchangMinimap v" WUCHANG_MINIMAP_VERSION
                               "  -  beta: chapters 2-5 maps and the x-ray highlight are not yet verified in-game");
            ImGui::Spacing();
            ImGui::Checkbox("Overlay enabled", &cfg.enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show minimap", &cfg.show_minimap);
            // THE MASTER SWITCH. Unticking it does not stop anything from here - it
            // only writes mod_enabled = 0 into the config file, which the loop thread's
            // 1 Hz watcher then acts on (modswitch.hpp). That keeps the whole shutdown
            // sequence on the one thread that is allowed to run it, and it means the
            // file and the running state can never disagree.
            if (ImGui::Checkbox("Mod enabled (master switch - turns EVERYTHING off)", &cfg.mod_enabled))
            {
                if (!cfg.mod_enabled)
                {
                    mm::set_config(cfg);
                    mm::g_save_config = true;
                    mm::log(L"master switch: turned off from the F2 panel - writing mod_enabled = 0; the "
                            L"mod stops within a second. Edit the config file to turn it back on.");
                }
            }
            if (!cfg.mod_enabled)
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f},
                                   "The mod is shutting down. Set mod_enabled = 1 in %s to restart it.",
                                   "config_wuchang_minimap.txt");
            }

            ImGui::SliderFloat("Size (fraction of screen height)", &cfg.size_frac, 0.08f, 0.6f, "%.2f");
            ImGui::SliderFloat("Zoom (uu per minimap pixel)", &cfg.zoom_uu_per_px, 4.0f, 200.0f, "%.0f");
            ImGui::SliderFloat("Opacity", &cfg.opacity, 0.15f, 1.0f, "%.2f");

            int shape = cfg.round ? 0 : 1;
            const char* shapes[] = {"round", "square"};
            if (ImGui::Combo("Shape", &shape, shapes, 2))
            {
                cfg.round = (shape == 0);
            }

            int anchor = static_cast<int>(cfg.anchor);
            const char* anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("Anchor", &anchor, anchors, 4))
            {
                cfg.anchor = static_cast<mm::Anchor>(anchor);
            }
            ImGui::DragFloat("Offset X", &cfg.offset_x, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::DragFloat("Offset Y", &cfg.offset_y, 1.0f, 0.0f, 2000.0f, "%.0f px");

            ImGui::Checkbox("Rotate with player (off = north up)", &cfg.rotate_with_player);
            ImGui::Checkbox("Show the floor below / above (dimmed)", &cfg.show_adjacent_floors);
            ImGui::SliderFloat("Adjacent floor opacity", &cfg.adjacent_floor_opacity, 0.0f, 0.6f, "%.2f");
            ImGui::SliderFloat("Floor Z tolerance (uu)", &cfg.floor_z_tolerance, 20.0f, 800.0f, "%.0f");
            ImGui::SliderFloat("Below / above fade range (uu)", &cfg.floor_fade_uu, 100.0f, 4000.0f, "%.0f");
            ImGui::SliderFloat("Height gradient strength", &cfg.floor_gradient_strength, 0.0f, 0.6f, "%.2f");
            ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -200.0f, 200.0f, "%.0f");
            ImGui::SliderInt("Slice rate (Hz)", &cfg.slice_hz, 2, 30);
            ImGui::Checkbox("Hide while a menu is open", &cfg.hide_in_menus);
            ImGui::SameLine();
            ImGui::Checkbox("Only when the camera follows the pawn", &cfg.require_pawn_view);
            ImGui::Checkbox("Debug readout", &cfg.debug_readout);

            //--------------------------------------------------------------------------
            // Markers
            //--------------------------------------------------------------------------
            if (ImGui::CollapsingHeader("Markers", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Show markers", &cfg.markers_enabled);
                ImGui::SameLine();
                ImGui::Checkbox("Live actor sweep", &cfg.markers_live);
                ImGui::SameLine();
                ImGui::Checkbox("Hide found", &cfg.markers_hide_found);
                // The chapters' world bounds overlap (chapter 4 covers nearly all of
                // chapter 1), so the static DB has to be cut down to the chapter the
                // player is actually in - otherwise foreign markers paint over the map.
                ImGui::Checkbox("Only this chapter's markers", &cfg.markers_filter_chapter);
                ImGui::SameLine();
                {
                    const markers::Stats fs = markers::stats();
                    if (!cfg.markers_filter_chapter)
                    {
                        ImGui::TextDisabled("(off - all chapters drawn)");
                    }
                    else if (fs.filter_chapter == chid::kNone)
                    {
                        ImGui::TextDisabled("(chapter not detected yet - all drawn)");
                    }
                    else if (fs.filter_chapter == chid::kDlc)
                    {
                        ImGui::TextDisabled("(showing DLC)");
                    }
                    else
                    {
                        ImGui::Text("(showing chapter %d)", fs.filter_chapter);
                    }
                }

                ImGui::SliderFloat("Marker size (px)", &cfg.markers_size, 2.0f, 16.0f, "%.1f");
                ImGui::SliderFloat("Found marker opacity", &cfg.markers_found_alpha, 0.0f, 1.0f, "%.2f");
                ImGui::Checkbox("Keep out-of-range markers on the rim", &cfg.markers_clamp_to_edge);

                // The two knobs that trade game-thread time for marker freshness.
                // Higher chunk / lower period = a fresher set and a costlier pump; the
                // "scan pump" line below is the read-out that says which way to move.
                ImGui::SliderInt("Object slots per pump", &cfg.markers_scan_chunk, scan::kChunkMin,
                                 scan::kChunkMax);
                ImGui::SliderInt("Min ms between pumps", &cfg.markers_scan_period_ms, scan::kPeriodMinMs,
                                 scan::kPeriodMaxMs);

                // The category filter. Exactly the same set of names the config file's
                // `markers_categories` list uses, so a filter set here and one written
                // into the file are one setting, not two.
                ImGui::SeparatorText("Categories");
                if (ImGui::SmallButton("All"))
                {
                    cfg.markers_categories = mdb::kAllCats;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("None"))
                {
                    cfg.markers_categories = 0u;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(config key: markers_categories)");

                const markers::Stats st = markers::stats();
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    const mdb::Cat cat = static_cast<mdb::Cat>(i);
                    bool on = mdb::cat_enabled(cfg.markers_categories, cat);
                    ImGui::PushID(i);
                    if (ImGui::Checkbox(mdb::cat_label(cat), &on))
                    {
                        if (on)
                        {
                            cfg.markers_categories |= mdb::cat_bit(cat);
                        }
                        else
                        {
                            cfg.markers_categories &= ~mdb::cat_bit(cat);
                        }
                    }
                    ImGui::PopID();
                    if ((i % 3) != 2 && i + 1 < mdb::kCatCount)
                    {
                        ImGui::SameLine(static_cast<float>(((i % 3) + 1) * 150));
                    }
                }

                //----------------------------------------------------------------------
                // The collection tracker
                //----------------------------------------------------------------------
                ImGui::SeparatorText("Collection tracker");
                ImGui::Checkbox("Write wuchang_minimap_found.txt", &cfg.found_tracker);
                if (!st.db_loaded || st.static_markers == 0)
                {
                    ImGui::TextDisabled("no markers\\<chapter>.json loaded - live markers only");
                }
                else if (ImGui::BeginTable("found", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Chapter");
                    ImGui::TableSetupColumn("Shrines");
                    ImGui::TableSetupColumn("Chests");
                    ImGui::TableSetupColumn("Pickups");
                    ImGui::TableHeadersRow();
                    const auto cell = [](const markers::CatStat& s) {
                        ImGui::TableNextColumn();
                        if (s.total == 0)
                        {
                            ImGui::TextDisabled("-");
                        }
                        else
                        {
                            ImGui::Text("%d / %d", s.found, s.total);
                        }
                    };
                    // Row 0 is the bucket for a manifest whose "chapter" is not a
                    // number - the DLC one spells it "DLC".
                    for (int ch = 0; ch <= 8; ++ch)
                    {
                        int any = 0;
                        for (int i = 0; i < mdb::kCatCount; ++i)
                        {
                            any += st.chapter[ch][i].total;
                        }
                        if (any == 0)
                        {
                            continue;
                        }
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ch == 0)
                        {
                            ImGui::Text("DLC");
                        }
                        else
                        {
                            ImGui::Text("%d", ch);
                        }
                        cell(st.chapter[ch][static_cast<int>(mdb::Cat::Shrine)]);
                        cell(st.chapter[ch][static_cast<int>(mdb::Cat::Chest)]);
                        cell(st.chapter[ch][static_cast<int>(mdb::Cat::Pickup)]);
                    }
                    // The summary row follows the filter: when only one chapter is
                    // drawn, a whole-DB total would count five chapters the player
                    // cannot see and can never collect from here.
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const int fch = st.filter_chapter;
                    if (fch >= 0 && fch <= 8)
                    {
                        ImGui::TextDisabled("current");
                        cell(st.chapter[fch][static_cast<int>(mdb::Cat::Shrine)]);
                        cell(st.chapter[fch][static_cast<int>(mdb::Cat::Chest)]);
                        cell(st.chapter[fch][static_cast<int>(mdb::Cat::Pickup)]);
                    }
                    else
                    {
                        ImGui::TextDisabled("all");
                        cell(st.cat[static_cast<int>(mdb::Cat::Shrine)]);
                        cell(st.cat[static_cast<int>(mdb::Cat::Chest)]);
                        cell(st.cat[static_cast<int>(mdb::Cat::Pickup)]);
                    }
                    ImGui::EndTable();
                }
                ImGui::Text("db %d marker(s) / %d chapter(s)   found file %d id(s)   published %d   live %d",
                            st.static_markers,
                            st.chapters_loaded,
                            st.found_ids,
                            st.published,
                            st.live_entries);
                // Two independent numbers, and they answer different questions.
                //   PUMP  - what one game-thread pump costs. This is the frame-hitch
                //           number; the target is well under 1 ms, and `max` is the
                //           worst single pump since the mod loaded.
                //   ROUND - what a full pass over the object array cost and how many
                //           object slots it visited. This is the freshness number: the
                //           marker set is `slices x period_ms` old at worst.
                // `!` marks the FindAllOf fallback, which is the old 28 ms-per-pump
                // path and only runs when GUObjectArray reports no elements.
                ImGui::Text("scan pump %.3f ms (avg %.3f, peak %.3f, max %.3f)%s",
                            st.scan_slice_ms,
                            st.scan_slice_ms_avg,
                            st.scan_slice_ms_peak,
                            st.scan_slice_ms_max,
                            st.scan_fallback ? "   ! FindAllOf fallback" : "");
                ImGui::Text("round %.1f ms / %d pump(s) / %d object(s) of %d   chunk %d   %llu round(s)",
                            st.scan_round_ms,
                            st.scan_round_slices,
                            st.scan_round_objects,
                            st.scan_total,
                            st.scan_chunk,
                            static_cast<unsigned long long>(st.rounds));
                ImGui::Text("drawn %d of %d (%d clamped, %d filtered)",
                            g_marker_draw.drawn,
                            g_marker_draw.total,
                            g_marker_draw.clamped,
                            g_marker_draw.filtered);
                if (!g_marker_draw.nearest.empty())
                {
                    ImGui::Text("nearest: %s (%.0f uu)", g_marker_draw.nearest.c_str(), g_marker_draw.nearest_uu);
                }
            }

            //--------------------------------------------------------------------------
            // The full map
            //--------------------------------------------------------------------------
            if (ImGui::CollapsingHeader("Full map"))
            {
                ImGui::TextDisabled("Press %s in-world. The minimap hides while it is open.",
                                    key_name_ascii(cfg.map_key).c_str());
                ImGui::SliderFloat("Zoom on open (uu per screen px)", &cfg.map_zoom, cfg.map_zoom_min,
                                   cfg.map_zoom_max, "%.0f");
                ImGui::SliderFloat("Zoom limit - closest", &cfg.map_zoom_min, 1.0f, 200.0f, "%.0f");
                ImGui::SliderFloat("Zoom limit - furthest", &cfg.map_zoom_max, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Zoom per wheel notch", &cfg.map_zoom_factor, 1.02f, 1.6f, "%.2f");
                ImGui::SliderFloat("Pan speed (screen px per second)", &cfg.map_pan_speed, 100.0f, 4000.0f,
                                   "%.0f");
                ImGui::SliderFloat("Floor step (uu)", &cfg.map_floor_step, 20.0f, 2000.0f, "%.0f");
                ImGui::SliderFloat("Marker size (px)", &cfg.map_marker_size, 3.0f, 24.0f, "%.1f");
                ImGui::Checkbox("Show every floor (ignore the height slice)", &cfg.map_show_all_floors);
                ImGui::SliderInt("Slice texture width (px)", &cfg.map_slice_px, 256, 2048);
                ImGui::SliderInt("Slice rate cap (Hz)", &cfg.map_slice_hz, 1, 30);
                ImGui::Checkbox("Gamepad (XInput)", &cfg.map_gamepad);
                ImGui::SameLine();
                ImGui::Checkbox("Remember the waypoint", &cfg.map_waypoint_persist);
                ImGui::SliderFloat("Gamepad deadzone", &cfg.map_gamepad_deadzone, 0.05f, 0.6f, "%.2f");

                const pad::State gp = pad::state();
                char padmod[64]{};
                ::WideCharToMultiByte(CP_UTF8, 0, pad::module_name(), -1, padmod, sizeof(padmod) - 1, nullptr,
                                      nullptr);
                ImGui::Text("pad: %s (%s)   sticks %.2f,%.2f / %.2f,%.2f   triggers %.2f/%.2f",
                            gp.connected ? "connected" : "none",
                            padmod,
                            static_cast<double>(gp.lx),
                            static_cast<double>(gp.ly),
                            static_cast<double>(gp.rx),
                            static_cast<double>(gp.ry),
                            static_cast<double>(gp.lt),
                            static_cast<double>(gp.rt));
                ImGui::Text("map slice %dx%d   %.2f ms (peak %.2f)   %llu cut(s), %llu skipped   "
                            "opaque %u / dim %u / faint %u",
                            g_mslice[0].w,
                            g_mslice[0].h,
                            g_mslice_ms,
                            g_mslice_ms_peak,
                            static_cast<unsigned long long>(g_mslice_updates),
                            static_cast<unsigned long long>(g_mslice_skipped),
                            g_mslice_counts.opaque,
                            g_mslice_counts.dim,
                            g_mslice_counts.faint);
                const mv::Waypoint wp = mm::waypoint();
                if (wp.set)
                {
                    ImGui::Text("waypoint  X %.0f  Y %.0f  Z %.0f", wp.x, wp.y, wp.z);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear waypoint"))
                    {
                        mm::set_waypoint(mv::Waypoint{});
                    }
                }
                else
                {
                    ImGui::TextDisabled("no waypoint (right-click on the full map sets one)");
                }
            }

            //--------------------------------------------------------------------------
            // The hold-key x-ray highlight
            //--------------------------------------------------------------------------
            if (ImGui::CollapsingHeader("X-ray highlight (hold a key)"))
            {
                std::string hold = key_name_ascii(cfg.highlight_key);
                if (cfg.highlight_gamepad)
                {
                    hold += " or pad " + wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                                         cfg.highlight_pad_lt,
                                                                         cfg.highlight_pad_rt));
                }
                ImGui::TextWrapped("HOLD %s in-world: every uncollected marker of the categories below, "
                                   "within the radius, is drawn through walls at its position on screen.",
                                   hold.c_str());
                ImGui::Checkbox("Enabled", &cfg.highlight_enabled);
                ImGui::SameLine();
                ImGui::Checkbox("Gamepad chord", &cfg.highlight_gamepad);
                ImGui::SliderFloat("Radius (uu)", &cfg.highlight_radius, 200.0f, 20000.0f, "%.0f");
                ImGui::SameLine();
                ImGui::TextDisabled("= %.0f m", static_cast<double>(cfg.highlight_radius) / 100.0);
                ImGui::Checkbox("Names + distance", &cfg.highlight_labels);
                ImGui::SameLine();
                ImGui::Checkbox("Edge arrows (off screen / behind)", &cfg.highlight_edge_arrows);
                ImGui::SameLine();
                ImGui::Checkbox("Include found", &cfg.highlight_show_found);
                ImGui::SliderFloat("Glyph size (px)", &cfg.highlight_size, 2.0f, 24.0f, "%.1f");
                ImGui::SliderFloat("Alpha at the camera", &cfg.highlight_alpha_near, 0.1f, 1.0f, "%.2f");
                ImGui::SliderFloat("Alpha at the radius", &cfg.highlight_alpha_far, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max drawn (nearest first)", &cfg.highlight_max_draw, 1, 400);
                ImGui::SliderInt("Camera read rate (Hz)", &cfg.highlight_camera_hz, 5, 240);

                // The category filter: the same names the config file uses.
                std::uint32_t hcats = cfg.highlight_categories;
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    const mdb::Cat cat = static_cast<mdb::Cat>(i);
                    bool on = mdb::cat_enabled(hcats, cat);
                    if (i % 4 != 0)
                    {
                        ImGui::SameLine();
                    }
                    ImGui::PushID(i + 200);
                    if (ImGui::Checkbox(mdb::cat_name(cat), &on))
                    {
                        hcats = on ? (hcats | mdb::cat_bit(cat)) : (hcats & ~mdb::cat_bit(cat));
                    }
                    ImGui::PopID();
                }
                cfg.highlight_categories = hcats;

                // WHERE THE CAMERA COMES FROM. This is the block to screenshot if the
                // labels are in the wrong place: it names the route, the pinned offset
                // and how old the pose is.
                const hl::Stats hs = hl::stats();
                const char* route = "none yet";
                switch (hs.route)
                {
                case hl::Route::RawPinned:
                    route = "raw POV read (offset calibrated against the getters)";
                    break;
                case hl::Route::RawSane:
                    route = "raw POV read (offset accepted on sanity ranges only)";
                    break;
                case hl::Route::Getters:
                    route = "GetCameraLocation / GetCameraRotation / GetFOVAngle per read";
                    break;
                case hl::Route::None:
                default:
                    break;
                }
                ImGui::Text("camera: %s", route);
                ImGui::Text("manager %s   CameraCachePrivate +%d   POV +%d   %llu read(s), %llu rejected",
                            hs.have_manager ? "yes" : "NO",
                            hs.cache_offset,
                            hs.pov_offset,
                            static_cast<unsigned long long>(hs.reads),
                            static_cast<unsigned long long>(hs.fails));
                hl::Pose pose{};
                if (hl::camera(pose))
                {
                    ImGui::Text("pose  X %.0f  Y %.0f  Z %.0f   pitch %.1f  yaw %.1f  roll %.1f   FOV %.1f   "
                                "%llu ms old",
                                pose.x,
                                pose.y,
                                pose.z,
                                pose.pitch,
                                pose.yaw,
                                pose.roll,
                                pose.fov,
                                static_cast<unsigned long long>(
                                    pose.stamp_ms == 0 ? 0 : ::GetTickCount64() - pose.stamp_ms));
                }
                else
                {
                    ImGui::TextDisabled("no camera pose published yet (hold the key in-world)");
                }
                ImGui::Text("held %s   %d of %d in range drawn (%d on screen, %d on the rim)",
                            g_hl_debug.active ? "YES" : "no",
                            g_hl_debug.drawn,
                            g_hl_debug.considered,
                            g_hl_debug.on_screen,
                            g_hl_debug.edge);
            }

            //--------------------------------------------------------------------------
            // The compass strip
            //--------------------------------------------------------------------------
            if (ImGui::CollapsingHeader("Compass"))
            {
                ImGui::Checkbox("Enabled", &cfg.compass_enabled);
                ImGui::SameLine();
                ImGui::Checkbox("Show the waypoint bearing", &cfg.compass_show_waypoint);
                ImGui::SliderFloat("Width (fraction of the screen)", &cfg.compass_width, 0.1f, 1.0f, "%.2f");
                ImGui::SliderFloat("Distance from the top (px)", &cfg.compass_offset_y, 0.0f, 400.0f, "%.0f");
                ImGui::SliderFloat("Height (px)", &cfg.compass_height, 10.0f, 120.0f, "%.0f");
                ImGui::SliderFloat("Degrees across the strip", &cfg.compass_span_deg, 30.0f, 360.0f, "%.0f");
                ImGui::SliderFloat("Opacity", &cfg.compass_opacity, 0.1f, 1.0f, "%.2f");
                ImGui::SliderFloat("Marker bearing range (uu)", &cfg.compass_marker_distance, 500.0f, 60000.0f,
                                   "%.0f");
                std::uint32_t ccats = cfg.compass_categories;
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    const mdb::Cat cat = static_cast<mdb::Cat>(i);
                    bool on = mdb::cat_enabled(ccats, cat);
                    if (i % 4 != 0)
                    {
                        ImGui::SameLine();
                    }
                    ImGui::PushID(i + 100);
                    if (ImGui::Checkbox(mdb::cat_name(cat), &on))
                    {
                        ccats = on ? (ccats | mdb::cat_bit(cat)) : (ccats & ~mdb::cat_bit(cat));
                    }
                    ImGui::PopID();
                }
                cfg.compass_categories = ccats;
                ImGui::Text("%s   heading %.1f deg from the %s   %d bearing pip(s)",
                            g_compass_debug.visible ? "visible" : "hidden (same gate as the minimap)",
                            g_compass_debug.heading,
                            g_compass_debug.from_camera ? "camera" : "pawn",
                            g_compass_debug.pips);
            }

            if (ImGui::Button("Save settings"))
            {
                mm::g_save_config = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload settings + maps"))
            {
                mm::g_reload_config = true;
            }
            ImGui::Spacing();
            // The key hints, read from the config so they stay truthful after a rebind.
            ImGui::SeparatorText("Keys");
            ImGui::TextWrapped("%s", bindings_hint(cfg).c_str());

            if (cfg.debug_readout)
            {
                ImGui::SeparatorText("Debug");
                if (!have_state)
                {
                    ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.4f, 1.0f}, "no game-state snapshot yet");
                }
                else
                {
                    const std::uint64_t age = ::GetTickCount64() - snap.stamp_ms;
                    ImGui::Text("world  X %.1f  Y %.1f  Z %.1f   yaw %.1f deg", snap.x, snap.y, snap.z, snap.yaw);
                    ImGui::Text("uv     %.5f, %.5f   chapter '%s'",
                                g_last_mini.u,
                                g_last_mini.v,
                                g_last_mini.chapter.empty() ? "-" : g_last_mini.chapter.c_str());
                    ImGui::Text("gameplay pawn %s   transition %s   state-ok age %llu ms",
                                snap.pawn_is_gameplay ? "yes" : "NO",
                                snap.transition ? "YES" : "no",
                                static_cast<unsigned long long>(
                                    snap.state_ok_since_ms == 0 ? 0 : ::GetTickCount64() - snap.state_ok_since_ms));
                    // The height slicer: what it cut, how much it cost, where it is.
                    ImGui::Text("slice  %dx%d px x %d surface(s) @ %.4f px/uu   %.2f ms (peak %.2f)   "
                                "%llu update(s), %llu skipped",
                                g_slice_size,
                                g_slice_size,
                                g_slice_surfaces,
                                g_slice_px_per_uu,
                                g_slice_ms,
                                g_slice_ms_peak,
                                static_cast<unsigned long long>(g_slice_updates),
                                static_cast<unsigned long long>(g_slice_skipped));
                    ImGui::Text("       feet Z %.0f (raw %.0f)   tol %.0f  fade %.0f  gradient %.2f   "
                                "opaque %u / dim %u / faint %u",
                                static_cast<double>(g_feet_z),
                                snap.z - static_cast<double>(cfg.player_z_offset),
                                static_cast<double>(cfg.floor_z_tolerance),
                                static_cast<double>(cfg.floor_fade_uu),
                                static_cast<double>(cfg.floor_gradient_strength),
                                g_slice_opaque,
                                g_slice_dim,
                                g_slice_faint);
                    ImGui::Text("pawn %s   pawn-view %s   menu %s   state age %llu ms",
                                snap.has_pawn ? "yes" : "no",
                                snap.is_pawn_view ? "yes" : "no",
                                snap.menu_open ? "OPEN" : "no",
                                static_cast<unsigned long long>(age));
                    ImGui::Text("widgets seen %u, visible in viewport %u   location via %s",
                                snap.widgets_seen,
                                snap.widgets_visible_in_viewport,
                                snap.loc_from_function ? "K2_GetActorLocation" : "RootComponent");
                    // Menu hide/show latency: how long ago the game thread saw the menu
                    // state change, and how many roots it re-tests on every pump.
                    char holder[128]{};
                    ::WideCharToMultiByte(CP_UTF8, 0, snap.menu_holder, -1, holder, sizeof(holder) - 1, nullptr,
                                          nullptr);
                    ImGui::Text("menu state changed %llu ms ago   %u cached in-viewport root(s)   "
                                "show delay %d ms   holder '%s'",
                                static_cast<unsigned long long>(
                                    snap.menu_change_ms == 0 ? 0 : ::GetTickCount64() - snap.menu_change_ms),
                                snap.menu_roots_cached,
                                cfg.menu_close_show_delay_ms,
                                holder[0] != '\0' ? holder : "-");
                    ImGui::Text("teleport %s",
                                snap.teleport_ms == 0
                                    ? "never this session"
                                    : std::format("{} ms ago",
                                                  ::GetTickCount64() - snap.teleport_ms)
                                          .c_str());
                    char narrow[256]{};
                    ::WideCharToMultiByte(CP_UTF8, 0, snap.level_name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
                    ImGui::TextWrapped("pawn: %s", narrow);
                }

                ImGui::Spacing();
                char reason[192]{};
                ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr, nullptr);
                // THE ONE LINE THAT ANSWERS "why is the minimap not there". It is
                // recomputed from live state every frame - there is no latch anywhere in
                // the show condition - and every change to it is also logged.
                if (g_last_mini.visible)
                {
                    ImGui::TextColored(ImVec4{0.55f, 0.9f, 0.6f, 1.0f}, "minimap: %s", reason);
                }
                else
                {
                    ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f}, "hidden because: %s", reason);
                }
                ImGui::TextDisabled("       this state has held for %llu ms",
                                    static_cast<unsigned long long>(
                                        g_reason_since_ms == 0 ? 0 : ::GetTickCount64() - g_reason_since_ms));
                ImGui::Text("backbuffer %ux%u, %u buffer(s), composite %dx%d %s",
                            g_width,
                            g_height,
                            g_buffer_count,
                            g_map.width,
                            g_map.height,
                            g_map.ready ? "ready" : "NOT ready");
                ImGui::Text("presents %llu, resizes %llu",
                            static_cast<unsigned long long>(g_present_count.load()),
                            static_cast<unsigned long long>(g_resize_count.load()));
            }

            ImGui::End();

            if (std::memcmp(&before, &cfg, sizeof(mm::Config)) != 0)
            {
                mm::set_config(cfg);
            }
            if (!open)
            {
                mm::g_panel_open = false;
            }
        }

        //==============================================================================
        // Per-frame work
        //==============================================================================

        void build_ui()
        {
            const mm::Config cfg = mm::config();
            // The disc-drawing helpers take geometry, not the config, so the live
            // roundness is cached here once per frame (render thread only).
            g_circle_segments = cfg.minimap_circle_segments;
            mm::Snapshot snap{};
            const bool have = mm::read_snapshot(snap);

            const bool map_open = mm::g_map_open.load(std::memory_order_relaxed);

            // The mouse cursor belongs to whoever is taking the input. Both conditions
            // are plain reads of the live flags - nothing here is remembered, so the
            // frame the map or the panel closes is the frame the game gets the cursor
            // back.
            ImGui::GetIO().MouseDrawCursor = map_open || mm::g_panel_open.load(std::memory_order_relaxed);

            if (mm::g_panel_open.load(std::memory_order_relaxed))
            {
                draw_panel(cfg, snap, have);
                mm::g_panel_drew_frame.store(true, std::memory_order_relaxed);
            }

            if (map_open)
            {
                draw_full_map(cfg, snap, have);
            }
            if (g_map_was_open && !mm::g_map_open.load(std::memory_order_relaxed))
            {
                // Closed (by the key, by the gate, or from inside the map): the next
                // open starts centred on the player again.
                g_mv_init = false;
            }
            g_map_was_open = mm::g_map_open.load(std::memory_order_relaxed);

            if (!cfg.enabled || !cfg.show_minimap)
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
                draw_minimap(cfg, snap, have);
            }

            // The compass and the x-ray highlight ask the SAME gate the minimap does -
            // one evaluation, no second set of rules, no second latch - and additionally
            // stand down while the full map is open, because the map is a mode of its own
            // (it swallows the input and covers the scene they would be drawn over).
            const bool gate_ok = hud_gate(cfg, snap, have, ::GetTickCount64()) == nullptr;
            const bool hud_ok = gate_ok && !mm::g_map_open.load(std::memory_order_relaxed);
            draw_compass(cfg, snap, hud_ok);
            draw_highlight(cfg, snap, hud_ok);
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

            if (!create_render_targets(swapchain))
            {
                g_failed = true;
                return false;
            }

            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&g_frames[i].allocator))))
                {
                    mm::log(L"CreateCommandAllocator failed");
                    g_failed = true;
                    return false;
                }
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

            g_prev_wndproc = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hooked_wndproc)));
            if (g_prev_wndproc == nullptr)
            {
                mm::logf(L"SetWindowLongPtr(GWLP_WNDPROC) failed (error {}) - the F2 panel will get no mouse input",
                         static_cast<unsigned>(::GetLastError()));
            }

            g_imgui_ready = true;
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

        UINT current_backbuffer_index(IDXGISwapChain* swapchain)
        {
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
            {
                const UINT index = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
                if (index < g_buffer_count)
                {
                    return index;
                }
            }
            static UINT fallback = 0;
            fallback = (fallback + 1) % (g_buffer_count == 0 ? 1 : g_buffer_count);
            return fallback;
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

        void shutdown_render()
        {
            if (g_imgui_ready || g_device != nullptr)
            {
                wait_for_gpu();
                destroy_all_map_textures();
                if (g_imgui_ready)
                {
                    if (g_hwnd != nullptr && g_prev_wndproc != nullptr)
                    {
                        ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                            reinterpret_cast<LONG_PTR>(g_prev_wndproc));
                    }
                    g_prev_wndproc = nullptr;
                    ImGui_ImplDX12_Shutdown();
                    ImGui_ImplWin32_Shutdown();
                    ImGui::DestroyContext();
                    g_imgui_ready = false;
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
                mm::log(L"master switch: the render thread has released ImGui, the descriptor heaps, "
                        L"the slice buffers and the map textures");
            }
            g_swapchain = nullptr;
            g_candidates_logged = 0;
            g_queue.store(nullptr, std::memory_order_release);
            g_failed = false;
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
                    SpinGuard guard(g_render_lock);
                    shutdown_render();
                }
                return;
            }

            g_present_count.fetch_add(1, std::memory_order_relaxed);
            if (g_failed || g_queue.load(std::memory_order_acquire) == nullptr)
            {
                return;
            }

            SpinGuard guard(g_render_lock);
            if (g_swapchain == nullptr)
            {
                const bool d3d12 = is_d3d12_swapchain(swapchain);
                log_candidate(swapchain, d3d12);
                if (!d3d12)
                {
                    return;
                }
                g_swapchain = swapchain;
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
                return;
            }

            const UINT index = current_backbuffer_index(swapchain);
            FrameCtx& frame = g_frames[index];
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
            if (g_drop_textures.exchange(false, std::memory_order_acq_rel))
            {
                wait_for_gpu();
                destroy_all_map_textures();
                mm::log(L"map textures and slice buffers dropped for a reload");
            }
            release_finished_uploads();

            ImGui_ImplWin32_NewFrame();
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            build_ui();
            ImGui::Render();

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

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_cmd_list->ResourceBarrier(1, &barrier);
            g_cmd_list->Close();

            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            ID3D12CommandList* lists[] = {g_cmd_list};
            // The ORIGINAL, so our own submission does not re-enter the hook.
            o_ExecuteCommandLists(queue, 1, lists);
            ++g_fence_value;
            queue->Signal(g_fence, g_fence_value);
            frame.fence_value = g_fence_value;
            // The buffer this frame sampled may not be rewritten until the GPU is past
            // this fence.
            if (g_slice_shown >= 0)
            {
                g_slice[g_slice_shown].in_flight_fence = g_fence_value;
            }
            if (g_mslice_shown >= 0)
            {
                g_mslice[g_mslice_shown].in_flight_fence = g_fence_value;
            }

            if (g_map.upload != nullptr && g_map.upload_fence == 0)
            {
                g_map.upload_fence = g_fence_value;
            }
            else if (g_map.upload != nullptr && g_fence->GetCompletedValue() >= g_map.upload_fence)
            {
                safe_release(g_map.upload); // the copy has landed; give the 90 MB back
            }
        }

        //==============================================================================
        // Hooks
        //==============================================================================

        HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            render(sc);
            return o_Present(sc, sync, flags);
        }

        HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
        {
            render(sc);
            return o_Present1(sc, sync, flags, params);
        }

        HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                                   UINT flags)
        {
            if (!mm::mod_active())
            {
                return o_ResizeBuffers(sc, count, w, h, format, flags);
            }
            g_resize_count.fetch_add(1, std::memory_order_relaxed);
            {
                SpinGuard guard(g_render_lock);
                mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}) - releasing render targets",
                         count,
                         w,
                         h,
                         format_name(format));
                if (g_imgui_ready && sc == g_swapchain)
                {
                    wait_for_gpu();
                }
                if (sc == g_swapchain)
                {
                    release_render_targets();
                }
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
            if (mm::mod_active() && g_queue.load(std::memory_order_relaxed) == nullptr && queue != nullptr)
            {
                const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
                if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    ID3D12CommandQueue* expected = nullptr;
                    if (g_queue.compare_exchange_strong(expected, queue))
                    {
                        mm::logf(L"captured the game's DIRECT command queue {:p} (priority {}, flags {})",
                                 static_cast<void*>(queue),
                                 desc.Priority,
                                 static_cast<unsigned>(desc.Flags));
                    }
                }
            }
            o_ExecuteCommandLists(queue, count, lists);
        }

        //==============================================================================
        // Hook installation via a throwaway device + swapchain
        //==============================================================================

        bool install_hooks()
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

                void* present = sc_vtable[8];        // IDXGISwapChain::Present
                void* resize = sc_vtable[13];        // IDXGISwapChain::ResizeBuffers
                void* present1 = sc_vtable[22];      // IDXGISwapChain1::Present1
                void* execute = q_vtable[10];        // ID3D12CommandQueue::ExecuteCommandLists

                const MH_STATUS s1 = MH_CreateHook(present, reinterpret_cast<void*>(&hk_Present),
                                                   reinterpret_cast<void**>(&o_Present));
                const MH_STATUS s2 = MH_CreateHook(resize, reinterpret_cast<void*>(&hk_ResizeBuffers),
                                                   reinterpret_cast<void**>(&o_ResizeBuffers));
                const MH_STATUS s3 = MH_CreateHook(present1, reinterpret_cast<void*>(&hk_Present1),
                                                   reinterpret_cast<void**>(&o_Present1));
                const MH_STATUS s4 = MH_CreateHook(execute, reinterpret_cast<void*>(&hk_ExecuteCommandLists),
                                                   reinterpret_cast<void**>(&o_ExecuteCommandLists));
                const MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);

                g_hook_report = std::format(L"Present {} @ {} | ResizeBuffers {} @ {} | Present1 {} @ {} | "
                                            L"ExecuteCommandLists {} @ {} | enable {}",
                                            static_cast<int>(s1),
                                            module_of(present),
                                            static_cast<int>(s2),
                                            module_of(resize),
                                            static_cast<int>(s3),
                                            module_of(present1),
                                            static_cast<int>(s4),
                                            module_of(execute),
                                            static_cast<int>(en));
                mm::logf(L"hooks: {}", g_hook_report);
                mm::logf(L"hook addresses: Present {:p}  ResizeBuffers {:p}  Present1 {:p}  ExecuteCommandLists {:p}",
                         present,
                         resize,
                         present1,
                         execute);
                ok = (s1 == MH_OK && s4 == MH_OK && en == MH_OK);
                if (!ok)
                {
                    mm::log(L"at least one required hook did not install - the overlay will not draw");
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

        //==============================================================================
        // Selftest entry points (kept so the third-party objects cannot be dropped)
        //==============================================================================

        using Dx12InitFn = bool (*)(ImGui_ImplDX12_InitInfo*);
        using Win32InitFn = bool (*)(void*);

        const void* const g_entry_points[] = {
            reinterpret_cast<const void*>(static_cast<Dx12InitFn>(&ImGui_ImplDX12_Init)),
            reinterpret_cast<const void*>(static_cast<Win32InitFn>(&ImGui_ImplWin32_Init)),
            reinterpret_cast<const void*>(&MH_Initialize),
            reinterpret_cast<const void*>(&MH_CreateHook),
        };

        auto widen(std::string_view narrow) -> RC::StringType
        {
            return RC::StringType{narrow.begin(), narrow.end()};
        }
    } // namespace

    auto selftest() -> RC::StringType
    {
        std::size_t linked = 0;
        for (const void* const entry : g_entry_points)
        {
            if (entry != nullptr)
            {
                ++linked;
            }
        }
        return std::format(STR("imgui {} ({}), minhook reachable ({}), {}/{} entry points linked"),
                           widen(IMGUI_VERSION),
                           IMGUI_VERSION_NUM,
                           widen(MH_StatusToString(MH_ERROR_NOT_INITIALIZED)),
                           linked,
                           std::size(g_entry_points));
    }

    void start()
    {
        mm::load_waypoint_file();
        mapdata::load(mm::mod_dir());

        // The render thread is allowed to build its objects again from the next frame.
        g_render_stopped.store(false, std::memory_order_release);

        const mm::Config cfg = mm::config();
        if (!cfg.enabled)
        {
            mm::log(L"overlay disabled by config (enabled = 0) - no hooks installed");
            mm::drain_log();
            return;
        }

        if (!g_hooks_created)
        {
            g_hooks_created = install_hooks();
        }
        else
        {
            // Re-enable after a master-switch disable. The trampolines were never
            // removed, so this can never install a second hook on the same address.
            const MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
            g_hooks_installed.store(en == MH_OK, std::memory_order_release);
            g_hook_install_ms = ::GetTickCount64();
            g_watchdog_reported = false;
            mm::logf(L"master switch: the existing DX12 hooks were re-enabled (MH_EnableHook = {})",
                     static_cast<int>(en));
        }

        if (cfg.debug_show_panel_on_start)
        {
            mm::g_panel_open = true;
            mm::log(L"debug_show_panel_on_start = 1: the F2 panel starts open (turn it off for normal play)");
        }
        mm::logf(L"hotkeys: {} settings panel, {} full map ({} recentres it), {} reload "
                 L"config + maps + markers, HOLD {} (pad {}) for the x-ray highlight [{}]; "
                 L"compass {}",
                 mm::key_name(cfg.panel_key),
                 mm::key_name(cfg.map_key),
                 mm::key_name(cfg.map_recenter_key),
                 mm::key_name(cfg.reload_key),
                 mm::key_name(cfg.highlight_key),
                 mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt),
                 cfg.highlight_enabled ? L"on" : L"off",
                 cfg.compass_enabled ? L"on" : L"off");
        mm::drain_log();
    }

    //======================================================================================
    // The master switch (loop thread)
    //======================================================================================

    void request_stop()
    {
        // mm::g_mod_active is already false, so the next Present takes the teardown
        // path. If no frame is coming - the game is minimised, or the hooks never fired
        // at all - stop_complete() below answers for it.
        if (!g_hooks_installed.load(std::memory_order_acquire) || g_present_count.load() == 0)
        {
            SpinGuard guard(g_render_lock);
            shutdown_render(); // nothing of ours is in flight; safe from this thread
        }
    }

    bool stop_complete()
    {
        return g_render_stopped.load(std::memory_order_acquire);
    }

    void finish_stop()
    {
        if (!g_hooks_created)
        {
            return;
        }
        const MH_STATUS st = MH_DisableHook(MH_ALL_HOOKS);
        g_hooks_installed.store(false, std::memory_order_release);
        mm::logf(L"master switch: the DX12 hooks were disabled (MH_DisableHook = {}); the trampolines "
                 L"stay created so turning the mod back on cannot double-hook",
                 static_cast<int>(st));
    }

    void on_update()
    {
        // UE4SS EVENT-LOOP THREAD. No D3D12, no UObjects.
        static bool panel_down = false;
        static bool reload_down = false;
        static std::uint64_t last_key = 0;
        static bool logged_first_present = false;

        const mm::Config cfg = mm::config();
        const std::uint64_t now = ::GetTickCount64();

        const HWND fg = ::GetForegroundWindow();
        DWORD pid = 0;
        if (fg != nullptr)
        {
            ::GetWindowThreadProcessId(fg, &pid);
        }
        const bool foreground = (pid == ::GetCurrentProcessId());

        const bool panel_now = (::GetAsyncKeyState(cfg.panel_key) & 0x8000) != 0;
        if (panel_now && !panel_down && foreground && now - last_key > 250)
        {
            last_key = now;
            const bool open = !mm::g_panel_open.load();
            mm::g_panel_open = open;
            mm::logf(L"settings panel {}", open ? L"opened" : L"closed");
        }
        panel_down = panel_now;

        const bool reload_now = (::GetAsyncKeyState(cfg.reload_key) & 0x8000) != 0;
        if (reload_now && !reload_down && foreground && now - last_key > 250)
        {
            last_key = now;
            mm::g_reload_config = true;
        }
        reload_down = reload_now;

        // The full map. GetAsyncKeyState rather than a WndProc test on purpose: while
        // the map is open the WndProc hook swallows every key, so the message-based
        // route could not close it again.
        static bool map_down = false;
        const bool map_now = (::GetAsyncKeyState(cfg.map_key) & 0x8000) != 0;
        if (map_now && !map_down && foreground && now - last_key > 250)
        {
            last_key = now;
            const bool open = !mm::g_map_open.load();
            mm::g_map_open = open;
            mm::logf(L"full map {}", open ? L"opened" : L"closed");
        }
        map_down = map_now;

        static bool recenter_down = false;
        const bool recenter_now = (::GetAsyncKeyState(cfg.map_recenter_key) & 0x8000) != 0;
        if (recenter_now && !recenter_down && foreground && mm::g_map_open.load())
        {
            g_map_recenter.store(true, std::memory_order_relaxed);
        }
        recenter_down = recenter_now;

        // XInput, on THIS thread - the same place the keyboard is sampled, and never on
        // the game thread (lessons.md). Polling a disconnected pad is expensive, so it
        // only runs while something wants it: the full map, or the highlight's chord.
        const bool want_pad = (cfg.map_gamepad && mm::g_map_open.load()) ||
                              (cfg.highlight_enabled && cfg.highlight_gamepad &&
                               (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt));
        pad::poll(want_pad, cfg.map_gamepad_deadzone);

        // THE X-RAY HIGHLIGHT'S HOLD KEY. A hold, not a toggle - so it is sampled as a
        // level, never edge-detected, and it needs no debounce and no "close it again"
        // path. The window must be in the foreground, or alt-tabbing away with the key
        // down would leave the game thread reading the camera forever.
        bool held = foreground && cfg.highlight_enabled &&
                    (::GetAsyncKeyState(cfg.highlight_key) & 0x8000) != 0;
        if (!held && foreground && cfg.highlight_enabled && cfg.highlight_gamepad)
        {
            const pad::State gp = pad::state();
            const bool chord = (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt) &&
                               (gp.held & cfg.highlight_pad_mask) == cfg.highlight_pad_mask &&
                               (!cfg.highlight_pad_lt || gp.lt > 0.5f) && (!cfg.highlight_pad_rt || gp.rt > 0.5f);
            held = gp.connected && chord;
        }
        // This is what makes the game thread read the camera at all: with neither the
        // highlight held nor the compass on, highlight.cpp costs one atomic load a pump.
        hl::set_demand(held, cfg.enabled && cfg.compass_enabled);

        // The waypoint is set on the render thread and written here, because the loop
        // thread is the only one allowed to touch a file.
        if (mm::g_waypoint_dirty.exchange(false))
        {
            if (cfg.map_waypoint_persist)
            {
                mm::save_waypoint_file();
            }
        }

        if (mm::g_reload_config.exchange(false))
        {
            mm::log(L"reloading config + maps + markers");
            mm::load_config_file();
            mm::load_waypoint_file();
            g_drop_textures.store(true, std::memory_order_release);
            mapdata::load(mm::mod_dir());
            markers::reload();
        }
        if (mm::g_save_config.exchange(false))
        {
            mm::save_config_file();
        }

        if (!logged_first_present && g_present_count.load() > 0)
        {
            logged_first_present = true;
            mm::logf(L"first Present seen; the hook is live (count {})", g_present_count.load());
        }
        // The panel renders every frame, so this is throttled hard: once when it first
        // draws, then at most one line every 10 s. (Without the throttle a single
        // main-menu verification run wrote 2 100 identical lines.)
        static std::uint64_t last_panel_log = 0;
        if (mm::g_panel_drew_frame.exchange(false) && (last_panel_log == 0 || now - last_panel_log > 10000))
        {
            last_panel_log = now;
            mm::log(L"the settings panel is rendering");
        }
        if (!g_watchdog_reported.load() && g_hooks_installed.load() && g_hook_install_ms != 0 &&
            now - g_hook_install_ms > 8000 && g_present_count.load() == 0)
        {
            g_watchdog_reported = true;
            mm::log(L"WATCHDOG: 8 s after installing the hooks not a single Present has arrived. The game's "
                    L"swapchain is behind a proxy we did not create ours through (a DLSS frame-generation wrapper "
                    L"is the likely candidate). Loaded graphics modules follow:");
            static const wchar_t* const suspects[] = {L"dxgi.dll",       L"d3d12.dll",  L"ReShade64.dll",
                                                      L"nvngx_dlssg.dll", L"sl.interposer.dll", L"sl.dlss_g.dll",
                                                      L"amd_fidelityfx_dx12.dll"};
            for (const wchar_t* name : suspects)
            {
                const HMODULE mod = ::GetModuleHandleW(name);
                if (mod != nullptr)
                {
                    wchar_t path[MAX_PATH * 2]{};
                    ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
                    mm::logf(L"  loaded: {} -> {}", name, path);
                }
            }
        }

        mm::drain_log();
    }
} // namespace overlay
