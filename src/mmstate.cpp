#include "mmstate.hpp"

#include "atomicfile.hpp"
#include "config_keys.hpp"
#include "config_rewrite.hpp"
#include "saveslot.hpp"
#include "spinlock.hpp"

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace RC;

namespace mm
{
    namespace
    {
        std::atomic<std::uint32_t> g_seq{0};
        Snapshot g_snapshot{};

        spin::Spinlock g_cfg_lock;
        Config g_cfg{};

        spin::Spinlock g_wp_lock;
        mv::WaypointSet g_wp{};

        spin::Spinlock g_log_lock;
        std::vector<std::wstring> g_log_queue;
        DWORD g_loop_thread = 0;
        // Dropped, never blocked and never grown: the game thread must not wait on the loop thread.
        constexpr std::size_t kLogQueueMax = 4096;
        std::size_t g_log_dropped = 0;

        std::wstring g_mod_dir;

        std::wstring resolve_mod_dir()
        {
            // main.dll lives in ...\ue4ss\Mods\WuchangMinimap\dlls, so the mod folder is one level up.
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

        // Text file I/O: plain Win32, no iostreams anywhere in this mod.

        bool read_whole_file(const std::wstring& path, std::string& out)
        {
            const mmfile::ReadInfo info = mmfile::read_whole_file(path, out, 64ull << 20);
            if (info.status == mmfile::ReadStatus::Ok)
            {
                return true;
            }
            if (info.too_big)
            {
                logf(L"FAILED to read {} - it is {} byte(s), over the 64 MB cap this mod reads; "
                     L"it was IGNORED",
                     path,
                     info.size);
            }
            else if (info.status == mmfile::ReadStatus::Failed)
            {
                logf(L"FAILED to read {} (error {}) - the file exists but could not be read, so its "
                     L"contents were IGNORED (something else may have it open)",
                     path,
                     info.error);
            }
            return false;
        }

        // Atomic: temp file, flush, rename (atomicfile.hpp). No backup.
        bool write_whole_file(const std::wstring& path, const std::string& data)
        {
            unsigned err = 0;
            if (mmfile::write_whole_file_atomic(path, data, false, &err))
            {
                return true;
            }
            ::SetLastError(err);
            return false;
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

        std::wstring widen_ascii(std::string_view v)
        {
            std::wstring out;
            out.reserve(v.size());
            for (const char c : v)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return out;
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

        // Named keys beyond the F-keys / letters / digits. GetAsyncKeyState answers for either side
        // with VK_MENU / VK_SHIFT / VK_CONTROL.
        struct NamedKey
        {
            const char* name;
            int vk;
        };

        constexpr NamedKey kNamedKeys[] = {
            {"TAB", VK_TAB},          {"SPACE", VK_SPACE},        {"LALT", VK_LMENU},
            {"RALT", VK_RMENU},       {"ALT", VK_MENU},           {"LSHIFT", VK_LSHIFT},
            {"RSHIFT", VK_RSHIFT},    {"SHIFT", VK_SHIFT},        {"LCTRL", VK_LCONTROL},
            {"RCTRL", VK_RCONTROL},   {"CTRL", VK_CONTROL},
            {"UP", VK_UP},            {"DOWN", VK_DOWN},          {"LEFT", VK_LEFT},
            {"RIGHT", VK_RIGHT},      {"ENTER", VK_RETURN},       {"BACKSPACE", VK_BACK},
            {"INSERT", VK_INSERT},    {"DELETE", VK_DELETE},      {"HOME", VK_HOME},
            {"END", VK_END},          {"PAGEUP", VK_PRIOR},       {"PAGEDOWN", VK_NEXT},
            // The numpad: different virtual keys from the digit row.
            {"NUM0", VK_NUMPAD0},     {"NUM1", VK_NUMPAD1},       {"NUM2", VK_NUMPAD2},
            {"NUM3", VK_NUMPAD3},     {"NUM4", VK_NUMPAD4},       {"NUM5", VK_NUMPAD5},
            {"NUM6", VK_NUMPAD6},     {"NUM7", VK_NUMPAD7},       {"NUM8", VK_NUMPAD8},
            {"NUM9", VK_NUMPAD9},     {"NUMPLUS", VK_ADD},        {"NUMMINUS", VK_SUBTRACT},
            {"NUMMUL", VK_MULTIPLY},  {"NUMDIV", VK_DIVIDE},      {"NUMDOT", VK_DECIMAL},
            // Mouse buttons 3-5 only: left and right belong to the game and to the overlay.
            {"MOUSE3", VK_MBUTTON},   {"MOUSE4", VK_XBUTTON1},    {"MOUSE5", VK_XBUTTON2},
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

        // Strips ONE modifier prefix (`ctrl+m`) and advances `name` past the `+`. Two prefixes leave
        // a non-key-name behind and the key parser rejects it.
        int take_key_modifier(std::string& name)
        {
            const std::size_t plus = name.find('+');
            if (plus == std::string::npos || plus == 0 || plus + 1 >= name.size())
            {
                return kKeyModNone;
            }
            const std::string up = upper(name.substr(0, plus));
            int mod = kKeyModNone;
            if (up == "CTRL" || up == "CONTROL")
            {
                mod = kKeyModCtrl;
            }
            else if (up == "SHIFT")
            {
                mod = kKeyModShift;
            }
            else if (up == "ALT")
            {
                mod = kKeyModAlt;
            }
            else
            {
                return kKeyModNone;
            }
            name = name.substr(plus + 1);
            // Trim blanks after the `+` ("ctrl + m") so the key parser sees a bare name.
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
            {
                name.erase(name.begin());
            }
            return mod;
        }

        // Hotkey name -> virtual key. Accepted: F1..F5, F7, F8; one letter A..Z or digit 0..9; the named
        // keys above. F6 (RenoDX DLSS5), F9/F11 (engine binds), F10 (console) and F12 (Steam) are rejected.
        int vk_from_name(const std::string& raw_name, int fallback, const char* key_label)
        {
            std::string name = raw_name;
            const int mod = take_key_modifier(name);
            const std::wstring label(key_label, key_label + std::strlen(key_label));
            const std::wstring shown(raw_name.begin(), raw_name.end());

            const auto reject = [&](const wchar_t* why) {
                logf(L"config: {} = '{}' rejected ({}) - keeping the default. Allowed: F1..F5, F7, F8, "
                     L"a single letter or digit, TAB, SPACE, ENTER, BACKSPACE, the arrows, the "
                     L"navigation block, NUM0..NUM9 and the numpad operators, MOUSE3..MOUSE5, or "
                     L"L/R ALT / SHIFT / CTRL, optionally with ONE `ctrl+` / `shift+` / `alt+` "
                     L"prefix. `none` leaves the action unbound.",
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
                if (up == "NONE")
                {
                    return 0; // deliberately unbound
                }
                for (const NamedKey& k : kNamedKeys)
                {
                    if (up == k.name)
                    {
                        return key_make(k.vk, mod);
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
                return key_make(VK_F1 + (n - 1), mod);
            }

            if (name.size() == 1)
            {
                const char c = name[0];
                if (c >= 'a' && c <= 'z')
                {
                    return key_make(static_cast<int>(c - 'a' + 'A'), mod);
                }
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                {
                    return key_make(static_cast<int>(c), mod);
                }
                return reject(L"not a letter or a digit");
            }

            return reject(L"not a recognised key name");
        }

        // The key half on its own ("F2", "M", "TAB"); vk_name() adds the modifier.
        std::string vk_name_plain(int vk);

        // The inverse of vk_from_name, modifier prefix included: what Save writes, the loader reads back unchanged.
        std::string vk_name(int binding)
        {
            const int vk = key_vk(binding);
            if (vk == 0)
            {
                return "none";
            }
            const int mod = key_mod(binding);
            const char* prefix = mod == kKeyModCtrl    ? "ctrl+"
                                 : mod == kKeyModShift ? "shift+"
                                 : mod == kKeyModAlt   ? "alt+"
                                                       : "";
            return std::string{prefix} + vk_name_plain(vk);
        }

        std::string vk_name_plain(int vk)
        {
            if (vk == 0)
            {
                return "none";
            }
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
            return "none";
        }

        // The gamepad chord: "LB+RB", "A", "LT+RT", "none". Face / shoulder / dpad buttons are bits in the
        // XInput mask; the two triggers are analogue and carried as flags. The names are the ones on the pad.

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

        // One parser for every `*_categories` key. Unknown names are ignored with a log line; a renamed name is honoured.
        std::uint32_t parse_cats(std::string_view key, const std::string& value, std::uint32_t current)
        {
            std::string rejected;
            std::string legacy;
            const std::uint32_t mask = mdb::parse_category_mask(value, current, &rejected, &legacy);
            const std::wstring wkey(key.begin(), key.end());
            if (!rejected.empty())
            {
                const std::string known = mdb::format_category_mask(mdb::kAllCats);
                logf(L"config: {} - unknown name(s) '{}' ignored. Known: {}",
                     wkey,
                     std::wstring(rejected.begin(), rejected.end()),
                     std::wstring(known.begin(), known.end()));
            }
            if (!legacy.empty())
            {
                logf(L"config: {} - '{}' is the old name of a renamed category and was "
                     L"accepted; Save will rewrite the line with the current name",
                     wkey,
                     std::wstring(legacy.begin(), legacy.end()));
            }
            return mask;
        }

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

        // "R G B" / "R,G,B", 0..255 each. A missing channel keeps its current value.
        void parse_rgb(const std::string& value, float& r, float& g, float& b)
        {
            float rgb[3] = {r, g, b};
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
            r = rgb[0];
            g = rgb[1];
            b = rgb[2];
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

        const char* preset_name(HudPreset p)
        {
            switch (p)
            {
            case HudPreset::TopLeft:
                return "top-left";
            case HudPreset::TopRight:
                return "top-right";
            case HudPreset::BottomLeft:
                return "bottom-left";
            case HudPreset::BottomRight:
                return "bottom-right";
            case HudPreset::Custom:
            default:
                return "custom";
            }
        }

        HudPreset preset_from_name(const std::string& v, HudPreset fallback)
        {
            if (v == "custom")
            {
                return HudPreset::Custom;
            }
            if (v == "top-left")
            {
                return HudPreset::TopLeft;
            }
            if (v == "top-right")
            {
                return HudPreset::TopRight;
            }
            if (v == "bottom-left")
            {
                return HudPreset::BottomLeft;
            }
            if (v == "bottom-right")
            {
                return HudPreset::BottomRight;
            }
            return fallback;
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

        bool apply_core(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "mod_enabled")
            {
                cfg.mod_enabled = parse_bool(value, cfg.mod_enabled);
            }
            else if (key == "show_minimap")
            {
                cfg.show_minimap = parse_bool(value, cfg.show_minimap);
            }
            else if (key == "ui_scale")
            {
                // `auto` derives the factor from the back buffer height on the render thread; a number pins it.
                if (value == "auto" || value.empty())
                {
                    cfg.ui_scale_auto = true;
                }
                else
                {
                    cfg.ui_scale_auto = false;
                    cfg.ui_scale = parse_float(value, cfg.ui_scale);
                }
            }
            else if (key == "font_size")
            {
                cfg.font_size = parse_int(value, cfg.font_size);
            }
            else if (key == "hud_preset")
            {
                cfg.hud_preset = preset_from_name(value, cfg.hud_preset);
            }
            // A bad theme / palette value keeps what is in force and says so.
            else if (key == "theme")
            {
                if (!gly::theme_from_name(value, cfg.theme))
                {
                    logf(L"config: theme = '{}' is not a theme (expected ink or neutral) - keeping {}",
                         widen_ascii(value), widen_ascii(gly::theme_name(cfg.theme)));
                }
            }
            else if (key == "palette")
            {
                if (!gly::palette_from_name(value, cfg.palette))
                {
                    logf(L"config: palette = '{}' is not a palette (expected default or colorblind) - "
                         L"keeping {}",
                         widen_ascii(value), widen_ascii(gly::palette_name(cfg.palette)));
                }
            }
            else if (key == "minimap_size")
            {
                cfg.size_frac = parse_float(value, cfg.size_frac);
            }
            else if (key == "minimap_zoom")
            {
                cfg.zoom_uu_per_px = parse_float(value, cfg.zoom_uu_per_px);
            }
            else if (key == "minimap_zoom_presets")
            {
                std::string rejected;
                float presets[mv::kMaxZoomPresets]{};
                const int n = mv::parse_zoom_presets(value, presets, &rejected);
                if (n > 0)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        cfg.minimap_zoom_presets[i] = presets[i];
                    }
                    cfg.minimap_zoom_preset_count = n;
                }
                if (!rejected.empty() || n == 0)
                {
                    logf(L"config: minimap_zoom_presets - {} usable rung(s){}{}. Expected 1..{} numbers "
                         L"between 2 and 400, e.g. 13, 26, 52",
                         n,
                         rejected.empty() ? L"" : L", ignored: ",
                         widen_ascii(rejected),
                         mv::kMaxZoomPresets);
                }
            }
            else if (key == "zoom_key")
            {
                cfg.zoom_key = vk_from_name(value, cfg.zoom_key, "zoom_key");
            }
            else if (key == "screenshot_key")
            {
                cfg.screenshot_key = vk_from_name(value, cfg.screenshot_key, "screenshot_key");
            }
            else if (key == "waypoint_nearest_key")
            {
                cfg.waypoint_nearest_key =
                    vk_from_name(value, cfg.waypoint_nearest_key, "waypoint_nearest_key");
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
            else
            {
                return false;
            }
            return true;
        }

        bool apply_floors(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "show_adjacent_floors")
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
            else if (key == "floor_fade_above_uu")
            {
                cfg.floor_fade_above_uu = parse_float(value, cfg.floor_fade_above_uu);
            }
            else if (key == "map_unreachable")
            {
                if (!srule::unreachable_from_name(value, cfg.map_unreachable))
                {
                    logf(L"config: map_unreachable = '{}' is not hide, dim or show - keeping {}",
                         widen_ascii(value), widen_ascii(srule::unreachable_name(cfg.map_unreachable)));
                }
            }
            else if (key == "floor_gradient_strength")
            {
                cfg.floor_gradient_strength = parse_float(value, cfg.floor_gradient_strength);
            }
            else if (key == "floor_base_color")
            {
                parse_rgb(value, cfg.floor_base_r, cfg.floor_base_g, cfg.floor_base_b);
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
            else
            {
                return false;
            }
            return true;
        }

        bool apply_markers(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "markers_enabled")
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
                cfg.markers_categories = parse_cats(key, value, cfg.markers_categories);
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
            else if (key == "found_profile")
            {
                // Free text: `auto`, `shared`, or any name. It reaches a FILENAME and is sanitised by slotid::sanitise_key.
                const std::string v = trim(value);
                if (v.empty())
                {
                    logf(L"config: found_profile = '' is not a profile - keeping '{}'",
                         widen_ascii(cfg.found_profile));
                }
                else
                {
                    ::strncpy_s(cfg.found_profile, sizeof(cfg.found_profile), v.c_str(), _TRUNCATE);
                }
            }
            else if (key == "first_run_toast")
            {
                cfg.first_run_toast = parse_bool(value, cfg.first_run_toast);
            }
            else if (key == "menu_ignore_roots")
            {
                // Free text: widget class-name prefixes, possibly naming a class this build has never seen.
                ::strncpy_s(cfg.menu_ignore_roots, sizeof(cfg.menu_ignore_roots), trim(value).c_str(),
                            _TRUNCATE);
            }
            // A bad value keeps the level in force and names the three that exist.
            else if (key == "log_level")
            {
                LogLv lv = cfg.log_level;
                if (log_level_from_name(value, lv))
                {
                    cfg.log_level = lv;
                }
                else
                {
                    logf(L"config: log_level = '{}' is not a level (expected normal, verbose or "
                         L"trace) - keeping {}",
                         widen_ascii(value), widen_ascii(log_level_name(cfg.log_level)));
                }
            }
            else if (key == "crash_breadcrumb")
            {
                cfg.crash_breadcrumb = parse_bool(value, cfg.crash_breadcrumb);
            }
            else if (key == "map_pad_open_chord")
            {
                // Same spelling as highlight_pad_chord, minus the triggers: LT / RT are parsed and then dropped.
                bool ignored_lt = false;
                bool ignored_rt = false;
                parse_pad_chord(value, cfg.map_pad_open_chord, ignored_lt, ignored_rt);
                if (ignored_lt || ignored_rt)
                {
                    log(L"config: map_pad_open_chord - the triggers are not buttons; LT / RT ignored");
                }
            }
            else if (key == "panel_pad_open_chord")
            {
                // Buttons only, exactly like map_pad_open_chord.
                bool ignored_lt = false;
                bool ignored_rt = false;
                parse_pad_chord(value, cfg.panel_pad_open_chord, ignored_lt, ignored_rt);
                if (ignored_lt || ignored_rt)
                {
                    log(L"config: panel_pad_open_chord - the triggers are not buttons; LT / RT ignored");
                }
            }
            else if (key == "ui_font")
            {
                // Free text: an absolute path to a .ttf / .otf; `none` or "" means the built-in bitmap font.
                // Only the render thread can open it.
                ::strncpy_s(cfg.ui_font, sizeof(cfg.ui_font), trim(value).c_str(), _TRUNCATE);
            }
            else if (key == "zoom_dpi_scaled")
            {
                cfg.zoom_dpi_scaled = parse_bool(value, cfg.zoom_dpi_scaled);
            }
            else if (key == "saveslot_uuid_call")
            {
                cfg.saveslot_uuid_call = parse_bool(value, cfg.saveslot_uuid_call);
            }
            else if (key == "map_recenter_key")
            {
                cfg.map_recenter_key = vk_from_name(value, cfg.map_recenter_key, "map_recenter_key");
            }
            else
            {
                return false;
            }
            return true;
        }

        bool apply_map(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "map_zoom")
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
            else
            {
                return false;
            }
            return true;
        }

        bool apply_highlight(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "highlight_enabled")
            {
                cfg.highlight_enabled = parse_bool(value, cfg.highlight_enabled);
            }
            else if (key == "highlight_mode")
            {
                if (value == "toggle")
                {
                    cfg.highlight_mode = HighlightMode::Toggle;
                }
                else if (value == "hold")
                {
                    cfg.highlight_mode = HighlightMode::Hold;
                }
                else
                {
                    logf(L"config: highlight_mode = '{}' is not a mode (expected toggle or hold) - "
                         L"keeping {}",
                         widen_ascii(value),
                         cfg.highlight_mode == HighlightMode::Hold ? L"hold" : L"toggle");
                }
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
                cfg.highlight_categories = parse_cats(key, value, cfg.highlight_categories);
            }
            else if (key == "highlight_show_found")
            {
                cfg.highlight_show_found = parse_bool(value, cfg.highlight_show_found);
            }
            else if (key == "highlight_labels_max")
            {
                cfg.highlight_labels_max = parse_int(value, cfg.highlight_labels_max);
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
            else if (key == "xray_rarity_colors_enabled")
            {
                cfg.xray_rarity_colors_enabled = parse_bool(value, cfg.xray_rarity_colors_enabled);
            }
            else if (key == "xray_rarity_colors")
            {
                std::string rejected;
                mdb::parse_rarity_colors(value, cfg.xray_rarity_colors, &rejected);
                if (!rejected.empty())
                {
                    logf(L"config: xray_rarity_colors - bad entr(ies) '{}' ignored. Expected "
                         L"{} hex colours, e.g. {}",
                         std::wstring(rejected.begin(), rejected.end()), mdb::kRarityCount,
                         L"ADAFDA, DAADC5, DAD6AD");
                }
            }
            else if (key == "markers_rarity_tint")
            {
                cfg.markers_rarity_tint = parse_bool(value, cfg.markers_rarity_tint);
            }
            else if (key == "highlight_camera_hz")
            {
                cfg.highlight_camera_hz = parse_int(value, cfg.highlight_camera_hz);
            }
            else
            {
                return false;
            }
            return true;
        }

        bool apply_compass_and_keys(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "compass_enabled")
            {
                cfg.compass_enabled = parse_bool(value, cfg.compass_enabled);
            }
            else if (key == "compass_width")
            {
                cfg.compass_width = parse_float(value, cfg.compass_width);
            }
            else if (key == "compass_plate")
            {
                cfg.compass_plate = parse_bool(value, cfg.compass_plate);
            }
            else if (key == "compass_anchor")
            {
                cfg.compass_anchor = (value == "bottom") ? VAnchor::Bottom : VAnchor::Top;
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
                cfg.compass_categories = parse_cats(key, value, cfg.compass_categories);
            }
            else if (key == "compass_marker_distance")
            {
                cfg.compass_marker_distance = parse_float(value, cfg.compass_marker_distance);
            }
            else if (key == "compass_show_waypoint")
            {
                cfg.compass_show_waypoint = parse_bool(value, cfg.compass_show_waypoint);
            }
            else if (key == "found_save_debounce_ms")
            {
                cfg.found_save_debounce_ms = parse_int(value, cfg.found_save_debounce_ms);
            }
            else if (key == "boss_defeat_from_save")
            {
                cfg.boss_defeat_from_save = parse_bool(value, cfg.boss_defeat_from_save);
            }
            else if (key == "markers_absence_rounds")
            {
                cfg.markers_absence_rounds = parse_int(value, cfg.markers_absence_rounds);
            }
            else if (key == "markers_absence_categories")
            {
                cfg.markers_absence_categories = parse_cats(key, value, cfg.markers_absence_categories);
            }
            else
            {
                return false;
            }
            return true;
        }

        bool apply_tuning(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "minimap_backdrop")
            {
                cfg.minimap_backdrop = parse_float(value, cfg.minimap_backdrop);
            }
            else if (key == "minimap_backdrop_color")
            {
                parse_rgb(value, cfg.minimap_backdrop_r, cfg.minimap_backdrop_g, cfg.minimap_backdrop_b);
            }
            else if (key == "minimap_frame_color")
            {
                parse_rgb(value, cfg.minimap_frame_r, cfg.minimap_frame_g, cfg.minimap_frame_b);
            }
            else if (key == "minimap_frame_alpha")
            {
                cfg.minimap_frame_alpha = parse_float(value, cfg.minimap_frame_alpha);
            }
            else if (key == "minimap_composite_alpha")
            {
                cfg.minimap_composite_alpha = parse_float(value, cfg.minimap_composite_alpha);
            }
            else if (key == "minimap_min_px")
            {
                cfg.minimap_min_px = parse_float(value, cfg.minimap_min_px);
            }
            else if (key == "minimap_arrow_frac")
            {
                cfg.minimap_arrow_frac = parse_float(value, cfg.minimap_arrow_frac);
            }
            else if (key == "minimap_arrow_min_px")
            {
                cfg.minimap_arrow_min_px = parse_float(value, cfg.minimap_arrow_min_px);
            }
            else if (key == "waypoint_size_scale")
            {
                cfg.waypoint_size_scale = parse_float(value, cfg.waypoint_size_scale);
            }
            else if (key == "reader_position_period_ms")
            {
                cfg.reader_position_period_ms = parse_int(value, cfg.reader_position_period_ms);
            }
            else if (key == "reader_resolve_period_ms")
            {
                cfg.reader_resolve_period_ms = parse_int(value, cfg.reader_resolve_period_ms);
            }
            else if (key == "reader_widget_sweep_max_period_ms")
            {
                cfg.reader_widget_sweep_max_period_ms =
                    parse_int(value, cfg.reader_widget_sweep_max_period_ms);
            }
            else if (key == "reader_widget_sweep_warm_ms")
            {
                cfg.reader_widget_sweep_warm_ms = parse_int(value, cfg.reader_widget_sweep_warm_ms);
            }
            else if (key == "reader_widget_sweep_period_ms")
            {
                cfg.reader_widget_sweep_period_ms = parse_int(value, cfg.reader_widget_sweep_period_ms);
            }
            else if (key == "reader_transition_cooldown_ms")
            {
                cfg.reader_transition_cooldown_ms = parse_int(value, cfg.reader_transition_cooldown_ms);
            }
            else if (key == "reader_teleport_jump_uu")
            {
                cfg.reader_teleport_jump_uu = parse_float(value, static_cast<float>(cfg.reader_teleport_jump_uu));
            }
            else if (key == "reader_chapter_period_ms")
            {
                cfg.reader_chapter_period_ms = parse_int(value, cfg.reader_chapter_period_ms);
            }
            else if (key == "reader_log_throttle_ms")
            {
                cfg.reader_log_throttle_ms = parse_int(value, cfg.reader_log_throttle_ms);
            }
            else if (key == "markers_live_grace_rounds")
            {
                cfg.markers_live_grace_rounds = parse_int(value, cfg.markers_live_grace_rounds);
            }
            else if (key == "map_asset_retire_grace_ms")
            {
                cfg.map_asset_retire_grace_ms = parse_int(value, cfg.map_asset_retire_grace_ms);
            }
            else if (key == "hide_reason_log_ms")
            {
                cfg.hide_reason_log_ms = parse_int(value, cfg.hide_reason_log_ms);
            }
            else if (key == "srv_heap_size")
            {
                cfg.srv_heap_size = parse_int(value, cfg.srv_heap_size);
            }
            else if (key == "navmesh_dump")
            {
                cfg.navmesh_dump = parse_bool(value, cfg.navmesh_dump);
            }
            else if (key == "highlight_camera_resolve_ms")
            {
                cfg.highlight_camera_resolve_ms = parse_int(value, cfg.highlight_camera_resolve_ms);
            }
            else if (key == "highlight_compass_period_ms")
            {
                cfg.highlight_compass_period_ms = parse_int(value, cfg.highlight_compass_period_ms);
            }
            else if (key == "highlight_getter_period_ms")
            {
                cfg.highlight_getter_period_ms = parse_int(value, cfg.highlight_getter_period_ms);
            }
            else if (key == "highlight_pov_scan_bytes")
            {
                cfg.highlight_pov_scan_bytes = parse_int(value, cfg.highlight_pov_scan_bytes);
            }
            else if (key == "highlight_pov_bad_reads")
            {
                cfg.highlight_pov_bad_reads = parse_int(value, cfg.highlight_pov_bad_reads);
            }
            else if (key == "compass_tick_step_deg")
            {
                cfg.compass_tick_step_deg = parse_float(value, cfg.compass_tick_step_deg);
            }
            else if (key == "compass_max_pips")
            {
                cfg.compass_max_pips = parse_int(value, cfg.compass_max_pips);
            }
            else if (key == "compass_pip_labels")
            {
                cfg.compass_pip_labels = parse_bool(value, cfg.compass_pip_labels);
            }
            else if (key == "compass_pip_height_uu")
            {
                cfg.compass_pip_height_uu = parse_float(value, cfg.compass_pip_height_uu);
            }
            else
            {
                return false;
            }
            return true;
        }

        // The whole key table, split into groups because MSVC refuses one if/else-if chain past ~123 arms
        // (C1061). tests/markers_test.cpp scrapes the key literals out of this file, so a key that lands
        // in no group is caught on the build machine.
        void apply_setting(Config& cfg, const std::string& key, const std::string& value)
        {
            if (apply_core(cfg, key, value))
            {
                return;
            }
            if (apply_floors(cfg, key, value))
            {
                return;
            }
            if (apply_markers(cfg, key, value))
            {
                return;
            }
            if (apply_map(cfg, key, value))
            {
                return;
            }
            if (apply_highlight(cfg, key, value))
            {
                return;
            }
            if (apply_compass_and_keys(cfg, key, value))
            {
                return;
            }
            if (apply_tuning(cfg, key, value))
            {
                return;
            }
            // An unknown key is ignored: a config from a NEWER build must not stop an older one from starting.
        }
    } // namespace

    // The master switch. Written only by modswitch (loop thread); read by the game and render threads.
    // Starts TRUE so ProcessEvent and Present behave normally before the config is read.
    std::atomic<bool> g_mod_active{true};

    // Bumped by every set_config; read by cfg_cached(). Starts at 1 so a thread-local generation of 0 means "never loaded".
    std::atomic<std::uint32_t> g_cfg_gen{1};

    // The per-activity counter table (perf.hpp). Every counter has exactly one writing thread.
    perf::Table g_perf{};

    // "config refresh" - the generation-cached config's slow path. Registered from load_config_file (loop thread).
    int g_pf_config = -1;

    std::atomic<bool> g_panel_open{false};
    std::atomic<bool> g_map_open{false};
    std::atomic<bool> g_reload_config{false};
    std::atomic<bool> g_save_config{false};
    // Raised wherever a setting changes; consumed, debounced, by the loop thread.
    std::atomic<bool> g_save_config_soon{false};
    std::atomic<bool> g_key_capture{false};
    std::atomic<bool> g_panel_drew_frame{false};
    std::atomic<bool> g_waypoint_dirty{false};

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

    Config config()
    {
        spin::SpinGuard guard(g_cfg_lock);
        return g_cfg;
    }

    void set_config(const Config& cfg)
    {
        {
            spin::SpinGuard guard(g_cfg_lock);
            g_cfg = cfg;
        }
        // The log macros read this atomic instead of taking the config lock, so a suppressed line costs one relaxed load.
        g_log_level.store(static_cast<int>(cfg.log_level), std::memory_order_relaxed);
        // Bump AFTER the store so a reader that sees the new generation copies the new value.
        g_cfg_gen.fetch_add(1, std::memory_order_release);
    }

    std::uint64_t qpc_us()
    {
        static const std::int64_t freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return static_cast<std::int64_t>(f.QuadPart);
        }();
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        return freq > 0 ? static_cast<std::uint64_t>(now.QuadPart * 1000000 / freq) : 0;
    }

    namespace
    {
        // GetTickCount64() until which the process counts as stalled, and a literal naming what did it.
        // Only ever moved FORWARD.
        std::atomic<std::uint64_t> g_perf_stall_until{0};
        std::atomic<const wchar_t*> g_perf_stall_why{nullptr};
    } // namespace

    int perf_register(const char* name, perf::Thread thread)
    {
        // Each registration is a `static const int` on a periodic activity's first call, from that activity's own thread.
        return perf::register_counter(g_perf, name, thread);
    }

    void perf_record(int id, std::uint64_t t0_us)
    {
        const std::uint64_t now_us = qpc_us();
        const double ms = now_us > t0_us ? static_cast<double>(now_us - t0_us) / 1000.0 : 0.0;
        const std::uint64_t now = ::GetTickCount64();
        perf::record(g_perf, id, ms, now, now >= g_perf_stall_until.load(std::memory_order_relaxed));
    }

    void perf_record_ms(int id, double ms)
    {
        const std::uint64_t now = ::GetTickCount64();
        perf::record(g_perf, id, ms, now, now >= g_perf_stall_until.load(std::memory_order_relaxed));
    }

    void perf_note_stall(const wchar_t* why, unsigned ms)
    {
        const std::uint64_t until = ::GetTickCount64() + ms;
        // Never shorten a window somebody else opened: two overlapping stalls are one.
        std::uint64_t was = g_perf_stall_until.load(std::memory_order_relaxed);
        while (until > was && !g_perf_stall_until.compare_exchange_weak(was, until, std::memory_order_relaxed))
        {
        }
        if (why != nullptr)
        {
            g_perf_stall_why.store(why, std::memory_order_relaxed);
        }
    }

    bool perf_in_stall()
    {
        return ::GetTickCount64() < g_perf_stall_until.load(std::memory_order_relaxed);
    }

    const wchar_t* perf_last_stall()
    {
        const wchar_t* why = g_perf_stall_why.load(std::memory_order_relaxed);
        return why != nullptr ? why : L"none";
    }

    const perf::Table& perf_table()
    {
        return g_perf;
    }

    void perf_reset_peaks()
    {
        perf::reset_peaks(g_perf);
    }

    // The per-thread cache slot, at NAMESPACE scope: a guarded function-local static is forbidden on this
    // game's game thread, because MSVC implements one with the host vcruntime's `_Init_thread_header`
    // machinery - the host-CRT dependency class that makes std::mutex fault.
    static_assert(std::is_trivially_copyable_v<Config> && std::is_trivially_destructible_v<Config>,
                  "Config must stay a POD: cfg_cached() keeps a thread_local copy of it and a "
                  "non-trivial member would make MSVC emit CRT TLS-init code on the game thread");
    thread_local Config g_tls_cfg{};
    thread_local std::uint32_t g_tls_cfg_gen = 0;

    const Config& cfg_cached()
    {
        // Generation 0 is never published (g_cfg_gen starts at 1), so a new thread takes the slow path exactly once.
        Config& tls_cfg = g_tls_cfg;
        std::uint32_t& tls_gen = g_tls_cfg_gen;

        const std::uint32_t gen = g_cfg_gen.load(std::memory_order_acquire);
        if (tls_gen != gen)
        {
            const std::uint64_t t0 = qpc_us();
            tls_cfg = config();
            tls_gen = gen;
            // Counts and times the SLOW path only.
            perf_record(g_pf_config, t0);
        }
        return tls_cfg;
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

    // The accepted set, mirroring vk_from_name exactly: F1..F5 / F7 / F8, a letter or a digit, and every entry of kNamedKeys.
    bool vk_bindable(int vk)
    {
        if (vk == 0)
        {
            return false;
        }
        if (vk >= VK_F1 && vk <= VK_F8 && vk != VK_F6)
        {
            return true;
        }
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        {
            return true;
        }
        for (const NamedKey& k : kNamedKeys)
        {
            if (vk == k.vk)
            {
                return true;
            }
        }
        return false;
    }

    const std::vector<int>& bindable_vks()
    {
        static const std::vector<int> all = [] {
            std::vector<int> v;
            for (int vk = 1; vk < 256; ++vk)
            {
                if (vk_bindable(vk))
                {
                    v.push_back(vk);
                }
            }
            return v;
        }();
        return all;
    }

    bool set_pad_chord(const std::string& text, std::uint16_t& mask, bool& lt, bool& rt)
    {
        const std::uint16_t was_mask = mask;
        const bool was_lt = lt;
        const bool was_rt = rt;
        parse_pad_chord(text, mask, lt, rt);
        return mask != was_mask || lt != was_lt || rt != was_rt;
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

    // The dev overlay file: not shipped, parsed only if it exists, loaded AFTER the main
    // file so a key set in both wins here. Everything in it is Tier::Dev (cfgkeys).
    std::wstring dev_config_path()
    {
        return mod_dir() + L"\\config_wuchang_minimap_dev.txt";
    }

    namespace
    {
        // Written by load_config_file() / save_config_file() (loop thread), read by the F2 panel (render thread).
        std::atomic<bool> g_dev_config_active{false};
    } // namespace

    bool dev_config_active()
    {
        return g_dev_config_active.load(std::memory_order_relaxed);
    }

    namespace
    {
        // One warning per key per process: every F5 and every 1 Hz mtime reload re-parses the file.
        bool g_warned[cfgkeys::kKeyCount] = {};

        void warn_once(std::string_view key, const std::wstring& text)
        {
            for (std::size_t i = 0; i < cfgkeys::kKeyCount; ++i)
            {
                if (key == cfgkeys::kKeys[i].name)
                {
                    if (!g_warned[i])
                    {
                        g_warned[i] = true;
                        log(text);
                    }
                    return;
                }
            }
        }

        // Unknown key names already reported. Loop thread only, and capped: the names come from a file
        // anyone can edit, and past the cap the remaining strangers go unreported rather than unbounded.
        constexpr std::size_t kMaxUnknownWarned = 32;
        std::string g_unknown_warned[kMaxUnknownWarned];
        std::size_t g_unknown_count = 0;

        void warn_unknown_once(const std::string& key)
        {
            for (std::size_t i = 0; i < g_unknown_count; ++i)
            {
                if (g_unknown_warned[i] == key)
                {
                    return;
                }
            }
            if (g_unknown_count >= kMaxUnknownWarned)
            {
                return;
            }
            g_unknown_warned[g_unknown_count++] = key;
            logf(L"config: `{}` is not a setting this build knows - the line has no effect, and Save "
                 L"keeps it where it is",
                 widen_ascii(key));
        }

        // Applies one config file's text onto `cfg` and returns how many `key = value` lines it understood.
        // The line rules are mirrored exactly in cfgkeys::keys_in().
        int apply_text(Config& cfg, const std::string& text)
        {
            int lines = 0;
            std::size_t pos = 0;
            // A UTF-8 BOM would otherwise glue itself to the first key's name.
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            {
                pos = 3;
            }
            while (pos <= text.size())
            {
                const std::size_t nl = text.find('\n', pos);
                std::string_view raw =
                    std::string_view{text}.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
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

                // A key that is a hard-coded constant now: named rather than ignored in silence.
                if (cfgkeys::is_removed(key))
                {
                    warn_once(key,
                              std::format(L"config: `{}` is not a setting (it is a hard-coded sanity "
                                          L"cap) - the line is ignored and can be deleted",
                                          widen_ascii(key)));
                    continue;
                }
                if (const char* to = cfgkeys::renamed_to(key); to != nullptr)
                {
                    warn_once(key,
                              std::format(L"config: `{}` is now called `{}` - the old name still "
                                          L"works, but please rename it",
                                          widen_ascii(key),
                                          widen_ascii(to)));
                }
                else if (cfgkeys::find(key) == nullptr)
                {
                    warn_unknown_once(key);
                }

                ++lines;
                apply_setting(cfg, key, value);
            }
            return lines;
        }

        // Theme precedence: a theme supplies every colour the player has not personally picked - a key
        // the config files never mention, or one whose value is exactly what some built-in theme sets.
        // The second half is what lets `theme` be edited by hand: Save writes all five colour keys out,
        // so "the file is silent" alone would make the key inert. `seen` is every key name that appeared
        // in either file, and this runs after both files are parsed.
        void apply_theme_defaults(Config& cfg, const std::vector<std::string>& seen)
        {
            constexpr gly::Theme kThemes[] = {gly::Theme::Neutral, gly::Theme::Ink};

            const auto mentioned = [&seen](std::string_view k) {
                for (const std::string& s : seen)
                {
                    if (s == k)
                    {
                        return true;
                    }
                }
                return false;
            };
            // The file round-trips channels through "{:.0f}" and alphas through "{:.2f}", so the
            // comparison is against the printed precision rather than the bit pattern.
            const auto about_eq = [](float a, float b, float eps) {
                return (a - b) < eps && (b - a) < eps;
            };
            const auto same_rgb = [&about_eq](float r, float g, float b, const mdb::Rgb& c) {
                return about_eq(r, static_cast<float>(c.r), 0.5f) &&
                       about_eq(g, static_cast<float>(c.g), 0.5f) &&
                       about_eq(b, static_cast<float>(c.b), 0.5f);
            };

            bool frame_custom = mentioned("minimap_frame_color");
            bool frame_alpha_custom = mentioned("minimap_frame_alpha");
            bool backdrop_custom = mentioned("minimap_backdrop_color");
            bool backdrop_alpha_custom = mentioned("minimap_backdrop");
            bool floor_custom = mentioned("floor_base_color");
            for (const gly::Theme t : kThemes)
            {
                const gly::ThemeColors c = gly::theme_colors(t);
                frame_custom = frame_custom && !same_rgb(cfg.minimap_frame_r, cfg.minimap_frame_g,
                                                         cfg.minimap_frame_b, c.frame);
                frame_alpha_custom =
                    frame_alpha_custom && !about_eq(cfg.minimap_frame_alpha, c.frame_alpha, 0.005f);
                backdrop_custom = backdrop_custom && !same_rgb(cfg.minimap_backdrop_r,
                                                               cfg.minimap_backdrop_g,
                                                               cfg.minimap_backdrop_b, c.backdrop);
                backdrop_alpha_custom =
                    backdrop_alpha_custom && !about_eq(cfg.minimap_backdrop, c.backdrop_alpha, 0.005f);
                floor_custom = floor_custom && !same_rgb(cfg.floor_base_r, cfg.floor_base_g,
                                                         cfg.floor_base_b, c.floor_base);
            }

            const gly::ThemeColors tc = gly::theme_colors(cfg.theme);
            if (!frame_custom)
            {
                cfg.minimap_frame_r = static_cast<float>(tc.frame.r);
                cfg.minimap_frame_g = static_cast<float>(tc.frame.g);
                cfg.minimap_frame_b = static_cast<float>(tc.frame.b);
            }
            if (!frame_alpha_custom)
            {
                cfg.minimap_frame_alpha = tc.frame_alpha;
            }
            if (!backdrop_custom)
            {
                cfg.minimap_backdrop_r = static_cast<float>(tc.backdrop.r);
                cfg.minimap_backdrop_g = static_cast<float>(tc.backdrop.g);
                cfg.minimap_backdrop_b = static_cast<float>(tc.backdrop.b);
            }
            if (!backdrop_alpha_custom)
            {
                cfg.minimap_backdrop = tc.backdrop_alpha;
            }
            if (!floor_custom)
            {
                cfg.floor_base_r = static_cast<float>(tc.floor_base.r);
                cfg.floor_base_g = static_cast<float>(tc.floor_base.g);
                cfg.floor_base_b = static_cast<float>(tc.floor_base.b);
            }
            // The item-quality tiers follow the PALETTE, not the theme.
            if (!mentioned("xray_rarity_colors"))
            {
                const mdb::Rgb* src = gly::rarity_colors(cfg.palette);
                for (int i = 0; i < mdb::kRarityCount; ++i)
                {
                    cfg.xray_rarity_colors[i] = src[i];
                }
            }
        }

        void clamp_config(Config& cfg)
        {
            // Every numeric key is clamped: a hand-edited file must not divide by zero, allocate unboundedly or stall the game thread.
            cfg.font_size = (std::max)(8, (std::min)(48, cfg.font_size));
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
            // 0 is meaningful here: never draw a floor above the player.
            cfg.floor_fade_above_uu = (std::max)(0.0f, (std::min)(20000.0f, cfg.floor_fade_above_uu));
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
            cfg.markers_absence_rounds = (std::max)(1, (std::min)(30, cfg.markers_absence_rounds));
            cfg.markers_absence_categories &= mdb::kAllCats;
            // 40 is lbl::Layout::kMaxRects; past it the overlap pass has nowhere to put a box and the caller draws the glyph alone.
            cfg.highlight_labels_max = (std::max)(0, (std::min)(40, cfg.highlight_labels_max));
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
            cfg.minimap_backdrop = (std::max)(0.0f, (std::min)(1.0f, cfg.minimap_backdrop));
            cfg.minimap_backdrop_r = (std::max)(0.0f, (std::min)(255.0f, cfg.minimap_backdrop_r));
            cfg.minimap_backdrop_g = (std::max)(0.0f, (std::min)(255.0f, cfg.minimap_backdrop_g));
            cfg.minimap_backdrop_b = (std::max)(0.0f, (std::min)(255.0f, cfg.minimap_backdrop_b));
            cfg.minimap_frame_r = (std::max)(0.0f, (std::min)(255.0f, cfg.minimap_frame_r));
            cfg.minimap_frame_g = (std::max)(0.0f, (std::min)(255.0f, cfg.minimap_frame_g));
            cfg.minimap_frame_b = (std::max)(0.0f, (std::min)(255.0f, cfg.minimap_frame_b));
            cfg.minimap_frame_alpha = (std::max)(0.0f, (std::min)(1.0f, cfg.minimap_frame_alpha));
            cfg.minimap_composite_alpha = (std::max)(0.0f, (std::min)(1.0f, cfg.minimap_composite_alpha));
            cfg.minimap_min_px = (std::max)(16.0f, (std::min)(512.0f, cfg.minimap_min_px));
            cfg.minimap_arrow_frac = (std::max)(0.01f, (std::min)(0.3f, cfg.minimap_arrow_frac));
            cfg.minimap_arrow_min_px = (std::max)(2.0f, (std::min)(64.0f, cfg.minimap_arrow_min_px));
            cfg.waypoint_size_scale = (std::max)(0.2f, (std::min)(4.0f, cfg.waypoint_size_scale));
            cfg.reader_position_period_ms = (std::max)(16, (std::min)(1000, cfg.reader_position_period_ms));
            cfg.reader_resolve_period_ms = (std::max)(100, (std::min)(10000, cfg.reader_resolve_period_ms));
            cfg.reader_widget_sweep_period_ms = (std::max)(50, (std::min)(5000, cfg.reader_widget_sweep_period_ms));
            cfg.reader_widget_sweep_max_period_ms =
                (std::max)(cfg.reader_widget_sweep_period_ms,
                           (std::min)(30000, cfg.reader_widget_sweep_max_period_ms));
            cfg.reader_widget_sweep_warm_ms = (std::max)(0, (std::min)(60000, cfg.reader_widget_sweep_warm_ms));
            cfg.reader_transition_cooldown_ms = (std::max)(0, (std::min)(30000, cfg.reader_transition_cooldown_ms));
            cfg.reader_teleport_jump_uu = (std::max)(200.0, (std::min)(100000.0, cfg.reader_teleport_jump_uu));
            cfg.reader_chapter_period_ms = (std::max)(200, (std::min)(60000, cfg.reader_chapter_period_ms));
            cfg.reader_log_throttle_ms = (std::max)(500, (std::min)(600000, cfg.reader_log_throttle_ms));
            cfg.markers_live_grace_rounds = (std::max)(1, (std::min)(30, cfg.markers_live_grace_rounds));
            cfg.map_asset_retire_grace_ms = (std::max)(0, (std::min)(60000, cfg.map_asset_retire_grace_ms));
            cfg.hide_reason_log_ms = (std::max)(0, (std::min)(600000, cfg.hide_reason_log_ms));
            cfg.srv_heap_size = (std::max)(16, (std::min)(1024, cfg.srv_heap_size));
            cfg.highlight_camera_resolve_ms = (std::max)(100, (std::min)(10000, cfg.highlight_camera_resolve_ms));
            cfg.highlight_compass_period_ms = (std::max)(10, (std::min)(1000, cfg.highlight_compass_period_ms));
            cfg.highlight_getter_period_ms = (std::max)(10, (std::min)(1000, cfg.highlight_getter_period_ms));
            cfg.highlight_pov_scan_bytes = (std::max)(64, (std::min)(1024, cfg.highlight_pov_scan_bytes));
            cfg.highlight_pov_bad_reads = (std::max)(1, (std::min)(64, cfg.highlight_pov_bad_reads));
            cfg.compass_tick_step_deg = (std::max)(1.0f, (std::min)(90.0f, cfg.compass_tick_step_deg));
            cfg.compass_max_pips = (std::max)(0, (std::min)(256, cfg.compass_max_pips));
            cfg.compass_pip_height_uu =
                (std::max)(0.0f, (std::min)(10000.0f, cfg.compass_pip_height_uu));
        }
    } // namespace

    void load_config_file()
    {
        if (g_pf_config < 0)
        {
            g_pf_config = perf_register("config refresh", perf::Thread::Unknown);
        }
        Config cfg{};
        std::string text;
        const std::wstring path = config_path();
        if (!read_whole_file(path, text))
        {
            logf(L"config: {} not found - using defaults (a file is written when you press Save in the F2 panel)",
                 path);
            apply_theme_defaults(cfg, std::vector<std::string>{});
            clamp_config(cfg);
            set_config(cfg);
            return;
        }

        const int lines = apply_text(cfg, text);
        std::vector<std::string> seen = cfgkeys::keys_in(text);

        // The dev overlay, loaded second so it overrides the shipped file.
        int dev_lines = 0;
        bool have_dev = false;
        std::string dev_text;
        if (read_whole_file(dev_config_path(), dev_text))
        {
            have_dev = true;
            dev_lines = apply_text(cfg, dev_text);
            for (std::string& k : cfgkeys::keys_in(dev_text))
            {
                seen.push_back(std::move(k));
            }
        }

        apply_theme_defaults(cfg, seen);
        clamp_config(cfg);
        set_config(cfg);
        if (have_dev)
        {
            g_dev_config_active.store(true, std::memory_order_relaxed);
        }
        if (have_dev)
        {
            logf(L"config: loaded {} setting(s) from {} + {} dev setting(s) from {}", lines, path, dev_lines,
                 dev_config_path());
        }
        else
        {
            logf(L"config: loaded {} setting(s) from {}", lines, path);
        }
    }

    // Writing: the config as key -> value. cfgkeys decides which file each key belongs in and
    // config_rewrite.hpp owns the layout. Every key in cfgkeys::kKeys of tier Player / Advanced / Dev
    // must appear here exactly once - one that does not is never saved.

    std::vector<std::pair<std::string, std::string>> config_kv(const Config& cfg)
    {
        std::vector<std::pair<std::string, std::string>> kv;
        kv.reserve(cfgkeys::kKeyCount);
        const auto add = [&kv](const char* k, std::string v) { kv.emplace_back(k, std::move(v)); };
        const auto b = [](bool v) { return std::string(v ? "1" : "0"); };
        const auto f0 = [](float v) { return std::format("{:.0f}", v); };
        const auto f1 = [](float v) { return std::format("{:.1f}", v); };
        const auto f2 = [](float v) { return std::format("{:.2f}", v); };
        const auto f3 = [](float v) { return std::format("{:.3f}", v); };
        const auto rgb = [](float r, float g, float bb) {
            return std::format("{:.0f} {:.0f} {:.0f}", r, g, bb);
        };
        const auto vk = [](int v) { return vk_name(v); };

        add("mod_enabled", b(cfg.mod_enabled));
        add("show_minimap", b(cfg.show_minimap));
        add("ui_scale", cfg.ui_scale_auto ? std::string{"auto"} : f2(cfg.ui_scale));
        add("font_size", std::to_string(cfg.font_size));
        add("hud_preset", preset_name(cfg.hud_preset));
        add("theme", gly::theme_name(cfg.theme));
        add("palette", gly::palette_name(cfg.palette));
        add("map_unreachable", srule::unreachable_name(cfg.map_unreachable));
        add("minimap_size", f3(cfg.size_frac));
        add("minimap_zoom", f1(cfg.zoom_uu_per_px));
        add("minimap_zoom_presets", [&cfg] {
            std::string out;
            for (int i = 0; i < cfg.minimap_zoom_preset_count && i < mv::kMaxZoomPresets; ++i)
            {
                if (!out.empty())
                {
                    out += ", ";
                }
                out += std::format("{:.0f}", cfg.minimap_zoom_presets[i]);
            }
            return out;
        }());
        add("minimap_shape", cfg.round ? "round" : "square");
        add("minimap_anchor", anchor_name(cfg.anchor));
        add("minimap_offset_x", f0(cfg.offset_x));
        add("minimap_offset_y", f0(cfg.offset_y));
        add("rotate_with_player", b(cfg.rotate_with_player));
        add("opacity", f2(cfg.opacity));
        add("hide_in_menus", b(cfg.hide_in_menus));
        add("show_adjacent_floors", b(cfg.show_adjacent_floors));
        add("floor_z_tolerance", f0(cfg.floor_z_tolerance));
        add("markers_enabled", b(cfg.markers_enabled));
        add("markers_categories", mdb::format_category_mask(cfg.markers_categories));
        add("markers_hide_found", b(cfg.markers_hide_found));
        add("markers_size", f1(cfg.markers_size));
        add("markers_clamp_to_edge", b(cfg.markers_clamp_to_edge));
        add("found_profile", std::string{cfg.found_profile});
        add("first_run_toast", b(cfg.first_run_toast));
        add("map_zoom", f0(cfg.map_zoom));
        add("map_marker_size", f1(cfg.map_marker_size));
        add("map_show_all_floors", b(cfg.map_show_all_floors));
        add("map_gamepad", b(cfg.map_gamepad));
        add("highlight_enabled", b(cfg.highlight_enabled));
        add("highlight_mode", std::string{cfg.highlight_mode == HighlightMode::Hold ? "hold" : "toggle"});
        add("highlight_key", vk(cfg.highlight_key));
        add("highlight_gamepad", b(cfg.highlight_gamepad));
        {
            const std::wstring chord =
                pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt);
            std::string narrow;
            narrow.reserve(chord.size());
            for (const wchar_t c : chord)
            {
                narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            add("highlight_pad_chord", narrow);
        }
        add("highlight_radius", f0(cfg.highlight_radius));
        add("highlight_categories", mdb::format_category_mask(cfg.highlight_categories));
        add("highlight_labels", b(cfg.highlight_labels));
        add("highlight_size", f1(cfg.highlight_size));
        add("xray_rarity_colors_enabled", b(cfg.xray_rarity_colors_enabled));
        add("markers_rarity_tint", b(cfg.markers_rarity_tint));
        add("compass_enabled", b(cfg.compass_enabled));
        add("compass_width", f3(cfg.compass_width));
        add("compass_plate", b(cfg.compass_plate));
        add("compass_offset_y", f0(cfg.compass_offset_y));
        add("compass_span_deg", f0(cfg.compass_span_deg));
        add("compass_opacity", f2(cfg.compass_opacity));
        add("compass_categories", mdb::format_category_mask(cfg.compass_categories));
        add("compass_pip_labels", b(cfg.compass_pip_labels));
        add("map_pad_open_chord", [&cfg] {
            const std::wstring chord = pad_chord_name(cfg.map_pad_open_chord, false, false);
            std::string narrow;
            narrow.reserve(chord.size());
            for (const wchar_t c : chord)
            {
                narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return narrow;
        }());
        add("panel_pad_open_chord", [&cfg] {
            const std::wstring chord = pad_chord_name(cfg.panel_pad_open_chord, false, false);
            std::string narrow;
            narrow.reserve(chord.size());
            for (const wchar_t c : chord)
            {
                narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return narrow;
        }());
        add("panel_key", vk(cfg.panel_key));
        add("map_key", vk(cfg.map_key));
        add("map_recenter_key", vk(cfg.map_recenter_key));
        add("zoom_key", vk(cfg.zoom_key));
        add("reload_key", vk(cfg.reload_key));
        add("screenshot_key", vk(cfg.screenshot_key));
        add("waypoint_nearest_key", vk(cfg.waypoint_nearest_key));

        add("require_pawn_view", b(cfg.require_pawn_view));
        add("state_stale_ms", std::to_string(cfg.state_stale_ms));
        add("min_visible_after_state_ok_ms", std::to_string(cfg.min_visible_after_state_ok_ms));
        add("menu_close_show_delay_ms", std::to_string(cfg.menu_close_show_delay_ms));
        add("adjacent_floor_opacity", f2(cfg.adjacent_floor_opacity));
        add("floor_fade_uu", f0(cfg.floor_fade_uu));
        add("floor_fade_above_uu", f0(cfg.floor_fade_above_uu));
        add("floor_gradient_strength", f2(cfg.floor_gradient_strength));
        add("floor_base_color", rgb(cfg.floor_base_r, cfg.floor_base_g, cfg.floor_base_b));
        add("slice_hz", std::to_string(cfg.slice_hz));
        add("feet_z_smooth_ms", std::to_string(cfg.feet_z_smooth_ms));
        add("player_z_offset", f0(cfg.player_z_offset));
        add("markers_live", b(cfg.markers_live));
        add("markers_filter_chapter", b(cfg.markers_filter_chapter));
        add("markers_rounds_per_sec", std::to_string(cfg.markers_rounds_per_sec));
        add("markers_scan_chunk", std::to_string(cfg.markers_scan_chunk));
        add("markers_scan_period_ms", std::to_string(cfg.markers_scan_period_ms));
        add("markers_found_alpha", f2(cfg.markers_found_alpha));
        add("markers_max_draw", std::to_string(cfg.markers_max_draw));
        add("found_save_debounce_ms", std::to_string(cfg.found_save_debounce_ms));
        add("markers_absence_rounds", std::to_string(cfg.markers_absence_rounds));
        add("boss_defeat_from_save", b(cfg.boss_defeat_from_save));
        add("markers_absence_categories", mdb::format_category_mask(cfg.markers_absence_categories));
        add("minimap_backdrop", f2(cfg.minimap_backdrop));
        add("minimap_backdrop_color",
            rgb(cfg.minimap_backdrop_r, cfg.minimap_backdrop_g, cfg.minimap_backdrop_b));
        add("minimap_frame_color", rgb(cfg.minimap_frame_r, cfg.minimap_frame_g, cfg.minimap_frame_b));
        add("minimap_frame_alpha", f2(cfg.minimap_frame_alpha));
        add("minimap_min_px", f0(cfg.minimap_min_px));
        add("minimap_arrow_frac", f3(cfg.minimap_arrow_frac));
        add("minimap_arrow_min_px", f0(cfg.minimap_arrow_min_px));
        add("waypoint_size_scale", f2(cfg.waypoint_size_scale));
        add("map_zoom_min", f0(cfg.map_zoom_min));
        add("map_zoom_max", f0(cfg.map_zoom_max));
        add("map_zoom_factor", f2(cfg.map_zoom_factor));
        add("map_pan_speed", f0(cfg.map_pan_speed));
        add("map_margin", f3(cfg.map_margin));
        add("map_backdrop", f2(cfg.map_backdrop));
        add("map_markers_max_draw", std::to_string(cfg.map_markers_max_draw));
        add("map_floor_step", f0(cfg.map_floor_step));
        add("map_slice_px", std::to_string(cfg.map_slice_px));
        add("map_slice_hz", std::to_string(cfg.map_slice_hz));
        add("map_gamepad_deadzone", f2(cfg.map_gamepad_deadzone));
        add("highlight_show_found", b(cfg.highlight_show_found));
        add("highlight_max_draw", std::to_string(cfg.highlight_max_draw));
        add("highlight_labels_max", std::to_string(cfg.highlight_labels_max));
        add("highlight_alpha_near", f2(cfg.highlight_alpha_near));
        add("highlight_alpha_far", f2(cfg.highlight_alpha_far));
        add("highlight_edge_arrows", b(cfg.highlight_edge_arrows));
        add("highlight_camera_hz", std::to_string(cfg.highlight_camera_hz));
        add("xray_rarity_colors", mdb::format_rarity_colors(cfg.xray_rarity_colors));
        add("compass_anchor", cfg.compass_anchor == VAnchor::Bottom ? "bottom" : "top");
        add("compass_height", f0(cfg.compass_height));
        add("compass_marker_distance", f0(cfg.compass_marker_distance));
        add("compass_show_waypoint", b(cfg.compass_show_waypoint));
        add("compass_tick_step_deg", f0(cfg.compass_tick_step_deg));
        add("compass_max_pips", std::to_string(cfg.compass_max_pips));
        add("compass_pip_height_uu", f0(cfg.compass_pip_height_uu));
        add("log_level", log_level_name(cfg.log_level));
        add("crash_breadcrumb", b(cfg.crash_breadcrumb));
        add("ui_font", std::string{cfg.ui_font});
        add("zoom_dpi_scaled", b(cfg.zoom_dpi_scaled));

        add("debug_readout", b(cfg.debug_readout));
        add("debug_show_panel_on_start", b(cfg.debug_show_panel_on_start));
        add("fallback_use_composite", b(cfg.fallback_use_composite));
        add("minimap_composite_alpha", f2(cfg.minimap_composite_alpha));
        add("reader_position_period_ms", std::to_string(cfg.reader_position_period_ms));
        add("reader_resolve_period_ms", std::to_string(cfg.reader_resolve_period_ms));
        add("reader_widget_sweep_period_ms", std::to_string(cfg.reader_widget_sweep_period_ms));
        add("reader_widget_sweep_max_period_ms", std::to_string(cfg.reader_widget_sweep_max_period_ms));
        add("reader_widget_sweep_warm_ms", std::to_string(cfg.reader_widget_sweep_warm_ms));
        add("menu_ignore_roots", std::string{cfg.menu_ignore_roots});
        add("reader_transition_cooldown_ms", std::to_string(cfg.reader_transition_cooldown_ms));
        add("reader_teleport_jump_uu", std::format("{:.0f}", cfg.reader_teleport_jump_uu));
        add("reader_chapter_period_ms", std::to_string(cfg.reader_chapter_period_ms));
        add("reader_log_throttle_ms", std::to_string(cfg.reader_log_throttle_ms));
        add("markers_live_grace_rounds", std::to_string(cfg.markers_live_grace_rounds));
        add("map_asset_retire_grace_ms", std::to_string(cfg.map_asset_retire_grace_ms));
        add("hide_reason_log_ms", std::to_string(cfg.hide_reason_log_ms));
        add("srv_heap_size", std::to_string(cfg.srv_heap_size));
        add("navmesh_dump", b(cfg.navmesh_dump));
        add("highlight_camera_resolve_ms", std::to_string(cfg.highlight_camera_resolve_ms));
        add("highlight_compass_period_ms", std::to_string(cfg.highlight_compass_period_ms));
        add("highlight_getter_period_ms", std::to_string(cfg.highlight_getter_period_ms));
        add("highlight_pov_scan_bytes", std::to_string(cfg.highlight_pov_scan_bytes));
        add("highlight_pov_bad_reads", std::to_string(cfg.highlight_pov_bad_reads));
        add("saveslot_uuid_call", b(cfg.saveslot_uuid_call));

        return kv;
    }

    namespace
    {
        // The values of one tier as plain `key = value` lines. Used only when the file does not exist yet.
        std::string tier_block(const std::vector<std::pair<std::string, std::string>>& kv,
                               cfgkeys::Tier tier)
        {
            std::string out;
            for (const auto& [k, v] : kv)
            {
                if (cfgkeys::tier_is(k, tier))
                {
                    out += k + " = " + v + "\r\n";
                }
            }
            return out;
        }

        constexpr const char* kAppendedBanner = "; ---- added by the settings panel ----";

        void log_config_write(const std::wstring& path, bool ok, int changed, int appended)
        {
            if (ok)
            {
                logf(L"config: saved -> {} ({} value(s) changed, {} key(s) appended)", path, changed, appended);
            }
            else
            {
                logf(L"config: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
            }
        }

        // The VALUES of `existing`'s `key = value` lines, with anything missing appended once under
        // kAppendedBanner. `existing` is the caller's read of `path`, so a partial save never has to
        // re-read a file that could be gone by now.
        void rewrite_config(const std::wstring& path, const std::string& existing,
                            const std::vector<cfgrw::Pair>& mine)
        {
            const cfgrw::Result r = cfgrw::rewrite(existing, mine, kAppendedBanner);
            log_config_write(path, write_whole_file(path, r.text), r.changed, r.appended);
        }

        // Writes `mine` into `path`. On an existing file only the VALUES of its `key = value` lines change and
        // anything missing is appended once under kAppendedBanner; otherwise `fresh` is written verbatim.
        void write_config(const std::wstring& path, const std::vector<cfgrw::Pair>& mine,
                          const std::string& fresh)
        {
            std::string existing;
            if (read_whole_file(path, existing))
            {
                rewrite_config(path, existing, mine);
                return;
            }
            log_config_write(path, write_whole_file(path, fresh), 0, static_cast<int>(mine.size()));
        }

        // Does any Dev key differ from its built-in default?
        bool dev_values_differ(const std::vector<std::pair<std::string, std::string>>& kv)
        {
            const Config defaults{};
            const std::vector<std::pair<std::string, std::string>> base = config_kv(defaults);
            for (const auto& [k, v] : kv)
            {
                if (!cfgkeys::tier_is(k, cfgkeys::Tier::Dev))
                {
                    continue;
                }
                const std::string* d = cfgrw::lookup(base, k);
                if (d != nullptr && *d != v)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    void save_config_file()
    {
        const Config cfg = config();
        const std::vector<std::pair<std::string, std::string>> kv = config_kv(cfg);

        {
            const std::vector<cfgrw::Pair> mine = cfgrw::filter(kv, [](const std::string& k) {
                return cfgkeys::tier_is(k, cfgkeys::Tier::Player) ||
                       cfgkeys::tier_is(k, cfgkeys::Tier::Advanced);
            });
            std::string fresh;
            fresh += "; WuchangMinimap settings. Written by the F2 panel; hand edits are picked up with F5.\r\n";
            fresh += "; Only the VALUES on `key = value` lines are ever rewritten - comments, ordering and\r\n";
            fresh += "; keys this build does not know are left exactly as they are.\r\n";
            fresh += "; Hotkeys may only be F1..F5, F7 or F8: F6 is the RenoDX DLSS5 toggle, F9/F11 engine\r\n";
            fresh += "; binds, F10 the game console and F12 the Steam screenshot key.\r\n\r\n";
            fresh += "; ---- PLAYER SETTINGS ----\r\n";
            fresh += tier_block(kv, cfgkeys::Tier::Player);
            fresh += "\r\n; ---- ADVANCED ----\r\n";
            fresh += "; Correct as shipped. Change one to answer a symptom, not for fun.\r\n";
            fresh += tier_block(kv, cfgkeys::Tier::Advanced);
            write_config(config_path(), mine, fresh);
        }

        // The dev file: written when it already exists, or when some Dev key has been moved off its built-in
        // default. Dev keys never leak into the player file - the filter above cannot see them.
        std::string dev_existing;
        const bool have_dev = read_whole_file(dev_config_path(), dev_existing);
        // What the F2 Save button's label promises. Set BEFORE the write.
        g_dev_config_active.store(have_dev || dev_values_differ(kv), std::memory_order_relaxed);
        if (have_dev || dev_values_differ(kv))
        {
            const std::vector<cfgrw::Pair> mine = cfgrw::filter(
                kv, [](const std::string& k) { return cfgkeys::tier_is(k, cfgkeys::Tier::Dev); });
            std::string fresh;
            fresh += "; WuchangMinimap DEVELOPER settings - not shipped in the release zip.\r\n";
            fresh += "; Read only if this file exists, and AFTER config_wuchang_minimap.txt, so a key set\r\n";
            fresh += "; in both wins here. Delete the file to go back to the built-in defaults.\r\n\r\n";
            fresh += tier_block(kv, cfgkeys::Tier::Dev);
            write_config(dev_config_path(), mine, fresh);
        }
    }

    // The master switch, read straight off disk. NOT load_config_file(): no other key is applied on the way
    // back in. `modswitch` calls load_config_file() itself once it has decided to turn the mod on.

    bool peek_mod_enabled(bool& out)
    {
        std::string text;
        if (!read_whole_file(config_path(), text))
        {
            return false;
        }
        std::size_t pos = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            pos = 3;
        }
        bool seen = false;
        while (pos <= text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            std::string_view raw =
                std::string_view{text}.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
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
            if (trim(raw.substr(0, eq)) != "mod_enabled")
            {
                continue;
            }
            // The LAST occurrence wins, exactly as it does in load_config_file().
            out = parse_bool(trim(raw.substr(eq + 1)), out);
            seen = true;
        }
        return seen;
    }

    namespace
    {
        std::uint64_t file_mtime(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0)
            {
                return 0;
            }
            return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                   data.ftLastWriteTime.dwLowDateTime;
        }
    } // namespace

    // BOTH files, mixed into one number, so the 1 Hz watcher in modswitch reloads both when either is edited.
    std::uint64_t config_mtime()
    {
        const std::uint64_t main_ft = file_mtime(config_path());
        const std::uint64_t dev_ft = file_mtime(dev_config_path());
        if (main_ft == 0 && dev_ft == 0)
        {
            return 0;
        }
        return main_ft ^ (dev_ft * 0x9E3779B97F4A7C15ull);
    }

    mv::WaypointSet waypoints()
    {
        spin::SpinGuard guard(g_wp_lock);
        return g_wp;
    }

    void set_waypoints(const mv::WaypointSet& set)
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            g_wp = set;
            if (g_wp.count > mv::kMaxWaypoints)
            {
                g_wp.count = mv::kMaxWaypoints;
            }
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    bool add_waypoint(const mv::Waypoint& wp)
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            if (g_wp.count >= mv::kMaxWaypoints)
            {
                return false;
            }
            g_wp.items[g_wp.count] = wp;
            g_wp.items[g_wp.count].set = true;
            ++g_wp.count;
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
        return true;
    }

    void remove_waypoint(std::size_t index)
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            if (index >= g_wp.count)
            {
                return;
            }
            for (std::size_t i = index + 1; i < g_wp.count; ++i)
            {
                g_wp.items[i - 1] = g_wp.items[i];
            }
            --g_wp.count;
            g_wp.items[g_wp.count] = mv::Waypoint{};
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    void clear_waypoints()
    {
        {
            spin::SpinGuard guard(g_wp_lock);
            g_wp = mv::WaypointSet{};
        }
        g_waypoint_dirty.store(true, std::memory_order_release);
    }

    namespace
    {
        std::string g_wp_key;        // "" = the shared file
        bool g_wp_key_valid = false; // has a key ever been taken from slotid?

        std::wstring waypoint_path_for(const std::string& key)
        {
            const std::string name = slotid::waypoint_filename(key);
            std::wstring wide;
            wide.reserve(name.size());
            for (char c : name)
            {
                wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return mod_dir() + L"\\" + wide;
        }

        bool wp_file_exists(const std::wstring& path)
        {
            return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // The waypoint half of the found tracker's legacy reconcile: builds before the
        // save key became the slot name alone also wrote `<steam account id>_<slot>`
        // waypoint files. Each one's waypoints are added to the canonical set (places
        // already in it are not duplicated, and the set stops at mv::kMaxWaypoints) and
        // the legacy file is removed. Loop thread.
        void reconcile_legacy_waypoints(const std::string& key)
        {
            if (key.empty())
            {
                return;
            }
            const std::wstring dir = mod_dir();
            std::wstring wpattern;
            for (char c : std::string{slotid::kWaypointPrefix} + "*_" + key + ".txt")
            {
                wpattern.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            std::vector<std::string> legacy_names;
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((dir + L"\\" + wpattern).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        continue;
                    }
                    std::string name;
                    for (const wchar_t* p = fd.cFileName; *p != 0; ++p)
                    {
                        name.push_back(*p < 128 ? static_cast<char>(*p) : '?');
                    }
                    const std::string cand = slotid::key_in_filename(name, slotid::kWaypointPrefix);
                    if (slotid::is_legacy_account_key(key, cand))
                    {
                        legacy_names.push_back(name);
                    }
                } while (::FindNextFileW(h, &fd) != 0 && legacy_names.size() < 16);
                ::FindClose(h);
            }
            if (legacy_names.empty())
            {
                return;
            }
            const std::wstring dst = waypoint_path_for(key);
            mv::WaypointSet set{};
            std::string text;
            if (wp_file_exists(dst) && read_whole_file(dst, text))
            {
                mv::waypoints_parse(text, set);
            }
            for (const std::string& name : legacy_names)
            {
                std::wstring src = dir + L"\\";
                for (char c : name)
                {
                    src.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                }
                std::string legacy_text;
                mv::WaypointSet legacy{};
                if (!read_whole_file(src, legacy_text) || !mv::waypoints_parse(legacy_text, legacy))
                {
                    logf(L"waypoint: the legacy file {} could not be read - it is LEFT in place", src);
                    continue;
                }
                std::size_t added = 0;
                for (std::size_t i = 0; i < legacy.count; ++i)
                {
                    const mv::Waypoint& w = legacy.items[i];
                    if (set.count >= mv::kMaxWaypoints)
                    {
                        break;
                    }
                    const mv::WaypointToggleResult hit =
                        mv::waypoint_toggle_at(set, w.x, w.y, w.z, mv::kWaypointSamePlace);
                    if (hit.action != mv::WaypointToggle::Add)
                    {
                        continue;
                    }
                    set.items[set.count++] = w;
                    ++added;
                }
                if (!write_whole_file(dst, mv::waypoints_serialize(set)))
                {
                    logf(L"waypoint: could not merge the legacy file {} into {} (error {}) - both files "
                         L"are left alone",
                         src, dst, static_cast<unsigned>(::GetLastError()));
                    return;
                }
                logf(L"waypoint: merged the legacy file {} ({} waypoint(s), {} of them new) into {} and "
                     L"removed it",
                     src, legacy.count, added, dst);
                ::DeleteFileW(src.c_str());
            }
        }

        // First sight of a slot with no waypoint file of its own: seed it from the shared
        // one, as the found tracker does. The copy is itself the "already seeded" mark -
        // once the file exists this is a no-op.
        void seed_waypoints_from_shared(const std::string& key)
        {
            if (key.empty())
            {
                return;
            }
            const std::wstring dst = waypoint_path_for(key);
            if (wp_file_exists(dst))
            {
                return;
            }
            const std::wstring src = waypoint_path_for(std::string{});
            std::string text;
            if (!wp_file_exists(src) || !read_whole_file(src, text) || text.empty())
            {
                return;
            }
            if (write_whole_file(dst, text))
            {
                logf(L"waypoint: first sight of save slot '{}' - copied the shared waypoints "
                     L"({} bytes) into {}",
                     std::wstring(key.begin(), key.end()), text.size(), dst);
            }
            else
            {
                logf(L"waypoint: could not seed {} from the shared file (error {})", dst,
                     static_cast<unsigned>(::GetLastError()));
            }
        }

        // Takes whatever key slotid has resolved. Loop thread. True when the file changed
        // and the caller must reload it.
        bool adopt_waypoint_key()
        {
            const slotid::Status st = slotid::status();
            const std::string key{st.key};
            if (g_wp_key_valid && key == g_wp_key)
            {
                return false;
            }
            const bool first = !g_wp_key_valid;
            g_wp_key = key;
            g_wp_key_valid = true;
            reconcile_legacy_waypoints(key);
            seed_waypoints_from_shared(key);
            logf(L"waypoint: profile {} '{}' -> {}", first ? L"=" : L"changed to",
                 std::wstring(key.begin(), key.end()), waypoint_path_for(key));
            return true;
        }
    } // namespace

    // The file the waypoints are READ from: the slot's own, falling back to the shared one
    // while the slot has none of its own.
    std::wstring waypoint_path()
    {
        const std::wstring path = waypoint_path_for(g_wp_key);
        if (!g_wp_key.empty() && !wp_file_exists(path))
        {
            const std::wstring shared = waypoint_path_for(std::string{});
            if (wp_file_exists(shared))
            {
                return shared;
            }
        }
        return path;
    }

    std::string waypoint_file_name()
    {
        return slotid::waypoint_filename(g_wp_key);
    }

    // The save-slot watch, mirroring the found tracker's: a pending write goes to the OLD
    // file first, because those waypoints belong to the save that was loaded when they
    // were dropped. Loop thread, 1 Hz.
    void waypoint_slot_poll()
    {
        static std::uint64_t last_check = 0;
        const std::uint64_t now = ::GetTickCount64();
        if (now - last_check < 1000)
        {
            return;
        }
        last_check = now;
        if (g_wp_key_valid && std::string{slotid::status().key} == g_wp_key)
        {
            return;
        }
        if (g_wp_key_valid && g_waypoint_dirty.load(std::memory_order_acquire))
        {
            save_waypoint_file();
        }
        if (adopt_waypoint_key())
        {
            // Clears the dirty flag: the pending set belonged to the previous file.
            load_waypoint_file();
        }
    }

    void load_waypoint_file()
    {
        if (!g_wp_key_valid)
        {
            adopt_waypoint_key();
        }
        const std::wstring path = waypoint_path();
        std::string text;
        mv::WaypointSet set{};
        if (read_whole_file(path, text))
        {
            if (!mv::waypoints_parse(text, set))
            {
                logf(L"waypoint: {} exists but carries no usable coordinates - ignored", path);
                set = mv::WaypointSet{};
            }
            else if (set.count != 0)
            {
                logf(L"waypoint: loaded {} waypoint(s) from {}", set.count, path);
            }
        }
        {
            spin::SpinGuard guard(g_wp_lock);
            g_wp = set;
        }
        // What was just read is what the file says, so nothing is pending.
        g_waypoint_dirty.store(false, std::memory_order_release);
    }

    void save_waypoint_file()
    {
        const mv::WaypointSet set = waypoints();
        // The slot's own file, never the shared one waypoint_path() may fall back to.
        const std::wstring path = waypoint_path_for(g_wp_key);
        if (!write_whole_file(path, mv::waypoints_serialize(set)))
        {
            logf(L"waypoint: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
            return;
        }
        logf(L"waypoint: saved {} waypoint(s)", set.count);
    }

    std::atomic<int> g_log_level{static_cast<int>(LogLv::Normal)};

    const char* log_level_name(LogLv lv) noexcept
    {
        switch (lv)
        {
        case LogLv::Verbose:
            return "verbose";
        case LogLv::Trace:
            return "trace";
        case LogLv::Normal:
        default:
            return "normal";
        }
    }

    bool log_level_from_name(std::string_view name, LogLv& out) noexcept
    {
        if (name == "normal")
        {
            out = LogLv::Normal;
            return true;
        }
        if (name == "verbose")
        {
            out = LogLv::Verbose;
            return true;
        }
        if (name == "trace")
        {
            out = LogLv::Trace;
            return true;
        }
        return false;
    }

    void set_loop_thread()
    {
        g_loop_thread = ::GetCurrentThreadId();
    }

    // The mod's own rolling log - wuchang_minimap.log, keeping the last four sessions through its own
    // `.1` / `.2` / `.3` rotation, because UE4SS truncates `UE4SS.log` on every launch.
    // Flat `CreateFileW` / `WriteFile` over a hand-built UTF-8 buffer: the game thread must never touch
    // C++ iostreams or the C++ locale. Writing is BUFFERED (8 KB) and flushed when the buffer fills, on
    // every crash breadcrumb write, and every few seconds from the loop thread.
    // `modlog_line()` runs on the loop thread only, but `modlog_flush()` is called from any thread, so the
    // buffer and the handle sit behind a spinlock of their OWN (g_modlog_lock), never the log queue's.
    // Nothing in here allocates while holding it.

    namespace
    {
        constexpr std::size_t kModLogFlushAt = 8192;
        // The longest line written verbatim. The buffer flushes at kModLogFlushAt and is reserved at twice that,
        // so buffer + line + cap notice never reach the reserve - `append` cannot reallocate under the lock.
        constexpr std::size_t kModLogMaxLine = 4096;
        // Per-session cap. At the cap one line says so and writing stops; the rotation is untouched.
        constexpr std::uint64_t kModLogMaxBytes = 20ull * 1024ull * 1024ull;
        constexpr const wchar_t* kModLogName = L"\\wuchang_minimap.log";

        // A lock of its own, not the log QUEUE's, so enqueueing a line from the game thread never waits on a file
        // syscall. `drain_log` swaps the queue out under `g_log_lock`, releases it, then writes.
        spin::Spinlock g_modlog_lock;
        HANDLE g_modlog = INVALID_HANDLE_VALUE;
        std::string g_modlog_buf;
        bool g_modlog_opened = false;
        bool g_modlog_capped = false;
        std::uint64_t g_modlog_bytes = 0;
        std::uint64_t g_modlog_last_flush_ms = 0;

        std::string utf8_of(const std::wstring& w)
        {
            if (w.empty())
            {
                return {};
            }
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                                   nullptr, 0, nullptr, nullptr);
            if (need <= 0)
            {
                return {};
            }
            std::string out;
            out.resize(static_cast<std::size_t>(need));
            ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                                  nullptr, nullptr);
            return out;
        }

        // Rotate, then create: `.3` is dropped, everything else shifts up one, and the live file is always `wuchang_minimap.log`.
        void modlog_rotate(const std::wstring& base)
        {
            const std::wstring p1 = base + L".1";
            const std::wstring p2 = base + L".2";
            const std::wstring p3 = base + L".3";
            ::DeleteFileW(p3.c_str());
            ::MoveFileExW(p2.c_str(), p3.c_str(), MOVEFILE_REPLACE_EXISTING);
            ::MoveFileExW(p1.c_str(), p2.c_str(), MOVEFILE_REPLACE_EXISTING);
            ::MoveFileExW(base.c_str(), p1.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        // Loop thread, lazily on the first line. FILE_SHARE_READ so the file can be read while the game is still running.
        void modlog_open_locked()
        {
            if (g_modlog_opened)
            {
                return;
            }
            g_modlog_opened = true; // one attempt per session, success or not
            const std::wstring base = mod_dir() + kModLogName;
            modlog_rotate(base);
            g_modlog = ::CreateFileW(base.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_modlog == INVALID_HANDLE_VALUE)
            {
                return;
            }
            g_modlog_buf.reserve(kModLogFlushAt * 2);
        }

        void modlog_write_locked()
        {
            if (g_modlog == INVALID_HANDLE_VALUE || g_modlog_buf.empty())
            {
                return;
            }
            DWORD written = 0;
            ::WriteFile(g_modlog, g_modlog_buf.data(), static_cast<DWORD>(g_modlog_buf.size()), &written,
                        nullptr);
            g_modlog_bytes += g_modlog_buf.size();
            g_modlog_buf.clear();
        }

        void modlog_line(const std::wstring& line)
        {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t stamp[32]{};
            ::_snwprintf_s(stamp, std::size(stamp), _TRUNCATE, L"%02u:%02u:%02u.%03u ",
                           static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                           static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
            std::string text = utf8_of(std::wstring{stamp} + line);
            // The buffer must not reallocate under g_modlog_lock: modlog_flush() takes that lock from the crash
            // breadcrumb and the stall watchdog. Truncating on a UTF-8 character boundary keeps buffer + line +
            // cap notice inside the 2 x kModLogFlushAt bytes reserved once at open time.
            if (text.size() > kModLogMaxLine)
            {
                std::size_t cut = kModLogMaxLine;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
                {
                    --cut;
                }
                text.resize(cut);
                text += " [line truncated]";
            }
            spin::SpinGuard guard(g_modlog_lock);
            modlog_open_locked();
            if (g_modlog == INVALID_HANDLE_VALUE)
            {
                return;
            }
            if (g_modlog_capped)
            {
                return;
            }
            g_modlog_buf.append(text);
            g_modlog_buf.append("\r\n");
            if (g_modlog_bytes + g_modlog_buf.size() >= kModLogMaxBytes)
            {
                g_modlog_buf.append("--- log capped at 20 MB for this session; nothing more is written "
                                    "to this file (the .1 / .2 / .3 rotation is unaffected) ---\r\n");
                modlog_write_locked();
                g_modlog_capped = true;
                return;
            }
            if (g_modlog_buf.size() >= kModLogFlushAt)
            {
                modlog_write_locked();
            }
        }
    } // namespace

    void modlog_flush()
    {
        spin::SpinGuard guard(g_modlog_lock);
        modlog_write_locked();
        if (g_modlog != INVALID_HANDLE_VALUE)
        {
            ::FlushFileBuffers(g_modlog);
        }
    }

    void modlog_tick(std::uint64_t now_ms)
    {
        if (now_ms - g_modlog_last_flush_ms < 3000)
        {
            return;
        }
        g_modlog_last_flush_ms = now_ms;
        modlog_flush();
    }

    // The game build the shipped data was dumped from: every marker coordinate and map picture came out of
    // ONE cooked build. The data files may carry a top-level `"game_build"` string, compared here against
    // the running executable's FILEVERSION. A missing stamp is verbose; a disagreeing one is a warning.

    namespace
    {
        bool g_game_build_checked = false;

        // The running executable's FILEVERSION as "a.b.c.d", or an empty string. No engine call.
        std::wstring exe_file_version()
        {
            wchar_t path[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))) == 0)
            {
                return {};
            }
            DWORD ignored = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(path, &ignored);
            if (size == 0)
            {
                return {};
            }
            std::vector<unsigned char> buf(size);
            if (::GetFileVersionInfoW(path, 0, size, buf.data()) == 0)
            {
                return {};
            }
            VS_FIXEDFILEINFO* info = nullptr;
            UINT len = 0;
            if (::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) == 0 ||
                info == nullptr || len < sizeof(VS_FIXEDFILEINFO))
            {
                return {};
            }
            return std::format(L"{}.{}.{}.{}", HIWORD(info->dwFileVersionMS),
                               LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                               LOWORD(info->dwFileVersionLS));
        }

        // The value of a top-level `"key": "..."` string, searched inside the first `kHeadBytes` only - these
        // files put their scalar header keys before the big arrays. No escape handling.
        bool json_head_string(const std::string& text, const char* key, std::string& out)
        {
            constexpr std::size_t kHeadBytes = 2048;
            const std::string needle = std::string{"\""} + key + "\"";
            const std::size_t head = text.size() < kHeadBytes ? text.size() : kHeadBytes;
            const std::size_t at = text.find(needle);
            if (at == std::string::npos || at >= head)
            {
                return false;
            }
            std::size_t i = text.find(':', at + needle.size());
            if (i == std::string::npos)
            {
                return false;
            }
            ++i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            {
                ++i;
            }
            if (i >= text.size() || text[i] != '"')
            {
                return false;
            }
            const std::size_t begin = i + 1;
            const std::size_t end = text.find('"', begin);
            if (end == std::string::npos || end - begin > 64)
            {
                return false;
            }
            out = text.substr(begin, end - begin);
            return true;
        }
    } // namespace

    void check_game_build()
    {
        if (g_game_build_checked)
        {
            return;
        }
        g_game_build_checked = true;

        // The two shipped manifests, in the order a mismatch matters: marker positions first, then the map pictures.
        const std::wstring files[2] = {mod_dir() + L"\\markers\\chapter1.json",
                                       mod_dir() + L"\\maps\\maps.json"};
        std::string stamped;
        std::wstring source;
        for (const std::wstring& path : files)
        {
            std::string text;
            if (!read_whole_file(path, text))
            {
                continue;
            }
            if (json_head_string(text, "game_build", stamped))
            {
                source = path;
                break;
            }
        }
        if (stamped.empty())
        {
            MM_LOGVS(L"data provenance: neither markers\\chapter1.json nor maps\\maps.json "
                     L"carries a \"game_build\" stamp, so the data cannot be checked against "
                     L"the running game build");
            return;
        }
        const std::wstring running = exe_file_version();
        std::wstring wide;
        wide.reserve(stamped.size());
        for (char c : stamped)
        {
            wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        if (running.empty())
        {
            MM_LOGV(L"data provenance: the shipped data was dumped from game build {}; this "
                    L"executable has no version info to compare it with",
                    wide);
            return;
        }
        if (running == wide)
        {
            MM_LOGV(L"data provenance: the shipped data matches this game build ({})", wide);
            return;
        }
        logf(L"data provenance: THIS GAME IS BUILD {} BUT THE MARKER / MAP DATA WAS DUMPED FROM "
             L"{} ({}). Markers can be in the wrong place or missing after a game patch - if "
             L"anything looks misplaced, that is the first thing to report.",
             running,
             wide,
             source);
    }

    std::wstring modlog_path()
    {
        return mod_dir() + kModLogName;
    }

    void log(const std::wstring& line)
    {
        if (g_loop_thread == 0 || ::GetCurrentThreadId() != g_loop_thread)
        {
            spin::SpinGuard guard(g_log_lock);
            if (g_log_queue.size() < kLogQueueMax)
            {
                g_log_queue.push_back(line);
            }
            else
            {
                ++g_log_dropped;
            }
            return;
        }
        Output::send<LogLevel::Verbose>(STR("[minimap] {}\n"), line);
        modlog_line(line);
    }

    void drain_log()
    {
        std::vector<std::wstring> lines;
        std::size_t dropped = 0;
        {
            spin::SpinGuard guard(g_log_lock);
            if (g_log_queue.empty() && g_log_dropped == 0)
            {
                return;
            }
            lines.swap(g_log_queue);
            dropped = g_log_dropped;
            g_log_dropped = 0;
        }
        if (dropped != 0)
        {
            const std::wstring note =
                std::format(L"log: dropped {} line(s) - the queue was full (the loop thread was not "
                            L"draining, or something is logging far too fast)",
                            dropped);
            Output::send<LogLevel::Warning>(STR("[minimap] {}\n"), note);
            modlog_line(note);
        }
        for (const std::wstring& l : lines)
        {
            Output::send<LogLevel::Verbose>(STR("[minimap] {}\n"), l);
            modlog_line(l);
        }
    }
} // namespace mm
