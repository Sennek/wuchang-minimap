// markers_test - the offline half of the marker feature's verification: a console exe
// linking src/markers_db.cpp, no UE4SS or D3D. argv[1] = the repo's markers directory.

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <type_traits>

#include "chapterid.hpp"
#include "clipimg.hpp"
#include "config_keys.hpp"
#include "config_rewrite.hpp"
#include "compass.hpp"
#include "glyphs.hpp"
#include "label_layout.hpp"
#include "mapmanifest.hpp"
#include "mapdata.hpp"
#include "mapview.hpp"
#include "atomicfile.hpp"
#include "marker_dedupe.hpp"
// <Windows.h> defines the keyword macros `near` and `far` (WIN32_LEAN_AND_MEAN keeps
// them), and test_glyphs() has a local array called `near`.
#include "pngdecode.hpp"
#undef near
#undef far
#include "mmstate.hpp"
#include "markers_db.hpp"
#include "exchange.hpp"
#include "textmatch.hpp"
#include "typing_gate.hpp"
#include "perf.hpp"
#include "projection.hpp"
#include "saveslot.hpp"
#include "scan_sched.hpp"
#include "shrines_db.hpp"
#include "slicerule.hpp"
#include "spinlock.hpp"


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

    // A minimal but complete manifest: every id shape, an unknown category, a skipped entry.
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

    // `merchant` is the legacy name for `note`; such markers must land in `note`.
    const char* const kLegacyCatJson = R"JSON(
{
  "schema": "wuchang-minimap-markers/1",
  "chapter": 1,
  "markers": [
    {"id":"Chapter1_DGong_logic/DKDC_NPC_C_0","cat":"merchant","cls":"DKDC_NPC_C",
     "name":"Reading point","x":1,"y":2,"z":3,"level":"Chapter1_DGong_logic"},
    {"id":"Chapter1_DGong_logic/DKDC_NPC_C_1","cat":"note","cls":"DKDC_NPC_C",
     "name":"Reading point","x":4,"y":5,"z":6,"level":"Chapter1_DGong_logic"}
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
        CHECK_EQ(rep.legacy_cat, 0);
        CHECK_EQ(db.size(), 5);

        CHECK_STR(db[0].id, "digong01");
        CHECK(db[0].cat == mdb::Cat::Shrine);
        CHECK(db[0].x == 18214.0 && db[0].y == 5505.0 && db[0].z == -1579.0);
        CHECK_EQ(db[0].chapter, 1);
        CHECK(db[1].cat == mdb::Cat::Chest);
        CHECK(db[2].cat == mdb::Cat::Pickup);
        // An unknown category degrades to `other`, never drops the marker.
        CHECK(db[3].cat == mdb::Cat::Other);
        CHECK_EQ(db[4].chapter, 4);

        // Appending a second file must not clear the first.
        mdb::ParseReport rep2{};
        CHECK(mdb::parse_markers_json(kGoodJson, db, rep2));
        CHECK_EQ(db.size(), 10);

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

        // A "DLC" chapter loads as numeric bucket 0 with the label carried through.
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

        // A UTF-8 BOM must not make the first token unparseable.
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

    // The generated database, skipped when tools/markers has not produced it yet.
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
        // An unknown category name means the extractor and the runtime have drifted apart.
        CHECK_EQ(rep.unknown_cat, 0);
        CHECK(rep.added > 100);

        int per_cat[mdb::kCatCount]{};
        std::vector<std::string> ids;
        for (const mdb::StaticMarker& m : db)
        {
            CHECK(!m.id.empty());
            per_cat[static_cast<int>(m.cat)] += 1;
            ids.push_back(m.id);
            // A shrine's id is game-authored; every other is <level>/<object>, as GetFullName() gives.
            if (m.cat == mdb::Cat::Shrine)
            {
                CHECK(m.id.find('/') == std::string::npos);
            }
            else
            {
                CHECK(m.id.find('/') != std::string::npos);
            }
        }
        // Ids are the join key with the live actors; a duplicate is unreachable.
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

        // The runtime enumerates the markers directory, so every one of these is loaded in-game.
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

    // North-up: world X maps to the canvas HEIGHT, world Y to its WIDTH.
    void test_fit_zoom()
    {
        section("full map - zoom to fit");

        // A 16:9 canvas and a square world: the fit is decided by the SHORT side.
        const double z = mv::fit_zoom(1000.0, 1000.0, 1600.0, 900.0, 0.0);
        CHECK(std::abs(z - 1000.0 / 900.0) < 1e-9);
        CHECK(std::abs(mv::fit_zoom(100.0, 3200.0, 1600.0, 900.0, 0.0) - 2.0) < 1e-9);
        CHECK(std::abs(mv::fit_zoom(1800.0, 100.0, 1600.0, 900.0, 0.0) - 2.0) < 1e-9);
        // The fitted rectangle really does fit, both ways round, with the margin on.
        {
            const double sx = 45000.0;
            const double sy = 30000.0;
            const double cw = 1200.0;
            const double chh = 800.0;
            const double zz = mv::fit_zoom(sx, sy, cw, chh);
            CHECK(zz > 0.0);
            CHECK(sx / zz <= chh + 1e-6); // north-south inside the canvas height
            CHECK(sy / zz <= cw + 1e-6);  // east-west inside its width
            CHECK(sx / zz < chh || sy / zz < cw);
        }
        // Degenerate input leaves the caller's zoom alone rather than dividing by zero.
        CHECK(mv::fit_zoom(0.0, 1000.0, 800.0, 600.0) == 0.0);
        CHECK(mv::fit_zoom(1000.0, 0.0, 800.0, 600.0) == 0.0);
        CHECK(mv::fit_zoom(1000.0, 1000.0, 0.0, 600.0) == 0.0);
        CHECK(mv::fit_zoom(1000.0, 1000.0, 800.0, 1.0) == 0.0);
        CHECK(mv::fit_zoom(-5.0, 1000.0, 800.0, 600.0) == 0.0);

        // Chapter 1 is ~450 x 300 m; a 1080p fit lands inside the shipped map_zoom_min/max (4..240).
        {
            const double zz = mv::fit_zoom(45000.0, 30000.0, 1700.0, 900.0);
            CHECK(zz > 4.0 && zz < 240.0);
            CHECK(mv::clamp_zoom(zz, 4.0, 240.0) == zz);
        }
    }

    // X-ray label layout (src/label_layout.hpp)
    // Invariant: no two placed labels overlap.
    void test_label_layout()
    {
        section("x-ray label layout");

        // Rectangles that merely touch are not a clash.
        const lbl::Rect a{0.0f, 0.0f, 10.0f, 10.0f};
        const lbl::Rect touching{10.0f, 0.0f, 20.0f, 10.0f};
        const lbl::Rect over{9.0f, 9.0f, 20.0f, 20.0f};
        CHECK(!a.intersects(touching));
        CHECK(a.intersects(over));
        CHECK(over.intersects(a)); // symmetric

        lbl::Layout l{};
        float y = 0.0f;

        CHECK(l.place(100.0f, 200.0f, 80.0f, 14.0f, 200.0f, y));
        CHECK(y == 200.0f);
        CHECK_EQ(l.rect_count, 1);

        // A second label at the same anchor is pushed BELOW the first, not on top of it.
        CHECK(l.place(100.0f, 200.0f, 80.0f, 14.0f, 200.0f, y));
        CHECK(y > 214.0f);
        const float second = y;
        CHECK(l.place(100.0f, 200.0f, 80.0f, 14.0f, 200.0f, y));
        CHECK(y > second);

        // A label whose x does not overlap is NOT pushed: the test is a rectangle intersection.
        CHECK(l.place(400.0f, 200.0f, 80.0f, 14.0f, 200.0f, y));
        CHECK(y == 200.0f);

        // Over a pile of anchors, nothing placed overlaps anything else placed.
        {
            lbl::Layout p{};
            int placed = 0;
            for (int i = 0; i < 30; ++i)
            {
                float at = 0.0f;
                const float ax = 500.0f + static_cast<float>(i % 3);
                const float ay = 300.0f + static_cast<float>(i % 5);
                if (p.place(ax, ay, 120.0f, 15.0f, 400.0f, at))
                {
                    ++placed;
                }
            }
            CHECK(placed > 0);
            CHECK_EQ(p.rect_count, placed);
            for (int i = 0; i < p.rect_count; ++i)
            {
                for (int j = i + 1; j < p.rect_count; ++j)
                {
                    CHECK(!p.rects[i].intersects(p.rects[j]));
                }
            }
        }

        // A label further than max_push is refused, and refusing records nothing.
        {
            lbl::Layout p{};
            float at = 0.0f;
            CHECK(p.place(0.0f, 0.0f, 50.0f, 20.0f, 0.0f, at));
            const int before = p.rect_count;
            CHECK(!p.place(0.0f, 0.0f, 50.0f, 20.0f, 5.0f, at)); // needs 21 px, allowed 5
            CHECK_EQ(p.rect_count, before);
            CHECK(p.place(0.0f, 0.0f, 50.0f, 20.0f, 50.0f, at)); // allowed 50 - fits
        }

        // Once kMaxRects labels are placed the answer is "no", for ever.
        {
            lbl::Layout p{};
            float at = 0.0f;
            for (int i = 0; i < lbl::Layout::kMaxRects; ++i)
            {
                CHECK(p.place(0.0f, static_cast<float>(i) * 20.0f, 40.0f, 15.0f, 10.0f, at));
            }
            CHECK(p.full());
            CHECK(!p.place(2000.0f, 2000.0f, 40.0f, 15.0f, 100.0f, at));
        }

        // Degenerate sizes are refused, not recorded as zero-area rectangles.
        {
            lbl::Layout p{};
            float at = 0.0f;
            CHECK(!p.place(0.0f, 0.0f, 0.0f, 15.0f, 50.0f, at));
            CHECK(!p.place(0.0f, 0.0f, 40.0f, 0.0f, 50.0f, at));
            CHECK_EQ(p.rect_count, 0);
        }

        // Never a label for a glyph within r * 2 of one that already has one.
        {
            lbl::Layout p{};
            CHECK(!p.near_labelled(100.0f, 100.0f, 20.0f));
            p.note_glyph(100.0f, 100.0f);
            CHECK(p.near_labelled(105.0f, 100.0f, 20.0f));
            CHECK(p.near_labelled(100.0f, 119.0f, 20.0f));
            CHECK(!p.near_labelled(130.0f, 100.0f, 20.0f));
            CHECK(!p.near_labelled(100.0f, 100.0f, 0.0f)); // a zero radius disables it
            CHECK_EQ(p.glyph_count, 1);
            // reset() clears both lists; it runs once per frame.
            float at = 0.0f;
            CHECK(p.place(0.0f, 0.0f, 10.0f, 10.0f, 10.0f, at));
            p.reset();
            CHECK_EQ(p.rect_count, 0);
            CHECK_EQ(p.glyph_count, 0);
            CHECK(!p.near_labelled(100.0f, 100.0f, 20.0f));
        }
    }

    void test_zoom_presets()
    {
        section("minimap zoom presets");

        float p[mv::kMaxZoomPresets]{};
        std::string rejected;
        CHECK_EQ(mv::parse_zoom_presets("13, 26, 52", p, &rejected), 3);
        CHECK(rejected.empty());
        CHECK(p[0] == 13.0f && p[1] == 26.0f && p[2] == 52.0f);

        rejected.clear();
        CHECK_EQ(mv::parse_zoom_presets("52;13 26  26", p, &rejected), 3);
        CHECK(p[0] == 13.0f && p[1] == 26.0f && p[2] == 52.0f);
        CHECK(rejected.empty());

        // Out-of-range and unparseable tokens are named, not dropped, and do NOT consume a slot.
        rejected.clear();
        CHECK_EQ(mv::parse_zoom_presets("1, 26, 900, wide", p, &rejected), 1);
        CHECK(p[0] == 26.0f);
        CHECK(rejected == "1, 900, wide");

        float keep[mv::kMaxZoomPresets] = {13.0f, 26.0f, 52.0f};
        CHECK_EQ(mv::parse_zoom_presets("nonsense", keep, nullptr), 0);
        CHECK(keep[0] == 13.0f && keep[1] == 26.0f && keep[2] == 52.0f);
        CHECK_EQ(mv::parse_zoom_presets("", keep, nullptr), 0);

        CHECK_EQ(mv::parse_zoom_presets("2,3,4,5,6,7,8,9,10,11,12", p, nullptr), mv::kMaxZoomPresets);

        const float ladder[3] = {13.0f, 26.0f, 52.0f};
        CHECK(mv::next_zoom_preset(ladder, 3, 13.0f) == 26.0f);
        CHECK(mv::next_zoom_preset(ladder, 3, 26.0f) == 52.0f);
        CHECK(mv::next_zoom_preset(ladder, 3, 52.0f) == 13.0f); // wraps
        // A zoom off the ladder lands on the next rung above; above the top it wraps.
        CHECK(mv::next_zoom_preset(ladder, 3, 20.0f) == 26.0f);
        CHECK(mv::next_zoom_preset(ladder, 3, 400.0f) == 13.0f);
        CHECK(mv::step_zoom_preset(ladder, 3, 26.0f, -1) == 13.0f);
        CHECK(mv::step_zoom_preset(ladder, 3, 13.0f, -1) == 52.0f); // wraps the other way
        CHECK(mv::step_zoom_preset(ladder, 3, 30.0f, -1) == 26.0f);
        CHECK(mv::step_zoom_preset(ladder, 0, 26.0f, 1) == 26.0f);
        CHECK(mv::step_zoom_preset(nullptr, 3, 26.0f, 1) == 26.0f);
        CHECK(mv::step_zoom_preset(ladder, 3, 26.0f, 0) == 26.0f);
        // Every rung is reachable: three presses of the key from any rung come back.
        float z = 26.0f;
        for (int i = 0; i < 3; ++i)
        {
            z = mv::next_zoom_preset(ladder, 3, z);
        }
        CHECK(z == 26.0f);
    }

    void test_glyphs()
    {
        section("glyph shapes and marker palettes");

        // Every category has its OWN shape: shape survives dimming, 6 px and a recoloured palette.
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

        // An out-of-range category byte must never index off the end of the table.
        CHECK(gly::shape_of(static_cast<mdb::Cat>(mdb::kCatCount)) == gly::Shape::SmallSquare);
        CHECK(gly::shape_of(static_cast<mdb::Cat>(200)) == gly::Shape::SmallSquare);

        const gly::Palette palettes[] = {gly::Palette::Default, gly::Palette::Colorblind};
        for (const gly::Palette pal : palettes)
        {
            // Two categories may share a hue or a shape, never both.
            CHECK(gly::palette_is_separable(pal));

            // All shapes are distinct, so the pairs a player compares in one glance (listed as data
            // in glyphs.hpp) must differ by HUE.
            CHECK(gly::palette_competing_hues_ok(pal));

            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Rgb c = gly::marker_rgb(static_cast<mdb::Cat>(i), pal);
                // Nothing is drawn in near-black: the minimap backdrop is (6, 9, 13), the full map darker.
                CHECK(static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b) > 150);
            }
            CHECK(gly::marker_rgb(static_cast<mdb::Cat>(200), pal) ==
                  gly::marker_rgb(mdb::Cat::Other, pal));
        }

        CHECK(gly::marker_rgb(mdb::Cat::Ladder, gly::Palette::Default) ==
              gly::marker_rgb(mdb::Cat::Lift, gly::Palette::Default));
        CHECK(gly::shape_of(mdb::Cat::Ladder) != gly::shape_of(mdb::Cat::Lift));
        CHECK(gly::shape_of(mdb::Cat::Boss) != gly::shape_of(mdb::Cat::Elite));
        CHECK(gly::shape_of(mdb::Cat::Elite) != gly::shape_of(mdb::Cat::Enemy));
        CHECK(gly::shape_of(mdb::Cat::Hidden) != gly::shape_of(mdb::Cat::Shrine));
        CHECK(gly::marker_rgb(mdb::Cat::Boss, gly::Palette::Colorblind) !=
              gly::marker_rgb(mdb::Cat::Enemy, gly::Palette::Colorblind));

        // A note has its OWN hue and shape in both palettes, distinct from its neighbours.
        for (const gly::Palette pal : palettes)
        {
            const mdb::Cat near[] = {mdb::Cat::Npc, mdb::Cat::Door, mdb::Cat::Chest,
                                     mdb::Cat::Pickup, mdb::Cat::Ladder, mdb::Cat::Other};
            for (const mdb::Cat other : near)
            {
                CHECK(!(gly::marker_rgb(mdb::Cat::Note, pal) == gly::marker_rgb(other, pal)));
                CHECK(gly::shape_of(mdb::Cat::Note) != gly::shape_of(other));
            }
        }
        CHECK(gly::shape_of(mdb::Cat::Note) == gly::Shape::NotePage);

        // Both values come from a hand-edited file: parsing is forgiving of case, spaces and the
        // British spelling, and keeps the caller's value when it recognises neither.
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

        // `neutral` is pinned exactly: a change alters the look of every existing config.
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

        // The default set is the game's own pickup-beam palette; the colour-blind set is
        // three genuinely different colours.
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

    void test_legacy_manifest_category()
    {
        section("a manifest with a renamed category");

        std::vector<mdb::StaticMarker> db;
        mdb::ParseReport rep{};
        CHECK(mdb::parse_markers_json(kLegacyCatJson, db, rep));
        CHECK_EQ(rep.added, 2);
        CHECK_EQ(rep.unknown_cat, 0); // NOT dumped into `other`
        CHECK_EQ(rep.legacy_cat, 1);  // counted, so the loader can say so once
        CHECK_EQ(db.size(), 2);
        CHECK(db[0].cat == mdb::Cat::Note);
        CHECK(db[1].cat == mdb::Cat::Note);
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
        CHECK(mdb::cat_from_name("  SHRINE ", unused) && unused == mdb::Cat::Shrine);

        CHECK_EQ(mdb::parse_category_mask("all", 0u), mdb::kAllCats);
        CHECK_EQ(mdb::parse_category_mask("none", mdb::kAllCats), 0u);
        CHECK_EQ(mdb::parse_category_mask("shrine,chest", 0u),
                 mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest));
        CHECK_EQ(mdb::parse_category_mask("shrine chest;pickup", 0u),
                 mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup));

        std::string rejected;
        CHECK_EQ(mdb::parse_category_mask("shrine,wombat,chest", 0u, &rejected),
                 mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest));
        CHECK_STR(rejected, "wombat");

        // A value that names nothing usable keeps the previous setting.
        CHECK_EQ(mdb::parse_category_mask("", mdb::kAllCats), mdb::kAllCats);
        CHECK_EQ(mdb::parse_category_mask("wombat,badger", mdb::kAllCats, &rejected), mdb::kAllCats);
        CHECK_STR(rejected, "wombat,badger");

        // `merchant` is the legacy name for `note`: it sets Note's bit and is reported in `legacy`.
        CHECK(!mdb::cat_from_name("merchant", unused)); // not a current name
        mdb::Cat legacy_cat = mdb::Cat::Other;
        CHECK(mdb::cat_from_legacy_name("merchant", legacy_cat));
        CHECK(legacy_cat == mdb::Cat::Note);
        CHECK(mdb::cat_from_legacy_name("  MERCHANT ", legacy_cat) && legacy_cat == mdb::Cat::Note);
        CHECK(!mdb::cat_from_legacy_name("note", legacy_cat));  // current names are not aliases
        CHECK(!mdb::cat_from_legacy_name("wombat", legacy_cat));

        std::string legacy;
        CHECK_EQ(mdb::parse_category_mask("chest,pickup,shrine,boss,npc,merchant", 0u, &rejected, &legacy),
                 mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup) |
                     mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Boss) |
                     mdb::cat_bit(mdb::Cat::Npc) | mdb::cat_bit(mdb::Cat::Note));
        CHECK_STR(legacy, "merchant");
        CHECK_STR(rejected, "");
        // A legacy-only value is a usable value, so it must NOT fall back.
        CHECK_EQ(mdb::parse_category_mask("merchant", mdb::kAllCats, &rejected, &legacy),
                 mdb::cat_bit(mdb::Cat::Note));
        CHECK_STR(legacy, "merchant");
        CHECK_EQ(mdb::parse_category_mask("merchant,wombat", 0u, &rejected, &legacy),
                 mdb::cat_bit(mdb::Cat::Note));
        CHECK_STR(legacy, "merchant");
        CHECK_STR(rejected, "wombat");
        // Save writes back the CURRENT name, so the warning clears itself.
        CHECK_STR(mdb::format_category_mask(mdb::cat_bit(mdb::Cat::Note)), "note");

        CHECK_STR(mdb::format_category_mask(mdb::kAllCats), "all");
        CHECK_STR(mdb::format_category_mask(0u), "none");
        CHECK_STR(mdb::format_category_mask(mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Chest)),
                  "shrine,chest");

        // format -> parse -> format is the config file's save/load path: exact for every mask.
        for (std::uint32_t mask = 0; mask <= mdb::kAllCats; mask += 37u)
        {
            const std::string text = mdb::format_category_mask(mask);
            CHECK_EQ(mdb::parse_category_mask(text, ~mask & mdb::kAllCats), mask);
        }
        const std::uint32_t shipped = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        CHECK_EQ(mdb::parse_category_mask(mdb::format_category_mask(shipped), 0u), shipped);
    }

    void test_schema_gate()
    {
        section("manifest schema gate (major 1 only)");

        // Only major 1 passes, with or without a minor.
        const struct
        {
            const char* schema;
            bool want_ok;
        } cases[] = {
            {"wuchang-minimap-markers/1", true},
            {"wuchang-minimap-markers/1.0", true},
            {"wuchang-minimap-markers/1.7", true},
            {"wuchang-minimap-markers/10", false},
            {"wuchang-minimap-markers/11.0", false},
            {"wuchang-minimap-markers/2", false},
            {"wuchang-minimap-markers/", false},
            {"wuchang-minimap-markers", false},
            {"wuchang-minimap-items/1", false},
            {"", false},
        };
        for (const auto& c : cases)
        {
            const std::string text = std::string("{\"schema\":\"") + c.schema +
                                     "\",\"chapter\":1,\"markers\":[{\"id\":\"a/b\",\"cat\":\"chest\","
                                     "\"x\":1,\"y\":2,\"z\":3}]}";
            std::vector<mdb::StaticMarker> out;
            mdb::ParseReport r{};
            const bool ok = mdb::parse_markers_json(text, out, r);
            ++g_checks;
            if (ok != c.want_ok || (!ok && !out.empty()))
            {
                ++g_failures;
                std::printf("  FAIL  schema \"%s\": got %s, want %s\n", c.schema, ok ? "accepted" : "rejected",
                            c.want_ok ? "accepted" : "rejected");
            }
            CHECK_STR(r.schema, c.schema);
        }
    }

    mdb::StaticMarker marker(const char* id, double x)
    {
        mdb::StaticMarker m{};
        m.id = id;
        m.x = x;
        m.cat = mdb::Cat::Chest;
        return m;
    }

    void test_dedupe()
    {
        section("duplicate marker ids are dropped at load");

        std::vector<mdb::StaticMarker> markers;
        markers.push_back(marker("a", 1));
        markers.push_back(marker("b", 2));
        markers.push_back(marker("a", 3)); // duplicate of index 0
        markers.push_back(marker("c", 4));
        markers.push_back(marker("b", 5)); // duplicate of index 1
        markers.push_back(marker("a", 6)); // duplicate of index 0 again

        std::unordered_map<std::string, int> by_id;
        std::vector<mdb::DupDrop> drops;
        CHECK_EQ(mdb::dedupe_by_id(markers, by_id, drops), 3);

        // What is left is the FIRST copy of each id, in order, and nothing else.
        CHECK_EQ(markers.size(), 3);
        CHECK_STR(markers[0].id, "a");
        CHECK_STR(markers[1].id, "b");
        CHECK_STR(markers[2].id, "c");
        CHECK(markers[0].x == 1.0 && markers[1].x == 2.0 && markers[2].x == 4.0);

        // by_id indexes the SHRUNK vector: that is the map note_found() resolves through.
        CHECK_EQ(by_id.size(), 3);
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            const auto it = by_id.find(markers[i].id);
            ++g_checks;
            if (it == by_id.end() || it->second != static_cast<int>(i))
            {
                ++g_failures;
                std::printf("  FAIL  by_id[%s] does not point at index %d\n", markers[i].id.c_str(),
                            static_cast<int>(i));
            }
        }

        CHECK_EQ(drops.size(), 3);
        CHECK_EQ(drops[0].dropped, 2);
        CHECK_EQ(drops[0].kept, 0);
        CHECK_STR(drops[0].id, "a");
        CHECK_EQ(drops[1].dropped, 4);
        CHECK_EQ(drops[1].kept, 1);
        CHECK_EQ(drops[2].dropped, 5);
        CHECK_EQ(drops[2].kept, 0);

        std::vector<mdb::StaticMarker> clean;
        clean.push_back(marker("x", 1));
        clean.push_back(marker("y", 2));
        CHECK_EQ(mdb::dedupe_by_id(clean, by_id, drops), 0);
        CHECK_EQ(clean.size(), 2);
        CHECK_EQ(drops.size(), 0);
        CHECK_STR(clean[0].id, "x");
        CHECK_STR(clean[1].id, "y");
        CHECK(clean[1].x == 2.0); // the entries were not left moved-from
    }

    void test_atomic_write()
    {
        section("crash-safe file writes (temp file + rename)");

        wchar_t dir[MAX_PATH]{};
        const DWORD n = ::GetTempPathW(MAX_PATH, dir);
        ++g_checks;
        if (n == 0 || n >= MAX_PATH)
        {
            ++g_failures;
            std::printf("  FAIL  GetTempPathW\n");
            return;
        }
        const std::wstring path = std::wstring(dir) + L"wuchang_minimap_test_atomic.txt";
        ::DeleteFileW(path.c_str());
        ::DeleteFileW(mmfile::tmp_path(path).c_str());
        ::DeleteFileW(mmfile::bak_path(path).c_str());

        CHECK(mmfile::tmp_path(path) == path + L".tmp");
        CHECK(mmfile::bak_path(path) == path + L".bak");

        // A missing file is NotFound, not Failed; the found tracker depends on the distinction.
        std::string text;
        CHECK(mmfile::read_whole_file(path, text, 1 << 20).status == mmfile::ReadStatus::NotFound);

        unsigned err = 123;
        CHECK(mmfile::write_whole_file_atomic(path, "first generation\n", true, &err));
        CHECK_EQ(err, 0);
        // The temp file is gone: the rename IS the commit.
        CHECK(::GetFileAttributesW(mmfile::tmp_path(path).c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK(::GetFileAttributesW(mmfile::bak_path(path).c_str()) == INVALID_FILE_ATTRIBUTES);
        mmfile::ReadInfo info = mmfile::read_whole_file(path, text, 1 << 20);
        CHECK(info.status == mmfile::ReadStatus::Ok);
        CHECK_STR(text, "first generation\n");

        // The second write keeps the previous generation as .bak.
        CHECK(mmfile::write_whole_file_atomic(path, "second generation\n", true, &err));
        info = mmfile::read_whole_file(path, text, 1 << 20);
        CHECK(info.status == mmfile::ReadStatus::Ok);
        CHECK_STR(text, "second generation\n");
        info = mmfile::read_whole_file(mmfile::bak_path(path), text, 1 << 20);
        CHECK(info.status == mmfile::ReadStatus::Ok);
        CHECK_STR(text, "first generation\n");

        // keep_backup = false leaves the old .bak alone (config / waypoint writes).
        CHECK(mmfile::write_whole_file_atomic(path, "third\n", false, &err));
        info = mmfile::read_whole_file(mmfile::bak_path(path), text, 1 << 20);
        CHECK(info.status == mmfile::ReadStatus::Ok);
        CHECK_STR(text, "first generation\n");

        // A shorter write leaves no tail of the longer one behind.
        info = mmfile::read_whole_file(path, text, 1 << 20);
        CHECK_STR(text, "third\n");

        CHECK(mmfile::write_whole_file_atomic(path, std::string(4096, 'x'), false, &err));
        info = mmfile::read_whole_file(path, text, 1024);
        CHECK(info.too_big);
        CHECK(info.status == mmfile::ReadStatus::Failed);
        CHECK_EQ(info.size, 4096);
        CHECK(text.empty());

        CHECK(mmfile::write_whole_file_atomic(path, "", false, &err));
        info = mmfile::read_whole_file(path, text, 1 << 20);
        CHECK(info.status == mmfile::ReadStatus::Ok);
        CHECK_EQ(info.size, 0);

        const std::wstring bad = std::wstring(dir) + L"no_such_dir_wuchang\\deeper\\x.txt";
        err = 0;
        CHECK(!mmfile::write_whole_file_atomic(bad, "nope", false, &err));
        CHECK(err != 0);

        ::DeleteFileW(path.c_str());
        ::DeleteFileW(mmfile::bak_path(path).c_str());
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

        // serialize -> parse -> serialize runs on every auto-mark; it has to be a fixed point.
        const std::string once = mdb::found_serialize(ids);
        std::vector<std::string> back;
        mdb::found_parse(once, back);
        CHECK_EQ(back.size(), 3);
        const std::string twice = mdb::found_serialize(back);
        CHECK_STR(twice, once);

        CHECK(once.find("Chapter1_DGong_logic/BP_ItemRedBox_C_0") < once.find("digong01"));

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

    // Level-name interning (mdb::lower_ascii / mdb::intern_levels)
    // The join is case-insensitive: the marker DB and GetFullName() need not agree on case.

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

        // Every id is a valid index into `levels` or -1, and names the marker's own level.
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

        std::vector<mdb::StaticMarker> none;
        mdb::intern_levels(none, levels, marker_level);
        CHECK(levels.empty());
        CHECK(marker_level.empty());
    }

    // The per-activity performance counters (src/perf.hpp)

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
        CHECK(perf::register_counter(t, "publish_round", perf::Thread::Game) == 0);
        CHECK(t.count == 2);
        CHECK(perf::register_counter(t, nullptr, perf::Thread::Loop) == -1);

        // An id that was never handed out is a no-op, not a write past the array.
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
        CHECK(t.c[a].win_calls == 0);
        CHECK_NEAR(t.c[a].win_total_ms, 0.0, 1e-12);

        // A clock that goes backwards (a suspend) must not wedge the window or give a negative rate.
        perf::record(t, a, 1.0, 10);
        CHECK(t.c[a].calls == 5);
        CHECK(t.c[a].rate_hz >= 0.0);

        // Idle detection: never recorded is idle, just recorded is not, a long-open window is idle.
        perf::Table q{};
        const int c = perf::register_counter(q, "widget sweep", perf::Thread::Game);
        CHECK(perf::idle(q.c[c], 0));
        perf::record(q, c, 30.0, 5000);
        CHECK(!perf::idle(q.c[c], 5000));
        CHECK(!perf::idle(q.c[c], 5000 + perf::kWindowMs * 2));
        CHECK(perf::idle(q.c[c], 5000 + perf::kWindowMs * 4));

        CHECK_NEAR(q.c[c].peak_ms, 30.0, 1e-12);
        perf::reset_peaks(q);
        CHECK_NEAR(q.c[c].peak_ms, 0.0, 1e-12);
        CHECK(q.c[c].calls == 1);
        CHECK_NEAR(q.c[c].last_ms, 30.0, 1e-12);

        // A stalled sample shows in peak_ms and last_ms, never in peak_calm_ms (the F2 number).
        perf::Table r{};
        const int d = perf::register_counter(r, "render frame", perf::Thread::Render);
        perf::record(r, d, 1.5, 1000);              // calm by default
        CHECK_NEAR(r.c[d].peak_calm_ms, 1.5, 1e-12);
        CHECK(r.c[d].stalls == 0);
        perf::record(r, d, 358.0, 1100, false);     // a loading screen
        CHECK_NEAR(r.c[d].peak_ms, 358.0, 1e-12);   // the raw peak sees it
        CHECK_NEAR(r.c[d].peak_calm_ms, 1.5, 1e-12); // the shown peak does not
        CHECK(r.c[d].stalls == 1);
        CHECK_NEAR(r.c[d].peak_stall_ms, 358.0, 1e-12);
        CHECK_NEAR(r.c[d].last_ms, 358.0, 1e-12);
        CHECK(r.c[d].calls == 2);
        // A stalled sample is still part of the window average: it happened.
        perf::record(r, d, 2.5, 1200);
        CHECK_NEAR(r.c[d].peak_calm_ms, 2.5, 1e-12);
        perf::record(r, d, 0.5, 4000);              // closes the window
        CHECK_NEAR(r.c[d].avg_ms, (1.5 + 358.0 + 2.5 + 0.5) / 4.0, 1e-9);
        perf::reset_peaks(r);
        CHECK_NEAR(r.c[d].peak_ms, 0.0, 1e-12);
        CHECK_NEAR(r.c[d].peak_calm_ms, 0.0, 1e-12);
        CHECK_NEAR(r.c[d].peak_stall_ms, 0.0, 1e-12);
        CHECK(r.c[d].stalls == 0);

        // The table is a fixed array: registering past it is refused, never written.
        perf::Table full{};
        // Distinct NAMES: the name check compares content, not the pointer.
        static char storage[perf::kMaxCounters + 4][4] = {};
        for (int i = 0; i < perf::kMaxCounters + 4; ++i)
        {
            storage[i][0] = 'c';
            storage[i][1] = static_cast<char>('0' + i / 10);
            storage[i][2] = static_cast<char>('0' + i % 10);
        }
        for (int i = 0; i < perf::kMaxCounters; ++i)
        {
            CHECK(perf::register_counter(full, storage[i], perf::Thread::Loop) == i);
        }
        // The same TEXT is row 0 even with the table full: the name lookup runs before the cap.
        static char alias[4] = {'c', '0', '0', 0};
        CHECK(perf::register_counter(full, alias, perf::Thread::Loop) == 0);
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

        // The loader's ids must equal what the runtime builds for the same actors.
        CHECK_STR(mdb::stable_id(mdb::level_from_full_name(
                                     "BP_ItemRedBox_C /Game/Maps/Chapter1/Chapter1_DGong_logic."
                                     "Chapter1_DGong_logic:PersistentLevel.BP_ItemRedBox_C_0"),
                                 "BP_ItemRedBox_C_0"),
                  "Chapter1_DGong_logic/BP_ItemRedBox_C_0");
    }
    // The full map: viewport math, zoom, waypoint file

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

        float sx = 0.0f;
        float sy = 0.0f;
        mv::world_to_screen(v, r, v.cx, v.cy, sx, sy);
        CHECK_NEAR(sx, 550.0, 1e-3);
        CHECK_NEAR(sy, 350.0, 1e-3);

        // NORTH IS UP and EAST IS RIGHT, the same convention as build_map.py and the minimap.
        mv::world_to_screen(v, r, v.cx + 400.0, v.cy, sx, sy); // 400 uu north
        CHECK_NEAR(sx, 550.0, 1e-3);
        CHECK_NEAR(sy, 350.0 - 10.0, 1e-3); // 400 / 40 = 10 px UP
        mv::world_to_screen(v, r, v.cx, v.cy + 800.0, sx, sy); // 800 uu east
        CHECK_NEAR(sx, 550.0 + 20.0, 1e-3); // 20 px RIGHT
        CHECK_NEAR(sy, 350.0, 1e-3);

        // The inverse is exact over the whole viewport, at several zooms.
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
        // The limits are accepted in either order; nonsense falls back to the low limit.
        CHECK_NEAR(mv::clamp_zoom(50.0, 100.0, 10.0), 50.0, 1e-9);
        CHECK_NEAR(mv::clamp_zoom(0.0, 10.0, 100.0), 10.0, 1e-9);
        CHECK_NEAR(mv::clamp_zoom(-3.0, 10.0, 100.0), 10.0, 1e-9);

        // Positive notches zoom IN (fewer uu per pixel); a whole notch is exactly the factor.
        CHECK_NEAR(mv::zoom_by(100.0, 1.0, 1.25, 1.0, 1000.0), 80.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, -1.0, 1.25, 1.0, 1000.0), 125.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, 2.0, 1.25, 1.0, 1000.0), 64.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, 50.0, 1.25, 6.0, 900.0), 6.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, -50.0, 1.25, 6.0, 900.0), 900.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, 3.0, 1.0, 6.0, 900.0), 100.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(100.0, 0.0, 1.25, 6.0, 900.0), 100.0, 1e-9);
        CHECK_NEAR(mv::zoom_by(2.0, 0.0, 1.25, 6.0, 900.0), 6.0, 1e-9);

        section("full map - the waypoint file");

        // The format 0.9.x wrote, which is still read. Written out here rather than
        // produced by a serializer: nothing in the mod writes it any more.
        mv::Waypoint back{};
        CHECK(mv::waypoint_parse("set = 1\nx = 18176.671875\ny = -13905.2109375\nz = -7641.22\n", back));
        CHECK(back.set);
        // EXACT: the file was written at 17 significant digits.
        CHECK(back.x == 18176.671875);
        CHECK(back.y == -13905.2109375);
        CHECK(back.z == -7641.22);

        mv::Waypoint none_back{};
        none_back.set = true;
        CHECK(mv::waypoint_parse("set = 0\nx = 0\ny = 0\nz = 0\n", none_back));
        CHECK(!none_back.set);

        // Hand-written files: a BOM, CRLF, comments, spacing, and no `set` line at all.
        mv::Waypoint hand{};
        CHECK(mv::waypoint_parse("\xEF\xBB\xBF; mine\r\n  x =  100.5 \r\ny=-200\r\n; z is optional\r\n",
                                 hand));
        CHECK(hand.set);
        CHECK_NEAR(hand.x, 100.5, 1e-9);
        CHECK_NEAR(hand.y, -200.0, 1e-9);
        CHECK_NEAR(hand.z, 0.0, 1e-9);

        // Without a usable x AND y the parse is rejected and the caller's value is untouched.
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
    // The waypoint list, the marker-name filter and the import / export file.

    void test_waypoint_list()
    {
        section("full map - the waypoint list file");

        mv::WaypointSet set{};
        set.count = 3;
        for (int i = 0; i < 3; ++i)
        {
            set.items[i].set = true;
            set.items[i].x = 100.5 + i;
            set.items[i].y = -200.25 - i;
            set.items[i].z = 7.125 * (i + 1);
        }
        mv::WaypointSet back{};
        CHECK(mv::waypoints_parse(mv::waypoints_serialize(set), back));
        CHECK_EQ(static_cast<int>(back.count), 3);
        for (int i = 0; i < 3; ++i)
        {
            CHECK(back.items[i].set);
            // EXACT round trip: the file is written at 17 significant digits.
            CHECK(back.items[i].x == set.items[i].x);
            CHECK(back.items[i].y == set.items[i].y);
            CHECK(back.items[i].z == set.items[i].z);
        }

        // An empty set writes comments only, and reads back as the empty set - what
        // Clear all leaves behind must not look like a damaged file.
        mv::WaypointSet empty{};
        mv::WaypointSet keep{};
        keep.count = 1;
        CHECK(mv::waypoints_parse(mv::waypoints_serialize(empty), keep));
        CHECK_EQ(static_cast<int>(keep.count), 0);
        mv::WaypointSet blank{};
        blank.count = 3;
        CHECK(mv::waypoints_parse("", blank));
        CHECK_EQ(static_cast<int>(blank.count), 0);

        // A `waypoint` line whose coordinates are unreadable IS a damaged file.
        mv::WaypointSet bad{};
        bad.count = 2;
        CHECK(!mv::waypoints_parse("waypoint = hello there\n", bad));
        CHECK_EQ(static_cast<int>(bad.count), 2);

        // THE OLD SINGLE-WAYPOINT FORMAT still loads, as a list of one.
        mv::Waypoint one{};
        one.set = true;
        one.x = 18176.671875;
        one.y = -13905.2109375;
        one.z = -7641.22;
        mv::WaypointSet old_back{};
        CHECK(mv::waypoints_parse("set = 1\nx = 18176.671875\ny = -13905.2109375\nz = -7641.22\n",
                                  old_back));
        CHECK_EQ(static_cast<int>(old_back.count), 1);
        CHECK(old_back.items[0].x == one.x);
        CHECK(old_back.items[0].y == one.y);
        CHECK(old_back.items[0].z == one.z);
        // RIGHT-CLICK ON A MARKER - add, remove, full.
        mv::WaypointSet t{};
        CHECK(mv::waypoint_toggle_at(t, 1.0, 2.0, 3.0, mv::kWaypointSamePlace).action ==
              mv::WaypointToggle::Add);
        t.count = 2;
        t.items[0] = mv::Waypoint{true, 1000.0, 2000.0, 3000.0};
        t.items[1] = mv::Waypoint{true, 1000.0, 2000.0, 9000.0}; // the floor above
        const mv::WaypointToggleResult on = mv::waypoint_toggle_at(t, 1000.0 + mv::kWaypointSamePlace * 0.5,
                                                                   2000.0, 3000.0, mv::kWaypointSamePlace);
        CHECK(on.action == mv::WaypointToggle::Remove);
        CHECK_EQ(on.index, 0);
        // Same x/y, a different floor: a different place, and its own waypoint.
        CHECK_EQ(mv::waypoint_toggle_at(t, 1000.0, 2000.0, 9000.0, mv::kWaypointSamePlace).index, 1);
        CHECK(mv::waypoint_toggle_at(t, 1000.0, 2000.0, 6000.0, mv::kWaypointSamePlace).action ==
              mv::WaypointToggle::Add);
        // A full set still removes what it stands on, and only refuses to add.
        mv::WaypointSet full{};
        full.count = mv::kMaxWaypoints;
        for (std::size_t i = 0; i < mv::kMaxWaypoints; ++i)
        {
            full.items[i] = mv::Waypoint{true, static_cast<double>(i) * 1000.0, 0.0, 0.0};
        }
        CHECK(mv::waypoint_toggle_at(full, 7000.0, 0.0, 0.0, mv::kWaypointSamePlace).action ==
              mv::WaypointToggle::Remove);
        CHECK(mv::waypoint_toggle_at(full, -5000.0, 0.0, 0.0, mv::kWaypointSamePlace).action ==
              mv::WaypointToggle::Full);

        // An old file saying `set = 0` carries no waypoint at all.
        mv::WaypointSet cleared{};
        cleared.count = 1;
        CHECK(mv::waypoints_parse("set = 0\nx = 0\ny = 0\nz = 0\n", cleared));
        CHECK_EQ(static_cast<int>(cleared.count), 0);

        // Hand-written: a BOM, CRLF, comments, commas, a missing z, and junk lines.
        mv::WaypointSet hand{};
        CHECK(mv::waypoints_parse("\xEF\xBB\xBF; mine\r\nwaypoint = 1 2 3\r\n"
                                  "waypoint = 4, 5\r\nwaypoint = hello there\r\nnonsense\r\n",
                                  hand));
        CHECK_EQ(static_cast<int>(hand.count), 2);
        CHECK_NEAR(hand.items[0].x, 1.0, 1e-9);
        CHECK_NEAR(hand.items[0].z, 3.0, 1e-9);
        CHECK_NEAR(hand.items[1].y, 5.0, 1e-9);
        CHECK_NEAR(hand.items[1].z, 0.0, 1e-9);

        // The cap holds: extra lines are dropped, not written past the array.
        std::string many;
        for (std::size_t i = 0; i < mv::kMaxWaypoints + 5; ++i)
        {
            many += "waypoint = 1 2 3\n";
        }
        mv::WaypointSet capped{};
        CHECK(mv::waypoints_parse(many, capped));
        CHECK_EQ(static_cast<int>(capped.count), static_cast<int>(mv::kMaxWaypoints));

        section("full map - the nearest waypoint");

        mv::WaypointSet near_set{};
        near_set.count = 3;
        near_set.items[0] = mv::Waypoint{true, 1000.0, 0.0, 0.0};
        near_set.items[1] = mv::Waypoint{true, 10.0, 10.0, 0.0};
        near_set.items[2] = mv::Waypoint{true, -500.0, 0.0, 0.0};
        CHECK_EQ(mv::nearest_waypoint(near_set, 0.0, 0.0), 1);
        CHECK_EQ(mv::nearest_waypoint(near_set, 900.0, 0.0), 0);
        CHECK_EQ(mv::nearest_waypoint(mv::WaypointSet{}, 0.0, 0.0), -1);
    }

    // The gate that keeps a letter typed into a text box out of the hotkey bindings.
    // The loop thread samples the keyboard 60 times a second; the render thread tells it
    // a caret is up once per frame it manages to draw.
    void test_typing_gate()
    {
        section("hotkeys - the typing gate");

        tgate::Latch l{};
        // Nothing said yet: the bindings are live.
        CHECK(!tgate::typing(l, false, 1000));
        // The frame that reports a caret blocks the same pass it arrives on - the "M"
        // of "Mercury" goes down in the frame the click activated the box.
        CHECK(tgate::typing(l, true, 1000));
        // ... and every 60 Hz pass in the gap before the next frame is published.
        CHECK(tgate::typing(l, false, 1016));
        CHECK(tgate::typing(l, false, 1000 + tgate::kTypingHoldMs - 1));
        // The tail is finite: a caret that is never reported again gives the keys back.
        CHECK(!tgate::typing(l, false, 1000 + tgate::kTypingHoldMs));
        CHECK(!tgate::typing(l, false, 9999));

        // A word typed across a stall - one report, then a long gap - never opens up.
        tgate::Latch word{};
        std::uint64_t t = 5000;
        for (int i = 0; i < 8; ++i)
        {
            CHECK(tgate::typing(word, i % 4 == 0, t)); // a frame every 4th pass
            t += 100;                                  // ... which is a 400 ms gap
        }

        // The hold is a duration, not a deadline: every fresh report pushes it out.
        tgate::Latch push{};
        CHECK(tgate::typing(push, true, 100));
        CHECK(tgate::typing(push, true, 100 + tgate::kTypingHoldMs * 3));
        CHECK(tgate::typing(push, false, 100 + tgate::kTypingHoldMs * 3 + 1));
        CHECK(!tgate::typing(push, false, 100 + tgate::kTypingHoldMs * 4));

        // GetTickCount64 is milliseconds since boot, so `now` is never 0 in the mod -
        // but a zeroed latch must still read as "not typing" at any clock value.
        tgate::Latch zero{};
        CHECK(!tgate::typing(zero, false, 0));
    }

    void test_search_match()
    {
        section("full map - the marker name filter");

        CHECK(txt::contains_ci("Red Box", "red"));
        CHECK(txt::contains_ci("Red Box", "BOX"));
        CHECK(txt::contains_ci("Red Box", "d B"));
        CHECK(txt::contains_ci("Red Box", ""));       // an empty box filters nothing
        CHECK(txt::contains_ci("", ""));
        CHECK(!txt::contains_ci("Red Box", "boxes")); // longer than the name
        CHECK(!txt::contains_ci("", "a"));
        CHECK(!txt::contains_ci("Red Box", "green"));
        // The match may sit at either end, and a near miss must not slide into one.
        CHECK(txt::contains_ci("abcabd", "abd"));
        CHECK(!txt::contains_ci("abcabc", "abd"));
    }

    void test_exchange()
    {
        section("import / export - the JSON round trip");

        xch::Payload p{};
        p.profile = "wuchang_minimap_found_0123.txt";
        p.found = {"Chapter1_DGong_logic/BP_treasurebox_C_12", "Chapter2/BP_item_C_3",
                   "quote\"and\\slash"};
        p.waypoints.push_back(mv::Waypoint{true, 18176.671875, -13905.2109375, -7641.22});
        p.waypoints.push_back(mv::Waypoint{true, 0.0, 0.0, 0.0});

        xch::Payload back{};
        std::string error;
        CHECK(xch::parse(xch::serialize(p), back, error));
        CHECK(error.empty());
        CHECK(back.profile == p.profile);
        CHECK_EQ(static_cast<int>(back.found.size()), 3);
        for (std::size_t i = 0; i < p.found.size(); ++i)
        {
            CHECK(back.found[i] == p.found[i]);
        }
        CHECK_EQ(static_cast<int>(back.waypoints.size()), 2);
        // EXACT: the file is written at 17 significant digits.
        CHECK(back.waypoints[0].x == p.waypoints[0].x);
        CHECK(back.waypoints[0].y == p.waypoints[0].y);
        CHECK(back.waypoints[0].z == p.waypoints[0].z);
        CHECK(back.waypoints[0].set);

        // An empty payload is still valid JSON that round-trips.
        xch::Payload none{};
        xch::Payload none_back{};
        CHECK(xch::parse(xch::serialize(none), none_back, error));
        CHECK(none_back.found.empty());
        CHECK(none_back.waypoints.empty());

        // Rejections, each with a reason.
        xch::Payload junk{};
        CHECK(!xch::parse("", junk, error));
        CHECK(!error.empty());
        CHECK(!xch::parse("[1,2,3]", junk, error));
        CHECK(!xch::parse("{}", junk, error));
        CHECK(!xch::parse("{\"schema\": \"something-else\"}", junk, error));

        // Rows that are not usable are skipped, not fatal.
        xch::Payload lax{};
        CHECK(xch::parse("{\"schema\": \"wuchang-minimap-export-1\", \"found\": [\"a\", 3, \"\"], "
                         "\"waypoints\": [{\"x\": 1}, {\"x\": 1, \"y\": 2}]}",
                         lax, error));
        CHECK_EQ(static_cast<int>(lax.found.size()), 1);
        CHECK_EQ(static_cast<int>(lax.waypoints.size()), 1);
        CHECK_NEAR(lax.waypoints[0].z, 0.0, 1e-9);

        section("import - the waypoint merge");

        mv::WaypointSet set{};
        set.count = 1;
        set.items[0] = mv::Waypoint{true, 100.0, 200.0, 300.0};
        std::vector<mv::Waypoint> incoming;
        incoming.push_back(mv::Waypoint{true, 100.0, 200.0, 300.0});                       // exact twin
        incoming.push_back(mv::Waypoint{true, 100.0 + xch::kWaypointEpsilon * 0.5, 200.0, 300.0}); // inside
        incoming.push_back(mv::Waypoint{true, 100.0 + xch::kWaypointEpsilon * 2.0, 200.0, 300.0}); // outside
        incoming.push_back(mv::Waypoint{true, 100.0 + xch::kWaypointEpsilon * 2.0, 200.0, 300.0}); // its twin
        xch::MergeResult r = xch::merge_waypoints(set, incoming);
        CHECK_EQ(r.added, 1);
        CHECK_EQ(r.duplicates, 3);
        CHECK_EQ(r.dropped, 0);
        CHECK_EQ(static_cast<int>(set.count), 2);
        CHECK(set.items[1].set);

        // The cap counts what did not fit instead of dropping it silently.
        mv::WaypointSet full{};
        std::vector<mv::Waypoint> many;
        for (std::size_t i = 0; i < mv::kMaxWaypoints + 3; ++i)
        {
            many.push_back(mv::Waypoint{true, static_cast<double>(i) * 1000.0, 0.0, 0.0});
        }
        r = xch::merge_waypoints(full, many);
        CHECK_EQ(static_cast<int>(full.count), static_cast<int>(mv::kMaxWaypoints));
        CHECK_EQ(r.added, static_cast<int>(mv::kMaxWaypoints));
        CHECK_EQ(r.dropped, 3);

        // Z separates two waypoints one above the other.
        mv::WaypointSet stack{};
        stack.count = 1;
        stack.items[0] = mv::Waypoint{true, 0.0, 0.0, 0.0};
        r = xch::merge_waypoints(stack, {mv::Waypoint{true, 0.0, 0.0, xch::kWaypointEpsilon * 4.0}});
        CHECK_EQ(r.added, 1);

        section("import - resolving the typed path");

        CHECK(xch::path_is_absolute(L"C:\\x\\y.json"));
        CHECK(xch::path_is_absolute(L"c:/x/y.json"));
        CHECK(xch::path_is_absolute(L"\\\\server\\share\\y.json"));
        CHECK(xch::path_is_absolute(L"\\rooted.json"));
        CHECK(!xch::path_is_absolute(L"y.json"));
        CHECK(!xch::path_is_absolute(L"backups\\y.json"));
        CHECK(!xch::path_is_absolute(L""));
        const std::wstring dir = L"D:\\game\\Mods\\WuchangMinimap";
        CHECK(xch::resolve_import_path(dir, L"y.json") == dir + L"\\y.json");
        // A separator in the name does NOT make it absolute - that was the bug.
        CHECK(xch::resolve_import_path(dir, L"backups\\y.json") == dir + L"\\backups\\y.json");
        CHECK(xch::resolve_import_path(dir, L"C:\\else\\y.json") == L"C:\\else\\y.json");
        CHECK(xch::resolve_import_path(dir, L"") == L"");
    }

    // The chunked object-array scan scheduler (src/scan_sched.hpp)

    void test_scan_sched()
    {
        std::printf("-- scan scheduler --\n");

        CHECK(scan::clamp_chunk(scan::kChunkDefault) == scan::kChunkDefault);
        CHECK(scan::clamp_chunk(0) == scan::kChunkMin);
        CHECK(scan::clamp_chunk(-5) == scan::kChunkMin);
        CHECK(scan::clamp_chunk(1 << 30) == scan::kChunkMax);
        CHECK(scan::clamp_period_ms(0) == scan::kPeriodMinMs);
        CHECK(scan::clamp_period_ms(100000) == scan::kPeriodMaxMs);
        CHECK(scan::clamp_period_ms(scan::kPeriodDefaultMs) == scan::kPeriodDefaultMs);

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
            // ceil(25000 / 8192) == 4.
            CHECK(slices == 4);
            CHECK(c.round == 1);
            CHECK(c.index == 0);
            CHECK(c.visited == 0); // reset by the wrap
        }

        {
            scan::Cursor c{};
            const scan::Slice s = scan::next_slice(c, 100, 8192);
            CHECK(s.begin == 0);
            CHECK(s.end == 100);
            CHECK(scan::advance(c, s, 100));
            CHECK(c.round == 1);
        }

        // GUObjectArray grows as levels stream in and can drop after a GC, so `total` is re-read
        {
            scan::Cursor c{};
            c.index = 40000;
            const scan::Slice s = scan::next_slice(c, 1000, 8192);
            CHECK(s.begin == 0);
            CHECK(s.end == 1000);
        }

        {
            scan::Cursor c{};
            const scan::Slice s = scan::next_slice(c, 0, 8192);
            CHECK(s.empty());
            CHECK(scan::advance(c, s, 0));
            CHECK(c.round == 1);
            CHECK(c.index == 0);
        }

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

        // A zero "last" means never-ran and is always due.
        CHECK(scan::elapsed(1000, 0, 1000000));
        CHECK(!scan::slice_due(5000, 1000, 8));   // 4 ms into an 8 ms period
        CHECK(scan::slice_due(9001, 1000, 8));    // 8.001 ms
        CHECK(scan::slice_due(1000000, 0, 8));
        // A QPC that appears to go backwards must not wedge the scan forever.
        CHECK(scan::slice_due(500, 1000, 8));

        // rounds_per_sec 1 => a finished round waits a second before the next starts.
        CHECK(!scan::round_due(999999, 1, 1));
        CHECK(scan::round_due(1000002, 1, 1));
        CHECK(scan::round_due(1000002, 1, 0));
        CHECK(scan::round_due(20000, 1, 1000));

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

    // The adaptive menu-widget discovery sweep (scan::SweepSched)
    // The sweep only DISCOVERS unseen menu roots; gamestate.cpp re-tests confirmed ones each pump.

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

        scan::sweep_arm(s, 10000);
        CHECK(scan::sweep_due(s, 10000));
        CHECK(scan::sweep_armed(s, 10000));
        CHECK(scan::sweep_armed(s, 11999));
        CHECK(!scan::sweep_armed(s, 12000)); // warm_ms after the arm

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

        // An empty watchlist backs off like any other quiet run, merely CAPPED at unknown_ms.
        scan::SweepSched e{};
        scan::sweep_arm(e, 0);
        for (std::uint64_t t = 0; t < 60000; t += 250)
        {
            scan::sweep_done(e, t, false, /*nothing_known=*/true);
            const std::uint64_t p = scan::sweep_period_ms(e, t);
            CHECK(p >= 250);
            CHECK(p <= e.unknown_ms);
        }
        CHECK(scan::sweep_period_ms(e, 60000) == 1000); // settled at the cap
        CHECK(e.nothing_known);

        // A sweep that DOES find something on the watchlist backs off all the way to slow_ms.
        scan::SweepSched k{};
        scan::sweep_arm(k, 0);
        for (std::uint64_t t = 3000; t < 60000; t += 2000)
        {
            scan::sweep_done(k, t, false, /*nothing_known=*/false);
        }
        CHECK(!k.nothing_known);
        CHECK(scan::sweep_period_ms(k, 60000) == 2000);

        // A cap looser than fast_ms can never speed the schedule UP past what the config asked for.
        scan::SweepSched u{};
        u.fast_ms = 1500;
        u.slow_ms = 4000;
        u.unknown_ms = 1000;
        scan::sweep_arm(u, 0);
        for (std::uint64_t t = 3000; t < 60000; t += 2000)
        {
            scan::sweep_done(u, t, false, /*nothing_known=*/true);
            CHECK(scan::sweep_period_ms(u, t) >= 1500);
        }

        // A re-arm mid-backoff returns to fast and makes a sweep due at once (menu flip / teleport).
        scan::SweepSched r2{};
        scan::sweep_arm(r2, 0);
        scan::sweep_done(r2, 9000, false, false);
        scan::sweep_done(r2, 11000, false, false);
        CHECK(scan::sweep_period_ms(r2, 11000) > 250);
        scan::sweep_arm(r2, 11500);
        CHECK(scan::sweep_due(r2, 11500));
        CHECK(scan::sweep_period_ms(r2, 11500) == 250);

        // The shift is bounded: a very long quiet run can never overflow or exceed slow_ms.
        scan::SweepSched b{};
        b.slow_ms = 1000000;
        scan::sweep_arm(b, 0);
        for (int i = 0; i < 40; ++i)
        {
            scan::sweep_done(b, 100000, false, false);
        }
        CHECK(b.backoff == scan::kSweepMaxBackoff);
        CHECK(scan::sweep_period_ms(b, 100000) == 250ull << scan::kSweepMaxBackoff);

        // The discovery pass is one full round of the shared GUObjectArray cursor; a round of a
        // 360 k-slot array must finish inside the quiet cadences or rounds queue up.
        CHECK(scan::kWidgetChunkDefault == scan::kChunkDefault);
        CHECK(scan::kWidgetSlicePeriodMs >= scan::kPeriodMinMs);
        {
            constexpr int kTotal = 360000;
            scan::Cursor c{};
            int slices = 0;
            bool wrapped = false;
            while (!wrapped && slices < 10000)
            {
                const scan::Slice s = scan::next_slice(c, kTotal, scan::kWidgetChunkDefault);
                CHECK(s.count() > 0);
                CHECK(s.count() <= scan::kWidgetChunkDefault);
                wrapped = scan::advance(c, s, kTotal);
                ++slices;
            }
            CHECK(wrapped);
            CHECK(slices == 44); // ceil(360000 / 8192)
            // Wall time of one round at the slice spacing, in ms.
            CHECK(slices * scan::kWidgetSlicePeriodMs == 352);
            // Inside the quiet cadences (unknown_ms / slow_ms) and LONGER than fast_ms: while armed
            // the walk runs continuously.
            const scan::SweepSched def{};
            CHECK(static_cast<std::uint64_t>(slices * scan::kWidgetSlicePeriodMs) < def.unknown_ms);
            CHECK(static_cast<std::uint64_t>(slices * scan::kWidgetSlicePeriodMs) > def.fast_ms);
        }
        // The candidate cap covers every widget whose Visibility BYTE says Visible - this game
        // leaves it set on widgets removed from the viewport - and the list fills in index order.
        CHECK(scan::kWidgetCandidateMax >= 256);

        // The menu answer ORs two fresh tests: a watchlist root in the viewport, or a new root.
        CHECK(!scan::menu_open_from(false, false));
        CHECK(scan::menu_open_from(true, false));
        CHECK(scan::menu_open_from(false, true));
        CHECK(scan::menu_open_from(true, true));


        // THE NOT-A-MENU DENY-LIST
        // Furniture the game authors as plain Visible - ZiMu is its own name for SUBTITLES -
        // needs a named exception to the "in-viewport and Visible" rule.
        std::printf("-- the not-a-menu deny-list --\n");

        // Both character widths: the runtime matches a wchar_t class name, the table is ASCII.
        CHECK(scan::builtin_non_menu_reason(L"WB_ZiMu_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason("WB_ZiMu_C") != nullptr);
        // A prefix, so every instance's class variant is covered.
        CHECK(scan::builtin_non_menu_reason(L"WB_ZiMu_Combat_C") != nullptr);
        // Case-insensitive: the list must not depend on how the game capitalised it.
        CHECK(scan::builtin_non_menu_reason(L"wb_zimu_c") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_ZIMU_C") != nullptr);

        // The roots that really are menus must survive the list, or the minimap stops hiding.
        CHECK(scan::builtin_non_menu_reason(L"WB_MenuMain_C") == nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_PlumeArchive_Main_C") == nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_Login_C") == nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_PlumeTransit_C") == nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_Setting_C") == nullptr);

        // The HUD roots. All HitTestInvisible, so the Visibility test already excludes them.
        CHECK(scan::builtin_non_menu_reason(L"WB_MainUI_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_InteractionTips_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_ShowAddItemMain_New_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_NPCBG_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_GameSaving_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_AddressInfo_C") != nullptr);
        CHECK(scan::builtin_non_menu_reason(L"WB_AnimationSlot_Fade_C") != nullptr);

        // Every entry needs a reason and a non-empty prefix: an empty prefix matches EVERY widget.
        for (const scan::NonMenuRoot& row : scan::kNonMenuRoots)
        {
            CHECK(row.prefix != nullptr && row.prefix[0] != '\0');
            CHECK(row.why != nullptr && row.why[0] != '\0');
        }

        // A nullptr name is never a match, and the empty name is not either.
        CHECK(scan::builtin_non_menu_reason(static_cast<const wchar_t*>(nullptr)) == nullptr);
        CHECK(scan::builtin_non_menu_reason(L"") == nullptr);

        // `menu_ignore_roots`: the player's own additions, separators mixed.
        CHECK(scan::non_menu_root_reason(L"WB_MenuMain_C", "") == nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_Weird_C", "WB_Weird") != nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_Weird_C", "WB_Other, WB_Weird ; WB_Third") != nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_Weird_C", "wb_weird") != nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_MenuMain_C", "WB_Weird,WB_Other") == nullptr);
        // A list of nothing but separators matches nothing; an empty token would silence every menu.
        CHECK(scan::non_menu_root_reason(L"WB_MenuMain_C", " , ; ,, ") == nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_MenuMain_C", ",") == nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_MenuMain_C", static_cast<const char*>(nullptr)) == nullptr);
        CHECK(scan::non_menu_root_reason(L"WB_", "WB_MenuMain_C") == nullptr);

        // A pump tests at most kWidgetCommitPerPump candidates; the rest stay pending.
        CHECK_EQ(scan::commit_batch(0, 128), 0);
        CHECK_EQ(scan::commit_batch(5, 128), 5);
        CHECK_EQ(scan::commit_batch(300, 128), 128);
        CHECK_EQ(scan::commit_batch(128, 128), 128);
        CHECK_EQ(scan::commit_batch(-3, 128), 0);
        CHECK_EQ(scan::commit_batch(10, 0), 0);

        // The latency contract:
        //   * a known root, and a menu CLOSING: one pump (~100 ms);
        //   * a never-seen root: one quiet period + one round of the walk + the commit pumps.
        {
            constexpr int kPositionMs = 100;
            const scan::SweepSched def{};
            CHECK_EQ(scan::commit_pumps_needed(0, scan::kWidgetCommitPerPump), 0);
            const int drain_pumps =
                scan::commit_pumps_needed(scan::kWidgetCandidateMax, scan::kWidgetCommitPerPump);
            CHECK(drain_pumps >= 1);
            const std::uint64_t worst_first_seen_ms =
                def.unknown_ms + static_cast<std::uint64_t>(44 * scan::kWidgetSlicePeriodMs) +
                static_cast<std::uint64_t>(drain_pumps * kPositionMs);
            CHECK(worst_first_seen_ms <= 1800);
            // A KNOWN menu is one pump, whatever the discovery walk is doing.
            CHECK(scan::menu_open_from(true, false));
        }
    }
    // src/projection.hpp - world -> screen
    // Expectations are computed by hand from the conventions at the top of projection.hpp.

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

        // tan(vfov/2) = tan(hfov/2) / aspect, so the top of the screen is at up/forward == 0.5625.
        {
            const proj::Result top = proj::project(cam, 1000.0, 0.0, 1000.0 / aspect, kW, kH);
            CHECK(top.on_screen);
            CHECK_NEAR(top.ndc_y, 1.0, 1e-12);
            CHECK_NEAR(top.sy, 0.0, 1e-6);

            const proj::Result high = proj::project(cam, 1000.0, 0.0, 1000.0, kW, kH);
            CHECK(!high.on_screen);
            CHECK(!high.behind);
            CHECK_NEAR(high.ndc_y, aspect, 1e-12);
        }

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
            // Straight ahead is now exactly 90 degrees off, i.e. ON the camera plane: the behind case.
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

            // Behind AND to the right: the arrow points RIGHT, the short way round.
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

    // src/compass.cpp - the heading strip

    void test_compass()
    {
        std::printf("compass (headings and bearings)\n");

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

            CHECK(!cmp::strip_x(s, 61.0, x, rel));
            CHECK_NEAR(x, 600.0, 1e-9);
            CHECK(!cmp::strip_x(s, 180.0, x, rel));
            CHECK(!cmp::strip_x(s, 299.0, x, rel));
            CHECK_NEAR(x, 0.0, 1e-9);
        }

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
            for (int i = 1; i < n; ++i)
            {
                CHECK(t[i].x >= t[i - 1].x);
                CHECK(t[i].x >= s.x0 - 1e-9 && t[i].x <= s.x0 + s.width + 1e-9);
            }
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
    // chapterid.hpp - "which chapter is the player in?", from the streamed level names
    // The fixtures are real names from the pak index and the WuchangRecon world dumps.

    // The chapter filter for the static marker DB (mdb::marker_in_chapter)
    // Cuts the flat DB down to the player's chapter. Chapter 4's bounds cover nearly all of
    // chapter 1, so a position test cannot do this job.

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

        // --- the DLC is bucket 0, what the manifest's "chapter":"DLC" parses to, and it must
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

        // --- the join with the parser: a real manifest's chapter number feeds the predicate ----
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
        CHECK_EQ(chid::classify(L"Level /Game/Maps/Generate/Chapter4/EX0/"
                                L"B4EX0_L0_X0_Y0_DL0_WP.B4EX0_L0_X0_Y0_DL0_WP:PersistentLevel")
                     .chapter,
                 4);
        CHECK_EQ(chid::classify(L"B4EX0_L0_X0_Y0_DL0_WP").tier, chid::kTierCell);
        // The cell wins over the "Chapter2" also in the path: the tier comes from the cell.
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

        CHECK_EQ(chid::chapter_from_key("chapter1"), 1);
        CHECK_EQ(chid::chapter_from_key("chapter5"), 5);
        CHECK_EQ(chid::chapter_from_key("chapterdlc"), chid::kDlc);
        CHECK_EQ(chid::chapter_from_key("nonsense"), chid::kNone);
        CHECK_EQ(chid::chapter_from_key(""), chid::kNone);

        // A plain sum over "Chapter<N> appears" would follow how much art each chapter streams;
        // the cells under the player's feet win outright.
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

        // The DLC: no cell package exists in the paks, so tier 2 decides, and it still has to beat
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

        // Nothing recognisable answers kNone, which the runtime treats as "keep what you had".
        {
            chid::Vote v{};
            v.add("Lobby");
            v.add("/Game/Maps/ProjectMain.ProjectMain:PersistentLevel");
            CHECK_EQ(v.best(), chid::kNone);
            CHECK_EQ(v.best_tier(), 0);
            CHECK_EQ(v.best_count(), 0);
        }

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

    // mapmanifest.hpp - maps/maps.json

    void test_map_manifest(const std::string& markers_dir)
    {
        std::printf("mapmanifest: schema /5 and /4, five chapters, and the ways it can be wrong\n");

        {
            const char* text = R"({
              "schema": "wuchang-minimap-maps/4",
              "chapters": {
                "chapter1": { "chapter": 1, "image": "chapter1/small.png",
                              "image_width": 100, "image_height": 200,
                              "min_x": -10, "min_y": -20, "max_x": 30, "max_y": 40,
                              "px_per_uu": 0.06, "z_min": -5, "z_max": 15,
                              "z_bits": 12, "z_code_max": 4095,
                              "max_surfaces": 2,
                              "height_planes": ["chapter1/small_h0.png", "chapter1/small_h1.png"] },
                "chapterdlc": { "chapter": 0, "image": "dlc/small.png",
                                "image_width": 10, "image_height": 10,
                                "min_x": 0, "min_y": 0, "max_x": 1, "max_y": 1,
                                "px_per_uu": 0.5, "z_min": 0, "z_max": 1,
                                "max_surfaces": 1, "height_planes": ["dlc/small_h0.png"] }
              } })";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(mapmanifest::parse(text, m, problems));
            CHECK_EQ(static_cast<long long>(problems.size()), 0);
            CHECK_STR(m.schema, std::string(mapmanifest::kSchemaNoReach));
            CHECK(!m.has_reachability());
            CHECK(!m.chapters[0].has_reachability);
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 2);
            CHECK_STR(m.chapters[0].key, std::string("chapter1"));
            CHECK_EQ(m.chapters[0].chapter, 1);
            CHECK_EQ(m.chapters[0].max_surfaces, 2);
            CHECK(m.chapters[0].geometry_ok());
            CHECK(m.chapters[0].heights_ok());
            CHECK(!m.chapters[0].height_maps_guessed);
            CHECK(m.schema_ok());
            CHECK_EQ(m.chapters[0].z_bits, 12);
            CHECK_EQ(m.chapters[0].z_code_max, 4095);
            // 20 uu of span over 4094 steps.
            CHECK_NEAR(m.chapters[0].z_step_uu(), 20.0 / 4094.0, 1e-9);
            CHECK_EQ(m.index_of_number(1), 0);
            CHECK_EQ(m.index_of_number(chid::kDlc), 1);
            CHECK_EQ(m.index_of_number(4), -1);
            CHECK_EQ(m.index_of_key("chapterdlc"), 1);
            CHECK_EQ(m.index_of_key("chapter9"), -1);
            // The DLC is not a numbered chapter, so it is never the start-up default.
            CHECK_EQ(m.default_index(), 0);
        }

        // --- schema /5: the same document, with reachability in bit 12 ---------------
        {
            const char* text = R"({
              "schema": "wuchang-minimap-maps/5",
              "chapters": {
                "chapter1": { "chapter": 1, "image": "chapter1/small.png",
                              "image_width": 100, "image_height": 200,
                              "min_x": -10, "min_y": -20, "max_x": 30, "max_y": 40,
                              "px_per_uu": 0.06, "z_min": -5, "z_max": 15,
                              "z_bits": 12, "z_code_max": 4095,
                              "max_surfaces": 1,
                              "height_planes": ["chapter1/small_h0.png"] }
              } })";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(mapmanifest::parse(text, m, problems));
            CHECK_EQ(static_cast<long long>(problems.size()), 0);
            CHECK(m.schema_ok());
            CHECK_STR(m.schema, std::string(mapmanifest::kSchema));
            CHECK(m.has_reachability());
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK(m.chapters[0].has_reachability);
            // /5 changed nothing else: same geometry, same 12-bit Z.
            CHECK_EQ(m.chapters[0].z_bits, 12);
            CHECK_EQ(m.chapters[0].z_code_max, 4095);
            CHECK(m.chapters[0].geometry_ok());
            CHECK(m.chapters[0].heights_ok());
        }

        // --- a chapter that states max_surfaces but no plane list --------------------
        // The plane names come from the composite's /4 `_h` spelling, the chapter from the key.
        {
            const char* text = R"({
              "schema": "wuchang-minimap-maps/4",
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
            CHECK_STR(m.chapters[0].height_maps[0], std::string("chapter1/small_h0.png"));
            CHECK_STR(m.chapters[0].height_maps[7], std::string("chapter1/small_h7.png"));
            CHECK(m.chapters[0].heights_ok());
            // Absent z_bits / z_code_max default to this build's values; the schema is checked first.
            CHECK_EQ(m.chapters[0].z_code_max, mapmanifest::kZCodeMax);
        }

        // --- the version gate, both directions ---------------------------------------
        // A /3 tree read here would put every surface sixteen times too low, so a wrong or missing
        // schema is fatal. A /3 build reading this /4 file finds no plane list and fails loudly.
        {
            const char* v3 = R"({
              "schema": "wuchang-minimap-maps/3",
              "chapters": {
                "chapter1": { "chapter": 1, "image": "chapter1/small.png",
                              "image_width": 100, "image_height": 200,
                              "min_x": -10, "min_y": -20, "max_x": 30, "max_y": 40,
                              "px_per_uu": 0.06, "z_min": -5, "z_max": 15,
                              "max_surfaces": 1,
                              "height_maps": ["chapter1/small_z0.png"] }
              } })";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(!mapmanifest::parse(v3, m, problems));
            CHECK_EQ(static_cast<long long>(problems.size()), 1);
            CHECK(!m.schema_ok());
            CHECK(m.chapters.empty()); // nothing is drawn from a file we cannot read
            CHECK(problems[0].find("wuchang-minimap-maps/3") != std::string::npos);
            CHECK(problems[0].find("wuchang-minimap-maps/5") != std::string::npos);
            CHECK(problems[0].find("wuchang-minimap-maps/4") != std::string::npos);
            CHECK(problems[0].find("build_map.py") != std::string::npos);

            problems.clear();
            CHECK(!mapmanifest::parse(R"({"chapters":{}})", m, problems));
            CHECK_EQ(static_cast<long long>(problems.size()), 1);
            CHECK(problems[0].find("(none)") != std::string::npos);

            // A /4 file whose plane list is spelled the /3 way loses the list, which makes it loud.
            problems.clear();
            const char* mixed_key = R"({
              "schema": "wuchang-minimap-maps/4",
              "chapters": {
                "chapter1": { "chapter": 1, "image": "chapter1/small.png",
                              "image_width": 100, "image_height": 200,
                              "min_x": -10, "min_y": -20, "max_x": 30, "max_y": 40,
                              "px_per_uu": 0.06, "z_min": -5, "z_max": 15,
                              "max_surfaces": 1,
                              "height_maps": ["chapter1/small_z0.png"] }
              } })";
            CHECK(mapmanifest::parse(mixed_key, m, problems));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK(m.chapters[0].height_maps_guessed);
            CHECK_STR(m.chapters[0].height_maps[0], std::string("chapter1/small_h0.png"));
        }

        {
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(!mapmanifest::parse("not json at all", m, problems));
            CHECK(!problems.empty());

            problems.clear();
            CHECK(!mapmanifest::parse(R"({"schema":"wuchang-minimap-maps/4"})", m, problems));
            CHECK(!problems.empty());

            problems.clear();
            // array, not object
            CHECK(!mapmanifest::parse(
                R"({"schema":"wuchang-minimap-maps/4","chapters": []})", m, problems));

            // A broken chapter is skipped and REPORTED, and its siblings still load.
            problems.clear();
            const char* mixed = R"({
              "schema": "wuchang-minimap-maps/4",
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
                                     "height_planes": ["chapter4/small_h0.png"] }
              } })";
            CHECK(mapmanifest::parse(mixed, m, problems));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK_EQ(static_cast<long long>(problems.size()), 2);
            CHECK_STR(m.chapters[0].key, std::string("chapter4"));
            CHECK_EQ(m.default_index(), 0);
        }

        // --- a chapter with no z range has geometry but no usable height maps ---------
        {
            const char* text = R"({"schema": "wuchang-minimap-maps/4",
              "chapters": {"chapter1": {
                "chapter": 1, "image": "c/s.png", "image_width": 4, "image_height": 4,
                "min_x": 0, "min_y": 0, "max_x": 1, "max_y": 1, "px_per_uu": 0.5,
                "height_planes": ["c/s_h0.png"] }}})";
            mapmanifest::Manifest m{};
            std::vector<std::string> problems;
            CHECK(mapmanifest::parse(text, m, problems));
            CHECK_EQ(static_cast<long long>(m.chapters.size()), 1);
            CHECK(m.chapters[0].geometry_ok());
            CHECK(!m.chapters[0].heights_ok()); // z_max == z_min == 0
        }

        // The repo's manifest must parse, name five chapters, and carry every chapter number and
        // every height plane the runtime looks for.
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
                CHECK(m.schema_ok());
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
                    // Every shipped chapter is 12-bit, every plane is named the `_h` way,
                    // and reachability is a property of the whole tree.
                    CHECK_EQ(e.has_reachability, m.has_reachability());
                    CHECK_EQ(e.z_bits, mapmanifest::kZBits);
                    CHECK_EQ(e.z_code_max, mapmanifest::kZCodeMax);
                    CHECK(e.height_maps[0].find("_h0.png") != std::string::npos);
                    CHECK(e.z_step_uu() > 0.0 && e.z_step_uu() < 20.0);
                    // The DENSE size has to fit build_map.py's budget (--max-ram-mb 340); what is allocated
                    // is the sparse tile subset, which test_map_assets() checks against the manifest.
                    const std::size_t bytes = static_cast<std::size_t>(e.image_width) *
                                              static_cast<std::size_t>(e.image_height) * 2u *
                                              e.height_maps.size();
                    CHECK(bytes <= 340u * 1024u * 1024u);
                    total_ram = bytes > total_ram ? bytes : total_ram;
                }
                std::printf("  shipped manifest: %d chapters, worst chapter %llu MB resident\n",
                            static_cast<int>(m.chapters.size()),
                            static_cast<unsigned long long>(total_ram / (1024 * 1024)));
                // The DLC deliberately has NO map asset: the paks carry no Maps/Generate/ChapterDLC cells.
                CHECK_EQ(m.index_of_number(chid::kDlc), -1);
            }
        }
    }

    // The SPARSE height-plane store (src/mapdata.hpp: build_plane + gather_row)
    // The planes are 128-px blocks with an index, and an absent block reads as code 0. The test
    // is differential against a dense reference over patterns hitting every indexing edge.

    // The height code's two fields, and the slicer's per-pixel rule (src/slicerule.hpp).

    void test_slice_rule()
    {
        std::printf("height codes and the slice rule: masking, reachability, fade above\n");

        section("bit 12 never reaches the Z");
        {
            mapdata::HeightMaps hm{};
            hm.count = 1;
            hm.width = 8;
            hm.height = 8;
            hm.z_min = 0.0f;
            hm.z_max = 4094.0f;
            hm.z_code_max = 4095; // one uu per step
            const std::uint16_t codes[] = {1, 2, 1234, 4095};
            for (const std::uint16_t c : codes)
            {
                const std::uint16_t marked = static_cast<std::uint16_t>(c | mapdata::kReachableBit);
                CHECK_EQ(mapdata::z_code(marked), c);
                CHECK_EQ(mapdata::z_code(c), c);
                CHECK_NEAR(static_cast<double>(hm.decode(marked)), static_cast<double>(hm.decode(c)), 1e-6);
            }
            // 0 is "no surface" whatever bit 12 says, and nothing sets bits 13..15.
            CHECK_EQ(mapdata::z_code(mapdata::kReachableBit), 0);
            CHECK_EQ(mapdata::kReachableBit, 0x1000);
            CHECK_EQ(mapdata::kZCodeMask, 0x0FFF);
            CHECK_EQ(mapdata::kZCodeMask | mapdata::kReachableBit, 0x1FFF);

            // A /4 asset has no bit 12, so everything in it is reachable.
            hm.has_reachability = false;
            CHECK(hm.reachable(1234));
            CHECK(hm.reachable(static_cast<std::uint16_t>(1234 | mapdata::kReachableBit)));
            hm.has_reachability = true;
            CHECK(!hm.reachable(1234));
            CHECK(hm.reachable(static_cast<std::uint16_t>(1234 | mapdata::kReachableBit)));
        }

        section("map_unreachable = hide | dim | show, on a two-surface column");
        {
            srule::Unreachable u = srule::Unreachable::Show;
            CHECK(srule::unreachable_from_name("Hide", u));
            CHECK(u == srule::Unreachable::Hide);
            CHECK(srule::unreachable_from_name(" d i m ", u));
            CHECK(u == srule::Unreachable::Dim);
            CHECK(srule::unreachable_from_name("SHOW", u));
            CHECK(u == srule::Unreachable::Show);
            CHECK(!srule::unreachable_from_name("maybe", u));
            CHECK(u == srule::Unreachable::Show); // a bad value keeps what is in force
            CHECK_STR(std::string(srule::unreachable_name(srule::Unreachable::Hide)), std::string("hide"));
            CHECK_STR(std::string(srule::unreachable_name(srule::Unreachable::Dim)), std::string("dim"));
            CHECK_STR(std::string(srule::unreachable_name(srule::Unreachable::Show)), std::string("show"));

            // The player stands at Z 1000. Under their feet: an UNREACHABLE surface at
            // their own level (a wall top the flood never reached) and a REACHABLE floor
            // 500 uu below it.
            srule::SliceStyle st{};
            st.tol = 200.0f;
            st.fade = 800.0f;
            st.fade_above = 300.0f;
            st.a_dim = 0.25f;
            st.a_faint = 0.15f;

            const auto column = [&st](srule::Unreachable mode, std::uint8_t& cls, bool& reach, float& alpha) {
                st.unreachable = mode;
                std::uint8_t rank = 0;
                float ad = 0.0f;
                float d = 0.0f;
                srule::accumulate(rank, ad, d, 0.0f, false, st);     // my level, unreachable
                srule::accumulate(rank, ad, d, -500.0f, true, st);   // 500 uu below, reachable
                cls = srule::rank_class(rank);
                reach = srule::rank_reachable(rank);
                alpha = srule::alpha_for(cls, reach, st);
            };

            std::uint8_t cls = 0;
            bool reach = false;
            float alpha = 0.0f;

            // hide: the wall top is not a surface at all, so the floor below wins.
            column(srule::Unreachable::Hide, cls, reach, alpha);
            CHECK_EQ(cls, srule::kClassBelow);
            CHECK(reach);
            CHECK_NEAR(static_cast<double>(alpha), 0.25, 1e-6);

            // dim: it is drawn, one rung down the same ladder - never at full opacity.
            column(srule::Unreachable::Dim, cls, reach, alpha);
            CHECK_EQ(cls, srule::kClassFloor);
            CHECK(!reach);
            CHECK_NEAR(static_cast<double>(alpha), 0.25, 1e-6);

            // show: indistinguishable from a reachable floor - 1.0.0's picture.
            column(srule::Unreachable::Show, cls, reach, alpha);
            CHECK_EQ(cls, srule::kClassFloor);
            CHECK(reach);
            CHECK_NEAR(static_cast<double>(alpha), 1.0, 1e-6);

            // Within one class a REACHABLE surface beats an unreachable one even when the
            // unreachable one is nearer.
            st.unreachable = srule::Unreachable::Dim;
            std::uint8_t rank = 0;
            float ad = 0.0f;
            float d = 0.0f;
            srule::accumulate(rank, ad, d, 10.0f, false, st);
            srule::accumulate(rank, ad, d, -150.0f, true, st);
            CHECK_EQ(srule::rank_class(rank), srule::kClassFloor);
            CHECK(srule::rank_reachable(rank));
            CHECK_NEAR(static_cast<double>(d), -150.0, 1e-6);
            // ... and within one rank, the nearest wins.
            rank = 0;
            ad = 0.0f;
            d = 0.0f;
            srule::accumulate(rank, ad, d, -150.0f, true, st);
            srule::accumulate(rank, ad, d, 20.0f, true, st);
            CHECK_NEAR(static_cast<double>(d), 20.0, 1e-6);
        }

        section("floor_fade_above_uu splits the above case off floor_fade_uu");
        {
            srule::SliceStyle st{};
            st.tol = 200.0f;
            st.fade = 800.0f;
            st.unreachable = srule::Unreachable::Show;

            const auto one = [&st](float d) {
                std::uint8_t rank = 0;
                float ad = 0.0f;
                float dd = 0.0f;
                srule::accumulate(rank, ad, dd, d, true, st);
                return srule::rank_class(rank);
            };

            // At the shipped default a floor 250 uu up is drawn and one 500 uu up is not,
            // while the same distances BELOW are both drawn - fade is 800 either way in
            // 1.0.0 and the two dials are now independent.
            st.fade_above = 300.0f;
            CHECK_EQ(one(250.0f), srule::kClassAbove);
            CHECK_EQ(one(500.0f), srule::kClassNone);
            CHECK_EQ(one(-250.0f), srule::kClassBelow);
            CHECK_EQ(one(-500.0f), srule::kClassBelow);
            CHECK_EQ(one(-900.0f), srule::kClassNone);

            // 0 = never draw a floor above the player; below is untouched.
            st.fade_above = 0.0f;
            CHECK_EQ(one(250.0f), srule::kClassNone);
            CHECK_EQ(one(201.0f), srule::kClassNone);
            CHECK_EQ(one(1.0f), srule::kClassFloor); // still inside the tolerance
            CHECK_EQ(one(-250.0f), srule::kClassBelow);

            // The gradient span follows the class it belongs to.
            st.fade_above = 300.0f;
            CHECK_NEAR(static_cast<double>(srule::span_for(srule::kClassFloor, st)), 200.0, 1e-6);
            CHECK_NEAR(static_cast<double>(srule::span_for(srule::kClassBelow, st)), 800.0, 1e-6);
            CHECK_NEAR(static_cast<double>(srule::span_for(srule::kClassAbove, st)), 300.0, 1e-6);
        }
    }

    void test_height_planes()
    {
        section("the sparse height-plane store: gather_row vs a dense reference");

        // 600x500 is 5 x 4 blocks of 128 px with a partial block at the right (88 px) and at the
        // bottom (116 px), so every combination of full and partial block is present.
        const int w = 600;
        const int h = 500;
        std::vector<std::uint16_t> dense(static_cast<std::size_t>(w) * h, 0);
        // A diagonal band plus a solid square in the bottom-right partial block, with block COLUMN 1
        // and block ROW 2 empty: absent interior blocks in both axes, and 128 fully empty rows.
        for (int y = 0; y < h; ++y)
        {
            if (y >= 256 && y < 384)
            {
                continue; // the empty block row
            }
            for (int x = 0; x < w; ++x)
            {
                if (x >= 128 && x < 256)
                {
                    continue; // the empty block column
                }
                const bool band = ((x + y) % 37) < 7;
                const bool square = x >= 520 && y >= 400;
                if (band || square)
                {
                    dense[static_cast<std::size_t>(y) * w + x] =
                        static_cast<std::uint16_t>(1 + ((x * 7 + y * 13) % 4095));
                }
            }
        }

        mapdata::HeightPlane p{};
        mapdata::build_plane(p, dense.data(), w, h);
        CHECK_EQ(p.ntx, 5); // ceil(600 / 128)
        CHECK_EQ(p.nty, 4); // ceil(500 / 128)
        CHECK_EQ(p.tiles, 12);
        CHECK(p.block(1, 0) == nullptr); // the empty column
        CHECK(p.block(0, 2) == nullptr); // the empty row
        CHECK(p.block(0, 0) != nullptr);
        CHECK(!p.empty());
        CHECK_EQ(static_cast<long long>(p.data.size()),
                 static_cast<long long>(p.tiles) * mapdata::kTileCells);
        CHECK(p.bytes() < dense.size() * sizeof(std::uint16_t));

        mapdata::HeightMaps hm{};
        hm.width = w;
        hm.height = h;
        hm.count = 1;
        hm.z_min = -1000.0f;
        hm.z_max = 3000.0f;
        hm.z_code_max = 4095;
        hm.layer[0] = p;

        CHECK(!hm.plane_empty(0));
        CHECK(hm.plane_empty(1));
        CHECK(hm.plane_empty(-1));
        CHECK(hm.plane_empty(mapdata::kMaxSurfaces));

        long long mismatches = 0;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                if (hm.code_at(0, x, y) != dense[static_cast<std::size_t>(y) * w + x])
                {
                    ++mismatches;
                }
            }
        }
        CHECK_EQ(mismatches, 0);
        // Out of bounds is "no surface", not a crash and not a wrap.
        CHECK_EQ(hm.code_at(0, -1, 0), 0);
        CHECK_EQ(hm.code_at(0, 0, -1), 0);
        CHECK_EQ(hm.code_at(0, w, 0), 0);
        CHECK_EQ(hm.code_at(0, 0, h), 0);
        CHECK_EQ(hm.code_at(1, 10, 10), 0); // an empty plane

        // Three column sets: 1:1, decimating (src_step > 1), and one with out-of-asset columns.
        std::vector<std::vector<int>> col_sets;
        {
            std::vector<int> ones(w);
            for (int i = 0; i < w; ++i)
            {
                ones[static_cast<std::size_t>(i)] = i;
            }
            col_sets.push_back(ones);

            std::vector<int> deci(220);
            for (int i = 0; i < 220; ++i)
            {
                const int sx = i * 3 + 1;
                deci[static_cast<std::size_t>(i)] = sx < w ? sx : -1;
            }
            col_sets.push_back(deci);

            std::vector<int> holes(w + 20);
            for (std::size_t i = 0; i < holes.size(); ++i)
            {
                const int sx = static_cast<int>(i) - 10;
                holes[i] = (sx >= 0 && sx < w && (sx < 300 || sx > 320)) ? sx : -1;
            }
            col_sets.push_back(holes);
        }

        long long gathered_rows = 0;
        long long skipped_rows = 0;
        for (const std::vector<int>& cols : col_sets)
        {
            const int n = static_cast<int>(cols.size());
            std::vector<std::uint16_t> got(static_cast<std::size_t>(n), 0xFFFF);
            for (int y = 0; y < h; ++y)
            {
                std::fill(got.begin(), got.end(), static_cast<std::uint16_t>(0xFFFF));
                const bool any = hm.gather_row(0, y, cols.data(), n, got.data());
                bool want_any = false;
                for (int i = 0; i < n; ++i)
                {
                    const int sx = cols[static_cast<std::size_t>(i)];
                    const std::uint16_t want =
                        sx < 0 ? 0 : dense[static_cast<std::size_t>(y) * w + sx];
                    want_any = want_any || want != 0;
                    if (got[static_cast<std::size_t>(i)] != want)
                    {
                        ++mismatches;
                    }
                }
                // The return value is "this row contributed something"; the caller skips the row on false.
                if (any != want_any)
                {
                    ++mismatches;
                }
                if (any)
                {
                    ++gathered_rows;
                }
                else
                {
                    ++skipped_rows;
                }
            }
        }
        CHECK_EQ(mismatches, 0);
        CHECK(gathered_rows > 0);
        CHECK(skipped_rows > 0); // the empty strip has to produce some

        // A row outside the picture, an empty plane and a nonsense argument all say "nothing here".
        std::vector<std::uint16_t> one(8, 0xFFFF);
        const int cols8[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        CHECK(!hm.gather_row(0, -1, cols8, 8, one.data()));
        CHECK(!hm.gather_row(0, h, cols8, 8, one.data()));
        CHECK(!hm.gather_row(1, 0, cols8, 8, one.data()));
        CHECK(!hm.gather_row(0, 0, cols8, 0, one.data()));
        CHECK(!hm.gather_row(0, 0, nullptr, 8, one.data()));
        CHECK(!hm.gather_row(0, 0, cols8, 8, nullptr));

        int px = -1;
        int py = -1;
        std::uint16_t code = 0;
        CHECK(hm.first_lit(0, px, py, code));
        CHECK(px >= 0 && px < w && py >= 0 && py < h);
        CHECK(code != 0);
        CHECK_EQ(code, dense[static_cast<std::size_t>(py) * w + px]);
        CHECK(!hm.first_lit(1, px, py, code)); // an empty plane has none

        CHECK_NEAR(hm.z_step(), 4000.0f / 4094.0f, 1e-4);
        CHECK_NEAR(hm.decode(1), -1000.0, 1e-3);   // code 1 is exactly z_min
        CHECK_NEAR(hm.decode(4095), 3000.0, 1e-2); // and the top code is z_max
        // 12 bits over this span: the step stays far under the slicer's 200 uu floor tolerance.
        CHECK(hm.z_step() < 20.0f);

        // ---- an all-empty plane costs the index and nothing else ---------------------
        std::vector<std::uint16_t> nothing(static_cast<std::size_t>(w) * h, 0);
        mapdata::HeightPlane blank{};
        mapdata::build_plane(blank, nothing.data(), w, h);
        CHECK(blank.empty());
        CHECK_EQ(static_cast<long long>(blank.tiles), 0);
        CHECK_EQ(static_cast<long long>(blank.data.size()), 0);
        CHECK(blank.block(0, 0) == nullptr);
        mapdata::build_plane(blank, nullptr, w, h);
        CHECK(blank.empty());
        mapdata::build_plane(blank, nothing.data(), 0, h);
        CHECK(blank.empty());

        std::printf("  %dx%d reference: %d/%d blocks of %d px, %llu KB against %llu KB dense; "
                    "%lld rows gathered, %lld skipped\n",
                    w, h, p.tiles, p.ntx * p.nty, mapdata::kTilePx,
                    static_cast<unsigned long long>(p.bytes() / 1024),
                    static_cast<unsigned long long>(dense.size() * 2 / 1024), gathered_rows,
                    skipped_rows);
    }

    // The SHIPPED PNGs, through the runtime's own decode (src/pngdecode.hpp)
    // Two things maps.json cannot state:
    //   * the composite is readable as RGBA though it is PNG colour type 3 (256-colour palette)
    //     with a tRNS ARRAY, which WIC expands through its own palette;
    //   * the height codes are in range and in the right byte order. PNG stores 16-bit samples
    //     big-endian and WIC hands them back native-endian; 4095 byte-swapped is 65295.
    // Plus a cross-check: surface_hist[k] counts pixels with EXACTLY k surfaces, so plane z0's

    void test_map_assets(const std::string& markers_dir)
    {
        section("the shipped map PNGs, decoded through src/pngdecode.hpp");

        const std::string maps_dir = markers_dir + "/../maps";
        std::string text;
        if (!read_file(maps_dir + "/maps.json", text))
        {
            std::printf("  (skipped: %s/maps.json not readable)\n", maps_dir.c_str());
            return;
        }
        mapmanifest::Manifest m{};
        std::vector<std::string> problems;
        if (!mapmanifest::parse(text, m, problems) || m.chapters.empty())
        {
            std::printf("  (skipped: the manifest did not parse)\n");
            return;
        }

        // These per-chapter numbers are in maps.json but not in mapmanifest::Entry: a text scan.
        const auto number_in_chapter = [&text](const std::string& key, const char* field,
                                               double fallback) {
            const std::size_t at = text.find("\"" + key + "\"");
            if (at == std::string::npos)
            {
                return fallback;
            }
            const std::string needle = std::string("\"") + field + "\"";
            const std::size_t f = text.find(needle, at);
            if (f == std::string::npos)
            {
                return fallback;
            }
            const std::size_t colon = text.find(':', f + needle.size());
            if (colon == std::string::npos)
            {
                return fallback;
            }
            return std::strtod(text.c_str() + colon + 1, nullptr);
        };
        const auto sum_surface_hist_tail = [&text](const std::string& key) -> long long {
            const std::size_t at = text.find("\"" + key + "\"");
            const std::size_t h =
                at == std::string::npos ? std::string::npos : text.find("\"surface_hist\"", at);
            if (h == std::string::npos)
            {
                return -1;
            }
            const std::size_t open = text.find('[', h);
            const std::size_t close = text.find(']', open);
            if (open == std::string::npos || close == std::string::npos)
            {
                return -1;
            }
            long long total = 0;
            int index = 0;
            const char* p = text.c_str() + open + 1;
            const char* end = text.c_str() + close;
            while (p < end)
            {
                while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t'))
                {
                    ++p;
                }
                if (p >= end)
                {
                    break;
                }
                char* stop = nullptr;
                const long long v = std::strtoll(p, &stop, 10);
                if (stop == p)
                {
                    break;
                }
                if (index > 0) // hist[0] = pixels with NO surface
                {
                    total += v;
                }
                ++index;
                p = stop;
            }
            return total;
        };
        const auto widen = [](const std::string& narrow) {
            return std::wstring(narrow.begin(), narrow.end());
        };

        for (const mapmanifest::Entry& e : m.chapters)
        {
            std::vector<std::uint8_t> px;
            const std::wstring cpath = widen(maps_dir + "/" + e.image);
            const pngdec::Result cr = pngdec::decode(cpath.c_str(), pngdec::kRgba, px);
            ++g_checks;
            if (!cr.ok())
            {
                ++g_failures;
                std::printf("  FAIL  %s did not decode (0x%08X)\n", e.image.c_str(),
                            static_cast<unsigned>(cr.hr));
                continue;
            }
            CHECK_EQ(cr.width, e.image_width);
            CHECK_EQ(cr.height, e.image_height);
            CHECK_EQ(static_cast<long long>(px.size()),
                     static_cast<long long>(e.image_width) * e.image_height * 4);

            // Alpha is exactly two values, 0 and the fill alpha; a lost tRNS array shows up as one.
            int alpha_lo = 256;
            int alpha_hi = -1;
            std::size_t opaque = 0;
            std::size_t distinct_alpha = 0;
            bool seen[256] = {};
            for (std::size_t i = 3; i < px.size(); i += 4)
            {
                const int a = px[i];
                if (!seen[a])
                {
                    seen[a] = true;
                    ++distinct_alpha;
                }
                alpha_lo = a < alpha_lo ? a : alpha_lo;
                alpha_hi = a > alpha_hi ? a : alpha_hi;
                if (a != 0)
                {
                    ++opaque;
                }
            }
            CHECK_EQ(static_cast<long long>(distinct_alpha), 2);
            CHECK_EQ(alpha_lo, 0);
            CHECK_EQ(alpha_hi, 235); // build_map.py's FILL_ALPHA
            // All-transparent or all-opaque passes every check above except this one.
            const std::size_t total_px =
                static_cast<std::size_t>(e.image_width) * static_cast<std::size_t>(e.image_height);
            CHECK(opaque > total_px / 100 && opaque < total_px * 9 / 10);

            const int z_code_max = e.z_code_max;
            std::vector<std::uint8_t> raw;
            const std::wstring hpath = widen(maps_dir + "/" + e.height_maps[0]);
            const pngdec::Result hres = pngdec::decode(hpath.c_str(), pngdec::kGray16, raw);
            ++g_checks;
            if (!hres.ok())
            {
                ++g_failures;
                std::printf("  FAIL  %s did not decode (0x%08X)\n", e.height_maps[0].c_str(),
                            static_cast<unsigned>(hres.hr));
                continue;
            }
            CHECK_EQ(hres.width, e.image_width);
            CHECK_EQ(hres.height, e.image_height);

            const std::uint16_t* code = reinterpret_cast<const std::uint16_t*>(raw.data());
            const std::size_t n = raw.size() / 2;
            long long lit = 0;
            long long reached = 0;
            int code_hi = 0;
            int code_lo = 0x10000;
            bool stray_bits = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                const int c = mapdata::z_code(code[i]);
                if (c == 0)
                {
                    continue;
                }
                ++lit;
                reached += (code[i] & mapdata::kReachableBit) != 0;
                stray_bits |= (code[i] & ~(mapdata::kZCodeMask | mapdata::kReachableBit)) != 0;
                code_hi = c > code_hi ? c : code_hi;
                code_lo = c < code_lo ? c : code_lo;
            }
            CHECK(lit > 0);
            CHECK(code_hi <= z_code_max);
            CHECK(code_lo >= 1);
            CHECK(!stray_bits);
            // Bit 12 is set on a real subset of the lowest plane: the flood keeps the arenas
            // and drops the wall tops, so neither "all" nor "none" is a plausible /5 plane.
            if (e.has_reachability)
            {
                CHECK(reached > 0 && reached < lit);
            }
            else
            {
                CHECK_EQ(reached, 0LL);
            }
            // And the manifest's own histogram has to predict that count exactly.
            const long long want_lit = sum_surface_hist_tail(e.key);
            if (want_lit >= 0)
            {
                CHECK_EQ(lit, want_lit);
            }

            // The Z the runtime reads back lands inside the manifest's [z_min, z_max].
            const double step = (e.z_max - e.z_min) / static_cast<double>(z_code_max - 1);
            const double z_lo = e.z_min + (code_lo - 1) * step;
            const double z_hi = e.z_min + (code_hi - 1) * step;
            CHECK(z_lo >= e.z_min - 1.0 && z_hi <= e.z_max + 1.0);

            // The requantisation error over every lit pixel. The slicer's floor tolerance is 200 uu,
            // so anything above 20 uu is a format change that has stopped being free.
            const double shift = number_in_chapter(e.key, "z_requantise_worst_uu", 0.0);
            CHECK(shift < 20.0);

            // What the runtime allocates: the pipeline's count of non-empty 128-px tiles.
            const double tiles = number_in_chapter(e.key, "height_tiles_128", 0.0);
            const double tile_ram = number_in_chapter(e.key, "height_tile_ram_bytes", 0.0);
            const double dense_ram = number_in_chapter(e.key, "height_map_raw_bytes", 0.0);
            CHECK(tiles > 0.0);
            CHECK_NEAR(tile_ram, tiles * 128.0 * 128.0 * 2.0, 1.0);
            CHECK(tile_ram > 0.0 && tile_ram <= 100.0 * 1024.0 * 1024.0);
            CHECK(dense_ram > 3.0 * tile_ram); // the whole point of the tile store

            // End to end on the FIRST chapter: every shipped plane through pngdec + build_plane.
            //   1. The block count the runtime allocates is the one the pipeline measured.
            //   2. gather_row() over the real asset answers what the dense buffer it came from holds.
            if (&e == &m.chapters.front())
            {
                mapdata::HeightMaps hm{};
                hm.width = e.image_width;
                hm.height = e.image_height;
                hm.z_min = static_cast<float>(e.z_min);
                hm.z_max = static_cast<float>(e.z_max);
                hm.z_code_max = e.z_code_max;
                long long tiles_built = 0;
                long long mismatches = 0;
                long long sampled = 0;
                std::vector<int> col_x(static_cast<std::size_t>(hm.width));
                for (int i = 0; i < hm.width; ++i)
                {
                    col_x[static_cast<std::size_t>(i)] = i;
                }
                std::vector<std::uint16_t> got(static_cast<std::size_t>(hm.width), 0);
                for (std::size_t k = 0; k < e.height_maps.size() && k < mapdata::kMaxSurfaces; ++k)
                {
                    std::vector<std::uint8_t> plane_raw;
                    const std::wstring pp = widen(maps_dir + "/" + e.height_maps[k]);
                    const pngdec::Result pr = pngdec::decode(pp.c_str(), pngdec::kGray16, plane_raw);
                    ++g_checks;
                    if (!pr.ok() || pr.width != hm.width || pr.height != hm.height)
                    {
                        ++g_failures;
                        std::printf("  FAIL  %s did not decode to %dx%d\n",
                                    e.height_maps[k].c_str(), hm.width, hm.height);
                        break;
                    }
                    const std::uint16_t* src =
                        reinterpret_cast<const std::uint16_t*>(plane_raw.data());
                    mapdata::build_plane(hm.layer[k], src, hm.width, hm.height);
                    hm.count = static_cast<int>(k + 1);
                    tiles_built += hm.layer[k].tiles;

                    // 37 rows, prime-strided so the sample is not aligned to the 128-px block grid.
                    for (int row = 0; row < 37; ++row)
                    {
                        const int sy = (row * 4093) % hm.height;
                        const bool any =
                            hm.gather_row(static_cast<int>(k), sy, col_x.data(), hm.width, got.data());
                        bool want_any = false;
                        const std::uint16_t* want_row =
                            src + static_cast<std::size_t>(sy) * static_cast<std::size_t>(hm.width);
                        for (int x = 0; x < hm.width; ++x)
                        {
                            want_any = want_any || want_row[x] != 0;
                            if (got[static_cast<std::size_t>(x)] != want_row[x])
                            {
                                ++mismatches;
                            }
                        }
                        if (any != want_any)
                        {
                            ++mismatches;
                        }
                        ++sampled;
                    }
                }
                CHECK_EQ(mismatches, 0);
                CHECK(sampled > 0);
                CHECK_EQ(tiles_built, static_cast<long long>(tiles));
                // `height_tile_ram_bytes` is the block PAYLOAD; the runtime also holds one int32 per block
                // slot as the index, which is what makes the two numbers differ.
                const double tiles_total = number_in_chapter(e.key, "height_tiles_128_total", 0.0);
                CHECK(tiles_total > 0.0);
                CHECK_EQ(static_cast<long long>(hm.bytes()),
                         static_cast<long long>(tile_ram + tiles_total * 4.0));
                CHECK_EQ(static_cast<long long>(hm.dense_bytes()),
                         static_cast<long long>(dense_ram));
                std::printf("    %s end to end: %d plane(s) built, %lld block(s) = %llu MB "
                            "(manifest says %.0f blocks / %.0f MB), %lld row(s) gathered "
                            "byte-for-byte\n",
                            e.key.c_str(), hm.count, tiles_built,
                            static_cast<unsigned long long>(hm.bytes() / (1024 * 1024)), tiles,
                            tile_ram / (1024.0 * 1024.0), sampled);
            }

            std::printf("  %s: composite %dx%d, %llu opaque px (%.0f %%), z0 %lld lit px, "
                        "codes %d..%d of %d, step %.2f uu, requantise shift %.2f uu, "
                        "%.0f tiles = %.0f MB resident against %.0f MB dense\n",
                        e.key.c_str(), cr.width, cr.height,
                        static_cast<unsigned long long>(opaque),
                        100.0 * static_cast<double>(opaque) / static_cast<double>(total_px), lit,
                        code_lo, code_hi, z_code_max, step, shift, tiles,
                        tile_ram / (1024.0 * 1024.0), dense_ram / (1024.0 * 1024.0));
        }
    }

    // The config file's keys: shipped file == known keys == what the parser accepts
    // The shipped file, the config_keys.hpp tiers and the parser's own set are compared both
    // ways; the parser's set is SCRAPED from src/mmstate.cpp (`key == "..."`).

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
        for (const std::string& key : a)
        {
            const bool found = std::find(b.begin(), b.end(), key) != b.end();
            const std::string msg = std::string{what} + ": " + key;
            check(found, msg.c_str(), __FILE__, __LINE__);
        }
    }

    // mm::Config equality is COMPLETE
    // The F2 panel and the full map publish a config copy only if it differs, so a field missing
    // from the field-by-field operator== is a setting whose slider does nothing.
    // The guard needs no field list: fill two Configs with a byte pattern, flip each byte of one
    // in turn and require operator== to notice. Only PADDING bytes are invisible to it.
    // The pattern is 0x01: every float and double in it is a small NORMAL number, every bool 1.
    void test_config_equality()
    {
        section("mm::Config equality is complete");

        // PADDING bytes in mm::Config. A failure means either a field was added to the struct and
        // not to operator==, or the layout changed and the new count belongs here with a note.
        constexpr std::size_t kPaddingBytes = 75;

        mm::Config a{};
        mm::Config b{};
        CHECK(a == b);
        CHECK(!(a != b));

        unsigned char* const pa = reinterpret_cast<unsigned char*>(&a);
        unsigned char* const pb = reinterpret_cast<unsigned char*>(&b);
        std::memset(pa, 0x01, sizeof(mm::Config));
        std::memset(pb, 0x01, sizeof(mm::Config));
        CHECK(a == b);

        std::vector<std::size_t> invisible;
        for (std::size_t i = 0; i < sizeof(mm::Config); ++i)
        {
            pb[i] = static_cast<unsigned char>(pb[i] ^ 0xFF);
            const bool seen = !(a == b);
            pb[i] = static_cast<unsigned char>(pb[i] ^ 0xFF);
            if (!seen)
            {
                invisible.push_back(i);
            }
        }
        CHECK(a == b); // every flip undone

        if (invisible.size() != kPaddingBytes)
        {
            std::printf("    mm::Config is %d bytes; operator== cannot see %d of them",
                        static_cast<int>(sizeof(mm::Config)), static_cast<int>(invisible.size()));
            for (std::size_t i = 0; i < invisible.size() && i < 16; ++i)
            {
                std::printf("%s+%d", i == 0 ? " (offsets " : ", ", static_cast<int>(invisible[i]));
            }
            std::printf("%s\n", invisible.empty() ? "" : ")");
            std::printf("    -> a field was probably added to mm::Config and not to mm::operator==\n");
        }
        check(invisible.size() == kPaddingBytes, "every byte of mm::Config is compared by operator==",
              __FILE__, __LINE__);
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

        // The shipped x-ray set and the compiled default must agree and both carry the readable
        // notes. The shipped file WINS over the compiled default, so both halves are pinned.
        {
            std::string header;
            const bool have_header = read_file(root + "/src/mmstate.hpp", header);
            CHECK(have_header);
            CHECK(shipped.find("highlight_categories = chest,pickup,shrine,boss,npc,note") !=
                  std::string::npos);
            CHECK(header.find("mdb::cat_bit(mdb::Cat::Npc) | mdb::cat_bit(mdb::Cat::Note);") !=
                  std::string::npos);
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

        // Every set comparison below would pass by accident if a key were in two tiers.
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

        report_missing("in the shipped config but not a Player/Advanced key", shipped_keys, known);
        report_missing("a Player/Advanced key missing from the shipped config", known, shipped_keys);

        report_missing("in the dev config but not a Dev key", dev_file_keys, dev_keys);
        report_missing("a Dev key missing from the dev config", dev_keys, dev_file_keys);

        // A Removed key must NOT be a parser literal: cfgkeys::is_removed() answers it with a warning.
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

        // Every key above the `; ---- ADVANCED ----` banner is Player, every key below it Advanced.
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

    // Saving: rewriting the VALUES in a config file and nothing else
    // A save that changes no value must produce the file BYTE FOR BYTE.

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
            // ...and appending it twice does not happen: feeding the OUTPUT back in is a fixed point.
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
            // Duplicate lines for one key: the loader lets the last win, so BOTH are rewritten.
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

        for (const char* name : {"config_wuchang_minimap.txt", "config_wuchang_minimap_dev.txt"})
        {
            std::string text;
            if (!read_file(dir + name, text))
            {
                std::printf("  SKIPPED %s (run from the repo)\n", name);
                continue;
            }
            // Rewriting every key with the value it already has gives the file back unchanged.
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

    // Absence as evidence of a collect
    // The one auto-mark that fires on something NOT being there; every condition on its own.

    // Item quality ("rarity")
    // The JSON field is optional and defaults to 0; the palette parser survives whatever a
    // player types, and the round-trip is exact or the F2 panel's Save rewrites the colours.

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

        // A bad entry is reported, keeps its own tier's old value, and does not shift later colours.
        defaults(pal);
        rejected.clear();
        CHECK_EQ(mdb::parse_rarity_colors("112233, nope, 445566", pal, &rejected), 2);
        CHECK_STR(rejected, "nope");
        CHECK(pal[0] == (mdb::Rgb{0x11, 0x22, 0x33}));
        CHECK(pal[1] == mdb::kDefaultRarityColors[1]);
        CHECK(pal[2] == (mdb::Rgb{0x44, 0x55, 0x66}));

        // Empty text, and more entries than there are tiers, both change nothing beyond what fits.
        defaults(pal);
        CHECK_EQ(mdb::parse_rarity_colors("", pal, nullptr), 0);
        CHECK(pal[1] == mdb::kDefaultRarityColors[1]);
        CHECK_EQ(mdb::parse_rarity_colors("000, 111, 222, 333, 444", pal, nullptr), 3);
        CHECK(pal[2] == (mdb::Rgb{0x22, 0x22, 0x22}));

        defaults(pal);
        rejected.clear();
        CHECK_EQ(mdb::parse_rarity_colors("1234, 12345678, ABCDE", pal, &rejected), 0);
        CHECK_STR(rejected, "1234,12345678,ABCDE");
        for (int i = 0; i < mdb::kRarityCount; ++i)
        {
            CHECK(pal[i] == mdb::kDefaultRarityColors[i]);
        }

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
        // DT_Particle LightColor of PickupEffect / PickupEffect4 / PickupEffect7, linear -> sRGB.
        CHECK(mdb::kDefaultRarityColors[0] == (mdb::Rgb{0xAD, 0xAF, 0xDA}));
        CHECK(mdb::kDefaultRarityColors[1] == (mdb::Rgb{0xDA, 0xAD, 0xC5}));
        CHECK(mdb::kDefaultRarityColors[2] == (mdb::Rgb{0xDA, 0xD6, 0xAD}));
    }

    // The real database must carry tiers.
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
        // Both non-default tiers occur in chapter 1; "some of each" is the invariant.
        CHECK(per_tier[1] > 0);
        CHECK(per_tier[2] > 0);
        CHECK(per_tier[0] > per_tier[1] + per_tier[2]);
    }

    // Data invariants over EVERY shipped markers/*.json
    // Properties of the DATA, not of the loader: floors, not exact counts.

    struct ChapterFile
    {
        const char* file;
        const char* label;
        int chapter;
        int min_markers;
    };

    // The six shipped chapters. Floors are ~85 % of the extracted counts.
    constexpr ChapterFile kChapterFiles[] = {
        {"chapter1.json", "1", 1, 780},
        {"chapter2.json", "2", 2, 770},
        {"chapter3.json", "3", 3, 630},
        {"chapter4.json", "4", 4, 290},
        {"chapter5.json", "5", 5, 305},
        {"chapterdlc.json", "DLC", 0, 310},
    };

    // Per-(chapter, category) floors. Only the categories that must not vanish are listed; one
    // genuinely absent from a chapter is listed as 0 with the reason.
    struct CatFloor
    {
        int chapter;      // 0 = DLC
        mdb::Cat cat;
        int least;
    };

    constexpr CatFloor kCatFloors[] = {
        // shrines: the game's own rest-and-travel network. Losing one loses a travel point.
        {1, mdb::Cat::Shrine, 12}, {2, mdb::Cat::Shrine, 12},
        {3, mdb::Cat::Shrine, 12}, {4, mdb::Cat::Shrine, 9},
        {5, mdb::Cat::Shrine, 5},  {0, mdb::Cat::Shrine, 7},
        // bosses: 28 in the game, every one of them authored.
        {1, mdb::Cat::Boss, 9}, {2, mdb::Cat::Boss, 5}, {3, mdb::Cat::Boss, 6},
        {4, mdb::Cat::Boss, 5}, {5, mdb::Cat::Boss, 2}, {0, mdb::Cat::Boss, 1},
        // pickups and chests: the collection tracker's whole content.
        {1, mdb::Cat::Pickup, 250}, {2, mdb::Cat::Pickup, 250},
        {3, mdb::Cat::Pickup, 220}, {4, mdb::Cat::Pickup, 90},
        {5, mdb::Cat::Pickup, 65},  {0, mdb::Cat::Pickup, 65},
        {1, mdb::Cat::Chest, 10}, {2, mdb::Cat::Chest, 16},
        {3, mdb::Cat::Chest, 13}, {4, mdb::Cat::Chest, 9},
        {5, mdb::Cat::Chest, 2},  {0, mdb::Cat::Chest, 8},
        // navigation aids. Chapter 5 has no ladder and chapters 4 / DLC no lift: the class table is
        // BP_LadderV2_C + BP_InteractionLadder_C and BP_ElevatorBase_C + BP_ElevatorBox_C.
        {1, mdb::Cat::Ladder, 28}, {2, mdb::Cat::Ladder, 38},
        {3, mdb::Cat::Ladder, 32}, {4, mdb::Cat::Ladder, 4},
        {5, mdb::Cat::Ladder, 0},  {0, mdb::Cat::Ladder, 2},
        {1, mdb::Cat::Lift, 6}, {2, mdb::Cat::Lift, 7}, {3, mdb::Cat::Lift, 1},
        {4, mdb::Cat::Lift, 0}, {5, mdb::Cat::Lift, 14}, {0, mdb::Cat::Lift, 0},
        // a floor is the only thing that keeps these two produced.
        {1, mdb::Cat::Elite, 3}, {2, mdb::Cat::Elite, 2}, {3, mdb::Cat::Elite, 4},
        {4, mdb::Cat::Elite, 5}, {5, mdb::Cat::Elite, 12}, {0, mdb::Cat::Elite, 24},
        {1, mdb::Cat::Hidden, 4}, {2, mdb::Cat::Hidden, 8},
        {3, mdb::Cat::Hidden, 3}, {4, mdb::Cat::Hidden, 4},
        {5, mdb::Cat::Hidden, 3},
        {1, mdb::Cat::Enemy, 330}, {2, mdb::Cat::Enemy, 350},
        {3, mdb::Cat::Enemy, 240}, {4, mdb::Cat::Enemy, 135},
        {5, mdb::Cat::Enemy, 180}, {0, mdb::Cat::Enemy, 200},
        {1, mdb::Cat::Npc, 44}, {2, mdb::Cat::Npc, 30}, {3, mdb::Cat::Npc, 38},
        {4, mdb::Cat::Npc, 14}, {5, mdb::Cat::Npc, 4},
        {1, mdb::Cat::FogGate, 17}, {2, mdb::Cat::FogGate, 8},
        {3, mdb::Cat::FogGate, 10}, {4, mdb::Cat::FogGate, 3},
        {5, mdb::Cat::FogGate, 2},  {0, mdb::Cat::FogGate, 2},
        {1, mdb::Cat::Door, 20}, {2, mdb::Cat::Door, 14}, {3, mdb::Cat::Door, 8},
        {4, mdb::Cat::Door, 2},  {5, mdb::Cat::Door, 1},  {0, mdb::Cat::Door, 2},
        {1, mdb::Cat::Note, 30}, {2, mdb::Cat::Note, 13}, {3, mdb::Cat::Note, 20},
        {4, mdb::Cat::Note, 5},  {5, mdb::Cat::Note, 9},
    };

    // The generic label `tools/markers/marker_classes.LABEL` writes when nothing better is
    // known. NOT `mdb::cat_word()`, so both spellings are accepted.
    bool is_generic_name(mdb::Cat cat, const std::string& name)
    {
        static const char* kGeneric[mdb::kCatCount][2] = {
            {"Shrine", "Shrine"},   {"Chest", "Chest"},   {"Pickup", "Item"},
            {"Boss", "Boss"},       {"Elite", "Elite"},   {"Enemy", "Enemy"},
            {"NPC", "NPC"},         {"Note", "Note"},     {"Door", "Door"},
            {"Ladder", "Ladder"},   {"Lift", "Lift"},     {"Fog gate", "Fog gate"},
            {"Trap", "Hidden item"}, {"Object", "Marker"},
        };
        const int i = static_cast<int>(cat);
        if (i < 0 || i >= mdb::kCatCount)
        {
            return true;
        }
        if (name == kGeneric[i][0] || name == kGeneric[i][1])
        {
            return true;
        }
        // A shrine with no `DT_FirePoint` name keeps the extractor's "Shrine <fire-point id>" form.
        if (cat == mdb::Cat::Shrine && name.rfind("Shrine ", 0) == 0)
        {
            return true;
        }
        return name.empty();
    }

    // Every `"NNNNN": {` key in markers/items.json. A text scan: an item DESCRIPTION cannot
    // contain `": {`.
    std::vector<int> read_item_ids(const std::string& text)
    {
        std::vector<int> out;
        for (std::size_t i = 0; i + 3 < text.size(); ++i)
        {
            if (text[i] != '"')
            {
                continue;
            }
            std::size_t j = i + 1;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9')
            {
                ++j;
            }
            if (j == i + 1 || j >= text.size() || text[j] != '"')
            {
                continue;
            }
            std::size_t k = j + 1;
            if (k >= text.size() || text[k] != ':')
            {
                continue;
            }
            ++k;
            while (k < text.size() && (text[k] == ' ' || text[k] == '\t'))
            {
                ++k;
            }
            if (k < text.size() && text[k] == '{')
            {
                out.push_back(std::atoi(text.substr(i + 1, j - i - 1).c_str()));
            }
            i = j;
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Every integer inside every `"items": [ ... ]` array; `StaticMarker` drops the id list.
    std::vector<int> read_marker_item_ids(const std::string& text)
    {
        std::vector<int> out;
        const std::string key = "\"items\"";
        std::size_t at = 0;
        while ((at = text.find(key, at)) != std::string::npos)
        {
            std::size_t p = text.find('[', at);
            const std::size_t end = text.find(']', at);
            at += key.size();
            if (p == std::string::npos || end == std::string::npos || p > end)
            {
                continue;
            }
            for (++p; p < end; ++p)
            {
                if (text[p] < '0' || text[p] > '9')
                {
                    continue;
                }
                std::size_t q = p;
                while (q < end && text[q] >= '0' && text[q] <= '9')
                {
                    ++q;
                }
                out.push_back(std::atoi(text.substr(p, q - p).c_str()));
                p = q;
            }
            at = end;
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    void test_data_invariants(const std::string& markers_dir)
    {
        section("data invariants over every shipped markers/*.json");

        std::string items_text;
        const bool have_items = read_file(markers_dir + "/items.json", items_text);
        const std::vector<int> item_ids = have_items ? read_item_ids(items_text) : std::vector<int>{};
        if (have_items)
        {
            // A floor over the six item DataTables, because a game patch may add items.
            CHECK(item_ids.size() > 2000);
        }

        std::string shrines_text;
        std::vector<shdb::Shrine> shrine_rows;
        if (read_file(markers_dir + "/shrines.json", shrines_text))
        {
            shdb::Report sr{};
            CHECK(shdb::parse(shrines_text, shrine_rows, sr));
        }

        std::vector<std::string> all_ids;
        int files_seen = 0;
        int name_cap_violations = 0;
        int unknown_items = 0;
        int missing_shrine_rows = 0;

        for (const ChapterFile& cf : kChapterFiles)
        {
            const std::string path = markers_dir + "/" + cf.file;
            std::string text;
            if (!read_file(path, text))
            {
                std::printf("  SKIP  %s not readable\n", path.c_str());
                continue;
            }
            ++files_seen;

            std::vector<mdb::StaticMarker> db;
            mdb::ParseReport rep{};
            CHECK(mdb::parse_markers_json(text, db, rep));
            CHECK_STR(rep.error, "");
            CHECK_STR(rep.schema, "wuchang-minimap-markers/1");
            CHECK_STR(rep.chapter_label, cf.label);
            CHECK_EQ(rep.chapter, cf.chapter);
            // A skipped entry is a marker the generator wrote and the loader threw away.
            CHECK_EQ(rep.skipped, 0);
            CHECK_EQ(rep.unknown_cat, 0);
            CHECK_EQ(rep.legacy_cat, 0);
            CHECK(static_cast<int>(rep.added) >= cf.min_markers);

            for (int c = 0; c < mdb::kCatCount; ++c)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(c);
                std::vector<std::string> named;
                int total = 0;
                for (const mdb::StaticMarker& m : db)
                {
                    if (m.cat != cat)
                    {
                        continue;
                    }
                    ++total;
                    CHECK_EQ(m.chapter, cf.chapter);
                    CHECK(!m.id.empty());
                    // The world is a few hundred thousand uu across; a drifted decode gives 1e38 or 1e-317.
                    CHECK(std::fabs(m.x) < 1.0e7 && std::fabs(m.y) < 1.0e7 && std::fabs(m.z) < 1.0e7);
                    if (!is_generic_name(cat, m.name))
                    {
                        named.push_back(m.name);
                    }
                }
                for (const CatFloor& f : kCatFloors)
                {
                    if (f.chapter == cf.chapter && f.cat == cat && total < f.least)
                    {
                        std::printf("  FAIL  chapter %s: %d %s marker(s), floor is %d\n",
                                    cf.label, total, mdb::cat_name(cat), f.least);
                        ++g_failures;
                        ++g_checks;
                    }
                }

                // Among the entries that carry a REAL name, no single name may account for more than half -
                // an unconfigured pickup carries the first row of DT_Item_ToolTable. Generic labels excluded.
                if (named.size() >= 8)
                {
                    std::sort(named.begin(), named.end());
                    std::size_t best = 0;
                    std::size_t run = 0;
                    for (std::size_t i = 0; i < named.size(); ++i)
                    {
                        run = (i > 0 && named[i] == named[i - 1]) ? run + 1 : 1;
                        best = run > best ? run : best;
                    }
                    if (best * 2 > named.size())
                    {
                        std::printf("  FAIL  chapter %s %s: %zu of %zu named entries share one "
                                    "name (\"%s\")\n", cf.label, mdb::cat_name(cat), best,
                                    named.size(), named[0].c_str());
                        ++name_cap_violations;
                    }
                    ++g_checks;
                }
            }

            // Every item id a pickup grants must be a row of the item database.
            if (have_items)
            {
                for (int id : read_marker_item_ids(text))
                {
                    if (!std::binary_search(item_ids.begin(), item_ids.end(), id))
                    {
                        std::printf("  FAIL  %s references item %d, which is not in items.json\n",
                                    cf.file, id);
                        ++unknown_items;
                    }
                }
                ++g_checks;
            }

            // Every shrine marker must have a row in the shrine table: it backs the full map's Shrines
            // panel and the save's unlocked-id join.
            if (!shrine_rows.empty())
            {
                for (const mdb::StaticMarker& m : db)
                {
                    if (m.cat == mdb::Cat::Shrine && shdb::find_id(shrine_rows, m.id) < 0)
                    {
                        std::printf("  FAIL  %s: shrine %s has no row in shrines.json\n",
                                    cf.file, m.id.c_str());
                        ++missing_shrine_rows;
                    }
                }
                ++g_checks;
            }

            for (const mdb::StaticMarker& m : db)
            {
                all_ids.push_back(m.id);
            }
        }

        CHECK_EQ(files_seen, static_cast<int>(sizeof(kChapterFiles) / sizeof(kChapterFiles[0])));
        CHECK_EQ(name_cap_violations, 0);
        CHECK_EQ(unknown_items, 0);
        CHECK_EQ(missing_shrine_rows, 0);

        // No duplicate id within a file OR across files: the loader globs every markers/*.json into
        // ONE database keyed by id, and by-id keeps only the first index.
        std::sort(all_ids.begin(), all_ids.end());
        int dupes = 0;
        for (std::size_t i = 1; i < all_ids.size(); ++i)
        {
            if (all_ids[i] == all_ids[i - 1])
            {
                ++dupes;
                if (dupes <= 5)
                {
                    std::printf("  FAIL  duplicate marker id across markers/*.json: %s\n",
                                all_ids[i].c_str());
                }
            }
        }
        CHECK_EQ(dupes, 0);
        CHECK(all_ids.size() > 3000);
        std::printf("  %d file(s), %zu marker(s), %zu item id(s), %zu shrine row(s)\n",
                    files_seen, all_ids.size(), item_ids.size(), shrine_rows.size());
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

        CHECK(mdb::absence_round_confirms(all_true()));

        // Each condition alone is enough to refuse. An unmatched level, or one that streamed in
        // mid-round, must never mark: an unloaded level and a collected pickup look identical.
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
        // A nonsensical requirement never marks, without leaning on the config clamp.
        CHECK(!mdb::absence_marks(ok, 1, 0));
        CHECK(!mdb::absence_marks(ok, 1, -3));

        // The runtime's state machine simulated: a marker whose level loads at round 4, unseen from
        // round 5, marked on the second confirming round, then a live twin resets the streak.
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

        section("hide-found on the map surfaces");

        {
            // The bug this rule exists for: a lit shrine is FOUND, and hiding it takes the
            // checkpoints out of an explored area. Landmarks are exempt, loot is not.
            CHECK(!mdb::hidden_as_found(mdb::Cat::Shrine, true, true));
            CHECK(!mdb::hidden_as_found(mdb::Cat::Shrine, false, true));
            CHECK(mdb::hidden_as_found(mdb::Cat::Chest, true, true));
            CHECK(!mdb::hidden_as_found(mdb::Cat::Chest, true, false));
            CHECK(!mdb::hidden_as_found(mdb::Cat::Chest, false, true));

            // Only the shrine is a landmark; every other category hides when found.
            for (int c = 0; c < mdb::kCatCount; ++c)
            {
                const auto cat = static_cast<mdb::Cat>(c);
                CHECK_EQ(mdb::hidden_as_found(cat, true, true), cat != mdb::Cat::Shrine);
                CHECK(!mdb::hidden_as_found(cat, false, true));
                CHECK(!mdb::hidden_as_found(cat, true, false));

                // Hollow is the found look everywhere but a shrine, where it is the UNLIT look.
                CHECK_EQ(mdb::drawn_as_found(cat, true), cat != mdb::Cat::Shrine);
                CHECK_EQ(mdb::drawn_as_found(cat, false), cat == mdb::Cat::Shrine);
            }
        }

        section("which gate drops a marker from the x-ray");

        {
            // A chest 11 m away, in the x-ray set, not collected: it must be DRAWN.
            mdb::XrayFacts f{};
            f.cat = mdb::Cat::Chest;
            f.cat_selected = true;
            f.within_radius = true;
            CHECK(mdb::xray_gate(f) == mdb::XrayDrop::Drawn);

            // Each way it can be dropped, one at a time, so an extra condition cannot hide inside one.
            mdb::XrayFacts g = f;
            g.cat_selected = false;
            CHECK(mdb::xray_gate(g) == mdb::XrayDrop::Category);

            g = f;
            g.within_radius = false;
            CHECK(mdb::xray_gate(g) == mdb::XrayDrop::Radius);

            g = f;
            g.found = true; // an opened chest, with "hide collected loot" on
            CHECK(mdb::xray_gate(g) == mdb::XrayDrop::Found);
            g.show_found = true;
            CHECK(mdb::xray_gate(g) == mdb::XrayDrop::Drawn);

            // Found only hides loot and a defeated boss: a lit shrine, a met NPC and a read note stay.
            for (int c = 0; c < mdb::kCatCount; ++c)
            {
                const auto cat = static_cast<mdb::Cat>(c);
                mdb::XrayFacts h{};
                h.cat = cat;
                h.cat_selected = true;
                h.within_radius = true;
                h.found = true;
                h.live = true; // so the people rule is not what answers
                const bool expect_hidden = mdb::is_loot_cat(cat) || cat == mdb::Cat::Boss;
                CHECK_EQ(mdb::xray_gate(h) == mdb::XrayDrop::Found, expect_hidden);
            }

            // A person is highlighted where they stand or not at all; no other category needs live.
            for (int c = 0; c < mdb::kCatCount; ++c)
            {
                const auto cat = static_cast<mdb::Cat>(c);
                mdb::XrayFacts h{};
                h.cat = cat;
                h.cat_selected = true;
                h.within_radius = true;
                h.live = false;
                CHECK_EQ(mdb::xray_gate(h) == mdb::XrayDrop::Live, mdb::is_mobile_category(cat));
            }

            // Order matters for the DIAGNOSTIC: a marker failing several gates is reported under the
            // first, so the counters add up to the published total.
            g = f;
            g.cat_selected = false;
            g.within_radius = false;
            g.found = true;
            CHECK(mdb::xray_gate(g) == mdb::XrayDrop::Category);

            CHECK_STR(mdb::xray_drop_name(mdb::XrayDrop::Category), "category");
            CHECK_STR(mdb::xray_drop_name(mdb::XrayDrop::Found), "found");
            CHECK_STR(mdb::xray_drop_name(mdb::XrayDrop::Live), "not live");
            CHECK_STR(mdb::xray_drop_name(mdb::XrayDrop::Radius), "radius");
            CHECK_STR(mdb::xray_drop_name(mdb::XrayDrop::Drawn), "drawn");
        }

        section("no user-facing label is ever a class name");

        // The shapes a class name takes in this game.
        CHECK(mdb::looks_like_class_name("BP_PickupActor_C"));
        CHECK(mdb::looks_like_class_name("BP_DropItem_C"));
        CHECK(mdb::looks_like_class_name("BP_treasurebox_C"));
        CHECK(mdb::looks_like_class_name("DKDC_NPC_C"));
        CHECK(mdb::looks_like_class_name("Impl_BaseAIController_C"));
        CHECK(mdb::looks_like_class_name("ItemCollectionBox_C"));
        CHECK(mdb::looks_like_class_name("pickup_actor"));
        CHECK(mdb::looks_like_class_name("  BP_Wumen_C  ")); // trimmed first

        // Real display names all survive: an apostrophe, a '+1' suffix, a two-word category label
        // used as a name, a proper noun.
        CHECK(!mdb::looks_like_class_name("Cloudfrost's Edge"));
        CHECK(!mdb::looks_like_class_name("Cloudfrost's Edge +1"));
        CHECK(!mdb::looks_like_class_name("Blood of Wangdi"));
        CHECK(!mdb::looks_like_class_name("Huang Jian'e"));
        CHECK(!mdb::looks_like_class_name("Note"));
        CHECK(!mdb::looks_like_class_name("Fog gate"));
        CHECK(!mdb::looks_like_class_name("Commander Honglan"));
        CHECK(!mdb::looks_like_class_name(""));    // no label, not a class name
        CHECK(!mdb::looks_like_class_name("Ash")); // three letters, no underscore

        // Every category has a non-empty singular word, and none is itself a class name.
        for (int c = 0; c < mdb::kCatCount; ++c)
        {
            const auto cat = static_cast<mdb::Cat>(c);
            const char* w = mdb::cat_word(cat);
            CHECK(w != nullptr && w[0] != '\0');
            CHECK(!mdb::looks_like_class_name(w));
        }

        // display_label: a real name passes through; empty and class names become the category word.
        CHECK_STR(mdb::display_label(mdb::Cat::Pickup, "Blood of Wangdi"), "Blood of Wangdi");
        CHECK_STR(mdb::display_label(mdb::Cat::Pickup, "BP_PickupActor_C"), "Item");
        CHECK_STR(mdb::display_label(mdb::Cat::Pickup, ""), "Item");
        CHECK_STR(mdb::display_label(mdb::Cat::Pickup, nullptr), "Item");
        CHECK_STR(mdb::display_label(mdb::Cat::Enemy, "Impl_BaseAIController_C"), "Enemy");
        CHECK_STR(mdb::display_label(mdb::Cat::Note, "DKDC_NPC_C"), "Note");
        CHECK_STR(mdb::display_label(mdb::Cat::Npc, "BP_NPC_C"), "NPC");
        CHECK_STR(mdb::display_label(mdb::Cat::Chest, "BP_treasurebox_C"), "Chest");

        section("markers/items.json at runtime (names for loot an enemy drops)");

        {
            std::unordered_map<int, std::string> names;
            std::string err;
            CHECK(mdb::parse_items_json(
                R"({"schema":"wuchang-minimap-items/2","items":{
                     "20001":{"name":"Ancient Chisel","des":"long text"},
                     "22107":{"name":"Faint Red Feather"},
                     "10000":{"name":"Cloudfrost's Edge","rarity":1}}})",
                names, err));
            CHECK_STR(err.c_str(), "");
            CHECK_EQ(static_cast<int>(names.size()), 3);
            CHECK_STR(names[20001].c_str(), "Ancient Chisel");
            CHECK_STR(names[10000].c_str(), "Cloudfrost's Edge");

            // Schema /1 is accepted too: this reader only wants {id -> name}, which both minors carry.
            CHECK(mdb::parse_items_json(R"({"schema":"wuchang-minimap-items/1","items":{"1":{"name":"x"}}})",
                                        names, err));
            CHECK(!mdb::parse_items_json(R"({"schema":"wuchang-minimap-markers/1","items":{}})", names, err));
            CHECK(!mdb::parse_items_json(R"({"schema":"wuchang-minimap-items/2"})", names, err));
            CHECK(!mdb::parse_items_json("not json", names, err));
            // A named entry is required: an empty table would silently disable the feature.
            CHECK(!mdb::parse_items_json(R"({"schema":"wuchang-minimap-items/2","items":{}})", names, err));
            CHECK(mdb::parse_items_json(
                R"({"schema":"wuchang-minimap-items/2","items":{
                     "notanid":{"name":"x"},"20002":{"des":"no name"},"20003":{"name":""},
                     "20004":{"name":"Real"}}})",
                names, err));
            CHECK_EQ(static_cast<int>(names.size()), 1);
            CHECK_STR(names[20004].c_str(), "Real");
        }

        section("is this character dead? (the three-way health answer)");

        // A read that did not answer is UNKNOWN, never dead: guessing dead erases living enemies.
        CHECK(mdb::health_answer(false, 0.0, 100.0) == mdb::Health::Unknown);
        CHECK(mdb::health_answer(false, 50.0, 100.0) == mdb::Health::Unknown);

        CHECK(mdb::health_answer(true, 0.0, 100.0) == mdb::Health::Dead);
        CHECK(mdb::health_answer(true, -3.0, 100.0) == mdb::Health::Dead);
        CHECK(mdb::health_answer(true, 1.0, 100.0) == mdb::Health::Alive);
        CHECK(mdb::health_answer(true, 269.1, 269.1) == mdb::Health::Alive);

        // Max <= 0 is an uninitialised or hot-swapped stat component, not a corpse.
        CHECK(mdb::health_answer(true, 0.0, 0.0) == mdb::Health::Unknown);
        CHECK(mdb::health_answer(true, 0.0, -1.0) == mdb::Health::Unknown);

        // Garbage from a misaligned 8-byte read (~1e-317 / ~1e-299), NaN and the infinities are
        // UNKNOWN. A denormal CURRENT with a sane MAX is a live character, so only the bound rejects.
        CHECK(mdb::health_answer(true, 1e-317, 1e-299) == mdb::Health::Alive);
        {
            const double nan_v = std::numeric_limits<double>::quiet_NaN();
            const double inf_v = std::numeric_limits<double>::infinity();
            CHECK(mdb::health_answer(true, nan_v, 100.0) == mdb::Health::Unknown);
            CHECK(mdb::health_answer(true, 50.0, nan_v) == mdb::Health::Unknown);
            CHECK(mdb::health_answer(true, inf_v, 100.0) == mdb::Health::Unknown);
            CHECK(mdb::health_answer(true, 50.0, inf_v) == mdb::Health::Unknown);
            CHECK(mdb::health_answer(true, -inf_v, 100.0) == mdb::Health::Unknown);
        }
        // constexpr, so a mistake in it is a compile error.
        static_assert(mdb::health_answer(true, 0.0, 10.0) == mdb::Health::Dead);
        static_assert(mdb::health_answer(true, 10.0, 10.0) == mdb::Health::Alive);
        static_assert(mdb::health_answer(false, 0.0, 10.0) == mdb::Health::Unknown);
        static_assert(mdb::health_answer(true, 0.0, 0.0) == mdb::Health::Unknown);

        section("a boss killed before the mod existed (the save-backed rule)");

        // A killed boss never spawns again, so the health read has no actor. The only state that
        // outlives it is the save's `UnlockedFirepoints`, keyed by the marker's `bossdoor_*` id.
        CHECK(mdb::boss_found_from_save(true, true, true, true));
        // No door in the manifest (2 of the 28 boss markers) - the save cannot answer.
        CHECK(!mdb::boss_found_from_save(true, true, false, true));
        CHECK(!mdb::boss_found_from_save(true, true, false, false));
        CHECK(!mdb::boss_found_from_save(true, true, true, false));
        // The config key is a real off switch: the mark is derived rather than persisted.
        CHECK(!mdb::boss_found_from_save(false, true, true, true));
        CHECK(!mdb::boss_found_from_save(true, false, true, true));
        static_assert(mdb::boss_found_from_save(true, true, true, true));
        static_assert(!mdb::boss_found_from_save(true, true, true, false));
        static_assert(!mdb::boss_found_from_save(false, true, true, true));

        // `bossdoor` is additive and optional: a manifest without it parses and the rule never fires.
        {
            std::vector<mdb::StaticMarker> out;
            mdb::ParseReport rep{};
            CHECK(mdb::parse_markers_json(
                R"({"schema":"wuchang-minimap-markers/1","chapter":1,"markers":[
                     {"id":"L/BossA","cat":"boss","x":1,"y":2,"z":3,
                      "bossdoor":"bossdoor_dyy"},
                     {"id":"L/BossB","cat":"boss","x":4,"y":5,"z":6}]})",
                out, rep));
            CHECK_EQ(static_cast<int>(out.size()), 2);
            CHECK_STR(out[0].bossdoor.c_str(), "bossdoor_dyy");
            CHECK(out[1].bossdoor.empty());
            CHECK(mdb::boss_found_from_save(true, out[0].cat == mdb::Cat::Boss,
                                            !out[0].bossdoor.empty(), true));
            CHECK(!mdb::boss_found_from_save(true, out[1].cat == mdb::Cat::Boss,
                                             !out[1].bossdoor.empty(), true));
        }

        section("a corpse has two halves and both must go");

        // An enemy is in the published buffer twice - authored spawn point and the live pawn - and
        // both halves have to go, or the marker jumps back to the spawn point on the next publish.
        CHECK(mdb::static_twin_is_hidden_by_corpse(true, true));
        // A live twin that is alive does not hide its spawn point: the live position wins.
        CHECK(!mdb::static_twin_is_hidden_by_corpse(true, false));
        // No live twin at all: nothing is known, so the authored hint stays.
        CHECK(!mdb::static_twin_is_hidden_by_corpse(false, true));
        CHECK(!mdb::static_twin_is_hidden_by_corpse(false, false));

        // The live half: a corpse is never drawn where it fell, nor an actor with no usable position.
        CHECK(mdb::live_only_is_drawn(true, false));
        CHECK(!mdb::live_only_is_drawn(true, true));
        CHECK(!mdb::live_only_is_drawn(false, false));
        CHECK(!mdb::live_only_is_drawn(false, true));

        // A dead enemy is in NEITHER half of the published buffer, so every view agrees: one buffer.
        {
            const bool has_twin = true;
            const bool dead = true;
            CHECK(mdb::static_twin_is_hidden_by_corpse(has_twin, dead));
            CHECK(!mdb::live_only_is_drawn(/*pos_valid=*/false, dead));
        }

        // A defeated boss stays in the buffer for the hollow found glyph and leaves via the found gate.
        {
            mdb::XrayFacts b{};
            b.cat = mdb::Cat::Boss;
            b.cat_selected = true;
            b.within_radius = true;
            b.found = true;
            CHECK(mdb::xray_gate(b) == mdb::XrayDrop::Found);
            CHECK(!mdb::static_twin_is_hidden_by_corpse(false, false));
        }

        section("npc markers that have walked away");

        // Only people move: every other category's authored position is a fact about the level, and
        // `Note` is explicitly NOT mobile - a reading point hangs on a wall.
        for (int c = 0; c < mdb::kCatCount; ++c)
        {
            const auto cat = static_cast<mdb::Cat>(c);
            const bool expect = cat == mdb::Cat::Npc;
            CHECK_EQ(mdb::is_mobile_category(cat), expect);
        }
        CHECK(!mdb::is_mobile_category(mdb::Cat::Note));

        {
            mdb::MobileTwinFacts f{};
            f.mobile = true;
            f.live_twin_this_round = false;
            f.level_known = true;
            f.full_round_since_level_load = true;
            CHECK(mdb::mobile_twin_is_stale(f)); // case (c): nobody answered

            // Case (a): a locatable live actor answered - its position wins and the entry stays.
            mdb::MobileTwinFacts g = f;
            g.live_twin_this_round = true;
            CHECK(!mdb::mobile_twin_is_stale(g));

            // Case (d): the level is not resident, so absence means nothing - keep it.
            g = f;
            g.level_known = false;
            CHECK(!mdb::mobile_twin_is_stale(g));

            g = f;
            g.full_round_since_level_load = false;
            CHECK(!mdb::mobile_twin_is_stale(g));

            g = f;
            g.mobile = false;
            CHECK(!mdb::mobile_twin_is_stale(g));

            // Case (b): a live actor answered for the id but could NOT be located - this game parks a
            // used-up actor at (0,0,0). It hides with NO help from the level table.
            g = mdb::MobileTwinFacts{};
            g.mobile = true;
            g.live_twin_unlocatable = true;
            CHECK(!g.level_known);
            CHECK(!g.full_round_since_level_load);
            CHECK(mdb::mobile_twin_is_stale(g));

            // It must not override a live actor we CAN locate: this round's locatable answer wins.
            g.live_twin_this_round = true;
            CHECK(!mdb::mobile_twin_is_stale(g));

            // Unlocatable is only a rule about people: a note that has not streamed in must not vanish.
            g = mdb::MobileTwinFacts{};
            g.live_twin_unlocatable = true;
            CHECK(!mdb::mobile_twin_is_stale(g)); // mobile == false

            // Case (e): the actor is neither parked nor destroyed - it is made INVISIBLE while still
            // answering with its authored position, so (b), (c) and (d) are all unreachable.
            g = mdb::MobileTwinFacts{};
            g.mobile = true;
            g.live_twin_this_round = true; // located, at its authored position
            g.live_twin_invisible = true;
            CHECK(mdb::mobile_twin_is_stale(g));
            // (e) is tested BEFORE (a), or the located answer would win.
            CHECK(g.live_twin_this_round && mdb::mobile_twin_is_stale(g));
            g.level_known = false;
            g.full_round_since_level_load = false;
            CHECK(mdb::mobile_twin_is_stale(g));
            g.live_twin_invisible = false;
            CHECK(!mdb::mobile_twin_is_stale(g));
            // Invisibility is a rule about people only: a note's flags are not evidence about anybody.
            g = mdb::MobileTwinFacts{};
            g.live_twin_invisible = true;
            g.live_twin_this_round = true;
            CHECK(!mdb::mobile_twin_is_stale(g)); // mobile == false
        }
        // Nothing is hidden by default: a zeroed fact set must be a no-op.
        CHECK(!mdb::mobile_twin_is_stale(mdb::MobileTwinFacts{}));

        // The join key: the level short name out of a ULevel's full name.
        CHECK_STR(mdb::level_from_full_name(
                      "Level /Game/Maps/Chapter1/Chapter1_DGong_logic.Chapter1_DGong_logic:PersistentLevel"),
                  "Chapter1_DGong_logic");
    }

    // The save-slot key: sanitising, filenames, and pulling a slot out of a path
    // The half of src/saveslot.hpp that never touches the engine and decides a FILENAME.

    // src/spinlock.hpp - the mod's only lock (std::mutex faults on the game thread).

    void test_spinlock()
    {
        section("spinlock");

        // Constant-initialised: a namespace-scope Spinlock needs no dynamic initialiser.
        static_assert(std::is_trivially_destructible_v<spin::Spinlock>);
        static spin::Spinlock g_static_lock;
        CHECK(g_static_lock.try_lock());
        g_static_lock.unlock();

        spin::Spinlock lock;

        CHECK(lock.try_lock());
        CHECK(!lock.try_lock());
        lock.unlock();
        CHECK(lock.try_lock());
        lock.unlock();

        {
            spin::SpinGuard guard(lock);
            CHECK(!lock.try_lock());
        }
        CHECK(lock.try_lock());
        lock.unlock();

        // Two threads increment a plain int 20 000 times each under the lock.
        {
            int counter = 0;
            const int per_thread = 20000;
            auto bump = [&]() {
                for (int i = 0; i < per_thread; ++i)
                {
                    spin::SpinGuard guard(lock);
                    ++counter;
                }
            };
            std::thread t1(bump);
            std::thread t2(bump);
            t1.join();
            t2.join();
            CHECK_EQ(counter, 2 * per_thread);
            CHECK(lock.try_lock());
            lock.unlock();
        }

        // try_lock_ms honours its deadline: a held lock is not acquired and the wait is bounded.
        // bounded rather than returning early or hanging.
        {
            spin::Spinlock held;
            held.lock();
            const std::uint64_t t0 = ::GetTickCount64();
            CHECK(!held.try_lock_ms(50));
            const std::uint64_t waited = ::GetTickCount64() - t0;
            CHECK(waited >= 40);   // it really waited (GetTickCount64 granularity ~16 ms)
            CHECK(waited < 2000);  // and it really gave up
            held.unlock();
            const std::uint64_t t1 = ::GetTickCount64();
            CHECK(held.try_lock_ms(50));
            CHECK(::GetTickCount64() - t1 < 50);
            held.unlock();
        }

        {
            spin::Spinlock l;
            CHECK(l.try_lock_ms(0));
            CHECK(!l.try_lock_ms(0));
            l.unlock();
        }

        {
            spin::Spinlock l;
            l.lock();
            std::thread releaser([&]() {
                ::Sleep(30);
                l.unlock();
            });
            CHECK(l.try_lock_ms(3000));
            releaser.join();
            l.unlock();
        }
    }

    void test_saveslot()
    {
        std::printf("save-slot keys and found-file names\n");

        CHECK_STR(slotid::sanitise_key("maingame0"), "maingame0");
        CHECK_STR(slotid::sanitise_key("5849e75e473333a06fd8ad9ef440ed8c"),
                  "5849e75e473333a06fd8ad9ef440ed8c");
        CHECK_STR(slotid::sanitise_key("36053875_maingame0"), "36053875_maingame0");
        // Anything that could escape the mod directory, or confuse a shell, is gone.
        CHECK_STR(slotid::sanitise_key("..\\..\\windows\\system32"), "windows_system32");
        CHECK_STR(slotid::sanitise_key("a/b:c*d?e"), "a_b_c_d_e");
        CHECK_STR(slotid::sanitise_key("  spaced   name  "), "spaced_name");
        CHECK_STR(slotid::sanitise_key("___"), "");
        CHECK_STR(slotid::sanitise_key(""), "");
        // Non-ASCII is not a key character, so a CJK profile name degrades to "".
        CHECK_STR(slotid::sanitise_key("\xe4\xb8\xad\xe6\x96\x87"), "");
        // Capped, and the cap is applied before the trailing-'_' trim.
        CHECK(slotid::sanitise_key(std::string(200, 'x')).size() == slotid::kMaxKeyLen);

        CHECK_STR(slotid::found_filename(""), "wuchang_minimap_found.txt");
        CHECK_STR(slotid::found_filename("maingame0"), "wuchang_minimap_found_maingame0.txt");
        // The empty key is the ONLY thing that may produce the shared name.
        CHECK(slotid::found_filename("a") != slotid::found_filename(""));

        const char* kReal =
            "C:\\Users\\me\\AppData\\Local\\Project_Plague\\Saved\\36053875\\GameSlots\\maingame0\\maingame0.sav";
        CHECK_STR(slotid::slot_from_path(kReal), "maingame0");
        CHECK_STR(slotid::account_from_path(kReal), "36053875");
        CHECK_STR(slotid::key_from_sav_path(kReal), "36053875_maingame0");
        // Forward slashes and a different case of the anchor component both work.
        CHECK_STR(slotid::key_from_sav_path("D:/x/saved/99/gameslots/ng2/ng2.sav"), "99_ng2");
        // No GameSlots component -> no key, so the caller falls back to the shared name.
        CHECK_STR(slotid::key_from_sav_path("C:\\nothing\\here.sav"), "");
        CHECK_STR(slotid::key_from_sav_path(""), "");
        CHECK_STR(slotid::slot_from_path("C:\\a\\GameSlots"), "");
        CHECK_STR(slotid::key_from_sav_path("GameSlots\\maingame1\\x.sav"), "maingame1");
    }

    // Map -> clipboard: the pixel unpack and the DIB layout
    // A wrong unpack shifts channels and a wrong DIB pastes upside down. This game's back
    // buffer is R10G10B10A2_UNORM.

    void test_clipimg()
    {
        std::printf("map screenshot: pixel unpack and DIB layout\n");

        CHECK(clipimg::fmt_from_dxgi(24) == clipimg::Fmt::R10G10B10A2); // the real one here
        CHECK(clipimg::fmt_from_dxgi(28) == clipimg::Fmt::R8G8B8A8);
        CHECK(clipimg::fmt_from_dxgi(29) == clipimg::Fmt::R8G8B8A8); // _SRGB
        CHECK(clipimg::fmt_from_dxgi(87) == clipimg::Fmt::B8G8R8A8);
        CHECK(clipimg::fmt_from_dxgi(91) == clipimg::Fmt::B8G8R8A8);
        // A float back buffer needs tone mapping, not unpacking: refused, not guessed.
        CHECK(clipimg::fmt_from_dxgi(10) == clipimg::Fmt::Unknown); // R16G16B16A16_FLOAT
        CHECK(clipimg::fmt_from_dxgi(0) == clipimg::Fmt::Unknown);

        // Rounded, not shifted: `v >> 2` makes white 252, a visible grey cast.
        CHECK_EQ(clipimg::from10(0), 0);
        CHECK_EQ(clipimg::from10(1023), 255);
        CHECK_EQ(clipimg::from10(512), 128);
        // Round-to-nearest against the real range: 1020/1023 is 254.25 and lands on 254,
        // where `v >> 2` gives 255.
        CHECK_EQ(clipimg::from10(1020), 254);
        CHECK(clipimg::from10(1020) != (1020u >> 2));

        {
            // R = 1023, G = 0, B = 512, A = 3  ->  bits: R 0-9, G 10-19, B 20-29, A 30-31.
            const std::uint32_t px = 1023u | (0u << 10) | (512u << 20) | (3u << 30);
            std::uint8_t src[4];
            std::memcpy(src, &px, 4);
            std::uint8_t dst[4]{};
            CHECK(clipimg::unpack_row(clipimg::Fmt::R10G10B10A2, src, dst, 1));
            CHECK_EQ(dst[0], 128); // B
            CHECK_EQ(dst[1], 0);   // G
            CHECK_EQ(dst[2], 255); // R
            CHECK_EQ(dst[3], 255); // alpha is forced opaque, never the buffer's 2 bits
        }
        {
            const std::uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8}; // RGBA, two pixels
            std::uint8_t dst[8]{};
            CHECK(clipimg::unpack_row(clipimg::Fmt::R8G8B8A8, src, dst, 2));
            CHECK_EQ(dst[0], 3); // B
            CHECK_EQ(dst[1], 2); // G
            CHECK_EQ(dst[2], 1); // R
            CHECK_EQ(dst[3], 255);
            CHECK_EQ(dst[4], 7);
            CHECK_EQ(dst[6], 5);
        }
        {
            const std::uint8_t src[4] = {9, 8, 7, 0}; // already BGRA
            std::uint8_t dst[4]{};
            CHECK(clipimg::unpack_row(clipimg::Fmt::B8G8R8A8, src, dst, 1));
            CHECK_EQ(dst[0], 9);
            CHECK_EQ(dst[1], 8);
            CHECK_EQ(dst[2], 7);
            CHECK_EQ(dst[3], 255);
        }
        {
            std::uint8_t dst[4]{};
            const std::uint8_t src[4]{};
            CHECK(!clipimg::unpack_row(clipimg::Fmt::Unknown, src, dst, 1));
            CHECK(!clipimg::unpack_row(clipimg::Fmt::R8G8B8A8, nullptr, dst, 1));
            CHECK(!clipimg::unpack_row(clipimg::Fmt::R8G8B8A8, src, dst, 0));
        }

        {
            // 2x3 BGRA, top-down, with a source pitch bigger than the row (D3D12 readback is 256-aligned).
            const int w = 2;
            const int h = 3;
            const std::size_t pitch = 16;
            std::vector<std::uint8_t> src(pitch * h, 0u);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    // Encode the row in blue so the flip is checkable by value.
                    src[pitch * static_cast<std::size_t>(y) + static_cast<std::size_t>(x) * 4u] =
                        static_cast<std::uint8_t>(10 + y);
                }
            }
            std::vector<std::uint8_t> dib;
            CHECK(clipimg::build_dib(w, h, src.data(), pitch, dib));
            CHECK_EQ(dib.size(), clipimg::kHeaderSize + static_cast<std::size_t>(w) * 4u * h);
            const std::uint8_t* p = dib.data();
            CHECK_EQ(clipimg::get_u32(p + 0), 40u);  // biSize - NOT a BITMAPFILEHEADER
            CHECK_EQ(clipimg::get_i32(p + 4), w);
            CHECK_EQ(clipimg::get_i32(p + 8), h);    // positive = bottom-up
            CHECK_EQ(p[14], 32);                     // biBitCount
            CHECK_EQ(clipimg::get_u32(p + 16), 0u);  // BI_RGB
            CHECK_EQ(clipimg::get_u32(p + 20), static_cast<std::uint32_t>(w * 4 * h));
            // Row 0 of the DIB is the BOTTOM of the image, i.e. source row h-1.
            const std::uint8_t* px = dib.data() + clipimg::kHeaderSize;
            CHECK_EQ(px[0], 12);                                  // source row 2
            CHECK_EQ(px[static_cast<std::size_t>(w) * 4u], 11);    // source row 1
            CHECK_EQ(px[static_cast<std::size_t>(w) * 8u], 10);    // source row 0
        }
        {
            std::vector<std::uint8_t> dib;
            const std::uint8_t one[4]{};
            CHECK(!clipimg::build_dib(0, 1, one, 4, dib));
            CHECK(!clipimg::build_dib(1, 0, one, 4, dib));
            CHECK(!clipimg::build_dib(1, 1, nullptr, 4, dib));
            // A pitch smaller than a row is a caller bug, not something to read past.
            CHECK(!clipimg::build_dib(2, 1, one, 4, dib));
            CHECK(dib.empty());
        }
    }

    // markers/shrines.json

    void test_shrines_db(const std::string& markers_dir)
    {
        std::printf("shrine table (markers/shrines.json)\n");

        {
            std::vector<shdb::Shrine> v;
            shdb::Report rep{};
            CHECK(!shdb::parse("{}", v, rep));
            CHECK(!rep.error.empty());
            CHECK(!shdb::parse("[]", v, rep));
            CHECK(!shdb::parse(R"({"schema":"wuchang-minimap-shrines/2","shrines":[]})", v, rep));
            CHECK(!shdb::parse(R"({"schema":"wuchang-minimap-shrines/1"})", v, rep));
            CHECK(shdb::parse(R"({"schema":"wuchang-minimap-shrines/1","shrines":[]})", v, rep));
            CHECK(v.empty());
            CHECK(rep.error.empty());
        }
        {
            // One good entry, one with no id and one that is not an object: a bad LINE costs one entry.
            const char* json = R"({"schema":"wuchang-minimap-shrines/1","shrines":[
                {"id":"temple02","name":"Reverent Temple","chapter":1,"shrine":true,
                 "x":1.0,"y":2.0,"z":3.0,"bx":4.0,"by":5.0,"bz":6.0},
                {"name":"no id here"},
                17,
                {"id":"Task1","chapter":1,"shrine":false,"bx":7.0,"by":8.0,"bz":9.0}]})";
            std::vector<shdb::Shrine> v;
            shdb::Report rep{};
            CHECK(shdb::parse(json, v, rep));
            CHECK_EQ(v.size(), 2);
            CHECK_EQ(rep.rows, 2);
            CHECK_EQ(rep.shrines, 1);
            CHECK_EQ(rep.named, 1);
            CHECK_STR(v[0].id, "temple02");
            CHECK_STR(v[0].name, "Reverent Temple");
            CHECK_STR(v[0].label(), "Reverent Temple");
            CHECK(v[0].shrine);
            CHECK(v[0].has_pos);
            CHECK(v[0].has_birth);
            CHECK_EQ(static_cast<int>(v[0].z), 3);
            CHECK_EQ(static_cast<int>(v[0].bz), 6);
            // A pseudo-row: no shrine, no actor position, but it does have a destination.
            CHECK(!v[1].shrine);
            CHECK(!v[1].has_pos);
            CHECK(v[1].has_birth);
            CHECK_STR(v[1].label(), "Task1");

            // ---- the id join, which is case-insensitive on purpose -------------------
            CHECK_EQ(shdb::find_id(v, "temple02"), 0);
            CHECK_EQ(shdb::find_id(v, "TEMPLE02"), 0);
            CHECK_EQ(shdb::find_id(v, "task1"), 1);
            CHECK_EQ(shdb::find_id(v, "temple0"), -1);
            CHECK_EQ(shdb::find_id(v, "temple020"), -1);
            CHECK_EQ(shdb::find_id(v, ""), -1);
        }

        const std::string path = markers_dir + "/shrines.json";
        std::string text;
        if (!read_file(path, text))
        {
            std::printf("  (skipped: %s not found)\n", path.c_str());
            return;
        }
        std::vector<shdb::Shrine> v;
        shdb::Report rep{};
        CHECK(shdb::parse(text, v, rep));
        CHECK_STR(rep.error, "");
        // 88 contiguous rows in DT_FirePoint, 50 joining a shrine marker, the rest bossdoor_/Task
        // pseudo-points. Plus the seven DLC shrines, which have no DT_FirePoint row in this build.
        CHECK_EQ(rep.rows, 95);
        CHECK_EQ(rep.shrines, 57);
        CHECK_EQ(rep.named, 95);
        std::printf("  shipped table: %d row(s), %d shrine(s), %d named\n", rep.rows, rep.shrines,
                    rep.named);

        int with_birth = 0;
        int bad = 0;
        for (const shdb::Shrine& s : v)
        {
            with_birth += s.has_birth ? 1 : 0;
            // Every real shrine must be usable by the UI: a name, a chapter and a place on the map. A
            // DLC shrine's name comes from the marker DB rather than DT_FirePoint, but is never EMPTY.
            if (s.shrine && (s.name.empty() || s.chapter < 0 || !s.has_pos))
            {
                ++bad;
            }
            // The world is a few hundred thousand uu across; a drifted decode gives 1e38 or 1e-317.
            if (s.has_birth && !(std::fabs(s.bx) < 1.0e7 && std::fabs(s.by) < 1.0e7 &&
                                 std::fabs(s.bz) < 1.0e7))
            {
                ++bad;
            }
        }
        CHECK_EQ(bad, 0);
        // 88 `DT_FirePoint` rows carry a `BirthPosition`; the seven DLC shrines have none.
        CHECK_EQ(with_birth, 88);
        CHECK_EQ(static_cast<int>(v.size()), 95);
        int dlc = 0;
        for (const shdb::Shrine& s : v)
        {
            if (s.chapter == 0 && s.shrine)
            {
                ++dlc;
                CHECK(s.has_pos);
                CHECK(!s.name.empty());
            }
        }
        CHECK_EQ(dlc, 7);
        // The first row of the table, a fixed point on the decode chain: row name -> locres key.
        const int ti = shdb::find_id(v, "temple02");
        CHECK(ti >= 0);
        if (ti >= 0)
        {
            CHECK_STR(v[static_cast<std::size_t>(ti)].name, "Reverent Temple");
            CHECK_EQ(v[static_cast<std::size_t>(ti)].chapter, 1);
            CHECK(v[static_cast<std::size_t>(ti)].shrine);
        }
        // Ids are unique: the runtime joins the save's unlocked list to this table by id.
        int dupes = 0;
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            if (shdb::find_id(v, v[i].id) != static_cast<int>(i))
            {
                ++dupes;
            }
        }
        CHECK_EQ(dupes, 0);
    }

} // namespace

int main(int argc, char** argv)
{
    std::printf("WuchangMinimap - offline marker tests\n\n");

    test_loader();
    test_legacy_manifest_category();
    const std::string markers_dir = argc > 1 ? argv[1] : "markers";
    test_sample_file(markers_dir);
    test_real_db(markers_dir);
    test_categories();
    test_glyphs();
    test_fit_zoom();
    test_label_layout();
    test_zoom_presets();
    test_schema_gate();
    test_dedupe();
    test_atomic_write();
    test_found_file();
    test_spinlock();
    test_saveslot();
    test_clipimg();
    test_shrines_db(markers_dir);
    test_config_equality();
    test_ids();
    test_intern_levels();
    test_perf();
    test_mapview();
    test_waypoint_list();
    test_search_match();
    test_typing_gate();
    test_exchange();
    test_scan_sched();
    test_sweep_sched();
    test_projection();
    test_compass();
    test_chapter_id();
    test_marker_chapter_filter();
    test_map_manifest(markers_dir);
    test_slice_rule();
    test_height_planes();
    test_map_assets(markers_dir);
    test_config_keys(markers_dir);
    test_config_rewrite(markers_dir);
    test_absence();
    test_rarity();
    test_rarity_db(markers_dir);
    test_data_invariants(markers_dir);

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
