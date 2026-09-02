#pragma once

//
// config_keys - the canonical list of every key config_wuchang_minimap.txt may carry,
// plus the same line parser the loader uses, as PURE C++.
//
// WHY IT EXISTS
// -------------
// The config has three independent halves that can drift apart without anyone
// noticing: the parser in mmstate.cpp (`key == "..."`), the writer in the same file
// (which is what the F2 panel's Save produces), and the SHIPPED file under
// deploy/ue4ss/Mods/WuchangMinimap/. A key added to the struct and the parser but left
// out of the shipped file is invisible to every player who never presses Save; a key
// left in the shipped file after being renamed is silently ignored, which reads exactly
// like the setting not working.
//
// So tests/markers_test.cpp asserts, in BOTH directions, that
//
//     keys(shipped config file)  ==  kConfigKeys  ==  { key == "..." in mmstate.cpp }
//
// The third set is scraped from the source, which is what makes the table below a
// description of the parser rather than a second thing to maintain by hand.
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
    // Every key the loader understands, grouped the way the shipped file groups them.
    // Order is irrelevant to the tests (they compare sets), but keeping it in file
    // order makes a diff readable.
    inline constexpr const char* kConfigKeys[] = {
        // the master switch and the overlay
        "mod_enabled",
        "enabled",
        "show_minimap",
        "minimap_size",
        "minimap_zoom",
        "minimap_shape",
        "minimap_anchor",
        "minimap_offset_x",
        "minimap_offset_y",
        "rotate_with_player",
        "opacity",
        "hide_in_menus",
        "require_pawn_view",
        "state_stale_ms",
        "min_visible_after_state_ok_ms",
        "menu_close_show_delay_ms",
        // height slicing
        "show_adjacent_floors",
        "adjacent_floor_opacity",
        "floor_z_tolerance",
        "floor_fade_uu",
        "floor_gradient_strength",
        "floor_base_color",
        "slice_hz",
        "feet_z_smooth_ms",
        "player_z_offset",
        "fallback_use_composite",
        // diagnostics
        "debug_readout",
        "debug_show_panel_on_start",
        // markers
        "markers_enabled",
        "markers_live",
        "markers_filter_chapter",
        "markers_rounds_per_sec",
        "markers_scan_chunk",
        "markers_scan_period_ms",
        "markers_categories",
        "markers_hide_found",
        "markers_found_alpha",
        "markers_size",
        "markers_clamp_to_edge",
        "markers_max_draw",
        "found_tracker",
        "found_save_debounce_ms",
        // minimap look
        "minimap_backdrop",
        "minimap_backdrop_color",
        "minimap_frame_color",
        "minimap_frame_alpha",
        "minimap_composite_alpha",
        "minimap_min_px",
        "minimap_arrow_frac",
        "minimap_arrow_min_px",
        "minimap_circle_segments",
        "waypoint_size_scale",
        // height-slice sizing
        "slice_min_px",
        "slice_max_px",
        "map_slice_margin",
        // the game-state reader
        "reader_position_period_ms",
        "reader_resolve_period_ms",
        "reader_widget_sweep_period_ms",
        "reader_transition_cooldown_ms",
        "reader_teleport_jump_uu",
        "reader_chapter_period_ms",
        "reader_max_widgets",
        "reader_max_menu_roots",
        "reader_max_levels",
        "reader_log_throttle_ms",
        // marker sweep internals
        "markers_live_grace_rounds",
        "markers_live_max",
        "markers_id_cache_max",
        "markers_class_cache_max",
        "markers_fallback_max_per_class",
        // assets and diagnostics
        "map_asset_retire_grace_ms",
        "hide_reason_log_ms",
        "srv_heap_size",
        // the full map
        "map_zoom",
        "map_zoom_min",
        "map_zoom_max",
        "map_zoom_factor",
        "map_pan_speed",
        "map_margin",
        "map_backdrop",
        "map_marker_size",
        "map_markers_max_draw",
        "map_floor_step",
        "map_show_all_floors",
        "map_slice_px",
        "map_slice_hz",
        "map_gamepad",
        "map_gamepad_deadzone",
        "map_waypoint_persist",
        // the x-ray highlight
        "highlight_enabled",
        "highlight_key",
        "highlight_gamepad",
        "highlight_pad_chord",
        "highlight_radius",
        "highlight_categories",
        "highlight_show_found",
        "highlight_max_draw",
        "highlight_alpha_near",
        "highlight_alpha_far",
        "highlight_size",
        "highlight_labels",
        "highlight_edge_arrows",
        "highlight_camera_hz",
        "highlight_camera_resolve_ms",
        "highlight_compass_period_ms",
        "highlight_getter_period_ms",
        "highlight_pov_scan_bytes",
        "highlight_pov_bad_reads",
        // the compass strip
        "compass_enabled",
        "compass_width",
        "compass_offset_y",
        "compass_height",
        "compass_span_deg",
        "compass_opacity",
        "compass_categories",
        "compass_marker_distance",
        "compass_show_waypoint",
        "compass_tick_step_deg",
        "compass_max_pips",
        // hotkeys
        "panel_key",
        "reload_key",
        "map_key",
        "map_recenter_key",
    };

    inline constexpr std::size_t kConfigKeyCount = sizeof(kConfigKeys) / sizeof(kConfigKeys[0]);

    inline bool is_known(std::string_view key)
    {
        for (const char* k : kConfigKeys)
        {
            if (key == k)
            {
                return true;
            }
        }
        return false;
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
