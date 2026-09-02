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

#include "mapview.hpp"
#include "markers_db.hpp"

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

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
