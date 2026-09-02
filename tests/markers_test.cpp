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
#include <vector>

#include "chapterid.hpp"
#include "compass.hpp"
#include "mapmanifest.hpp"
#include "mapview.hpp"
#include "markers_db.hpp"
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

} // namespace

int main(int argc, char** argv)
{
    std::printf("WuchangMinimap - offline marker tests\n\n");

    test_loader();
    const std::string markers_dir = argc > 1 ? argv[1] : "markers";
    test_sample_file(markers_dir);
    test_real_db(markers_dir);
    test_categories();
    test_found_file();
    test_ids();
    test_mapview();
    test_scan_sched();
    test_projection();
    test_compass();
    test_chapter_id();
    test_marker_chapter_filter();
    test_map_manifest(markers_dir);

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
