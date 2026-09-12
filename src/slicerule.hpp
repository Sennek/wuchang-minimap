#pragma once

//
// slicerule - the height slicer's per-pixel decision, as pure C++.
//
// The map asset carries the Z of up to eight stacked walkable surfaces per pixel plus a
// REACHABLE bit (src/mapdata.hpp). This header holds what the slicer decides from that
// and nothing else - no Windows, no D3D12 - so tests/markers_test.cpp exercises the
// exact rule the minimap and the full map run.
//
// Shading is hypsometric: the COLOUR of a pixel is its surface's absolute world Z on a
// two-colour ramp, one ramp for all three classes and nothing else on top of it. Height
// alone carries the picture, so a staircase, a slope and the storey below all read by
// where they sit, and the player's own storey is found by the marker at its centre.
//
// Per pixel, over the surfaces stored under it, with feet = the player's feet Z:
//
//     any surface with |Z - feet| <= tol
//         -> class FLOOR, the one NEAREST the feet. Everything above is a ceiling, ignored.
//     else any surface in (feet + tol, feet + above_band]
//         -> class ABOVE, the LOWEST such one: a ledge or upper terrain the player walks up
//            to. Surfaces below it are ignored - the ledge hides them.
//     else any surface below feet - tol
//         -> class BELOW, the HIGHEST one. No lower bound: its colour says how deep it is.
//     else nothing is drawn.
//
// A floor underfoot wins outright, so an upper deck over the player's own storey shows only
// through the holes in that storey - a gallery over a solid floor is invisible from under it.
// That is the price of never drawing a ceiling over the player, and why the band overhead is
// one storey (600 uu): it is for the ledge or upper terrain the player sees across, not a
// whole floor above.
//
// Class priority is FLOOR > ABOVE > BELOW, which is the order of the class constants; a
// REACHABLE surface beats an unreachable one of the same class, so the rank is
// `class * 2 + reachable` and one comparison settles both orderings. Equal ranks are
// settled by height, in whichever direction that class wants.
//
// `map_unreachable` decides what an unreachable surface is worth:
//
//     Hide - it is not a surface at all
//     Dim  - it is drawn one rung further down the same opacity ladder
//     Show - it is drawn exactly like a reachable one
//
// A flat slab one storey up is a single Z and therefore a single flat tone, so the ramp
// draws no edge where two storeys abut. `seam_factor` is that edge: a pixel whose left or
// up neighbour sits more than `kSeamStepUu` away in Z is darkened, which is the only
// boundary cue the cut has and what keeps abutting slabs from reading as one surface.
//
// The ramp's two ends are percentiles of the Z of the pixels a cut actually DRAWS
// (`ZHistogram` + `hist_range`), never the asset's raw Z range: one deep pit or one high
// gallery must not push every playable storey into two colour levels. The minimap widens
// that span to `min_range_uu` and eases it through `ease_range`.
//
// The full map runs the same rule with two settings of its own, because it is a picture
// of a whole chapter and not a window on the player's storey:
//
//     above_band unbounded - away from the player every pixel takes the ground of the
//         nearest storey at or above the feet. The priority above is what keeps that
//         honest: a floor underfoot still wins, so a ceiling never covers the player.
//         A one-storey band would cull most of a chapter.
//     equalize - `t` is the cut's own CDF (`ZHistogram::cdf`) instead of a linear
//         position between the percentile ends. Over ten kilometres of chapter a linear
//         ramp spends its contrast on the tails and leaves the playable storeys inside
//         a couple of tones; the CDF spends it where the area is.
//

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace srule
{
    // What to do with a surface the reachability flood never reached.
    enum class Unreachable : std::uint8_t
    {
        Hide = 0,
        Dim = 1,
        Show = 2,
    };

    inline const char* unreachable_name(Unreachable u)
    {
        return u == Unreachable::Show ? "show" : (u == Unreachable::Dim ? "dim" : "hide");
    }

    inline bool unreachable_from_name(std::string_view name, Unreachable& out)
    {
        char buf[16]{};
        std::size_t n = 0;
        for (const char c : name)
        {
            if (c == ' ' || c == '\t' || c == '-' || c == '_')
            {
                continue;
            }
            if (n + 1 >= sizeof(buf))
            {
                return false;
            }
            buf[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        const std::string_view v{buf, n};
        if (v == "hide")
        {
            out = Unreachable::Hide;
            return true;
        }
        if (v == "dim")
        {
            out = Unreachable::Dim;
            return true;
        }
        if (v == "show")
        {
            out = Unreachable::Show;
            return true;
        }
        return false;
    }

    // How the height slice is shaded. The same formula as the offline preview
    // (tools/navmesh/slice_preview.py), so a reported spot reproduces without the game.
    struct SliceStyle
    {
        // The ramp's ends, 0..255 per channel: lo is z_lo, hi is z_hi.
        float lo_r = 88.0f;
        float lo_g = 84.0f;
        float lo_b = 78.0f;
        float hi_r = 244.0f;
        float hi_g = 240.0f;
        float hi_b = 232.0f;
        float gamma = 0.80f;       // t -> pow(t, gamma) along the ramp
        float tol = 200.0f;        // uu: |Z - feetZ| within this is my own floor
        float above_band = 600.0f; // uu ABOVE THE FEET a ledge may sit and still show; 0 = none
        float a_below = 1.0f;
        float a_above = 1.0f;
        // The world Z the ramp's two ends stand for. The caller sets them per cut.
        float z_lo = 0.0f;
        float z_hi = 1.0f;
        float range_pct_lo = 3.0f;      // ramp ends = p(this) .. p(100 - this) of the drawn Z
        float min_range_uu = 400.0f;    // narrowest window span the ramp is stretched over
        float range_smooth_ms = 400.0f; // time constant of the easing on that span
        // Spend the ramp on area rather than on height: t is the cut's own CDF at z.
        // The full map's mode; the minimap stays linear.
        bool equalize = false;
        Unreachable unreachable = Unreachable::Hide;
    };

    // 3 = my floor, 2 = a ledge above, 1 = below, 0 = out of range. The numbers ARE the
    // priority (see the per-pixel table in the file header).
    constexpr std::uint8_t kClassNone = 0;
    constexpr std::uint8_t kClassBelow = 1;
    constexpr std::uint8_t kClassAbove = 2;
    constexpr std::uint8_t kClassFloor = 3;

    // `d` is surfaceZ - feetZ.
    inline std::uint8_t classify(float d, const SliceStyle& st)
    {
        if (d > st.tol)
        {
            return d <= st.above_band ? kClassAbove : kClassNone;
        }
        return d >= -st.tol ? kClassFloor : kClassBelow;
    }

    // The storey seam: a Z step bigger than this between drawn neighbours darkens the pixel.
    constexpr float kSeamStepUu = 300.0f;
    constexpr float kSeamDarken = 0.45f;

    inline std::uint8_t rank_of(std::uint8_t cls, bool reachable)
    {
        return static_cast<std::uint8_t>(cls * 2 + (reachable ? 1 : 0));
    }

    inline std::uint8_t rank_class(std::uint8_t rank)
    {
        return static_cast<std::uint8_t>(rank >> 1);
    }

    inline bool rank_reachable(std::uint8_t rank)
    {
        return (rank & 1u) != 0;
    }

    // Which of two surfaces of the same class and reachability the pixel keeps: the one
    // nearest the feet on my own storey, the lowest of a stack overhead, the highest of
    // a stack below.
    inline bool prefer_d(std::uint8_t cls, float d, float best_d)
    {
        if (cls == kClassFloor)
        {
            return std::fabs(d) < std::fabs(best_d);
        }
        return cls == kClassAbove ? d < best_d : d > best_d;
    }

    // Folds one surface into a pixel's running answer. `rank` / `best_d` start at 0 and
    // are the pixel's state; `d` is surfaceZ - feetZ.
    inline void accumulate(std::uint8_t& rank, float& best_d, float d, bool reachable,
                           const SliceStyle& st)
    {
        bool reach = reachable;
        if (st.unreachable == Unreachable::Show)
        {
            reach = true;
        }
        else if (st.unreachable == Unreachable::Hide && !reach)
        {
            return;
        }
        const std::uint8_t cls = classify(d, st);
        if (cls == kClassNone)
        {
            return;
        }
        const std::uint8_t r = rank_of(cls, reach);
        if (r > rank)
        {
            rank = r;
            best_d = d;
            return;
        }
        if (r == rank && prefer_d(cls, d, best_d))
        {
            best_d = d;
        }
    }

    // The opacity ladder. An unreachable surface takes the next rung down, so it reads
    // as present-but-not-yours without a second colour ramp.
    inline float alpha_for(std::uint8_t cls, bool reachable, const SliceStyle& st)
    {
        switch (cls)
        {
        case kClassFloor:
            return reachable ? 1.0f : st.a_below;
        case kClassBelow:
            return reachable ? st.a_below : st.a_above;
        case kClassAbove:
            return reachable ? st.a_above : st.a_above * 0.6f;
        default:
            return 0.0f;
        }
    }

    // An area-weighted histogram of the Z of the pixels one cut draws. The ramp's ends
    // come out of it as percentiles, so a single pit or gallery cannot set them and
    // flatten everything else onto two tones. Bins span the ASSET's Z range, coarse on
    // purpose: a bin is a few tens of uu, well under what the eye reads off the ramp.
    struct ZHistogram
    {
        static constexpr int kBins = 128;

        float lo = 0.0f;
        float hi = 1.0f;
        std::uint32_t bins[kBins]{};
        // Running sum of `bins`, cum[i] = everything below bin i. Filled by build_cdf()
        // once a cut has counted every pixel, so cdf() costs two loads instead of a
        // 128-bin walk per pixel.
        std::uint32_t cum[kBins + 1]{};
        std::uint32_t total = 0;
        bool cum_ready = false;

        void reset(float z_min, float z_max)
        {
            lo = z_min;
            hi = z_max > z_min + 1.0e-3f ? z_max : z_min + 1.0f;
            total = 0;
            cum_ready = false;
            for (int i = 0; i < kBins; ++i)
            {
                bins[i] = 0;
                cum[i] = 0;
            }
            cum[kBins] = 0;
        }

        void add(float z)
        {
            const float t = (z - lo) / (hi - lo);
            int i = static_cast<int>(t * static_cast<float>(kBins));
            i = i < 0 ? 0 : (i >= kBins ? kBins - 1 : i);
            ++bins[i];
            ++total;
            cum_ready = false;
        }

        void build_cdf()
        {
            std::uint32_t seen = 0;
            for (int i = 0; i < kBins; ++i)
            {
                cum[i] = seen;
                seen += bins[i];
            }
            cum[kBins] = seen;
            cum_ready = true;
        }

        // The fraction of the counted area lying below `z`, interpolated inside the bin
        // `z` falls in. This is the equalised ramp: it rises only where there is area,
        // so a stretch of Z nothing was drawn at costs no contrast at all, and every
        // tenth of the ramp holds a tenth of the pixels. Needs build_cdf() first.
        float cdf(float z) const
        {
            if (total == 0 || !cum_ready)
            {
                return 0.5f;
            }
            const float width = (hi - lo) / static_cast<float>(kBins);
            const float pos = (z - lo) / width;
            int i = static_cast<int>(pos);
            i = i < 0 ? 0 : (i >= kBins ? kBins - 1 : i);
            float frac = pos - static_cast<float>(i);
            frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
            const float below = static_cast<float>(cum[i]) + frac * static_cast<float>(bins[i]);
            const float t = below / static_cast<float>(total);
            return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }

        // The world Z below which `pct` per cent of the counted area lies, interpolated
        // inside the bin it falls in so the ramp does not step as the window moves.
        float percentile(float pct) const
        {
            if (total == 0)
            {
                return lo;
            }
            const float p = pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct);
            const float target = static_cast<float>(total) * p * 0.01f;
            const float width = (hi - lo) / static_cast<float>(kBins);
            float seen = 0.0f;
            for (int i = 0; i < kBins; ++i)
            {
                const float c = static_cast<float>(bins[i]);
                if (c <= 0.0f)
                {
                    continue;
                }
                if (seen + c >= target)
                {
                    const float frac = (target - seen) / c;
                    return lo + (static_cast<float>(i) + frac) * width;
                }
                seen += c;
            }
            return hi;
        }
    };

    // The ramp's two ends for one cut: p(pct_lo) and p(100 - pct_lo). False when the cut
    // drew nothing, which leaves the caller's previous answer standing.
    inline bool hist_range(const ZHistogram& h, float pct_lo, float& out_lo, float& out_hi)
    {
        if (h.total == 0)
        {
            return false;
        }
        const float p = pct_lo < 0.0f ? 0.0f : (pct_lo > 49.0f ? 49.0f : pct_lo);
        out_lo = h.percentile(p);
        out_hi = h.percentile(100.0f - p);
        return out_hi >= out_lo;
    }

    // Where a world Z lands on the ramp, 0 at the low end and 1 at the high one. Two modes,
    // chosen by `st.equalize`:
    //
    //     linear     - (z - z_lo) / (z_hi - z_lo), clamped. `eq` is ignored.
    //     equalised  - the cut's own CDF at z, so the ramp is spent in proportion to the area
    //                  at each height instead of to the height itself.
    //
    // `eq` is the histogram of the cut being painted, already through build_cdf(); a null one
    // (or an empty cut) falls back to linear, so a caller with no histogram still gets a
    // picture. Gamma applies to both.
    inline float shade_t(float z, const SliceStyle& st, const ZHistogram* eq = nullptr)
    {
        float t = 0.5f;
        if (st.equalize && eq != nullptr && eq->total > 0 && eq->cum_ready)
        {
            t = eq->cdf(z);
        }
        else
        {
            const float span = st.z_hi - st.z_lo;
            if (!(span > 1.0e-3f))
            {
                return 0.5f; // a chapter or a window with no height in it at all
            }
            t = (z - st.z_lo) / span;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }
        if (st.gamma != 1.0f && st.gamma > 0.0f)
        {
            t = std::pow(t, st.gamma);
        }
        return t;
    }

    // The ramp colour of a world Z, 0..255 per channel.
    inline void shade_rgb(float z, const SliceStyle& st, float& r, float& g, float& b,
                          const ZHistogram* eq = nullptr)
    {
        const float t = shade_t(z, st, eq);
        r = st.lo_r + (st.hi_r - st.lo_r) * t;
        g = st.lo_g + (st.hi_g - st.lo_g) * t;
        b = st.lo_b + (st.hi_b - st.lo_b) * t;
    }

    // What a pixel is actually painted with. One ramp serves every class: a height is the
    // same colour whoever is standing where, so the cut reads as one floor plan.
    inline void class_rgb(float z, std::uint8_t cls, const SliceStyle& st, float& r, float& g,
                          float& b, const ZHistogram* eq = nullptr)
    {
        (void)cls;
        shade_rgb(z, st, r, g, b, eq);
    }

    // Colour factor for the seam. Only the left and up neighbours are asked, so a step
    // draws one line, on its far side; an undrawn neighbour is empty space and bounds nothing.
    inline float seam_factor(float z, float left_z, bool left_drawn, float up_z, bool up_drawn)
    {
        const bool step = (left_drawn && std::fabs(z - left_z) > kSeamStepUu) ||
                          (up_drawn && std::fabs(z - up_z) > kSeamStepUu);
        return step ? kSeamDarken : 1.0f;
    }

    // Widens a measured span to at least `min_range_uu` about its own centre, so flat
    // ground does not explode to full contrast.
    inline void widen_range(float& lo, float& hi, float min_range_uu)
    {
        const float min_range = min_range_uu > 0.0f ? min_range_uu : 0.0f;
        if (hi - lo < min_range)
        {
            const float mid = (lo + hi) * 0.5f;
            lo = mid - min_range * 0.5f;
            hi = mid + min_range * 0.5f;
        }
    }

    // The ramp ends the minimap is using, carried across cuts.
    struct RangeState
    {
        float lo = 0.0f;
        float hi = 0.0f;
        bool valid = false;
    };

    // Folds one window's measured Z span into `rs`: widened to at least `min_range_uu`
    // around its own centre, then eased towards over `range_smooth_ms`. A window with
    // nothing in it leaves the last answer standing, so a step through a doorway into
    // empty space does not flash.
    inline void ease_range(RangeState& rs, float raw_lo, float raw_hi, float dt_ms,
                           const SliceStyle& st)
    {
        if (!(raw_hi >= raw_lo))
        {
            return;
        }
        float lo = raw_lo;
        float hi = raw_hi;
        widen_range(lo, hi, st.min_range_uu);
        if (!rs.valid)
        {
            rs.lo = lo;
            rs.hi = hi;
            rs.valid = true;
            return;
        }
        const float tau = st.range_smooth_ms > 1.0f ? st.range_smooth_ms : 1.0f;
        float a = (dt_ms > 0.0f ? dt_ms : 16.0f) / tau;
        a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        rs.lo += (lo - rs.lo) * a;
        rs.hi += (hi - rs.hi) * a;
    }
} // namespace srule
