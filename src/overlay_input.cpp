//
// overlay_input - the WndProc hook, the hotkey swallow and the message replay.
//
// The game thread owns the window, so its messages are recorded under g_msg_lock and
// replayed on the render thread just before ImGui builds a frame.
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // [fix-ui] HOTKEY SWALLOW (review B.13)
        //==============================================================================
        //
        // A key bound to a mod action used to reach the game as well: the loop thread
        // samples it with GetAsyncKeyState and the WndProc hook let the message through,
        // so `M` opened the full map AND did whatever `M` does in the game. The async
        // key state is kernel-side and cannot be denied to a game that polls it
        // (lessons.md) - but the WINDOW MESSAGE can be, and that is how UE reads its
        // keyboard here.
        //
        // The decision has to cost one atomic read, because it runs on the window thread
        // for every key message. So the LOOP thread - the only place that knows which
        // bindings exist, which of them are live at this instant and whether their
        // modifier is held - publishes a 256-bit set of virtual keys, and this tests a
        // bit. publish_swallow_set(), in the hotkey block, is the other half.
        //
        // WHEN THE SET WAS LAST PUBLISHED. The loop thread refreshes it on every 60 Hz

        void swallow_set_clear()
        {
            for (std::atomic<std::uint32_t>& w : g_swallow_bits)
            {
                w.store(0, std::memory_order_relaxed);
            }
        }

        void swallow_set_add(int vk)
        {
            if (vk > 0 && vk < 256)
            {
                g_swallow_bits[vk >> 5].fetch_or(1u << (static_cast<unsigned>(vk) & 31u),
                                                 std::memory_order_relaxed);
            }
        }

        // Key DOWN / UP only. WM_CHAR carries a character rather than a virtual key, so
        // testing it against a VK would be a coincidence, and nothing here reads text.
        bool is_hotkey_message(UINT msg)
        {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        }

        bool hotkey_swallow(WPARAM wparam)
        {
            const unsigned vk = static_cast<unsigned>(wparam);
            if (vk == 0 || vk >= 256)
            {
                return false;
            }
            const std::uint64_t stamp = g_swallow_stamp.load(std::memory_order_relaxed);
            if (stamp == 0 || ::GetTickCount64() - stamp > kSwallowStaleMs)
            {
                return false;
            }
            return (g_swallow_bits[vk >> 5].load(std::memory_order_relaxed) & (1u << (vk & 31u))) != 0;
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

        // ESCAPE, as a WINDOW MESSAGE. WM_CHAR carries the control character (0x1B), the
        // key messages carry the virtual key - two different numbers that happen to be
        // the same one here, which is worth spelling out rather than relying on.
        bool is_escape_message(UINT msg, WPARAM wparam)
        {
            if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
            {
                return wparam == VK_ESCAPE;
            }
            if (msg == WM_CHAR || msg == WM_SYSCHAR)
            {
                return wparam == 0x1B;
            }
            return false;
        }

        bool is_escape_key_down(UINT msg, WPARAM wparam)
        {
            return (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wparam == VK_ESCAPE;
        }

        // THE TWO CHORDS THE OVERLAY MUST NEVER EAT. ALT+F4 closes the game and ALT+ENTER
        // toggles fullscreen, and both arrive as WM_SYSKEYDOWN / WM_SYSKEYUP (WM_SYSCHAR
        // for the character half) - which `is_keyboard_message` matches, so with the full
        // map open the map branch was returning 1 for them. The consequence was not just
        // "alt+F4 does nothing": WM_CLOSE never arrived, so `crumb::mark_closing()` never
        // ran and the NEXT launch reported the clean exit as a crash.
        bool is_system_chord(UINT msg, WPARAM wparam)
        {
            if (msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP && msg != WM_SYSCHAR)
            {
                return false;
            }
            return wparam == VK_F4 || wparam == VK_RETURN;
        }

        // RAW INPUT. UE reads the mouse through WM_INPUT, not only through WM_MOUSEMOVE,
        // so swallowing the window messages alone still lets the camera turn under an
        // open overlay. One RID_HEADER read says which device a message came from, which
        // is what lets the panel take the mouse and leave the keyboard with the game.
        //

        RawKind raw_kind(UINT msg, LPARAM lparam, bool want_key)
        {
            RawKind out{};
            if (msg != WM_INPUT)
            {
                return out;
            }
            RAWINPUTHEADER hdr{};
            UINT size = sizeof(hdr);
            const UINT got = ::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_HEADER, &hdr, &size,
                                               sizeof(RAWINPUTHEADER));
            if (got != sizeof(RAWINPUTHEADER))
            {
                return out;
            }
            out.mouse = hdr.dwType == RIM_TYPEMOUSE;
            out.keyboard = hdr.dwType == RIM_TYPEKEYBOARD;
            if (out.keyboard && want_key)
            {
                RAWINPUT ri{};
                UINT rsize = sizeof(ri);
                if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &ri, &rsize,
                                      sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                    ri.header.dwType == RIM_TYPEKEYBOARD)
                {
                    out.escape = ri.data.keyboard.VKey == VK_ESCAPE;
                }
            }
            return out;
        }

        bool is_raw_mouse_message(UINT msg, LPARAM lparam)
        {
            return raw_kind(msg, lparam, false).mouse;
        }

        //==============================================================================
        // The game thread's window messages, REPLAYED on the render thread
        //==============================================================================
        //
        // WHY THIS EXISTS AT ALL. `hooked_wndproc` runs on the thread that owns the
        // game's window - the GAME thread - and it used to hand every message straight
        // to `ImGui_ImplWin32_WndProcHandler`. That function mutates the Dear ImGui
        // context: `io.AddKeyEvent` / `AddMousePosEvent` / `AddMouseButtonEvent` all
        // push onto `ImGuiContext::InputEventsQueue`, which is an `ImVector` - a raw
        // pointer, a Size and a Capacity, grown with a realloc and no synchronisation
        // whatsoever. Meanwhile the RENDER thread is inside `ImGui::NewFrame()`, whose
        // `UpdateInputEvents` READS that queue and then `resize(0)`s it, and inside
        // `build_ui()` / `ImGui::Render()`, which read and write the rest of the same
        // context.
        //
        // So two threads were growing and clearing one ImVector. The failure that
        // follows is not a torn read: `push_back` on a stale `Data` pointer writes into
        // a block the other thread has just freed, and a `Size++` that races a
        // `resize(0)` writes one element PAST the capacity. Both land in the CRT heap,
        // and a corrupted free list is a hard, dumpless hang - every thread that then
        // allocates blocks inside the heap lock for ever. That is the exact shape of the
        // 2026-09-03 20:56 freeze: the render thread stopped within a second, the game
        // thread with it, no crash dump, no exception, and the loop thread lived just
        // long enough to flush a log buffer that needed no allocation.
        //
        // THE FIX IS THE ONLY CORRECT ONE: exactly one thread may touch the ImGui
        // context, and that thread is the one that renders. The WndProc hook now only
        // RECORDS the message into a fixed-size ring - no allocation, no ImGui call, a
        // few instructions under a spinlock the render thread holds only for the length
        // of a memcpy - and `replay_imgui_messages()` feeds them to the backend at the
        // top of the frame, before `ImGui_ImplWin32_NewFrame()`. The swallow decision
        // stays on the game thread and no longer reads the context: `WantCaptureKeyboard`
        // is published to an atomic once per frame.
        //
        // WHAT THIS COSTS. Three things in the backend's handler are thread-affine and
        // now answer differently, all of them cosmetic:
        //   * `::GetMessageExtraInfo()` is per-thread and only meaningful while that
        //     thread is dispatching, so every mouse event is reported as a MOUSE rather
        //     than as a pen or a touch. This mod has no pen or touch behaviour.
        //   * `::SetCapture()` / `::GetCapture()` fail on a window owned by another
        //     thread, so a drag that leaves the client area stops being tracked. The
        //     game runs fullscreen and the panel is drawn inside it, so the cursor
        //     cannot leave the client area in the first place.
        //   * `::TrackMouseEvent()` may refuse, in which case WM_MOUSELEAVE never
        //     arrives - but the mouse position keeps coming from the replayed
        //     WM_MOUSEMOVE messages, which is where it comes from today.
        // A one-frame delay on input is the other cost, and it is not observable: the
        // messages are replayed in order, in the same frame the game thread's own
        // dispatch would have been drawn.




        // Does the backend's handler do anything with this message? Recording only what
        // it handles keeps the ring from filling with WM_TIMER / WM_PAINT traffic. The
        // list is `ImGui_ImplWin32_WndProcHandlerEx`'s switch, verbatim.
        bool imgui_handles(UINT msg)
        {
            switch (msg)
            {
            case WM_MOUSEMOVE:
            case WM_NCMOUSEMOVE:
            case WM_MOUSELEAVE:
            case WM_NCMOUSELEAVE:
            case WM_DESTROY:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            case WM_XBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
            case WM_INPUTLANGCHANGE:
            case WM_CHAR:
            case WM_IME_COMPOSITION:
            case WM_IME_CHAR:
            case WM_SETCURSOR:
            case WM_DEVICECHANGE:
                return true;
            default:
                return false;
            }
        }

        // GAME THREAD (the window's owner). Nothing but a bounds check and a 24-byte
        // copy under a spinlock that is never held across anything that can block.
        void record_imgui_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            if (!imgui_handles(msg))
            {
                return;
            }
            spin::SpinGuard guard(g_msg_lock);
            if (g_msg_count >= kMsgRing)
            {
                g_msg_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_msg_ring[(g_msg_head + g_msg_count) % kMsgRing] = PendingMsg{hwnd, msg, wparam, lparam};
            ++g_msg_count;
        }

        // RENDER THREAD, at the top of the frame and BEFORE ImGui_ImplWin32_NewFrame.
        // Bounded by the count read at entry, so a game thread that keeps posting
        // cannot keep this loop alive.
        void replay_imgui_messages()
        {
            int budget = 0;
            {
                spin::SpinGuard guard(g_msg_lock);
                budget = g_msg_count;
            }
            for (int i = 0; i < budget; ++i)
            {
                PendingMsg m{};
                {
                    spin::SpinGuard guard(g_msg_lock);
                    if (g_msg_count == 0)
                    {
                        break;
                    }
                    m = g_msg_ring[g_msg_head];
                    g_msg_head = (g_msg_head + 1) % kMsgRing;
                    --g_msg_count;
                }
                ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wparam, m.lparam);
            }
        }

        LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            // ALT+F4 / the close button: write the terminal crash-breadcrumb stage while
            // there is still a process to write it from. None of the mod's teardown paths
            // run on this route (see the comment on DllMain in dllmain.cpp), so without
            // this every ALT+F4 made the NEXT launch report a crash. Idempotent, and it
            // never swallows the message.
            if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT)
            {
                crumb::mark_closing();
            }
            if (g_imgui_ready)
            {
                // RECORD ONLY. The ImGui context belongs to the render thread; see the
                // comment on record_imgui_message above for what happened when this
                // line called into the backend from here.
                record_imgui_message(hwnd, msg, wparam, lparam);

                // Input is only ever taken away from the game while the F2 panel or
                // the full map is up. With both closed the minimap is a pure overlay
                // and every message goes straight through, so gameplay input is
                // untouched - and because the test is a plain read of the two flags,
                // closing either one hands the input back on the very next message.
                // NOTHING IS LATCHED HERE (lessons.md).
                // ALT+F4 and ALT+ENTER go to the game whatever is open on top of it.
                const bool sys_chord = is_system_chord(msg, wparam);
                if (sys_chord)
                {
                    // Nothing is swallowed and nothing is closed: the game decides. The
                    // message was recorded for ImGui above, which is harmless - the
                    // backend only turns it into a key event.
                }
                else if (mm::g_map_open.load(std::memory_order_relaxed))
                {
                    // The map owns the whole keyboard and mouse: WASD pans it, and a
                    // click on a marker must not also swing the camera. io.WantCapture*
                    // is not enough - it is only true over an ImGui window, and the
                    // canvas deliberately reads raw keys rather than focusing a widget.
                    // The map's own toggle key is sampled with GetAsyncKeyState on the
                    // loop thread, so it still closes the map from here.
                    // ESC. The map already swallows every key, so the game's pause menu
                    // never saw it; what was missing is the other half - the key that a
                    // player expects to CLOSE a full-screen overlay. Closing is a plain
                    // store on the same flag the swallow condition reads, so the input is
                    // back on the very next message (lessons.md: nothing latched here).
                    if (is_escape_key_down(msg, wparam))
                    {
                        mm::g_map_open.store(false);
                        MM_LOGVS(L"full map closed (Esc)");
                    }
                    if (is_mouse_message(msg) || is_keyboard_message(msg) || msg == WM_INPUT)
                    {
                        return 1;
                    }
                }
                else if (mm::g_panel_open.load(std::memory_order_relaxed))
                {
                    // THE PANEL OWNS THE MOUSE, ALL OF IT. io.WantCaptureMouse is only
                    // true over an ImGui window, so with it as the gate every drag that
                    // started a pixel outside the panel turned the game camera while the
                    // player was reading the settings. Raw mouse input is swallowed for
                    // the same reason. The keyboard still goes to the game except while
                    // ImGui wants it (a text field), so the panel key - sampled with
                    // GetAsyncKeyState on the loop thread - always closes it again.
                    // Published by the render thread at the end of its frame - never
                    // read off the context from this thread (see record_imgui_message).
                    const bool want_keys = g_imgui_want_keyboard.load(std::memory_order_relaxed);
                    const bool capturing = mm::g_key_capture.load(std::memory_order_relaxed);

                    // ESC CLOSES THE PANEL, AND THE GAME MUST NOT SEE IT. Without this
                    // the one key everybody presses to dismiss a settings window opened
                    // the game's pause menu on top of it. Esc is therefore swallowed in
                    // all four shapes it can arrive in - WM_KEYDOWN / WM_KEYUP / WM_CHAR
                    // and a raw-input keyboard packet - and the key-DOWN closes the panel.
                    //
                    // While a binding capture is armed, Esc keeps its existing meaning
                    // (cancel the capture, handled on the render thread) and the panel
                    // stays open; the capture already swallows the whole keyboard.
                    const RawKind raw = raw_kind(msg, lparam, true);
                    const bool esc = is_escape_message(msg, wparam) || raw.escape;
                    if (esc && !capturing && is_escape_key_down(msg, wparam))
                    {
                        mm::g_panel_open.store(false);
                        MM_LOGVS(L"settings panel closed (Esc)");
                    }
                    if (is_mouse_message(msg) || msg == WM_SETCURSOR || raw.mouse || esc ||
                        ((want_keys || capturing) && is_keyboard_message(msg)) ||
                        (capturing && msg == WM_INPUT))
                    {
                        return 1;
                    }
                }
                // [fix-ui] hotkey swallow - one atomic read; see g_swallow_bits above.
                if (is_hotkey_message(msg) && hotkey_swallow(wparam))
                {
                    return 1;
                }
            }
            const WNDPROC prev = g_prev_wndproc.load(std::memory_order_acquire);
            if (prev == nullptr)
            {
                return ::DefWindowProcW(hwnd, msg, wparam, lparam);
            }
            return ::CallWindowProcW(prev, hwnd, msg, wparam, lparam);
        }

        // INSTALL. Refuses to hook a window that is ALREADY ours: after a re-adoption
        // (a lost device, a replaced swapchain) `ensure_initialised` runs again, and
        // saving our own proc as the "previous" one would make `hooked_wndproc` call
        // itself for ever on the first message.
        void hook_wndproc()
        {
            if (g_hwnd == nullptr)
            {
                return;
            }
            const auto current = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
            if (current == &hooked_wndproc)
            {
                mm::log(L"the window proc is still ours from an earlier init - not hooking it twice");
                return;
            }
            const auto prev = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hooked_wndproc)));
            g_prev_wndproc.store(prev, std::memory_order_release);
            if (prev == nullptr)
            {
                mm::logf(L"SetWindowLongPtr(GWLP_WNDPROC) failed (error {}) - the F2 panel will get no mouse input",
                         static_cast<unsigned>(::GetLastError()));
            }
        }

        // REMOVE, but only if we are still the outermost proc. Restoring the saved
        // pointer unconditionally UNINSTALLS whoever subclassed the window after us
        // (ReShade, the Steam overlay), which is somebody else's overlay disappearing
        // with no diagnostic. If we are no longer outermost the chain is left exactly as
        // it is: `hooked_wndproc` keeps working because it does nothing at all once
        // `g_imgui_ready` is false, and the saved pointer is deliberately kept so a call
        // still inside it has something to chain to.
        void unhook_wndproc()
        {
            const WNDPROC prev = g_prev_wndproc.load(std::memory_order_acquire);
            if (g_hwnd == nullptr || prev == nullptr)
            {
                return;
            }
            const auto current = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
            if (current != &hooked_wndproc)
            {
                mm::log(L"window proc: somebody else subclassed the window after us, so ours is left in "
                        L"the chain (removing it would uninstall theirs)");
                return;
            }
            ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
        }
    } // namespace ovl
} // namespace overlay
