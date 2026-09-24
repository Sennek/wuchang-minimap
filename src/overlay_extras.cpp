//
// overlay_extras - the shrine list and the collection statistics page.
//
// Both are full-screen reading surfaces drawn over the map rather than parts of the HUD.
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // The shrine list
        //==============================================================================
        //
        // Every shrine of the current chapter with its in-game name, its distance and
        // whether the save has lit it. Sorted by distance; the chapter filter follows
        // the marker filter.
        //
        // Render thread only. Sources are published snapshots: shr::table()
        // (markers/shrines.json), shr::state() (the save's UnlockedFirepoints, read raw
        // at 1 Hz) and snap (the pawn position).

        void draw_shrine_list(const mm::Snapshot& snap, bool have_state, int filter_chapter)
        {
            const std::vector<shdb::Shrine>* table = shr::table();
            if (table == nullptr || table->empty())
            {
                const shr::TableInfo info = shr::table_info();
                ImGui::TextDisabled("%s", lang::fmt<S::ShNoTable, 512>(info.error[0] != '\0' ? info.error
                                                                                       : tr(S::ShTableEmpty))
                                              .c_str());
                return;
            }
            const shr::State st = shr::state();

            struct Row
            {
                const shdb::Shrine* s;
                double dist;
                bool unlocked;
            };
            // Reused frame to frame instead of reallocated at frame rate. Render thread
            // only, like every other static in this file.
            static std::vector<Row> rows;
            rows.clear();
            rows.reserve(table->size());
            for (const shdb::Shrine& sh : *table)
            {
                if (!sh.shrine || !sh.has_pos)
                {
                    continue; // a bossdoor_/Task pseudo-row is not a place
                }
                if (filter_chapter != chid::kNone && sh.chapter >= 0 && sh.chapter != filter_chapter)
                {
                    continue;
                }
                double d = -1.0;
                if (have_state)
                {
                    const double dx = sh.x - snap.x;
                    const double dy = sh.y - snap.y;
                    const double dz = sh.z - snap.z;
                    d = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                rows.push_back(Row{&sh, d, st.valid && shr::is_unlocked(sh.id.c_str())});
            }
            if (rows.empty())
            {
                ImGui::TextDisabled("%s", tr(S::ShNoneHere));
                return;
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if ((a.dist < 0.0) != (b.dist < 0.0))
                {
                    return b.dist < 0.0;
                }
                if (a.dist != b.dist)
                {
                    return a.dist < b.dist;
                }
                return a.s->id < b.s->id;
            });

            if (!st.valid)
            {
                ImGui::TextDisabled("%s", lang::fmt<S::ShUnlockedNa>(st.route[0] != '\0' ? st.route
                                                                                     : tr(S::NotReadYet))
                                              .c_str());
            }

            if (!ImGui::BeginTable("shrines", 4,
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f)))
            {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr(S::WordShrine));
            ImGui::TableSetupColumn(tr(S::ShColChapter), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(tr(S::ShColDistance), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(tr(S::ShColLit), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const Row& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const bool selected = ::strcmp(g_shrine_selected, r.s->id.c_str()) == 0;
                // One click sets a waypoint, a double-click centres the map. Selectable,
                // so the whole row is the hit target.
                if (ImGui::Selectable(r.s->label().c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick))
                {
                    ::strncpy_s(g_shrine_selected, sizeof(g_shrine_selected), r.s->id.c_str(),
                                _TRUNCATE);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        g_mv.cx = r.s->x;
                        g_mv.cy = r.s->y;
                        // The height slice is cut around the view centre, so a jump must
                        // invalidate it or the map draws the old storey at the new place
                        // until the next scheduled cut.
                        g_map_recut.store(true, std::memory_order_release);
                        toast(tr(S::TsCentredOnShrine));
                    }
                    else
                    {
                        mv::Waypoint wp{};
                        wp.set = true;
                        wp.x = r.s->x;
                        wp.y = r.s->y;
                        wp.z = r.s->z;
                        toast(tr(mm::add_waypoint(wp) ? S::TsWaypointOnShrine : S::TsWaypointFull));
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    const lang::Text<512> tip = lang::fmt<S::ShRowTip, 512>(r.s->label().c_str(), r.s->id.c_str());
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::TableNextColumn();
                if (r.s->chapter < 0)
                {
                    ImGui::TextDisabled("-");
                }
                else if (r.s->chapter == 0)
                {
                    ImGui::TextUnformatted(tr(S::Dlc));
                }
                else
                {
                    ImGui::Text("%d", r.s->chapter);
                }
                ImGui::TableNextColumn();
                if (r.dist < 0.0)
                {
                    ImGui::TextDisabled("-");
                }
                else
                {
                    ImGui::TextUnformatted(distance_text(r.dist / 100.0).c_str());
                }
                ImGui::TableNextColumn();
                if (!st.valid)
                {
                    ImGui::TextDisabled("%s", tr(S::NotAvailable));
                }
                else if (r.unlocked)
                {
                    ImGui::TextColored(ImVec4{0.55f, 0.85f, 0.55f, 1.0f}, "%s", tr(S::Yes));
                }
                else
                {
                    ImGui::TextDisabled("%s", tr(S::No));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        //==============================================================================
        // The collection statistics page
        //==============================================================================
        //
        // Found/total for every chapter and every category, the shrines the save has
        // lit, and one overall percentage.
        //
        // Render thread only, and it asks the game nothing. Both sources are published
        // snapshots - `markers::stats()` (a spinlock and a ~1.5 KB copy) and
        // `shr::state()` (~6.5 KB) - and their numbers change once per sweep round, so
        // the cache refreshes when `markers::rounds()` moves, with a 1 s floor for when
        // the live sweep is off and rounds never advance.

        //======================================================================
        // The collection statistics, block by block
        //======================================================================
        namespace
        {
            void stats_headline(const markers::Stats& st)
            {
                int found_all = 0;
                int total_all = 0;
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    found_all += st.cat[i].found;
                    total_all += st.cat[i].total;
                }
                const float pct = total_all > 0 ? 100.0f * static_cast<float>(found_all) /
                                                      static_cast<float>(total_all)
                                                : 0.0f;
                ImGui::TextUnformatted(lang::fmt<S::StCollected>(found_all, total_all).c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", lang::fmt<S::StOfEveryChapter>(static_cast<double>(pct)).c_str());
                ImGui::ProgressBar(total_all > 0 ? static_cast<float>(found_all) /
                                                       static_cast<float>(total_all)
                                                 : 0.0f,
                                   ImVec2(-1.0f, ImGui::GetTextLineHeight()));
            }

            // `UnlockedFirepoints` also holds boss-door and task pseudo-points, so only ids
            // that join to a shrine marker in the static DB are counted; the raw list length
            // is shown beside it so a bad join is visible.
            void stats_shrines(const StatsCache& c, const markers::Stats& st)
            {
                const int shrine_total = st.cat[static_cast<int>(mdb::Cat::Shrine)].total;
                if (!c.shrines.valid)
                {
                    ImGui::TextDisabled("%s", lang::fmt<S::StShrinesLitNa>(c.shrines.route[0] != '\0'
                                                                               ? c.shrines.route
                                                                               : tr(S::NotReadYet))
                                                  .c_str());
                    return;
                }
                int lit = 0;
                const markers::View view = markers::view();
                for (int i = 0; i < c.shrines.id_count; ++i)
                {
                    for (std::size_t m = 0; m < view.count; ++m)
                    {
                        if (view.data[m].cat != static_cast<std::uint8_t>(mdb::Cat::Shrine))
                        {
                            continue;
                        }
                        if (::_stricmp(view.data[m].id, c.shrines.ids[i]) == 0)
                        {
                            ++lit;
                            break;
                        }
                    }
                }
                ImGui::TextUnformatted(lang::fmt<S::StShrinesLit>(lit).c_str());
                // Both trailers keep the line only while they fit; a narrowed window drops
                // them onto the next one rather than clipping them.
                const lang::Text<512> note = lang::fmt<S::StShrinesNote, 512>(
                    c.shrines.unlocked, shrine_total, c.shrines.truncated ? tr(S::StTruncated) : "");
                (void)same_line_if_fits(ImGui::CalcTextSize(note.c_str()).x);
                ImGui::TextDisabled("%s", note.c_str());
                if (c.shrines.current[0] != '\0')
                {
                    const lang::Text<256> rested = lang::fmt<S::StLastRested>(c.shrines.current);
                    (void)same_line_if_fits(ImGui::CalcTextSize(rested.c_str()).x);
                    ImGui::TextDisabled("%s", rested.c_str());
                }
            }

            void stats_by_category(const markers::Stats& st)
            {
                ImGui::Spacing();
                if (!ImGui::BeginTable("stats_cat", 4,
                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_BordersInnerV))
                {
                    return;
                }
                ImGui::TableSetupColumn(tr(S::CgCategory));
                ImGui::TableSetupColumn(tr(S::StColFound));
                ImGui::TableSetupColumn(tr(S::StColTotal));
                ImGui::TableSetupColumn("%");
                ImGui::TableHeadersRow();
                for (const StatsGroup& grp : kStatsGroups)
                {
                    const markers::CatStat cs = group_stat(st.cat, grp.cats);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(tr(grp.label));
                    ImGui::TableNextColumn();
                    if (cs.total == 0)
                    {
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        continue;
                    }
                    ImGui::Text("%d", cs.found);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", cs.total);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%",
                                100.0 * static_cast<double>(cs.found) / static_cast<double>(cs.total));
                }
                ImGui::EndTable();
            }

            // The six collectable groups only - the full 25-column enum scrolls sideways
            // inside a 1080p panel.
            void stats_by_chapter(const markers::Stats& st)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", tr(S::StPerChapter));
                if (ImGui::BeginTable("stats_matrix", kStatsGroupCount + 1,
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerV,
                                      ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 11.0f)))
                {
                    ImGui::TableSetupColumn(tr(S::StColChapter));
                    for (const StatsGroup& grp : kStatsGroups)
                    {
                        ImGui::TableSetupColumn(tr(grp.label));
                    }
                    ImGui::TableHeadersRow();

                    const auto cell = [](const markers::CatStat& cs) {
                        ImGui::TableNextColumn();
                        if (cs.total == 0)
                        {
                            ImGui::TextDisabled("-");
                        }
                        else if (cs.found >= cs.total)
                        {
                            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%d/%d", cs.found,
                                               cs.total);
                        }
                        else
                        {
                            ImGui::Text("%d/%d", cs.found, cs.total);
                        }
                    };

                    // Row 0 is the bucket for a manifest whose "chapter" is not a number - the
                    // DLC one spells it "DLC".
                    for (int ch = 0; ch <= 8; ++ch)
                    {
                        markers::CatStat row{};
                        for (int i = 0; i < mdb::kCatCount; ++i)
                        {
                            row.total += st.chapter[ch][i].total;
                            row.found += st.chapter[ch][i].found;
                        }
                        if (row.total == 0)
                        {
                            continue;
                        }
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        // The chapter the markers are currently filtered to (the one the player
                        // is standing in) is starred and highlighted.
                        char label[64]{};
                        const bool here = (ch == st.filter_chapter);
                        if (ch == 0)
                        {
                            (void)utf8::format(label, sizeof(label), "%s%s", tr(S::Dlc), here ? "*" : "");
                        }
                        else
                        {
                            (void)utf8::format(label, sizeof(label), here ? "%d*" : "%d", ch);
                        }
                        if (here)
                        {
                            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", label);
                        }
                        else
                        {
                            ImGui::TextUnformatted(label);
                        }
                        for (const StatsGroup& grp : kStatsGroups)
                        {
                            cell(group_stat(st.chapter[ch], grp.cats));
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("%s", tr(S::StHereNote));
            }
        } // namespace

        // Draws into whatever window is current. `compact` drops the per-chapter matrix
        // and keeps the summary, for the F2 panel where vertical space is scarce.
        void draw_collection_stats(std::uint64_t now, bool compact)
        {
            const StatsCache& c = stats_cached(now);
            const markers::Stats& st = c.st;
            if (!st.db_loaded || st.static_markers == 0)
            {
                ImGui::TextDisabled("%s", tr(S::StNoMarkers));
                return;
            }
            stats_headline(st);
            stats_shrines(c, st);
            stats_by_category(st);
            // The compact form is the map's Stats panel, which has the chapter matrix in the
            // F2 tab instead.
            if (!compact)
            {
                stats_by_chapter(st);
            }
        }

        void draw_toast()
        {
            const std::uint64_t now = ::GetTickCount64();
            if (g_toast[0] == '\0' || now >= g_toast_until)
            {
                return;
            }
            const std::uint64_t left = g_toast_until - now;
            const float a = left >= 300 ? 1.0f : static_cast<float>(left) / 300.0f;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            // The foreground list, unlike the HUD: a toast must be readable over the
            // panel and over the full map.
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(g_toast);
            const float pad = ImGui::GetTextLineHeight() * 0.5f;
            const ImVec2 c{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.72f};
            const ImVec2 tl{c.x - ts.x * 0.5f - pad, c.y - ts.y * 0.5f - pad * 0.5f};
            const ImVec2 br{c.x + ts.x * 0.5f + pad, c.y + ts.y * 0.5f + pad * 0.5f};
            dl->AddRectFilled(tl, br, plate_color(static_cast<int>(220.0f * a)), 4.0f);
            dl->AddRect(tl, br, IM_COL32(150, 158, 168, static_cast<int>(180.0f * a)), 4.0f, 0, 1.2f);
            dl->AddText(ImVec2{c.x - ts.x * 0.5f, c.y - ts.y * 0.5f},
                        IM_COL32(240, 242, 246, static_cast<int>(255.0f * a)), g_toast);
        }

        // ---- "this marker just became found" -----------------------------------------
        //
        // A 400 ms ring where a marker was collected.

        void note_found_event(double x, double y, std::uint64_t now)
        {
            g_found_events[g_found_event_head] = FoundEvent{x, y, now};
            g_found_event_head = (g_found_event_head + 1) % kFoundEvents;
        }

        // Called from build_frame_candidates, once per PUBLISHED ROUND rather than per
        // frame - the flags cannot change in between.
        void update_found_watch(std::uint64_t round, std::uint64_t now)
        {
            if (round == g_found_watch_round)
            {
                return;
            }
            const bool first = g_found_watch_round == 0;
            g_found_watch_round = round;

            // The ring only makes sense for a marker the player can see, so the watch
            // list carries the same filter the minimap and the full map draw with. One
            // read per round, not per frame. A category switched back on rebuilds the
            // list before it can fire: an id the previous round did not watch produces
            // no event.
            const mm::Config cfg = mm::config();

            FoundWatch next[kFoundWatch]{};
            int n = 0;
            const float radius2 = static_cast<float>(kFoundWatchUu * kFoundWatchUu);
            for (const FrameCand& fc : g_frame_cands)
            {
                if (!cfg.markers_enabled || n >= kFoundWatch)
                {
                    break;
                }
                if (fc.d2_xy > radius2 || fc.m->id[0] == '\0')
                {
                    continue;
                }
                if (!mdb::cat_enabled(cfg.markers_categories, static_cast<mdb::Cat>(fc.cat)))
                {
                    continue;
                }
                ::strncpy_s(next[n].id, sizeof(next[n].id), fc.m->id, _TRUNCATE);
                next[n].found = fc.found;
                // The event is a marker the previous round watched as not found. One
                // that was not being watched produces nothing, so the first round after
                // a load fires no rings for items the save already has.
                if (!first && next[n].found)
                {
                    for (int j = 0; j < g_found_watch_n; ++j)
                    {
                        if (!g_found_watch[j].found && std::strcmp(g_found_watch[j].id, next[n].id) == 0)
                        {
                            note_found_event(fc.m->x, fc.m->y, now);
                            break;
                        }
                    }
                }
                ++n;
            }
            for (int i = 0; i < n; ++i)
            {
                g_found_watch[i] = next[i];
            }
            g_found_watch_n = n;
        }

        void build_frame_candidates(const mm::Snapshot& snap)
        {
            if (g_pf_markpass < 0)
            {
                g_pf_markpass = mm::perf_register("marker pass", perf::Thread::Render);
            }
            const mm::PerfScope scope(g_pf_markpass);
            g_frame_cands.clear();
            g_frame_marker_total = 0;
            g_frame_bad_cat = 0;

            const markers::View v = markers::view();
            g_frame_marker_total = static_cast<int>(v.count);
            if (v.data == nullptr || v.count == 0)
            {
                return;
            }
            g_frame_cands.reserve(v.count);
            for (std::size_t i = 0; i < v.count; ++i)
            {
                const markers::DrawMarker& m = v.data[i];
                if (static_cast<int>(m.cat) >= mdb::kCatCount)
                {
                    ++g_frame_bad_cat;
                    continue;
                }
                const double dx = m.x - snap.x;
                const double dy = m.y - snap.y;
                const double dz = m.z - snap.z;
                FrameCand c{};
                c.m = &m;
                c.d2_xy = static_cast<float>(dx * dx + dy * dy);
                c.d2_3d = static_cast<float>(dx * dx + dy * dy + dz * dz);
                c.cat = m.cat;
                c.flags = m.flags;
                c.found = (m.flags & markers::kFlagFound) != 0;
                g_frame_cands.push_back(c);
            }

            // The found-ring events. Diffed once per published marker round, not per
            // frame - the flags cannot change in between.
            update_found_watch(markers::rounds(), ::GetTickCount64());
        }
    } // namespace ovl
} // namespace overlay
