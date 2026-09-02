#pragma once

//
// markers_db - the PURE half of the marker feature.
//
// Everything in here is plain C++ over strings and PODs: the marker model, the
// `markers/<chapter>.json` reader (schema `wuchang-minimap-markers/1`), the category
// name <-> bitmask mapping the config file and the F2 filter checkboxes share, and the
// `wuchang_minimap_found.txt` round-trip.
//
// It deliberately has NO dependency on Windows, on UE4SS, on Dear ImGui or on
// mm::log - which is what lets tests/markers_test.cpp link it into a console exe and
// exercise the loader, the filter parsing and the found-file round-trip without the
// game. Everything that needs the engine (the live actor sweep) lives in markers.cpp,
// and everything that needs a file handle lives in markers.cpp too; this layer only
// ever sees text that somebody else read.
//
// SCHEMA (context/common.md, 2026-09-02):
//   {"schema":"wuchang-minimap-markers/1","chapter":1,
//    "markers":[{"id":"<stable id>","cat":"shrine","cls":"BP_RebornFire_C",
//                "name":"<display name>","x":..,"y":..,"z":..,
//                "cell":"B1EX0_L0_X1_Y0","level":"<owning level short name>"}]}
//
// The stable id is the join key between this offline DB and the live actors:
//   * shrines      -> the game-authored shrine id (`digong01`), read at runtime from
//                     BP_RebornFire_C's CJK-named "sitting-Buddha point ID" property;
//   * everything else -> `<level short name>/<actor object name>`, because the cooked
//                     object name is what FindAllOf hands back at runtime.
//

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chapterid.hpp"

namespace mdb
{
    //==================================================================================
    // Categories
    //==================================================================================

    enum class Cat : std::uint8_t
    {
        Shrine = 0,
        Chest,
        Pickup,
        Boss,
        Elite,
        Enemy,
        Npc,
        Merchant,
        Door,
        Ladder,
        Lift,
        FogGate,
        Hidden,
        Other,
        Count
    };

    constexpr int kCatCount = static_cast<int>(Cat::Count);
    constexpr std::uint32_t kAllCats = (1u << kCatCount) - 1u;

    // The wire names used by markers.json AND by the config file, in Cat order.
    const char* cat_name(Cat cat);

    // The short label shown next to the F2 filter checkbox.
    const char* cat_label(Cat cat);

    // Exact, case-insensitive match against cat_name(). False for an unknown name.
    bool cat_from_name(std::string_view name, Cat& out);

    inline std::uint32_t cat_bit(Cat cat)
    {
        return 1u << static_cast<int>(cat);
    }

    inline bool cat_enabled(std::uint32_t mask, Cat cat)
    {
        return (mask & cat_bit(cat)) != 0u;
    }

    // "shrine,chest, pickup" (also accepts spaces / semicolons as separators) -> mask.
    // "all" and "none" are accepted as the whole value. Unknown names are collected
    // into `rejected` (comma separated) so the caller can log them; they never change
    // the result. An empty / all-unknown list yields `fallback`.
    std::uint32_t parse_category_mask(std::string_view text, std::uint32_t fallback, std::string* rejected = nullptr);

    // The inverse, for save_config_file(): "all", "none", or a comma-separated list.
    std::string format_category_mask(std::uint32_t mask);

    //==================================================================================
    // Item quality ("rarity")
    //==================================================================================
    //
    // Wuchang has NO rarity ladder. There is no `E_ItemQuality` / `Rarity` / `Grade`
    // enum anywhere in the paks, no quality word in `MMGame.locres`, and not one of the
    // six item row structs declares such a field (the offline hunt is written up in
    // `context/item-names-research.md`). What the game DOES have is the pickup beam:
    // `BP_PickupActor_C` picks a `DT_Particle` row whose `LightColor` is blue, pink or
    // gold, and which row it picks follows the item's `ItemType` (`E_ItemType`).
    //
    // So "rarity" here is the game's own three-way pickup grouping, produced offline by
    // `tools/markers/build_items.py` and baked into `markers/chapter*.json` as an
    // optional `"rarity"` field. Anything without one - every chest, every live-only
    // actor, every pickup whose item ids could not be resolved - is tier 0, which is
    // drawn exactly the way it was before this existed.

    enum class Rarity : std::uint8_t
    {
        Common = 0, // Tool / Arrows / EnchantingMaterial - the blue beam
        Equipment,  // weapons, armour, accessories, gems, spells - the pink beam
        Key,        // Material / SpecialItem (quest and upgrade items) - the gold beam
        Count
    };

    constexpr int kRarityCount = static_cast<int>(Rarity::Count);

    // Display name of a tier ("Common"); anything out of range reads as "Common".
    const char* rarity_name(int rarity);

    // Clamp an untrusted tier (a JSON field, a config index) into range.
    constexpr int rarity_clamp(int rarity)
    {
        return (rarity < 0 || rarity >= kRarityCount) ? 0 : rarity;
    }

    struct Rgb
    {
        std::uint8_t r = 255;
        std::uint8_t g = 255;
        std::uint8_t b = 255;
    };

    constexpr bool operator==(const Rgb& a, const Rgb& b)
    {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }

    // The GAME's own palette: the `LightColor` of `DT_Particle`'s `PickupEffect`,
    // `PickupEffect4` and `PickupEffect7` rows, converted from linear to sRGB
    // (0.420, 0.428, 0.700) / (0.700, 0.420, 0.560) / (0.701, 0.672, 0.418).
    inline constexpr Rgb kDefaultRarityColors[kRarityCount] = {
        Rgb{0xAD, 0xAF, 0xDA}, // Common    - blue
        Rgb{0xDA, 0xAD, 0xC5}, // Equipment - pink
        Rgb{0xDA, 0xD6, 0xAD}, // Key       - gold
    };

    // "ADAFDA, DAADC5, DAD6AD" -> `out`. Accepts a `#` prefix, either case, and comma /
    // semicolon / whitespace separators; the 3-digit form ("ABC") expands the way CSS
    // does. Entries beyond kRarityCount are ignored and missing ones keep whatever `out`
    // already held (so the caller seeds it with the defaults), while a malformed entry
    // still consumes its tier - otherwise one typo would shift every later colour onto
    // the wrong tier - and is appended to `rejected`. Returns how many tiers were set.
    int parse_rarity_colors(std::string_view text, Rgb out[kRarityCount], std::string* rejected = nullptr);

    // The inverse, for save_config_file(): "ADAFDA, DAADC5, DAD6AD".
    std::string format_rarity_colors(const Rgb in[kRarityCount]);

    //==================================================================================
    // The static marker database
    //==================================================================================

    struct StaticMarker
    {
        std::string id;    // stable id - the join key with the live actors
        std::string name;  // display name, may be empty
        std::string cls;   // "BP_RebornFire_C", for diagnostics only
        std::string level; // owning level short name, for diagnostics only
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        int chapter = 0; // 1..8, from the file's "chapter" field
        Cat cat = Cat::Other;
        // Item quality tier of what a pickup grants (see Rarity above). Absent from the
        // JSON == 0 == Common, which is what every non-pickup marker is.
        std::uint8_t rarity = 0;
    };

    // Does a static marker belong to the chapter the player is currently in?
    //
    // WHY THIS EXISTS: the DB is one flat set of all 3 601 markers of all six chapters,
    // and the chapters' world bounds overlap badly (chapter 4 covers nearly all of
    // chapter 1). Without this test the minimap, the full map, the compass and the
    // x-ray highlight all paint foreign chapters' markers over the current one.
    //
    // `detected` is `mapdata::detected_chapter()`:
    //   chid::kNone (-1)  nothing recognised yet -> do NOT filter, show everything
    //                     (a wrong hide is worse than a stale extra marker, and the
    //                     detector answers within a pump of the level streaming in);
    //   chid::kDlc  (0)   the DLC, which is exactly the bucket the DLC manifest's
    //                     non-numeric "chapter": "DLC" parses to;
    //   1..9              a numbered chapter.
    //
    // Pure and total - no clamping, no special cases beyond kNone.
    constexpr bool marker_in_chapter(int marker_chapter, int detected)
    {
        return detected == chid::kNone || marker_chapter == detected;
    }

    //==================================================================================
    // Absence as evidence of a collect
    //==================================================================================
    //
    // THE PROBLEM. An item collected before the mod existed leaves a static DB entry
    // and no live actor at all: the level saver parked the actor at (0,0,0) at load
    // time and a GC later freed it. `dying` and the (0,0,0) test can only speak for an
    // actor that still exists, so those markers stayed drawn for ever.
    //
    // WHY ABSENCE IS NORMALLY NOT EVIDENCE (lessons.md): an unloaded level and a
    // collected pickup are indistinguishable from the object array. The rule below is
    // what closes that gap - it only ever looks at a marker whose OWNING LEVEL the game
    // says is loaded right now, and only after a full pass over the object array has
    // completed since that level became loaded, so "I have not seen it" means "I looked
    // at every object in the game while its level was streamed in and it was not there".
    //
    // Five conditions, all required, plus a debounce:
    //   a) the feature is on and the marker's category is selected;
    //   b) it is not already marked found (nothing to do);
    //   c) its `level` matched one of the levels gamestate currently reports as loaded -
    //      an unmatched level NEVER marks;
    //   d) at least one full object-array round completed since that level was first
    //      seen loaded;
    //   e) no live twin answered in that round with a usable position and no collected
    //      flag. A twin that IS at (0,0,0) or `dying` is already handled by the normal
    //      rule, so it does not block this one.
    // and then `markers_absence_rounds` consecutive confirming rounds before the mark.
    //
    // Pure and total, so tests/markers_test.cpp can enumerate the whole truth table.

    struct AbsenceFacts
    {
        bool feature_on = false;               // markers_absence_marks
        bool cat_selected = false;             // markers_absence_categories
        bool already_found = false;
        bool level_known = false;              // the marker's level is in the loaded set
        bool full_round_since_level_load = false;
        // A live actor answered for this id in the round that just ended, had a usable
        // position (not the (0,0,0) parking spot) and carried no collected flag.
        bool twin_alive = false;
    };

    // Does the round that just ended CONFIRM the absence? (One tick of the debounce.)
    constexpr bool absence_round_confirms(const AbsenceFacts& f)
    {
        return f.feature_on && f.cat_selected && !f.already_found && f.level_known &&
               f.full_round_since_level_load && !f.twin_alive;
    }

    // Should the marker be auto-marked collected now? `streak` counts the consecutive
    // confirming rounds INCLUDING this one; `required_rounds` is markers_absence_rounds.
    constexpr bool absence_marks(const AbsenceFacts& f, int streak, int required_rounds)
    {
        return absence_round_confirms(f) && required_rounds >= 1 && streak >= required_rounds;
    }

    struct ParseReport
    {
        std::string schema;      // the file's own "schema" string
        // The file's "chapter" field. It is normally a number, but the DLC manifest
        // spells it "DLC" - so the numeric form is 0 there and `chapter_label` carries
        // whatever was actually written, for the log and the F2 counters.
        int chapter = 0;
        std::string chapter_label;
        std::size_t added = 0;   // markers appended to `out`
        std::size_t skipped = 0; // entries rejected (bad id / coords)
        std::size_t unknown_cat = 0; // entries whose "cat" was not a known name -> Other
        std::string error;       // empty on success; a human-readable reason otherwise
    };

    // Appends to `out` (so several chapter files accumulate into one DB).
    // Returns false and fills `report.error` when the text is not a usable manifest.
    bool parse_markers_json(std::string_view text, std::vector<StaticMarker>& out, ParseReport& report);

    //==================================================================================
    // The found tracker file - wuchang_minimap_found.txt
    //==================================================================================
    //
    // One stable id per line, `;` / `#` comments and blank lines ignored, whitespace
    // trimmed, duplicates collapsed. Same shape as the Wukong mod's file, so it stays
    // hand-editable.

    void found_parse(std::string_view text, std::vector<std::string>& out);

    // Sorted, with a one-line header. Sorting is what makes the file diffable and the
    // round-trip test exact.
    std::string found_serialize(std::vector<std::string> ids);

    //==================================================================================
    // Helpers shared with the runtime
    //==================================================================================

    // "BP_RebornFire_C /Game/Maps/.../Chapter1_DGong_logic.Chapter1_DGong_logic:
    //  PersistentLevel.BP_RebornFire_C_0"  ->  "Chapter1_DGong_logic"
    //
    // UObject::GetFullName() is the only cheap route to the owning level's name, and
    // the stable id needs it. Returns an empty string when the shape is unrecognised.
    std::string level_from_full_name(std::string_view full_name);

    // `<level short name>/<object name>`, or just `<object name>` when the level could
    // not be determined - never an empty string for a non-empty object name.
    std::string stable_id(std::string_view level, std::string_view object_name);
} // namespace mdb
