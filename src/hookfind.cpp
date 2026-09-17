#include "hookfind.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "mem.hpp"
#include "mmstate.hpp"
#include "ptrwalk.hpp"
#include "ue_min.hpp"

//
// GAME THREAD. See hookfind.hpp for what this module is for.
//
// The walk starts at the one `GameViewportClient` in the process and crosses
// UGameViewportClient -> FViewport -> FRHIViewport (FD3D12Viewport) -> IDXGISwapChain.
// None of those three hops is a reflected property, so none of them is a number written
// down here: `pw::search` visits the pointers inside each object and this file judges
// what it finds. An object is followed when its first word is a vtable inside some
// loaded module, which is what makes it a C++ object at all, and it is accepted when
// that vtable belongs to a module other than the game's own image AND the object
// answers QueryInterface for IDXGISwapChain1 AND hands out a D3D12 back buffer AND
// presents into a window of this process.
//
// The "not the game's image" gate is what keeps a call off the engine's own objects:
// UE's classes live in the executable and would be called through slot 0 as if they
// were IUnknown. Everything that survives the gate came out of dxgi.dll, d3d12.dll,
// an injector's proxy or a wrapper of theirs, where slot 0 really is QueryInterface -
// and the call is made through mem::guarded_call even then.
//

namespace hf
{
    namespace
    {
        std::atomic<State> g_state{State::Searching};
        std::atomic<void*> g_present{nullptr};
        std::atomic<void*> g_resize{nullptr};
        std::atomic<void*> g_present1{nullptr};
        std::atomic<const void*> g_swapchain{nullptr};
        // Read by the loop thread's own timeout; see hf::Progress.
        std::atomic<int> g_attempts{0};
        std::atomic<int> g_candidates{0};
        std::atomic<int> g_objects{0};
        std::atomic<bool> g_root_seen{false};
        std::atomic<bool> g_budget_hit{false};

        // Game thread only.
        pw::Arena g_arena{};
        std::uint64_t g_first_try_ms = 0;
        std::uint64_t g_last_try_ms = 0;
        bool g_no_root_logged = false;

        // How long the engine is given to have a viewport with a swapchain in it. The
        // pump starts before the first frame is presented, so the first tries are
        // expected to find nothing.
        constexpr std::uint64_t kDeadlineMs = 30000;
        constexpr std::uint64_t kRetryMs = 500;

        // IDXGISwapChain1 has 29 methods - IUnknown 3, IDXGIObject 4, IDXGIDeviceSubObject
        // 1, IDXGISwapChain 10, IDXGISwapChain1 11. Every one of them must be a readable
        // address inside the vtable's own module before anything is called through it.
        constexpr int kSlotPresent = 8;
        constexpr int kSlotResize = 13;
        constexpr int kSlotPresent1 = 22;
        constexpr int kSlotCount = 29;

        // The loaded images, sorted by base address. The walk asks "which module owns
        // this address?" thousands of times per attempt, and GetModuleHandleExW takes the
        // loader lock every time - on the game thread, thousands of times a second, that
        // is not a question to ask the operating system. The list is taken once per
        // attempt and searched here.
        class ModuleMap
        {
        public:
            static constexpr int kCap = 512;

            bool build()
            {
                count_ = 0;
                exe_ = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
                const HANDLE snap =
                    ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
                if (snap == INVALID_HANDLE_VALUE)
                {
                    return false;
                }
                MODULEENTRY32W me{};
                me.dwSize = sizeof(me);
                for (BOOL ok = ::Module32FirstW(snap, &me); ok && count_ < kCap;
                     ok = ::Module32NextW(snap, &me))
                {
                    const auto base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
                    image_[count_].base = base;
                    image_[count_].end = base + me.modBaseSize;
                    ++count_;
                }
                ::CloseHandle(snap);
                // Insertion sort: a couple of hundred entries, already almost ordered.
                for (int i = 1; i < count_; ++i)
                {
                    const Image v = image_[i];
                    int j = i - 1;
                    for (; j >= 0 && image_[j].base > v.base; --j)
                    {
                        image_[j + 1] = image_[j];
                    }
                    image_[j + 1] = v;
                }
                return count_ > 0;
            }

            // The base of the image holding `p`, or 0.
            std::uintptr_t owner(const void* p) const
            {
                const auto v = reinterpret_cast<std::uintptr_t>(p);
                int lo = 0;
                int hi = count_ - 1;
                while (lo <= hi)
                {
                    const int mid = (lo + hi) / 2;
                    if (v < image_[mid].base)
                    {
                        hi = mid - 1;
                    }
                    else if (v >= image_[mid].end)
                    {
                        lo = mid + 1;
                    }
                    else
                    {
                        return image_[mid].base;
                    }
                }
                return 0;
            }

            std::uintptr_t exe() const { return exe_; }

        private:
            struct Image
            {
                std::uintptr_t base;
                std::uintptr_t end;
            };

            Image image_[kCap]{};
            int count_ = 0;
            std::uintptr_t exe_ = 0;
        };

        ModuleMap g_modules;

        // What a candidate turned out to be. Filled inside the guarded call, read after.
        struct ComProbe
        {
            unsigned width = 0;
            unsigned height = 0;
            unsigned buffers = 0;
            void* hwnd = nullptr;
            bool is_swapchain = false;
            bool is_d3d12 = false;
        };

        // Runs inside mem::guarded_call, so: no C++ objects with destructors, no
        // exceptions, nothing that needs unwinding (MSVC C2712).
        void probe_com(void* candidate, void* out, void*)
        {
            auto* r = static_cast<ComProbe*>(out);
            auto* unk = static_cast<IUnknown*>(candidate);

            IDXGISwapChain1* sc = nullptr;
            if (FAILED(unk->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc))) ||
                sc == nullptr)
            {
                return;
            }
            r->is_swapchain = true;

            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(sc->GetDesc(&desc)))
            {
                r->width = desc.BufferDesc.Width;
                r->height = desc.BufferDesc.Height;
                r->buffers = desc.BufferCount;
                r->hwnd = desc.OutputWindow;
            }

            // The same question the Present-time election asks, and the one that rejects
            // the game's 144x8 D3D11 decoy chain: does it hand out a D3D12 back buffer?
            ID3D12Resource* buffer = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D12Resource),
                                        reinterpret_cast<void**>(&buffer))) &&
                buffer != nullptr)
            {
                r->is_d3d12 = true;
                buffer->Release();
            }
            sc->Release();
        }

        bool window_is_ours(void* hwnd)
        {
            if (hwnd == nullptr || ::IsWindow(static_cast<HWND>(hwnd)) == 0)
            {
                return false;
            }
            DWORD pid = 0;
            ::GetWindowThreadProcessId(static_cast<HWND>(hwnd), &pid);
            return pid == ::GetCurrentProcessId();
        }

        pw::Verdict judge(const void* candidate, ComProbe& hit)
        {
            void* vtable = nullptr;
            if (!mem::read_ptr(candidate, vtable))
            {
                return pw::Verdict::Skip;
            }
            const std::uintptr_t home = g_modules.owner(vtable);
            if (home == 0)
            {
                // No vtable in any image: plain data, not an object worth crossing.
                return pw::Verdict::Skip;
            }
            if (home == g_modules.exe())
            {
                // An engine object - the viewport, the RHI viewport, and everything else
                // UE holds. These are the hops, never the destination.
                return pw::Verdict::Follow;
            }

            // A foreign vtable. Before calling through it, insist that it has the shape
            // of an IDXGISwapChain1 vtable: 23 readable code addresses, all at home.
            for (int i = 0; i < kSlotCount; ++i)
            {
                void* slot = nullptr;
                if (!mem::read_at<void*>(vtable, static_cast<std::size_t>(i) * sizeof(void*), slot) ||
                    g_modules.owner(slot) != home)
                {
                    return pw::Verdict::Follow;
                }
            }

            ComProbe probe{};
            if (!mem::guarded_call(&probe_com, const_cast<void*>(candidate), &probe, nullptr))
            {
                return pw::Verdict::Follow;
            }
            if (!probe.is_swapchain || !probe.is_d3d12 || !window_is_ours(probe.hwnd) || probe.width == 0)
            {
                return pw::Verdict::Follow;
            }

            hit = probe;
            return pw::Verdict::Accept;
        }

        bool publish(const void* swapchain)
        {
            void* vtable = nullptr;
            if (!mem::read_ptr(swapchain, vtable))
            {
                return false;
            }
            void* slots[3] = {nullptr, nullptr, nullptr};
            const int wanted[3] = {kSlotPresent, kSlotResize, kSlotPresent1};
            for (int i = 0; i < 3; ++i)
            {
                const std::size_t at = static_cast<std::size_t>(wanted[i]) * sizeof(void*);
                if (!mem::read_at<void*>(vtable, at, slots[i]) || g_modules.owner(slots[i]) == 0)
                {
                    return false;
                }
            }

            g_present.store(slots[0], std::memory_order_relaxed);
            g_resize.store(slots[1], std::memory_order_relaxed);
            g_present1.store(slots[2], std::memory_order_relaxed);
            g_swapchain.store(swapchain, std::memory_order_relaxed);
            g_state.store(State::Found, std::memory_order_release);
            return true;
        }
    } // namespace

    void tick()
    {
        if (g_state.load(std::memory_order_relaxed) != State::Searching)
        {
            return;
        }

        const std::uint64_t now = ::GetTickCount64();
        if (g_first_try_ms == 0)
        {
            g_first_try_ms = now;
        }
        else if (now - g_last_try_ms < kRetryMs)
        {
            return;
        }
        g_last_try_ms = now;

        RC::Unreal::UObject* root = RC::Unreal::UObjectGlobals::FindFirstOf(L"GameViewportClient");
        if (root == nullptr)
        {
            g_attempts.fetch_add(1, std::memory_order_relaxed);
            if (!g_no_root_logged)
            {
                g_no_root_logged = true;
                MM_LOGV(L"hook discovery: no GameViewportClient yet - the engine has no viewport to "
                        L"read a swapchain from; retrying every {} ms",
                        kRetryMs);
            }
        }
        else
        {
            // One module list per attempt: the judge asks which image owns an address
            // once per candidate, and the answer may not cost a loader lock each time.
            if (!g_modules.build())
            {
                return;
            }
            g_attempts.fetch_add(1, std::memory_order_relaxed);
            g_root_seen.store(true, std::memory_order_relaxed);
            ComProbe hit{};
            const pw::Limits limits{4, 0x400, 3000};
            const pw::Result found = pw::search(
                root,
                limits,
                [](const void* addr, void* out, std::size_t bytes)
                { return mem::readable(addr, bytes) && mem::copy(addr, out, bytes); },
                [&hit](const void* candidate) { return judge(candidate, hit); },
                g_arena);
            g_candidates.store(found.nodes, std::memory_order_relaxed);
            g_objects.store(found.follows, std::memory_order_relaxed);
            g_budget_hit.store(found.budget_hit, std::memory_order_relaxed);

            if (found.object != nullptr && publish(found.object))
            {
                mm::logf(L"hook discovery: the engine's own swapchain {} was found {} hop(s) from the "
                         L"GameViewportClient ({} candidate(s) judged, {} object(s) crossed): {}x{} x{} "
                         L"buffers on hwnd {:p}, D3D12 confirmed. Nothing was created in this process - "
                         L"no window, no device, no queue, no factory, no swapchain.",
                         found.object,
                         found.depth,
                         found.nodes,
                         found.follows,
                         hit.width,
                         hit.height,
                         hit.buffers,
                         hit.hwnd);
                return;
            }
            MM_LOGV(L"hook discovery: no swapchain under the GameViewportClient yet ({} candidate(s), "
                    L"{} object(s), budget {})",
                    found.nodes,
                    found.follows,
                    found.budget_hit ? L"spent" : L"left");
        }

        if (now - g_first_try_ms >= kDeadlineMs)
        {
            g_state.store(State::GaveUp, std::memory_order_release);
            mm::logf(L"hook discovery: {} s and {} search(es) later the engine's swapchain was still not "
                     L"found. A GameViewportClient {}; the last walk judged {} candidate(s) over {} "
                     L"object(s) and {} the node budget. The addresses have to come from somewhere else.",
                     kDeadlineMs / 1000,
                     g_attempts.load(std::memory_order_relaxed),
                     g_root_seen.load(std::memory_order_relaxed) ? L"was found" : L"was NEVER found",
                     g_candidates.load(std::memory_order_relaxed),
                     g_objects.load(std::memory_order_relaxed),
                     g_budget_hit.load(std::memory_order_relaxed) ? L"spent" : L"stayed inside");
        }
    }

    State state()
    {
        return g_state.load(std::memory_order_acquire);
    }

    Progress progress()
    {
        Progress p{};
        p.attempts = g_attempts.load(std::memory_order_relaxed);
        p.candidates = g_candidates.load(std::memory_order_relaxed);
        p.objects = g_objects.load(std::memory_order_relaxed);
        p.root_seen = g_root_seen.load(std::memory_order_relaxed);
        p.budget_hit = g_budget_hit.load(std::memory_order_relaxed);
        return p;
    }

    bool addresses(Addresses& out)
    {
        if (g_state.load(std::memory_order_acquire) != State::Found)
        {
            return false;
        }
        out.present = g_present.load(std::memory_order_relaxed);
        out.resize = g_resize.load(std::memory_order_relaxed);
        out.present1 = g_present1.load(std::memory_order_relaxed);
        out.swapchain = g_swapchain.load(std::memory_order_relaxed);
        return out.present != nullptr;
    }
} // namespace hf
