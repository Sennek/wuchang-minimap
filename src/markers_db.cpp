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
            "shrine", "chest", "pickup", "boss",   "elite",  "enemy",  "npc",
            "note",   "door",  "ladder", "lift",   "fog_gate", "hidden", "other",
        };

        constexpr const char* kCatLabels[kCatCount] = {
            "Shrines", "Chests", "Pickups", "Bosses", "Elites",  "Enemies", "NPCs",
            "Notes",   "Doors",  "Ladders", "Lifts",  "Fog gates", "Hidden", "Other",
        };

        // Renamed categories: {what an older file says, what it means now}. Kept for
        // one release, so a 0.9.4 config or a stale `markers/` folder still parses.
        struct LegacyCatName
        {
            const char* name;
            Cat cat;
        };

        constexpr LegacyCatName kLegacyCatNames[] = {
            {"merchant", Cat::Note},
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

    bool cat_from_legacy_name(std::string_view name, Cat& out)
    {
        const std::string_view n = trim(name);
        for (const LegacyCatName& l : kLegacyCatNames)
        {
            if (iequal(n, l.name))
            {
                out = l.cat;
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
            else if (cat_from_legacy_name(token, cat))
            {
                // A renamed category still selects its slot - the setting the player
                // made is honoured - and is reported so the caller can say so once.
                mask |= cat_bit(cat);
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
    // Item quality ("rarity")
    //======================================================================================

    const char* rarity_name(int rarity)
    {
        switch (static_cast<Rarity>(rarity_clamp(rarity)))
        {
        case Rarity::Equipment:
            return "Equipment";
        case Rarity::Key:
            return "Key";
        case Rarity::Common:
        default:
            return "Common";
        }
    }

    namespace
    {
        bool is_list_sep(char c)
        {
            return c == ',' || c == ';' || is_space(c);
        }

        // -1 when `c` is not a hex digit.
        int hex_digit(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }

        bool parse_hex_rgb(std::string_view token, Rgb& out)
        {
            if (!token.empty() && token.front() == '#')
            {
                token.remove_prefix(1);
            }
            if (token.size() != 3 && token.size() != 6)
            {
                return false;
            }
            int d[6]{};
            for (std::size_t i = 0; i < token.size(); ++i)
            {
                d[i] = hex_digit(token[i]);
                if (d[i] < 0)
                {
                    return false;
                }
            }
            if (token.size() == 3)
            {
                // CSS shorthand: "ABC" == "AABBCC".
                out.r = static_cast<std::uint8_t>(d[0] * 17);
                out.g = static_cast<std::uint8_t>(d[1] * 17);
                out.b = static_cast<std::uint8_t>(d[2] * 17);
                return true;
            }
            out.r = static_cast<std::uint8_t>(d[0] * 16 + d[1]);
            out.g = static_cast<std::uint8_t>(d[2] * 16 + d[3]);
            out.b = static_cast<std::uint8_t>(d[4] * 16 + d[5]);
            return true;
        }
    } // namespace

    int parse_rarity_colors(std::string_view text, Rgb out[kRarityCount], std::string* rejected)
    {
        int set = 0;
        int slot = 0;
        std::size_t pos = 0;
        while (pos < text.size() && slot < kRarityCount)
        {
            while (pos < text.size() && is_list_sep(text[pos]))
            {
                ++pos;
            }
            const std::size_t start = pos;
            while (pos < text.size() && !is_list_sep(text[pos]))
            {
                ++pos;
            }
            if (pos == start)
            {
                break;
            }
            const std::string_view token = text.substr(start, pos - start);
            Rgb parsed{};
            if (parse_hex_rgb(token, parsed))
            {
                out[slot] = parsed;
                ++set;
            }
            else if (rejected != nullptr)
            {
                if (!rejected->empty())
                {
                    *rejected += ",";
                }
                rejected->append(token);
            }
            ++slot;
        }
        return set;
    }

    std::string format_rarity_colors(const Rgb in[kRarityCount])
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        for (int i = 0; i < kRarityCount; ++i)
        {
            if (i != 0)
            {
                out += ", ";
            }
            const std::uint8_t channels[3] = {in[i].r, in[i].g, in[i].b};
            for (const std::uint8_t v : channels)
            {
                out += kHex[(v >> 4) & 0x0F];
                out += kHex[v & 0x0F];
            }
        }
        return out;
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
        // Accept any minor of major 1; refuse anything else rather than guess at a
        // layout we have never seen.
        constexpr std::string_view kWant = "wuchang-minimap-markers/1";
        if (report.schema.compare(0, kWant.size(), kWant) != 0)
        {
            report.error = "unexpected schema \"" + report.schema + "\" (want " + std::string(kWant) + ")";
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
                // A manifest written by an older release: accept the old spelling for
                // one release rather than dumping 76 markers into `other`.
                if (cat_from_legacy_name(cat_text, m.cat))
                {
                    ++report.legacy_cat;
                }
                else
                {
                    m.cat = Cat::Other;
                    ++report.unknown_cat;
                }
            }

            // Item quality tier. Optional and additive: extract_markers.py omits it when
            // it is 0, so a file written before rarity existed reads as all-Common.
            const mjson::JValue* rv = entry.find("rarity");
            m.rarity = static_cast<std::uint8_t>(
                rv != nullptr ? rarity_clamp(static_cast<int>(rv->number_or(0.0))) : 0);

            // A per-marker "chapter" overrides the file's, so one file could in
            // principle carry several chapters.
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
