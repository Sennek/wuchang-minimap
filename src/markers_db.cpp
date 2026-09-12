#include "markers_db.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "json.hpp"

namespace mdb
{
    namespace
    {
        // Order must match enum Cat exactly - both directions of the mapping and the
        // config file's spelling depend on it.
        constexpr const char* kCatNames[kCatCount] = {
            "shrine",   "chest",
            "consumable", "item",   "harvest", "ammo",  "armour", "amulet",
            "weapon",   "jade",   "spell",   "material", "key",
            "boss",     "elite",  "enemy",   "bamboozling", "npc",
            "note",     "door",   "mystery_gate", "benediction_door",
            "ladder",   "lift",   "fog_gate",     "hidden", "other",
        };

        // The loot family's labels are the bucket names of `context/buckets.md`: the same
        // word names the F2 filter row, the glyph's tooltip and the marker a pickup with no
        // readable item gets from the extractor, so the three never disagree.
        constexpr const char* kCatLabels[kCatCount] = {
            "Shrines",    "Chests",
            "Consumable", "Item",   "Harvest", "Cannon ammo", "Armour", "Amulet",
            "Weapon",     "Jade",   "Spell",   "Material",    "Key item",
            "Bosses",     "Elites", "Enemies", "Bamboozlings", "NPCs",
            "Notes",      "Doors",  "Mystery gates", "Benediction doors",
            "Ladders",    "Lifts",  "Fog gates",     "Traps",  "Other",
        };

        // The last-resort SINGULAR word for one marker. `cat_label` is the plural filter
        // title ("Chests") and reads wrong on a single glyph; "Marker" is vague for
        // `other`, the bucket the classifier could not place.
        constexpr const char* kCatWords[kCatCount] = {
            "Shrine",     "Chest",
            "Consumable", "Item",   "Harvest", "Cannon ammo", "Armour", "Amulet",
            "Weapon",     "Jade",   "Spell",   "Material",    "Key item",
            "Boss",       "Elite",  "Enemy",   "Bamboozling", "NPC",
            "Note",       "Door",   "Mystery gate", "Benediction door",
            "Ladder",     "Lift",   "Fog gate",     "Hidden item", "Marker",
        };

        // Renamed categories: what an older file says, the bit(s) it selects in a config
        // list, and the single category one marker of that name becomes.
        struct LegacyCatName
        {
            const char* name;
            std::uint32_t mask;
            Cat cat;
        };

        constexpr LegacyCatName kLegacyCatNames[] = {
            {"merchant", cat_bit(Cat::Note), Cat::Note},
            // `pickup` split into the eleven buckets: a filter list means all of them, and
            // a marker still spelling it is one whose contents nothing resolved.
            {"pickup", kLootCats, Cat::Item},
        };

        char lower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool iequal(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (lower(a[i]) != lower(b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        bool is_space(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        std::string_view trim(std::string_view v)
        {
            std::size_t a = 0;
            std::size_t b = v.size();
            while (a < b && is_space(v[a]))
            {
                ++a;
            }
            while (b > a && is_space(v[b - 1]))
            {
                --b;
            }
            return v.substr(a, b - a);
        }
    } // namespace

    //======================================================================================
    // Categories
    //======================================================================================

    const char* cat_name(Cat cat)
    {
        const int i = static_cast<int>(cat);
        return (i >= 0 && i < kCatCount) ? kCatNames[i] : "other";
    }

    const char* cat_label(Cat cat)
    {
        const int i = static_cast<int>(cat);
        return (i >= 0 && i < kCatCount) ? kCatLabels[i] : "Other";
    }

    const char* cat_word(Cat cat)
    {
        const int i = static_cast<int>(cat);
        return (i >= 0 && i < kCatCount) ? kCatWords[i] : "Marker";
    }

    bool looks_like_class_name(std::string_view text)
    {
        const std::string_view t = trim(text);
        if (t.empty())
        {
            return false; // empty is not a class name, it is simply no label
        }
        // The two shapes a cooked blueprint class takes in this game: a `BP_` prefix
        // (`BP_DropItem_C`, `BP_PickupActor_C`) and the `_C` suffix every generated class
        // carries (`DKDC_NPC_C`, `Impl_BaseAIController_C`, `ItemCollectionBox_C`).
        if (t.size() >= 3 && lower(t[0]) == 'b' && lower(t[1]) == 'p' && (t[2] == '_' || t[2] == '-'))
        {
            return true;
        }
        if (t.size() >= 2 && t[t.size() - 2] == '_' && lower(t[t.size() - 1]) == 'c')
        {
            return true;
        }
        // The third shape: `pickup_actor` - a snake_case transliteration of a class name.
        // Anything with no space and an underscore, all lower case, is not a display name
        // in this game (every real one is English prose or a proper noun).
        bool has_underscore = false;
        bool has_space = false;
        bool has_upper = false;
        for (const char c : t)
        {
            has_underscore = has_underscore || c == '_';
            has_space = has_space || c == ' ';
            has_upper = has_upper || (c >= 'A' && c <= 'Z');
        }
        return has_underscore && !has_space && !has_upper;
    }

    const char* display_label(Cat cat, const char* raw)
    {
        if (raw == nullptr || raw[0] == '\0' || looks_like_class_name(raw))
        {
            return cat_word(cat);
        }
        return raw;
    }

    bool cat_from_name(std::string_view name, Cat& out)
    {
        const std::string_view n = trim(name);
        for (int i = 0; i < kCatCount; ++i)
        {
            if (iequal(n, kCatNames[i]))
            {
                out = static_cast<Cat>(i);
                return true;
            }
        }
        return false;
    }

    bool cat_from_legacy_name(std::string_view name, Cat& cat, std::uint32_t& mask)
    {
        const std::string_view n = trim(name);
        for (const LegacyCatName& l : kLegacyCatNames)
        {
            if (iequal(n, l.name))
            {
                cat = l.cat;
                mask = l.mask;
                return true;
            }
        }
        return false;
    }

    std::uint32_t parse_category_mask(std::string_view text, std::uint32_t fallback, std::string* rejected,
                                      std::string* legacy)
    {
        if (rejected != nullptr)
        {
            rejected->clear();
        }
        if (legacy != nullptr)
        {
            legacy->clear();
        }
        const std::string_view whole = trim(text);
        if (iequal(whole, "all"))
        {
            return kAllCats;
        }
        if (iequal(whole, "none"))
        {
            return 0u;
        }

        std::uint32_t mask = 0u;
        bool any = false;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= whole.size(); ++i)
        {
            const char c = (i < whole.size()) ? whole[i] : ',';
            if (c != ',' && c != ';' && c != ' ' && c != '\t')
            {
                continue;
            }
            const std::string_view token = trim(whole.substr(start, i - start));
            start = i + 1;
            if (token.empty())
            {
                continue;
            }
            Cat cat = Cat::Other;
            if (cat_from_name(token, cat))
            {
                mask |= cat_bit(cat);
                any = true;
            }
            else if (std::uint32_t legacy_mask = 0u; cat_from_legacy_name(token, cat, legacy_mask))
            {
                // A renamed category still selects its slot - every slot, where the name
                // split - and is reported so the caller can say so once.
                mask |= legacy_mask;
                any = true;
                if (legacy != nullptr)
                {
                    if (!legacy->empty())
                    {
                        *legacy += ",";
                    }
                    legacy->append(token);
                }
            }
            else if (rejected != nullptr)
            {
                if (!rejected->empty())
                {
                    *rejected += ",";
                }
                rejected->append(token);
            }
        }
        return any ? mask : fallback;
    }

    std::string format_category_mask(std::uint32_t mask)
    {
        mask &= kAllCats;
        if (mask == kAllCats)
        {
            return "all";
        }
        if (mask == 0u)
        {
            return "none";
        }
        std::string out;
        for (int i = 0; i < kCatCount; ++i)
        {
            if ((mask & (1u << i)) != 0u)
            {
                if (!out.empty())
                {
                    out += ",";
                }
                out += kCatNames[i];
            }
        }
        return out;
    }

    //======================================================================================
    // Item quality tier
    //======================================================================================

    const char* tier_name(int tier)
    {
        switch (static_cast<Tier>(tier))
        {
        case Tier::Equipment:
            return "Equipment";
        case Tier::Key:
            return "Key";
        case Tier::Common:
        default:
            return "Common";
        }
    }

    //======================================================================================
    // markers/<chapter>.json
    //======================================================================================

    bool parse_markers_json(std::string_view text, std::vector<StaticMarker>& out, ParseReport& report)
    {
        report = ParseReport{};

        // A UTF-8 BOM would otherwise make the very first token unparseable.
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text = text.substr(3);
        }

        mjson::JValue root{};
        if (!mjson::JParser{text}.parse(root) || root.kind != mjson::JValue::Kind::Object)
        {
            report.error = "not valid JSON, or the top level is not an object";
            return false;
        }

        const mjson::JValue* schema = root.find("schema");
        report.schema = schema != nullptr ? schema->string_or("") : "";
        // Accept any MINOR of major 1 - "…/1", "…/1.2" - and refuse everything else. A
        // plain prefix compare is not that test: it also accepts "…/10", a future major
        // with a different layout.
        constexpr std::string_view kWant = "wuchang-minimap-markers/1";
        const bool major_1 = report.schema.compare(0, kWant.size(), kWant) == 0 &&
                             (report.schema.size() == kWant.size() || report.schema[kWant.size()] == '.');
        if (!major_1)
        {
            report.error = "unexpected schema \"" + report.schema + "\" (want " + std::string(kWant) +
                           ", or any /1.x minor of it)";
            return false;
        }

        const mjson::JValue* chapter = root.find("chapter");
        if (chapter != nullptr && chapter->kind == mjson::JValue::Kind::Number)
        {
            report.chapter = static_cast<int>(chapter->num);
            report.chapter_label = std::to_string(report.chapter);
        }
        else if (chapter != nullptr && chapter->kind == mjson::JValue::Kind::String)
        {
            // "DLC" and anything else non-numeric: chapter 0, which the runtime groups
            // under a single "other" row rather than dropping.
            report.chapter = 0;
            report.chapter_label = chapter->str;
        }

        const mjson::JValue* markers = root.find("markers");
        if (markers == nullptr || markers->kind != mjson::JValue::Kind::Array || !markers->arr)
        {
            report.error = "no \"markers\" array";
            return false;
        }

        out.reserve(out.size() + markers->arr->size());
        for (const mjson::JValue& entry : *markers->arr)
        {
            if (entry.kind != mjson::JValue::Kind::Object)
            {
                ++report.skipped;
                continue;
            }
            const auto str = [&entry](const char* key) -> std::string {
                const mjson::JValue* v = entry.find(key);
                return v != nullptr ? v->string_or("") : std::string{};
            };
            const auto num = [&entry](const char* key, bool& ok) -> double {
                const mjson::JValue* v = entry.find(key);
                if (v == nullptr || v->kind != mjson::JValue::Kind::Number)
                {
                    ok = false;
                    return 0.0;
                }
                return v->num;
            };

            StaticMarker m{};
            m.id = str("id");
            m.name = str("name");
            m.cls = str("cls");
            m.level = str("level");
            bool ok = true;
            m.x = num("x", ok);
            m.y = num("y", ok);
            m.z = num("z", ok);
            if (m.id.empty() || !ok || !std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.z))
            {
                ++report.skipped;
                continue;
            }

            const std::string cat_text = str("cat");
            if (!cat_from_name(cat_text, m.cat))
            {
                // Accept the old spelling for one release rather than dumping 76 markers
                // into `other`.
                std::uint32_t legacy_mask = 0u;
                if (cat_from_legacy_name(cat_text, m.cat, legacy_mask))
                {
                    ++report.legacy_cat;
                }
                else
                {
                    m.cat = Cat::Other;
                    ++report.unknown_cat;
                }
            }

            // The boss' save-backed defeat signal. Optional: a manifest built without
            // build_bossdoors.py has no boss doors.
            m.bossdoor = str("bossdoor");

            // A per-marker "chapter" overrides the file's, so one file can carry several.
            const mjson::JValue* mc = entry.find("chapter");
            m.chapter = mc != nullptr ? static_cast<int>(mc->number_or(report.chapter)) : report.chapter;

            out.push_back(std::move(m));
            ++report.added;
        }
        return true;
    }

    //======================================================================================
    // wuchang_minimap_found.txt
    //======================================================================================

    void found_parse(std::string_view text, std::vector<std::string>& out)
    {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text = text.substr(3);
        }
        std::size_t pos = 0;
        while (pos <= text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            std::string_view line = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
            pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

            const std::size_t comment = line.find_first_of(";#");
            if (comment != std::string_view::npos)
            {
                line = line.substr(0, comment);
            }
            line = trim(line);
            if (line.empty())
            {
                continue;
            }
            std::string id{line};
            if (std::find(out.begin(), out.end(), id) == out.end())
            {
                out.push_back(std::move(id));
            }
        }
    }

    std::string found_serialize(std::vector<std::string> ids)
    {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

        std::string out;
        out += "; WuchangMinimap - collected / opened markers, one stable id per line.\n";
        out += "; Written automatically; hand edits are picked up on the next reload (F5).\n";
        out.reserve(out.size() + ids.size() * 24);
        for (const std::string& id : ids)
        {
            out += id;
            out += "\n";
        }
        return out;
    }

    //======================================================================================
    // Helpers
    //======================================================================================

    std::string level_from_full_name(std::string_view full_name)
    {
        // "<Class> /Game/Maps/.../Chapter1_DGong_logic.Chapter1_DGong_logic:PersistentLevel.BP_RebornFire_C_0"
        const std::size_t slash = full_name.find_last_of('/');
        if (slash == std::string_view::npos)
        {
            return {};
        }
        std::string_view tail = full_name.substr(slash + 1);
        const std::size_t stop = tail.find_first_of(".:");
        if (stop != std::string_view::npos)
        {
            tail = tail.substr(0, stop);
        }
        tail = trim(tail);
        return std::string{tail};
    }

    bool parse_items_json(std::string_view text, std::unordered_map<int, ItemInfo>& out,
                          std::string& error)
    {
        error.clear();
        out.clear();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text = text.substr(3);
        }
        mjson::JValue root{};
        if (!mjson::JParser{text}.parse(root) || root.kind != mjson::JValue::Kind::Object)
        {
            error = "not valid JSON, or the top level is not an object";
            return false;
        }
        const mjson::JValue* schema = root.find("schema");
        const std::string schema_str = schema != nullptr ? schema->string_or("") : "";
        // Any minor of the item database. The name is in every minor; the `bucket` field
        // arrived in /3 and an entry without one simply has no bucket, which leaves a live
        // pickup on its class rule.
        constexpr std::string_view kWant = "wuchang-minimap-items/";
        if (schema_str.compare(0, kWant.size(), kWant) != 0)
        {
            error = "unexpected schema \"" + schema_str + "\" (want " + std::string(kWant) + "N)";
            return false;
        }
        const mjson::JValue* items = root.find("items");
        if (items == nullptr || items->kind != mjson::JValue::Kind::Object || !items->obj)
        {
            error = "no \"items\" object";
            return false;
        }
        // The keys are the numeric item ids, as strings, exactly as the game's own
        // DataTable row FNames are.
        for (const auto& kv : *items->obj)
        {
            if (kv.first.empty())
            {
                continue;
            }
            char* end = nullptr;
            const long id = std::strtol(kv.first.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || id <= 0 || id > 1000000)
            {
                continue;
            }
            const mjson::JValue* name = kv.second.find("name");
            if (name == nullptr || name->kind != mjson::JValue::Kind::String || name->str.empty())
            {
                continue;
            }
            ItemInfo info{};
            info.name = name->str;
            const mjson::JValue* bucket = kv.second.find("bucket");
            if (bucket != nullptr && bucket->kind == mjson::JValue::Kind::String)
            {
                Cat cat = Cat::Other;
                if (cat_from_name(bucket->str, cat) && cat_enabled(kLootCats, cat))
                {
                    info.cat = cat;
                }
            }
            out.emplace(static_cast<int>(id), std::move(info));
        }
        if (out.empty())
        {
            error = "\"items\" carries no named entry";
            return false;
        }
        return true;
    }

    std::string stable_id(std::string_view level, std::string_view object_name)
    {
        const std::string_view lv = trim(level);
        const std::string_view on = trim(object_name);
        if (on.empty())
        {
            return {};
        }
        if (lv.empty())
        {
            return std::string{on};
        }
        std::string out;
        out.reserve(lv.size() + 1 + on.size());
        out.append(lv);
        out.push_back('/');
        out.append(on);
        return out;
    }

    std::string lower_ascii(std::string_view v)
    {
        std::string out;
        out.reserve(v.size());
        for (char c : v)
        {
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        return out;
    }

    void intern_levels(const std::vector<StaticMarker>& markers, std::vector<std::string>& levels,
                       std::vector<int>& marker_level)
    {
        levels.clear();
        marker_level.assign(markers.size(), -1);
        std::unordered_map<std::string, int> ids;
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            if (markers[i].level.empty())
            {
                continue;
            }
            std::string key = lower_ascii(markers[i].level);
            const auto it = ids.find(key);
            if (it != ids.end())
            {
                marker_level[i] = it->second;
                continue;
            }
            const int id = static_cast<int>(levels.size());
            levels.push_back(key);
            ids.emplace(std::move(key), id);
            marker_level[i] = id;
        }
    }
} // namespace mdb
