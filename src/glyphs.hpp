#pragma once

//
// glyphs - what shape and what colour each marker category is drawn in, as pure data.
//
// The rule the tables hold to: every category has its own shape, and no two categories
// share a shape and a colour. A dimmed "found" marker loses its colour contrast first,
// so the shape carries the identity. tests/markers_test.cpp asserts the property for
// every palette.
//
// The drawing itself (ImDrawList primitives) is in overlay.cpp; this header knows
// nothing about ImGui, Windows or UE4SS.
//

#include <cstdint>
#include <string_view>

#include "markers_db.hpp"

namespace gly
{
    // Shapes: one per category, all fourteen distinct. overlay.cpp's
    // draw_marker_glyph() switches on this enum, so a category with no shape is a
    // compile error there.
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

    // Cat order; the static_assert below pins the length to kCatCount.
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

    // How far a shape reaches from its centre, as a multiple of the glyph radius `r`,
    // sizing the dark halo drawn under it. Capped at 1.25: the boss triangle's apex
    // (1.5 r) and the lift's arrow (1.35 r) poke out of their halo rather than turning
    // it into a blob.
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

    // Below this radius a glyph is drawn simplified: the ladder loses two of its three
    // rungs, the note its two rules and the lift the outline inside its box. Those
    // details sit 1-2 pixels apart at markers_size 6.5 and smudge the silhouette.
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
    // `colorblind` is derived from Okabe-Ito: eight hues that stay distinguishable under
    // deuteranopia / protanopia / tritanopia, plus a near-white for the two traversal
    // categories. Fourteen categories cannot have fourteen safe hues, so the shape
    // carries the identity and the palette only has to keep neighbours apart;
    // palette_is_separable() checks that.

    enum class Palette : std::uint8_t
    {
        Default = 0,
        Colorblind = 1,
    };

    inline const char* palette_name(Palette p)
    {
        return p == Palette::Colorblind ? "colorblind" : "default";
    }

    // Accepts the wire names case-insensitively plus common spellings; false (and `out`
    // untouched) for anything else.
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
        mdb::Rgb{238, 232, 205}, // Note  - parchment
        mdb::Rgb{172, 194, 224}, // Door
        mdb::Rgb{206, 184, 142}, // Ladder
        mdb::Rgb{206, 184, 142}, // Lift  - ladder's hue, different shape
        mdb::Rgb{198, 150, 255}, // FogGate
        mdb::Rgb{255, 130, 220}, // Hidden
        mdb::Rgb{196, 196, 196}, // Other
    };

    // Okabe-Ito, minus the black (unusable on this mod's dark backdrop):
    //   orange 230 159 0 | sky blue 86 180 233 | bluish green 0 158 115 |
    //   yellow 240 228 66 | blue 0 114 178 | vermillion 213 94 0 |
    //   reddish purple 204 121 167
    // Assigned so that categories a player hunts for at the same time never share a hue;
    // kCompeting below lists those pairs as data and the test asserts them. The hues
    // that do repeat - pickup/door, elite/fog gate, ladder/lift/hidden - pair a hunted
    // thing with a piece of furniture, and their shapes differ.
    inline constexpr mdb::Rgb kPaletteColorblind[] = {
        mdb::Rgb{230, 159, 0},   // Shrine   - orange
        mdb::Rgb{240, 228, 66},  // Chest    - yellow
        mdb::Rgb{86, 180, 233},  // Pickup   - sky blue
        mdb::Rgb{213, 94, 0},    // Boss     - vermillion
        mdb::Rgb{204, 121, 167}, // Elite    - reddish purple
        mdb::Rgb{150, 150, 150}, // Enemy    - neutral grey, leaving boss the vermillion
        mdb::Rgb{0, 158, 115},   // Npc      - bluish green
        mdb::Rgb{0, 114, 178},   // Note     - blue, used by nothing else
        mdb::Rgb{86, 180, 233},  // Door     - sky blue (tall box vs the pickup dot)
        mdb::Rgb{235, 235, 235}, // Ladder   - near-white
        mdb::Rgb{235, 235, 235}, // Lift     - near-white
        mdb::Rgb{204, 121, 167}, // FogGate  - reddish purple (barred ring vs the elite triangle)
        mdb::Rgb{235, 235, 235}, // Hidden   - near-white; loot, so it must differ from
                                 //            the chest's yellow and the pickup's blue
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
    // The default tier colours (mdb::kDefaultRarityColors) are the game's own
    // pickup-beam pastels, which are indistinguishable under deuteranopia.
    // `palette = colorblind` swaps in three Okabe-Ito hues; an explicit
    // `xray_rarity_colors` in the config still wins.
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
    // Themes (the chrome, not the markers)
    //==================================================================================
    //
    // `theme` presets the colours that are not a marker: the minimap's frame and
    // backdrop, the dark plate under every label, and the walkable fill the height
    // slicer paints.
    //
    // Precedence: a theme supplies every colour the player has not personally picked - a
    // key the config file does not mention, or one whose value is exactly what some
    // built-in theme sets. A `minimap_frame_color` of the player's own choosing survives
    // a theme change; one left at a theme's colour follows the theme.
    //
    //   neutral - cold blue-grey frame on a blue-black disc.
    //   ink     - bronze on near-black with a warmer parchment fill.

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
    // The categories a player compares in one glance: three kinds of hostile, three
    // kinds of loot. At the ~13-pixel draw size the hue has to carry the difference
    // inside such a group, so palette_competing_hues_ok() asserts these pairs differ.
    struct CatPair
    {
        mdb::Cat a;
        mdb::Cat b;
    };

    inline constexpr CatPair kCompeting[] = {
        // Hostiles.
        {mdb::Cat::Boss, mdb::Cat::Elite},
        {mdb::Cat::Boss, mdb::Cat::Enemy},
        {mdb::Cat::Elite, mdb::Cat::Enemy},
        // Loot.
        {mdb::Cat::Chest, mdb::Cat::Pickup},
        {mdb::Cat::Chest, mdb::Cat::Hidden},
        {mdb::Cat::Pickup, mdb::Cat::Hidden},
        // The map's landmark must not read as a chest.
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

    // No two categories share a shape and a colour.
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
