//
// overlay_fullmap - the pannable, zoomable full map.
//
// Owns the map window's pan/zoom state, its marker pass, its legend and its help text.
//

#include "overlay_internal.hpp"

#include "textmatch.hpp"
#include "imgui_caret.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // The full map's own render-thread state
        //==============================================================================
        namespace
        {
            char g_map_search[64]{};   // the marker-name filter; empty = no filter
            int g_map_search_hits = 0; // markers the filter kept, over the whole buffer
            bool g_search_panel = false;
            // The search box owns the caret: true from the frame the player clicks into
            // it until a click lands outside it and outside the results dropdown. While
            // it holds, a frame that finds no widget active hands the caret back - that
            // is what survives a click on a result row, which focuses the dropdown's
            // window and clears the active id.
            bool g_search_focus = false;
            bool g_wp_panel = false;
        } // namespace

        //==============================================================================
        // Drawing: the FULL MAP
        //==============================================================================
        //
        // Toggled by `map_key` (M). While it is open the minimap is hidden, a dark
        // backdrop covers the scene and one ImGui window holds the map: north-up,
        // pannable and zoomable, with every marker on it.
        //
        // Shared with the minimap: the asset (mapdata::HeightMaps, one ~327 MB copy in
        // RAM), the height-slice rule and its config (floor_z_tolerance and the shade_*
        // ramp) plus a floor OFFSET the player can nudge, and the marker draw buffer,
        // category mask and glyphs.
        //
        // Different: the height ramp's percentiles come out of one cut over the whole
        // visible map and are taken as measured, with none of the minimap's easing;
        // the cut is decimated (one texture pixel covers `step`
        // source pixels) and covers the visible viewport plus a 30 % margin; it re-cuts only
        // when something changed - a pan out of the cut region, a zoom, a floor change,
        // a big player move - capped at map_slice_hz; and it is always north-up.
        //
        // No latches: the map closes itself the moment the state that allows it stops
        // being true, and closing hands the mouse and keyboard back on the same frame,
        // because the swallow condition IS `g_map_open`.

        // The published found flag with the render side's own pending toggle applied.
        // An override is dropped as soon as the published buffer says the same thing,
        // so this can never latch: at worst it holds for one marker round (~1 s).
        bool marker_found_now(const markers::DrawMarker& m)
        {
            const bool published = (m.flags & markers::kFlagFound) != 0;
            for (std::size_t i = 0; i < g_found_override.size(); ++i)
            {
                if (g_found_override[i].first != m.id)
                {
                    continue;
                }
                if (g_found_override[i].second == published)
                {
                    g_found_override.erase(g_found_override.begin() + static_cast<std::ptrdiff_t>(i));
                    return published;
                }
                return g_found_override[i].second;
            }
            return published;
        }

        void toggle_found(const markers::DrawMarker& m)
        {
            if (m.id[0] == '\0')
            {
                return;
            }
            const bool want = !marker_found_now(m);
            for (auto& kv : g_found_override)
            {
                if (kv.first == m.id)
                {
                    kv.second = want;
                    markers::request_toggle_found(m.id, want);
                    return;
                }
            }
            if (g_found_override.size() < 512)
            {
                g_found_override.emplace_back(std::string{m.id}, want);
            }
            markers::request_toggle_found(m.id, want);
        }

        // The full map's texture size for a given canvas. Pure geometry, so both the
        // render thread (which allocates) and the loop thread (which cuts) can derive
        // the same numbers from the same request.
        void map_slice_size(const mm::Config& cfg, const mv::Rect& canvas, int& tw, int& th)
        {
            const double kMargin = kMapSliceMargin;
            const double zoom = g_mv.uu_per_px;
            const double want_w_uu = static_cast<double>(canvas.w()) * zoom * kMargin;
            const double want_h_uu = static_cast<double>(canvas.h()) * zoom * kMargin;

            tw = static_cast<int>(std::lround(static_cast<double>(canvas.w()) * kMargin));
            if (tw > cfg.map_slice_px)
            {
                tw = cfg.map_slice_px;
            }
            tw = (tw / 8) * 8;
            if (tw < 64)
            {
                tw = 64;
            }
            // One step for both axes (a non-square pixel would shear the picture), so
            // the height follows from it rather than from the aspect ratio directly.
            const double step = (want_w_uu * static_cast<double>(tw) > 0.0)
                                    ? (want_w_uu / static_cast<double>(tw))
                                    : 1.0;
            th = step > 0.0 ? static_cast<int>(std::lround(want_h_uu / step)) : 64;
            th = (th / 8) * 8;
            if (th < 64)
            {
                th = 64;
            }
            if (th > cfg.map_slice_px * 2)
            {
                th = (cfg.map_slice_px * 2 / 8) * 8;
            }
        }

        // RENDER THREAD. Size and allocate the full map's buffers and publish what the
        // slicer should cut. The cut runs on the loop thread - see slice_map_step().
        // Returns true while a buffer is available to draw.
        bool plan_map_slice(const mm::Config& cfg, const mapdata::Chapter& ch, const mv::Rect& canvas,
                            float feet, std::uint64_t now)
        {
            if (!ch.has_heights() || canvas.w() < 8.0f || canvas.h() < 8.0f)
            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
                return false;
            }

            int tw = 0;
            int th = 0;
            map_slice_size(cfg, canvas, tw, th);

            if (g_mslice[0].w != tw || g_mslice[0].h != th || g_mslice[0].tex == nullptr)
            {
                // A failing allocation must not retry (and log) once per frame.
                static std::uint64_t create_failed_ms = 0;
                if (create_failed_ms != 0 && now - create_failed_ms < 5000)
                {
                    return false;
                }
                if (!slicer_pause_begin(kSlicerPauseMs))
                {
                    return map_slice_view().shown >= 0; // try again next frame
                }
                wait_for_gpu(); // the old buffers may still be in flight
                const bool ok = create_slice_set(g_mslice, kMapSliceBufs, tw, th, L"full map");
                if (!ok)
                {
                    destroy_map_slice_buffers();
                }
                else
                {
                    for (int i = 0; i < kMapSliceBufs; ++i)
                    {
                        g_mslice_copy_pending[i].store(false);
                        g_mslice_in_flight[i].store(0);
                    }
                    g_mslice_next = 0;
                    clear_map_slice_view();
                    note_slice_buffers_changed();
                }
                slicer_pause_end();
                if (!ok)
                {
                    create_failed_ms = now;
                    return false;
                }
                create_failed_ms = 0;
            }

            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = true;
                g_map_req.cx = g_mv.cx;
                g_map_req.cy = g_mv.cy;
                g_map_req.zoom = g_mv.uu_per_px;
                g_map_req.canvas_w = canvas.w();
                g_map_req.canvas_h = canvas.h();
                g_map_req.feet = feet;
            }
            g_map_req_ms.store(now, std::memory_order_relaxed);
            return map_slice_view().shown >= 0;
        }

        // The config generation the standing cut was made with (mm::g_cfg_gen). Loop
        // thread only, and a plain int on purpose - no guarded function static on a path
        // the loop thread shares with the pump.
        std::uint32_t g_mr_cfg_gen = 0;

        // LOOP THREAD. Cut the visible region (plus a margin) into the map's own dynamic
        // texture, if anything changed and the next buffer is free.
        void slice_map_step(std::uint64_t now)
        {
            MapSliceReq req{};
            {
                spin::SpinGuard guard(g_slice_req_lock);
                req = g_map_req;
            }
            if (!req.wanted || now - g_map_req_ms.load(std::memory_order_relaxed) > 500)
            {
                return; // the map is closed, or the render thread stopped asking
            }

            mm::Snapshot snap{};
            if (!mm::read_snapshot(snap))
            {
                return;
            }
            // Re-read the planes on every cut: a chapter switch retires them and frees
            // them after a grace period. mapdata's retire runs on this same thread, so a
            // pointer read here cannot be freed while the cut is running.
            const mapdata::Chapter* chp = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (chp == nullptr || !chp->has_heights())
            {
                return;
            }
            const mapdata::Chapter& ch = *chp;
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                return;
            }

            if (g_map_recut.exchange(false, std::memory_order_acq_rel))
            {
                g_mr_valid = false; // the map was recentred: the old region says nothing
            }

            const mm::Config& cfg = mm::cfg_cached();
            // A config edit changes what the cut would paint, and the map re-cuts only
            // when the view or the player moves - so without this a slider dragged in the
            // F2 panel with the map open leaves the previous cut on screen.
            const std::uint32_t cfg_gen = mm::g_cfg_gen.load(std::memory_order_acquire);
            if (cfg_gen != g_mr_cfg_gen)
            {
                g_mr_cfg_gen = cfg_gen;
                g_mr_valid = false;
            }
            SliceBuf& b = g_mslice[g_mslice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0 || b.h <= 0)
            {
                return;
            }

            // The step the render thread's sizing implies, recomputed from the buffer
            // that actually exists.
            const double kMargin = kMapSliceMargin;
            const double want_w_uu = static_cast<double>(req.canvas_w) * req.zoom * kMargin;
            const double step = want_w_uu * hm.px_per_uu / static_cast<double>(b.w);
            if (!(step > 0.0))
            {
                return;
            }

            // The world rectangle this cut will cover, derived from the texture so the
            // drawn quad matches the pixels exactly.
            const double half_w = static_cast<double>(b.w) * step / hm.px_per_uu * 0.5;
            const double half_h = static_cast<double>(b.h) * step / hm.px_per_uu * 0.5;

            // Does the visible viewport still sit inside the region we already cut?
            const double view_half_x = static_cast<double>(req.canvas_h) * req.zoom * 0.5;
            const double view_half_y = static_cast<double>(req.canvas_w) * req.zoom * 0.5;
            const bool inside = g_mr_valid && req.cx - view_half_x >= g_mr_x0 &&
                                req.cx + view_half_x <= g_mr_x1 && req.cy - view_half_y >= g_mr_y0 &&
                                req.cy + view_half_y <= g_mr_y1;
            const bool feet_moved = !g_mr_valid || std::abs(req.feet - g_mr_feet) > 20.0f;
            const bool zoomed = !g_mr_valid || req.zoom != g_mr_zoom;
            const bool chapter_changed = g_mr_chapter != ch.key;
            const bool resized = g_mr_w != b.w || g_mr_h != b.h;
            const bool urgent = !inside || zoomed || chapter_changed || resized;
            if (!urgent && !feet_moved)
            {
                return;
            }

            // The rate cap. An urgent cut (the view left the region, the zoom changed)
            // waits for the buffer but not for the clock: an empty edge is worse than
            // one extra cut.
            const int period = cfg.map_slice_hz > 0 ? 1000 / cfg.map_slice_hz : 166;
            if (!urgent && g_mslice_last_ms != 0 &&
                now - g_mslice_last_ms < static_cast<std::uint64_t>(period))
            {
                return;
            }

            const std::uint64_t in_flight = g_mslice_in_flight[g_mslice_next].load(std::memory_order_acquire);
            ID3D12Fence* fence = g_fence;
            if (in_flight != 0 && fence != nullptr && fence->GetCompletedValue() < in_flight)
            {
                ++g_mslice_skipped;
                return; // the GPU is still sampling it; keep showing the other buffer
            }

            SliceStyle st = style_from(cfg);
            // The full map is a picture of a whole chapter, so it slices with an
            // UNBOUNDED band overhead: away from the player each pixel takes the ground
            // of the nearest storey at or above the feet. The player's own floor still
            // wins outright wherever it exists, so no ceiling covers them - the minimap's
            // one-storey band is what keeps a gallery off the window they are standing
            // in, and applying it to a chapter culls most of the chapter instead.
            if (st.above_band > 0.0f)
            {
                st.above_band = 1.0e9f; // show_adjacent_floors = 0 still means "my storey"
            }
            // Colour is equalised over this cut: `t` is the CDF of the drawn Z, so the
            // ramp is spent in proportion to the area at each height. A linear ramp over
            // ten kilometres of chapter puts every playable storey inside a tone or two.
            st.equalize = cfg.shade_map_equalize;

            const double x1 = req.cx + half_h; // north edge
            const double y0 = req.cy - half_w; // west edge
            const double src_x0 = (y0 - hm.min_y) * hm.px_per_uu;
            const double src_y0 = (hm.max_x - x1) * hm.px_per_uu;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            slice_region(hm, src_x0, src_y0, step, b.w, b.h, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, req.feet, st, g_mslice_scratch, g_mslice_counts,
                         nullptr, 0.0f);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            double g_mslice_last_cut_ms = 0.0;
            if (freq > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq);
                g_mslice_last_cut_ms = ms;
                g_mslice_ms = g_mslice_ms == 0.0 ? ms : g_mslice_ms * 0.7 + ms * 0.3;
                if (ms > g_mslice_ms_peak)
                {
                    g_mslice_ms_peak = ms;
                }
            }

            g_mr_x0 = req.cx - half_h;
            g_mr_x1 = x1;
            g_mr_y0 = y0;
            g_mr_y1 = req.cy + half_w;
            g_mr_zoom = req.zoom;
            g_mr_feet = req.feet;
            g_mr_chapter = ch.key;
            g_mr_w = b.w;
            g_mr_h = b.h;
            g_mr_valid = true;

            if (g_pf_mslice < 0)
            {
                g_pf_mslice = mm::perf_register("full map cut", perf::Thread::Loop);
            }
            mm::perf_record_ms(g_pf_mslice, g_mslice_last_cut_ms);

            g_mslice_copy_pending[g_mslice_next].store(true, std::memory_order_release);
            {
                spin::SpinGuard guard(g_slice_view_lock);
                g_mslice_view.shown = g_mslice_next;
                g_mslice_view.valid = true;
                g_mslice_view.x0 = g_mr_x0;
                g_mslice_view.x1 = g_mr_x1;
                g_mslice_view.y0 = g_mr_y0;
                g_mslice_view.y1 = g_mr_y1;
            }
            g_mslice_next = (g_mslice_next + 1) % kMapSliceBufs;
            g_mslice_last_ms = now;
            ++g_mslice_updates;
        }

        void draw_waypoint_glyph(ImDrawList* dl, ImVec2 p, float r, int alpha)
        {
            const ImU32 col = IM_COL32(255, 92, 210, alpha);
            // A 1 Hz pulse ring; the waypoint is never culled and never dimmed. The
            // phase comes from the wall clock, so the minimap, the full map and the
            // compass pulse together.
            {
                const float phase =
                    static_cast<float>(::GetTickCount64() % 1000) / 1000.0f;
                const float rad = r * (1.2f + 1.1f * phase);
                const int a = static_cast<int>(static_cast<float>(alpha) * 0.55f * (1.0f - phase));
                if (a > 3)
                {
                    dl->AddCircle(p, rad, IM_COL32(255, 92, 210, a), 18, 1.6f);
                }
            }
            const ImU32 edge = IM_COL32(20, 8, 18, static_cast<int>(alpha * 0.9f));
            const ImVec2 tip{p.x, p.y + r * 1.5f};
            const ImVec2 l{p.x - r * 0.75f, p.y + r * 0.25f};
            const ImVec2 rr{p.x + r * 0.75f, p.y + r * 0.25f};
            dl->AddTriangleFilled(l, rr, tip, col);
            dl->AddCircleFilled(ImVec2{p.x, p.y - r * 0.15f}, r * 0.85f, col, 14);
            dl->AddCircle(ImVec2{p.x, p.y - r * 0.15f}, r * 0.85f, edge, 14, 1.4f);
            dl->AddCircleFilled(ImVec2{p.x, p.y - r * 0.15f}, r * 0.3f, edge, 8);
        }

        // Everything the map mode owns, dropped. The panels belong to the map: left
        // open they would sit over the game with nothing swallowing the input, and the
        // search text left behind would filter the next open and eat its first Escape.
        // Called from close_map and, for a close the map itself never sees (the M key,
        // the pad chord, the HUD gate), from the render loop's edge.
        void reset_map_mode()
        {
            g_stats_page = false;
            g_shrine_panel = false;
            g_search_panel = false;
            g_search_focus = false;
            g_wp_panel = false;
            g_shot_canvas_valid = false;
            g_map_search[0] = '\0';
            g_map_search_hits = 0;
            g_map_search_active.store(false, std::memory_order_relaxed);
        }

        // Closes the map and says why, exactly once per transition.
        void close_map(const wchar_t* why)
        {
            if (!mm::g_map_open.exchange(false))
            {
                return;
            }
            reset_map_mode();
            MM_LOGV(L"full map closed: {}", why);
        }

        void draw_full_map(mm::Config cfg, const mm::Snapshot& snap, bool have_state, float ui_scale)
        {
            const mm::Config before = cfg;
            const std::uint64_t now = ::GetTickCount64();
            ImGuiIO& io = ImGui::GetIO();
            const ImGuiViewport* vp = ImGui::GetMainViewport();

            //--------------------------------------------------------------------------
            // The gate. Every condition is re-evaluated from the live snapshot on every
            // frame and closes the map outright - there is nothing here that can latch.
            //--------------------------------------------------------------------------
            if (!have_state || snap.stamp_ms == 0 ||
                now - snap.stamp_ms > static_cast<std::uint64_t>(cfg.state_stale_ms))
            {
                close_map(L"no fresh game-state snapshot");
                return;
            }
            if (snap.transition)
            {
                close_map(L"a level transition started");
                return;
            }
            if (!snap.has_pawn || !snap.pawn_is_gameplay)
            {
                close_map(L"there is no gameplay pawn");
                return;
            }
            if (cfg.hide_in_menus && snap.menu_open)
            {
                close_map(L"a game menu opened");
                return;
            }

            //--------------------------------------------------------------------------
            // Geometry and the backdrop
            //--------------------------------------------------------------------------
            const float margin = cfg.map_margin * vp->Size.y;
            const mv::Rect frame{vp->Pos.x + margin,
                                 vp->Pos.y + margin,
                                 vp->Pos.x + vp->Size.x - margin,
                                 vp->Pos.y + vp->Size.y - margin};

            ImDrawList* back = ImGui::GetBackgroundDrawList();
            back->AddRectFilled(vp->Pos,
                                ImVec2{vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y},
                                IM_COL32(3, 5, 8, static_cast<int>(cfg.map_backdrop * 255.0f + 0.5f)));

            const mapdata::Chapter* chapter_ptr = mapdata::chapter_ptr_for(snap.x, snap.y);
            // Copied once: the set is 520 bytes taken under the loop thread's spinlock,
            // and the header, the glyphs and the list all want the same frame's answer.
            const mv::WaypointSet wps = mm::waypoints();

            //--------------------------------------------------------------------------
            // First frame after opening: centre on the player, reset the zoom and the
            // floor offset, and drop any gamepad edges from while it was closed.
            //--------------------------------------------------------------------------
            // This view gets the UNSCALED config - the legend's filter chips write back
            // into it - so the ui-scale factor ui_scaled() applies to the minimap's zoom
            // key is applied here at the point of use. 1/ui_scale, not ui_scale: the
            // canvas is `ui_scale` times as many pixels across, so uu-per-pixel comes
            // down by the same factor to cover the same ground.
            const float zscale = (cfg.zoom_dpi_scaled && ui_scale > 0.0f) ? 1.0f / ui_scale : 1.0f;
            if (!g_mv_init)
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_mv.uu_per_px = mv::clamp_zoom(static_cast<double>(cfg.map_zoom * zscale),
                                                static_cast<double>(cfg.map_zoom_min * zscale),
                                                static_cast<double>(cfg.map_zoom_max * zscale));
                g_map_floor_off = 0.0f;
                g_map_recut.store(true, std::memory_order_release);
                g_mv_init = true;
                pad::clear_pressed();
                g_map_recenter.store(false, std::memory_order_relaxed);
            }

            ImGui::SetNextWindowPos(ImVec2{frame.x0, frame.y0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{frame.w(), frame.h()}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.97f);
            constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                ImGuiWindowFlags_NoSavedSettings |
                                                // It fills the screen, so it is the map mode's backdrop and
                                                // stays behind its own panels: without this, focusing it -
                                                // a click on the canvas, or the search box taking the caret
                                                // back - would draw it over the search dropdown.
                                                ImGuiWindowFlags_NoBringToFrontOnFocus;
            if (!ImGui::Begin("##wuchang_full_map", nullptr, kFlags))
            {
                ImGui::End();
                return;
            }

            //--------------------------------------------------------------------------
            // Header + the category filter (the SAME mask the minimap and the F2 panel
            // use, so a filter toggled here is toggled everywhere)
            //--------------------------------------------------------------------------
            ImGui::Text("Wuchang map");
            ImGui::SameLine();
            // The floor offset in metres, named as a storey delta. 1 uu = 1 cm.
            ImGui::TextDisabled("%s   |   %.0f uu/px   |   floor %+.1f m   |   X %.0f  Y %.0f",
                                chapter_ptr != nullptr ? chapter_ptr->key.c_str() : "no chapter here",
                                g_mv.uu_per_px,
                                static_cast<double>(g_map_floor_off) / 100.0,
                                snap.x,
                                snap.y);
            // The button group is measured from its own labels, so it ends flush with
            // the window's right edge at every width, font and UI scale, and drops to
            // its own line when the readout leaves it no room.
            {
                float bw[5]{};
                int bn = 0;
                bw[bn++] = button_width("Fit");
                bw[bn++] = button_width("Stats");
                bw[bn++] = button_width("Shrines");
                bw[bn++] = button_width("Recentre");
                bw[bn++] = button_width("Close");
                right_align_group(row_width(bw, bn), ImGui::GetCursorScreenPos().x);
            }
            // Zoom to fit, from the chapter's bounds in the manifest.
            bool want_fit = ImGui::SmallButton("Fit");
            ImGui::SameLine();
            if (ImGui::SmallButton("Stats"))
            {
                g_stats_page = !g_stats_page;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Shrines"))
            {
                g_shrine_panel = !g_shrine_panel;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Recentre"))
            {
                g_map_recenter.store(true, std::memory_order_relaxed);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Close"))
            {
                close_map(L"the Close button");
            }

            // THE NAME FILTER. Optional by design: everything on this map still works
            // with the box empty, which is what keeps a gamepad-only player whole.
            // Nothing else may hold the caret while a search is up. The results
            // dropdown never takes it on its own (NoFocusOnAppearing), and a click on
            // one of its rows gives it back here, on the next frame.
            const bool refocus = g_search_focus && !ImGui::IsAnyItemActive();
            if (refocus)
            {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth((std::min)(240.0f * g_chrome_scale,
                                               ImGui::GetContentRegionAvail().x));
            if (ImGui::InputTextWithHint("##mapsearch", "search marker names...", g_map_search,
                                         sizeof(g_map_search)))
            {
                g_search_panel = g_map_search[0] != '\0';
            }
            if (ImGui::IsItemActivated())
            {
                g_search_focus = true;
            }
            // Focus taken by code selects the whole box, and the next letter typed would
            // replace the query instead of extending it. The caret belongs at the end.
            if (refocus)
            {
                if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID()))
                {
                    st->SetSelection(st->TextLen, st->TextLen);
                }
            }
            // The dropdown hangs off this rect: same left edge, right under the box.
            const ImVec2 search_min = ImGui::GetItemRectMin();
            const ImVec2 search_max = ImGui::GetItemRectMax();
            const bool searching = g_map_search[0] != '\0';
            g_map_search_active.store(searching, std::memory_order_relaxed);

            //--------------------------------------------------------------------------
            // The search pass, over the WHOLE published buffer rather than the viewport,
            // so a name typed in is found wherever it is. Here rather than beside the
            // results window because the header below prints the count. The rows are
            // built and sorted only while the results window is up; with it closed this
            // is a count and nothing else.
            //--------------------------------------------------------------------------
            const markers::View mv_all = markers::view();
            static std::vector<const markers::DrawMarker*> hits;
            hits.clear();
            g_map_search_hits = 0;
            if (searching && cfg.markers_enabled && mv_all.data != nullptr)
            {
                const bool want_rows = g_search_panel;
                if (want_rows && hits.capacity() < mv_all.count)
                {
                    hits.reserve(mv_all.count);
                }
                for (std::size_t i = 0; i < mv_all.count; ++i)
                {
                    const markers::DrawMarker& m = mv_all.data[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                    if (static_cast<int>(m.cat) >= mdb::kCatCount ||
                        !mdb::cat_enabled(cfg.markers_categories, cat) ||
                        mdb::hidden_as_found(cat, marker_found_now(m), cfg.markers_hide_found) ||
                        !txt::contains_ci(mdb::display_label(cat, m.label), g_map_search))
                    {
                        continue;
                    }
                    ++g_map_search_hits;
                    if (want_rows)
                    {
                        hits.push_back(&m);
                    }
                }
                if (want_rows)
                {
                    const auto nearer_player = [&snap](const markers::DrawMarker* a,
                                                       const markers::DrawMarker* b) {
                        const double ax = a->x - snap.x;
                        const double ay = a->y - snap.y;
                        const double bx = b->x - snap.x;
                        const double by = b->y - snap.y;
                        return ax * ax + ay * ay < bx * bx + by * by;
                    };
                    std::sort(hits.begin(), hits.end(), nearer_player);
                }
            }
            // Every item of this row keeps the line only while it still fits; the rest
            // wrap onto the next one rather than run under the window's right edge.
            (void)same_line_if_fits(button_width("clear"));
            ImGui::BeginDisabled(!searching);
            if (ImGui::SmallButton("clear"))
            {
                g_map_search[0] = '\0';
                g_search_panel = false;
                g_search_focus = false;
            }
            ImGui::EndDisabled();
            if (searching)
            {
                char matches[64]{};
                (void)std::snprintf(matches, sizeof(matches), "%d match(es)   Esc clears",
                                    g_map_search_hits);
                (void)same_line_if_fits(ImGui::CalcTextSize(matches).x);
                ImGui::TextDisabled("%s", matches);
            }
            (void)same_line_if_fits(button_width("Waypoints"));
            if (ImGui::SmallButton("Waypoints"))
            {
                g_wp_panel = !g_wp_panel;
            }
            char wp_count[32]{};
            (void)std::snprintf(wp_count, sizeof(wp_count), "%zu set", wps.count);
            (void)same_line_if_fits(ImGui::CalcTextSize(wp_count).x);
            ImGui::TextDisabled("%s", wp_count);

            //--------------------------------------------------------------------------
            // The canvas, with the legend column reserved on its right
            //--------------------------------------------------------------------------
            //
            // The legend is the filter: it says what each glyph means, how many of that
            // category the chapter has and how many are found, and clicking a row
            // toggles it.
            const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            // Sized from the text, so it is right at every ui_scale, and capped at a
            // share of the row so a narrow window keeps a canvas instead of pushing the
            // legend past the right edge.
            const float legend_want =
                ImGui::CalcTextSize("      Fog gates   9999/9999").x + ImGui::GetStyle().FramePadding.x * 4.0f;
            const float legend_w = (std::max)(48.0f, (std::min)(legend_want, avail.x * 0.4f));
            const ImVec2 csize{(std::max)(64.0f, avail.x - legend_w - ImGui::GetStyle().ItemSpacing.x),
                               (std::max)(64.0f, avail.y - footer_h)};
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##canvas", csize, ImGuiButtonFlags_MouseButtonLeft);
            const bool canvas_hovered = ImGui::IsItemHovered();
            const bool canvas_active = ImGui::IsItemActive();
            const mv::Rect canvas{cpos.x, cpos.y, cpos.x + csize.x, cpos.y + csize.y};
            // What the screenshot key copies. Recorded every frame the map draws, on the
            // thread that reads it, and cleared by close_map, so a copy with the map shut
            // is a reportable failure rather than a stale picture.
            g_shot_canvas = canvas;
            g_shot_canvas_valid = true;

            //--------------------------------------------------------------------------
            // The legend, which IS the category filter
            //--------------------------------------------------------------------------
            //
            // One row per category: the glyph as drawn on the map, the name, and
            // `found / total` from markers::stats() - counted for the chapter in force
            // when the marker filter is on, so the total is not five chapters the player
            // cannot see. Clicking a row toggles that category in `markers_categories`,
            // the mask this map, the minimap and the F2 chips share. The compass has its
            // own `compass_categories`, so a row here does not move its pips.
            ImGui::SameLine();
            if (ImGui::BeginChild("##legend", ImVec2{legend_w, csize.y}, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoSavedSettings))
            {
                const markers::Stats lst = markers::stats();
                const int fch = lst.filter_chapter;
                const bool per_chapter = fch >= 0 && fch <= 8;
                ImGui::TextDisabled(per_chapter ? "legend - chapter" : "legend - all chapters");
                ImDrawList* ldl = ImGui::GetWindowDrawList();
                const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.34f);
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    const mdb::Cat cat = static_cast<mdb::Cat>(i);
                    const bool on = mdb::cat_enabled(cfg.markers_categories, cat);
                    const markers::CatStat& cs =
                        per_chapter ? lst.chapter[fch][i] : lst.cat[i];
                    const ImVec2 row = ImGui::GetCursorScreenPos();
                    ImGui::PushID(i);
                    // The leading spaces are the glyph's gutter: the glyph is drawn over
                    // the row afterwards, so the Selectable owns the whole width.
                    //
                    // Cached, keyed on exactly what the text is made of: otherwise these
                    // are fourteen std::format allocations per frame inside Present for
                    // text that changes when a marker is found or the chapter changes.
                    static char row_text[mdb::kCatCount][64]{};
                    static int row_found[mdb::kCatCount]{};
                    static int row_total[mdb::kCatCount]{};
                    static bool row_valid[mdb::kCatCount]{};
                    if (!row_valid[i] || row_found[i] != cs.found || row_total[i] != cs.total)
                    {
                        row_valid[i] = true;
                        row_found[i] = cs.found;
                        row_total[i] = cs.total;
                        if (cs.total > 0)
                        {
                            (void)std::snprintf(row_text[i], sizeof(row_text[i]), "      %s   %d/%d",
                                                mdb::cat_label(cat), cs.found, cs.total);
                        }
                        else
                        {
                            (void)std::snprintf(row_text[i], sizeof(row_text[i]), "      %s",
                                                mdb::cat_label(cat));
                        }
                    }
                    const char* const text = row_text[i];
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          on ? marker_color(cat, 255) : IM_COL32(150, 150, 150, 170));
                    if (ImGui::Selectable(text, on))
                    {
                        cfg.markers_categories ^= mdb::cat_bit(cat);
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    const ImVec2 at{row.x + glyph_r + 4.0f, row.y + ImGui::GetTextLineHeight() * 0.5f};
                    draw_marker_glyph(ldl, cat, at, glyph_r, marker_color(cat, on ? 255 : 90),
                                      IM_COL32(14, 16, 20, on ? 220 : 80));
                }
                ImGui::Spacing();
                if (ImGui::SmallButton("all"))
                {
                    cfg.markers_categories = mdb::kAllCats;
                }
                (void)same_line_if_fits(button_width("none"));
                if (ImGui::SmallButton("none"))
                {
                    cfg.markers_categories = 0u;
                }
                // A checkbox is the square plus the inner gap plus its label.
                (void)same_line_if_fits(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                        ImGui::CalcTextSize("found").x);
                bool show_found = !cfg.markers_hide_found;
                if (ImGui::Checkbox("found", &show_found))
                {
                    cfg.markers_hide_found = !show_found;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("show markers already found");
                }
            }
            ImGui::EndChild();

            const double zmin = static_cast<double>(cfg.map_zoom_min * zscale);
            const double zmax = static_cast<double>(cfg.map_zoom_max * zscale);
            g_mv.uu_per_px = mv::clamp_zoom(g_mv.uu_per_px, zmin, zmax);

            //--------------------------------------------------------------------------
            // Input: mouse
            //--------------------------------------------------------------------------
            static float drag_px = 0.0f;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canvas_hovered)
            {
                drag_px = 0.0f;
            }
            if (canvas_active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const ImVec2 d = io.MouseDelta;
                drag_px += std::abs(d.x) + std::abs(d.y);
                // Keep the world point under the cursor under the cursor.
                g_mv.cx += static_cast<double>(d.y) * g_mv.uu_per_px;
                g_mv.cy -= static_cast<double>(d.x) * g_mv.uu_per_px;
            }

            const auto zoom_about = [&](float sx, float sy, double notches) {
                double wx = 0.0;
                double wy = 0.0;
                mv::screen_to_world(g_mv, canvas, sx, sy, wx, wy);
                g_mv.uu_per_px =
                    mv::zoom_by(g_mv.uu_per_px, notches, static_cast<double>(cfg.map_zoom_factor), zmin, zmax);
                // The same transform read the other way round: solve for the centre that
                // puts (wx, wy) back on (sx, sy).
                g_mv.cx = wx + (static_cast<double>(sy) - static_cast<double>(canvas.cy())) * g_mv.uu_per_px;
                g_mv.cy = wy - (static_cast<double>(sx) - static_cast<double>(canvas.cx())) * g_mv.uu_per_px;
            };

            if (canvas_hovered && io.MouseWheel != 0.0f)
            {
                if (io.KeyCtrl)
                {
                    g_map_floor_off += io.MouseWheel * cfg.map_floor_step;
                }
                else
                {
                    zoom_about(io.MousePos.x, io.MousePos.y, static_cast<double>(io.MouseWheel));
                }
            }

            //--------------------------------------------------------------------------
            // Input: keyboard. ImGui sees these because the WndProc hook feeds it every
            // message BEFORE deciding to swallow it.
            //--------------------------------------------------------------------------
            const float dt = io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : 1.0f / 60.0f;
            const double pan_uu = static_cast<double>(cfg.map_pan_speed) * static_cast<double>(dt) * g_mv.uu_per_px;
            // Every bare key below belongs to the text box while it has the caret. The
            // map already swallows the keyboard from the game, so this is only about who
            // inside the map gets the letter.
            const bool typing = tgate::text_active();
            const auto down = [&typing](ImGuiKey a, ImGuiKey b) {
                return !typing && (ImGui::IsKeyDown(a) || ImGui::IsKeyDown(b));
            };
            if (down(ImGuiKey_W, ImGuiKey_UpArrow))
            {
                g_mv.cx += pan_uu; // screen up is world +X (north)
            }
            if (down(ImGuiKey_S, ImGuiKey_DownArrow))
            {
                g_mv.cx -= pan_uu;
            }
            if (down(ImGuiKey_D, ImGuiKey_RightArrow))
            {
                g_mv.cy += pan_uu; // screen right is world +Y (east)
            }
            if (down(ImGuiKey_A, ImGuiKey_LeftArrow))
            {
                g_mv.cy -= pan_uu;
            }
            if (down(ImGuiKey_Equal, ImGuiKey_KeypadAdd))
            {
                zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(dt) * 6.0);
            }
            if (down(ImGuiKey_Minus, ImGuiKey_KeypadSubtract))
            {
                zoom_about(canvas.cx(), canvas.cy(), -static_cast<double>(dt) * 6.0);
            }
            if (!typing && (ImGui::IsKeyPressed(ImGuiKey_E, true) || ImGui::IsKeyPressed(ImGuiKey_PageUp, true)))
            {
                g_map_floor_off += cfg.map_floor_step;
            }
            if (!typing && (ImGui::IsKeyPressed(ImGuiKey_Q, true) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true)))
            {
                g_map_floor_off -= cfg.map_floor_step;
            }
            // Esc empties the search box first, and closes the map only once it is empty.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                if (searching)
                {
                    g_map_search[0] = '\0';
                    g_search_panel = false;
                    g_search_focus = false;
                }
                else
                {
                    close_map(L"Escape");
                }
            }
            // Home = Fit, the same action as the header button. Safe as a bare key: the
            // full map swallows the keyboard for as long as it is open.
            if (!typing && ImGui::IsKeyPressed(ImGuiKey_Home, false))
            {
                want_fit = true;
            }
            // F1 / H (and pad Back, below) toggle the controls legend. Not `?`: that is
            // a CHARACTER with no portable name in ImGui's key enum, and testing
            // ImGuiKey_Slash means the unshifted key on every US/UK layout. Both are
            // safe bare keys because the map swallows the whole keyboard while it is
            // open, and F1 is outside the F6/F9-F12 minefield this machine's other
            // injected DLLs own. `/` stays wired as an unadvertised third route.
            if (!typing && (ImGui::IsKeyPressed(ImGuiKey_F1, false) || ImGui::IsKeyPressed(ImGuiKey_H, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Slash, false)))
            {
                g_map_help = !g_map_help;
            }
            // Keyboard equivalents of the two mouse actions, at the view centre: the
            // cursor is the one part of this that depends on what the game does with it
            // while we hold the input, and the map stays usable without it.
            bool key_waypoint = !typing && (ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                                            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
            bool key_toggle = !typing && ImGui::IsKeyPressed(ImGuiKey_F, false);

            //--------------------------------------------------------------------------
            // Input: gamepad. The state is polled on the LOOP thread (gamepad.cpp) - as
            // with the keyboard, nothing here touches XInput or the game thread.
            //--------------------------------------------------------------------------
            bool pad_waypoint = false;
            bool pad_toggle = false;
            const pad::State gp = pad::state();
            if (cfg.map_gamepad && gp.connected)
            {
                if (gp.lx != 0.0f || gp.ly != 0.0f)
                {
                    g_mv.cx += static_cast<double>(gp.ly) * pan_uu;
                    g_mv.cy += static_cast<double>(gp.lx) * pan_uu;
                }
                const float trig = gp.rt - gp.lt;
                if (trig > 0.02f || trig < -0.02f)
                {
                    zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(trig * dt) * 8.0);
                }
                if (gp.ry > 0.02f || gp.ry < -0.02f)
                {
                    zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(gp.ry * dt) * 6.0);
                }
                const std::uint16_t pressed = pad::take_pressed();
                if ((pressed & pad::kRightShoulder) != 0)
                {
                    g_map_floor_off += cfg.map_floor_step;
                }
                if ((pressed & pad::kLeftShoulder) != 0)
                {
                    g_map_floor_off -= cfg.map_floor_step;
                }
                if ((pressed & pad::kY) != 0)
                {
                    g_map_recenter.store(true, std::memory_order_relaxed);
                }
                if ((pressed & pad::kA) != 0)
                {
                    pad_waypoint = true;
                }
                if ((pressed & pad::kX) != 0)
                {
                    pad_toggle = true;
                }
                if ((pressed & pad::kB) != 0)
                {
                    close_map(L"the gamepad B button");
                }
                if ((pressed & pad::kBack) != 0)
                {
                    g_map_help = !g_map_help;
                }
            }

            if (g_map_recenter.exchange(false, std::memory_order_relaxed))
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_map_floor_off = 0.0f;
            }
            // Fit. The chapter's bounds come from maps.json, and mv::fit_zoom() spends
            // world X on the canvas HEIGHT and world Y on its WIDTH, because the map is
            // north-up; crossing them over is right on a square canvas only.
            if (want_fit)
            {
                if (chapter_ptr == nullptr)
                {
                    mm::log(L"full map: Fit needs a chapter, and none covers this position");
                }
                else
                {
                    const double z = mv::fit_zoom(chapter_ptr->max_x - chapter_ptr->min_x,
                                                  chapter_ptr->max_y - chapter_ptr->min_y,
                                                  static_cast<double>(csize.x),
                                                  static_cast<double>(csize.y));
                    if (z > 0.0)
                    {
                        g_mv.uu_per_px = mv::clamp_zoom(z, zmin, zmax);
                        g_mv.cx = (chapter_ptr->min_x + chapter_ptr->max_x) * 0.5;
                        g_mv.cy = (chapter_ptr->min_y + chapter_ptr->max_y) * 0.5;
                        g_map_floor_off = 0.0f;
                        g_map_recut.store(true, std::memory_order_release);
                    }
                }
            }
            g_map_floor_off = (std::max)(-20000.0f, (std::min)(20000.0f, g_map_floor_off));

            //--------------------------------------------------------------------------
            // The map picture
            //--------------------------------------------------------------------------
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1}, true);
            dl->AddRectFilled(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1},
                              IM_COL32(9, 12, 16, 255));

            const float feet = static_cast<float>(snap.z) - cfg.player_z_offset + g_map_floor_off;
            bool have_picture = false;
            const bool map_slice_ok =
                chapter_ptr != nullptr && plan_map_slice(cfg, *chapter_ptr, canvas, feet, now);
            const MapSliceView msv = map_slice_view();
            if (map_slice_ok && msv.shown >= 0 && msv.valid)
            {
                const SliceBuf& b = g_mslice[msv.shown];
                float sx0 = 0.0f;
                float sy0 = 0.0f;
                float sx1 = 0.0f;
                float sy1 = 0.0f;
                // Top-left of the cut region is its NORTH-WEST corner (max X, min Y).
                mv::world_to_screen(g_mv, canvas, msv.x1, msv.y0, sx0, sy0);
                mv::world_to_screen(g_mv, canvas, msv.x0, msv.y1, sx1, sy1);
                const ImTextureRef tex{static_cast<ImTextureID>(b.srv_gpu.ptr)};
                dl->AddImage(tex, ImVec2{sx0, sy0}, ImVec2{sx1, sy1}, ImVec2{0.0f, 0.0f}, ImVec2{1.0f, 1.0f},
                             IM_COL32(255, 255, 255, 255));
                have_picture = true;
            }

            //--------------------------------------------------------------------------
            // Markers
            //--------------------------------------------------------------------------
            g_map_markers_drawn = 0;
            g_map_markers_total = static_cast<int>(mv_all.count);
            const markers::DrawMarker* hover = nullptr;
            const markers::DrawMarker* centre_marker = nullptr;
            float hover_d2 = 0.0f;
            float centre_d2 = 0.0f;
            // The one pixel key this view owns. `cfg` here is the UNSCALED config (the
            // filter chips write back into it), so the UI scale is applied at the point
            // of use rather than through ui_scaled().
            const float mr = cfg.map_marker_size * ui_scale;
            const float pick_r = (std::max)(8.0f, mr * 1.6f);
            const float kCentrePickR = (std::max)(48.0f, mr * 5.0f);

            if (cfg.markers_enabled && mv_all.data != nullptr)
            {
                //----------------------------------------------------------------------
                // TWO PASSES, NOT ONE
                //----------------------------------------------------------------------
                //
                // Collect what is on screen, order it by distance from the VIEW CENTRE
                // (what the player is looking at, and what Fit and the recentre key
                // aim), cap that, merge coincident glyphs of the same category, and only
                // then draw - so the cap drops the far markers rather than whichever the
                // database listed last. All three buffers are static and reused, because
                // this runs inside Present.
                struct MapCand
                {
                    float sx = 0.0f;
                    float sy = 0.0f;
                    float cd2 = 0.0f; // squared distance from the canvas centre
                    float dz = 0.0f;  // marker Z minus the player's Z, uu (signed)
                    const markers::DrawMarker* m = nullptr;
                    bool found = false;
                    int count = 1;
                };
                static std::vector<MapCand> cands;
                cands.clear();
                if (cands.capacity() < mv_all.count)
                {
                    cands.reserve(mv_all.count);
                }
                for (std::size_t i = 0; i < mv_all.count; ++i)
                {
                    const markers::DrawMarker& m = mv_all.data[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                    if (static_cast<int>(m.cat) >= mdb::kCatCount ||
                        !mdb::cat_enabled(cfg.markers_categories, cat))
                    {
                        continue;
                    }
                    const bool found = marker_found_now(m);
                    if (mdb::hidden_as_found(cat, found, cfg.markers_hide_found))
                    {
                        continue;
                    }
                    if (searching && !txt::contains_ci(mdb::display_label(cat, m.label), g_map_search))
                    {
                        continue;
                    }
                    float sx = 0.0f;
                    float sy = 0.0f;
                    mv::world_to_screen(g_mv, canvas, m.x, m.y, sx, sy);
                    if (sx < canvas.x0 - mr || sx > canvas.x1 + mr || sy < canvas.y0 - mr || sy > canvas.y1 + mr)
                    {
                        continue;
                    }
                    MapCand c{};
                    c.sx = sx;
                    c.sy = sy;
                    const float cdx = sx - canvas.cx();
                    const float cdy = sy - canvas.cy();
                    c.cd2 = cdx * cdx + cdy * cdy;
                    // Against the PLAYER, not against the storey the slicer shows: the
                    // arrow answers "is this above or below me", so browsing floors with
                    // Q / E (g_map_floor_off, map_show_all_floors) must not flip it. Same
                    // reference the minimap and the compass use.
                    c.dz = static_cast<float>(m.z - snap.z);
                    c.m = &m;
                    c.found = found;
                    cands.push_back(c);
                }

                // Nearest the centre first; partial_sort leaves [0, cap) sorted, which
                // is all the draw order needs.
                const std::size_t cap = cfg.map_markers_max_draw > 0
                                            ? static_cast<std::size_t>(cfg.map_markers_max_draw)
                                            : cands.size();
                const auto nearer = [](const MapCand& a, const MapCand& b) { return a.cd2 < b.cd2; };
                if (cands.size() > cap)
                {
                    std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap),
                                      cands.end(), nearer);
                    cands.resize(cap);
                }
                else
                {
                    std::sort(cands.begin(), cands.end(), nearer);
                }

                // MERGE, same category and same found state only - see MergeGrid.
                {
                    static MergeGrid grid;
                    grid.reset(canvas.x0 - mr, canvas.y0 - mr, canvas.w() + mr * 2.0f,
                               canvas.h() + mr * 2.0f, (std::max)(3.0f, mr));
                    std::size_t kept = 0;
                    for (std::size_t i = 0; i < cands.size(); ++i)
                    {
                        const MapCand& c = cands[i];
                        const int key = grid.find(c.sx, c.sy, static_cast<int>(c.m->cat));
                        if (key >= 0 && cands[static_cast<std::size_t>(key)].found == c.found)
                        {
                            ++cands[static_cast<std::size_t>(key)].count;
                            continue;
                        }
                        grid.add(c.sx, c.sy, static_cast<int>(c.m->cat), static_cast<int>(kept));
                        cands[kept++] = c;
                    }
                    cands.resize(kept);
                }

                // Far to near, so the marker nearest what the player is looking at ends
                // up on top.
                for (std::size_t ci = cands.size(); ci-- > 0;)
                {
                    const MapCand& c = cands[ci];
                    const markers::DrawMarker& m = *c.m;
                    const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                    const bool hollow = mdb::drawn_as_found(cat, c.found);
                    const int alpha =
                        static_cast<int>((hollow ? cfg.markers_found_alpha : 1.0f) * 255.0f + 0.5f);
                    const ImU32 edge = IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f));
                    const ImVec2 p{c.sx, c.sy};
                    draw_marker_glyph(dl, cat, p, mr,
                                      marker_color_q(cat, m.rarity, alpha, cfg.markers_rarity_tint,
                                                     cfg.xray_rarity_colors),
                                      edge, hollow);
                    draw_count_badge(dl, p, mr, c.count, alpha);
                    // Above / below, the same rule the minimap and the compass use: a
                    // marker more than compass_pip_height_uu off the player's own Z gets
                    // an arrow beside its glyph. Within that band it counts as this floor
                    // and gets none. The arrow sits left of the glyph when a count badge
                    // already occupies the right.
                    const float thr = cfg.compass_pip_height_uu;
                    if (thr > 0.0f && (c.dz > thr || c.dz < -thr))
                    {
                        const float ar = (std::max)(2.5f, mr * 0.62f);
                        const float sgn = c.count > 1 ? -1.0f : 1.0f;
                        const float ax = p.x + sgn * (mr + ar * 0.9f);
                        const float up = c.dz > 0.0f ? -1.0f : 1.0f;
                        const ImVec2 tip{ax, p.y + up * ar};
                        const ImVec2 bl{ax - ar * 0.8f, p.y - up * ar * 0.55f};
                        const ImVec2 br{ax + ar * 0.8f, p.y - up * ar * 0.55f};
                        dl->AddTriangleFilled(tip, bl, br, IM_COL32(246, 246, 250, alpha));
                        dl->AddTriangle(tip, bl, br, edge, 1.0f);
                    }
                    ++g_map_markers_drawn;

                    const float mdx = c.sx - io.MousePos.x;
                    const float mdy = c.sy - io.MousePos.y;
                    const float d2 = mdx * mdx + mdy * mdy;
                    if (canvas_hovered && d2 <= pick_r * pick_r && (hover == nullptr || d2 < hover_d2))
                    {
                        hover = &m;
                        hover_d2 = d2;
                    }
                    // The keyboard / gamepad "toggle found" acts on the marker nearest
                    // the view centre, within the same radius a mouse would need.
                    if (c.cd2 <= kCentrePickR * kCentrePickR && (centre_marker == nullptr || c.cd2 < centre_d2))
                    {
                        centre_marker = &m;
                        centre_d2 = c.cd2;
                    }
                }
            }

            //--------------------------------------------------------------------------
            // The waypoint and the player
            //--------------------------------------------------------------------------
            draw_found_rings(dl, now, (std::max)(5.0f, mr * 1.4f),
                             [&](double wx, double wy, float& sx, float& sy) {
                                 mv::world_to_screen(g_mv, canvas, wx, wy, sx, sy);
                                 return canvas.contains(sx, sy);
                             });
            for (std::size_t wi = 0; wi < wps.count; ++wi)
            {
                float sx = 0.0f;
                float sy = 0.0f;
                mv::world_to_screen(g_mv, canvas, wps.items[wi].x, wps.items[wi].y, sx, sy);
                if (canvas.contains(sx, sy))
                {
                    // waypoint_size_scale sizes the waypoint everywhere it is drawn.
                    draw_waypoint_glyph(dl, ImVec2{sx, sy}, mr * 1.1f * cfg.waypoint_size_scale, 255);
                }
            }
            {
                float sx = 0.0f;
                float sy = 0.0f;
                mv::world_to_screen(g_mv, canvas, snap.x, snap.y, sx, sy);
                if (canvas.contains(sx, sy))
                {
                    // The view cone first, so the arrow sits on top of it.
                    const float a = snap.yaw * kPi / 180.0f;
                    const float len = 46.0f;
                    const float half = 32.0f * kPi / 180.0f;
                    const ImVec2 c{sx, sy};
                    const auto dir = [&](float ang) {
                        return ImVec2{c.x + std::sin(ang) * len, c.y - std::cos(ang) * len};
                    };
                    dl->AddTriangleFilled(c, dir(a - half), dir(a + half), IM_COL32(255, 226, 92, 46));
                    add_player_arrow(dl, c, snap.yaw, 11.0f);
                }
            }

            dl->PopClipRect();
            dl->AddRect(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1},
                        IM_COL32(120, 130, 145, 160), 0.0f, 0, 1.5f);

            //--------------------------------------------------------------------------
            // Clicks (after the draw, so `hover` is known)
            //--------------------------------------------------------------------------
            if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && drag_px < 5.0f &&
                hover != nullptr)
            {
                toggle_found(*hover);
            }
            // Drops a waypoint, or removes the one the gesture landed on.
            const auto set_or_remove = [&](double wx, double wy) {
                float cx2 = 0.0f;
                float cy2 = 0.0f;
                mv::world_to_screen(g_mv, canvas, wx, wy, cx2, cy2);
                for (std::size_t wi = 0; wi < wps.count; ++wi)
                {
                    float hx = 0.0f;
                    float hy = 0.0f;
                    mv::world_to_screen(g_mv, canvas, wps.items[wi].x, wps.items[wi].y, hx, hy);
                    const float ddx = hx - cx2;
                    const float ddy = hy - cy2;
                    if (ddx * ddx + ddy * ddy <= pick_r * pick_r)
                    {
                        mm::remove_waypoint(wi);
                        toast("waypoint removed");
                        return;
                    }
                }
                mv::Waypoint set{};
                set.set = true;
                set.x = wx;
                set.y = wy;
                set.z = static_cast<double>(feet);
                toast(mm::add_waypoint(set) ? "waypoint set" : "no room for another waypoint");
            };
            // Right-click on a marker: a waypoint on the marker's own spot, or off it.
            // The marker's z comes along, so the waypoint sits on the marker's floor and
            // the compass and the minimap bear on it exactly.
            const auto toggle_marker_waypoint = [&](const markers::DrawMarker& m) {
                const mv::WaypointToggleResult t =
                    mv::waypoint_toggle_at(wps, m.x, m.y, m.z, mv::kWaypointSamePlace);
                if (t.action == mv::WaypointToggle::Remove)
                {
                    mm::remove_waypoint(static_cast<std::size_t>(t.index));
                    toast("waypoint removed");
                    return;
                }
                if (t.action == mv::WaypointToggle::Full)
                {
                    toast("no room for another waypoint");
                    return;
                }
                mv::Waypoint wp{};
                wp.set = true;
                wp.x = m.x;
                wp.y = m.y;
                wp.z = m.z;
                toast(mm::add_waypoint(wp) ? "waypoint set" : "no room for another waypoint");
            };
            if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                if (hover != nullptr)
                {
                    toggle_marker_waypoint(*hover);
                }
                else
                {
                    double wx = 0.0;
                    double wy = 0.0;
                    mv::screen_to_world(g_mv, canvas, io.MousePos.x, io.MousePos.y, wx, wy);
                    set_or_remove(wx, wy);
                }
            }
            if (pad_waypoint || key_waypoint)
            {
                set_or_remove(g_mv.cx, g_mv.cy);
            }
            if ((pad_toggle || key_toggle) && centre_marker != nullptr)
            {
                toggle_found(*centre_marker);
            }

            //--------------------------------------------------------------------------
            // Hover tooltip
            //--------------------------------------------------------------------------
            if (hover != nullptr)
            {
                ImGui::BeginTooltip();
                const mdb::Cat cat = static_cast<mdb::Cat>(hover->cat);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(marker_color(cat, 255)), "%s",
                                   mdb::display_label(cat, hover->label));
                ImGui::Text("category: %s", mdb::cat_name(cat));
                // The stable id is a path (`Chapter1_DGong_logic/BP_treasurebox_C_12`),
                // the join key with the live actors and what a bug report needs - noise
                // to everyone else, hence the debug switch.
                if (cfg.debug_readout)
                {
                    ImGui::TextDisabled("%s", hover->id);
                }
                const double ddx = hover->x - snap.x;
                const double ddy = hover->y - snap.y;
                const double ddz = hover->z - snap.z;
                const double dz_m = ddz / 100.0;
                if (std::fabs(dz_m) < 0.5)
                {
                    ImGui::Text("%s   %.0f m away, same level",
                                marker_found_now(*hover) ? "FOUND" : "not found",
                                std::sqrt(ddx * ddx + ddy * ddy) / 100.0);
                }
                else
                {
                    ImGui::Text("%s   %.0f m away, %.0f m %s",
                                marker_found_now(*hover) ? "FOUND" : "not found",
                                std::sqrt(ddx * ddx + ddy * ddy) / 100.0, std::fabs(dz_m),
                                dz_m > 0.0 ? "above" : "below");
                }
                const bool hover_wp =
                    mv::waypoint_toggle_at(wps, hover->x, hover->y, hover->z, mv::kWaypointSamePlace)
                        .action == mv::WaypointToggle::Remove;
                ImGui::TextDisabled(hover_wp ? "left-click toggles found - right-click removes the waypoint"
                                             : "left-click toggles found - right-click toggles waypoint");
                ImGui::EndTooltip();
            }

            //--------------------------------------------------------------------------
            // Search results (while the box is not empty) - the rows collected above,
            // nearest first; clicking one waypoints it.
            //
            // A dropdown, not a window: it hangs under the search box, has no title bar
            // and no close button (Esc empties the box, which takes it away), and it
            // never takes the caret - NoFocusOnAppearing, or the first letter typed
            // would end the typing. It grows with its rows up to 40 % of the screen and
            // scrolls past that.
            //--------------------------------------------------------------------------
            {
                bool dropdown_hovered = false;
                if (searching && g_search_panel)
                {
                    const ImVec2 vpsz = ImGui::GetMainViewport()->Size;
                    const float drop_w = (std::max)(search_max.x - search_min.x, 420.0f * g_chrome_scale);
                    ImGui::SetNextWindowPos(ImVec2(search_min.x, search_max.y + 2.0f * g_chrome_scale));
                    ImGui::SetNextWindowSizeConstraints(ImVec2(drop_w, 0.0f), ImVec2(drop_w, vpsz.y * 0.40f));
                    constexpr ImGuiWindowFlags kDropFlags =
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNavFocus;
                    if (ImGui::Begin("##mapsearch_results", nullptr, kDropFlags))
                    {
                        dropdown_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                                                  ImGuiHoveredFlags_ChildWindows);
                        ImGui::TextDisabled("%d marker(s) match \"%s\" - click one to waypoint it, "
                                            "right-click to toggle",
                                            g_map_search_hits, g_map_search);
                        constexpr std::size_t kMaxRows = 200;
                        for (std::size_t i = 0; i < hits.size() && i < kMaxRows; ++i)
                        {
                            const markers::DrawMarker& m = *hits[i];
                            const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                            const double ddx = m.x - snap.x;
                            const double ddy = m.y - snap.y;
                            ImGui::PushID(static_cast<int>(i));
                            char row[128]{};
                            (void)std::snprintf(row, sizeof(row), "%s   %.0f m   (%s)",
                                                mdb::display_label(cat, m.label),
                                                std::sqrt(ddx * ddx + ddy * ddy) / 100.0,
                                                mdb::cat_name(cat));
                            if (ImGui::Selectable(row))
                            {
                                mv::Waypoint wp{};
                                wp.set = true;
                                wp.x = m.x;
                                wp.y = m.y;
                                wp.z = m.z;
                                toast(mm::add_waypoint(wp) ? "waypoint set"
                                                           : "no room for another waypoint");
                            }
                            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            {
                                toggle_marker_waypoint(m);
                            }
                            ImGui::PopID();
                        }
                        if (hits.size() > kMaxRows)
                        {
                            ImGui::TextDisabled("... and %zu more", hits.size() - kMaxRows);
                        }
                    }
                    ImGui::End();
                }
                // The caret stays in the box until the player takes it somewhere else:
                // a click on the box or on a result row keeps it, anything else - the
                // canvas, the legend, a header button - gives it up.
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    const ImVec2 mp = ImGui::GetMousePos();
                    const bool on_box = mp.x >= search_min.x && mp.x <= search_max.x &&
                                        mp.y >= search_min.y && mp.y <= search_max.y;
                    g_search_focus = on_box || dropdown_hovered;
                }
            }

            //--------------------------------------------------------------------------
            // The waypoint list (the `Waypoints` button in the header)
            //--------------------------------------------------------------------------
            if (g_wp_panel)
            {
                const ImVec2 vpsz = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vpsz.x * 0.5f, vpsz.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(460.0f * g_chrome_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Waypoints", &g_wp_panel,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    const mv::WaypointSet& live = wps;
                    {
                        char head[128]{};
                        (void)std::snprintf(head, sizeof(head),
                                            "%zu of %zu   -   right-click a marker or the map to "
                                            "toggle one",
                                            live.count, mv::kMaxWaypoints);
                        text_disabled_wrapped(head);
                    }
                    const int nearest = mv::nearest_waypoint(live, snap.x, snap.y);
                    for (std::size_t wi = 0; wi < live.count; ++wi)
                    {
                        const mv::Waypoint& wp = live.items[wi];
                        const double ddx = wp.x - snap.x;
                        const double ddy = wp.y - snap.y;
                        ImGui::PushID(static_cast<int>(wi));
                        if (ImGui::SmallButton("X"))
                        {
                            mm::remove_waypoint(wi);
                            ImGui::PopID();
                            break;
                        }
                        (void)same_line_if_fits(button_width("go to"));
                        if (ImGui::SmallButton("go to"))
                        {
                            g_mv.cx = wp.x;
                            g_mv.cy = wp.y;
                            g_map_recut.store(true, std::memory_order_release);
                        }
                        ImGui::SameLine();
                        // Wrapped: a coordinate line is longer than the window's default
                        // width, and the player can narrow the window further.
                        ImGui::TextWrapped("%zu.  %.0f m   X %.0f  Y %.0f  Z %.0f%s", wi + 1,
                                           std::sqrt(ddx * ddx + ddy * ddy) / 100.0, wp.x, wp.y, wp.z,
                                           static_cast<int>(wi) == nearest ? "   (nearest)" : "");
                        ImGui::PopID();
                    }
                    ImGui::Spacing();
                    ImGui::BeginDisabled(live.count == 0);
                    if (ImGui::Button("Clear all"))
                    {
                        mm::clear_waypoints();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Close"))
                    {
                        g_wp_panel = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The shrine list (the `Shrines` button in the header)
            //--------------------------------------------------------------------------
            if (g_shrine_panel)
            {
                const ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(620.0f * g_chrome_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Shrines", &g_shrine_panel,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    draw_shrine_list(snap, have_state, markers::stats().filter_chapter);
                    ImGui::Spacing();
                    if (ImGui::Button("Close"))
                    {
                        g_shrine_panel = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The collection statistics panel (the `Stats` button in the header)
            //--------------------------------------------------------------------------
            //
            // A child window over the map, not a top-level one: the map is a mode that
            // owns the screen and swallows the input, so a floating window would be a
            // trap. It closes with the same button, with Esc, and with the map.
            if (g_stats_page)
            {
                const ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(720.0f * g_chrome_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Collection", &g_stats_page,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    draw_collection_stats(::GetTickCount64(), false);
                    ImGui::Spacing();
                    if (ImGui::Button("Close"))
                    {
                        g_stats_page = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The controls legend (`?` / pad Back)
            //--------------------------------------------------------------------------
            //
            // Two columns, built from the config so a rebound key is what the player is
            // told, with the gamepad column present only while a pad is connected.
            if (g_map_help)
            {
                struct Row
                {
                    std::string control;
                    std::string action;
                };
                // Built once, not per frame: ~28 strings in two static vectors would
                // otherwise be ~30 allocations inside Present for text that changes only
                // when a binding changes or a pad is plugged in. The signature below is
                // exactly what the text is made of.
                struct HelpKey
                {
                    int panel = 0;
                    int map = 0;
                    int recenter = 0;
                    int zoom = 0;
                    int reload = 0;
                    int shot = 0;
                    int nearest = 0;
                    int highlight = 0;
                    bool hl_on = false;
                    bool hl_hold = false;
                    bool pad = false;
                    // Defaulted ==, not memcmp: padding bytes in an aggregate are
                    // unspecified, and a spurious "changed" puts the per-frame
                    // allocations back silently.
                    bool operator==(const HelpKey&) const = default;
                };
                const HelpKey want{cfg.panel_key,          cfg.map_key,
                                   cfg.map_recenter_key,   cfg.zoom_key,
                                   cfg.reload_key,         cfg.screenshot_key,
                                   cfg.waypoint_nearest_key, cfg.highlight_key,
                                   cfg.highlight_enabled,
                                   cfg.highlight_mode == mm::HighlightMode::Hold,
                                   cfg.map_gamepad && gp.connected};
                static std::vector<Row> left;
                static std::vector<Row> right;
                static HelpKey have{};
                static bool built = false;
                const bool rebuild = !built || !(want == have);
                const auto add = [](std::vector<Row>& into, std::string c, std::string a) {
                    into.push_back(Row{std::move(c), std::move(a)});
                };
                if (rebuild)
                {
                built = true;
                have = want;
                left.clear();
                right.clear();
                add(left, "mouse / keyboard", "");
                add(left, "drag, WASD, arrows", "pan");
                add(left, "wheel, + / -", "zoom");
                add(left, "ctrl+wheel, Q / E", "floor down / up");
                add(left, "Home", "zoom to fit the chapter");
                add(left, key_name_ascii(cfg.map_recenter_key), "recentre on the player");
                add(left, "right-click a marker", "waypoint it (again on it removes it)");
                add(left, "right-click, Space", "drop a waypoint on the spot (again removes it)");
                add(left, "the search box", "show only markers whose name matches");
                add(left, "Waypoints", "the waypoint list (remove one, or all)");
                add(left, "left-click, F", "toggle found");
                add(left, "click a legend row", "filter that category (remembered)");
                add(left, key_name_ascii(cfg.screenshot_key),
                    "copy the map to the clipboard (keyboard)");
                add(left, "Shrines", "shrine list (click = waypoint, double-click = centre)");
                add(left, "Stats", "collection statistics");
                add(left, "F1 or H", "this legend");
                add(left, key_name_ascii(cfg.map_key) + ", Esc", "close the map");
                if (cfg.map_gamepad && gp.connected)
                {
                    add(right, "gamepad", "");
                    add(right, "left stick", "pan");
                    add(right, "triggers, right stick", "zoom");
                    add(right, "LB / RB", "floor down / up");
                    add(right, "A", "drop a waypoint (again on it removes it)");
                    add(right, "X", "toggle found");
                    add(right, "Y", "recentre");
                    add(right, "Back", "this legend");
                    add(right, "B", "close the map");
                    add(right, "", "");
                }
                add(right, "outside the map (keyboard)", "");
                add(right, key_name_ascii(cfg.panel_key), "settings panel");
                add(right, key_name_ascii(cfg.zoom_key), "cycle the minimap zoom");
                add(right, key_name_ascii(cfg.reload_key), "reload config, maps and markers");
                // Unbound by default (the game owns G), so the row is there only once
                // the player has bound it.
                if (mm::key_vk(cfg.waypoint_nearest_key) != 0)
                {
                    add(right, key_name_ascii(cfg.waypoint_nearest_key),
                        "waypoint the nearest unfound marker");
                }
                if (cfg.highlight_enabled)
                {
                    add(right,
                        (cfg.highlight_mode == mm::HighlightMode::Hold ? "hold " : "press ") +
                            key_name_ascii(cfg.highlight_key),
                        "x-ray nearby markers");
                }
                if (cfg.map_gamepad && cfg.map_pad_open_chord != 0)
                {
                    add(right, wide_to_ascii(mm::pad_chord_name(cfg.map_pad_open_chord, false, false)),
                        "open / close the map (pad)");
                }
                } // rebuild

                const float line = ImGui::GetTextLineHeightWithSpacing();
                const float pad_px = ImGui::GetTextLineHeight();
                float ctrl_w[2] = {0.0f, 0.0f};
                float act_w[2] = {0.0f, 0.0f};
                const std::vector<Row>* cols[2] = {&left, &right};
                for (int c = 0; c < 2; ++c)
                {
                    for (const Row& row : *cols[c])
                    {
                        ctrl_w[c] = (std::max)(ctrl_w[c], ImGui::CalcTextSize(row.control.c_str()).x);
                        act_w[c] = (std::max)(act_w[c], ImGui::CalcTextSize(row.action.c_str()).x);
                    }
                }
                const float gap = pad_px;
                const float col_w[2] = {ctrl_w[0] + gap + act_w[0], ctrl_w[1] + gap + act_w[1]};
                const float box_w = col_w[0] + col_w[1] + pad_px * 3.0f;
                const std::size_t rows =
                    (std::max)(left.size(), right.size());
                const float box_h = static_cast<float>(rows) * line + pad_px * 2.0f;
                const ImVec2 tl{canvas.cx() - box_w * 0.5f, canvas.cy() - box_h * 0.5f};
                dl->AddRectFilled(tl, ImVec2{tl.x + box_w, tl.y + box_h}, plate_color(235), 5.0f);
                dl->AddRect(tl, ImVec2{tl.x + box_w, tl.y + box_h}, IM_COL32(150, 158, 168, 200), 5.0f, 0,
                            1.4f);
                for (int c = 0; c < 2; ++c)
                {
                    const float x = tl.x + pad_px + (c == 1 ? col_w[0] + pad_px : 0.0f);
                    float y = tl.y + pad_px;
                    for (const Row& row : *cols[c])
                    {
                        // A row with no action is a heading, and is the only thing in
                        // here drawn bright.
                        const bool heading = row.action.empty();
                        if (!row.control.empty())
                        {
                            dl->AddText(ImVec2{x, y},
                                        heading ? IM_COL32(255, 226, 160, 255) : IM_COL32(226, 230, 236, 235),
                                        row.control.c_str());
                        }
                        if (!heading)
                        {
                            dl->AddText(ImVec2{x + ctrl_w[c] + gap, y}, IM_COL32(180, 186, 196, 220),
                                        row.action.c_str());
                        }
                        y += line;
                    }
                }
            }

            //--------------------------------------------------------------------------
            // Footer
            //--------------------------------------------------------------------------
            if (!have_picture)
            {
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f},
                                   chapter_ptr == nullptr
                                       ? "no chapter covers this position - markers only"
                                       : "no height maps for this chapter - markers only");
            }
            else
            {
                ImGui::TextDisabled("F1 or H (or pad Back) shows the controls   %s or Esc closes the map",
                                    key_name_ascii(cfg.map_key).c_str());
            }
            ImGui::TextDisabled("%d of %d marker(s)   cut %dx%d @ %.2f ms%s", g_map_markers_drawn,
                                g_map_markers_total, g_mslice[0].w, g_mslice[0].h, g_mslice_ms,
                                cfg.map_gamepad && gp.connected ? "   gamepad connected" : "");

            ImGui::End();

            // Field by field, not memcmp: a Config is a value, and `mm::operator==` is
            // generated from the struct with a byte-flip drift guard in markers_test.
            if (before != cfg)
            {
                mm::set_config(cfg);
                mm::g_save_config_soon = true;
            }
        }
    } // namespace ovl
} // namespace overlay
