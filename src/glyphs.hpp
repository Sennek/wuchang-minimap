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
        Coin,            // merchant - a circle with a dark centre
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
        case Shape::Coin:
            return "coin";
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
        Shape::Coin,            // Merchant
        Shape::DoorBox,         // Door
        Shape::Ladder,          // Ladder
        Shape::Lift,            // Lift
        Shape::RingBar,         // FogGate
        Shape::Cross,           // Hidden
        Shape::SmallSquare,     // Other
    };

    static_assert(sizeof(kShapes) / sizeof(kShapes[0]) == static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one glyph shape");

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
    // overlay.cpp's marker_color()); it is here so the shape/colour property can be
    // checked against it. `colorblind` arrives with the theme work and is derived from
    // Okabe-Ito.

    enum class Palette : std::uint8_t
    {
        Default = 0,
        Colorblind = 1,
    };

    inline constexpr mdb::Rgb kPaletteDefault[] = {
        mdb::Rgb{255, 186, 72},  // Shrine
        mdb::Rgb{255, 226, 120}, // Chest
        mdb::Rgb{120, 220, 255}, // Pickup
        mdb::Rgb{255, 86, 86},   // Boss
        mdb::Rgb{255, 140, 80},  // Elite
        mdb::Rgb{232, 96, 96},   // Enemy
        mdb::Rgb{140, 235, 140}, // Npc
        mdb::Rgb{120, 230, 210}, // Merchant
        mdb::Rgb{172, 194, 224}, // Door
        mdb::Rgb{206, 184, 142}, // Ladder
        mdb::Rgb{206, 184, 142}, // Lift  (same hue as the ladder, different shape)
        mdb::Rgb{198, 150, 255}, // FogGate
        mdb::Rgb{255, 130, 220}, // Hidden
        mdb::Rgb{196, 196, 196}, // Other
    };

    static_assert(sizeof(kPaletteDefault) / sizeof(kPaletteDefault[0]) ==
                      static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one default colour");

    inline constexpr mdb::Rgb marker_rgb(mdb::Cat cat, Palette pal)
    {
        const int i = static_cast<int>(cat);
        const int j = (i < 0 || i >= mdb::kCatCount) ? (mdb::kCatCount - 1) : i;
        (void)pal;
        return kPaletteDefault[j];
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
