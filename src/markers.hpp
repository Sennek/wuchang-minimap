#pragma once

//
// markers - the runtime half of the marker feature.
//
// Three threads touch this module and each is allowed to do exactly one thing:
//
//   UE4SS event-loop thread  on_unreal_init / on_update / reload:
//                            every file access (markers/<chapter>.json, the found
//                            tracker) and every log line. NEVER touches a UObject.
//   game thread              game_thread_pump(), called from gamestate's ProcessEvent
//                            pre-callback: FindAllOf + raw property reads only. It
//                            publishes a POD draw buffer and pushes newly-found ids
//                            into an outbox for the loop thread to write out.
//   render thread            view() / stats(), inside the hooked Present.
//
// The rules this shape comes from are in lessons.md: no std::mutex anywhere (it faults
// against the process's MSVCP140 on the game thread), no iostreams / locale off the
// loop thread, and no UObject traversal outside the ProcessEvent pump.
//

#include <cstddef>
#include <cstdint>

#include "markers_db.hpp"

namespace markers
{
    //==================================================================================
    // What the render thread draws
    //==================================================================================

    constexpr std::uint8_t kFlagFound = 1u << 0;  // collected / opened / used
    constexpr std::uint8_t kFlagLive = 1u << 1;   // a live actor answered for it this round
    constexpr std::uint8_t kFlagStatic = 1u << 2; // it came from markers/<chapter>.json

    // Trivially copyable and self-contained: the render thread never dereferences
    // anything the game thread owns except this array.
    struct DrawMarker
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        std::uint8_t cat = static_cast<std::uint8_t>(mdb::Cat::Other);
        std::uint8_t flags = 0;
        char id[54]{}; // truncated stable id, for the F2 "nearest marker" readout
        // The blueprint class ("BP_ItemRedBox_C"), or the manifest's display name when
        // there is no class. Only the full map's hover tooltip uses it - the id alone
        // reads as a path and does not say WHAT the thing is.
        char label[40]{};
    };

    // A borrowed view of the last published buffer. Valid for the duration of the
    // frame that took it: the game thread cycles through three slots and only reuses
    // one after two further publishes (~2 s).
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
        int published = 0;                          // markers in the last draw buffer
        int live_entries = 0;                       // live actors currently tracked
        std::uint64_t rounds = 0;                   // completed live sweep rounds

        // The chunked GUObjectArray walk. A "slice" is one game-thread pump's worth of
        // the object array; a "round" is a full pass over it, after which the draw
        // buffer is published. Per-slice numbers say whether the game thread is being
        // stalled; per-round numbers say whether the marker set is still fresh.
        double scan_slice_ms = 0.0;      // the most recent slice
        double scan_slice_ms_avg = 0.0;  // mean slice of the last COMPLETED round
        double scan_slice_ms_peak = 0.0; // worst slice of the last completed round
        double scan_slice_ms_max = 0.0;  // worst slice since the mod loaded
        double scan_round_ms = 0.0;      // summed slice time of the last completed round
        int scan_round_slices = 0;       // slices the last completed round took
        int scan_round_objects = 0;      // object-array slots it visited
        int scan_total = 0;              // GUObjectArray size at the last slice
        int scan_chunk = 0;              // slots per slice currently in force
        bool scan_fallback = false;      // true = the FindAllOf-per-class fallback is running

        bool db_loaded = false;
    };

    Stats stats();

    //==================================================================================
    // Thread entry points
    //==================================================================================

    // Loop thread, once. Loads markers/<chapter>.json and the found tracker.
    void on_unreal_init();

    // ANY THREAD (in practice the render thread, from the full map's click handler):
    // manually mark a marker found or not found. The request is queued and applied by
    // the loop thread, which owns the master set and the file; the game thread then
    // gets the whole set back through the existing inbox. Note the live sweep still
    // owns the truth: un-marking a chest the game reports as `Used` will be undone on
    // the next round, which is correct - the tracker follows the save, not the mod.
    void request_toggle_found(const char* id, bool found);

    // Loop thread, every tick. Drains the game thread's newly-found outbox and writes
    // the found file once the debounce has elapsed.
    void on_update();

    // Loop thread. F5 / "Reload settings + maps": re-read both files from disk.
    void reload();

    // GAME THREAD ONLY, from gamestate's ProcessEvent pump, and only while a validated
    // gameplay pawn exists outside the transition cooldown. `world` is the pawn's
    // UWorld* - a change means every cached pointer is dead.
    void game_thread_pump(std::uint64_t now, const void* world);

    // GAME THREAD ONLY. Drops every cached class layout, class classification and live
    // actor. Called by gamestate whenever it drops the pawn.
    void drop_caches();
} // namespace markers
