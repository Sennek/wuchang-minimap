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
        // HOTKEY SWALLOW
        //==============================================================================
        //
        // UE reads its keyboard from window messages, so a key bound to a mod action is denied to
        // the game here (GetAsyncKeyState is kernel-side and cannot be). The loop thread publishes
        // a 256-bit set of virtual keys - it alone knows the live bindings and their modifiers -
        // and this tests one bit. The other half is publish_swallow_set() in the hotkey block.

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

        // Key DOWN / UP only. WM_CHAR carries a character, not a virtual key.
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

        // Escape as a window message: WM_CHAR carries the control character 0x1B, the
        // key messages carry VK_ESCAPE.
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

        // The two chords the overlay must never eat: ALT+F4 (closes the game, and its WM_CLOSE is
        // what drives crumb::mark_closing()) and ALT+ENTER (fullscreen). Both arrive as
        // WM_SYSKEYDOWN / WM_SYSKEYUP / WM_SYSCHAR, which is_keyboard_message() matches.
        bool is_system_chord(UINT msg, WPARAM wparam)
        {
            if (msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP && msg != WM_SYSCHAR)
            {
                return false;
            }
            return wparam == VK_F4 || wparam == VK_RETURN;
        }

        // UE reads the mouse through WM_INPUT and WM_MOUSEMOVE, so the camera turns under an open
        // overlay unless raw input is swallowed too. One RID_HEADER read says which device a
        // message came from, so the panel can take the mouse and leave the keyboard with the game.

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
        // ONLY THE RENDER THREAD MAY TOUCH THE IMGUI CONTEXT. The backend's WndProc handler
        // pushes onto `ImGuiContext::InputEventsQueue`, an unsynchronised ImVector that
        // `ImGui::NewFrame()` reads and resizes - two threads growing and clearing it corrupt the
        // CRT heap and hang the process, with no dump. The hook only RECORDS into a fixed ring (no
        // allocation, no ImGui call, a memcpy under a spinlock); `replay_imgui_messages()` feeds
        // the backend at the top of the frame, before `ImGui_ImplWin32_NewFrame()`. The swallow
        // decision stays on the game thread, reading an atomic copy of `WantCaptureKeyboard`
        // published each frame.
        //
        // Three thread-affine behaviours of the backend answer differently, all cosmetic:
        // `::GetMessageExtraInfo()` reports every mouse event as a mouse (no pen/touch);
        // `::SetCapture()` / `::GetCapture()` fail across threads, so a drag leaving the client
        // area is untracked (the game is fullscreen); `::TrackMouseEvent()` may refuse, dropping
        // WM_MOUSELEAVE. Input lags by one frame, replayed in order.

        // What the backend's handler acts on: recording only those keeps the ring clear of WM_TIMER
        // / WM_PAINT traffic. It is `ImGui_ImplWin32_WndProcHandlerEx`'s switch, verbatim.
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

        // Game thread (the window's owner): a bounds check and a 24-byte copy under a
        // spinlock that is never held across anything that can block.
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

        // Render thread, at the top of the frame, before ImGui_ImplWin32_NewFrame. Bounded by
        // the count read at entry, so a posting game thread cannot keep this loop alive.
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
            // ALT+F4 / the close button: the terminal crash-breadcrumb stage, written while a
            // process still exists to write it from. Idempotent; never swallows the message.
            if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT)
            {
                crumb::mark_closing();
            }
            if (g_imgui_ready)
            {
                // Record only - the ImGui context belongs to the render thread.
                record_imgui_message(hwnd, msg, wparam, lparam);

                // Input is taken only while the F2 panel or the full map is up; with both closed
                // every message passes through. The test is a plain read of the two flags, so
                // closing either restores input on the next message; nothing is latched.
                const bool sys_chord = is_system_chord(msg, wparam);
                if (sys_chord)
                {
                    // ALT+F4 / ALT+ENTER: the game decides. Nothing swallowed, nothing
                    // closed.
                }
                else if (mm::g_map_open.load(std::memory_order_relaxed))
                {
                    // The map owns the whole keyboard and mouse: WASD pans it, and a marker click
                    // must not also swing the camera. io.WantCapture* is only true over an ImGui
                    // window, and the canvas reads raw keys rather than focusing a widget. The
                    // map's toggle key comes from GetAsyncKeyState on the loop thread, so it still
                    // closes the map. Esc closes it too, by a store on the flag the swallow
                    // condition reads - input is back on the next message.
                    if (is_escape_key_down(msg, wparam) &&
                        !g_map_search_active.load(std::memory_order_relaxed))
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
                    // The panel owns the whole mouse, raw input included - io.WantCaptureMouse is
                    // only true over an ImGui window, which lets a drag starting a pixel outside
                    // it turn the game camera. The keyboard goes to the game except while ImGui
                    // wants it (a text field), so the panel key from the loop thread always closes
                    // it. want_keys is published by the render thread at the end of its frame; the
                    // context is never read from this thread.
                    const bool want_keys = g_imgui_want_keyboard.load(std::memory_order_relaxed);
                    const bool capturing = mm::g_key_capture.load(std::memory_order_relaxed);

                    // Esc closes the panel and the game must not see it, so it is swallowed in all
                    // four shapes it arrives in - WM_KEYDOWN / WM_KEYUP / WM_CHAR and a raw-input
                    // keyboard packet - and the key-down closes. While a binding capture is armed,
                    // Esc cancels the capture (on the render thread) and the panel stays open.
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
                // Hotkey swallow - one atomic read; see g_swallow_bits above.
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

        // Refuses to hook a window already ours: `ensure_initialised` re-runs after re-adoption,
        // and saving our own proc as "previous" would make `hooked_wndproc` recurse for ever.
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

        // Removes the hook only while we are the outermost proc - restoring the saved pointer over
        // a later subclass (ReShade, the Steam overlay) would uninstall theirs. Otherwise the
        // chain is left as it is: `hooked_wndproc` does nothing once `g_imgui_ready` is false, and
        // the saved pointer is kept so a call still inside it has something to chain to.
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
