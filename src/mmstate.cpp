#include "mmstate.hpp"

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

using namespace RC;

namespace mm
{
    namespace
    {
        //==============================================================================
        // Spinlock (std::mutex faults on this game's game thread - see lessons.md)
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
        // Snapshot seqlock
        //==============================================================================

        std::atomic<std::uint32_t> g_seq{0};
        Snapshot g_snapshot{};

        //==============================================================================
        // Config
        //==============================================================================

        Spinlock g_cfg_lock;
        Config g_cfg{};

        //==============================================================================
        // Log queue
        //==============================================================================

        Spinlock g_log_lock;
        std::vector<std::wstring> g_log_queue;
        DWORD g_loop_thread = 0;

        //==============================================================================
        // Paths
        //==============================================================================

        std::wstring g_mod_dir;

        std::wstring resolve_mod_dir()
        {
            // main.dll lives in ...\ue4ss\Mods\WuchangMinimap\dlls, so the mod folder is
            // one level up. Same resolution the navmesh dumper uses.
            HMODULE self = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(&resolve_mod_dir),
                                     &self) != 0 &&
                self != nullptr)
            {
                wchar_t buffer[MAX_PATH * 2]{};
                const DWORD n = ::GetModuleFileNameW(self, buffer, static_cast<DWORD>(std::size(buffer)));
                if (n > 0 && static_cast<std::size_t>(n) < std::size(buffer))
                {
                    std::wstring path{buffer};
                    // strip \main.dll
                    auto slash = path.find_last_of(L'\\');
                    if (slash != std::wstring::npos)
                    {
                        path.erase(slash);
                    }
                    // strip \dlls
                    slash = path.find_last_of(L'\\');
                    if (slash != std::wstring::npos)
                    {
                        path.erase(slash);
                    }
                    return path;
                }
            }
            return L".";
        }

        //==============================================================================
        // Plain Win32 text file I/O - no iostreams anywhere in this mod (lessons.md)
        //==============================================================================

        bool read_whole_file(const std::wstring& path, std::string& out)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size) == 0 || size.QuadPart < 0 || size.QuadPart > (64 << 20))
            {
                ::CloseHandle(h);
                return false;
            }
            out.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            const bool ok = out.empty() ||
                            (::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 &&
                             read == out.size());
            ::CloseHandle(h);
            return ok;
        }

        bool write_whole_file(const std::wstring& path, const std::string& data)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD written = 0;
            const bool ok = data.empty() ||
                            (::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0 &&
                             written == data.size());
            ::CloseHandle(h);
            return ok;
        }

        std::string trim(std::string_view v)
        {
            std::size_t a = 0;
            std::size_t b = v.size();
            const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (a < b && space(v[a]))
            {
                ++a;
            }
            while (b > a && space(v[b - 1]))
            {
                --b;
            }
            return std::string{v.substr(a, b - a)};
        }

        bool parse_bool(const std::string& v, bool fallback)
        {
            if (v == "1" || v == "true" || v == "yes" || v == "on")
            {
                return true;
            }
            if (v == "0" || v == "false" || v == "no" || v == "off")
            {
                return false;
            }
            return fallback;
        }

        float parse_float(const std::string& v, float fallback)
        {
            try
            {
                std::size_t used = 0;
                const float f = std::stof(v, &used);
                return used == 0 ? fallback : f;
            }
            catch (...)
            {
                return fallback;
            }
        }

        int parse_int(const std::string& v, int fallback)
        {
            try
            {
                std::size_t used = 0;
                const int n = std::stoi(v, &used);
                return used == 0 ? fallback : n;
            }
            catch (...)
            {
                return fallback;
            }
        }

        // F-key name -> virtual key. F6 / F9..F12 are rejected outright: on this machine
        // F6 is the RenoDX DLSS5 toggle (it ignores modifiers and has already caused one
        // GPU crash), F10 the game console, F11 the engine fullscreen bind and F12 the
        // Steam screenshot key. See lessons.md.
        int vk_from_name(const std::string& name, int fallback, const char* key_label)
        {
            if (name.size() < 2 || (name[0] != 'F' && name[0] != 'f'))
            {
                return fallback;
            }
            int n = 0;
            for (std::size_t i = 1; i < name.size(); ++i)
            {
                if (name[i] < '0' || name[i] > '9')
                {
                    return fallback;
                }
                n = n * 10 + (name[i] - '0');
            }
            if (n < 1 || n > 8 || n == 6)
            {
                logf(L"config: {} = '{}' rejected - only F1..F5, F7, F8 are safe here (F6 is the RenoDX DLSS5 "
                     L"toggle, F9/F11 engine binds, F10 the game console, F12 the Steam screenshot key)",
                     std::wstring(key_label, key_label + std::strlen(key_label)),
                     std::wstring(name.begin(), name.end()));
                return fallback;
            }
            return VK_F1 + (n - 1);
        }

        std::string vk_name(int vk)
        {
            if (vk >= VK_F1 && vk <= VK_F24)
            {
                return "F" + std::to_string(vk - VK_F1 + 1);
            }
            return "F2";
        }

        const char* anchor_name(Anchor a)
        {
            switch (a)
            {
            case Anchor::TopRight:
                return "top-right";
            case Anchor::BottomLeft:
                return "bottom-left";
            case Anchor::BottomRight:
                return "bottom-right";
            case Anchor::TopLeft:
            default:
                return "top-left";
            }
        }

        Anchor anchor_from_name(const std::string& v, Anchor fallback)
        {
            if (v == "top-left")
            {
                return Anchor::TopLeft;
            }
            if (v == "top-right")
            {
                return Anchor::TopRight;
            }
            if (v == "bottom-left")
            {
                return Anchor::BottomLeft;
            }
            if (v == "bottom-right")
            {
                return Anchor::BottomRight;
            }
            return fallback;
        }
    } // namespace

    std::atomic<bool> g_panel_open{false};
    std::atomic<bool> g_reload_config{false};
    std::atomic<bool> g_save_config{false};
    std::atomic<bool> g_panel_drew_frame{false};

    //==================================================================================
    // Snapshot seqlock
    //==================================================================================

    void publish(const Snapshot& snap)
    {
        const std::uint32_t start = g_seq.load(std::memory_order_relaxed);
        g_seq.store(start + 1, std::memory_order_release); // odd => write in progress
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&g_snapshot, &snap, sizeof(Snapshot));
        std::atomic_thread_fence(std::memory_order_release);
        g_seq.store(start + 2, std::memory_order_release);
    }

    bool read_snapshot(Snapshot& out)
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const std::uint32_t before = g_seq.load(std::memory_order_acquire);
            if ((before & 1u) != 0u)
            {
                YieldProcessor();
                continue;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(&out, &g_snapshot, sizeof(Snapshot));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_seq.load(std::memory_order_acquire) == before)
            {
                return before != 0;
            }
        }
        return false;
    }

    //==================================================================================
    // Config
    //==================================================================================

    Config config()
    {
        SpinGuard guard(g_cfg_lock);
        return g_cfg;
    }

    void set_config(const Config& cfg)
    {
        SpinGuard guard(g_cfg_lock);
        g_cfg = cfg;
    }

    std::wstring mod_dir()
    {
        if (g_mod_dir.empty())
        {
            g_mod_dir = resolve_mod_dir();
        }
        return g_mod_dir;
    }

    std::wstring config_path()
    {
        return mod_dir() + L"\\config_wuchang_minimap.txt";
    }

    void load_config_file()
    {
        Config cfg{};
        std::string text;
        const std::wstring path = config_path();
        if (!read_whole_file(path, text))
        {
            logf(L"config: {} not found - using defaults (a file is written when you press Save in the F2 panel)",
                 path);
            set_config(cfg);
            return;
        }

        int lines = 0;
        std::size_t pos = 0;
        // A UTF-8 BOM (PowerShell's Set-Content -Encoding utf8 writes one) would
        // otherwise be glued to the first key's name.
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            pos = 3;
        }
        while (pos <= text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            std::string_view raw = std::string_view{text}.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
            pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

            const std::size_t comment = raw.find_first_of(";#");
            if (comment != std::string_view::npos)
            {
                raw = raw.substr(0, comment);
            }
            const std::size_t eq = raw.find('=');
            if (eq == std::string_view::npos)
            {
                continue;
            }
            const std::string key = trim(raw.substr(0, eq));
            const std::string value = trim(raw.substr(eq + 1));
            if (key.empty())
            {
                continue;
            }
            ++lines;

            if (key == "enabled")
            {
                cfg.enabled = parse_bool(value, cfg.enabled);
            }
            else if (key == "show_minimap")
            {
                cfg.show_minimap = parse_bool(value, cfg.show_minimap);
            }
            else if (key == "minimap_size")
            {
                cfg.size_frac = parse_float(value, cfg.size_frac);
            }
            else if (key == "minimap_zoom")
            {
                cfg.zoom_uu_per_px = parse_float(value, cfg.zoom_uu_per_px);
            }
            else if (key == "minimap_shape")
            {
                cfg.round = (value != "square");
            }
            else if (key == "minimap_anchor")
            {
                cfg.anchor = anchor_from_name(value, cfg.anchor);
            }
            else if (key == "minimap_offset_x")
            {
                cfg.offset_x = parse_float(value, cfg.offset_x);
            }
            else if (key == "minimap_offset_y")
            {
                cfg.offset_y = parse_float(value, cfg.offset_y);
            }
            else if (key == "rotate_with_player")
            {
                cfg.rotate_with_player = parse_bool(value, cfg.rotate_with_player);
            }
            else if (key == "opacity")
            {
                cfg.opacity = parse_float(value, cfg.opacity);
            }
            else if (key == "hide_in_menus")
            {
                cfg.hide_in_menus = parse_bool(value, cfg.hide_in_menus);
            }
            else if (key == "require_pawn_view")
            {
                cfg.require_pawn_view = parse_bool(value, cfg.require_pawn_view);
            }
            else if (key == "state_stale_ms")
            {
                cfg.state_stale_ms = parse_int(value, cfg.state_stale_ms);
            }
            else if (key == "min_visible_after_state_ok_ms")
            {
                cfg.min_visible_after_state_ok_ms = parse_int(value, cfg.min_visible_after_state_ok_ms);
            }
            else if (key == "menu_close_show_delay_ms")
            {
                cfg.menu_close_show_delay_ms = parse_int(value, cfg.menu_close_show_delay_ms);
            }
            else if (key == "show_adjacent_floors")
            {
                cfg.show_adjacent_floors = parse_bool(value, cfg.show_adjacent_floors);
            }
            else if (key == "adjacent_floor_opacity")
            {
                cfg.adjacent_floor_opacity = parse_float(value, cfg.adjacent_floor_opacity);
            }
            else if (key == "floor_z_tolerance")
            {
                cfg.floor_z_tolerance = parse_float(value, cfg.floor_z_tolerance);
            }
            else if (key == "floor_hysteresis")
            {
                cfg.floor_hysteresis = parse_float(value, cfg.floor_hysteresis);
            }
            else if (key == "player_z_offset")
            {
                cfg.player_z_offset = parse_float(value, cfg.player_z_offset);
            }
            else if (key == "floor_fallback_hold_ms")
            {
                cfg.floor_fallback_hold_ms = parse_int(value, cfg.floor_fallback_hold_ms);
            }
            else if (key == "fallback_use_composite")
            {
                cfg.fallback_use_composite = parse_bool(value, cfg.fallback_use_composite);
            }
            else if (key == "debug_readout")
            {
                cfg.debug_readout = parse_bool(value, cfg.debug_readout);
            }
            else if (key == "debug_show_panel_on_start")
            {
                cfg.debug_show_panel_on_start = parse_bool(value, cfg.debug_show_panel_on_start);
            }
            else if (key == "panel_key")
            {
                cfg.panel_key = vk_from_name(value, cfg.panel_key, "panel_key");
            }
            else if (key == "reload_key")
            {
                cfg.reload_key = vk_from_name(value, cfg.reload_key, "reload_key");
            }
        }

        // Clamp everything: a hand-edited file must not be able to produce a 40 000 px
        // minimap or a divide-by-zero zoom.
        cfg.size_frac = (std::max)(0.05f, (std::min)(0.9f, cfg.size_frac));
        cfg.zoom_uu_per_px = (std::max)(2.0f, (std::min)(400.0f, cfg.zoom_uu_per_px));
        cfg.opacity = (std::max)(0.1f, (std::min)(1.0f, cfg.opacity));
        cfg.offset_x = (std::max)(0.0f, (std::min)(4000.0f, cfg.offset_x));
        cfg.offset_y = (std::max)(0.0f, (std::min)(4000.0f, cfg.offset_y));
        cfg.state_stale_ms = (std::max)(100, (std::min)(60000, cfg.state_stale_ms));
        cfg.min_visible_after_state_ok_ms = (std::max)(0, (std::min)(10000, cfg.min_visible_after_state_ok_ms));
        cfg.menu_close_show_delay_ms = (std::max)(0, (std::min)(3000, cfg.menu_close_show_delay_ms));
        cfg.adjacent_floor_opacity = (std::max)(0.0f, (std::min)(1.0f, cfg.adjacent_floor_opacity));
        cfg.floor_z_tolerance = (std::max)(0.0f, (std::min)(2000.0f, cfg.floor_z_tolerance));
        cfg.floor_hysteresis = (std::max)(0.0f, (std::min)(2000.0f, cfg.floor_hysteresis));
        cfg.player_z_offset = (std::max)(-500.0f, (std::min)(500.0f, cfg.player_z_offset));
        cfg.floor_fallback_hold_ms = (std::max)(0, (std::min)(60000, cfg.floor_fallback_hold_ms));

        set_config(cfg);
        logf(L"config: loaded {} setting(s) from {}", lines, path);
    }

    void save_config_file()
    {
        const Config cfg = config();
        std::string out;
        out += "; WuchangMinimap settings. Written by the F2 panel; hand edits are picked up with F5.\n";
        out += "; Hotkeys may only be F1..F5, F7 or F8: F6 is the RenoDX DLSS5 toggle, F9/F11 engine\n";
        out += "; binds, F10 the game console and F12 the Steam screenshot key.\n\n";
        out += "enabled = " + std::string(cfg.enabled ? "1" : "0") + "\n";
        out += "show_minimap = " + std::string(cfg.show_minimap ? "1" : "0") + "\n";
        out += "minimap_size = " + std::format("{:.3f}", cfg.size_frac) + "\n";
        out += "minimap_zoom = " + std::format("{:.1f}", cfg.zoom_uu_per_px) + "\n";
        out += "minimap_shape = " + std::string(cfg.round ? "round" : "square") + "\n";
        out += "minimap_anchor = " + std::string(anchor_name(cfg.anchor)) + "\n";
        out += "minimap_offset_x = " + std::format("{:.0f}", cfg.offset_x) + "\n";
        out += "minimap_offset_y = " + std::format("{:.0f}", cfg.offset_y) + "\n";
        out += "rotate_with_player = " + std::string(cfg.rotate_with_player ? "1" : "0") + "\n";
        out += "opacity = " + std::format("{:.2f}", cfg.opacity) + "\n";
        out += "hide_in_menus = " + std::string(cfg.hide_in_menus ? "1" : "0") + "\n";
        out += "require_pawn_view = " + std::string(cfg.require_pawn_view ? "1" : "0") + "\n";
        out += "state_stale_ms = " + std::to_string(cfg.state_stale_ms) + "\n";
        out += "min_visible_after_state_ok_ms = " + std::to_string(cfg.min_visible_after_state_ok_ms) + "\n";
        out += "menu_close_show_delay_ms = " + std::to_string(cfg.menu_close_show_delay_ms) + "\n";
        out += "\n; Floor (Z) awareness - which pre-rendered floor layer the minimap shows.\n";
        out += "show_adjacent_floors = " + std::string(cfg.show_adjacent_floors ? "1" : "0") + "\n";
        out += "adjacent_floor_opacity = " + std::format("{:.2f}", cfg.adjacent_floor_opacity) + "\n";
        out += "floor_z_tolerance = " + std::format("{:.0f}", cfg.floor_z_tolerance) + "\n";
        out += "floor_hysteresis = " + std::format("{:.0f}", cfg.floor_hysteresis) + "\n";
        out += "player_z_offset = " + std::format("{:.0f}", cfg.player_z_offset) + "\n";
        out += "floor_fallback_hold_ms = " + std::to_string(cfg.floor_fallback_hold_ms) + "\n";
        out += "fallback_use_composite = " + std::string(cfg.fallback_use_composite ? "1" : "0") + "\n\n";
        out += "debug_readout = " + std::string(cfg.debug_readout ? "1" : "0") + "\n";
        out += "debug_show_panel_on_start = " + std::string(cfg.debug_show_panel_on_start ? "1" : "0") + "\n";
        out += "panel_key = " + vk_name(cfg.panel_key) + "\n";
        out += "reload_key = " + vk_name(cfg.reload_key) + "\n";

        const std::wstring path = config_path();
        if (write_whole_file(path, out))
        {
            logf(L"config: saved -> {}", path);
        }
        else
        {
            logf(L"config: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
        }
    }

    //==================================================================================
    // Logging
    //==================================================================================

    void set_loop_thread()
    {
        g_loop_thread = ::GetCurrentThreadId();
    }

    void log(const std::wstring& line)
    {
        if (g_loop_thread == 0 || ::GetCurrentThreadId() != g_loop_thread)
        {
            SpinGuard guard(g_log_lock);
            if (g_log_queue.size() < 4096)
            {
                g_log_queue.push_back(line);
            }
            return;
        }
        Output::send<LogLevel::Verbose>(STR("[minimap] {}\n"), line);
    }

    void drain_log()
    {
        std::vector<std::wstring> lines;
        {
            SpinGuard guard(g_log_lock);
            if (g_log_queue.empty())
            {
                return;
            }
            lines.swap(g_log_queue);
        }
        for (const std::wstring& l : lines)
        {
            Output::send<LogLevel::Verbose>(STR("[minimap] {}\n"), l);
        }
    }
} // namespace mm
