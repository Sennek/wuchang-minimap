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
        // whether the save has lit it - and, behind `fast_travel_enabled`, a Travel
        // action per row.
        //
        // Sources, all published snapshots (render thread, no game-thread work):
        //   shr::table()  markers/shrines.json - the id, the localised name, the chapter,
        //                 the actor position and the game's own BirthPosition;
        //   shr::state()  the save's UnlockedFirepoints list, read raw at 1 Hz;
        //   snap          the pawn position, for the distance.
        //
        // Sorted by distance, because "which shrine is near me" is the question a player
        // asks; the chapter filter follows the marker filter so the list and the map
        // agree about what exists.


        void draw_shrine_list(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state,
                              int filter_chapter)
        {
            const std::vector<shdb::Shrine>* table = shr::table();
            if (table == nullptr || table->empty())
            {
                const shr::TableInfo info = shr::table_info();
                ImGui::TextDisabled("no shrine table: %s",
                                    info.error[0] != '\0' ? info.error : "markers\\shrines.json is empty");
                return;
            }
            const shr::State st = shr::state();

            struct Row
            {
                const shdb::Shrine* s;
                double dist;
                bool unlocked;
            };
            // REUSED FRAME TO FRAME (review B.18): the shrine window is open while the
            // player reads it, so this vector was allocated and freed on the render
            // thread at frame rate. Render thread only, like every other static in this
            // file. (`Shrine::label()` returns a reference and allocates nothing.)
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
                ImGui::TextDisabled("no shrines in this chapter");
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
                ImGui::TextDisabled("unlocked state: n/a (%s)",
                                    st.route[0] != '\0' ? st.route : "not read yet");
            }
            const shr::TravelState tv = shr::travel_state();
            if (tv.phase == shr::Travel::Refused)
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "travel refused: %s", tv.note);
                ImGui::SameLine();
                if (ImGui::SmallButton("dismiss"))
                {
                    shr::clear_travel();
                }
            }

            if (!ImGui::BeginTable("shrines", cfg.fast_travel_enabled ? 5 : 4,
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f)))
            {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Shrine");
            ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Lit", ImGuiTableColumnFlags_WidthFixed);
            if (cfg.fast_travel_enabled)
            {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            }
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const Row& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const bool selected = ::strcmp(g_shrine_selected, r.s->id.c_str()) == 0;
                // One click sets a waypoint on it, a double-click centres the map there -
                // review item 1. Selectable, so the whole row is the hit target rather
                // than the eight characters of a name.
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
                        // The height slice is cut around the view centre, so a jump has
                        // to invalidate it or the map draws the old storey at the new
                        // place until the next scheduled cut.
                        g_map_recut.store(true, std::memory_order_release);
                        toast("centred on the shrine");
                    }
                    else
                    {
                        mv::Waypoint wp{};
                        wp.set = true;
                        wp.x = r.s->x;
                        wp.y = r.s->y;
                        wp.z = r.s->z;
                        mm::set_waypoint(wp);
                        mm::g_waypoint_dirty.store(true, std::memory_order_release);
                        toast("waypoint set on the shrine");
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\nid %s\nclick: waypoint    double-click: centre the map",
                                      r.s->label().c_str(), r.s->id.c_str());
                }
                ImGui::TableNextColumn();
                if (r.s->chapter < 0)
                {
                    ImGui::TextDisabled("-");
                }
                else if (r.s->chapter == 0)
                {
                    ImGui::TextUnformatted("DLC");
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
                else if (r.dist >= 100000.0)
                {
                    ImGui::Text("%.1f km", r.dist / 100000.0);
                }
                else
                {
                    ImGui::Text("%.0f m", r.dist / 100.0);
                }
                ImGui::TableNextColumn();
                if (!st.valid)
                {
                    ImGui::TextDisabled("n/a");
                }
                else if (r.unlocked)
                {
                    ImGui::TextColored(ImVec4{0.55f, 0.85f, 0.55f, 1.0f}, "yes");
                }
                else
                {
                    ImGui::TextDisabled("no");
                }
                if (cfg.fast_travel_enabled)
                {
                    ImGui::TableNextColumn();
                    // Only an id the save says is unlocked: travelling to a locked one is
                    // untested and is exactly the call that can wedge level streaming.
                    const bool can = r.unlocked && tv.phase != shr::Travel::Requested &&
                                     tv.phase != shr::Travel::InFlight;
                    ImGui::BeginDisabled(!can);
                    if (ImGui::SmallButton("Travel"))
                    {
                        shr::request_travel(r.s->id.c_str());
                        toast("fast travel requested");
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!cfg.fast_travel_enabled)
            {
                ImGui::TextDisabled("Fast travel is off (fast_travel_enabled, Advanced tab).");
            }
        }

        //==============================================================================
        // The collection statistics page
        //==============================================================================
        //
        // "Why am I using this mod" on one screen: found/total for every chapter and
        // every category, the shrines the save has lit, and one overall percentage.
        //
        // RENDER THREAD ONLY, and it never asks the game anything. Both sources are
        // published snapshots - `markers::stats()` (a spinlock and a ~1.5 KB copy) and
        // `shr::state()` (~6.5 KB) - so asking per frame would be ~8 KB of copying and a
        // lock round-trip per frame for numbers that change once per SWEEP ROUND. The
        // cache below therefore refreshes when `markers::rounds()` moves, with a 1 s
        // floor so the page still fills in when the live sweep is off entirely (rounds
        // never advance then, and a page that stays empty for ever reads as a bug).




        // THE CATEGORIES THE COLLECTION PAGE COUNTS, and the order they are shown in.

        // Draws into whatever window is current. `compact` drops the per-chapter matrix
        // and keeps the summary, for the F2 panel where vertical space is scarce.
        void draw_collection_stats(std::uint64_t now, bool compact)
        {
            const StatsCache& c = stats_cached(now);
            const markers::Stats& st = c.st;

            if (!st.db_loaded || st.static_markers == 0)
            {
                ImGui::TextDisabled("no markers\\<chapter>.json loaded - live markers only");
                return;
            }

            // ---- the headline ---------------------------------------------------------
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
            ImGui::Text("%d / %d collected", found_all, total_all);
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1f%% of every chapter)", static_cast<double>(pct));
            ImGui::ProgressBar(total_all > 0 ? static_cast<float>(found_all) /
                                                   static_cast<float>(total_all)
                                             : 0.0f,
                               ImVec2(-1.0f, ImGui::GetTextLineHeight()));

            // ---- shrines lit ----------------------------------------------------------
            //
            // `UnlockedFirepoints` also holds boss-door and task pseudo-points, so the
            // raw count is not "shrines". Only the ids that JOIN to a shrine marker in
            // the static DB are counted; the raw list length is shown beside it so a
            // join that goes wrong is visible rather than silent.
            const int shrine_total = st.cat[static_cast<int>(mdb::Cat::Shrine)].total;
            if (!c.shrines.valid)
            {
                ImGui::TextDisabled("Shrines lit: n/a  (%s)",
                                    c.shrines.route[0] != '\0' ? c.shrines.route : "not read yet");
            }
            else
            {
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
                ImGui::Text("Shrines lit: %d", lit);
                ImGui::SameLine();
                ImGui::TextDisabled("(%d unlocked ids incl. boss doors / tasks; %d shrines in the DB%s)",
                                    c.shrines.unlocked, shrine_total,
                                    c.shrines.truncated ? "; list TRUNCATED" : "");
                if (c.shrines.current[0] != '\0')
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("| last rested at %s", c.shrines.current);
                }
            }

            // ---- per category ---------------------------------------------------------
            ImGui::Spacing();
            if (ImGui::BeginTable("stats_cat", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Category");
                ImGui::TableSetupColumn("Found");
                ImGui::TableSetupColumn("Total");
                ImGui::TableSetupColumn("%");
                ImGui::TableHeadersRow();
                for (const mdb::Cat cat : kStatsCats)
                {
                    const int i = static_cast<int>(cat);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", mdb::cat_label(cat));
                    ImGui::TableNextColumn();
                    if (st.cat[i].total == 0)
                    {
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        continue;
                    }
                    ImGui::Text("%d", st.cat[i].found);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", st.cat[i].total);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%", 100.0 * static_cast<double>(st.cat[i].found) /
                                              static_cast<double>(st.cat[i].total));
                }
                ImGui::EndTable();
            }

            if (compact)
            {
                return;
            }

            // ---- per chapter x category ----------------------------------------------
            //
            // The six collectable categories, always all six, and nothing else: with the
            // full enum this was 15 columns and had to scroll sideways inside a 1080p
            // panel, which made the numbers on the right unreachable in practice.
            ImGui::Spacing();
            ImGui::TextDisabled("per chapter");
            if (ImGui::BeginTable("stats_matrix", kStatsCatCount + 1,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 11.0f)))
            {
                ImGui::TableSetupColumn("Chapter");
                for (const mdb::Cat cat : kStatsCats)
                {
                    ImGui::TableSetupColumn(mdb::cat_label(cat));
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
                        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%d/%d", cs.found, cs.total);
                    }
                    else
                    {
                        ImGui::Text("%d/%d", cs.found, cs.total);
                    }
                };

                // Row 0 is the bucket for a manifest whose "chapter" is not a number -
                // the DLC one spells it "DLC".
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
                    // The chapter the markers are currently filtered to (the one the
                    // player is standing in) is starred and highlighted.
                    char label[16]{};
                    const bool here = (ch == st.filter_chapter);
                    if (ch == 0)
                    {
                        ::strncpy_s(label, sizeof(label), here ? "DLC*" : "DLC", _TRUNCATE);
                    }
                    else
                    {
                        ::_snprintf_s(label, sizeof(label), _TRUNCATE, here ? "%d*" : "%d", ch);
                    }
                    if (here)
                    {
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", label);
                    }
                    else
                    {
                        ImGui::TextUnformatted(label);
                    }
                    for (const mdb::Cat cat : kStatsCats)
                    {
                        cell(st.chapter[ch][static_cast<int>(cat)]);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("* the chapter the markers are filtered to right now");
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
            // The one thing that KEEPS the foreground list (review B.20 moved the HUD
            // off it): a toast is a two-second notice about something the player just
            // did, and it has to be readable over the panel and over the full map -
            // "map copied to clipboard" is raised by a key that only works while the map
            // is open.
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
        // A 400 ms ring where a marker was collected. The event has to come from




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

            FoundWatch next[kFoundWatch]{};
            int n = 0;
            const float radius2 = static_cast<float>(kFoundWatchUu * kFoundWatchUu);
            for (const FrameCand& fc : g_frame_cands)
            {
                if (n >= kFoundWatch)
                {
                    break;
                }
                if (fc.d2_xy > radius2 || fc.m->id[0] == '\0')
                {
                    continue;
                }
                ::strncpy_s(next[n].id, sizeof(next[n].id), fc.m->id, _TRUNCATE);
                next[n].found = fc.found;
                // A marker that was in the previous round's watch as NOT found and is
                // found now is the event. A marker that was not being watched cannot
                // produce one - which is what stops the first round after a load firing
                // a ring for every item the save says is already collected.
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
                c.rarity = m.rarity;
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
