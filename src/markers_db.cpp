#include "markers_db.hpp"

#include <algorithm>
#include <cmath>

#include "json.hpp"

namespace mdb
{
    namespace
    {
        // Order must match enum Cat exactly - both directions of the mapping and the
        // config file's spelling depend on it.
        constexpr const char* kCatNames[kCatCount] = {
            "shrine", "chest", "pickup", "boss",   "elite",  "enemy",  "npc",
            "merchant", "door", "ladder", "lift",  "fog_gate", "hidden", "other",
        };

        constexpr const char* kCatLabels[kCatCount] = {
            "Shrines", "Chests", "Pickups", "Bosses", "Elites",  "Enemies", "NPCs",
            "Merchants", "Doors", "Ladders", "Lifts", "Fog gates", "Hidden", "Other",
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

    std::uint32_t parse_category_mask(std::string_view text, std::uint32_t fallback, std::string* rejected)
    {
        if (rejected != nullptr)
        {
            rejected->clear();
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
        report.chapter = chapter != nullptr ? static_cast<int>(chapter->number_or(0.0)) : 0;

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
                m.cat = Cat::Other;
                ++report.unknown_cat;
            }

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
} // namespace mdb
