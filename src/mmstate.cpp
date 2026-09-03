#include "mmstate.hpp"

#include "atomicfile.hpp"
#include "config_keys.hpp"
#include "config_rewrite.hpp"

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
        // Lines the queue refused because it was already full. DROPPED, never blocked
        // and never grown: the game thread must not wait on the loop thread, and a
        // runaway diagnostic must not eat memory. The count is reported (and reset) by
        // the next drain, so a drop is always visible in the log rather than silent.
        constexpr std::size_t kLogQueueMax = 4096;
        std::size_t g_log_dropped = 0;

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

        // A read that fails on a file which EXISTS is not the same thing as a missing
        // file, and both used to come back as a bare `false` - so a config file held
        // open by something else looked exactly like a fresh install and the defaults
        // silently won. The distinction is made in mmfile::read_whole_file; this
        // wrapper keeps the old bool signature for the callers and says out loud, at
        // the normal log level, when data was dropped.
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

        // ATOMIC. See atomicfile.hpp: temp file, flush, rename. No backup for these -
        // the config and the waypoint are cheap to recreate; the found tracker (which
        // keeps one) is not.
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
            {"TAB", VK_TAB},          {"SPACE", VK_SPACE},        {"LALT", VK_LMENU},
            {"RALT", VK_RMENU},       {"ALT", VK_MENU},           {"LSHIFT", VK_LSHIFT},
            {"RSHIFT", VK_RSHIFT},    {"SHIFT", VK_SHIFT},        {"LCTRL", VK_LCONTROL},
            {"RCTRL", VK_RCONTROL},   {"CTRL", VK_CONTROL},
            // Arrows, the navigation block and Enter / Backspace: ordinary keys a
            // player may well prefer for a toggle.
            {"UP", VK_UP},            {"DOWN", VK_DOWN},          {"LEFT", VK_LEFT},
            {"RIGHT", VK_RIGHT},      {"ENTER", VK_RETURN},       {"BACKSPACE", VK_BACK},
            {"INSERT", VK_INSERT},    {"DELETE", VK_DELETE},      {"HOME", VK_HOME},
            {"END", VK_END},          {"PAGEUP", VK_PRIOR},       {"PAGEDOWN", VK_NEXT},
            // The numpad. Named apart from the digit row, because the two are different
            // virtual keys and a player who binds one means that one.
            {"NUM0", VK_NUMPAD0},     {"NUM1", VK_NUMPAD1},       {"NUM2", VK_NUMPAD2},
            {"NUM3", VK_NUMPAD3},     {"NUM4", VK_NUMPAD4},       {"NUM5", VK_NUMPAD5},
            {"NUM6", VK_NUMPAD6},     {"NUM7", VK_NUMPAD7},       {"NUM8", VK_NUMPAD8},
            {"NUM9", VK_NUMPAD9},     {"NUMPLUS", VK_ADD},        {"NUMMINUS", VK_SUBTRACT},
            {"NUMMUL", VK_MULTIPLY},  {"NUMDIV", VK_DIVIDE},      {"NUMDOT", VK_DECIMAL},
            // Mouse buttons 3-5 only. Left and right belong to the game and to the
            // overlay's own clicks; taking either of them away would break both.
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
                     L"a single letter or digit, TAB, SPACE, ENTER, BACKSPACE, the arrows, the "
                     L"navigation block, NUM0..NUM9 and the numpad operators, MOUSE3..MOUSE5, or "
                     L"L/R ALT / SHIFT / CTRL. `none` leaves the action unbound.",
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

        // ONE parser for every `*_categories` key, because there are four of them and
        // each one needs the same two diagnostics: unknown names are ignored with a log
        // line naming the whole known set, and a RENAMED name (`merchant` -> `note`)
        // is honoured with a log line saying it will be rewritten. The next Save writes
        // the current spelling, so the warning is self-clearing.
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

        // "R G B" / "R,G,B", 0..255 each. Anything missing keeps the current value, so
        // a truncated line degrades one channel at a time instead of resetting three.
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

        // Returns true when `key` belonged to this group.
        bool apply_core(Config& cfg, const std::string& key, const std::string& value)
        {
            if (key == "mod_enabled")
            {
                cfg.mod_enabled = parse_bool(value, cfg.mod_enabled);
            }
            // `enabled` is the pre-0.9.2 name. It is still accepted (the loader logs
            // one warning naming the new key), so an old file keeps working.
            else if (key == "overlay_enabled" || key == "enabled")
            {
                cfg.overlay_enabled = parse_bool(value, cfg.overlay_enabled);
            }
            else if (key == "show_minimap")
            {
                cfg.show_minimap = parse_bool(value, cfg.show_minimap);
            }
            else if (key == "ui_scale")
            {
                // `auto` (the shipped default) derives the factor from the back buffer
                // height on the render thread; a number pins it.
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
            else if (key == "hud_preset")
            {
                cfg.hud_preset = preset_from_name(value, cfg.hud_preset);
            }
            // theme / palette. A bad value keeps whatever is already in force and says
            // so, rather than silently reverting the look to the default.
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

        // Returns true when `key` belonged to this group.
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

        // Returns true when `key` belonged to this group.
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
                // Free text: `auto`, `shared`, or a name of the player's choosing. It
                // reaches a FILENAME, so it is sanitised (slotid::sanitise_key) before
                // it is used - here we only reject the empty string.
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
                // Free text, and deliberately not validated against anything: it is a
                // list of widget class-name prefixes and the whole point is that it can
                // name a class this build has never seen. An empty value is legal (it
                // means "the built-in table only").
                ::strncpy_s(cfg.menu_ignore_roots, sizeof(cfg.menu_ignore_roots), trim(value).c_str(),
                            _TRUNCATE);
            }
            else if (key == "shrine_list")
            {
                cfg.shrine_list = parse_bool(value, cfg.shrine_list);
            }
            // How much the mod says. A bad value keeps the level already in force and
            // names the three that exist, rather than silently going quiet.
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
            else if (key == "fast_travel_enabled")
            {
                cfg.fast_travel_enabled = parse_bool(value, cfg.fast_travel_enabled);
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

        // Returns true when `key` belonged to this group.
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
            else if (key == "map_waypoint_persist")
            {
                cfg.map_waypoint_persist = parse_bool(value, cfg.map_waypoint_persist);
            }
            else
            {
                return false;
            }
            return true;
        }

        // Returns true when `key` belonged to this group.
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

        // Returns true when `key` belonged to this group.
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
            else if (key == "found_tracker")
            {
                cfg.found_tracker = parse_bool(value, cfg.found_tracker);
            }
            else if (key == "found_save_debounce_ms")
            {
                cfg.found_save_debounce_ms = parse_int(value, cfg.found_save_debounce_ms);
            }
            else if (key == "markers_absence_marks")
            {
                cfg.markers_absence_marks = parse_bool(value, cfg.markers_absence_marks);
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

        // Returns true when `key` belonged to this group.
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

        // The whole key table, in groups. It USED to be one if/else-if chain and
        // MSVC refused it at 123 keys ("compiler limit: blocks nested too deeply",
        // C1061) - an else-if chain counts as nesting. Each group answers "was this
        // key mine?", and tests/markers_test.cpp scrapes the key literals out of this
        // file and compares it with cfgkeys::kConfigKeys and with the shipped config,
        // so a key that lands in no group is caught on the build machine.
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
            // An unknown key is ignored on purpose: a config written by a NEWER
            // build must not stop an older one from starting.
        }
    } // namespace

    // The master switch. Written only by modswitch (loop thread); read by the game
    // thread and the render thread on their first statement. Starts TRUE so the
    // ProcessEvent callback and Present behave normally between the DLL loading and
    // the config being read - neither is reachable before that anyway.
    std::atomic<bool> g_mod_active{true};

    // Bumped by every set_config; read by cfg_cached() on each thread. Starts at 1 so
    // that a thread-local generation of 0 always means "never loaded".
    std::atomic<std::uint32_t> g_cfg_gen{1};

    // The per-activity counter table (see perf.hpp). Plain storage on purpose: every
    // counter has exactly one writing thread, and the F2 panel is a reader that can
    // live with a row being one update stale.
    perf::Table g_perf{};

    // "config refresh" - the generation-cached config's slow path. Registered from
    // load_config_file (loop thread, at start-up) so no call site needs a guarded
    // static on a hot path.
    int g_pf_config = -1;

    std::atomic<bool> g_panel_open{false};
    std::atomic<bool> g_map_open{false};
    std::atomic<bool> g_reload_config{false};
    std::atomic<bool> g_revert_config{false};
    std::atomic<bool> g_save_config{false};
    std::atomic<bool> g_key_capture{false};
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
        {
            SpinGuard guard(g_cfg_lock);
            g_cfg = cfg;
        }
        // The log macros read this one atomic instead of taking the config lock, so a
        // suppressed line costs a relaxed load and nothing else. Published here, which
        // is the single place every config change goes through - a reload, an F5 or the
        // panel's Save all change the level immediately, with no restart.
        g_log_level.store(static_cast<int>(cfg.log_level), std::memory_order_relaxed);
        // Bump AFTER the store so a reader that sees the new generation is guaranteed to
        // copy the new value. A reader that reads the generation first and then copies
        // may pick up an even newer struct while recording the older generation - it
        // simply refreshes once more on the next call, which is harmless.
        g_cfg_gen.fetch_add(1, std::memory_order_release);
    }

    //==================================================================================
    // Per-activity performance counters
    //==================================================================================

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
        // GetTickCount64() until which the process counts as stalled, and what said so.
        // Only ever moved FORWARD, and only by an externally attributable event. The
        // string is a pointer to a literal, so storing it is a relaxed pointer write.
        std::atomic<std::uint64_t> g_perf_stall_until{0};
        std::atomic<const wchar_t*> g_perf_stall_why{nullptr};
    } // namespace

    int perf_register(const char* name, perf::Thread thread)
    {
        // Registration happens once per call site, from that call site's own thread,
        // before or during the first invocation. Two threads registering at the same
        // instant could in theory both take the same slot; every registration in this
        // mod is a `static const int` initialised on the first call of a periodic
        // activity, and the periodic activities start seconds apart.
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
        // Never shorten a window somebody else opened: two overlapping stalls are one
        // stall, and the longer answer is the right one.
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

    const Config& cfg_cached()
    {
        // Generation 0 is never published (g_cfg_gen starts at 1), so a thread that has
        // never asked before always takes the slow path exactly once.
        static thread_local Config tls_cfg{};
        static thread_local std::uint32_t tls_gen = 0;

        const std::uint32_t gen = g_cfg_gen.load(std::memory_order_acquire);
        if (tls_gen != gen)
        {
            const std::uint64_t t0 = qpc_us();
            tls_cfg = config();
            tls_gen = gen;
            // Counts and times the SLOW path only, which is the whole point: the table
            // shows how often a thread actually had to take the spinlock and copy.
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

    // THE ACCEPTED SET, in one place, so the Bindings tab's capture widget can only ever
    // produce a key the config file can also spell. Mirrors vk_from_name exactly:
    // F1..F5 / F7 / F8, a letter or a digit, and every entry of kNamedKeys.
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

    // The DEV overlay file. It is not shipped in the release zip, it is parsed only if
    // it exists, and it is loaded AFTER the main file so a dev key set in both wins
    // here. Everything in it is Tier::Dev (cfgkeys) - dials that existed because a
    // developer needed one during bring-up.
    std::wstring dev_config_path()
    {
        return mod_dir() + L"\\config_wuchang_minimap_dev.txt";
    }

    namespace
    {
        // One warning per key per process. A removed key or the old `enabled` spelling
        // is a fact about the user's file, not an event - repeating it on every F5 and
        // on every 1 Hz mtime reload would bury everything else in the log.
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

        // Applies one config file's text onto `cfg`. Returns how many `key = value`
        // lines it understood. The line rules are mirrored EXACTLY in
        // cfgkeys::keys_in(), which is what the offline drift test parses files with.
        int apply_text(Config& cfg, const std::string& text)
        {
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

                // A key that used to exist and is now a hard-coded constant. It is
                // NAMED rather than ignored in silence, because "I set it and nothing
                // happened" is exactly what an ignored key looks like.
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

                ++lines;
                apply_setting(cfg, key, value);
            }
            return lines;
        }

        //==============================================================================
        // THEME PRECEDENCE
        //==============================================================================
        //
        // A theme supplies a colour ONLY where the config file is silent. `seen` is
        // every key name that appeared in either file, so a value the player wrote out
        // by hand always wins - and because this runs after the whole file has been
        // parsed, it does not matter whether the `theme` line sits above or below the
        // colour it would otherwise preset.
        //
        // With `theme = neutral` (the shipped default) every value below is what 0.9.2
        // already had, so a config that never mentions a theme is bit-for-bit unchanged.
        void apply_theme_defaults(Config& cfg, const std::vector<std::string>& seen)
        {
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

            const gly::ThemeColors tc = gly::theme_colors(cfg.theme);
            if (!mentioned("minimap_frame_color"))
            {
                cfg.minimap_frame_r = static_cast<float>(tc.frame.r);
                cfg.minimap_frame_g = static_cast<float>(tc.frame.g);
                cfg.minimap_frame_b = static_cast<float>(tc.frame.b);
            }
            if (!mentioned("minimap_frame_alpha"))
            {
                cfg.minimap_frame_alpha = tc.frame_alpha;
            }
            if (!mentioned("minimap_backdrop_color"))
            {
                cfg.minimap_backdrop_r = static_cast<float>(tc.backdrop.r);
                cfg.minimap_backdrop_g = static_cast<float>(tc.backdrop.g);
                cfg.minimap_backdrop_b = static_cast<float>(tc.backdrop.b);
            }
            if (!mentioned("minimap_backdrop"))
            {
                cfg.minimap_backdrop = tc.backdrop_alpha;
            }
            if (!mentioned("floor_base_color"))
            {
                cfg.floor_base_r = static_cast<float>(tc.floor_base.r);
                cfg.floor_base_g = static_cast<float>(tc.floor_base.g);
                cfg.floor_base_b = static_cast<float>(tc.floor_base.b);
            }
            // The item-quality tiers follow the PALETTE, not the theme: the shipped
            // defaults are the game's own pale pickup-beam colours, which are the one
            // place a colour-blind player is left with hue as the only channel.
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
            cfg.markers_absence_rounds = (std::max)(1, (std::min)(30, cfg.markers_absence_rounds));
            cfg.markers_absence_categories &= mdb::kAllCats;
            // 40 is lbl::Layout::kMaxRects - past that the overlap pass has nowhere to
            // put a box and the caller draws the glyph alone anyway.
            cfg.highlight_labels_max = (std::max)(0, (std::min)(40, cfg.highlight_labels_max));
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
            // The minimap's own look, the slicer's sizing, the reader's rates and the sweep's
            // caps. Same rule as everything above: a hand-edited file may be wrong, it may
            // not be able to divide by zero, allocate unboundedly or stall the game thread.
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

        // The dev overlay, if the developer put one there. Loaded second on purpose: a
        // dev file is a deliberate override of whatever the shipped file says.
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

        // The theme fills in the colour keys the two files did not mention - after both
        // of them have been read, so the order of the lines cannot matter.
        apply_theme_defaults(cfg, seen);
        clamp_config(cfg);
        set_config(cfg);
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

    //==================================================================================
    // Writing: the config as key -> value
    //==================================================================================
    //
    // ONE function produces the text form of every setting, and cfgkeys decides which
    // file each one belongs in. That is what lets the F2 panel's Save rewrite values in
    // place (src/config_rewrite.hpp) instead of regenerating a file: the writer no
    // longer owns the layout, only the values.
    //
    // Every key in cfgkeys::kKeys with tier Player / Advanced / Dev must appear here
    // exactly once; one that does not would silently never be saved.

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

        // ---- Player -----------------------------------------------------------------
        add("mod_enabled", b(cfg.mod_enabled));
        add("overlay_enabled", b(cfg.overlay_enabled));
        add("show_minimap", b(cfg.show_minimap));
        add("ui_scale", cfg.ui_scale_auto ? std::string{"auto"} : f2(cfg.ui_scale));
        add("hud_preset", preset_name(cfg.hud_preset));
        add("theme", gly::theme_name(cfg.theme));
        add("palette", gly::palette_name(cfg.palette));
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
        add("markers_absence_marks", b(cfg.markers_absence_marks));
        add("found_tracker", b(cfg.found_tracker));
        add("found_profile", std::string{cfg.found_profile});
        add("first_run_toast", b(cfg.first_run_toast));
        add("map_zoom", f0(cfg.map_zoom));
        add("map_marker_size", f1(cfg.map_marker_size));
        add("map_show_all_floors", b(cfg.map_show_all_floors));
        add("map_gamepad", b(cfg.map_gamepad));
        add("map_waypoint_persist", b(cfg.map_waypoint_persist));
        add("shrine_list", b(cfg.shrine_list));
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
        add("panel_key", vk(cfg.panel_key));
        add("map_key", vk(cfg.map_key));
        add("map_recenter_key", vk(cfg.map_recenter_key));
        add("zoom_key", vk(cfg.zoom_key));
        add("reload_key", vk(cfg.reload_key));
        add("screenshot_key", vk(cfg.screenshot_key));

        // ---- Advanced ---------------------------------------------------------------
        add("require_pawn_view", b(cfg.require_pawn_view));
        add("state_stale_ms", std::to_string(cfg.state_stale_ms));
        add("min_visible_after_state_ok_ms", std::to_string(cfg.min_visible_after_state_ok_ms));
        add("menu_close_show_delay_ms", std::to_string(cfg.menu_close_show_delay_ms));
        add("adjacent_floor_opacity", f2(cfg.adjacent_floor_opacity));
        add("floor_fade_uu", f0(cfg.floor_fade_uu));
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
        add("fast_travel_enabled", b(cfg.fast_travel_enabled));

        // ---- Dev --------------------------------------------------------------------
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
        // The values of one tier as plain `key = value` lines. Used ONLY when a file
        // does not exist yet; an existing file is rewritten in place, so its
        // documentation, its ordering and any key we do not know survive a Save.
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

        // Writes `mine` into `path`. If the file exists, only the VALUES on its
        // existing `key = value` lines are replaced and anything missing is appended
        // once under kAppendedBanner - nothing else about the file changes. If it does
        // not exist, `fresh` is written verbatim.
        void write_config(const std::wstring& path, const std::vector<cfgrw::Pair>& mine,
                          const std::string& fresh)
        {
            std::string existing;
            std::string out;
            int changed = 0;
            int appended = 0;
            if (read_whole_file(path, existing))
            {
                const cfgrw::Result r = cfgrw::rewrite(existing, mine, kAppendedBanner);
                out = r.text;
                changed = r.changed;
                appended = r.appended;
            }
            else
            {
                out = fresh;
                appended = static_cast<int>(mine.size());
            }

            if (write_whole_file(path, out))
            {
                logf(L"config: saved -> {} ({} value(s) changed, {} key(s) appended)", path, changed, appended);
            }
            else
            {
                logf(L"config: FAILED to write {} (error {})", path, static_cast<unsigned>(::GetLastError()));
            }
        }

        // Does any Dev key differ from the built-in default? That is the test for
        // "somebody actually wanted a dev file" - see save_config_file.
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

        // ---- the player file: Player + Advanced -------------------------------------
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

        // ---- the dev file: Dev only, and never created for nothing -------------------
        //
        // It is written when it already exists (a developer is using it), or when some
        // Dev key has been moved off its built-in default - which is the only way the
        // Debug tab's edits can be persisted at all. A player who never touched one must
        // not find a file full of developer dials appear next to their config, so an
        // all-defaults Dev set with no file writes nothing. Dev keys never leak into the
        // player file: the filter above cannot see them.
        std::string dev_existing;
        const bool have_dev = read_whole_file(dev_config_path(), dev_existing);
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

    //==================================================================================
    // The master switch, read straight off disk
    //==================================================================================
    //
    // Deliberately NOT load_config_file(): while the mod is off, the file is the only
    // channel the player has, and re-applying every other key on the way back in would
    // mean an unrelated edit took effect at a moment nobody asked for. `modswitch`
    // calls load_config_file() itself once it has decided to turn the mod on.

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

    // BOTH files, mixed into one number. The 1 Hz watcher in modswitch compares this
    // against the value it last saw, so editing EITHER the shipped config or the dev
    // overlay reloads both - a dev file that only took effect on a restart would be a
    // trap during bring-up, which is the one thing it exists for.
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

    //==================================================================================
    // THE MOD'S OWN ROLLING LOG - wuchang_minimap.log
    //==================================================================================
    //
    // WHY THIS EXISTS. Every line the mod writes also goes to UE4SS's `UE4SS.log`, and
    // UE4SS TRUNCATES that file on every launch. So the evidence from an in-game session
    // is gone the moment the player starts the game again to try the next build - which
    // happened twice in this project, and both times the answer to "which gate dropped
    // the marker" had been printed in plain text and then overwritten. A log the mod owns
    // and ROTATES (`.1` / `.2` / `.3`) keeps the last four sessions whatever UE4SS does.
    //
    // THE CONSTRAINTS. Same family as the crash breadcrumb (breadcrumb.hpp): the game
    // thread must never touch C++ iostreams or the C++ locale in this process, so this is
    // flat `CreateFileW` / `WriteFile` over a hand-built UTF-8 buffer. Writing is
    // BUFFERED (8 KB) because a state line every 10 s plus a marker census is not worth a
    // syscall each, and flushed (a) whenever the buffer fills, (b) on every crash
    // breadcrumb write - which is what makes the log and the breadcrumb agree about the
    // last thing that happened - and (c) every few seconds from the loop thread.
    //
    // `modlog_line()` is only ever reached from the loop thread (mm::log's direct path and
    // drain_log), but `modlog_flush()` is called from the breadcrumb and from the stall
    // watchdog, i.e. from any thread - so the buffer and the handle are behind a spinlock
    // of their OWN (see g_modlog_lock), never the log queue's, because everything under
    // it is a blocking file syscall and the queue is what the game thread touches.
    // Nothing in here allocates while holding it.

    namespace
    {
        constexpr std::size_t kModLogFlushAt = 8192;
        // PER-SESSION CAP. A 40-minute session at the default level is a few hundred
        // kilobytes; 20 MB is two orders of magnitude of headroom and still small enough
        // to attach to a bug report. At the cap one line says so and writing stops - the
        // rotation (.1 / .2 / .3) is untouched, so the previous sessions are still there.
        constexpr std::uint64_t kModLogMaxBytes = 20ull * 1024ull * 1024ull;
        constexpr const wchar_t* kModLogName = L"\\wuchang_minimap.log";

        // A LOCK OF ITS OWN, not the log QUEUE's.
        //
        // Both used to be behind `g_log_lock`, and that put a `WriteFile` /
        // `FlushFileBuffers` / a four-file rotation INSIDE the lock that the game thread
        // and the render thread take to enqueue a log line - a spinlock held across a
        // blocking syscall, with the game thread as the victim. Nothing needs the two to
        // be the same lock: the queue is a vector of strings, the buffer is bytes and a
        // handle, and no path holds one while taking the other (`drain_log` swaps the
        // queue out under `g_log_lock`, releases it, and only then writes). So they are
        // separate, and enqueueing a line from the game thread can no longer wait on the
        // disk.
        Spinlock g_modlog_lock;
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

        // ROTATE, then create. `.3` is dropped, everything else shifts up one, and the
        // live file is always `wuchang_minimap.log` - so "the log" is one stable name in a
        // bug report and the three previous sessions are still on disk beside it.
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

        // Loop thread, lazily on the first line. FILE_SHARE_READ so the file can be read
        // (and pasted into a bug report) while the game is still running.
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
            const std::string text = utf8_of(std::wstring{stamp} + line);
            SpinGuard guard(g_modlog_lock);
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
        SpinGuard guard(g_modlog_lock);
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

    std::wstring modlog_path()
    {
        return mod_dir() + kModLogName;
    }

    void log(const std::wstring& line)
    {
        if (g_loop_thread == 0 || ::GetCurrentThreadId() != g_loop_thread)
        {
            SpinGuard guard(g_log_lock);
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
            SpinGuard guard(g_log_lock);
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
