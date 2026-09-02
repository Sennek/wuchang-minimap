#pragma once

//
// mapmanifest - `maps/maps.json`, parsed. Pure: json.hpp and the standard library.
//
// Split out of mapdata.cpp for the same reason markers_db.cpp is split out of
// markers.cpp (lessons.md): the manifest is a file a user can hand-edit into nonsense,
// the parse has no dependency on Windows / WIC / UE4SS, and an in-game session is the
// scarce resource here. tests/markers_test.cpp runs it on the build machine.
//
// SCHEMA `wuchang-minimap-maps/3`, produced by tools/navmesh/build_map.py:
//
//     { "schema": "wuchang-minimap-maps/3",
//       "chapters": {
//         "chapter1": { "chapter": 1, "image": "chapter1/small.png",
//                       "image_width": ..., "image_height": ...,
//                       "min_x": ..., "min_y": ..., "max_x": ..., "max_y": ...,
//                       "px_per_uu": ..., "z_min": ..., "z_max": ...,
//                       "max_surfaces": 8,
//                       "height_maps": ["chapter1/small_z0.png", ...] },
//         "chapter2": { ... }, ... } }
//
// The SHAPE did not change when the file went from one chapter to five - "chapters"
// was always an object keyed by chapter - so the schema string stays at /3 and a mod
// built against the one-chapter file reads the five-chapter one unchanged. Two fields
// are new and both are optional:
//
//   * `chapter` - the chapter NUMBER. The runtime detects a number from the streamed
//     `B<N>EX0_...` cell packages (chapterid.hpp), so the manifest states it rather
//     than making the spelling of the key load-bearing. Absent -> derived from the key.
//   * `max_surfaces_requested` - what was asked for before empty trailing planes were
//     dropped. Informational.
//
// Per-chapter `px_per_uu` was always in the schema and is now actually used: each
// chapter is scaled to its own RAM budget, so the five differ (0.031 to 0.060).
//

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "chapterid.hpp"
#include "json.hpp"

namespace mapmanifest
{
    // Must match tools/navmesh/build_map.py's SCHEMA.
    inline constexpr const char* kSchema = "wuchang-minimap-maps/3";

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
        int max_surfaces = 0;
        std::vector<std::string> height_maps; // lowest surface first
        // True when `height_maps` was not in the file and the names had to be guessed
        // from the composite's. Kept so the caller can say so in the log.
        bool height_maps_guessed = false;

        bool geometry_ok() const
        {
            return !image.empty() && image_width > 0 && image_height > 0 && px_per_uu > 0.0 &&
                   max_x > min_x && max_y > min_y;
        }

        bool heights_ok() const
        {
            return !height_maps.empty() && z_max > z_min;
        }
    };

    struct Manifest
    {
        std::string schema;
        std::vector<Entry> chapters; // in the order the file lists them

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

        // The chapter to have resident before anything is known about where the player
        // is: the LOWEST chapter number present. That keeps the pre-detection behaviour
        // identical to the single-chapter build (chapter 1 resident at start-up, so the
        // main-menu slicer self-test still has an asset to exercise).
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

    // Parses `text`. `problems` collects one human-readable line per chapter that was
    // skipped, so the caller can log exactly what the user has to fix. Returns false
    // only when the document itself is unusable (bad JSON, no "chapters" object);
    // a file whose every chapter is broken parses fine and yields an empty list.
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
            e.chapter = static_cast<int>(
                detail::number_or(c, "chapter", static_cast<double>(chid::chapter_from_key(e.key))));
            if (e.chapter < chid::kDlc || e.chapter > chid::kMaxChapter)
            {
                e.chapter = chid::kNone;
            }

            const mjson::JValue* jh = c.find("height_maps");
            if (jh != nullptr && jh->kind == mjson::JValue::Kind::Array && jh->arr)
            {
                for (const mjson::JValue& item : *jh->arr)
                {
                    // Either a bare filename or {"image": "..."}; accept both so a
                    // manifest tweak cannot silently produce a mapless overlay.
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
                // Fallback for a manifest written before the key existed: the shipped
                // naming is <chapter>/small_z<k>.png next to the composite.
                const int want = declared > 0 && declared <= kMaxSurfaces ? declared : kMaxSurfaces;
                const std::string stem = detail::stem_of(e.image);
                for (int k = 0; k < want; ++k)
                {
                    e.height_maps.push_back(stem + "_z" + std::to_string(k) + ".png");
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
