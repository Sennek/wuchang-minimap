//==============================================================================
// overlay_dcomp.cpp - the overlay's own composition surface
//==============================================================================
//
// RENDER THREAD only, like the rest of the D3D12 layer. Owns everything the mod
// draws into when it does not draw into the game's back buffer: a DIRECT queue
// of its own, a swapchain created with CreateSwapChainForComposition, and the
// DirectComposition target and visual that put that swapchain over the game's
// window.
//
// Why it exists. Drawing into the game's back buffer requires permission this
// process cannot ask for: D3D12 has no way to ask a resource whether it may be
// written, so the only test is to write it, and where a frame-generation layer
// owns the buffers that test removes the D3D12 device and hangs the game. Every
// resource here is one this module created, so the question does not arise.
//
// It does NOT create a device. Resources are made on the game's own
// ID3D12Device, which was never the problem - only the buffers of the game's
// swapchain are - and using it keeps ImGui, the map textures and the height
// slicer on the device they are already on.
//
// It must not: touch a UObject, touch the game's swapchain or its queue, or run
// on any thread but the one inside the Present hook.

#include "overlay_internal.hpp"

#include <dcomp.h>

namespace overlay
{
    namespace ovl
    {
        namespace
        {
            // dcomp.dll is looked up rather than imported. A static import would
            // make the whole mod fail to load if it were ever absent, and a
            // silent load-time failure is a failure mode this project has already
            // paid for once.
            using DCompositionCreateDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);

            HMODULE g_dcomp_dll = nullptr;
            IDCompositionDevice* g_comp_device = nullptr;
            IDCompositionTarget* g_comp_target = nullptr;
            IDCompositionVisual* g_comp_visual = nullptr;
            ID3D12CommandQueue* g_comp_queue = nullptr;
            IDXGISwapChain3* g_comp_sc = nullptr;
            UINT g_comp_width = 0;
            UINT g_comp_height = 0;
            bool g_comp_logged = false;

            // The format the visual is composed in. BGRA because that is what a
            // composition swapchain takes everywhere, and premultiplied because
            // that is what ImGui's DX12 blend state already emits.
            constexpr DXGI_FORMAT kCompFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            // Two is enough: this swapchain presents with no vertical sync and
            // carries one overlay, not a game.
            constexpr UINT kCompBuffers = 2;
        } // namespace

        DXGI_FORMAT comp_format()
        {
            return kCompFormat;
        }

        UINT comp_buffer_count()
        {
            return kCompBuffers;
        }

        IDXGISwapChain3* comp_swapchain()
        {
            return g_comp_sc;
        }

        ID3D12CommandQueue* comp_queue()
        {
            return g_comp_queue;
        }

        bool comp_ready()
        {
            return g_comp_sc != nullptr && g_comp_queue != nullptr && g_comp_visual != nullptr;
        }

        void comp_release()
        {
            // Order matters only in that the visual must stop referencing the
            // swapchain before the swapchain goes. Everything here is released on
            // the render thread, which is the only thread allowed to.
            if (g_comp_visual != nullptr)
            {
                g_comp_visual->SetContent(nullptr);
            }
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
            safe_release(g_comp_sc);
            safe_release(g_comp_queue);
            g_comp_width = 0;
            g_comp_height = 0;
            // `g_dcomp_dll` is deliberately not freed: it is a system module that
            // was already resident, and this module may be created again.
        }

        // Creates the queue, the swapchain and the composition chain. `device` is
        // the game's; `hwnd` is the game's window, which belongs to this process,
        // which is what CreateTargetForHwnd requires.
        bool comp_create(ID3D12Device* device, HWND hwnd, UINT width, UINT height)
        {
            if (device == nullptr || hwnd == nullptr || width == 0 || height == 0)
            {
                mm::logf(L"composition: nothing to create from (device {:p}, hwnd 0x{:X}, {}x{})",
                         static_cast<void*>(device),
                         reinterpret_cast<std::uintptr_t>(hwnd),
                         width,
                         height);
                return false;
            }
            if (g_dcomp_dll == nullptr)
            {
                g_dcomp_dll = ::LoadLibraryW(L"dcomp.dll");
            }
            if (g_dcomp_dll == nullptr)
            {
                mm::log(L"composition: dcomp.dll is not available on this system");
                return false;
            }
            auto create_device = reinterpret_cast<DCompositionCreateDeviceFn>(
                reinterpret_cast<void*>(::GetProcAddress(g_dcomp_dll, "DCompositionCreateDevice")));
            if (create_device == nullptr)
            {
                mm::log(L"composition: dcomp.dll exports no DCompositionCreateDevice");
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_comp_queue));
            if (FAILED(hr) || g_comp_queue == nullptr)
            {
                mm::logf(L"composition: the overlay's own command queue could not be created (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }

            IDXGIFactory2* factory = nullptr;
            hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (FAILED(hr) || factory == nullptr)
            {
                mm::logf(L"composition: no IDXGIFactory2 (0x{:08X})", static_cast<unsigned>(hr));
                comp_release();
                return false;
            }

            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = width;
            sd.Height = height;
            sd.Format = kCompFormat;
            sd.Stereo = FALSE;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = kCompBuffers;
            // A composition swapchain takes STRETCH and nothing else.
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            sd.Flags = 0;

            IDXGISwapChain1* sc1 = nullptr;
            hr = factory->CreateSwapChainForComposition(g_comp_queue, &sd, nullptr, &sc1);
            safe_release(factory);
            if (FAILED(hr) || sc1 == nullptr)
            {
                mm::logf(L"composition: CreateSwapChainForComposition failed (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            hr = sc1->QueryInterface(IID_PPV_ARGS(&g_comp_sc));
            safe_release(sc1);
            if (FAILED(hr) || g_comp_sc == nullptr)
            {
                mm::logf(L"composition: the composition swapchain has no IDXGISwapChain3 (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }

            // No IDXGIDevice is passed: a D3D12 device does not expose one, and
            // the parameter is optional - DirectComposition then uses its own.
            hr = create_device(nullptr, __uuidof(IDCompositionDevice),
                               reinterpret_cast<void**>(&g_comp_device));
            if (FAILED(hr) || g_comp_device == nullptr)
            {
                mm::logf(L"composition: DCompositionCreateDevice failed (0x{:08X})",
                         static_cast<unsigned>(hr));
                comp_release();
                return false;
            }
            // Topmost is where an overlay belongs, and the window must belong to this
            // process - it does, because the mod is loaded into the game. A window
            // holds at most one topmost target and one below it, so if something
            // else in this process already took the topmost slot the only honest
            // answer is the other one: drawn under whatever that is, but drawn.
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
            if (FAILED(g_comp_visual->SetContent(g_comp_sc))
                || FAILED(g_comp_target->SetRoot(g_comp_visual))
                || FAILED(g_comp_device->Commit()))
            {
                mm::log(L"composition: the visual could not be attached to the window");
                comp_release();
                return false;
            }

            g_comp_width = width;
            g_comp_height = height;
            if (!g_comp_logged)
            {
                g_comp_logged = true;
                mm::logf(L"composition: the overlay draws into a surface of its own - {}x{} {}, {} "
                         L"buffer(s), premultiplied alpha - composed over the game's window by "
                         L"DirectComposition. The game's own back buffers are never written, which is "
                         L"what makes this work under frame generation and capture layers.",
                         width,
                         height,
                         format_name(kCompFormat),
                         kCompBuffers);
            }
            return true;
        }

        bool comp_resize(UINT width, UINT height)
        {
            if (g_comp_sc == nullptr || width == 0 || height == 0)
            {
                return false;
            }
            if (width == g_comp_width && height == g_comp_height)
            {
                return true;
            }
            const HRESULT hr = g_comp_sc->ResizeBuffers(kCompBuffers, width, height, kCompFormat, 0);
            if (FAILED(hr))
            {
                mm::logf(L"composition: ResizeBuffers to {}x{} failed (0x{:08X})",
                         width,
                         height,
                         static_cast<unsigned>(hr));
                return false;
            }
            g_comp_width = width;
            g_comp_height = height;
            return true;
        }

    } // namespace ovl
} // namespace overlay
