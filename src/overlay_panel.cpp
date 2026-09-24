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
#include "langsel.hpp"
#include "imgui_internal.h"

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
            kSecPerformance,
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
            ImGui::Checkbox(lbl(S::Minimap).c_str(), &cfg.show_minimap);
            ImGui::Indent();
            // Every range here is the loader's clamp, so the panel never shows a value
            // the file will not keep.
            ImGui::SliderFloat(lbl(S::OvSize).c_str(), &cfg.size_frac, 0.05f, 0.9f, "%.2f");
            ImGui::SliderFloat(lbl(S::OvZoom).c_str(), &cfg.zoom_uu_per_px, 2.0f, 400.0f, "%.0f");
            ImGui::SliderFloat(lbl(S::OvOpacity).c_str(), &cfg.opacity, 0.1f, 1.0f, "%.2f");
            bool round_shape = cfg.round;
            if (ImGui::Checkbox(lbl(S::OvRound).c_str(), &round_shape))
            {
                cfg.round = round_shape;
            }
            ImGui::SameLine();
            ImGui::Checkbox(lbl(S::OvRotate).c_str(), &cfg.rotate_with_player);
            ImGui::Checkbox(lbl(S::OvAdjacentFloors).c_str(), &cfg.show_adjacent_floors);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::Checkbox(lbl(S::Compass).c_str(), &cfg.compass_enabled);
            ImGui::Indent();
            ImGui::SliderFloat(lbl(S::OvWidth).c_str(), &cfg.compass_width, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat(lbl(S::OvOpacity, "compass").c_str(), &cfg.compass_opacity, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat(lbl(S::OvFieldOfView).c_str(), &cfg.compass_span_deg, 30.0f, 360.0f,
                               tr(S::FmtDeg));
            ImGui::SliderFloat(lbl(S::OvHeight).c_str(), &cfg.compass_height, 10.0f, 120.0f, tr(S::FmtPx));
            ImGui::Checkbox(lbl(S::OvPipLabels).c_str(), &cfg.compass_pip_labels);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::Checkbox(lbl(S::Xray).c_str(), &cfg.highlight_enabled);
            ImGui::Indent();
            // The combo carries the key beside it, which is what the two "hold means..."
            // paragraphs used to say.
            std::string hold = key_display(cfg.highlight_key);
            if (cfg.highlight_gamepad)
            {
                const std::string chord = wide_to_ascii(mm::pad_chord_name(
                    cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt));
                hold = lang::fmt<S::OvKeyOrPad>(hold.c_str(), chord.c_str()).c_str();
            }
            int hl_mode = cfg.highlight_mode == mm::HighlightMode::Hold ? 1 : 0;
            const char* const hl_modes[] = {tr(S::OvModeToggle), tr(S::OvModeHold)};
            // Wide enough for the longer word in this language, plus the arrow button.
            const float mode_w =
                (std::max)(ImGui::CalcTextSize(hl_modes[0]).x, ImGui::CalcTextSize(hl_modes[1]).x) +
                ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth((std::max)(110.0f * g_chrome_scale, mode_w));
            if (ImGui::Combo("##hl_mode", &hl_mode, hl_modes, 2))
            {
                cfg.highlight_mode = hl_mode == 1 ? mm::HighlightMode::Hold : mm::HighlightMode::Toggle;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(lang::fmt<S::OvInWorld>(hold.c_str()).c_str());
            // Metres on the slider: the key is world units and 1 uu = 1 cm, so the panel
            // converts rather than teach the player a second unit.
            float radius_m = cfg.highlight_radius / 100.0f;
            if (ImGui::SliderFloat(lbl(S::OvRadius).c_str(), &radius_m, 2.0f, 500.0f, tr(S::UnitM)))
            {
                cfg.highlight_radius = radius_m * 100.0f;
            }
            ImGui::Checkbox(lbl(S::OvNamesDistance).c_str(), &cfg.highlight_labels);
            ImGui::SameLine();
            ImGui::Checkbox(lbl(S::OvEdgeArrows).c_str(), &cfg.highlight_edge_arrows);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::Checkbox(lbl(S::OvShowMarkers).c_str(), &cfg.markers_enabled);
            ImGui::SameLine();
            ImGui::Checkbox(lbl(S::OvHideInMenus).c_str(), &cfg.hide_in_menus);
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
            const char* const corners[] = {tr(S::OvTopLeft), tr(S::OvTopRight), tr(S::OvBottomLeft),
                                           tr(S::OvBottomRight)};
            const char* const presets[] = {tr(S::OvPresetCustom), corners[0], corners[1], corners[2],
                                           corners[3]};
            if (ImGui::Combo(lbl(S::OvHudPlacement).c_str(), &preset, presets, 5))
            {
                cfg.hud_preset = static_cast<mm::HudPreset>(preset);
            }
            if (cfg.hud_preset == mm::HudPreset::Custom)
            {
                ImGui::Indent();
                int anchor = static_cast<int>(cfg.anchor);
                if (ImGui::Combo(lbl(S::OvMinimapCorner).c_str(), &anchor, corners, 4))
                {
                    cfg.anchor = static_cast<mm::Anchor>(anchor);
                }
                int canchor = cfg.compass_anchor == mm::VAnchor::Bottom ? 1 : 0;
                const char* const canchors[] = {tr(S::OvEdgeTop), tr(S::OvEdgeBottom)};
                if (ImGui::Combo(lbl(S::OvCompassEdge).c_str(), &canchor, canchors, 2))
                {
                    cfg.compass_anchor = canchor == 1 ? mm::VAnchor::Bottom : mm::VAnchor::Top;
                }
                ImGui::Unindent();
            }
            // One control, because the two numbers are one gap from one corner.
            float offset[2] = {cfg.offset_x, cfg.offset_y};
            if (ImGui::DragFloat2(lbl(S::OvMinimapOffset).c_str(), offset, 1.0f, 0.0f, 4000.0f, tr(S::FmtPx)))
            {
                cfg.offset_x = offset[0];
                cfg.offset_y = offset[1];
            }
            ImGui::SliderFloat(lbl(S::OvCompassOffset).c_str(), &cfg.compass_offset_y, 0.0f, 2000.0f,
                               tr(S::FmtPx));

            // `auto` is a checkbox over the slider rather than a magic value inside the
            // number, so the slider always says what is in force.
            bool auto_scale = cfg.ui_scale_auto;
            if (ImGui::Checkbox(lbl(S::OvAutoScale).c_str(), &auto_scale))
            {
                cfg.ui_scale_auto = auto_scale;
                if (!auto_scale)
                {
                    cfg.ui_scale = g_ui_scale; // start from what is on screen right now
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", lang::fmt<S::OvScaleInForce>(static_cast<double>(g_ui_scale)).c_str());
            ImGui::BeginDisabled(cfg.ui_scale_auto);
            ImGui::SliderFloat(lbl(S::OvUiScale).c_str(), &cfg.ui_scale, kUiScaleMin, kUiScaleMax, "%.2f");
            ImGui::EndDisabled();
            // The text size the UI scale multiplies. The whole overlay re-rasterises on
            // the next frame, so the slider shows its own result while it is dragged.
            ImGui::SliderInt(lbl(S::OvTextSize).c_str(), &cfg.font_size, kFontPxMin, kFontPxMax, tr(S::FmtIntPx));
        }

        //--------------------------------------------------------------------------
        // Overview: look
        //--------------------------------------------------------------------------
        //
        // In the FILE a theme recolours every colour key the player has not customised; in the
        // PANEL choosing one is explicit, so it writes the theme's colours into the four
        // colour keys there and then - otherwise the combo looks broken for a player
        // whose config spells one of those keys out.
        void overview_look(mm::Config& cfg)
        {
            int theme_i = static_cast<int>(cfg.theme);
            const char* const themes[] = {tr(S::OvThemeNeutral), tr(S::OvThemeInk)};
            if (ImGui::Combo(lbl(S::OvTheme).c_str(), &theme_i, themes, 2))
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
            }
            int pal_i = static_cast<int>(cfg.palette);
            const char* const pals[] = {tr(S::OvPaletteDefault), tr(S::OvPaletteColorblind)};
            if (ImGui::Combo(lbl(S::OvPalette).c_str(), &pal_i, pals, 2))
            {
                cfg.palette = static_cast<gly::Palette>(pal_i);
            }
            // The whole overlay's typeface. Empty or unreadable falls back to the
            // built-in font, which the log says.
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(lbl(S::OvFont).c_str(), tr(S::OvFontHint), cfg.ui_font, sizeof(cfg.ui_font));
        }

        //==============================================================================
        // The Categories tab
        //==============================================================================
        //
        // One grid instead of four chip rows: every category down the side, the loot ones
        // as one indented block under their tier,
        // the three surfaces across the top, one mask per column, and one leading column
        // that holds the row itself - the category, or the whole tier under its heading,
        // on all three surfaces at once. A row label is the legend only: the category's
        // own glyph and colour, and its live found / known count.

        // A column header: the surface's name over `all` / `none` for its mask. The mask
        // it edits is the id, so the buttons keep their identity in every language.
        void mask_column_header(const char* name, std::uint32_t& mask)
        {
            ImGui::PushID(&mask);
            ImGui::TextUnformatted(name);
            if (ImGui::SmallButton(lbl(S::All).c_str()))
            {
                mask = mdb::kAllCats;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(lbl(S::None).c_str()))
            {
                mask = 0u;
            }
            ImGui::PopID();
        }

        // One box for a GROUP on all three surfaces at once, which is the leading
        // column's per-category box and the per-tier one on a `Loot - <tier>` heading.
        // `group` is a category bit or a tier_mask(), and the three masks answer for
        // themselves: a fully-on group is the only ON, a group no mask holds is OFF, and
        // every answer between the two draws as a dash. A click follows the rule a single
        // surface already follows - a partial group goes fully on and only a fully-on one
        // clears - so the three masks are written from the one answer.
        void group_checkbox_all_surfaces(const char* id, std::uint32_t* const (&masks)[3],
                                         std::uint32_t group)
        {
            int all_count = 0;
            int any_count = 0;
            for (std::uint32_t* const m : masks)
            {
                const mdb::GroupState s = mdb::group_state(*m, group);
                if (s == mdb::GroupState::All)
                {
                    ++all_count;
                }
                if (s != mdb::GroupState::None)
                {
                    ++any_count;
                }
            }
            bool on = all_count == 3;
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, any_count > 0 && all_count < 3);
            if (ImGui::Checkbox(id, &on))
            {
                for (std::uint32_t* m : masks)
                {
                    *m = on ? (*m | group) : (*m & ~group);
                }
            }
            ImGui::PopItemFlag();
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
            if (!ImGui::BeginTable("categories", 5,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp))
            {
                return;
            }
            // The leading column is the row's own box and takes a heading from no one.
            ImGui::TableSetupColumn("##all", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(tr(S::CgCategory), ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn(tr(S::CgMinimapAndMap));
            ImGui::TableSetupColumn(tr(S::Xray));
            ImGui::TableSetupColumn(tr(S::Compass));
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(S::CgCategory));
            ImGui::TextDisabled("%s", tr(per_chapter ? S::CgFoundKnownChapter : S::CgFoundKnown));
            ImGui::TableNextColumn();
            mask_column_header(tr(S::CgMinimapAndMap), cfg.markers_categories);
            ImGui::TableNextColumn();
            mask_column_header(tr(S::Xray), cfg.highlight_categories);
            ImGui::TableNextColumn();
            mask_column_header(tr(S::Compass), cfg.compass_categories);

            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.34f);
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(i);
                const std::uint32_t bit = mdb::cat_bit(cat);
                // The eleven loot categories are one block under three tier headings, in
                // tier order, and indented: they are kinds of one thing, not eleven peers
                // of `shrine`. The Cat enum holds them contiguously, so "the first of its
                // tier" is the whole test.
                mdb::Tier tier = mdb::Tier::Common;
                const bool loot = mdb::tier_of(cat, tier);
                mdb::Tier prev_tier = mdb::Tier::Common;
                const bool prev_loot =
                    i > 0 && mdb::tier_of(static_cast<mdb::Cat>(i - 1), prev_tier);
                if (loot && (!prev_loot || prev_tier != tier))
                {
                    const std::uint32_t tm = mdb::tier_mask(tier);
                    ImGui::TableNextRow();
                    // The heading is a tier's own row: the whole tier on every surface, or
                    // on one. Its id keeps the three headings' boxes, and the category
                    // rows', off one another.
                    ImGui::PushID(static_cast<int>(tier));
                    ImGui::TableNextColumn();
                    group_checkbox_all_surfaces("##tier", masks, tm);
                    ImGui::TableNextColumn();
                    const lang::Text<128> heading = lang::fmt<S::CgLootTier, 128>(mdb::tier_name(static_cast<int>(tier)));
                    ImGui::TextDisabled("%s", heading.c_str());
                    for (int c = 0; c < 3; ++c)
                    {
                        ImGui::TableNextColumn();
                        ImGui::PushID(c);
                        const mdb::GroupState s = mdb::group_state(*masks[c], tm);
                        bool on = s == mdb::GroupState::All;
                        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue,
                                            s == mdb::GroupState::Some);
                        if (ImGui::Checkbox("##tier_on", &on))
                        {
                            *masks[c] = mdb::group_toggle(*masks[c], tm, s);
                        }
                        ImGui::PopItemFlag();
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
                const float indent = loot ? ImGui::GetTextLineHeight() : 0.0f;
                const markers::CatStat& cs = per_chapter ? st.chapter[fch][i] : st.cat[i];
                int on_count = 0;
                for (const std::uint32_t* m : masks)
                {
                    on_count += mdb::group_state(*m, bit) != mdb::GroupState::None ? 1 : 0;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                // The row's one control, and the only thing on it that toggles anything:
                // the label below is the legend.
                group_checkbox_all_surfaces("##row", masks, bit);
                ImGui::TableNextColumn();
                // The leading spaces are the glyph's gutter: the glyph is drawn over the
                // row afterwards.
                char label[128]{};
                if (cs.total > 0)
                {
                    const lang::Text<112> count = lang::fmt<S::CgRowCount, 112>(mdb::cat_label(cat), cs.found, cs.total);
                    (void)utf8::format(label, sizeof(label), "%s%s", kGlyphGutter, count.c_str());
                }
                else
                {
                    (void)utf8::format(label, sizeof(label), "%s%s", kGlyphGutter, mdb::cat_label(cat));
                }
                ImGui::Indent(indent);
                const ImVec2 row = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      on_count > 0 ? marker_color(cat, 255) : IM_COL32(150, 150, 150, 170));
                ImGui::TextUnformatted(label);
                ImGui::PopStyleColor();
                draw_marker_glyph(dl, cat,
                                  ImVec2{row.x + glyph_r + 4.0f, row.y + ImGui::GetTextLineHeight() * 0.5f},
                                  glyph_r, marker_color(cat, on_count > 0 ? 255 : 90),
                                  IM_COL32(14, 16, 20, on_count > 0 ? 220 : 80));
                ImGui::Unindent(indent);
                for (int c = 0; c < 3; ++c)
                {
                    ImGui::TableNextColumn();
                    ImGui::PushID(c);
                    const mdb::GroupState s = mdb::group_state(*masks[c], bit);
                    bool on = s == mdb::GroupState::All;
                    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue,
                                        s == mdb::GroupState::Some);
                    if (ImGui::Checkbox("##on", &on))
                    {
                        *masks[c] = mdb::group_toggle(*masks[c], bit, s);
                    }
                    ImGui::PopItemFlag();
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
            ImGui::TextUnformatted(tr(S::CgShowFound));
            ImGui::SameLine();
            bool show_found = !cfg.markers_hide_found;
            if (ImGui::Checkbox(lbl(S::CgShowFoundSurfaces).c_str(), &show_found))
            {
                cfg.markers_hide_found = !show_found;
            }
            ImGui::SameLine();
            ImGui::Checkbox(lbl(S::CgXrayLower, "found").c_str(), &cfg.highlight_show_found);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat(lbl(S::CgHowFaint).c_str(), &cfg.markers_found_alpha, 0.0f, 1.0f, "%.2f");
            text_disabled_wrapped(tr(S::CgFoundHelp));

            ImGui::TextUnformatted(tr(S::CgGlyphSize));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat(lbl(S::CgGlyphMinimap).c_str(), &cfg.markers_size, 2.0f, 24.0f, tr(S::FmtPx1));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat(lbl(S::CgGlyphMap).c_str(), &cfg.map_marker_size, 2.0f, 32.0f, tr(S::FmtPx1));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(dial_w);
            ImGui::SliderFloat(lbl(S::CgXrayLower, "glyph").c_str(), &cfg.highlight_size, 2.0f, 32.0f,
                               tr(S::FmtPx1));

            ImGui::Checkbox(lbl(S::CgClampToRim).c_str(), &cfg.markers_clamp_to_edge);
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
            ImGui::TextDisabled("%s", lang::fmt<S::MtPressToOpen>(key_display(cfg.map_key).c_str()).c_str());
            ImGui::Checkbox(lbl(S::Gamepad).c_str(), &cfg.map_gamepad);
        }

        // What a waypoint is ON, in words: the published marker nearest to it, when one
        // is close enough to be what the player clicked. The published buffer runs to
        // thousands of entries, so the whole table is recomputed twice a second and the
        // rows read it - the first row of a due frame pays for all of them.
        const char* waypoint_place(const mv::WaypointSet& wps, std::size_t index, std::uint64_t now)
        {
            constexpr double kNearUu = 400.0;
            static char names[mv::kMaxWaypoints][64]{};
            static std::uint64_t due = 0;
            if (now >= due)
            {
                due = now + 500;
                const markers::View published = markers::view();
                for (std::size_t w = 0; w < mv::kMaxWaypoints; ++w)
                {
                    const char* name = tr(S::MtAPlaceOnTheMap);
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
                    utf8::copy(names[w], sizeof(names[w]), name);
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
                    text_disabled_wrapped(lang::fmt<S::MtNoWaypointsKey, 1024>(
                                              key_display(cfg.waypoint_nearest_key).c_str())
                                              .c_str());
                }
                else
                {
                    text_disabled_wrapped(tr(S::MtNoWaypoints));
                }
                return;
            }
            ImGui::TextUnformatted(lang::fmt<S::CountOf>(wps.count, mv::kMaxWaypoints).c_str());
            const Label clear_wps = lbl(S::MtClearWaypoints);
            (void)same_line_if_fits(button_width(clear_wps.c_str()));
            if (ImGui::SmallButton(clear_wps.c_str()))
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
                    const double away = std::sqrt(dx * dx + dy * dy) / 100.0;
                    ImGui::TextUnformatted(lang::fmt<S::MtWaypointRowAway>(wi + 1, place, away).c_str());
                }
                else
                {
                    ImGui::TextUnformatted(lang::fmt<S::MtWaypointRow>(wi + 1, place).c_str());
                }
                ImGui::PopID();
            }
        }

        // The save-slot key inside a found filename, or the word for the file every save
        // shares. Cheap enough to do per frame: one small string.
        std::string slot_key_of(const char* found_file)
        {
            static constexpr char kPrefix[] = "wuchang_minimap_found_";
            constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
            constexpr std::size_t kSuffixLen = 4; // ".txt"
            const std::string name = found_file != nullptr ? found_file : "";
            if (name.empty())
            {
                return tr(S::MtSlotUnknown);
            }
            if (name.size() <= kPrefixLen + kSuffixLen || name.compare(0, kPrefixLen, kPrefix) != 0)
            {
                return tr(S::MtSlotShared);
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
            // One id for both faces of the button, so the arming click and the confirming one
            // land on the same widget in every language.
            const bool clear_pressed = ImGui::Button(
                lang::Text<256>("%s###clear_found", tr(armed ? S::MtClickAgain : S::MtClearFound)).c_str());
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
                ImGui::SetTooltip("%s", lang::fmt<S::MtClearFoundTip, 1024>(
                                            st.found_file[0] != '\0' ? st.found_file : tr(S::MtTheFoundFile))
                                            .c_str());
            }
            ImGui::SameLine();
            // The save it would wipe, spelled out beside the button: the filename above is
            // easy to read past, and this is the one thing worth being sure of.
            ImGui::TextDisabled("%s", lang::fmt<S::MtSave>(slot_key_of(st.found_file).c_str()).c_str());

            //---- import / export ------------------------------------------------------
            //
            // The found list and the waypoints of this profile as one JSON file in the
            // mod folder. Both buttons only raise a flag: the loop thread owns every
            // read and write (overlay.cpp).
            ImGui::SeparatorText(tr(S::MtBackup));
            static char import_path[512]{};
            // The newest export the loop thread found, offered once - on the first frame
            // it exists, and again after an export writes a newer one. Never on every
            // empty frame: a box the player has cleared has to stay clear.
            static bool import_path_seeded = false;
            if (ImGui::Button(lbl(S::MtExport).c_str()))
            {
                g_export_request.store(true, std::memory_order_release);
                import_path_seeded = false; // offer the file it is about to write
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", lang::fmt<S::MtExportTip, 1024>().c_str());
            }
            if (!import_path_seeded && import_path[0] == '\0' &&
                g_latest_export_ready.load(std::memory_order_acquire))
            {
                spin::SpinGuard guard(g_exchange_lock);
                ::strncpy_s(import_path, sizeof(import_path), g_latest_export, _TRUNCATE);
                import_path_seeded = import_path[0] != '\0';
            }
            ImGui::SameLine();
            if (ImGui::Button(lbl(S::MtImport).c_str()))
            {
                {
                    spin::SpinGuard guard(g_exchange_lock);
                    ::strncpy_s(g_import_path, sizeof(g_import_path), import_path, _TRUNCATE);
                }
                g_import_request.store(true, std::memory_order_release);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", tr(S::MtImportTip));
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##import_path", tr(S::MtImportHint), import_path, sizeof(import_path));

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
            return mm::state_dir() + L"\\wuchang_minimap_panel.txt";
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

        // How often the overlay's frame happens at all - the one setting here that trades a
        // little smoothness for the game's own frame rate, which is why it sits with the
        // player's settings and not in the Tuning block. Dragging it is the only way to
        // judge it, so the panel itself is drawn through it: what the slider does to the
        // x-ray and the minimap, it does to this window too.
        void overview_update_rate(mm::Config& cfg)
        {
            // The uncapped end is its own control rather than the slider's zero: a slider
            // that accepts 2 and snaps to 15 reads as a broken setting, and the floor is
            // not negotiable - this window is drawn through the same ceiling.
            bool uncapped = cfg.overlay_update_hz <= fgate::kUncapped;
            if (ImGui::Checkbox(lbl(S::OvRedrawEveryFrame).c_str(), &uncapped))
            {
                cfg.overlay_update_hz = uncapped ? fgate::kUncapped : 60;
            }
            ImGui::BeginDisabled(uncapped);
            int hz = uncapped ? 60 : cfg.overlay_update_hz;
            if (ImGui::SliderInt(lbl(S::OvUpdatesPerSecond).c_str(), &hz, fgate::kHzMin, 240, tr(S::FmtHz)) &&
                !uncapped)
            {
                cfg.overlay_update_hz = fgate::clamp_hz(hz);
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", tr(S::OvUpdateRateHelp));
        }

        //--------------------------------------------------------------------------
        // Overview: the language
        //--------------------------------------------------------------------------
        //
        // Above every section and in no header, because it is the one setting a player who
        // cannot read the rest has to find. Every language is listed by its own name, so the
        // list is readable from any of them; drawing those names needs fonts beyond the
        // culture's own, which are asked for once the list is hovered or opened - the closed
        // combo only ever shows the culture in force. The choice lands in
        // `language` like any other panel edit, and the loop thread applies it (langsel).
        void overview_language(mm::Config& cfg)
        {
            bool is_auto = true;
            lang::Culture pinned = lang::Culture::En;
            (void)lang::parse_override(cfg.language, is_auto, pinned);
            // Named after what `auto` would pick, not the culture in force: with a language
            // pinned the two differ, and this entry is the way back to the game's.
            const std::string_view follows = lang::endonym(lsel::auto_culture());
            const lang::Text<128> auto_item("%s (%.*s)###lang_auto", tr(S::LangAuto),
                                            static_cast<int>(follows.size()), follows.data());
            const std::string_view shown = is_auto ? std::string_view{auto_item.c_str()} : lang::endonym(pinned);
            const std::string preview{shown.substr(0, shown.find("###"))};
            if (ImGui::BeginCombo("##language", preview.c_str()))
            {
                g_font_endonyms = true;
                if (ImGui::Selectable(auto_item.c_str(), is_auto))
                {
                    mm::set_language(cfg, "auto");
                }
                for (int i = 0; i < lang::kCultureCount; ++i)
                {
                    const auto c = static_cast<lang::Culture>(i);
                    const std::string_view name = lang::endonym(c);
                    const std::string_view code = lang::code(c);
                    const lang::Text<64> item("%.*s###lang_%.*s", static_cast<int>(name.size()), name.data(),
                                              static_cast<int>(code.size()), code.data());
                    if (ImGui::Selectable(item.c_str(), !is_auto && c == pinned))
                    {
                        mm::set_language(cfg, code);
                    }
                }
                ImGui::EndCombo();
            }
            // Hovered, not focused: gamepad navigation lands on the first widget of a panel
            // just opened, and that alone must not cost a player 33 MB of fonts. A list
            // opened from the pad draws its foreign names on its second frame.
            else if (ImGui::IsItemHovered())
            {
                g_font_endonyms = true;
            }
        }

        void panel_overview(mm::Config& cfg)
        {
            overview_language(cfg);
            if (panel_section(lbl(S::SecWhatIsOn).c_str(), kSecWhatIsOn))
            {
                overview_what_is_on(cfg);
            }
            if (panel_section(lbl(S::SecPlacement).c_str(), kSecPlacement))
            {
                overview_placement(cfg);
            }
            if (panel_section(lbl(S::SecLook).c_str(), kSecLook))
            {
                overview_look(cfg);
            }
            if (panel_section(lbl(S::SecPerformance).c_str(), kSecPerformance))
            {
                overview_update_rate(cfg);
            }
        }

        void panel_map_tracker(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            // The tracker leads: it is the section with the per-save buttons, and further
            // down the tab they sit below the fold.
            if (panel_section(lbl(S::SecTracker).c_str(), kSecTracker))
            {
                map_tracker();
            }
            if (panel_section(lbl(S::FullMap).c_str(), kSecFullMap))
            {
                map_fullmap(cfg);
            }
            if (panel_section(lbl(S::Waypoints).c_str(), kSecWaypoints))
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
                ImGui::TextDisabled("zoom presets (%s cycles): %s", key_display(cfg.zoom_key).c_str(),
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
            ImGui::SameLine();
            ImGui::TextDisabled("the ramp itself is on the Developer settings");
            ImGui::SliderInt("Slice rate (Hz)", &cfg.slice_hz, 2, 30);
            ImGui::SliderInt("Feet Z smoothing (ms)", &cfg.feet_z_smooth_ms, 1, 2000);
            ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -500.0f, 500.0f,
                               "%.0f");
        }

        void tune_sweep(mm::Config& cfg, float wrap)
        {
            ImGui::Checkbox("Live actor sweep", &cfg.markers_live);
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

        // The height ramp: colour by absolute Z, one ramp for every storey. Every edit
        // lands in the live config on the same frame and is written back to the dev file
        // a moment later, so tuning here is what the next launch starts with.
        void dev_shading(mm::Config& cfg)
        {
            const auto color_key = [](const char* label, float& r, float& g, float& b) {
                float c[3] = {r / 255.0f, g / 255.0f, b / 255.0f};
                if (ImGui::ColorEdit3(label, c, ImGuiColorEditFlags_NoInputs))
                {
                    r = c[0] * 255.0f;
                    g = c[1] * 255.0f;
                    b = c[2] * 255.0f;
                }
            };
            color_key("Low ground", cfg.shade_lo_r, cfg.shade_lo_g, cfg.shade_lo_b);
            ImGui::SameLine();
            color_key("High ground", cfg.shade_hi_r, cfg.shade_hi_g, cfg.shade_hi_b);
            ImGui::SliderFloat("Ramp gamma", &cfg.shade_gamma, 0.1f, 4.0f, "%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("< 1 lifts the low ground");
            ImGui::SliderFloat("Storey below, opacity", &cfg.shade_below_alpha, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Ledge overhead, opacity", &cfg.shade_above_alpha, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Ledge overhead, band (uu)", &cfg.shade_above_band_uu, 0.0f, 3000.0f,
                               "%.0f");
            ImGui::SameLine();
            ImGui::TextDisabled("0 = nothing above you");
            ImGui::SliderFloat("Ramp ends, percentile", &cfg.shade_range_pct_lo, 0.0f, 25.0f, "%.0f");
            ImGui::SameLine();
            ImGui::TextDisabled("of the Z the cut drew; 0 = its min..max");
            ImGui::SliderFloat("Minimap ramp, narrowest span (uu)", &cfg.shade_min_range_uu, 0.0f,
                               5000.0f, "%.0f");
            ImGui::SliderInt("Minimap ramp, easing (ms)", &cfg.shade_range_smooth_ms, 0, 5000);
            ImGui::Checkbox("Full map: equalise the ramp", &cfg.shade_map_equalize);
            ImGui::SameLine();
            ImGui::TextDisabled("spend it on area, not on height");
            ImGui::SliderFloat("Full map: one height's share, cap", &cfg.shade_map_clip, 0.0f,
                               64.0f, "%.0fx");
            ImGui::SameLine();
            ImGui::TextDisabled("x its flat share of the ramp; 0 = uncapped");
            ImGui::TextDisabled("the full map ignores the band and the two minimap ramp keys: it "
                                "draws the nearest storey at or above your feet, however high, and "
                                "colours its own cut");
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
            ImGui::SeparatorText("Height shading");
            dev_shading(cfg);
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

        //======================================================================
        // The Keys tab
        //======================================================================
        //
        // The capture state machine, one row per binding, and the three gamepad chords -
        // which are one editor called three times.
        namespace
        {
            // What a binding reverts to. Namespace scope rather than a function static: a
            // guarded static's first call runs the CRT's thread-safe-init path.
            const mm::Config kKeyDefaults{};

            // THE CAPTURE, before anything is drawn.
            //
            // A capture takes two shapes: `ctrl+m` (hold Ctrl, press M) and a bare modifier
            // (`LALT`, the x-ray's shipped default). So a non-modifier key wins immediately
            // and carries whatever modifier is held with it, while a modifier pressed on its
            // own is only taken once everything is released - the only way to tell "reaching
            // for Ctrl+M" from "I want Ctrl".
            void keys_capture(mm::Config& cfg)
            {
                if (g_capture_row < 0)
                {
                    return;
                }
                if (g_capture_row >= kKeyBindCount)
                {
                    arm_capture(-1);
                    return;
                }
                bool any_down = false;
                int pressed = 0;  // a real key: bind it now, with the held modifier
                int mod_only = 0; // a modifier on its own: bind it on release
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
                    return;
                }
                if (ImGui::GetIO().WantTextInput)
                {
                    // A text field has the caret (the import path, the font path): the letters
                    // are its, not the capture's. Re-arming the release wait means the key that
                    // leaves the field is not the one bound either.
                    g_capture_wait_release = true;
                    return;
                }
                if (g_capture_wait_release)
                {
                    g_capture_wait_release = any_down;
                    return;
                }
                if (pressed == 0 && mod_only != 0)
                {
                    pending_mod_only = mod_only;
                }
                const int take = pressed != 0 ? mm::key_make(pressed, held_modifier())
                                 : (!any_down && pending_mod_only != 0)
                                     ? mm::key_make(pending_mod_only, mm::kKeyModNone)
                                     : 0;
                if (take == 0)
                {
                    return;
                }
                pending_mod_only = 0;
                cfg.*kKeyBinds[g_capture_row].member = take;
                mm::logf(L"binding: {} = {}",
                         std::wstring(kKeyBinds[g_capture_row].key,
                                      kKeyBinds[g_capture_row].key +
                                          std::strlen(kKeyBinds[g_capture_row].key)),
                         mm::key_name(take));
                arm_capture(-1);
            }

            void keys_help(const gb::Table& game_binds)
            {
                text_disabled_wrapped(tr(S::KyHelpRebind));
                text_disabled_wrapped(tr(S::KyHelpTaken));
                // Only while the game's own bindings are still a guess: once they are read,
                // the clash column below is exact and needs no caveat.
                if (game_binds.valid)
                {
                    return;
                }
                text_disabled_wrapped(lang::fmt<S::KyNotReadYet, 1024>(game_binds.status[0] != '\0'
                                                                           ? game_binds.status
                                                                           : tr(S::KyNoPlayerInput))
                                          .c_str());
            }

            // The note column: at most one line, in the order that matters most to the
            // player - our own double binding, then the game's, then the unmodified twin.
            void keys_note(const gb::Table& game_binds, const char* clash, const char* game,
                           const char* twin)
            {
                ImGui::PushTextWrapPos(0.0f);
                if (clash != nullptr)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.72f, 0.35f, 1.0f});
                    ImGui::TextWrapped("%s", lang::fmt<S::KyAlso>(clash).c_str());
                    ImGui::PopStyleColor();
                }
                else if (game != nullptr)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.72f, 0.35f, 1.0f});
                    ImGui::TextWrapped("%s", game_binds.valid ? lang::fmt<S::KyGameUses>(game).c_str()
                                                              : lang::fmt<S::KyGameMayUse>(game).c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", lang::fmt<S::KyGameTip, 1024>(
                                                    tr(game_binds.valid ? S::KyReadFromGame : S::KyAGuess))
                                                    .c_str());
                    }
                }
                else if (twin != nullptr)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.80f, 0.80f, 0.55f, 1.0f});
                    ImGui::TextWrapped("%s", lang::fmt<S::KySameKeyAs>(twin).c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PopTextWrapPos();
            }

            // One binding: the action, the key button that arms the capture, reset, and
            // whatever has to be said about the key.
            void keys_row(mm::Config& cfg, const gb::Table& game_binds, int i)
            {
                const int vk = cfg.*kKeyBinds[i].member;
                // Two actions on one key both fire: named rather than prevented.
                const char* clash = nullptr;
                for (int j = 0; j < kKeyBindCount && clash == nullptr; ++j)
                {
                    if (j != i && vk != 0 && cfg.*kKeyBinds[j].member == vk)
                    {
                        clash = tr(kKeyBinds[j].label);
                    }
                }
                // The unmodified twin: `ctrl+m` and `m` are different bindings but the same
                // key press, because a no-modifier binding does not require the modifiers to
                // be up (mm::key_mod) - which is what keeps every hotkey alive while the
                // x-ray's Alt is held. Named, not prevented.
                const char* twin = nullptr;
                for (int j = 0; j < kKeyBindCount && twin == nullptr; ++j)
                {
                    const int other = cfg.*kKeyBinds[j].member;
                    if (j != i && vk != 0 && mm::key_vk(other) == mm::key_vk(vk) &&
                        mm::key_mod(other) != mm::key_mod(vk))
                    {
                        twin = tr(kKeyBinds[j].label);
                    }
                }
                // The live table wins whole: once the game has answered, a key it does NOT
                // list is genuinely free, whatever the fallback guess says.
                const std::string live_clash =
                    game_binds.valid ? live_bind_clash(game_binds, vk) : std::string{};
                const char* game =
                    game_binds.valid ? (live_clash.empty() ? nullptr : live_clash.c_str())
                                     : game_bind_clash(vk);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tr(kKeyBinds[i].label));

                ImGui::TableNextColumn();
                ImGui::PushID(i + 900);
                // The key's name is the label, and names change; `###key` is the id.
                const std::string shown =
                    (g_capture_row == i ? std::string(tr(S::KyPressAKey)) : key_display(vk)) + "###key";
                if (ImGui::Button(shown.c_str(),
                                  ImVec2{(std::max)(130.0f * g_chrome_scale, button_width(shown.c_str())), 0.0f}))
                {
                    arm_capture(g_capture_row == i ? -1 : i);
                }

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(vk == kKeyDefaults.*kKeyBinds[i].member);
                if (ImGui::SmallButton(lbl(S::KyReset).c_str()))
                {
                    cfg.*kKeyBinds[i].member = kKeyDefaults.*kKeyBinds[i].member;
                }
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                keys_note(game_binds, clash, game, twin);
                ImGui::PopID();
            }

            void keys_table(mm::Config& cfg, const gb::Table& game_binds)
            {
                if (ImGui::BeginTable("bindings", 4,
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerV))
                {
                    ImGui::TableSetupColumn(tr(S::KyAction));
                    ImGui::TableSetupColumn(tr(S::KyKey));
                    ImGui::TableSetupColumn("");
                    // The note is the only elastic column: it takes whatever the three fixed
                    // ones leave, and its text wraps inside that instead of pushing the table
                    // past the panel's right edge.
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < kKeyBindCount; ++i)
                    {
                        keys_row(cfg, game_binds, i);
                    }
                    ImGui::EndTable();
                }

                if (ImGui::Button(lbl(S::KyResetAll).c_str()))
                {
                    for (int i = 0; i < kKeyBindCount; ++i)
                    {
                        cfg.*kKeyBinds[i].member = kKeyDefaults.*kKeyBinds[i].member;
                    }
                    arm_capture(-1);
                }
            }

            // ONE CHORD EDITOR, and there are three of them: the x-ray's, the map's and this
            // panel's. The box is primed from the value in force, written back on Enter and
            // then re-printed from what the parser actually took, so a typo never looks
            // accepted. `lt` / `rt` null means BUTTONS ONLY - the map and the panel are
            // opened by buttons, so the parser's trigger answers are sunk and never echoed,
            // exactly as the config parser discards them.
            void chord_editor(const char* label, char* buf, int cap, bool& primed,
                              const std::string& live, std::uint16_t& mask, bool* lt, bool* rt)
            {
                if (!primed)
                {
                    ::strncpy_s(buf, static_cast<std::size_t>(cap), live.c_str(), _TRUNCATE);
                    primed = true;
                }
                ImGui::SetNextItemWidth(180.0f * g_chrome_scale);
                if (!ImGui::InputText(label, buf, static_cast<std::size_t>(cap),
                                      ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    return;
                }
                bool sunk_lt = false;
                bool sunk_rt = false;
                bool& use_lt = lt != nullptr ? *lt : sunk_lt;
                bool& use_rt = rt != nullptr ? *rt : sunk_rt;
                mm::set_pad_chord(buf, mask, use_lt, use_rt);
                ::strncpy_s(buf, static_cast<std::size_t>(cap),
                            wide_to_ascii(mm::pad_chord_name(mask, lt != nullptr && use_lt,
                                                             rt != nullptr && use_rt))
                                .c_str(),
                            _TRUNCATE);
            }

            void keys_gamepad(mm::Config& cfg)
            {
                ImGui::Checkbox(lbl(S::KyXrayOnPad).c_str(), &cfg.highlight_gamepad);
                static char chord[64]{};
                static bool chord_primed = false;
                const std::string live = wide_to_ascii(mm::pad_chord_name(
                    cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt));
                chord_editor(lbl(S::KyXrayChord).c_str(), chord, static_cast<int>(sizeof(chord)), chord_primed,
                             live, cfg.highlight_pad_mask, &cfg.highlight_pad_lt, &cfg.highlight_pad_rt);
                text_disabled_wrapped(lang::fmt<S::KyChordInForce, 1024>(live.c_str()).c_str());
                if (ImGui::SmallButton(lbl(S::KyResetChord).c_str()))
                {
                    cfg.highlight_pad_mask = kKeyDefaults.highlight_pad_mask;
                    cfg.highlight_pad_lt = kKeyDefaults.highlight_pad_lt;
                    cfg.highlight_pad_rt = kKeyDefaults.highlight_pad_rt;
                    chord_primed = false;
                }

                static char open_chord[64]{};
                static bool open_primed = false;
                const std::string open_live =
                    wide_to_ascii(mm::pad_chord_name(cfg.map_pad_open_chord, false, false));
                chord_editor(lbl(S::KyOpenTheMap).c_str(), open_chord, static_cast<int>(sizeof(open_chord)),
                             open_primed, open_live, cfg.map_pad_open_chord, nullptr, nullptr);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", lang::fmt<S::KyOpensTheMap>(open_live.c_str()).c_str());

                // The settings panel's own chord, so a pad-only player can reach this panel.
                static char panel_chord[64]{};
                static bool panel_primed = false;
                const std::string panel_live =
                    wide_to_ascii(mm::pad_chord_name(cfg.panel_pad_open_chord, false, false));
                chord_editor(lbl(S::KyOpenThisPanel).c_str(), panel_chord, static_cast<int>(sizeof(panel_chord)),
                             panel_primed, panel_live, cfg.panel_pad_open_chord, nullptr, nullptr);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", lang::fmt<S::KyOpensThisPanel>(panel_live.c_str()).c_str());
                text_disabled_wrapped(tr(S::KyPadMapFixed));
            }
        } // namespace

        void panel_keys(mm::Config& cfg)
        {
            keys_capture(cfg);
            const gb::Table& game_binds = live_binds();
            keys_help(game_binds);
            keys_table(cfg, game_binds);
            if (panel_section(lbl(S::Gamepad).c_str(), kSecGamepad))
            {
                keys_gamepad(cfg);
            }
        }

        //======================================================================
        // The Debug tab's readouts
        //======================================================================
        //
        // One block per question, in the order they are drawn. Nothing here decides
        // anything: every one reads live state and prints it, so they share no state and
        // take only what they print.
        namespace
        {
            // Where the overlay is now, and what the PREVIOUS session left in
            // wuchang_minimap_last_stage.txt. A non-terminal value there is the only
            // evidence surviving a death with UE4SS's log buffer unflushed, so it is called
            // out in colour.
            void debug_stage(const mm::Config& cfg)
            {
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
            }

            // The one-press recon dump: the four things that cannot be recovered from the
            // cooked assets (context/saveslot-and-teleport-research.md section 3). It calls
            // nothing and changes nothing - reflection lookups and raw reads only - and
            // writes one file the user can send back.
            void debug_recon()
            {
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
            }

            // The runtime navmesh dump. A button, not a binding: it scans engine memory and
            // writes JSON, which no player should trigger by leaning on a key. The module
            // ships disabled (navmesh_dump = 1 in the dev config) and the button says so.
            void debug_navmesh_dump()
            {
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
                    ImGui::TextDisabled("off - set navmesh_dump = 1 in the dev config and restart");
                }
            }

            void debug_markers(const mm::Config& cfg)
            {
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
            }

            void debug_fullmap_and_pad(const mm::Config& cfg)
            {
                ImGui::SeparatorText("Full map and gamepad");
                const pad::State gp = pad::state();
                char padmod[64]{};
                ::WideCharToMultiByte(CP_UTF8, 0, pad::module_name(), -1, padmod, sizeof(padmod) - 1,
                                      nullptr, nullptr);
                // Says whether anything is ASKING as well as what was found: with map_gamepad
                // off nothing polls, and "none" then means "not looked at".
                ImGui::Text("pad: %s (%s)   sticks %.2f,%.2f / %.2f,%.2f   triggers %.2f/%.2f",
                            gp.connected ? "connected"
                            : (cfg.map_gamepad || (cfg.highlight_enabled && cfg.highlight_gamepad))
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
            }

            // The block to screenshot when the labels are in the wrong place: the route, the
            // pinned offset and the age of the pose.
            void debug_xray_camera()
            {
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
            }

            // The two height cuts. The minimap's scale belongs to the published window, not
            // to the diagnostics block; the full map runs the same slicer with an unbounded
            // band overhead and (by default) an equalised ramp, so its numbers say nothing
            // about the minimap's and belong on their own line.
            void debug_slices(const mm::Config& cfg, const mm::Snapshot& snap)
            {
                const SliceView sv = slice_view();
                ImGui::Text("slice  %dx%d px x %d surface(s) @ %.4f px/uu   %.2f ms (peak %.2f)   "
                            "%llu update(s), %llu skipped, %llu unchanged",
                            g_slice_size,
                            g_slice_size,
                            g_slice_surfaces,
                            sv.px_per_uu,
                            g_slice_ms,
                            g_slice_ms_peak,
                            static_cast<unsigned long long>(g_slice_updates),
                            static_cast<unsigned long long>(g_slice_skipped),
                            static_cast<unsigned long long>(g_slice_unchanged));
                ImGui::Text("       feet Z %.0f (raw %.0f)   tol %.0f  band %.0f  ramp %.0f..%.0f "
                            "(p%.0f)   opaque %u / dim %u / faint %u   unreachable %s, %u px",
                            static_cast<double>(g_feet_z),
                            snap.z - static_cast<double>(cfg.player_z_offset),
                            static_cast<double>(cfg.floor_z_tolerance),
                            static_cast<double>(cfg.shade_above_band_uu),
                            static_cast<double>(g_slice_z_lo),
                            static_cast<double>(g_slice_z_hi),
                            static_cast<double>(cfg.shade_range_pct_lo),
                            g_slice_opaque,
                            g_slice_dim,
                            g_slice_faint,
                            mapdata::reachability_available()
                                ? srule::unreachable_name(cfg.map_unreachable)
                                : "n/a (asset has no reachability)",
                            g_slice_unreach);
                char ramp_kind[32] = "linear";
                if (cfg.shade_map_equalize)
                {
                    if (cfg.shade_map_clip > 0.0f)
                    {
                        (void)std::snprintf(ramp_kind, sizeof(ramp_kind), "equalised, cap %.0fx",
                                            static_cast<double>(cfg.shade_map_clip));
                    }
                    else
                    {
                        (void)std::snprintf(ramp_kind, sizeof(ramp_kind), "equalised, uncapped");
                    }
                }
                ImGui::Text("full map  ramp %.0f..%.0f (%s)   floor %u / below %u / above %u   "
                            "unreachable %u px   %.2f ms",
                            static_cast<double>(g_mslice_counts.z_lo),
                            static_cast<double>(g_mslice_counts.z_hi),
                            ramp_kind,
                            g_mslice_counts.opaque,
                            g_mslice_counts.dim,
                            g_mslice_counts.faint,
                            g_mslice_counts.unreachable,
                            g_mslice_ms);
            }

            void debug_game_state(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
            {
                ImGui::SeparatorText("Game state");
                if (!have_state)
                {
                    ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.4f, 1.0f}, "no game-state snapshot yet");
                    return;
                }
                const std::uint64_t age = ::GetTickCount64() - snap.stamp_ms;
                ImGui::Text("world  X %.1f  Y %.1f  Z %.1f   yaw %.1f deg", snap.x, snap.y, snap.z,
                            snap.yaw);
                ImGui::Text("uv     %.5f, %.5f   chapter '%s'",
                            g_last_mini.u,
                            g_last_mini.v,
                            g_last_mini.chapter.empty() ? "-" : g_last_mini.chapter.c_str());
                ImGui::Text("gameplay pawn %s   transition %s   state-ok age %llu ms",
                            snap.pawn_is_gameplay ? "yes" : "NO",
                            snap.transition ? "YES" : "no",
                            static_cast<unsigned long long>(
                                snap.state_ok_since_ms == 0 ? 0
                                                            : ::GetTickCount64() - snap.state_ok_since_ms));
                debug_slices(cfg, snap);
                ImGui::Text("pawn %s   pawn-view %s   menu %s   input %s   state age %llu ms",
                            snap.has_pawn ? "yes" : "no",
                            snap.is_pawn_view ? "yes" : "no",
                            snap.menu_open ? "OPEN" : "no",
                            snap.device == mm::InputDevice::Kbm   ? "keyboard and mouse"
                            : snap.device == mm::InputDevice::Pad ? "gamepad"
                                                                  : "unknown",
                            static_cast<unsigned long long>(age));
                ImGui::Text("widgets seen %u, visible in viewport %u   location via %s",
                            snap.widgets_seen,
                            snap.widgets_visible_in_viewport,
                            snap.loc_from_function ? "K2_GetActorLocation" : "RootComponent");
                // Menu hide/show latency: how long ago the game thread saw the state change,
                // and how many roots it re-tests per pump.
                char holder[128]{};
                ::WideCharToMultiByte(CP_UTF8, 0, snap.menu_holder, -1, holder, sizeof(holder) - 1,
                                      nullptr, nullptr);
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

            // Why the minimap is or is not on screen, and what it is drawn into. The reason
            // is recomputed from live state every frame - the show condition has no latch -
            // and every change to it is logged.
            void debug_visibility()
            {
                ImGui::Spacing();
                const char* reason = hide_reason_name(g_hide_reason);
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
                                        g_reason_since_ms == 0 ? 0
                                                               : ::GetTickCount64() - g_reason_since_ms));
                ImGui::Text("overlay targets %ux%u, %u of them, %s, composite %dx%d %s   ui scale %.2f",
                            g_width,
                            g_height,
                            kTargets,
                            wide_to_ascii(format_name(comp_format())).c_str(),
                            g_map.width,
                            g_map.height,
                            g_map.ready ? "ready" : "NOT ready",
                            static_cast<double>(g_ui_scale));
                ImGui::Text("presents %llu, resizes %llu",
                            static_cast<unsigned long long>(g_present_count.load()),
                            static_cast<unsigned long long>(g_resize_count.load()));
            }
        } // namespace

        void panel_debug(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            panel_dev_keys(cfg);
            debug_tuning(cfg);
            debug_found_profile(cfg);
            debug_stage(cfg);
            debug_recon();
            debug_navmesh_dump();
            draw_perf_table();
            debug_markers(cfg);
            debug_fullmap_and_pad(cfg);
            debug_xray_camera();
            debug_game_state(cfg, snap, have_state);
            debug_visibility();
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

            const Label reload = lbl(S::PnlReloadData);
            const Label reset = lbl(S::PnlResetDefaults);
            const Label reset_yes = lbl(S::PnlResetConfirm);
            const Label cancel = lbl(S::Cancel);
            float fw[3]{};
            int fn = 0;
            fw[fn++] = button_width(reload.c_str());
            if (confirm_reset)
            {
                fw[fn++] = button_width(reset_yes.c_str());
                fw[fn++] = button_width(cancel.c_str());
            }
            else
            {
                fw[fn++] = button_width(reset.c_str());
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
                    if (ImGui::BeginTabItem(lbl(S::TabOverview).c_str()))
                    {
                        panel_overview(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem(lbl(S::TabCategories).c_str()))
                    {
                        panel_categories(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem(lbl(S::TabMapTracker).c_str()))
                    {
                        panel_map_tracker(cfg, snap, have_state);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem(lbl(S::TabKeys).c_str()))
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
            if (ImGui::Button(reload.c_str()))
            {
                mm::g_reload_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", tr(S::PnlReloadTip));
            }
            (void)same_line_if_fits(button_width(confirm_reset ? reset_yes.c_str() : reset.c_str()));
            if (!confirm_reset)
            {
                if (ImGui::Button(reset.c_str()))
                {
                    confirm_reset = true;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", tr(S::PnlResetTip));
                }
            }
            else
            {
                if (ImGui::Button(reset_yes.c_str()))
                {
                    confirm_reset = false;
                    const bool was_on = cfg.mod_enabled;
                    cfg = mm::Config{};
                    // The master switch is not a preference: it has its own checkbox and
                    // its own log line.
                    cfg.mod_enabled = was_on;
                    mm::log(L"config: reset to the shipped defaults from the F2 panel");
                }
                (void)same_line_if_fits(button_width(cancel.c_str()));
                if (ImGui::Button(cancel.c_str()))
                {
                    confirm_reset = false;
                }
            }

            // The master switch. Unticking it stops nothing from here: it writes
            // mod_enabled = 0 into the config file and the loop thread's 1 Hz watcher
            // acts on it (modswitch.hpp), so the whole shutdown runs on the one thread
            // allowed to run it and the file cannot disagree with the running state.
            if (ImGui::Checkbox(lbl(S::PnlMasterSwitch).c_str(), &cfg.mod_enabled))
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
                const lang::Text<512> note = lang::fmt<S::PnlShuttingDown, 512>("config_wuchang_minimap.txt");
                ImGui::TextWrapped("%s", note.c_str());
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
