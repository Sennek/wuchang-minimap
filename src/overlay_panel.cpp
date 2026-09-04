//
// overlay_panel - the F2 settings panel.
//
// The Player, Advanced, Bindings and Debug tabs, the category chips, the player presets,
// the performance table and the panel's own state file.
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // Drawing: the F2 panel
        //==============================================================================

        //==============================================================================
        // The per-activity performance table
        //==============================================================================
        //
        // Every periodic activity in this mod records into perf.hpp's counter table;
        // this prints it. The columns: how often it runs, what an average invocation
        // costs, the worst since the peaks were last reset, the most recent, and which
        // thread pays. `avg` and `Hz` are over a rolling window (perf::kWindowMs).
        void draw_perf_table()
        {
            const perf::Table& pt = mm::perf_table();
            if (pt.count == 0)
            {
                return;
            }
            const std::uint64_t now = ::GetTickCount64();
            if (!ImGui::CollapsingHeader("Performance (per activity)"))
            {
                return;
            }
            if (ImGui::SmallButton("reset peaks"))
            {
                mm::perf_reset_peaks();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("a peak from a loading screen otherwise hides every later one");
            // All three threads measure wall clock, so a sample taken during a
            // synchronous load, a swapchain resize or a one-off blocking job is time
            // spent WAITING and would hide every later regression. Those samples go to
            // `stalls`, with their own worst case; hover a peak for the raw one that
            // includes them.
            ImGui::TextDisabled("peak ms = the worst sample OUTSIDE a load / resize / one-off job; "
                                "the rest are counted under stalls (hover a peak for the raw one)");
            {
                char why[128]{};
                ::WideCharToMultiByte(CP_UTF8, 0, mm::perf_last_stall(), -1, why, sizeof(why) - 1, nullptr,
                                      nullptr);
                ImGui::TextDisabled("last stall: %s%s", why, mm::perf_in_stall() ? " (now)" : "");
            }

            if (ImGui::BeginTable("perf", 7,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("activity");
                ImGui::TableSetupColumn("Hz");
                ImGui::TableSetupColumn("avg ms");
                ImGui::TableSetupColumn("peak ms");
                ImGui::TableSetupColumn("stalls");
                ImGui::TableSetupColumn("last ms");
                ImGui::TableSetupColumn("thread");
                ImGui::TableHeadersRow();
                for (int i = 0; i < pt.count; ++i)
                {
                    const perf::Counter& c = pt.c[i];
                    const bool is_idle = perf::idle(c, now);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", c.name != nullptr ? c.name : "?");
                    ImGui::TableNextColumn();
                    if (is_idle)
                    {
                        ImGui::TextDisabled("idle");
                    }
                    else
                    {
                        ImGui::Text("%.1f", c.rate_hz);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", c.avg_ms);
                    ImGui::TableNextColumn();
                    // Over a millisecond on a periodic path is a frame-time or
                    // game-thread problem.
                    if (c.peak_calm_ms >= 4.0)
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%.3f", c.peak_calm_ms);
                    }
                    else if (c.peak_calm_ms >= 1.0)
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.85f, 0.4f, 1.0f}, "%.3f", c.peak_calm_ms);
                    }
                    else
                    {
                        ImGui::Text("%.3f", c.peak_calm_ms);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("raw peak including stalls: %.3f ms", c.peak_ms);
                    }
                    ImGui::TableNextColumn();
                    if (c.stalls == 0)
                    {
                        ImGui::TextDisabled("-");
                    }
                    else
                    {
                        ImGui::TextDisabled("%llu / %.0f ms",
                                            static_cast<unsigned long long>(c.stalls),
                                            c.peak_stall_ms);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", c.last_ms);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", perf::thread_name(c.thread));
                }
                ImGui::EndTable();
            }
        }

        //==============================================================================
        // Category chips
        //==============================================================================
        //
        // A chip is a SmallButton filled with the category's own glyph colour when the
        // category is on and a flat outline when it is off, so the row doubles as the
        // legend. Three rows exist (minimap, x-ray, compass) and `base_id` keeps them
        // apart: ImGui identifies a widget by its label, so three rows of "Chest" in one
        // window would be one widget.
        bool category_chips(std::uint32_t& mask, int base_id, float wrap_width)
        {
            bool changed = false;
            const ImGuiStyle& style = ImGui::GetStyle();
            float x = 0.0f;
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(i);
                const bool on = mdb::cat_enabled(mask, cat);
                // A gutter for the glyph: the label is padded on the left and the glyph
                // drawn into that gap afterwards, so the filter shows the shape too.
                const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.30f);
                const float gutter = glyph_r * 2.0f + 4.0f;
                const char* label = mdb::cat_label(cat);
                const float w = ImGui::CalcTextSize(label).x + gutter + style.FramePadding.x * 4.0f;
                if (i > 0)
                {
                    if (x + w < wrap_width)
                    {
                        ImGui::SameLine();
                    }
                    else
                    {
                        x = 0.0f;
                    }
                }
                x += w + style.ItemSpacing.x;

                const ImU32 col = marker_color(cat, 255);
                const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(col);
                // Black text on a saturated fill, the category's colour as a thin
                // outline when off; the luminance test keeps a yellow chip readable.
                const float lum = 0.299f * fill.x + 0.587f * fill.y + 0.114f * fill.z;
                const ImVec4 text_col = on ? (lum > 0.55f ? ImVec4{0.06f, 0.06f, 0.06f, 1.0f}
                                                          : ImVec4{1.0f, 1.0f, 1.0f, 1.0f})
                                           : ImVec4{fill.x, fill.y, fill.z, 0.75f};
                const ImVec4 bg = on ? fill : ImVec4{fill.x * 0.18f, fill.y * 0.18f, fill.z * 0.18f, 0.55f};
                ImGui::PushID(base_id + i);
                ImGui::PushStyleColor(ImGuiCol_Button, bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4{fill.x * 0.75f, fill.y * 0.75f, fill.z * 0.75f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
                ImGui::PushStyleColor(ImGuiCol_Text, text_col);
                const std::string padded = std::string(static_cast<std::size_t>(
                                               gutter / (std::max)(1.0f, ImGui::CalcTextSize(" ").x)) + 1,
                                           ' ') + label;
                if (ImGui::SmallButton(padded.c_str()))
                {
                    mask = on ? (mask & ~mdb::cat_bit(cat)) : (mask | mdb::cat_bit(cat));
                    changed = true;
                }
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                draw_marker_glyph(ImGui::GetWindowDrawList(), cat,
                                  ImVec2{rmin.x + style.FramePadding.x + glyph_r,
                                         (rmin.y + rmax.y) * 0.5f},
                                  glyph_r, on ? IM_COL32(20, 22, 26, 235) : marker_color(cat, 210),
                                  on ? IM_COL32(235, 238, 242, 200) : IM_COL32(14, 16, 20, 160));
                ImGui::PopStyleColor(4);
                ImGui::PopID();
            }
            return changed;
        }

        // One category filter: a title, `all` / `none`, the config key it writes, then
        // the chip grid. The map and minimap share one mask, the compass and the x-ray
        // have their own, so the title says what the filter is FOR, not its key name.
        void category_filter(const char* title, const char* key, std::uint32_t& mask, int base_id,
                             float wrap_width)
        {
            ImGui::PushID(base_id);
            ImGui::TextUnformatted(title);
            ImGui::SameLine();
            if (ImGui::SmallButton("all"))
            {
                mask = mdb::kAllCats;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("none"))
            {
                mask = 0u;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", key);
            ImGui::PopID();
            category_chips(mask, base_id + 1, wrap_width);
        }

        //==============================================================================
        // Player presets
        //==============================================================================
        //
        // Three named starting points, each setting several Player keys at once. They
        // never touch a hotkey, the UI scale, the placement, the master switch or any
        // Advanced key.

        void apply_preset(mm::Config& cfg, Preset which)
        {
            const std::uint32_t chest = mdb::cat_bit(mdb::Cat::Chest);
            const std::uint32_t pickup = mdb::cat_bit(mdb::Cat::Pickup);
            const std::uint32_t hidden = mdb::cat_bit(mdb::Cat::Hidden);
            const std::uint32_t shrine = mdb::cat_bit(mdb::Cat::Shrine);
            const std::uint32_t boss = mdb::cat_bit(mdb::Cat::Boss);
            const std::uint32_t elite = mdb::cat_bit(mdb::Cat::Elite);
            const std::uint32_t fog = mdb::cat_bit(mdb::Cat::FogGate);
            const std::uint32_t npc = mdb::cat_bit(mdb::Cat::Npc);
            const std::uint32_t note = mdb::cat_bit(mdb::Cat::Note);
            const std::uint32_t door = mdb::cat_bit(mdb::Cat::Door);
            const std::uint32_t ladder = mdb::cat_bit(mdb::Cat::Ladder);
            const std::uint32_t lift = mdb::cat_bit(mdb::Cat::Lift);

            cfg.overlay_enabled = true;
            cfg.markers_enabled = true;
            cfg.highlight_enabled = true;
            switch (which)
            {
            case Preset::Minimal:
                // A small disc, only the landmarks you navigate by, nothing already
                // taken, no compass.
                cfg.show_minimap = true;
                cfg.size_frac = 0.16f;
                cfg.zoom_uu_per_px = 30.0f;
                cfg.opacity = 0.85f;
                cfg.markers_categories = shrine | boss | fog;
                cfg.markers_hide_found = true;
                cfg.markers_clamp_to_edge = false;
                cfg.compass_enabled = false;
                cfg.highlight_categories = chest | pickup;
                break;
            case Preset::Loot:
                // Sweeping a level: a close zoom, loot only, found things gone,
                // off-disc markers kept on the rim, a wide x-ray with quality colours.
                cfg.show_minimap = true;
                cfg.size_frac = 0.24f;
                cfg.zoom_uu_per_px = 20.0f;
                cfg.opacity = 0.92f;
                cfg.markers_categories = chest | pickup | hidden;
                cfg.markers_hide_found = true;
                cfg.markers_clamp_to_edge = true;
                cfg.compass_enabled = true;
                cfg.compass_categories = chest | pickup;
                cfg.highlight_categories = chest | pickup;
                cfg.highlight_radius = 5000.0f;
                cfg.highlight_labels = true;
                cfg.xray_rarity_colors_enabled = true;
                break;
            case Preset::Exploration:
            default:
                // Learning the level: a wide view, every landmark and connection
                // (doors, ladders, lifts), found markers still drawn. Enemies stay off -
                // the sweep refreshes them once a second, so they lag while they move.
                cfg.show_minimap = true;
                cfg.size_frac = 0.28f;
                cfg.zoom_uu_per_px = 40.0f;
                cfg.opacity = 0.92f;
                cfg.markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
                cfg.markers_hide_found = false;
                cfg.markers_clamp_to_edge = true;
                cfg.compass_enabled = true;
                cfg.compass_categories = shrine | boss | elite | fog | door | ladder | lift | npc;
                // `note` is in the x-ray set: a readable sign is worth pointing out.
                cfg.highlight_categories = chest | pickup | shrine | boss | npc | note;
                break;
            }
        }

        //==============================================================================
        // The F2 panel: Player / Advanced / Debug
        //==============================================================================
        //
        // The tabs are the config's tiers made visible: the Player tab is the Player
        // tier, the Advanced tab the Advanced tier, and the Debug tab the Dev tier plus
        // every read-only diagnostic.
        //
        // Each tab is its own function for a reason beyond tidiness: MSVC counts nested
        // blocks and C1061s this file otherwise.

        // One function per Player-tab section, so a CollapsingHeader can SKIP a
        // section's contents without wrapping 300 lines in an if.

        void player_presets(mm::Config& cfg)
        {
            ImGui::TextDisabled("set several of the settings on this tab at once");
            if (ImGui::Button("Minimal HUD"))
            {
                apply_preset(cfg, Preset::Minimal);
            }
            ImGui::SameLine();
            if (ImGui::Button("Loot hunting"))
            {
                apply_preset(cfg, Preset::Loot);
            }
            ImGui::SameLine();
            if (ImGui::Button("Exploration"))
            {
                apply_preset(cfg, Preset::Exploration);
            }
        }

        //--------------------------------------------------------------------------
        // Look: theme and palette
        //--------------------------------------------------------------------------
        //
        // In the FILE a theme only fills in colours the file does not mention; in the
        // PANEL choosing one is explicit, so it writes the theme's colours into the five
        // colour keys there and then - otherwise the combo looks broken for a player
        // whose config spells one of those keys out.
        void player_look(mm::Config& cfg)
        {
            int theme_i = static_cast<int>(cfg.theme);
            const char* themes[] = {"neutral", "ink"};
            if (ImGui::Combo("Theme (frame / backdrop / plates / fill)", &theme_i, themes, 2))
            {
                cfg.theme = static_cast<gly::Theme>(theme_i);
                const gly::ThemeColors tc = gly::theme_colors(cfg.theme);
                cfg.minimap_frame_r = static_cast<float>(tc.frame.r);
                cfg.minimap_frame_g = static_cast<float>(tc.frame.g);
                cfg.minimap_frame_b = static_cast<float>(tc.frame.b);
                cfg.minimap_frame_alpha = tc.frame_alpha;
                cfg.minimap_backdrop_r = static_cast<float>(tc.backdrop.r);
                cfg.minimap_backdrop_g = static_cast<float>(tc.backdrop.g);
                cfg.minimap_backdrop_b = static_cast<float>(tc.backdrop.b);
                cfg.minimap_backdrop = tc.backdrop_alpha;
                cfg.floor_base_r = static_cast<float>(tc.floor_base.r);
                cfg.floor_base_g = static_cast<float>(tc.floor_base.g);
                cfg.floor_base_b = static_cast<float>(tc.floor_base.b);
            }
            int pal_i = static_cast<int>(cfg.palette);
            const char* pals[] = {"default", "colorblind"};
            if (ImGui::Combo("Marker palette", &pal_i, pals, 2))
            {
                const gly::Palette was = cfg.palette;
                cfg.palette = static_cast<gly::Palette>(pal_i);
                // The item-quality tiers follow the palette only while they are still
                // the other palette's set, so a hand-picked xray_rarity_colors survives.
                bool untouched = true;
                const mdb::Rgb* old_set = gly::rarity_colors(was);
                for (int i = 0; i < mdb::kRarityCount; ++i)
                {
                    untouched = untouched && cfg.xray_rarity_colors[i] == old_set[i];
                }
                if (untouched)
                {
                    const mdb::Rgb* now = gly::rarity_colors(cfg.palette);
                    for (int i = 0; i < mdb::kRarityCount; ++i)
                    {
                        cfg.xray_rarity_colors[i] = now[i];
                    }
                }
            }
            ImGui::TextDisabled("every category has its own glyph shape");
        }

        //--------------------------------------------------------------------------
        // Minimap
        //--------------------------------------------------------------------------
        void player_minimap(mm::Config& cfg)
        {
            // The one line that answers "why is the minimap not there", on the tab a
            // player opens (the Debug tab is hidden without the unshipped dev config).
            // Only shown while the minimap is hidden.
            if (!g_last_mini.visible)
            {
                char reason[192]{};
                ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr,
                                      nullptr);
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f}, "hidden because: %s", reason);
            }
            ImGui::Checkbox("Overlay enabled", &cfg.overlay_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show minimap", &cfg.show_minimap);
            ImGui::SliderFloat("Size (fraction of screen height)", &cfg.size_frac, 0.08f, 0.6f, "%.2f");
            ImGui::SliderFloat("Zoom (uu per minimap pixel)", &cfg.zoom_uu_per_px, 4.0f, 200.0f, "%.0f");
            ImGui::SliderFloat("Opacity", &cfg.opacity, 0.15f, 1.0f, "%.2f");

            bool round_shape = cfg.round;
            if (ImGui::Checkbox("Round (off = square)", &round_shape))
            {
                cfg.round = round_shape;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Rotate with player (off = north up)", &cfg.rotate_with_player);
            ImGui::Checkbox("Show the floor below / above (dimmed)", &cfg.show_adjacent_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Hide while a menu is open", &cfg.hide_in_menus);
            ImGui::SliderFloat("Floor Z tolerance (uu)", &cfg.floor_z_tolerance, 20.0f, 800.0f, "%.0f");
        }

        //--------------------------------------------------------------------------
        // Placement and scale
        //--------------------------------------------------------------------------
        void player_placement(mm::Config& cfg)
        {
            // One key that moves the whole HUD. `custom` keeps the three placement keys
            // below in force; anything else overrides the minimap's corner and puts the
            // compass on the same vertical side.
            int preset = static_cast<int>(cfg.hud_preset);
            const char* presets[] = {"custom", "top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("HUD placement", &preset, presets, 5))
            {
                cfg.hud_preset = static_cast<mm::HudPreset>(preset);
            }
            ImGui::BeginDisabled(cfg.hud_preset != mm::HudPreset::Custom);
            int anchor = static_cast<int>(cfg.anchor);
            const char* anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("Minimap corner", &anchor, anchors, 4))
            {
                cfg.anchor = static_cast<mm::Anchor>(anchor);
            }
            ImGui::EndDisabled();
            ImGui::DragFloat("Offset X", &cfg.offset_x, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::DragFloat("Offset Y", &cfg.offset_y, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::TextDisabled("in 1080p pixels");

            // `auto` is a checkbox over the slider rather than a magic value inside the
            // number, so the slider always says what is in force.
            bool auto_scale = cfg.ui_scale_auto;
            if (ImGui::Checkbox("Scale the UI automatically", &auto_scale))
            {
                cfg.ui_scale_auto = auto_scale;
                if (!auto_scale)
                {
                    cfg.ui_scale = g_ui_scale; // start from what is on screen right now
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(in force: %.2f)", static_cast<double>(g_ui_scale));
            ImGui::BeginDisabled(cfg.ui_scale_auto);
            ImGui::SliderFloat("UI scale", &cfg.ui_scale, kUiScaleMin, kUiScaleMax, "%.2f");
            ImGui::EndDisabled();
        }

        //--------------------------------------------------------------------------
        // Markers
        //--------------------------------------------------------------------------
        void player_markers(mm::Config& cfg, float wrap)
        {
            ImGui::Checkbox("Show markers", &cfg.markers_enabled);
            ImGui::SameLine();
            // The inverse of markers_hide_found: the key is phrased as "hide", the
            // question a player asks is "show".
            bool show_found = !cfg.markers_hide_found;
            if (ImGui::Checkbox("Show found markers", &show_found))
            {
                cfg.markers_hide_found = !show_found;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(found ones are drawn hollow)");
            ImGui::SameLine();
            ImGui::Checkbox("Keep out-of-range markers on the rim", &cfg.markers_clamp_to_edge);
            ImGui::SliderFloat("Marker size (px)", &cfg.markers_size, 2.0f, 16.0f, "%.1f");
            // The chips ARE the legend: each one is filled with the colour that category
            // is drawn in on the map.
            category_filter("Map & minimap", "markers_categories", cfg.markers_categories, 1000, wrap);
        }

        //--------------------------------------------------------------------------
        // Collection tracker
        //--------------------------------------------------------------------------
        void player_tracker(mm::Config& cfg)
        {
            ImGui::Checkbox("Remember what I have collected", &cfg.found_tracker);
            ImGui::SameLine();
            ImGui::Checkbox("Mark items whose level is loaded but absent", &cfg.markers_absence_marks);
            const markers::Stats st = markers::stats();
            // Which file, and how it was chosen: a per-save tracker that picked the
            // wrong save looks exactly like a lost collection.
            ImGui::Text("Profile: %s", st.found_file[0] != '\0' ? st.found_file : "(none yet)");
            ImGui::SameLine();
            ImGui::TextDisabled("(via %s)", st.found_route[0] != '\0' ? st.found_route : "unresolved");
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("found_profile", cfg.found_profile, sizeof(cfg.found_profile)))
            {
                // Free text on purpose: `auto`, `shared`, or a name of the player's own.
                // It takes effect on Save (or F5) - the loop thread owns the file.
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("auto = one file per save slot (recommended)\n"
                                  "shared = one file for every save\n"
                                  "anything else = wuchang_minimap_found_<name>.txt");
            }
            //---- import / export ------------------------------------------------------
            //
            // The found list and the waypoints of this profile as one JSON file in the
            // mod folder. Both buttons only raise a flag: the loop thread owns every
            // read and write (overlay.cpp).
            ImGui::SeparatorText("Backup / transfer");
            if (ImGui::Button("Export"))
            {
                g_export_request.store(true, std::memory_order_release);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("writes wuchang_minimap_export_<date>_<time>.json next to the DLL");
            }
            static char import_path[512]{};
            static bool import_primed = false;
            // The newest export the loop thread found, offered once so a typed path is
            // never overwritten under the cursor.
            if (!import_primed && g_latest_export_ready.load(std::memory_order_acquire))
            {
                spin::SpinGuard guard(g_exchange_lock);
                ::strncpy_s(import_path, sizeof(import_path), g_latest_export, _TRUNCATE);
                import_primed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Import"))
            {
                {
                    spin::SpinGuard guard(g_exchange_lock);
                    ::strncpy_s(g_import_path, sizeof(g_import_path), import_path, _TRUNCATE);
                }
                g_import_request.store(true, std::memory_order_release);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("merges the file's found ids and appends its waypoints;\n"
                                  "nothing is ever removed by an import");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##import_path", "path to a .json export", import_path,
                                     sizeof(import_path));

            // The collection-statistics page, shared with the full map's Stats panel.
            draw_collection_stats(::GetTickCount64(), false);
        }

        //--------------------------------------------------------------------------
        // Full map
        //--------------------------------------------------------------------------
        void player_fullmap(mm::Config& cfg)
        {
            ImGui::TextDisabled("Press %s in-world.",
                                key_name_ascii(cfg.map_key).c_str());
            ImGui::SliderFloat("Zoom on open (uu per screen px)", &cfg.map_zoom, cfg.map_zoom_min,
                               cfg.map_zoom_max, "%.0f");
            ImGui::SliderFloat("Map marker size (px)", &cfg.map_marker_size, 3.0f, 24.0f, "%.1f");
            ImGui::Checkbox("Show every floor (ignore the height slice)", &cfg.map_show_all_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad (XInput)", &cfg.map_gamepad);
            ImGui::SameLine();
            ImGui::Checkbox("Remember the waypoints", &cfg.map_waypoint_persist);
            //---- waypoints ------------------------------------------------------------
            const mv::WaypointSet wps = mm::waypoints();
            if (wps.count == 0)
            {
                ImGui::TextDisabled("no waypoints - right-click on the full map to drop one, or press "
                                    "%s in-world for the nearest unfound marker",
                                    key_name_ascii(cfg.waypoint_nearest_key).c_str());
            }
            else
            {
                ImGui::Text("%zu waypoint(s) of %zu", wps.count, mv::kMaxWaypoints);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear waypoints"))
                {
                    mm::clear_waypoints();
                }
                for (std::size_t wi = 0; wi < wps.count; ++wi)
                {
                    ImGui::PushID(static_cast<int>(wi) + 4100);
                    if (ImGui::SmallButton("X"))
                    {
                        mm::remove_waypoint(wi);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%zu.  X %.0f  Y %.0f  Z %.0f", wi + 1, wps.items[wi].x, wps.items[wi].y,
                                wps.items[wi].z);
                    ImGui::PopID();
                }
            }
        }

        //--------------------------------------------------------------------------
        // The hold-key x-ray highlight
        //--------------------------------------------------------------------------
        void player_xray(mm::Config& cfg, float wrap)
        {
            std::string hold = key_name_ascii(cfg.highlight_key);
            if (cfg.highlight_gamepad)
            {
                hold += " or pad " + wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                                     cfg.highlight_pad_lt,
                                                                     cfg.highlight_pad_rt));
            }
            // The mode, next to the key it applies to.
            int hl_mode = cfg.highlight_mode == mm::HighlightMode::Hold ? 1 : 0;
            ImGui::TextUnformatted("Mode");
            ImGui::SameLine();
            // Both radios must be drawn every frame, so neither call may sit behind a
            // short-circuiting || - the second would vanish on the frame the first was
            // clicked.
            bool hl_mode_changed = ImGui::RadioButton("Toggle", &hl_mode, 0);
            ImGui::SameLine();
            hl_mode_changed = ImGui::RadioButton("Hold", &hl_mode, 1) || hl_mode_changed;
            if (hl_mode_changed)
            {
                cfg.highlight_mode = hl_mode == 1 ? mm::HighlightMode::Hold : mm::HighlightMode::Toggle;
            }
            if (cfg.highlight_mode == mm::HighlightMode::Hold)
            {
                ImGui::TextWrapped("Hold %s in-world to see nearby markers through walls.", hold.c_str());
            }
            else
            {
                ImGui::TextWrapped("Press %s in-world to see nearby markers through walls, and again to "
                                   "hide them. A level transition turns it off.",
                                   hold.c_str());
            }
            ImGui::Checkbox("Enabled##xray", &cfg.highlight_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad chord", &cfg.highlight_gamepad);
            ImGui::SameLine();
            ImGui::Checkbox("Names + distance", &cfg.highlight_labels);
            ImGui::SliderFloat("Radius (uu)", &cfg.highlight_radius, 200.0f, 20000.0f, "%.0f");
            ImGui::SameLine();
            ImGui::TextDisabled("= %.0f m", static_cast<double>(cfg.highlight_radius) / 100.0);
            ImGui::SliderFloat("Glyph size (px)", &cfg.highlight_size, 2.0f, 24.0f, "%.1f");
            ImGui::Checkbox("Colour by item quality", &cfg.xray_rarity_colors_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Also tint the minimap / map / compass", &cfg.markers_rarity_tint);
            ImGui::TextDisabled("colours pickups by the game's own item-type grouping");
            category_filter("X-ray highlight", "highlight_categories", cfg.highlight_categories, 2000,
                            wrap);
        }

        //--------------------------------------------------------------------------
        // The compass strip
        //--------------------------------------------------------------------------
        void player_compass(mm::Config& cfg, float wrap)
        {
            ImGui::Checkbox("Enabled##compass", &cfg.compass_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show the waypoint bearing", &cfg.compass_show_waypoint);
            ImGui::SliderFloat("Width (fraction of the screen)", &cfg.compass_width, 0.1f, 1.0f, "%.2f");
            // Overridden by a non-custom hud_preset, hence greyed out when one is set.
            ImGui::BeginDisabled(cfg.hud_preset != mm::HudPreset::Custom);
            int canchor = cfg.compass_anchor == mm::VAnchor::Bottom ? 1 : 0;
            const char* canchors[] = {"top", "bottom"};
            if (ImGui::Combo("Edge", &canchor, canchors, 2))
            {
                cfg.compass_anchor = canchor == 1 ? mm::VAnchor::Bottom : mm::VAnchor::Top;
            }
            ImGui::EndDisabled();
            ImGui::SliderFloat("Distance from that edge (px)", &cfg.compass_offset_y, 0.0f, 400.0f, "%.0f");
            ImGui::SliderFloat("Degrees across the strip", &cfg.compass_span_deg, 30.0f, 360.0f, "%.0f");
            ImGui::SliderFloat("Compass opacity", &cfg.compass_opacity, 0.1f, 1.0f, "%.2f");
            ImGui::Checkbox("Distance in metres under each pip", &cfg.compass_pip_labels);
            category_filter("Compass", "compass_categories", cfg.compass_categories, 3000, wrap);
        }

        //--------------------------------------------------------------------------
        // Keys
        //--------------------------------------------------------------------------
        // Built by the same builder as the full map's footer, so a rebind cannot make
        // one of the two lie.
        void player_keys(mm::Config& cfg)
        {
            ImGui::TextWrapped("%s", bindings_hint(cfg).c_str());
            ImGui::TextDisabled("rebind them on the Bindings tab");
        }

        //==============================================================================
        // THE PANEL'S OWN STATE FILE
        //==============================================================================
        //
        // Which Player-tab sections are folded up, remembered between sessions: one
        // line, one number, in wuchang_minimap_panel.txt beside the config.
        //
        // Not imgui.ini - io.IniFilename is nullptr and stays that way, or the panel's
        // "come back centred" behaviour stops working. Not a config key either: it is
        // not a setting, and it must not appear in the file a Save writes or in the
        // drift test that guards it.
        //

        std::wstring panel_state_path()
        {
            return mm::mod_dir() + L"\\wuchang_minimap_panel.txt";
        }

        // Loop thread. Plain CreateFileW/ReadFile and a hand-rolled hex parse: this mod
        // uses no iostreams anywhere.
        void panel_state_load()
        {
            if (g_panel_state_loaded.exchange(true))
            {
                return;
            }
            const HANDLE h = ::CreateFileW(panel_state_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return; // no file yet: every section open, which is what 1.0.0 did
            }
            char buf[256]{};
            DWORD read = 0;
            const bool ok = ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) != 0;
            ::CloseHandle(h);
            if (!ok || read == 0)
            {
                return;
            }
            const char* p = ::strstr(buf, "sections");
            if (p == nullptr)
            {
                return;
            }
            p = ::strchr(p, '=');
            if (p == nullptr)
            {
                return;
            }
            ++p;
            while (*p == ' ' || *p == '\t')
            {
                ++p;
            }
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            {
                base = 16;
                p += 2;
            }
            std::uint32_t v = 0;
            bool any = false;
            for (; *p != '\0'; ++p)
            {
                int d = -1;
                if (*p >= '0' && *p <= '9')
                {
                    d = *p - '0';
                }
                else if (base == 16 && *p >= 'a' && *p <= 'f')
                {
                    d = *p - 'a' + 10;
                }
                else if (base == 16 && *p >= 'A' && *p <= 'F')
                {
                    d = *p - 'A' + 10;
                }
                if (d < 0)
                {
                    break;
                }
                v = v * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(d);
                any = true;
            }
            if (any)
            {
                g_panel_sections.store(v, std::memory_order_relaxed);
                MM_LOGV(L"panel state: sections 0x{:X}", v);
            }
        }

        // Loop thread, and only when the render thread says something changed.
        void panel_state_save()
        {
            char text[256]{};
            const int n = std::snprintf(text, sizeof(text),
                                        "; WuchangMinimap - where you left the F2 panel. Not a setting:\r\n"
                                        "; delete this file to get every section back open.\r\n"
                                        "sections = 0x%X\r\n",
                                        g_panel_sections.load(std::memory_order_relaxed));
            if (n <= 0)
            {
                return;
            }
            const HANDLE h = ::CreateFileW(panel_state_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD written = 0;
            ::WriteFile(h, text, static_cast<DWORD>(n), &written, nullptr);
            ::CloseHandle(h);
        }

        //==============================================================================
        // THE PLAYER TAB
        //==============================================================================
        //
        // Ten collapsible sections whose fold state lives in the panel's own state file
        // (panel_state_load / panel_state_save - never imgui.ini), plus a filter box.
        // Matching is per SECTION, against its title and the words its settings are
        // named with (`kSections` below), not per widget: filtering widgets would mean a
        // test around each of the ~120 calls inside the section functions.

        // Thin adapters, so every section has the same signature and the table stays a
        // table. (`wrap` is the content width the category-chip rows need.)
        void sec_presets(mm::Config& cfg, float) { player_presets(cfg); }
        void sec_look(mm::Config& cfg, float) { player_look(cfg); }
        void sec_minimap(mm::Config& cfg, float) { player_minimap(cfg); }
        void sec_placement(mm::Config& cfg, float) { player_placement(cfg); }
        void sec_markers(mm::Config& cfg, float wrap) { player_markers(cfg, wrap); }
        void sec_tracker(mm::Config& cfg, float) { player_tracker(cfg); }
        void sec_fullmap(mm::Config& cfg, float) { player_fullmap(cfg); }
        void sec_xray(mm::Config& cfg, float wrap) { player_xray(cfg, wrap); }
        void sec_compass(mm::Config& cfg, float wrap) { player_compass(cfg, wrap); }
        void sec_keys(mm::Config& cfg, float) { player_keys(cfg); }

        constexpr PanelSection kSections[] = {
            {"Presets", "preset hud layout corner placement", &sec_presets},
            {"Look", "theme palette colour color opacity ink neutral colourblind font scale", &sec_look},
            {"Minimap", "minimap shape round square zoom size rotate north floors adjacent", &sec_minimap},
            {"Placement and scale", "anchor offset position ui scale dpi corner", &sec_placement},
            {"Markers", "markers categories glyph size found hide clamp edge rarity quality", &sec_markers},
            {"Collection tracker", "collection tracker found profile save slot absence", &sec_tracker},
            {"Full map", "full map zoom gamepad waypoint shrine list travel", &sec_fullmap},
            {"X-ray highlight", "x-ray xray highlight through walls hold toggle radius labels", &sec_xray},
            {"Compass", "compass strip heading pips width degrees plate", &sec_compass},
            {"Keys", "keys hotkeys bindings rebind", &sec_keys},
        };
        constexpr int kSectionCount = static_cast<int>(std::size(kSections));
        static_assert(kSectionCount <= 32, "one bit per section in g_panel_sections");

        // Case-insensitive substring, both ways round: "colour" finds "Look" through
        // its words, "compa" finds "Compass" through its title.
        bool section_matches(const PanelSection& s, const char* needle)
        {
            if (needle == nullptr || needle[0] == '\0')
            {
                return true;
            }
            char low[64]{};
            std::size_t n = 0;
            for (const char* p = needle; *p != '\0' && n + 1 < sizeof(low); ++p)
            {
                low[n++] = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
            }
            if (n == 0)
            {
                return true;
            }
            // Both haystacks are ASCII literals and there is no _stristr, so the needle
            // is lowered once above and compared case-insensitively here.
            const auto contains = [&low, n](const char* hay) {
                for (const char* h = hay; *h != '\0'; ++h)
                {
                    if (::_strnicmp(h, low, n) == 0)
                    {
                        return true;
                    }
                }
                return false;
            };
            return contains(s.title) || contains(s.words);
        }

        void panel_player(mm::Config& cfg)
        {
            const float wrap = ImGui::GetContentRegionAvail().x;

            // ---- the filter ----------------------------------------------------------
            static char filter[64]{};
            ImGui::SetNextItemWidth(220.0f * g_ui_scale);
            ImGui::InputTextWithHint("##filter", "filter settings...", filter, sizeof(filter));
            ImGui::SameLine();
            if (ImGui::SmallButton("clear"))
            {
                filter[0] = '\0';
            }
            const bool filtering = filter[0] != '\0';
            ImGui::SameLine();
            if (filtering)
            {
                ImGui::TextDisabled("matching sections only");
            }
            else
            {
                ImGui::TextDisabled("type a setting's name");
            }

            // ---- the sections --------------------------------------------------------
            std::uint32_t bits = g_panel_sections.load(std::memory_order_relaxed);
            const std::uint32_t before_bits = bits;
            int shown = 0;
            for (int i = 0; i < kSectionCount; ++i)
            {
                const PanelSection& sec = kSections[i];
                if (!section_matches(sec, filter))
                {
                    continue;
                }
                ++shown;
                const std::uint32_t bit = 1u << i;
                // While filtering, everything that matched is forced open. The stored
                // bit is not touched by that (`Always` sets the state without asking the
                // header), so clearing the filter restores the fold exactly.
                if (filtering)
                {
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                }
                else
                {
                    ImGui::SetNextItemOpen((bits & bit) != 0, ImGuiCond_Always);
                }
                if (ImGui::CollapsingHeader(sec.title))
                {
                    if (!filtering)
                    {
                        bits |= bit;
                    }
                    sec.draw(cfg, wrap);
                }
                else if (!filtering)
                {
                    bits &= ~bit;
                }
            }
            if (shown == 0)
            {
                ImGui::TextDisabled("nothing matches '%s'", filter);
            }
            if (bits != before_bits)
            {
                g_panel_sections.store(bits, std::memory_order_relaxed);
                g_panel_state_dirty.store(true, std::memory_order_release);
            }

            // ---- reset ---------------------------------------------------------------
            ImGui::Spacing();
            ImGui::Separator();
            // Two clicks: this throws away every tuned value in the struct. Armed until
            // the panel is closed or the button is pressed.
            static bool confirm_reset = false;
            if (!confirm_reset)
            {
                if (ImGui::Button("Reset to the shipped defaults"))
                {
                    confirm_reset = true;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Every setting back to what the mod ships with.\n"
                                      "Revert only re-reads the file, so it cannot undo a saved value.");
                }
            }
            else
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "Reset every setting?");
                ImGui::SameLine();
                if (ImGui::Button("Yes, reset"))
                {
                    confirm_reset = false;
                    const bool was_on = cfg.mod_enabled;
                    cfg = mm::Config{};
                    // The master switch is not a preference: it has its own checkbox
                    // and its own log line.
                    cfg.mod_enabled = was_on;
                    mm::log(L"config: reset to the shipped defaults from the F2 panel (not saved yet)");
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    confirm_reset = false;
                }
            }
            ImGui::TextDisabled("Nothing is written until Save.");
        }

        void panel_advanced(mm::Config& cfg)
        {
            const float wrap = ImGui::GetContentRegionAvail().x;

            ImGui::TextDisabled("Correct as shipped.");

            if (ImGui::CollapsingHeader("When the overlay is allowed on screen"))
            {
                ImGui::Checkbox("Only when the camera follows the pawn", &cfg.require_pawn_view);
                ImGui::SliderInt("State stale after (ms)", &cfg.state_stale_ms, 100, 5000);
                ImGui::SliderInt("Grace after a valid pawn (ms)", &cfg.min_visible_after_state_ok_ms, 0, 3000);
                ImGui::SliderInt("Delay after a menu closes (ms)", &cfg.menu_close_show_delay_ms, 0, 1000);
            }

            if (ImGui::CollapsingHeader("Height slicing"))
            {
                ImGui::SliderFloat("Adjacent floor opacity", &cfg.adjacent_floor_opacity, 0.0f, 0.6f, "%.2f");
                ImGui::SliderFloat("Below / above fade range (uu)", &cfg.floor_fade_uu, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Height gradient strength", &cfg.floor_gradient_strength, 0.0f, 0.6f, "%.2f");
                float base[3] = {cfg.floor_base_r / 255.0f, cfg.floor_base_g / 255.0f, cfg.floor_base_b / 255.0f};
                if (ImGui::ColorEdit3("Walkable fill colour", base, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.floor_base_r = base[0] * 255.0f;
                    cfg.floor_base_g = base[1] * 255.0f;
                    cfg.floor_base_b = base[2] * 255.0f;
                }
                ImGui::SliderInt("Slice rate (Hz)", &cfg.slice_hz, 2, 30);
                ImGui::SliderInt("Feet Z smoothing (ms)", &cfg.feet_z_smooth_ms, 1, 1000);
                ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -200.0f,
                                   200.0f, "%.0f");
            }

            if (ImGui::CollapsingHeader("Marker sweep and tracker"))
            {
                ImGui::Checkbox("Live actor sweep", &cfg.markers_live);
                ImGui::SameLine();
                ImGui::Checkbox("Only this chapter's markers", &cfg.markers_filter_chapter);
                ImGui::SameLine();
                {
                    const markers::Stats fs = markers::stats();
                    if (!cfg.markers_filter_chapter)
                    {
                        ImGui::TextDisabled("(off - all chapters drawn)");
                    }
                    else if (fs.filter_chapter == chid::kNone)
                    {
                        ImGui::TextDisabled("(chapter not detected yet - all drawn)");
                    }
                    else if (fs.filter_chapter == chid::kDlc)
                    {
                        ImGui::TextDisabled("(showing DLC)");
                    }
                    else
                    {
                        ImGui::Text("(showing chapter %d)", fs.filter_chapter);
                    }
                }
                // The two knobs that trade game-thread time for marker freshness. The
                // "scan pump" line on the Debug tab is the read-out that says which way
                // to move them.
                ImGui::SliderInt("Full passes per second", &cfg.markers_rounds_per_sec, 1, 10);
                ImGui::SliderInt("Object slots per pump", &cfg.markers_scan_chunk, scan::kChunkMin,
                                 scan::kChunkMax);
                ImGui::SliderInt("Min ms between pumps", &cfg.markers_scan_period_ms, scan::kPeriodMinMs,
                                 scan::kPeriodMaxMs);
                ImGui::SliderFloat("Found marker opacity", &cfg.markers_found_alpha, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max markers drawn per frame", &cfg.markers_max_draw, 0, 4000);
                ImGui::SliderInt("Found file debounce (ms)", &cfg.found_save_debounce_ms, 200, 20000);
                ImGui::SeparatorText("Absence as evidence of a collect");
                ImGui::SliderInt("Confirming rounds", &cfg.markers_absence_rounds, 1, 30);
                category_filter("Absence rule", "markers_absence_categories",
                                cfg.markers_absence_categories, 4000, wrap);
            }

            if (ImGui::CollapsingHeader("Minimap look"))
            {
                ImGui::SliderFloat("Backdrop opacity", &cfg.minimap_backdrop, 0.0f, 1.0f, "%.2f");
                float back[3] = {cfg.minimap_backdrop_r / 255.0f, cfg.minimap_backdrop_g / 255.0f,
                                 cfg.minimap_backdrop_b / 255.0f};
                if (ImGui::ColorEdit3("Backdrop colour", back, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.minimap_backdrop_r = back[0] * 255.0f;
                    cfg.minimap_backdrop_g = back[1] * 255.0f;
                    cfg.minimap_backdrop_b = back[2] * 255.0f;
                }
                float frame[3] = {cfg.minimap_frame_r / 255.0f, cfg.minimap_frame_g / 255.0f,
                                  cfg.minimap_frame_b / 255.0f};
                if (ImGui::ColorEdit3("Frame colour", frame, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.minimap_frame_r = frame[0] * 255.0f;
                    cfg.minimap_frame_g = frame[1] * 255.0f;
                    cfg.minimap_frame_b = frame[2] * 255.0f;
                }
                ImGui::SliderFloat("Frame alpha", &cfg.minimap_frame_alpha, 0.0f, 1.0f, "%.2f");
                // The zoom ladder is a list and is edited in the file; the panel shows
                // what is in force and which key steps through it.
                {
                    std::string ladder;
                    for (int i = 0; i < cfg.minimap_zoom_preset_count && i < mv::kMaxZoomPresets; ++i)
                    {
                        ladder += std::format("{}{:.0f}", ladder.empty() ? "" : ", ",
                                              cfg.minimap_zoom_presets[i]);
                    }
                    ImGui::TextDisabled("zoom presets (%s cycles): %s", key_name_ascii(cfg.zoom_key).c_str(),
                                        ladder.empty() ? "none - edit minimap_zoom_presets" : ladder.c_str());
                }
                ImGui::SliderFloat("Minimum side (px)", &cfg.minimap_min_px, 16.0f, 512.0f, "%.0f");
                ImGui::SliderFloat("Player arrow (fraction of the side)", &cfg.minimap_arrow_frac, 0.01f,
                                   0.3f, "%.3f");
                ImGui::SliderFloat("Player arrow minimum (px)", &cfg.minimap_arrow_min_px, 2.0f, 64.0f, "%.0f");
                ImGui::SliderFloat("Waypoint size (x marker size)", &cfg.waypoint_size_scale, 0.2f, 4.0f,
                                   "%.2f");
            }

            if (ImGui::CollapsingHeader("Full map tuning"))
            {
                ImGui::SliderFloat("Zoom limit - closest", &cfg.map_zoom_min, 1.0f, 200.0f, "%.0f");
                ImGui::SliderFloat("Zoom limit - furthest", &cfg.map_zoom_max, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Zoom per wheel notch", &cfg.map_zoom_factor, 1.02f, 1.6f, "%.2f");
                ImGui::SliderFloat("Pan speed (screen px per second)", &cfg.map_pan_speed, 100.0f, 4000.0f,
                                   "%.0f");
                ImGui::SliderFloat("Margin (fraction of screen height)", &cfg.map_margin, 0.0f, 0.3f, "%.3f");
                ImGui::SliderFloat("Backdrop opacity##map", &cfg.map_backdrop, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max markers drawn##map", &cfg.map_markers_max_draw, 0, 20000);
                ImGui::SliderFloat("Floor step (uu)", &cfg.map_floor_step, 20.0f, 2000.0f, "%.0f");
                ImGui::SliderInt("Slice texture width (px)", &cfg.map_slice_px, 256, 2048);
                ImGui::SliderInt("Slice rate cap (Hz)", &cfg.map_slice_hz, 1, 30);
                ImGui::SliderFloat("Gamepad deadzone", &cfg.map_gamepad_deadzone, 0.05f, 0.6f, "%.2f");
            }

            if (ImGui::CollapsingHeader("X-ray tuning"))
            {
                bool hide_loot = !cfg.highlight_show_found;
                if (ImGui::Checkbox("Hide collected loot", &hide_loot))
                {
                    cfg.highlight_show_found = !hide_loot;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(chests, pickups, hidden items)");
                ImGui::SameLine();
                ImGui::Checkbox("Edge arrows (off screen / behind)", &cfg.highlight_edge_arrows);
                ImGui::SliderInt("Max drawn (nearest first)", &cfg.highlight_max_draw, 1, 400);
                ImGui::SliderInt("Max labelled (nearest first)", &cfg.highlight_labels_max, 0, 40);
                ImGui::SameLine();
                ImGui::TextDisabled("names only");
                ImGui::SliderFloat("Alpha at the camera", &cfg.highlight_alpha_near, 0.1f, 1.0f, "%.2f");
                ImGui::SliderFloat("Alpha at the radius", &cfg.highlight_alpha_far, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Camera read rate (Hz)", &cfg.highlight_camera_hz, 5, 240);
                ImGui::SeparatorText("Item quality palette");
                for (int i = 1; i < mdb::kRarityCount; ++i)
                {
                    mdb::Rgb& c = cfg.xray_rarity_colors[i];
                    float rgb[3] = {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                                    static_cast<float>(c.b) / 255.0f};
                    ImGui::PushID(i + 700);
                    if (ImGui::ColorEdit3(mdb::rarity_name(i), rgb,
                                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
                    {
                        const auto to_byte = [](float v) {
                            const float clamped = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
                            return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
                        };
                        c.r = to_byte(rgb[0]);
                        c.g = to_byte(rgb[1]);
                        c.b = to_byte(rgb[2]);
                    }
                    ImGui::PopID();
                    if (i + 1 < mdb::kRarityCount)
                    {
                        ImGui::SameLine();
                    }
                }
            }

            if (ImGui::CollapsingHeader("Diagnostics"))
            {
                // `normal` is what ships; the other two reproduce a problem with the
                // running commentary on, without editing a file.
                int lv = static_cast<int>(cfg.log_level);
                if (ImGui::Combo("Log detail", &lv, "normal\0verbose\0trace\0"))
                {
                    cfg.log_level = static_cast<mm::LogLv>(lv);
                }
                ImGui::TextWrapped(
                    "normal = what a bug report needs. verbose = the running commentary "
                    "(player state, menu open/close, why the minimap is hidden). trace = "
                    "everything, including a marker census every two seconds. Takes effect "
                    "as soon as you press Save.");
                char logpath[MAX_PATH * 2]{};
                ::WideCharToMultiByte(CP_UTF8, 0, mm::modlog_path().c_str(), -1, logpath,
                                      sizeof(logpath) - 1, nullptr, nullptr);
                ImGui::TextDisabled("Log file: %s", logpath);
            }

            if (ImGui::CollapsingHeader("Compass tuning"))
            {
                ImGui::SliderFloat("Height (px)", &cfg.compass_height, 10.0f, 120.0f, "%.0f");
                ImGui::Checkbox("Filled plate behind the strip", &cfg.compass_plate);
                ImGui::SameLine();
                ImGui::TextDisabled("off = ticks and letters with a shadow");
                ImGui::SliderFloat("Marker bearing range (uu)", &cfg.compass_marker_distance, 500.0f,
                                   60000.0f, "%.0f");
                ImGui::SliderFloat("Minor tick spacing (deg)", &cfg.compass_tick_step_deg, 1.0f, 90.0f, "%.0f");
                ImGui::SliderInt("Max bearing pips", &cfg.compass_max_pips, 0, 256);
                ImGui::SliderFloat("Above / below arrow from (uu)", &cfg.compass_pip_height_uu, 0.0f,
                                   3000.0f, "%.0f");
                ImGui::SameLine();
                ImGui::TextDisabled("= %.1f m", static_cast<double>(cfg.compass_pip_height_uu) / 100.0);
            }
        }

        // The Dev tier (config_wuchang_minimap_dev.txt). Its own function so the Debug
        // tab does not become one more 300-line block.
        void panel_dev_keys(mm::Config& cfg)
        {
            if (!ImGui::CollapsingHeader("Developer settings (config_wuchang_minimap_dev.txt)"))
            {
                return;
            }
            ImGui::TextDisabled("Saved to config_wuchang_minimap_dev.txt.");
            ImGui::Checkbox("Debug readout (this tab)", &cfg.debug_readout);
            ImGui::SameLine();
            ImGui::Checkbox("Open the panel on start", &cfg.debug_show_panel_on_start);
            ImGui::Checkbox("Load the composite fallback picture", &cfg.fallback_use_composite);
            ImGui::SliderFloat("Composite alpha", &cfg.minimap_composite_alpha, 0.0f, 1.0f, "%.2f");
            ImGui::SeparatorText("Game-state reader");
            ImGui::SliderInt("Position period (ms)", &cfg.reader_position_period_ms, 16, 1000);
            ImGui::SliderInt("Resolve period (ms)", &cfg.reader_resolve_period_ms, 100, 10000);
            ImGui::SliderInt("Widget sweep, fast (ms)", &cfg.reader_widget_sweep_period_ms, 50, 5000);
            ImGui::SliderInt("Widget sweep, backed off (ms)", &cfg.reader_widget_sweep_max_period_ms, 50,
                             30000);
            ImGui::SliderInt("Widget sweep warm window (ms)", &cfg.reader_widget_sweep_warm_ms, 0, 60000);
            ImGui::SliderInt("Transition cooldown (ms)", &cfg.reader_transition_cooldown_ms, 0, 30000);
            {
                float jump = static_cast<float>(cfg.reader_teleport_jump_uu);
                if (ImGui::SliderFloat("Teleport jump (uu per pump)", &jump, 200.0f, 20000.0f, "%.0f"))
                {
                    cfg.reader_teleport_jump_uu = static_cast<double>(jump);
                }
            }
            ImGui::SliderInt("Chapter period (ms)", &cfg.reader_chapter_period_ms, 200, 60000);
            ImGui::SliderInt("Reader log throttle (ms)", &cfg.reader_log_throttle_ms, 500, 60000);
            ImGui::SeparatorText("Sweep, assets, diagnostics");
            ImGui::SliderInt("Live grace rounds", &cfg.markers_live_grace_rounds, 1, 30);
            ImGui::SliderInt("Asset retire grace (ms)", &cfg.map_asset_retire_grace_ms, 0, 60000);
            ImGui::SliderInt("Hide-reason log throttle (ms)", &cfg.hide_reason_log_ms, 0, 60000);
            ImGui::SliderInt("SRV heap size (RESTART)", &cfg.srv_heap_size, 16, 1024);
            ImGui::SeparatorText("X-ray camera reader");
            ImGui::SliderInt("Camera resolve (ms)", &cfg.highlight_camera_resolve_ms, 100, 10000);
            ImGui::SliderInt("Compass-only period (ms)", &cfg.highlight_compass_period_ms, 10, 1000);
            ImGui::SliderInt("Getter fallback period (ms)", &cfg.highlight_getter_period_ms, 10, 1000);
            ImGui::SliderInt("POV scan bytes", &cfg.highlight_pov_scan_bytes, 64, 1024);
            ImGui::SliderInt("POV bad reads before re-pin", &cfg.highlight_pov_bad_reads, 1, 64);
        }

        //==============================================================================
        // The Bindings tab
        //==============================================================================
        //
        // Every hotkey in one place, with a "press a key" capture. Three rules make it
        // safe:
        //   * the capture only accepts a key mm::vk_bindable() says the config file can
        //     spell, so a binding always survives a save and a reload;
        //   * while it is armed mm::g_key_capture makes the WndProc hook swallow the
        //     whole keyboard, so the key being bound cannot also reach the game;
        //   * it waits for every key to be released first, or the click that armed it
        //     would capture whatever the player is still holding down.
        // A change lands in the live config on the same frame (draw_panel diffs the
        // struct and publishes it), and is written to the file by Save like anything
        // else.

        //==============================================================================
        // MODIFIERS, AND THE KEYS THE GAME ITSELF WANTS
        //==============================================================================

        bool is_modifier_vk(int vk)
        {
            switch (vk)
            {
            case VK_SHIFT:
            case VK_CONTROL:
            case VK_MENU:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_LMENU:
            case VK_RMENU:
                return true;
            default:
                return false;
            }
        }

        // Which modifier is physically down, if any. One, not a set: a binding carries
        // one (mm::key_mod), and Ctrl wins over Shift wins over Alt so the answer is
        // deterministic when a player is leaning on two of them.
        int held_modifier()
        {
            if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                return mm::kKeyModCtrl;
            }
            if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
            {
                return mm::kKeyModShift;
            }
            if ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
            {
                return mm::kKeyModAlt;
            }
            return mm::kKeyModNone;
        }

        // THE KEYS SOMETHING ELSE ALREADY OWNS.
        //
        // Advisory, and not read from the game - there is no API for that. Two sources:
        //
        //   * the movement / interaction set this genre binds by default (WASD, Space,
        //     Shift, Ctrl, E, F, Q, R, Tab, Esc, 1..5) - the hotkey swallow makes the
        //     GAME action the casualty of a clash, so the player has to be told;
        //   * the keys other injected DLLs own here: F6 is RenoDX's DLSS 5 toggle (it
        //     ignores modifiers and has caused a GPU crash), F10 is the UE4SS console,
        //     F9 / F11 are engine binds and F12 is the Steam screenshot key. Those four
        //     are refused by the config parser outright.

        void arm_capture(int row)
        {
            g_capture_row = row;
            g_capture_wait_release = true;
            mm::g_key_capture.store(row >= 0, std::memory_order_relaxed);
        }

        void panel_bindings(mm::Config& cfg)
        {
            static const mm::Config kDefaults{};

            // ---- the capture, before anything is drawn --------------------------------
            //
            // A capture takes two shapes: `ctrl+m` (hold Ctrl, press M) and a bare
            // modifier (`LALT`, the x-ray's shipped default). So a non-modifier key wins
            // immediately and carries whatever modifier is held with it, while a
            // modifier pressed on its own is only taken once everything is released -
            // the only way to tell "reaching for Ctrl+M" from "I want Ctrl".
            if (g_capture_row >= 0 && g_capture_row < kKeyBindCount)
            {
                bool any_down = false;
                int pressed = 0;      // a real key: bind it now, with the held modifier
                int mod_only = 0;     // a modifier on its own: bind it on release
                for (const int vk : mm::bindable_vks())
                {
                    if ((::GetAsyncKeyState(vk) & 0x8000) == 0)
                    {
                        continue;
                    }
                    any_down = true;
                    if (is_modifier_vk(vk))
                    {
                        if (mod_only == 0)
                        {
                            mod_only = vk;
                        }
                    }
                    else if (pressed == 0)
                    {
                        pressed = vk;
                    }
                }
                static int pending_mod_only = 0;
                if ((::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
                {
                    pending_mod_only = 0;
                    arm_capture(-1);
                }
                else if (g_capture_wait_release)
                {
                    g_capture_wait_release = any_down;
                }
                else
                {
                    if (pressed == 0 && mod_only != 0)
                    {
                        pending_mod_only = mod_only;
                    }
                    const int take = pressed != 0 ? mm::key_make(pressed, held_modifier())
                                     : (!any_down && pending_mod_only != 0)
                                         ? mm::key_make(pending_mod_only, mm::kKeyModNone)
                                         : 0;
                    if (take != 0)
                    {
                        pending_mod_only = 0;
                        cfg.*kKeyBinds[g_capture_row].member = take;
                        mm::logf(L"binding: {} = {}",
                                 std::wstring(kKeyBinds[g_capture_row].key,
                                              kKeyBinds[g_capture_row].key +
                                                  std::strlen(kKeyBinds[g_capture_row].key)),
                                 mm::key_name(take));
                        arm_capture(-1);
                    }
                }
            }
            else if (g_capture_row >= 0)
            {
                arm_capture(-1);
            }

            ImGui::TextDisabled("Click a key to rebind it, then press the new key - hold Ctrl, Shift or "
                                "Alt with it for a modified binding. Esc cancels.");
            ImGui::TextDisabled("A key bound here is taken away from the game while the mod is using it.");

            if (ImGui::BeginTable("bindings", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Action");
                ImGui::TableSetupColumn("Key");
                ImGui::TableSetupColumn("");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (int i = 0; i < kKeyBindCount; ++i)
                {
                    const int vk = cfg.*kKeyBinds[i].member;
                    // Two actions on one key both fire: named rather than prevented.
                    const char* clash = nullptr;
                    for (int j = 0; j < kKeyBindCount && clash == nullptr; ++j)
                    {
                        if (j != i && vk != 0 && cfg.*kKeyBinds[j].member == vk)
                        {
                            clash = kKeyBinds[j].label;
                        }
                    }
                    // The unmodified twin: `ctrl+m` and `m` are different bindings but
                    // the same key press, because a no-modifier binding does not require
                    // the modifiers to be up (mm::key_mod) - which is what keeps every
                    // hotkey alive while the x-ray's Alt is held. Named, not prevented.
                    const char* twin = nullptr;
                    for (int j = 0; j < kKeyBindCount && twin == nullptr; ++j)
                    {
                        const int other = cfg.*kKeyBinds[j].member;
                        if (j != i && vk != 0 && mm::key_vk(other) == mm::key_vk(vk) &&
                            mm::key_mod(other) != mm::key_mod(vk))
                        {
                            twin = kKeyBinds[j].label;
                        }
                    }
                    const char* game = game_bind_clash(vk);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kKeyBinds[i].label);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", kKeyBinds[i].key);

                    ImGui::TableNextColumn();
                    ImGui::PushID(i + 900);
                    const std::string shown = g_capture_row == i
                                                  ? std::string("press a key...")
                                                  : key_name_ascii(vk);
                    if (ImGui::Button(shown.c_str(), ImVec2{130.0f * g_ui_scale, 0.0f}))
                    {
                        arm_capture(g_capture_row == i ? -1 : i);
                    }

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(vk == kDefaults.*kKeyBinds[i].member);
                    if (ImGui::SmallButton("reset"))
                    {
                        cfg.*kKeyBinds[i].member = kDefaults.*kKeyBinds[i].member;
                    }
                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    if (clash != nullptr)
                    {
                        ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "also %s", clash);
                    }
                    else if (game != nullptr)
                    {
                        ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "the game may use it for %s",
                                           game);
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("While the mod is using this key the game does not get it.\n"
                                              "Add Ctrl, Shift or Alt to give it back.");
                        }
                    }
                    else if (twin != nullptr)
                    {
                        ImGui::TextColored(ImVec4{0.80f, 0.80f, 0.55f, 1.0f}, "same key as %s", twin);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (ImGui::Button("Reset every binding"))
            {
                for (int i = 0; i < kKeyBindCount; ++i)
                {
                    cfg.*kKeyBinds[i].member = kDefaults.*kKeyBinds[i].member;
                }
                arm_capture(-1);
            }

            //--------------------------------------------------------------------------
            // The gamepad chord
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Gamepad");
            ImGui::Checkbox("X-ray on a gamepad chord", &cfg.highlight_gamepad);
            static char chord[64]{};
            static bool chord_primed = false;
            const std::string live = wide_to_ascii(
                mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt));
            if (!chord_primed)
            {
                ::strncpy_s(chord, sizeof(chord), live.c_str(), _TRUNCATE);
                chord_primed = true;
            }
            ImGui::SetNextItemWidth(180.0f * g_ui_scale);
            if (ImGui::InputText("highlight_pad_chord", chord, sizeof(chord),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                mm::set_pad_chord(chord, cfg.highlight_pad_mask, cfg.highlight_pad_lt,
                                  cfg.highlight_pad_rt);
                ::strncpy_s(chord, sizeof(chord),
                            wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                             cfg.highlight_pad_lt,
                                                             cfg.highlight_pad_rt))
                                .c_str(),
                            _TRUNCATE);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("in force: %s   (LB, RB, LT, RT, A, B, X, Y, BACK, START, LS, RS, "
                                "UP, DOWN, LEFT, RIGHT, joined with +; `none` disables it)",
                                live.c_str());
            if (ImGui::SmallButton("reset the chord"))
            {
                cfg.highlight_pad_mask = kDefaults.highlight_pad_mask;
                cfg.highlight_pad_lt = kDefaults.highlight_pad_lt;
                cfg.highlight_pad_rt = kDefaults.highlight_pad_rt;
                chord_primed = false;
            }
            ImGui::TextDisabled("the full map's own gamepad controls are fixed (left stick pans, "
                                "triggers zoom, LB / RB change floor)");
        }

        void panel_debug(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            panel_dev_keys(cfg);

            //--------------------------------------------------------------------------
            // The crash breadcrumb
            //--------------------------------------------------------------------------
            //
            // Where the overlay is now, and what the PREVIOUS session left in
            // wuchang_minimap_last_stage.txt. A non-terminal value there is the only
            // evidence surviving a death with UE4SS's log buffer unflushed, so it is
            // called out in colour.
            ImGui::SeparatorText("Stage");
            ImGui::Text("now: %s", crumb::current()[0] != '\0' ? crumb::current() : "(none)");
            ImGui::SameLine();
            ImGui::TextDisabled("(file %s)", cfg.crash_breadcrumb ? "on" : "off - crash_breadcrumb = 0");
            if (crumb::previous_suspicious())
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f},
                                   "last session ended at '%s' - it did NOT shut down cleanly",
                                   crumb::previous());
            }
            else if (crumb::previous()[0] != '\0')
            {
                ImGui::TextDisabled("last session ended at '%s'", crumb::previous());
            }
            else
            {
                ImGui::TextDisabled("no previous session recorded");
            }

            //--------------------------------------------------------------------------
            // The one-press recon dump
            //--------------------------------------------------------------------------
            //
            // The four things that cannot be recovered from the cooked assets
            // (context/saveslot-and-teleport-research.md section 3). It calls nothing
            // and changes nothing - reflection lookups and raw reads only - and writes
            // one file the user can send back.
            ImGui::SeparatorText("Recon dump");
            const recon::Status rc = recon::status();
            ImGui::BeginDisabled(rc.pending);
            if (ImGui::Button("Dump the fast-travel / save-slot recon"))
            {
                recon::request();
            }
            ImGui::EndDisabled();
            if (rc.pending)
            {
                ImGui::TextDisabled("gathering on the next game-thread pump...");
            }
            else if (rc.error[0] != '\0')
            {
                ImGui::TextColored(ImVec4{0.95f, 0.5f, 0.4f, 1.0f}, "%s", rc.error);
            }
            else if (rc.file[0] != '\0')
            {
                ImGui::TextWrapped("wrote %d line(s) to %s", rc.lines, rc.file);
            }

            //--------------------------------------------------------------------------
            // The runtime navmesh dump
            //--------------------------------------------------------------------------
            //
            // A button, not a binding: it scans engine memory and writes JSON, which no
            // player should trigger by leaning on a key. The module ships disabled
            // (config.ini [navmesh] navmesh_dump = 1) and the button says so.
            ImGui::SeparatorText("Runtime navmesh dump");
            ImGui::BeginDisabled(!navmesh::enabled());
            if (ImGui::Button("Dump the live navmesh tiles"))
            {
                navmesh::request_dump();
                post_toast("navmesh dump requested", 2500);
            }
            ImGui::EndDisabled();
            if (!navmesh::enabled())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("off - set navmesh_dump = 1 in config.ini and restart");
            }

            draw_perf_table();

            //--------------------------------------------------------------------------
            // Marker sweep
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Markers");
            const markers::Stats st = markers::stats();
            ImGui::Text("db %d marker(s) / %d chapter(s)   found file %d id(s)   published %d   live %d",
                        st.static_markers,
                        st.chapters_loaded,
                        st.found_ids,
                        st.published,
                        st.live_entries);
            // The absence rule (markers_absence_*). `levels loaded` at 0 means the rule
            // can never fire - nothing to match a marker's level against.
            ImGui::Text("absence marks %d   levels loaded %d   (rule %s, %d round(s), %s)",
                        st.absence_marks,
                        st.levels_loaded,
                        cfg.markers_absence_marks ? "on" : "off",
                        cfg.markers_absence_rounds,
                        mdb::format_category_mask(cfg.markers_absence_categories).c_str());
            // Two numbers, two questions.
            //   PUMP  - what one game-thread pump costs: the frame-hitch number, target
            //           well under 1 ms, `max` the worst single pump since load.
            //   ROUND - what a full pass over the object array cost and how many slots
            //           it visited: the freshness number, the marker set being
            //           `slices x period_ms` old at worst.
            // `!` marks the FindAllOf fallback, ~28 ms per pump, which runs only when
            // GUObjectArray reports no elements.
            ImGui::Text("scan pump %.3f ms (avg %.3f, peak %.3f, max %.3f)%s",
                        st.scan_slice_ms,
                        st.scan_slice_ms_avg,
                        st.scan_slice_ms_peak,
                        st.scan_slice_ms_max,
                        st.scan_fallback ? "   ! FindAllOf fallback" : "");
            ImGui::Text("round %.1f ms / %d pump(s) / %d object(s) of %d   chunk %d   %llu round(s)",
                        st.scan_round_ms,
                        st.scan_round_slices,
                        st.scan_round_objects,
                        st.scan_total,
                        st.scan_chunk,
                        static_cast<unsigned long long>(st.rounds));
            ImGui::Text("publish %.3f ms (avg %.3f, peak %.3f)",
                        st.publish_ms,
                        st.publish_ms_avg,
                        st.publish_ms_peak);
            ImGui::Text("drawn %d of %d (%d clamped, %d filtered, %d merged)",
                        g_marker_draw.drawn,
                        g_marker_draw.total,
                        g_marker_draw.clamped,
                        g_marker_draw.filtered,
                        g_marker_draw.merged);
            if (g_marker_draw.nearest[0] != 0)
            {
                ImGui::Text("nearest: %s (%.0f uu)", g_marker_draw.nearest, g_marker_draw.nearest_uu);
            }

            //--------------------------------------------------------------------------
            // Full map / gamepad
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Full map and gamepad");
            const pad::State gp = pad::state();
            char padmod[64]{};
            ::WideCharToMultiByte(CP_UTF8, 0, pad::module_name(), -1, padmod, sizeof(padmod) - 1, nullptr,
                                  nullptr);
            // Says whether anything is ASKING as well as what was found: with
            // map_gamepad off nothing polls, and "none" then means "not looked at".
            ImGui::Text("pad: %s (%s)   sticks %.2f,%.2f / %.2f,%.2f   triggers %.2f/%.2f",
                        gp.connected ? "connected"
                                     : (cfg.map_gamepad ||
                                        (cfg.highlight_enabled && cfg.highlight_gamepad))
                                           ? "none found (polling)"
                                           : "not polled (map_gamepad = 0)",
                        padmod,
                        static_cast<double>(gp.lx),
                        static_cast<double>(gp.ly),
                        static_cast<double>(gp.rx),
                        static_cast<double>(gp.ry),
                        static_cast<double>(gp.lt),
                        static_cast<double>(gp.rt));
            ImGui::Text("map slice %dx%d   %.2f ms (peak %.2f)   %llu cut(s), %llu skipped   "
                        "opaque %u / dim %u / faint %u",
                        g_mslice[0].w,
                        g_mslice[0].h,
                        g_mslice_ms,
                        g_mslice_ms_peak,
                        static_cast<unsigned long long>(g_mslice_updates),
                        static_cast<unsigned long long>(g_mslice_skipped),
                        g_mslice_counts.opaque,
                        g_mslice_counts.dim,
                        g_mslice_counts.faint);

            //--------------------------------------------------------------------------
            // The x-ray camera
            //--------------------------------------------------------------------------
            // The block to screenshot when the labels are in the wrong place: the
            // route, the pinned offset and the age of the pose.
            ImGui::SeparatorText("X-ray camera");
            const hl::Stats hs = hl::stats();
            const char* route = "none yet";
            switch (hs.route)
            {
            case hl::Route::RawPinned:
                route = "raw POV read (offset calibrated against the getters)";
                break;
            case hl::Route::RawSane:
                route = "raw POV read (offset accepted on sanity ranges only)";
                break;
            case hl::Route::Getters:
                route = "GetCameraLocation / GetCameraRotation / GetFOVAngle per read";
                break;
            case hl::Route::None:
            default:
                break;
            }
            ImGui::Text("camera: %s", route);
            ImGui::Text("manager %s   CameraCachePrivate +%d   POV +%d   %llu read(s), %llu rejected",
                        hs.have_manager ? "yes" : "NO",
                        hs.cache_offset,
                        hs.pov_offset,
                        static_cast<unsigned long long>(hs.reads),
                        static_cast<unsigned long long>(hs.fails));
            hl::Pose pose{};
            if (hl::camera(pose))
            {
                ImGui::Text("pose  X %.0f  Y %.0f  Z %.0f   pitch %.1f  yaw %.1f  roll %.1f   FOV %.1f   "
                            "%llu ms old",
                            pose.x,
                            pose.y,
                            pose.z,
                            pose.pitch,
                            pose.yaw,
                            pose.roll,
                            pose.fov,
                            static_cast<unsigned long long>(
                                pose.stamp_ms == 0 ? 0 : ::GetTickCount64() - pose.stamp_ms));
            }
            else
            {
                ImGui::TextDisabled("no camera pose published yet (hold the key in-world)");
            }
            ImGui::Text("held %s   %d of %d in range drawn (%d on screen, %d on the rim)",
                        g_hl_debug.active ? "YES" : "no",
                        g_hl_debug.drawn,
                        g_hl_debug.considered,
                        g_hl_debug.on_screen,
                        g_hl_debug.edge);
            ImGui::Text("compass: %s   heading %.1f deg from the %s   %d bearing pip(s)",
                        g_compass_debug.visible ? "visible" : "hidden (same gate as the minimap)",
                        g_compass_debug.heading,
                        g_compass_debug.from_camera ? "camera" : "pawn",
                        g_compass_debug.pips);

            //--------------------------------------------------------------------------
            // The game state
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Game state");
            if (!have_state)
            {
                ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.4f, 1.0f}, "no game-state snapshot yet");
            }
            else
            {
                const std::uint64_t age = ::GetTickCount64() - snap.stamp_ms;
                ImGui::Text("world  X %.1f  Y %.1f  Z %.1f   yaw %.1f deg", snap.x, snap.y, snap.z, snap.yaw);
                ImGui::Text("uv     %.5f, %.5f   chapter '%s'",
                            g_last_mini.u,
                            g_last_mini.v,
                            g_last_mini.chapter.empty() ? "-" : g_last_mini.chapter.c_str());
                ImGui::Text("gameplay pawn %s   transition %s   state-ok age %llu ms",
                            snap.pawn_is_gameplay ? "yes" : "NO",
                            snap.transition ? "YES" : "no",
                            static_cast<unsigned long long>(
                                snap.state_ok_since_ms == 0 ? 0 : ::GetTickCount64() - snap.state_ok_since_ms));
                // The height slicer: what it cut, how much it cost, where it is.
                ImGui::Text("slice  %dx%d px x %d surface(s) @ %.4f px/uu   %.2f ms (peak %.2f)   "
                            "%llu update(s), %llu skipped",
                            g_slice_size,
                            g_slice_size,
                            g_slice_surfaces,
                            g_slice_px_per_uu,
                            g_slice_ms,
                            g_slice_ms_peak,
                            static_cast<unsigned long long>(g_slice_updates),
                            static_cast<unsigned long long>(g_slice_skipped));
                ImGui::Text("       feet Z %.0f (raw %.0f)   tol %.0f  fade %.0f  gradient %.2f   "
                            "opaque %u / dim %u / faint %u",
                            static_cast<double>(g_feet_z),
                            snap.z - static_cast<double>(cfg.player_z_offset),
                            static_cast<double>(cfg.floor_z_tolerance),
                            static_cast<double>(cfg.floor_fade_uu),
                            static_cast<double>(cfg.floor_gradient_strength),
                            g_slice_opaque,
                            g_slice_dim,
                            g_slice_faint);
                ImGui::Text("pawn %s   pawn-view %s   menu %s   state age %llu ms",
                            snap.has_pawn ? "yes" : "no",
                            snap.is_pawn_view ? "yes" : "no",
                            snap.menu_open ? "OPEN" : "no",
                            static_cast<unsigned long long>(age));
                ImGui::Text("widgets seen %u, visible in viewport %u   location via %s",
                            snap.widgets_seen,
                            snap.widgets_visible_in_viewport,
                            snap.loc_from_function ? "K2_GetActorLocation" : "RootComponent");
                // Menu hide/show latency: how long ago the game thread saw the state
                // change, and how many roots it re-tests per pump.
                char holder[128]{};
                ::WideCharToMultiByte(CP_UTF8, 0, snap.menu_holder, -1, holder, sizeof(holder) - 1, nullptr,
                                      nullptr);
                ImGui::Text("menu state changed %llu ms ago   %u cached in-viewport root(s)   "
                            "show delay %d ms   holder '%s'",
                            static_cast<unsigned long long>(
                                snap.menu_change_ms == 0 ? 0 : ::GetTickCount64() - snap.menu_change_ms),
                            snap.menu_roots_cached,
                            cfg.menu_close_show_delay_ms,
                            holder[0] != '\0' ? holder : "-");
                ImGui::Text("teleport %s",
                            snap.teleport_ms == 0
                                ? "never this session"
                                : std::format("{} ms ago", ::GetTickCount64() - snap.teleport_ms).c_str());
                char narrow[256]{};
                ::WideCharToMultiByte(CP_UTF8, 0, snap.level_name, -1, narrow, sizeof(narrow) - 1, nullptr,
                                      nullptr);
                ImGui::TextWrapped("pawn: %s", narrow);
            }

            ImGui::Spacing();
            char reason[192]{};
            ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr, nullptr);
            // Recomputed from live state every frame - the show condition has no latch -
            // and every change to it is logged.
            if (g_last_mini.visible)
            {
                ImGui::TextColored(ImVec4{0.55f, 0.9f, 0.6f, 1.0f}, "minimap: %s", reason);
            }
            else
            {
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f}, "hidden because: %s", reason);
            }
            ImGui::TextDisabled("       this state has held for %llu ms",
                                static_cast<unsigned long long>(
                                    g_reason_since_ms == 0 ? 0 : ::GetTickCount64() - g_reason_since_ms));
            ImGui::Text("backbuffer %ux%u, %u buffer(s), composite %dx%d %s   ui scale %.2f",
                        g_width,
                        g_height,
                        g_buffer_count,
                        g_map.width,
                        g_map.height,
                        g_map.ready ? "ready" : "NOT ready",
                        static_cast<double>(g_ui_scale));
            ImGui::Text("presents %llu, resizes %llu",
                        static_cast<unsigned long long>(g_present_count.load()),
                        static_cast<unsigned long long>(g_resize_count.load()));
        }

        void draw_panel(mm::Config cfg, const mm::Snapshot& snap, bool have_state)
        {
            bool open = true;
            ImGui::SetNextWindowSize(ImVec2{520.0f * g_ui_scale, 620.0f * g_ui_scale},
                                     ImGuiCond_FirstUseEver);
            // Centred every time it opens: `Appearing`, not `FirstUseEver`, so a panel
            // dragged to a corner and closed comes back in the middle. The pivot is the
            // window's own centre, so its size does not change where it lands, and
            // dragging still works - the position is written only on the appearing frame.
            const ImGuiViewport* pvp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2{pvp->Pos.x + pvp->Size.x * 0.5f,
                                           pvp->Pos.y + pvp->Size.y * 0.5f},
                                    ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
            if (!ImGui::Begin("Wuchang Minimap  v" WUCHANG_MINIMAP_VERSION
                              "###wuchang_minimap_panel",
                              &open, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::End();
                if (!open)
                {
                    mm::g_panel_open = false;
                }
                return;
            }

            const mm::Config before = cfg;

            // The version and nothing else; version.hpp is the single source of truth.
            ImGui::TextColored(ImVec4{0.62f, 0.68f, 0.78f, 1.0f},
                               "WuchangMinimap v" WUCHANG_MINIMAP_VERSION);

            // The tabs get their own child so the Save / Revert / master-switch row is
            // always at the bottom of the window and never scrolls away with them.
            const float row_h = ImGui::GetFrameHeightWithSpacing() * 3.0f;
            if (ImGui::BeginChild("tabs", ImVec2{0.0f, -row_h}, ImGuiChildFlags_None))
            {
                if (ImGui::BeginTabBar("wuchang_tabs"))
                {
                    if (ImGui::BeginTabItem("Player"))
                    {
                        panel_player(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Advanced"))
                    {
                        panel_advanced(cfg);
                        ImGui::EndTabItem();
                    }
                    // The Debug tab exists only while debug_readout is on, a Dev key in
                    // a file players do not have, so a player sees two tabs.
                    if (ImGui::BeginTabItem("Bindings"))
                    {
                        panel_bindings(cfg);
                        ImGui::EndTabItem();
                    }
                    if (cfg.debug_readout && ImGui::BeginTabItem("Debug"))
                    {
                        panel_debug(cfg, snap, have_state);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            // The button names which file it writes, and both when both are written: a
            // Save with a dev file present, or with a Dev dial off its default, rewrites
            // config_wuchang_minimap_dev.txt as well.
            const bool dev_too = mm::dev_config_active();
            if (ImGui::Button(dev_too ? "Save to config_wuchang_minimap.txt + _dev.txt"
                                      : "Save to config_wuchang_minimap.txt"))
            {
                mm::g_save_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(dev_too ? "Write the current settings back to config_wuchang_minimap.txt "
                                            "and the developer dials to config_wuchang_minimap_dev.txt."
                                          : "Write the current settings back to the config file.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert"))
            {
                mm::g_revert_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Re-read the config files and discard unsaved changes.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload settings + maps"))
            {
                mm::g_reload_config = true;
            }

            // The master switch. Unticking it stops nothing from here: it writes
            // mod_enabled = 0 into the config file and the loop thread's 1 Hz watcher
            // acts on it (modswitch.hpp), so the whole shutdown runs on the one thread
            // allowed to run it and the file cannot disagree with the running state.
            if (ImGui::Checkbox("Mod enabled (master switch - turns EVERYTHING off)", &cfg.mod_enabled))
            {
                if (!cfg.mod_enabled)
                {
                    mm::set_config(cfg);
                    mm::g_save_config = true;
                    mm::log(L"master switch: turned off from the F2 panel - writing mod_enabled = 0; the "
                            L"mod stops within a second. Edit the config file to turn it back on.");
                }
            }
            if (!cfg.mod_enabled)
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f},
                                   "The mod is shutting down. Set mod_enabled = 1 in %s to restart it.",
                                   "config_wuchang_minimap.txt");
            }
            // The other off switch: the same shutdown with no file written, for ruling
            // the mod out of a problem without a config to repair afterwards.
            ImGui::SameLine();
            if (ImGui::Button("Disable for this session"))
            {
                modswitch::request_session_disable();
                mm::log(L"master switch: disable for this session requested from the F2 panel - "
                        L"nothing is written to the config file; save or edit it to turn the mod "
                        L"back on (checked once a second).");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Stop the mod until the config file is saved or edited again.");
            }

            ImGui::End();

            // Field by field, not memcmp: a Config is a value, and `mm::operator==` is
            // generated from the struct with a byte-flip drift guard in markers_test.
            if (before != cfg)
            {
                mm::set_config(cfg);
            }
            if (!open)
            {
                mm::g_panel_open = false;
            }
        }
    } // namespace ovl
} // namespace overlay
