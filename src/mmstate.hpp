#pragma once

//
// mmstate - everything shared between the mod's three threads.
//
//   UE4SS event-loop thread   CppUserModBase::on_update: hotkeys, config file I/O,
//                             maps.json + PNG decoding, log draining.
//   game thread               UE4SS ProcessEvent pre-callback: reads the pawn, the
//                             view target and the widgets, publishes a Snapshot.
//   render thread             the hooked IDXGISwapChain::Present: D3D12 + ImGui only.
//
// Rules that follow from lessons.md and are enforced by this design:
//   * no std::mutex anywhere (it faults against the process's MSVCP140 when first
//     touched from the game thread) - a Snapshot is handed over with a seqlock and
//     everything else is a plain atomic;
//   * no iostreams / locale off the loop thread - all file I/O in this mod is plain
//     CreateFileW + ReadFile / WriteFile;
//   * the render thread never touches a UObject, and the game thread never touches
//     D3D12.
//

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include "markers_db.hpp"

namespace mm
{
    //==================================================================================
    // Game state published by the game thread
    //==================================================================================

    // Trivially copyable on purpose: the seqlock copies it with a plain memcpy.
    struct Snapshot
    {
        double x = 0.0; // world location, uu
        double y = 0.0;
        double z = 0.0;
        float yaw = 0.0f; // degrees, UE convention (0 = +X = the image's "north")

        // DEFAULTS ARE "HIDDEN". A snapshot that was never filled in, or one published
        // while the reader is idling (main menu, level transition), must never let the
        // minimap appear: has_pawn/pawn_is_gameplay/is_pawn_view stay false and
        // menu_open starts TRUE.
        bool has_pawn = false;
        bool pawn_is_gameplay = false; // pawn class contains BP_CombatCharacter_Player
        bool is_pawn_view = false;     // view target == pawn (false in menus and cutscenes)
        bool menu_open = true;         // some in-viewport root widget is ESlateVisibility::Visible
        bool loc_from_function = false; // true: K2_GetActorLocation; false: RootComponent property
        bool transition = false;        // a level transition / pawn change cooldown is running

        std::uint32_t widgets_seen = 0;
        std::uint32_t widgets_visible_in_viewport = 0;
        std::uint32_t menu_roots_cached = 0; // in-viewport roots re-tested every pump
        // GetTickCount64() when menu_open last CHANGED, and the number of pumps since.
        // The overlay uses it for the short "menu just closed" delay, and the F2 debug
        // block prints the age so the hide/show latency is measurable in one screenshot.
        std::uint64_t menu_change_ms = 0;
        std::uint64_t stamp_ms = 0; // GetTickCount64() when this snapshot was published
        // GetTickCount64() at which the reader last STARTED to have a validated
        // gameplay pawn continuously. 0 = right now it does not. The overlay requires
        // a minimum age here before it draws anything, so the frames around a level
        // load never flash the minimap.
        std::uint64_t state_ok_since_ms = 0;
        // GetTickCount64() of the last detected teleport (a position jump inside one
        // pump). A shrine fast-travel keeps the same pawn and the same world, so this is
        // the only signal the render side gets that its position-derived caches (the
        // height-slice window, the smoothed feet Z) must be dropped.
        std::uint64_t teleport_ms = 0;

        wchar_t pawn_name[96]{}; // pawn class / object name, for the debug readout
        wchar_t level_name[160]{}; // pawn full name (carries the world + level path)
        // The in-viewport Visible root widget that is currently holding menu_open true,
        // or empty. Printed by the F2 debug block so "hidden because: a menu is open"
        // always names the widget responsible.
        wchar_t menu_holder[64]{};
    };

    // Writer (game thread) and reader (render thread). A torn read is retried, never
    // blocked - the render thread must never wait on the game thread.
    void publish(const Snapshot& snap);
    bool read_snapshot(Snapshot& out);

    //==================================================================================
    // Configuration - config_wuchang_minimap.txt in the mod folder
    //==================================================================================

    enum class Anchor : int
    {
        TopLeft = 0,
        TopRight = 1,
        BottomLeft = 2,
        BottomRight = 3,
    };

    struct Config
    {
        bool enabled = true;          // master switch for the whole overlay
        bool show_minimap = true;     // draw the minimap window
        float size_frac = 0.24f;      // minimap side as a fraction of screen height
        float zoom_uu_per_px = 26.0f; // world uu per minimap pixel (smaller = closer)
        bool round = true;            // round mask instead of a square
        Anchor anchor = Anchor::TopLeft;
        float offset_x = 24.0f;
        float offset_y = 24.0f;
        bool rotate_with_player = false; // false = north-up
        float opacity = 0.92f;
        bool hide_in_menus = true;
        bool require_pawn_view = true;
        int state_stale_ms = 1000;
        int min_visible_after_state_ok_ms = 600; // grace after the state becomes good
        // A menu closing is NOT a level transition: showing the minimap again waits
        // only this long, not min_visible_after_state_ok_ms. Hiding is immediate.
        int menu_close_show_delay_ms = 150;

        //==============================================================================
        // Height slicing (what the minimap actually draws)
        //==============================================================================
        //
        // The map ships a MULTI-SURFACE HEIGHT MAP: four 16-bit PNGs holding the Z of
        // up to four stacked walkable surfaces per pixel. Every ~80 ms the overlay
        // slices the window around the player on the CPU:
        //
        //     |surfaceZ - feetZ| <= floor_z_tolerance -> opaque, shaded by the gradient
        //     nearest below within floor_fade_uu      -> adjacent_floor_opacity
        //     nearest above within floor_fade_uu      -> adjacent_floor_opacity x 0.6
        //     nothing                                 -> transparent
        //
        // This replaced the surface-ordinal layer scheme, which drew a temple interior
        // as several blended layers. There are no floor ranks or band grids left, so
        // `floor_hysteresis` and `floor_fallback_hold_ms` are gone too - the only
        // smoothing is feet_z_smooth_ms.

        bool show_adjacent_floors = true;     // draw the surfaces below / above, dimmed
        float adjacent_floor_opacity = 0.25f; // below; above uses 0.6 x this
        float floor_z_tolerance = 200.0f;     // uu: |Z - feetZ| within this = my floor
        float floor_fade_uu = 800.0f;         // uu: how far below / above is still shown
        // Height gradient. lum = 1 + strength * clamp((surfaceZ - feetZ) / span, -1, 1)
        // with span = tolerance (current floor) or fade (dimmed), so a ramp or a
        // staircase inside one storey reads as a gentle gradient rather than a flat
        // silhouette. The SAME formula is implemented offline in
        // tools/navmesh/slice_preview.py, so a reported spot can be reproduced without
        // the game.
        float floor_gradient_strength = 0.18f;
        float floor_base_r = 214.0f; // the walkable fill: a light warm grey that reads
        float floor_base_g = 208.0f; // on this game's dark scenes
        float floor_base_b = 196.0f;
        int slice_hz = 12;           // CPU re-slices per second (2..30)
        int feet_z_smooth_ms = 100;  // EMA time constant on feet Z
        // The pawn's location is its capsule centre, ~90 uu above the navmesh it is
        // standing on. Subtracted to get feet Z.
        float player_z_offset = 90.0f;
        // Load the Z-shaded composite PNG as well (+82 MB of VRAM for Chapter 1). It
        // merges every storey, so it is only the fallback for a chapter whose height
        // maps are missing - which the runtime loads on its own anyway.
        bool fallback_use_composite = false;

        //==============================================================================
        // Markers
        //==============================================================================
        //
        // The static database is markers/<chapter>.json (schema
        // `wuchang-minimap-markers/1`); the live half is a class sweep on the game
        // thread that reads each marker actor's own state flag. `markers_categories`
        // is the same name list the F2 filter checkboxes drive, so a filter toggled in
        // the panel and one written into the config file are the same setting.

        bool markers_enabled = true;  // draw markers at all
        bool markers_live = true;     // run the game-thread class sweep
        int markers_rounds_per_sec = 1; // full sweeps of the class table per second
        std::uint32_t markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        bool markers_hide_found = false; // hide instead of dimming a found marker
        float markers_found_alpha = 0.3f;
        float markers_size = 6.5f;         // glyph radius in minimap pixels
        bool markers_clamp_to_edge = false; // keep out-of-range markers on the rim
        int markers_max_draw = 400;         // hard cap per frame, nearest first

        // The found tracker: wuchang_minimap_found.txt, one stable id per line.
        bool found_tracker = true;
        int found_save_debounce_ms = 2000;

        bool debug_readout = true;
        bool debug_show_panel_on_start = false; // main-menu verification aid
        int panel_key = 0x71;                   // VK_F2
        int reload_key = 0x74;                  // VK_F5
        int map_key = 0x4D;                     // 'M' - full map (reserved, not built yet)
    };

    // The config lives here and is copied under a spinlock. The loop thread writes it
    // on load / F5; the render thread writes it when the F2 panel is used.
    Config config();
    void set_config(const Config& cfg);

    // Loop thread only (plain Win32 file I/O, no iostreams).
    void load_config_file();
    void save_config_file();
    std::wstring config_path();
    std::wstring mod_dir();

    // "F2", "M", "TAB", ... - the same spelling the config file uses. Any thread.
    std::wstring key_name(int vk);

    //==================================================================================
    // Cross-thread flags
    //==================================================================================

    extern std::atomic<bool> g_panel_open;      // F2
    extern std::atomic<bool> g_map_open;        // M - full map (reserved)
    extern std::atomic<bool> g_reload_config;   // F5 -> loop thread reloads
    extern std::atomic<bool> g_save_config;     // panel -> loop thread saves
    extern std::atomic<bool> g_panel_drew_frame; // set by the render thread, for the log

    //==================================================================================
    // Logging that is safe from any thread
    //==================================================================================
    //
    // Output::send touches fmt and, through it, the C++ locale, which is fatal on the
    // game thread. So every thread except the loop thread only queues text; the loop
    // thread drains the queue in on_update.

    void log(const std::wstring& line);
    void set_loop_thread();
    void drain_log();

    template <typename... Args>
    void logf(std::wformat_string<Args...> fmt, Args&&... args)
    {
        log(std::format(fmt, std::forward<Args>(args)...));
    }
} // namespace mm
