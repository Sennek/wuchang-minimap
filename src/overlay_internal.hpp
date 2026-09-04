#pragma once

//
// overlay_internal.hpp - the state and the helpers that the overlay_*.cpp translation
// units share. Everything here lives in `overlay::ovl`; none of it is part of the
// mod's public surface, which is overlay.hpp.
//
// The globals keep the order the overlay uses them in, and the thread that owns each
// one is named next to it. Three threads reach this state:
//
//   RENDER thread - Present / Present1 / ResizeBuffers. Creates and releases every
//                   D3D12 object and builds every draw list.
//   LOOP thread   - UE4SS on_update: hotkeys, config and waypoint I/O, both height
//                   slicers, the map asset load.
//   GAME thread   - never enters this file; its state arrives through
//                   mm::read_snapshot() and markers::publish().
//
// Every `extern` here is defined in overlay.cpp.
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

#include "overlay.hpp"

#include "compass.hpp"
#include "gamepad.hpp"
#include "gamestate.hpp"
#include "glyphs.hpp"
#include "highlight.hpp"
#include "label_layout.hpp"
#include "mapdata.hpp"
#include "mapview.hpp"
#include "breadcrumb.hpp"
#include "clipimg.hpp"
#include "markers.hpp"
#include "modswitch.hpp"
#include "navmesh_dump.hpp"
#include "recon.hpp"
#include "shrines.hpp"
#include "spinlock.hpp"
#include "mmstate.hpp"
#include "projection.hpp"
#include "version.hpp"

// imgui_impl_win32.h deliberately hides this behind `#if 0` so the header does not
// depend on <windows.h>; the backend expects you to copy the declaration yourself.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay
{
    namespace ovl
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr int kSrvHeapSize = 64;
        constexpr int kMaxBuffers = 8;
        // ImGui 1.92's font atlas is dynamic (ImGuiBackendFlags_RendererHasTextures), so
        // `style.FontScaleMain` re-rasterises the glyphs: no atlas rebuild, no texture
        // of ours to release.
        constexpr float kBaseScreenHeight = 1080.0f;
        constexpr float kUiScaleMin = 0.5f;
        constexpr float kUiScaleMax = 4.0f;
        extern float g_ui_scale; // what the HUD is currently drawn at
        extern float g_ui_scale_applied; // what the ImGui style was last built for
        // Render thread only, at the top of the frame: io.Fonts is read by
        // ImGui::NewFrame and by every draw-list text call, so it may only be swapped
        // there - the same rule the F5 texture drop obeys.
        extern char g_font_loaded[192]; // the path the atlas currently holds
        extern bool g_font_checked; // false = the config's path has not been tried yet
        // Roundness of the minimap disc and its rings. A global because the drawing
        // helpers are handed geometry rather than the config.
        constexpr int kCircleSegments = 72;
        extern int g_circle_segments;
        struct ModuleId
        {
            wchar_t name[64]{};      // file name only, lower case
            std::uint32_t size = 0;  // SizeOfImage
            std::uint32_t stamp = 0; // TimeDateStamp
            std::uint32_t sum = 0;   // CheckSum
            std::uint32_t rva = 0;   // the address's offset into the module
            HMODULE base = nullptr;
        };
        inline const wchar_t* format_name(DXGI_FORMAT f)
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
        extern SrvHeap g_srv_heap;
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
            // Written only while the slicer is paused. The loop <-> render handshake
            // lives in the parallel atomic arrays below.
        };
        // Per-pixel scratch for the plane-major slice pass, plus the destination ->
        // source index tables. One set per slicer - the minimap and the full map run at
        // different sizes and must not resize each other's buffers every frame.
        struct SliceScratch
        {
            std::vector<std::uint8_t> state;
            std::vector<float> best_ad;
            std::vector<float> best_d;
            std::vector<int> col_x;
            std::vector<int> row_y;
            // One destination row of height codes, gathered out of the sparse block
            // store (mapdata::HeightMaps::gather_row): a row of the picture crosses
            // several 128-px blocks and the absent ones have no address.
            std::vector<std::uint16_t> gather;

            void clear()
            {
                state.clear();
                best_ad.clear();
                best_d.clear();
                col_x.clear();
                row_y.clear();
                gather.clear();
            }
        };
        struct SliceCounts
        {
            std::uint32_t opaque = 0;
            std::uint32_t dim = 0;
            std::uint32_t faint = 0;
            int surfaces = 0;
        };
        // Perf counter ids (perf.hpp). Namespace-scope, initialised on first use by
        // their single owning thread - never a guarded function static, because one of
        // these paths is entered from the game thread's callback chain.
        extern int g_pf_frame; // the whole render prologue + build_ui
        extern int g_pf_minimap; // draw_minimap
        extern int g_pf_markpass; // build_frame_candidates
        extern int g_pf_slice; // the minimap height-slice cut (loop thread)
        extern int g_pf_mslice; // the full map's cut (loop thread)
        extern int g_pf_input; // the hotkey block and the loop thread's file I/O
        extern int g_pf_pad; // XInput only, split out of the block above
        extern spin::Spinlock g_render_lock;
        extern ID3D12Device* g_device;
        extern std::atomic<ID3D12CommandQueue*> g_queue;
        extern ID3D12GraphicsCommandList* g_cmd_list;
        extern ID3D12DescriptorHeap* g_rtv_heap;
        extern ID3D12Resource* g_backbuffers[kMaxBuffers];
        extern D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kMaxBuffers];
        extern FrameCtx g_frames[kMaxBuffers];
        extern ID3D12Fence* g_fence;
        extern HANDLE g_fence_event;
        extern UINT64 g_fence_value;
        extern UINT g_buffer_count;
        extern DXGI_FORMAT g_format;
        extern UINT g_width;
        extern UINT g_height;
        extern HWND g_hwnd;
        // The proc `hooked_wndproc` chains to. Never set back to null: the game thread
        // may be inside a `CallWindowProcW` on it while the render thread unhooks, and a
        // null would turn that in-flight call into a `DefWindowProcW` that eats the
        // game's own message.
        extern std::atomic<WNDPROC> g_prev_wndproc;
        // Read by the game thread, written by the render thread.
        extern std::atomic<bool> g_imgui_ready;
        extern std::atomic<bool> g_rt_ready;
        // Permanent give-up: a device that is not reachable, an ImGui backend that would
        // not start, an exception escaping our own frame. A lost device or a replaced
        // swapchain goes through `request_readoption()` instead.
        extern std::atomic<bool> g_failed;
        // The chapter composite - only loaded when fallback_use_composite = 1, or when
        // the height maps failed to load at all.
        extern MapTexture g_map;
        // How long the render thread waits for the loop-thread slicer to leave its
        // critical section before giving up on (re)allocating buffers this frame. The
        // slice is 1-4 ms and never blocks, so a timeout means something is badly wrong.
        constexpr unsigned kSlicerPauseMs = 50;
        constexpr int kSliceBufs = 2;
        // Here rather than with the full map's block below: the slicer handshake arrays
        // need both counts.
        constexpr int kMapSliceBufs = 2;
        // Bounds on the square the CPU slicer cuts for the minimap; slice_size_for()
        // reads them.
        constexpr int kSliceMinPx = 128;
        constexpr int kSliceMaxPx = 1024;
        // How much bigger than the visible canvas the full map's cut is, so a drag can
        // move inside the cut before it has to be redone (1.30 = 15 % of the canvas in
        // either direction).
        constexpr double kMapSliceMargin = 1.30;
        extern SliceBuf g_slice[kSliceBufs];
        extern int g_slice_next; // the buffer the next update writes (loop thread)
        extern int g_slice_size; // side of the currently allocated buffers, px
        extern std::uint64_t g_slice_last_ms;
        // Diagnostics only: written by the loop thread (the slicer), read by the F2
        // panel on the render thread. Not atomics - a torn read costs one stale number
        // for one frame.
        extern double g_slice_ms; // cost of the last slice, ms (EMA)
        extern double g_slice_ms_peak;
        extern std::uint64_t g_slice_updates;
        extern std::uint64_t g_slice_skipped;
        // The window the SHOWN buffer covers, as a world->uv mapping.
        extern double g_slice_min_y;
        extern double g_slice_max_x;
        extern double g_slice_px_per_uu;
        // Feet Z, EMA-smoothed so a jump or a step does not snap the whole picture.
        extern float g_feet_z;
        extern bool g_feet_z_valid;
        // Slice statistics, for the F2 debug block.
        extern std::uint32_t g_slice_opaque;
        extern std::uint32_t g_slice_dim;
        extern std::uint32_t g_slice_faint;
        extern int g_slice_surfaces; // height planes the slicer is reading
        extern SliceScratch g_slice_scratch;
        extern std::atomic<bool> g_slicer_pause;
        extern std::atomic<bool> g_slicer_busy;
        extern std::atomic<std::uint32_t> g_slice_gen;
        // loop -> render: this buffer has been filled and its copy is not recorded yet.
        extern std::atomic<bool> g_slice_copy_pending[kSliceBufs];
        extern std::atomic<bool> g_mslice_copy_pending[kMapSliceBufs];
        // render -> loop: the fence value of the last frame that SAMPLED this buffer.
        extern std::atomic<std::uint64_t> g_slice_in_flight[kSliceBufs];
        extern std::atomic<std::uint64_t> g_mslice_in_flight[kMapSliceBufs];
        // render -> loop: what the minimap needs. 0 = the minimap is not drawing, so
        // the slicer stands down (one relaxed load per loop iteration).
        extern std::atomic<int> g_slice_want_px;
        extern std::atomic<std::uint64_t> g_slice_want_ms; // GetTickCount64 of the last request
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
        extern spin::Spinlock g_slice_req_lock;
        extern MapSliceReq g_map_req;
        extern std::atomic<std::uint64_t> g_map_req_ms; // GetTickCount64 of the last request
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
        extern spin::Spinlock g_slice_view_lock;
        extern SliceView g_slice_view;
        extern MapSliceView g_mslice_view;
        extern SliceBuf g_mslice[kMapSliceBufs];
        extern int g_mslice_next;
        extern SliceScratch g_mslice_scratch;
        extern SliceCounts g_mslice_counts;
        extern double g_mslice_ms;
        extern double g_mslice_ms_peak;
        extern std::uint64_t g_mslice_updates;
        extern std::uint64_t g_mslice_skipped;
        extern std::uint64_t g_mslice_last_ms;
        // The WORLD rectangle the shown buffer covers. X is north/south (screen up is
        // +X), Y is west/east - the same axes as everywhere else in this mod.
        extern double g_mr_x0; // south edge
        extern double g_mr_x1; // north edge
        extern double g_mr_y0; // west edge
        extern double g_mr_y1; // east edge
        extern bool g_mr_valid;
        // Render -> loop: "throw the cut region away and re-cut" (the map was recentred).
        // g_mr_* is the loop thread's now, so the render thread may not clear it itself.
        extern std::atomic<bool> g_map_recut;
        extern int g_mr_w; // the buffer size the cut was made at (a resize is urgent)
        extern int g_mr_h;
        extern double g_mr_zoom;
        extern float g_mr_feet;
        extern double g_mr_px; // the player position the cut was made at
        extern double g_mr_py;
        extern std::string g_mr_chapter;
        // The view itself. Render thread only.
        extern mv::View g_mv;
        extern bool g_mv_init;
        extern float g_map_floor_off; // uu added to feet Z by the floor adjustment
        // The full map's controls legend (`?` / pad Back). Render thread only, and
        // deliberately NOT config: it is a thing you glance at, not a setting.
        extern bool g_map_help;
        extern bool g_map_was_open;
        // Set by the loop thread when the recentre key is pressed; consumed by the map.
        extern std::atomic<bool> g_map_recenter;
        // Manual found toggles are applied by the LOOP thread (it owns the master set
        // and the file), so the draw buffer only agrees a round later. These overrides
        // make the click feel instant and are dropped the moment the published buffer
        // says the same thing.
        extern std::vector<std::pair<std::string, bool>> g_found_override;
        // What the map is doing, for the F2 debug block and the map's own footer.
        extern int g_map_markers_drawn;
        extern int g_map_markers_total;
        // The swapchain we render on. Present is called for more than one swapchain
        // (ReShade wraps its own, DLSS frame generation adds another), so the first one
        // that proves to be D3D12 wins and every other Present is ignored.
        extern IDXGISwapChain* g_swapchain;
        // The QI for the current back-buffer index. When it fails - a wrapper that does
        // not forward it - the frame is skipped rather than guessed at (see
        // current_backbuffer_index).
        extern IDXGISwapChain3* g_sc3;
        extern int g_candidates_logged;
        // Re-adoption: the next Present releases everything under the render lock and
        // starts over - a new swapchain, a new queue, a new device off it.
        extern std::atomic<bool> g_readopt;
        extern std::atomic<std::uint64_t> g_readopt_count;
        // A queue that was captured and then proved not to belong to the presenting
        // device. One slot: ExecuteCommandLists must not immediately re-capture the same
        // wrong queue, and there is never more than one wrong answer in flight.
        extern std::atomic<ID3D12CommandQueue*> g_bad_queue;
        extern std::atomic<std::uint64_t> g_present_count;
        extern std::atomic<std::uint64_t> g_resize_count;
        // Where the render thread is, for the loop thread's stall watchdog. Relaxed
        // stores of a literal and a tid: no allocation, no lock, nothing that can stall.
        extern std::atomic<const char*> g_render_stage;
        extern std::atomic<unsigned long> g_render_tid;
        // Set by the loop thread on an F5 reload; consumed on the render thread,
        // which is the only place a D3D12 resource may be released.
        extern std::atomic<bool> g_drop_textures;
        extern std::atomic<bool> g_hooks_installed;
        // Master-switch state. `g_hooks_created` is set once the MinHook trampolines
        // exist, so a re-enable only has to MH_EnableHook them and no address can be
        // hooked twice. `g_render_stopped` is the render thread's answer to "you have
        // been switched off" (see shutdown_render()).
        extern bool g_hooks_created; // loop thread only
        extern std::atomic<bool> g_render_stopped; // render -> loop
        extern std::atomic<bool> g_watchdog_reported;
        extern std::uint64_t g_hook_install_ms;
        // Written by the render thread and read by the F2 panel on the same thread; also
        // logged once from the loop thread, hence a fixed buffer rather than a
        // std::string.
        extern wchar_t g_hide_reason[96];
        // The one-off blocking jobs, split out of the loop and render counters.
        extern int g_pf_newframe; // ImGui_ImplWin32_NewFrame - cross-thread user32
        extern int g_pf_buildui; // build_ui() - our own drawing
        extern int g_pf_clip; // the map -> clipboard hand-off
        extern int g_pf_save; // config / waypoint file writes
        extern int g_pf_reload; // F5: config + maps + markers
        extern std::wstring g_hook_report;
        // True when this launch hooked the addresses out of wuchang_minimap_hookaddr.txt
        // instead of discovering them with a dummy device + queue + swapchain. The
        // watchdog reads it: no Present with cached addresses means the cache is stale
        // and deleting it makes the next launch rediscover them.
        extern bool g_hooks_from_cache;
        // The hide-reason log. Rate-limited: an unchanged reason is never logged again,
        // and a changing one at most once per kReasonLogMs, so a flapping condition
        // cannot flood the log.
        constexpr std::uint64_t kReasonLogMs = 2000; // the default of `hide_reason_log_ms`
        extern wchar_t g_reason_logged[96];
        extern std::uint64_t g_reason_log_ms;
        extern std::uint64_t g_reason_since_ms;
        extern std::uint64_t g_reason_suppressed;
        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
        using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
        extern PresentFn o_Present;
        extern Present1Fn o_Present1;
        extern ResizeBuffersFn o_ResizeBuffers;
        extern ExecuteCommandListsFn o_ExecuteCommandLists;
        // Frames in flight the ImGui backend keeps per-frame buffers for. A fullscreen
        // toggle can raise the swapchain's BufferCount, and a stale count here reuses
        // the descriptor and vertex buffers of a frame the GPU has not finished. Beside
        // the allocators because both are grown by the same event.
        extern int g_imgui_frames_in_flight;
        // The set of virtual keys the window messages of which are swallowed. Raw-input
        // (WM_INPUT) keyboard packets are NOT filtered here.
        extern std::atomic<std::uint32_t> g_swallow_bits[8];
        // When the set was last published. A stale set means the loop thread stopped
        // sampling (disabled for the session, shutting down), and a mod that has stopped
        // running must not still eat the player's keys. Nothing is latched.
        extern std::atomic<std::uint64_t> g_swallow_stamp;
        constexpr std::uint64_t kSwallowStaleMs = 250;
        // A raw keyboard packet needs a second, bigger read (RID_INPUT) to see which key
        // it was, so the header is read first and the payload only for the keyboard -
        // the mouse half runs on every mouse move and must not pay for the Esc half.
        struct RawKind
        {
            bool mouse = false;
            bool keyboard = false;
            bool escape = false; // keyboard && VKey == VK_ESCAPE
        };
        struct PendingMsg
        {
            HWND hwnd = nullptr;
            UINT msg = 0;
            WPARAM wparam = 0;
            LPARAM lparam = 0;
        };
        // A sanity cap, not a working limit: a 60 Hz frame sees a handful of coalesced
        // WM_MOUSEMOVEs and a key event or two. Overflow drops the newest and is
        // counted, which keeps the order of what does get through.
        constexpr int kMsgRing = 512;
        extern spin::Spinlock g_msg_lock;
        extern PendingMsg g_msg_ring[kMsgRing];
        extern int g_msg_head; // oldest unreplayed slot
        extern int g_msg_count; // slots in use
        extern std::atomic<std::uint64_t> g_msg_dropped;
        // Published by the render thread once per frame so the game thread's swallow
        // decision never reads the ImGui context. `WantCaptureMouse` is not needed - the
        // panel owns the whole mouse whenever it is open.
        extern std::atomic<bool> g_imgui_want_keyboard;
        // render -> loop: a text box of ours has the caret. Narrower than
        // WantCaptureKeyboard, which keyboard navigation also raises - a binding must
        // still work while the map has nav focus, and must not while a name is typed.
        extern std::atomic<bool> g_imgui_want_text;
        // World -> texture uv for one image. The composite and every floor layer have
        // their own bounds (layers are cropped to their own footprint), so the mapping
        // travels with the picture rather than with the chapter.
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
        // A point on the minimap. `clamp_to_edge` false means "tell me it is outside the
        // disc" (the rings, and markers with markers_clamp_to_edge off); true means "put
        // it on the rim and say so" (the waypoint, which is never culled).
        struct MiniOffset
        {
            double dx = 0.0;
            double dy = 0.0;
            bool visible = false; // may be drawn at (dx, dy)
            bool clamped = false; // ...but it was pushed onto the rim to get there
        };
        struct MiniDebug
        {
            bool visible = false;
            float u = 0.0f;
            float v = 0.0f;
            std::string chapter;
            float side = 0.0f;
        };
        extern MiniDebug g_last_mini;
        // How the height slice is shaded. The same formula as the offline preview
        // (tools/navmesh/slice_preview.py), so a reported spot reproduces without the
        // game:
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
        // Minimap zoom steps the loop thread owes the config. The render thread may not
        // write the config file and the loop thread cannot see the cursor, so the
        // wheel-over-the-disc gesture and the zoom_key press both land here and are
        // applied in on_update(). Accumulated, so a fast flick of the wheel is not lost.
        extern std::atomic<int> g_zoom_steps;
        // The HUD gate has opened over a gameplay pawn at least once - the first frame a
        // player could see anything of ours, which is what the first-run tip waits for.
        // Set once and never cleared: a "has happened", not a state.
        extern std::atomic<bool> g_hud_gate_ever_open;
        extern gly::Palette g_palette;
        extern mdb::Rgb g_plate;
        // Loot categories: collecting one consumes it, so a found one is finished
        // business. Every other category's "found" is a visit and the thing is still
        // there. The rule lives in mdb::is_loot_cat, beside the offline test that pins
        // it.
        using mdb::is_loot_cat;
        struct MarkerDrawStats
        {
            int total = 0;
            int drawn = 0;
            int clamped = 0;
            int filtered = 0;
            int merged = 0; // rolled into another glyph's count badge
            // A fixed buffer, not a std::string: assigned on the render thread every
            // frame, so one memcpy of at most 64 bytes and no allocation.
            char nearest[64]{};
            float nearest_uu = 0.0f;
        };
        extern MarkerDrawStats g_marker_draw;
        struct FrameCand
        {
            const markers::DrawMarker* m = nullptr;
            float d2_xy = 0.0f; // squared horizontal distance from the player, uu^2
            float d2_3d = 0.0f; // squared 3D distance from the player
            std::uint8_t cat = 0;
            std::uint8_t rarity = 0;
            std::uint8_t flags = 0; // markers::kFlag* of the published row
            bool found = false;
        };
        extern std::vector<FrameCand> g_frame_cands; // render thread only, reused every frame
        extern int g_frame_marker_total; // rows in the published buffer
        extern int g_frame_bad_cat; // rows whose category byte is out of range
        constexpr std::uint64_t kHudFadeMs = 150;
        extern float g_hud_fade;
        extern std::uint64_t g_hud_fade_ms; // when the current show started
        // ---- toasts ------------------------------------------------------------------
        extern char g_toast[160];
        extern std::uint64_t g_toast_until;
        enum class ShotStage
        {
            Idle = 0,
            Recorded, // the copy is in flight on the GPU
            Waiting,  // the loop thread has the bytes
        };
        extern ShotStage g_shot_stage;
        extern std::atomic<bool> g_shot_request; // loop -> render (the hotkey)
        extern ID3D12Resource* g_shot_readback; // render thread only
        extern std::uint64_t g_shot_fence;
        extern UINT g_shot_w;
        extern UINT g_shot_h;
        extern UINT g_shot_pitch;
        extern clipimg::Fmt g_shot_fmt;
        // The canvas rect of the last full-map frame, in back-buffer pixels. Written by
        // draw_full_map every frame it draws, read by the render thread in the same
        // frame - same thread, no synchronisation needed.
        extern mv::Rect g_shot_canvas;
        extern bool g_shot_canvas_valid;
        // render -> loop: the finished DIB. A spinlock, not a queue - one screenshot is
        // in flight at a time and the payload is handed over exactly once.
        extern spin::Spinlock g_shot_lock;
        extern std::vector<std::uint8_t> g_shot_dib;
        extern std::atomic<bool> g_shot_dib_ready;
        // loop -> render: the handover is complete, so the next request may be recorded.
        // A failed clipboard write must not park the state machine in Waiting for ever.
        extern std::atomic<bool> g_shot_stage_done;
        // A toast may only be RAISED where toasts are drawn (the render thread owns
        // g_toast), so this is how the loop thread asks. Newest wins: a toast is a
        // notice, and a queue of stale notices is worse than the latest one.
        extern spin::Spinlock g_toast_lock;
        extern char g_toast_pending[160];
        extern unsigned g_toast_pending_ms;
        extern std::atomic<bool> g_toast_pending_ready;
        // ---- waypoints, search, import / export ---------------------------------------
        // loop -> render: waypoint the nearest unfound marker. The pick needs the
        // published marker buffer and the frame's category mask, both render-thread.
        extern std::atomic<bool> g_nearest_request;
        // render -> window thread: the full map's search box holds text, so Esc empties
        // it instead of closing the map. The WndProc hook closes the map on Esc itself,
        // which is why this cannot be decided on the render thread alone.
        extern std::atomic<bool> g_map_search_active;
        // render -> loop: the found list + waypoints as one JSON file. All file I/O for
        // both directions is the loop thread's.
        extern std::atomic<bool> g_export_request;
        extern std::atomic<bool> g_import_request;
        extern spin::Spinlock g_exchange_lock;
        extern char g_import_path[512];   // what the panel's path box holds
        extern char g_latest_export[512]; // newest wuchang_minimap_export_*.json, or ""
        extern std::atomic<bool> g_latest_export_ready;
        extern bool g_shrine_panel;
        extern char g_shrine_selected[shdb::kMaxIdLen];
        struct StatsCache
        {
            markers::Stats st{};
            shr::State shrines{};
            std::uint64_t round = ~0ull;
            std::uint64_t at_ms = 0;
            bool primed = false;
        };
        extern StatsCache g_stats_cache;
        extern bool g_stats_page; // the full map's Stats panel
        inline const StatsCache& stats_cached(std::uint64_t now)
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
        // The six of the map's fourteen categories a player collects or ticks off.
        // Fixed, not derived from the DB, so a chapter with none of a category still
        // gets its column and the layout does not move between chapters.
        constexpr mdb::Cat kStatsCats[] = {
            mdb::Cat::Shrine, mdb::Cat::Chest, mdb::Cat::Pickup,
            mdb::Cat::Boss,   mdb::Cat::Npc,   mdb::Cat::Note,
        };
        constexpr int kStatsCatCount = static_cast<int>(std::size(kStatsCats));
        // The found-ring watch. The render thread already walks the published buffer
        // every frame, so it keeps the found flags of the markers near the player and
        // diffs them whenever the marker sweep publishes a round (~1 Hz). No
        // game-thread work, no per-frame string compares, nothing survives a round.
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
        extern FoundWatch g_found_watch[kFoundWatch];
        extern int g_found_watch_n;
        extern std::uint64_t g_found_watch_round;
        extern FoundEvent g_found_events[kFoundEvents];
        extern int g_found_event_head;
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
        // Spatial hash that merges same-category glyphs closer than one cell into a
        // single glyph with a count badge.
        //
        // Render thread only. Every buffer is reused, so a full map with four thousand
        // markers allocates nothing per frame.
        class MergeGrid
        {
        public:
            // `cell` is the merge distance in pixels; the grid covers [x0, x0 + w] x
            // [y0, y0 + h] and is clamped to kMaxCells cells on a side (a coarser cell
            // merges slightly more aggressively, which is the safe direction).
            void reset(float x0, float y0, float w, float h, float cell)
            {
                constexpr int kMaxSide = 128;
                m_x0 = x0;
                m_y0 = y0;
                m_cell = (std::max)(1.0f, cell);
                m_cols = (std::min)(kMaxSide, (std::max)(1, static_cast<int>(w / m_cell) + 2));
                m_rows = (std::min)(kMaxSide, (std::max)(1, static_cast<int>(h / m_cell) + 2));
                // Re-derive the cell size from the clamp, so a huge canvas still covers
                // itself rather than merging everything into the top-left corner.
                m_cell = (std::max)(m_cell, (std::max)(w / static_cast<float>(m_cols),
                                                       h / static_cast<float>(m_rows)));
                m_head.assign(static_cast<std::size_t>(m_cols) * static_cast<std::size_t>(m_rows), -1);
                m_next.clear();
                m_cat.clear();
                m_slot.clear();
            }

            // The kept glyph this one should join, or -1 to keep it as a new one.
            int find(float x, float y, int cat) const
            {
                const int c = cell_of(x, y);
                if (c < 0)
                {
                    return -1;
                }
                for (int e = m_head[static_cast<std::size_t>(c)]; e >= 0; e = m_next[static_cast<std::size_t>(e)])
                {
                    if (m_cat[static_cast<std::size_t>(e)] == cat)
                    {
                        return m_slot[static_cast<std::size_t>(e)];
                    }
                }
                return -1;
            }

            void add(float x, float y, int cat, int slot)
            {
                const int c = cell_of(x, y);
                if (c < 0)
                {
                    return;
                }
                m_next.push_back(m_head[static_cast<std::size_t>(c)]);
                m_cat.push_back(cat);
                m_slot.push_back(slot);
                m_head[static_cast<std::size_t>(c)] = static_cast<int>(m_next.size()) - 1;
            }

        private:
            int cell_of(float x, float y) const
            {
                const int cx = static_cast<int>((x - m_x0) / m_cell);
                const int cy = static_cast<int>((y - m_y0) / m_cell);
                if (cx < 0 || cy < 0 || cx >= m_cols || cy >= m_rows)
                {
                    return -1;
                }
                return cy * m_cols + cx;
            }

            float m_x0 = 0.0f;
            float m_y0 = 0.0f;
            float m_cell = 1.0f;
            int m_cols = 1;
            int m_rows = 1;
            std::vector<int> m_head;
            std::vector<int> m_next;
            std::vector<int> m_cat;
            std::vector<int> m_slot;
        };
        // The single show/hide gate: returns why the HUD is hidden, or nullptr. Nothing
        // is remembered between frames - every condition is recomputed from the snapshot
        // the game thread published, so hiding is immediate and cannot latch.
        inline const wchar_t* hud_gate(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state,
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
            // A fresh gameplay pawn must be valid for a while before anything is drawn,
            // or the first snapshot after a load flashes the minimap.
            if (snap.state_ok_since_ms == 0)
            {
                return L"waiting for a valid gameplay state";
            }
            if (now - snap.state_ok_since_ms < static_cast<std::uint64_t>(cfg.min_visible_after_state_ok_ms))
            {
                return L"gameplay state is too fresh (grace period)";
            }
            // The game thread re-tests the cached in-viewport menu roots on every pump
            // (10 Hz), so this is true within ~100 ms of the inventory opening.
            if (cfg.hide_in_menus && snap.menu_open)
            {
                return L"a menu is open";
            }
            // Showing again waits only menu_close_show_delay_ms: a menu closing is not a
            // level transition and must not pay min_visible_after_state_ok_ms.
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
        struct HighlightDebug
        {
            bool active = false;
            bool have_camera = false;
            int gated = 0;      // rows the gate looked at (= the published buffer)
            int considered = 0; // rows that passed the gate
            int drawn = 0;
            int on_screen = 0;
            int edge = 0;
            int labels = 0; // how many actually got a label after the overlap pass
            // Why the others were dropped, indexed by mdb::XrayDrop. [Drawn] is unused.
            int dropped[5]{};
            // Drops that are about drawing rather than about the marker: behind the
            // camera, off screen with the rim arrows off, faded out by the distance
            // ramp.
            int no_projection = 0;
            int offscreen_no_arrow = 0;
            int faded_out = 0;
            std::uint64_t cam_age_ms = 0;
        };
        extern HighlightDebug g_hl_debug;
        struct CompassDebug
        {
            bool visible = false;
            bool from_camera = false;
            double heading = 0.0;
            int pips = 0;
            int deduped = 0; // pips left after the 3-px dedupe
        };
        extern CompassDebug g_compass_debug;
        // One-click Player presets. They touch nothing on the Advanced tab, no hotkey,
        // not the master switch and not the UI scale - a preset must never undo a
        // machine-specific setting.
        enum class Preset
        {
            Minimal,
            Loot,
            Exploration,
        };
        // Which panel sections are open. The render thread owns the bits (it draws the
        // headers) and raises a flag; the loop thread does the file I/O, in the same
        // block as every other write this mod does. One atomic each way, no lock.
        constexpr std::uint32_t kPanelSectionsDefault = 0xFFFFFFFFu; // all open
        extern std::atomic<std::uint32_t> g_panel_sections;
        extern std::atomic<bool> g_panel_state_dirty;
        extern std::atomic<bool> g_panel_state_loaded;
        // One collapsible section of the settings panel.
        struct PanelSection
        {
            const char* title;
            // The words a player would type looking for something in here. The section's
            // own setting names, in lower case; matched as substrings both ways.
            const char* words;
            void (*draw)(mm::Config&, float);
        };
        // Keys the game or another injected DLL already uses, warned about when a
        // binding lands on one. A binding with a modifier is not flagged: `ctrl+e` is
        // the escape hatch this table points at.
        struct GameBind
        {
            int vk;
            const char* what;
        };
        constexpr GameBind kGameBinds[] = {
            {'W', "move forward"},   {'A', "move left"},      {'S', "move back"},
            {'D', "move right"},     {VK_SPACE, "dodge"},     {VK_SHIFT, "sprint"},
            {VK_LSHIFT, "sprint"},   {VK_CONTROL, "crouch"},  {VK_LCONTROL, "crouch"},
            {'E', "interact"},       {'F', "an action bind"}, {'Q', "an action bind"},
            {'R', "an action bind"}, {'G', "an action bind"}, {VK_TAB, "inventory"},
            {VK_ESCAPE, "the pause menu"},
            {'1', "an item slot"},   {'2', "an item slot"},   {'3', "an item slot"},
            {'4', "an item slot"},   {'5', "an item slot"},
            {VK_F6, "RenoDX / DLSS 5 (it ignores modifiers)"},
            {VK_F9, "an engine screenshot bind"},
            {VK_F10, "the UE4SS console"},
            {VK_F11, "the engine fullscreen bind"},
            {VK_F12, "the Steam screenshot key"},
        };
        // nullptr = nothing known wants this binding.
        inline const char* game_bind_clash(int binding)
        {
            if (mm::key_mod(binding) != mm::kKeyModNone)
            {
                return nullptr; // a modifier is the way OUT of a clash
            }
            const int vk = mm::key_vk(binding);
            for (const GameBind& g : kGameBinds)
            {
                if (g.vk == vk)
                {
                    return g.what;
                }
            }
            return nullptr;
        }
        struct KeyBind
        {
            const char* label;
            const char* key;
            int mm::Config::*member;
        };
        constexpr KeyBind kKeyBinds[] = {
            {"Settings panel", "panel_key", &mm::Config::panel_key},
            {"Full map", "map_key", &mm::Config::map_key},
            {"Recentre the map on the player", "map_recenter_key", &mm::Config::map_recenter_key},
            {"Cycle the minimap zoom", "zoom_key", &mm::Config::zoom_key},
            {"Reload settings, maps and markers", "reload_key", &mm::Config::reload_key},
            {"Copy the full map to the clipboard", "screenshot_key", &mm::Config::screenshot_key},
            {"Waypoint the nearest unfound marker", "waypoint_nearest_key",
             &mm::Config::waypoint_nearest_key},
            {"X-ray highlight", "highlight_key", &mm::Config::highlight_key},
        };
        constexpr int kKeyBindCount = static_cast<int>(std::size(kKeyBinds));
        extern int g_capture_row; // render thread only; -1 = nothing armed
        extern bool g_capture_wait_release;
        // "No answer" from current_backbuffer_index: the frame is dropped, not guessed.
        constexpr UINT kNoBackbuffer = ~0u;
        constexpr int kHookCount = 4;
        const wchar_t* const kHookNames[kHookCount] = {L"present", L"resize", L"present1", L"execute"};

        //==============================================================================
        // The functions the overlay_*.cpp files define, in the order they appear
        //==============================================================================

        float wanted_ui_scale(const mm::Config& cfg, float screen_h);
        bool font_path_is_none(const char* path);
        void ensure_ui_font(const mm::Config& cfg);
        void ui_init_io(ImGuiIO& io);
        void feed_pad_nav(const mm::Config& cfg);
        void apply_ui_scale(float scale);
        mm::Config ui_scaled(const mm::Config& cfg, float s);
        mm::Anchor effective_anchor(const mm::Config& cfg);
        bool compass_at_bottom(const mm::Config& cfg);
        std::wstring module_of(const void* addr);
        bool module_identity(HMODULE mod, ModuleId& out);
        bool module_id_of(const void* addr, ModuleId& out);
        std::wstring detour_report(const void* addr);
        void log_overlay_modules();
        void srv_alloc_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
        void srv_free_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE);
        std::int64_t qpc_freq();
        SliceView slice_view();
        MapSliceView map_slice_view();
        void clear_slice_view();
        void clear_map_slice_view();
        bool slicer_pause_begin(unsigned budget_ms);
        void slicer_pause_end();
        void note_slice_buffers_changed();
        void safe_release_queue();
        void request_readoption(const wchar_t* why);
        std::wstring stage_w(const char* s);
        void set_hide_reason(const wchar_t* text);
        void release_render_targets();
        void wait_for_gpu();
        bool ensure_frame_allocators();
        bool create_render_targets(IDXGISwapChain* swapchain);
        void swallow_set_clear();
        void swallow_set_add(int vk);
        bool is_hotkey_message(UINT msg);
        bool hotkey_swallow(WPARAM wparam);
        bool is_mouse_message(UINT msg);
        bool is_keyboard_message(UINT msg);
        bool is_escape_message(UINT msg, WPARAM wparam);
        bool is_escape_key_down(UINT msg, WPARAM wparam);
        bool is_system_chord(UINT msg, WPARAM wparam);
        RawKind raw_kind(UINT msg, LPARAM lparam, bool want_key);
        bool is_raw_mouse_message(UINT msg, LPARAM lparam);
        bool imgui_handles(UINT msg);
        void record_imgui_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
        void replay_imgui_messages();
        LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
        void hook_wndproc();
        void unhook_wndproc();
        void destroy_texture(MapTexture& t);
        void destroy_slice_set(SliceBuf* bufs, int count);
        void destroy_slice_buffers();
        void destroy_map_slice_buffers();
        void destroy_all_map_textures();
        void release_finished_uploads();
        bool begin_map_upload(const mapdata::PendingImage& img, ID3D12GraphicsCommandList* list);
        UvMap uv_of(const mapdata::Chapter& c);
        MiniOffset mini_offset(const MiniGeom& g, double wx, double wy, bool round, float limit,
                               bool clamp_to_edge);
        ImVec2 uv_at(const MiniGeom& g, float dx, float dy);
        void add_image_circle(ImDrawList* dl, ImTextureRef tex, const MiniGeom& g, ImU32 col);
        void add_player_arrow(ImDrawList* dl, ImVec2 c, float angle_deg, float size);
        bool create_slice_set(SliceBuf* bufs, int count, int w, int h, const wchar_t* what);
        bool create_slice_buffers(int size);
        int slice_size_for(const mm::Config& cfg, const mapdata::HeightMaps& hm, float half_px);
        void slice_region(const mapdata::HeightMaps& hm, double sx0, double sy0, double src_step, int w, int h,
                          std::uint8_t* dst, UINT pitch, float feet, const SliceStyle& st, SliceScratch& sc,
                          SliceCounts& counts);
        void slice_window(const mapdata::HeightMaps& hm, int x0, int y0, int size, std::uint8_t* dst, UINT pitch,
                          float feet, const SliceStyle& st);
        SliceStyle style_from(const mm::Config& cfg);
        bool plan_slice(const mm::Config& cfg, const mapdata::Chapter& ch, float half_px, std::uint64_t now);
        void slice_minimap_step(std::uint64_t now);
        void slice_selftest();
        void record_slice_copies(ID3D12GraphicsCommandList* list, SliceBuf* bufs,
                                std::atomic<bool>* pending, std::atomic<std::uint64_t>* in_flight,
                                int count);
        void record_slice_copy(ID3D12GraphicsCommandList* list);
        void draw_srv(ImDrawList* dl, D3D12_GPU_DESCRIPTOR_HANDLE srv, const UvMap& uv, MiniGeom g, ImU32 col,
                      bool round, float x0, float y0, float side);
        void draw_image(ImDrawList* dl, const MapTexture& t, const UvMap& uv, const MiniGeom& g, ImU32 col,
                        bool round, float x0, float y0, float side);
        std::string wide_to_ascii(const std::wstring& wide);
        std::string key_name_ascii(int binding);
        std::string bindings_hint(const mm::Config& cfg);
        ImU32 marker_color(mdb::Cat cat, int alpha);
        ImU32 plate_color(int alpha);
        ImU32 marker_color_q(mdb::Cat cat, std::uint8_t rarity, int alpha, bool use_rarity,
                             const mdb::Rgb* palette);
        void draw_marker_glyph(ImDrawList* dl, mdb::Cat cat, ImVec2 p, float r, ImU32 col, ImU32 edge,
                               bool hollow = false);
        float hud_fade_step(bool target_on, std::uint64_t now);
        void toast_for(const char* text, unsigned ms);
        void toast(const char* text);
        void post_toast(const char* text, unsigned ms);
        void shot_fail(const char* why);
        void shot_reset();
        bool record_shot_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, UINT index);
        void shot_collect();
        void draw_shrine_list(const mm::Snapshot& snap, bool have_state, int filter_chapter);
        void draw_collection_stats(std::uint64_t now, bool compact);
        void draw_toast();
        void note_found_event(double x, double y, std::uint64_t now);
        void update_found_watch(std::uint64_t round, std::uint64_t now);
        void build_frame_candidates(const mm::Snapshot& snap);
        void draw_count_badge(ImDrawList* dl, ImVec2 at, float r, int count, int alpha);
        void draw_markers(const mm::Config& cfg, const MiniGeom& g, bool round, float x0, float y0, float side,
                          ImDrawList* dl);
        void draw_minimap(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state);
        void add_edge_arrow(ImDrawList* dl, ImVec2 p, float dx, float dy, float r, ImU32 col, ImU32 edge);
        void draw_label(ImDrawList* dl, ImVec2 at, const char* text, ImU32 col, int alpha);
        void draw_highlight(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok);
        void draw_compass(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok);
        bool marker_found_now(const markers::DrawMarker& m);
        void toggle_found(const markers::DrawMarker& m);
        void map_slice_size(const mm::Config& cfg, const mv::Rect& canvas, int& tw, int& th);
        bool plan_map_slice(const mm::Config& cfg, const mapdata::Chapter& ch, const mv::Rect& canvas,
                            float feet, std::uint64_t now);
        void slice_map_step(std::uint64_t now);
        void draw_waypoint_glyph(ImDrawList* dl, ImVec2 p, float r, int alpha);
        void reset_map_mode();
        void close_map(const wchar_t* why);
        void draw_full_map(mm::Config cfg, const mm::Snapshot& snap, bool have_state, float ui_scale);
        void draw_perf_table();
        bool category_chips(std::uint32_t& mask, int base_id, float wrap_width);
        void category_filter(const char* title, const char* key, std::uint32_t& mask, int base_id,
                             float wrap_width);
        void apply_preset(mm::Config& cfg, Preset which);
        void player_presets(mm::Config& cfg);
        void player_look(mm::Config& cfg);
        void player_minimap(mm::Config& cfg);
        void player_placement(mm::Config& cfg);
        void player_markers(mm::Config& cfg, float wrap);
        void player_tracker(mm::Config& cfg);
        void player_fullmap(mm::Config& cfg);
        void player_xray(mm::Config& cfg, float wrap);
        void player_compass(mm::Config& cfg, float wrap);
        void player_keys(mm::Config& cfg);
        std::wstring panel_state_path();
        void panel_state_load();
        void panel_state_save();
        bool section_matches(const PanelSection& s, const char* needle);
        void panel_player(mm::Config& cfg);
        void panel_advanced(mm::Config& cfg);
        void panel_dev_keys(mm::Config& cfg);
        bool is_modifier_vk(int vk);
        int held_modifier();
        void arm_capture(int row);
        void panel_bindings(mm::Config& cfg);
        void panel_debug(mm::Config& cfg, const mm::Snapshot& snap, bool have_state);
        void draw_panel(mm::Config cfg, const mm::Snapshot& snap, bool have_state);
        void build_ui();
        bool ensure_initialised(IDXGISwapChain* swapchain);
        UINT current_backbuffer_index(IDXGISwapChain* swapchain);
        bool is_d3d12_swapchain(IDXGISwapChain* sc);
        void log_candidate(IDXGISwapChain* sc, bool d3d12);
        void release_device_objects();
        void shutdown_render();
        void render(IDXGISwapChain* swapchain);
        void render_guarded(IDXGISwapChain* sc);
        void collect_guarded();
        void note_present_result(HRESULT hr);
        HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags);
        HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params);
        HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                                   UINT flags);
        void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                      ID3D12CommandList* const* lists);
        std::wstring hook_cache_path();
        bool write_hook_cache(const ModuleId* ids);
        bool parse_hex_field(std::string_view t, std::uint64_t& out);
        bool read_hook_cache(void** addr);
        void delete_hook_cache(const wchar_t* why);
        bool create_and_enable(void** addr, const wchar_t* how);
        bool install_hooks_from_cache();
        bool install_hooks_by_discovery();
        bool install_hooks();
        auto widen(std::string_view narrow) -> RC::StringType;
    } // namespace ovl
} // namespace overlay
