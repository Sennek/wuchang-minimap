#pragma once

//
// markers - the runtime half of the marker feature.
//
// Three threads touch this module, each allowed exactly one thing:
//
//   UE4SS event-loop thread  on_unreal_init / on_update / reload: all file access
//                            (markers/<chapter>.json, the found tracker) and all
//                            logging. NEVER touches a UObject.
//   game thread              game_thread_pump(), from gamestate's ProcessEvent
//                            pre-callback: object traversal + raw property reads only.
//                            Publishes a POD draw buffer and pushes newly-found ids
//                            into an outbox for the loop thread.
//   render thread            view() / stats(), inside the hooked Present.
//
// Constraints: no std::mutex anywhere (it faults against the process's MSVCP140 on the
// game thread), no iostreams / locale off the loop thread, no UObject traversal outside
// the ProcessEvent pump.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "markers_db.hpp"

namespace markers
{
    //==================================================================================
    // What the render thread draws
    //==================================================================================

    constexpr std::uint8_t kFlagFound = 1u << 0;  // collected / opened / used
    constexpr std::uint8_t kFlagLive = 1u << 1;   // a live actor answered for it this round
    constexpr std::uint8_t kFlagStatic = 1u << 2; // it came from markers/<chapter>.json

    // Trivially copyable and self-contained: the render thread dereferences nothing the
    // game thread owns except this array.
    struct DrawMarker
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        std::uint8_t cat = static_cast<std::uint8_t>(mdb::Cat::Other);
        std::uint8_t flags = 0;
        // Item quality tier (mdb::Rarity) of what a pickup grants, from the static DB.
        // 0 for everything else, including every live-only actor.
        std::uint8_t rarity = 0;
        char id[54]{}; // truncated stable id, for the F2 "nearest marker" readout
        // Blueprint class ("BP_ItemRedBox_C"), or the manifest's display name when there
        // is no class. Used by the full map's hover tooltip.
        char label[40]{};
    };

    // A borrowed view of the last published buffer, valid for the frame that took it:
    // the game thread cycles three slots and reuses one only after two further
    // publishes (~2 s).
    struct View
    {
        const DrawMarker* data = nullptr;
        std::size_t count = 0;
    };

    View view();

    //==================================================================================
    // Counters for the F2 panel
    //==================================================================================

    struct CatStat
    {
        int total = 0;
        int found = 0;
    };

    struct Stats
    {
        CatStat cat[mdb::kCatCount]{};              // over the whole static DB
        CatStat chapter[9][mdb::kCatCount]{};       // per chapter 1..8 (index 0 unused)
        int static_markers = 0;                     // entries loaded from JSON
        int chapters_loaded = 0;
        int found_ids = 0;                          // lines in wuchang_minimap_found.txt
        // F2 diagnostics: "0 levels" means the absence rule can never fire.
        int absence_marks = 0; // marks from the absence rule rather than a state flag
        int levels_loaded = 0;
        // F2 diagnostics: "0 met / 0 dead dropped with health unknown climbing" means
        // the health property name is wrong.
        int shrine_lit_marks = 0; // shrine markers marked from UnlockedFirepoints
        int met_marks = 0;        // NPC / note markers marked as met
        int boss_defeated = 0;    // boss markers marked as defeated THIS SESSION
        // Boss gauge over the current chapter. `boss_from_save` counts the `bossdoor_*`
        // rule, the only thing that answers for a boss killed before the mod existed.
        int boss_found = 0;
        int boss_total = 0;
        int boss_from_save = 0;
        int boss_no_door = 0;
        int dead_dropped = 0;     // live enemies dropped because their health read 0
        int health_unknown = 0;   // characters whose Health.Current could not be read
        // Chapter the published buffer is filtered to, or chid::kNone when unfiltered.
        int filter_chapter = chid::kNone;
        int published = 0;                          // markers in the last draw buffer
        int live_entries = 0;                       // live actors currently tracked
        std::uint64_t rounds = 0;                   // completed live sweep rounds

        // The chunked GUObjectArray walk. A "slice" is one game-thread pump's worth of
        // the object array; a "round" is a full pass, after which the draw buffer is
        // published.
        double scan_slice_ms = 0.0;      // the most recent slice
        double scan_slice_ms_avg = 0.0;  // mean slice of the last COMPLETED round
        double scan_slice_ms_peak = 0.0; // worst slice of the last completed round
        double scan_slice_ms_max = 0.0;  // worst slice since the mod loaded
        double scan_round_ms = 0.0;      // summed slice time of the last completed round
        // What publish_round() costs. It runs once per round on the game thread and is
        // not sliced.
        double publish_ms = 0.0;      // the most recent publish
        double publish_ms_avg = 0.0;  // mean since the mod loaded
        double publish_ms_peak = 0.0; // worst since the mod loaded
        int scan_round_slices = 0;       // slices the last completed round took
        int scan_round_objects = 0;      // object-array slots it visited
        int scan_total = 0;              // GUObjectArray size at the last slice
        int scan_chunk = 0;              // slots per slice currently in force
        bool scan_fallback = false;      // true = the FindAllOf-per-class fallback is running

        bool db_loaded = false;

        // Which found file is in force, and which rung of the save-slot ladder chose it
        // (src/saveslot.hpp). Shown on the F2 Map & tracker tab.
        char found_file[80]{};
        char found_route[24]{};
    };

    Stats stats();

    // Lock-free (one relaxed atomic load): how many full sweep rounds have completed.
    // `stats()` takes a spinlock and copies ~1.4 KB, too costly per frame.
    std::uint64_t rounds();

    //==================================================================================
    // Thread entry points
    //==================================================================================

    // Loop thread, once. Loads markers/<chapter>.json and the found tracker.
    void on_unreal_init();

    // ANY THREAD (in practice the render thread, from the full map's click handler):
    // manually mark a marker found or not found. Queued and applied by the loop thread,
    // which owns the master set and the file. The live sweep still owns the truth:
    // un-marking a chest the game reports as `Used` is undone on the next round.
    void request_toggle_found(const char* id, bool found);

    // Loop thread, every tick. Drains the game thread's newly-found outbox and writes
    // the found file once the debounce has elapsed.
    void on_update();

    // Loop thread. F5 / "Reload settings + maps": re-read both files from disk.
    void reload();

    //==================================================================================
    // Import / export of the found list
    //==================================================================================

    // LOOP THREAD. Every id in the found set of the profile in force, unordered.
    std::vector<std::string> found_ids();

    // LOOP THREAD. Adds `ids` to the found set; returns how many were new. A non-zero
    // return schedules the same debounced write a manual mark does.
    int merge_found_ids(const std::vector<std::string>& ids);

    // LOOP THREAD. The found file the profile in force writes to, for an export's label.
    std::string found_file_name();

    // Loop thread. Writes the found tracker NOW if a debounced write is pending.
    void flush_found_tracker();

    // DLL_PROCESS_DETACH. POD-only by contract: writes bytes the loop thread staged
    // ahead of time, allocates nothing, takes no lock, logs nothing - DllMain runs under
    // the loader lock and the process may be dying with a broken heap. Idempotent.
    void flush_found_tracker_at_exit();

    // GAME THREAD ONLY, from gamestate's ProcessEvent pump, and only while a validated
    // gameplay pawn exists outside the transition cooldown. `world` is the pawn's
    // UWorld* - a change means every cached pointer is dead.
    void game_thread_pump(std::uint64_t now, const void* world);

    // GAME THREAD ONLY, from gamestate's chapter refresh (1 Hz): the short names of the
    // levels the game currently reports as loaded, e.g. "Chapter1_DGong_logic". This is
    // what makes absence usable as evidence of a collect - an unloaded level and a
    // collected pickup look identical from the object array. A marker whose level is not
    // in this set is never auto-marked.
    //
    // The set is replaced wholesale on every call; a level that stays loaded keeps the
    // round number it was first seen at.
    void set_loaded_levels(const std::vector<std::string>& short_names);

    // GAME THREAD ONLY. Drops every cached class layout, class classification and live
    // actor. Called by gamestate whenever it drops the pawn.
    void drop_caches();
} // namespace markers
