#pragma once

//
// glyphs - what shape and what colour each marker category is drawn in, as pure data.
//
// Two families of category and one rule joining them. A PEER category (shrine, chest, a
// door) has its own silhouette and its own hue. The eleven LOOT categories share one
// silhouette - a filled disc - take their hue from their quality tier, and are told apart
// by a small MARK drawn inside the disc; a mark only ever has to be read against the marks
// of its own tier colour, of which there are at most five. The rule the tables hold to is
// that no two categories share a shape, a mark and a colour, and that a family's
// silhouette is used by nothing outside it. A dimmed "found" marker loses its colour
// contrast first, so the silhouette carries the identity.
// tests/markers_test.cpp asserts the properties for every palette.
//
// The drawing itself (ImDrawList primitives) is in overlay.cpp; this header knows
// nothing about ImGui, Windows or UE4SS.
//

#include <cstdint>
#include <string_view>

#include "markers_db.hpp"

namespace gly
{
    // Shapes: one per peer category and one for the whole loot family. overlay.cpp's
    // draw_marker_glyph() switches on this enum, so a category with no shape is a
    // compile error there.
    enum class Shape : std::uint8_t
    {
        Diamond = 0,     // shrine   - 4-gon on the axes with a dark centre pip
        ChestBox,        // chest    - a wide box with a lid line
        LootDisc,        // the eleven loot categories - a filled disc carrying a mark
        Triangle,        // boss     - a big triangle
        TriangleNotched, // elite    - a smaller triangle with a bar across it
        DotRing,         // enemy    - a small dot inside a detached ring
        Leaf,            // bamboozling - a pointed leaf with a centre vein
        Pentagon,        // npc      - a filled 5-gon, point up
        NotePage,        // note     - a page with a folded top-right corner + two rules
        DoorBox,         // door     - a tall narrow box
        ArchPip,         // mystery gate     - a domed arch with a centre pip
        ArchSplit,       // benediction door - a domed arch split down the middle
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
        case Shape::LootDisc:
            return "loot disc";
        case Shape::Triangle:
            return "triangle";
        case Shape::TriangleNotched:
            return "notched triangle";
        case Shape::DotRing:
            return "dot in a ring";
        case Shape::Leaf:
            return "leaf";
        case Shape::Pentagon:
            return "pentagon";
        case Shape::NotePage:
            return "folded page";
        case Shape::DoorBox:
            return "door";
        case Shape::ArchPip:
            return "arch with a pip";
        case Shape::ArchSplit:
            return "split arch";
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

    // The mark drawn inside the loot family's disc, in the glyph's `edge` colour. A peer
    // category carries `None`, and so does `consumable`: the 845 most common markers get
    // the quietest sign. Geometry is in overlay_hud.cpp's draw_marker_glyph(), in units of
    // the glyph radius.
    enum class Mark : std::uint8_t
    {
        None = 0,
        Pip,     // item     - a small filled circle
        Sprout,  // harvest  - a stem with two arms
        Square,  // ammo     - a filled square
        Blade,   // weapon   - one diagonal stroke
        Chevron, // armour   - an open V pointing up
        Ring,    // amulet   - a hollow circle
        Gem,     // jade     - a hollow 4-gon on the axes
        Plus,    // spell    - two strokes on the axes
        Grains,  // material - three small filled circles
        Key,     // key item - a stroke with a bow at one end
        Count
    };

    constexpr int kMarkCount = static_cast<int>(Mark::Count);

    // Cat order; the static_assert below pins the length to kCatCount.
    inline constexpr Shape kShapes[] = {
        Shape::Diamond,         // Shrine
        Shape::ChestBox,        // Chest
        Shape::LootDisc,        // Consumable
        Shape::LootDisc,        // Item
        Shape::LootDisc,        // Harvest
        Shape::LootDisc,        // Ammo
        Shape::LootDisc,        // Armour
        Shape::LootDisc,        // Amulet
        Shape::LootDisc,        // Weapon
        Shape::LootDisc,        // Jade
        Shape::LootDisc,        // Spell
        Shape::LootDisc,        // Material
        Shape::LootDisc,        // Key
        Shape::Triangle,        // Boss
        Shape::TriangleNotched, // Elite
        Shape::DotRing,         // Enemy
        Shape::Leaf,            // Bamboozling
        Shape::Pentagon,        // Npc
        Shape::NotePage,        // Note
        Shape::DoorBox,         // Door
        Shape::ArchPip,         // MysteryGate
        Shape::ArchSplit,       // BenedictionDoor
        Shape::Ladder,          // Ladder
        Shape::Lift,            // Lift
        Shape::RingBar,         // FogGate
        Shape::Cross,           // Hidden
        Shape::SmallSquare,     // Other
    };

    static_assert(sizeof(kShapes) / sizeof(kShapes[0]) == static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one glyph shape");

    inline constexpr Mark kMarks[] = {
        Mark::None,    // Shrine
        Mark::None,    // Chest
        Mark::None,    // Consumable - the plain disc
        Mark::Pip,     // Item
        Mark::Sprout,  // Harvest
        Mark::Square,  // Ammo
        Mark::Chevron, // Armour
        Mark::Ring,    // Amulet
        Mark::Blade,   // Weapon
        Mark::Gem,     // Jade
        Mark::Plus,    // Spell
        Mark::Grains,  // Material
        Mark::Key,     // Key
        Mark::None,    // Boss
        Mark::None,    // Elite
        Mark::None,    // Enemy
        Mark::None,    // Bamboozling
        Mark::None,    // Npc
        Mark::None,    // Note
        Mark::None,    // Door
        Mark::None,    // MysteryGate
        Mark::None,    // BenedictionDoor
        Mark::None,    // Ladder
        Mark::None,    // Lift
        Mark::None,    // FogGate
        Mark::None,    // Hidden
        Mark::None,    // Other
    };

    static_assert(sizeof(kMarks) / sizeof(kMarks[0]) == static_cast<std::size_t>(mdb::kCatCount),
                  "every category needs exactly one mark");

    constexpr Mark mark_of(mdb::Cat cat)
    {
        const int i = static_cast<int>(cat);
        return (i < 0 || i >= mdb::kCatCount) ? Mark::None : kMarks[i];
    }

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
        case Shape::ArchPip:       // 0.58 x 0.95 at the base -> 1.11
        case Shape::ArchSplit:
        case Shape::Lift:          // box 0.85 x 0.5, arrow to 1.35
        case Shape::Cross:         // arms to 0.85 x 0.85     -> 1.20
        case Shape::Diamond:       // vertices on the axes at 1.15
        case Shape::Triangle:      // apex at 1.5 (capped)
        case Shape::TriangleNotched:
        case Shape::Ladder:        // rails to 0.5 x 1.0      -> 1.12
        case Shape::Leaf:          // tips on the vertical at 1.15, 0.55 wide
            return 1.25f;
        case Shape::RingBar:       // ring at exactly r
        case Shape::Pentagon:      // 1.05
        case Shape::DotRing:       // detached ring at 0.92
        case Shape::LootDisc:      // the disc itself at 0.82
        case Shape::SmallSquare:
        case Shape::Count:
        default:
            return 1.1f;
        }
    }

    // Below this radius a glyph is drawn simplified: the ladder loses two of its three
    // rungs, the note its two rules, the lift the outline inside its box and a loot disc
    // its mark. Those details sit 1-2 pixels apart at markers_size 6.5 and smudge the
    // silhouette. That is the minimap, where the question is "is that dot worth the
    // detour" and the tier colour answers it; the full map and the F2 legend draw marks.
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
    // categories. Sixteen categories cannot have sixteen safe hues, so the shape
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

    //==================================================================================
    // The quality tiers
    //==================================================================================
    //
    // Saturated versions of the game's three pickup-beam hues, so the loot family reads
    // over a lit map: blue Common, pink Equipment, gold Key. These ARE the eleven loot
    // categories' palette rows below - the family occupies one hue slot per tier instead
    // of eleven of its own - and they are what the F2 legend, the compass and the x-ray
    // draw. The colour-blind set is three Okabe-Ito hues, which stay apart under
    // deuteranopia where the beam pastels do not.

    inline constexpr mdb::Rgb kTierDefault[mdb::kTierCount] = {
        mdb::Rgb{0x4F, 0x7F, 0xE6}, // Common    - blue
        mdb::Rgb{0xE0, 0x55, 0x9A}, // Equipment - pink
        mdb::Rgb{0xE6, 0xB4, 0x22}, // Key       - gold
    };

    inline constexpr mdb::Rgb kTierColorblind[mdb::kTierCount] = {
        mdb::Rgb{86, 180, 233},  // Common    - sky blue
        mdb::Rgb{204, 121, 167}, // Equipment - reddish purple
        mdb::Rgb{240, 228, 66},  // Key       - yellow
    };

    inline constexpr const mdb::Rgb* tier_colors(Palette pal)
    {
        return pal == Palette::Colorblind ? kTierColorblind : kTierDefault;
    }

    inline constexpr mdb::Rgb kPaletteDefault[] = {
        mdb::Rgb{255, 186, 72},  // Shrine
        mdb::Rgb{255, 226, 120}, // Chest
        kTierDefault[0],         // Consumable
        kTierDefault[0],         // Item
        kTierDefault[0],         // Harvest
        kTierDefault[0],         // Ammo
        kTierDefault[1],         // Armour
        kTierDefault[1],         // Amulet
        kTierDefault[1],         // Weapon
        kTierDefault[1],         // Jade
        kTierDefault[1],         // Spell
        kTierDefault[2],         // Material
        kTierDefault[2],         // Key
        mdb::Rgb{255, 86, 86},   // Boss
        mdb::Rgb{255, 140, 80},  // Elite
        mdb::Rgb{232, 96, 96},   // Enemy
        mdb::Rgb{176, 214, 60},  // Bamboozling - young bamboo
        mdb::Rgb{140, 235, 140}, // Npc
        mdb::Rgb{238, 232, 205}, // Note  - parchment
        mdb::Rgb{172, 194, 224}, // Door
        mdb::Rgb{224, 66, 78},   // MysteryGate     - the riddle door's own red
        mdb::Rgb{255, 212, 64},  // BenedictionDoor - the chisel door's own gold
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
    // that do repeat - Common loot/door, Equipment loot/elite/fog gate, chest/npc,
    // ladder/lift/hidden - pair things whose shapes differ outright.
    inline constexpr mdb::Rgb kPaletteColorblind[] = {
        mdb::Rgb{230, 159, 0},   // Shrine   - orange
        // The three loot tiers take the sky blue, the reddish purple and the yellow, and
        // the shrine the orange; bluish green is what is left for a chest, which must
        // differ from every kind of loot. Only the NPC's pentagon shares it.
        mdb::Rgb{0, 158, 115},   // Chest    - bluish green
        kTierColorblind[0],      // Consumable
        kTierColorblind[0],      // Item
        kTierColorblind[0],      // Harvest
        kTierColorblind[0],      // Ammo
        kTierColorblind[1],      // Armour
        kTierColorblind[1],      // Amulet
        kTierColorblind[1],      // Weapon
        kTierColorblind[1],      // Jade
        kTierColorblind[1],      // Spell
        kTierColorblind[2],      // Material
        kTierColorblind[2],      // Key
        mdb::Rgb{213, 94, 0},    // Boss     - vermillion
        mdb::Rgb{204, 121, 167}, // Elite    - reddish purple
        mdb::Rgb{150, 150, 150}, // Enemy    - neutral grey, leaving boss the vermillion
        // Green in both palettes, so the bamboo creature tells the same story in each; the
        // leaf is a shape nothing else has, and the chest and the NPC differ outright.
        mdb::Rgb{0, 158, 115},   // Bamboozling - bluish green
        mdb::Rgb{0, 158, 115},   // Npc      - bluish green
        mdb::Rgb{0, 114, 178},   // Note     - blue, used by nothing else
        mdb::Rgb{86, 180, 233},  // Door     - sky blue (tall box vs a loot disc)
        // The two special doors keep a red and a gold here too: they are told apart from
        // each other by hue, so they cannot both fall back on the plain door's blue.
        mdb::Rgb{213, 94, 0},    // MysteryGate     - vermillion (arch vs the boss triangle)
        mdb::Rgb{240, 228, 66},  // BenedictionDoor - yellow (arch vs a loot disc)
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
    // Themes (the chrome, not the markers)
    //==================================================================================
    //
    // `theme` presets the colours that are not a marker: the minimap's frame and
    // backdrop, and the dark plate under every label. The ground the height slicer
    // paints is not one of them - it is shaded by absolute height (src/slicerule.hpp).
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
        mdb::Rgb plate{8, 10, 14}; // the dark box behind a label / the compass strip
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
        }
        return c;
    }

    //==================================================================================
    // The pairs that must never share a hue
    //==================================================================================
    //
    // The categories a player compares in one glance: three kinds of hostile, three
    // kinds of loot, three kinds of door. At the ~13-pixel draw size the hue has to
    // carry the difference inside such a group, so palette_competing_hues_ok() asserts
    // these pairs differ.
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
        // Loot. The eleven loot categories are covered by the tier loop in
        // palette_competing_hues_ok(): the three tier hues differ from these two and from
        // each other, which is the same statement for all eleven at once.
        {mdb::Cat::Chest, mdb::Cat::Hidden},
        // The map's landmark must not read as a chest.
        {mdb::Cat::Shrine, mdb::Cat::Chest},
        // The doors. Three kinds sit side by side in one area and the question is which
        // one this is, so the hue has to answer it - the arches differ only in their
        // centre detail.
        {mdb::Cat::MysteryGate, mdb::Cat::BenedictionDoor},
        {mdb::Cat::MysteryGate, mdb::Cat::Door},
        {mdb::Cat::BenedictionDoor, mdb::Cat::Door},
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
        // The other two kinds of loot against every tier of the first kind, and the tiers
        // against each other: "gold yes, pink probably, blue no" is the minimap's whole
        // answer, so two tiers sharing a hue would erase it.
        const mdb::Rgb* tiers = tier_colors(pal);
        for (int i = 0; i < mdb::kTierCount; ++i)
        {
            if (tiers[i] == marker_rgb(mdb::Cat::Chest, pal) ||
                tiers[i] == marker_rgb(mdb::Cat::Hidden, pal))
            {
                return false;
            }
            for (int j = i + 1; j < mdb::kTierCount; ++j)
            {
                if (tiers[i] == tiers[j])
                {
                    return false;
                }
            }
        }
        return true;
    }

    // No two categories share a shape, a mark and a colour.
    inline bool palette_is_separable(Palette pal)
    {
        for (int a = 0; a < mdb::kCatCount; ++a)
        {
            for (int b = a + 1; b < mdb::kCatCount; ++b)
            {
                const mdb::Cat ca = static_cast<mdb::Cat>(a);
                const mdb::Cat cb = static_cast<mdb::Cat>(b);
                if (shape_of(ca) == shape_of(cb) && mark_of(ca) == mark_of(cb) &&
                    marker_rgb(ca, pal) == marker_rgb(cb, pal))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // The loot family's own rule, independent of the palette: its silhouette belongs to it
    // alone, every one of the eleven carries it, and no two of them in the same tier - the
    // marks a player has to tell apart at a glance - carry the same mark. A mark is drawn
    // by nothing outside the family.
    inline bool loot_family_marks_ok()
    {
        for (int a = 0; a < mdb::kCatCount; ++a)
        {
            const mdb::Cat ca = static_cast<mdb::Cat>(a);
            if (mdb::is_loot_family(ca) != (shape_of(ca) == Shape::LootDisc))
            {
                return false;
            }
            if (!mdb::is_loot_family(ca) && mark_of(ca) != Mark::None)
            {
                return false;
            }
            mdb::Tier ta = mdb::Tier::Common;
            if (!mdb::tier_of(ca, ta))
            {
                continue;
            }
            for (int b = a + 1; b < mdb::kCatCount; ++b)
            {
                const mdb::Cat cb = static_cast<mdb::Cat>(b);
                mdb::Tier tb = mdb::Tier::Common;
                if (mdb::tier_of(cb, tb) && ta == tb && mark_of(ca) == mark_of(cb))
                {
                    return false;
                }
            }
        }
        return true;
    }
} // namespace gly
