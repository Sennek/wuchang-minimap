#include "overlay.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstddef>
#include <format>
#include <string_view>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <MinHook.h>

// imgui_impl_win32.h deliberately hides this behind `#if 0` so the header does not
// depend on <windows.h>; the backend expects you to copy the declaration yourself.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay
{
    namespace
    {
        // Taking the address of one symbol per third-party translation unit is what
        // forces the linker to pull imgui_impl_dx12.obj / imgui_impl_win32.obj /
        // hook.obj out of their static libraries. Without a reference the objects
        // compile but get dropped, so "it links" would prove nothing.
        //
        // ImGui_ImplDX12_Init is overloaded (the modern InitInfo form and the legacy
        // one), hence the explicitly typed pointers rather than a plain address-of.
        using Dx12InitFn = bool (*)(ImGui_ImplDX12_InitInfo*);
        using Dx12NewFrameFn = void (*)();
        using Dx12RenderFn = void (*)(ImDrawData*, ID3D12GraphicsCommandList*);
        using Win32InitFn = bool (*)(void*);
        using Win32NewFrameFn = void (*)();
        using Win32WndProcFn = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);

        const void* const g_entry_points[] = {
            reinterpret_cast<const void*>(static_cast<Dx12InitFn>(&ImGui_ImplDX12_Init)),
            reinterpret_cast<const void*>(static_cast<Dx12NewFrameFn>(&ImGui_ImplDX12_NewFrame)),
            reinterpret_cast<const void*>(static_cast<Dx12RenderFn>(&ImGui_ImplDX12_RenderDrawData)),
            reinterpret_cast<const void*>(static_cast<Win32InitFn>(&ImGui_ImplWin32_Init)),
            reinterpret_cast<const void*>(static_cast<Win32NewFrameFn>(&ImGui_ImplWin32_NewFrame)),
            reinterpret_cast<const void*>(static_cast<Win32WndProcFn>(&ImGui_ImplWin32_WndProcHandler)),
            reinterpret_cast<const void*>(&MH_Initialize),
            reinterpret_cast<const void*>(&MH_CreateHook),
            reinterpret_cast<const void*>(&MH_EnableHook),
            reinterpret_cast<const void*>(&MH_Uninitialize),
        };

        auto widen(std::string_view narrow) -> RC::StringType
        {
            return RC::StringType{narrow.begin(), narrow.end()};
        }
    } // namespace

    auto selftest() -> RC::StringType
    {
        // MH_StatusToString is side-effect free - it does not initialise MinHook.
        const auto mh_status = widen(MH_StatusToString(MH_ERROR_NOT_INITIALIZED));

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
                           mh_status,
                           linked,
                           std::size(g_entry_points));
    }
} // namespace overlay
