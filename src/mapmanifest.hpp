#pragma once

//
// mapmanifest - `maps/maps.json`, parsed. Pure: json.hpp and the standard library, no
// Windows / WIC / UE4SS.
//
// SCHEMA `wuchang-minimap-maps/5`, produced by tools/navmesh/build_map.py (and by
// tools/navmesh/repack_maps.py, which re-encodes an already-shipped tree):
//
//     { "schema": "wuchang-minimap-maps/5",
//       "chapters": {
//         "chapter1": { "chapter": 1, "image": "chapter1/small.png",
//                       "image_width": ..., "image_height": ...,
//                       "min_x": ..., "min_y": ..., "max_x": ..., "max_y": ...,
//                       "px_per_uu": ..., "z_min": ..., "z_max": ...,
//                       "z_bits": 12, "z_code_max": 4095,
//                       "max_surfaces": 8,
//                       "height_planes": ["chapter1/small_h0.png", ...] },
//         "chapter2": { ... }, ... } }
//
// A HEIGHT CODE is one 16-bit sample of a height plane:
//
//     bits 0..11   the Z code, 1..4095 over [z_min, z_max]; 0 = no surface here
//     bit  12      REACHABLE - a marker-seeded walk-and-fall flood reached this surface
//     bits 13..15  zero
//
// Two schemas are accepted. /5 carries bit 12; /4 does not, and a /4 asset is read with
// `has_reachability` false, which makes every surface reachable. /3 is refused: its
// codes span the full 16 bits, so a /3 plane read here lands sixteen times off and looks
// like an empty map rather than a version error.
//
// Fields worth naming:
//
//   * `chapter` - the chapter number, stated rather than spelled into the key. Absent
//     -> derived from the key.
//   * `z_bits` / `z_code_max` - the height quantisation. The runtime divides by
//     `z_code_max`, so a wider asset needs no code change.
//   * `max_surfaces_requested` - what was asked for before empty trailing planes were
//     dropped. Informational.
//   * `height_tiles_128` / `height_tile_ram_bytes` - the cost of mapdata.cpp's sparse
//     tile store, measured by the pipeline. Informational.
//
// `px_per_uu` is per chapter: each is scaled to its own RAM budget (0.032 to 0.060).
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chapterid.hpp"
#include "json.hpp"

namespace mapmanifest
{
    // Must match tools/navmesh/mapfmt.py's SCHEMA. Compared for exact equality, so a
    // schema this build has never heard of is a stated version error and not a map that
    // silently decodes wrong.
    inline constexpr const char* kSchema = "wuchang-minimap-maps/5";
    // Accepted too: the same geometry and the same 12-bit Z, without bit 12.
    inline constexpr const char* kSchemaNoReach = "wuchang-minimap-maps/4";

    // Height-quantisation fallbacks for a file that omits them.
    inline constexpr int kZBits = 12;
    inline constexpr int kZCodeMax = (1 << kZBits) - 1; // 4095; 0 = "no surface"

    // The height code's two fields (see the header comment).
    inline constexpr std::uint16_t kZCodeMask = 0x0FFF;
    inline constexpr std::uint16_t kReachableBit = 0x1000;

    // How many stacked walkable surfaces one pixel can carry. Must match
    // build_map.py's --max-surfaces (shipped: 8).
    inline constexpr int kMaxSurfaces = 8;

    struct Entry
    {
        std::string key;   // "chapter1"
        std::string image; // "chapter1/small.png", relative to the maps dir
        int chapter = chid::kNone;
        int image_width = 0;
        int image_height = 0;
        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        double px_per_uu = 0.0;
        double z_min = 0.0;
        double z_max = 0.0;
        // Height quantisation: `code = 1 + round(t * (z_code_max - 1))`, 0 = no surface.
        int z_bits = kZBits;
        int z_code_max = kZCodeMax;
        int max_surfaces = 0;
        std::vector<std::string> height_maps; // lowest surface first
        // The file carried no `height_planes`; the names were guessed from the
        // composite's.
        bool height_maps_guessed = false;
        // The planes carry bit 12 (schema /5). False for a /4 asset, where every
        // surface counts as reachable.
        bool has_reachability = false;

        // uu per height code - what HeightMaps::z_step() reproduces at runtime.
        double z_step_uu() const
        {
            return z_code_max > 1 && z_max > z_min ? (z_max - z_min) / (z_code_max - 1) : 0.0;
        }

        bool geometry_ok() const
        {
            return !image.empty() && image_width > 0 && image_height > 0 && px_per_uu > 0.0 &&
                   max_x > min_x && max_y > min_y;
        }

        bool heights_ok() const
        {
            return !height_maps.empty() && z_max > z_min && z_code_max > 1;
        }
    };

    struct Manifest
    {
        std::string schema;
        std::vector<Entry> chapters; // in the order the file lists them

        bool schema_ok() const
        {
            return schema == kSchema || schema == kSchemaNoReach;
        }

        // The planes carry bit 12. Mirrored onto every Entry.
        bool has_reachability() const
        {
            return schema == kSchema;
        }

        // Index of the chapter with this chapter NUMBER, or -1.
        int index_of_number(int number) const
        {
            for (std::size_t i = 0; i < chapters.size(); ++i)
            {
                if (chapters[i].chapter == number)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        int index_of_key(std::string_view key) const
        {
            for (std::size_t i = 0; i < chapters.size(); ++i)
            {
                if (chapters[i].key == key)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // The chapter resident before the player's location is known: the lowest
        // chapter number present.
        int default_index() const
        {
            int best = -1;
            for (std::size_t i = 0; i < chapters.size(); ++i)
            {
                if (chapters[i].chapter < 1)
                {
                    continue;
                }
                if (best < 0 || chapters[i].chapter < chapters[static_cast<std::size_t>(best)].chapter)
                {
                    best = static_cast<int>(i);
                }
            }
            return best >= 0 ? best : (chapters.empty() ? -1 : 0);
        }
    };

    namespace detail
    {
        inline double number_or(const mjson::JValue& obj, const char* key, double fallback)
        {
            const mjson::JValue* v = obj.find(key);
            return v != nullptr ? v->number_or(fallback) : fallback;
        }

        inline std::string stem_of(const std::string& image)
        {
            const std::size_t dot = image.rfind('.');
            return dot == std::string::npos ? image : image.substr(0, dot);
        }
    } // namespace detail

    // Parses `text`, collecting one line in `problems` per skipped chapter. False only
    // when the document itself is unusable (bad JSON, no "chapters" object); a file
    // whose every chapter is broken parses fine and yields an empty list.
    inline bool parse(std::string_view text, Manifest& out, std::vector<std::string>& problems)
    {
        out = Manifest{};

        mjson::JValue root{};
        if (!mjson::JParser{text}.parse(root) || root.kind != mjson::JValue::Kind::Object)
        {
            problems.emplace_back("maps.json is not valid JSON");
            return false;
        }
        const mjson::JValue* schema = root.find("schema");
        out.schema = schema != nullptr ? schema->string_or("") : "";
        if (!out.schema_ok())
        {
            // Fatal: a mismatched height encoding decodes at sixteen times the wrong
            // scale and looks like an empty map rather than an error.
            problems.push_back("maps.json is schema \"" +
                               (out.schema.empty() ? std::string("(none)") : out.schema) +
                               "\" but this build reads \"" + std::string(kSchema) +
                               "\" or \"" + std::string(kSchemaNoReach) +
                               "\" - re-run tools/navmesh/build_map.py (or "
                               "tools/navmesh/repack_maps.py) and deploy.ps1, or install the "
                               "matching mod version");
            return false;
        }

        const mjson::JValue* chapters = root.find("chapters");
        if (chapters == nullptr || chapters->kind != mjson::JValue::Kind::Object || !chapters->obj)
        {
            problems.emplace_back("maps.json has no \"chapters\" object");
            return false;
        }

        for (const auto& kv : *chapters->obj)
        {
            const mjson::JValue& c = kv.second;
            Entry e{};
            e.key = kv.first;
            e.has_reachability = out.has_reachability();
            const mjson::JValue* image = c.find("image");
            e.image = image != nullptr ? image->string_or("") : "";
            e.image_width = static_cast<int>(detail::number_or(c, "image_width", 0.0));
            e.image_height = static_cast<int>(detail::number_or(c, "image_height", 0.0));
            e.min_x = detail::number_or(c, "min_x", 0.0);
            e.min_y = detail::number_or(c, "min_y", 0.0);
            e.max_x = detail::number_or(c, "max_x", 0.0);
            e.max_y = detail::number_or(c, "max_y", 0.0);
            e.px_per_uu = detail::number_or(c, "px_per_uu", 0.0);
            e.z_min = detail::number_or(c, "z_min", 0.0);
            e.z_max = detail::number_or(c, "z_max", 0.0);
            e.z_bits = static_cast<int>(detail::number_or(c, "z_bits", kZBits));
            e.z_code_max = static_cast<int>(detail::number_or(c, "z_code_max", kZCodeMax));
            if (e.z_code_max < 2 || e.z_code_max > 65535)
            {
                e.z_code_max = kZCodeMax;
            }
            e.chapter = static_cast<int>(
                detail::number_or(c, "chapter", static_cast<double>(chid::chapter_from_key(e.key))));
            if (e.chapter < chid::kDlc || e.chapter > chid::kMaxChapter)
            {
                e.chapter = chid::kNone;
            }

            // `height_planes` only: the /3 name `height_maps` is not accepted, so a /3
            // asset tree fails loudly instead of decoding at the wrong scale.
            const mjson::JValue* jh = c.find("height_planes");
            if (jh != nullptr && jh->kind == mjson::JValue::Kind::Array && jh->arr)
            {
                for (const mjson::JValue& item : *jh->arr)
                {
                    // Either a bare filename or {"image": "..."}.
                    std::string rel = item.string_or("");
                    if (rel.empty())
                    {
                        const mjson::JValue* img2 = item.find("image");
                        rel = img2 != nullptr ? img2->string_or("") : "";
                    }
                    if (!rel.empty() && e.height_maps.size() < static_cast<std::size_t>(kMaxSurfaces))
                    {
                        e.height_maps.push_back(std::move(rel));
                    }
                }
            }
            const int declared =
                static_cast<int>(detail::number_or(c, "max_surfaces", static_cast<double>(kMaxSurfaces)));
            if (e.height_maps.empty() && !e.image.empty())
            {
                // No plane list: the shipped naming is <chapter>/small_h<k>.png next to
                // the composite.
                const int want = declared > 0 && declared <= kMaxSurfaces ? declared : kMaxSurfaces;
                const std::string stem = detail::stem_of(e.image);
                for (int k = 0; k < want; ++k)
                {
                    e.height_maps.push_back(stem + "_h" + std::to_string(k) + ".png");
                }
                e.height_maps_guessed = true;
            }
            e.max_surfaces = static_cast<int>(e.height_maps.size());

            if (!e.geometry_ok())
            {
                problems.push_back("chapter \"" + e.key + "\" is incomplete - skipped");
                continue;
            }
            out.chapters.push_back(std::move(e));
        }
        return true;
    }
} // namespace mapmanifest
