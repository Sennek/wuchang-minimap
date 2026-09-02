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
            int floor = -1;    // -1 = the composite
            int channels = 4;
            std::size_t bytes = 0;
            bool ready = false;
            std::string chapter;
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

        // The chapter composite (only loaded when fallback_use_composite = 1) and one
        // texture per floor layer, indexed the same way as `Chapter::layers`.
        MapTexture g_map;
        MapTexture g_layer_tex[mapdata::kMaxLayers];
        int g_layers_ready = 0;
        std::size_t g_layer_bytes = 0;

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

        void set_hide_reason(const wchar_t* text)
        {
            ::wcsncpy_s(g_hide_reason, text, std::size(g_hide_reason) - 1);
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

        void destroy_all_map_textures()
        {
            destroy_texture(g_map);
            for (MapTexture& t : g_layer_tex)
            {
                destroy_texture(t);
            }
            g_layers_ready = 0;
            g_layer_bytes = 0;
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
            for (MapTexture& t : g_layer_tex)
            {
                sweep(t);
            }
        }

        // Creates the texture and its upload buffer, and records the copy into
        // `list`. Called with the render lock held, from inside a frame.
        bool begin_map_upload(const mapdata::PendingImage& img, ID3D12GraphicsCommandList* list)
        {
            const bool is_layer = img.layer_index >= 0;
            if (is_layer && img.layer_index >= mapdata::kMaxLayers)
            {
                return false;
            }
            MapTexture& target = is_layer ? g_layer_tex[img.layer_index] : g_map;
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
            target.floor = img.floor;
            target.channels = img.channels;
            target.bytes = static_cast<std::size_t>(total);
            target.chapter = img.chapter_key;
            target.ready = true;
            // The fence this frame will signal at the end of Present; once the GPU has
            // passed it, release_finished_uploads() frees the staging buffer.
            target.upload_fence = g_fence_value + 1;

            if (is_layer)
            {
                g_layer_bytes = 0;
                g_layers_ready = 0;
                for (const MapTexture& t : g_layer_tex)
                {
                    if (t.ready)
                    {
                        ++g_layers_ready;
                        g_layer_bytes += t.bytes;
                    }
                }
                mm::logf(L"map texture: floor layer f{} {}x{} uploaded (R8, {} MB); {} layer(s) resident, "
                         L"{} MB of texture memory",
                         img.floor,
                         img.width,
                         img.height,
                         total / (1024 * 1024),
                         g_layers_ready,
                         g_layer_bytes / (1024 * 1024));
            }
            else
            {
                mm::logf(L"map texture: composite {}x{} uploaded (RGBA8, {} MB), chapter \"{}\"",
                         img.width,
                         img.height,
                         total / (1024 * 1024),
                         std::wstring(img.chapter_key.begin(), img.chapter_key.end()));
            }
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

        UvMap uv_of(const mapdata::Layer& l)
        {
            return UvMap{l.min_y, l.max_x, l.px_per_uu, l.image_width, l.image_height};
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
        // Which floor is the player on?
        //==============================================================================
        //
        // maps.json carries, per XY grid cell, the walkable *surface bands* at that
        // spot (see build_map.py's build_floor_grid: the cell's polygons split by Z at
        // gaps > 250 uu, each band tagged - dominant first - with the layer(s) its
        // polygons were rasterised into). A layer is one SURFACE ORDINAL: layer k is
        // the k-th walkable surface from the bottom at each pixel, so two surfaces
        // stacked above one another are never in the same layer.
        // Every frame:
        //
        //   1. look up the player's cell and score each band by the distance from the
        //      player's feet Z to the band's Z range, with `floor_z_tolerance` uu of
        //      slack (so standing on a ramp or a 1 px-off surface still counts as
        //      "inside");
        //   2. take the nearest band - which is also the rule for being *between*
        //      floors, mid-jump or mid-fall;
        //   3. HYSTERESIS: if last frame's band is still present in this cell and is
        //      no worse than the winner by `floor_hysteresis` uu, keep it. Without this
        //      a staircase flickers between two layers at every step.
        //
        // The band immediately below and above the chosen one are what
        // `show_adjacent_floors` draws dimmed - by band, not by floor *index*, because
        // the floor index is a global rank and the rank order inside one cell is not
        // always the Z order.

        struct FloorPick
        {
            bool have = false;                          // a band was resolved this frame
            bool stale = false;                         // no band here; holding the last one
            int floors[mapdata::kMaxBandFloors]{};
            int floor_count = 0;
            int below[mapdata::kMaxBandFloors]{};
            int below_count = 0;
            int above[mapdata::kMaxBandFloors]{};
            int above_count = 0;
            float z_min = 0.0f;
            float z_max = 0.0f;
            double distance = 0.0;
            int bands_in_cell = 0;
            int band_index = -1;
            std::uint64_t resolved_ms = 0;
        };

        FloorPick g_floor{};

        void copy_floors(const mapdata::Band& band, int* dst, int& count)
        {
            count = band.floor_count;
            for (int i = 0; i < band.floor_count && i < mapdata::kMaxBandFloors; ++i)
            {
                dst[i] = band.floors[i];
            }
        }

        // Returns true when a band was resolved (g_floor.have), false when the player
        // is off the grid entirely - the caller then decides between holding the last
        // floor and the fallback.
        bool pick_floor(const mm::Config& cfg, const mapdata::Chapter& ch, const mm::Snapshot& snap,
                        std::uint64_t now)
        {
            const std::vector<mapdata::Band>* bands =
                ch.grid ? ch.grid->at(snap.x, snap.y) : nullptr;
            if (bands == nullptr || bands->empty())
            {
                if (g_floor.have && now - g_floor.resolved_ms <=
                                        static_cast<std::uint64_t>(cfg.floor_fallback_hold_ms))
                {
                    g_floor.stale = true;
                    return true; // keep drawing the floor we had
                }
                g_floor = FloorPick{};
                return false;
            }

            // The pawn's location is its capsule centre; the navmesh is at its feet.
            const double z = snap.z - static_cast<double>(cfg.player_z_offset);
            const double tol = static_cast<double>(cfg.floor_z_tolerance);
            const auto score = [&](const mapdata::Band& b) {
                const double d = b.distance(z);
                return d <= tol ? 0.0 : d - tol;
            };

            int best = 0;
            double best_score = score((*bands)[0]);
            for (std::size_t i = 1; i < bands->size(); ++i)
            {
                const double s = score((*bands)[i]);
                if (s < best_score)
                {
                    best_score = s;
                    best = static_cast<int>(i);
                }
            }

            if (g_floor.have && !g_floor.stale)
            {
                // The same storey, one frame later: the band whose Z range still
                // overlaps the one we were on (grown by the tolerance both ways).
                int previous = -1;
                for (std::size_t i = 0; i < bands->size(); ++i)
                {
                    const mapdata::Band& b = (*bands)[i];
                    if (static_cast<double>(b.z_min) - tol <= static_cast<double>(g_floor.z_max) + tol &&
                        static_cast<double>(g_floor.z_min) - tol <= static_cast<double>(b.z_max) + tol)
                    {
                        previous = static_cast<int>(i);
                        break;
                    }
                }
                if (previous >= 0 && previous != best &&
                    score((*bands)[previous]) <= best_score + static_cast<double>(cfg.floor_hysteresis))
                {
                    best = previous;
                    best_score = score((*bands)[previous]);
                }
            }

            const mapdata::Band& band = (*bands)[best];
            g_floor = FloorPick{};
            g_floor.have = true;
            g_floor.stale = false;
            g_floor.z_min = band.z_min;
            g_floor.z_max = band.z_max;
            g_floor.distance = best_score;
            g_floor.bands_in_cell = static_cast<int>(bands->size());
            g_floor.band_index = best;
            g_floor.resolved_ms = now;
            copy_floors(band, g_floor.floors, g_floor.floor_count);
            if (best > 0)
            {
                copy_floors((*bands)[best - 1], g_floor.below, g_floor.below_count);
            }
            if (best + 1 < static_cast<int>(bands->size()))
            {
                copy_floors((*bands)[best + 1], g_floor.above, g_floor.above_count);
            }
            return true;
        }

        //==============================================================================
        // Drawing one image (the composite or one floor layer)
        //==============================================================================

        // True when the layer's footprint can be seen at all in the current window. A
        // layer that cannot is skipped: the CLAMP sampler would only smear its
        // transparent margin, but skipping saves a 64-triangle fan per layer.
        bool layer_in_view(const mapdata::Layer& l, const mm::Snapshot& snap, float half, float zoom)
        {
            const double r = static_cast<double>(half) * static_cast<double>(zoom) * 1.45;
            return snap.x + r >= l.min_x && snap.x - r <= l.max_x && snap.y + r >= l.min_y &&
                   snap.y - r <= l.max_y;
        }

        void draw_image(ImDrawList* dl, const MapTexture& t, const UvMap& uv, MiniGeom g, ImU32 col,
                        bool round, float x0, float y0, float side)
        {
            if (!t.ready || t.srv_gpu.ptr == 0)
            {
                return;
            }
            g.uv = uv;
            const ImTextureRef tex{static_cast<ImTextureID>(t.srv_gpu.ptr)};
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
            if (g_layers_ready == 0 && !composite_ready)
            {
                set_hide_reason(L"no map texture");
                return;
            }
            if (g_layers_ready == 0 && chapter.key != g_map.chapter)
            {
                set_hide_reason(L"the loaded map texture is for another chapter");
                return;
            }

            const bool have_floor = pick_floor(cfg, chapter, snap, now);

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

            // Floor tints. The current floor is a bright, slightly cool white; the
            // storey below is cooler and dim, the one above warmer and dimmer still,
            // so "which one am I on" reads at a glance.
            const ImU32 tint_current = IM_COL32(214, 224, 232, alpha(1.0f));
            // A band's polygons can land on more than one surface ordinal (the ordinal
            // boundary is per pixel), so the band names its layers dominant-first: the
            // first one is the storey, the rest are drawn a little dimmer.
            const ImU32 tint_secondary = IM_COL32(196, 208, 218, alpha(0.65f));
            const ImU32 tint_below = IM_COL32(140, 172, 200, alpha(cfg.adjacent_floor_opacity));
            const ImU32 tint_above = IM_COL32(206, 176, 148, alpha(cfg.adjacent_floor_opacity * 0.6f));
            const ImU32 tint_all = IM_COL32(190, 200, 210, alpha(0.55f));

            ImDrawList* dl = ImGui::GetForegroundDrawList();

            if (cfg.round)
            {
                dl->AddCircleFilled(g.center, g.half, backdrop, kCircleSegments);
            }
            else
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, backdrop, 4.0f);
            }

            const auto draw_floor = [&](int floor, ImU32 col) {
                const int idx = chapter.layer_of_floor(floor);
                if (idx < 0 || idx >= mapdata::kMaxLayers || !g_layer_tex[idx].ready)
                {
                    return;
                }
                const mapdata::Layer& l = (*chapter.layers)[static_cast<std::size_t>(idx)];
                if (!layer_in_view(l, snap, g.half, g.zoom))
                {
                    return;
                }
                draw_image(dl, g_layer_tex[idx], uv_of(l), g, col, cfg.round, x0, y0, side);
            };

            int layers_drawn = 0;
            if (have_floor && g_layers_ready > 0)
            {
                if (cfg.show_adjacent_floors)
                {
                    for (int i = 0; i < g_floor.below_count; ++i)
                    {
                        draw_floor(g_floor.below[i], tint_below);
                    }
                    for (int i = 0; i < g_floor.above_count; ++i)
                    {
                        draw_floor(g_floor.above[i], tint_above);
                    }
                }
                // Dominant ordinal last and brightest, so it wins the overlap.
                for (int i = g_floor.floor_count - 1; i >= 0; --i)
                {
                    draw_floor(g_floor.floors[i], i == 0 ? tint_current : tint_secondary);
                    ++layers_drawn;
                }
            }
            else if (composite_ready)
            {
                // Off the floor grid for longer than floor_fallback_hold_ms and the
                // config asked for the Z-shaded composite.
                draw_image(dl, g_map, uv_of(chapter), g, IM_COL32(255, 255, 255, alpha(1.0f)), cfg.round, x0, y0,
                           side);
            }
            else
            {
                // The default fallback: every layer at once. That is the composite
                // minus its Z shading, and it costs no extra VRAM.
                const int count = chapter.layer_count();
                for (int i = 0; i < count && i < mapdata::kMaxLayers; ++i)
                {
                    draw_floor((*chapter.layers)[static_cast<std::size_t>(i)].floor, tint_all);
                }
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
            (void)layers_drawn;
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
            ImGui::SliderFloat("Floor Z tolerance (uu)", &cfg.floor_z_tolerance, 0.0f, 600.0f, "%.0f");
            ImGui::SliderFloat("Floor hysteresis (uu)", &cfg.floor_hysteresis, 0.0f, 600.0f, "%.0f");
            ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -200.0f, 200.0f, "%.0f");
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
                    // Floor (Z) awareness: which layer is on screen and why.
                    if (g_floor.have)
                    {
                        std::string floors;
                        for (int i = 0; i < g_floor.floor_count; ++i)
                        {
                            floors += (i == 0 ? "" : "+") + std::to_string(g_floor.floors[i]);
                        }
                        ImGui::Text("floor  f%s   band %d/%d   Z %.0f..%.0f   feet Z %.0f (dist %.0f)%s",
                                    floors.c_str(),
                                    g_floor.band_index + 1,
                                    g_floor.bands_in_cell,
                                    static_cast<double>(g_floor.z_min),
                                    static_cast<double>(g_floor.z_max),
                                    snap.z - static_cast<double>(cfg.player_z_offset),
                                    g_floor.distance,
                                    g_floor.stale ? "  HELD (off grid)" : "");
                        ImGui::Text("       below %d layer(s), above %d layer(s)   %d layer texture(s) resident, "
                                    "%llu MB",
                                    g_floor.below_count,
                                    g_floor.above_count,
                                    g_layers_ready,
                                    static_cast<unsigned long long>(g_layer_bytes / (1024 * 1024)));
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.75f, 0.4f, 1.0f},
                                           "floor  no surface band at this cell - drawing the fallback "
                                           "(%d layer texture(s), %llu MB)",
                                           g_layers_ready,
                                           static_cast<unsigned long long>(g_layer_bytes / (1024 * 1024)));
                    }
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
                    ImGui::Text("menu state changed %llu ms ago   %u cached in-viewport root(s)   "
                                "show delay %d ms",
                                static_cast<unsigned long long>(
                                    snap.menu_change_ms == 0 ? 0 : ::GetTickCount64() - snap.menu_change_ms),
                                snap.menu_roots_cached,
                                cfg.menu_close_show_delay_ms);
                    char narrow[256]{};
                    ::WideCharToMultiByte(CP_UTF8, 0, snap.level_name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
                    ImGui::TextWrapped("pawn: %s", narrow);
                }

                ImGui::Spacing();
                char reason[192]{};
                ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr, nullptr);
                ImGui::Text("minimap: %s", reason);
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
                g_floor = FloorPick{};
                mm::log(L"map textures dropped for a reload");
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
