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
//   * the stable-id helpers that join the offline DB to the live actors.
//
// Run it with the repo's `markers` directory as argv[1] (build.ps1 does) to include
// the real sample file in the run; without it the embedded fixtures still cover
// everything else. Exit code 0 = all green.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
        int enemies = 0;
        for (const mdb::StaticMarker& m : db)
        {
            CHECK(!m.id.empty());
            CHECK_EQ(m.chapter, 1);
            shrines += (m.cat == mdb::Cat::Shrine) ? 1 : 0;
            enemies += (m.cat == mdb::Cat::Enemy) ? 1 : 0;
        }
        CHECK(shrines >= 1);
        // Enemies are live-only by decision; a static file must never carry one.
        CHECK_EQ(enemies, 0);
        std::printf("  %zu marker(s) parsed from %s\n", db.size(), path.c_str());
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
} // namespace

int main(int argc, char** argv)
{
    std::printf("WuchangMinimap - offline marker tests\n\n");

    test_loader();
    test_sample_file(argc > 1 ? argv[1] : "markers");
    test_categories();
    test_found_file();
    test_ids();

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
