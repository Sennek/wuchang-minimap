#pragma once

//
// mapmanifest - `maps/maps.json`, parsed. Pure: json.hpp and the standard library, no
// Windows / WIC / UE4SS.
//
// SCHEMA `wuchang-minimap-maps/6`, produced by tools/navmesh/build_map.py (and by
// tools/navmesh/repack_maps.py, which re-encodes an already-shipped tree):
//
//     { "schema": "wuchang-minimap-maps/6",
//       "chapters": {
//         "chapter1": { "chapter": 1, "image": "chapter1/small.png",
//                       "image_width": ..., "image_height": ...,
//                       "min_x": ..., "min_y": ..., "max_x": ..., "max_y": ...,
//                       "px_per_uu": ..., "z_min": ..., "z_max": ...,
//                       "z_bits": 12, "z_code_max": 4095,
//                       "max_surfaces": 8,
//                       "height_planes": ["chapter1/small_h0.png", ...],
//                       "coverage": { "tile_px": 32, "tiles_x": .., "tiles_y": ..,
//                                     "encoding": "u16le-lo-hi-base64",
//                                     "data": "<base64>" } },
//         "chapter2": { ... }, ... } }
//
// A HEIGHT CODE is one 16-bit sample of a height plane:
//
//     bits 0..11   the Z code, 1..4095 over [z_min, z_max]; 0 = no surface here
//     bit  12      REACHABLE - a marker-seeded walk-and-fall flood reached this surface
//     bits 13..15  zero
//
// THE COVERAGE INDEX (`coverage`, since /6) is a dense grid of 32-px tiles over the
// chapter's own pixel grid, two 16-bit Z codes a tile - the lowest and the highest code
// stored anywhere in that tile across every plane, 0/0 for a tile with no surface -
// base64, little-endian, row-major. `Entry::covers()` answers the one question the
// runtime cannot answer about a chapter it has not loaded: does THIS chapter's asset have
// ground at the player's feet? gamestate.cpp asks it of the chapter vote's contested
// candidates, because at a chapter boundary the vote can name the chapter whose asset
// stops short of the player.
//
// Three schemas are accepted. /6 is /5 plus the coverage index. /5 carries bit 12 but no
// index, and `covers()` then answers false everywhere, which leaves the vote alone. /4
// carries neither, and is read with `has_reachability` false, which makes every surface
// reachable. /3 is refused: its codes span the full 16 bits, so a /3 plane read here
// lands sixteen times off and looks like an empty map rather than a version error.
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
    inline constexpr const char* kSchema = "wuchang-minimap-maps/6";
    // Accepted too: the same geometry and the same 12-bit Z, without the coverage index.
    inline constexpr const char* kSchemaNoCoverage = "wuchang-minimap-maps/5";
    // And without bit 12 either.
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

    // The coverage index's tile size, in pixels of the chapter's own grid. Must match
    // mapfmt.py's COVERAGE_TILE. A file that states another value is read at the value it
    // states, so only the producer has to move.
    inline constexpr int kCoverageTilePx = 32;

    // `Entry::cover_score`'s probe pattern: the player plus two rings of eight spokes out
    // to 15 m, which is about what the minimap shows. Unit vectors at 45 degrees.
    inline constexpr double kCoverRadiusUu = 1500.0;
    inline constexpr int kCoverRings = 2;
    inline constexpr double kSqrtHalf = 0.70710678118654752;
    inline constexpr double kCoverSpokes[8][2] = {
        {1.0, 0.0},        {kSqrtHalf, kSqrtHalf},   {0.0, 1.0},        {-kSqrtHalf, kSqrtHalf},
        {-1.0, 0.0},       {-kSqrtHalf, -kSqrtHalf}, {0.0, -1.0},       {kSqrtHalf, -kSqrtHalf},
    };
    inline constexpr int kCoverProbes = 1 + kCoverRings * 8;

    //==================================================================================
    // The coverage index
    //==================================================================================

    // One chapter's tile grid: the lowest and the highest Z CODE stored in each tile,
    // 0/0 where the tile holds no surface. Codes, not world Z - the Entry owns the
    // quantisation that turns them into uu.
    struct Coverage
    {
        int tile_px = 0;
        int tiles_x = 0;
        int tiles_y = 0;
        std::vector<std::uint16_t> lo;
        std::vector<std::uint16_t> hi;

        bool ok() const
        {
            const std::size_t want =
                static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);
            return tile_px > 0 && tiles_x > 0 && tiles_y > 0 && lo.size() == want &&
                   hi.size() == want;
        }

        // False when the tile is outside the grid or holds no surface at all.
        bool tile_range(int tx, int ty, std::uint16_t& lo_code, std::uint16_t& hi_code) const
        {
            if (!ok() || tx < 0 || ty < 0 || tx >= tiles_x || ty >= tiles_y)
            {
                return false;
            }
            const std::size_t i =
                static_cast<std::size_t>(ty) * static_cast<std::size_t>(tiles_x) +
                static_cast<std::size_t>(tx);
            lo_code = lo[i];
            hi_code = hi[i];
            return hi_code != 0;
        }
    };

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
        // The planes carry bit 12 (schema /5 and up). False for a /4 asset, where every
        // surface counts as reachable.
        bool has_reachability = false;
        // The coverage index (schema /6). Empty for an older asset, and `covers()` then
        // answers false everywhere.
        Coverage coverage;

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

        // The chapter's own pixel for a world position, through the north-up mapping
        // `u = (Y - min_y) * px_per_uu`, `v = (max_x - X) * px_per_uu`. False outside the
        // picture.
        bool to_px(double wx, double wy, int& px, int& py) const
        {
            if (!geometry_ok())
            {
                return false;
            }
            const double upx = (wy - min_y) * px_per_uu;
            const double vpx = (max_x - wx) * px_per_uu;
            if (!(upx >= 0.0) || !(vpx >= 0.0))
            {
                return false;
            }
            px = static_cast<int>(upx);
            py = static_cast<int>(vpx);
            return px < image_width && py < image_height;
        }

        // Does this chapter's asset hold a surface within `tol` uu of `feet_z` at
        // (wx, wy)? A tile brackets every storey it touches, so this reads "there is
        // ground about here at about this height" rather than "the pixel under the player
        // is lit" - which is what a boundary tiebreak wants, and all a 32-px tile can say.
        //
        // False whenever the answer is not known: no index (a pre-/6 asset), a position
        // off the picture, or an empty tile.
        bool covers(double wx, double wy, double feet_z, double tol) const
        {
            int px = 0;
            int py = 0;
            std::uint16_t lo_code = 0;
            std::uint16_t hi_code = 0;
            if (!coverage.ok() || !heights_ok() || !to_px(wx, wy, px, py) ||
                !coverage.tile_range(px / coverage.tile_px, py / coverage.tile_px, lo_code,
                                     hi_code))
            {
                return false;
            }
            const double step = z_step_uu();
            const double z_lo = z_min + (static_cast<double>(lo_code) - 1.0) * step;
            const double z_hi = z_min + (static_cast<double>(hi_code) - 1.0) * step;
            return feet_z >= z_lo - tol && feet_z <= z_hi + tol;
        }

        // HOW MUCH ground this chapter's asset has AROUND the player, 0..100, as `covers()`
        // at the player and on two rings of eight out to `kCoverRadiusUu`.
        //
        // One pixel is a coin flip on a seam. Two chapters' assets overlap in world space
        // and each has holes where the other has ground, so the pixel under the player's
        // feet flips between them step by step while the place he is standing in is
        // plainly one chapter's - measured on the 2026-09-10 seam at X 40843 Y 25755, the
        // pixel said both chapters and the rings said 98 against 67. The map the player
        // wants is the one that describes the PLACE.
        int cover_score(double wx, double wy, double feet_z, double tol) const
        {
            int hits = covers(wx, wy, feet_z, tol) ? 1 : 0;
            for (int ring = 1; ring <= kCoverRings; ++ring)
            {
                const double r = kCoverRadiusUu * static_cast<double>(ring) / kCoverRings;
                for (const auto& s : kCoverSpokes)
                {
                    if (covers(wx + r * s[0], wy + r * s[1], feet_z, tol))
                    {
                        ++hits;
                    }
                }
            }
            return hits * 100 / kCoverProbes;
        }
    };

    struct Manifest
    {
        std::string schema;
        std::vector<Entry> chapters; // in the order the file lists them

        bool schema_ok() const
        {
            return schema == kSchema || schema == kSchemaNoCoverage || schema == kSchemaNoReach;
        }

        // The planes carry bit 12. Mirrored onto every Entry.
        bool has_reachability() const
        {
            return schema == kSchema || schema == kSchemaNoCoverage;
        }

        // The file carries a coverage index. False for /5 and /4, where `Entry::covers()`
        // answers false everywhere and the chapter vote stands on its own.
        bool has_coverage() const
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

        // -1 for anything that is not a base64 digit; '=' and whitespace are handled by
        // the caller.
        inline int b64_digit(char c)
        {
            if (c >= 'A' && c <= 'Z')
            {
                return c - 'A';
            }
            if (c >= 'a' && c <= 'z')
            {
                return c - 'a' + 26;
            }
            if (c >= '0' && c <= '9')
            {
                return c - '0' + 52;
            }
            if (c == '+')
            {
                return 62;
            }
            if (c == '/')
            {
                return 63;
            }
            return -1;
        }

        // Standard base64. Padding and whitespace are skipped; any other character makes
        // the whole string a failure, so a truncated or mangled blob is refused rather
        // than half-decoded.
        inline bool b64_decode(std::string_view text, std::vector<std::uint8_t>& out)
        {
            out.clear();
            out.reserve(text.size() / 4 * 3);
            std::uint32_t acc = 0;
            int bits = 0;
            for (const char c : text)
            {
                if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                {
                    continue;
                }
                const int d = b64_digit(c);
                if (d < 0)
                {
                    out.clear();
                    return false;
                }
                acc = (acc << 6) | static_cast<std::uint32_t>(d);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
                }
            }
            return true;
        }

        // The `coverage` object, or a left-empty Coverage. `problem` is set only when the
        // object is there and unusable, so a pre-/6 chapter is silent.
        inline void parse_coverage(const mjson::JValue& c, Coverage& out, std::string& problem)
        {
            out = Coverage{};
            const mjson::JValue* jc = c.find("coverage");
            if (jc == nullptr || jc->kind != mjson::JValue::Kind::Object)
            {
                return;
            }
            Coverage cov{};
            cov.tile_px = static_cast<int>(number_or(*jc, "tile_px", kCoverageTilePx));
            cov.tiles_x = static_cast<int>(number_or(*jc, "tiles_x", 0.0));
            cov.tiles_y = static_cast<int>(number_or(*jc, "tiles_y", 0.0));
            const mjson::JValue* jd = jc->find("data");
            const std::string data = jd != nullptr ? jd->string_or("") : "";
            if (cov.tile_px <= 0 || cov.tiles_x <= 0 || cov.tiles_y <= 0 || data.empty())
            {
                problem = "its \"coverage\" object is incomplete";
                return;
            }
            const std::size_t tiles =
                static_cast<std::size_t>(cov.tiles_x) * static_cast<std::size_t>(cov.tiles_y);
            std::vector<std::uint8_t> raw;
            if (!b64_decode(data, raw) || raw.size() != tiles * 4u)
            {
                problem = "its \"coverage\" blob is not " + std::to_string(tiles * 4u) +
                          " base64 byte(s)";
                return;
            }
            cov.lo.resize(tiles);
            cov.hi.resize(tiles);
            for (std::size_t i = 0; i < tiles; ++i)
            {
                cov.lo[i] = static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[i * 4 + 0]) |
                                                       (static_cast<std::uint16_t>(raw[i * 4 + 1])
                                                        << 8));
                cov.hi[i] = static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[i * 4 + 2]) |
                                                       (static_cast<std::uint16_t>(raw[i * 4 + 3])
                                                        << 8));
            }
            out = std::move(cov);
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
                               "\" but this build reads \"" + std::string(kSchema) + "\", \"" +
                               std::string(kSchemaNoCoverage) + "\" or \"" +
                               std::string(kSchemaNoReach) +
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

            // A broken index costs the chapter its tiebreak vote, never the chapter
            // itself: the map still draws.
            std::string cov_problem;
            detail::parse_coverage(c, e.coverage, cov_problem);
            if (!cov_problem.empty())
            {
                problems.push_back("chapter \"" + e.key + "\": " + cov_problem +
                                   " - the coverage tiebreak is off for it");
            }

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
