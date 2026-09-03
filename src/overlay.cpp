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
#include "glyphs.hpp"
#include "highlight.hpp"
#include "label_layout.hpp"
#include "mapdata.hpp"
#include "mapview.hpp"
#include "breadcrumb.hpp"
#include "clipimg.hpp"
#include "markers.hpp"
#include "modswitch.hpp"
#include "recon.hpp"
#include "shrines.hpp"
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
        //==============================================================================
        // UI SCALE (review-0.9.1 § 1 item 1)
        //==============================================================================
        //
        // ImGui's built-in font is 13 px and nothing here ever touched it, so on a 4K
        // screen every label, tooltip, x-ray name and compass letter was a quarter of
        // its intended physical size - the single biggest legibility problem in the mod.
        //
        // ONE number fixes all of it. `ui_scale = auto` derives it from the back buffer
        // height; a number in the config pins it. It is applied in exactly two places:
        //
        //   apply_ui_scale()  - the ImGui font size and the whole style, on the RENDER
        //                       thread, before the frame's draw lists are built (the
        //                       same rule the F5 texture drop obeys).
        //   ui_scaled()       - every PIXEL config key, multiplied once per frame into
        //                       a copy of the Config the HUD draws from.
        //
        // Nothing else in the drawing code knows about it, which is what stops the
        // scale being applied twice to something or not at all to something else.
        //
        // ImGui 1.92's font atlas is dynamic (ImGuiBackendFlags_RendererHasTextures),
        // so `style.FontScaleMain` re-rasterises the glyphs at the new size on its own -
        // there is no atlas to rebuild and no texture of ours to release.
        constexpr float kBaseScreenHeight = 1080.0f;
        constexpr float kUiScaleMin = 0.5f;
        constexpr float kUiScaleMax = 4.0f;
        float g_ui_scale = 1.0f;          // what the HUD is currently drawn at
        float g_ui_scale_applied = 0.0f;  // what the ImGui style was last built for

        float wanted_ui_scale(const mm::Config& cfg, float screen_h)
        {
            float s = cfg.ui_scale;
            if (cfg.ui_scale_auto)
            {
                // 1080p is the design size, so 1.0 there and 2.0 at 2160p. Never below
                // 1: shrinking the UI on a small screen helps nobody.
                s = screen_h > 0.0f ? screen_h / kBaseScreenHeight : 1.0f;
                s = (std::max)(1.0f, (std::min)(kUiScaleMax, s));
            }
            else
            {
                s = (std::max)(kUiScaleMin, (std::min)(kUiScaleMax, s));
            }
            // Snap to a hundredth: a back buffer of 1081 px must not make the style be
            // rebuilt on the next frame for a difference nobody can see.
            return std::round(s * 100.0f) / 100.0f;
        }

        // Render thread. Rebuilds the ImGui style FROM SCRATCH at the new scale - never
        // ScaleAllSizes on the already-scaled style, which would compound every time.
        void apply_ui_scale(float scale)
        {
            if (scale == g_ui_scale_applied)
            {
                return;
            }
            ImGuiStyle& style = ImGui::GetStyle();
            style = ImGuiStyle{};
            ImGui::StyleColorsDark();
            style.WindowRounding = 4.0f;
            style.ScaleAllSizes(scale);
            style.FontScaleMain = scale;
            g_ui_scale_applied = scale;
            g_ui_scale = scale;
            mm::logf(L"ui scale: {:.2f} (font {:.0f} px, style rebuilt)", scale, 13.0f * scale);
        }

        // Every config key that is a NUMBER OF PIXELS, multiplied once. Fractions of the
        // screen (minimap_size, compass_width, map_margin) are already resolution
        // independent and are deliberately absent.
        mm::Config ui_scaled(const mm::Config& cfg, float s)
        {
            mm::Config out = cfg;
            if (s == 1.0f)
            {
                return out;
            }
            out.markers_size *= s;
            out.map_marker_size *= s;
            out.highlight_size *= s;
            out.compass_height *= s;
            out.compass_offset_y *= s;
            out.offset_x *= s;
            out.offset_y *= s;
            out.minimap_min_px *= s;
            out.minimap_arrow_min_px *= s;
            return out;
        }

        //==============================================================================
        // HUD PLACEMENT (review-0.9.1 § 1 item 12)
        //==============================================================================
        //
        // `hud_preset` is one key that moves the WHOLE HUD. `custom` (the shipped
        // default) means "obey minimap_anchor / minimap_offset_* / compass_anchor
        // exactly as written", i.e. v0.9.1 unchanged; any other value puts the minimap
        // in that corner and the compass on the same vertical side, so the two can no
        // longer end up on opposite halves of the screen because one key was edited and
        // the other was not.
        //
        // Both readers are pure functions of the config, evaluated per frame, so a
        // preset change is live and there is nothing to keep in sync.
        mm::Anchor effective_anchor(const mm::Config& cfg)
        {
            switch (cfg.hud_preset)
            {
            case mm::HudPreset::TopLeft:
                return mm::Anchor::TopLeft;
            case mm::HudPreset::TopRight:
                return mm::Anchor::TopRight;
            case mm::HudPreset::BottomLeft:
                return mm::Anchor::BottomLeft;
            case mm::HudPreset::BottomRight:
                return mm::Anchor::BottomRight;
            case mm::HudPreset::Custom:
            default:
                return cfg.anchor;
            }
        }

        bool compass_at_bottom(const mm::Config& cfg)
        {
            switch (cfg.hud_preset)
            {
            case mm::HudPreset::BottomLeft:
            case mm::HudPreset::BottomRight:
                return true;
            case mm::HudPreset::TopLeft:
            case mm::HudPreset::TopRight:
                return false;
            case mm::HudPreset::Custom:
            default:
                return cfg.compass_anchor == mm::VAnchor::Bottom;
            }
        }

        // Roundness of the minimap disc and its rings. This was `minimap_circle_segments`
        // until 0.9.2: a sanity dial, never a preference, so it is a constant now. The
        // drawing helpers are handed geometry rather than the config, hence the global.
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
            bool in_copy_dest = true;     // resource state tracking for the barriers
            // `needs_copy` (loop -> render: the CPU has filled this one) and
            // `in_flight_fence` (render -> loop: the last frame that sampled it) used to
            // live here. The slice runs on the LOOP thread now, so both are atomics in
            // the parallel arrays below; everything left in this struct is written only
            // while the slicer is paused.
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

        // Perf counter ids (perf.hpp). Namespace-scope, initialised on first use by
        // their single owning thread - never a guarded function static, because one of
        // these paths is entered from the game thread's callback chain.
        int g_pf_frame = -1;    // the whole render prologue + build_ui
        int g_pf_minimap = -1;  // draw_minimap
        int g_pf_markpass = -1; // build_frame_candidates
        int g_pf_slice = -1;    // the minimap height-slice cut (loop thread)
        int g_pf_mslice = -1;   // the full map's cut (loop thread)
        int g_pf_input = -1;    // the hotkey block (loop thread)

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

        // How long the render thread will wait for the loop-thread slicer to leave its
        // critical section before giving up on (re)allocating buffers this frame. The
        // slice itself is 1-4 ms and never blocks, so this is generous by an order of
        // magnitude and a timeout means something is badly wrong.
        constexpr unsigned kSlicerPauseMs = 50;

        constexpr int kSliceBufs = 2;
        // Declared here rather than with the full map's own block below, because the
        // slicer handshake arrays need both counts.
        constexpr int kMapSliceBufs = 2;
        // Bounds on the square the CPU slicer cuts for the minimap. Hard-coded since
        // 0.9.2 (they were `slice_min_px` / `slice_max_px`); slice_size_for() reads them.
        constexpr int kSliceMinPx = 128;
        constexpr int kSliceMaxPx = 1024;
        // How much bigger than the visible canvas the FULL MAP's cut is, so a drag can
        // move inside the cut before it has to be redone (1.30 = 15 % of the canvas in
        // either direction). Was `map_slice_margin`.
        constexpr double kMapSliceMargin = 1.30;
        SliceBuf g_slice[kSliceBufs];
        int g_slice_next = 0;  // the buffer the next update writes (loop thread)
        int g_slice_size = 0;   // side of the currently allocated buffers, px
        std::uint64_t g_slice_last_ms = 0;
        // DIAGNOSTICS ONLY, and written by the LOOP thread (the slicer) while the F2
        // panel reads them on the render thread. They are lone scalars refreshed many
        // times a second; a torn read would show one stale number for one frame, which
        // is why they are not atomics.
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

        std::atomic<bool> g_slicer_pause{false};
        std::atomic<bool> g_slicer_busy{false};
        std::atomic<std::uint32_t> g_slice_gen{0};

        // loop -> render: this buffer has been filled and its copy is not recorded yet.
        std::atomic<bool> g_slice_copy_pending[kSliceBufs];
        std::atomic<bool> g_mslice_copy_pending[kMapSliceBufs];
        // render -> loop: the fence value of the last frame that SAMPLED this buffer.
        std::atomic<std::uint64_t> g_slice_in_flight[kSliceBufs];
        std::atomic<std::uint64_t> g_mslice_in_flight[kMapSliceBufs];

        // render -> loop: what the minimap needs. 0 = the minimap is not drawing, so
        // the slicer stands down (one relaxed load per loop iteration).
        std::atomic<int> g_slice_want_px{0};
        std::atomic<std::uint64_t> g_slice_want_ms{0}; // GetTickCount64 of the last request

        // render -> loop: the full map's viewport. Only meaningful while the map is open.
        struct MapSliceReq
        {
            bool wanted = false;
            double cx = 0.0;
            double cy = 0.0;
            double zoom = 0.0;
            float canvas_w = 0.0f;
            float canvas_h = 0.0f;
            float feet = 0.0f;
            bool show_all_floors = false;
        };
        Spinlock g_slice_req_lock;
        MapSliceReq g_map_req;
        std::atomic<std::uint64_t> g_map_req_ms{0}; // GetTickCount64 of the last request

        // loop -> render: which buffer to draw and the world mapping it covers. Copied
        // under the lock so the index and its geometry can never disagree.
        struct SliceView
        {
            int shown = -1;
            double min_y = 0.0;
            double max_x = 0.0;
            double px_per_uu = 0.0;
        };
        struct MapSliceView
        {
            int shown = -1;
            bool valid = false;
            double x0 = 0.0; // south edge
            double x1 = 0.0; // north edge
            double y0 = 0.0; // west edge
            double y1 = 0.0; // east edge
        };
        Spinlock g_slice_view_lock;
        SliceView g_slice_view;
        MapSliceView g_mslice_view;

        SliceView slice_view()
        {
            SpinGuard guard(g_slice_view_lock);
            return g_slice_view;
        }

        MapSliceView map_slice_view()
        {
            SpinGuard guard(g_slice_view_lock);
            return g_mslice_view;
        }

        void clear_slice_view()
        {
            SpinGuard guard(g_slice_view_lock);
            g_slice_view = SliceView{};
        }

        void clear_map_slice_view()
        {
            SpinGuard guard(g_slice_view_lock);
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

        SliceBuf g_mslice[kMapSliceBufs];
        int g_mslice_next = 0;
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
        // Render -> loop: "throw the cut region away and re-cut" (the map was recentred).
        // g_mr_* is the loop thread's now, so the render thread may not clear it itself.
        std::atomic<bool> g_map_recut{false};
        int g_mr_w = 0; // the buffer size the cut was made at (a resize is urgent)
        int g_mr_h = 0;
        double g_mr_zoom = 0.0;
        float g_mr_feet = 0.0f;
        double g_mr_px = 0.0; // the player position the cut was made at
        double g_mr_py = 0.0;
        std::string g_mr_chapter;

        // The view itself. Render thread only.
        mv::View g_mv{};
        bool g_mv_init = false;
        float g_map_floor_off = 0.0f; // uu added to feet Z by the floor adjustment
        // The full map's controls legend (`?` / pad Back). Render thread only, and
        // deliberately NOT config: it is a thing you glance at, not a setting.
        bool g_map_help = false;
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
            if (now - g_reason_log_ms < static_cast<std::uint64_t>(mm::cfg_cached().hide_reason_log_ms))
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

        // RENDER THREAD, and only with the slicer paused.
        void destroy_slice_buffers()
        {
            destroy_slice_set(g_slice, kSliceBufs);
            for (int i = 0; i < kSliceBufs; ++i)
            {
                g_slice_copy_pending[i].store(false);
                g_slice_in_flight[i].store(0);
            }
            g_slice_size = 0;
            g_slice_next = 0;
            clear_slice_view();
            g_slice_last_ms = 0;
            note_slice_buffers_changed();
        }

        // RENDER THREAD, and only with the slicer paused.
        void destroy_map_slice_buffers()
        {
            destroy_slice_set(g_mslice, kMapSliceBufs);
            for (int i = 0; i < kMapSliceBufs; ++i)
            {
                g_mslice_copy_pending[i].store(false);
                g_mslice_in_flight[i].store(0);
            }
            g_mslice_next = 0;
            clear_map_slice_view();
            g_mslice_last_ms = 0;
            g_mr_valid = false;
            note_slice_buffers_changed();
        }

        // RENDER THREAD. Every caller must already have the slicer paused; the two
        // that matter (the F5 texture drop and shutdown_render) do it around
        // wait_for_gpu() as well, so the GPU is idle AND the loop thread is out.
        void destroy_all_map_textures()
        {
            destroy_texture(g_map);
            destroy_slice_buffers();
            destroy_map_slice_buffers();
            g_feet_z_valid = false;
            g_slice_scratch.clear();
            // g_mslice_scratch belongs to the loop thread now, but the slicer is paused
            // here, so clearing it is safe and keeps the memory from a closed map.
            g_mslice_scratch.clear();
            g_slice_want_px.store(0, std::memory_order_relaxed);
            {
                SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
            }
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

        // RENDER THREAD, and only with the slicer paused.
        bool create_slice_buffers(int size)
        {
            if (!create_slice_set(g_slice, kSliceBufs, size, size, L"minimap"))
            {
                destroy_slice_buffers();
                return false;
            }
            for (int i = 0; i < kSliceBufs; ++i)
            {
                g_slice_copy_pending[i].store(false);
                g_slice_in_flight[i].store(0);
            }
            g_slice_size = size;
            g_slice_next = 0;
            clear_slice_view();
            note_slice_buffers_changed();
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

        // The slice style, built from the config. Both slicers use it and the loop
        // thread builds its own copy, so it lives in one place.
        SliceStyle style_from(const mm::Config& cfg)
        {
            SliceStyle st{};
            st.base_r = cfg.floor_base_r;
            st.base_g = cfg.floor_base_g;
            st.base_b = cfg.floor_base_b;
            st.strength = cfg.floor_gradient_strength;
            st.tol = cfg.floor_z_tolerance;
            st.fade = cfg.floor_fade_uu;
            st.a_dim = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity : 0.0f;
            st.a_faint = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity * 0.6f : 0.0f;
            return st;
        }

        // RENDER THREAD. Everything about the minimap slice that must happen inside the
        // frame: size the buffers (creation is the render thread's alone) and tell the
        // slicer the minimap is drawing and how big a window it needs. The CUT itself
        // runs on the loop thread - see slice_minimap_step().
        //
        // Returns true when a buffer is available to draw. The window carries margin,
        // so a cut that has not landed yet is invisible.
        bool plan_slice(const mm::Config& cfg, const mapdata::Chapter& ch, float half_px, std::uint64_t now)
        {
            if (!ch.has_heights())
            {
                g_slice_want_px.store(0, std::memory_order_relaxed);
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;

            const int want = slice_size_for(cfg, hm, half_px);
            if (want != g_slice_size)
            {
                // The buffers are the render thread's to allocate, and the slicer may be
                // writing into the old ones right now. If it will not stand down we
                // simply keep the current size for this frame.
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    wait_for_gpu(); // the old buffers may still be in flight
                    const bool ok = create_slice_buffers(want);
                    slicer_pause_end();
                    if (!ok)
                    {
                        g_slice_want_px.store(0, std::memory_order_relaxed);
                        return false;
                    }
                }
            }
            g_slice_want_px.store(g_slice_size, std::memory_order_relaxed);
            g_slice_want_ms.store(now, std::memory_order_relaxed);
            return slice_view().shown >= 0;
        }

        // LOOP THREAD. The actual cut: pace it, take the next buffer if the GPU is done
        // with it, fill the mapped upload heap and publish the result.
        //
        // The height planes are re-read from `mapdata` on EVERY call and never cached
        // across one: a chapter switch retires the old planes and frees them after a
        // grace period, so a pointer held from the previous slice could be freed memory.
        void slice_minimap_step(std::uint64_t now)
        {
            const int want = g_slice_want_px.load(std::memory_order_relaxed);
            if (want <= 0 || want != g_slice_size)
            {
                return; // the minimap is not drawing, or the render thread is resizing
            }
            // The minimap stopped drawing (the overlay hid, the map opened) and nobody
            // has asked since: stop cutting rather than burning a millisecond forever.
            if (now - g_slice_want_ms.load(std::memory_order_relaxed) > 500)
            {
                return;
            }

            mm::Snapshot snap{};
            if (!mm::read_snapshot(snap) || !snap.has_pawn)
            {
                return;
            }
            const mapdata::Chapter* ch = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (ch == nullptr || !ch->has_heights())
            {
                return;
            }
            const mapdata::HeightMaps& hm = *ch->heights;

            const mm::Config& cfg = mm::cfg_cached();

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
                g_slice_last_ms = 0; // a teleport must re-slice immediately
            }
            else
            {
                const float tau = cfg.feet_z_smooth_ms > 1 ? static_cast<float>(cfg.feet_z_smooth_ms) : 1.0f;
                // The loop runs at roughly frame rate; the exact dt does not matter for
                // a 100 ms EMA.
                const float a = 16.0f / tau;
                g_feet_z += (raw_feet - g_feet_z) * (a > 1.0f ? 1.0f : a);
            }

            const int period = cfg.slice_hz > 0 ? 1000 / cfg.slice_hz : 80;
            if (g_slice_last_ms != 0 && now - g_slice_last_ms < static_cast<std::uint64_t>(period))
            {
                return; // the previous window is still good enough
            }

            SliceBuf& b = g_slice[g_slice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0)
            {
                return;
            }
            const std::uint64_t in_flight = g_slice_in_flight[g_slice_next].load(std::memory_order_acquire);
            ID3D12Fence* fence = g_fence;
            if (in_flight != 0 && fence != nullptr && fence->GetCompletedValue() < in_flight)
            {
                // The GPU is still sampling this one. Never stall for the map: keep
                // showing the other buffer and try again on the next loop iteration.
                ++g_slice_skipped;
                return;
            }

            double pxc = 0.0;
            double pyc = 0.0;
            hm.to_px(snap.x, snap.y, pxc, pyc);
            const int x0 = static_cast<int>(std::lround(pxc)) - b.w / 2;
            const int y0 = static_cast<int>(std::lround(pyc)) - b.w / 2;

            const SliceStyle st = style_from(cfg);

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            slice_window(hm, x0, y0, b.w, b.mapped + b.footprint.Offset, b.footprint.Footprint.RowPitch,
                         g_feet_z, st);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            double g_slice_last_cut_ms = 0.0;
            if (freq > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq);
                g_slice_last_cut_ms = ms;
                g_slice_ms = g_slice_ms == 0.0 ? ms : g_slice_ms * 0.8 + ms * 0.2;
                if (ms > g_slice_ms_peak)
                {
                    g_slice_ms_peak = ms;
                }
            }

            if (g_pf_slice < 0)
            {
                g_pf_slice = mm::perf_register("minimap slice cut", perf::Thread::Loop);
            }
            mm::perf_record_ms(g_pf_slice, g_slice_last_cut_ms);

            g_slice_copy_pending[g_slice_next].store(true, std::memory_order_release);
            {
                SpinGuard guard(g_slice_view_lock);
                g_slice_view.shown = g_slice_next;
                // The window's own world -> pixel mapping (see mapdata::HeightMaps::to_px):
                //   px_local = px - x0 = (Y - (min_y + x0/s)) * s
                //   py_local = py - y0 = ((max_x - y0/s) - X) * s
                g_slice_view.px_per_uu = hm.px_per_uu;
                g_slice_view.min_y = hm.min_y + static_cast<double>(x0) / hm.px_per_uu;
                g_slice_view.max_x = hm.max_x - static_cast<double>(y0) / hm.px_per_uu;
            }
            g_slice_next = (g_slice_next + 1) % kSliceBufs;
            g_slice_last_ms = now;
            {
                // The first slice is the stage where a map asset, the CPU slicer and a
                // D3D12 upload heap are all live at once - i.e. the first moment a bad
                // manifest or a bad buffer would take the process down.
                static bool first = true;
                if (first)
                {
                    first = false;
                    crumb::stage(crumb::kFirstSlice);
                }
            }
        }

        // MAIN-MENU SELF-TEST. The slicer only ever runs inside draw_minimap, which is
        // gated on a gameplay pawn - so at the main menu neither the D3D12 resources nor
        // the CPU loop is exercised, and a verification run there could only say "it did
        // not crash". This allocates the buffers and slices one window at the chapter's
        // centre so a Lobby log line proves the whole path: texture + mapped upload heap
        // created, the loop ran, and what it cost. It also removes the first-frame hitch
        // in-world, since the buffers already exist.
        // RENDER THREAD, during set-up. It creates the buffers and writes into one of
        // them, so the loop-thread slicer must be held off for its duration.
        void slice_selftest()
        {
            if (!slicer_pause_begin(kSlicerPauseMs))
            {
                return; // the next frame will try again
            }
            struct Resume
            {
                ~Resume()
                {
                    slicer_pause_end();
                }
            } resume;
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
            const std::int64_t freq = qpc_freq();
            const double ms = freq > 0 ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                             static_cast<double>(freq)
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
        void record_slice_copies(ID3D12GraphicsCommandList* list, SliceBuf* bufs,
                                std::atomic<bool>* pending, std::atomic<std::uint64_t>* in_flight,
                                int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                // exchange, not load+store: the slicer may fill this buffer again the
                // instant we clear the flag, and that next fill must not be lost.
                if (b.tex == nullptr || !pending[i].exchange(false))
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
                // The upload heap this copy READS may not be rewritten until the GPU has
                // run it, so claim the fence this frame is about to signal. Without this
                // the loop thread could refill the heap between the recording and the
                // execution of the copy (two buffers, and a stalled frame is all it
                // takes) and the texture would show a window the mapping does not
                // describe. The stamp for a buffer that is merely being SAMPLED happens
                // at the end of the frame, alongside the fence signal.
                in_flight[i].store(g_fence_value + 1, std::memory_order_release);
            }
        }

        void record_slice_copy(ID3D12GraphicsCommandList* list)
        {
            record_slice_copies(list, g_slice, g_slice_copy_pending, g_slice_in_flight, kSliceBufs);
            record_slice_copies(list, g_mslice, g_mslice_copy_pending, g_mslice_in_flight, kMapSliceBufs);
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
            std::string s = std::format("{} panel   {} full map ({} recentres)   {} minimap zoom   "
                                        "{} reload",
                                        key_name_ascii(cfg.panel_key),
                                        key_name_ascii(cfg.map_key),
                                        key_name_ascii(cfg.map_recenter_key),
                                        key_name_ascii(cfg.zoom_key),
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

        // THE LOOK, cached once per frame (build_ui) so nothing on a draw path has to
        // take a config copy to know what colour to be. Render thread only.
        //
        // `g_palette` is the marker hue set and `g_plate` the theme's dark label plate;
        // the theme's other colours are resolved into the ordinary colour config keys at
        // load time (mmstate.cpp's apply_theme_defaults), which is what lets an explicit
        // key in the file override a theme.
        // MINIMAP ZOOM STEPS the loop thread still owes the config. The render thread
        // may not write the config file and the loop thread cannot see the cursor, so
        // the wheel-over-the-disc gesture and the zoom_key press both land here and are
        // applied in on_update(). Accumulated, so a fast flick of the wheel is not lost.
        std::atomic<int> g_zoom_steps{0};

        gly::Palette g_palette = gly::Palette::Default;
        mdb::Rgb g_plate = gly::theme_colors(gly::Theme::Neutral).plate;

        // The hue table lives in the PURE header src/glyphs.hpp, next to the shape
        // table, because "no two categories share a shape and a colour" is a property
        // of the two together and is asserted offline.
        ImU32 marker_color(mdb::Cat cat, int alpha)
        {
            const mdb::Rgb c = gly::marker_rgb(cat, g_palette);
            return IM_COL32(c.r, c.g, c.b, alpha);
        }

        // The dark box behind a label, the compass strip and the waypoint's distance -
        // one colour, from the theme, instead of the same literal in five places.
        ImU32 plate_color(int alpha)
        {
            return IM_COL32(g_plate.r, g_plate.g, g_plate.b, alpha);
        }

        // Category colour, or the ITEM QUALITY colour when the caller has quality
        // colouring switched on and this marker actually has a tier.
        //
        // Tier 0 deliberately falls through to the category colour: it is what every
        // chest, every live-only actor and every ordinary consumable is, so a palette
        // that repainted it would recolour most of the screen to say nothing. See
        // mdb::Rarity for where the tiers and the default palette come from.
        ImU32 marker_color_q(mdb::Cat cat, std::uint8_t rarity, int alpha, bool use_rarity,
                             const mdb::Rgb* palette)
        {
            const int tier = mdb::rarity_clamp(static_cast<int>(rarity));
            if (!use_rarity || tier == 0 || palette == nullptr)
            {
                return marker_color(cat, alpha);
            }
            const mdb::Rgb& c = palette[tier];
            return IM_COL32(c.r, c.g, c.b, alpha);
        }

        // ONE glyph. `hollow` is what a FOUND marker is drawn as: dimming alone takes
        // the colour away first and leaves an unreadable grey blob, while an outline
        // keeps the shape - which is the half of the identity that survives at 6 px.
        //
        // Every glyph gets a dark halo first (the review's `AddCircleFilled(p, r + 1,
        // 0x000000_78)`), so a light glyph still has an edge against the parchment fill
        // and a dark one against a lit scene. Its alpha follows the glyph's, so a
        // faded-out marker does not leave a black dot behind.
        void draw_marker_glyph(ImDrawList* dl, mdb::Cat cat, ImVec2 p, float r, ImU32 col, ImU32 edge,
                               bool hollow = false)
        {
            const int ca = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFFu);
            dl->AddCircleFilled(p, r + 1.0f, IM_COL32(0, 0, 0, (ca * 120) / 255), 12);

            const float w = hollow ? 1.7f : 1.2f;
            // A filled shape when the marker is live, the same shape as an outline when
            // it is found. Both take the SAME geometry, so the two states are the same
            // glyph and nothing moves when one becomes the other.
            const auto ngon = [&](float rad, int n) {
                if (hollow)
                {
                    dl->AddNgon(p, rad, col, n, w);
                }
                else
                {
                    dl->AddNgonFilled(p, rad, col, n);
                    dl->AddNgon(p, rad, edge, n, w);
                }
            };
            const auto circle = [&](ImVec2 c, float rad, int n) {
                if (hollow)
                {
                    dl->AddCircle(c, rad, col, n, w);
                }
                else
                {
                    dl->AddCircleFilled(c, rad, col, n);
                    dl->AddCircle(c, rad, edge, n, w);
                }
            };
            const auto rect = [&](float hw, float hh) {
                const ImVec2 a{p.x - r * hw, p.y - r * hh};
                const ImVec2 b{p.x + r * hw, p.y + r * hh};
                if (hollow)
                {
                    dl->AddRect(a, b, col, 1.5f, 0, w);
                }
                else
                {
                    dl->AddRectFilled(a, b, col, 1.5f);
                    dl->AddRect(a, b, edge, 1.5f, 0, w);
                }
            };
            const auto tri = [&](float scale) {
                const ImVec2 a{p.x, p.y - r * scale};
                const ImVec2 b{p.x - r * scale * 0.92f, p.y + r * scale * 0.72f};
                const ImVec2 c{p.x + r * scale * 0.92f, p.y + r * scale * 0.72f};
                if (hollow)
                {
                    dl->AddTriangle(a, b, c, col, w);
                }
                else
                {
                    dl->AddTriangleFilled(a, b, c, col);
                    dl->AddTriangle(a, b, c, edge, w);
                }
            };
            // A pip is the dark centre that tells a shrine from a plain diamond and a
            // merchant's coin from a pickup's dot. On a hollow glyph it is drawn in the
            // marker's own colour, because there is no fill for it to contrast against.
            const auto pip = [&](float rad) {
                dl->AddCircleFilled(p, r * rad, hollow ? col : edge, 8);
            };

            switch (gly::shape_of(cat))
            {
            case gly::Shape::Diamond:
                // AddNgon starts at angle 0, so a 4-gon has its vertices on the axes.
                ngon(r * 1.15f, 4);
                pip(0.32f);
                break;
            case gly::Shape::ChestBox:
                rect(0.95f, 0.75f);
                dl->AddLine(ImVec2{p.x - r * 0.95f, p.y}, ImVec2{p.x + r * 0.95f, p.y},
                            hollow ? col : edge, w);
                break;
            case gly::Shape::Dot:
                circle(p, r * 0.72f, 10);
                break;
            case gly::Shape::Triangle:
                tri(1.5f);
                break;
            case gly::Shape::TriangleNotched:
                // The elite's triangle is smaller than the boss's AND wears a bar. The
                // scale difference alone was a three-pixel difference at markers_size
                // 6.5, which is what made the two read as one category.
                tri(1.15f);
                dl->AddLine(ImVec2{p.x - r * 0.62f, p.y + r * 0.30f},
                            ImVec2{p.x + r * 0.62f, p.y + r * 0.30f}, hollow ? col : edge, w + 0.3f);
                break;
            case gly::Shape::DotRing:
                // A small dot with a DETACHED ring: at glyph size the gap is what makes
                // it read as an enemy rather than as a pickup.
                dl->AddCircleFilled(p, r * 0.34f, col, 8);
                dl->AddCircle(p, r * 0.92f, col, 12, w);
                break;
            case gly::Shape::Pentagon:
                ngon(r * 1.05f, 5);
                break;
            case gly::Shape::Coin:
                circle(p, r * 0.82f, 12);
                pip(0.30f);
                break;
            case gly::Shape::DoorBox:
                rect(0.55f, 0.95f);
                break;
            case gly::Shape::Ladder:
                dl->AddLine(ImVec2{p.x - r * 0.5f, p.y - r}, ImVec2{p.x - r * 0.5f, p.y + r}, col, 1.6f);
                dl->AddLine(ImVec2{p.x + r * 0.5f, p.y - r}, ImVec2{p.x + r * 0.5f, p.y + r}, col, 1.6f);
                for (int i = -1; i <= 1; ++i)
                {
                    const float y = p.y + static_cast<float>(i) * r * 0.55f;
                    dl->AddLine(ImVec2{p.x - r * 0.5f, y}, ImVec2{p.x + r * 0.5f, y}, col, 1.2f);
                }
                break;
            case gly::Shape::Lift:
                rect(0.85f, 0.5f);
                if (hollow)
                {
                    dl->AddTriangle(ImVec2{p.x, p.y - r * 1.35f}, ImVec2{p.x - r * 0.5f, p.y - r * 0.6f},
                                    ImVec2{p.x + r * 0.5f, p.y - r * 0.6f}, col, w);
                }
                else
                {
                    dl->AddTriangleFilled(ImVec2{p.x, p.y - r * 1.35f},
                                          ImVec2{p.x - r * 0.5f, p.y - r * 0.6f},
                                          ImVec2{p.x + r * 0.5f, p.y - r * 0.6f}, col);
                }
                break;
            case gly::Shape::RingBar:
                dl->AddCircle(p, r, col, 14, 2.0f);
                dl->AddLine(ImVec2{p.x - r * 0.7f, p.y}, ImVec2{p.x + r * 0.7f, p.y}, col, 1.4f);
                break;
            case gly::Shape::Cross:
                // "X marks the spot" - and, unlike the unfilled 4-gon it replaces, it is
                // not a shrine with the fill switched off.
                dl->AddLine(ImVec2{p.x - r * 0.85f, p.y - r * 0.85f},
                            ImVec2{p.x + r * 0.85f, p.y + r * 0.85f}, col, 2.1f);
                dl->AddLine(ImVec2{p.x - r * 0.85f, p.y + r * 0.85f},
                            ImVec2{p.x + r * 0.85f, p.y - r * 0.85f}, col, 2.1f);
                break;
            case gly::Shape::SmallSquare:
            case gly::Shape::Count:
            default:
                rect(0.55f, 0.55f);
                break;
            }
        }

        struct MarkerDrawStats
        {
            int total = 0;
            int drawn = 0;
            int clamped = 0;
            int filtered = 0;
            // A fixed buffer, not a std::string: this used to be assigned on the render
            // thread every single frame, which is a heap allocation per frame for a
            // 54-character id. DrawMarker::id is a fixed char array too, so this is one
            // memcpy of at most 64 bytes.
            char nearest[64]{};
            float nearest_uu = 0.0f;
        };

        MarkerDrawStats g_marker_draw{};

        //==============================================================================
        // ONE marker pass per frame
        //==============================================================================
        //
        // The minimap, the compass pips and the x-ray highlight each used to walk the
        // whole published marker buffer (700-3 600 entries) with its own loop and its own
        // sqrt. They ask different questions of the same rows, so the walk happens ONCE,
        // here, and each of them filters the result with its own rule.
        //
        // Distances are kept SQUARED: every consumer only needs them to compare and to
        // sort, and the two that want metres take the square root of the handful they
        // actually draw.

        struct FrameCand
        {
            const markers::DrawMarker* m = nullptr;
            float d2_xy = 0.0f; // squared horizontal distance from the player, uu^2
            float d2_3d = 0.0f; // squared 3D distance from the player
            std::uint8_t cat = 0;
            std::uint8_t rarity = 0;
            bool found = false;
        };

        std::vector<FrameCand> g_frame_cands; // render thread only, reused every frame
        int g_frame_marker_total = 0;         // rows in the published buffer
        int g_frame_bad_cat = 0;              // rows whose category byte is out of range

        //==============================================================================
        // Animation, toasts, and the "it just became found" event
        //==============================================================================
        //
        // All of it is RENDER-THREAD state derived from what the frame already has. None
        // of it costs the game thread anything, and - the rule that matters - none of it
        // can keep something on screen after the state that allows it went away:
        //
        //   * the HUD fade's TARGET is the hud_gate result. A target of 0 is applied
        //     instantly (hiding is immediate; the gate also stops the draw outright),
        //     and only showing is eased. That is the no-latch rule from lessons.md
        //     applied to an animation instead of to a condition.
        //   * a toast is a string plus a deadline; it says what just happened and then
        //     goes.
        //   * the found ring is driven by comparing the PUBLISHED found flags of the
        //     markers near the player between marker rounds - the game thread does no
        //     extra work for it, and nothing is remembered longer than one round.

        constexpr std::uint64_t kHudFadeMs = 150;
        float g_hud_fade = 0.0f;
        std::uint64_t g_hud_fade_ms = 0; // when the current show started

        // Eases towards 1 while `target_on`, drops to 0 the instant it is false.
        float hud_fade_step(bool target_on, std::uint64_t now)
        {
            if (!target_on)
            {
                g_hud_fade = 0.0f;
                g_hud_fade_ms = 0;
                return 0.0f;
            }
            if (g_hud_fade_ms == 0)
            {
                g_hud_fade_ms = now;
            }
            const std::uint64_t age = now - g_hud_fade_ms;
            const float t = age >= kHudFadeMs ? 1.0f : static_cast<float>(age) / static_cast<float>(kHudFadeMs);
            // Smoothstep: a linear ramp on an alpha reads as a hard edge at both ends.
            g_hud_fade = t * t * (3.0f - 2.0f * t);
            return g_hud_fade;
        }

        // ---- toasts ------------------------------------------------------------------
        char g_toast[160]{};
        std::uint64_t g_toast_until = 0;

        void toast_for(const char* text, unsigned ms)
        {
            ::strncpy_s(g_toast, sizeof(g_toast), text, _TRUNCATE);
            g_toast_until = ::GetTickCount64() + ms;
        }

        void toast(const char* text)
        {
            toast_for(text, 1000);
        }

        //==============================================================================
        // Map -> clipboard
        //==============================================================================
        //
        // The picture we want is exactly what the player is looking at, so the source is
        // the BACK BUFFER, taken from the frame that has just been drawn - not a
        // re-render of the map into an offscreen target (which would need our own RTV,
        // our own pass and would then differ from the screen).
        //
        // THE SEQUENCE, and why it is spread over frames
        //   frame N   : after ImGui's draw call is recorded, the back buffer is
        //               transitioned RENDER_TARGET -> COPY_SOURCE, one
        //               CopyTextureRegion of the canvas rect is recorded into a readback
        //               buffer, and the fence value this frame will signal is remembered.
        //   frame N+k : once GetCompletedValue() has passed that fence the readback is
        //               mapped, unpacked to BGRA8 (clipimg - the back buffer is HDR10
        //               R10G10B10A2 on this game, not R8G8B8A8) and turned into a CF_DIB
        //               payload, which is handed to the LOOP thread.
        //   loop      : OpenClipboard / EmptyClipboard / SetClipboardData / CloseClipboard.
        //
        // Two rules from lessons.md are load-bearing: nothing is mapped before its fence
        // has passed (a readback read early is a garbage picture, not an error), and the
        // clipboard - which is a blocking, window-station-wide, message-pumping API - is
        // never touched from the render thread inside Present.
        //
        // No file is ever written.

        enum class ShotStage
        {
            Idle = 0,
            Recorded, // the copy is in flight on the GPU
            Waiting,  // the loop thread has the bytes
        };

        ShotStage g_shot_stage = ShotStage::Idle;
        std::atomic<bool> g_shot_request{false};   // loop -> render (the hotkey)
        ID3D12Resource* g_shot_readback = nullptr; // render thread only
        std::uint64_t g_shot_fence = 0;
        UINT g_shot_w = 0;
        UINT g_shot_h = 0;
        UINT g_shot_pitch = 0;
        clipimg::Fmt g_shot_fmt = clipimg::Fmt::Unknown;
        // The canvas rect of the last full-map frame, in back-buffer pixels. Written by
        // draw_full_map every frame it draws, read by the render thread in the same
        // frame - same thread, no synchronisation needed.
        mv::Rect g_shot_canvas{};
        bool g_shot_canvas_valid = false;

        // render -> loop: the finished DIB. A spinlock, not a queue: one screenshot can
        // be in flight and the payload is handed over exactly once.
        Spinlock g_shot_lock;
        std::vector<std::uint8_t> g_shot_dib;
        std::atomic<bool> g_shot_dib_ready{false};

        // loop -> render: what to say in the toast. The loop thread owns the clipboard
        // call, but toasts are drawn by the render thread, so the text comes back the
        // same way everything else does - a buffer plus a flag.
        // The loop thread posts toast text here; the render thread picks it up in
        // build_ui. Used by the clipboard result AND by the first-run tip, because a
        // toast may only be raised where toasts are drawn.
        char g_shot_toast[160]{};
        unsigned g_shot_toast_ms = 2500;
        std::atomic<bool> g_shot_toast_ready{false};
        // loop -> render: the handover is complete, so the next request may be recorded.
        // Without it a failed clipboard write would leave the state machine parked in
        // Waiting for ever and the key would silently stop working.
        std::atomic<bool> g_shot_stage_done{false};

        // ANY THREAD. Queues a toast for the render thread to draw.
        void post_toast(const char* text, unsigned ms)
        {
            {
                SpinGuard guard(g_shot_lock);
                ::strncpy_s(g_shot_toast, sizeof(g_shot_toast), text, _TRUNCATE);
                g_shot_toast_ms = ms;
            }
            g_shot_toast_ready.store(true, std::memory_order_release);
        }

        void shot_fail(const char* why)
        {
            post_toast(why, 2500);
        }

        void shot_reset()
        {
            safe_release(g_shot_readback);
            g_shot_stage = ShotStage::Idle;
            g_shot_fence = 0;
            g_shot_w = 0;
            g_shot_h = 0;
            g_shot_pitch = 0;
        }

        // RENDER THREAD. Records the copy into the command list the frame is already
        // building, between ImGui's draw call and the transition back to PRESENT. The
        // back buffer is in RENDER_TARGET state on entry and is left in PRESENT state,
        // i.e. this REPLACES the caller's closing barrier when it returns true.
        bool record_shot_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, UINT index)
        {
            if (g_shot_stage != ShotStage::Idle || !g_shot_request.exchange(false, std::memory_order_acquire))
            {
                return false;
            }
            ID3D12Device* dev = g_device;
            if (dev == nullptr || backbuffer == nullptr)
            {
                shot_fail("screenshot: no device");
                return false;
            }
            const DXGI_FORMAT fmt = g_format;
            g_shot_fmt = clipimg::fmt_from_dxgi(static_cast<unsigned>(fmt));
            if (g_shot_fmt == clipimg::Fmt::Unknown)
            {
                shot_fail("screenshot: back buffer format not supported");
                mm::logf(L"screenshot: DXGI format {} is not one this build can unpack",
                         static_cast<int>(fmt));
                return false;
            }
            // The region: the map canvas as it was laid out this frame, clamped to the
            // back buffer. Without a canvas (the map is not open) there is nothing to
            // copy, and the hotkey should not have fired.
            if (!g_shot_canvas_valid)
            {
                shot_fail("screenshot: the map is not open");
                return false;
            }
            long x0 = static_cast<long>(g_shot_canvas.x0);
            long y0 = static_cast<long>(g_shot_canvas.y0);
            long x1 = static_cast<long>(g_shot_canvas.x1);
            long y1 = static_cast<long>(g_shot_canvas.y1);
            x0 = x0 < 0 ? 0 : x0;
            y0 = y0 < 0 ? 0 : y0;
            x1 = x1 > static_cast<long>(g_width) ? static_cast<long>(g_width) : x1;
            y1 = y1 > static_cast<long>(g_height) ? static_cast<long>(g_height) : y1;
            if (x1 - x0 < 16 || y1 - y0 < 16)
            {
                shot_fail("screenshot: the map canvas is too small");
                return false;
            }
            g_shot_w = static_cast<UINT>(x1 - x0);
            g_shot_h = static_cast<UINT>(y1 - y0);
            // A readback footprint's row pitch must be 256-aligned.
            g_shot_pitch = (g_shot_w * 4u + 255u) & ~255u;

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = static_cast<UINT64>(g_shot_pitch) * g_shot_h;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&g_shot_readback))))
            {
                shot_fail("screenshot: readback buffer allocation failed");
                shot_reset();
                return false;
            }

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = backbuffer;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_shot_readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = fmt;
            dst.PlacedFootprint.Footprint.Width = g_shot_w;
            dst.PlacedFootprint.Footprint.Height = g_shot_h;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = g_shot_pitch;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            D3D12_BOX box{};
            box.left = static_cast<UINT>(x0);
            box.top = static_cast<UINT>(y0);
            box.front = 0;
            box.right = static_cast<UINT>(x1);
            box.bottom = static_cast<UINT>(y1);
            box.back = 1;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &b);
            g_shot_stage = ShotStage::Recorded;
            (void)index;
            return true;
        }

        // RENDER THREAD, top of the frame. Once the GPU is past the fence, map the
        // readback and build the DIB.
        void shot_collect()
        {
            if (g_shot_stage_done.exchange(false, std::memory_order_acquire))
            {
                shot_reset(); // the loop thread is done with the bytes; re-arm
            }
            if (g_shot_stage != ShotStage::Recorded || g_shot_readback == nullptr || g_fence == nullptr)
            {
                return;
            }
            if (g_shot_fence == 0 || g_fence->GetCompletedValue() < g_shot_fence)
            {
                return;
            }
            void* mapped = nullptr;
            D3D12_RANGE range{};
            range.Begin = 0;
            range.End = static_cast<SIZE_T>(g_shot_pitch) * g_shot_h;
            if (FAILED(g_shot_readback->Map(0, &range, &mapped)) || mapped == nullptr)
            {
                shot_fail("screenshot: readback map failed");
                shot_reset();
                return;
            }
            // Unpack row by row into a tight BGRA image, then let clipimg flip it into
            // the bottom-up DIB the clipboard wants.
            std::vector<std::uint8_t> bgra(static_cast<std::size_t>(g_shot_w) * 4u * g_shot_h);
            bool ok = true;
            for (UINT y = 0; y < g_shot_h && ok; ++y)
            {
                const auto* srow = static_cast<const std::uint8_t*>(mapped) +
                                   static_cast<std::size_t>(y) * g_shot_pitch;
                ok = clipimg::unpack_row(g_shot_fmt, srow,
                                         bgra.data() + static_cast<std::size_t>(y) * g_shot_w * 4u,
                                         static_cast<int>(g_shot_w));
            }
            const D3D12_RANGE none{0, 0};
            g_shot_readback->Unmap(0, &none);
            if (!ok)
            {
                shot_fail("screenshot: pixel unpack failed");
                shot_reset();
                return;
            }
            std::vector<std::uint8_t> dib;
            if (!clipimg::build_dib(static_cast<int>(g_shot_w), static_cast<int>(g_shot_h), bgra.data(),
                                    static_cast<std::size_t>(g_shot_w) * 4u, dib))
            {
                shot_fail("screenshot: bitmap build failed");
                shot_reset();
                return;
            }
            {
                SpinGuard guard(g_shot_lock);
                g_shot_dib = std::move(dib);
            }
            g_shot_dib_ready.store(true, std::memory_order_release);
            safe_release(g_shot_readback);
            g_shot_stage = ShotStage::Waiting;
        }

        //==============================================================================
        // The shrine list
        //==============================================================================
        //
        // Every shrine of the current chapter with its in-game name, its distance and
        // whether the save has lit it - and, behind `fast_travel_enabled`, a Travel
        // action per row.
        //
        // Sources, all published snapshots (render thread, no game-thread work):
        //   shr::table()  markers/shrines.json - the id, the localised name, the chapter,
        //                 the actor position and the game's own BirthPosition;
        //   shr::state()  the save's UnlockedFirepoints list, read raw at 1 Hz;
        //   snap          the pawn position, for the distance.
        //
        // Sorted by distance, because "which shrine is near me" is the question a player
        // asks; the chapter filter follows the marker filter so the list and the map
        // agree about what exists.

        bool g_shrine_panel = false;
        char g_shrine_selected[shdb::kMaxIdLen]{};

        void draw_shrine_list(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state,
                              int filter_chapter)
        {
            const std::vector<shdb::Shrine>* table = shr::table();
            if (table == nullptr || table->empty())
            {
                const shr::TableInfo info = shr::table_info();
                ImGui::TextDisabled("no shrine table: %s",
                                    info.error[0] != '\0' ? info.error : "markers\\shrines.json is empty");
                return;
            }
            const shr::State st = shr::state();

            struct Row
            {
                const shdb::Shrine* s;
                double dist;
                bool unlocked;
            };
            std::vector<Row> rows;
            rows.reserve(table->size());
            for (const shdb::Shrine& sh : *table)
            {
                if (!sh.shrine || !sh.has_pos)
                {
                    continue; // a bossdoor_/Task pseudo-row is not a place
                }
                if (filter_chapter != chid::kNone && sh.chapter >= 0 && sh.chapter != filter_chapter)
                {
                    continue;
                }
                double d = -1.0;
                if (have_state)
                {
                    const double dx = sh.x - snap.x;
                    const double dy = sh.y - snap.y;
                    const double dz = sh.z - snap.z;
                    d = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                rows.push_back(Row{&sh, d, st.valid && shr::is_unlocked(sh.id.c_str())});
            }
            if (rows.empty())
            {
                ImGui::TextDisabled("no shrines in this chapter");
                return;
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if ((a.dist < 0.0) != (b.dist < 0.0))
                {
                    return b.dist < 0.0;
                }
                if (a.dist != b.dist)
                {
                    return a.dist < b.dist;
                }
                return a.s->id < b.s->id;
            });

            if (!st.valid)
            {
                ImGui::TextDisabled("unlocked state: n/a (%s)",
                                    st.route[0] != '\0' ? st.route : "not read yet");
            }
            const shr::TravelState tv = shr::travel_state();
            if (tv.phase == shr::Travel::Refused)
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "travel refused: %s", tv.note);
                ImGui::SameLine();
                if (ImGui::SmallButton("dismiss"))
                {
                    shr::clear_travel();
                }
            }

            if (!ImGui::BeginTable("shrines", cfg.fast_travel_enabled ? 5 : 4,
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f)))
            {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Shrine");
            ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Lit", ImGuiTableColumnFlags_WidthFixed);
            if (cfg.fast_travel_enabled)
            {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            }
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const Row& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const bool selected = ::strcmp(g_shrine_selected, r.s->id.c_str()) == 0;
                // One click sets a waypoint on it, a double-click centres the map there -
                // review item 1. Selectable, so the whole row is the hit target rather
                // than the eight characters of a name.
                if (ImGui::Selectable(r.s->label().c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick))
                {
                    ::strncpy_s(g_shrine_selected, sizeof(g_shrine_selected), r.s->id.c_str(),
                                _TRUNCATE);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        g_mv.cx = r.s->x;
                        g_mv.cy = r.s->y;
                        // The height slice is cut around the view centre, so a jump has
                        // to invalidate it or the map draws the old storey at the new
                        // place until the next scheduled cut.
                        g_map_recut.store(true, std::memory_order_release);
                        toast("centred on the shrine");
                    }
                    else
                    {
                        mv::Waypoint wp{};
                        wp.set = true;
                        wp.x = r.s->x;
                        wp.y = r.s->y;
                        wp.z = r.s->z;
                        mm::set_waypoint(wp);
                        mm::g_waypoint_dirty.store(true, std::memory_order_release);
                        toast("waypoint set on the shrine");
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\nid %s\nclick: waypoint    double-click: centre the map",
                                      r.s->label().c_str(), r.s->id.c_str());
                }
                ImGui::TableNextColumn();
                if (r.s->chapter < 0)
                {
                    ImGui::TextDisabled("-");
                }
                else if (r.s->chapter == 0)
                {
                    ImGui::TextUnformatted("DLC");
                }
                else
                {
                    ImGui::Text("%d", r.s->chapter);
                }
                ImGui::TableNextColumn();
                if (r.dist < 0.0)
                {
                    ImGui::TextDisabled("-");
                }
                else if (r.dist >= 100000.0)
                {
                    ImGui::Text("%.1f km", r.dist / 100000.0);
                }
                else
                {
                    ImGui::Text("%.0f m", r.dist / 100.0);
                }
                ImGui::TableNextColumn();
                if (!st.valid)
                {
                    ImGui::TextDisabled("n/a");
                }
                else if (r.unlocked)
                {
                    ImGui::TextColored(ImVec4{0.55f, 0.85f, 0.55f, 1.0f}, "yes");
                }
                else
                {
                    ImGui::TextDisabled("no");
                }
                if (cfg.fast_travel_enabled)
                {
                    ImGui::TableNextColumn();
                    // Only an id the save says is unlocked: travelling to a locked one is
                    // untested and is exactly the call that can wedge level streaming.
                    const bool can = r.unlocked && tv.phase != shr::Travel::Requested &&
                                     tv.phase != shr::Travel::InFlight;
                    ImGui::BeginDisabled(!can);
                    if (ImGui::SmallButton("Travel"))
                    {
                        shr::request_travel(r.s->id.c_str());
                        toast("fast travel requested");
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!cfg.fast_travel_enabled)
            {
                ImGui::TextDisabled("Fast travel is off (fast_travel_enabled, Advanced tab).");
            }
        }

        //==============================================================================
        // The collection statistics page
        //==============================================================================
        //
        // "Why am I using this mod" on one screen: found/total for every chapter and
        // every category, the shrines the save has lit, and one overall percentage.
        //
        // RENDER THREAD ONLY, and it never asks the game anything. Both sources are
        // published snapshots - `markers::stats()` (a spinlock and a ~1.5 KB copy) and
        // `shr::state()` (~6.5 KB) - so asking per frame would be ~8 KB of copying and a
        // lock round-trip per frame for numbers that change once per SWEEP ROUND. The
        // cache below therefore refreshes when `markers::rounds()` moves, with a 1 s
        // floor so the page still fills in when the live sweep is off entirely (rounds
        // never advance then, and a page that stays empty for ever reads as a bug).

        struct StatsCache
        {
            markers::Stats st{};
            shr::State shrines{};
            std::uint64_t round = ~0ull;
            std::uint64_t at_ms = 0;
            bool primed = false;
        };

        StatsCache g_stats_cache;
        bool g_stats_page = false; // the full map's Stats panel

        const StatsCache& stats_cached(std::uint64_t now)
        {
            const std::uint64_t round = markers::rounds();
            if (!g_stats_cache.primed || round != g_stats_cache.round ||
                now - g_stats_cache.at_ms >= 1000)
            {
                g_stats_cache.st = markers::stats();
                g_stats_cache.shrines = shr::state();
                g_stats_cache.round = round;
                g_stats_cache.at_ms = now;
                g_stats_cache.primed = true;
            }
            return g_stats_cache;
        }

        // THE CATEGORIES THE COLLECTION PAGE COUNTS, and the order they are shown in.
        // The map knows fourteen; only these six are things a player collects or ticks
        // off, and the other eight were what made the matrix wider than a 1080p panel.
        // Fixed, not derived from the DB, so a chapter with none of a category still
        // gets its column and the layout does not move between chapters.
        constexpr mdb::Cat kStatsCats[] = {
            mdb::Cat::Shrine, mdb::Cat::Chest, mdb::Cat::Pickup,
            mdb::Cat::Boss,   mdb::Cat::Npc,   mdb::Cat::Merchant,
        };
        constexpr int kStatsCatCount = static_cast<int>(std::size(kStatsCats));

        // Draws into whatever window is current. `compact` drops the per-chapter matrix
        // and keeps the summary, for the F2 panel where vertical space is scarce.
        void draw_collection_stats(std::uint64_t now, bool compact)
        {
            const StatsCache& c = stats_cached(now);
            const markers::Stats& st = c.st;

            if (!st.db_loaded || st.static_markers == 0)
            {
                ImGui::TextDisabled("no markers\\<chapter>.json loaded - live markers only");
                return;
            }

            // ---- the headline ---------------------------------------------------------
            int found_all = 0;
            int total_all = 0;
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                found_all += st.cat[i].found;
                total_all += st.cat[i].total;
            }
            const float pct = total_all > 0 ? 100.0f * static_cast<float>(found_all) /
                                                  static_cast<float>(total_all)
                                            : 0.0f;
            ImGui::Text("%d / %d collected", found_all, total_all);
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1f%% of every chapter)", static_cast<double>(pct));
            ImGui::ProgressBar(total_all > 0 ? static_cast<float>(found_all) /
                                                   static_cast<float>(total_all)
                                             : 0.0f,
                               ImVec2(-1.0f, ImGui::GetTextLineHeight()));

            // ---- shrines lit ----------------------------------------------------------
            //
            // `UnlockedFirepoints` also holds boss-door and task pseudo-points, so the
            // raw count is not "shrines". Only the ids that JOIN to a shrine marker in
            // the static DB are counted; the raw list length is shown beside it so a
            // join that goes wrong is visible rather than silent.
            const int shrine_total = st.cat[static_cast<int>(mdb::Cat::Shrine)].total;
            if (!c.shrines.valid)
            {
                ImGui::TextDisabled("Shrines lit: n/a  (%s)",
                                    c.shrines.route[0] != '\0' ? c.shrines.route : "not read yet");
            }
            else
            {
                int lit = 0;
                const markers::View view = markers::view();
                for (int i = 0; i < c.shrines.id_count; ++i)
                {
                    for (std::size_t m = 0; m < view.count; ++m)
                    {
                        if (view.data[m].cat != static_cast<std::uint8_t>(mdb::Cat::Shrine))
                        {
                            continue;
                        }
                        if (::_stricmp(view.data[m].id, c.shrines.ids[i]) == 0)
                        {
                            ++lit;
                            break;
                        }
                    }
                }
                ImGui::Text("Shrines lit: %d", lit);
                ImGui::SameLine();
                ImGui::TextDisabled("(%d unlocked ids incl. boss doors / tasks; %d shrines in the DB%s)",
                                    c.shrines.unlocked, shrine_total,
                                    c.shrines.truncated ? "; list TRUNCATED" : "");
                if (c.shrines.current[0] != '\0')
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("| last rested at %s", c.shrines.current);
                }
            }

            // ---- per category ---------------------------------------------------------
            ImGui::Spacing();
            if (ImGui::BeginTable("stats_cat", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Category");
                ImGui::TableSetupColumn("Found");
                ImGui::TableSetupColumn("Total");
                ImGui::TableSetupColumn("%");
                ImGui::TableHeadersRow();
                for (const mdb::Cat cat : kStatsCats)
                {
                    const int i = static_cast<int>(cat);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", mdb::cat_label(cat));
                    ImGui::TableNextColumn();
                    if (st.cat[i].total == 0)
                    {
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        continue;
                    }
                    ImGui::Text("%d", st.cat[i].found);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", st.cat[i].total);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%", 100.0 * static_cast<double>(st.cat[i].found) /
                                              static_cast<double>(st.cat[i].total));
                }
                ImGui::EndTable();
            }

            if (compact)
            {
                return;
            }

            // ---- per chapter x category ----------------------------------------------
            //
            // The six collectable categories, always all six, and nothing else: with the
            // full enum this was 15 columns and had to scroll sideways inside a 1080p
            // panel, which made the numbers on the right unreachable in practice.
            ImGui::Spacing();
            ImGui::TextDisabled("per chapter");
            if (ImGui::BeginTable("stats_matrix", kStatsCatCount + 1,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 11.0f)))
            {
                ImGui::TableSetupColumn("Chapter");
                for (const mdb::Cat cat : kStatsCats)
                {
                    ImGui::TableSetupColumn(mdb::cat_label(cat));
                }
                ImGui::TableHeadersRow();

                const auto cell = [](const markers::CatStat& cs) {
                    ImGui::TableNextColumn();
                    if (cs.total == 0)
                    {
                        ImGui::TextDisabled("-");
                    }
                    else if (cs.found >= cs.total)
                    {
                        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%d/%d", cs.found, cs.total);
                    }
                    else
                    {
                        ImGui::Text("%d/%d", cs.found, cs.total);
                    }
                };

                // Row 0 is the bucket for a manifest whose "chapter" is not a number -
                // the DLC one spells it "DLC".
                for (int ch = 0; ch <= 8; ++ch)
                {
                    markers::CatStat row{};
                    for (int i = 0; i < mdb::kCatCount; ++i)
                    {
                        row.total += st.chapter[ch][i].total;
                        row.found += st.chapter[ch][i].found;
                    }
                    if (row.total == 0)
                    {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // The chapter the markers are currently filtered to (the one the
                    // player is standing in) is starred and highlighted.
                    char label[16]{};
                    const bool here = (ch == st.filter_chapter);
                    if (ch == 0)
                    {
                        ::strncpy_s(label, sizeof(label), here ? "DLC*" : "DLC", _TRUNCATE);
                    }
                    else
                    {
                        ::_snprintf_s(label, sizeof(label), _TRUNCATE, here ? "%d*" : "%d", ch);
                    }
                    if (here)
                    {
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", label);
                    }
                    else
                    {
                        ImGui::TextUnformatted(label);
                    }
                    for (const mdb::Cat cat : kStatsCats)
                    {
                        cell(st.chapter[ch][static_cast<int>(cat)]);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("* the chapter the markers are filtered to right now");
        }

        void draw_toast()
        {
            const std::uint64_t now = ::GetTickCount64();
            if (g_toast[0] == '\0' || now >= g_toast_until)
            {
                return;
            }
            const std::uint64_t left = g_toast_until - now;
            const float a = left >= 300 ? 1.0f : static_cast<float>(left) / 300.0f;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(g_toast);
            const float pad = ImGui::GetTextLineHeight() * 0.5f;
            const ImVec2 c{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.72f};
            const ImVec2 tl{c.x - ts.x * 0.5f - pad, c.y - ts.y * 0.5f - pad * 0.5f};
            const ImVec2 br{c.x + ts.x * 0.5f + pad, c.y + ts.y * 0.5f + pad * 0.5f};
            dl->AddRectFilled(tl, br, plate_color(static_cast<int>(220.0f * a)), 4.0f);
            dl->AddRect(tl, br, IM_COL32(150, 158, 168, static_cast<int>(180.0f * a)), 4.0f, 0, 1.2f);
            dl->AddText(ImVec2{c.x - ts.x * 0.5f, c.y - ts.y * 0.5f},
                        IM_COL32(240, 242, 246, static_cast<int>(255.0f * a)), g_toast);
        }

        // ---- "this marker just became found" -----------------------------------------
        //
        // A 400 ms ring where a marker was collected. The event has to come from
        // somewhere cheap: the render thread already walks the published buffer every
        // frame, so it keeps the found flags of the markers NEAR the player and diffs
        // them whenever the marker sweep publishes a new round (~1 Hz). No game-thread
        // work, no per-frame string compares, and nothing survives a round.
        constexpr int kFoundWatch = 24;      // markers watched, nearest-ish first
        constexpr int kFoundEvents = 6;      // rings that can be in flight at once
        constexpr double kFoundWatchUu = 6000.0; // 60 m: as far as a ring is worth drawing
        constexpr std::uint64_t kFoundRingMs = 400;

        struct FoundWatch
        {
            char id[54]{};
            bool found = false;
        };

        struct FoundEvent
        {
            double x = 0.0;
            double y = 0.0;
            std::uint64_t t0 = 0;
        };

        FoundWatch g_found_watch[kFoundWatch]{};
        int g_found_watch_n = 0;
        std::uint64_t g_found_watch_round = 0;
        FoundEvent g_found_events[kFoundEvents]{};
        int g_found_event_head = 0;

        void note_found_event(double x, double y, std::uint64_t now)
        {
            g_found_events[g_found_event_head] = FoundEvent{x, y, now};
            g_found_event_head = (g_found_event_head + 1) % kFoundEvents;
        }

        // Called from build_frame_candidates, once per PUBLISHED ROUND rather than per
        // frame - the flags cannot change in between.
        void update_found_watch(std::uint64_t round, std::uint64_t now)
        {
            if (round == g_found_watch_round)
            {
                return;
            }
            const bool first = g_found_watch_round == 0;
            g_found_watch_round = round;

            FoundWatch next[kFoundWatch]{};
            int n = 0;
            const float radius2 = static_cast<float>(kFoundWatchUu * kFoundWatchUu);
            for (const FrameCand& fc : g_frame_cands)
            {
                if (n >= kFoundWatch)
                {
                    break;
                }
                if (fc.d2_xy > radius2 || fc.m->id[0] == '\0')
                {
                    continue;
                }
                ::strncpy_s(next[n].id, sizeof(next[n].id), fc.m->id, _TRUNCATE);
                next[n].found = fc.found;
                // A marker that was in the previous round's watch as NOT found and is
                // found now is the event. A marker that was not being watched cannot
                // produce one - which is what stops the first round after a load firing
                // a ring for every item the save says is already collected.
                if (!first && next[n].found)
                {
                    for (int j = 0; j < g_found_watch_n; ++j)
                    {
                        if (!g_found_watch[j].found && std::strcmp(g_found_watch[j].id, next[n].id) == 0)
                        {
                            note_found_event(fc.m->x, fc.m->y, now);
                            break;
                        }
                    }
                }
                ++n;
            }
            for (int i = 0; i < n; ++i)
            {
                g_found_watch[i] = next[i];
            }
            g_found_watch_n = n;
        }

        // Draws whatever rings are still in flight, through a caller-supplied
        // world -> screen mapping. `scale` sizes the ring to the view (a minimap glyph
        // is much smaller than a full-map one).
        template <typename ToScreen>
        void draw_found_rings(ImDrawList* dl, std::uint64_t now, float scale, ToScreen to_screen)
        {
            for (const FoundEvent& e : g_found_events)
            {
                if (e.t0 == 0 || now - e.t0 > kFoundRingMs)
                {
                    continue;
                }
                const float t = static_cast<float>(now - e.t0) / static_cast<float>(kFoundRingMs);
                float sx = 0.0f;
                float sy = 0.0f;
                if (!to_screen(e.x, e.y, sx, sy))
                {
                    continue;
                }
                const float rad = scale * (0.6f + 2.2f * t);
                const int a = static_cast<int>(220.0f * (1.0f - t));
                dl->AddCircle(ImVec2{sx, sy}, rad, IM_COL32(255, 236, 180, a), 20, 2.0f);
            }
        }

        void build_frame_candidates(const mm::Snapshot& snap)
        {
            if (g_pf_markpass < 0)
            {
                g_pf_markpass = mm::perf_register("marker pass", perf::Thread::Render);
            }
            const mm::PerfScope scope(g_pf_markpass);
            g_frame_cands.clear();
            g_frame_marker_total = 0;
            g_frame_bad_cat = 0;

            const markers::View v = markers::view();
            g_frame_marker_total = static_cast<int>(v.count);
            if (v.data == nullptr || v.count == 0)
            {
                return;
            }
            g_frame_cands.reserve(v.count);
            for (std::size_t i = 0; i < v.count; ++i)
            {
                const markers::DrawMarker& m = v.data[i];
                if (static_cast<int>(m.cat) >= mdb::kCatCount)
                {
                    ++g_frame_bad_cat;
                    continue;
                }
                const double dx = m.x - snap.x;
                const double dy = m.y - snap.y;
                const double dz = m.z - snap.z;
                FrameCand c{};
                c.m = &m;
                c.d2_xy = static_cast<float>(dx * dx + dy * dy);
                c.d2_3d = static_cast<float>(dx * dx + dy * dy + dz * dz);
                c.cat = m.cat;
                c.rarity = m.rarity;
                c.found = (m.flags & markers::kFlagFound) != 0;
                g_frame_cands.push_back(c);
            }

            // The found-ring events. Diffed once per published marker round, not per
            // frame - the flags cannot change in between.
            update_found_watch(markers::rounds(), ::GetTickCount64());
        }

        void draw_markers(const mm::Config& cfg, const MiniGeom& g, bool round, float x0, float y0, float side,
                          ImDrawList* dl)
        {
            g_marker_draw = MarkerDrawStats{};
            if (!cfg.markers_enabled)
            {
                return;
            }
            g_marker_draw.total = g_frame_marker_total;
            g_marker_draw.filtered = g_frame_bad_cat;
            if (g_frame_cands.empty())
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
                std::uint8_t rarity = 0;
                bool found = false;
                bool clamped = false;
                const char* id = nullptr;
            };
            // Render thread only, and reused frame to frame so a full minimap never
            // allocates during Present.
            static std::vector<Cand> cands;
            cands.clear();

            for (const FrameCand& fc : g_frame_cands)
            {
                const markers::DrawMarker& m = *fc.m;
                const mdb::Cat cat = static_cast<mdb::Cat>(fc.cat);
                if (!mdb::cat_enabled(cfg.markers_categories, cat))
                {
                    ++g_marker_draw.filtered;
                    continue;
                }
                const bool found = fc.found;
                if (found && cfg.markers_hide_found)
                {
                    ++g_marker_draw.filtered;
                    continue;
                }

                // The minimap is centred on the position the RENDER side works from,
                // so the on-screen offset is still computed here; the distance used for
                // the cap and the sort comes from the shared pass.
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
                cand.d2 = fc.d2_xy;
                cand.cat = fc.cat;
                cand.rarity = fc.rarity;
                cand.found = found;
                cand.clamped = clamped;
                cand.id = m.id;
                cands.push_back(cand);
            }

            // ONE sort, nearest first: the cap drops the far ones and the draw loop then
            // walks the array BACKWARDS, so the near ones are painted last (on top).
            // partial_sort already leaves [0, cap) sorted ascending, so after the resize
            // the whole array is sorted - the second full sort this used to do was pure
            // waste on up to 400 candidates every single frame.
            const std::size_t cap = cfg.markers_max_draw > 0
                                        ? static_cast<std::size_t>(cfg.markers_max_draw)
                                        : cands.size();
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            else
            {
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            }

            const float op = cfg.opacity;
            for (std::size_t ci = cands.size(); ci-- > 0;)
            {
                const Cand& cand = cands[ci];
                const float a = op * (cand.found ? cfg.markers_found_alpha : 1.0f);
                if (a <= 0.01f)
                {
                    continue;
                }
                const int alpha = static_cast<int>((std::min)(1.0f, a) * 255.0f + 0.5f);
                const ImU32 col = marker_color_q(static_cast<mdb::Cat>(cand.cat), cand.rarity, alpha,
                                                 cfg.markers_rarity_tint, cfg.xray_rarity_colors);
                const ImU32 edge = IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f));
                const ImVec2 p{g.center.x + cand.dx, g.center.y + cand.dy};
                draw_marker_glyph(dl, static_cast<mdb::Cat>(cand.cat), p, cand.clamped ? r * 0.72f : r, col, edge,
                                  cand.found);
                ++g_marker_draw.drawn;
                g_marker_draw.clamped += cand.clamped ? 1 : 0;
            }
            if (!cands.empty())
            {
                // cands is sorted near -> far, so the FIRST one is the nearest.
                const Cand& near_one = cands.front();
                if (near_one.id != nullptr)
                {
                    ::strncpy_s(g_marker_draw.nearest, sizeof(g_marker_draw.nearest), near_one.id, _TRUNCATE);
                }
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

            // A non-custom hud_preset overrides minimap_anchor (and puts the compass on
            // the same vertical side - see draw_compass).
            const mm::Anchor anchor = effective_anchor(cfg);
            float x0 = cfg.offset_x;
            float y0 = cfg.offset_y;
            if (anchor == mm::Anchor::TopRight || anchor == mm::Anchor::BottomRight)
            {
                x0 = screen_w - cfg.offset_x - side;
            }
            if (anchor == mm::Anchor::BottomLeft || anchor == mm::Anchor::BottomRight)
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
            const bool slice_ok = plan_slice(cfg, chapter, g.half, now);
            const SliceView sv = slice_view();
            if (slice_ok && sv.shown >= 0)
            {
                const SliceBuf& b = g_slice[sv.shown];
                const UvMap window{sv.min_y, sv.max_x, sv.px_per_uu, b.w, b.h};
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

            // A ring where something was just collected - so a pickup taken off screen
            // (or behind the player) still registers on the minimap.
            draw_found_rings(dl, now, (std::max)(4.0f, cfg.markers_size * 1.4f),
                             [&](double wx, double wy, float& sx, float& sy) {
                                 const double wdx = wx - g.px;
                                 const double wdy = wy - g.py;
                                 const double zz = g.zoom > 0.0001f ? static_cast<double>(g.zoom) : 1.0;
                                 const double dx = (-g.sin_yaw * wdx + g.cos_yaw * wdy) / zz;
                                 const double dy = (-g.cos_yaw * wdx - g.sin_yaw * wdy) / zz;
                                 const double lim = static_cast<double>(g.half) - 2.0;
                                 if (cfg.round)
                                 {
                                     if (dx * dx + dy * dy > lim * lim)
                                     {
                                         return false;
                                     }
                                 }
                                 else if (std::abs(dx) > lim || std::abs(dy) > lim)
                                 {
                                     return false;
                                 }
                                 sx = g.center.x + static_cast<float>(dx);
                                 sy = g.center.y + static_cast<float>(dy);
                                 return true;
                             });

            // THE CARDINAL REFERENCE, always drawn (review-0.9.1 item 5). Until 0.9.3
            // the north dot appeared only when `rotate_with_player` was on - so the
            // SHIPPED north-up default had no cardinal reference at all, and nothing on
            // screen said which way the picture was oriented. Now: four ticks on the
            // rim plus the letter N, rotating with the yaw in rotate mode and standing
            // still (N straight up) in north-up mode, where they say "this is north-up"
            // rather than nothing.
            {
                // A point on the rim, `inset` pixels in from it - the disc's circle or
                // the square's edge, so the ticks sit ON the frame either way.
                const auto rim = [&](float ang, float inset) {
                    const float dx = std::sin(ang);
                    const float dy = -std::cos(ang);
                    const float lim = (std::max)(4.0f, g.half - inset);
                    if (cfg.round)
                    {
                        return ImVec2{g.center.x + dx * lim, g.center.y + dy * lim};
                    }
                    const float m = (std::max)(std::abs(dx), std::abs(dy));
                    const float sc = m > 1e-4f ? lim / m : 0.0f;
                    return ImVec2{g.center.x + dx * sc, g.center.y + dy * sc};
                };
                const ImU32 tick_col = IM_COL32(226, 230, 236, alpha(0.6f));
                const ImU32 north_col = IM_COL32(255, 226, 160, alpha(0.95f));
                for (int i = 0; i < 4; ++i)
                {
                    const float ang = (static_cast<float>(i) * 90.0f - eff_yaw) * kPi / 180.0f;
                    const bool north = i == 0;
                    dl->AddLine(rim(ang, 3.0f), rim(ang, north ? 11.0f : 8.0f),
                                north ? north_col : tick_col, north ? 2.2f : 1.4f);
                }
                const float na = -eff_yaw * kPi / 180.0f;
                const ImVec2 np = rim(na, 19.0f);
                const ImVec2 ts = ImGui::CalcTextSize("N");
                const ImVec2 tp{np.x - ts.x * 0.5f, np.y - ts.y * 0.5f};
                // A shadow rather than a plate: a plate at the rim would cover the map,
                // and the letter has to be legible over both the fill and the backdrop.
                dl->AddText(ImVec2{tp.x + 1.0f, tp.y + 1.0f}, IM_COL32(0, 0, 0, alpha(0.8f)), "N");
                dl->AddText(tp, north_col, "N");
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
                                      plate_color(alpha(0.7f)), 3.0f);
                    dl->AddText(tp, IM_COL32(255, 190, 235, alpha(1.0f)), label.c_str());
                }
            }

            add_player_arrow(dl, g.center, snap.yaw - eff_yaw,
                             (std::max)(cfg.minimap_arrow_min_px, side * cfg.minimap_arrow_frac));

            // THE WHEEL OVER THE DISC, and why it is gated on the panel being open.
            // Nothing here swallows the wheel: the WndProc hook only swallows input for
            // a MODE (the full map), so during play a wheel notch reaches the game as
            // well - and stealing the player's weapon / item wheel to zoom a minimap is
            // not a trade anybody asked for. While the F2 panel is up the cursor is
            // already ours and the wheel is already an overlay gesture, so it is safe
            // there. `zoom_key` is the route that works during play.
            if (mm::g_panel_open.load(std::memory_order_relaxed) && !ImGui::GetIO().WantCaptureMouse)
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.MouseWheel != 0.0f)
                {
                    const float mdx = io.MousePos.x - g.center.x;
                    const float mdy = io.MousePos.y - g.center.y;
                    const bool over = cfg.round
                                          ? (mdx * mdx + mdy * mdy) <= g.half * g.half
                                          : (std::abs(mdx) <= g.half && std::abs(mdy) <= g.half);
                    if (over)
                    {
                        // Away from the player = zoom out = a LARGER uu/px, so the sign
                        // matches the full map's wheel.
                        g_zoom_steps.fetch_add(io.MouseWheel > 0.0f ? -1 : 1, std::memory_order_relaxed);
                    }
                }
            }

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
            int labels = 0; // how many actually got a label after the overlap pass
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
                              plate_color(static_cast<int>(alpha * 0.62f)), 3.0f);
            dl->AddText(tp, col, text.c_str());
        }

        void draw_highlight(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_hl_debug = HighlightDebug{};
            if (!cfg.overlay_enabled || !cfg.highlight_enabled || !gate_ok)
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

            if (g_frame_cands.empty())
            {
                return;
            }

            struct Cand
            {
                float d2 = 0.0f; // squared 3D distance from the player
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Cand> cands; // render thread only, reused every frame
            cands.clear();

            // The one per-frame marker pass has already walked the buffer and computed
            // the distances; this only filters. The square root is taken for the handful
            // that are actually drawn, where metres are needed.
            const double radius = static_cast<double>(cfg.highlight_radius);
            const float radius2 = static_cast<float>(radius * radius);
            for (const FrameCand& fc : g_frame_cands)
            {
                if (!mdb::cat_enabled(cfg.highlight_categories, static_cast<mdb::Cat>(fc.cat)))
                {
                    continue;
                }
                if (!cfg.highlight_show_found && fc.found)
                {
                    continue; // the point of the feature is what is still UNcollected
                }
                if (fc.d2_3d > radius2)
                {
                    continue;
                }
                cands.push_back(Cand{fc.d2_3d, fc.m});
            }
            g_hl_debug.considered = static_cast<int>(cands.size());

            // One sort (nearest first); the draw loop then runs BACKWARDS so the nearest
            // label ends up on top of the pile.
            const std::size_t cap = static_cast<std::size_t>((std::max)(1, cfg.highlight_max_draw));
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            else
            {
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            }

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float r = cfg.highlight_size;
            const float pad = r * 2.4f;

            // ONE projection pass, nearest first. Everything after it works off this
            // list: the glyphs are drawn from it backwards (far to near, so the nearest
            // ends up on top) and the labels are chosen from it forwards (nearest first,
            // so the cap keeps the ones the player is walking towards).
            struct Shown
            {
                float sx = 0.0f; // screen position, viewport-relative already applied
                float sy = 0.0f;
                float nx = 0.0f; // edge direction, when this one is off screen / behind
                float ny = 0.0f;
                double dist = 0.0; // uu from the player
                int alpha = 0;
                ImU32 col = 0;
                std::uint8_t cat = 0;
                bool on_screen = false;
                bool found = false;
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Shown> shown; // render thread only, reused every frame
            shown.clear();

            for (const Cand& cand : cands)
            {
                const double cand_dist = std::sqrt(static_cast<double>(cand.d2));
                const markers::DrawMarker& m = *cand.m;
                const proj::Result pr = proj::project(cam, m.x, m.y, m.z, screen_w, screen_h);
                if (!pr.valid)
                {
                    continue;
                }

                // Fade with distance: fully lit at the camera, highlight_alpha_far at the
                // radius. Linear - a squared falloff makes everything past half the radius
                // look identical.
                const double t = radius > 1.0 ? (cand_dist / radius) : 0.0;
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
                Shown sh{};
                sh.dist = cand_dist;
                sh.alpha = alpha;
                // THE POINT OF THE FEATURE: while the key is held, quality wins over
                // category, so a weapon and a key item stand out from the consumables.
                sh.col = marker_color_q(cat, m.rarity, alpha, cfg.xray_rarity_colors_enabled,
                                        cfg.xray_rarity_colors);
                sh.cat = m.cat;
                sh.found = (m.flags & markers::kFlagFound) != 0;
                sh.m = &m;

                if (pr.on_screen && !pr.behind)
                {
                    sh.on_screen = true;
                    sh.sx = vp->Pos.x + static_cast<float>(pr.sx);
                    sh.sy = vp->Pos.y + static_cast<float>(pr.sy);
                    shown.push_back(sh);
                    continue;
                }
                if (!cfg.highlight_edge_arrows)
                {
                    continue;
                }
                // Off screen (or behind): an arrow on the rim pointing the way to turn.
                // proj::project() already handed us a direction rather than a mirrored
                // position, so this is just a projection onto the border box.
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
                sh.sx = vp->Pos.x + (std::max)(pad, (std::min)(static_cast<float>(screen_w) - pad, sx));
                sh.sy = vp->Pos.y + (std::max)(pad, (std::min)(static_cast<float>(screen_h) - pad, sy));
                sh.nx = static_cast<float>(nx);
                sh.ny = static_cast<float>(-ny);
                shown.push_back(sh);
            }

            // Glyphs and arrows, far to near.
            for (std::size_t ci = shown.size(); ci-- > 0;)
            {
                const Shown& sh = shown[ci];
                const mdb::Cat cat = static_cast<mdb::Cat>(sh.cat);
                const ImU32 edge = IM_COL32(10, 12, 16, static_cast<int>(sh.alpha * 0.9f));
                if (sh.on_screen)
                {
                    draw_marker_glyph(dl, cat, ImVec2{sh.sx, sh.sy}, sh.found ? r * 0.75f : r, sh.col, edge,
                                      sh.found);
                    ++g_hl_debug.on_screen;
                }
                else
                {
                    const int dim = static_cast<int>(sh.alpha * 0.8f);
                    add_edge_arrow(dl, ImVec2{sh.sx, sh.sy}, sh.nx, sh.ny, r * 1.15f,
                                   marker_color_q(cat, sh.m->rarity, dim, cfg.xray_rarity_colors_enabled,
                                                  cfg.xray_rarity_colors),
                                   IM_COL32(10, 12, 16, dim));
                    ++g_hl_debug.edge;
                }
                ++g_hl_debug.drawn;
            }

            //--------------------------------------------------------------------------
            // LABELS (review-0.9.1 item 6)
            //--------------------------------------------------------------------------
            //
            // Eight pickups in one room used to produce eight name+distance boxes on the
            // same few pixels. Now: at most `highlight_labels_max` of them get a label
            // at all - chosen NEAREST FIRST, and never two for glyphs within `r * 2` of
            // each other, because two markers a few pixels apart are one thing to the
            // player - and the survivors are laid out top to bottom by the pure
            // lbl::Layout, which pushes each box down until it clears the ones already
            // placed. A box that had to move gets a leader line back to its glyph.
            if (cfg.highlight_labels && !shown.empty())
            {
                static std::vector<std::size_t> labelled; // render thread only
                labelled.clear();
                lbl::Layout layout{};
                const std::size_t label_cap = static_cast<std::size_t>(
                    (std::max)(0, (std::min)(cfg.highlight_labels_max, lbl::Layout::kMaxRects)));
                for (std::size_t i = 0; i < shown.size() && labelled.size() < label_cap; ++i)
                {
                    const Shown& sh = shown[i];
                    if (!sh.on_screen)
                    {
                        continue; // an edge arrow has no room for a name
                    }
                    if (layout.near_labelled(sh.sx, sh.sy, r * 2.0f))
                    {
                        continue;
                    }
                    layout.note_glyph(sh.sx, sh.sy);
                    labelled.push_back(i);
                }
                // Top to bottom, which is what makes pushing DOWN terminate and keeps
                // the arrangement stable from frame to frame.
                std::sort(labelled.begin(), labelled.end(), [](std::size_t a, std::size_t b) {
                    return shown[a].sy < shown[b].sy;
                });
                const float line_h = ImGui::GetTextLineHeight() + 2.0f;
                const float max_push = line_h * 8.0f;
                for (const std::size_t i : labelled)
                {
                    const Shown& sh = shown[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(sh.cat);
                    const char* name = sh.m->label[0] != '\0' ? sh.m->label : mdb::cat_label(cat);
                    const std::string text =
                        std::format("{}  {:.0f} m{}", name, sh.dist / 100.0, sh.found ? "  (found)" : "");
                    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
                    const float want_y = sh.sy + r + 3.0f;
                    float at_y = want_y;
                    if (!layout.place(sh.sx - ts.x * 0.5f - 4.0f, want_y, ts.x + 8.0f, ts.y + 2.0f, max_push,
                                      at_y))
                    {
                        continue; // no room: the glyph speaks for itself
                    }
                    if (at_y - want_y > 2.0f)
                    {
                        // The leader line, so a displaced label still belongs to a glyph.
                        dl->AddLine(ImVec2{sh.sx, sh.sy + r}, ImVec2{sh.sx, at_y},
                                    IM_COL32(200, 206, 214, static_cast<int>(sh.alpha * 0.55f)), 1.0f);
                    }
                    draw_label(dl, ImVec2{sh.sx, at_y}, text, sh.col, sh.alpha);
                    ++g_hl_debug.labels;
                }
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
            int deduped = 0; // pips left after the 3-px dedupe
        };

        CompassDebug g_compass_debug{};

        void draw_compass(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_compass_debug = CompassDebug{};
            if (!cfg.overlay_enabled || !cfg.compass_enabled || !gate_ok)
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
            const float screen_h = vp->Size.y;
            const float width = (std::max)(120.0f, cfg.compass_width * screen_w);
            const float height = cfg.compass_height;
            const float x0 = vp->Pos.x + (screen_w - width) * 0.5f;
            // compass_offset_y is the distance from whichever edge the strip hangs off,
            // so the key means the same thing in both directions.
            const float y0 = compass_at_bottom(cfg)
                                 ? vp->Pos.y + screen_h - cfg.compass_offset_y - height
                                 : vp->Pos.y + cfg.compass_offset_y;
            const float y1 = y0 + height;

            const float op = cfg.compass_opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            // THE PLATE. `compass_plate = 0` leaves ticks and letters only, which is what
            // the strip needs to sit lightly over the game's own top-centre HUD; with no
            // plate every glyph gets a one-pixel shadow instead, or a bright scene
            // swallows it.
            if (cfg.compass_plate)
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, plate_color(alpha(0.72f)), 4.0f);
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, IM_COL32(150, 158, 168, alpha(0.55f)),
                            4.0f, 0, 1.2f);
            }
            const bool shadow = !cfg.compass_plate;
            const auto shadowed_line = [&](ImVec2 sa, ImVec2 sb, ImU32 scol, float thick) {
                if (shadow)
                {
                    dl->AddLine(ImVec2{sa.x + 1.0f, sa.y + 1.0f}, ImVec2{sb.x + 1.0f, sb.y + 1.0f},
                                IM_COL32(0, 0, 0, alpha(0.75f)), thick);
                }
                dl->AddLine(sa, sb, scol, thick);
            };
            const auto shadowed_text = [&](ImVec2 at, ImU32 scol, const char* text) {
                if (shadow)
                {
                    dl->AddText(ImVec2{at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, alpha(0.8f)), text);
                }
                dl->AddText(at, scol, text);
            };

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
                shadowed_line(ImVec2{x, y1 - len}, ImVec2{x, y1 - 2.0f}, IM_COL32(226, 230, 236, a),
                              t.rank == 2 ? 2.0f : 1.2f);
                if (t.label[0] != '\0')
                {
                    const ImVec2 ts = ImGui::CalcTextSize(t.label);
                    shadowed_text(ImVec2{x - ts.x * 0.5f, y0 + 1.0f},
                                  IM_COL32(240, 242, 246, t.rank == 2 ? alpha(1.0f) : alpha(0.8f)),
                                  t.label);
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
            if (!g_frame_cands.empty())
            {
                struct Pip
                {
                    float d2 = 0.0f; // squared: only ever compared and sorted on
                    double x = 0.0;  // where on the strip it lands, in screen px
                    std::uint8_t cat = 0;
                    std::uint8_t rarity = 0;
                    bool found = false;
                };
                static std::vector<Pip> pips; // render thread only
                pips.clear();
                // The one per-frame marker pass (build_frame_candidates) has already
                // walked the buffer and computed the distances; this only filters.
                const double max_d = static_cast<double>(cfg.compass_marker_distance);
                const float max_d2 = static_cast<float>(max_d * max_d);
                for (const FrameCand& fc : g_frame_cands)
                {
                    if (!mdb::cat_enabled(cfg.compass_categories, static_cast<mdb::Cat>(fc.cat)))
                    {
                        continue;
                    }
                    if (fc.d2_xy > max_d2)
                    {
                        continue;
                    }
                    const markers::DrawMarker& m = *fc.m;
                    double px = 0.0;
                    double rel = 0.0;
                    // Off-strip pips are dropped HERE rather than in the draw loop, so
                    // the cap and the dedupe below both work on pips that will actually
                    // be drawn.
                    if (!cmp::strip_x(strip, cmp::bearing_deg(snap.x, snap.y, m.x, m.y), px, rel))
                    {
                        continue;
                    }
                    Pip p{};
                    p.d2 = fc.d2_xy;
                    p.x = px;
                    p.cat = fc.cat;
                    p.rarity = fc.rarity;
                    p.found = fc.found;
                    pips.push_back(p);
                }
                // One sort (nearest first), then drawn back to front so the nearest pip
                // ends up on top - the same trick as draw_markers.
                const std::size_t kMaxPips = static_cast<std::size_t>(cfg.compass_max_pips);
                if (pips.size() > kMaxPips)
                {
                    std::partial_sort(pips.begin(), pips.begin() + kMaxPips, pips.end(),
                                      [](const Pip& a, const Pip& b) { return a.d2 < b.d2; });
                    pips.resize(kMaxPips);
                }
                else
                {
                    std::sort(pips.begin(), pips.end(), [](const Pip& a, const Pip& b) { return a.d2 < b.d2; });
                }
                // DEDUPE. Six chests in one room are six pips within a pixel of each
                // other: a solid smear that says "chests" less clearly than one glyph
                // would. The list is sorted NEAREST FIRST, so keeping the first pip in
                // each 3-px column keeps the nearest one of every cluster - and the
                // walk is O(n x kept) over at most compass_max_pips entries.
                constexpr double kDedupePx = 3.0;
                std::size_t kept = 0;
                for (std::size_t i = 0; i < pips.size(); ++i)
                {
                    bool crowded = false;
                    for (std::size_t j = 0; j < kept; ++j)
                    {
                        if (pips[j].cat == pips[i].cat &&
                            std::abs(pips[j].x - pips[i].x) < kDedupePx)
                        {
                            crowded = true;
                            break;
                        }
                    }
                    if (!crowded)
                    {
                        pips[kept++] = pips[i];
                    }
                }
                pips.resize(kept);
                g_compass_debug.deduped = static_cast<int>(pips.size());

                // Drawn back to front, so the nearest pip of a cluster ends up on top -
                // the same trick as draw_markers.
                for (std::size_t pi = pips.size(); pi-- > 0;)
                {
                    const Pip& p = pips[pi];
                    const int a = p.found ? alpha(0.35f) : alpha(1.0f);
                    const ImU32 col = marker_color_q(static_cast<mdb::Cat>(p.cat), p.rarity, a,
                                                     cfg.markers_rarity_tint, cfg.xray_rarity_colors);
                    const ImVec2 at{static_cast<float>(p.x), y1 - height * 0.30f};
                    draw_marker_glyph(dl, static_cast<mdb::Cat>(p.cat), at, height * 0.22f, col,
                                      IM_COL32(10, 12, 16, a), p.found);
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

        // The full map's texture size for a given canvas. Pure geometry, so both the
        // render thread (which allocates) and the loop thread (which cuts) can derive
        // the same numbers from the same request.
        void map_slice_size(const mm::Config& cfg, const mv::Rect& canvas, int& tw, int& th)
        {
            const double kMargin = kMapSliceMargin;
            const double zoom = g_mv.uu_per_px;
            const double want_w_uu = static_cast<double>(canvas.w()) * zoom * kMargin;
            const double want_h_uu = static_cast<double>(canvas.h()) * zoom * kMargin;

            tw = static_cast<int>(std::lround(static_cast<double>(canvas.w()) * kMargin));
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
            const double step = (want_w_uu * static_cast<double>(tw) > 0.0)
                                    ? (want_w_uu / static_cast<double>(tw))
                                    : 1.0;
            th = step > 0.0 ? static_cast<int>(std::lround(want_h_uu / step)) : 64;
            th = (th / 8) * 8;
            if (th < 64)
            {
                th = 64;
            }
            if (th > cfg.map_slice_px * 2)
            {
                th = (cfg.map_slice_px * 2 / 8) * 8;
            }
        }

        // RENDER THREAD. Size and allocate the full map's buffers and publish what the
        // slicer should cut. The cut runs on the loop thread - see slice_map_step().
        // Returns true while a buffer is available to draw.
        bool plan_map_slice(const mm::Config& cfg, const mapdata::Chapter& ch, const mv::Rect& canvas,
                            float feet, std::uint64_t now)
        {
            if (!ch.has_heights() || canvas.w() < 8.0f || canvas.h() < 8.0f)
            {
                SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
                return false;
            }

            int tw = 0;
            int th = 0;
            map_slice_size(cfg, canvas, tw, th);

            if (g_mslice[0].w != tw || g_mslice[0].h != th || g_mslice[0].tex == nullptr)
            {
                // A failing allocation must not retry (and log) once per frame - the
                // same "back a failing blind path off" rule the navmesh scan learned.
                static std::uint64_t create_failed_ms = 0;
                if (create_failed_ms != 0 && now - create_failed_ms < 5000)
                {
                    return false;
                }
                if (!slicer_pause_begin(kSlicerPauseMs))
                {
                    return map_slice_view().shown >= 0; // try again next frame
                }
                wait_for_gpu(); // the old buffers may still be in flight
                const bool ok = create_slice_set(g_mslice, kMapSliceBufs, tw, th, L"full map");
                if (!ok)
                {
                    destroy_map_slice_buffers();
                }
                else
                {
                    for (int i = 0; i < kMapSliceBufs; ++i)
                    {
                        g_mslice_copy_pending[i].store(false);
                        g_mslice_in_flight[i].store(0);
                    }
                    g_mslice_next = 0;
                    clear_map_slice_view();
                    note_slice_buffers_changed();
                }
                slicer_pause_end();
                if (!ok)
                {
                    create_failed_ms = now;
                    return false;
                }
                create_failed_ms = 0;
            }

            {
                SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = true;
                g_map_req.cx = g_mv.cx;
                g_map_req.cy = g_mv.cy;
                g_map_req.zoom = g_mv.uu_per_px;
                g_map_req.canvas_w = canvas.w();
                g_map_req.canvas_h = canvas.h();
                g_map_req.feet = feet;
                g_map_req.show_all_floors = cfg.map_show_all_floors;
            }
            g_map_req_ms.store(now, std::memory_order_relaxed);
            return map_slice_view().shown >= 0;
        }

        // LOOP THREAD. Cut the visible region (plus a margin) into the map's own dynamic
        // texture, if anything changed and the next buffer is free.
        void slice_map_step(std::uint64_t now)
        {
            MapSliceReq req{};
            {
                SpinGuard guard(g_slice_req_lock);
                req = g_map_req;
            }
            if (!req.wanted || now - g_map_req_ms.load(std::memory_order_relaxed) > 500)
            {
                return; // the map is closed, or the render thread stopped asking
            }

            mm::Snapshot snap{};
            if (!mm::read_snapshot(snap))
            {
                return;
            }
            // Re-read the planes on EVERY cut: a chapter switch retires them and frees
            // them after a grace period. mapdata's retire runs on this same thread, so a
            // pointer read here cannot be freed while the cut is running.
            const mapdata::Chapter* chp = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (chp == nullptr || !chp->has_heights())
            {
                return;
            }
            const mapdata::Chapter& ch = *chp;
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                return;
            }

            if (g_map_recut.exchange(false, std::memory_order_acq_rel))
            {
                g_mr_valid = false; // the map was recentred: the old region says nothing
            }

            const mm::Config& cfg = mm::cfg_cached();
            SliceBuf& b = g_mslice[g_mslice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0 || b.h <= 0)
            {
                return;
            }

            // The step the render thread's sizing implies, recomputed from the buffer
            // that actually exists.
            const double kMargin = kMapSliceMargin;
            const double want_w_uu = static_cast<double>(req.canvas_w) * req.zoom * kMargin;
            const double step = want_w_uu * hm.px_per_uu / static_cast<double>(b.w);
            if (!(step > 0.0))
            {
                return;
            }

            // The world rectangle this cut will cover, derived from the texture so the
            // drawn quad matches the pixels exactly.
            const double half_w = static_cast<double>(b.w) * step / hm.px_per_uu * 0.5;
            const double half_h = static_cast<double>(b.h) * step / hm.px_per_uu * 0.5;

            // Does the visible viewport still sit inside the region we already cut?
            const double view_half_x = static_cast<double>(req.canvas_h) * req.zoom * 0.5;
            const double view_half_y = static_cast<double>(req.canvas_w) * req.zoom * 0.5;
            const bool inside = g_mr_valid && req.cx - view_half_x >= g_mr_x0 &&
                                req.cx + view_half_x <= g_mr_x1 && req.cy - view_half_y >= g_mr_y0 &&
                                req.cy + view_half_y <= g_mr_y1;
            const bool feet_moved = !g_mr_valid || std::abs(req.feet - g_mr_feet) > 20.0f;
            const bool zoomed = !g_mr_valid || req.zoom != g_mr_zoom;
            const bool chapter_changed = g_mr_chapter != ch.key;
            const bool resized = g_mr_w != b.w || g_mr_h != b.h;
            const bool urgent = !inside || zoomed || chapter_changed || resized;
            if (!urgent && !feet_moved)
            {
                return;
            }

            // The rate cap. An urgent cut (the view has left the region, or the zoom
            // changed) still has to wait for the buffer, but not for the clock: showing
            // an empty edge is worse than one extra cut.
            const int period = cfg.map_slice_hz > 0 ? 1000 / cfg.map_slice_hz : 166;
            if (!urgent && g_mslice_last_ms != 0 &&
                now - g_mslice_last_ms < static_cast<std::uint64_t>(period))
            {
                return;
            }

            const std::uint64_t in_flight = g_mslice_in_flight[g_mslice_next].load(std::memory_order_acquire);
            ID3D12Fence* fence = g_fence;
            if (in_flight != 0 && fence != nullptr && fence->GetCompletedValue() < in_flight)
            {
                ++g_mslice_skipped;
                return; // the GPU is still sampling it; keep showing the other buffer
            }

            SliceStyle st = style_from(cfg);
            if (req.show_all_floors)
            {
                // "Show everything": no surface is ever out of range, so the whole
                // chapter's walkable area is on screen with the current storey still
                // picked out at full opacity.
                st.fade = 1.0e9f;
                st.a_dim = 0.34f;
                st.a_faint = 0.24f;
            }

            const double x1 = req.cx + half_h; // north edge
            const double y0 = req.cy - half_w; // west edge
            const double src_x0 = (y0 - hm.min_y) * hm.px_per_uu;
            const double src_y0 = (hm.max_x - x1) * hm.px_per_uu;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            slice_region(hm, src_x0, src_y0, step, b.w, b.h, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, req.feet, st, g_mslice_scratch, g_mslice_counts);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            double g_mslice_last_cut_ms = 0.0;
            if (freq > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq);
                g_mslice_last_cut_ms = ms;
                g_mslice_ms = g_mslice_ms == 0.0 ? ms : g_mslice_ms * 0.7 + ms * 0.3;
                if (ms > g_mslice_ms_peak)
                {
                    g_mslice_ms_peak = ms;
                }
            }

            g_mr_x0 = req.cx - half_h;
            g_mr_x1 = x1;
            g_mr_y0 = y0;
            g_mr_y1 = req.cy + half_w;
            g_mr_zoom = req.zoom;
            g_mr_feet = req.feet;
            g_mr_chapter = ch.key;
            g_mr_w = b.w;
            g_mr_h = b.h;
            g_mr_valid = true;

            if (g_pf_mslice < 0)
            {
                g_pf_mslice = mm::perf_register("full map cut", perf::Thread::Loop);
            }
            mm::perf_record_ms(g_pf_mslice, g_mslice_last_cut_ms);

            g_mslice_copy_pending[g_mslice_next].store(true, std::memory_order_release);
            {
                SpinGuard guard(g_slice_view_lock);
                g_mslice_view.shown = g_mslice_next;
                g_mslice_view.valid = true;
                g_mslice_view.x0 = g_mr_x0;
                g_mslice_view.x1 = g_mr_x1;
                g_mslice_view.y0 = g_mr_y0;
                g_mslice_view.y1 = g_mr_y1;
            }
            g_mslice_next = (g_mslice_next + 1) % kMapSliceBufs;
            g_mslice_last_ms = now;
            ++g_mslice_updates;
        }

        void draw_waypoint_glyph(ImDrawList* dl, ImVec2 p, float r, int alpha)
        {
            const ImU32 col = IM_COL32(255, 92, 210, alpha);
            // A 1 Hz pulse ring. The waypoint is the one marker that is never culled and
            // never dimmed, and on a screen of static glyphs the eye finds the moving
            // one - which is the whole job of a waypoint. Phase comes from the wall
            // clock, so every view (minimap, full map, compass) pulses together.
            {
                const float phase =
                    static_cast<float>(::GetTickCount64() % 1000) / 1000.0f;
                const float rad = r * (1.2f + 1.1f * phase);
                const int a = static_cast<int>(static_cast<float>(alpha) * 0.55f * (1.0f - phase));
                if (a > 3)
                {
                    dl->AddCircle(p, rad, IM_COL32(255, 92, 210, a), 18, 1.6f);
                }
            }
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
            // The Stats panel belongs to the map mode, so it goes with it - otherwise it
            // would be left drawn over the game with nothing swallowing the input.
            g_stats_page = false;
            g_shrine_panel = false;
            g_shot_canvas_valid = false;
            mm::logf(L"full map closed: {}", why);
        }

        void draw_full_map(mm::Config cfg, const mm::Snapshot& snap, bool have_state, float ui_scale)
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
                g_map_recut.store(true, std::memory_order_release);
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
            // The floor offset in METRES, and named as a storey delta: `floor +200 uu`
            // was a number in the engine's unit that meant nothing to a player standing
            // one storey up. 1 uu = 1 cm.
            ImGui::TextDisabled("%s   |   %.0f uu/px   |   floor %+.1f m   |   X %.0f  Y %.0f",
                                chapter_ptr != nullptr ? chapter_ptr->key.c_str() : "no chapter here",
                                g_mv.uu_per_px,
                                static_cast<double>(g_map_floor_off) / 100.0,
                                snap.x,
                                snap.y);
            ImGui::SameLine((std::max)(200.0f, ImGui::GetWindowWidth() -
                                                   ImGui::CalcTextSize("FitRecentreClose").x - 90.0f));
            // ZOOM TO FIT. The chapter's bounds are in the manifest, so this is the one
            // view control that cannot be reached by panning and zooming by hand.
            bool want_fit = ImGui::SmallButton("Fit");
            ImGui::SameLine();
            if (ImGui::SmallButton("Stats"))
            {
                g_stats_page = !g_stats_page;
            }
            ImGui::SameLine();
            if (cfg.shrine_list && ImGui::SmallButton("Shrines"))
            {
                g_shrine_panel = !g_shrine_panel;
            }
            if (cfg.shrine_list)
            {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Recentre"))
            {
                g_map_recenter.store(true, std::memory_order_relaxed);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Close"))
            {
                close_map(L"the Close button");
            }

            //--------------------------------------------------------------------------
            // The canvas, with the legend column reserved on its right
            //--------------------------------------------------------------------------
            //
            // The 14 saturated `SmallButton`s that used to sit here (black text on a
            // filled category colour, no wrapping, no counts) are gone: the LEGEND is
            // the filter now. It says what each glyph means, how many of that category
            // this chapter has and how many are found, and clicking a row toggles it -
            // which is three answers from the space one of them used to take.
            const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            // Sized from the text, so it is right at every ui_scale.
            const float legend_w =
                ImGui::CalcTextSize("      merchant   9999/9999").x + ImGui::GetStyle().FramePadding.x * 4.0f;
            const ImVec2 csize{(std::max)(64.0f, avail.x - legend_w - ImGui::GetStyle().ItemSpacing.x),
                               (std::max)(64.0f, avail.y - footer_h)};
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##canvas", csize, ImGuiButtonFlags_MouseButtonLeft);
            const bool canvas_hovered = ImGui::IsItemHovered();
            const bool canvas_active = ImGui::IsItemActive();
            const mv::Rect canvas{cpos.x, cpos.y, cpos.x + csize.x, cpos.y + csize.y};
            // What the screenshot key copies. Recorded every frame the map draws, on
            // the same thread that reads it, so no synchronisation is involved - and
            // cleared by close_map, which is what makes "the map is not open" a
            // reportable failure instead of a copy of whatever was there last.
            g_shot_canvas = canvas;
            g_shot_canvas_valid = true;

            //--------------------------------------------------------------------------
            // The legend, which IS the category filter
            //--------------------------------------------------------------------------
            //
            // One row per category: the glyph as it is actually drawn on the map, the
            // name, and `found / total` from markers::stats() - counted for the CHAPTER
            // in force when the marker filter is on, because a whole-DB total would
            // count five chapters the player cannot see. Clicking a row toggles that
            // category in `markers_categories`, the same mask the minimap, the compass
            // and the F2 chips share.
            ImGui::SameLine();
            if (ImGui::BeginChild("##legend", ImVec2{legend_w, csize.y}, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoSavedSettings))
            {
                const markers::Stats lst = markers::stats();
                const int fch = lst.filter_chapter;
                const bool per_chapter = fch >= 0 && fch <= 8;
                ImGui::TextDisabled(per_chapter ? "legend - chapter" : "legend - all chapters");
                ImDrawList* ldl = ImGui::GetWindowDrawList();
                const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.34f);
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    const mdb::Cat cat = static_cast<mdb::Cat>(i);
                    const bool on = mdb::cat_enabled(cfg.markers_categories, cat);
                    const markers::CatStat& cs =
                        per_chapter ? lst.chapter[fch][i] : lst.cat[i];
                    const ImVec2 row = ImGui::GetCursorScreenPos();
                    ImGui::PushID(i);
                    // The leading spaces are the glyph's gutter: the glyph is drawn over
                    // the row afterwards, so a Selectable still owns the whole width and
                    // the hit area is the row, not the text.
                    const std::string text =
                        cs.total > 0 ? std::format("      {}   {}/{}", mdb::cat_label(cat), cs.found, cs.total)
                                     : std::format("      {}", mdb::cat_label(cat));
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          on ? marker_color(cat, 255) : IM_COL32(150, 150, 150, 170));
                    if (ImGui::Selectable(text.c_str(), on))
                    {
                        cfg.markers_categories ^= mdb::cat_bit(cat);
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    const ImVec2 at{row.x + glyph_r + 4.0f, row.y + ImGui::GetTextLineHeight() * 0.5f};
                    draw_marker_glyph(ldl, cat, at, glyph_r, marker_color(cat, on ? 255 : 90),
                                      IM_COL32(14, 16, 20, on ? 220 : 80));
                }
                ImGui::Spacing();
                if (ImGui::SmallButton("all"))
                {
                    cfg.markers_categories = mdb::kAllCats;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("none"))
                {
                    cfg.markers_categories = 0u;
                }
                ImGui::SameLine();
                bool show_found = !cfg.markers_hide_found;
                if (ImGui::Checkbox("found", &show_found))
                {
                    cfg.markers_hide_found = !show_found;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("show markers already found");
                }
            }
            ImGui::EndChild();

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
            // Home = Fit, the same action as the header button. Safe as a bare key: the
            // full map swallows the keyboard for as long as it is open (lessons.md), so
            // nothing here can leak into the game.
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
            {
                want_fit = true;
            }
            // `?` (and pad Back, below) toggles the controls legend. One long
            // TextDisabled sentence in the footer was unreadable and could not grow.
            if (ImGui::IsKeyPressed(ImGuiKey_Slash, false))
            {
                g_map_help = !g_map_help;
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
                if ((pressed & pad::kBack) != 0)
                {
                    g_map_help = !g_map_help;
                }
            }

            if (g_map_recenter.exchange(false, std::memory_order_relaxed))
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_map_floor_off = 0.0f;
            }
            // FIT. The chapter's bounds come from maps.json, and mv::fit_zoom() spends
            // world X on the canvas height and world Y on its width (the map is
            // north-up) - crossing those over gives a fit that is right on a square
            // canvas and wrong on a 16:9 one.
            if (want_fit)
            {
                if (chapter_ptr == nullptr)
                {
                    mm::log(L"full map: Fit needs a chapter, and none covers this position");
                }
                else
                {
                    const double z = mv::fit_zoom(chapter_ptr->max_x - chapter_ptr->min_x,
                                                  chapter_ptr->max_y - chapter_ptr->min_y,
                                                  static_cast<double>(csize.x),
                                                  static_cast<double>(csize.y));
                    if (z > 0.0)
                    {
                        g_mv.uu_per_px = mv::clamp_zoom(z, zmin, zmax);
                        g_mv.cx = (chapter_ptr->min_x + chapter_ptr->max_x) * 0.5;
                        g_mv.cy = (chapter_ptr->min_y + chapter_ptr->max_y) * 0.5;
                        g_map_floor_off = 0.0f;
                        g_map_recut.store(true, std::memory_order_release);
                    }
                }
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
            const bool map_slice_ok =
                chapter_ptr != nullptr && plan_map_slice(cfg, *chapter_ptr, canvas, feet, now);
            const MapSliceView msv = map_slice_view();
            if (map_slice_ok && msv.shown >= 0 && msv.valid)
            {
                const SliceBuf& b = g_mslice[msv.shown];
                float sx0 = 0.0f;
                float sy0 = 0.0f;
                float sx1 = 0.0f;
                float sy1 = 0.0f;
                // Top-left of the cut region is its NORTH-WEST corner (max X, min Y).
                mv::world_to_screen(g_mv, canvas, msv.x1, msv.y0, sx0, sy0);
                mv::world_to_screen(g_mv, canvas, msv.x0, msv.y1, sx1, sy1);
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
            // The one pixel key this view owns. `cfg` here is the UNSCALED config (the
            // filter chips write back into it), so the UI scale is applied at the point
            // of use rather than through ui_scaled().
            const float mr = cfg.map_marker_size * ui_scale;
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
                    draw_marker_glyph(dl, cat, ImVec2{sx, sy}, mr,
                                      marker_color_q(cat, m.rarity, alpha, cfg.markers_rarity_tint,
                                                     cfg.xray_rarity_colors),
                                      IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f)), found);
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
            draw_found_rings(dl, now, (std::max)(5.0f, mr * 1.4f),
                             [&](double wx, double wy, float& sx, float& sy) {
                                 mv::world_to_screen(g_mv, canvas, wx, wy, sx, sy);
                                 return canvas.contains(sx, sy);
                             });
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
            // SET, or CLEAR when the same gesture lands on the waypoint that is
            // already there. Clearing used to be possible only from the F2 panel, which
            // means leaving the map to undo something done on the map.
            const auto set_or_clear = [&](double wx, double wy) {
                const mv::Waypoint had = mm::waypoint();
                if (had.set)
                {
                    float hx = 0.0f;
                    float hy = 0.0f;
                    float cx2 = 0.0f;
                    float cy2 = 0.0f;
                    mv::world_to_screen(g_mv, canvas, had.x, had.y, hx, hy);
                    mv::world_to_screen(g_mv, canvas, wx, wy, cx2, cy2);
                    const float ddx = hx - cx2;
                    const float ddy = hy - cy2;
                    if (ddx * ddx + ddy * ddy <= pick_r * pick_r)
                    {
                        mm::set_waypoint(mv::Waypoint{});
                        toast("waypoint cleared");
                        return;
                    }
                }
                mv::Waypoint set{};
                set.set = true;
                set.x = wx;
                set.y = wy;
                set.z = static_cast<double>(feet);
                mm::set_waypoint(set);
                toast("waypoint set");
            };
            if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                double wx = 0.0;
                double wy = 0.0;
                mv::screen_to_world(g_mv, canvas, io.MousePos.x, io.MousePos.y, wx, wy);
                set_or_clear(wx, wy);
            }
            if (pad_waypoint || key_waypoint)
            {
                set_or_clear(g_mv.cx, g_mv.cy);
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
                // The stable id is a path (`Chapter1_DGong_logic/BP_treasurebox_C_12`).
                // It is the join key with the live actors and is exactly what a bug
                // report needs - and it is noise to everyone else, so it lives behind
                // the debug switch now.
                if (cfg.debug_readout)
                {
                    ImGui::TextDisabled("%s", hover->id);
                }
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
            // The shrine list (the `Shrines` button in the header)
            //--------------------------------------------------------------------------
            if (g_shrine_panel && cfg.shrine_list)
            {
                const ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(620.0f * ui_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Shrines", &g_shrine_panel,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    draw_shrine_list(cfg, snap, have_state, markers::stats().filter_chapter);
                    ImGui::Spacing();
                    if (ImGui::Button("Close"))
                    {
                        g_shrine_panel = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The collection statistics panel (the `Stats` button in the header)
            //--------------------------------------------------------------------------
            //
            // A child window over the map rather than a separate top-level one: the map
            // is a MODE that owns the whole screen and swallows the input, so a floating
            // window the player cannot reach with a normal Alt+Tab-style focus change
            // would be a trap. It closes with the same button, with Esc, and with the map.
            if (g_stats_page)
            {
                const ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(720.0f * ui_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Collection", &g_stats_page,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    draw_collection_stats(::GetTickCount64(), false);
                    ImGui::Spacing();
                    if (ImGui::Button("Close"))
                    {
                        g_stats_page = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The controls legend (`?` / pad Back)
            //--------------------------------------------------------------------------
            //
            // What used to be one long TextDisabled sentence in the footer. Two columns,
            // built from the CONFIG (so a rebound key is what the player is told), with
            // the gamepad column present only while a pad is actually connected -
            // telling a keyboard player about LB/RB is noise, and XInput already knows
            // the answer.
            if (g_map_help)
            {
                struct Row
                {
                    std::string control;
                    std::string action;
                };
                std::vector<Row> left;
                std::vector<Row> right;
                const auto add = [](std::vector<Row>& into, std::string c, std::string a) {
                    into.push_back(Row{std::move(c), std::move(a)});
                };
                add(left, "mouse / keyboard", "");
                add(left, "drag, WASD, arrows", "pan");
                add(left, "wheel, + / -", "zoom");
                add(left, "ctrl+wheel, Q / E", "floor down / up");
                add(left, "Home", "zoom to fit the chapter");
                add(left, key_name_ascii(cfg.map_recenter_key), "recentre on the player");
                add(left, "right-click, Space", "set a waypoint (again on it clears)");
                add(left, "left-click, F", "toggle found");
                add(left, "click a legend row", "filter that category");
                add(left, key_name_ascii(cfg.screenshot_key), "copy the map to the clipboard");
                add(left, "Shrines", "shrine list (click = waypoint, double-click = centre)");
                add(left, "Stats", "collection statistics");
                add(left, "?", "this legend");
                add(left, key_name_ascii(cfg.map_key) + ", Esc", "close the map");
                if (cfg.map_gamepad && gp.connected)
                {
                    add(right, "gamepad", "");
                    add(right, "left stick", "pan");
                    add(right, "triggers, right stick", "zoom");
                    add(right, "LB / RB", "floor down / up");
                    add(right, "A", "set a waypoint (again on it clears)");
                    add(right, "X", "toggle found");
                    add(right, "Y", "recentre");
                    add(right, "Back", "this legend");
                    add(right, "B", "close the map");
                    add(right, "", "");
                }
                add(right, "outside the map", "");
                add(right, key_name_ascii(cfg.panel_key), "settings panel");
                add(right, key_name_ascii(cfg.zoom_key), "cycle the minimap zoom");
                add(right, key_name_ascii(cfg.reload_key), "reload config, maps and markers");
                if (cfg.highlight_enabled)
                {
                    add(right, "hold " + key_name_ascii(cfg.highlight_key), "x-ray nearby markers");
                }

                const float line = ImGui::GetTextLineHeightWithSpacing();
                const float pad_px = ImGui::GetTextLineHeight();
                float ctrl_w[2] = {0.0f, 0.0f};
                float act_w[2] = {0.0f, 0.0f};
                const std::vector<Row>* cols[2] = {&left, &right};
                for (int c = 0; c < 2; ++c)
                {
                    for (const Row& row : *cols[c])
                    {
                        ctrl_w[c] = (std::max)(ctrl_w[c], ImGui::CalcTextSize(row.control.c_str()).x);
                        act_w[c] = (std::max)(act_w[c], ImGui::CalcTextSize(row.action.c_str()).x);
                    }
                }
                const float gap = pad_px;
                const float col_w[2] = {ctrl_w[0] + gap + act_w[0], ctrl_w[1] + gap + act_w[1]};
                const float box_w = col_w[0] + col_w[1] + pad_px * 3.0f;
                const std::size_t rows =
                    (std::max)(left.size(), right.size());
                const float box_h = static_cast<float>(rows) * line + pad_px * 2.0f;
                const ImVec2 tl{canvas.cx() - box_w * 0.5f, canvas.cy() - box_h * 0.5f};
                dl->AddRectFilled(tl, ImVec2{tl.x + box_w, tl.y + box_h}, plate_color(235), 5.0f);
                dl->AddRect(tl, ImVec2{tl.x + box_w, tl.y + box_h}, IM_COL32(150, 158, 168, 200), 5.0f, 0,
                            1.4f);
                for (int c = 0; c < 2; ++c)
                {
                    const float x = tl.x + pad_px + (c == 1 ? col_w[0] + pad_px : 0.0f);
                    float y = tl.y + pad_px;
                    for (const Row& row : *cols[c])
                    {
                        // A row with no action is a heading, and is the only thing in
                        // here drawn bright.
                        const bool heading = row.action.empty();
                        if (!row.control.empty())
                        {
                            dl->AddText(ImVec2{x, y},
                                        heading ? IM_COL32(255, 226, 160, 255) : IM_COL32(226, 230, 236, 235),
                                        row.control.c_str());
                        }
                        if (!heading)
                        {
                            dl->AddText(ImVec2{x + ctrl_w[c] + gap, y}, IM_COL32(180, 186, 196, 220),
                                        row.action.c_str());
                        }
                        y += line;
                    }
                }
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
                ImGui::TextDisabled("? (or pad Back) shows the controls   %s or Esc closes the map",
                                    key_name_ascii(cfg.map_key).c_str());
            }
            ImGui::TextDisabled("%d of %d marker(s)   cut %dx%d @ %.2f ms%s", g_map_markers_drawn,
                                g_map_markers_total, g_mslice[0].w, g_mslice[0].h, g_mslice_ms,
                                cfg.map_gamepad && gp.connected ? "   gamepad connected" : "");

            ImGui::End();

            if (std::memcmp(&before, &cfg, sizeof(mm::Config)) != 0)
            {
                mm::set_config(cfg);
            }
        }

        //==============================================================================
        // Drawing: the F2 panel
        //==============================================================================

        //==============================================================================
        // The per-activity performance table
        //==============================================================================
        //
        // Every periodic activity in this mod records into perf.hpp's counter table;
        // this prints it. The review of v0.9.1 found two costs that had been invisible
        // for weeks (the 4 Hz widget sweep at 28-51 ms, and publish_round, never timed
        // at all) by reading code - this is so the next one is found by looking.
        //
        // The columns: how often it runs, what an average invocation costs, the worst
        // one since the peaks were last reset, the most recent one, and which thread
        // pays. `avg` and `Hz` are over a rolling window (perf::kWindowMs), so they
        // react instead of being diluted by the whole session.
        void draw_perf_table()
        {
            const perf::Table& pt = mm::perf_table();
            if (pt.count == 0)
            {
                return;
            }
            const std::uint64_t now = ::GetTickCount64();
            if (!ImGui::CollapsingHeader("Performance (per activity)"))
            {
                return;
            }
            if (ImGui::SmallButton("reset peaks"))
            {
                mm::perf_reset_peaks();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("a peak from a loading screen otherwise hides every later one");

            if (ImGui::BeginTable("perf", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("activity");
                ImGui::TableSetupColumn("Hz");
                ImGui::TableSetupColumn("avg ms");
                ImGui::TableSetupColumn("peak ms");
                ImGui::TableSetupColumn("last ms");
                ImGui::TableSetupColumn("thread");
                ImGui::TableHeadersRow();
                for (int i = 0; i < pt.count; ++i)
                {
                    const perf::Counter& c = pt.c[i];
                    const bool is_idle = perf::idle(c, now);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", c.name != nullptr ? c.name : "?");
                    ImGui::TableNextColumn();
                    if (is_idle)
                    {
                        ImGui::TextDisabled("idle");
                    }
                    else
                    {
                        ImGui::Text("%.1f", c.rate_hz);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", c.avg_ms);
                    ImGui::TableNextColumn();
                    // The one number worth colouring: anything over a millisecond on a
                    // periodic path is a frame-time or game-thread problem.
                    if (c.peak_ms >= 4.0)
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%.3f", c.peak_ms);
                    }
                    else if (c.peak_ms >= 1.0)
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.85f, 0.4f, 1.0f}, "%.3f", c.peak_ms);
                    }
                    else
                    {
                        ImGui::Text("%.3f", c.peak_ms);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", c.last_ms);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", perf::thread_name(c.thread));
                }
                ImGui::EndTable();
            }
        }

        //==============================================================================
        // Category chips
        //==============================================================================
        //
        // Three places used to draw fourteen `ImGui::Checkbox`es in a hand-computed grid
        // (the minimap filter, the x-ray filter, the compass filter) - forty-two
        // checkboxes in one window, all identical, none of them telling you what colour
        // the category is on the map.
        //
        // A chip is a SmallButton filled with the category's own glyph colour when the
        // category is on and drawn as a flat outline when it is off, so the row doubles
        // as the legend. `base_id` keeps the three rows apart: ImGui identifies a widget
        // by its label, and three rows of "Chest" in one window would be one widget
        // (that is the PushID rule in lessons.md, and it is why each row passes its own
        // base).
        bool category_chips(std::uint32_t& mask, int base_id, float wrap_width)
        {
            bool changed = false;
            const ImGuiStyle& style = ImGui::GetStyle();
            float x = 0.0f;
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(i);
                const bool on = mdb::cat_enabled(mask, cat);
                const char* label = mdb::cat_label(cat);
                const float w = ImGui::CalcTextSize(label).x + style.FramePadding.x * 4.0f;
                if (i > 0)
                {
                    if (x + w < wrap_width)
                    {
                        ImGui::SameLine();
                    }
                    else
                    {
                        x = 0.0f;
                    }
                }
                x += w + style.ItemSpacing.x;

                const ImU32 col = marker_color(cat, 255);
                const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(col);
                // Black text on a saturated fill, the category's own colour as a thin
                // outline when off. The luminance test is what stops a yellow chip from
                // getting white text nobody can read.
                const float lum = 0.299f * fill.x + 0.587f * fill.y + 0.114f * fill.z;
                const ImVec4 text_col = on ? (lum > 0.55f ? ImVec4{0.06f, 0.06f, 0.06f, 1.0f}
                                                          : ImVec4{1.0f, 1.0f, 1.0f, 1.0f})
                                           : ImVec4{fill.x, fill.y, fill.z, 0.75f};
                const ImVec4 bg = on ? fill : ImVec4{fill.x * 0.18f, fill.y * 0.18f, fill.z * 0.18f, 0.55f};
                ImGui::PushID(base_id + i);
                ImGui::PushStyleColor(ImGuiCol_Button, bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4{fill.x * 0.75f, fill.y * 0.75f, fill.z * 0.75f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
                ImGui::PushStyleColor(ImGuiCol_Text, text_col);
                if (ImGui::SmallButton(label))
                {
                    mask = on ? (mask & ~mdb::cat_bit(cat)) : (mask | mdb::cat_bit(cat));
                    changed = true;
                }
                ImGui::PopStyleColor(4);
                ImGui::PopID();
            }
            return changed;
        }

        // "All" / "None" next to a chip row. Same base_id discipline.
        void chips_with_all_none(const char* what, std::uint32_t& mask, int base_id, float wrap_width)
        {
            ImGui::PushID(base_id);
            if (ImGui::SmallButton("All"))
            {
                mask = mdb::kAllCats;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("None"))
            {
                mask = 0u;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(config key: %s)", what);
            ImGui::PopID();
            category_chips(mask, base_id + 1, wrap_width);
        }

        //==============================================================================
        // Player presets
        //==============================================================================
        //
        // Three named starting points, each setting SEVERAL Player keys at once, so the
        // common answers to "what do I want on screen" are one click instead of fifteen.
        // They deliberately do not touch anything on the Advanced tab, the hotkeys, the
        // master switch or the UI scale: a preset must never undo a machine-specific
        // setting the player had to get right once.
        enum class Preset
        {
            Minimal,
            Loot,
            Exploration,
        };

        void apply_preset(mm::Config& cfg, Preset which)
        {
            const std::uint32_t chest = mdb::cat_bit(mdb::Cat::Chest);
            const std::uint32_t pickup = mdb::cat_bit(mdb::Cat::Pickup);
            const std::uint32_t hidden = mdb::cat_bit(mdb::Cat::Hidden);
            const std::uint32_t shrine = mdb::cat_bit(mdb::Cat::Shrine);
            const std::uint32_t boss = mdb::cat_bit(mdb::Cat::Boss);
            const std::uint32_t elite = mdb::cat_bit(mdb::Cat::Elite);
            const std::uint32_t fog = mdb::cat_bit(mdb::Cat::FogGate);
            const std::uint32_t merchant = mdb::cat_bit(mdb::Cat::Merchant);
            const std::uint32_t npc = mdb::cat_bit(mdb::Cat::Npc);
            const std::uint32_t door = mdb::cat_bit(mdb::Cat::Door);
            const std::uint32_t ladder = mdb::cat_bit(mdb::Cat::Ladder);
            const std::uint32_t lift = mdb::cat_bit(mdb::Cat::Lift);

            cfg.overlay_enabled = true;
            cfg.markers_enabled = true;
            cfg.highlight_enabled = true;
            switch (which)
            {
            case Preset::Minimal:
                // As little as possible while still being a map: a small disc, only the
                // landmarks you navigate by, nothing you have already taken, no compass.
                cfg.show_minimap = true;
                cfg.size_frac = 0.16f;
                cfg.zoom_uu_per_px = 30.0f;
                cfg.opacity = 0.85f;
                cfg.markers_categories = shrine | boss | fog;
                cfg.markers_hide_found = true;
                cfg.markers_clamp_to_edge = false;
                cfg.compass_enabled = false;
                cfg.highlight_categories = chest | pickup;
                break;
            case Preset::Loot:
                // Sweeping a level for what is left in it: a close zoom, only loot on
                // the map, found things gone, everything that is off the disc kept on
                // its rim, and the x-ray tuned wide with the quality colours on.
                cfg.show_minimap = true;
                cfg.size_frac = 0.24f;
                cfg.zoom_uu_per_px = 20.0f;
                cfg.opacity = 0.92f;
                cfg.markers_categories = chest | pickup | hidden | merchant;
                cfg.markers_hide_found = true;
                cfg.markers_clamp_to_edge = true;
                cfg.compass_enabled = true;
                cfg.compass_categories = chest | pickup;
                cfg.highlight_categories = chest | pickup | hidden;
                cfg.highlight_radius = 5000.0f;
                cfg.highlight_labels = true;
                cfg.xray_rarity_colors_enabled = true;
                break;
            case Preset::Exploration:
            default:
                // Learning the level: a wide view, every landmark and every connection
                // (doors, ladders, lifts), found markers still drawn so you can see
                // where you have been. Enemies stay off - the sweep only refreshes them
                // once a second, so they lag while they move.
                cfg.show_minimap = true;
                cfg.size_frac = 0.28f;
                cfg.zoom_uu_per_px = 40.0f;
                cfg.opacity = 0.92f;
                cfg.markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
                cfg.markers_hide_found = false;
                cfg.markers_clamp_to_edge = true;
                cfg.compass_enabled = true;
                cfg.compass_categories = shrine | boss | elite | fog | door | ladder | lift | npc;
                cfg.highlight_categories = chest | pickup;
                break;
            }
        }

        //==============================================================================
        // The F2 panel: Player / Advanced / Debug
        //==============================================================================
        //
        // Until 0.9.2 this was one flat window with fourteen category checkboxes three
        // times over, scan-chunk sliders, POV offsets and fence counters - a developer
        // console with the player's settings mixed into it. The three tabs below are the
        // §2 classification of the config made visible: the Player tab is the Player
        // tier, the Advanced tab is the Advanced tier, and the Debug tab is the Dev tier
        // plus every read-only diagnostic. NOTHING was dropped in the move; the
        // diagnostics that used to sit inside the Markers / Full map / X-ray / Compass
        // headers are on the Debug tab now.
        //
        // Each tab is its own function for a reason beyond tidiness: MSVC counts nested
        // blocks and C1061'd this file once already (lessons.md).

        void panel_player(mm::Config& cfg)
        {
            const float wrap = ImGui::GetContentRegionAvail().x;

            ImGui::SeparatorText("Presets");
            ImGui::TextDisabled("set several of the settings on this tab at once");
            if (ImGui::Button("Minimal HUD"))
            {
                apply_preset(cfg, Preset::Minimal);
            }
            ImGui::SameLine();
            if (ImGui::Button("Loot hunting"))
            {
                apply_preset(cfg, Preset::Loot);
            }
            ImGui::SameLine();
            if (ImGui::Button("Exploration"))
            {
                apply_preset(cfg, Preset::Exploration);
            }

            //--------------------------------------------------------------------------
            // Look: theme and palette
            //--------------------------------------------------------------------------
            //
            // In the FILE a theme only fills in colours the file does not mention; in
            // the PANEL choosing one is an explicit act, so it writes the theme's
            // colours into the five colour keys there and then (they are on the Advanced
            // tab, and the change is visible on the next frame). Anything else would
            // make the combo look broken for a player whose config happens to spell one
            // of those keys out.
            ImGui::SeparatorText("Look");
            int theme_i = static_cast<int>(cfg.theme);
            const char* themes[] = {"neutral", "ink"};
            if (ImGui::Combo("Theme (frame / backdrop / plates / fill)", &theme_i, themes, 2))
            {
                cfg.theme = static_cast<gly::Theme>(theme_i);
                const gly::ThemeColors tc = gly::theme_colors(cfg.theme);
                cfg.minimap_frame_r = static_cast<float>(tc.frame.r);
                cfg.minimap_frame_g = static_cast<float>(tc.frame.g);
                cfg.minimap_frame_b = static_cast<float>(tc.frame.b);
                cfg.minimap_frame_alpha = tc.frame_alpha;
                cfg.minimap_backdrop_r = static_cast<float>(tc.backdrop.r);
                cfg.minimap_backdrop_g = static_cast<float>(tc.backdrop.g);
                cfg.minimap_backdrop_b = static_cast<float>(tc.backdrop.b);
                cfg.minimap_backdrop = tc.backdrop_alpha;
                cfg.floor_base_r = static_cast<float>(tc.floor_base.r);
                cfg.floor_base_g = static_cast<float>(tc.floor_base.g);
                cfg.floor_base_b = static_cast<float>(tc.floor_base.b);
            }
            int pal_i = static_cast<int>(cfg.palette);
            const char* pals[] = {"default", "colorblind"};
            if (ImGui::Combo("Marker palette", &pal_i, pals, 2))
            {
                const gly::Palette was = cfg.palette;
                cfg.palette = static_cast<gly::Palette>(pal_i);
                // The item-quality tiers follow the palette only while they are still
                // the OTHER palette's set - a hand-picked xray_rarity_colors survives.
                bool untouched = true;
                const mdb::Rgb* old_set = gly::rarity_colors(was);
                for (int i = 0; i < mdb::kRarityCount; ++i)
                {
                    untouched = untouched && cfg.xray_rarity_colors[i] == old_set[i];
                }
                if (untouched)
                {
                    const mdb::Rgb* now = gly::rarity_colors(cfg.palette);
                    for (int i = 0; i < mdb::kRarityCount; ++i)
                    {
                        cfg.xray_rarity_colors[i] = now[i];
                    }
                }
            }
            ImGui::TextDisabled("every category has its own glyph shape");

            //--------------------------------------------------------------------------
            // Minimap
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Minimap");
            // THE ONE LINE THAT ANSWERS "why is the minimap not there", on the tab a
            // PLAYER actually opens. It used to live only on the Debug tab, which since
            // 0.9.2 is hidden unless the unshipped dev config turns it on - so the
            // diagnostic the whole show/hide design exists to produce was invisible to
            // everyone it was written for. Only shown while the minimap is hidden;
            // saying "minimap: shown" over a visible minimap is noise.
            if (!g_last_mini.visible)
            {
                char reason[192]{};
                ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr,
                                      nullptr);
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f}, "hidden because: %s", reason);
            }
            ImGui::Checkbox("Overlay enabled", &cfg.overlay_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show minimap", &cfg.show_minimap);
            ImGui::SliderFloat("Size (fraction of screen height)", &cfg.size_frac, 0.08f, 0.6f, "%.2f");
            ImGui::SliderFloat("Zoom (uu per minimap pixel)", &cfg.zoom_uu_per_px, 4.0f, 200.0f, "%.0f");
            ImGui::SliderFloat("Opacity", &cfg.opacity, 0.15f, 1.0f, "%.2f");

            bool round_shape = cfg.round;
            if (ImGui::Checkbox("Round (off = square)", &round_shape))
            {
                cfg.round = round_shape;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Rotate with player (off = north up)", &cfg.rotate_with_player);
            ImGui::Checkbox("Show the floor below / above (dimmed)", &cfg.show_adjacent_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Hide while a menu is open", &cfg.hide_in_menus);
            ImGui::SliderFloat("Floor Z tolerance (uu)", &cfg.floor_z_tolerance, 20.0f, 800.0f, "%.0f");

            //--------------------------------------------------------------------------
            // Placement and scale
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Placement and scale");
            // ONE key that moves the whole HUD. `custom` keeps the three placement keys
            // below in force; anything else overrides the minimap's corner and puts the
            // compass on the same vertical side.
            int preset = static_cast<int>(cfg.hud_preset);
            const char* presets[] = {"custom", "top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("HUD placement", &preset, presets, 5))
            {
                cfg.hud_preset = static_cast<mm::HudPreset>(preset);
            }
            ImGui::BeginDisabled(cfg.hud_preset != mm::HudPreset::Custom);
            int anchor = static_cast<int>(cfg.anchor);
            const char* anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("Minimap corner", &anchor, anchors, 4))
            {
                cfg.anchor = static_cast<mm::Anchor>(anchor);
            }
            ImGui::EndDisabled();
            ImGui::DragFloat("Offset X", &cfg.offset_x, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::DragFloat("Offset Y", &cfg.offset_y, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::TextDisabled("in 1080p pixels");

            // UI SCALE. `auto` is a checkbox over the slider rather than a magic value
            // inside the number, so the slider always says what is actually in force.
            bool auto_scale = cfg.ui_scale_auto;
            if (ImGui::Checkbox("Scale the UI automatically", &auto_scale))
            {
                cfg.ui_scale_auto = auto_scale;
                if (!auto_scale)
                {
                    cfg.ui_scale = g_ui_scale; // start from what is on screen right now
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(in force: %.2f)", static_cast<double>(g_ui_scale));
            ImGui::BeginDisabled(cfg.ui_scale_auto);
            ImGui::SliderFloat("UI scale", &cfg.ui_scale, kUiScaleMin, kUiScaleMax, "%.2f");
            ImGui::EndDisabled();

            //--------------------------------------------------------------------------
            // Markers
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Markers");
            ImGui::Checkbox("Show markers", &cfg.markers_enabled);
            ImGui::SameLine();
            // The INVERSE of markers_hide_found. The config key is phrased as "hide",
            // the question a player asks is "show" - and a checkbox whose label is the
            // opposite of what ticking it does is a bug report waiting to happen.
            bool show_found = !cfg.markers_hide_found;
            if (ImGui::Checkbox("Show found markers", &show_found))
            {
                cfg.markers_hide_found = !show_found;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(found ones are drawn hollow)");
            ImGui::SameLine();
            ImGui::Checkbox("Keep out-of-range markers on the rim", &cfg.markers_clamp_to_edge);
            ImGui::SliderFloat("Marker size (px)", &cfg.markers_size, 2.0f, 16.0f, "%.1f");
            // The chips ARE the legend: each one is filled with the colour that category
            // is drawn in on the map.
            chips_with_all_none("markers_categories", cfg.markers_categories, 1000, wrap);

            //--------------------------------------------------------------------------
            // Collection tracker
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Collection tracker");
            ImGui::Checkbox("Remember what I have collected", &cfg.found_tracker);
            ImGui::SameLine();
            ImGui::Checkbox("Mark items whose level is loaded but absent", &cfg.markers_absence_marks);
            const markers::Stats st = markers::stats();
            // WHICH file, and how it was chosen. A per-save tracker that silently picked
            // the wrong save is indistinguishable from a lost collection, so the answer
            // is on screen rather than only in the log.
            ImGui::Text("Profile: %s", st.found_file[0] != '\0' ? st.found_file : "(none yet)");
            ImGui::SameLine();
            ImGui::TextDisabled("(via %s)", st.found_route[0] != '\0' ? st.found_route : "unresolved");
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("found_profile", cfg.found_profile, sizeof(cfg.found_profile)))
            {
                // Free text on purpose: `auto`, `shared`, or a name of the player's own.
                // It takes effect on Save (or F5) - the loop thread owns the file.
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("auto = one file per save slot (recommended)\n"
                                  "shared = one file for every save\n"
                                  "anything else = wuchang_minimap_found_<name>.txt");
            }
            // The whole collection-statistics page, shared with the full map's Stats
            // panel. One function, so the two views can never disagree about a number.
            draw_collection_stats(::GetTickCount64(), false);

            //--------------------------------------------------------------------------
            // Full map
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Full map");
            ImGui::TextDisabled("Press %s in-world.",
                                key_name_ascii(cfg.map_key).c_str());
            ImGui::SliderFloat("Zoom on open (uu per screen px)", &cfg.map_zoom, cfg.map_zoom_min,
                               cfg.map_zoom_max, "%.0f");
            ImGui::SliderFloat("Map marker size (px)", &cfg.map_marker_size, 3.0f, 24.0f, "%.1f");
            ImGui::Checkbox("Show every floor (ignore the height slice)", &cfg.map_show_all_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad (XInput)", &cfg.map_gamepad);
            ImGui::SameLine();
            ImGui::Checkbox("Remember the waypoint", &cfg.map_waypoint_persist);
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
                ImGui::TextDisabled("no waypoint - right-click on the full map to set one");
            }

            //--------------------------------------------------------------------------
            // The hold-key x-ray highlight
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("X-ray highlight (hold a key)");
            std::string hold = key_name_ascii(cfg.highlight_key);
            if (cfg.highlight_gamepad)
            {
                hold += " or pad " + wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                                     cfg.highlight_pad_lt,
                                                                     cfg.highlight_pad_rt));
            }
            ImGui::TextWrapped("Hold %s in-world to see nearby markers through walls.", hold.c_str());
            ImGui::Checkbox("Enabled##xray", &cfg.highlight_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad chord", &cfg.highlight_gamepad);
            ImGui::SameLine();
            ImGui::Checkbox("Names + distance", &cfg.highlight_labels);
            ImGui::SliderFloat("Radius (uu)", &cfg.highlight_radius, 200.0f, 20000.0f, "%.0f");
            ImGui::SameLine();
            ImGui::TextDisabled("= %.0f m", static_cast<double>(cfg.highlight_radius) / 100.0);
            ImGui::SliderFloat("Glyph size (px)", &cfg.highlight_size, 2.0f, 24.0f, "%.1f");
            ImGui::Checkbox("Colour by item quality", &cfg.xray_rarity_colors_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Also tint the minimap / map / compass", &cfg.markers_rarity_tint);
            ImGui::TextDisabled("colours pickups by the game's own item-type grouping");
            chips_with_all_none("highlight_categories", cfg.highlight_categories, 2000, wrap);

            //--------------------------------------------------------------------------
            // The compass strip
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Compass");
            ImGui::Checkbox("Enabled##compass", &cfg.compass_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show the waypoint bearing", &cfg.compass_show_waypoint);
            ImGui::SliderFloat("Width (fraction of the screen)", &cfg.compass_width, 0.1f, 1.0f, "%.2f");
            // Overridden by a non-custom hud_preset, which is why it goes flat when one
            // is chosen rather than silently doing nothing.
            ImGui::BeginDisabled(cfg.hud_preset != mm::HudPreset::Custom);
            int canchor = cfg.compass_anchor == mm::VAnchor::Bottom ? 1 : 0;
            const char* canchors[] = {"top", "bottom"};
            if (ImGui::Combo("Edge", &canchor, canchors, 2))
            {
                cfg.compass_anchor = canchor == 1 ? mm::VAnchor::Bottom : mm::VAnchor::Top;
            }
            ImGui::EndDisabled();
            ImGui::SliderFloat("Distance from that edge (px)", &cfg.compass_offset_y, 0.0f, 400.0f, "%.0f");
            ImGui::SliderFloat("Degrees across the strip", &cfg.compass_span_deg, 30.0f, 360.0f, "%.0f");
            ImGui::SliderFloat("Compass opacity", &cfg.compass_opacity, 0.1f, 1.0f, "%.2f");
            chips_with_all_none("compass_categories", cfg.compass_categories, 3000, wrap);

            //--------------------------------------------------------------------------
            // Keys
            //--------------------------------------------------------------------------
            // Read from the config by the SAME builder the full map's footer uses, so a
            // rebind cannot make one of the two lie.
            ImGui::SeparatorText("Keys");
            ImGui::TextWrapped("%s", bindings_hint(cfg).c_str());
            ImGui::TextDisabled("rebind them in config_wuchang_minimap.txt (panel_key, map_key, "
                                "map_recenter_key, zoom_key, reload_key, highlight_key)");
        }

        void panel_advanced(mm::Config& cfg)
        {
            const float wrap = ImGui::GetContentRegionAvail().x;

            ImGui::TextDisabled("Correct as shipped.");

            if (ImGui::CollapsingHeader("When the overlay is allowed on screen"))
            {
                ImGui::Checkbox("Only when the camera follows the pawn", &cfg.require_pawn_view);
                ImGui::SliderInt("State stale after (ms)", &cfg.state_stale_ms, 100, 5000);
                ImGui::SliderInt("Grace after a valid pawn (ms)", &cfg.min_visible_after_state_ok_ms, 0, 3000);
                ImGui::SliderInt("Delay after a menu closes (ms)", &cfg.menu_close_show_delay_ms, 0, 1000);
            }

            if (ImGui::CollapsingHeader("Height slicing"))
            {
                ImGui::SliderFloat("Adjacent floor opacity", &cfg.adjacent_floor_opacity, 0.0f, 0.6f, "%.2f");
                ImGui::SliderFloat("Below / above fade range (uu)", &cfg.floor_fade_uu, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Height gradient strength", &cfg.floor_gradient_strength, 0.0f, 0.6f, "%.2f");
                float base[3] = {cfg.floor_base_r / 255.0f, cfg.floor_base_g / 255.0f, cfg.floor_base_b / 255.0f};
                if (ImGui::ColorEdit3("Walkable fill colour", base, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.floor_base_r = base[0] * 255.0f;
                    cfg.floor_base_g = base[1] * 255.0f;
                    cfg.floor_base_b = base[2] * 255.0f;
                }
                ImGui::SliderInt("Slice rate (Hz)", &cfg.slice_hz, 2, 30);
                ImGui::SliderInt("Feet Z smoothing (ms)", &cfg.feet_z_smooth_ms, 1, 1000);
                ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -200.0f,
                                   200.0f, "%.0f");
            }

            if (ImGui::CollapsingHeader("Marker sweep and tracker"))
            {
                ImGui::Checkbox("Live actor sweep", &cfg.markers_live);
                ImGui::SameLine();
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
                // The two knobs that trade game-thread time for marker freshness. The
                // "scan pump" line on the Debug tab is the read-out that says which way
                // to move them.
                ImGui::SliderInt("Full passes per second", &cfg.markers_rounds_per_sec, 1, 10);
                ImGui::SliderInt("Object slots per pump", &cfg.markers_scan_chunk, scan::kChunkMin,
                                 scan::kChunkMax);
                ImGui::SliderInt("Min ms between pumps", &cfg.markers_scan_period_ms, scan::kPeriodMinMs,
                                 scan::kPeriodMaxMs);
                ImGui::SliderFloat("Found marker opacity", &cfg.markers_found_alpha, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max markers drawn per frame", &cfg.markers_max_draw, 0, 4000);
                ImGui::SliderInt("Found file debounce (ms)", &cfg.found_save_debounce_ms, 200, 20000);
                ImGui::SeparatorText("Absence as evidence of a collect");
                ImGui::SliderInt("Confirming rounds", &cfg.markers_absence_rounds, 1, 30);
                chips_with_all_none("markers_absence_categories", cfg.markers_absence_categories, 4000, wrap);
            }

            if (ImGui::CollapsingHeader("Minimap look"))
            {
                ImGui::SliderFloat("Backdrop opacity", &cfg.minimap_backdrop, 0.0f, 1.0f, "%.2f");
                float back[3] = {cfg.minimap_backdrop_r / 255.0f, cfg.minimap_backdrop_g / 255.0f,
                                 cfg.minimap_backdrop_b / 255.0f};
                if (ImGui::ColorEdit3("Backdrop colour", back, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.minimap_backdrop_r = back[0] * 255.0f;
                    cfg.minimap_backdrop_g = back[1] * 255.0f;
                    cfg.minimap_backdrop_b = back[2] * 255.0f;
                }
                float frame[3] = {cfg.minimap_frame_r / 255.0f, cfg.minimap_frame_g / 255.0f,
                                  cfg.minimap_frame_b / 255.0f};
                if (ImGui::ColorEdit3("Frame colour", frame, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.minimap_frame_r = frame[0] * 255.0f;
                    cfg.minimap_frame_g = frame[1] * 255.0f;
                    cfg.minimap_frame_b = frame[2] * 255.0f;
                }
                ImGui::SliderFloat("Frame alpha", &cfg.minimap_frame_alpha, 0.0f, 1.0f, "%.2f");
                // The zoom ladder is edited in the file (it is a list); the panel shows
                // what is in force and which key steps through it, because "nothing
                // happens when I press N" is otherwise unanswerable from in-game.
                {
                    std::string ladder;
                    for (int i = 0; i < cfg.minimap_zoom_preset_count && i < mv::kMaxZoomPresets; ++i)
                    {
                        ladder += std::format("{}{:.0f}", ladder.empty() ? "" : ", ",
                                              cfg.minimap_zoom_presets[i]);
                    }
                    ImGui::TextDisabled("zoom presets (%s cycles): %s", key_name_ascii(cfg.zoom_key).c_str(),
                                        ladder.empty() ? "none - edit minimap_zoom_presets" : ladder.c_str());
                }
                ImGui::SliderFloat("Minimum side (px)", &cfg.minimap_min_px, 16.0f, 512.0f, "%.0f");
                ImGui::SliderFloat("Player arrow (fraction of the side)", &cfg.minimap_arrow_frac, 0.01f,
                                   0.3f, "%.3f");
                ImGui::SliderFloat("Player arrow minimum (px)", &cfg.minimap_arrow_min_px, 2.0f, 64.0f, "%.0f");
                ImGui::SliderFloat("Waypoint size (x marker size)", &cfg.waypoint_size_scale, 0.2f, 4.0f,
                                   "%.2f");
            }

            if (ImGui::CollapsingHeader("Full map tuning"))
            {
                ImGui::SliderFloat("Zoom limit - closest", &cfg.map_zoom_min, 1.0f, 200.0f, "%.0f");
                ImGui::SliderFloat("Zoom limit - furthest", &cfg.map_zoom_max, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Zoom per wheel notch", &cfg.map_zoom_factor, 1.02f, 1.6f, "%.2f");
                ImGui::SliderFloat("Pan speed (screen px per second)", &cfg.map_pan_speed, 100.0f, 4000.0f,
                                   "%.0f");
                ImGui::SliderFloat("Margin (fraction of screen height)", &cfg.map_margin, 0.0f, 0.3f, "%.3f");
                ImGui::SliderFloat("Backdrop opacity##map", &cfg.map_backdrop, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max markers drawn##map", &cfg.map_markers_max_draw, 0, 20000);
                ImGui::SliderFloat("Floor step (uu)", &cfg.map_floor_step, 20.0f, 2000.0f, "%.0f");
                ImGui::SliderInt("Slice texture width (px)", &cfg.map_slice_px, 256, 2048);
                ImGui::SliderInt("Slice rate cap (Hz)", &cfg.map_slice_hz, 1, 30);
                ImGui::SliderFloat("Gamepad deadzone", &cfg.map_gamepad_deadzone, 0.05f, 0.6f, "%.2f");
            }

            if (ImGui::CollapsingHeader("X-ray tuning"))
            {
                ImGui::Checkbox("Include found", &cfg.highlight_show_found);
                ImGui::SameLine();
                ImGui::Checkbox("Edge arrows (off screen / behind)", &cfg.highlight_edge_arrows);
                ImGui::SliderInt("Max drawn (nearest first)", &cfg.highlight_max_draw, 1, 400);
                ImGui::SliderInt("Max labelled (nearest first)", &cfg.highlight_labels_max, 0, 40);
                ImGui::SameLine();
                ImGui::TextDisabled("names only");
                ImGui::SliderFloat("Alpha at the camera", &cfg.highlight_alpha_near, 0.1f, 1.0f, "%.2f");
                ImGui::SliderFloat("Alpha at the radius", &cfg.highlight_alpha_far, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Camera read rate (Hz)", &cfg.highlight_camera_hz, 5, 240);
                ImGui::SeparatorText("Item quality palette");
                for (int i = 1; i < mdb::kRarityCount; ++i)
                {
                    mdb::Rgb& c = cfg.xray_rarity_colors[i];
                    float rgb[3] = {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                                    static_cast<float>(c.b) / 255.0f};
                    ImGui::PushID(i + 700);
                    if (ImGui::ColorEdit3(mdb::rarity_name(i), rgb,
                                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
                    {
                        const auto to_byte = [](float v) {
                            const float clamped = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
                            return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
                        };
                        c.r = to_byte(rgb[0]);
                        c.g = to_byte(rgb[1]);
                        c.b = to_byte(rgb[2]);
                    }
                    ImGui::PopID();
                    if (i + 1 < mdb::kRarityCount)
                    {
                        ImGui::SameLine();
                    }
                }
            }

            if (ImGui::CollapsingHeader("Compass tuning"))
            {
                ImGui::SliderFloat("Height (px)", &cfg.compass_height, 10.0f, 120.0f, "%.0f");
                ImGui::Checkbox("Filled plate behind the strip", &cfg.compass_plate);
                ImGui::SameLine();
                ImGui::TextDisabled("off = ticks and letters with a shadow");
                ImGui::SliderFloat("Marker bearing range (uu)", &cfg.compass_marker_distance, 500.0f,
                                   60000.0f, "%.0f");
                ImGui::SliderFloat("Minor tick spacing (deg)", &cfg.compass_tick_step_deg, 1.0f, 90.0f, "%.0f");
                ImGui::SliderInt("Max bearing pips", &cfg.compass_max_pips, 0, 256);
            }
        }

        // The Dev tier (config_wuchang_minimap_dev.txt). Its own function so the Debug
        // tab does not become one more 300-line block.
        void panel_dev_keys(mm::Config& cfg)
        {
            if (!ImGui::CollapsingHeader("Developer settings (config_wuchang_minimap_dev.txt)"))
            {
                return;
            }
            ImGui::TextDisabled("Saved to config_wuchang_minimap_dev.txt.");
            ImGui::Checkbox("Debug readout (this tab)", &cfg.debug_readout);
            ImGui::SameLine();
            ImGui::Checkbox("Open the panel on start", &cfg.debug_show_panel_on_start);
            ImGui::Checkbox("Load the composite fallback picture", &cfg.fallback_use_composite);
            ImGui::SliderFloat("Composite alpha", &cfg.minimap_composite_alpha, 0.0f, 1.0f, "%.2f");
            ImGui::SeparatorText("Game-state reader");
            ImGui::SliderInt("Position period (ms)", &cfg.reader_position_period_ms, 16, 1000);
            ImGui::SliderInt("Resolve period (ms)", &cfg.reader_resolve_period_ms, 100, 10000);
            ImGui::SliderInt("Widget sweep, fast (ms)", &cfg.reader_widget_sweep_period_ms, 50, 5000);
            ImGui::SliderInt("Widget sweep, backed off (ms)", &cfg.reader_widget_sweep_max_period_ms, 50,
                             30000);
            ImGui::SliderInt("Widget sweep warm window (ms)", &cfg.reader_widget_sweep_warm_ms, 0, 60000);
            ImGui::SliderInt("Transition cooldown (ms)", &cfg.reader_transition_cooldown_ms, 0, 30000);
            {
                float jump = static_cast<float>(cfg.reader_teleport_jump_uu);
                if (ImGui::SliderFloat("Teleport jump (uu per pump)", &jump, 200.0f, 20000.0f, "%.0f"))
                {
                    cfg.reader_teleport_jump_uu = static_cast<double>(jump);
                }
            }
            ImGui::SliderInt("Chapter period (ms)", &cfg.reader_chapter_period_ms, 200, 60000);
            ImGui::SliderInt("Reader log throttle (ms)", &cfg.reader_log_throttle_ms, 500, 60000);
            ImGui::SeparatorText("Sweep, assets, diagnostics");
            ImGui::SliderInt("Live grace rounds", &cfg.markers_live_grace_rounds, 1, 30);
            ImGui::SliderInt("Asset retire grace (ms)", &cfg.map_asset_retire_grace_ms, 0, 60000);
            ImGui::SliderInt("Hide-reason log throttle (ms)", &cfg.hide_reason_log_ms, 0, 60000);
            ImGui::SliderInt("SRV heap size (RESTART)", &cfg.srv_heap_size, 16, 1024);
            ImGui::SeparatorText("X-ray camera reader");
            ImGui::SliderInt("Camera resolve (ms)", &cfg.highlight_camera_resolve_ms, 100, 10000);
            ImGui::SliderInt("Compass-only period (ms)", &cfg.highlight_compass_period_ms, 10, 1000);
            ImGui::SliderInt("Getter fallback period (ms)", &cfg.highlight_getter_period_ms, 10, 1000);
            ImGui::SliderInt("POV scan bytes", &cfg.highlight_pov_scan_bytes, 64, 1024);
            ImGui::SliderInt("POV bad reads before re-pin", &cfg.highlight_pov_bad_reads, 1, 64);
        }

        void panel_debug(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            panel_dev_keys(cfg);

            //--------------------------------------------------------------------------
            // The crash breadcrumb
            //--------------------------------------------------------------------------
            //
            // Where the overlay is now, and - the useful half - what the PREVIOUS session
            // left in wuchang_minimap_last_stage.txt. A non-terminal value there is the
            // only surviving evidence when the process died with UE4SS's log buffer
            // unflushed, so it is called out in colour rather than printed as a fact.
            ImGui::SeparatorText("Stage");
            ImGui::Text("now: %s", crumb::current()[0] != '\0' ? crumb::current() : "(none)");
            ImGui::SameLine();
            ImGui::TextDisabled("(file %s)", cfg.crash_breadcrumb ? "on" : "off - crash_breadcrumb = 0");
            if (crumb::previous_suspicious())
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f},
                                   "last session ended at '%s' - it did NOT shut down cleanly",
                                   crumb::previous());
            }
            else if (crumb::previous()[0] != '\0')
            {
                ImGui::TextDisabled("last session ended at '%s'", crumb::previous());
            }
            else
            {
                ImGui::TextDisabled("no previous session recorded");
            }

            //--------------------------------------------------------------------------
            // The one-press recon dump
            //--------------------------------------------------------------------------
            //
            // context/saveslot-and-teleport-research.md section 3: the four things that
            // cannot be recovered from the cooked assets. It calls nothing and changes
            // nothing - reflection lookups and raw reads only - and writes one file the
            // user can send back.
            ImGui::SeparatorText("Recon dump");
            const recon::Status rc = recon::status();
            ImGui::BeginDisabled(rc.pending);
            if (ImGui::Button("Dump the fast-travel / save-slot recon"))
            {
                recon::request();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("or press %s", key_name_ascii(cfg.recon_dump_key).c_str());
            if (rc.pending)
            {
                ImGui::TextDisabled("gathering on the next game-thread pump...");
            }
            else if (rc.error[0] != '\0')
            {
                ImGui::TextColored(ImVec4{0.95f, 0.5f, 0.4f, 1.0f}, "%s", rc.error);
            }
            else if (rc.file[0] != '\0')
            {
                ImGui::TextWrapped("wrote %d line(s) to %s", rc.lines, rc.file);
            }

            draw_perf_table();

            //--------------------------------------------------------------------------
            // Marker sweep
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Markers");
            const markers::Stats st = markers::stats();
            ImGui::Text("db %d marker(s) / %d chapter(s)   found file %d id(s)   published %d   live %d",
                        st.static_markers,
                        st.chapters_loaded,
                        st.found_ids,
                        st.published,
                        st.live_entries);
            // The absence rule (markers_absence_*). Both numbers are diagnostics:
            // `levels loaded` at 0 means the rule can NEVER fire (nothing to match a
            // marker's level against), which is the failure worth seeing at a glance.
            ImGui::Text("absence marks %d   levels loaded %d   (rule %s, %d round(s), %s)",
                        st.absence_marks,
                        st.levels_loaded,
                        cfg.markers_absence_marks ? "on" : "off",
                        cfg.markers_absence_rounds,
                        mdb::format_category_mask(cfg.markers_absence_categories).c_str());
            // Two independent numbers, and they answer different questions.
            //   PUMP  - what one game-thread pump costs. This is the frame-hitch number;
            //           the target is well under 1 ms, and `max` is the worst single pump
            //           since the mod loaded.
            //   ROUND - what a full pass over the object array cost and how many object
            //           slots it visited. This is the freshness number: the marker set is
            //           `slices x period_ms` old at worst.
            // `!` marks the FindAllOf fallback, which is the old 28 ms-per-pump path and
            // only runs when GUObjectArray reports no elements.
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
            ImGui::Text("publish %.3f ms (avg %.3f, peak %.3f)",
                        st.publish_ms,
                        st.publish_ms_avg,
                        st.publish_ms_peak);
            ImGui::Text("drawn %d of %d (%d clamped, %d filtered)",
                        g_marker_draw.drawn,
                        g_marker_draw.total,
                        g_marker_draw.clamped,
                        g_marker_draw.filtered);
            if (g_marker_draw.nearest[0] != 0)
            {
                ImGui::Text("nearest: %s (%.0f uu)", g_marker_draw.nearest, g_marker_draw.nearest_uu);
            }

            //--------------------------------------------------------------------------
            // Full map / gamepad
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Full map and gamepad");
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

            //--------------------------------------------------------------------------
            // The x-ray camera
            //--------------------------------------------------------------------------
            // WHERE THE CAMERA COMES FROM. This is the block to screenshot if the labels
            // are in the wrong place: it names the route, the pinned offset and how old
            // the pose is.
            ImGui::SeparatorText("X-ray camera");
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
            ImGui::Text("compass: %s   heading %.1f deg from the %s   %d bearing pip(s)",
                        g_compass_debug.visible ? "visible" : "hidden (same gate as the minimap)",
                        g_compass_debug.heading,
                        g_compass_debug.from_camera ? "camera" : "pawn",
                        g_compass_debug.pips);

            //--------------------------------------------------------------------------
            // The game state
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Game state");
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
                // Menu hide/show latency: how long ago the game thread saw the menu state
                // change, and how many roots it re-tests on every pump.
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
                                : std::format("{} ms ago", ::GetTickCount64() - snap.teleport_ms).c_str());
                char narrow[256]{};
                ::WideCharToMultiByte(CP_UTF8, 0, snap.level_name, -1, narrow, sizeof(narrow) - 1, nullptr,
                                      nullptr);
                ImGui::TextWrapped("pawn: %s", narrow);
            }

            ImGui::Spacing();
            char reason[192]{};
            ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr, nullptr);
            // THE ONE LINE THAT ANSWERS "why is the minimap not there". It is recomputed
            // from live state every frame - there is no latch anywhere in the show
            // condition - and every change to it is also logged.
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
            ImGui::Text("backbuffer %ux%u, %u buffer(s), composite %dx%d %s   ui scale %.2f",
                        g_width,
                        g_height,
                        g_buffer_count,
                        g_map.width,
                        g_map.height,
                        g_map.ready ? "ready" : "NOT ready",
                        static_cast<double>(g_ui_scale));
            ImGui::Text("presents %llu, resizes %llu",
                        static_cast<unsigned long long>(g_present_count.load()),
                        static_cast<unsigned long long>(g_resize_count.load()));
        }

        void draw_panel(mm::Config cfg, const mm::Snapshot& snap, bool have_state)
        {
            bool open = true;
            ImGui::SetNextWindowSize(ImVec2{520.0f * g_ui_scale, 620.0f * g_ui_scale},
                                     ImGuiCond_FirstUseEver);
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
                               "  -  beta");

            // The tabs get their own child so the Save / Revert / master-switch row is
            // always at the bottom of the window and never scrolls away with them.
            const float row_h = ImGui::GetFrameHeightWithSpacing() * 3.0f;
            if (ImGui::BeginChild("tabs", ImVec2{0.0f, -row_h}, ImGuiChildFlags_None))
            {
                if (ImGui::BeginTabBar("wuchang_tabs"))
                {
                    if (ImGui::BeginTabItem("Player"))
                    {
                        panel_player(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Advanced"))
                    {
                        panel_advanced(cfg);
                        ImGui::EndTabItem();
                    }
                    // The Debug tab EXISTS only while debug_readout is on, which is a
                    // Dev key in a file players do not have. So the panel a player sees
                    // has two tabs and no developer surface at all.
                    if (cfg.debug_readout && ImGui::BeginTabItem("Debug"))
                    {
                        panel_debug(cfg, snap, have_state);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            // The button says WHICH FILE it writes: there are two now, and the panel is
            // the only place that says which of them a setting lives in.
            if (ImGui::Button("Save to config_wuchang_minimap.txt"))
            {
                mm::g_save_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Write the current settings back to the config file.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert"))
            {
                mm::g_revert_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Re-read the config files and discard unsaved changes.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload settings + maps"))
            {
                mm::g_reload_config = true;
            }

            // THE MASTER SWITCH. Unticking it does not stop anything from here - it only
            // writes mod_enabled = 0 into the config file, which the loop thread's 1 Hz
            // watcher then acts on (modswitch.hpp). That keeps the whole shutdown
            // sequence on the one thread that is allowed to run it, and it means the file
            // and the running state can never disagree.
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
            // THE OTHER OFF SWITCH. Same shutdown, no file written - for ruling the mod
            // out of a problem without ending up with a config to repair afterwards.
            ImGui::SameLine();
            if (ImGui::Button("Disable for this session"))
            {
                modswitch::request_session_disable();
                mm::log(L"master switch: disable for this session requested from the F2 panel - "
                        L"nothing is written to the config file; save or edit it to turn the mod "
                        L"back on (checked once a second).");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Stop the mod until the config file is saved or edited again.");
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
            if (g_shot_toast_ready.exchange(false, std::memory_order_acquire))
            {
                char text[160]{};
                unsigned ms = 2500;
                {
                    SpinGuard guard(g_shot_lock);
                    ::strncpy_s(text, sizeof(text), g_shot_toast, _TRUNCATE);
                    ms = g_shot_toast_ms;
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
                mm::log(L"master switch: the render thread has released ImGui, the descriptor heaps, "
                        L"the slice buffers and the map textures");
            }
            g_swapchain = nullptr;
            g_candidates_logged = 0;
            g_queue.store(nullptr, std::memory_order_release);
            g_failed = false;
            g_render_stopped.store(true, std::memory_order_release);
            crumb::stage(crumb::kTeardownEnd);
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
            if (g_drop_textures.load(std::memory_order_acquire))
            {
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    g_drop_textures.store(false, std::memory_order_release);
                    wait_for_gpu();
                    destroy_all_map_textures();
                    slicer_pause_end();
                    mm::log(L"map textures and slice buffers dropped for a reload");
                }
                // else: the request stays pending and the next frame retries. Never
                // release a resource the loop thread may still be writing into.
            }
            release_finished_uploads();
            // A screenshot whose copy has landed becomes a DIB here, at the top of a
            // frame and after the fence has passed - never inside the frame that
            // recorded it.
            shot_collect();

            if (g_pf_frame < 0)
            {
                g_pf_frame = mm::perf_register("render frame (ImGui)", perf::Thread::Render);
            }
            const std::uint64_t frame_t0 = mm::qpc_us();
            // The UI scale, decided from the CURRENT back buffer and applied before the
            // frame's draw lists exist. ResizeBuffers changes g_height, and a config
            // change comes through cfg_cached, so both re-enter here on their own.
            apply_ui_scale(wanted_ui_scale(mm::cfg_cached(), static_cast<float>(g_height)));
            ImGui_ImplWin32_NewFrame();
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            build_ui();
            ImGui::Render();
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
        if (!cfg.overlay_enabled)
        {
            mm::log(L"overlay disabled by config (enabled = 0) - no hooks installed");
            mm::drain_log();
            return;
        }

        if (!g_hooks_created)
        {
            g_hooks_created = install_hooks();
            crumb::stage(g_hooks_created ? crumb::kHooksInstalled : "hook install FAILED");
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

        const mm::Config& cfg = mm::cfg_cached();
        const std::uint64_t now = ::GetTickCount64();

        // THE HEIGHT SLICER. 1-4 ms of CPU that used to run inside Present twelve times
        // a second; it writes into a persistently mapped upload heap and needs nothing
        // from the frame. render() now only records the CopyTextureRegion.
        //
        // The guard sequence is the loop-thread half of the pause handshake: check,
        // mark busy, check AGAIN. The render thread sets `pause` and then waits for
        // `busy`, so whichever order the two interleave in, one of them backs off.
        if (mm::mod_active() && !g_slicer_pause.load())
        {
            g_slicer_busy.store(true);
            if (g_slicer_pause.load())
            {
                g_slicer_busy.store(false);
            }
            else
            {
                const std::uint32_t gen = g_slice_gen.load(std::memory_order_acquire);
                slice_minimap_step(now);
                slice_map_step(now);
                if (g_slice_gen.load(std::memory_order_acquire) != gen)
                {
                    // The buffer set was recreated while we were cutting. Whatever was
                    // just published describes buffers that no longer exist, so drop it;
                    // the next iteration cuts into the new ones.
                    clear_slice_view();
                    clear_map_slice_view();
                }
                g_slicer_busy.store(false);
            }
        }

        // THE HOTKEY BLOCK, gated to ~60 Hz. UE4SS spins this loop far faster than
        // that, and every iteration used to cost a GetForegroundWindow +
        // GetWindowThreadProcessId pair plus five GetAsyncKeyState calls. A key press
        // lasts tens of milliseconds, so nothing is missed - and the pad poll and the
        // hold key ride along with it, which is exactly the cadence they want.
        static std::uint64_t last_input_ms = 0;
        if (now - last_input_ms < 16)
        {
            return;
        }
        last_input_ms = now;
        if (g_pf_input < 0)
        {
            g_pf_input = mm::perf_register("hotkeys + pad", perf::Thread::Loop);
        }
        const mm::PerfScope input_scope(g_pf_input);

        // The foreground answer changes only when the player alt-tabs, so it is worth
        // 250 ms of cache: two user32 round-trips saved per sample.
        static std::uint64_t fg_checked_ms = 0;
        static bool fg_cached = false;
        if (fg_checked_ms == 0 || now - fg_checked_ms >= 250)
        {
            fg_checked_ms = now;
            const HWND fg = ::GetForegroundWindow();
            DWORD pid = 0;
            if (fg != nullptr)
            {
                ::GetWindowThreadProcessId(fg, &pid);
            }
            fg_cached = (pid == ::GetCurrentProcessId());
        }
        const bool foreground = fg_cached;

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

        // THE MINIMAP ZOOM LADDER. `zoom_key` is a press, the wheel gesture arrives as
        // accumulated steps from the render thread, and both are applied here - the loop
        // thread is the only one allowed to publish a config. Skipped while the full map
        // is open: it swallows the keyboard and owns its own zoom.
        static bool zoom_down = false;
        const bool zoom_now = (::GetAsyncKeyState(cfg.zoom_key) & 0x8000) != 0;
        if (zoom_now && !zoom_down && foreground && !mm::g_map_open.load() && now - last_key > 250)
        {
            last_key = now;
            g_zoom_steps.fetch_add(1, std::memory_order_relaxed);
        }
        zoom_down = zoom_now;
        if (const int steps = g_zoom_steps.exchange(0, std::memory_order_relaxed); steps != 0)
        {
            mm::Config edit = mm::config();
            const int n = (std::min)(edit.minimap_zoom_preset_count, mv::kMaxZoomPresets);
            const int dir = steps > 0 ? 1 : -1;
            for (int i = 0, taken = steps > 0 ? steps : -steps; i < taken && i < 64; ++i)
            {
                edit.zoom_uu_per_px =
                    mv::step_zoom_preset(edit.minimap_zoom_presets, n, edit.zoom_uu_per_px, dir);
            }
            if (edit.zoom_uu_per_px != cfg.zoom_uu_per_px)
            {
                mm::set_config(edit);
                mm::logf(L"minimap zoom: {:.0f} uu/px (cycled with {} over {} preset(s))",
                         edit.zoom_uu_per_px, mm::key_name(edit.zoom_key), n);
            }
            else if (n <= 0)
            {
                mm::log(L"minimap zoom: minimap_zoom_presets is empty - nothing to cycle through");
            }
        }

        // THE FIRST-RUN TIP. Once per install: a 10-second toast naming the keys that
        // are actually bound, because "the mod does nothing" is almost always "I did not
        // know which key opens it". The sentinel is a file next to the config, so
        // reinstalling into a clean folder shows it again and a config reload does not.
        static bool first_run_checked = false;
        if (!first_run_checked && cfg.first_run_toast)
        {
            first_run_checked = true;
            const std::wstring sentinel = mm::mod_dir() + L"\\wuchang_minimap_firstrun.txt";
            if (::GetFileAttributesW(sentinel.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                const std::string text =
                    std::format("{} settings   {} map   hold {} to see items through walls",
                                key_name_ascii(cfg.panel_key), key_name_ascii(cfg.map_key),
                                key_name_ascii(cfg.highlight_key));
                post_toast(text.c_str(), 10000);
                const HANDLE h = ::CreateFileW(sentinel.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    const char* note =
                        "This file only records that the first-run tip has been shown.\r\n"
                        "Delete it to see the tip again on the next launch.\r\n";
                    DWORD written = 0;
                    ::WriteFile(h, note, static_cast<DWORD>(std::strlen(note)), &written, nullptr);
                    ::CloseHandle(h);
                }
                mm::logf(L"first run: showing the key tip - {}",
                         std::wstring(text.begin(), text.end()));
            }
        }

        // MAP -> CLIPBOARD. Only while the full map is open, which is also what makes a
        // plain letter safe as the default: the map mode swallows every keyboard message
        // (lessons.md), so `C` cannot reach the game while this can fire.
        static bool shot_down = false;
        const bool shot_now = (::GetAsyncKeyState(cfg.screenshot_key) & 0x8000) != 0;
        if (shot_now && !shot_down && foreground && mm::g_map_open.load())
        {
            g_shot_request.store(true, std::memory_order_release);
        }
        shot_down = shot_now;

        // The finished bitmap, handed over by the render thread. The clipboard API opens
        // a window-station-wide lock and can block; it belongs here and nowhere near
        // Present.
        if (g_shot_dib_ready.exchange(false, std::memory_order_acquire))
        {
            std::vector<std::uint8_t> dib;
            {
                SpinGuard guard(g_shot_lock);
                dib.swap(g_shot_dib);
            }
            const char* why = nullptr;
            if (dib.size() <= clipimg::kHeaderSize)
            {
                why = "map copy failed: empty bitmap";
            }
            else if (::OpenClipboard(nullptr) == 0)
            {
                why = "map copy failed: clipboard is busy";
            }
            else
            {
                // GMEM_MOVEABLE is required: the clipboard takes OWNERSHIP of the handle
                // on success, so it must not be freed afterwards - and must be freed by
                // us on failure, which is the only branch that calls GlobalFree.
                HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, dib.size());
                void* dst = mem != nullptr ? ::GlobalLock(mem) : nullptr;
                if (dst != nullptr)
                {
                    std::memcpy(dst, dib.data(), dib.size());
                    ::GlobalUnlock(mem);
                    ::EmptyClipboard();
                    if (::SetClipboardData(CF_DIB, mem) == nullptr)
                    {
                        ::GlobalFree(mem);
                        why = "map copy failed: SetClipboardData";
                    }
                }
                else
                {
                    if (mem != nullptr)
                    {
                        ::GlobalFree(mem);
                    }
                    why = "map copy failed: out of memory";
                }
                ::CloseClipboard();
            }
            post_toast(why != nullptr ? why : "map copied to clipboard", 2500);
            g_shot_stage_done.store(true, std::memory_order_release);
            mm::logf(L"screenshot: {} ({} bytes)",
                     why != nullptr ? std::wstring(why, why + std::strlen(why))
                                    : std::wstring{L"copied to the clipboard"},
                     dib.size());
        }

        // THE RECON HOTKEY (Dev). Read-only, so it is safe to leave bound; it is a Dev
        // key, so it only exists when the dev config file does.
        static bool recon_down = false;
        const bool recon_now = cfg.recon_dump_key != 0 &&
                               (::GetAsyncKeyState(cfg.recon_dump_key) & 0x8000) != 0;
        if (recon_now && !recon_down && foreground)
        {
            recon::request();
            post_toast("recon dump requested", 2500);
        }
        recon_down = recon_now;

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
        hl::set_demand(held, cfg.overlay_enabled && cfg.compass_enabled);

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
        // REVERT: re-read the config files and publish them, throwing away every
        // unsaved edit made in the panel. Deliberately not a maps / markers reload -
        // "undo what I just fiddled with" should not cost a 340 MB asset swap.
        if (mm::g_revert_config.exchange(false))
        {
            mm::log(L"config: reverting to what is on disk");
            mm::load_config_file();
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
