#pragma once

//
// label_layout - keeping the x-ray highlight's labels off each other, as pure C++.
//
// WHAT IT FIXES
// -------------
// `draw_highlight` drew a name + distance box at every projected marker position with
// no overlap test at all, so a room with eight pickups produced eight boxes stacked on
// the same few pixels: unreadable, and the review of v0.9.1 called it out. The fix is
// the classic greedy one - keep the rectangles already used, push each new label DOWN
// until it lands clear, give up past a cap - and it is entirely arithmetic, so it lives
// here where tests/markers_test.cpp can prove the property that matters: no two placed
// labels overlap, ever.
//
// HOW THE CALLER USES IT (overlay.cpp)
//   1. pick who gets a label at all, NEAREST FIRST, skipping any glyph that sits within
//      `min_glyph_dist` of one that already has one (note_glyph / near_labelled) - so
//      the labels that survive the cap are the ones the player is walking towards;
//   2. sort the survivors by screen Y and place() them top to bottom, which is what
//      makes pushing DOWN terminate and makes the result stable frame to frame;
//   3. draw a leader line whenever place() moved a label away from its glyph.
//
// No Windows, no ImGui, no allocation - the same rule as markers_db / mapview / glyphs.
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
            // Touching edges do NOT count as an intersection: two labels flush against
            // each other are readable, and treating them as a clash would push a whole
            // column one pixel further down for nothing.
            return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
        }
    };

    // The occupied-rectangle list plus the labelled-glyph list. A fixed size on purpose:
    // it lives on the render thread's stack, is reset every frame, and past ~40 labels
    // the screen is full anyway - the caller then draws the glyph alone, which is the
    // honest answer to "there are sixty items in this room".
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

        // Is there already a labelled glyph within `min_dist` of (x, y)? Two markers a
        // few pixels apart on screen are one thing to the player, and labelling both
        // just makes two boxes fight.
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

        // Place a `w` x `h` label whose preferred top-left is (x, y), pushing it DOWN
        // until it clears every rectangle already placed. Returns false - and records
        // nothing - when the cap is reached or when it would have to move more than
        // `max_push` pixels; the caller then draws the glyph without a label.
        //
        // `out_y` receives the final top edge, which the caller compares against `y` to
        // decide whether a leader line is needed.
        bool place(float x, float y, float w, float h, float max_push, float& out_y)
        {
            if (rect_count >= kMaxRects || !(w > 0.0f) || !(h > 0.0f))
            {
                return false;
            }
            const float start = y;
            Rect r{x, y, x + w, y + h};
            // Each iteration jumps below the DEEPEST rectangle currently hit, so this
            // cannot loop more times than there are rectangles - it is not a scan in
            // one-pixel steps.
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
