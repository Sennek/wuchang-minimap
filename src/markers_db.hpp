#pragma once

//
// markers_db - pure C++ marker model: the `markers/<chapter>.json` reader, the category
// name <-> bitmask mapping shared by the config file and the F2 filter, and the
// `wuchang_minimap_found.txt` round-trip. No Windows / UE4SS / ImGui / mm::log dependency.
//
// SCHEMA (`wuchang-minimap-markers/1`):
//   {"schema":"wuchang-minimap-markers/1","chapter":1,
//    "markers":[{"id":"<stable id>","cat":"shrine","cls":"BP_RebornFire_C",
//                "name":"<display name>","x":..,"y":..,"z":..,
//                "cell":"B1EX0_L0_X1_Y0","level":"<owning level short name>"}]}
//
// The stable id is the join key with the live actors: shrines use the game-authored shrine
// id (`digong01`) from BP_RebornFire_C's CJK-named "sitting-Buddha point ID" property;
// everything else uses `<level short name>/<actor object name>`.
//

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "chapterid.hpp"

namespace mdb
{
    enum class Cat : std::uint8_t
    {
        Shrine = 0,
        Chest,
        // ---- the loot family, in tier order ----
        //
        // One category per pickup bucket, decided offline from the first item the pickup
        // grants (`tools/markers/pickup_buckets.py`) or, for a pickup that grants none,
        // from its actor class. They share a glyph silhouette and their tier's colour;
        // tier_of() below is the one place that says which tier a category is in.
        Consumable,
        Item,    // contents unresolved: an ordinary pickup the extraction could not read
        Harvest, // a respawning resource node
        Ammo,    // the cannon resupply box
        Armour,
        Amulet,
        Weapon,
        Jade,
        Spell,
        Material,
        Key,
        // ---- end of the loot family ----
        Boss,
        Elite,
        Enemy,
        // The bamboo-shoot creature that flees and burrows when startled, and drops a
        // Bamboo Shoot for the Panda's shop. A category of its own because it is a finite
        // COLLECTION, not a mob: 20 in the whole game, the count of the game's own "Defeat
        // 20 Bamboozlings" achievement, and one slain never returns for that journey.
        Bamboozling,
        Npc,
        // The game's readable notes / inscriptions (`DKDC_NPC_C` and friends).
        Note,
        Door,
        // The two special doors, one category each because "which riddle is unanswered" and
        // "which chisel door is unopened" are different questions. Red riddle gate
        // (`BP_NewPuzzlesDoor_C`, 3) and gold chisel door (`BP_NewGetGeemDoor_C`, 7).
        MysteryGate,
        BenedictionDoor,
        Ladder,
        Lift,
        FogGate,
        Hidden,
        Other,
        Count
    };

    constexpr int kCatCount = static_cast<int>(Cat::Count);
    constexpr std::uint32_t kAllCats = (1u << kCatCount) - 1u;

    constexpr std::uint32_t cat_bit(Cat cat)
    {
        return 1u << static_cast<int>(cat);
    }

    constexpr bool cat_enabled(std::uint32_t mask, Cat cat)
    {
        return (mask & cat_bit(cat)) != 0u;
    }

    // The eleven pickup buckets as one mask: what `pickup` used to select, and what the
    // x-ray, the absence rule and the legacy config name mean by "loot the player picks up".
    constexpr std::uint32_t kLootCats =
        cat_bit(Cat::Consumable) | cat_bit(Cat::Item) | cat_bit(Cat::Harvest) |
        cat_bit(Cat::Ammo) | cat_bit(Cat::Armour) | cat_bit(Cat::Amulet) |
        cat_bit(Cat::Weapon) | cat_bit(Cat::Jade) | cat_bit(Cat::Spell) |
        cat_bit(Cat::Material) | cat_bit(Cat::Key);

    // ---- Item quality tier ----
    //
    // Wuchang has no rarity ladder. The tier is the three-way pickup-beam grouping
    // `BP_PickupActor_C` picks from `DT_Particle` by `E_ItemType`: the player reads it in
    // the world as the colour of the beam over a dropped item. Every loot bucket sits in
    // exactly one tier, so the tier is a coarsening of the category and needs no per-marker
    // field; the glyph palette gives the whole family its tier's hue.

    enum class Tier : std::uint8_t
    {
        Common = 0, // consumables, unresolved pickups, harvest nodes, cannon ammo - blue
        Equipment,  // weapons, armour, amulets, jades, spells                      - pink
        Key,        // materials and key items (quest and upgrade)                  - gold
        Count
    };

    constexpr int kTierCount = static_cast<int>(Tier::Count);

    // Which tier a category's colour comes from. False for every peer category: a chest's
    // contents are not resolved in the data and a door has no quality at all, so they keep
    // a palette row of their own.
    constexpr bool tier_of(Cat cat, Tier& out)
    {
        switch (cat)
        {
        case Cat::Consumable:
        case Cat::Item:
        case Cat::Harvest:
        case Cat::Ammo:
            out = Tier::Common;
            return true;
        case Cat::Armour:
        case Cat::Amulet:
        case Cat::Weapon:
        case Cat::Jade:
        case Cat::Spell:
            out = Tier::Equipment;
            return true;
        case Cat::Material:
        case Cat::Key:
            out = Tier::Key;
            return true;
        default:
            return false;
        }
    }

    // Is this category one of the eleven the former `pickup` split into?
    constexpr bool is_loot_family(Cat cat)
    {
        Tier t = Tier::Common;
        return tier_of(cat, t);
    }

    // Display name of a tier ("Common"); anything out of range reads as "Common".
    const char* tier_name(int tier);

    // The wire names used by markers.json AND by the config file, in Cat order.
    const char* cat_name(Cat cat);

    // The short label shown next to the F2 filter checkbox.
    const char* cat_label(Cat cat);

    // A label may never be a class name: this recognises `BP_...`, `..._C`, and lowercase
    // transliterations of either.
    bool looks_like_class_name(std::string_view text);

    // A plain singular word per category. Never empty.
    const char* cat_word(Cat cat);

    // The label to draw: `raw` when it is a real name, the category's plain word otherwise.
    const char* display_label(Cat cat, const char* raw);

    // Exact, case-insensitive match against cat_name(). False for an unknown name.
    bool cat_from_name(std::string_view name, Cat& out);

    // Accepts the RENAMED categories of past releases. `cat` is what ONE marker of that
    // name becomes and `mask` is what a config list of that name selects; the two differ
    // where a name SPLIT, as `pickup` did into the eleven loot buckets - a config line
    // selects all eleven, a single marker becomes `Item`, the bucket for a pickup whose
    // contents are unresolved. Callers take the value AND warn, once, so Save rewrites
    // the file with the current names.
    bool cat_from_legacy_name(std::string_view name, Cat& cat, std::uint32_t& mask);

    // "shrine,chest, weapon" (spaces / semicolons also separate) -> mask. "all" and "none" are
    // accepted as the whole value. Unknown names are collected into `rejected` and never change
    // the result; an empty / all-unknown list yields `fallback`. A legacy spelling DOES set its
    // bit and is reported in `legacy`.
    std::uint32_t parse_category_mask(std::string_view text, std::uint32_t fallback,
                                      std::string* rejected = nullptr, std::string* legacy = nullptr);

    // The inverse, for save_config_file(): "all", "none", or a comma-separated list.
    std::string format_category_mask(std::uint32_t mask);

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

    // ---- The static marker database ----

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
        // BOSS MARKERS ONLY: the `bossdoor_<abbr>` firepoint id the level script names for this
        // boss, and the id to look for in the save's `UnlockedFirepoints`. Empty for 2 of the 28
        // boss markers and for every other category. See boss_found_from_save().
        std::string bossdoor;
    };

    // Does a static marker belong to the chapter the player is in? The DB is one flat set of
    // all chapters and their world bounds overlap, so without this test foreign chapters'
    // markers paint over the current one. `detected` is `mapdata::detected_chapter()`:
    // chid::kNone (-1) = nothing recognised, do NOT filter; chid::kDlc (0) = the DLC bucket;
    // 1..9 = a numbered chapter.
    constexpr bool marker_in_chapter(int marker_chapter, int detected)
    {
        return detected == chid::kNone || marker_chapter == detected;
    }

    // ---- Categories with no COLLECTED state ----
    //
    // Meeting a person does not use them up: an NPC stands where they stand whether or not the
    // player has walked past, and a merchant is the marker one most needs to find again. So an
    // NPC has no collected state at all - no rule marks one, an id left in the found file by an
    // older build does not light one, and `markers_hide_found` never has anything to act on. The
    // only rule that takes an NPC off the map is mobility, in twin_drop(). Every place that
    // composes or offers the found flag asks this.
    constexpr bool has_found_state(Cat cat)
    {
        return cat != Cat::Npc;
    }

    // ---- Absence as evidence of a collect ----
    //
    // An item collected before the mod existed leaves a static DB entry and no live actor: the
    // level saver parks the actor at (0,0,0) and a GC frees it. Absence only counts under all
    // five conditions, plus a debounce:
    //   a) the feature is on and the marker's category is selected;
    //   b) it is not already marked found;
    //   c) its `level` is one gamestate reports as loaded - an unmatched level NEVER marks;
    //   d) a full object-array round completed since that level was first seen loaded;
    //   e) no live twin answered in that round with a usable position and no collected flag;
    // then `markers_absence_rounds` consecutive confirming rounds mark it.

    struct AbsenceFacts
    {
        bool feature_on = false;               // the absence rule is armed
        bool cat_selected = false;             // markers_absence_categories, and has_found_state()
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

    // ---- Has the player MET this note? ----
    //
    // The whole of `Rule::Proximity`, and the only thing that writes a note into the collection
    // tracker. "Met" is SEEN, not read: an actor loaded within `kMetRadius` of the player. People
    // are not asked - see has_found_state().
    //
    // A used-up one keeps its actor, its id and its position and is made INVISIBLE, so an
    // actor the player cannot see is never met, however close they stand - a consumed note
    // is a thing the player can neither see nor interact with, and marking it collected
    // writes a lie into a file that outlives the session. A visibility read that could not
    // answer counts as visible: this rule is what fills the tracker at all.
    constexpr double kMetRadius = 3000.0; // Unreal units, 1 uu = 1 cm

    struct MetFacts
    {
        bool player_pos_known = false; // the player's own position was read this round
        bool actor_pos_known = false;  // and so was the actor's - not the (0,0,0) parking spot
        bool known_invisible = false;  // the visibility read ANSWERED and said hidden
        double dist2 = 0.0;            // squared distance between the two, uu^2
    };

    // Is the player close enough for the question to arise at all?
    constexpr bool met_in_range(const MetFacts& f)
    {
        return f.player_pos_known && f.actor_pos_known && f.dist2 <= kMetRadius * kMetRadius;
    }

    constexpr bool met_marks(const MetFacts& f)
    {
        return met_in_range(f) && !f.known_invisible;
    }

    // ---- Is this character dead? ----
    //
    // UNKNOWN must never collapse into DEAD: guessing "dead" hides living enemies, guessing
    // "alive" makes a broken read look like a quiet battlefield. UNKNOWNs are counted.
    enum class Health
    {
        Unknown,
        Alive,
        Dead
    };

    // `read_ok` is "both numbers came back"; doubles because in UE5 a blueprint "float" IS a
    // double (see uereflect::read_numeric_prop). `max <= 0` is UNKNOWN, not dead: an
    // uninitialised or hot-swapped stat component reads zero for both.
    constexpr Health health_answer(bool read_ok, double current, double max)
    {
        if (!read_ok)
        {
            return Health::Unknown;
        }
        // Hand-rolled finite test: std::isfinite is not constexpr. The bounds reject the
        // infinities as well as the ~1e-317 denormals a misaligned read produces.
        const auto finite = [](double v) { return v == v && v > -1e300 && v < 1e300; };
        if (!finite(current) || !finite(max) || max <= 0.0)
        {
            return Health::Unknown;
        }
        return current <= 0.0 ? Health::Dead : Health::Alive;
    }

    // ---- Whose death is a find ----
    //
    // The three character categories that are a finite COLLECTION rather than a mob: the 28
    // bosses, the elites - the tougher `_High` / `_Special` variant of a mob, placed one or
    // two to a level - and the 20 Bamboozlings. One killed is done with for that journey, so
    // the kill is the found event and it is persisted. An ordinary enemy is not tracked at
    // all: its marker is a spawn point and the spawn point keeps working.
    constexpr bool slain_is_found(Cat cat)
    {
        return cat == Cat::Boss || cat == Cat::Elite || cat == Cat::Bamboozling;
    }

    // ---- A boss killed before the mod existed ----
    //
    // Such a boss never spawns again, so `Rule::BossPawn` cannot see it. The only per-boss
    // state that outlives the encounter is the save's
    // `RebornManagerComponent_C::UnlockedFirepoints`: 24 `bossdoor_<abbr>` pseudo-points beside
    // the 57 real shrine ids. The id is also written when a fight BEGINS, so it may mean
    // "fought here"; it sits behind `boss_defeat_from_save` and is derived from the live save on
    // every publish rather than written to `wuchang_minimap_found_<slot>.txt`.
    constexpr bool boss_found_from_save(bool feature_on, bool is_boss, bool has_door,
                                        bool door_unlocked)
    {
        return feature_on && is_boss && has_door && door_unlocked;
    }

    // ---- A corpse has two halves and both must go ----
    //
    // An enemy exists twice in the published buffer: its authored spawn point (a STATIC marker)
    // and the pawn the live sweep found (a LIVE entry). Erasing only the live one makes the
    // marker jump back to the spawn point on the next publish. The static half is `twin_drop`'s
    // `Corpse`; this is the live one. `pos_valid` is false both for an actor parked at the
    // origin and for a corpse, so a dead entry is never drawn where it fell.
    constexpr bool live_only_is_drawn(bool pos_valid, bool live_dead)
    {
        return pos_valid && !live_dead;
    }

    // ---- Categories that WALK AWAY ----
    //
    // The static DB records where an NPC was AUTHORED; the game relocates them, usually to a
    // different sublevel with a different marker id, so the authored position is only a hint.
    // For these categories a live actor's position wins and no live actor is evidence. `Note`
    // is excluded: a reading point is a thing on a wall.
    constexpr bool is_mobile_category(Cat cat)
    {
        return cat == Cat::Npc;
    }

    // ---- What the live twin says about drawing the static marker ----
    //
    // Every reason the publish point drops a static marker, named once and counted by name.

    enum class TwinDrop : std::uint8_t
    {
        Keep = 0,
        Corpse,     // a dead enemy: the body and the spawn point go together
        Invisible,  // the game has the actor hidden
        WalkedAway, // mobile only: an actor answered but cannot be located
        Absent,     // mobile only: nobody answered with the marker's own level loaded
    };

    constexpr int kTwinDropCount = 5;

    struct TwinFacts
    {
        bool mobile = false;         // is_mobile_category(marker.cat)
        bool live_twin = false;      // an entry exists for this id, of any age
        bool live_twin_dead = false; // LiveEntry::dead - a corpse still in the object array
        // A live actor answered for this id THIS round and we know where it stands.
        bool live_twin_this_round = false;
        // A live actor answered this round but we do NOT know where it stands: the position read
        // failed, or it read the (0,0,0) parking spot.
        bool live_twin_unlocatable = false;
        // A live actor answered this round and the visibility read said HIDDEN (`bHidden` /
        // `bLocalHidden`, or the root component's `bHiddenInGame` / `!bVisible`). False when no
        // route reads on that class: "could not ask" is not "visible".
        bool live_twin_invisible = false;
        bool level_known = false; // the marker's level is in the loaded set
        bool full_round_since_level_load = false;
    };

    // A HIDDEN ACTOR IS NOT THERE, whatever category it is. This game hides rather than
    // destroys, and it hides before it spawns: a used-up NPC, a note already read and loot a
    // quest has yet to switch on all keep their actor, their id and their authored position and
    // are simply not rendered. None of them is a thing the player can see, reach or take, so
    // none of them is drawn. Only the actor's OWN flags count - `bPerformanceHidden`, the
    // distance cull, is deliberately not among them.
    //
    // Everything after it is about MOBILITY and fires for mobile categories only: a locatable
    // answer wins, an unlocatable one means the actor was parked, and silence is proof only once
    // the marker's own level is loaded and a full sweep round has passed. With the level
    // unloaded nothing has been looked at and the authored hint stays.
    constexpr TwinDrop twin_drop(const TwinFacts& f)
    {
        if (f.live_twin && f.live_twin_dead)
        {
            return TwinDrop::Corpse;
        }
        if (f.live_twin_invisible)
        {
            return TwinDrop::Invisible;
        }
        if (!f.mobile || f.live_twin_this_round)
        {
            return TwinDrop::Keep;
        }
        if (f.live_twin_unlocatable)
        {
            return TwinDrop::WalkedAway;
        }
        return f.level_known && f.full_round_since_level_load ? TwinDrop::Absent : TwinDrop::Keep;
    }

    constexpr const char* twin_drop_name(TwinDrop d)
    {
        switch (d)
        {
        case TwinDrop::Corpse:
            return "corpse";
        case TwinDrop::Invisible:
            return "invisible";
        case TwinDrop::WalkedAway:
            return "walked away";
        case TwinDrop::Absent:
            return "absent";
        case TwinDrop::Keep:
        default:
            return "drawn";
        }
    }

    // ---- Which gate dropped this marker from the x-ray? ----
    //
    // The x-ray draws from the same published buffer as the minimap, plus a few extra
    // conditions. The gate NAMES its reason and the drawing code counts the reasons per round.
    // `Category` uses `highlight_categories`, a different question from the map's; `Radius` is
    // the one gate the minimap has not got. `Found` is `consumed_when_found` only, and only
    // while "Hide collected loot" is on; a lit shrine and a read note stay landmarks, and a
    // person has no collected state to gate on at all.
    // `Live` is people only: a hint for somebody who walked away is never drawn. Nothing else
    // may be added here without a counter to go with it.
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

    // FINDING the thing consumes it, so a found one is finished business and the x-ray
    // drops it. The loot family, the two containers it comes in, and everything whose
    // "found" is a kill (slain_is_found) - none of them is there any more once found. Every
    // other category's "found" is a visit: the shrine, the note and the door are all still
    // standing.
    constexpr bool consumed_when_found(Cat cat)
    {
        return cat == Cat::Chest || cat == Cat::Hidden || is_loot_family(cat) ||
               slain_is_found(cat);
    }

    // A landmark is a place you navigate BY, so using it does not use it up and
    // `markers_hide_found` never removes it. Shrines are the checkpoints: hiding a lit one
    // erases the way back through an area you have explored.
    constexpr bool is_landmark_cat(Cat cat)
    {
        return cat == Cat::Shrine;
    }

    // THE ONE DECISION every map surface asks: does `markers_hide_found` drop this marker?
    // The minimap, the full map and the full map's search all call this, so they agree.
    constexpr bool hidden_as_found(Cat cat, bool found, bool hide_found)
    {
        return found && hide_found && !is_landmark_cat(cat);
    }

    // Solid or hollow. A found marker is drawn hollow and dimmed; a landmark inverts it,
    // because the useful state is the USED one: a lit shrine is solid, an unlit one hollow.
    constexpr bool drawn_as_found(Cat cat, bool found)
    {
        return is_landmark_cat(cat) ? !found : found;
    }

    constexpr XrayDrop xray_gate(const XrayFacts& f)
    {
        if (!f.cat_selected)
        {
            return XrayDrop::Category;
        }
        if (!f.show_found && f.found && consumed_when_found(f.cat))
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
        // The file's "chapter" field. The DLC manifest spells it "DLC", so the numeric form is
        // 0 there and `chapter_label` carries what was written.
        int chapter = 0;
        std::string chapter_label;
        std::size_t added = 0;   // markers appended to `out`
        std::size_t skipped = 0; // entries rejected (bad id / coords)
        std::size_t unknown_cat = 0; // entries whose "cat" was not a known name -> Other
        // Entries whose "cat" was a RENAMED category (`merchant` -> `note`). Accepted, counted.
        std::size_t legacy_cat = 0;
        std::string error;       // empty on success; a human-readable reason otherwise
    };

    // Appends to `out`, so several chapter files accumulate into one DB. False + `report.error`
    // when the text is not a usable manifest.
    bool parse_markers_json(std::string_view text, std::vector<StaticMarker>& out, ParseReport& report);

    // ---- markers/items.json - the item database, at RUNTIME ----
    //
    // Describes an enemy's dropped loot: a `BP_DropItem_C` is spawned while you play and has
    // no static twin. The actor carries its item id, so {id -> name + bucket} in memory gives
    // both the label and the category. The file's descriptions are drawn nowhere.

    struct ItemInfo
    {
        std::string name;
        // The loot bucket this item puts a pickup in, straight from the file's own `bucket`
        // field (`tools/markers/pickup_buckets.py` decides it, once, offline). `Cat::Count`
        // when the file predates the field or spells a name this build does not know.
        Cat cat = Cat::Count;
    };

    // Only NAMED items are kept: an id that has no display name is what rejects a misread of
    // an actor's item array, so it must not become a known id.
    bool parse_items_json(std::string_view text, std::unordered_map<int, ItemInfo>& out,
                          std::string& error);

    // The category a live pickup carries. `class_cat` is what the class table gave the actor
    // and `item_cat` is the bucket of the first item it grants (`Cat::Count` when nothing
    // resolved).
    //
    // `Item` IS "contents unresolved", so it is the one answer an item can improve on: the
    // first item wins there, exactly as it does offline in `extract_markers.py`, and an
    // enemy's dropped armour draws as armour rather than as a nameless pickup. Every other
    // category is already an answer - a harvest node is a harvest node and a cannon resupply
    // box is ammo whatever it hands out (the offline class rule, `context/buckets.md`) - so
    // nothing else is re-categorised and a peer category is never touched at all.
    constexpr Cat live_loot_cat(Cat class_cat, Cat item_cat)
    {
        const bool to_loot = item_cat != Cat::Count && cat_enabled(kLootCats, item_cat);
        return class_cat == Cat::Item && to_loot ? item_cat : class_cat;
    }

    // ---- The found tracker file - wuchang_minimap_found.txt ----
    //
    // One stable id per line, `;` / `#` comments and blank lines ignored, whitespace trimmed,
    // duplicates collapsed. Hand-editable.

    void found_parse(std::string_view text, std::vector<std::string>& out);

    // Sorted, with a one-line header, so the file is diffable and the round-trip test exact.
    std::string found_serialize(std::vector<std::string> ids);

    // "BP_RebornFire_C /Game/Maps/.../Chapter1_DGong_logic.Chapter1_DGong_logic:
    //  PersistentLevel.BP_RebornFire_C_0"  ->  "Chapter1_DGong_logic"
    //
    // UObject::GetFullName() is the only cheap route to the owning level's name, and the
    // stable id needs it. Returns an empty string when the shape is unrecognised.
    std::string level_from_full_name(std::string_view full_name);

    // `<level short name>/<object name>`, or just `<object name>` when the level could
    // not be determined - never an empty string for a non-empty object name.
    std::string stable_id(std::string_view level, std::string_view object_name);

    // ASCII lower-case. The DB's level names and UObject::GetFullName()'s need not agree on
    // case, so every level-name comparison goes through this.
    std::string lower_ascii(std::string_view v);

    // ---- Level-name interning ----
    //
    // Fills `levels` with the unique lower-cased level names in first-appearance order and
    // `marker_level` with one entry per marker: its index into `levels`, or -1 when the marker
    // names no level. Both outputs are overwritten.
    void intern_levels(const std::vector<StaticMarker>& markers, std::vector<std::string>& levels,
                       std::vector<int>& marker_level);
} // namespace mdb
