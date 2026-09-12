#pragma once

//
// config_keys - every key the config files may carry, tagged with its tier, plus the
// loader's line parser as pure C++. tests/markers_test.cpp asserts, in both directions,
// that the tiers match the shipped files under deploy/ue4ss/Mods/WuchangMinimap/ and the
// `key == "..."` set scraped out of mmstate.cpp, that the tiers are pairwise disjoint,
// and that no Removed or Legacy key appears in a shipped file.
//
// Tiers
//   Player    - shipped config under `; ---- PLAYER SETTINGS ----`.
//   Advanced  - shipped config under `; ---- ADVANCED ----`; mostly the F2 Tuning tab.
//   Dev       - config_wuchang_minimap_dev.txt, not shipped, parsed only when present.
//   Removed   - a hard-coded constant or a dropped feature now; recognised only to warn
//               instead of ignoring.
//   Legacy    - old name for a live key; accepted, warned once, mapped to the new name.
//
// No Windows, no UE4SS, no allocation beyond the strings the caller asks for.
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

    // Every key the loader understands or deliberately refuses, in shipped-file order.
    // Order matters only for the banner-order test; everything else compares sets.
    inline constexpr KeyInfo kKeys[] = {
        // PLAYER
        {"mod_enabled", Tier::Player},
        {"show_minimap", Tier::Player},
        {"theme", Tier::Player},
        {"palette", Tier::Player},
        {"ui_scale", Tier::Player},
        {"font_size", Tier::Player},
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
        {"map_zoom", Tier::Player},
        {"map_marker_size", Tier::Player},
        {"map_gamepad", Tier::Player},
        {"highlight_enabled", Tier::Player},
        {"highlight_mode", Tier::Player},
        {"highlight_key", Tier::Player},
        {"highlight_gamepad", Tier::Player},
        {"highlight_pad_chord", Tier::Player},
        {"highlight_radius", Tier::Player},
        {"highlight_categories", Tier::Player},
        {"highlight_labels", Tier::Player},
        {"highlight_show_found", Tier::Player},
        {"highlight_size", Tier::Player},
        {"compass_enabled", Tier::Player},
        {"compass_anchor", Tier::Player},
        {"compass_width", Tier::Player},
        {"compass_offset_y", Tier::Player},
        {"compass_span_deg", Tier::Player},
        {"compass_opacity", Tier::Player},
        {"compass_categories", Tier::Player},
        {"compass_pip_labels", Tier::Player},
        {"map_pad_open_chord", Tier::Player},
        {"panel_pad_open_chord", Tier::Player},
        {"panel_key", Tier::Player},
        {"map_key", Tier::Player},
        {"map_recenter_key", Tier::Player},
        {"zoom_key", Tier::Player},
        {"reload_key", Tier::Player},
        {"screenshot_key", Tier::Player},
        {"waypoint_nearest_key", Tier::Player}, // sets a waypoint on the nearest unfound marker

        // ADVANCED
        {"overlay_hooks", Tier::Advanced},
        {"require_pawn_view", Tier::Advanced},
        {"state_stale_ms", Tier::Advanced},
        {"min_visible_after_state_ok_ms", Tier::Advanced},
        {"menu_close_show_delay_ms", Tier::Advanced},
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
        {"boss_defeat_from_save", Tier::Advanced},
        {"minimap_zoom_presets", Tier::Advanced},
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
        {"highlight_labels_max", Tier::Advanced},
        {"highlight_max_draw", Tier::Advanced},
        {"highlight_alpha_near", Tier::Advanced},
        {"highlight_alpha_far", Tier::Advanced},
        {"highlight_edge_arrows", Tier::Advanced},
        {"highlight_camera_hz", Tier::Advanced},
        {"compass_plate", Tier::Advanced},
        {"compass_height", Tier::Advanced},
        {"compass_marker_distance", Tier::Advanced},
        {"compass_show_waypoint", Tier::Advanced},
        {"compass_tick_step_deg", Tier::Advanced},
        {"compass_max_pips", Tier::Advanced},
        {"compass_pip_height_uu", Tier::Advanced},
        {"ui_font", Tier::Advanced},

        // DEV - config_wuchang_minimap_dev.txt, not shipped
        {"map_unreachable", Tier::Dev},
        {"shade_lo_color", Tier::Dev},
        {"shade_hi_color", Tier::Dev},
        {"shade_gamma", Tier::Dev},
        {"shade_below_alpha", Tier::Dev},
        {"shade_above_alpha", Tier::Dev},
        {"shade_above_band_uu", Tier::Dev},
        {"shade_range_pct_lo", Tier::Dev},
        {"shade_min_range_uu", Tier::Dev},
        {"shade_range_smooth_ms", Tier::Dev},
        {"shade_map_equalize", Tier::Dev},
        {"first_run_toast", Tier::Dev},
        {"found_profile", Tier::Dev},
        {"log_level", Tier::Dev},
        {"crash_breadcrumb", Tier::Dev},
        {"zoom_dpi_scaled", Tier::Dev},
        {"debug_readout", Tier::Dev},
        {"debug_show_panel_on_start", Tier::Dev},
        {"fallback_use_composite", Tier::Dev},
        {"minimap_composite_alpha", Tier::Dev},
        {"reader_position_period_ms", Tier::Dev},
        {"reader_resolve_period_ms", Tier::Dev},
        {"reader_widget_sweep_period_ms", Tier::Dev},
        {"reader_widget_sweep_max_period_ms", Tier::Dev},
        {"reader_widget_sweep_warm_ms", Tier::Dev},
        {"menu_ignore_roots", Tier::Dev},
        {"reader_transition_cooldown_ms", Tier::Dev},
        {"reader_teleport_jump_uu", Tier::Dev},
        {"reader_chapter_period_ms", Tier::Dev},
        {"reader_log_throttle_ms", Tier::Dev},
        {"markers_live_grace_rounds", Tier::Dev},
        {"map_asset_retire_grace_ms", Tier::Dev},
        {"hide_reason_log_ms", Tier::Dev},
        {"srv_heap_size", Tier::Dev},
        {"navmesh_dump", Tier::Dev},
        {"highlight_camera_resolve_ms", Tier::Dev},
        {"highlight_compass_period_ms", Tier::Dev},
        {"highlight_getter_period_ms", Tier::Dev},
        {"highlight_pov_scan_bytes", Tier::Dev},
        {"highlight_pov_bad_reads", Tier::Dev},
        {"saveslot_uuid_call", Tier::Dev},

        // REMOVED - hard-coded constants and dropped features; listed so the key gets a
        // named warning
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
        {"fast_travel_enabled", Tier::Removed}, // the game only travels at a shrine
        {"recon_dump_key", Tier::Removed},      // recon dump is a Debug-tab button
        // Behaviour the mod now always has; the panel toggles for them are gone.
        {"overlay_enabled", Tier::Removed},     // overlay_hooks is the off switch
        {"enabled", Tier::Removed},             // the older name for overlay_enabled
        {"found_tracker", Tier::Removed},       // the collection tracker is always on
        {"markers_absence_marks", Tier::Removed},
        {"map_waypoint_persist", Tier::Removed},
        {"shrine_list", Tier::Removed},
        // The quality tier is a property of the CATEGORY now: the eleven loot categories
        // take their tier's hue from the marker palette, on every surface. There is no
        // longer a tint to switch on, a surface to switch it on for, or a second set of
        // three colours beside the palette's own.
        {"xray_rarity_colors_enabled", Tier::Removed},
        {"markers_rarity_tint", Tier::Removed},
        {"xray_rarity_colors", Tier::Removed},
        // The height slice shades by ABSOLUTE Z: the colour of a pixel is its surface's own
        // height on the shade_* ramp, whichever storey it belongs to.
        {"adjacent_floor_opacity", Tier::Removed},  // shade_below_alpha / shade_above_alpha
        {"floor_fade_uu", Tier::Removed},           // everything below the player is drawn
        {"floor_fade_above_uu", Tier::Removed},     // shade_above_band_uu
        {"floor_gradient_strength", Tier::Removed}, // shade_gamma
        {"floor_base_color", Tier::Removed},        // shade_lo_color / shade_hi_color
        // One hypsometric ramp serves every class; the own storey is not tinted apart.
        {"shade_floor_tint", Tier::Removed},
        {"shade_floor_tint_mix", Tier::Removed},
        // The full map always slices with an unbounded band overhead, so there is no
        // longer a floor for this to show.
        {"map_show_all_floors", Tier::Removed},
    };

    inline constexpr std::size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

    // The live key a removed name should send the reader to, or nullptr when the generic
    // "this is a hard-coded constant" line says everything. Only for names a player
    // deliberately typed in the hope of an effect.
    inline const char* removed_advice(std::string_view key)
    {
        if (key == "overlay_enabled" || key == "enabled")
        {
            return "overlay_hooks";
        }
        if (key == "xray_rarity_colors_enabled" || key == "markers_rarity_tint" ||
            key == "xray_rarity_colors")
        {
            return "palette";
        }
        return nullptr;
    }

    // The new name a legacy key maps onto, or nullptr when `key` is not a legacy name.
    // No key is renamed right now; the loader keeps the path for the next one.
    inline const char* renamed_to(std::string_view key)
    {
        (void)key;
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

    // The loader acts on this key: Player, Advanced, Dev or Legacy. A Removed key is
    // deliberately not known - it is recognised, warned about and ignored.
    inline bool is_known(std::string_view key)
    {
        const KeyInfo* k = find(key);
        return k != nullptr && k->tier != Tier::Removed;
    }

    // Recognised but no longer honoured - the warning path.
    inline bool is_removed(std::string_view key)
    {
        return tier_is(key, Tier::Removed);
    }

    // Same line rules as mmstate.cpp's loader: UTF-8 BOM skipped, `;` and `#` start a
    // comment, key is everything left of the first `=`, trimmed. Duplicates once each,
    // in first-seen order.
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
