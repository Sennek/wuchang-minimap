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

#include "mapview.hpp"
#include "markers_db.hpp"
#include "scan_sched.hpp"

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
        // The live sweep walks GUObjectArray in slices, one slice per game-thread pump
        // (src/scan_sched.hpp). `markers_rounds_per_sec` caps how often a FULL pass is
        // started; `markers_scan_chunk` and `markers_scan_period_ms` decide how finely
        // that pass is sliced, i.e. how much game-thread time a single pump can cost.
        // Raise the chunk for a fresher marker set, lower it if the frame time suffers.
        int markers_rounds_per_sec = 1;   // full passes over the object array per second
        int markers_scan_chunk = scan::kChunkDefault;        // object slots per pump
        int markers_scan_period_ms = scan::kPeriodDefaultMs; // min ms between pumps
        std::uint32_t markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        bool markers_hide_found = false; // hide instead of dimming a found marker
        float markers_found_alpha = 0.3f;
        float markers_size = 6.5f;         // glyph radius in minimap pixels
        bool markers_clamp_to_edge = false; // keep out-of-range markers on the rim
        int markers_max_draw = 400;         // hard cap per frame, nearest first

        // The found tracker: wuchang_minimap_found.txt, one stable id per line.
        bool found_tracker = true;
        int found_save_debounce_ms = 2000;

        //==============================================================================
        // The full map (step C1)
        //==============================================================================
        //
        // The full map is the same height-sliced asset the minimap draws, at map scale:
        // a north-up, pannable, zoomable window over the chapter with every marker on
        // it. While it is open the minimap is hidden and ImGui takes the mouse and the
        // keyboard (never latched - closing the map gives both back on the same frame).
        //
        // Zoom is world units per SCREEN pixel, the same unit as `minimap_zoom`, so the
        // three numbers below are directly comparable with it (26 = the minimap).

        // 30 uu/px shows ~270 m across a 900 px canvas - a district, not a continent.
        // Chapter 1 is ~450 m wide, so ~55 fits the whole chapter and 240 is as far out
        // as is ever useful.
        float map_zoom = 30.0f;      // uu per screen pixel when the map opens
        float map_zoom_min = 4.0f;   // most zoomed IN (fewest uu per pixel)
        float map_zoom_max = 240.0f; // most zoomed OUT
        float map_zoom_factor = 1.15f; // multiplier per wheel notch / per 0.1 s of trigger
        float map_pan_speed = 900.0f;  // keyboard / stick pan, SCREEN px per second
        float map_margin = 0.045f;     // border around the map, fraction of screen height
        float map_backdrop = 0.86f;    // opacity of the dark backdrop behind it
        float map_marker_size = 8.0f;  // glyph radius in screen px
        int map_markers_max_draw = 4000;
        float map_floor_step = 200.0f; // uu per floor-adjust notch (Q/E, LB/RB, Ctrl+wheel)
        bool map_show_all_floors = false; // ignore the height slice: draw every surface
        // The dynamic texture the map slice is cut into. The cut costs roughly
        // (width x height x surfaces) plane reads, so 768 x ~430 x 8 is ~2.6 M and lands
        // around 5-10 ms - which is why it only runs when something actually changed.
        int map_slice_px = 768;
        int map_slice_hz = 6;        // hard cap on re-cuts per second
        bool map_gamepad = true;     // poll XInput while the map is open
        float map_gamepad_deadzone = 0.22f;
        bool map_waypoint_persist = true; // write wuchang_minimap_waypoint.txt

        //==============================================================================
        // Hold-key x-ray highlight (master plan step 7, v1)
        //==============================================================================
        //
        // While `highlight_key` is HELD (or the gamepad chord is), every marker of an
        // enabled category within `highlight_radius` is drawn at its projected screen
        // position - glyph, name and distance in metres - fading with distance. The
        // overlay draws over the scene, so "through walls" costs nothing extra; there is
        // no occlusion test and no render state anywhere near the game's.
        //
        // It is a HOLD, not a toggle: a toggle would be one more piece of latched state
        // to unlatch when a menu opens, and lessons.md is unambiguous about those.

        bool highlight_enabled = true;
        int highlight_key = 0xA4; // VK_LMENU - left Alt
        bool highlight_gamepad = true;
        // XInput chord (pad::kLeftShoulder | pad::kRightShoulder by default). The
        // triggers are analogue, so they are their own flags rather than mask bits.
        std::uint16_t highlight_pad_mask = 0x0300;
        bool highlight_pad_lt = false;
        bool highlight_pad_rt = false;
        float highlight_radius = 3000.0f; // uu (30 m)
        // Chests and pickups: the "uncollected loot" set. Everything mdb knows about is
        // selectable, which is what makes this useful for shrines or fog gates too.
        std::uint32_t highlight_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup);
        bool highlight_show_found = false; // draw collected / opened ones too
        int highlight_max_draw = 60;       // nearest first
        float highlight_alpha_near = 1.0f; // at the camera
        float highlight_alpha_far = 0.25f; // at highlight_radius
        float highlight_size = 7.0f;       // glyph radius, screen px
        bool highlight_labels = true;      // name + distance next to the glyph
        bool highlight_edge_arrows = true; // off-screen / behind: an arrow on the rim
        // How often the game thread re-reads the camera while the key is held. The read
        // is a handful of raw doubles at a cached offset, so this is cheap; it only has
        // to keep up with how fast the player can swing the camera.
        int highlight_camera_hz = 60;

        //==============================================================================
        // The compass strip
        //==============================================================================
        //
        // A heading strip across the top of the screen: N/E/S/W plus 15-degree ticks
        // derived from the camera yaw (the pawn's yaw when no camera pose is fresh),
        // with bearing pips for the waypoint and for nearby markers of the selected
        // categories. It is hidden by exactly the same evaluation as the minimap - no
        // second set of show/hide rules, no second latch.

        bool compass_enabled = true;
        float compass_width = 0.42f;    // fraction of the screen width
        float compass_offset_y = 18.0f; // px from the top of the screen
        float compass_height = 26.0f;   // px
        float compass_span_deg = 120.0f; // degrees visible across the strip
        float compass_opacity = 0.9f;
        std::uint32_t compass_categories = mdb::cat_bit(mdb::Cat::Shrine) |
                                           mdb::cat_bit(mdb::Cat::Boss) |
                                           mdb::cat_bit(mdb::Cat::Elite) |
                                           mdb::cat_bit(mdb::Cat::FogGate);
        float compass_marker_distance = 15000.0f; // uu (150 m)
        bool compass_show_waypoint = true;

        bool debug_readout = true;
        bool debug_show_panel_on_start = false; // main-menu verification aid
        int panel_key = 0x71;                   // VK_F2
        int reload_key = 0x74;                  // VK_F5
        int map_key = 0x4D;                     // 'M' - full map
        int map_recenter_key = 0x52;            // 'R' - recentre the full map on the player
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

    // "F2", "M", "TAB", "LALT", ... - the same spelling the config file uses. Any thread.
    std::wstring key_name(int vk);

    // "LB+RB", "A", "LT+RT", "none" - the gamepad chord spelling the config file uses.
    // Any thread.
    std::wstring pad_chord_name(std::uint16_t mask, bool lt, bool rt);

    //==================================================================================
    // The waypoint
    //==================================================================================
    //
    // One waypoint at a time, set on the full map and drawn on both the full map and
    // the minimap (edge-clamped, with a distance). It lives in its own tiny file rather
    // than in config_wuchang_minimap.txt, because the config file is rewritten wholesale
    // by the F2 panel's Save button and a waypoint set during play must survive without
    // anyone pressing Save.
    //
    // The render thread sets it (a right-click on the map); the loop thread writes the
    // file. The handover is the same spinlocked-copy pattern the config uses.

    mv::Waypoint waypoint();
    void set_waypoint(const mv::Waypoint& wp); // any thread; marks the file dirty
    void load_waypoint_file();                 // loop thread
    void save_waypoint_file();                 // loop thread
    std::wstring waypoint_path();

    //==================================================================================
    // Cross-thread flags
    //==================================================================================

    extern std::atomic<bool> g_panel_open;      // F2
    extern std::atomic<bool> g_map_open;        // M - full map (reserved)
    extern std::atomic<bool> g_reload_config;   // F5 -> loop thread reloads
    extern std::atomic<bool> g_save_config;     // panel -> loop thread saves
    extern std::atomic<bool> g_panel_drew_frame; // set by the render thread, for the log
    extern std::atomic<bool> g_waypoint_dirty;   // render -> loop: write the waypoint file

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
