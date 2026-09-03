#pragma once

//
// glyphs - WHAT SHAPE and WHAT COLOUR each marker category is drawn in, as pure data.
//
// WHY THIS IS A HEADER OF ITS OWN
// ------------------------------
// The v0.9.1 review found boss / elite / enemy drawn as the SAME triangle at scale
// 1.5 / 1.15 / 0.85: at `markers_size = 6.5` that is a three-pixel difference and the
// three categories read as one. Hidden was an unfilled 4-gon, which is a shrine's
// filled diamond with the fill switched off. A dimmed "found" marker loses its colour
// contrast first, so the SHAPE is what has to carry the identity.
//
// The fix is a rule, not a redraw: every category gets its own shape, and no two
// categories may share a shape AND a colour. That is a property of two tables, and a
// property of a table can be proven offline - `tests/markers_test.cpp` asserts it for
// every palette, so a future palette edit cannot quietly collapse two categories into
// one glyph. The drawing itself (ImDrawList primitives) stays in overlay.cpp; this
// header knows nothing about ImGui, Windows or UE4SS, exactly like markers_db.
//

#include <cstdint>
#include <string_view>

#include "markers_db.hpp"

namespace gly
{
    //==================================================================================
    // Shapes
    //==================================================================================
    //
    // One per category, all fourteen distinct. The names describe the drawing, and
    // overlay.cpp's draw_marker_glyph() switches on exactly this enum - so adding a
    // category without giving it a shape is a compile error there, not a marker that
    // silently comes out as a dot.

    enum class Shape : std::uint8_t
    {
        Diamond = 0,     // shrine   - 4-gon on the axes with a dark centre pip
        ChestBox,        // chest    - a wide box with a lid line
        Dot,             // pickup   - a plain filled circle
        Triangle,        // boss     - a big triangle
        TriangleNotched, // elite    - a smaller triangle with a bar across it
        DotRing,         // enemy    - a small dot inside a detached ring
        Pentagon,        // npc      - a filled 5-gon, point up
        NotePage,        // note     - a page with a folded top-right corner + two rules
        DoorBox,         // door     - a tall narrow box
        Ladder,          // ladder   - two rails and three rungs
        Lift,            // lift     - a flat box under an up arrow
        RingBar,         // fog gate - a hollow ring with a bar across it
        Cross,           // hidden   - a bold X
        SmallSquare,     // other    - a small square
        Count
    };

    constexpr int kShapeCount = static_cast<int>(Shape::Count);

    inline const char* shape_name(Shape s)
    {
        switch (s)
        {
        case Shape::Diamond:
            return "diamond";
        case Shape::ChestBox:
            return "chest";
        case Shape::Dot:
            return "dot";
        case Shape::Triangle:
            return "triangle";
        case Shape::TriangleNotched:
            return "notched triangle";
        case Shape::DotRing:
            return "dot in a ring";
        case Shape::Pentagon:
            return "pentagon";
        case Shape::NotePage:
            return "folded page";
        case Shape::DoorBox:
            return "door";
        case Shape::Ladder:
            return "ladder";
        case Shape::Lift:
            return "lift";
        case Shape::RingBar:
            return "barred ring";
        case Shape::Cross:
            return "cross";
        case Shape::SmallSquare:
            return "small square";
        case Shape::Count:
        default:
            return "?";
        }
    }

    // Cat order. A static_assert below pins the length to kCatCount, so adding a
    // category without a shape does not compile.
    inline constexpr Shape kShapes[] = {
        Shape::Diamond,         // Shrine
        Shape::ChestBox,        // Chest
        Shape::Dot,             // Pickup
        Shape::Triangle,        // Boss
        Shape::TriangleNotched, // Elite
        Shape::DotRing,         // Enemy
        Shape::Pentagon,        // Npc
        Shape::NotePage,        // Note
        Shape::DoorBox,         // Door
        Shape::Ladder,          // Ladder
        Shape::Lift,            // Lift
        Shape::RingBar,         // FogGate
        Shape::Cross,           // Hidden
        Shape::SmallSquare,     // Other
    };

    static_assert(sizeof(kShapes) / sizeof(kShapes[0]) == static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one glyph shape");

    // HOW FAR A SHAPE REACHES from its centre, as a multiple of the glyph radius `r`.
    //
    // It exists for the dark halo every glyph is drawn on top of. That halo was a fixed
    // circle of r + 1 with twelve segments, and several shapes stick out of it: the
    // chest is a box whose corners are at 1.21 r (review B.17 - the halo did not cover
    // them, so a light chest on a light floor lost its edge exactly at the corners), the
    // boss triangle's apex is at 1.5 r and the lift's arrow at 1.35 r.
    //
    // The number is CAPPED at 1.25 on purpose: a halo drawn to the boss triangle's full
    // 1.5 r is a black disc half again as wide as the glyph, which reads as a blob at
    // markers_size 6.5. So the box corners are covered, and the two spikes are allowed
    // to poke out of theirs - a spike has an edge on both sides and is legible anyway.
    inline constexpr float shape_extent(Shape s)
    {
        switch (s)
        {
        case Shape::ChestBox:      // half-extents 0.95 x 0.75 -> corner at 1.21
        case Shape::NotePage:      // 0.62 x 0.88             -> 1.08
        case Shape::DoorBox:       // 0.55 x 0.95             -> 1.10
        case Shape::Lift:          // box 0.85 x 0.5, arrow to 1.35
        case Shape::Cross:         // arms to 0.85 x 0.85     -> 1.20
        case Shape::Diamond:       // vertices on the axes at 1.15
        case Shape::Triangle:      // apex at 1.5 (capped)
        case Shape::TriangleNotched:
        case Shape::Ladder:        // rails to 0.5 x 1.0      -> 1.12
            return 1.25f;
        case Shape::RingBar:       // ring at exactly r
        case Shape::Pentagon:      // 1.05
        case Shape::DotRing:       // detached ring at 0.92
        case Shape::Dot:
        case Shape::SmallSquare:
        case Shape::Count:
        default:
            return 1.1f;
        }
    }

    // BELOW THIS RADIUS a glyph is drawn in its simplified form (review B.17): the
    // ladder loses two of its three rungs, the note its two rules of writing and the
    // lift the outline inside its box. Those details are 1-2 pixels apart at
    // markers_size 6.5 and turn into a smudge that makes the silhouette HARDER to read,
    // not easier - and the minimap draws at exactly that size while the full map draws
    // at 8 and the x-ray at 7.
    constexpr float kSimpleGlyphRadius = 7.0f;

    constexpr Shape shape_of(mdb::Cat cat)
    {
        const int i = static_cast<int>(cat);
        return (i < 0 || i >= mdb::kCatCount) ? Shape::SmallSquare : kShapes[i];
    }

    //==================================================================================
    // Palettes
    //==================================================================================
    //
    // `default` is the hue set the mod has always drawn (it lived as a switch inside
    // overlay.cpp's marker_color()). `colorblind` is derived from Okabe-Ito: eight hues
    // chosen to stay distinguishable under deuteranopia / protanopia / tritanopia, plus
    // a near-white for the two traversal categories. Fourteen categories cannot have
    // fourteen safe hues, which is exactly why every category also has its own SHAPE -
    // the palette only has to keep neighbours in the same picture apart, and
    // palette_is_separable() proves it does.

    enum class Palette : std::uint8_t
    {
        Default = 0,
        Colorblind = 1,
    };

    inline const char* palette_name(Palette p)
    {
        return p == Palette::Colorblind ? "colorblind" : "default";
    }

    // Accepts the wire names, case-insensitively, plus the common spellings a player
    // will actually type. Returns false (and leaves `out` alone) for anything else.
    inline bool palette_from_name(std::string_view name, Palette& out)
    {
        char buf[24]{};
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
        if (v == "default" || v == "normal" || v == "standard")
        {
            out = Palette::Default;
            return true;
        }
        if (v == "colorblind" || v == "colourblind" || v == "cb" || v == "okabeito")
        {
            out = Palette::Colorblind;
            return true;
        }
        return false;
    }

    inline constexpr mdb::Rgb kPaletteDefault[] = {
        mdb::Rgb{255, 186, 72},  // Shrine
        mdb::Rgb{255, 226, 120}, // Chest
        mdb::Rgb{120, 220, 255}, // Pickup
        mdb::Rgb{255, 86, 86},   // Boss
        mdb::Rgb{255, 140, 80},  // Elite
        mdb::Rgb{232, 96, 96},   // Enemy
        mdb::Rgb{140, 235, 140}, // Npc
        mdb::Rgb{238, 232, 205}, // Note  - parchment; its legend neighbours are the
                                 //         npc green and the door's cold blue-grey
        mdb::Rgb{172, 194, 224}, // Door
        mdb::Rgb{206, 184, 142}, // Ladder
        mdb::Rgb{206, 184, 142}, // Lift  (same hue as the ladder, different shape)
        mdb::Rgb{198, 150, 255}, // FogGate
        mdb::Rgb{255, 130, 220}, // Hidden
        mdb::Rgb{196, 196, 196}, // Other
    };

    // Okabe-Ito, minus the black (unusable on this mod's dark backdrop):
    //   orange 230 159 0 | sky blue 86 180 233 | bluish green 0 158 115 |
    //   yellow 240 228 66 | blue 0 114 178 | vermillion 213 94 0 |
    //   reddish purple 204 121 167
    // Assigned so that the categories a player hunts for at the same time never share a
    // hue. WHICH THOSE ARE is written down as data below (kCompeting) and asserted, not
    // asserted in a comment: 1.0.0's comment claimed "enemy/boss are deliberately NOT
    // paired" while the table gave both the same vermillion, which is exactly the pair a
    // player is reading the map for when a boss arena is full of adds. Enemy is a
    // neutral grey now - it is the one category that is filtered OUT by default, so
    // taking a hue away from it costs nothing and gives boss its own.
    //
    // The hues that still repeat are pickup/door (sky blue), elite/fog gate (reddish
    // purple) and ladder/lift/hidden (near-white). Fourteen categories cannot have
    // fourteen safe hues; in each of those the pair is a thing you hunt for beside a
    // piece of furniture or a doorway, they are never the two things being told apart,
    // and their shapes differ - a filled dot vs a tall door, a barred ring vs a
    // triangle, rails-and-rungs vs a box under an arrow vs a bold X.
    inline constexpr mdb::Rgb kPaletteColorblind[] = {
        mdb::Rgb{230, 159, 0},   // Shrine   - orange
        mdb::Rgb{240, 228, 66},  // Chest    - yellow
        mdb::Rgb{86, 180, 233},  // Pickup   - sky blue
        mdb::Rgb{213, 94, 0},    // Boss     - vermillion
        mdb::Rgb{204, 121, 167}, // Elite    - reddish purple
        mdb::Rgb{150, 150, 150}, // Enemy    - neutral grey: the boss keeps vermillion to
                                 //            itself, and enemies are off by default
        mdb::Rgb{0, 158, 115},   // Npc      - bluish green
        mdb::Rgb{0, 114, 178},   // Note     - blue, the one Okabe-Ito hue nothing else
                                 //            uses (the ladder/lift near-white would
                                 //            have been the parchment analogue)
        mdb::Rgb{86, 180, 233},  // Door     - sky blue (tall box vs the pickup dot)
        mdb::Rgb{235, 235, 235}, // Ladder   - near-white
        mdb::Rgb{235, 235, 235}, // Lift     - near-white
        mdb::Rgb{204, 121, 167}, // FogGate  - reddish purple (barred ring vs the elite triangle)
        mdb::Rgb{235, 235, 235}, // Hidden   - near-white: a hidden item is LOOT and has
                                 //            to differ from the chest's yellow and the
                                 //            pickup's sky blue (kCompeting asserts it);
                                 //            the near-white it now shares with the
                                 //            ladder and the lift is furniture, which is
                                 //            never the thing being told apart from it
        mdb::Rgb{190, 190, 190}, // Other    - grey
    };

    static_assert(sizeof(kPaletteDefault) / sizeof(kPaletteDefault[0]) ==
                      static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one default colour");
    static_assert(sizeof(kPaletteColorblind) / sizeof(kPaletteColorblind[0]) ==
                      static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one colour-blind colour");

    inline constexpr mdb::Rgb marker_rgb(mdb::Cat cat, Palette pal)
    {
        const int i = static_cast<int>(cat);
        const int j = (i < 0 || i >= mdb::kCatCount) ? (mdb::kCatCount - 1) : i;
        return pal == Palette::Colorblind ? kPaletteColorblind[j] : kPaletteDefault[j];
    }

    //==================================================================================
    // The item-quality (rarity) palette, colour-blind variant
    //==================================================================================
    //
    // The default tier colours are the game's OWN pickup-beam colours (mdb::
    // kDefaultRarityColors) - three pale pastels that are only just apart for normal
    // vision and not at all under deuteranopia. `palette = colorblind` therefore swaps
    // in three Okabe-Ito hues; as everywhere else, an explicit `xray_rarity_colors` in
    // the config file still wins.
    inline constexpr mdb::Rgb kRarityColorblind[mdb::kRarityCount] = {
        mdb::Rgb{86, 180, 233},  // Common    - sky blue
        mdb::Rgb{204, 121, 167}, // Equipment - reddish purple
        mdb::Rgb{240, 228, 66},  // Key       - yellow
    };

    inline constexpr const mdb::Rgb* rarity_colors(Palette pal)
    {
        return pal == Palette::Colorblind ? kRarityColorblind : mdb::kDefaultRarityColors;
    }

    //==================================================================================
    // Themes (the CHROME, not the markers)
    //==================================================================================
    //
    // `theme` presets the handful of colours that are not a marker: the minimap's frame
    // and backdrop, the dark plate under every label, and the walkable fill the height
    // slicer paints. Those used to be five separate config keys, so changing "the look"
    // meant editing five lines and knowing which five.
    //
    // PRECEDENCE, and it is the whole point: the theme only supplies a colour key that
    // the config file does NOT mention. A file that spells out `minimap_frame_color`
    // keeps its own value under every theme, so a theme can never overwrite a tuned
    // config.
    //
    //   neutral - what 0.9.2 shipped: a cold blue-grey frame on a blue-black disc.
    //   ink     - bronze on near-black, with a warmer parchment fill: the look the
    //             v0.9.1 review asked for. Both ship; the user judges them on screen.

    enum class Theme : std::uint8_t
    {
        Neutral = 0,
        Ink = 1,
    };

    inline const char* theme_name(Theme t)
    {
        return t == Theme::Ink ? "ink" : "neutral";
    }

    inline bool theme_from_name(std::string_view name, Theme& out)
    {
        char buf[24]{};
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
        if (v == "neutral" || v == "default")
        {
            out = Theme::Neutral;
            return true;
        }
        if (v == "ink" || v == "parchment")
        {
            out = Theme::Ink;
            return true;
        }
        return false;
    }

    struct ThemeColors
    {
        mdb::Rgb frame{168, 176, 186};
        float frame_alpha = 0.85f;
        mdb::Rgb backdrop{6, 9, 13};
        float backdrop_alpha = 0.86f;
        mdb::Rgb plate{8, 10, 14};  // the dark box behind a label / the compass strip
        mdb::Rgb floor_base{214, 208, 196}; // the walkable fill the height slicer paints
    };

    inline constexpr ThemeColors theme_colors(Theme t)
    {
        ThemeColors c{};
        if (t == Theme::Ink)
        {
            c.frame = mdb::Rgb{200, 168, 108};
            c.frame_alpha = 0.9f;
            c.backdrop = mdb::Rgb{14, 11, 9};
            c.backdrop_alpha = 0.88f;
            c.plate = mdb::Rgb{18, 14, 10};
            c.floor_base = mdb::Rgb{222, 210, 186};
        }
        return c;
    }

    //==================================================================================
    // The pairs that must never share a hue
    //==================================================================================
    //
    // "Every pair differs by hue or by shape" is satisfied by construction here - all
    // fourteen shapes are distinct - so on its own it proves very little. The property
    // that actually matters at the 13-pixel size these are drawn at is about the
    // categories a player is comparing IN THE SAME GLANCE: three kinds of hostile, and
    // the three kinds of loot. Inside such a group the hue has to carry the difference,
    // because at that size a triangle and a smaller barred triangle do not.
    //
    // Written as data so palette_competing_hues_ok() can be asserted per palette, which
    // is what turns "the comment says they are not paired" into a test.
    struct CatPair
    {
        mdb::Cat a;
        mdb::Cat b;
    };

    inline constexpr CatPair kCompeting[] = {
        // Hostiles: which of the three shapes in a fight is the one worth walking to.
        {mdb::Cat::Boss, mdb::Cat::Elite},
        {mdb::Cat::Boss, mdb::Cat::Enemy},
        {mdb::Cat::Elite, mdb::Cat::Enemy},
        // Loot: what is still out there in this room.
        {mdb::Cat::Chest, mdb::Cat::Pickup},
        {mdb::Cat::Chest, mdb::Cat::Hidden},
        {mdb::Cat::Pickup, mdb::Cat::Hidden},
        // A shrine is the map's landmark and must not read as a chest.
        {mdb::Cat::Shrine, mdb::Cat::Chest},
    };

    inline bool palette_competing_hues_ok(Palette pal)
    {
        for (const CatPair& p : kCompeting)
        {
            if (marker_rgb(p.a, pal) == marker_rgb(p.b, pal))
            {
                return false;
            }
        }
        return true;
    }

    // "no two categories share a shape AND a colour" - the property the whole header
    // exists to guarantee. Written as a runnable function so the test can assert it per
    // palette instead of eyeballing two tables.
    inline bool palette_is_separable(Palette pal)
    {
        for (int a = 0; a < mdb::kCatCount; ++a)
        {
            for (int b = a + 1; b < mdb::kCatCount; ++b)
            {
                const mdb::Cat ca = static_cast<mdb::Cat>(a);
                const mdb::Cat cb = static_cast<mdb::Cat>(b);
                if (shape_of(ca) == shape_of(cb) && marker_rgb(ca, pal) == marker_rgb(cb, pal))
                {
                    return false;
                }
            }
        }
        return true;
    }
} // namespace gly
