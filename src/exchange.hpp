#pragma once

//
// exchange - the found list and the waypoints of one profile as a single JSON file,
// for backing them up or moving them between machines. Header-only and pure (json.hpp
// + mapview.hpp), so tests/markers_test.cpp links it.
//
// {
//   "schema": "wuchang-minimap-export-1",
//   "profile": "wuchang_minimap_found_0123.txt",
//   "found": ["Chapter1_DGong_logic/BP_treasurebox_C_12", ...],
//   "waypoints": [{"x": 1.0, "y": 2.0, "z": 3.0}, ...]
// }
//

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "mapview.hpp"

namespace xch
{
    inline constexpr std::string_view kSchema = "wuchang-minimap-export-1";

    struct Payload
    {
        std::string profile;
        std::vector<std::string> found;
        std::vector<mv::Waypoint> waypoints;
    };

    // Two waypoints closer than this in every axis are the same place (uu).
    inline constexpr double kWaypointEpsilon = mv::kWaypointSamePlace;

    struct MergeResult
    {
        int added = 0;      // appended to the set
        int duplicates = 0; // already there, within kWaypointEpsilon
        int dropped = 0;    // no room left
    };

    // Appends what `incoming` adds to `set`: a waypoint within kWaypointEpsilon of one
    // already present - including one this merge just added - is a duplicate, and what
    // does not fit under mv::kMaxWaypoints is counted rather than silently lost.
    inline MergeResult merge_waypoints(mv::WaypointSet& set, const std::vector<mv::Waypoint>& incoming)
    {
        MergeResult r{};
        for (const mv::Waypoint& wp : incoming)
        {
            bool dup = false;
            for (std::size_t i = 0; i < set.count && i < mv::kMaxWaypoints; ++i)
            {
                const mv::Waypoint& have = set.items[i];
                if (std::abs(have.x - wp.x) <= kWaypointEpsilon &&
                    std::abs(have.y - wp.y) <= kWaypointEpsilon &&
                    std::abs(have.z - wp.z) <= kWaypointEpsilon)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
            {
                ++r.duplicates;
                continue;
            }
            if (set.count >= mv::kMaxWaypoints)
            {
                ++r.dropped;
                continue;
            }
            set.items[set.count] = wp;
            set.items[set.count].set = true;
            ++set.count;
            ++r.added;
        }
        return r;
    }

    // A path the player typed into the import box. True for `C:\x`, `C:/x`, a UNC
    // `\\server\share` and a rooted `\x`; everything else is relative.
    inline bool path_is_absolute(std::wstring_view p)
    {
        if (p.size() >= 2 && p[1] == L':')
        {
            return true;
        }
        return !p.empty() && (p[0] == L'\\' || p[0] == L'/');
    }

    // What the import reads: the path as typed when it is absolute, otherwise the same
    // name under `dir` (the mod folder, where the exports land).
    inline std::wstring resolve_import_path(std::wstring_view dir, std::wstring_view typed)
    {
        if (typed.empty() || path_is_absolute(typed))
        {
            return std::wstring{typed};
        }
        return std::wstring{dir} + L"\\" + std::wstring{typed};
    }

    inline std::string json_escape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c : s)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8]{};
                    (void)std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    // 17 significant digits round-trips a double exactly.
    inline std::string json_num(double v)
    {
        if (!(v == v) || v > 1e300 || v < -1e300)
        {
            return "0";
        }
        char buf[64]{};
        (void)std::snprintf(buf, sizeof(buf), "%.17g", v);
        return std::string{buf};
    }

    inline std::string serialize(const Payload& p)
    {
        std::string out;
        out += "{\n";
        out += "  \"schema\": \"";
        out += kSchema;
        out += "\",\n";
        out += "  \"profile\": \"" + json_escape(p.profile) + "\",\n";
        out += "  \"found\": [";
        for (std::size_t i = 0; i < p.found.size(); ++i)
        {
            out += i == 0 ? "\n    \"" : ",\n    \"";
            out += json_escape(p.found[i]);
            out += "\"";
        }
        out += p.found.empty() ? "],\n" : "\n  ],\n";
        out += "  \"waypoints\": [";
        for (std::size_t i = 0; i < p.waypoints.size(); ++i)
        {
            const mv::Waypoint& w = p.waypoints[i];
            out += i == 0 ? "\n    " : ",\n    ";
            out += "{\"x\": " + json_num(w.x) + ", \"y\": " + json_num(w.y) + ", \"z\": " + json_num(w.z) + "}";
        }
        out += p.waypoints.empty() ? "]\n" : "\n  ]\n";
        out += "}\n";
        return out;
    }

    // False with `error` set when the text is not JSON, is not an object, or carries a
    // schema this build does not know.
    inline bool parse(std::string_view text, Payload& out, std::string& error)
    {
        mjson::JValue root;
        if (!mjson::JParser(text).parse(root) || root.kind != mjson::JValue::Kind::Object)
        {
            error = "not a JSON object";
            return false;
        }
        const mjson::JValue* schema = root.find("schema");
        if (schema == nullptr || schema->kind != mjson::JValue::Kind::String)
        {
            error = "no \"schema\" string";
            return false;
        }
        if (schema->str != kSchema)
        {
            error = "unknown schema \"" + schema->str + "\"";
            return false;
        }

        Payload p{};
        if (const mjson::JValue* profile = root.find("profile"); profile != nullptr)
        {
            p.profile = profile->string_or(std::string{});
        }
        if (const mjson::JValue* found = root.find("found");
            found != nullptr && found->kind == mjson::JValue::Kind::Array && found->arr)
        {
            for (const mjson::JValue& v : *found->arr)
            {
                if (v.kind == mjson::JValue::Kind::String && !v.str.empty())
                {
                    p.found.push_back(v.str);
                }
            }
        }
        if (const mjson::JValue* wps = root.find("waypoints");
            wps != nullptr && wps->kind == mjson::JValue::Kind::Array && wps->arr)
        {
            for (const mjson::JValue& v : *wps->arr)
            {
                const mjson::JValue* x = v.find("x");
                const mjson::JValue* y = v.find("y");
                if (x == nullptr || y == nullptr || x->kind != mjson::JValue::Kind::Number ||
                    y->kind != mjson::JValue::Kind::Number)
                {
                    continue;
                }
                const mjson::JValue* z = v.find("z");
                mv::Waypoint w{};
                w.set = true;
                w.x = x->num;
                w.y = y->num;
                w.z = z != nullptr ? z->number_or(0.0) : 0.0;
                p.waypoints.push_back(w);
            }
        }
        out = std::move(p);
        error.clear();
        return true;
    }
} // namespace xch
