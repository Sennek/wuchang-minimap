#pragma once

//
// slicerule - the height slicer's per-pixel decision, as pure C++.
//
// The map asset carries the Z of up to eight stacked walkable surfaces per pixel plus a
// REACHABLE bit (src/mapdata.hpp). This header holds what the slicer decides from that
// and nothing else - no Windows, no D3D12 - so tests/markers_test.cpp exercises the
// exact rule the minimap and the full map run.
//
// Per surface, relative to the player's feet Z:
//
//     |dZ| <= tol                      -> class 3, the floor I am on, opaque
//     below and |dZ| <= fade           -> class 2, dimmed
//     above and |dZ| <= fade_above     -> class 1, fainter
//     otherwise                        -> not drawn
//
// `fade_above` is its own dial because ground overhead is never ground the player can
// walk on now: at the Ai Nengqi arena the above class is 31 % of everything drawn, in
// 230 separate blobs.
//
// A pixel keeps the best of the surfaces under it, ranked class first and then by
// distance, with a REACHABLE surface beating an unreachable one of the same class. The
// rank is `class * 2 + reachable`, so both orderings fall out of one comparison.
//
// `map_unreachable` decides what an unreachable surface is worth:
//
//     Hide - it is not a surface at all
//     Dim  - it is drawn one rung further down the same opacity ladder
//     Show - it is drawn exactly like a reachable one
//

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
    // (tools/navmesh/slice_preview.py), so a reported spot reproduces without the game:
    //     lum = 1 + gradient_strength * clamp((surfaceZ - feetZ) / span, -1, +1)
    // with span = tol for the current floor, fade below it and fade_above over it.
    struct SliceStyle
    {
        float base_r = 214.0f;
        float base_g = 208.0f;
        float base_b = 196.0f;
        float strength = 0.18f;
        float tol = 200.0f;
        float fade = 800.0f;       // below
        float fade_above = 300.0f; // above; 0 = never draw a floor above
        float a_dim = 0.25f;
        float a_faint = 0.15f;
        Unreachable unreachable = Unreachable::Hide;
    };

    // 3 = my floor, 2 = below, 1 = above, 0 = out of range.
    constexpr std::uint8_t kClassNone = 0;
    constexpr std::uint8_t kClassAbove = 1;
    constexpr std::uint8_t kClassBelow = 2;
    constexpr std::uint8_t kClassFloor = 3;

    inline std::uint8_t classify(float d, float ad, const SliceStyle& st)
    {
        if (ad <= st.tol)
        {
            return kClassFloor;
        }
        if (d < 0.0f)
        {
            return ad <= st.fade ? kClassBelow : kClassNone;
        }
        return ad <= st.fade_above ? kClassAbove : kClassNone;
    }

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

    // Folds one surface into a pixel's running answer. `d` is surfaceZ - feetZ.
    // `rank` / `best_ad` / `best_d` start at 0 and are the pixel's state.
    inline void accumulate(std::uint8_t& rank, float& best_ad, float& best_d, float d, bool reachable,
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
        const float ad = d < 0.0f ? -d : d;
        const std::uint8_t cls = classify(d, ad, st);
        if (cls == kClassNone)
        {
            return;
        }
        const std::uint8_t r = rank_of(cls, reach);
        if (r > rank || (r == rank && ad < best_ad))
        {
            rank = r;
            best_ad = ad;
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
            return reachable ? 1.0f : st.a_dim;
        case kClassBelow:
            return reachable ? st.a_dim : st.a_faint;
        case kClassAbove:
            return reachable ? st.a_faint : st.a_faint * 0.6f;
        default:
            return 0.0f;
        }
    }

    // What the height gradient is measured against for this class.
    inline float span_for(std::uint8_t cls, const SliceStyle& st)
    {
        if (cls == kClassFloor)
        {
            return st.tol;
        }
        return cls == kClassAbove ? st.fade_above : st.fade;
    }
} // namespace srule
