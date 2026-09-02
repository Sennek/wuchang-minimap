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

#include "mapdata.hpp"
#include "mmstate.hpp"

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
        constexpr int kCircleSegments = 72;

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
            int size = 0;                 // side, px
            bool needs_copy = false;      // filled by the CPU, copy not recorded yet
            bool in_copy_dest = true;     // resource state tracking for the barriers
            UINT64 in_flight_fence = 0;   // last frame that sampled it
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
        // Per-pixel scratch for the PLANE-MAJOR slice pass (see slice_window).
        std::vector<std::uint8_t> g_slice_state;
        std::vector<float> g_slice_best_ad;
        std::vector<float> g_slice_best_d;

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
        constexpr std::uint64_t kReasonLogMs = 2000;
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
            if (now - g_reason_log_ms < kReasonLogMs)
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

                // Input is only ever taken away from the game while the F2 panel is up.
                // With the panel closed the minimap is a pure overlay and every message
                // goes straight through, so gameplay input is untouched.
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

        void destroy_slice_buffers()
        {
            for (SliceBuf& b : g_slice)
            {
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
            g_slice_size = 0;
            g_slice_next = 0;
            g_slice_shown = -1;
            g_slice_last_ms = 0;
        }

        void destroy_all_map_textures()
        {
            destroy_texture(g_map);
            destroy_slice_buffers();
            g_feet_z_valid = false;
            g_slice_state.clear();
            g_slice_best_ad.clear();
            g_slice_best_d.clear();
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
            dl->PrimReserve(kCircleSegments * 3, kCircleSegments + 1);
            const unsigned int base = dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(g.center, uv_at(g, 0.0f, 0.0f), col);
            for (int i = 0; i < kCircleSegments; ++i)
            {
                const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kCircleSegments);
                const float dx = std::cos(a) * g.half;
                const float dy = std::sin(a) * g.half;
                dl->PrimWriteVtx(ImVec2{g.center.x + dx, g.center.y + dy}, uv_at(g, dx, dy), col);
            }
            for (int i = 0; i < kCircleSegments; ++i)
            {
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + ((i + 1) % kCircleSegments)));
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

        bool create_slice_buffers(int size)
        {
            destroy_slice_buffers();
            if (g_device == nullptr || size <= 0)
            {
                return false;
            }

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(size);
            desc.Height = static_cast<UINT>(size);
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

            for (SliceBuf& b : g_slice)
            {
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&b.tex))))
                {
                    mm::logf(L"slice: CreateCommittedResource({}x{} RGBA) failed", size, size);
                    destroy_slice_buffers();
                    return false;
                }
                if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&b.upload))))
                {
                    mm::logf(L"slice: upload buffer of {} KB failed", total / 1024);
                    destroy_slice_buffers();
                    return false;
                }
                void* mapped = nullptr;
                D3D12_RANGE none{0, 0};
                if (FAILED(b.upload->Map(0, &none, &mapped)) || mapped == nullptr)
                {
                    mm::log(L"slice: Map() of the upload buffer failed");
                    destroy_slice_buffers();
                    return false;
                }
                b.mapped = static_cast<std::uint8_t*>(mapped);
                if (!g_srv_heap.alloc(b.srv_cpu, b.srv_gpu))
                {
                    mm::log(L"slice: no free SRV descriptor");
                    destroy_slice_buffers();
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
                b.size = size;
                b.in_copy_dest = true;
                b.in_flight_fence = 0;
                b.needs_copy = false;
            }
            g_slice_size = size;
            g_slice_next = 0;
            g_slice_shown = -1;
            mm::logf(L"slice: {} dynamic texture(s) of {}x{} RGBA created ({} KB each, {} KB of "
                     L"mapped upload memory)",
                     kSliceBufs,
                     size,
                     size,
                     total / 1024,
                     (total * kSliceBufs) / 1024);
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
            if (size < kSliceMinPx)
            {
                size = kSliceMinPx;
            }
            if (size > kSliceMaxPx)
            {
                size = kSliceMaxPx;
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
        void slice_window(const mapdata::HeightMaps& hm, int x0, int y0, int size, std::uint8_t* dst,
                          UINT pitch, float feet, const SliceStyle& st)
        {
            g_slice_opaque = 0;
            g_slice_dim = 0;
            g_slice_faint = 0;

            const std::size_t n = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
            if (g_slice_state.size() != n)
            {
                g_slice_state.assign(n, 0);
                g_slice_best_ad.assign(n, 0.0f);
                g_slice_best_d.assign(n, 0.0f);
            }
            else
            {
                std::memset(g_slice_state.data(), 0, n);
            }
            std::uint8_t* state = g_slice_state.data();
            float* best_ad = g_slice_best_ad.data();
            float* best_d = g_slice_best_d.data();

            const float z0 = hm.z_min;
            const float step = hm.z_step();
            const int planes = hm.count < mapdata::kMaxSurfaces ? hm.count : mapdata::kMaxSurfaces;
            g_slice_surfaces = planes;

            // Rows and columns of the window that exist in the source at all; the rest
            // stay state 0 and come out transparent.
            const int row_lo = y0 < 0 ? -y0 : 0;
            const int row_hi = (y0 + size) > hm.height ? hm.height - y0 : size;
            const int col_lo = x0 < 0 ? -x0 : 0;
            const int col_hi = (x0 + size) > hm.width ? hm.width - x0 : size;

            for (int k = 0; k < planes; ++k)
            {
                const std::uint16_t* plane = hm.plane[k].data();
                if (hm.plane[k].empty())
                {
                    continue;
                }
                const std::uint8_t cand_state_none = 0;
                (void)cand_state_none;
                for (int row = row_lo; row < row_hi; ++row)
                {
                    const std::uint16_t* src = plane + static_cast<std::size_t>(y0 + row) *
                                                           static_cast<std::size_t>(hm.width) +
                                               static_cast<std::size_t>(x0);
                    const std::size_t out_base = static_cast<std::size_t>(row) * static_cast<std::size_t>(size);
                    for (int col = col_lo; col < col_hi; ++col)
                    {
                        const std::uint16_t code = src[col];
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
            for (int row = 0; row < size; ++row)
            {
                std::uint8_t* out = dst + static_cast<std::size_t>(row) * pitch;
                const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(size);
                for (int col = 0; col < size; ++col)
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
                        ++g_slice_opaque;
                    }
                    else if (cls == 2)
                    {
                        px[3] = a_dim;
                        ++g_slice_dim;
                    }
                    else
                    {
                        px[3] = a_faint;
                        ++g_slice_faint;
                    }
                }
            }
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
            if (b.tex == nullptr || b.mapped == nullptr)
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
            const int x0 = static_cast<int>(std::lround(pxc)) - b.size / 2;
            const int y0 = static_cast<int>(std::lround(pyc)) - b.size / 2;

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
            slice_window(hm, x0, y0, b.size, b.mapped + b.footprint.Offset, b.footprint.Footprint.RowPitch,
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
            const int wx0 = (hm.width - b.size) / 2;
            const int wy0 = (hm.height - b.size) / 2;
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
                        sx0 = px - b.size / 2;
                        sy0 = py - b.size / 2;
                        break;
                    }
                }
            }
            slice_window(hm, sx0, sy0, b.size, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, probe_z, st);
            ::QueryPerformanceCounter(&t1);
            const double ms = freq.QuadPart > 0 ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                                      static_cast<double>(freq.QuadPart)
                                                : 0.0;
            // Deliberately NOT marked needs_copy: nothing may be drawn at the main menu.
            mm::logf(L"slice: self-test sliced a {}x{} window of \"{}\" at source ({}, {}), feet Z "
                     L"{:.0f}, over {} surface(s) in {:.2f} ms (opaque {}, dim {}, faint {}) - the CPU "
                     L"path and the dynamic texture both work",
                     b.size,
                     b.size,
                     std::wstring(ch->key.begin(), ch->key.end()),
                     sx0 + b.size / 2,
                     sy0 + b.size / 2,
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
        void record_slice_copy(ID3D12GraphicsCommandList* list)
        {
            for (SliceBuf& b : g_slice)
            {
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

        void draw_minimap(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            g_last_mini = MiniDebug{};

            if (!have_state)
            {
                set_hide_reason(L"no game-state snapshot yet");
                return;
            }
            const std::uint64_t now = ::GetTickCount64();
            if (snap.stamp_ms == 0 || now - snap.stamp_ms > static_cast<std::uint64_t>(cfg.state_stale_ms))
            {
                set_hide_reason(L"game state is stale (game thread not pumping)");
                return;
            }
            if (!snap.transition && !snap.has_pawn)
            {
                set_hide_reason(L"no player pawn");
                return;
            }
            if (snap.transition)
            {
                set_hide_reason(L"level transition in progress (reader idling)");
                return;
            }
            // The class gate lives on the game thread; this is its render-side echo.
            if (!snap.pawn_is_gameplay)
            {
                set_hide_reason(L"the pawn is not a gameplay pawn (Lobby / spectator)");
                return;
            }
            // A fresh gameplay pawn must have been valid for a while before anything is
            // drawn: without this the first snapshot after a load can flash the minimap.
            if (snap.state_ok_since_ms == 0)
            {
                set_hide_reason(L"waiting for a valid gameplay state");
                return;
            }
            if (now - snap.state_ok_since_ms < static_cast<std::uint64_t>(cfg.min_visible_after_state_ok_ms))
            {
                set_hide_reason(L"gameplay state is too fresh (grace period)");
                return;
            }
            // HIDING IS IMMEDIATE: the game thread now re-tests the cached in-viewport
            // menu roots on every pump (10 Hz), so this is true within ~100 ms of the
            // inventory opening.
            if (cfg.hide_in_menus && snap.menu_open)
            {
                set_hide_reason(L"a menu is open");
                return;
            }
            // Showing again waits only menu_close_show_delay_ms - a menu closing is not
            // a level transition, so it must not pay min_visible_after_state_ok_ms.
            if (cfg.hide_in_menus && snap.menu_change_ms != 0 &&
                now - snap.menu_change_ms < static_cast<std::uint64_t>(cfg.menu_close_show_delay_ms))
            {
                set_hide_reason(L"the menu just closed (short show delay)");
                return;
            }
            if (cfg.require_pawn_view && !snap.is_pawn_view)
            {
                set_hide_reason(L"view target is not the pawn (menu / cutscene / Lobby)");
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
            const float side = (std::max)(72.0f, (std::min)(cfg.size_frac * screen_h, (std::min)(screen_w, screen_h) * 0.9f));

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
            const ImU32 backdrop = IM_COL32(6, 9, 13, alpha(0.86f));
            const ImU32 frame = IM_COL32(168, 176, 186, alpha(0.85f));
            const ImU32 inner_ring = IM_COL32(0, 0, 0, alpha(0.55f));

            // The slice texture already carries the floor colour, the height gradient
            // and the per-pixel alpha, so the only tint left is the global opacity.
            const ImU32 tint_slice = IM_COL32(255, 255, 255, alpha(1.0f));
            const ImU32 tint_composite = IM_COL32(255, 255, 255, alpha(0.85f));

            ImDrawList* dl = ImGui::GetForegroundDrawList();

            if (cfg.round)
            {
                dl->AddCircleFilled(g.center, g.half, backdrop, kCircleSegments);
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
                const UvMap window{g_slice_min_y, g_slice_max_x, g_slice_px_per_uu, b.size, b.size};
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
                dl->AddCircle(g.center, g.half - 1.0f, inner_ring, kCircleSegments, 2.0f);
                dl->AddCircle(g.center, g.half, frame, kCircleSegments, 2.0f);
            }
            else
            {
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, frame, 4.0f, 0, 2.0f);
            }

            // North marker, only meaningful when the map itself is rotating.
            if (cfg.rotate_with_player)
            {
                const float na = -eff_yaw * kPi / 180.0f;
                const ImVec2 np{g.center.x + std::sin(na) * (g.half - 12.0f),
                                g.center.y - std::cos(na) * (g.half - 12.0f)};
                dl->AddCircleFilled(np, 3.5f, IM_COL32(230, 90, 80, 235), 12);
            }

            add_player_arrow(dl, g.center, snap.yaw - eff_yaw, (std::max)(8.0f, side * 0.055f));

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
        // Drawing: the F2 panel
        //==============================================================================

        void draw_panel(mm::Config cfg, const mm::Snapshot& snap, bool have_state)
        {
            bool open = true;
            ImGui::SetNextWindowSize(ImVec2{460.0f, 0.0f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2{ImGui::GetMainViewport()->Pos.x + 60.0f,
                                           ImGui::GetMainViewport()->Pos.y + 60.0f},
                                    ImGuiCond_FirstUseEver);
            if (!ImGui::Begin("Wuchang Minimap", &open, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::End();
                if (!open)
                {
                    mm::g_panel_open = false;
                }
                return;
            }

            const mm::Config before = cfg;

            ImGui::Checkbox("Overlay enabled", &cfg.enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show minimap", &cfg.show_minimap);

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

            if (ImGui::Button("Save settings"))
            {
                mm::g_save_config = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload settings + maps"))
            {
                mm::g_reload_config = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("F2 panel  |  F5 reload");

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
            mm::Snapshot snap{};
            const bool have = mm::read_snapshot(snap);

            if (mm::g_panel_open.load(std::memory_order_relaxed))
            {
                ImGui::GetIO().MouseDrawCursor = true;
                draw_panel(cfg, snap, have);
                mm::g_panel_drew_frame.store(true, std::memory_order_relaxed);
            }
            else
            {
                ImGui::GetIO().MouseDrawCursor = false;
            }

            if (cfg.enabled && cfg.show_minimap)
            {
                draw_minimap(cfg, snap, have);
            }
            else
            {
                set_hide_reason(L"disabled in the config");
            }
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

            if (!g_srv_heap.create(g_device, kSrvHeapSize))
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

        void render(IDXGISwapChain* swapchain)
        {
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
            if (g_queue.load(std::memory_order_relaxed) == nullptr && queue != nullptr)
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

    void on_unreal_init()
    {
        mm::set_loop_thread();
        mm::load_config_file();
        mapdata::load(mm::mod_dir());

        const mm::Config cfg = mm::config();
        if (!cfg.enabled)
        {
            mm::log(L"overlay disabled by config (enabled = 0) - no hooks installed");
            mm::drain_log();
            return;
        }

        install_hooks();

        if (cfg.debug_show_panel_on_start)
        {
            mm::g_panel_open = true;
            mm::log(L"debug_show_panel_on_start = 1: the F2 panel starts open (turn it off for normal play)");
        }
        mm::logf(L"hotkeys: F{} settings panel, F{} reload config + maps",
                 cfg.panel_key - VK_F1 + 1,
                 cfg.reload_key - VK_F1 + 1);
        mm::drain_log();
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

        if (mm::g_reload_config.exchange(false))
        {
            mm::log(L"reloading config + maps");
            mm::load_config_file();
            g_drop_textures.store(true, std::memory_order_release);
            mapdata::load(mm::mod_dir());
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
