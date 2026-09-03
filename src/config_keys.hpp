#pragma once

//
// config_keys - the canonical list of every key the mod's config files may carry,
// each tagged with the TIER it belongs to, plus the same line parser the loader
// uses, as PURE C++.
//
// WHY IT EXISTS
// -------------
// The config has several independent halves that can drift apart without anyone
// noticing: the parser in mmstate.cpp (`key == "..."`), the key->value writer in the
// same file (which is what the F2 panel's Save produces), and the two SHIPPED files
// under deploy/ue4ss/Mods/WuchangMinimap/. A key added to the struct and the parser
// but left out of the shipped file is invisible to every player who never presses
// Save; a key left in a shipped file after being renamed is silently ignored, which
// reads exactly like the setting not working.
//
// So tests/markers_test.cpp asserts, in BOTH directions, that
//
//     keys(config_wuchang_minimap.txt)      == { Tier::Player } U { Tier::Advanced }
//     keys(config_wuchang_minimap_dev.txt)  == { Tier::Dev }
//     { key == "..." in mmstate.cpp }       == Player U Advanced U Dev U Legacy
//
// and that the four tiers are pairwise disjoint, that no Removed or Legacy key appears
// in either shipped file, and that the shipped file's `; ---- PLAYER SETTINGS ----` /
// `; ---- ADVANCED ----` banners agree with the Player/Advanced tags below.
//
// The parser's set is scraped from the source, which is what makes the table below a
// description of the parser rather than a second thing to maintain by hand.
//
// THE TIERS
//   Player    - a person tuning the HUD would plausibly change it. Lives in the shipped
//               config under `; ---- PLAYER SETTINGS ----` and in the F2 Player tab.
//   Advanced  - correct as shipped; changed to answer a symptom. Shipped config under
//               `; ---- ADVANCED ----`, F2 Advanced tab.
//   Dev       - a dial that existed because a developer needed it during bring-up.
//               Lives in config_wuchang_minimap_dev.txt, which is NOT shipped in the
//               release zip and is parsed only when it exists.
//   Removed   - a sanity cap that used to be a key and is now a hard-coded constant. A
//               wrong value was never a preference, it was a bug report. Recognised
//               only so that a user's old file gets one warning instead of silence.
//   Legacy    - an old NAME for a key that still exists. Accepted, warned about once,
//               and mapped onto the new name.
//
// No Windows, no UE4SS, no allocation beyond the strings the caller asks for - the same
// rule as markers_db / mapview / chapterid.
//

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cfgkeys
{
    enum class Tier
    {
        Player,
        Advanced,
        Dev,
        Removed,
        Legacy,
    };

    struct KeyInfo
    {
        const char* name;
        Tier tier;
    };

    // Every key the loader understands or deliberately refuses, in the order the
    // shipped files list them. Order matters for exactly one test (the banner order in
    // the shipped file); everything else compares sets.
    inline constexpr KeyInfo kKeys[] = {
        //------------------------------------------------------------------------------
        // PLAYER
        //------------------------------------------------------------------------------
        {"mod_enabled", Tier::Player},
        {"overlay_enabled", Tier::Player},
        {"show_minimap", Tier::Player},
        {"ui_scale", Tier::Player},
        {"hud_preset", Tier::Player},
        {"minimap_size", Tier::Player},
        {"minimap_zoom", Tier::Player},
        {"minimap_shape", Tier::Player},
        {"minimap_anchor", Tier::Player},
        {"minimap_offset_x", Tier::Player},
        {"minimap_offset_y", Tier::Player},
        {"rotate_with_player", Tier::Player},
        {"opacity", Tier::Player},
        {"hide_in_menus", Tier::Player},
        {"show_adjacent_floors", Tier::Player},
        {"floor_z_tolerance", Tier::Player},
        {"markers_enabled", Tier::Player},
        {"markers_categories", Tier::Player},
        {"markers_hide_found", Tier::Player},
        {"markers_size", Tier::Player},
        {"markers_clamp_to_edge", Tier::Player},
        {"markers_absence_marks", Tier::Player},
        {"found_tracker", Tier::Player},
        {"map_zoom", Tier::Player},
        {"map_marker_size", Tier::Player},
        {"map_show_all_floors", Tier::Player},
        {"map_gamepad", Tier::Player},
        {"map_waypoint_persist", Tier::Player},
        {"highlight_enabled", Tier::Player},
        {"highlight_key", Tier::Player},
        {"highlight_gamepad", Tier::Player},
        {"highlight_pad_chord", Tier::Player},
        {"highlight_radius", Tier::Player},
        {"highlight_categories", Tier::Player},
        {"highlight_labels", Tier::Player},
        {"highlight_size", Tier::Player},
        {"xray_rarity_colors_enabled", Tier::Player},
        {"markers_rarity_tint", Tier::Player},
        {"compass_enabled", Tier::Player},
        {"compass_width", Tier::Player},
        {"compass_offset_y", Tier::Player},
        {"compass_span_deg", Tier::Player},
        {"compass_opacity", Tier::Player},
        {"compass_categories", Tier::Player},
        {"panel_key", Tier::Player},
        {"map_key", Tier::Player},
        {"map_recenter_key", Tier::Player},
        {"reload_key", Tier::Player},

        //------------------------------------------------------------------------------
        // ADVANCED
        //------------------------------------------------------------------------------
        {"require_pawn_view", Tier::Advanced},
        {"state_stale_ms", Tier::Advanced},
        {"min_visible_after_state_ok_ms", Tier::Advanced},
        {"menu_close_show_delay_ms", Tier::Advanced},
        {"adjacent_floor_opacity", Tier::Advanced},
        {"floor_fade_uu", Tier::Advanced},
        {"floor_gradient_strength", Tier::Advanced},
        {"floor_base_color", Tier::Advanced},
        {"slice_hz", Tier::Advanced},
        {"feet_z_smooth_ms", Tier::Advanced},
        {"player_z_offset", Tier::Advanced},
        {"markers_live", Tier::Advanced},
        {"markers_filter_chapter", Tier::Advanced},
        {"markers_rounds_per_sec", Tier::Advanced},
        {"markers_scan_chunk", Tier::Advanced},
        {"markers_scan_period_ms", Tier::Advanced},
        {"markers_found_alpha", Tier::Advanced},
        {"markers_max_draw", Tier::Advanced},
        {"found_save_debounce_ms", Tier::Advanced},
        {"markers_absence_rounds", Tier::Advanced},
        {"markers_absence_categories", Tier::Advanced},
        {"minimap_backdrop", Tier::Advanced},
        {"minimap_backdrop_color", Tier::Advanced},
        {"minimap_frame_color", Tier::Advanced},
        {"minimap_frame_alpha", Tier::Advanced},
        {"minimap_min_px", Tier::Advanced},
        {"minimap_arrow_frac", Tier::Advanced},
        {"minimap_arrow_min_px", Tier::Advanced},
        {"waypoint_size_scale", Tier::Advanced},
        {"map_zoom_min", Tier::Advanced},
        {"map_zoom_max", Tier::Advanced},
        {"map_zoom_factor", Tier::Advanced},
        {"map_pan_speed", Tier::Advanced},
        {"map_margin", Tier::Advanced},
        {"map_backdrop", Tier::Advanced},
        {"map_markers_max_draw", Tier::Advanced},
        {"map_floor_step", Tier::Advanced},
        {"map_slice_px", Tier::Advanced},
        {"map_slice_hz", Tier::Advanced},
        {"map_gamepad_deadzone", Tier::Advanced},
        {"highlight_show_found", Tier::Advanced},
        {"highlight_max_draw", Tier::Advanced},
        {"highlight_alpha_near", Tier::Advanced},
        {"highlight_alpha_far", Tier::Advanced},
        {"highlight_edge_arrows", Tier::Advanced},
        {"highlight_camera_hz", Tier::Advanced},
        {"xray_rarity_colors", Tier::Advanced},
        {"compass_anchor", Tier::Advanced},
        {"compass_height", Tier::Advanced},
        {"compass_marker_distance", Tier::Advanced},
        {"compass_show_waypoint", Tier::Advanced},
        {"compass_tick_step_deg", Tier::Advanced},
        {"compass_max_pips", Tier::Advanced},

        //------------------------------------------------------------------------------
        // DEV - config_wuchang_minimap_dev.txt, not shipped
        //------------------------------------------------------------------------------
        {"debug_readout", Tier::Dev},
        {"debug_show_panel_on_start", Tier::Dev},
        {"fallback_use_composite", Tier::Dev},
        {"minimap_composite_alpha", Tier::Dev},
        {"reader_position_period_ms", Tier::Dev},
        {"reader_resolve_period_ms", Tier::Dev},
        {"reader_widget_sweep_period_ms", Tier::Dev},
        {"reader_widget_sweep_max_period_ms", Tier::Dev},
        {"reader_widget_sweep_warm_ms", Tier::Dev},
        {"reader_transition_cooldown_ms", Tier::Dev},
        {"reader_teleport_jump_uu", Tier::Dev},
        {"reader_chapter_period_ms", Tier::Dev},
        {"reader_log_throttle_ms", Tier::Dev},
        {"markers_live_grace_rounds", Tier::Dev},
        {"map_asset_retire_grace_ms", Tier::Dev},
        {"hide_reason_log_ms", Tier::Dev},
        {"srv_heap_size", Tier::Dev},
        {"highlight_camera_resolve_ms", Tier::Dev},
        {"highlight_compass_period_ms", Tier::Dev},
        {"highlight_getter_period_ms", Tier::Dev},
        {"highlight_pov_scan_bytes", Tier::Dev},
        {"highlight_pov_bad_reads", Tier::Dev},

        //------------------------------------------------------------------------------
        // REMOVED - hard-coded constants since 0.9.2. A wrong value here was never a
        // preference; it was a bug report. Kept in the table so an old file gets one
        // warning naming the key instead of the silence an unknown key gets.
        //------------------------------------------------------------------------------
        {"minimap_circle_segments", Tier::Removed},
        {"slice_min_px", Tier::Removed},
        {"slice_max_px", Tier::Removed},
        {"map_slice_margin", Tier::Removed},
        {"reader_max_widgets", Tier::Removed},
        {"reader_max_menu_roots", Tier::Removed},
        {"reader_max_levels", Tier::Removed},
        {"markers_live_max", Tier::Removed},
        {"markers_id_cache_max", Tier::Removed},
        {"markers_class_cache_max", Tier::Removed},
        {"markers_fallback_max_per_class", Tier::Removed},

        //------------------------------------------------------------------------------
        // LEGACY - old names, still accepted with one warning
        //------------------------------------------------------------------------------
        {"enabled", Tier::Legacy}, // -> overlay_enabled (0.9.2)
    };

    inline constexpr std::size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

    // The new name a legacy key maps onto, or nullptr when `key` is not a legacy name.
    inline const char* renamed_to(std::string_view key)
    {
        if (key == "enabled")
        {
            return "overlay_enabled";
        }
        return nullptr;
    }

    inline constexpr std::size_t count_of(Tier t)
    {
        std::size_t n = 0;
        for (const KeyInfo& k : kKeys)
        {
            if (k.tier == t)
            {
                ++n;
            }
        }
        return n;
    }

    inline constexpr std::size_t kPlayerKeyCount = count_of(Tier::Player);
    inline constexpr std::size_t kAdvancedKeyCount = count_of(Tier::Advanced);
    inline constexpr std::size_t kDevKeyCount = count_of(Tier::Dev);
    inline constexpr std::size_t kRemovedKeyCount = count_of(Tier::Removed);
    inline constexpr std::size_t kLegacyKeyCount = count_of(Tier::Legacy);

    // Player + Advanced: everything the SHIPPED config file carries.
    inline constexpr std::size_t kConfigKeyCount = kPlayerKeyCount + kAdvancedKeyCount;

    inline std::vector<std::string> keys_of(Tier t)
    {
        std::vector<std::string> out;
        for (const KeyInfo& k : kKeys)
        {
            if (k.tier == t)
            {
                out.emplace_back(k.name);
            }
        }
        return out;
    }

    // The shipped config file's keys, in file order (Player block, then Advanced).
    inline std::vector<std::string> shipped_keys()
    {
        std::vector<std::string> out = keys_of(Tier::Player);
        for (std::string& k : keys_of(Tier::Advanced))
        {
            out.push_back(std::move(k));
        }
        return out;
    }

    // Returns nullptr when the key is in no tier at all.
    inline const KeyInfo* find(std::string_view key)
    {
        for (const KeyInfo& k : kKeys)
        {
            if (key == k.name)
            {
                return &k;
            }
        }
        return nullptr;
    }

    inline bool tier_is(std::string_view key, Tier t)
    {
        const KeyInfo* k = find(key);
        return k != nullptr && k->tier == t;
    }

    // "the loader will do something with this key" - Player, Advanced, Dev or Legacy.
    // A Removed key is deliberately NOT known: it is recognised, warned about, ignored.
    inline bool is_known(std::string_view key)
    {
        const KeyInfo* k = find(key);
        return k != nullptr && k->tier != Tier::Removed;
    }

    // "this key used to exist and no longer does" - the warning path.
    inline bool is_removed(std::string_view key)
    {
        return tier_is(key, Tier::Removed);
    }

    // The SAME line rules as mmstate.cpp's loader: a UTF-8 BOM is skipped, `;` and `#`
    // start a comment, the key is everything left of the first `=`, trimmed. Duplicates
    // are reported once each, in first-seen order.
    inline std::vector<std::string> keys_in(std::string_view text)
    {
        const auto trim = [](std::string_view v) {
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
        };

        std::vector<std::string> out;
        std::size_t pos = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            pos = 3;
        }
        while (pos <= text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            std::string_view raw = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
            pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
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
            std::string key = trim(raw.substr(0, eq));
            if (key.empty())
            {
                continue;
            }
            bool seen = false;
            for (const std::string& k : out)
            {
                if (k == key)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
            {
                out.push_back(std::move(key));
            }
        }
        return out;
    }
} // namespace cfgkeys
