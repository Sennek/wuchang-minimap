//
// overlay_hud - the always-on HUD: the minimap disc, its markers, the toasts, the
// declutter pass, the hold-key x-ray highlight and the compass strip.
//
// Every function here draws into a frame the render thread is already inside.
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // Drawing: the minimap
        //==============================================================================

        UvMap uv_of(const mapdata::Chapter& c)
        {
            return UvMap{c.min_y, c.max_x, c.px_per_uu, c.image_width, c.image_height};
        }

        //==============================================================================
        // WORLD -> MINIMAP OFFSET, and the edge clamp
        //==============================================================================
        //
        // The one copy of the rotate-and-divide, the round-or-square limit test and the
        // scale-onto-the-rim, shared by draw_markers, the found-ring projector and the
        // waypoint block, so they cannot disagree by a pixel.
        //
        // The three callers differ only in what they want done when the point falls
        // outside the disc - see MiniOffset.

        MiniOffset mini_offset(const MiniGeom& g, double wx, double wy, bool round, float limit,
                               bool clamp_to_edge)
        {
            const double wdx = wx - g.px;
            const double wdy = wy - g.py;
            const double zz = g.zoom > 0.0001f ? static_cast<double>(g.zoom) : 1.0;
            MiniOffset out{};
            // Screen up is the player's forward (rotate mode) or world +X (north-up);
            // see uv_at() below for the derivation of these two rows.
            out.dx = (-g.sin_yaw * wdx + g.cos_yaw * wdy) / zz;
            out.dy = (-g.cos_yaw * wdx - g.sin_yaw * wdy) / zz;
            const double lim = static_cast<double>(limit);
            if (round)
            {
                const double d2 = out.dx * out.dx + out.dy * out.dy;
                if (d2 <= lim * lim)
                {
                    out.visible = true;
                    return out;
                }
                const double d = std::sqrt(d2);
                if (!clamp_to_edge || d <= 0.0001)
                {
                    return out;
                }
                out.dx = out.dx * lim / d;
                out.dy = out.dy * lim / d;
            }
            else
            {
                if (std::abs(out.dx) <= lim && std::abs(out.dy) <= lim)
                {
                    out.visible = true;
                    return out;
                }
                if (!clamp_to_edge)
                {
                    return out;
                }
                const double sc = lim / (std::max)(std::abs(out.dx), std::abs(out.dy));
                out.dx *= sc;
                out.dy *= sc;
            }
            out.visible = true;
            out.clamped = true;
            return out;
        }

        // Screen offset (dx, dy) in minimap pixels -> texture uv.
        //
        // Screen up is the player's forward when rotate_with_player is on, and world +X
        // (north) otherwise; screen right is the corresponding right vector. In UE, for
        // yaw a, forward = (cos a, sin a) and right = (-sin a, cos a). So
        //     world = player + forward * (-dy * zoom) + right * (dx * zoom)
        // which for yaw = 0 reduces to (X - dy*zoom, Y + dx*zoom): screen right -> +Y
        // (east -> right in the image) and screen up -> +X (north -> up). That is
        // exactly render.py / build_map.py's north-up convention.
        ImVec2 uv_at(const MiniGeom& g, float dx, float dy)
        {
            const double fx = g.cos_yaw;
            const double fy = g.sin_yaw;
            const double rx = -g.sin_yaw;
            const double ry = g.cos_yaw;
            const double wx = g.px + fx * (-dy * g.zoom) + rx * (dx * g.zoom);
            const double wy = g.py + fy * (-dy * g.zoom) + ry * (dx * g.zoom);
            float u = 0.0f;
            float v = 0.0f;
            g.uv.to_uv(wx, wy, u, v);
            return ImVec2{u, v};
        }

        void add_image_circle(ImDrawList* dl, ImTextureRef tex, const MiniGeom& g, ImU32 col)
        {
            dl->PushTexture(tex);
            dl->PrimReserve(g_circle_segments * 3, g_circle_segments + 1);
            const unsigned int base = dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(g.center, uv_at(g, 0.0f, 0.0f), col);
            for (int i = 0; i < g_circle_segments; ++i)
            {
                const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(g_circle_segments);
                const float dx = std::cos(a) * g.half;
                const float dy = std::sin(a) * g.half;
                dl->PrimWriteVtx(ImVec2{g.center.x + dx, g.center.y + dy}, uv_at(g, dx, dy), col);
            }
            for (int i = 0; i < g_circle_segments; ++i)
            {
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + ((i + 1) % g_circle_segments)));
            }
            dl->PopTexture();
        }

        void add_player_arrow(ImDrawList* dl, ImVec2 c, float angle_deg, float size)
        {
            // angle measured clockwise from screen up.
            const float a = angle_deg * kPi / 180.0f;
            const float dirx = std::sin(a);
            const float diry = -std::cos(a);
            const float perpx = std::cos(a);
            const float perpy = std::sin(a);
            const ImVec2 tip{c.x + dirx * size, c.y + diry * size};
            const ImVec2 l{c.x - dirx * size * 0.62f + perpx * size * 0.60f,
                           c.y - diry * size * 0.62f + perpy * size * 0.60f};
            const ImVec2 r{c.x - dirx * size * 0.62f - perpx * size * 0.60f,
                           c.y - diry * size * 0.62f - perpy * size * 0.60f};
            dl->AddTriangleFilled(tip, l, r, IM_COL32(255, 226, 92, 255));
            dl->AddTriangle(tip, l, r, IM_COL32(30, 26, 10, 220), 1.6f);
        }

        //==============================================================================
        // Drawing: markers
        //==============================================================================
        //
        // Glyphs are ImDrawList primitives, not an image atlas. Each category has a
        // shape AND a colour, because a dimmed "found" marker keeps only its shape.
        //
        // World -> minimap pixels is the inverse of uv_at(): for yaw a,
        // forward = (cos a, sin a) and right = (-sin a, cos a), so
        //     wdx = -s*(dx*z) - c*(dy*z)
        //     wdy =  c*(dx*z) - s*(dy*z)
        // whose inverse (the matrix is a rotation, det = 1) is
        //     dx = (-s*wdx + c*wdy) / z
        //     dy = (-c*wdx - s*wdy) / z
        // At yaw 0 that is dx = wdy/z (east to the right) and dy = -wdx/z (north up),
        // i.e. exactly build_map.py's north-up convention.

        // Defined with the full map, below: the minimap draws the same glyph.

        // The look, cached once per frame (build_ui). Render thread only. `g_palette`
        // is the marker hue set, `g_plate` the theme's dark label plate; the theme's
        // other colours are resolved into ordinary colour config keys at load time
        // (mmstate.cpp's apply_theme_defaults), so an explicit key overrides a theme.

        // "The HUD has been on screen at least once", published by the render thread on
        // the first frame hud_gate() answers yes - the first frame with a validated
        // gameplay pawn.

        // The hue table lives in the pure header src/glyphs.hpp, beside the shape
        // table: "no two categories share a shape and a colour" is asserted offline.
        ImU32 marker_color(mdb::Cat cat, int alpha)
        {
            const mdb::Rgb c = gly::marker_rgb(cat, g_palette);
            return IM_COL32(c.r, c.g, c.b, alpha);
        }

        // The dark box behind a label, the compass strip and the waypoint's distance.
        ImU32 plate_color(int alpha)
        {
            return IM_COL32(g_plate.r, g_plate.g, g_plate.b, alpha);
        }

        // Category colour, or the item-quality colour when the caller asked for it and
        // this marker has a tier. Tier 0 falls through to the category colour - it is
        // what every chest, live-only actor and ordinary consumable is. Tiers and the
        // default palette come from mdb::Rarity.
        ImU32 marker_color_q(mdb::Cat cat, std::uint8_t rarity, int alpha, bool use_rarity,
                             const mdb::Rgb* palette)
        {
            const int tier = mdb::rarity_clamp(static_cast<int>(rarity));
            if (!use_rarity || tier == 0 || palette == nullptr)
            {
                return marker_color(cat, alpha);
            }
            const mdb::Rgb& c = palette[tier];
            return IM_COL32(c.r, c.g, c.b, alpha);
        }

        // One glyph. `hollow` is how a FOUND marker is drawn - an outline keeps the
        // shape where dimming would leave a grey blob. Every glyph gets a dark halo
        // first, at the glyph's own alpha, so it has an edge over any scene.

        void draw_marker_glyph(ImDrawList* dl, mdb::Cat cat, ImVec2 p, float r, ImU32 col, ImU32 edge,
                               bool hollow)
        {
            const int ca = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFFu);
            // gly::shape_extent() is how far this shape reaches, so the halo covers
            // corners a plain r + 1 circle would leave sticking out.
            const gly::Shape shape = gly::shape_of(cat);
            dl->AddCircleFilled(p, r * gly::shape_extent(shape) + 1.0f,
                                IM_COL32(0, 0, 0, (ca * 120) / 255), 16);

            // Below ~7 px the fine detail inside a glyph is a smudge rather than a
            // silhouette, so the three complex shapes have a simplified form.
            const bool simple = r < gly::kSimpleGlyphRadius;
            const float w = hollow ? 1.7f : 1.2f;
            // Filled when live, the same geometry as an outline when found, so nothing
            // moves when one state becomes the other.
            const auto ngon = [&](float rad, int n) {
                if (hollow)
                {
                    dl->AddNgon(p, rad, col, n, w);
                }
                else
                {
                    dl->AddNgonFilled(p, rad, col, n);
                    dl->AddNgon(p, rad, edge, n, w);
                }
            };
            const auto circle = [&](ImVec2 c, float rad, int n) {
                if (hollow)
                {
                    dl->AddCircle(c, rad, col, n, w);
                }
                else
                {
                    dl->AddCircleFilled(c, rad, col, n);
                    dl->AddCircle(c, rad, edge, n, w);
                }
            };
            const auto rect = [&](float hw, float hh) {
                const ImVec2 a{p.x - r * hw, p.y - r * hh};
                const ImVec2 b{p.x + r * hw, p.y + r * hh};
                if (hollow)
                {
                    dl->AddRect(a, b, col, 1.5f, 0, w);
                }
                else
                {
                    dl->AddRectFilled(a, b, col, 1.5f);
                    dl->AddRect(a, b, edge, 1.5f, 0, w);
                }
            };
            const auto tri = [&](float scale) {
                const ImVec2 a{p.x, p.y - r * scale};
                const ImVec2 b{p.x - r * scale * 0.92f, p.y + r * scale * 0.72f};
                const ImVec2 c{p.x + r * scale * 0.92f, p.y + r * scale * 0.72f};
                if (hollow)
                {
                    dl->AddTriangle(a, b, c, col, w);
                }
                else
                {
                    dl->AddTriangleFilled(a, b, c, col);
                    dl->AddTriangle(a, b, c, edge, w);
                }
            };
            // The dark centre that tells a shrine from a plain diamond. On a hollow
            // glyph it takes the marker's own colour - there is no fill to contrast with.
            const auto pip = [&](float rad) {
                dl->AddCircleFilled(p, r * rad, hollow ? col : edge, 8);
            };

            switch (shape)
            {
            case gly::Shape::Diamond:
                ngon(r * 1.15f, 4);
                pip(0.32f);
                break;
            case gly::Shape::ChestBox:
                rect(0.95f, 0.75f);
                dl->AddLine(ImVec2{p.x - r * 0.95f, p.y}, ImVec2{p.x + r * 0.95f, p.y},
                            hollow ? col : edge, w);
                break;
            case gly::Shape::Dot:
                circle(p, r * 0.72f, 10);
                break;
            case gly::Shape::Triangle:
                tri(1.5f);
                break;
            case gly::Shape::TriangleNotched:
                tri(1.15f);
                dl->AddLine(ImVec2{p.x - r * 0.62f, p.y + r * 0.30f},
                            ImVec2{p.x + r * 0.62f, p.y + r * 0.30f}, hollow ? col : edge, w + 0.3f);
                break;
            case gly::Shape::DotRing:
                dl->AddCircleFilled(p, r * 0.34f, col, 8);
                dl->AddCircle(p, r * 0.92f, col, 12, w);
                break;
            case gly::Shape::Pentagon:
                ngon(r * 1.05f, 5);
                break;
            case gly::Shape::NotePage:
            {
                // A page with its top-right corner folded away, plus two text rules.
                // The fold and the proportions are what keep it apart from the door's
                // plain tall box at glyph size.
                const float hw = r * 0.62f;
                const float hh = r * 0.88f;
                const float fold = r * 0.44f; // the 45-degree bite out of the corner
                const ImVec2 pts[5] = {
                    ImVec2{p.x - hw, p.y - hh},
                    ImVec2{p.x + hw - fold, p.y - hh},
                    ImVec2{p.x + hw, p.y - hh + fold},
                    ImVec2{p.x + hw, p.y + hh},
                    ImVec2{p.x - hw, p.y + hh},
                };
                if (hollow)
                {
                    dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, w);
                }
                else
                {
                    dl->AddConvexPolyFilled(pts, 5, col);
                    dl->AddPolyline(pts, 5, edge, ImDrawFlags_Closed, w);
                }
                const ImU32 ink = hollow ? col : edge;
                dl->AddLine(ImVec2{p.x + hw - fold, p.y - hh}, ImVec2{p.x + hw - fold, p.y - hh + fold},
                            ink, w);
                dl->AddLine(ImVec2{p.x + hw - fold, p.y - hh + fold}, ImVec2{p.x + hw, p.y - hh + fold},
                            ink, w);
                // Two rules of "writing" - dropped in the simplified form, where they
                // are 2 px apart inside a 5 px page and fill it in.
                if (!simple)
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        const float y = p.y + r * (i == 0 ? 0.10f : 0.45f);
                        dl->AddLine(ImVec2{p.x - hw * 0.6f, y}, ImVec2{p.x + hw * 0.6f, y}, ink, w * 0.8f);
                    }
                }
                break;
            }
            case gly::Shape::DoorBox:
                rect(0.55f, 0.95f);
                break;
            case gly::Shape::Ladder:
                dl->AddLine(ImVec2{p.x - r * 0.5f, p.y - r}, ImVec2{p.x - r * 0.5f, p.y + r}, col, 1.6f);
                dl->AddLine(ImVec2{p.x + r * 0.5f, p.y - r}, ImVec2{p.x + r * 0.5f, p.y + r}, col, 1.6f);
                // Simplified: one rung, not three - at r = 6.5 three rungs merge into
                // the chest's filled box.
                if (simple)
                {
                    dl->AddLine(ImVec2{p.x - r * 0.5f, p.y}, ImVec2{p.x + r * 0.5f, p.y}, col, 1.2f);
                }
                else
                {
                    for (int i = -1; i <= 1; ++i)
                    {
                        const float y = p.y + static_cast<float>(i) * r * 0.55f;
                        dl->AddLine(ImVec2{p.x - r * 0.5f, y}, ImVec2{p.x + r * 0.5f, y}, col, 1.2f);
                    }
                }
                break;
            case gly::Shape::Lift:
            {
                // Simplified: a flatter platform and a taller, narrower arrow, drawn
                // filled even when found - at r = 6.5 two outlines this close are a blob.
                const float box_hh = simple ? 0.36f : 0.5f;
                rect(0.85f, box_hh);
                const float tip = p.y - r * (simple ? 1.2f : 1.35f);
                const float base = p.y - r * (simple ? 0.5f : 0.6f);
                const float half_w = r * (simple ? 0.42f : 0.5f);
                if (hollow && !simple)
                {
                    dl->AddTriangle(ImVec2{p.x, tip}, ImVec2{p.x - half_w, base},
                                    ImVec2{p.x + half_w, base}, col, w);
                }
                else
                {
                    dl->AddTriangleFilled(ImVec2{p.x, tip}, ImVec2{p.x - half_w, base},
                                          ImVec2{p.x + half_w, base}, col);
                }
                break;
            }
            case gly::Shape::RingBar:
                dl->AddCircle(p, r, col, 14, 2.0f);
                dl->AddLine(ImVec2{p.x - r * 0.7f, p.y}, ImVec2{p.x + r * 0.7f, p.y}, col, 1.4f);
                break;
            case gly::Shape::Cross:
                dl->AddLine(ImVec2{p.x - r * 0.85f, p.y - r * 0.85f},
                            ImVec2{p.x + r * 0.85f, p.y + r * 0.85f}, col, 2.1f);
                dl->AddLine(ImVec2{p.x - r * 0.85f, p.y + r * 0.85f},
                            ImVec2{p.x + r * 0.85f, p.y - r * 0.85f}, col, 2.1f);
                break;
            case gly::Shape::SmallSquare:
            case gly::Shape::Count:
            default:
                rect(0.55f, 0.55f);
                break;
            }
        }

        //==============================================================================
        // ONE marker pass per frame
        //==============================================================================
        //
        // The minimap, the compass pips and the x-ray highlight ask different questions
        // of the same 700-3 600 published rows, so the buffer is walked once here and
        // each of them filters the result.
        //
        // Distances are kept squared: consumers only compare and sort on them, and the
        // two that want metres take the square root of the handful they draw.

        //==============================================================================
        // Animation, toasts, and the "it just became found" event
        //==============================================================================
        //
        // All of it is render-thread state derived from what the frame already has. None
        // of it costs the game thread anything, and none of it can keep something on
        // screen after the state that allows it went away:
        //
        //   * the HUD fade's target is the hud_gate result. A target of 0 is applied
        //     instantly (hiding is immediate; the gate also stops the draw outright),
        //     and only showing is eased - the no-latch rule applied to an animation.
        //   * a toast is a string plus a deadline; it says what just happened and then
        //     goes.
        //   * the found ring is driven by comparing the PUBLISHED found flags of the
        //     markers near the player between marker rounds - the game thread does no
        //     extra work for it, and nothing is remembered longer than one round.

        // Eases towards 1 while `target_on`, drops to 0 the instant it is false.
        float hud_fade_step(bool target_on, std::uint64_t now)
        {
            if (!target_on)
            {
                g_hud_fade = 0.0f;
                g_hud_fade_ms = 0;
                return 0.0f;
            }
            if (g_hud_fade_ms == 0)
            {
                g_hud_fade_ms = now;
            }
            const std::uint64_t age = now - g_hud_fade_ms;
            const float t = age >= kHudFadeMs ? 1.0f : static_cast<float>(age) / static_cast<float>(kHudFadeMs);
            // Smoothstep: a linear ramp on an alpha reads as a hard edge at both ends.
            g_hud_fade = t * t * (3.0f - 2.0f * t);
            return g_hud_fade;
        }

        void toast_for(const char* text, unsigned ms)
        {
            ::strncpy_s(g_toast, sizeof(g_toast), text, _TRUNCATE);
            g_toast_until = ::GetTickCount64() + ms;
        }

        void toast(const char* text)
        {
            toast_for(text, 1000);
        }

        //==============================================================================
        // Map -> clipboard
        //==============================================================================
        //
        // The source is the BACK BUFFER of the frame just drawn, so the picture is what
        // the player is looking at. Spread over frames:
        //   frame N   : after ImGui's draw call is recorded, the back buffer goes
        //               RENDER_TARGET -> COPY_SOURCE, one CopyTextureRegion of the
        //               canvas rect is recorded into a readback buffer, and the fence
        //               this frame will signal is remembered.
        //   frame N+k : once GetCompletedValue() has passed that fence the readback is
        //               mapped, unpacked to BGRA8 (clipimg - the back buffer is HDR10
        //               R10G10B10A2 here, not R8G8B8A8) and turned into a CF_DIB payload
        //               handed to the LOOP thread.
        //   loop      : OpenClipboard / EmptyClipboard / SetClipboardData / CloseClipboard.
        //
        // Nothing is mapped before its fence has passed (a readback read early is a
        // garbage picture, not an error), and the clipboard - a blocking,
        // window-station-wide, message-pumping API - is never touched inside Present.
        // No file is ever written.

        //==============================================================================
        // THE TOAST MAILBOX
        //==============================================================================
        //
        // One slot and its own lock, separate from the screenshot's: the two share
        // nothing but a direction, loop -> render.
        //

        // Any thread. Queues a toast for the render thread to draw.
        void post_toast(const char* text, unsigned ms)
        {
            {
                spin::SpinGuard guard(g_toast_lock);
                ::strncpy_s(g_toast_pending, sizeof(g_toast_pending), text, _TRUNCATE);
                g_toast_pending_ms = ms;
            }
            g_toast_pending_ready.store(true, std::memory_order_release);
        }

        void shot_fail(const char* why)
        {
            post_toast(why, 2500);
        }

        void shot_reset()
        {
            safe_release(g_shot_readback);
            g_shot_stage = ShotStage::Idle;
            g_shot_fence = 0;
            g_shot_w = 0;
            g_shot_h = 0;
            g_shot_pitch = 0;
        }

        // Render thread. Records the copy into the command list the frame is already
        // building, between ImGui's draw call and the transition back to PRESENT. The
        // back buffer is in RENDER_TARGET state on entry and left in PRESENT state, so
        // this replaces the caller's closing barrier when it returns true.
        bool record_shot_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, UINT index)
        {
            if (g_shot_stage != ShotStage::Idle || !g_shot_request.exchange(false, std::memory_order_acquire))
            {
                return false;
            }
            ID3D12Device* dev = g_device;
            if (dev == nullptr || backbuffer == nullptr)
            {
                shot_fail("screenshot: no device");
                return false;
            }
            const DXGI_FORMAT fmt = g_format;
            g_shot_fmt = clipimg::fmt_from_dxgi(static_cast<unsigned>(fmt));
            if (g_shot_fmt == clipimg::Fmt::Unknown)
            {
                shot_fail("screenshot: back buffer format not supported");
                mm::logf(L"screenshot: DXGI format {} is not one this build can unpack",
                         static_cast<int>(fmt));
                return false;
            }
            // The map canvas as laid out this frame, clamped to the back buffer.
            if (!g_shot_canvas_valid)
            {
                shot_fail("screenshot: the map is not open");
                return false;
            }
            long x0 = static_cast<long>(g_shot_canvas.x0);
            long y0 = static_cast<long>(g_shot_canvas.y0);
            long x1 = static_cast<long>(g_shot_canvas.x1);
            long y1 = static_cast<long>(g_shot_canvas.y1);
            x0 = x0 < 0 ? 0 : x0;
            y0 = y0 < 0 ? 0 : y0;
            x1 = x1 > static_cast<long>(g_width) ? static_cast<long>(g_width) : x1;
            y1 = y1 > static_cast<long>(g_height) ? static_cast<long>(g_height) : y1;
            if (x1 - x0 < 16 || y1 - y0 < 16)
            {
                shot_fail("screenshot: the map canvas is too small");
                return false;
            }
            g_shot_w = static_cast<UINT>(x1 - x0);
            g_shot_h = static_cast<UINT>(y1 - y0);
            // A readback footprint's row pitch must be 256-aligned.
            g_shot_pitch = (g_shot_w * 4u + 255u) & ~255u;

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = static_cast<UINT64>(g_shot_pitch) * g_shot_h;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&g_shot_readback))))
            {
                shot_fail("screenshot: readback buffer allocation failed");
                shot_reset();
                return false;
            }

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = backbuffer;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_shot_readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = fmt;
            dst.PlacedFootprint.Footprint.Width = g_shot_w;
            dst.PlacedFootprint.Footprint.Height = g_shot_h;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = g_shot_pitch;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            D3D12_BOX box{};
            box.left = static_cast<UINT>(x0);
            box.top = static_cast<UINT>(y0);
            box.front = 0;
            box.right = static_cast<UINT>(x1);
            box.bottom = static_cast<UINT>(y1);
            box.back = 1;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &b);
            g_shot_stage = ShotStage::Recorded;
            (void)index;
            return true;
        }

        // Render thread, top of the frame. Once the GPU is past the fence, maps the
        // readback and builds the DIB.
        void shot_collect()
        {
            if (g_shot_stage_done.exchange(false, std::memory_order_acquire))
            {
                shot_reset(); // the loop thread is done with the bytes; re-arm
            }
            if (g_shot_stage != ShotStage::Recorded || g_shot_readback == nullptr || g_fence == nullptr)
            {
                return;
            }
            if (g_shot_fence == 0 || g_fence->GetCompletedValue() < g_shot_fence)
            {
                return;
            }
            void* mapped = nullptr;
            D3D12_RANGE range{};
            range.Begin = 0;
            range.End = static_cast<SIZE_T>(g_shot_pitch) * g_shot_h;
            if (FAILED(g_shot_readback->Map(0, &range, &mapped)) || mapped == nullptr)
            {
                shot_fail("screenshot: readback map failed");
                shot_reset();
                return;
            }
            // Row by row into a tight BGRA image; clipimg flips it into the bottom-up
            // DIB the clipboard wants.
            std::vector<std::uint8_t> bgra(static_cast<std::size_t>(g_shot_w) * 4u * g_shot_h);
            bool ok = true;
            for (UINT y = 0; y < g_shot_h && ok; ++y)
            {
                const auto* srow = static_cast<const std::uint8_t*>(mapped) +
                                   static_cast<std::size_t>(y) * g_shot_pitch;
                ok = clipimg::unpack_row(g_shot_fmt, srow,
                                         bgra.data() + static_cast<std::size_t>(y) * g_shot_w * 4u,
                                         static_cast<int>(g_shot_w));
            }
            const D3D12_RANGE none{0, 0};
            g_shot_readback->Unmap(0, &none);
            if (!ok)
            {
                shot_fail("screenshot: pixel unpack failed");
                shot_reset();
                return;
            }
            std::vector<std::uint8_t> dib;
            if (!clipimg::build_dib(static_cast<int>(g_shot_w), static_cast<int>(g_shot_h), bgra.data(),
                                    static_cast<std::size_t>(g_shot_w) * 4u, dib))
            {
                shot_fail("screenshot: bitmap build failed");
                shot_reset();
                return;
            }
            {
                spin::SpinGuard guard(g_shot_lock);
                g_shot_dib = std::move(dib);
            }
            g_shot_dib_ready.store(true, std::memory_order_release);
            safe_release(g_shot_readback);
            g_shot_stage = ShotStage::Waiting;
        }

        //==============================================================================
        // DECLUTTER: MERGING COINCIDENT GLYPHS
        //==============================================================================
        //
        // Six chests in one room are six glyphs inside one glyph's width, so glyphs of
        // the same category and the same found state merge into one with a count badge,
        // and the nearest member of a cluster is the one drawn.
        //
        // A uniform grid keyed on (cell, category): anything landing in the cell of an
        // already-kept glyph of the same category joins it. Cell size is the merge
        // distance, so two glyphs straddling a boundary can stay separate - the price of
        // an O(n) grid instead of an O(n x kept) sweep on the render thread.

        // The "and N more like this one" badge, up and to the right of the glyph, on
        // the plate colour, and never drawn for a cluster of one.
        void draw_count_badge(ImDrawList* dl, ImVec2 at, float r, int count, int alpha)
        {
            if (count < 2)
            {
                return;
            }
            char text[8]{};
            if (count > 99)
            {
                (void)std::snprintf(text, sizeof(text), "99+");
            }
            else
            {
                (void)std::snprintf(text, sizeof(text), "%d", count);
            }
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 tp{at.x + r * 0.65f, at.y - r * 0.65f - ts.y * 0.5f};
            dl->AddRectFilled(ImVec2{tp.x - 2.0f, tp.y}, ImVec2{tp.x + ts.x + 2.0f, tp.y + ts.y},
                              plate_color(static_cast<int>(alpha * 0.82f)), 2.0f);
            dl->AddText(tp, IM_COL32(238, 242, 248, alpha), text);
        }

        void draw_markers(const mm::Config& cfg, const MiniGeom& g, bool round, float x0, float y0, float side,
                          ImDrawList* dl)
        {
            g_marker_draw = MarkerDrawStats{};
            if (!cfg.markers_enabled)
            {
                return;
            }
            g_marker_draw.total = g_frame_marker_total;
            g_marker_draw.filtered = g_frame_bad_cat;
            if (g_frame_cands.empty())
            {
                return;
            }

            const float r = cfg.markers_size;
            const float limit = (std::max)(4.0f, g.half - r - 2.0f);

            struct Cand
            {
                float dx = 0.0f;
                float dy = 0.0f;
                float d2 = 0.0f;
                std::uint8_t cat = 0;
                std::uint8_t rarity = 0;
                bool found = false;
                bool clamped = false;
                int count = 1; // how many markers this glyph stands for
                const char* id = nullptr;
            };
            // Render thread only, and reused frame to frame so a full minimap never
            // allocates during Present.
            static std::vector<Cand> cands;
            cands.clear();

            for (const FrameCand& fc : g_frame_cands)
            {
                const markers::DrawMarker& m = *fc.m;
                const mdb::Cat cat = static_cast<mdb::Cat>(fc.cat);
                if (!mdb::cat_enabled(cfg.markers_categories, cat))
                {
                    ++g_marker_draw.filtered;
                    continue;
                }
                const bool found = fc.found;
                if (found && cfg.markers_hide_found)
                {
                    ++g_marker_draw.filtered;
                    continue;
                }

                // The minimap is centred on the position the render side works from,
                // so the on-screen offset is computed here; the distance for the cap and
                // the sort comes from the shared pass.
                const MiniOffset off =
                    mini_offset(g, m.x, m.y, round, limit, cfg.markers_clamp_to_edge);
                if (!off.visible)
                {
                    continue;
                }

                Cand cand{};
                cand.dx = static_cast<float>(off.dx);
                cand.dy = static_cast<float>(off.dy);
                cand.d2 = fc.d2_xy;
                cand.cat = fc.cat;
                cand.rarity = fc.rarity;
                cand.found = found;
                cand.clamped = off.clamped;
                cand.id = m.id;
                cands.push_back(cand);
            }

            // One sort, nearest first: the cap drops the far ones and the draw loop
            // walks the array backwards, so the near ones are painted last (on top).
            // partial_sort leaves [0, cap) sorted ascending, so after the resize the
            // whole array is sorted and no second sort is needed.
            const std::size_t cap = cfg.markers_max_draw > 0
                                        ? static_cast<std::size_t>(cfg.markers_max_draw)
                                        : cands.size();
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            else
            {
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            }

            // ---- declutter -----------------------------------------------------------
            //
            // After the sort, so a cluster's kept glyph is its nearest member, and after
            // the cap, so merging cannot resurrect a dropped marker. Clamped glyphs are
            // left out: merging those collapses a whole direction into one number.
            {
                static MergeGrid grid;
                const float merge_r = (std::max)(3.0f, r);
                grid.reset(g.center.x - g.half, g.center.y - g.half, side, side, merge_r);
                std::size_t kept = 0;
                for (std::size_t i = 0; i < cands.size(); ++i)
                {
                    const Cand& c = cands[i];
                    const float sx = g.center.x + c.dx;
                    const float sy = g.center.y + c.dy;
                    const int key = c.clamped ? -1 : grid.find(sx, sy, static_cast<int>(c.cat));
                    if (key >= 0 && cands[static_cast<std::size_t>(key)].found == c.found)
                    {
                        ++cands[static_cast<std::size_t>(key)].count;
                        continue;
                    }
                    if (!c.clamped)
                    {
                        grid.add(sx, sy, static_cast<int>(c.cat), static_cast<int>(kept));
                    }
                    cands[kept++] = c;
                }
                g_marker_draw.merged = static_cast<int>(cands.size() - kept);
                cands.resize(kept);
            }

            const float op = cfg.opacity;
            for (std::size_t ci = cands.size(); ci-- > 0;)
            {
                const Cand& cand = cands[ci];
                const float a = op * (cand.found ? cfg.markers_found_alpha : 1.0f);
                if (a <= 0.01f)
                {
                    continue;
                }
                const int alpha = static_cast<int>((std::min)(1.0f, a) * 255.0f + 0.5f);
                const ImU32 col = marker_color_q(static_cast<mdb::Cat>(cand.cat), cand.rarity, alpha,
                                                 cfg.markers_rarity_tint, cfg.xray_rarity_colors);
                const ImU32 edge = IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f));
                const ImVec2 p{g.center.x + cand.dx, g.center.y + cand.dy};
                draw_marker_glyph(dl, static_cast<mdb::Cat>(cand.cat), p, cand.clamped ? r * 0.72f : r, col, edge,
                                  cand.found);
                draw_count_badge(dl, p, r, cand.count, alpha);
                ++g_marker_draw.drawn;
                g_marker_draw.clamped += cand.clamped ? 1 : 0;
            }
            if (!cands.empty())
            {
                // cands is sorted near -> far, so the FIRST one is the nearest.
                const Cand& near_one = cands.front();
                if (near_one.id != nullptr)
                {
                    ::strncpy_s(g_marker_draw.nearest, sizeof(g_marker_draw.nearest), near_one.id, _TRUNCATE);
                }
                g_marker_draw.nearest_uu = std::sqrt(near_one.d2);
            }
            (void)x0;
            (void)y0;
            (void)side;
        }

        //==============================================================================
        // THE ONE HUD GATE
        //==============================================================================
        //
        // "May anything of ours be on screen right now?" - the minimap, the compass and
        // the x-ray highlight all ask this, evaluated once from live state only. It
        // returns nullptr, or the reason nothing may draw: the minimap feeds that to
        // set_hide_reason() (which owns the "hidden because" readout and the transition
        // log lines), the compass and the highlight simply do not draw.
        //

        void draw_minimap(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            g_last_mini = MiniDebug{};

            const std::uint64_t now = ::GetTickCount64();
            if (const wchar_t* blocked = hud_gate(cfg, snap, have_state, now); blocked != nullptr)
            {
                set_hide_reason(blocked);
                return;
            }
            const mapdata::Chapter* chapter_ptr = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (chapter_ptr == nullptr)
            {
                set_hide_reason(L"player is outside every mapped chapter");
                return;
            }
            const mapdata::Chapter& chapter = *chapter_ptr;
            const bool composite_ready = g_map.ready && chapter.key == g_map.chapter;
            if (!chapter.has_heights() && !composite_ready)
            {
                set_hide_reason(L"no height maps and no composite texture loaded");
                return;
            }

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float screen_w = vp->Size.x;
            const float screen_h = vp->Size.y;
            const float side = (std::max)(cfg.minimap_min_px,
                                          (std::min)(cfg.size_frac * screen_h,
                                                     (std::min)(screen_w, screen_h) * 0.9f));

            // A non-custom hud_preset overrides minimap_anchor (and puts the compass on
            // the same vertical side - see draw_compass).
            const mm::Anchor anchor = effective_anchor(cfg);
            float x0 = cfg.offset_x;
            float y0 = cfg.offset_y;
            if (anchor == mm::Anchor::TopRight || anchor == mm::Anchor::BottomRight)
            {
                x0 = screen_w - cfg.offset_x - side;
            }
            if (anchor == mm::Anchor::BottomLeft || anchor == mm::Anchor::BottomRight)
            {
                y0 = screen_h - cfg.offset_y - side;
            }
            x0 += vp->Pos.x;
            y0 += vp->Pos.y;

            MiniGeom g{};
            g.half = side * 0.5f;
            g.center = ImVec2{x0 + g.half, y0 + g.half};
            g.zoom = cfg.zoom_uu_per_px;
            g.uv = uv_of(chapter);
            g.px = snap.x;
            g.py = snap.y;
            const float eff_yaw = cfg.rotate_with_player ? snap.yaw : 0.0f;
            g.cos_yaw = std::cos(eff_yaw * kPi / 180.0f);
            g.sin_yaw = std::sin(eff_yaw * kPi / 180.0f);

            const float op = cfg.opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };
            // The walkable fill needs something dark to sit on, or a light scene eats it.
            const auto ch = [](float v) { return static_cast<int>(v + 0.5f); };
            const ImU32 backdrop = IM_COL32(ch(cfg.minimap_backdrop_r), ch(cfg.minimap_backdrop_g),
                                            ch(cfg.minimap_backdrop_b), alpha(cfg.minimap_backdrop));
            const ImU32 frame = IM_COL32(ch(cfg.minimap_frame_r), ch(cfg.minimap_frame_g), ch(cfg.minimap_frame_b),
                                         alpha(cfg.minimap_frame_alpha));
            const ImU32 inner_ring = IM_COL32(0, 0, 0, alpha(0.55f));

            // The slice texture already carries the floor colour, the height gradient
            // and the per-pixel alpha, so the only tint left is the global opacity.
            const ImU32 tint_slice = IM_COL32(255, 255, 255, alpha(1.0f));
            const ImU32 tint_composite = IM_COL32(255, 255, 255, alpha(cfg.minimap_composite_alpha));

            // The background draw list: over the game, under our own windows (the
            // panel, the full map, the tooltips).
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            if (cfg.round)
            {
                dl->AddCircleFilled(g.center, g.half, backdrop, g_circle_segments);
            }
            else
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, backdrop, 4.0f);
            }

            // One textured quad: the height slice. `update_slice` re-cuts the window
            // around the player at slice_hz and returns true while a window is available
            // (its own or the previous one's - the window carries margin, so a skipped
            // update is invisible).
            const bool slice_ok = plan_slice(cfg, chapter, g.half, now);
            const SliceView sv = slice_view();
            if (slice_ok && sv.shown >= 0)
            {
                const SliceBuf& b = g_slice[sv.shown];
                const UvMap window{sv.min_y, sv.max_x, sv.px_per_uu, b.w, b.h};
                draw_srv(dl, b.srv_gpu, window, g, tint_slice, cfg.round, x0, y0, side);
            }
            else if (composite_ready)
            {
                // No height maps: the Z-shaded composite, which merges every storey.
                draw_image(dl, g_map, uv_of(chapter), g, tint_composite, cfg.round, x0, y0, side);
            }
            else
            {
                set_hide_reason(L"the height slicer has no window yet");
                return;
            }

            if (cfg.round)
            {
                dl->AddCircle(g.center, g.half - 1.0f, inner_ring, g_circle_segments, 2.0f);
                dl->AddCircle(g.center, g.half, frame, g_circle_segments, 2.0f);
            }
            else
            {
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, frame, 4.0f, 0, 2.0f);
            }

            // Markers go over the map and under the frame ring and the player arrow, so
            // the arrow is never hidden by a glyph standing on it.
            draw_markers(cfg, g, cfg.round, x0, y0, side, dl);

            // A ring where something was just collected, so a pickup taken off screen
            // still registers on the minimap.
            draw_found_rings(dl, now, (std::max)(4.0f, cfg.markers_size * 1.4f),
                             [&](double wx, double wy, float& sx, float& sy) {
                                 // Never clamped: a ring on the rim would name the
                                 // wrong place.
                                 const MiniOffset off =
                                     mini_offset(g, wx, wy, cfg.round, g.half - 2.0f, false);
                                 if (!off.visible)
                                 {
                                     return false;
                                 }
                                 sx = g.center.x + static_cast<float>(off.dx);
                                 sy = g.center.y + static_cast<float>(off.dy);
                                 return true;
                             });

            // The cardinal reference, always drawn: four ticks on the rim plus the
            // letter N, rotating with the yaw in rotate mode and standing still (N
            // straight up) in north-up mode.
            {
                // A point `inset` pixels in from the rim - the disc's circle or the
                // square's edge, so the ticks sit on the frame either way.
                const auto rim = [&](float ang, float inset) {
                    const float dx = std::sin(ang);
                    const float dy = -std::cos(ang);
                    const float lim = (std::max)(4.0f, g.half - inset);
                    if (cfg.round)
                    {
                        return ImVec2{g.center.x + dx * lim, g.center.y + dy * lim};
                    }
                    const float m = (std::max)(std::abs(dx), std::abs(dy));
                    const float sc = m > 1e-4f ? lim / m : 0.0f;
                    return ImVec2{g.center.x + dx * sc, g.center.y + dy * sc};
                };
                const ImU32 tick_col = IM_COL32(226, 230, 236, alpha(0.6f));
                const ImU32 north_col = IM_COL32(255, 226, 160, alpha(0.95f));
                for (int i = 0; i < 4; ++i)
                {
                    const float ang = (static_cast<float>(i) * 90.0f - eff_yaw) * kPi / 180.0f;
                    const bool north = i == 0;
                    dl->AddLine(rim(ang, 3.0f), rim(ang, north ? 11.0f : 8.0f),
                                north ? north_col : tick_col, north ? 2.2f : 1.4f);
                }
                const float na = -eff_yaw * kPi / 180.0f;
                const ImVec2 np = rim(na, 19.0f);
                const ImVec2 ts = ImGui::CalcTextSize("N");
                const ImVec2 tp{np.x - ts.x * 0.5f, np.y - ts.y * 0.5f};
                // A shadow rather than a plate, which at the rim would cover the map.
                dl->AddText(ImVec2{tp.x + 1.0f, tp.y + 1.0f}, IM_COL32(0, 0, 0, alpha(0.8f)), "N");
                dl->AddText(tp, north_col, "N");
            }

            // Every waypoint, edge-clamped and never culled; only the nearest one
            // carries the distance readout.
            {
                const mv::WaypointSet set = mm::waypoints();
                const int near_i = mv::nearest_waypoint(set, g.px, g.py);
                for (std::size_t wi = 0; wi < set.count; ++wi)
                {
                    const mv::Waypoint& wp = set.items[wi];
                    const float wr = (std::max)(5.0f, cfg.markers_size * cfg.waypoint_size_scale);
                    const float lim = (std::max)(4.0f, g.half - wr - 3.0f);
                    // Always clamped: an off-map waypoint must still say which way to
                    // walk.
                    const MiniOffset off = mini_offset(g, wp.x, wp.y, cfg.round, lim, true);
                    const ImVec2 wp_pos{g.center.x + static_cast<float>(off.dx),
                                        g.center.y + static_cast<float>(off.dy)};
                    draw_waypoint_glyph(dl, wp_pos, off.clamped ? wr * 0.85f : wr, alpha(1.0f));
                    if (static_cast<int>(wi) != near_i)
                    {
                        continue;
                    }
                    const double wdx = wp.x - g.px;
                    const double wdy = wp.y - g.py;
                    const double dist_m = std::sqrt(wdx * wdx + wdy * wdy) / 100.0;
                    char label[32]{};
                    if (dist_m >= 1000.0)
                    {
                        (void)std::snprintf(label, sizeof(label), "%.1f km", dist_m / 1000.0);
                    }
                    else
                    {
                        (void)std::snprintf(label, sizeof(label), "%.0f m", dist_m);
                    }
                    const ImVec2 ts = ImGui::CalcTextSize(label);
                    const ImVec2 tp{wp_pos.x - ts.x * 0.5f, wp_pos.y + wr * 1.6f};
                    dl->AddRectFilled(ImVec2{tp.x - 3.0f, tp.y - 1.0f}, ImVec2{tp.x + ts.x + 3.0f, tp.y + ts.y + 1.0f},
                                      plate_color(alpha(0.7f)), 3.0f);
                    dl->AddText(tp, IM_COL32(255, 190, 235, alpha(1.0f)), label);
                }
            }

            add_player_arrow(dl, g.center, snap.yaw - eff_yaw,
                             (std::max)(cfg.minimap_arrow_min_px, side * cfg.minimap_arrow_frac));

            // The wheel over the disc, gated on the panel being open. Nothing here
            // swallows the wheel - the WndProc hook only swallows input for a MODE (the
            // full map) - so during play a notch would reach the game's weapon wheel as
            // well. While the F2 panel is up the cursor is already ours. `zoom_key` is
            // the route that works during play.
            if (mm::g_panel_open.load(std::memory_order_relaxed) && !ImGui::GetIO().WantCaptureMouse)
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.MouseWheel != 0.0f)
                {
                    const float mdx = io.MousePos.x - g.center.x;
                    const float mdy = io.MousePos.y - g.center.y;
                    const bool over = cfg.round
                                          ? (mdx * mdx + mdy * mdy) <= g.half * g.half
                                          : (std::abs(mdx) <= g.half && std::abs(mdy) <= g.half);
                    if (over)
                    {
                        // Away from the player = zoom out = a larger uu/px, the same
                        // sign as the full map's wheel.
                        g_zoom_steps.fetch_add(io.MouseWheel > 0.0f ? -1 : 1, std::memory_order_relaxed);
                    }
                }
            }

            set_hide_reason(L"visible");
            g.uv = uv_of(chapter);
            const ImVec2 uv = uv_at(g, 0.0f, 0.0f);
            g_last_mini.visible = true;
            g_last_mini.u = uv.x;
            g_last_mini.v = uv.y;
            g_last_mini.chapter = chapter.key;
            g_last_mini.side = side;
        }

        //==============================================================================
        // Drawing: the X-RAY HIGHLIGHT
        //==============================================================================
        //
        // While the highlight key (or the pad chord) is held, every marker of an enabled
        // category within highlight_radius of the PLAYER is drawn at its projected screen
        // position: category glyph, name, distance in metres, alpha fading with distance.
        // "Through walls" is free - the overlay is composited over the finished frame, so
        // there is no occlusion test, no CustomDepth and no material.
        //
        //   * the camera pose comes from hl::camera() (game thread, see highlight.cpp)
        //     and is required: no fresh pose, no highlight. The pawn's own position and
        //     yaw would put every label a spring-arm's length off;
        //   * the screen size comes from the ImGui viewport, which is the swapchain's -
        //     the rectangle UE built its projection matrix for;
        //   * the radius test is against the player, the projection against the camera,
        //     and the distance shown is the player's.

        // A small outward-pointing triangle at `p`, aimed along (dx, dy) in screen space.
        void add_edge_arrow(ImDrawList* dl, ImVec2 p, float dx, float dy, float r, ImU32 col, ImU32 edge)
        {
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f)
            {
                return;
            }
            const float ux = dx / len;
            const float uy = dy / len;
            const ImVec2 tip{p.x + ux * r, p.y + uy * r};
            const ImVec2 a{p.x - ux * r * 0.6f - uy * r * 0.75f, p.y - uy * r * 0.6f + ux * r * 0.75f};
            const ImVec2 b{p.x - ux * r * 0.6f + uy * r * 0.75f, p.y - uy * r * 0.6f - ux * r * 0.75f};
            dl->AddTriangleFilled(tip, a, b, col);
            dl->AddTriangle(tip, a, b, edge, 1.2f);
        }

        // `const char*`, not std::string: every caller is inside Present and the x-ray
        // builds up to twelve of these a frame.
        void draw_label(ImDrawList* dl, ImVec2 at, const char* text, ImU32 col, int alpha)
        {
            if (text == nullptr || text[0] == '\0')
            {
                return;
            }
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 tp{at.x - ts.x * 0.5f, at.y};
            dl->AddRectFilled(ImVec2{tp.x - 4.0f, tp.y - 1.0f}, ImVec2{tp.x + ts.x + 4.0f, tp.y + ts.y + 1.0f},
                              plate_color(static_cast<int>(alpha * 0.62f)), 3.0f);
            dl->AddText(tp, col, text);
        }

        void draw_highlight(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_hl_debug = HighlightDebug{};
            if (!cfg.overlay_enabled || !cfg.highlight_enabled || !gate_ok)
            {
                return;
            }
            if (!hl::held())
            {
                return;
            }
            g_hl_debug.active = true;

            hl::Pose pose{};
            if (!hl::camera(pose))
            {
                return; // the reader has not produced a pose yet
            }
            const std::uint64_t now = ::GetTickCount64();
            g_hl_debug.cam_age_ms = pose.stamp_ms == 0 ? 0 : now - pose.stamp_ms;
            // A pose older than a few frames means the camera stopped being read (a
            // stalled game thread, a dropped pawn), and labels drawn from it lag visibly
            // behind the scene. 250 ms is ~15 frames.
            if (pose.stamp_ms == 0 || g_hl_debug.cam_age_ms > 250)
            {
                return;
            }
            g_hl_debug.have_camera = true;

            proj::Camera cam{};
            cam.x = pose.x;
            cam.y = pose.y;
            cam.z = pose.z;
            cam.pitch = pose.pitch;
            cam.yaw = pose.yaw;
            cam.roll = pose.roll;
            cam.fov_deg = pose.fov;
            if (!proj::camera_sane(cam))
            {
                return;
            }

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const double screen_w = static_cast<double>(vp->Size.x);
            const double screen_h = static_cast<double>(vp->Size.y);
            if (!(screen_w > 1.0) || !(screen_h > 1.0))
            {
                return;
            }

            if (g_frame_cands.empty())
            {
                return;
            }

            struct Cand
            {
                float d2 = 0.0f; // squared 3D distance from the player
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Cand> cands; // render thread only, reused every frame
            cands.clear();

            // The per-frame marker pass has already walked the buffer and computed the
            // distances; this only filters, and takes the square root for the handful
            // that are drawn in metres.
            const double radius = static_cast<double>(cfg.highlight_radius);
            const float radius2 = static_cast<float>(radius * radius);
            // One gate, and it names its reason: the conditions live in the pure,
            // offline-tested mdb::xray_gate() and every rejection is counted, so "it is
            // on the minimap and not in the x-ray" is answered by the per-round line
            // below rather than by a play session.
            for (const FrameCand& fc : g_frame_cands)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(fc.cat);
                mdb::XrayFacts xf{};
                xf.cat = cat;
                xf.cat_selected = mdb::cat_enabled(cfg.highlight_categories, cat);
                xf.found = fc.found;
                xf.show_found = cfg.highlight_show_found;
                xf.live = (fc.flags & markers::kFlagLive) != 0;
                xf.within_radius = fc.d2_3d <= radius2;
                ++g_hl_debug.gated;
                const mdb::XrayDrop drop = mdb::xray_gate(xf);
                if (drop != mdb::XrayDrop::Drawn)
                {
                    ++g_hl_debug.dropped[static_cast<int>(drop)];
                    continue;
                }
                cands.push_back(Cand{fc.d2_3d, fc.m});
            }
            g_hl_debug.considered = static_cast<int>(cands.size());

            // One sort (nearest first); the draw loop then runs BACKWARDS so the nearest
            // label ends up on top of the pile.
            const std::size_t cap = static_cast<std::size_t>((std::max)(1, cfg.highlight_max_draw));
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            else
            {
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            }

            // The background draw list: over the game, under our own windows (the
            // panel, the full map, the tooltips).
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const float r = cfg.highlight_size;
            const float pad = r * 2.4f;

            // One projection pass, nearest first. The glyphs are drawn from it
            // backwards (far to near) and the labels chosen from it forwards, so the
            // label cap keeps the ones the player is walking towards.
            struct Shown
            {
                float sx = 0.0f; // screen position, viewport-relative already applied
                float sy = 0.0f;
                float nx = 0.0f; // edge direction, when this one is off screen / behind
                float ny = 0.0f;
                double dist = 0.0; // uu from the player
                int alpha = 0;
                ImU32 col = 0;
                std::uint8_t cat = 0;
                bool on_screen = false;
                bool found = false;
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Shown> shown; // render thread only, reused every frame
            shown.clear();

            for (const Cand& cand : cands)
            {
                const double cand_dist = std::sqrt(static_cast<double>(cand.d2));
                const markers::DrawMarker& m = *cand.m;
                const proj::Result pr = proj::project(cam, m.x, m.y, m.z, screen_w, screen_h);
                if (!pr.valid)
                {
                    ++g_hl_debug.no_projection;
                    continue;
                }

                // Fully lit at the camera, highlight_alpha_far at the radius. Linear: a
                // squared falloff makes everything past half the radius look identical.
                const double t = radius > 1.0 ? (cand_dist / radius) : 0.0;
                const double a = static_cast<double>(cfg.highlight_alpha_near) +
                                 (static_cast<double>(cfg.highlight_alpha_far) -
                                  static_cast<double>(cfg.highlight_alpha_near)) *
                                     (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
                const int alpha = static_cast<int>((std::min)(1.0, (std::max)(0.0, a)) * 255.0 + 0.5);
                if (alpha <= 4)
                {
                    ++g_hl_debug.faded_out;
                    continue;
                }
                const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                Shown sh{};
                sh.dist = cand_dist;
                sh.alpha = alpha;
                // While the key is held quality wins over category, so a weapon and a
                // key item stand out from the consumables.
                sh.col = marker_color_q(cat, m.rarity, alpha, cfg.xray_rarity_colors_enabled,
                                        cfg.xray_rarity_colors);
                sh.cat = m.cat;
                sh.found = (m.flags & markers::kFlagFound) != 0;
                sh.m = &m;

                if (pr.on_screen && !pr.behind)
                {
                    sh.on_screen = true;
                    sh.sx = vp->Pos.x + static_cast<float>(pr.sx);
                    sh.sy = vp->Pos.y + static_cast<float>(pr.sy);
                    shown.push_back(sh);
                    continue;
                }
                if (!cfg.highlight_edge_arrows)
                {
                    ++g_hl_debug.offscreen_no_arrow;
                    continue;
                }
                // Off screen or behind: an arrow on the rim. proj::project() returns a
                // direction rather than a mirrored position, so this is a projection
                // onto the border box.
                double nx = pr.ndc_x;
                double ny = pr.ndc_y;
                const double mag = (std::max)(std::fabs(nx), std::fabs(ny));
                if (mag < 1e-6)
                {
                    continue;
                }
                nx /= mag;
                ny /= mag;
                float sx = static_cast<float>((nx * 0.5 + 0.5) * screen_w);
                float sy = static_cast<float>((0.5 - ny * 0.5) * screen_h);
                sh.sx = vp->Pos.x + (std::max)(pad, (std::min)(static_cast<float>(screen_w) - pad, sx));
                sh.sy = vp->Pos.y + (std::max)(pad, (std::min)(static_cast<float>(screen_h) - pad, sy));
                sh.nx = static_cast<float>(nx);
                sh.ny = static_cast<float>(-ny);
                shown.push_back(sh);
            }

            // Glyphs and arrows, far to near.
            for (std::size_t ci = shown.size(); ci-- > 0;)
            {
                const Shown& sh = shown[ci];
                const mdb::Cat cat = static_cast<mdb::Cat>(sh.cat);
                const ImU32 edge = IM_COL32(10, 12, 16, static_cast<int>(sh.alpha * 0.9f));
                if (sh.on_screen)
                {
                    draw_marker_glyph(dl, cat, ImVec2{sh.sx, sh.sy}, sh.found ? r * 0.75f : r, sh.col, edge,
                                      sh.found);
                    ++g_hl_debug.on_screen;
                }
                else
                {
                    const int dim = static_cast<int>(sh.alpha * 0.8f);
                    add_edge_arrow(dl, ImVec2{sh.sx, sh.sy}, sh.nx, sh.ny, r * 1.15f,
                                   marker_color_q(cat, sh.m->rarity, dim, cfg.xray_rarity_colors_enabled,
                                                  cfg.xray_rarity_colors),
                                   IM_COL32(10, 12, 16, dim));
                    ++g_hl_debug.edge;
                }
                ++g_hl_debug.drawn;
            }

            //--------------------------------------------------------------------------
            // LABELS
            //--------------------------------------------------------------------------
            //
            // At most `highlight_labels_max` glyphs get a name+distance box, chosen
            // nearest first and never two for glyphs within `r * 2` of each other -
            // markers a few pixels apart are one thing to the player. The survivors are
            // laid out top to bottom by the pure lbl::Layout, which pushes each box down
            // until it clears the ones already placed; a box that moved gets a leader
            // line back to its glyph.
            if (cfg.highlight_labels && !shown.empty())
            {
                static std::vector<std::size_t> labelled; // render thread only
                labelled.clear();
                lbl::Layout layout{};
                const std::size_t label_cap = static_cast<std::size_t>(
                    (std::max)(0, (std::min)(cfg.highlight_labels_max, lbl::Layout::kMaxRects)));
                for (std::size_t i = 0; i < shown.size() && labelled.size() < label_cap; ++i)
                {
                    const Shown& sh = shown[i];
                    if (!sh.on_screen)
                    {
                        continue; // an edge arrow has no room for a name
                    }
                    if (layout.near_labelled(sh.sx, sh.sy, r * 2.0f))
                    {
                        continue;
                    }
                    layout.note_glyph(sh.sx, sh.sy);
                    labelled.push_back(i);
                }
                // Top to bottom, which makes pushing down terminate and keeps the
                // arrangement stable from frame to frame.
                std::sort(labelled.begin(), labelled.end(), [](std::size_t a, std::size_t b) {
                    return shown[a].sy < shown[b].sy;
                });
                const float line_h = ImGui::GetTextLineHeight() + 2.0f;
                const float max_push = line_h * 8.0f;
                for (const std::size_t i : labelled)
                {
                    const Shown& sh = shown[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(sh.cat);
                    // Never a class name: `mdb::display_label` refuses one and falls
                    // back to the category's plain singular word.
                    const char* name = mdb::display_label(cat, sh.m->label);
                    // A stack buffer, not std::format: this runs up to twelve times per
                    // frame inside Present for a string nobody keeps.
                    char text[128]{};
                    (void)std::snprintf(text, sizeof(text), "%s  %.0f m%s", name, sh.dist / 100.0,
                                        sh.found ? "  (found)" : "");
                    const ImVec2 ts = ImGui::CalcTextSize(text);
                    const float want_y = sh.sy + r + 3.0f;
                    float at_y = want_y;
                    if (!layout.place(sh.sx - ts.x * 0.5f - 4.0f, want_y, ts.x + 8.0f, ts.y + 2.0f, max_push,
                                      at_y))
                    {
                        continue; // no room: the glyph speaks for itself
                    }
                    if (at_y - want_y > 2.0f)
                    {
                        // The leader line, so a displaced label still belongs to a glyph.
                        dl->AddLine(ImVec2{sh.sx, sh.sy + r}, ImVec2{sh.sx, at_y},
                                    IM_COL32(200, 206, 214, static_cast<int>(sh.alpha * 0.55f)), 1.0f);
                    }
                    draw_label(dl, ImVec2{sh.sx, at_y}, text, sh.col, sh.alpha);
                    ++g_hl_debug.labels;
                }
            }

            // The x-ray census: every gate has a number, so "the chest is on the minimap
            // but not in the x-ray" is answered from the log. It is a per-Present line
            // about a steady state, so it is `trace` only, at most once per ten seconds,
            // and only when a counter moved. The level check comes first, so at `normal`
            // this block is one relaxed atomic load per Present.
            if (mm::log_enabled(mm::LogLv::Trace))
            {
                static std::uint64_t last_log = 0;
                static std::uint64_t last_sig = 0;
                std::uint64_t sig = 0;
                for (int i = 0; i < 5; ++i)
                {
                    sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.dropped[i]);
                }
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.gated);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.considered);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.drawn);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.on_screen);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.edge);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.labels);
                if (sig != last_sig && now - last_log >= 10000)
                {
                    last_log = now;
                    last_sig = sig;
                    mm::logf(L"x-ray: {} published, dropped {} by category / {} found / {} not live / "
                             L"{} out of radius ({:.0f} m); of {} left, {} drawn ({} on screen, "
                             L"{} rim arrow, {} labelled), skipped {} behind the camera, {} off "
                             L"screen with arrows off, {} faded out",
                             g_hl_debug.gated,
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Category)],
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Found)],
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Live)],
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Radius)],
                             radius / 100.0,
                             g_hl_debug.considered,
                             g_hl_debug.drawn,
                             g_hl_debug.on_screen,
                             g_hl_debug.edge,
                             g_hl_debug.labels,
                             g_hl_debug.no_projection,
                             g_hl_debug.offscreen_no_arrow,
                             g_hl_debug.faded_out);
                }
            }
        }

        //==============================================================================
        // Drawing: the COMPASS STRIP
        //==============================================================================
        //
        // A heading strip across the top of the screen. The arithmetic (wrap, bearing,
        // strip position, tick layout) is in src/compass.cpp, where markers_test can
        // reach it; this is the drawing and nothing else.
        //
        // The heading is the camera's yaw when a fresh pose exists, because that is what
        // the player is looking along, and the pawn's yaw otherwise - so the compass
        // works with highlight_enabled = 0 and during the camera reader's warm-up. Which
        // one is in use is printed in the F2 debug block.

        void draw_compass(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_compass_debug = CompassDebug{};
            if (!cfg.overlay_enabled || !cfg.compass_enabled || !gate_ok)
            {
                return;
            }

            const std::uint64_t now = ::GetTickCount64();
            double heading = static_cast<double>(snap.yaw);
            bool from_camera = false;
            hl::Pose pose{};
            if (hl::camera(pose) && pose.stamp_ms != 0 && now - pose.stamp_ms <= 1000)
            {
                heading = pose.yaw;
                from_camera = true;
            }
            g_compass_debug.heading = cmp::wrap360(heading);
            g_compass_debug.from_camera = from_camera;

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float screen_w = vp->Size.x;
            const float screen_h = vp->Size.y;
            const float width = (std::max)(120.0f, cfg.compass_width * screen_w);
            const float height = cfg.compass_height;
            const float x0 = vp->Pos.x + (screen_w - width) * 0.5f;
            // compass_offset_y is the distance from whichever edge the strip hangs off,
            // so the key means the same thing in both directions.
            const float y0 = compass_at_bottom(cfg)
                                 ? vp->Pos.y + screen_h - cfg.compass_offset_y - height
                                 : vp->Pos.y + cfg.compass_offset_y;
            const float y1 = y0 + height;

            const float op = cfg.compass_opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };

            // The background draw list: over the game, under our own windows (the
            // panel, the full map, the tooltips).
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            // The plate. `compass_plate = 0` leaves ticks and letters only, so the strip
            // sits lightly over the game's own top-centre HUD; with no plate every glyph
            // gets a one-pixel shadow instead, or a bright scene swallows it.
            if (cfg.compass_plate)
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, plate_color(alpha(0.72f)), 4.0f);
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, IM_COL32(150, 158, 168, alpha(0.55f)),
                            4.0f, 0, 1.2f);
            }
            const bool shadow = !cfg.compass_plate;
            const auto shadowed_line = [&](ImVec2 sa, ImVec2 sb, ImU32 scol, float thick) {
                if (shadow)
                {
                    dl->AddLine(ImVec2{sa.x + 1.0f, sa.y + 1.0f}, ImVec2{sb.x + 1.0f, sb.y + 1.0f},
                                IM_COL32(0, 0, 0, alpha(0.75f)), thick);
                }
                dl->AddLine(sa, sb, scol, thick);
            };
            const auto shadowed_text = [&](ImVec2 at, ImU32 scol, const char* text) {
                if (shadow)
                {
                    dl->AddText(ImVec2{at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, alpha(0.8f)), text);
                }
                dl->AddText(at, scol, text);
            };

            cmp::Strip strip{};
            strip.x0 = static_cast<double>(x0);
            strip.width = static_cast<double>(width);
            strip.heading = heading;
            strip.span = static_cast<double>(cfg.compass_span_deg);

            // Ticks. A cardinal gets the full height and its letter, an intercardinal a
            // shorter line and its two-letter label, everything else a stub.
            cmp::Tick ticks[128]{};
            const int n = cmp::ticks(strip, ticks, static_cast<int>(std::size(ticks)),
                                     static_cast<double>(cfg.compass_tick_step_deg));
            for (int i = 0; i < n; ++i)
            {
                const cmp::Tick& t = ticks[i];
                const float x = static_cast<float>(t.x);
                const float len = t.rank == 2 ? height * 0.5f : (t.rank == 1 ? height * 0.34f : height * 0.22f);
                const int a = t.rank == 2 ? alpha(0.95f) : (t.rank == 1 ? alpha(0.75f) : alpha(0.45f));
                shadowed_line(ImVec2{x, y1 - len}, ImVec2{x, y1 - 2.0f}, IM_COL32(226, 230, 236, a),
                              t.rank == 2 ? 2.0f : 1.2f);
                if (t.label[0] != '\0')
                {
                    const ImVec2 ts = ImGui::CalcTextSize(t.label);
                    shadowed_text(ImVec2{x - ts.x * 0.5f, y0 + 1.0f},
                                  IM_COL32(240, 242, 246, t.rank == 2 ? alpha(1.0f) : alpha(0.8f)),
                                  t.label);
                }
            }

            // The centre reticle: what the player is actually facing.
            {
                const float cx = x0 + width * 0.5f;
                dl->AddTriangleFilled(ImVec2{cx, y1 - 1.0f}, ImVec2{cx - 5.0f, y1 + 7.0f},
                                      ImVec2{cx + 5.0f, y1 + 7.0f}, IM_COL32(255, 236, 180, alpha(0.95f)));
            }

            if (!snap.has_pawn)
            {
                g_compass_debug.visible = true;
                return;
            }

            // Marker pips, nearest first so the cap keeps what matters, and only inside
            // the strip's span - clamped off-strip pips pile into a block at both ends.
            if (!g_frame_cands.empty())
            {
                struct Pip
                {
                    float d2 = 0.0f; // squared: only ever compared and sorted on
                    double x = 0.0;  // where on the strip it lands, in screen px
                    float dz = 0.0f; // marker Z minus player Z, uu (signed)
                    std::uint8_t cat = 0;
                    std::uint8_t rarity = 0;
                    bool found = false;
                };
                static std::vector<Pip> pips; // render thread only
                pips.clear();
                // build_frame_candidates has already walked the buffer and computed the
                // distances; this only filters.
                const double max_d = static_cast<double>(cfg.compass_marker_distance);
                const float max_d2 = static_cast<float>(max_d * max_d);
                for (const FrameCand& fc : g_frame_cands)
                {
                    if (!mdb::cat_enabled(cfg.compass_categories, static_cast<mdb::Cat>(fc.cat)))
                    {
                        continue;
                    }
                    if (fc.d2_xy > max_d2)
                    {
                        continue;
                    }
                    const markers::DrawMarker& m = *fc.m;
                    double px = 0.0;
                    double rel = 0.0;
                    // Off-strip pips are dropped here rather than in the draw loop, so
                    // the cap and the dedupe below work on pips that will be drawn.
                    if (!cmp::strip_x(strip, cmp::bearing_deg(snap.x, snap.y, m.x, m.y), px, rel))
                    {
                        continue;
                    }
                    Pip p{};
                    p.d2 = fc.d2_xy;
                    p.x = px;
                    p.dz = static_cast<float>(m.z - snap.z);
                    p.cat = fc.cat;
                    p.rarity = fc.rarity;
                    p.found = fc.found;
                    pips.push_back(p);
                }
                // One sort, nearest first; drawn back to front so the nearest pip ends
                // up on top.
                const std::size_t kMaxPips = static_cast<std::size_t>(cfg.compass_max_pips);
                if (pips.size() > kMaxPips)
                {
                    std::partial_sort(pips.begin(), pips.begin() + kMaxPips, pips.end(),
                                      [](const Pip& a, const Pip& b) { return a.d2 < b.d2; });
                    pips.resize(kMaxPips);
                }
                else
                {
                    std::sort(pips.begin(), pips.end(), [](const Pip& a, const Pip& b) { return a.d2 < b.d2; });
                }
                // Dedupe: six chests in one room are six pips within a pixel of each
                // other. The list is sorted nearest first, so keeping the first pip in
                // each 3-px column keeps the nearest of every cluster; the walk is
                // O(n x kept) over at most compass_max_pips entries.
                constexpr double kDedupePx = 3.0;
                std::size_t kept = 0;
                for (std::size_t i = 0; i < pips.size(); ++i)
                {
                    bool crowded = false;
                    for (std::size_t j = 0; j < kept; ++j)
                    {
                        if (pips[j].cat == pips[i].cat &&
                            std::abs(pips[j].x - pips[i].x) < kDedupePx)
                        {
                            crowded = true;
                            break;
                        }
                    }
                    if (!crowded)
                    {
                        pips[kept++] = pips[i];
                    }
                }
                pips.resize(kept);
                g_compass_debug.deduped = static_cast<int>(pips.size());

                // Back to front, so the nearest pip of a cluster ends up on top.
                for (std::size_t pi = pips.size(); pi-- > 0;)
                {
                    const Pip& p = pips[pi];
                    const int a = p.found ? alpha(0.35f) : alpha(1.0f);
                    const ImU32 col = marker_color_q(static_cast<mdb::Cat>(p.cat), p.rarity, a,
                                                     cfg.markers_rarity_tint, cfg.xray_rarity_colors);
                    const ImVec2 at{static_cast<float>(p.x), y1 - height * 0.30f};
                    const float gr = height * 0.22f;
                    draw_marker_glyph(dl, static_cast<mdb::Cat>(p.cat), at, gr, col,
                                      IM_COL32(10, 12, 16, a), p.found);
                    // Above / below: a bearing alone sends the player at a wall when the
                    // chest is on the floor over their head, so a marker further than
                    // compass_pip_height_uu off the player's own Z gets an arrow beside
                    // its glyph. Within that band it counts as this floor and gets none.
                    const float thr = cfg.compass_pip_height_uu;
                    if (thr > 0.0f && (p.dz > thr || p.dz < -thr))
                    {
                        const float ar = (std::max)(2.5f, height * 0.15f);
                        const float ax = at.x + gr + ar * 0.9f;
                        const float up = p.dz > 0.0f ? -1.0f : 1.0f;
                        const ImU32 acol = IM_COL32(246, 246, 250, a);
                        dl->AddTriangleFilled(ImVec2{ax, at.y + up * ar},
                                              ImVec2{ax - ar * 0.8f, at.y - up * ar * 0.55f},
                                              ImVec2{ax + ar * 0.8f, at.y - up * ar * 0.55f}, acol);
                    }
                    ++g_compass_debug.pips;
                }

                // The distance labels, nearest first so a crowded strip keeps the ones
                // that matter. They sit outside the strip (below it, or above it when the
                // strip is anchored to the bottom edge), so they cannot collide with the
                // ticks and the cardinal letters, and each reserves its own x range.
                if (cfg.compass_pip_labels && !pips.empty())
                {
                    const bool at_bottom = compass_at_bottom(cfg);
                    const float text_h = ImGui::GetTextLineHeight();
                    const float label_y = at_bottom ? y0 - text_h - 1.0f : y1 + 1.0f;
                    static std::vector<std::pair<float, float>> taken; // render thread only
                    taken.clear();
                    for (const Pip& p : pips)
                    {
                        char text[16]{};
                        ::_snprintf_s(text, sizeof(text), _TRUNCATE, "%.0fm",
                                      static_cast<double>(std::sqrt(p.d2)) / 100.0);
                        const float tw = ImGui::CalcTextSize(text).x;
                        const float lx = static_cast<float>(p.x) - tw * 0.5f;
                        const float rx = lx + tw;
                        bool crowded = false;
                        for (const std::pair<float, float>& r : taken)
                        {
                            if (lx < r.second + 2.0f && r.first < rx + 2.0f)
                            {
                                crowded = true;
                                break;
                            }
                        }
                        if (crowded)
                        {
                            continue;
                        }
                        taken.emplace_back(lx, rx);
                        const int la = p.found ? alpha(0.45f) : alpha(0.9f);
                        dl->AddText(ImVec2{lx + 1.0f, label_y + 1.0f}, IM_COL32(0, 0, 0, la), text);
                        dl->AddText(ImVec2{lx, label_y}, IM_COL32(226, 230, 236, la), text);
                    }
                }
            }

            // A waypoint is never culled: being told which way to walk while it is off
            // the strip is the whole point, so it clamps to the edge instead. Only the
            // nearest one carries the distance readout.
            const mv::WaypointSet wps = mm::waypoints();
            const int near_wp = mv::nearest_waypoint(wps, snap.x, snap.y);
            for (std::size_t wi = 0; cfg.compass_show_waypoint && wi < wps.count; ++wi)
            {
                const mv::Waypoint& wp = wps.items[wi];
                double x = 0.0;
                double rel = 0.0;
                const bool inside = cmp::strip_x(strip, cmp::bearing_deg(snap.x, snap.y, wp.x, wp.y), x, rel);
                const float wx = static_cast<float>(x);
                const ImVec2 at{wx, y1 - height * 0.30f};
                draw_waypoint_glyph(dl, at, height * 0.24f, alpha(1.0f));
                if (!inside)
                {
                    add_edge_arrow(dl, ImVec2{wx + (rel < 0.0 ? -8.0f : 8.0f), at.y}, rel < 0.0 ? -1.0f : 1.0f,
                                   0.0f, 5.0f, IM_COL32(255, 190, 235, alpha(0.9f)),
                                   IM_COL32(10, 12, 16, alpha(0.9f)));
                }
                ++g_compass_debug.pips;
                if (static_cast<int>(wi) != near_wp)
                {
                    continue;
                }
                const double dxw = wp.x - snap.x;
                const double dyw = wp.y - snap.y;
                const double metres = std::sqrt(dxw * dxw + dyw * dyw) / 100.0;
                char text[32]{};
                if (metres >= 1000.0)
                {
                    (void)std::snprintf(text, sizeof(text), "%.1f km", metres / 1000.0);
                }
                else
                {
                    (void)std::snprintf(text, sizeof(text), "%.0f m", metres);
                }
                draw_label(dl, ImVec2{wx, y1 + 9.0f}, text, IM_COL32(255, 190, 235, alpha(1.0f)), alpha(1.0f));
            }

            g_compass_debug.visible = true;
        }
    } // namespace ovl
} // namespace overlay
