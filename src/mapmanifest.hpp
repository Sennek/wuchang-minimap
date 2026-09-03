#pragma once

//
// mapmanifest - `maps/maps.json`, parsed. Pure: json.hpp and the standard library.
//
// Split out of mapdata.cpp for the same reason markers_db.cpp is split out of
// markers.cpp (lessons.md): the manifest is a file a user can hand-edit into nonsense,
// the parse has no dependency on Windows / WIC / UE4SS, and an in-game session is the
// scarce resource here. tests/markers_test.cpp runs it on the build machine.
//
// SCHEMA `wuchang-minimap-maps/4`, produced by tools/navmesh/build_map.py (and by
// tools/navmesh/repack_maps.py, which re-encodes an already-shipped tree):
//
//     { "schema": "wuchang-minimap-maps/4",
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
// THE VERSION IS ENFORCED IN BOTH DIRECTIONS, and that is why /4 renamed two things
// it did not otherwise have to. The height codes went from 16-bit (1..65535) to
// 12-bit (1..4095) to save a third of the PNG bytes, and the two encodings differ
// only in how one number is SCALED - so a /3 plane read by a /4 decoder puts every
// surface sixteen times too low, and a /4 plane read by a /3 decoder puts it sixteen
// times too high. Neither looks like a version error in-game; both look like a map
// that is simply empty. So:
//
//   * a /4 build reading anything but "wuchang-minimap-maps/4" REFUSES the file
//     (mapmanifest::parse returns false and says both strings), rather than the old
//     behaviour of logging a warning and reading it anyway;
//   * a /3 build reading a /4 file cannot be changed retroactively - 1.0.0 is out -
//     so /4 moved the plane list from `height_maps` to `height_planes` and the files
//     from `<stem>_z<k>.png` to `<stem>_h<k>.png`. The /3 parser then finds no plane
//     list, falls back to guessing the `_z` names, finds nothing on disk and logs
//     "NO height plane decoded ... build them with build_map.py". Loud, accurate, and
//     it costs nothing but two names.
//
// Fields worth naming:
//
//   * `chapter` - the chapter NUMBER. The runtime detects a number from the streamed
//     `B<N>EX0_...` cell packages (chapterid.hpp), so the manifest states it rather
//     than making the spelling of the key load-bearing. Absent -> derived from the key.
//   * `z_bits` / `z_code_max` - the height quantisation. `z_code_max` is what the
//     runtime divides by, so a future 14-bit asset needs no code change at all.
//   * `max_surfaces_requested` - what was asked for before empty trailing planes were
//     dropped. Informational.
//   * `height_tiles_128` / `height_tile_ram_bytes` - what the sparse tile store in
//     mapdata.cpp will cost, measured by the pipeline. Informational; the runtime
//     logs its own count and the test compares the two.
//
// Per-chapter `px_per_uu` was always in the schema and is now actually used: each
// chapter is scaled to its own RAM budget, so the five differ (0.032 to 0.060).
//

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "chapterid.hpp"
#include "json.hpp"

namespace mapmanifest
{
    // Must match tools/navmesh/mapfmt.py's SCHEMA. Compared for EXACT equality: see
    // the version discussion above.
    inline constexpr const char* kSchema = "wuchang-minimap-maps/4";

    // The height quantisation this build was written for. `z_code_max` is read from
    // the manifest (so a wider asset needs no code change); these are the fallbacks
    // for a /4 file that omits it.
    inline constexpr int kZBits = 12;
    inline constexpr int kZCodeMax = (1 << kZBits) - 1; // 4095; 0 = "no surface"

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
        // The height quantisation, from the manifest: `code = 1 + round(t *
        // (z_code_max - 1))`, 0 = no surface. Read rather than assumed so the asset
        // can widen without a code change.
        int z_bits = kZBits;
        int z_code_max = kZCodeMax;
        int max_surfaces = 0;
        std::vector<std::string> height_maps; // lowest surface first
        // True when `height_planes` was not in the file and the names had to be
        // guessed from the composite's. Kept so the caller can say so in the log.
        bool height_maps_guessed = false;

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
        if (!out.schema_ok())
        {
            // FATAL, not a warning. The /3 and /4 height encodings differ by a factor
            // of sixteen in one scale, and reading the wrong one produces a map that
            // looks empty rather than an error - so the only safe answer is to draw
            // nothing and say exactly what to do about it.
            problems.push_back("maps.json is schema \"" +
                               (out.schema.empty() ? std::string("(none)") : out.schema) +
                               "\" but this build reads \"" + std::string(kSchema) +
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

            // `height_planes` since /4 (see the version discussion at the top); the
            // /3 name is deliberately NOT accepted, because that is what makes a /3
            // asset tree fail loudly instead of decoding at the wrong scale.
            const mjson::JValue* jh = c.find("height_planes");
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
                // Fallback for a manifest that states `max_surfaces` but no plane
                // list: the shipped naming is <chapter>/small_h<k>.png next to the
                // composite.
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
