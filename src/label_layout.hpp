#pragma once

//
// label_layout - greedy non-overlap placement for the x-ray highlight's labels: keep
// the rectangles already used, push each new label DOWN until it lands clear, give up
// past a cap. No two placed labels ever overlap.
//
// The caller (overlay.cpp):
//   1. picks who gets a label NEAREST FIRST, skipping any glyph within `min_glyph_dist`
//      of one that already has one (note_glyph / near_labelled);
//   2. sorts the survivors by screen Y and place()s them top to bottom, which is what
//      makes pushing down terminate and the result stable frame to frame;
//   3. draws a leader line whenever place() moved a label away from its glyph.
//
// No Windows, no ImGui, no allocation.
//

#include <cstddef>

namespace lbl
{
    struct Rect
    {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;

        bool intersects(const Rect& o) const
        {
            // Touching edges do not count: flush labels are readable.
            return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
        }
    };

    // Occupied rectangles plus labelled glyphs. Fixed size: it lives on the render
    // thread's stack and is reset every frame. Past the cap the caller draws the glyph
    // alone.
    struct Layout
    {
        static constexpr int kMaxRects = 40;

        Rect rects[kMaxRects]{};
        int rect_count = 0;
        float gx[kMaxRects]{};
        float gy[kMaxRects]{};
        int glyph_count = 0;

        void reset()
        {
            rect_count = 0;
            glyph_count = 0;
        }

        bool full() const
        {
            return rect_count >= kMaxRects;
        }

        // Remember that the glyph at (x, y) got a label.
        void note_glyph(float x, float y)
        {
            if (glyph_count < kMaxRects)
            {
                gx[glyph_count] = x;
                gy[glyph_count] = y;
                ++glyph_count;
            }
        }

        // True when a labelled glyph already sits within `min_dist` of (x, y).
        bool near_labelled(float x, float y, float min_dist) const
        {
            if (!(min_dist > 0.0f))
            {
                return false;
            }
            const float d2 = min_dist * min_dist;
            for (int i = 0; i < glyph_count; ++i)
            {
                const float dx = gx[i] - x;
                const float dy = gy[i] - y;
                if (dx * dx + dy * dy < d2)
                {
                    return true;
                }
            }
            return false;
        }

        // Places a `w` x `h` label preferring top-left (x, y), pushing it down until it
        // clears every placed rectangle. False, recording nothing, at the cap or past
        // `max_push` pixels of movement. `out_y` receives the final top edge.
        bool place(float x, float y, float w, float h, float max_push, float& out_y)
        {
            if (rect_count >= kMaxRects || !(w > 0.0f) || !(h > 0.0f))
            {
                return false;
            }
            const float start = y;
            Rect r{x, y, x + w, y + h};
            // Each iteration jumps below the deepest rectangle hit, so the loop runs at
            // most once per rectangle.
            for (int guard = 0; guard <= kMaxRects; ++guard)
            {
                float lowest = r.y0;
                bool hit = false;
                for (int i = 0; i < rect_count; ++i)
                {
                    if (r.intersects(rects[i]) && rects[i].y1 > lowest)
                    {
                        lowest = rects[i].y1;
                        hit = true;
                    }
                }
                if (!hit)
                {
                    if (r.y0 - start > max_push)
                    {
                        return false;
                    }
                    rects[rect_count++] = r;
                    out_y = r.y0;
                    return true;
                }
                r.y0 = lowest + 1.0f;
                r.y1 = r.y0 + h;
                if (r.y0 - start > max_push)
                {
                    return false;
                }
            }
            return false;
        }
    };
} // namespace lbl
