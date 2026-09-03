//
// markers_test - the offline half of the marker feature's verification.
//
// The mod itself can only be exercised by launching the game, which costs a play
// session; everything that does NOT need the engine is tested here instead, in a
// console exe that links exactly two translation units (src/markers_db.cpp and this
// file) and touches neither UE4SS nor Direct3D:
//
//   * the markers/<chapter>.json loader - the happy path against the shipped
//     hand-written sample, plus every way the file can be wrong;
//   * the category name <-> bitmask mapping the config file and the F2 filter
//     checkboxes share;
//   * the wuchang_minimap_found.txt round-trip;
//   * the stable-id helpers that join the offline DB to the live actors;
//   * the full map's viewport transform and its exact inverse, the zoom clamp, and the
//     waypoint file round-trip (src/mapview.cpp - same "pure half" idea).
//   * the chapter detection's string logic and its tiered vote (src/chapterid.hpp),
//     and the maps/maps.json parse (src/mapmanifest.hpp) - including the shipped
//     five-chapter file and the one-chapter file the same schema used to ship;
//   * the chunked GUObjectArray scan scheduler (src/scan_sched.hpp): slice
//     planning, round wrap, the clamps and the rate gates. That walk replaced the
//     per-class FindAllOf sweep that cost 28.30 ms mean / 51.05 ms peak per
//     game-thread pump in-game, and its arithmetic is the one part of it that can
//     be proven without the engine.
//
// Run it with the repo's `markers` directory as argv[1] (build.ps1 does) to include
// the real sample file in the run; without it the embedded fixtures still cover
// everything else. Exit code 0 = all green.
//

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "chapterid.hpp"
#include "config_keys.hpp"
#include "config_rewrite.hpp"
#include "compass.hpp"
#include "glyphs.hpp"
#include "mapmanifest.hpp"
#include "mapview.hpp"
#include "markers_db.hpp"
#include "perf.hpp"
#include "projection.hpp"
#include "scan_sched.hpp"

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void check(bool ok, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("  FAIL  %s   (%s:%d)\n", what, file, line);
        }
    }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

    void check_eq_int(long long got, long long want, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (got != want)
        {
            ++g_failures;
            std::printf("  FAIL  %s: got %lld, want %lld   (%s:%d)\n", what, got, want, file, line);
        }
    }

#define CHECK_EQ(got, want) check_eq_int(static_cast<long long>(got), static_cast<long long>(want), #got, __FILE__, __LINE__)

    void check_eq_str(const std::string& got, const std::string& want, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (got != want)
        {
            ++g_failures;
            std::printf("  FAIL  %s: got \"%s\", want \"%s\"   (%s:%d)\n", what, got.c_str(), want.c_str(), file,
                        line);
        }
    }

#define CHECK_STR(got, want) check_eq_str((got), (want), #got, __FILE__, __LINE__)

    void check_near(double got, double want, double tol, const char* what, const char* file, int line)
    {
        ++g_checks;
        const double d = got - want;
        if (!(d >= -tol && d <= tol))
        {
            ++g_failures;
            std::printf("  FAIL  %s: got %.6f, want %.6f (+-%g)   (%s:%d)\n", what, got, want, tol, file, line);
        }
    }

#define CHECK_NEAR(got, want, tol) check_near((got), (want), (tol), #got, __FILE__, __LINE__)

    void section(const char* name)
    {
        std::printf("%s\n", name);
    }

    bool read_file(const std::string& path, std::string& out)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
        {
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0)
        {
            std::fclose(f);
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        return read == out.size();
    }

    //==================================================================================
    // Fixtures
    //==================================================================================

    // A minimal but complete manifest: one of every id shape (a shrine's game id and a
    // <level>/<object> id), an unknown category, and an entry that must be skipped.
    const char* const kGoodJson = R"JSON(
{
  "schema": "wuchang-minimap-markers/1",
  "chapter": 1,
  "markers": [
    {"id":"digong01","cat":"shrine","cls":"BP_RebornFire_C","name":"Digong shrine",
     "x":18214.0,"y":5505.0,"z":-1579.0,"cell":"B1EX0_L0_X1_Y0","level":"Chapter1_DGong_logic"},
    {"id":"Chapter1_DGong_logic/BP_ItemRedBox_C_0","cat":"chest","cls":"BP_ItemRedBox_C",
     "x":18852,"y":7322,"z":-1079,"level":"Chapter1_DGong_logic"},
    {"id":"Chapter1_Shuwang_logic/BP_PickupActor_C_36","cat":"pickup","x":17489,"y":5523,"z":946},
    {"id":"Chapter1_X/thing","cat":"totally_made_up","x":1,"y":2,"z":3},
    {"id":"","cat":"chest","x":1,"y":2,"z":3},
    {"id":"no_coords","cat":"chest"},
    {"id":"Chapter1_X/second_chapter","cat":"other","chapter":4,"x":9,"y":8,"z":7}
  ]
}
)JSON";

    void test_loader()
    {
        section("markers/<chapter>.json loader");

        std::vector<mdb::StaticMarker> db;
        mdb::ParseReport rep{};
        CHECK(mdb::parse_markers_json(kGoodJson, db, rep));
        CHECK_STR(rep.error, "");
        CHECK_STR(rep.schema, "wuchang-minimap-markers/1");
        CHECK_EQ(rep.chapter, 1);
        CHECK_STR(rep.chapter_label, "1");
        CHECK_EQ(rep.added, 5);       // two entries are rejected
        CHECK_EQ(rep.skipped, 2);     // empty id, and the one with no coordinates
        CHECK_EQ(rep.unknown_cat, 1); // "totally_made_up"
        CHECK_EQ(db.size(), 5);

        CHECK_STR(db[0].id, "digong01");
        CHECK(db[0].cat == mdb::Cat::Shrine);
        CHECK(db[0].x == 18214.0 && db[0].y == 5505.0 && db[0].z == -1579.0);
        CHECK_EQ(db[0].chapter, 1);
        CHECK(db[1].cat == mdb::Cat::Chest);
        CHECK(db[2].cat == mdb::Cat::Pickup);
        // An unknown category must degrade to `other`, never drop the marker: a marker
        // we cannot classify is still a marker on the map.
        CHECK(db[3].cat == mdb::Cat::Other);
        // A per-marker chapter overrides the file's.
        CHECK_EQ(db[4].chapter, 4);

        // Appending a second file must not clear the first.
        mdb::ParseReport rep2{};
        CHECK(mdb::parse_markers_json(kGoodJson, db, rep2));
        CHECK_EQ(db.size(), 10);

        // --- every way the file can be wrong ---------------------------------------
        const struct
        {
            const char* text;
            const char* why;
        } bad[] = {
            {"", "empty file"},
            {"not json at all", "garbage"},
            {"[1,2,3]", "top level is an array"},
            {R"({"markers":[]})", "no schema"},
            {R"({"schema":"wuchang-minimap-markers/2","markers":[]})", "wrong major version"},
            {R"({"schema":"wuchang-minimap-markers/1"})", "no markers array"},
            {R"({"schema":"wuchang-minimap-markers/1","markers":{}})", "markers is not an array"},
        };
        for (const auto& b : bad)
        {
            std::vector<mdb::StaticMarker> out;
            mdb::ParseReport r{};
            const bool ok = mdb::parse_markers_json(b.text, out, r);
            if (ok || r.error.empty() || !out.empty())
            {
                ++g_failures;
                std::printf("  FAIL  bad manifest accepted (%s)\n", b.why);
            }
            ++g_checks;
        }

        // An empty but valid manifest is a success with zero markers, not an error.
        {
            std::vector<mdb::StaticMarker> out;
            mdb::ParseReport r{};
            CHECK(mdb::parse_markers_json(R"({"schema":"wuchang-minimap-markers/1","chapter":2,"markers":[]})",
                                          out, r));
            CHECK_EQ(out.size(), 0);
            CHECK_EQ(r.chapter, 2);
        }

        // The DLC manifest spells its chapter "DLC", not a number. That must load, with
        // chapter 0 as the numeric bucket and the label carried through - not be
        // rejected, and not silently claim to be chapter 1.
        {
            std::vector<mdb::StaticMarker> out;
            mdb::ParseReport r{};
            CHECK(mdb::parse_markers_json(R"({"schema":"wuchang-minimap-markers/1","chapter":"DLC",)"
                                          R"("markers":[{"id":"L/a","cat":"chest","x":1,"y":2,"z":3}]})",
                                          out, r));
            CHECK_EQ(out.size(), 1);
            CHECK_EQ(r.chapter, 0);
            CHECK_STR(r.chapter_label, "DLC");
            CHECK_EQ(out[0].chapter, 0);
        }

        // A UTF-8 BOM (which is what PowerShell's Set-Content -Encoding utf8 writes)
        // must not make the first token unparseable.
        {
            std::string bom = "\xEF\xBB\xBF";
            bom += kGoodJson;
            std::vector<mdb::StaticMarker> out;
            mdb::ParseReport r{};
            CHECK(mdb::parse_markers_json(bom, out, r));
            CHECK_EQ(out.size(), 5);
        }
    }

    void test_sample_file(const std::string& markers_dir)
    {
        section("the shipped sample manifest");
        const std::string path = markers_dir + "/chapter1.sample.json";
        std::string text;
        if (!read_file(path, text))
        {
            std::printf("  SKIP  %s not readable (pass the repo's markers dir as argv[1])\n", path.c_str());
            return;
        }
        std::vector<mdb::StaticMarker> db;
        mdb::ParseReport rep{};
        CHECK(mdb::parse_markers_json(text, db, rep));
        CHECK_STR(rep.error, "");
        CHECK_EQ(rep.chapter, 1);
        CHECK_EQ(rep.skipped, 0);
        CHECK_EQ(rep.unknown_cat, 0);
        CHECK(rep.added >= 5);

        int shrines = 0;
        for (const mdb::StaticMarker& m : db)
        {
            CHECK(!m.id.empty());
            CHECK_EQ(m.chapter, 1);
            shrines += (m.cat == mdb::Cat::Shrine) ? 1 : 0;
        }
        CHECK(shrines >= 1);
        std::printf("  %zu marker(s) parsed from %s\n", db.size(), path.c_str());
    }

    // The REAL database, once tools/markers has produced it. Skipped when it is not
    // there yet, so this file is useful before and after that lands - and once it does,
    // a regression in the extractor's output shows up here instead of in a play session.
    void test_real_db(const std::string& markers_dir)
    {
        section("the generated chapter database");
        const std::string path = markers_dir + "/chapter1.json";
        std::string text;
        if (!read_file(path, text))
        {
            std::printf("  SKIP  %s does not exist yet (tools/markers has not run)\n", path.c_str());
            return;
        }
        std::vector<mdb::StaticMarker> db;
        mdb::ParseReport rep{};
        CHECK(mdb::parse_markers_json(text, db, rep));
        CHECK_STR(rep.error, "");
        CHECK_STR(rep.schema, "wuchang-minimap-markers/1");
        CHECK_EQ(rep.chapter, 1);
        CHECK_STR(rep.chapter_label, "1");
        CHECK_EQ(rep.skipped, 0);
        // An unknown category name means the extractor and the runtime have drifted
        // apart - those markers would still draw, but as anonymous grey dots.
        CHECK_EQ(rep.unknown_cat, 0);
        CHECK(rep.added > 100);

        int per_cat[mdb::kCatCount]{};
        std::vector<std::string> ids;
        for (const mdb::StaticMarker& m : db)
        {
            CHECK(!m.id.empty());
            per_cat[static_cast<int>(m.cat)] += 1;
            ids.push_back(m.id);
            // A shrine's id must be the game-authored one (`digong01`), not
            // <level>/<object>: it is the only thing the live BP_RebornFire_C sweep can
            // join on. Everything else must be <level>/<object>, which is what the live
            // sweep builds from GetFullName().
            if (m.cat == mdb::Cat::Shrine)
            {
                CHECK(m.id.find('/') == std::string::npos);
            }
            else
            {
                CHECK(m.id.find('/') != std::string::npos);
            }
        }
        // Ids are the join key with the live actors, so a duplicate is a real defect:
        // the second one could never be reached.
        std::sort(ids.begin(), ids.end());
        CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

        std::printf("  %zu marker(s) parsed from %s\n", db.size(), path.c_str());
        for (int i = 0; i < mdb::kCatCount; ++i)
        {
            if (per_cat[i] != 0)
            {
                std::printf("    %-10s %d\n", mdb::cat_name(static_cast<mdb::Cat>(i)), per_cat[i]);
            }
        }

        // The rest of the generated set, if it is there. The runtime enumerates the
        // markers directory rather than probing chapter1..8, so every one of these is
        // loaded in-game and every one of them has to parse - including the DLC
        // manifest, whose "chapter" is the string "DLC" rather than a number.
        static const char* const kRest[] = {"chapter2", "chapter3", "chapter4", "chapter5",
                                            "chapter6", "chapter7", "chapter8", "chapterdlc"};
        for (const char* name : kRest)
        {
            const std::string other = markers_dir + "/" + name + ".json";
            std::string other_text;
            if (!read_file(other, other_text))
            {
                continue;
            }
            std::vector<mdb::StaticMarker> other_db;
            mdb::ParseReport other_rep{};
            CHECK(mdb::parse_markers_json(other_text, other_db, other_rep));
            CHECK_STR(other_rep.error, "");
            CHECK_EQ(other_rep.skipped, 0);
            CHECK_EQ(other_rep.unknown_cat, 0);
            CHECK(!other_rep.chapter_label.empty());
            for (const mdb::StaticMarker& m : other_db)
            {
                CHECK(!m.id.empty());
            }
            std::printf("  %s: %zu marker(s), chapter \"%s\"\n", name, other_db.size(),
                        other_rep.chapter_label.c_str());
        }
    }

    //==================================================================================
    // Glyph shapes and marker palettes (src/glyphs.hpp)
    //==================================================================================
    //
    // The v0.9.1 review's finding was that boss / elite / enemy were the same triangle
    // at three scales and that hidden was a shrine with the fill switched off. The fix
    // is a table, and the property that makes the table right - no two categories share
    // a shape AND a colour - is exactly the kind of thing a build machine can prove
    // while the game is closed.
    void test_glyphs()
    {
        section("glyph shapes and marker palettes");

        // Every category has a shape, and in this mod every category has its OWN shape:
        // shape is the half of a marker's identity that survives being dimmed, drawn at
        // 6 px, or recoloured by a colour-blind palette.
        int seen[gly::kShapeCount] = {};
        for (int i = 0; i < mdb::kCatCount; ++i)
        {
            const gly::Shape s = gly::shape_of(static_cast<mdb::Cat>(i));
            CHECK(static_cast<int>(s) >= 0 && static_cast<int>(s) < gly::kShapeCount);
            CHECK(gly::shape_name(s)[0] != '\0');
            ++seen[static_cast<int>(s)];
        }
        for (int i = 0; i < gly::kShapeCount; ++i)
        {
            CHECK(seen[i] == 1); // every shape used exactly once
        }

        // An out-of-range category byte (a corrupt DrawMarker, a future category from a
        // newer marker file) must never index off the end of the table.
        CHECK(gly::shape_of(static_cast<mdb::Cat>(mdb::kCatCount)) == gly::Shape::SmallSquare);
        CHECK(gly::shape_of(static_cast<mdb::Cat>(200)) == gly::Shape::SmallSquare);

        const gly::Palette palettes[] = {gly::Palette::Default, gly::Palette::Colorblind};
        for (const gly::Palette pal : palettes)
        {
            // THE PROPERTY. Two categories may share a hue (the ladder and the lift do)
            // as long as their shapes differ, and may share a shape as long as their
            // hues differ - but never both.
            CHECK(gly::palette_is_separable(pal));

            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Rgb c = gly::marker_rgb(static_cast<mdb::Cat>(i), pal);
                // Nothing may be drawn in near-black: the minimap's backdrop is
                // (6, 9, 13) and the full map's canvas is darker still.
                CHECK(static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b) > 150);
            }
            CHECK(gly::marker_rgb(static_cast<mdb::Cat>(200), pal) ==
                  gly::marker_rgb(mdb::Cat::Other, pal));
        }

        // The two categories that deliberately share a hue, and the two that used to be
        // indistinguishable and no longer are.
        CHECK(gly::marker_rgb(mdb::Cat::Ladder, gly::Palette::Default) ==
              gly::marker_rgb(mdb::Cat::Lift, gly::Palette::Default));
        CHECK(gly::shape_of(mdb::Cat::Ladder) != gly::shape_of(mdb::Cat::Lift));
        CHECK(gly::shape_of(mdb::Cat::Boss) != gly::shape_of(mdb::Cat::Elite));
        CHECK(gly::shape_of(mdb::Cat::Elite) != gly::shape_of(mdb::Cat::Enemy));
        CHECK(gly::shape_of(mdb::Cat::Hidden) != gly::shape_of(mdb::Cat::Shrine));

        // ---- palette and theme names -------------------------------------------------
        //
        // Both values come out of a hand-edited text file, so the parsers have to be
        // forgiving about case, spaces and the British spelling, and must leave the
        // caller's value alone when they do not recognise one (the loader then logs and
        // keeps whatever was in force).
        gly::Palette pal = gly::Palette::Default;
        CHECK(gly::palette_from_name("colorblind", pal) && pal == gly::Palette::Colorblind);
        CHECK(gly::palette_from_name("  Colour-Blind ", pal) && pal == gly::Palette::Colorblind);
        CHECK(gly::palette_from_name("DEFAULT", pal) && pal == gly::Palette::Default);
        pal = gly::Palette::Colorblind;
        CHECK(!gly::palette_from_name("nonsense", pal));
        CHECK(pal == gly::Palette::Colorblind); // untouched on a bad value
        CHECK(!gly::palette_from_name("", pal));
        CHECK(std::strcmp(gly::palette_name(gly::Palette::Colorblind), "colorblind") == 0);
        CHECK(std::strcmp(gly::palette_name(gly::Palette::Default), "default") == 0);

        gly::Theme th = gly::Theme::Neutral;
        CHECK(gly::theme_from_name("ink", th) && th == gly::Theme::Ink);
        CHECK(gly::theme_from_name(" Neutral ", th) && th == gly::Theme::Neutral);
        th = gly::Theme::Ink;
        CHECK(!gly::theme_from_name("bronze", th));
        CHECK(th == gly::Theme::Ink);
        CHECK(std::strcmp(gly::theme_name(gly::Theme::Ink), "ink") == 0);
        // Both names round-trip, which is what save_config_file() writes out.
        for (const gly::Theme t : {gly::Theme::Neutral, gly::Theme::Ink})
        {
            gly::Theme back = gly::Theme::Neutral;
            CHECK(gly::theme_from_name(gly::theme_name(t), back) && back == t);
        }
        for (const gly::Palette p2 : {gly::Palette::Default, gly::Palette::Colorblind})
        {
            gly::Palette back = gly::Palette::Default;
            CHECK(gly::palette_from_name(gly::palette_name(p2), back) && back == p2);
        }

        // ---- themes -------------------------------------------------------------------
        //
        // `neutral` must be EXACTLY what 0.9.2 shipped, or every existing config file
        // silently changes appearance on upgrade.
        const gly::ThemeColors neutral = gly::theme_colors(gly::Theme::Neutral);
        CHECK(neutral.frame == (mdb::Rgb{168, 176, 186}));
        CHECK(neutral.frame_alpha == 0.85f);
        CHECK(neutral.backdrop == (mdb::Rgb{6, 9, 13}));
        CHECK(neutral.backdrop_alpha == 0.86f);
        CHECK(neutral.floor_base == (mdb::Rgb{214, 208, 196}));
        const gly::ThemeColors ink = gly::theme_colors(gly::Theme::Ink);
        CHECK(!(ink.frame == neutral.frame));
        CHECK(!(ink.backdrop == neutral.backdrop));
        CHECK(!(ink.plate == neutral.plate));
        // A plate is a DARK box behind light text, and a frame is a light ring on it.
        for (const gly::ThemeColors& t2 : {neutral, ink})
        {
            const int plate_sum = static_cast<int>(t2.plate.r) + t2.plate.g + t2.plate.b;
            const int frame_sum = static_cast<int>(t2.frame.r) + t2.frame.g + t2.frame.b;
            CHECK(plate_sum < 120);
            CHECK(frame_sum > 300);
            CHECK(t2.frame_alpha > 0.0f && t2.frame_alpha <= 1.0f);
            CHECK(t2.backdrop_alpha > 0.0f && t2.backdrop_alpha <= 1.0f);
        }

        // ---- the item-quality tiers ---------------------------------------------------
        //
        // The default set is the GAME's own pickup-beam palette and must not be
        // rewritten here; the colour-blind set has to be three genuinely different
        // colours, which the pastel default barely is.
        CHECK(gly::rarity_colors(gly::Palette::Default) == mdb::kDefaultRarityColors);
        const mdb::Rgb* cb = gly::rarity_colors(gly::Palette::Colorblind);
        for (int i = 0; i < mdb::kRarityCount; ++i)
        {
            for (int j = i + 1; j < mdb::kRarityCount; ++j)
            {
                const int d = std::abs(static_cast<int>(cb[i].r) - static_cast<int>(cb[j].r)) +
                              std::abs(static_cast<int>(cb[i].g) - static_cast<int>(cb[j].g)) +
                              std::abs(static_cast<int>(cb[i].b) - static_cast<int>(cb[j].b));
                CHECK(d > 150);
            }
        }
    }

    void test_categories()
    {
        section("category names and the filter mask");

        // Every enumerator round-trips through its wire name, and the names are unique.
        for (int i = 0; i < mdb::kCatCount; ++i)
        {
            const mdb::Cat cat = static_cast<mdb::Cat>(i);
            mdb::Cat back = mdb::Cat::Other;
            CHECK(mdb::cat_from_name(mdb::cat_name(cat), back));
            CHECK(back == cat);
            CHECK(mdb::cat_label(cat)[0] != '\0');
            for (int j = i + 1; j < mdb::kCatCount; ++j)
            {
                CHECK(std::strcmp(mdb::cat_name(cat), mdb::cat_name(static_cast<mdb::Cat>(j))) != 0);
            }
        }

        mdb::Cat unused = mdb::Cat::Other;
        CHECK(!mdb::cat_from_name("nonsense", unused));
        CHECK(!mdb::cat_from_name("", unused));
        // Case and surrounding whitespace must not matter: the value comes out of a
        // hand-edited text file.
        CHECK(mdb::cat_from_name("  SHRINE ", unused) && unused == mdb::Cat::Shrine);

        CHECK_EQ(mdb::parse_category_mask("all", 0u), mdb::kAllCats);
        CHECK_EQ(mdb::parse_category_mask("none", mdb::kAllCats), 0u);
        CHECK_EQ(mdb::parse_category_mask("shrine,chest", 0u),
                 mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest));
        // Spaces and semicolons are separators too.
        CHECK_EQ(mdb::parse_category_mask("shrine chest;pickup", 0u),
                 mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup));

        // Unknown names are reported and ignored; they never change the result.
        std::string rejected;
        CHECK_EQ(mdb::parse_category_mask("shrine,wombat,chest", 0u, &rejected),
                 mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest));
        CHECK_STR(rejected, "wombat");

        // A value that names nothing usable keeps the previous setting rather than
        // silently turning every marker off.
        CHECK_EQ(mdb::parse_category_mask("", mdb::kAllCats), mdb::kAllCats);
        CHECK_EQ(mdb::parse_category_mask("wombat,badger", mdb::kAllCats, &rejected), mdb::kAllCats);
        CHECK_STR(rejected, "wombat,badger");

        CHECK_STR(mdb::format_category_mask(mdb::kAllCats), "all");
        CHECK_STR(mdb::format_category_mask(0u), "none");
        CHECK_STR(mdb::format_category_mask(mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest)),
                  "shrine,chest");

        // format -> parse -> format is the config file's save/load path; it must be
        // exact for every mask, including the shipped default.
        for (std::uint32_t mask = 0; mask <= mdb::kAllCats; mask += 37u)
        {
            const std::string text = mdb::format_category_mask(mask);
            CHECK_EQ(mdb::parse_category_mask(text, ~mask & mdb::kAllCats), mask);
        }
        const std::uint32_t shipped = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        CHECK_EQ(mdb::parse_category_mask(mdb::format_category_mask(shipped), 0u), shipped);
    }

    void test_found_file()
    {
        section("wuchang_minimap_found.txt round-trip");

        const char* const text = "; a comment\n"
                                 "digong01\n"
                                 "\n"
                                 "  Chapter1_DGong_logic/BP_ItemRedBox_C_0  \n"
                                 "# another comment\n"
                                 "digong01\n" // duplicate
                                 "Chapter1_Shuwang_logic/BP_PickupActor_C_36 ; trailing comment\n";
        std::vector<std::string> ids;
        mdb::found_parse(text, ids);
        CHECK_EQ(ids.size(), 3);
        CHECK_STR(ids[0], "digong01");
        CHECK_STR(ids[1], "Chapter1_DGong_logic/BP_ItemRedBox_C_0");
        CHECK_STR(ids[2], "Chapter1_Shuwang_logic/BP_PickupActor_C_36");

        // serialize -> parse -> serialize is what the mod does every time it
        // auto-marks something; it has to be a fixed point.
        const std::string once = mdb::found_serialize(ids);
        std::vector<std::string> back;
        mdb::found_parse(once, back);
        CHECK_EQ(back.size(), 3);
        const std::string twice = mdb::found_serialize(back);
        CHECK_STR(twice, once);

        // Sorted output is what makes the file diffable.
        CHECK(once.find("Chapter1_DGong_logic/BP_ItemRedBox_C_0") < once.find("digong01"));

        // Empty in, header-only out, and nothing comes back.
        std::vector<std::string> none;
        const std::string empty = mdb::found_serialize(none);
        std::vector<std::string> none_back;
        mdb::found_parse(empty, none_back);
        CHECK_EQ(none_back.size(), 0);

        // A file the mod has never written (BOM, CRLF, tabs) still reads.
        std::vector<std::string> crlf;
        mdb::found_parse("\xEF\xBB\xBF\tdigong01\r\n\r\ndigong02\r\n", crlf);
        CHECK_EQ(crlf.size(), 2);
        CHECK_STR(crlf[0], "digong01");
        CHECK_STR(crlf[1], "digong02");
    }

    //======================================================================================
    // Level-name interning (mdb::lower_ascii / mdb::intern_levels)
    //======================================================================================
    //
    // publish_round() indexes its per-round level state by these ids instead of hashing
    // a lower-cased copy of every marker's level name every round. The join is
    // case-insensitive because the marker DB and UObject::GetFullName() need not agree
    // on case, so getting the folding wrong would silently disable the absence rule.

    void test_intern_levels()
    {
        section("level-name interning");

        CHECK_STR(mdb::lower_ascii("Chapter1_DGong_logic"), "chapter1_dgong_logic");
        CHECK_STR(mdb::lower_ascii(""), "");
        CHECK_STR(mdb::lower_ascii("A-Z0_9[]"), "a-z0_9[]"); // only A-Z is touched

        std::vector<mdb::StaticMarker> markers(5);
        markers[0].level = "Chapter1_DGong_logic";
        markers[1].level = "chapter1_dgong_LOGIC"; // the same level, different case
        markers[2].level = "";                     // no level at all
        markers[3].level = "Chapter2_Temple_logic";
        markers[4].level = "Chapter1_DGong_logic";

        std::vector<std::string> levels;
        std::vector<int> marker_level;
        mdb::intern_levels(markers, levels, marker_level);

        CHECK(levels.size() == 2);
        CHECK_STR(levels[0], "chapter1_dgong_logic");
        CHECK_STR(levels[1], "chapter2_temple_logic");
        CHECK(marker_level.size() == markers.size());
        CHECK(marker_level[0] == 0);
        CHECK(marker_level[1] == 0); // case-folded onto the same id
        CHECK(marker_level[2] == -1);
        CHECK(marker_level[3] == 1);
        CHECK(marker_level[4] == 0);

        // Every id is a valid index into `levels` or -1, and it names the marker's own
        // level - the invariant publish_round() indexes on.
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            const int id = marker_level[i];
            CHECK(id >= -1 && id < static_cast<int>(levels.size()));
            if (id >= 0)
            {
                CHECK_STR(levels[static_cast<std::size_t>(id)], mdb::lower_ascii(markers[i].level));
            }
            else
            {
                CHECK(markers[i].level.empty());
            }
        }

        // The outputs are overwritten, never appended to.
        std::vector<mdb::StaticMarker> none;
        mdb::intern_levels(none, levels, marker_level);
        CHECK(levels.empty());
        CHECK(marker_level.empty());
    }

    //======================================================================================
    // The per-activity performance counters (src/perf.hpp)
    //======================================================================================
    //
    // The F2 debug table is only as useful as this arithmetic: a wrong window reset
    // would show a rate of zero for something running at 1 kHz, and the failure mode of
    // a diagnostic is that nobody notices it is lying.

    void test_perf()
    {
        section("per-activity performance counters");

        perf::Table t{};
        CHECK(t.count == 0);

        const int a = perf::register_counter(t, "publish_round", perf::Thread::Game);
        const int b = perf::register_counter(t, "render frame", perf::Thread::Render);
        CHECK(a == 0);
        CHECK(b == 1);
        CHECK(t.count == 2);
        // The same name pointer registers once, so a call site's lazy "if (id < 0)"
        // cannot duplicate a row.
        CHECK(perf::register_counter(t, "publish_round", perf::Thread::Game) == 0);
        CHECK(t.count == 2);
        CHECK(perf::register_counter(t, nullptr, perf::Thread::Loop) == -1);

        // Recording into an id that was never handed out must be a no-op, not a write
        // past the array.
        perf::record(t, -1, 5.0, 1000);
        perf::record(t, 99, 5.0, 1000);
        CHECK(t.c[0].calls == 0);

        // last / peak update immediately; avg and rate only when a window closes.
        perf::record(t, a, 2.0, 1000);
        CHECK(t.c[a].calls == 1);
        CHECK_NEAR(t.c[a].last_ms, 2.0, 1e-12);
        CHECK_NEAR(t.c[a].peak_ms, 2.0, 1e-12);
        CHECK_NEAR(t.c[a].avg_ms, 0.0, 1e-12);
        perf::record(t, a, 6.0, 1500);
        CHECK_NEAR(t.c[a].last_ms, 6.0, 1e-12);
        CHECK_NEAR(t.c[a].peak_ms, 6.0, 1e-12);
        perf::record(t, a, 4.0, 1999);
        CHECK_NEAR(t.c[a].peak_ms, 6.0, 1e-12); // a smaller sample never lowers the peak
        CHECK_NEAR(t.c[a].avg_ms, 0.0, 1e-12);  // still inside the first window

        // The window closes at kWindowMs: four samples of 2+6+4+8 over 2000 ms.
        perf::record(t, a, 8.0, 3000);
        CHECK(t.c[a].calls == 4);
        CHECK_NEAR(t.c[a].avg_ms, 5.0, 1e-9);
        CHECK_NEAR(t.c[a].rate_hz, 2.0, 1e-9); // 4 calls in 2000 ms
        // ...and the window restarts empty, so the next average is not diluted.
        CHECK(t.c[a].win_calls == 0);
        CHECK_NEAR(t.c[a].win_total_ms, 0.0, 1e-12);

        // A clock that goes backwards (it can, across a suspend) must not wedge the
        // window or produce a negative rate.
        perf::record(t, a, 1.0, 10);
        CHECK(t.c[a].calls == 5);
        CHECK(t.c[a].rate_hz >= 0.0);

        // Idle detection: a counter nobody has recorded into is idle, one just recorded
        // is not, and one whose window has been open for several windows is idle again.
        perf::Table q{};
        const int c = perf::register_counter(q, "widget sweep", perf::Thread::Game);
        CHECK(perf::idle(q.c[c], 0));
        perf::record(q, c, 30.0, 5000);
        CHECK(!perf::idle(q.c[c], 5000));
        CHECK(!perf::idle(q.c[c], 5000 + perf::kWindowMs * 2));
        CHECK(perf::idle(q.c[c], 5000 + perf::kWindowMs * 4));

        // Peaks are resettable - a hitch during a loading screen must not hide every
        // later regression behind it - and nothing else is disturbed.
        CHECK_NEAR(q.c[c].peak_ms, 30.0, 1e-12);
        perf::reset_peaks(q);
        CHECK_NEAR(q.c[c].peak_ms, 0.0, 1e-12);
        CHECK(q.c[c].calls == 1);
        CHECK_NEAR(q.c[c].last_ms, 30.0, 1e-12);

        // The table is a fixed array: registering past it is refused, never written.
        perf::Table full{};
        // Distinct pointers, so nothing is deduplicated by the name check.
        static char storage[perf::kMaxCounters + 4][2] = {};
        for (int i = 0; i < perf::kMaxCounters; ++i)
        {
            CHECK(perf::register_counter(full, storage[i], perf::Thread::Loop) == i);
        }
        CHECK(full.count == perf::kMaxCounters);
        CHECK(perf::register_counter(full, storage[perf::kMaxCounters], perf::Thread::Loop) == -1);
        CHECK(full.count == perf::kMaxCounters);

        CHECK_STR(perf::thread_name(perf::Thread::Loop), "loop");
        CHECK_STR(perf::thread_name(perf::Thread::Game), "game");
        CHECK_STR(perf::thread_name(perf::Thread::Render), "render");
        CHECK_STR(perf::thread_name(perf::Thread::Unknown), "?");
    }

    void test_ids()
    {
        section("stable ids");

        CHECK_STR(mdb::level_from_full_name(
                      "BP_RebornFire_C /Game/Maps/Chapter1/Chapter1_DGong_logic.Chapter1_DGong_logic:"
                      "PersistentLevel.BP_RebornFire_C_0"),
                  "Chapter1_DGong_logic");
        CHECK_STR(mdb::level_from_full_name("Actor /Game/Maps/ProjectMain.ProjectMain:PersistentLevel.Foo_2"),
                  "ProjectMain");
        CHECK_STR(mdb::level_from_full_name("no slashes here"), "");
        CHECK_STR(mdb::level_from_full_name(""), "");

        CHECK_STR(mdb::stable_id("Chapter1_DGong_logic", "BP_ItemRedBox_C_0"),
                  "Chapter1_DGong_logic/BP_ItemRedBox_C_0");
        // A missing level must still yield a usable id, never an empty one.
        CHECK_STR(mdb::stable_id("", "BP_ItemRedBox_C_0"), "BP_ItemRedBox_C_0");
        CHECK_STR(mdb::stable_id("Chapter1_DGong_logic", ""), "");

        // The ids the loader read out of the sample manifest must be exactly what the
        // runtime would build for the same actors - that join is the whole point.
        CHECK_STR(mdb::stable_id(mdb::level_from_full_name(
                                     "BP_ItemRedBox_C /Game/Maps/Chapter1/Chapter1_DGong_logic."
                                     "Chapter1_DGong_logic:PersistentLevel.BP_ItemRedBox_C_0"),
                                 "BP_ItemRedBox_C_0"),
                  "Chapter1_DGong_logic/BP_ItemRedBox_C_0");
    }
    //======================================================================================
    // The full map: viewport math, zoom, waypoint file
    //======================================================================================

    void test_mapview()
    {
        section("full map - the viewport transform");

        mv::Rect r{100.0f, 50.0f, 1000.0f, 650.0f}; // 900 x 600, centre (550, 350)
        CHECK_NEAR(r.cx(), 550.0, 1e-6);
        CHECK_NEAR(r.cy(), 350.0, 1e-6);
        CHECK_NEAR(r.w(), 900.0, 1e-6);
        CHECK_NEAR(r.h(), 600.0, 1e-6);
        CHECK(r.contains(100.0f, 50.0f));
        CHECK(r.contains(1000.0f, 650.0f));
        CHECK(!r.contains(99.0f, 300.0f));
        CHECK(!r.contains(500.0f, 651.0f));

        mv::View v{};
        v.cx = 12000.0;
        v.cy = -3400.0;
        v.uu_per_px = 40.0;

        // The centre of the rectangle is the centre of the view, by definition.
        float sx = 0.0f;
        float sy = 0.0f;
        mv::world_to_screen(v, r, v.cx, v.cy, sx, sy);
        CHECK_NEAR(sx, 550.0, 1e-3);
        CHECK_NEAR(sy, 350.0, 1e-3);

        // NORTH IS UP and EAST IS RIGHT - the same convention as build_map.py and the
        // minimap at yaw 0. Getting this backwards is the one bug that would put every
        // marker in the wrong quadrant, so it is asserted directly rather than only
        // through the round-trip below.
        mv::world_to_screen(v, r, v.cx + 400.0, v.cy, sx, sy); // 400 uu north
        CHECK_NEAR(sx, 550.0, 1e-3);
        CHECK_NEAR(sy, 350.0 - 10.0, 1e-3); // 400 / 40 = 10 px UP
        mv::world_to_screen(v, r, v.cx, v.cy + 800.0, sx, sy); // 800 uu east
        CHECK_NEAR(sx, 550.0 + 20.0, 1e-3); // 20 px RIGHT
        CHECK_NEAR(sy, 350.0, 1e-3);

        // The inverse is exact over the whole viewport, at several zooms. This is what
        // makes a click on the map land on the world position it looks like.
        const double zooms[] = {6.0, 26.0, 55.0, 240.0, 900.0};
        for (const double z : zooms)
        {
            v.uu_per_px = z;
            for (int i = 0; i <= 8; ++i)
            {
                for (int j = 0; j <= 8; ++j)
                {
                    const float px = r.x0 + r.w() * static_cast<float>(i) / 8.0f;
                    const float py = r.y0 + r.h() * static_cast<float>(j) / 8.0f;
                    double wx = 0.0;
                    double wy = 0.0;
                    mv::screen_to_world(v, r, px, py, wx, wy);
                    float bx = 0.0f;
                    float by = 0.0f;
                    mv::world_to_screen(v, r, wx, wy, bx, by);
                    // Everything is done in doubles and only the result is narrowed, so
                    // a 1/1000 px tolerance is generous even at 900 uu/px.
                    CHECK_NEAR(bx, px, 0.001);
                    CHECK_NEAR(by, py, 0.001);
                }
            }
        }

        // A degenerate zoom must not divide by zero or produce NaN.
        v.uu_per_px = 0.0;
        mv::world_to_screen(v, r, 1.0, 2.0, sx, sy);
        CHECK(sx == sx && sy == sy);

        section("full map - zoom limits");

        CHECK_NEAR(mv::clamp_zoom(50.0, 10.0, 100.0), 50.0, 1e-9);
        CHECK_NEAR(mv::clamp_zoom(5.0, 10.0, 100.0), 10.0, 1e-9);
        CHECK_NEAR(mv::clamp_zoom(500.0, 10.0, 100.0), 100.0, 1e-9);
        // The limits are accepted in either order, and nonsense falls back to the low
        // limit rather than to a division by zero.
        CHECK_NEAR(mv::clamp_zoom(50.0, 100.0, 10.0), 50.0, 1e-9);
        CHECK_NEAR(mv::clamp_zoom(0.0, 10.0, 100.0), 10.0, 1e-9);
        CHECK_NEAR(mv::clamp_zoom(-3.0, 10.0, 100.0), 10.0, 1e-9);

        // Positive notches zoom IN (fewer uu per pixel), and a whole notch is exactly
        // the factor.
        CHECK_NEAR(mv::zoom_by(100.0, 1.0, 1.25, 1.0, 1000.0), 80.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, -1.0, 1.25, 1.0, 1000.0), 125.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, 2.0, 1.25, 1.0, 1000.0), 64.0, 1e-9);
        // Zooming always stops at the limits, however many notches arrive.
        CHECK_NEAR(mv::zoom_by(100.0, 50.0, 1.25, 6.0, 900.0), 6.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, -50.0, 1.25, 6.0, 900.0), 900.0, 1e-9);
        // A no-op factor or no notches leaves the (clamped) zoom alone.
        CHECK_NEAR(mv::zoom_by(100.0, 3.0, 1.0, 6.0, 900.0), 100.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, 0.0, 1.25, 6.0, 900.0), 100.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(2.0, 0.0, 1.25, 6.0, 900.0), 6.0, 1e-9);

        section("full map - the waypoint file");

        mv::Waypoint wp{};
        wp.set = true;
        wp.x = 18176.671875;
        wp.y = -13905.2109375;
        wp.z = -7641.22;
        const std::string text = mv::waypoint_serialize(wp);
        mv::Waypoint back{};
        CHECK(mv::waypoint_parse(text, back));
        CHECK(back.set);
        // The round-trip is EXACT: the file is written at 17 significant digits, so a
        // waypoint survives a save/load with no drift at all.
        CHECK(back.x == wp.x);
        CHECK(back.y == wp.y);
        CHECK(back.z == wp.z);

        // A cleared waypoint round-trips as cleared, not as "at the origin".
        mv::Waypoint none{};
        mv::Waypoint none_back{};
        none_back.set = true;
        CHECK(mv::waypoint_parse(mv::waypoint_serialize(none), none_back));
        CHECK(!none_back.set);

        // Hand-written files: a BOM, CRLF, comments, spacing, and no `set` line at all.
        mv::Waypoint hand{};
        CHECK(mv::waypoint_parse("\xEF\xBB\xBF; mine\r\n  x =  100.5 \r\ny=-200\r\n; z is optional\r\n",
                                 hand));
        CHECK(hand.set);
        CHECK_NEAR(hand.x, 100.5, 1e-9);
        CHECK_NEAR(hand.y, -200.0, 1e-9);
        CHECK_NEAR(hand.z, 0.0, 1e-9);

        // Anything without a usable x AND y is rejected, and the caller's value is left
        // untouched - a corrupt file must never move an existing waypoint.
        mv::Waypoint keep{};
        keep.set = true;
        keep.x = 7.0;
        CHECK(!mv::waypoint_parse("", keep));
        CHECK(!mv::waypoint_parse("set = 1\n", keep));
        CHECK(!mv::waypoint_parse("x = 1\n", keep));
        CHECK(!mv::waypoint_parse("x = hello\ny = 3\n", keep));
        CHECK(!mv::waypoint_parse("; only a comment\n", keep));
        CHECK(keep.set);
        CHECK_NEAR(keep.x, 7.0, 1e-9);
    }
    //======================================================================================
    // The chunked object-array scan scheduler (src/scan_sched.hpp)
    //======================================================================================

    void test_scan_sched()
    {
        std::printf("-- scan scheduler --\n");

        // --- clamps ---------------------------------------------------------------
        CHECK(scan::clamp_chunk(scan::kChunkDefault) == scan::kChunkDefault);
        CHECK(scan::clamp_chunk(0) == scan::kChunkMin);
        CHECK(scan::clamp_chunk(-5) == scan::kChunkMin);
        CHECK(scan::clamp_chunk(1 << 30) == scan::kChunkMax);
        CHECK(scan::clamp_period_ms(0) == scan::kPeriodMinMs);
        CHECK(scan::clamp_period_ms(100000) == scan::kPeriodMaxMs);
        CHECK(scan::clamp_period_ms(scan::kPeriodDefaultMs) == scan::kPeriodDefaultMs);

        // --- a whole round visits every slot exactly once, in order ---------------
        {
            constexpr int kTotal = 25000;
            constexpr int kChunk = 8192;
            scan::Cursor c{};
            int expected_next = 0;
            int slices = 0;
            bool wrapped = false;
            while (!wrapped && slices < 1000)
            {
                const scan::Slice s = scan::next_slice(c, kTotal, kChunk);
                CHECK(s.begin == expected_next);
                CHECK(s.count() > 0);
                CHECK(s.count() <= kChunk);
                CHECK(s.end <= kTotal);
                expected_next = s.end;
                wrapped = scan::advance(c, s, kTotal);
                ++slices;
            }
            CHECK(wrapped);
            CHECK(expected_next == kTotal);
            // ceil(25000 / 8192) == 4, so a 1 s round at 8 ms per slice has plenty of
            // headroom - which is the whole point of the redesign.
            CHECK(slices == 4);
            CHECK(c.round == 1);
            CHECK(c.index == 0);
            CHECK(c.visited == 0); // reset by the wrap
        }

        // --- a chunk that swallows the array wraps in one slice --------------------
        {
            scan::Cursor c{};
            const scan::Slice s = scan::next_slice(c, 100, 8192);
            CHECK(s.begin == 0);
            CHECK(s.end == 100);
            CHECK(scan::advance(c, s, 100));
            CHECK(c.round == 1);
        }

        // --- the array shrinking under the cursor does not run off the end ---------
        // GUObjectArray grows as levels stream in and can drop after a GC, so `total`
        // is re-read every slice. A cursor left beyond the new end restarts at 0
        // instead of planning an out-of-range slice.
        {
            scan::Cursor c{};
            c.index = 40000;
            const scan::Slice s = scan::next_slice(c, 1000, 8192);
            CHECK(s.begin == 0);
            CHECK(s.end == 1000);
        }

        // --- an empty array still ends its round -----------------------------------
        // Otherwise nothing would ever be published again.
        {
            scan::Cursor c{};
            const scan::Slice s = scan::next_slice(c, 0, 8192);
            CHECK(s.empty());
            CHECK(scan::advance(c, s, 0));
            CHECK(c.round == 1);
            CHECK(c.index == 0);
        }

        // --- the visited counter tracks the round, not the run ---------------------
        {
            scan::Cursor c{};
            const scan::Slice s1 = scan::next_slice(c, 20000, 8192);
            CHECK(!scan::advance(c, s1, 20000));
            CHECK(c.visited == 8192);
            const scan::Slice s2 = scan::next_slice(c, 20000, 8192);
            CHECK(s2.begin == 8192);
            CHECK(!scan::advance(c, s2, 20000));
            CHECK(c.visited == 16384);
            const scan::Slice s3 = scan::next_slice(c, 20000, 8192);
            CHECK(s3.count() == 20000 - 16384);
            CHECK(scan::advance(c, s3, 20000));
            CHECK(c.visited == 0);
        }

        // --- rate gates ------------------------------------------------------------
        // A zero "last" means never-ran and is always due; that is what makes the very
        // first pump after a level load take a slice immediately.
        CHECK(scan::elapsed(1000, 0, 1000000));
        CHECK(!scan::slice_due(5000, 1000, 8));   // 4 ms into an 8 ms period
        CHECK(scan::slice_due(9001, 1000, 8));    // 8.001 ms
        CHECK(scan::slice_due(1000000, 0, 8));
        // A QPC that appears to go backwards (it can across a suspend) must not wedge
        // the scan forever.
        CHECK(scan::slice_due(500, 1000, 8));

        // rounds_per_sec 1 => a finished round waits a second before the next starts.
        CHECK(!scan::round_due(999999, 1, 1));
        CHECK(scan::round_due(1000002, 1, 1));
        // Out-of-range values are clamped, never divided by zero.
        CHECK(scan::round_due(1000002, 1, 0));
        CHECK(scan::round_due(20000, 1, 1000));

        // --- diagnostics arithmetic -------------------------------------------------
        {
            scan::RoundStats r{};
            CHECK_NEAR(r.avg_ms(), 0.0, 1e-12); // no slices yet: no division by zero
            scan::note_slice(r, 0.5, 8192);
            scan::note_slice(r, 1.5, 8192);
            scan::note_slice(r, 0.25, 4000);
            CHECK(r.slices == 3);
            CHECK(r.objects == 20384);
            CHECK_NEAR(r.total_ms, 2.25, 1e-12);
            CHECK_NEAR(r.peak_ms, 1.5, 1e-12);
            CHECK_NEAR(r.avg_ms(), 0.75, 1e-12);
        }
    }

    //======================================================================================
    // The adaptive menu-widget discovery sweep (scan::SweepSched)
    //======================================================================================
    //
    // The FindAllOf("UserWidget") sweep costs 28-51 ms of whole-object-array walk and it
    // used to run every 250 ms. It now only has to DISCOVER a menu root nobody has ever
    // seen: gamestate.cpp re-tests every root it has ever confirmed on each 10 Hz pump.
    // This schedule is the part of that which can be proven with the game closed.

    void test_sweep_sched()
    {
        std::printf("-- the adaptive menu-widget sweep schedule --\n");

        scan::SweepSched s{};
        CHECK(s.fast_ms == 250);
        CHECK(s.slow_ms == 2000);
        CHECK(s.warm_ms == 2000);

        // Never run: due immediately, so a fresh reader sweeps on its first pump.
        CHECK(scan::sweep_due(s, 0));
        CHECK(scan::sweep_due(s, 12345));

        // Arming makes it due now and restores the fast cadence.
        scan::sweep_arm(s, 10000);
        CHECK(scan::sweep_due(s, 10000));
        CHECK(scan::sweep_armed(s, 10000));
        CHECK(scan::sweep_armed(s, 11999));
        CHECK(!scan::sweep_armed(s, 12000)); // warm_ms after the arm

        // While armed, a fruitless sweep still reschedules at the fast cadence.
        scan::sweep_done(s, 10000, false, false);
        CHECK(s.backoff == 0);
        CHECK(!scan::sweep_due(s, 10249));
        CHECK(scan::sweep_due(s, 10250));

        // Once warm and quiet the period doubles per fruitless sweep, capped at slow_ms.
        scan::SweepSched q{};
        scan::sweep_arm(q, 0);
        scan::sweep_done(q, 5000, false, false); // 5 s in: no longer armed
        CHECK(q.backoff == 1);
        CHECK(scan::sweep_period_ms(q, 5000) == 500);
        CHECK(!scan::sweep_due(q, 5499));
        CHECK(scan::sweep_due(q, 5500));
        scan::sweep_done(q, 5500, false, false);
        CHECK(scan::sweep_period_ms(q, 5500) == 1000);
        scan::sweep_done(q, 6500, false, false);
        CHECK(scan::sweep_period_ms(q, 6500) == 2000);
        scan::sweep_done(q, 8500, false, false);
        CHECK(scan::sweep_period_ms(q, 8500) == 2000); // clamped at slow_ms
        scan::sweep_done(q, 10500, false, false);
        CHECK(scan::sweep_period_ms(q, 10500) == 2000);

        // Discovering a new root resets the backoff: something is changing.
        scan::sweep_done(q, 12500, true, false);
        CHECK(q.backoff == 0);
        CHECK(scan::sweep_period_ms(q, 12500) == 250);

        // THE LATENCY GUARANTEE. While nothing at all is on the watchlist there is no
        // cheap per-pump test that could notice a menu, so the sweep must stay fast no
        // matter how long it has been quiet - that is what bounds the FIRST menu open of
        // a session at one fast period.
        scan::SweepSched e{};
        scan::sweep_arm(e, 0);
        for (std::uint64_t t = 0; t < 60000; t += 250)
        {
            scan::sweep_done(e, t, false, /*nothing_known=*/true);
            CHECK(e.backoff == 0);
            CHECK(scan::sweep_period_ms(e, t) == 250);
        }

        // A re-arm in the middle of a backed-off run puts it straight back to fast and
        // makes a sweep due on the spot (this is what a menu flip / teleport does).
        scan::SweepSched r2{};
        scan::sweep_arm(r2, 0);
        scan::sweep_done(r2, 9000, false, false);
        scan::sweep_done(r2, 11000, false, false);
        CHECK(scan::sweep_period_ms(r2, 11000) > 250);
        scan::sweep_arm(r2, 11500);
        CHECK(scan::sweep_due(r2, 11500));
        CHECK(scan::sweep_period_ms(r2, 11500) == 250);

        // The shift is bounded, so a very long quiet run can never overflow or exceed
        // slow_ms.
        scan::SweepSched b{};
        b.slow_ms = 1000000;
        scan::sweep_arm(b, 0);
        for (int i = 0; i < 40; ++i)
        {
            scan::sweep_done(b, 100000, false, false);
        }
        CHECK(b.backoff == scan::kSweepMaxBackoff);
        CHECK(scan::sweep_period_ms(b, 100000) == 250ull << scan::kSweepMaxBackoff);
    }
    //======================================================================================
    // src/projection.hpp - world -> screen
    //======================================================================================
    //
    // The x-ray highlight is only as good as this, and a wrong basis row or FOV axis
    // would only ever show up in a play session. Every expectation below is computed by
    // hand from the conventions documented at the top of projection.hpp.

    void test_projection()
    {
        std::printf("projection (world -> screen)\n");

        constexpr double kW = 1920.0;
        constexpr double kH = 1080.0;
        const double aspect = kW / kH; // 1.7778

        proj::Camera cam{};
        cam.x = 0.0;
        cam.y = 0.0;
        cam.z = 0.0;
        cam.pitch = 0.0;
        cam.yaw = 0.0; // looking north (+X)
        cam.roll = 0.0;
        cam.fov_deg = 90.0; // tan(45) == 1

        // --- dead centre -------------------------------------------------------------
        {
            const proj::Result r = proj::project(cam, 1000.0, 0.0, 0.0, kW, kH);
            CHECK(r.valid);
            CHECK(!r.behind);
            CHECK(r.on_screen);
            CHECK_NEAR(r.depth, 1000.0, 1e-9);
            CHECK_NEAR(r.dist, 1000.0, 1e-9);
            CHECK_NEAR(r.ndc_x, 0.0, 1e-12);
            CHECK_NEAR(r.ndc_y, 0.0, 1e-12);
            CHECK_NEAR(r.sx, 960.0, 1e-6);
            CHECK_NEAR(r.sy, 540.0, 1e-6);
        }

        // --- the horizontal FOV edges: 45 deg off axis at fov 90 is exactly the rim ----
        {
            const proj::Result right = proj::project(cam, 1000.0, 1000.0, 0.0, kW, kH);
            CHECK(right.on_screen);
            CHECK_NEAR(right.ndc_x, 1.0, 1e-12);
            CHECK_NEAR(right.sx, 1920.0, 1e-6);
            CHECK_NEAR(right.sy, 540.0, 1e-6);

            const proj::Result left = proj::project(cam, 1000.0, -1000.0, 0.0, kW, kH);
            CHECK(left.on_screen);
            CHECK_NEAR(left.sx, 0.0, 1e-6);
        }

        // --- the vertical FOV is the horizontal one divided by the aspect --------------
        // tan(vfov/2) = tan(hfov/2) / aspect, so the top of the screen is at
        // up/forward == 1/aspect == 0.5625, NOT at 1.0.
        {
            const proj::Result top = proj::project(cam, 1000.0, 0.0, 1000.0 / aspect, kW, kH);
            CHECK(top.on_screen);
            CHECK_NEAR(top.ndc_y, 1.0, 1e-12);
            CHECK_NEAR(top.sy, 0.0, 1e-6);

            // A point at 45 degrees up is well off the top of a 16:9 screen.
            const proj::Result high = proj::project(cam, 1000.0, 0.0, 1000.0, kW, kH);
            CHECK(!high.on_screen);
            CHECK(!high.behind);
            CHECK_NEAR(high.ndc_y, aspect, 1e-12);
        }

        // --- yaw and pitch ------------------------------------------------------------
        {
            proj::Camera east = cam;
            east.yaw = 90.0; // looking east (+Y)
            const proj::Result r = proj::project(east, 0.0, 1000.0, 0.0, kW, kH);
            CHECK(r.on_screen);
            CHECK_NEAR(r.sx, 960.0, 1e-6);
            CHECK_NEAR(r.sy, 540.0, 1e-6);
            // 45 degrees to the left of the new forward axis is the left rim.
            const proj::Result left45 = proj::project(east, 1000.0, 1000.0, 0.0, kW, kH);
            CHECK(!left45.behind);
            CHECK(left45.on_screen);
            CHECK_NEAR(left45.ndc_x, -1.0, 1e-9);
            // What used to be straight ahead is now exactly 90 degrees off, i.e. ON the
            // camera plane - which is the behind case, not a screen position.
            const proj::Result north = proj::project(east, 1000.0, 0.0, 0.0, kW, kH);
            CHECK(north.behind);
            CHECK(north.ndc_x < 0.0);

            proj::Camera down = cam;
            down.pitch = -45.0; // looking down 45 degrees
            const proj::Result below = proj::project(down, 1000.0, 0.0, -1000.0, kW, kH);
            CHECK(below.on_screen);
            CHECK_NEAR(below.sx, 960.0, 1e-6);
            CHECK_NEAR(below.sy, 540.0, 1e-6);
        }

        // --- behind the camera: never a screen position, always a direction ------------
        {
            const proj::Result back = proj::project(cam, -1000.0, 0.0, 0.0, kW, kH);
            CHECK(back.valid);
            CHECK(back.behind);
            CHECK(!back.on_screen);
            CHECK(back.depth < 0.0);
            // Straight behind: the "turn around" convention is the bottom edge.
            CHECK_NEAR(back.ndc_x, 0.0, 1e-12);
            CHECK_NEAR(back.ndc_y, -2.0, 1e-12);

            // Behind AND to the right: the arrow must point RIGHT (the short way round),
            // which is exactly what a naive divide by a negative depth gets wrong.
            const proj::Result back_right = proj::project(cam, -1000.0, 500.0, 0.0, kW, kH);
            CHECK(back_right.behind);
            CHECK(back_right.ndc_x > 0.0);
            CHECK_NEAR(back_right.ndc_x, 2.0, 1e-12);

            const proj::Result back_left = proj::project(cam, -1000.0, -500.0, 0.0, kW, kH);
            CHECK(back_left.behind);
            CHECK(back_left.ndc_x < 0.0);

            // On the camera plane counts as behind (the divide is meaningless there).
            const proj::Result plane = proj::project(cam, 0.0, 300.0, 0.0, kW, kH);
            CHECK(plane.behind);
        }

        // --- FOV changes the scale, and only the scale ---------------------------------
        {
            proj::Camera narrow = cam;
            narrow.fov_deg = 60.0; // tan(30) = 0.5774
            const proj::Result r = proj::project(narrow, 1000.0, 1000.0, 0.0, kW, kH);
            CHECK(!r.on_screen); // 45 deg off axis no longer fits in a 60 deg view
            CHECK_NEAR(r.ndc_x, 1.0 / std::tan(30.0 * proj::kPi / 180.0), 1e-9);
        }

        // --- a garbage camera must produce nothing, never a screenful of labels --------
        {
            proj::Camera bad = cam;
            bad.x = 1.0e18;
            CHECK(!proj::camera_sane(bad));
            CHECK(!proj::project(bad, 1.0, 2.0, 3.0, kW, kH).valid);

            bad = cam;
            bad.fov_deg = 0.0;
            CHECK(!proj::camera_sane(bad));
            bad.fov_deg = 179.0;
            CHECK(!proj::camera_sane(bad));

            bad = cam;
            bad.pitch = 120.0; // UE clamps camera pitch to +-90
            CHECK(!proj::camera_sane(bad));

            bad = cam;
            bad.yaw = 447.0; // legal: UE does not wrap yaw
            CHECK(proj::camera_sane(bad));

            // A zero-size screen (the frame a swapchain is resized) is not a crash.
            CHECK(!proj::project(cam, 1000.0, 0.0, 0.0, 0.0, 0.0).valid);
        }
    }

    //======================================================================================
    // src/compass.cpp - the heading strip
    //======================================================================================

    void test_compass()
    {
        std::printf("compass (headings and bearings)\n");

        // --- wrapping -----------------------------------------------------------------
        CHECK_NEAR(cmp::wrap180(0.0), 0.0, 1e-12);
        CHECK_NEAR(cmp::wrap180(190.0), -170.0, 1e-12);
        CHECK_NEAR(cmp::wrap180(-190.0), 170.0, 1e-12);
        CHECK_NEAR(cmp::wrap180(180.0), 180.0, 1e-12);
        CHECK_NEAR(cmp::wrap180(-180.0), 180.0, 1e-12);
        CHECK_NEAR(cmp::wrap180(750.0), 30.0, 1e-9);
        // UE does not wrap yaw on save; a 447 degree heading must still work.
        CHECK_NEAR(cmp::wrap180(447.0), 87.0, 1e-9);
        CHECK_NEAR(cmp::wrap360(-90.0), 270.0, 1e-12);
        CHECK_NEAR(cmp::wrap360(360.0), 0.0, 1e-12);

        // --- bearings: +X is north, +Y is east -----------------------------------------
        CHECK_NEAR(cmp::bearing_deg(0.0, 0.0, 100.0, 0.0), 0.0, 1e-9);
        CHECK_NEAR(cmp::bearing_deg(0.0, 0.0, 0.0, 100.0), 90.0, 1e-9);
        CHECK_NEAR(cmp::bearing_deg(0.0, 0.0, -100.0, 0.0), 180.0, 1e-9);
        CHECK_NEAR(cmp::bearing_deg(0.0, 0.0, 0.0, -100.0), 270.0, 1e-9);
        CHECK_NEAR(cmp::bearing_deg(500.0, 500.0, 600.0, 600.0), 45.0, 1e-9);
        CHECK_NEAR(cmp::bearing_deg(7.0, 7.0, 7.0, 7.0), 0.0, 1e-12); // no delta, no NaN

        // --- the strip -----------------------------------------------------------------
        cmp::Strip s{};
        s.x0 = 0.0;
        s.width = 600.0;
        s.heading = 0.0;
        s.span = 120.0;
        {
            double x = 0.0;
            double rel = 0.0;
            CHECK(cmp::strip_x(s, 0.0, x, rel));
            CHECK_NEAR(x, 300.0, 1e-9);
            CHECK_NEAR(rel, 0.0, 1e-12);

            CHECK(cmp::strip_x(s, 30.0, x, rel));
            CHECK_NEAR(x, 450.0, 1e-9); // half a half-span to the right
            CHECK(cmp::strip_x(s, 60.0, x, rel));
            CHECK_NEAR(x, 600.0, 1e-9); // exactly the right edge is still on the strip
            CHECK(cmp::strip_x(s, 300.0, x, rel));
            CHECK_NEAR(x, 0.0, 1e-9);
            CHECK_NEAR(rel, -60.0, 1e-9);

            // Outside: false, and clamped to the side it is on.
            CHECK(!cmp::strip_x(s, 61.0, x, rel));
            CHECK_NEAR(x, 600.0, 1e-9);
            CHECK(!cmp::strip_x(s, 180.0, x, rel));
            CHECK(!cmp::strip_x(s, 299.0, x, rel));
            CHECK_NEAR(x, 0.0, 1e-9);
        }

        // A rotated heading moves the same bearing across the strip.
        {
            cmp::Strip east = s;
            east.heading = 90.0;
            double x = 0.0;
            double rel = 0.0;
            CHECK(cmp::strip_x(east, 90.0, x, rel));
            CHECK_NEAR(x, 300.0, 1e-9);
            CHECK(!cmp::strip_x(east, 0.0, x, rel)); // north is 90 deg off, span is 120
            CHECK_NEAR(rel, -90.0, 1e-9);
            CHECK(cmp::strip_x(east, 45.0, x, rel));
            CHECK_NEAR(rel, -45.0, 1e-9);
        }

        // --- ticks ---------------------------------------------------------------------
        {
            cmp::Tick t[64]{};
            const int n = cmp::ticks(s, t, 64, 15.0);
            CHECK_EQ(n, 9); // -60 .. +60 inclusive, every 15 degrees
            CHECK_NEAR(t[0].bearing, 300.0, 1e-9);
            CHECK_NEAR(t[8].bearing, 60.0, 1e-9);
            CHECK_NEAR(t[4].bearing, 0.0, 1e-9);
            CHECK_EQ(t[4].rank, 2); // north is a cardinal
            CHECK_STR(std::string(t[4].label), std::string("N"));
            CHECK_EQ(t[7].rank, 1); // 45 = NE, intercardinal
            CHECK_STR(std::string(t[7].label), std::string("NE"));
            CHECK_EQ(t[1].rank, 1);
            CHECK_STR(std::string(t[1].label), std::string("NW")); // 315
            CHECK_EQ(t[8].rank, 0);                                // 60 is a minor tick
            CHECK_STR(std::string(t[8].label), std::string(""));
            // Left to right, and every tick inside the strip.
            for (int i = 1; i < n; ++i)
            {
                CHECK(t[i].x >= t[i - 1].x);
                CHECK(t[i].x >= s.x0 - 1e-9 && t[i].x <= s.x0 + s.width + 1e-9);
            }
            // A cap smaller than the tick count truncates instead of overflowing.
            cmp::Tick few[3]{};
            CHECK_EQ(cmp::ticks(s, few, 3, 15.0), 3);
            CHECK_EQ(cmp::ticks(s, nullptr, 0, 15.0), 0);
            // A silly step or span falls back to the defaults rather than looping.
            CHECK_EQ(cmp::ticks(s, t, 64, 0.0), 9);
            cmp::Strip wide = s;
            wide.span = 360.0;
            CHECK_EQ(cmp::ticks(wide, t, 64, 90.0), 5); // -180 .. 180 in 90s
        }

        CHECK_STR(std::string(cmp::cardinal_label(270.0)), std::string("W"));
        CHECK_STR(std::string(cmp::cardinal_label(-90.0)), std::string("W"));
        CHECK_STR(std::string(cmp::cardinal_label(10.0)), std::string(""));
    }
    //==================================================================================
    // chapterid.hpp - "which chapter is the player in?", from the streamed level names
    //==================================================================================
    //
    // This is the whole of the runtime's chapter detection except the enumeration, and
    // it is the part that can be wrong in a way no log line would reveal: a bad parse
    // just quietly loads the wrong map. The fixtures below are real names taken from
    // the pak index and from the WuchangRecon world dumps.

    //==================================================================================
    // The chapter filter for the static marker DB (mdb::marker_in_chapter)
    //==================================================================================
    //
    // The predicate publish_round() uses to cut the flat 3 601-marker DB down to the
    // chapter the player is actually in. The whole point is that chapter 4's bounds
    // cover nearly all of chapter 1, so a position test cannot do this job.

    void test_marker_chapter_filter()
    {
        std::printf("markers: the static DB's chapter filter\n");

        // --- a numbered chapter keeps its own markers and nothing else ---------------
        CHECK(mdb::marker_in_chapter(1, 1));
        CHECK(!mdb::marker_in_chapter(4, 1));
        CHECK(!mdb::marker_in_chapter(1, 4));
        CHECK(mdb::marker_in_chapter(5, 5));
        for (int ch = 1; ch <= 5; ++ch)
        {
            for (int det = 1; det <= 5; ++det)
            {
                CHECK(mdb::marker_in_chapter(ch, det) == (ch == det));
            }
        }

        // --- the DLC is bucket 0, which is what the DLC manifest's "chapter":"DLC"
        //     parses to, and it must NOT collide with any numbered chapter ------------
        CHECK(mdb::marker_in_chapter(chid::kDlc, chid::kDlc));
        CHECK(!mdb::marker_in_chapter(3, chid::kDlc));
        CHECK(!mdb::marker_in_chapter(chid::kDlc, 3));
        // The DLC's marker range overlaps chapter 3's - hence the explicit pair above.

        // --- kNone means "not detected": show everything, never hide ----------------
        CHECK(mdb::marker_in_chapter(1, chid::kNone));
        CHECK(mdb::marker_in_chapter(4, chid::kNone));
        CHECK(mdb::marker_in_chapter(chid::kDlc, chid::kNone));
        for (int ch = 0; ch <= 8; ++ch)
        {
            CHECK(mdb::marker_in_chapter(ch, chid::kNone));
        }

        // --- and the join with the parser: a real manifest's chapter number is what
        //     goes into the predicate, DLC included ------------------------------------
        {
            std::vector<mdb::StaticMarker> db;
            mdb::ParseReport rep{};
            CHECK(mdb::parse_markers_json(
                R"({"schema":"wuchang-minimap-markers/1","chapter":"DLC","markers":[)"
                R"({"id":"a/b","cat":"chest","x":1,"y":2,"z":3}]})",
                db, rep));
            CHECK(rep.chapter == chid::kDlc);
            CHECK(rep.chapter_label == "DLC");
            CHECK(db.size() == 1 && db[0].chapter == chid::kDlc);
            CHECK(mdb::marker_in_chapter(db[0].chapter, chid::kDlc));
            CHECK(!mdb::marker_in_chapter(db[0].chapter, 1));
        }
    }

    void test_chapter_id()
    {
        std::printf("chapterid: cell packages, logic levels and the tiered vote\n");

        // --- tier 3: the streaming cell package, bare name and full object path ------
        CHECK_EQ(chid::classify("B1EX0_L0_X1_Y0_DL0_WP").chapter, 1);
        CHECK_EQ(chid::classify("B1EX0_L0_X1_Y0_DL0_WP").tier, chid::kTierCell);
        CHECK_EQ(chid::classify("B3EX0_L0_X-8_Y-17_DL0_WP").chapter, 3);
        CHECK_EQ(chid::classify("B5EX0_L0_X-19_Y2_DL0_WP").chapter, 5);
        // The name the engine actually hands back from UObject::GetFullName().
        CHECK_EQ(chid::classify("Level /Game/Maps/Generate/Chapter2/EX0/"
                                "B2EX0_L0_X4_Y-3_DL0_WP.B2EX0_L0_X4_Y-3_DL0_WP:PersistentLevel")
                     .chapter,
                 2);
        // ... and the same thing as wchar_t, which is what the mod really passes.
        CHECK_EQ(chid::classify(L"Level /Game/Maps/Generate/Chapter4/EX0/"
                                L"B4EX0_L0_X0_Y0_DL0_WP.B4EX0_L0_X0_Y0_DL0_WP:PersistentLevel")
                     .chapter,
                 4);
        CHECK_EQ(chid::classify(L"B4EX0_L0_X0_Y0_DL0_WP").tier, chid::kTierCell);
        // The cell wins over the "Chapter2" that is also in the same path - the two
        // always agree here, but the tier must come from the cell.
        CHECK_EQ(chid::classify("/Game/Maps/Generate/Chapter2/EX0/B2EX0_L0_X4_Y-3_DL0_WP").tier,
                 chid::kTierCell);

        // --- the DLC has no cell package anywhere in the paks ------------------------
        CHECK_EQ(chid::classify("ChapterDLC_LiuHuangKK_logic").chapter, chid::kDlc);
        CHECK_EQ(chid::classify("ChapterDLC_LiuHuangKK_logic").tier, chid::kTierLogic);
        CHECK_EQ(chid::classify("ChapterDLC_BOSS_ZhuoJinEL_AI").tier, chid::kTierLogic);
        CHECK_EQ(chid::classify("ChapterDLC_Area").chapter, chid::kDlc);
        CHECK_EQ(chid::classify("ChapterDLC_Area").tier, chid::kTierArt);

        // --- tier 2 / 1: logic levels beat art levels --------------------------------
        CHECK_EQ(chid::classify("Chapter1_Digong_logic").chapter, 1);
        CHECK_EQ(chid::classify("Chapter1_Digong_logic").tier, chid::kTierLogic);
        CHECK_EQ(chid::classify("Chapter3_BOSS_NvWu_AI").tier, chid::kTierLogic);
        CHECK_EQ(chid::classify("Chapter1_DaFo_Base").tier, chid::kTierArt);
        CHECK_EQ(chid::classify("Chapter2_Area").tier, chid::kTierArt);
        CHECK_EQ(chid::classify("Chapter4_Land").chapter, 4);

        // --- nothing to say ----------------------------------------------------------
        CHECK_EQ(chid::classify("").tier, 0);
        CHECK_EQ(chid::classify("").chapter, chid::kNone);
        CHECK_EQ(chid::classify("Lobby").chapter, chid::kNone);
        CHECK_EQ(chid::classify("/Game/Maps/ProjectMain.ProjectMain:PersistentLevel").chapter, chid::kNone);
        CHECK_EQ(chid::classify("program/DebugCommand").chapter, chid::kNone);
        // Not a chapter: a two-digit run is an index, and chapter 0 does not exist.
        CHECK_EQ(chid::classify("Chapter12_Whatever").chapter, chid::kNone);
        CHECK_EQ(chid::classify("Chapter0_Whatever").chapter, chid::kNone);
        // A malformed cell name must not be read as a cell.
        CHECK_EQ(chid::classify("EX0_L0_X1_Y0_DL0_WP").chapter, chid::kNone);
        CHECK_EQ(chid::classify("X1EX0_L0_X1_Y0_DL0_WP").chapter, chid::kNone);

        // --- chapter_from_key, the manifest fallback ---------------------------------
        CHECK_EQ(chid::chapter_from_key("chapter1"), 1);
        CHECK_EQ(chid::chapter_from_key("chapter5"), 5);
        CHECK_EQ(chid::chapter_from_key("chapterdlc"), chid::kDlc);
        CHECK_EQ(chid::chapter_from_key("nonsense"), chid::kNone);
        CHECK_EQ(chid::chapter_from_key(""), chid::kNone);

        // --- the tiered vote ---------------------------------------------------------
        //
        // THE CASE THIS EXISTS FOR (context/common.md, run 3): at one spot 49 levels
        // were visible, including eight different Chapter-1 `_Base` levels AND both
        // `Chapter1_Area` and `Chapter2_Area`. A plain sum over "Chapter<N> appears"
        // would be decided by how much art each chapter happens to stream; the cells
        // under the player's feet must win outright.
        {
            chid::Vote v{};
            v.add("B1EX0_L0_X1_Y0_DL0_WP");
            v.add("B1EX0_L0_X2_Y0_DL0_WP");
            v.add("B1EX0_L0_X1_Y-1_DL0_WP");
            v.add("B1EX0_L0_X2_Y-1_DL0_WP");
            v.add("Chapter1_Digong_logic");
            v.add("Chapter1_DaFo_Base");
            v.add("Chapter1_Cave_Base");
            v.add("Chapter1_Area");
            v.add("Chapter2_Area"); // the stray one that must not matter
            v.add("Chapter2_Land");
            v.add("Chapter2_Something_Base");
            v.add("Lobby");
            CHECK_EQ(v.best(), 1);
            CHECK_EQ(v.best_tier(), chid::kTierCell);
            CHECK_EQ(v.best_count(), 4);
            CHECK_EQ(v.count(1, chid::kTierCell), 4);
            CHECK_EQ(v.count(2, chid::kTierCell), 0);
            CHECK_EQ(v.count(2, chid::kTierArt), 3);
            CHECK_EQ(v.seen(), 12);
            CHECK_EQ(v.classified(), 11); // "Lobby" says nothing
        }

        // The DLC: no cell package exists anywhere in the paks, so tier 2 decides - and
        // it still has to beat any chapter art that happens to be resident.
        {
            chid::Vote v{};
            v.add("ChapterDLC_LiuHuangKK_logic");
            v.add("ChapterDLC_BOSS_ChongZhen_AI");
            v.add("Chapter3_Land");
            v.add("Chapter3_Something_Base");
            v.add("Chapter3_Area");
            CHECK_EQ(v.best(), chid::kDlc);
            CHECK_EQ(v.best_tier(), chid::kTierLogic);
            CHECK_EQ(v.best_count(), 2);
        }

        // Nothing recognisable at all - e.g. the Lobby - must answer kNone, which the
        // runtime treats as "keep whatever you had" rather than "unload the map".
        {
            chid::Vote v{};
            v.add("Lobby");
            v.add("/Game/Maps/ProjectMain.ProjectMain:PersistentLevel");
            CHECK_EQ(v.best(), chid::kNone);
            CHECK_EQ(v.best_tier(), 0);
            CHECK_EQ(v.best_count(), 0);
        }

        // Two chapters' cells at once cannot normally happen (the streaming window
        // follows the player), but if it did the answer must be deterministic and the
        // majority must win.
        {
            chid::Vote v{};
            v.add("B4EX0_L0_X0_Y0_DL0_WP");
            v.add("B1EX0_L0_X0_Y0_DL0_WP");
            v.add("B4EX0_L0_X1_Y0_DL0_WP");
            CHECK_EQ(v.best(), 4);
            chid::Vote tie{};
            tie.add("B4EX0_L0_X0_Y0_DL0_WP");
            tie.add("B1EX0_L0_X0_Y0_DL0_WP");
            CHECK_EQ(tie.best(), 1); // ties go to the lower chapter number
            tie.reset();
            CHECK_EQ(tie.best(), chid::kNone);
        }
    }

    //==================================================================================
    // mapmanifest.hpp - maps/maps.json
    //==================================================================================

    void test_map_manifest(const std::string& markers_dir)
    {
        std::printf("mapmanifest: schema /3, five chapters, and the ways it can be wrong\n");

        // --- the minimal well-formed document ----------------------------------------
        {
            const char* text = R"({
              "schema": "wuchang-minimap-maps/3",
              "chapters": {
                "chapter1": { "chapter": 1, "image": "chapter1/small.png",
                              "image_width": 100, "image_height": 200,
                              "min_x": -10, "min_y": -20, "max_x": 30, "max_y": 40,
                              "px_per_uu": 0.06, "z_min": -5, "z_max": 15,
                              "max_surfaces": 2,
                              "height_maps": ["chapter1/small_z0.png", "chapter1/small_z1.png"] },
                "chapterdlc": { "chapter": 0, "image": "dlc/small.png",
                                "image_width": 10, "image_height": 10,
                                "min_x": 0, "min_y": 0, "max_x": 1, "max_y": 1,
                                "px_per_uu": 0.5, "z_min": 0, "z_max": 1,
                                "max_surfaces": 1, "height_maps": ["dlc/small_z0.png"] }
              } })";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(mapmanifest::parse(text, m, problems));
            CHECK_EQ(static_cast<long long>(problems.size()), 0);
            CHECK_STR(m.schema, std::string(mapmanifest::kSchema));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 2);
            CHECK_STR(m.chapters[0].key, std::string("chapter1"));
            CHECK_EQ(m.chapters[0].chapter, 1);
            CHECK_EQ(m.chapters[0].max_surfaces, 2);
            CHECK(m.chapters[0].geometry_ok());
            CHECK(m.chapters[0].heights_ok());
            CHECK(!m.chapters[0].height_maps_guessed);
            CHECK_EQ(m.index_of_number(1), 0);
            CHECK_EQ(m.index_of_number(chid::kDlc), 1);
            CHECK_EQ(m.index_of_number(4), -1);
            CHECK_EQ(m.index_of_key("chapterdlc"), 1);
            CHECK_EQ(m.index_of_key("chapter9"), -1);
            // The DLC is not a numbered chapter, so it is never the start-up default.
            CHECK_EQ(m.default_index(), 0);
        }

        // --- backward compatibility: the ONE-CHAPTER file this schema shipped with ---
        //
        // No "chapter" field and no "height_maps" array existed then. Both have to be
        // recovered, or a mod update silently loses the map of an unchanged install.
        {
            const char* text = R"({
              "schema": "wuchang-minimap-maps/3",
              "chapters": {
                "chapter1": { "image": "chapter1/small.png",
                              "image_width": 4947, "image_height": 4333,
                              "min_x": -20736, "min_y": -41216, "max_x": 51456, "max_y": 41216,
                              "px_per_uu": 0.06, "z_min": -15089, "z_max": 38871,
                              "max_surfaces": 8 }
              } })";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(mapmanifest::parse(text, m, problems));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK_EQ(m.chapters[0].chapter, 1); // derived from the key
            CHECK(m.chapters[0].height_maps_guessed);
            CHECK_EQ(static_cast<long long>(m.chapters[0].height_maps.size()), 8);
            CHECK_STR(m.chapters[0].height_maps[0], std::string("chapter1/small_z0.png"));
            CHECK_STR(m.chapters[0].height_maps[7], std::string("chapter1/small_z7.png"));
            CHECK(m.chapters[0].heights_ok());
        }

        // --- the ways it can be wrong -------------------------------------------------
        {
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(!mapmanifest::parse("not json at all", m, problems));
            CHECK(!problems.empty());

            problems.clear();
            CHECK(!mapmanifest::parse(R"({"schema":"wuchang-minimap-maps/3"})", m, problems));
            CHECK(!problems.empty());

            problems.clear();
            CHECK(!mapmanifest::parse(R"({"chapters": []})", m, problems)); // array, not object

            // A broken chapter is skipped and REPORTED, and its siblings still load.
            problems.clear();
            const char* mixed = R"({
              "chapters": {
                "broken_no_image": { "chapter": 2, "image_width": 4, "image_height": 4,
                                     "min_x": 0, "min_y": 0, "max_x": 1, "max_y": 1,
                                     "px_per_uu": 0.5 },
                "broken_bounds":   { "chapter": 3, "image": "a.png", "image_width": 4,
                                     "image_height": 4, "min_x": 5, "min_y": 0,
                                     "max_x": 1, "max_y": 1, "px_per_uu": 0.5 },
                "chapter4":        { "chapter": 4, "image": "chapter4/small.png",
                                     "image_width": 4, "image_height": 4,
                                     "min_x": 0, "min_y": 0, "max_x": 1, "max_y": 1,
                                     "px_per_uu": 0.5, "z_min": 0, "z_max": 1,
                                     "height_maps": ["chapter4/small_z0.png"] }
              } })";
            CHECK(mapmanifest::parse(mixed, m, problems));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK_EQ(static_cast<long long>(problems.size()), 2);
            CHECK_STR(m.chapters[0].key, std::string("chapter4"));
            CHECK_EQ(m.default_index(), 0);
            // No schema string at all is not fatal - the mod logs and reads on.
            CHECK_STR(m.schema, std::string(""));
        }

        // --- a chapter with no z range has geometry but no usable height maps ---------
        {
            const char* text = R"({"chapters": {"chapter1": {
                "chapter": 1, "image": "c/s.png", "image_width": 4, "image_height": 4,
                "min_x": 0, "min_y": 0, "max_x": 1, "max_y": 1, "px_per_uu": 0.5,
                "height_maps": ["c/s_z0.png"] }}})";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(mapmanifest::parse(text, m, problems));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK(m.chapters[0].geometry_ok());
            CHECK(!m.chapters[0].heights_ok()); // z_max == z_min == 0
        }

        // --- the SHIPPED file --------------------------------------------------------
        //
        // Same idea as test_real_db: the manifest that is actually in the repo has to
        // parse, name five chapters, and have every chapter number and every height
        // plane the runtime is going to look for.
        {
            const std::string path = markers_dir + "/../maps/maps.json";
            std::string text;
            if (!read_file(path, text))
            {
                std::printf("  (skipped: %s not readable)\n", path.c_str());
            }
            else
            {
                mapmanifest::Manifest m{};
                std::vector<std::string> problems;
                CHECK(mapmanifest::parse(text, m, problems));
                CHECK_EQ(static_cast<long long>(problems.size()), 0);
                CHECK_STR(m.schema, std::string(mapmanifest::kSchema));
                CHECK_EQ(static_cast<long long>(m.chapters.size()), 5);
                CHECK_EQ(m.default_index(), m.index_of_number(1));
                std::size_t total_ram = 0;
                for (int n = 1; n <= 5; ++n)
                {
                    const int i = m.index_of_number(n);
                    CHECK(i >= 0);
                    if (i < 0)
                    {
                        continue;
                    }
                    const mapmanifest::Entry& e = m.chapters[static_cast<std::size_t>(i)];
                    CHECK_STR(e.key, std::string("chapter") + std::to_string(n));
                    CHECK(e.geometry_ok());
                    CHECK(e.heights_ok());
                    CHECK(!e.height_maps_guessed);
                    CHECK_EQ(static_cast<long long>(e.height_maps.size()), 8);
                    CHECK(e.px_per_uu > 0.02 && e.px_per_uu <= 0.06);
                    // Every plane is 16-bit and the whole set has to fit the RAM budget
                    // build_map.py enforced (--max-ram-mb 340).
                    const std::size_t bytes = static_cast<std::size_t>(e.image_width) *
                                              static_cast<std::size_t>(e.image_height) * 2u *
                                              e.height_maps.size();
                    CHECK(bytes <= 340u * 1024u * 1024u);
                    total_ram = bytes > total_ram ? bytes : total_ram;
                }
                std::printf("  shipped manifest: %d chapters, worst chapter %llu MB resident\n",
                            static_cast<int>(m.chapters.size()),
                            static_cast<unsigned long long>(total_ram / (1024 * 1024)));
                // The DLC deliberately has NO map asset: the paks carry no
                // Maps/Generate/ChapterDLC cells, so there is no cooked navmesh for it.
                CHECK_EQ(m.index_of_number(chid::kDlc), -1);
            }
        }
    }

    //==================================================================================
    // The config file's keys: shipped file == known keys == what the parser accepts
    //==================================================================================
    //
    // Three things drift apart silently. A key in the struct and the parser but not in
    // the SHIPPED file is invisible to everyone who never presses Save in the F2 panel;
    // a key left in the shipped file after a rename is ignored without a word, which
    // looks exactly like the setting not working; and a key documented in
    // config_keys.hpp that the parser never matches is a lie in the one place a reader
    // would trust. So all three sets are compared, in both directions, and the failure
    // message names the offending keys rather than just the count.
    //
    // The parser's set is SCRAPED from src/mmstate.cpp (`key == "..."`), so the table in
    // config_keys.hpp describes the code instead of being a second hand-kept list.

    std::vector<std::string> parser_keys(const std::string& source)
    {
        std::vector<std::string> out;
        const std::string needle = "key == \"";
        std::size_t at = 0;
        while ((at = source.find(needle, at)) != std::string::npos)
        {
            at += needle.size();
            const std::size_t end = source.find('"', at);
            if (end == std::string::npos)
            {
                break;
            }
            std::string key = source.substr(at, end - at);
            at = end + 1;
            if (std::find(out.begin(), out.end(), key) == out.end())
            {
                out.push_back(std::move(key));
            }
        }
        return out;
    }

    void report_missing(const char* what, const std::vector<std::string>& a, const std::vector<std::string>& b)
    {
        // Everything in `a` that is not in `b`.
        for (const std::string& key : a)
        {
            const bool found = std::find(b.begin(), b.end(), key) != b.end();
            const std::string msg = std::string{what} + ": " + key;
            check(found, msg.c_str(), __FILE__, __LINE__);
        }
    }

    void test_config_keys(const std::string& markers_dir)
    {
        std::printf("config files - shipped keys vs. the tiers vs. the parser\n");

        // markers_dir is <repo>\markers when build.ps1 runs us.
        std::string root = markers_dir;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
        {
            root.pop_back();
        }
        const std::size_t slash = root.find_last_of("/\\");
        root = slash == std::string::npos ? std::string{"."} : root.substr(0, slash);

        const std::string dir = root + "/deploy/ue4ss/Mods/WuchangMinimap/";
        const std::string shipped_path = dir + "config_wuchang_minimap.txt";
        const std::string dev_path = dir + "config_wuchang_minimap_dev.txt";
        const std::string source_path = root + "/src/mmstate.cpp";

        std::string shipped;
        std::string dev;
        std::string source;
        const bool have_shipped = read_file(shipped_path, shipped);
        const bool have_dev = read_file(dev_path, dev);
        const bool have_source = read_file(source_path, source);
        if (!have_shipped || !have_dev || !have_source)
        {
            std::printf("  SKIPPED (run from the repo, with the markers directory as argv[1])\n");
            return;
        }

        const std::vector<std::string> player = cfgkeys::keys_of(cfgkeys::Tier::Player);
        const std::vector<std::string> advanced = cfgkeys::keys_of(cfgkeys::Tier::Advanced);
        const std::vector<std::string> dev_keys = cfgkeys::keys_of(cfgkeys::Tier::Dev);
        const std::vector<std::string> removed = cfgkeys::keys_of(cfgkeys::Tier::Removed);
        const std::vector<std::string> legacy = cfgkeys::keys_of(cfgkeys::Tier::Legacy);
        const std::vector<std::string> known = cfgkeys::shipped_keys(); // player + advanced

        const std::vector<std::string> shipped_keys = cfgkeys::keys_in(shipped);
        const std::vector<std::string> dev_file_keys = cfgkeys::keys_in(dev);
        const std::vector<std::string> parsed = parser_keys(source);

        std::printf("  player %d + advanced %d = %d shipped (file has %d);  dev %d (file has %d);  "
                    "removed %d;  legacy %d;  parser %d\n",
                    static_cast<int>(player.size()),
                    static_cast<int>(advanced.size()),
                    static_cast<int>(known.size()),
                    static_cast<int>(shipped_keys.size()),
                    static_cast<int>(dev_keys.size()),
                    static_cast<int>(dev_file_keys.size()),
                    static_cast<int>(removed.size()),
                    static_cast<int>(legacy.size()),
                    static_cast<int>(parsed.size()));

        // ---- the tiers are pairwise disjoint, and nothing is listed twice ------------
        //
        // Every set comparison below would pass by accident if a key appeared in two
        // tiers, so this is checked first.
        for (std::size_t i = 0; i < cfgkeys::kKeyCount; ++i)
        {
            int seen = 0;
            for (std::size_t j = 0; j < cfgkeys::kKeyCount; ++j)
            {
                if (std::string_view{cfgkeys::kKeys[i].name} == cfgkeys::kKeys[j].name)
                {
                    ++seen;
                }
            }
            const std::string msg = std::string{"key listed once in cfgkeys::kKeys: "} + cfgkeys::kKeys[i].name;
            check(seen == 1, msg.c_str(), __FILE__, __LINE__);
        }

        // ---- shipped file == Player U Advanced --------------------------------------
        report_missing("in the shipped config but not a Player/Advanced key", shipped_keys, known);
        report_missing("a Player/Advanced key missing from the shipped config", known, shipped_keys);

        // ---- dev file == Dev ---------------------------------------------------------
        report_missing("in the dev config but not a Dev key", dev_file_keys, dev_keys);
        report_missing("a Dev key missing from the dev config", dev_keys, dev_file_keys);

        // ---- the parser accepts exactly Player U Advanced U Dev U Legacy -------------
        //
        // A Removed key must NOT be a parser literal: it is recognised through
        // cfgkeys::is_removed() and answered with one warning, never applied.
        std::vector<std::string> parseable = known;
        parseable.insert(parseable.end(), dev_keys.begin(), dev_keys.end());
        parseable.insert(parseable.end(), legacy.begin(), legacy.end());
        report_missing("parsed by mmstate.cpp but in no live tier", parsed, parseable);
        report_missing("a live key the parser never matches", parseable, parsed);

        // ---- a removed or legacy key may never appear in a shipped file --------------
        for (const std::string& k : removed)
        {
            const bool in_shipped = std::find(shipped_keys.begin(), shipped_keys.end(), k) != shipped_keys.end();
            const bool in_dev = std::find(dev_file_keys.begin(), dev_file_keys.end(), k) != dev_file_keys.end();
            const std::string msg = std::string{"a removed key is still in a config file: "} + k;
            check(!in_shipped && !in_dev, msg.c_str(), __FILE__, __LINE__);
            const std::string msg2 = std::string{"a removed key must not be parsed: "} + k;
            check(std::find(parsed.begin(), parsed.end(), k) == parsed.end(), msg2.c_str(), __FILE__, __LINE__);
        }
        for (const std::string& k : legacy)
        {
            const bool in_shipped = std::find(shipped_keys.begin(), shipped_keys.end(), k) != shipped_keys.end();
            const bool in_dev = std::find(dev_file_keys.begin(), dev_file_keys.end(), k) != dev_file_keys.end();
            const std::string msg = std::string{"an old key NAME is still in a config file: "} + k;
            check(!in_shipped && !in_dev, msg.c_str(), __FILE__, __LINE__);
            // ...and it must still map onto a live key, or the rename is a dead end.
            const char* to = cfgkeys::renamed_to(k);
            const std::string msg2 = std::string{"a legacy key maps onto a live key: "} + k;
            check(to != nullptr && std::find(known.begin(), known.end(), std::string{to}) != known.end(),
                  msg2.c_str(), __FILE__, __LINE__);
        }

        // ---- the shipped file's banner order agrees with the Player/Advanced tags ----
        //
        // The file has one `; ---- PLAYER SETTINGS ----` banner and one
        // `; ---- ADVANCED ----` banner; every key above the second is Player, every key
        // below it is Advanced. Without this the tags and the file could describe two
        // different layouts and no set comparison would notice.
        {
            const std::size_t player_banner = shipped.find("; ---- PLAYER SETTINGS ----");
            const std::size_t adv_banner = shipped.find("; ---- ADVANCED ----");
            CHECK(player_banner != std::string::npos);
            CHECK(adv_banner != std::string::npos);
            CHECK(player_banner < adv_banner);
            if (player_banner != std::string::npos && adv_banner != std::string::npos &&
                player_banner < adv_banner)
            {
                const std::vector<std::string> above = cfgkeys::keys_in(shipped.substr(0, adv_banner));
                const std::vector<std::string> below = cfgkeys::keys_in(shipped.substr(adv_banner));
                for (const std::string& k : above)
                {
                    const std::string msg = std::string{"above the ADVANCED banner, so tagged Player: "} + k;
                    check(cfgkeys::tier_is(k, cfgkeys::Tier::Player), msg.c_str(), __FILE__, __LINE__);
                }
                for (const std::string& k : below)
                {
                    const std::string msg = std::string{"below the ADVANCED banner, so tagged Advanced: "} + k;
                    check(cfgkeys::tier_is(k, cfgkeys::Tier::Advanced), msg.c_str(), __FILE__, __LINE__);
                }
                CHECK_EQ(static_cast<int>(above.size()), static_cast<int>(player.size()));
                CHECK_EQ(static_cast<int>(below.size()), static_cast<int>(advanced.size()));
            }
        }

        // ---- the helpers ------------------------------------------------------------
        CHECK(cfgkeys::is_known("mod_enabled"));
        CHECK(cfgkeys::is_known("debug_readout"));   // Dev counts as known
        CHECK(!cfgkeys::is_known("mod_enabled_typo"));
        CHECK(!cfgkeys::is_known("slice_min_px"));   // Removed is recognised, not known
        CHECK(cfgkeys::is_removed("slice_min_px"));
        CHECK(!cfgkeys::is_removed("minimap_min_px"));
        CHECK(cfgkeys::renamed_to("enabled") != nullptr);
        CHECK(cfgkeys::renamed_to("overlay_enabled") == nullptr);
        CHECK_EQ(static_cast<int>(cfgkeys::kConfigKeyCount), static_cast<int>(known.size()));

        // The line parser: the same rules as the loader.
        const std::vector<std::string> parsed_lines = cfgkeys::keys_in(
            "\xEF\xBB\xBFmod_enabled = 1\n"
            "; a comment = not a key\n"
            "  opacity  =  0.9   ; trailing comment\n"
            "no equals sign here\n"
            "= 5\n"
            "opacity = 0.5\n");
        CHECK_EQ(static_cast<int>(parsed_lines.size()), 2);
        if (parsed_lines.size() == 2)
        {
            CHECK(parsed_lines[0] == "mod_enabled");
            CHECK(parsed_lines[1] == "opacity");
        }
    }

    //==================================================================================
    // Saving: rewriting the VALUES in a config file and nothing else
    //==================================================================================
    //
    // The F2 panel used to regenerate config_wuchang_minimap.txt from a thin comment
    // block, so one click destroyed the whole documented file. cfgrw::rewrite is the
    // replacement, and the property that matters is boring and absolute: a save that
    // changes no value must produce the file BYTE FOR BYTE. That is checked against the
    // real shipped file, which is the only version of it anyone will ever hold.

    void test_config_rewrite(const std::string& markers_dir)
    {
        std::printf("config save - rewriting values in place\n");

        std::string root = markers_dir;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
        {
            root.pop_back();
        }
        const std::size_t slash = root.find_last_of("/\\");
        root = slash == std::string::npos ? std::string{"."} : root.substr(0, slash);
        const std::string dir = root + "/deploy/ue4ss/Mods/WuchangMinimap/";

        const char* kBanner = "; ---- added by the settings panel ----";

        // ---- the small cases, on a hand-written file --------------------------------
        const std::string sample =
            "; a header comment\n"
            "\n"
            "; what opacity does\n"
            "opacity = 0.92\n"
            "  minimap_zoom  =  26   ; tuned by hand\n"
            "# a hash comment = with an equals in it\n"
            "some_future_key = 7\n";

        {
            // Unchanged values -> byte-identical, and an inline comment survives.
            std::vector<cfgrw::Pair> kv{{"opacity", "0.92"}, {"minimap_zoom", "26"}};
            const cfgrw::Result r = cfgrw::rewrite(sample, kv, kBanner);
            CHECK(r.text == sample);
            CHECK_EQ(r.rewritten, 2);
            CHECK_EQ(r.changed, 0);
            CHECK_EQ(r.appended, 0);
        }
        {
            // One changed value changes exactly one line, keeping its spacing and note.
            std::vector<cfgrw::Pair> kv{{"opacity", "0.50"}, {"minimap_zoom", "26"}};
            const cfgrw::Result r = cfgrw::rewrite(sample, kv, kBanner);
            CHECK_EQ(r.changed, 1);
            CHECK(r.text.find("opacity = 0.50\n") != std::string::npos);
            CHECK(r.text.find("  minimap_zoom  =  26   ; tuned by hand\n") != std::string::npos);
            CHECK(r.text.find("; what opacity does\n") != std::string::npos);
            CHECK(r.text.find("some_future_key = 7\n") != std::string::npos);
            CHECK(r.text.find("0.92") == std::string::npos);
        }
        {
            // A changed value on a line that has an inline comment keeps the comment.
            std::vector<cfgrw::Pair> kv{{"minimap_zoom", "40"}};
            const cfgrw::Result r = cfgrw::rewrite(sample, kv, kBanner);
            CHECK(r.text.find("  minimap_zoom  =  40   ; tuned by hand\n") != std::string::npos);
        }
        {
            // A key the file does not carry is appended ONCE, under the banner.
            std::vector<cfgrw::Pair> kv{{"opacity", "0.92"}, {"ui_scale", "auto"}};
            const cfgrw::Result r = cfgrw::rewrite(sample, kv, kBanner);
            CHECK_EQ(r.appended, 1);
            CHECK(r.text.find(std::string{kBanner} + "\nui_scale = auto\n") != std::string::npos);
            // ...and appending it a second time does not happen: feeding the OUTPUT
            // back in with the same map is a fixed point.
            const cfgrw::Result again = cfgrw::rewrite(r.text, kv, kBanner);
            CHECK_EQ(again.appended, 0);
            CHECK(again.text == r.text);
        }
        {
            // A comment line that looks like a key is never touched.
            std::vector<cfgrw::Pair> kv{{"a", "2"}};
            const cfgrw::Result r = cfgrw::rewrite("; a = 1\n#a = 1\n", kv, kBanner);
            CHECK_EQ(r.rewritten, 0);
            CHECK(r.text.find("; a = 1\n#a = 1\n") == 0);
        }
        {
            // A UTF-8 BOM belongs to the file, not to the first key.
            const std::string bom = "\xEF\xBB\xBFopacity = 0.92\n";
            std::vector<cfgrw::Pair> kv{{"opacity", "0.92"}};
            const cfgrw::Result r = cfgrw::rewrite(bom, kv, kBanner);
            CHECK(r.text == bom);
        }
        {
            // CRLF: the file's own line ending is what an appended key gets.
            std::vector<cfgrw::Pair> kv{{"a", "1"}, {"b", "2"}};
            const cfgrw::Result r = cfgrw::rewrite("a = 1\r\n", kv, kBanner);
            CHECK(r.text == std::string{"a = 1\r\n\r\n"} + kBanner + "\r\nb = 2\r\n");
        }
        {
            // Duplicate lines for one key: the loader lets the last win, so BOTH have
            // to be rewritten or a save would be undone by the earlier line.
            std::vector<cfgrw::Pair> kv{{"a", "9"}};
            const cfgrw::Result r = cfgrw::rewrite("a = 1\na = 2\n", kv, kBanner);
            CHECK_EQ(r.rewritten, 2);
            CHECK(r.text == "a = 9\na = 9\n");
        }
        {
            // filter() is what keeps Dev keys out of the player file.
            std::vector<cfgrw::Pair> kv{{"opacity", "1"}, {"srv_heap_size", "64"}};
            const std::vector<cfgrw::Pair> player = cfgrw::filter(
                kv, [](const std::string& k) { return !cfgkeys::tier_is(k, cfgkeys::Tier::Dev); });
            CHECK_EQ(static_cast<int>(player.size()), 1);
            if (!player.empty())
            {
                CHECK(player[0].first == "opacity");
            }
        }

        // ---- the real shipped files --------------------------------------------------
        for (const char* name : {"config_wuchang_minimap.txt", "config_wuchang_minimap_dev.txt"})
        {
            std::string text;
            if (!read_file(dir + name, text))
            {
                std::printf("  SKIPPED %s (run from the repo)\n", name);
                continue;
            }
            // Rewriting every key it carries with the value it already has must give
            // the file back unchanged - all ~470 lines of documentation included.
            std::vector<cfgrw::Pair> kv;
            for (const std::string& k : cfgkeys::keys_in(text))
            {
                // The value as the file spells it, found the same way the loader does.
                std::size_t at = 0;
                std::string value;
                while (at < text.size())
                {
                    const std::size_t nl = text.find('\n', at);
                    const std::string line = text.substr(at, (nl == std::string::npos ? text.size() : nl) - at);
                    at = (nl == std::string::npos) ? text.size() : nl + 1;
                    const std::size_t hash = line.find_first_of(";#");
                    const std::string body = line.substr(0, hash == std::string::npos ? line.size() : hash);
                    const std::size_t eq = body.find('=');
                    if (eq == std::string::npos)
                    {
                        continue;
                    }
                    std::string key = body.substr(0, eq);
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                    {
                        key.pop_back();
                    }
                    std::size_t ks = 0;
                    while (ks < key.size() && (key[ks] == ' ' || key[ks] == '\t'))
                    {
                        ++ks;
                    }
                    key = key.substr(ks);
                    if (key != k)
                    {
                        continue;
                    }
                    std::string v = body.substr(eq + 1);
                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
                    {
                        v.pop_back();
                    }
                    std::size_t vs = 0;
                    while (vs < v.size() && (v[vs] == ' ' || v[vs] == '\t'))
                    {
                        ++vs;
                    }
                    value = v.substr(vs);
                }
                kv.emplace_back(k, value);
            }
            const cfgrw::Result r = cfgrw::rewrite(text, kv, kBanner);
            const std::string msg = std::string{"round-trips byte for byte: "} + name;
            check(r.text == text, msg.c_str(), __FILE__, __LINE__);
            CHECK_EQ(r.changed, 0);
            CHECK_EQ(r.appended, 0);
            CHECK_EQ(r.rewritten, static_cast<int>(kv.size()));
            std::printf("  %s: %d key(s), %d bytes, byte-identical round trip\n", name,
                        static_cast<int>(kv.size()), static_cast<int>(text.size()));

            // And changing ONE value changes exactly the bytes of that value.
            if (!kv.empty())
            {
                std::vector<cfgrw::Pair> one = kv;
                one[0].second += "X";
                const cfgrw::Result r2 = cfgrw::rewrite(text, one, kBanner);
                CHECK_EQ(r2.changed, 1);
                CHECK_EQ(r2.text.size(), text.size() + 1);
            }
        }
    }

    //==================================================================================
    // Absence as evidence of a collect
    //==================================================================================
    //
    // The whole truth table, because this is the one auto-mark that fires on something
    // NOT being there - and lessons.md is unambiguous that absence is normally not
    // evidence at all. Every condition is checked on its own, in both directions, and
    // the debounce is walked round by round.

    //==================================================================================
    // Item quality ("rarity")
    //==================================================================================
    //
    // Three things can go wrong independently and each is cheap to pin here: the JSON
    // field is optional and must default to 0 (a marker file written before rarity
    // existed has to keep working), the palette parser has to survive whatever a player
    // types into the config, and the palette round-trip has to be exact or the F2
    // panel's Save silently rewrites the colours.

    void test_rarity()
    {
        section("item quality (rarity) tiers and palette");

        CHECK_STR(mdb::rarity_name(0), "Common");
        CHECK_STR(mdb::rarity_name(1), "Equipment");
        CHECK_STR(mdb::rarity_name(2), "Key");
        // Out of range must not read off the end of anything.
        CHECK_STR(mdb::rarity_name(-1), "Common");
        CHECK_STR(mdb::rarity_name(99), "Common");
        CHECK_EQ(mdb::rarity_clamp(-3), 0);
        CHECK_EQ(mdb::rarity_clamp(0), 0);
        CHECK_EQ(mdb::rarity_clamp(mdb::kRarityCount - 1), mdb::kRarityCount - 1);
        CHECK_EQ(mdb::rarity_clamp(mdb::kRarityCount), 0);
        CHECK_EQ(mdb::kRarityCount, 3);

        // ---- the JSON field ---------------------------------------------------------
        {
            std::vector<mdb::StaticMarker> db;
            mdb::ParseReport rep{};
            CHECK(mdb::parse_markers_json(
                R"({"schema":"wuchang-minimap-markers/1","chapter":1,"markers":[)"
                R"({"id":"a/1","cat":"pickup","x":1,"y":2,"z":3,"rarity":2},)"
                R"({"id":"a/2","cat":"pickup","x":1,"y":2,"z":3,"rarity":1},)"
                R"({"id":"a/3","cat":"pickup","x":1,"y":2,"z":3},)"
                R"({"id":"a/4","cat":"chest","x":1,"y":2,"z":3,"rarity":47},)"
                R"({"id":"a/5","cat":"pickup","x":1,"y":2,"z":3,"rarity":-2}]})",
                db, rep));
            CHECK_EQ(db.size(), 5);
            CHECK_EQ(db[0].rarity, 2);
            CHECK_EQ(db[1].rarity, 1);
            CHECK_EQ(db[2].rarity, 0); // absent == Common, so an old file still loads
            CHECK_EQ(db[3].rarity, 0); // out of range is clamped, never propagated
            CHECK_EQ(db[4].rarity, 0);
        }

        // ---- the palette parser -----------------------------------------------------
        const auto defaults = [](mdb::Rgb (&out)[mdb::kRarityCount]) {
            for (int i = 0; i < mdb::kRarityCount; ++i)
            {
                out[i] = mdb::kDefaultRarityColors[i];
            }
        };

        mdb::Rgb pal[mdb::kRarityCount]{};
        defaults(pal);
        std::string rejected;
        CHECK_EQ(mdb::parse_rarity_colors("112233, #445566, 789abc", pal, &rejected), 3);
        CHECK_STR(rejected, "");
        CHECK(pal[0] == (mdb::Rgb{0x11, 0x22, 0x33}));
        CHECK(pal[1] == (mdb::Rgb{0x44, 0x55, 0x66}));
        CHECK(pal[2] == (mdb::Rgb{0x78, 0x9A, 0xBC})); // lower case is accepted

        // The 3-digit CSS short form, and semicolon / whitespace separators.
        defaults(pal);
        CHECK_EQ(mdb::parse_rarity_colors("F00; 0f0\t00F", pal, nullptr), 3);
        CHECK(pal[0] == (mdb::Rgb{0xFF, 0x00, 0x00}));
        CHECK(pal[1] == (mdb::Rgb{0x00, 0xFF, 0x00}));
        CHECK(pal[2] == (mdb::Rgb{0x00, 0x00, 0xFF}));

        // A short list leaves the remaining tiers at whatever the caller seeded.
        defaults(pal);
        CHECK_EQ(mdb::parse_rarity_colors("000000", pal, nullptr), 1);
        CHECK(pal[0] == (mdb::Rgb{0, 0, 0}));
        CHECK(pal[1] == mdb::kDefaultRarityColors[1]);
        CHECK(pal[2] == mdb::kDefaultRarityColors[2]);

        // A bad entry is reported, keeps its own tier's old value, and - the point of
        // the rule - does NOT shift the later colours onto the wrong tiers.
        defaults(pal);
        rejected.clear();
        CHECK_EQ(mdb::parse_rarity_colors("112233, nope, 445566", pal, &rejected), 2);
        CHECK_STR(rejected, "nope");
        CHECK(pal[0] == (mdb::Rgb{0x11, 0x22, 0x33}));
        CHECK(pal[1] == mdb::kDefaultRarityColors[1]);
        CHECK(pal[2] == (mdb::Rgb{0x44, 0x55, 0x66}));

        // Empty text, and more entries than there are tiers, both change nothing beyond
        // what fits.
        defaults(pal);
        CHECK_EQ(mdb::parse_rarity_colors("", pal, nullptr), 0);
        CHECK(pal[1] == mdb::kDefaultRarityColors[1]);
        CHECK_EQ(mdb::parse_rarity_colors("000, 111, 222, 333, 444", pal, nullptr), 3);
        CHECK(pal[2] == (mdb::Rgb{0x22, 0x22, 0x22}));

        // Wrong lengths are rejected rather than half-read.
        defaults(pal);
        rejected.clear();
        CHECK_EQ(mdb::parse_rarity_colors("1234, 12345678, ABCDE", pal, &rejected), 0);
        CHECK_STR(rejected, "1234,12345678,ABCDE");
        for (int i = 0; i < mdb::kRarityCount; ++i)
        {
            CHECK(pal[i] == mdb::kDefaultRarityColors[i]);
        }

        // ---- round trip -------------------------------------------------------------
        defaults(pal);
        CHECK_STR(mdb::format_rarity_colors(pal), "ADAFDA, DAADC5, DAD6AD");
        mdb::Rgb back[mdb::kRarityCount]{};
        CHECK_EQ(mdb::parse_rarity_colors(mdb::format_rarity_colors(pal), back, nullptr),
                 mdb::kRarityCount);
        for (int i = 0; i < mdb::kRarityCount; ++i)
        {
            CHECK(back[i] == pal[i]);
        }

        // ---- the shipped default palette IS the game's own pickup-beam palette -------
        // DT_Particle LightColor of PickupEffect / PickupEffect4 / PickupEffect7,
        // linear -> sRGB. If these ever change, the config file's comment is wrong too.
        CHECK(mdb::kDefaultRarityColors[0] == (mdb::Rgb{0xAD, 0xAF, 0xDA}));
        CHECK(mdb::kDefaultRarityColors[1] == (mdb::Rgb{0xDA, 0xAD, 0xC5}));
        CHECK(mdb::kDefaultRarityColors[2] == (mdb::Rgb{0xDA, 0xD6, 0xAD}));
    }

    // The real database must actually carry tiers - a pipeline that silently stopped
    // emitting `rarity` would leave every x-ray label one flat colour and nothing else
    // would fail.
    void test_rarity_db(const std::string& markers_dir)
    {
        section("item quality in the generated database");
        std::string text;
        if (!read_file(markers_dir + "/chapter1.json", text))
        {
            std::printf("  SKIP  %s/chapter1.json does not exist yet\n", markers_dir.c_str());
            return;
        }
        std::vector<mdb::StaticMarker> db;
        mdb::ParseReport rep{};
        CHECK(mdb::parse_markers_json(text, db, rep));

        int per_tier[mdb::kRarityCount]{};
        int non_pickup_with_tier = 0;
        for (const mdb::StaticMarker& m : db)
        {
            CHECK(m.rarity < mdb::kRarityCount);
            per_tier[m.rarity] += 1;
            if (m.cat != mdb::Cat::Pickup && m.rarity != 0)
            {
                ++non_pickup_with_tier;
            }
        }
        // Only pickups have items, so only pickups may carry a tier.
        CHECK_EQ(non_pickup_with_tier, 0);
        // Both non-default tiers occur in chapter 1 (measured: 18 Equipment, 21 Key of its
        // 287 pickups). Exact counts would be brittle; "some of each" is the invariant.
        CHECK(per_tier[1] > 0);
        CHECK(per_tier[2] > 0);
        CHECK(per_tier[0] > per_tier[1] + per_tier[2]);
    }

    mdb::AbsenceFacts all_true()
    {
        mdb::AbsenceFacts f{};
        f.feature_on = true;
        f.cat_selected = true;
        f.already_found = false;
        f.level_known = true;
        f.full_round_since_level_load = true;
        f.twin_alive = false;
        return f;
    }

    void test_absence()
    {
        section("absence as evidence of a collect");

        // The one combination that confirms.
        CHECK(mdb::absence_round_confirms(all_true()));

        // Each condition alone is enough to refuse. The level tests are the safety
        // rails: an unmatched level, or a level that streamed in mid-round, must never
        // mark anything - that is exactly the "an unloaded level and a collected pickup
        // look identical" trap.
        {
            mdb::AbsenceFacts f = all_true();
            f.feature_on = false;
            CHECK(!mdb::absence_round_confirms(f));
        }
        {
            mdb::AbsenceFacts f = all_true();
            f.cat_selected = false;
            CHECK(!mdb::absence_round_confirms(f));
        }
        {
            mdb::AbsenceFacts f = all_true();
            f.already_found = true;
            CHECK(!mdb::absence_round_confirms(f));
        }
        {
            mdb::AbsenceFacts f = all_true();
            f.level_known = false;
            CHECK(!mdb::absence_round_confirms(f));
            CHECK(!mdb::absence_marks(f, 1000, 2)); // no streak can rescue it
        }
        {
            mdb::AbsenceFacts f = all_true();
            f.full_round_since_level_load = false;
            CHECK(!mdb::absence_round_confirms(f));
            CHECK(!mdb::absence_marks(f, 1000, 2));
        }
        {
            mdb::AbsenceFacts f = all_true();
            f.twin_alive = true;
            CHECK(!mdb::absence_round_confirms(f));
            CHECK(!mdb::absence_marks(f, 1000, 2));
        }

        // The debounce. `streak` counts the confirming rounds including this one.
        const mdb::AbsenceFacts ok = all_true();
        CHECK(!mdb::absence_marks(ok, 1, 2));
        CHECK(mdb::absence_marks(ok, 2, 2));
        CHECK(mdb::absence_marks(ok, 3, 2));
        CHECK(mdb::absence_marks(ok, 1, 1));
        CHECK(!mdb::absence_marks(ok, 4, 5));
        CHECK(mdb::absence_marks(ok, 5, 5));
        // A nonsensical requirement never marks (the config clamp keeps it >= 1, but
        // the predicate must not depend on that).
        CHECK(!mdb::absence_marks(ok, 1, 0));
        CHECK(!mdb::absence_marks(ok, 1, -3));

        // The runtime's own state machine, simulated: a marker whose level loads at
        // round 4, is unseen from round 5 on, and is marked on the second confirming
        // round - then a live twin appears and the streak has to reset.
        {
            const std::uint64_t level_round = 4;
            int streak = 0;
            int marks = 0;
            bool twin[] = {false, false, false, true, false, false, false};
            for (std::uint64_t round = 4; round <= 10; ++round)
            {
                mdb::AbsenceFacts f = all_true();
                f.full_round_since_level_load = round > level_round;
                f.twin_alive = twin[round - 4];
                if (!mdb::absence_round_confirms(f))
                {
                    streak = 0;
                    continue;
                }
                ++streak;
                if (mdb::absence_marks(f, streak, 2))
                {
                    ++marks;
                    streak = 0;
                }
            }
            // round 4: same round the level loaded -> no. 5: streak 1. 6: MARK.
            // 7: a twin is alive -> reset. 8: streak 1. 9: MARK. 10: streak 1.
            CHECK_EQ(marks, 2);
            CHECK_EQ(streak, 1);
        }

        // The join key the rule uses: the level short name out of a ULevel's full name,
        // which is the same helper the marker ids are built with.
        CHECK_STR(mdb::level_from_full_name(
                      "Level /Game/Maps/Chapter1/Chapter1_DGong_logic.Chapter1_DGong_logic:PersistentLevel"),
                  "Chapter1_DGong_logic");
    }

} // namespace

int main(int argc, char** argv)
{
    std::printf("WuchangMinimap - offline marker tests\n\n");

    test_loader();
    const std::string markers_dir = argc > 1 ? argv[1] : "markers";
    test_sample_file(markers_dir);
    test_real_db(markers_dir);
    test_categories();
    test_glyphs();
    test_found_file();
    test_ids();
    test_intern_levels();
    test_perf();
    test_mapview();
    test_scan_sched();
    test_sweep_sched();
    test_projection();
    test_compass();
    test_chapter_id();
    test_marker_chapter_filter();
    test_map_manifest(markers_dir);
    test_config_keys(markers_dir);
    test_config_rewrite(markers_dir);
    test_absence();
    test_rarity();
    test_rarity_db(markers_dir);

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
