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
        // Waypoint
        //==============================================================================

        Spinlock g_wp_lock;
        mv::Waypoint g_wp{};

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

        // Named keys beyond the F-keys / letters / digits. The modifiers are here for
        // the x-ray highlight, which is a HOLD binding and therefore wants a key the
        // hand is already resting near: LALT is the shipped default. Both the
        // side-specific codes and the "either side" ones are offered, because
        // GetAsyncKeyState answers for either with VK_MENU / VK_SHIFT / VK_CONTROL and
        // some players will want that.
        struct NamedKey
        {
            const char* name;
            int vk;
        };

        constexpr NamedKey kNamedKeys[] = {
            {"TAB", VK_TAB},         {"SPACE", VK_SPACE},   {"LALT", VK_LMENU},
            {"RALT", VK_RMENU},      {"ALT", VK_MENU},      {"LSHIFT", VK_LSHIFT},
            {"RSHIFT", VK_RSHIFT},   {"SHIFT", VK_SHIFT},   {"LCTRL", VK_LCONTROL},
            {"RCTRL", VK_RCONTROL},  {"CTRL", VK_CONTROL},
        };

        std::string upper(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (const char c : s)
            {
                out.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
            }
            return out;
        }

        // Hotkey name -> virtual key.
        //
        // Accepted: F1..F5, F7, F8; a single letter A..Z or digit 0..9; and the named
        // keys above. F6 and F9..F12 are REJECTED outright rather than trusted to
        // whoever edits the file: on this machine F6 is the RenoDX DLSS5 toggle (it
        // ignores modifiers and has already caused one GPU crash), F10 the game console,
        // F11 the engine fullscreen bind and F12 the Steam screenshot key. See
        // lessons.md.
        int vk_from_name(const std::string& name, int fallback, const char* key_label)
        {
            const std::wstring label(key_label, key_label + std::strlen(key_label));
            const std::wstring shown(name.begin(), name.end());

            const auto reject = [&](const wchar_t* why) {
                logf(L"config: {} = '{}' rejected ({}) - keeping the default. Allowed: F1..F5, F7, F8, "
                     L"a single letter or digit, TAB, SPACE, or L/R ALT / SHIFT / CTRL.",
                     label,
                     shown,
                     std::wstring{why});
                return fallback;
            };

            if (name.empty())
            {
                return fallback;
            }

            {
                const std::string up = upper(name);
                for (const NamedKey& k : kNamedKeys)
                {
                    if (up == k.name)
                    {
                        return k.vk;
                    }
                }
            }

            // F-keys first, so the single letter "F" cannot swallow "F6".
            if ((name[0] == 'F' || name[0] == 'f') && name.size() >= 2)
            {
                int n = 0;
                for (std::size_t i = 1; i < name.size(); ++i)
                {
                    if (name[i] < '0' || name[i] > '9')
                    {
                        return reject(L"not a recognised key name");
                    }
                    n = n * 10 + (name[i] - '0');
                }
                if (n < 1 || n > 8 || n == 6)
                {
                    return reject(L"F6 is the RenoDX DLSS5 toggle, F9/F11 engine binds, F10 the game "
                                  L"console and F12 the Steam screenshot key");
                }
                return VK_F1 + (n - 1);
            }

            if (name.size() == 1)
            {
                const char c = name[0];
                if (c >= 'a' && c <= 'z')
                {
                    return static_cast<int>(c - 'a' + 'A');
                }
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                {
                    return static_cast<int>(c);
                }
                return reject(L"not a letter or a digit");
            }

            return reject(L"not a recognised key name");
        }

        std::string vk_name(int vk)
        {
            if (vk >= VK_F1 && vk <= VK_F24)
            {
                return "F" + std::to_string(vk - VK_F1 + 1);
            }
            for (const NamedKey& k : kNamedKeys)
            {
                if (vk == k.vk)
                {
                    return k.name;
                }
            }
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            {
                return std::string(1, static_cast<char>(vk));
            }
            return "F2";
        }

        //==============================================================================
        // The gamepad chord (the highlight's second route)
        //==============================================================================
        //
        // "LB+RB", "A", "LT+RT", "none". The face / shoulder / dpad buttons are bits in
        // the XInput mask; the two triggers are analogue and are carried as flags. The
        // names are the ones on the pad, not XInput's XINPUT_GAMEPAD_* spelling.

        struct PadButton
        {
            const char* name;
            std::uint16_t bit;
        };

        constexpr PadButton kPadButtons[] = {
            {"LB", 0x0100},    {"RB", 0x0200},    {"A", 0x1000},    {"B", 0x2000},
            {"X", 0x4000},     {"Y", 0x8000},     {"BACK", 0x0020}, {"START", 0x0010},
            {"LS", 0x0040},    {"RS", 0x0080},    {"UP", 0x0001},   {"DOWN", 0x0002},
            {"LEFT", 0x0004},  {"RIGHT", 0x0008},
        };

        void parse_pad_chord(const std::string& value, std::uint16_t& mask, bool& lt, bool& rt)
        {
            std::uint16_t new_mask = 0;
            bool new_lt = false;
            bool new_rt = false;
            std::string token;
            std::string rejected;
            const auto flush = [&]() {
                if (token.empty())
                {
                    return;
                }
                const std::string up = upper(token);
                token.clear();
                if (up == "NONE" || up == "OFF")
                {
                    return;
                }
                if (up == "LT")
                {
                    new_lt = true;
                    return;
                }
                if (up == "RT")
                {
                    new_rt = true;
                    return;
                }
                for (const PadButton& b : kPadButtons)
                {
                    if (up == b.name)
                    {
                        new_mask = static_cast<std::uint16_t>(new_mask | b.bit);
                        return;
                    }
                }
                if (!rejected.empty())
                {
                    rejected += ",";
                }
                rejected += up;
            };
            for (const char c : value)
            {
                if (c == '+' || c == ',' || c == ' ' || c == '\t')
                {
                    flush();
                }
                else
                {
                    token.push_back(c);
                }
            }
            flush();
            if (!rejected.empty())
            {
                logf(L"config: highlight_pad_chord - unknown button(s) '{}' ignored. Known: LB, RB, LT, "
                     L"RT, A, B, X, Y, BACK, START, LS, RS, UP, DOWN, LEFT, RIGHT (join with +).",
                     std::wstring(rejected.begin(), rejected.end()));
            }
            mask = new_mask;
            lt = new_lt;
            rt = new_rt;
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
    std::atomic<bool> g_map_open{false};
    std::atomic<bool> g_reload_config{false};
    std::atomic<bool> g_save_config{false};
    std::atomic<bool> g_panel_drew_frame{false};
    std::atomic<bool> g_waypoint_dirty{false};

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

    std::wstring key_name(int vk)
    {
        const std::string n = vk_name(vk);
        return std::wstring(n.begin(), n.end());
    }

    std::wstring pad_chord_name(std::uint16_t mask, bool lt, bool rt)
    {
        std::string out;
        const auto add = [&out](const char* n) {
            if (!out.empty())
            {
                out += "+";
            }
            out += n;
        };
        if (lt)
        {
            add("LT");
        }
        if (rt)
        {
            add("RT");
        }
        for (const PadButton& b : kPadButtons)
        {
            if ((mask & b.bit) != 0)
            {
                add(b.name);
            }
        }
        if (out.empty())
        {
            out = "none";
        }
        return std::wstring(out.begin(), out.end());
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
            else if (key == "floor_fade_uu")
            {
                cfg.floor_fade_uu = parse_float(value, cfg.floor_fade_uu);
            }
            else if (key == "floor_gradient_strength")
            {
                cfg.floor_gradient_strength = parse_float(value, cfg.floor_gradient_strength);
            }
            else if (key == "floor_base_color")
            {
                // "R G B" or "R,G,B", 0..255 each.
                float rgb[3] = {cfg.floor_base_r, cfg.floor_base_g, cfg.floor_base_b};
                std::string one;
                int n = 0;
                for (std::size_t i = 0; i <= value.size() && n < 3; ++i)
                {
                    const char c = i < value.size() ? value[i] : ',';
                    if (c == ',' || c == ' ' || c == '\t' || c == ';')
                    {
                        if (!one.empty())
                        {
                            rgb[n] = parse_float(one, rgb[n]);
                            ++n;
                            one.clear();
                        }
                    }
                    else
                    {
                        one.push_back(c);
                    }
                }
                cfg.floor_base_r = rgb[0];
                cfg.floor_base_g = rgb[1];
                cfg.floor_base_b = rgb[2];
            }
            else if (key == "slice_hz")
            {
                cfg.slice_hz = parse_int(value, cfg.slice_hz);
            }
            else if (key == "feet_z_smooth_ms")
            {
                cfg.feet_z_smooth_ms = parse_int(value, cfg.feet_z_smooth_ms);
            }
            else if (key == "player_z_offset")
            {
                cfg.player_z_offset = parse_float(value, cfg.player_z_offset);
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
            else if (key == "map_key")
            {
                cfg.map_key = vk_from_name(value, cfg.map_key, "map_key");
            }
            else if (key == "markers_enabled")
            {
                cfg.markers_enabled = parse_bool(value, cfg.markers_enabled);
            }
            else if (key == "markers_live")
            {
                cfg.markers_live = parse_bool(value, cfg.markers_live);
            }
            else if (key == "markers_filter_chapter")
            {
                cfg.markers_filter_chapter = parse_bool(value, cfg.markers_filter_chapter);
            }
            else if (key == "markers_rounds_per_sec")
            {
                cfg.markers_rounds_per_sec = parse_int(value, cfg.markers_rounds_per_sec);
            }
            else if (key == "markers_scan_chunk")
            {
                cfg.markers_scan_chunk = parse_int(value, cfg.markers_scan_chunk);
            }
            else if (key == "markers_scan_period_ms")
            {
                cfg.markers_scan_period_ms = parse_int(value, cfg.markers_scan_period_ms);
            }
            else if (key == "markers_categories")
            {
                std::string rejected;
                cfg.markers_categories = mdb::parse_category_mask(value, cfg.markers_categories, &rejected);
                if (!rejected.empty())
                {
                    const std::string known = mdb::format_category_mask(mdb::kAllCats);
                    logf(L"config: markers_categories - unknown name(s) '{}' ignored. Known: {}",
                         std::wstring(rejected.begin(), rejected.end()),
                         std::wstring(known.begin(), known.end()));
                }
            }
            else if (key == "markers_hide_found")
            {
                cfg.markers_hide_found = parse_bool(value, cfg.markers_hide_found);
            }
            else if (key == "markers_found_alpha")
            {
                cfg.markers_found_alpha = parse_float(value, cfg.markers_found_alpha);
            }
            else if (key == "markers_size")
            {
                cfg.markers_size = parse_float(value, cfg.markers_size);
            }
            else if (key == "markers_clamp_to_edge")
            {
                cfg.markers_clamp_to_edge = parse_bool(value, cfg.markers_clamp_to_edge);
            }
            else if (key == "markers_max_draw")
            {
                cfg.markers_max_draw = parse_int(value, cfg.markers_max_draw);
            }
            else if (key == "map_recenter_key")
            {
                cfg.map_recenter_key = vk_from_name(value, cfg.map_recenter_key, "map_recenter_key");
            }
            else if (key == "map_zoom")
            {
                cfg.map_zoom = parse_float(value, cfg.map_zoom);
            }
            else if (key == "map_zoom_min")
            {
                cfg.map_zoom_min = parse_float(value, cfg.map_zoom_min);
            }
            else if (key == "map_zoom_max")
            {
                cfg.map_zoom_max = parse_float(value, cfg.map_zoom_max);
            }
            else if (key == "map_zoom_factor")
            {
                cfg.map_zoom_factor = parse_float(value, cfg.map_zoom_factor);
            }
            else if (key == "map_pan_speed")
            {
                cfg.map_pan_speed = parse_float(value, cfg.map_pan_speed);
            }
            else if (key == "map_margin")
            {
                cfg.map_margin = parse_float(value, cfg.map_margin);
            }
            else if (key == "map_backdrop")
            {
                cfg.map_backdrop = parse_float(value, cfg.map_backdrop);
            }
            else if (key == "map_marker_size")
            {
                cfg.map_marker_size = parse_float(value, cfg.map_marker_size);
            }
            else if (key == "map_markers_max_draw")
            {
                cfg.map_markers_max_draw = parse_int(value, cfg.map_markers_max_draw);
            }
            else if (key == "map_floor_step")
            {
                cfg.map_floor_step = parse_float(value, cfg.map_floor_step);
            }
            else if (key == "map_show_all_floors")
            {
                cfg.map_show_all_floors = parse_bool(value, cfg.map_show_all_floors);
            }
            else if (key == "map_slice_px")
            {
                cfg.map_slice_px = parse_int(value, cfg.map_slice_px);
            }
            else if (key == "map_slice_hz")
            {
                cfg.map_slice_hz = parse_int(value, cfg.map_slice_hz);
            }
            else if (key == "map_gamepad")
            {
                cfg.map_gamepad = parse_bool(value, cfg.map_gamepad);
            }
            else if (key == "map_gamepad_deadzone")
            {
                cfg.map_gamepad_deadzone = parse_float(value, cfg.map_gamepad_deadzone);
            }
            else if (key == "map_waypoint_persist")
            {
                cfg.map_waypoint_persist = parse_bool(value, cfg.map_waypoint_persist);
            }
            else if (key == "highlight_enabled")
            {
                cfg.highlight_enabled = parse_bool(value, cfg.highlight_enabled);
            }
            else if (key == "highlight_key")
            {
                cfg.highlight_key = vk_from_name(value, cfg.highlight_key, "highlight_key");
            }
            else if (key == "highlight_gamepad")
            {
                cfg.highlight_gamepad = parse_bool(value, cfg.highlight_gamepad);
            }
            else if (key == "highlight_pad_chord")
            {
                parse_pad_chord(value, cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt);
            }
            else if (key == "highlight_radius")
            {
                cfg.highlight_radius = parse_float(value, cfg.highlight_radius);
            }
            else if (key == "highlight_categories")
            {
                std::string rejected;
                cfg.highlight_categories =
                    mdb::parse_category_mask(value, cfg.highlight_categories, &rejected);
                if (!rejected.empty())
                {
                    logf(L"config: highlight_categories - unknown name(s) '{}' ignored",
                         std::wstring(rejected.begin(), rejected.end()));
                }
            }
            else if (key == "highlight_show_found")
            {
                cfg.highlight_show_found = parse_bool(value, cfg.highlight_show_found);
            }
            else if (key == "highlight_max_draw")
            {
                cfg.highlight_max_draw = parse_int(value, cfg.highlight_max_draw);
            }
            else if (key == "highlight_alpha_near")
            {
                cfg.highlight_alpha_near = parse_float(value, cfg.highlight_alpha_near);
            }
            else if (key == "highlight_alpha_far")
            {
                cfg.highlight_alpha_far = parse_float(value, cfg.highlight_alpha_far);
            }
            else if (key == "highlight_size")
            {
                cfg.highlight_size = parse_float(value, cfg.highlight_size);
            }
            else if (key == "highlight_labels")
            {
                cfg.highlight_labels = parse_bool(value, cfg.highlight_labels);
            }
            else if (key == "highlight_edge_arrows")
            {
                cfg.highlight_edge_arrows = parse_bool(value, cfg.highlight_edge_arrows);
            }
            else if (key == "highlight_camera_hz")
            {
                cfg.highlight_camera_hz = parse_int(value, cfg.highlight_camera_hz);
            }
            else if (key == "compass_enabled")
            {
                cfg.compass_enabled = parse_bool(value, cfg.compass_enabled);
            }
            else if (key == "compass_width")
            {
                cfg.compass_width = parse_float(value, cfg.compass_width);
            }
            else if (key == "compass_offset_y")
            {
                cfg.compass_offset_y = parse_float(value, cfg.compass_offset_y);
            }
            else if (key == "compass_height")
            {
                cfg.compass_height = parse_float(value, cfg.compass_height);
            }
            else if (key == "compass_span_deg")
            {
                cfg.compass_span_deg = parse_float(value, cfg.compass_span_deg);
            }
            else if (key == "compass_opacity")
            {
                cfg.compass_opacity = parse_float(value, cfg.compass_opacity);
            }
            else if (key == "compass_categories")
            {
                std::string rejected;
                cfg.compass_categories = mdb::parse_category_mask(value, cfg.compass_categories, &rejected);
                if (!rejected.empty())
                {
                    logf(L"config: compass_categories - unknown name(s) '{}' ignored",
                         std::wstring(rejected.begin(), rejected.end()));
                }
            }
            else if (key == "compass_marker_distance")
            {
                cfg.compass_marker_distance = parse_float(value, cfg.compass_marker_distance);
            }
            else if (key == "compass_show_waypoint")
            {
                cfg.compass_show_waypoint = parse_bool(value, cfg.compass_show_waypoint);
            }
            else if (key == "found_tracker")
            {
                cfg.found_tracker = parse_bool(value, cfg.found_tracker);
            }
            else if (key == "found_save_debounce_ms")
            {
                cfg.found_save_debounce_ms = parse_int(value, cfg.found_save_debounce_ms);
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
        cfg.floor_z_tolerance = (std::max)(10.0f, (std::min)(2000.0f, cfg.floor_z_tolerance));
        cfg.floor_fade_uu = (std::max)(cfg.floor_z_tolerance, (std::min)(20000.0f, cfg.floor_fade_uu));
        cfg.floor_gradient_strength = (std::max)(0.0f, (std::min)(1.0f, cfg.floor_gradient_strength));
        cfg.floor_base_r = (std::max)(0.0f, (std::min)(255.0f, cfg.floor_base_r));
        cfg.floor_base_g = (std::max)(0.0f, (std::min)(255.0f, cfg.floor_base_g));
        cfg.floor_base_b = (std::max)(0.0f, (std::min)(255.0f, cfg.floor_base_b));
        cfg.slice_hz = (std::max)(2, (std::min)(30, cfg.slice_hz));
        cfg.feet_z_smooth_ms = (std::max)(1, (std::min)(2000, cfg.feet_z_smooth_ms));
        cfg.player_z_offset = (std::max)(-500.0f, (std::min)(500.0f, cfg.player_z_offset));
        cfg.markers_rounds_per_sec = (std::max)(1, (std::min)(10, cfg.markers_rounds_per_sec));
        cfg.markers_scan_chunk = scan::clamp_chunk(cfg.markers_scan_chunk);
        cfg.markers_scan_period_ms = scan::clamp_period_ms(cfg.markers_scan_period_ms);
        cfg.markers_found_alpha = (std::max)(0.0f, (std::min)(1.0f, cfg.markers_found_alpha));
        cfg.markers_size = (std::max)(2.0f, (std::min)(24.0f, cfg.markers_size));
        cfg.markers_max_draw = (std::max)(0, (std::min)(4000, cfg.markers_max_draw));
        cfg.found_save_debounce_ms = (std::max)(200, (std::min)(60000, cfg.found_save_debounce_ms));
        // The full map. Same hand-edit discipline as everything above: without a clamp
        // a typo could ask for a 1 uu/px view of a 500 m chapter, a zero-size slice
        // texture, or a zoom factor of 1.0 (which never changes the zoom at all).
        cfg.map_zoom_min = (std::max)(1.0f, (std::min)(2000.0f, cfg.map_zoom_min));
        cfg.map_zoom_max = (std::max)(cfg.map_zoom_min, (std::min)(4000.0f, cfg.map_zoom_max));
        cfg.map_zoom = (std::max)(cfg.map_zoom_min, (std::min)(cfg.map_zoom_max, cfg.map_zoom));
        cfg.map_zoom_factor = (std::max)(1.01f, (std::min)(2.0f, cfg.map_zoom_factor));
        cfg.map_pan_speed = (std::max)(50.0f, (std::min)(6000.0f, cfg.map_pan_speed));
        cfg.map_margin = (std::max)(0.0f, (std::min)(0.3f, cfg.map_margin));
        cfg.map_backdrop = (std::max)(0.0f, (std::min)(1.0f, cfg.map_backdrop));
        cfg.map_marker_size = (std::max)(2.0f, (std::min)(32.0f, cfg.map_marker_size));
        cfg.map_markers_max_draw = (std::max)(0, (std::min)(20000, cfg.map_markers_max_draw));
        cfg.map_floor_step = (std::max)(10.0f, (std::min)(5000.0f, cfg.map_floor_step));
        cfg.map_slice_px = (std::max)(128, (std::min)(2048, cfg.map_slice_px));
        cfg.map_slice_hz = (std::max)(1, (std::min)(30, cfg.map_slice_hz));
        cfg.map_gamepad_deadzone = (std::max)(0.05f, (std::min)(0.6f, cfg.map_gamepad_deadzone));
        cfg.markers_categories &= mdb::kAllCats;
        // The highlight and the compass. Same discipline: a hand-edited radius of 1e9
        // would ask the projector for every marker in the game, and a zero-degree span
        // would divide by zero on the strip.
        cfg.highlight_radius = (std::max)(200.0f, (std::min)(50000.0f, cfg.highlight_radius));
        cfg.highlight_categories &= mdb::kAllCats;
        cfg.highlight_max_draw = (std::max)(1, (std::min)(400, cfg.highlight_max_draw));
        cfg.highlight_alpha_near = (std::max)(0.05f, (std::min)(1.0f, cfg.highlight_alpha_near));
        cfg.highlight_alpha_far = (std::max)(0.0f, (std::min)(cfg.highlight_alpha_near, cfg.highlight_alpha_far));
        cfg.highlight_size = (std::max)(2.0f, (std::min)(32.0f, cfg.highlight_size));
        cfg.highlight_camera_hz = (std::max)(5, (std::min)(240, cfg.highlight_camera_hz));
        cfg.compass_width = (std::max)(0.1f, (std::min)(1.0f, cfg.compass_width));
        cfg.compass_offset_y = (std::max)(0.0f, (std::min)(2000.0f, cfg.compass_offset_y));
        cfg.compass_height = (std::max)(10.0f, (std::min)(120.0f, cfg.compass_height));
        cfg.compass_span_deg = (std::max)(30.0f, (std::min)(360.0f, cfg.compass_span_deg));
        cfg.compass_opacity = (std::max)(0.1f, (std::min)(1.0f, cfg.compass_opacity));
        cfg.compass_categories &= mdb::kAllCats;
        cfg.compass_marker_distance = (std::max)(500.0f, (std::min)(200000.0f, cfg.compass_marker_distance));

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
        out += "floor_fade_uu = " + std::format("{:.0f}", cfg.floor_fade_uu) + "\n";
        out += "floor_gradient_strength = " + std::format("{:.2f}", cfg.floor_gradient_strength) + "\n";
        out += "floor_base_color = " + std::format("{:.0f} {:.0f} {:.0f}", cfg.floor_base_r, cfg.floor_base_g,
                                                   cfg.floor_base_b) +
               "\n";
        out += "slice_hz = " + std::to_string(cfg.slice_hz) + "\n";
        out += "feet_z_smooth_ms = " + std::to_string(cfg.feet_z_smooth_ms) + "\n";
        out += "player_z_offset = " + std::format("{:.0f}", cfg.player_z_offset) + "\n";
        out += "fallback_use_composite = " + std::string(cfg.fallback_use_composite ? "1" : "0") + "\n\n";
        out += "debug_readout = " + std::string(cfg.debug_readout ? "1" : "0") + "\n";
        out += "debug_show_panel_on_start = " + std::string(cfg.debug_show_panel_on_start ? "1" : "0") + "\n";
        out += "\n; Markers. markers_categories is a comma-separated list of\n";
        out += ";   shrine, chest, pickup, boss, elite, enemy, npc, merchant, door, ladder, lift,\n";
        out += ";   fog_gate, hidden, other\n";
        out += "; (or `all` / `none`). The same list drives the F2 filter checkboxes.\n";
        out += "markers_enabled = " + std::string(cfg.markers_enabled ? "1" : "0") + "\n";
        out += "markers_live = " + std::string(cfg.markers_live ? "1" : "0") + "\n";
        out += "markers_filter_chapter = " + std::string(cfg.markers_filter_chapter ? "1" : "0") + "\n";
        out += "markers_rounds_per_sec = " + std::to_string(cfg.markers_rounds_per_sec) + "\n";
        out += "markers_scan_chunk = " + std::to_string(cfg.markers_scan_chunk) + "\n";
        out += "markers_scan_period_ms = " + std::to_string(cfg.markers_scan_period_ms) + "\n";
        out += "markers_categories = " + mdb::format_category_mask(cfg.markers_categories) + "\n";
        out += "markers_hide_found = " + std::string(cfg.markers_hide_found ? "1" : "0") + "\n";
        out += "markers_found_alpha = " + std::format("{:.2f}", cfg.markers_found_alpha) + "\n";
        out += "markers_size = " + std::format("{:.1f}", cfg.markers_size) + "\n";
        out += "markers_clamp_to_edge = " + std::string(cfg.markers_clamp_to_edge ? "1" : "0") + "\n";
        out += "markers_max_draw = " + std::to_string(cfg.markers_max_draw) + "\n";
        out += "found_tracker = " + std::string(cfg.found_tracker ? "1" : "0") + "\n";
        out += "found_save_debounce_ms = " + std::to_string(cfg.found_save_debounce_ms) + "\n\n";
        out += "\n; ---------------------------------------------------------------------------------\n";
        out += "; The full map (map_key - shipped default M)\n";
        out += "; ---------------------------------------------------------------------------------\n";
        out += "; Same height-sliced asset as the minimap, at map scale, always north-up. While it\n";
        out += "; is open the minimap is hidden and the mouse works. Zoom is world units per SCREEN\n";
        out += "; pixel - the same unit as minimap_zoom, so the numbers are comparable.\n";
        out += "map_zoom = " + std::format("{:.0f}", cfg.map_zoom) + "\n";
        out += "map_zoom_min = " + std::format("{:.0f}", cfg.map_zoom_min) + "\n";
        out += "map_zoom_max = " + std::format("{:.0f}", cfg.map_zoom_max) + "\n";
        out += "map_zoom_factor = " + std::format("{:.2f}", cfg.map_zoom_factor) + "\n";
        out += "map_pan_speed = " + std::format("{:.0f}", cfg.map_pan_speed) + "\n";
        out += "map_margin = " + std::format("{:.3f}", cfg.map_margin) + "\n";
        out += "map_backdrop = " + std::format("{:.2f}", cfg.map_backdrop) + "\n";
        out += "map_marker_size = " + std::format("{:.1f}", cfg.map_marker_size) + "\n";
        out += "map_markers_max_draw = " + std::to_string(cfg.map_markers_max_draw) + "\n";
        out += "map_floor_step = " + std::format("{:.0f}", cfg.map_floor_step) + "\n";
        out += "map_show_all_floors = " + std::string(cfg.map_show_all_floors ? "1" : "0") + "\n";
        out += "map_slice_px = " + std::to_string(cfg.map_slice_px) + "\n";
        out += "map_slice_hz = " + std::to_string(cfg.map_slice_hz) + "\n";
        out += "map_gamepad = " + std::string(cfg.map_gamepad ? "1" : "0") + "\n";
        out += "map_gamepad_deadzone = " + std::format("{:.2f}", cfg.map_gamepad_deadzone) + "\n";
        out += "map_waypoint_persist = " + std::string(cfg.map_waypoint_persist ? "1" : "0") + "\n\n";
        out += "\n; ---------------------------------------------------------------------------------\n";
        out += "; Hold-key x-ray highlight\n";
        out += "; ---------------------------------------------------------------------------------\n";
        out += "; While highlight_key is HELD (or the gamepad chord is), every marker of an enabled\n";
        out += "; category within highlight_radius uu is drawn at its projected screen position -\n";
        out += "; glyph, name and distance - through walls, fading with distance. It is a hold, not\n";
        out += "; a toggle, so there is no state to get stuck. highlight_key takes the same names as\n";
        out += "; the other hotkeys plus SPACE and L/R ALT / SHIFT / CTRL.\n";
        out += "highlight_enabled = " + std::string(cfg.highlight_enabled ? "1" : "0") + "\n";
        out += "highlight_key = " + vk_name(cfg.highlight_key) + "\n";
        out += "highlight_gamepad = " + std::string(cfg.highlight_gamepad ? "1" : "0") + "\n";
        {
            const std::wstring chord =
                pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt);
            std::string narrow;
            narrow.reserve(chord.size());
            for (const wchar_t c : chord)
            {
                narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            out += "highlight_pad_chord = " + narrow + "\n";
        }
        out += "highlight_radius = " + std::format("{:.0f}", cfg.highlight_radius) + "\n";
        out += "highlight_categories = " + mdb::format_category_mask(cfg.highlight_categories) + "\n";
        out += "highlight_show_found = " + std::string(cfg.highlight_show_found ? "1" : "0") + "\n";
        out += "highlight_max_draw = " + std::to_string(cfg.highlight_max_draw) + "\n";
        out += "highlight_alpha_near = " + std::format("{:.2f}", cfg.highlight_alpha_near) + "\n";
        out += "highlight_alpha_far = " + std::format("{:.2f}", cfg.highlight_alpha_far) + "\n";
        out += "highlight_size = " + std::format("{:.1f}", cfg.highlight_size) + "\n";
        out += "highlight_labels = " + std::string(cfg.highlight_labels ? "1" : "0") + "\n";
        out += "highlight_edge_arrows = " + std::string(cfg.highlight_edge_arrows ? "1" : "0") + "\n";
        out += "highlight_camera_hz = " + std::to_string(cfg.highlight_camera_hz) + "\n";
        out += "\n; ---------------------------------------------------------------------------------\n";
        out += "; The compass strip\n";
        out += "; ---------------------------------------------------------------------------------\n";
        out += "; Headings and ticks across the top of the screen, with bearing pips for the\n";
        out += "; waypoint and for nearby markers of the selected categories. Hidden by exactly the\n";
        out += "; same rules as the minimap.\n";
        out += "compass_enabled = " + std::string(cfg.compass_enabled ? "1" : "0") + "\n";
        out += "compass_width = " + std::format("{:.3f}", cfg.compass_width) + "\n";
        out += "compass_offset_y = " + std::format("{:.0f}", cfg.compass_offset_y) + "\n";
        out += "compass_height = " + std::format("{:.0f}", cfg.compass_height) + "\n";
        out += "compass_span_deg = " + std::format("{:.0f}", cfg.compass_span_deg) + "\n";
        out += "compass_opacity = " + std::format("{:.2f}", cfg.compass_opacity) + "\n";
        out += "compass_categories = " + mdb::format_category_mask(cfg.compass_categories) + "\n";
        out += "compass_marker_distance = " + std::format("{:.0f}", cfg.compass_marker_distance) + "\n";
        out += "compass_show_waypoint = " + std::string(cfg.compass_show_waypoint ? "1" : "0") + "\n\n";
        out += "panel_key = " + vk_name(cfg.panel_key) + "\n";
        out += "reload_key = " + vk_name(cfg.reload_key) + "\n";
        out += "map_key = " + vk_name(cfg.map_key) + "\n";
        out += "map_recenter_key = " + vk_name(cfg.map_recenter_key) + "\n";

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
    // The waypoint file
    //==================================================================================

    mv::Waypoint waypoint()
    {
        SpinGuard guard(g_wp_lock);
        return g_wp;
    }

    void set_waypoint(const mv::Waypoint& wp)
    {
        {
            SpinGuard guard(g_wp_lock);
            g_wp = wp;
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    std::wstring waypoint_path()
    {
        return mod_dir() + L"\\wuchang_minimap_waypoint.txt";
    }

    void load_waypoint_file()
    {
        const std::wstring path = waypoint_path();
        std::string text;
        mv::Waypoint wp{};
        if (read_whole_file(path, text))
        {
            if (!mv::waypoint_parse(text, wp))
            {
                logf(L"waypoint: {} exists but carries no usable x / y - ignored", path);
                wp = mv::Waypoint{};
            }
            else if (wp.set)
            {
                logf(L"waypoint: loaded ({:.0f}, {:.0f}, {:.0f}) from {}", wp.x, wp.y, wp.z, path);
            }
        }
        {
            SpinGuard guard(g_wp_lock);
            g_wp = wp;
        }
        // What was just read IS what the file says, so nothing is pending.
        g_waypoint_dirty.store(false, std::memory_order_release);
    }

    void save_waypoint_file()
    {
        const mv::Waypoint wp = waypoint();
        const std::wstring path = waypoint_path();
        if (!write_whole_file(path, mv::waypoint_serialize(wp)))
        {
            logf(L"waypoint: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
            return;
        }
        if (wp.set)
        {
            logf(L"waypoint: saved ({:.0f}, {:.0f}, {:.0f})", wp.x, wp.y, wp.z);
        }
        else
        {
            log(L"waypoint: cleared");
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
