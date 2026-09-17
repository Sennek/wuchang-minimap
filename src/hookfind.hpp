#pragma once

//
// hookfind - where the three DXGI hook addresses come from. GAME THREAD.
//
// The overlay hooks IDXGISwapChain::Present / ResizeBuffers / Present1 by address, and
// the addresses are three slots of a swapchain's vtable. This module reads them off the
// swapchain the engine already owns, so the mod creates no window, no D3D12 device, no
// command queue, no DXGI factory and no swapchain of its own: an injector that has
// hooked the factory never sees a call of ours to classify.
//
// It runs on the game thread, from the ProcessEvent pump, because it starts at a UObject.
// The answer is published through atomics and the loop thread installs the hooks.
//

#include <cstdint>

namespace hf
{
    enum class State
    {
        Searching, // no answer yet; tick() is still trying
        Found,     // addresses() answers
        GaveUp     // the deadline passed; the caller decides what to do instead
    };

    struct Addresses
    {
        void* present = nullptr;
        void* resize = nullptr;
        void* present1 = nullptr;
        const void* swapchain = nullptr; // for the log line only; never dereferenced again
    };

    // GAME THREAD, from the ProcessEvent pump. Cheap once it has answered.
    void tick();

    // Any thread.
    State state();
    bool addresses(Addresses& out);
} // namespace hf
