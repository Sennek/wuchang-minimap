//
// overlay_panel - the F2 settings panel.
//
// The Overview, Categories, Map & tracker, Keys and Debug tabs, the category grid, the
// performance table and the panel's own state file.
//
// Nothing here is saved by hand: a change is published to the live config on the frame
// it is made, and the loop thread writes both config files a moment later.
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
            // The caller's width or what is actually left here, whichever is narrower,
            // so a panel the player shrinks re-wraps the grid instead of clipping it.
            const float wrap = (std::min)(wrap_width, ImGui::GetContentRegionAvail().x);
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
                    if (x + w < wrap)
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

        // One category filter: a title, `all` / `none`, then the chip grid. The title
        // says what the filter is FOR.
        void category_filter(const char* title, std::uint32_t& mask, int base_id, float wrap_width)
        {
            ImGui::PushID(base_id);
            ImGui::TextUnformatted(title);
            // Each piece keeps the line only while it fits, so a narrowed panel wraps
            // the header instead of pushing the key name off the right edge.
            (void)same_line_if_fits(button_width("all"));
            if (ImGui::SmallButton("all"))
            {
                mask = mdb::kAllCats;
            }
            (void)same_line_if_fits(button_width("none"));
            if (ImGui::SmallButton("none"))
            {
                mask = 0u;
            }
            ImGui::PopID();
            category_chips(mask, base_id + 1, wrap_width);
        }

        //==============================================================================
        // The F2 panel: Overview / Categories / Map & tracker / Keys
        //==============================================================================
        //
        // One home per concept: a surface's on/off, its size and its categories are
        // never on different tabs. The Debug tab is the Dev tier plus every read-only
        // diagnostic, and exists only while `debug_readout` is on.
        //
        // Each section is its own function for a reason beyond tidiness: a
        // CollapsingHeader must be able to SKIP its contents, and MSVC counts nested
        // blocks and C1061s this file otherwise.

        // Every section's fold state is one bit of `g_panel_sections`. The bits are
        // positional, so this list IS the format of wuchang_minimap_panel.txt.
        enum PanelSectionBit
        {
            kSecWhatIsOn = 0,
            kSecPlacement,
            kSecLook,
            kSecFullMap,
            kSecWaypoints,
            kSecTracker,
            kSecGamepad,
            kSecTuneMinimap,
            kSecTuneFullMap,
            kSecTuneXray,
            kSecTuneCompass,
            kSecTuneFloors,
            kSecTuneSweep,
            kSecTuneGate,
            kSecTuneDiag,
            kSecTuneBackground,
            kSecCount,
        };
        static_assert(kSecCount <= 32, "one bit per section in g_panel_sections");

        // A CollapsingHeader that remembers whether it is open. The render thread owns
        // the bits and raises a flag; the loop thread writes the file.
        bool panel_section(const char* title, int bit)
        {
            const std::uint32_t mask = 1u << bit;
            const std::uint32_t bits = g_panel_sections.load(std::memory_order_relaxed);
            ImGui::SetNextItemOpen((bits & mask) != 0, ImGuiCond_Always);
            const bool open = ImGui::CollapsingHeader(title);
            const std::uint32_t now = open ? (bits | mask) : (bits & ~mask);
            if (now != bits)
            {
                g_panel_sections.store(now, std::memory_order_relaxed);
                g_panel_state_dirty.store(true, std::memory_order_release);
            }
            return open;
        }

        //--------------------------------------------------------------------------
        // Overview: what is on
        //--------------------------------------------------------------------------
        //
        // The three surfaces, each with the handful of dials that decide how it looks.
        // Everything else about them is on Categories, or on the Debug tab.
        void overview_what_is_on(mm::Config& cfg)
        {
            ImGui::Checkbox("Minimap", &cfg.show_minimap);
            ImGui::Indent();
            // Every range here is the loader's clamp, so the panel never shows a value
            // the file will not keep.
            ImGui::SliderFloat("Size", &cfg.size_frac, 0.05f, 0.9f, "%.2f");
            ImGui::SliderFloat("Zoom", &cfg.zoom_uu_per_px, 2.0f, 400.0f, "%.0f");
            ImGui::SliderFloat("Opacity", &cfg.opacity, 0.1f, 1.0f, "%.2f");
            bool round_shape = cfg.round;
            if (ImGui::Checkbox("Round", &round_shape))
            {
                cfg.round = round_shape;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Rotate with player", &cfg.rotate_with_player);
            ImGui::Checkbox("Show the floors above and below, dimmed", &cfg.show_adjacent_floors);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::Checkbox("Compass", &cfg.compass_enabled);
            ImGui::Indent();
            ImGui::SliderFloat("Width", &cfg.compass_width, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat("Opacity##compass", &cfg.compass_opacity, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat("Field of view", &cfg.compass_span_deg, 30.0f, 360.0f, "%.0f deg");
            ImGui::SliderFloat("Height", &cfg.compass_height, 10.0f, 120.0f, "%.0f px");
            ImGui::Checkbox("Distance under each mark", &cfg.compass_pip_labels);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::Checkbox("X-ray", &cfg.highlight_enabled);
            ImGui::Indent();
            // The combo carries the key beside it, which is what the two "hold means..."
            // paragraphs used to say.
            std::string hold = key_name_ascii(cfg.highlight_key);
            if (cfg.highlight_gamepad)
            {
                hold += " or pad " + wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                                     cfg.highlight_pad_lt,
                                                                     cfg.highlight_pad_rt));
            }
            int hl_mode = cfg.highlight_mode == mm::HighlightMode::Hold ? 1 : 0;
            ImGui::SetNextItemWidth(110.0f * g_chrome_scale);
            if (ImGui::Combo("##hl_mode", &hl_mode, "Toggle\0Hold\0"))
            {
                cfg.highlight_mode = hl_mode == 1 ? mm::HighlightMode::Hold : mm::HighlightMode::Toggle;
            }
            ImGui::SameLine();
            ImGui::Text("%s in-world", hold.c_str());
            // Metres on the slider: the key is world units and 1 uu = 1 cm, so the panel
            // converts rather than teach the player a second unit.
            float radius_m = cfg.highlight_radius / 100.0f;
            if (ImGui::SliderFloat("Radius", &radius_m, 2.0f, 500.0f, "%.0f m"))
            {
                cfg.highlight_radius = radius_m * 100.0f;
            }
            ImGui::Checkbox("Names + distance", &cfg.highlight_labels);
            ImGui::SameLine();
            ImGui::Checkbox("Arrows to what is off screen", &cfg.highlight_edge_arrows);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::Checkbox("Show markers", &cfg.markers_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Hide while a menu is open", &cfg.hide_in_menus);
        }

        //--------------------------------------------------------------------------
        // Overview: placement and scale
        //--------------------------------------------------------------------------
        void overview_placement(mm::Config& cfg)
        {
            // One key that moves the whole HUD. A non-custom preset owns both anchors,
            // so they are REVEALED under `custom` rather than greyed out; the offsets
            // stay, because a preset moves the corner and not the gap from it.
            int preset = static_cast<int>(cfg.hud_preset);
            const char* presets[] = {"custom", "top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("HUD placement", &preset, presets, 5))
            {
                cfg.hud_preset = static_cast<mm::HudPreset>(preset);
            }
            if (cfg.hud_preset == mm::HudPreset::Custom)
            {
                ImGui::Indent();
                int anchor = static_cast<int>(cfg.anchor);
                const char* anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
                if (ImGui::Combo("Minimap corner", &anchor, anchors, 4))
                {
                    cfg.anchor = static_cast<mm::Anchor>(anchor);
                }
                int canchor = cfg.compass_anchor == mm::VAnchor::Bottom ? 1 : 0;
                const char* canchors[] = {"top", "bottom"};
                if (ImGui::Combo("Compass edge", &canchor, canchors, 2))
                {
                    cfg.compass_anchor = canchor == 1 ? mm::VAnchor::Bottom : mm::VAnchor::Top;
                }
                ImGui::Unindent();
            }
            // One control, because the two numbers are one gap from one corner.
            float offset[2] = {cfg.offset_x, cfg.offset_y};
            if (ImGui::DragFloat2("Minimap offset", offset, 1.0f, 0.0f, 4000.0f, "%.0f px"))
            {
                cfg.offset_x = offset[0];
                cfg.offset_y = offset[1];
            }
            ImGui::SliderFloat("Compass offset", &cfg.compass_offset_y, 0.0f, 2000.0f, "%.0f px");

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
            // The text size the UI scale multiplies. The whole overlay re-rasterises on
            // the next frame, so the slider shows its own result while it is dragged.
            ImGui::SliderInt("Text size", &cfg.font_size, kFontPxMin, kFontPxMax, "%d px");
        }

        //--------------------------------------------------------------------------
        // Overview: look
        //--------------------------------------------------------------------------
        //
        // In the FILE a theme recolours every colour key the player has not customised; in the
        // PANEL choosing one is explicit, so it writes the theme's colours into the five
        // colour keys there and then - otherwise the combo looks broken for a player
        // whose config spells one of those keys out.
        void overview_look(mm::Config& cfg)
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
            // The whole overlay's typeface. Empty or unreadable falls back to the
            // built-in font, which the log says.
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("Font", "path to a .ttf, or `none` for the built-in font",
                                     cfg.ui_font, sizeof(cfg.ui_font));
        }

        //==============================================================================
        // The Categories tab
        //==============================================================================
        //
        // One grid instead of four chip rows: the fourteen categories down the side,
        // the three surfaces across the top, one mask per column. A row label is the
        // legend - the category's own glyph and colour, and its live found / known
        // count - and clicking it turns the whole row on or off.

        // A column header: the surface's name over `all` / `none` for its mask.
        void mask_column_header(const char* name, std::uint32_t& mask)
        {
            ImGui::PushID(name);
            ImGui::TextUnformatted(name);
            if (ImGui::SmallButton("all"))
            {
                mask = mdb::kAllCats;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("none"))
            {
                mask = 0u;
            }
            ImGui::PopID();
        }

        void category_grid(mm::Config& cfg)
        {
            // The counts the full map's legend shows: for the chapter in force when the
            // chapter filter is on, so the total is not five chapters the player cannot
            // see.
            const markers::Stats st = markers::stats();
            const int fch = st.filter_chapter;
            const bool per_chapter = fch >= 0 && fch <= 8;
            std::uint32_t* const masks[3] = {&cfg.markers_categories, &cfg.highlight_categories,
                                             &cfg.compass_categories};
            if (!ImGui::BeginTable("categories", 4,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp))
            {
                return;
            }
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Minimap & map");
            ImGui::TableSetupColumn("X-ray");
            ImGui::TableSetupColumn("Compass");
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Category");
            ImGui::TextDisabled(per_chapter ? "found / known, this chapter" : "found / known");
            ImGui::TableNextColumn();
            mask_column_header("Minimap & map", cfg.markers_categories);
            ImGui::TableNextColumn();
            mask_column_header("X-ray", cfg.highlight_categories);
            ImGui::TableNextColumn();
            mask_column_header("Compass", cfg.compass_categories);

            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.34f);
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(i);
                const std::uint32_t bit = mdb::cat_bit(cat);
                const markers::CatStat& cs = per_chapter ? st.chapter[fch][i] : st.cat[i];
                int on_count = 0;
                for (std::uint32_t* m : masks)
                {
                    on_count += (*m & bit) != 0 ? 1 : 0;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                // The leading spaces are the glyph's gutter: the glyph is drawn over the
                // row afterwards, so the Selectable owns the whole cell.
                char label[64]{};
                if (cs.total > 0)
                {
                    (void)std::snprintf(label, sizeof(label), "      %s   %d/%d", mdb::cat_label(cat),
                                        cs.found, cs.total);
                }
                else
                {
                    (void)std::snprintf(label, sizeof(label), "      %s", mdb::cat_label(cat));
                }
                const ImVec2 row = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      on_count > 0 ? marker_color(cat, 255) : IM_COL32(150, 150, 150, 170));
                if (ImGui::Selectable(label, on_count == 3))
                {
                    for (std::uint32_t* m : masks)
                    {
                        *m = on_count == 3 ? (*m & ~bit) : (*m | bit);
                    }
                }
                ImGui::PopStyleColor();
                draw_marker_glyph(dl, cat,
                                  ImVec2{row.x + glyph_r + 4.0f, row.y + ImGui::GetTextLineHeight() * 0.5f},
                                  glyph_r, marker_color(cat, on_count > 0 ? 255 : 90),
                                  IM_COL32(14, 16, 20, on_count > 0 ? 220 : 80));
                for (int c = 0; c < 3; ++c)
                {
                    ImGui::TableNextColumn();
                    ImGui::PushID(c);
                    bool on = mdb::cat_enabled(*masks[c], cat);
                    if (ImGui::Checkbox("##on", &on))
                    {
                        *masks[c] = on ? (*masks[c] | bit) : (*masks[c] & ~bit);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // The grid, then the marker rules that are about every surface at once.
        void panel_categories(mm::Config& cfg)
        {
            category_grid(cfg);
            ImGui::Spacing();
            ImGui::Separator();

            const float dial_w = 72.0f * g_chrome_scale;
            // One question, every surface that draws a marker. The key is phrased as
            // "hide", the question a player asks is "show".
            ImGui::TextUnformatted("Show found");
            ImGui::SameLine();
            bool show_found = !cfg.markers_hide_found;
            if (ImGui::Checkbox("map / minimap / compass", &show_found))
            {
                cfg.markers_hide_found = !show_found;
            }
            ImGui::SameLine();
            ImGui::Checkbox("x-ray", &cfg.highlight_show_found);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat("how faint", &cfg.markers_found_alpha, 0.0f, 1.0f, "%.2f");
            text_disabled_wrapped("found ones are drawn hollow; shrines always stay, lit ones solid");

            ImGui::TextUnformatted("Glyph size");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat("minimap##glyph", &cfg.markers_size, 2.0f, 24.0f, "%.1f px");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat("map##glyph", &cfg.map_marker_size, 2.0f, 32.0f, "%.1f px");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat("x-ray##glyph", &cfg.highlight_size, 2.0f, 32.0f, "%.1f px");

            // Two booleans that only make sense together; both keys are still written.
            int quality = cfg.markers_rarity_tint ? 2 : (cfg.xray_rarity_colors_enabled ? 1 : 0);
            ImGui::SetNextItemWidth(160.0f * g_chrome_scale);
            if (ImGui::Combo("Item quality colours", &quality, "off\0x-ray only\0everywhere\0"))
            {
                cfg.xray_rarity_colors_enabled = quality >= 1;
                cfg.markers_rarity_tint = quality == 2;
            }
            text_disabled_wrapped("pickups take the colour of the game's own item-type grouping");
            // The tier colours themselves, beside the switch that turns them on.
            ImGui::BeginDisabled(quality == 0);
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
            ImGui::EndDisabled();

            ImGui::Checkbox("Keep off-map markers on the rim", &cfg.markers_clamp_to_edge);
        }

        //==============================================================================
        // The Map & tracker tab
        //==============================================================================

        // What the map draws where the game's navigation data says a player cannot get.
        // Disabled on a map asset built before the flood existed - there is nothing
        // marked, so every surface counts as reachable whatever this says.
        void map_background(mm::Config& cfg)
        {
            const bool available = mapdata::reachability_available();
            if (!available)
            {
                ImGui::BeginDisabled();
            }
            ImGui::TextUnformatted("Ground you cannot reach");
            const struct
            {
                srule::Unreachable value;
                const char* label;
                const char* help;
            } kChoices[] = {
                {srule::Unreachable::Hide, "Hide", "draw only ground the game says you can walk to"},
                {srule::Unreachable::Dim, "Dim", "draw it one step fainter than ground you can reach"},
                {srule::Unreachable::Show, "Show", "draw it like any other ground"},
            };
            for (int i = 0; i < 3; ++i)
            {
                if (i != 0)
                {
                    ImGui::SameLine();
                }
                const bool on = cfg.map_unreachable == kChoices[i].value;
                if (ImGui::RadioButton(kChoices[i].label, on))
                {
                    cfg.map_unreachable = kChoices[i].value;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", kChoices[i].help);
                }
            }
            if (!available)
            {
                ImGui::EndDisabled();
                ImGui::TextDisabled("maps without reachability data");
            }
            else
            {
                ImGui::TextDisabled("wall tops, roof ridges and the ledges outside an arena");
            }
        }

        void map_fullmap(mm::Config& cfg)
        {
            ImGui::TextDisabled("Press %s in-world. It opens where you left it.",
                                key_name_ascii(cfg.map_key).c_str());
            ImGui::Checkbox("Show every floor", &cfg.map_show_all_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad", &cfg.map_gamepad);
        }

        // What a waypoint is ON, in words: the published marker nearest to it, when one
        // is close enough to be what the player clicked. The published buffer runs to
        // thousands of entries, so the whole table is recomputed twice a second and the
        // rows read it - the first row of a due frame pays for all of them.
        const char* waypoint_place(const mv::WaypointSet& wps, std::size_t index, std::uint64_t now)
        {
            constexpr double kNearUu = 400.0;
            static char names[mv::kMaxWaypoints][48]{};
            static std::uint64_t due = 0;
            if (now >= due)
            {
                due = now + 500;
                const markers::View published = markers::view();
                for (std::size_t w = 0; w < mv::kMaxWaypoints; ++w)
                {
                    const char* name = "a place on the map";
                    if (w < wps.count)
                    {
                        double best_d2 = kNearUu * kNearUu;
                        for (std::size_t i = 0; i < published.count; ++i)
                        {
                            const markers::DrawMarker& m = published.data[i];
                            const double dx = m.x - wps.items[w].x;
                            const double dy = m.y - wps.items[w].y;
                            const double dz = m.z - wps.items[w].z;
                            const double d2 = dx * dx + dy * dy + dz * dz;
                            if (d2 < best_d2)
                            {
                                best_d2 = d2;
                                name = mdb::display_label(static_cast<mdb::Cat>(m.cat), m.label);
                            }
                        }
                    }
                    ::strncpy_s(names[w], sizeof(names[w]), name, _TRUNCATE);
                }
            }
            return names[index];
        }

        void map_waypoints(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            const mv::WaypointSet wps = mm::waypoints();
            if (wps.count == 0)
            {
                if (mm::key_vk(cfg.waypoint_nearest_key) != 0)
                {
                    char none[256]{};
                    (void)std::snprintf(none, sizeof(none),
                                        "No waypoints. Right-click a marker or the ground on the full "
                                        "map, or press %s in-world for the nearest thing you have not "
                                        "collected.",
                                        key_name_ascii(cfg.waypoint_nearest_key).c_str());
                    text_disabled_wrapped(none);
                }
                else
                {
                    text_disabled_wrapped("No waypoints. Right-click a marker or the ground on the "
                                          "full map, or bind a key on the Keys tab for the nearest "
                                          "thing you have not collected.");
                }
                return;
            }
            ImGui::Text("%zu of %zu", wps.count, mv::kMaxWaypoints);
            (void)same_line_if_fits(button_width("Clear waypoints"));
            if (ImGui::SmallButton("Clear waypoints"))
            {
                mm::clear_waypoints();
            }
            const std::uint64_t now = ::GetTickCount64();
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
                const char* place = waypoint_place(wps, wi, now);
                if (have_state)
                {
                    const double dx = wps.items[wi].x - snap.x;
                    const double dy = wps.items[wi].y - snap.y;
                    ImGui::Text("%zu.  %s   %.0f m away", wi + 1, place,
                                std::sqrt(dx * dx + dy * dy) / 100.0);
                }
                else
                {
                    ImGui::Text("%zu.  %s", wi + 1, place);
                }
                ImGui::PopID();
            }
        }

        // The save-slot key inside a found filename, or "shared" for the file every save
        // shares. Cheap enough to do per frame: one small string.
        std::string slot_key_of(const char* found_file)
        {
            static constexpr char kPrefix[] = "wuchang_minimap_found_";
            constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
            constexpr std::size_t kSuffixLen = 4; // ".txt"
            const std::string name = found_file != nullptr ? found_file : "";
            if (name.empty())
            {
                return "unknown";
            }
            if (name.size() <= kPrefixLen + kSuffixLen || name.compare(0, kPrefixLen, kPrefix) != 0)
            {
                return "shared";
            }
            return name.substr(kPrefixLen, name.size() - kPrefixLen - kSuffixLen);
        }

        void map_tracker()
        {
            const markers::Stats st = markers::stats();
            //---- wiping this save's list ----------------------------------------------
            //
            // A stray click must not cost a playthrough, so the button arms on the first
            // click and only acts on a second one within kClearArmMs; any other click in
            // the panel disarms it. The loop thread owns the set and the file, so this
            // only raises a flag.
            constexpr std::uint64_t kClearArmMs = 3000;
            static std::uint64_t clear_armed_ms = 0;
            const std::uint64_t now_ms = ::GetTickCount64();
            const bool armed = clear_armed_ms != 0 && now_ms - clear_armed_ms < kClearArmMs;
            if (!armed)
            {
                clear_armed_ms = 0;
            }
            const bool clear_pressed =
                ImGui::Button(armed ? "Click again to confirm" : "Clear this save's found list");
            const bool clear_hovered = ImGui::IsItemHovered();
            if (clear_pressed)
            {
                if (armed)
                {
                    markers::request_clear_found();
                    // The map's optimistic per-marker overrides would keep drawing the old
                    // state until the published buffer disagrees with each of them.
                    g_found_override.clear();
                    clear_armed_ms = 0;
                }
                else
                {
                    clear_armed_ms = now_ms;
                }
            }
            else if (armed && !clear_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                clear_armed_ms = 0; // a click somewhere else means "no"
            }
            if (clear_hovered)
            {
                ImGui::SetTooltip("empties %s and forgets every marker you have collected in it\n"
                                  "markers the game has already removed come back as found\n"
                                  "within a few passes, and so do lit shrines and beaten bosses\n"
                                  "NG+ keeps the save id, so this is how you start the list over",
                                  st.found_file[0] != '\0' ? st.found_file : "the found file");
            }
            ImGui::SameLine();
            // The save it would wipe, spelled out beside the button: the filename above is
            // easy to read past, and this is the one thing worth being sure of.
            ImGui::TextDisabled("save: %s", slot_key_of(st.found_file).c_str());

            //---- import / export ------------------------------------------------------
            //
            // The found list and the waypoints of this profile as one JSON file in the
            // mod folder. Both buttons only raise a flag: the loop thread owns every
            // read and write (overlay.cpp).
            ImGui::SeparatorText("Backup");
            static char import_path[512]{};
            // The newest export the loop thread found, offered once - on the first frame
            // it exists, and again after an export writes a newer one. Never on every
            // empty frame: a box the player has cleared has to stay clear.
            static bool import_path_seeded = false;
            if (ImGui::Button("Export"))
            {
                g_export_request.store(true, std::memory_order_release);
                import_path_seeded = false; // offer the file it is about to write
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("writes wuchang_minimap_export_<date>_<time>.json next to the DLL\n"
                                  "(a counter is added when that name is taken)");
            }
            if (!import_path_seeded && import_path[0] == '\0' &&
                g_latest_export_ready.load(std::memory_order_acquire))
            {
                spin::SpinGuard guard(g_exchange_lock);
                ::strncpy_s(import_path, sizeof(import_path), g_latest_export, _TRUNCATE);
                import_path_seeded = import_path[0] != '\0';
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
                ImGui::SetTooltip("merges the file's found ids and adds the waypoints you do not\n"
                                  "already have; nothing is ever removed by an import");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##import_path", "path to a .json export", import_path,
                                     sizeof(import_path));

            // The collection-statistics page, shared with the full map's Stats panel.
            draw_collection_stats(::GetTickCount64(), false);
        }
        //==============================================================================
        // THE PANEL'S OWN STATE FILE
        //==============================================================================
        //
        // Which sections are folded up, remembered between sessions: one
        // line, one number, in wuchang_minimap_panel.txt beside the config.
        //
        // Not imgui.ini - io.IniFilename is nullptr and stays that way, or the panel's
        // "come back centred" behaviour stops working. Not a config key either: it is
        // not a setting, and it must not appear in a config file or in the drift test
        // that guards it.
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
            const char* p = ::strstr(buf, "sections3");
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
                MM_LOGV(L"panel state: sections3 0x{:X}", v);
            }
        }

        // Loop thread, and only when the render thread says something changed.
        void panel_state_save()
        {
            char text[256]{};
            const int n = std::snprintf(text, sizeof(text),
                                        "; WuchangMinimap - where you left the F2 panel. Not a setting:\r\n"
                                        "; delete this file to get every section back open.\r\n"
                                        "sections3 = 0x%X\r\n",
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
        // THE TABS
        //==============================================================================

        void panel_overview(mm::Config& cfg)
        {
            if (panel_section("What is on", kSecWhatIsOn))
            {
                overview_what_is_on(cfg);
            }
            if (panel_section("Placement", kSecPlacement))
            {
                overview_placement(cfg);
            }
            if (panel_section("Look", kSecLook))
            {
                overview_look(cfg);
            }
        }

        void panel_map_tracker(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            // The tracker leads: it is the section with the per-save buttons, and further
            // down the tab they sit below the fold.
            if (panel_section("Collection tracker", kSecTracker))
            {
                map_tracker();
            }
            if (panel_section("Full map", kSecFullMap))
            {
                map_fullmap(cfg);
            }
            if (panel_section("Waypoints", kSecWaypoints))
            {
                map_waypoints(cfg, snap, have_state);
            }
        }

        //==============================================================================
        // The Debug tab's Tuning section
        //==============================================================================
        //
        // The Advanced tier, grouped by the surface each dial tunes. One function per
        // section. The minimap's own frame and backdrop colours are the theme's storage
        // and are edited in the config file, not here.

        void tune_minimap(mm::Config& cfg)
        {
            // The zoom ladder is a list and is edited in the file; the panel shows what
            // is in force and which key steps through it.
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
            ImGui::SliderFloat("Player arrow (fraction of the side)", &cfg.minimap_arrow_frac, 0.01f, 0.3f,
                               "%.3f");
            ImGui::SliderFloat("Player arrow minimum (px)", &cfg.minimap_arrow_min_px, 2.0f, 64.0f, "%.0f");
            ImGui::SliderFloat("Waypoint size (x marker size)", &cfg.waypoint_size_scale, 0.2f, 4.0f, "%.2f");
        }

        void tune_fullmap(mm::Config& cfg)
        {
            ImGui::SliderFloat("Zoom limit - closest", &cfg.map_zoom_min, 1.0f, 2000.0f, "%.0f");
            ImGui::SliderFloat("Zoom limit - furthest", &cfg.map_zoom_max, cfg.map_zoom_min, 4000.0f,
                               "%.0f");
            ImGui::SliderFloat("Zoom per wheel notch", &cfg.map_zoom_factor, 1.01f, 2.0f, "%.2f");
            ImGui::SliderFloat("Pan speed (screen px per second)", &cfg.map_pan_speed, 50.0f, 6000.0f,
                               "%.0f");
            ImGui::SliderFloat("Margin (fraction of screen height)", &cfg.map_margin, 0.0f, 0.3f, "%.3f");
            ImGui::SliderFloat("Backdrop opacity##map", &cfg.map_backdrop, 0.0f, 1.0f, "%.2f");
            ImGui::SliderInt("Max markers drawn##map", &cfg.map_markers_max_draw, 0, 20000);
            ImGui::SliderFloat("Floor step (uu)", &cfg.map_floor_step, 10.0f, 5000.0f, "%.0f");
            ImGui::SliderInt("Slice texture width (px)", &cfg.map_slice_px, 128, 2048);
            ImGui::SliderInt("Slice rate cap (Hz)", &cfg.map_slice_hz, 1, 30);
            ImGui::SliderFloat("Gamepad deadzone", &cfg.map_gamepad_deadzone, 0.05f, 0.6f, "%.2f");
        }

        void tune_xray(mm::Config& cfg)
        {
            ImGui::SliderInt("Max drawn (nearest first)", &cfg.highlight_max_draw, 1, 400);
            ImGui::SliderInt("Max labelled (nearest first)", &cfg.highlight_labels_max, 0, 40);
            ImGui::SameLine();
            ImGui::TextDisabled("names only");
            ImGui::SliderFloat("Alpha at the camera", &cfg.highlight_alpha_near, 0.05f, 1.0f, "%.2f");
            // The far alpha never rises above the near one - the loader clamps it there,
            // so the slider stops there too rather than showing a value the file drops.
            ImGui::SliderFloat("Alpha at the radius", &cfg.highlight_alpha_far, 0.0f,
                               cfg.highlight_alpha_near, "%.2f");
            cfg.highlight_alpha_far = (std::min)(cfg.highlight_alpha_far, cfg.highlight_alpha_near);
            ImGui::SliderInt("Camera read rate (Hz)", &cfg.highlight_camera_hz, 5, 240);
        }

        void tune_compass(mm::Config& cfg)
        {
            ImGui::Checkbox("Filled plate behind the strip", &cfg.compass_plate);
            ImGui::SameLine();
            ImGui::TextDisabled("off = ticks and letters with a shadow");
            ImGui::SliderFloat("Marker bearing range (uu)", &cfg.compass_marker_distance, 500.0f, 200000.0f,
                               "%.0f");
            ImGui::SliderFloat("Minor tick spacing (deg)", &cfg.compass_tick_step_deg, 1.0f, 90.0f, "%.0f");
            ImGui::SliderInt("Max bearing pips", &cfg.compass_max_pips, 0, 256);
            ImGui::Checkbox("Show the waypoint bearing", &cfg.compass_show_waypoint);
        }

        void tune_floors(mm::Config& cfg)
        {
            ImGui::SliderFloat("Floor Z tolerance (uu)", &cfg.floor_z_tolerance, 10.0f, 2000.0f, "%.0f");
            ImGui::SliderFloat("Adjacent floor opacity", &cfg.adjacent_floor_opacity, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Fade range below (uu)", &cfg.floor_fade_uu, cfg.floor_z_tolerance,
                               20000.0f, "%.0f");
            ImGui::SliderFloat("Fade range above (uu)", &cfg.floor_fade_above_uu, 0.0f, 20000.0f, "%.0f");
            ImGui::SameLine();
            ImGui::TextDisabled("0 = never draw a floor above you");
            ImGui::SliderFloat("Height gradient strength", &cfg.floor_gradient_strength, 0.0f, 1.0f, "%.2f");
            float base[3] = {cfg.floor_base_r / 255.0f, cfg.floor_base_g / 255.0f, cfg.floor_base_b / 255.0f};
            if (ImGui::ColorEdit3("Walkable fill colour", base, ImGuiColorEditFlags_NoInputs))
            {
                cfg.floor_base_r = base[0] * 255.0f;
                cfg.floor_base_g = base[1] * 255.0f;
                cfg.floor_base_b = base[2] * 255.0f;
            }
            ImGui::SliderInt("Slice rate (Hz)", &cfg.slice_hz, 2, 30);
            ImGui::SliderInt("Feet Z smoothing (ms)", &cfg.feet_z_smooth_ms, 1, 2000);
            ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -500.0f, 500.0f,
                               "%.0f");
        }

        void tune_sweep(mm::Config& cfg, float wrap)
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
            // The two knobs that trade game-thread time for marker freshness. The "scan
            // pump" line on the Debug tab is the read-out that says which way to move
            // them.
            ImGui::SliderInt("Full passes per second", &cfg.markers_rounds_per_sec, 1, 10);
            ImGui::SliderInt("Object slots per pump", &cfg.markers_scan_chunk, scan::kChunkMin,
                             scan::kChunkMax);
            ImGui::SliderInt("Min ms between pumps", &cfg.markers_scan_period_ms, scan::kPeriodMinMs,
                             scan::kPeriodMaxMs);
            ImGui::SliderInt("Max markers drawn per frame", &cfg.markers_max_draw, 0, 4000);
            // One threshold, two surfaces: the minimap glyph and the compass pip both
            // take their above / below arrow from it.
            ImGui::SliderFloat("Above / below arrow from (uu)", &cfg.compass_pip_height_uu, 0.0f, 10000.0f,
                               "%.0f");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("A marker this far off the player's own Z gets an arrow beside its\n"
                                  "glyph on the minimap and beside its pip on the compass. Within the\n"
                                  "band it counts as this floor. 0 turns the arrows off.");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("= %.1f m", static_cast<double>(cfg.compass_pip_height_uu) / 100.0);
            ImGui::SliderInt("Found file debounce (ms)", &cfg.found_save_debounce_ms, 200, 60000);
            ImGui::Checkbox("Read boss defeats from the save", &cfg.boss_defeat_from_save);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("A boss also counts as defeated when the `bossdoor_*` firepoint its\n"
                                  "level script names is unlocked in the save. Derived on every publish,\n"
                                  "never written to the found file.");
            }
            ImGui::SeparatorText("Absence as evidence of a collect");
            ImGui::SliderInt("Confirming rounds", &cfg.markers_absence_rounds, 1, 30);
            ImGui::TextDisabled("in force: %s",
                                mdb::format_category_mask(cfg.markers_absence_categories).c_str());
            // A rule, not a filter: it is not one of the three masks the Categories grid
            // owns.
            category_filter("Categories the rule may mark", cfg.markers_absence_categories, 4000, wrap);
        }

        void tune_gate(mm::Config& cfg)
        {
            ImGui::Checkbox("Only when the camera follows the pawn", &cfg.require_pawn_view);
            ImGui::SliderInt("State stale after (ms)", &cfg.state_stale_ms, 100, 60000);
            ImGui::SliderInt("Grace after a valid pawn (ms)", &cfg.min_visible_after_state_ok_ms, 0, 10000);
            ImGui::SliderInt("Delay after a menu closes (ms)", &cfg.menu_close_show_delay_ms, 0, 3000);
        }

        void tune_diagnostics(mm::Config& cfg)
        {
            // `normal` is what ships; the other two reproduce a problem with the running
            // commentary on, without editing a file.
            int lv = static_cast<int>(cfg.log_level);
            if (ImGui::Combo("Log detail", &lv, "normal\0verbose\0trace\0"))
            {
                cfg.log_level = static_cast<mm::LogLv>(lv);
            }
            ImGui::TextWrapped("normal = what a bug report needs. verbose = the running commentary "
                               "(player state, menu open/close, why the minimap is hidden). trace = "
                               "everything, including a marker census every two seconds.");
            char logpath[MAX_PATH * 2]{};
            ::WideCharToMultiByte(CP_UTF8, 0, mm::modlog_path().c_str(), -1, logpath, sizeof(logpath) - 1,
                                  nullptr, nullptr);
            ImGui::TextDisabled("Log file: %s", logpath);
            ImGui::Checkbox("Crash breadcrumb file", &cfg.crash_breadcrumb);
            ImGui::Checkbox("Scale the zoom keys with the display too", &cfg.zoom_dpi_scaled);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("On: one config shows the same area of world at 1080p and 2160p.\n"
                                  "Off: minimap_zoom and map_zoom are literal uu per pixel.");
            }
            // The other off switch: the same shutdown with no file written, for ruling
            // the mod out of a problem without a config to repair afterwards.
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
        }

        // Everything the Advanced tier tunes, plus the read-outs that answer a symptom.
        // Behind the Debug tab because none of it is a taste decision.
        void debug_tuning(mm::Config& cfg)
        {
            if (!ImGui::CollapsingHeader("Tuning"))
            {
                return;
            }
            const float wrap = ImGui::GetContentRegionAvail().x;
            ImGui::TextDisabled("Correct as shipped.");
            if (panel_section("Map background", kSecTuneBackground))
            {
                map_background(cfg);
            }
            if (panel_section("Minimap", kSecTuneMinimap))
            {
                tune_minimap(cfg);
            }
            if (panel_section("Full map", kSecTuneFullMap))
            {
                tune_fullmap(cfg);
            }
            if (panel_section("X-ray", kSecTuneXray))
            {
                tune_xray(cfg);
            }
            if (panel_section("Compass", kSecTuneCompass))
            {
                tune_compass(cfg);
            }
            if (panel_section("Floors", kSecTuneFloors))
            {
                tune_floors(cfg);
            }
            if (panel_section("Sweep & tracker", kSecTuneSweep))
            {
                tune_sweep(cfg, wrap);
            }
            if (panel_section("When the overlay is allowed on screen", kSecTuneGate))
            {
                tune_gate(cfg);
            }
            if (panel_section("Diagnostics", kSecTuneDiag))
            {
                tune_diagnostics(cfg);
            }
        }

        // Which collection file the tracker is using, and how it was chosen: a per-save
        // tracker that picked the wrong save looks exactly like a lost collection.
        void debug_found_profile(mm::Config& cfg)
        {
            const markers::Stats st = markers::stats();
            ImGui::SeparatorText("Collection file");
            ImGui::Text("%s", st.found_file[0] != '\0' ? st.found_file : "(none yet)");
            ImGui::SameLine();
            ImGui::TextDisabled("(via %s)", st.found_route[0] != '\0' ? st.found_route : "unresolved");
            ImGui::SetNextItemWidth(180.0f * g_chrome_scale);
            ImGui::InputText("found_profile", cfg.found_profile, sizeof(cfg.found_profile));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("auto = one file per save slot (recommended)\n"
                                  "shared = one file for every save\n"
                                  "anything else = wuchang_minimap_found_<name>.txt\n"
                                  "It takes effect on the next reload - the loop thread owns the file.");
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
        // The Keys tab
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
        // struct and publishes it) and is written to the file a moment later.

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
        // The hotkey swallow makes the GAME action the casualty of a clash, so the
        // player has to be told. Two sources:
        //
        //   * the player's real bindings, read off the running game by src/gamebinds.cpp
        //     (`gb::table`), which is exact - it follows a remap in the options menu;
        //   * until that answers - at the main menu, or on a build whose input chain
        //     stopped resolving - gb::kFallbackBinds, a guess about a default keyboard
        //     layout.
        //
        // The keys other injected DLLs own are only ever in the fallback list, since no
        // game-side table can know about them: F6 is RenoDX's DLSS 5 toggle (it ignores
        // modifiers and has caused a GPU crash), F10 is the UE4SS console, F9 / F11 are
        // engine binds and F12 is the Steam screenshot key. Those four are refused by
        // the config parser outright.

        // Render thread. The live table is ~9 KB, so it is copied only when the game
        // thread says it changed.
        const gb::Table& live_binds()
        {
            static gb::Table s_table;
            static std::uint32_t s_gen = 0;
            const std::uint32_t gen = gb::generation();
            if (gen != s_gen)
            {
                s_table = gb::table();
                s_gen = gen;
            }
            return s_table;
        }

        // "attack, weapon skill" - every game action this key is bound to, or "" for
        // none. One key legitimately drives several actions, and one action several
        // keys, so the same label is printed once.
        std::string live_bind_clash(const gb::Table& live, int binding)
        {
            if (mm::key_mod(binding) != mm::kKeyModNone)
            {
                return {}; // a modifier is the way OUT of a clash
            }
            return gb::clash_text(live.row, live.rows, mm::key_vk(binding));
        }

        void arm_capture(int row)
        {
            g_capture_row = row;
            g_capture_wait_release = true;
            mm::g_key_capture.store(row >= 0, std::memory_order_relaxed);
        }

        void panel_keys(mm::Config& cfg)
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
                else if (ImGui::GetIO().WantTextInput)
                {
                    // A text field has the caret (the import path, the font path):
                    // the letters are its, not the capture's. Re-arming the release wait
                    // means the key that leaves the field is not the one bound either.
                    g_capture_wait_release = true;
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

            text_disabled_wrapped("Click a key to rebind it, then press the new key - hold Ctrl, Shift or "
                                  "Alt with it for a modified binding. Esc cancels.");
            text_disabled_wrapped("A key bound here is taken away from the game while the mod is using "
                                  "it.");
            const gb::Table& game_binds = live_binds();
            // Only while the game's own bindings are still a guess: once they are read,
            // the clash column below is exact and needs no caveat.
            if (!game_binds.valid)
            {
                char line[256]{};
                (void)std::snprintf(line, sizeof(line),
                                    "The game's own bindings have not been read yet (%s), so the "
                                    "clashes below come from a built-in list of the usual ones.",
                                    game_binds.status[0] != '\0' ? game_binds.status
                                                                 : "no PlayerInput yet");
                text_disabled_wrapped(line);
            }

            if (ImGui::BeginTable("bindings", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Action");
                ImGui::TableSetupColumn("Key");
                ImGui::TableSetupColumn("");
                // The note is the only elastic column: it takes whatever the three
                // fixed ones leave, and its text wraps inside that instead of pushing
                // the table past the panel's right edge.
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
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
                    // The live table wins whole: once the game has answered, a key it
                    // does NOT list is genuinely free, whatever the fallback guess says.
                    const std::string live_clash =
                        game_binds.valid ? live_bind_clash(game_binds, vk) : std::string{};
                    const char* game = game_binds.valid
                                           ? (live_clash.empty() ? nullptr : live_clash.c_str())
                                           : game_bind_clash(vk);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kKeyBinds[i].label);

                    ImGui::TableNextColumn();
                    ImGui::PushID(i + 900);
                    const std::string shown = g_capture_row == i
                                                  ? std::string("press a key...")
                                                  : key_name_ascii(vk);
                    if (ImGui::Button(shown.c_str(), ImVec2{130.0f * g_chrome_scale, 0.0f}))
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
                    ImGui::PushTextWrapPos(0.0f);
                    if (clash != nullptr)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.72f, 0.35f, 1.0f});
                        ImGui::TextWrapped("also %s", clash);
                        ImGui::PopStyleColor();
                    }
                    else if (game != nullptr)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.72f, 0.35f, 1.0f});
                        ImGui::TextWrapped(game_binds.valid ? "the game uses it for %s"
                                                            : "the game may use it for %s",
                                           game);
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s\nWhile the mod is using this key the game does not "
                                              "get it.\nAdd Ctrl, Shift or Alt to give it back.",
                                              game_binds.valid
                                                  ? "Read from the game's own input mappings."
                                                  : "A guess: the game's bindings have not been "
                                                    "read yet.");
                        }
                    }
                    else if (twin != nullptr)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.80f, 0.80f, 0.55f, 1.0f});
                        ImGui::TextWrapped("same key as %s", twin);
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopTextWrapPos();
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
            // The gamepad chords
            //--------------------------------------------------------------------------
            if (!panel_section("Gamepad", kSecGamepad))
            {
                return;
            }
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
            ImGui::SetNextItemWidth(180.0f * g_chrome_scale);
            if (ImGui::InputText("X-ray chord", chord, sizeof(chord),
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
            {
                char hint[192]{};
                (void)std::snprintf(hint, sizeof(hint),
                                    "in force: %s   (LB, RB, LT, RT, A, B, X, Y, BACK, START, LS, "
                                    "RS, UP, DOWN, LEFT, RIGHT, joined with +; `none` disables it)",
                                    live.c_str());
                text_disabled_wrapped(hint);
            }
            if (ImGui::SmallButton("reset the chord"))
            {
                cfg.highlight_pad_mask = kDefaults.highlight_pad_mask;
                cfg.highlight_pad_lt = kDefaults.highlight_pad_lt;
                cfg.highlight_pad_rt = kDefaults.highlight_pad_rt;
                chord_primed = false;
            }

            // The full map's open chord. Buttons only - the triggers are not buttons
            // here, so set_pad_chord's LT / RT answers are discarded, exactly as the
            // config parser discards them.
            static char open_chord[64]{};
            static bool open_primed = false;
            const std::string open_live =
                wide_to_ascii(mm::pad_chord_name(cfg.map_pad_open_chord, false, false));
            if (!open_primed)
            {
                ::strncpy_s(open_chord, sizeof(open_chord), open_live.c_str(), _TRUNCATE);
                open_primed = true;
            }
            ImGui::SetNextItemWidth(180.0f * g_chrome_scale);
            if (ImGui::InputText("Open the map", open_chord, sizeof(open_chord),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                bool lt = false;
                bool rt = false;
                mm::set_pad_chord(open_chord, cfg.map_pad_open_chord, lt, rt);
                ::strncpy_s(open_chord, sizeof(open_chord),
                            wide_to_ascii(mm::pad_chord_name(cfg.map_pad_open_chord, false, false))
                                .c_str(),
                            _TRUNCATE);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("opens the full map: in force %s", open_live.c_str());

            // The settings panel's own chord, so a pad-only player can reach this panel.
            // Buttons only, like the map's.
            static char panel_chord[64]{};
            static bool panel_primed = false;
            const std::string panel_live =
                wide_to_ascii(mm::pad_chord_name(cfg.panel_pad_open_chord, false, false));
            if (!panel_primed)
            {
                ::strncpy_s(panel_chord, sizeof(panel_chord), panel_live.c_str(), _TRUNCATE);
                panel_primed = true;
            }
            ImGui::SetNextItemWidth(180.0f * g_chrome_scale);
            if (ImGui::InputText("Open this panel", panel_chord, sizeof(panel_chord),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                bool lt = false;
                bool rt = false;
                mm::set_pad_chord(panel_chord, cfg.panel_pad_open_chord, lt, rt);
                ::strncpy_s(panel_chord, sizeof(panel_chord),
                            wide_to_ascii(
                                mm::pad_chord_name(cfg.panel_pad_open_chord, false, false))
                                .c_str(),
                            _TRUNCATE);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("opens this panel: in force %s", panel_live.c_str());
            text_disabled_wrapped("the full map's own gamepad controls are fixed (left stick pans, "
                                  "triggers zoom, LB / RB change floor)");
        }

        void panel_debug(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            panel_dev_keys(cfg);
            debug_tuning(cfg);
            debug_found_profile(cfg);

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
            ImGui::Text("absence marks %d   levels loaded %d   (%d round(s), %s)",
                        st.absence_marks,
                        st.levels_loaded,
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
                // The scale belongs to the published window, not to the diagnostics block.
                const SliceView sv = slice_view();
                ImGui::Text("slice  %dx%d px x %d surface(s) @ %.4f px/uu   %.2f ms (peak %.2f)   "
                            "%llu update(s), %llu skipped",
                            g_slice_size,
                            g_slice_size,
                            g_slice_surfaces,
                            sv.px_per_uu,
                            g_slice_ms,
                            g_slice_ms_peak,
                            static_cast<unsigned long long>(g_slice_updates),
                            static_cast<unsigned long long>(g_slice_skipped));
                ImGui::Text("       feet Z %.0f (raw %.0f)   tol %.0f  fade %.0f/%.0f  gradient %.2f   "
                            "opaque %u / dim %u / faint %u   unreachable %s, %u px",
                            static_cast<double>(g_feet_z),
                            snap.z - static_cast<double>(cfg.player_z_offset),
                            static_cast<double>(cfg.floor_z_tolerance),
                            static_cast<double>(cfg.floor_fade_uu),
                            static_cast<double>(cfg.floor_fade_above_uu),
                            static_cast<double>(cfg.floor_gradient_strength),
                            g_slice_opaque,
                            g_slice_dim,
                            g_slice_faint,
                            mapdata::reachability_available()
                                ? srule::unreachable_name(cfg.map_unreachable)
                                : "n/a (asset has no reachability)",
                            g_slice_unreach);
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
            ImGui::SetNextWindowSize(ImVec2{520.0f * g_chrome_scale, 620.0f * g_chrome_scale},
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

            // Two clicks to reset: this throws away every tuned value in the struct.
            // Armed until the panel is closed or the button is pressed. Declared here
            // because the footer's height is measured from the buttons it will hold.
            static bool confirm_reset = false;

            float fw[3]{};
            int fn = 0;
            fw[fn++] = button_width("Reload map data");
            if (confirm_reset)
            {
                fw[fn++] = button_width("Yes, reset everything");
                fw[fn++] = button_width("Cancel");
            }
            else
            {
                fw[fn++] = button_width("Reset to defaults");
            }

            // The tabs get their own child so the footer row is always at the bottom of
            // the window and never scrolls away with them. The reserve counts the lines
            // that row actually wraps to at this width, so a narrow panel grows the
            // footer instead of clipping half of it.
            const float footer_avail = ImGui::GetContentRegionAvail().x;
            int footer_lines = 1;
            {
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                float x = 0.0f;
                for (int i = 0; i < fn; ++i)
                {
                    const float need = (x > 0.0f ? spacing : 0.0f) + fw[i];
                    if (x > 0.0f && x + need > footer_avail)
                    {
                        ++footer_lines;
                        x = fw[i];
                    }
                    else
                    {
                        x += need;
                    }
                }
            }
            const float row_h =
                ImGui::GetFrameHeightWithSpacing() * static_cast<float>(footer_lines + 1) +
                (cfg.mod_enabled ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 2.0f) +
                ImGui::GetStyle().ItemSpacing.y;
            if (ImGui::BeginChild("tabs", ImVec2{0.0f, -row_h}, ImGuiChildFlags_None))
            {
                if (ImGui::BeginTabBar("wuchang_tabs"))
                {
                    if (ImGui::BeginTabItem("Overview"))
                    {
                        panel_overview(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Categories"))
                    {
                        panel_categories(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Map & tracker"))
                    {
                        panel_map_tracker(cfg, snap, have_state);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Keys"))
                    {
                        panel_keys(cfg);
                        ImGui::EndTabItem();
                    }
                    // The Debug tab exists only while debug_readout is on, a Dev key in
                    // a file players do not have, so a player sees four tabs.
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
            // Each button keeps the row only while it still fits: the footer wraps
            // rather than run off a narrowed panel.
            if (ImGui::Button("Reload map data"))
            {
                mm::g_reload_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Re-read the settings, the maps and the marker database from "
                                  "disk. Takes a moment.");
            }
            (void)same_line_if_fits(confirm_reset ? button_width("Yes, reset everything")
                                                  : button_width("Reset to defaults"));
            if (!confirm_reset)
            {
                if (ImGui::Button("Reset to defaults"))
                {
                    confirm_reset = true;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Every setting back to what the mod ships with, hotkeys "
                                      "included. Written to the config file like any other change.");
                }
            }
            else
            {
                if (ImGui::Button("Yes, reset everything"))
                {
                    confirm_reset = false;
                    const bool was_on = cfg.mod_enabled;
                    cfg = mm::Config{};
                    // The master switch is not a preference: it has its own checkbox and
                    // its own log line.
                    cfg.mod_enabled = was_on;
                    mm::log(L"config: reset to the shipped defaults from the F2 panel");
                }
                (void)same_line_if_fits(button_width("Cancel"));
                if (ImGui::Button("Cancel"))
                {
                    confirm_reset = false;
                }
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
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.72f, 0.35f, 1.0f});
                ImGui::TextWrapped("The mod is shutting down. Set mod_enabled = 1 in %s to restart it.",
                                   "config_wuchang_minimap.txt");
                ImGui::PopStyleColor();
            }

            ImGui::End();

            // Field by field, not memcmp: a Config is a value, and `mm::operator==` is
            // generated from the struct with a byte-flip drift guard in markers_test.
            // Every change is remembered: the loop thread rewrites the config files a
            // moment after the last one, so a dragged slider costs one write.
            if (before != cfg)
            {
                mm::set_config(cfg);
                mm::g_save_config_soon = true;
            }
            if (!open)
            {
                mm::g_panel_open = false;
            }
        }
    } // namespace ovl
} // namespace overlay
