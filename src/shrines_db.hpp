#pragma once

//
// shrines_db - the pure parser for `markers/shrines.json`, produced by
// tools/markers/extract_shrines.py from the game's `DT_FirePoint` DataTable. Per row:
//
//   id       the game's shrine id - also the marker DB's marker id and the string in
//            `RebornManagerComponent_C::UnlockedFirepoints`, so it is the join key;
//   name     localised display name from MMGame.locres ("Reverent Temple");
//   chapter  1..5, or 0 for the DLC bucket;
//   x/y/z    the shrine ACTOR's world position, joined from the marker DB;
//   bx/by/bz `BirthPosition`, the game's own travel destination for that id;
//   shrine   false for the `bossdoor_*` / `Task*` pseudo-rows, which share the table and
//            the unlocked list but are not places to travel to.
//
// No Windows, no UE4SS, no ImGui, so tests parse the shipped file on the build machine.
//

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace shdb
{
    inline constexpr const char* kSchema = "wuchang-minimap-shrines/1";

    // Longest id in the table is 23 chars (`Task_Special_Level_Back`); the runtime's
    // fixed-size copies use 40.
    inline constexpr std::size_t kMaxIdLen = 40;

    struct Shrine
    {
        std::string id;
        std::string name;    // "" when the locres lookup found nothing
        int chapter = -1;    // -1 = unknown, 0 = the DLC bucket
        bool shrine = false; // false = a bossdoor_/Task pseudo-row
        bool has_pos = false;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        bool has_birth = false;
        double bx = 0.0;
        double by = 0.0;
        double bz = 0.0;

        // An id is a poor label but it is never empty.
        const std::string& label() const
        {
            return name.empty() ? id : name;
        }
    };

    struct Report
    {
        std::string error; // "" = parsed
        int rows = 0;
        int shrines = 0; // rows with shrine == true
        int named = 0;
    };

    // Replaces `out`. Returns false with `rep.error` set on a wrong schema, a missing
    // `shrines` array, or a non-object root. A single bad ENTRY is skipped, not fatal.
    inline bool parse(std::string_view text, std::vector<Shrine>& out, Report& rep)
    {
        out.clear();
        rep = Report{};
        mjson::JValue root{};
        if (!mjson::JParser{text}.parse(root) || root.kind != mjson::JValue::Kind::Object)
        {
            rep.error = "not a JSON object";
            return false;
        }
        const mjson::JValue* schema = root.find("schema");
        if (schema == nullptr || schema->kind != mjson::JValue::Kind::String)
        {
            rep.error = "no \"schema\" string";
            return false;
        }
        if (schema->str != kSchema)
        {
            rep.error = "schema is \"" + schema->str + "\", expected \"" + kSchema + "\"";
            return false;
        }
        const mjson::JValue* arr = root.find("shrines");
        if (arr == nullptr || arr->kind != mjson::JValue::Kind::Array || !arr->arr)
        {
            rep.error = "no \"shrines\" array";
            return false;
        }
        for (const mjson::JValue& e : *arr->arr)
        {
            if (e.kind != mjson::JValue::Kind::Object)
            {
                continue;
            }
            const mjson::JValue* id = e.find("id");
            if (id == nullptr || id->kind != mjson::JValue::Kind::String || id->str.empty())
            {
                continue;
            }
            Shrine s{};
            s.id = id->str;
            const mjson::JValue* name = e.find("name");
            if (name != nullptr && name->kind == mjson::JValue::Kind::String)
            {
                s.name = name->str;
            }
            const mjson::JValue* ch = e.find("chapter");
            if (ch != nullptr && ch->kind == mjson::JValue::Kind::Number)
            {
                s.chapter = static_cast<int>(ch->num);
            }
            const mjson::JValue* sh = e.find("shrine");
            s.shrine = sh != nullptr && sh->kind == mjson::JValue::Kind::Bool && sh->b;
            const auto vec = [&e](const char* kx, const char* ky, const char* kz, double& x, double& y,
                                  double& z) {
                const mjson::JValue* a = e.find(kx);
                const mjson::JValue* b = e.find(ky);
                const mjson::JValue* c = e.find(kz);
                if (a == nullptr || b == nullptr || c == nullptr ||
                    a->kind != mjson::JValue::Kind::Number || b->kind != mjson::JValue::Kind::Number ||
                    c->kind != mjson::JValue::Kind::Number)
                {
                    return false;
                }
                x = a->num;
                y = b->num;
                z = c->num;
                return true;
            };
            s.has_pos = vec("x", "y", "z", s.x, s.y, s.z);
            s.has_birth = vec("bx", "by", "bz", s.bx, s.by, s.bz);
            ++rep.rows;
            rep.shrines += s.shrine ? 1 : 0;
            rep.named += s.name.empty() ? 0 : 1;
            out.push_back(std::move(s));
        }
        return true;
    }

    // Index of the entry with this id, or -1. Case-insensitive on ASCII: the game is
    // inconsistent (`Task1` next to `digong01`).
    inline int find_id(const std::vector<Shrine>& list, std::string_view id)
    {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const std::string& a = list[i].id;
            if (a.size() != id.size())
            {
                continue;
            }
            bool same = true;
            for (std::size_t k = 0; k < a.size() && same; ++k)
            {
                same = lower(a[k]) == lower(id[k]);
            }
            if (same)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Is this id a real shrine rather than a `bossdoor_*` / `Task*` pseudo-point?
    // An unknown id counts as NOT a shrine.
    inline bool is_shrine_id(const std::vector<Shrine>* list, std::string_view id)
    {
        if (list == nullptr)
        {
            return false;
        }
        const int i = find_id(*list, id);
        return i >= 0 && (*list)[static_cast<std::size_t>(i)].shrine;
    }
} // namespace shdb
