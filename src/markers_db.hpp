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
    };

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
