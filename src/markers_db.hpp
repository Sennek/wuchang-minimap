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
#include <unordered_map>
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
        // The game's readable notes / inscriptions (`DKDC_NPC_C` and friends). This
        // slot shipped as `merchant` up to 0.9.4 and every one of its 76 markers was
        // in fact a reading point - see `cat_from_legacy_name()` for the alias that
        // keeps an existing config file working.
        Note,
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

    //==========================================================================
    // NO USER-FACING LABEL MAY EVER BE A CLASS NAME
    //==========================================================================
    //
    // A marker's label is the display name the offline extractor resolved, and for the
    // 3 601 static markers it always is one. A LIVE-ONLY entry has no static twin to take
    // a name from, so it used to be labelled with the CLASS SPEC's name out of
    // markers.cpp's `kClasses[]` - which is a base class, so an enemy-dropped item read
    // `BP_PickupActor_C 1 m` in the x-ray even though the actor is a `BP_DropItem_C`, and
    // that is not even the right class name, let alone a name.
    //
    // Two functions, and both are needed:
    //   * `looks_like_class_name()` recognises the shapes this game's blueprints take
    //     (`BP_...`, anything ending `_C`, an all-lowercase `pickup_actor`-style
    //     transliteration of one), so a label that is really a class name can be refused
    //     wherever it comes from - the runtime, a hand-edited manifest, a future class
    //     table;
    //   * `cat_word()` is the FINAL fallback: one plain singular word per category, so
    //     the worst label the player can ever see is "Item" or "Enemy".
    //
    // Deliberately not a template on the drawing code: the same two rules have to hold
    // for the x-ray label, the minimap tooltip and the compass pip, and three copies of
    // "is this a class name" is how the third one ends up worded differently.
    bool looks_like_class_name(std::string_view text);

    // A plain singular word per category. Never empty.
    const char* cat_word(Cat cat);

    // The label to actually draw: `raw` when it is a real name, the category's plain word
    // otherwise. `raw` may be empty or a class name; the result never is.
    const char* display_label(Cat cat, const char* raw);

    // Exact, case-insensitive match against cat_name(). False for an unknown name.
    bool cat_from_name(std::string_view name, Cat& out);

    // The RENAMED categories of past releases, so a config file or a marker manifest
    // written by an older version still means what it said. `merchant` -> `Note`: the
    // slot never held a merchant (all 76 of its markers are the game's reading points,
    // and the game's one actual merchant is typed `npc`), so the alias is a spelling
    // change, not a re-typing. Callers accept the value AND warn, once, so the file
    // gets rewritten with the current name the next time Save runs.
    bool cat_from_legacy_name(std::string_view name, Cat& out);

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
    //
    // A legacy spelling (see cat_from_legacy_name) DOES set its category's bit and is
    // reported in `legacy` (comma separated) rather than in `rejected`, so the caller
    // can say "your config still says `merchant`; it now reads `note`" once instead of
    // silently ignoring the line.
    std::uint32_t parse_category_mask(std::string_view text, std::uint32_t fallback,
                                      std::string* rejected = nullptr, std::string* legacy = nullptr);

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

    //==========================================================================
    // Is this character dead? (pure, tested offline)
    //==========================================================================
    //
    // The three-way answer the whole dead-enemy / defeated-boss feature turns on.
    // UNKNOWN is not a synonym for ALIVE and must never collapse into DEAD: guessing
    // "dead" hides living enemies, which is strictly worse than the bug it fixes, and
    // guessing "alive" makes a broken read indistinguishable from a quiet battlefield.
    // The count of UNKNOWNs is published for exactly that reason (`health unknown`).
    enum class Health
    {
        Unknown,
        Alive,
        Dead
    };

    // `read_ok` is "both numbers came back"; the numbers are doubles because in UE5 a
    // blueprint "float" IS a double (see uereflect::read_numeric_prop).
    //
    // `max <= 0` is UNKNOWN, not dead: an uninitialised or hot-swapped stat component
    // reads zero for both, and a rule that called that dead would erase every enemy in
    // a level that had just streamed in.
    constexpr Health health_answer(bool read_ok, double current, double max)
    {
        if (!read_ok)
        {
            return Health::Unknown;
        }
        // Hand-rolled finite test: std::isfinite is not constexpr. A NaN fails `v == v`,
        // and the bounds reject the infinities as well as the ~1e-317 denormals a
        // misaligned read produces (lessons.md: garbage doubles look like that).
        const auto finite = [](double v) { return v == v && v > -1e300 && v < 1e300; };
        if (!finite(current) || !finite(max) || max <= 0.0)
        {
            return Health::Unknown;
        }
        return current <= 0.0 ? Health::Dead : Health::Alive;
    }

    //==========================================================================
    // Categories that WALK AWAY (pure, tested offline)
    //==========================================================================
    //
    // An NPC is a person. The static database records where one was
    // AUTHORED, and the game moves them: talk to a quest NPC and it relocates,
    // usually to a different sublevel with a different placed actor and therefore a
    // different marker id. So the authored position is only ever a hint, and it goes
    // stale silently - the user held the x-ray key and read "NPC 2 m (found)" at a
    // spot the NPC had left.
    //
    // The rule, for these categories only:
    //   * when a LIVE actor answered for the id, its position wins (that is already
    //     how every category works);
    //   * when the marker's own level is loaded and a full sweep round has finished
    //     since it loaded, and STILL no live actor answered, the static twin is not
    //     just unseen - the actor is provably not there, so it is not drawn at all;
    //   * when the level is not loaded we know nothing, so the hint is kept.
    // The found ("met") state is untouched either way: it lives in the found set.
    //
    // `Note` used to be in here, back when it was called `merchant` and was believed
    // to be one. A reading point is a thing on a wall: it does not walk away, so its
    // authored position never goes stale and dropping it when no live actor answered
    // would only hide notes in a level that had not finished streaming.
    constexpr bool is_mobile_category(Cat cat)
    {
        return cat == Cat::Npc;
    }

    struct MobileTwinFacts
    {
        bool mobile = false;                   // is_mobile_category(marker.cat)
        // A live actor answered for this id THIS round and we know where it stands.
        bool live_twin_this_round = false;
        // A live actor answered for this id this round and we do NOT know where it
        // stands: the position read failed, or it read the (0,0,0) parking spot this
        // game uses for a used-up actor. See the comment on case (b) below.
        bool live_twin_unlocatable = false;
        bool level_known = false;              // the marker's level is in the loaded set
        bool full_round_since_level_load = false;
    };

    // Should this static marker be dropped from the published set entirely?
    //
    // THE FOUR CASES, and why the middle one is the one that was missing. Run 3's census
    // read `people - static 53, live 23, joined 21, superseded 0, level resident 21,
    // hidden 0` while the user was still seeing an x-ray label at a spot an NPC had left,
    // and the arithmetic says why: only 21 of the 53 static people were in a level this
    // reader could NAME as resident, so for the other 32 the hide rule could not fire
    // whatever else was true - `level_known` is derived from the enumerated level set,
    // which is a second, independent thing that can be wrong or incomplete.
    //
    //   (a) a locatable live twin      -> KEEP, and the publish point draws the marker at
    //                                    the live position (the static hint is superseded)
    //   (b) an UNLOCATABLE live twin   -> HIDE. This is the "walked away" case, and it
    //                                    needs no level table at all: a live actor
    //                                    answering for this id is itself proof that the
    //                                    level is loaded, and this game parks a used-up
    //                                    actor at (0,0,0) exactly as it parks a collected
    //                                    pickup, so "found the actor, cannot locate it" is
    //                                    the NORMAL state of a person who has moved on.
    //   (c) no live twin, level loaded
    //       and a full round has passed -> HIDE (nobody answered, so nobody is there)
    //   (d) no live twin, level unknown -> KEEP. We have not looked; a hint is all we have.
    constexpr bool mobile_twin_is_stale(const MobileTwinFacts& f)
    {
        if (!f.mobile)
        {
            return false;
        }
        if (f.live_twin_this_round)
        {
            return false; // (a)
        }
        if (f.live_twin_unlocatable)
        {
            return true; // (b)
        }
        return f.level_known && f.full_round_since_level_load; // (c) / (d)
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
        // Entries whose "cat" was a RENAMED category (`merchant` -> `note`). Accepted,
        // counted, and worth one log line: a manifest that still spells it the old way
        // is a stale `markers/` folder, which is exactly the thing a player copies over
        // a new release and then wonders about.
        std::size_t legacy_cat = 0;
        std::string error;       // empty on success; a human-readable reason otherwise
    };

    // Appends to `out` (so several chapter files accumulate into one DB).
    // Returns false and fills `report.error` when the text is not a usable manifest.
    bool parse_markers_json(std::string_view text, std::vector<StaticMarker>& out, ParseReport& report);

    //==================================================================================
    // markers/items.json - the item display-name database, at RUNTIME
    //==================================================================================
    //
    // The offline extractor already bakes an item's name into every static pickup
    // marker, so this file used to be read only by the toolchain. What needs it at
    // runtime is the loot an ENEMY DROPS: a `BP_DropItem_C` is spawned while you play, it
    // has no static twin and therefore no name, and the x-ray labelled it with the class
    // spec's base class - `BP_PickupActor_C 1 m`. The actor does carry its item id (the
    // same inline `Items` array the extractor reads out of the cooked package), so with
    // {id -> name} in memory the label becomes the item's own name.
    //
    // Only the names are kept: the file also carries descriptions, which are a third of a
    // megabyte and are not drawn anywhere.
    bool parse_items_json(std::string_view text, std::unordered_map<int, std::string>& out,
                          std::string& error);

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

    // ASCII lower-case. The marker DB's level names and the ones UObject::GetFullName()
    // reports need not agree on case, so every level-name comparison in this mod goes
    // through this.
    std::string lower_ascii(std::string_view v);

    //==================================================================================
    // Level-name interning
    //==================================================================================
    //
    // publish_round() asks "is this marker's level loaded, and since when" for every
    // marker of every round. Answering it per marker means a lower-cased copy plus a
    // string hash per marker per second; interning turns it into one hash per UNIQUE
    // level name per round plus an array index per marker.
    //
    // Fills `levels` with the unique lower-cased level names in first-appearance order
    // and `marker_level` with one entry per marker: its index into `levels`, or -1 when
    // the marker names no level. Both outputs are overwritten.
    void intern_levels(const std::vector<StaticMarker>& markers, std::vector<std::string>& levels,
                       std::vector<int>& marker_level);
} // namespace mdb
