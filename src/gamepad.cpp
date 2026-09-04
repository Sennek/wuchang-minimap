#include "gamepad.hpp"

#include <Windows.h>

#include <atomic>
#include <cmath>

namespace pad
{
    namespace
    {
        // Fixed ABI, redeclared so the mod neither links nor needs the XInput SDK.
        struct XiGamepad
        {
            WORD wButtons;
            BYTE bLeftTrigger;
            BYTE bRightTrigger;
            SHORT sThumbLX;
            SHORT sThumbLY;
            SHORT sThumbRX;
            SHORT sThumbRY;
        };

        struct XiState
        {
            DWORD dwPacketNumber;
            XiGamepad Gamepad;
        };

        using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XiState*);

        HMODULE g_module = nullptr;
        XInputGetStateFn g_get_state = nullptr;
        const wchar_t* g_module_name = L"not loaded";
        bool g_tried = false;

        // Loop thread only.
        int g_slot = -1;                 // the pad we are following, or -1
        std::uint64_t g_next_probe_ms = 0; // when a disconnected slot may be re-probed
        std::uint16_t g_prev_held = 0;

        // Published to every other thread, individually lock-free: a torn read across
        // two axes costs one frame of a slightly wrong pan vector.
        std::atomic<bool> g_connected{false};
        std::atomic<float> g_lx{0.0f};
        std::atomic<float> g_ly{0.0f};
        std::atomic<float> g_rx{0.0f};
        std::atomic<float> g_ry{0.0f};
        std::atomic<float> g_lt{0.0f};
        std::atomic<float> g_rt{0.0f};
        std::atomic<std::uint32_t> g_held{0};
        std::atomic<std::uint32_t> g_pressed{0};

        bool load()
        {
            if (g_tried)
            {
                return g_get_state != nullptr;
            }
            g_tried = true;
            static const wchar_t* const kNames[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
            for (const wchar_t* name : kNames)
            {
                HMODULE mod = ::GetModuleHandleW(name);
                if (mod == nullptr)
                {
                    mod = ::LoadLibraryW(name);
                }
                if (mod == nullptr)
                {
                    continue;
                }
                const auto fn = reinterpret_cast<XInputGetStateFn>(
                    reinterpret_cast<void*>(::GetProcAddress(mod, "XInputGetState")));
                if (fn == nullptr)
                {
                    continue;
                }
                g_module = mod;
                g_get_state = fn;
                g_module_name = name;
                return true;
            }
            return false;
        }

        // Radial deadzone: magnitude is tested and rescaled, so a diagonal push is not
        // clipped to a square and a stick just past the deadzone starts from 0.
        void apply_deadzone(float x, float y, float dz, float& ox, float& oy)
        {
            const float mag = std::sqrt(x * x + y * y);
            if (mag <= dz || mag <= 0.0001f)
            {
                ox = 0.0f;
                oy = 0.0f;
                return;
            }
            const float scaled = (mag - dz) / (1.0f - dz);
            const float clamped = scaled > 1.0f ? 1.0f : scaled;
            ox = x / mag * clamped;
            oy = y / mag * clamped;
        }

        float axis(SHORT v)
        {
            // -32768 has no positive twin; clamping at -32767 keeps the range symmetric.
            const float f = static_cast<float>(v < -32767 ? -32767 : v) / 32767.0f;
            return f;
        }

        void publish_off()
        {
            g_connected.store(false, std::memory_order_relaxed);
            g_lx.store(0.0f, std::memory_order_relaxed);
            g_ly.store(0.0f, std::memory_order_relaxed);
            g_rx.store(0.0f, std::memory_order_relaxed);
            g_ry.store(0.0f, std::memory_order_relaxed);
            g_lt.store(0.0f, std::memory_order_relaxed);
            g_rt.store(0.0f, std::memory_order_relaxed);
            g_held.store(0, std::memory_order_relaxed);
            g_prev_held = 0;
        }
    } // namespace

    void poll(bool enabled, float deadzone)
    {
        if (!enabled)
        {
            if (g_connected.load(std::memory_order_relaxed))
            {
                publish_off();
            }
            return;
        }
        if (!load())
        {
            publish_off();
            return;
        }

        const float dz = deadzone < 0.05f ? 0.05f : (deadzone > 0.6f ? 0.6f : deadzone);
        const std::uint64_t now = ::GetTickCount64();

        XiState st{};
        int slot = g_slot;
        DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
        if (slot >= 0)
        {
            rc = g_get_state(static_cast<DWORD>(slot), &st);
            if (rc != ERROR_SUCCESS)
            {
                slot = -1;
                g_slot = -1;
                g_next_probe_ms = now + 1000;
            }
        }
        if (slot < 0)
        {
            // Probing every slot is expensive with nothing plugged in: once a second.
            if (now < g_next_probe_ms)
            {
                publish_off();
                return;
            }
            g_next_probe_ms = now + 1000;
            for (int i = 0; i < 4; ++i)
            {
                XiState probe{};
                if (g_get_state(static_cast<DWORD>(i), &probe) == ERROR_SUCCESS)
                {
                    g_slot = i;
                    st = probe;
                    rc = ERROR_SUCCESS;
                    // A fresh slot starts from what it is holding, not from zero:
                    // otherwise buttons already down at hot-plug arrive as rising edges.
                    g_prev_held = probe.Gamepad.wButtons;
                    g_pressed.store(0, std::memory_order_release);
                    break;
                }
            }
            if (g_slot < 0)
            {
                publish_off();
                return;
            }
        }
        if (rc != ERROR_SUCCESS)
        {
            publish_off();
            return;
        }

        float lx = 0.0f;
        float ly = 0.0f;
        float rx = 0.0f;
        float ry = 0.0f;
        apply_deadzone(axis(st.Gamepad.sThumbLX), axis(st.Gamepad.sThumbLY), dz, lx, ly);
        apply_deadzone(axis(st.Gamepad.sThumbRX), axis(st.Gamepad.sThumbRY), dz, rx, ry);

        const std::uint16_t held = st.Gamepad.wButtons;
        const std::uint16_t edges = static_cast<std::uint16_t>(held & ~g_prev_held);
        g_prev_held = held;

        g_connected.store(true, std::memory_order_relaxed);
        g_lx.store(lx, std::memory_order_relaxed);
        g_ly.store(ly, std::memory_order_relaxed);
        g_rx.store(rx, std::memory_order_relaxed);
        g_ry.store(ry, std::memory_order_relaxed);
        g_lt.store(static_cast<float>(st.Gamepad.bLeftTrigger) / 255.0f, std::memory_order_relaxed);
        g_rt.store(static_cast<float>(st.Gamepad.bRightTrigger) / 255.0f, std::memory_order_relaxed);
        g_held.store(held, std::memory_order_relaxed);
        if (edges != 0)
        {
            g_pressed.fetch_or(edges, std::memory_order_release);
        }
    }

    State state()
    {
        State s{};
        s.connected = g_connected.load(std::memory_order_relaxed);
        s.lx = g_lx.load(std::memory_order_relaxed);
        s.ly = g_ly.load(std::memory_order_relaxed);
        s.rx = g_rx.load(std::memory_order_relaxed);
        s.ry = g_ry.load(std::memory_order_relaxed);
        s.lt = g_lt.load(std::memory_order_relaxed);
        s.rt = g_rt.load(std::memory_order_relaxed);
        s.held = static_cast<std::uint16_t>(g_held.load(std::memory_order_relaxed));
        return s;
    }

    std::uint16_t take_pressed()
    {
        return static_cast<std::uint16_t>(g_pressed.exchange(0, std::memory_order_acq_rel));
    }

    void clear_pressed()
    {
        g_pressed.store(0, std::memory_order_release);
    }

    const wchar_t* module_name()
    {
        return g_module_name;
    }
} // namespace pad
