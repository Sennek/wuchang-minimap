#pragma once

//
// markers_db - the PURE half of the marker feature.
//
// Everything in here is plain C++ over strings and PODs: the marker model, the
// `markers/<chapter>.json` reader (schema `wuchang-minimap-markers/1`), the category
// name <-> bitmask mapping the config file and the F2 filter checkboxes share, and the
// `wuchang_minimap_found.txt` round-trip.
//
// It has NO dependency on Windows, on UE4SS, on Dear ImGui or on mm::log, which is what
// lets tests/markers_test.cpp link it into a console exe and exercise the loader, the
// filter parsing and the found-file round-trip without the game. Everything that needs the
// engine or a file handle lives in markers.cpp; this layer only ever sees text somebody
// else read.
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
        // The game's readable notes / inscriptions (`DKDC_NPC_C` and friends).
        // `cat_from_legacy_name()` maps the old `merchant` spelling onto this slot.
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
    // 3 601 static markers it always is one. A LIVE-ONLY entry has no static twin to take a
    // name from.
    //
    // Two functions, and both are needed:
    //   * `looks_like_class_name()` recognises the shapes this game's blueprints take
    //     (`BP_...`, anything ending `_C`, an all-lowercase `pickup_actor`-style
    //     transliteration of one), so a label that is really a class name can be refused
    //     wherever it comes from - the runtime, a hand-edited manifest, a future class
    //     table;
    //   * `cat_word()` is the FINAL fallback: one plain singular word per category, so the
    //     worst label the player can see is "Item" or "Enemy".
    //
    // Not a template on the drawing code: the same two rules hold for the x-ray label, the
    // minimap tooltip and the compass pip.
    bool looks_like_class_name(std::string_view text);

    // A plain singular word per category. Never empty.
    const char* cat_word(Cat cat);

    // The label to actually draw: `raw` when it is a real name, the category's plain word
    // otherwise. `raw` may be empty or a class name; the result never is.
    const char* display_label(Cat cat, const char* raw);

    // Exact, case-insensitive match against cat_name(). False for an unknown name.
    bool cat_from_name(std::string_view name, Cat& out);

    // The RENAMED categories of past releases, so a config file or a marker manifest
    // written by an older version still means what it said. `merchant` -> `Note`: all 76
    // of the slot's markers are reading points and the game's one actual merchant is typed
    // `npc`, so the alias is a spelling change, not a re-typing. Callers accept the value
    // AND warn, once, so Save rewrites the file with the current name.
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
    // "all" and "none" are accepted as the whole value. Unknown names are collected into
    // `rejected` (comma separated) and never change the result. An empty / all-unknown
    // list yields `fallback`.
    //
    // A legacy spelling (see cat_from_legacy_name) DOES set its category's bit and is
    // reported in `legacy` rather than in `rejected`.
    std::uint32_t parse_category_mask(std::string_view text, std::uint32_t fallback,
                                      std::string* rejected = nullptr, std::string* legacy = nullptr);

    // The inverse, for save_config_file(): "all", "none", or a comma-separated list.
    std::string format_category_mask(std::uint32_t mask);

    //==================================================================================
    // Item quality ("rarity")
    //==================================================================================
    //
    // Wuchang has NO rarity ladder: no `E_ItemQuality` / `Rarity` / `Grade` enum in the
    // paks, no quality word in `MMGame.locres`, no such field on any of the six item row
    // structs. What the game DOES have is the pickup beam: `BP_PickupActor_C` picks a
    // `DT_Particle` row whose `LightColor` is blue, pink or gold, following the item's
    // `ItemType` (`E_ItemType`).
    //
    // So "rarity" here is that three-way pickup grouping, produced offline by
    // `tools/markers/build_items.py` and baked into `markers/chapter*.json` as an optional
    // `"rarity"` field. Anything without one is tier 0.

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
    // semicolon / whitespace separators; the 3-digit form ("ABC") expands the way CSS does.
    // Entries beyond kRarityCount are ignored and missing ones keep whatever `out` already
    // held. A malformed entry still consumes its tier - otherwise one typo would shift
    // every later colour - and is appended to `rejected`. Returns how many tiers were set.
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
        // BOSS MARKERS ONLY: the `bossdoor_<abbr>` firepoint id the game's own level
        // script names for this boss (`tools/markers/build_bossdoors.py` reads it out of
        // the `_logic` level's `ST_LevelScriptBossData` beside the boss' own soft object
        // path). It is the id to look for in the save's `UnlockedFirepoints`, and the only
        // per-boss state that outlives the encounter. Empty for 2 of the 28 boss markers
        // and for every other category. See boss_found_from_save().
        std::string bossdoor;
    };

    // Does a static marker belong to the chapter the player is currently in?
    //
    // The DB is one flat set of all 3 601 markers of all six chapters, and the chapters'
    // world bounds overlap badly (chapter 4 covers nearly all of chapter 1). Without this
    // test the minimap, the full map, the compass and the x-ray highlight all paint
    // foreign chapters' markers over the current one.
    //
    // `detected` is `mapdata::detected_chapter()`:
    //   chid::kNone (-1)  nothing recognised yet -> do NOT filter, show everything
    //                     (a wrong hide is worse than a stale extra marker);
    //   chid::kDlc  (0)   the DLC, the bucket the DLC manifest's non-numeric
    //                     "chapter": "DLC" parses to;
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
    // An item collected before the mod existed leaves a static DB entry and no live actor
    // at all: the level saver parked the actor at (0,0,0) at load time and a GC later
    // freed it. `dying` and the (0,0,0) test can only speak for an actor that still
    // exists.
    //
    // Absence is normally NOT evidence: an unloaded level and a collected pickup are
    // indistinguishable from the object array. This rule only ever looks at a marker whose
    // OWNING LEVEL the game says is loaded right now, and only after a full pass over the
    // object array has completed since that level became loaded.
    //
    // Five conditions, all required, plus a debounce:
    //   a) the feature is on and the marker's category is selected;
    //   b) it is not already marked found;
    //   c) its `level` matched one of the levels gamestate currently reports as loaded -
    //      an unmatched level NEVER marks;
    //   d) at least one full object-array round completed since that level was first seen
    //      loaded;
    //   e) no live twin answered in that round with a usable position and no collected
    //      flag. A twin that IS at (0,0,0) or `dying` is handled by the normal rule, so it
    //      does not block this one.
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
    // The three-way answer the dead-enemy / defeated-boss feature turns on. UNKNOWN must
    // never collapse into DEAD: guessing "dead" hides living enemies, and guessing "alive"
    // makes a broken read indistinguishable from a quiet battlefield. The count of
    // UNKNOWNs is published for that reason (`health unknown`).
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
    // reads zero for both.
    constexpr Health health_answer(bool read_ok, double current, double max)
    {
        if (!read_ok)
        {
            return Health::Unknown;
        }
        // Hand-rolled finite test: std::isfinite is not constexpr. A NaN fails `v == v`,
        // and the bounds reject the infinities as well as the ~1e-317 denormals a
        // misaligned read produces.
        const auto finite = [](double v) { return v == v && v > -1e300 && v < 1e300; };
        if (!finite(current) || !finite(max) || max <= 0.0)
        {
            return Health::Unknown;
        }
        return current <= 0.0 ? Health::Dead : Health::Alive;
    }

    //==========================================================================
    // A BOSS KILLED BEFORE THE MOD EXISTED (pure, tested offline)
    //==========================================================================
    //
    // `Rule::BossPawn` reads the boss pawn's health, so it needs the boss actor to exist
    // AND to be dead. A boss killed before the mod was installed never spawns again. The
    // only per-boss state that outlives the encounter is in the SAVE:
    // `RebornManagerComponent_C::UnlockedFirepoints` carries 24 `bossdoor_<abbr>`
    // pseudo-points beside the 57 real shrine ids, and `tools/markers/build_bossdoors.py`
    // maps each one to its boss marker out of the game's own level scripts.
    //
    // Which boss a door belongs to is proven; WHEN the game unlocks one is not. The id is
    // the boss encounter's respawn firepoint, written into `restart/bossdoorfirepoint`
    // beside `PlayerInBossCombat` when the fight begins, so it may mean "fought and
    // respawned here" rather than "defeated". It is treated as DEFEATED, but:
    //
    //   * it is behind `boss_defeat_from_save`, so it can be switched off; and
    //   * it is NOT written to `wuchang_minimap_found_<slot>.txt` - it is DERIVED on every
    //     publish from the live save state, so a mark from an uncertain signal cannot
    //     outlive a fix to the rule.
    constexpr bool boss_found_from_save(bool feature_on, bool is_boss, bool has_door,
                                        bool door_unlocked)
    {
        return feature_on && is_boss && has_door && door_unlocked;
    }

    //==========================================================================
    // A CORPSE HAS TWO HALVES AND BOTH MUST GO (pure, tested offline)
    //==========================================================================
    //
    // An enemy exists twice in the published buffer: as its authored spawn point in
    // `markers/chapter*.json` (a STATIC marker) and as the pawn the live sweep found (a
    // LIVE entry). Erasing only the live one makes the marker JUMP BACK to the spawn point
    // on the next publish.
    //
    // Both predicates are called at the single publish point, so the minimap, the full
    // map, the compass and the x-ray cannot disagree.
    //
    // `live_dead` is `LiveEntry::dead`: the health read answered zero for this actor. The
    // entry is KEPT (refreshed every round the corpse is still in the object array) and
    // ages out once a GC takes the body, at which point a respawned enemy's spawn hint is
    // correct again.
    constexpr bool static_twin_is_hidden_by_corpse(bool has_live_twin, bool live_dead)
    {
        return has_live_twin && live_dead;
    }

    // ...and the live half. `pos_valid` is false both for an actor parked at the origin
    // and for a corpse (the dead branch clears it), so a dead entry is never drawn where
    // it fell.
    constexpr bool live_only_is_drawn(bool pos_valid, bool live_dead)
    {
        return pos_valid && !live_dead;
    }

    //==========================================================================
    // Categories that WALK AWAY (pure, tested offline)
    //==========================================================================
    //
    // An NPC is a person. The static database records where one was AUTHORED, and the game
    // moves them: talk to a quest NPC and it relocates, usually to a different sublevel
    // with a different placed actor and therefore a different marker id. The authored
    // position is only ever a hint.
    //
    // The rule, for these categories only:
    //   * when a LIVE actor answered for the id, its position wins (as in every category);
    //   * when the marker's own level is loaded and a full sweep round has finished since
    //     it loaded, and STILL no live actor answered, the actor is provably not there, so
    //     the static twin is not drawn at all;
    //   * when the level is not loaded we know nothing, so the hint is kept.
    // The found ("met") state is untouched either way: it lives in the found set.
    //
    // `Note` is not in here: a reading point is a thing on a wall, so its authored
    // position never goes stale.
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
        // stands: the position read failed, or it read the (0,0,0) parking spot this game
        // uses for a used-up actor. Case (b) below.
        bool live_twin_unlocatable = false;
        // A live actor answered for this id this round, we know exactly where it stands,
        // and it is INVISIBLE (`AActor::bHidden` / the game's own `bLocalHidden`, or the
        // root component's `bHiddenInGame` / `!bVisible`). See case (e).
        bool live_twin_invisible = false;
        bool level_known = false;              // the marker's level is in the loaded set
        bool full_round_since_level_load = false;
    };

    // Should this static marker be dropped from the published set entirely?
    //
    //   (a) a locatable live twin      -> KEEP, and the publish point draws the marker at
    //                                    the live position (the static hint is superseded)
    //   (b) an UNLOCATABLE live twin   -> HIDE. The "walked away" case, and it needs no
    //                                    level table: a live actor answering for this id is
    //                                    itself proof that the level is loaded, and this
    //                                    game parks a used-up actor at (0,0,0) exactly as
    //                                    it parks a collected pickup.
    //   (c) no live twin, level loaded
    //       and a full round has passed -> HIDE (nobody answered, so nobody is there)
    //   (d) no live twin, level unknown -> KEEP. We have not looked; a hint is all we have.
    //   (e) a locatable live twin that
    //       is HIDDEN in the game       -> HIDE. This game does not park or move a used-up
    //                                    NPC: it makes the actor INVISIBLE and puts the
    //                                    person you meet next somewhere else as a different
    //                                    placed actor. (The same mechanism is authored for
    //                                    bosses: `ST_LevelScriptBossData` has a
    //                                    "hide the corpse of a defeated boss" flag.) An
    //                                    invisible actor is not a person you can walk up
    //                                    to, so it must neither be drawn nor count as MET.
    //
    // (e) is tested before (a).
    constexpr bool mobile_twin_is_stale(const MobileTwinFacts& f)
    {
        if (!f.mobile)
        {
            return false;
        }
        if (f.live_twin_invisible)
        {
            return true; // (e)
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

    //==========================================================================
    // WHICH GATE DROPPED THIS MARKER FROM THE X-RAY? (pure, tested offline)
    //==========================================================================
    //
    // The x-ray draws from the SAME published buffer as the minimap, so "it is on the
    // minimap and not in the x-ray" is one of a handful of extra conditions. The gate is
    // one function that NAMES its reason and the drawing code counts the reasons per round.
    //
    // The gate is the minimap's, plus the radius:
    //   * `Category`  - `highlight_categories`, its own key on purpose (the x-ray is a
    //                   different question from the map);
    //   * `Found`     - only LOOT (a consumed thing) and a DEFEATED BOSS, and only while
    //                   "Hide collected loot" is on. A lit shrine, a met NPC and a read
    //                   note are still landmarks worth seeing through a wall;
    //   * `Live`      - people only. A static hint for somebody who has walked away must
    //                   never be drawn through a wall;
    //   * `Radius`    - 3D distance from the PLAYER, the only gate the minimap has not got.
    // Nothing else may be added here without a counter to go with it.
    enum class XrayDrop : std::uint8_t
    {
        Drawn = 0,
        Category,
        Found,
        Live,
        Radius,
    };

    struct XrayFacts
    {
        Cat cat = Cat::Other;
        bool cat_selected = false;  // cat_enabled(highlight_categories, cat)
        bool found = false;         // the published row carries kFlagFound
        bool show_found = false;    // !highlight_show_found is "hide collected loot"
        bool live = false;          // the published row carries kFlagLive
        bool within_radius = false; // 3D distance from the player <= highlight_radius
    };

    // Loot is a category where FINDING the thing consumes it, so a found one is noise.
    constexpr bool is_loot_cat(Cat cat)
    {
        return cat == Cat::Chest || cat == Cat::Pickup || cat == Cat::Hidden;
    }

    constexpr XrayDrop xray_gate(const XrayFacts& f)
    {
        if (!f.cat_selected)
        {
            return XrayDrop::Category;
        }
        if (!f.show_found && f.found && (is_loot_cat(f.cat) || f.cat == Cat::Boss))
        {
            return XrayDrop::Found;
        }
        if (is_mobile_category(f.cat) && !f.live)
        {
            return XrayDrop::Live;
        }
        if (!f.within_radius)
        {
            return XrayDrop::Radius;
        }
        return XrayDrop::Drawn;
    }

    // For the diagnostic line.
    constexpr const char* xray_drop_name(XrayDrop d)
    {
        switch (d)
        {
        case XrayDrop::Category:
            return "category";
        case XrayDrop::Found:
            return "found";
        case XrayDrop::Live:
            return "not live";
        case XrayDrop::Radius:
            return "radius";
        case XrayDrop::Drawn:
        default:
            return "drawn";
        }
    }

    struct ParseReport
    {
        std::string schema;      // the file's own "schema" string
        // The file's "chapter" field. Normally a number, but the DLC manifest spells it
        // "DLC" - so the numeric form is 0 there and `chapter_label` carries what was
        // written.
        int chapter = 0;
        std::string chapter_label;
        std::size_t added = 0;   // markers appended to `out`
        std::size_t skipped = 0; // entries rejected (bad id / coords)
        std::size_t unknown_cat = 0; // entries whose "cat" was not a known name -> Other
        // Entries whose "cat" was a RENAMED category (`merchant` -> `note`). Accepted,
        // counted, and worth one log line: a manifest spelling it the old way is a stale
        // `markers/` folder.
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
    // Names an enemy's dropped loot: a `BP_DropItem_C` is spawned while you play and has
    // no static twin, so no baked name. The actor carries its item id (the same inline
    // `Items` array the extractor reads out of the cooked package), so {id -> name} in
    // memory gives the label.
    //
    // Only the names are kept: the file also carries descriptions, a third of a megabyte
    // that is drawn nowhere.
    bool parse_items_json(std::string_view text, std::unordered_map<int, std::string>& out,
                          std::string& error);

    //==================================================================================
    // The found tracker file - wuchang_minimap_found.txt
    //==================================================================================
    //
    // One stable id per line, `;` / `#` comments and blank lines ignored, whitespace
    // trimmed, duplicates collapsed. Hand-editable.

    void found_parse(std::string_view text, std::vector<std::string>& out);

    // Sorted, with a one-line header. Sorting makes the file diffable and the round-trip
    // test exact.
    std::string found_serialize(std::vector<std::string> ids);

    //==================================================================================
    // Helpers shared with the runtime
    //==================================================================================

    // "BP_RebornFire_C /Game/Maps/.../Chapter1_DGong_logic.Chapter1_DGong_logic:
    //  PersistentLevel.BP_RebornFire_C_0"  ->  "Chapter1_DGong_logic"
    //
    // UObject::GetFullName() is the only cheap route to the owning level's name, and the
    // stable id needs it. Returns an empty string when the shape is unrecognised.
    std::string level_from_full_name(std::string_view full_name);

    // `<level short name>/<object name>`, or just `<object name>` when the level could
    // not be determined - never an empty string for a non-empty object name.
    std::string stable_id(std::string_view level, std::string_view object_name);

    // ASCII lower-case. The marker DB's level names and the ones UObject::GetFullName()
    // reports need not agree on case, so every level-name comparison goes through this.
    std::string lower_ascii(std::string_view v);

    //==================================================================================
    // Level-name interning
    //==================================================================================
    //
    // publish_round() asks "is this marker's level loaded, and since when" for every
    // marker of every round. Interning turns a lower-cased copy plus a string hash per
    // marker per second into one hash per UNIQUE level name per round plus an array index
    // per marker.
    //
    // Fills `levels` with the unique lower-cased level names in first-appearance order and
    // `marker_level` with one entry per marker: its index into `levels`, or -1 when the
    // marker names no level. Both outputs are overwritten.
    void intern_levels(const std::vector<StaticMarker>& markers, std::vector<std::string>& levels,
                       std::vector<int>& marker_level);
} // namespace mdb
