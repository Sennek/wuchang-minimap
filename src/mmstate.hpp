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
// Rules this design enforces:
//   * no std::mutex anywhere (it faults against the process's MSVCP140 when first
//     touched from the game thread) - a Snapshot is handed over with a seqlock and
//     everything else is a plain atomic;
//   * no iostreams / locale off the loop thread - all file I/O is plain CreateFileW +
//     ReadFile / WriteFile;
//   * the render thread never touches a UObject, and the game thread never touches
//     D3D12.
//

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "glyphs.hpp"
#include "mapview.hpp"
#include "markers_db.hpp"
#include "perf.hpp"
#include "scan_sched.hpp"

namespace mm
{
    // Game state published by the game thread.
    // Trivially copyable on purpose: the seqlock copies it with a plain memcpy.
    struct Snapshot
    {
        double x = 0.0; // world location, uu
        double y = 0.0;
        double z = 0.0;
        float yaw = 0.0f; // degrees, UE convention (0 = +X = the image's "north")

        // Defaults mean "hidden": an unfilled snapshot must never let the minimap appear.
        bool has_pawn = false;
        bool pawn_is_gameplay = false; // pawn class contains BP_CombatCharacter_Player
        bool is_pawn_view = false;     // view target == pawn (false in menus and cutscenes)
        bool menu_open = true;         // some in-viewport root widget is ESlateVisibility::Visible
        bool loc_from_function = false; // true: K2_GetActorLocation; false: RootComponent property
        bool transition = false;        // a level transition / pawn change cooldown is running

        std::uint32_t widgets_seen = 0;
        std::uint32_t widgets_visible_in_viewport = 0;
        std::uint32_t menu_roots_cached = 0; // in-viewport roots open right now
        // Roots on the menu watchlist (every root confirmed in the viewport this session,
        // all re-tested on every pump) and the period the discovery sweep has backed off to.
        std::uint32_t menu_watch_count = 0;
        std::uint32_t widget_sweep_period_ms = 0;
        std::uint64_t menu_change_ms = 0; // GetTickCount64() when menu_open last changed
        std::uint64_t stamp_ms = 0; // GetTickCount64() when this snapshot was published
        // GetTickCount64() at which the reader last started to hold a validated gameplay
        // pawn continuously; 0 = it does not. The overlay requires a minimum age here.
        std::uint64_t state_ok_since_ms = 0;
        // GetTickCount64() of the last teleport (a position jump inside one pump). The
        // render side drops its position-derived caches on it - a shrine fast-travel keeps
        // the same pawn and world, so nothing else signals it.
        std::uint64_t teleport_ms = 0;

        wchar_t pawn_name[96]{}; // pawn class / object name, for the debug readout
        wchar_t level_name[160]{}; // pawn full name (carries the world + level path)
        // The in-viewport Visible root widget holding menu_open true, or empty.
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

    // HUD placement. `Custom` obeys minimap_anchor / minimap_offset_* / compass_anchor as
    // written; any other value puts the minimap in that corner and moves the compass to
    // the same vertical side.
    enum class HudPreset : int
    {
        Custom = 0,
        TopLeft = 1,
        TopRight = 2,
        BottomLeft = 3,
        BottomRight = 4,
    };

    // How the x-ray highlight is armed. Toggle: one press on, the next off. Hold: on only
    // while the key (or pad chord) is physically down.
    enum class HighlightMode : int
    {
        Toggle = 0,
        Hold = 1,
    };

    // Which edge the compass strip hangs off. `compass_offset_y` is the distance from
    // that edge, so the key means the same thing in both directions.
    enum class VAnchor : int
    {
        Top = 0,
        Bottom = 1,
    };

    // How much the mod says. The logging contract is in the LOGGING block below.
    enum class LogLv : int
    {
        Normal = 0,
        Verbose = 1,
        Trace = 2,
    };

    struct Config
    {
        //==============================================================================
        // The master switch
        //==============================================================================
        //
        // `mod_enabled = 0` stops all measurable work: no DX12 hooks, the ProcessEvent
        // callback returns on its first statement, no object-array scan, the chapter's
        // height maps freed, no XInput. Only a 1 Hz stat() of this file keeps running on
        // the loop thread, which is what lets `mod_enabled = 1` turn it back on without
        // restarting the game (F5 is dead while it is off - nothing samples the keyboard).
        bool mod_enabled = true;

        // The three-switch ladder, in order of how much they stop:
        //   mod_enabled     = the whole mod (no hook, no scans, no map in memory)
        //   overlay_enabled = anything drawn (the reader and the sweep keep running)
        //   show_minimap    = just the minimap disc
        bool overlay_enabled = true;
        bool show_minimap = true;     // draw the minimap window

        //==============================================================================
        // UI scale, HUD placement, theme and palette
        //==============================================================================
        //
        // `ui_scale = auto` derives the factor from the back buffer's height
        // (clamp(h / 1080, 1, 4)); a number overrides it. The font size and the ImGui
        // style are scaled on the render thread and every PIXEL key below is multiplied
        // by the same factor at read time.
        //
        // Scaled: markers_size, map_marker_size, highlight_size, compass_height,
        // compass_offset_y, minimap_offset_x/y, minimap_min_px, minimap_arrow_min_px.
        // NOT scaled: minimap_size and compass_width, which are fractions already.
        //
        // `theme` is the chrome (frame, disc backdrop, label plates, walkable fill),
        // `palette` the marker hue set; the two axes are independent. A theme only
        // supplies a colour key the config file does not mention - `minimap_frame_color`,
        // `minimap_backdrop_color`, `minimap_frame_alpha`, `minimap_backdrop`,
        // `floor_base_color` and `xray_rarity_colors` written in the file win over every
        // theme. Resolved once in load_config_file(), order-independent.
        gly::Theme theme = gly::Theme::Neutral;
        gly::Palette palette = gly::Palette::Default;

        bool ui_scale_auto = true;
        float ui_scale = 1.0f; // only consulted when ui_scale_auto is false
        HudPreset hud_preset = HudPreset::Custom;
        float size_frac = 0.24f;      // minimap side as a fraction of screen height
        float zoom_uu_per_px = 26.0f; // world uu per minimap pixel (smaller = closer)
        // The zoom ladder: `zoom_key` steps through these in order and wraps. Parsed and
        // stepped by mapview (parse_zoom_presets / next_zoom_preset).
        float minimap_zoom_presets[mv::kMaxZoomPresets] = {13.0f, 26.0f, 52.0f};
        int minimap_zoom_preset_count = 3;
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
        // A menu closing is not a level transition: showing the minimap again waits only
        // this long, not min_visible_after_state_ok_ms. Hiding is immediate.
        int menu_close_show_delay_ms = 150;

        //==============================================================================
        // Height slicing (what the minimap actually draws)
        //==============================================================================
        //
        // The map ships a multi-surface height map: four 16-bit PNGs holding the Z of up
        // to four stacked walkable surfaces per pixel. Every ~80 ms the overlay slices the
        // window around the player on the CPU:
        //
        //     |surfaceZ - feetZ| <= floor_z_tolerance -> opaque, shaded by the gradient
        //     nearest below within floor_fade_uu      -> adjacent_floor_opacity
        //     nearest above within floor_fade_uu      -> adjacent_floor_opacity x 0.6
        //     nothing                                 -> transparent
        //
        // The only smoothing is feet_z_smooth_ms.

        bool show_adjacent_floors = true;     // draw the surfaces below / above, dimmed
        float adjacent_floor_opacity = 0.25f; // below; above uses 0.6 x this
        float floor_z_tolerance = 200.0f;     // uu: |Z - feetZ| within this = my floor
        float floor_fade_uu = 800.0f;         // uu: how far below / above is still shown
        // Height gradient. lum = 1 + strength * clamp((surfaceZ - feetZ) / span, -1, 1)
        // with span = tolerance (current floor) or fade (dimmed). The same formula is
        // implemented offline in tools/navmesh/slice_preview.py.
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
        // merges every storey; the fallback for a chapter whose height maps are missing.
        bool fallback_use_composite = false;

        //==============================================================================
        // Markers
        //==============================================================================
        //
        // The static database is markers/<chapter>.json (schema
        // `wuchang-minimap-markers/1`); the live half is a class sweep on the game thread
        // that reads each marker actor's own state flag. `markers_categories` is the same
        // name list the F2 filter checkboxes drive.

        bool markers_enabled = true;  // draw markers at all
        bool markers_live = true;     // run the game-thread class sweep
        // Draw only the markers of the chapter the player is in. The static DB is one flat
        // set of all six chapters whose world bounds overlap heavily (chapter 4 covers
        // nearly all of chapter 1), so off paints other chapters on top - a debug setting.
        bool markers_filter_chapter = true;
        // The live sweep walks GUObjectArray in slices, one slice per game-thread pump
        // (src/scan_sched.hpp). `markers_rounds_per_sec` caps how often a full pass
        // starts; the chunk and period decide how finely it is sliced, i.e. how much
        // game-thread time one pump costs.
        int markers_rounds_per_sec = 1;   // full passes over the object array per second
        int markers_scan_chunk = scan::kChunkDefault;        // object slots per pump
        int markers_scan_period_ms = scan::kPeriodDefaultMs; // min ms between pumps
        std::uint32_t markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        // The F2 Player tab offers the inverse of this as a "Show found markers" checkbox.
        bool markers_hide_found = true;  // hide instead of dimming a found marker
        float markers_found_alpha = 0.3f;
        float markers_size = 6.5f;         // glyph radius in minimap pixels
        bool markers_clamp_to_edge = false; // keep out-of-range markers on the rim
        int markers_max_draw = 400;         // hard cap per frame, nearest first

        //==============================================================================
        // Absence as evidence of a collect
        //==============================================================================
        //
        // An item collected before the mod was installed has a static DB entry and no live
        // actor at all, so neither `dying` nor the (0,0,0) test can speak for it. With this
        // on, a marker whose owning level the game reports as loaded and that no full
        // object-array round has seen since that level streamed in is marked collected
        // after `markers_absence_rounds` consecutive confirming rounds. A marker whose
        // level cannot be matched is never marked. Predicate: mdb::absence_marks().
        bool markers_absence_marks = true;
        int markers_absence_rounds = 2;
        // A boss killed before the mod was installed never spawns again, so the
        // health-based defeat rule cannot fire for it. With this on, a boss marker also
        // counts as defeated when the `bossdoor_*` firepoint the game's level script names
        // for it is in the save's `UnlockedFirepoints` (mdb::boss_found_from_save).
        // Derived on every publish, never written to the found file, so flipping this key
        // undoes the mark.
        bool boss_defeat_from_save = true;
        // Chests keep their `Used` flag and pickups their `dying`, so absence means "taken"
        // only for those two. Shrines, doors and fog gates are not in the default set.
        std::uint32_t markers_absence_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup);

        // The found tracker: wuchang_minimap_found.txt, one stable id per line.
        bool found_tracker = true;
        int found_save_debounce_ms = 2000;
        // Which found file. `auto` runs the save-slot ladder in src/saveslot.hpp and writes
        // wuchang_minimap_found_<key>.txt; `shared` pins the global file; anything else is
        // used verbatim as the key. A fixed array, not std::string: Config is copied by
        // value onto the render thread every frame.
        char found_profile[32] = "auto";
        // The once-per-install toast naming the hotkeys; a sentinel file next to the config
        // makes it once.
        bool first_run_toast = true;
        // The shrine list panel on the full map (names, chapter, distance, travel).
        bool shrine_list = true;
        // Extra widget class-name prefixes that are NOT menus, on top of the built-in
        // table in scan_sched.hpp (`kNonMenuRoots`) - "a menu is open" means "an
        // in-viewport widget's Visibility is Visible", which a combat subtitle also
        // satisfies. Every root discovered for the first time is logged by name, so the
        // name to put here comes out of the log. Comma-separated, matched
        // case-insensitively as a prefix. A fixed array, not std::string: Config is copied
        // by value onto the render thread every frame.
        char menu_ignore_roots[192] = "";

        //==============================================================================
        // The full map
        //==============================================================================
        //
        // The same height-sliced asset the minimap draws, at map scale: a north-up,
        // pannable, zoomable window over the chapter with every marker on it. While it is
        // open the minimap is hidden and ImGui takes the mouse and the keyboard (never
        // latched - closing the map gives both back on the same frame).
        //
        // Zoom is world units per SCREEN pixel, the same unit as `minimap_zoom` (26).
        // 30 uu/px shows ~270 m across a 900 px canvas; chapter 1 is ~450 m wide, so ~55
        // fits a whole chapter.

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
        // Side of the dynamic texture the map slice is cut into. The cut costs roughly
        // (width x height x surfaces) plane reads - 768 x ~430 x 8 is ~2.6 M, 5-10 ms - so
        // it only runs when something changed.
        int map_slice_px = 768;
        int map_slice_hz = 6;        // hard cap on re-cuts per second
        bool map_gamepad = true;     // poll XInput while the map is open
        float map_gamepad_deadzone = 0.22f;
        bool map_waypoint_persist = true; // write wuchang_minimap_waypoint.txt

        //==============================================================================
        // X-ray highlight
        //==============================================================================
        //
        // While the highlight is on, every marker of an enabled category within
        // `highlight_radius` is drawn at its projected screen position - glyph, name and
        // distance in metres - fading with distance. The overlay draws over the scene:
        // no occlusion test, no render state near the game's.
        //
        // `highlight_mode` says how it is armed. Hold needs no unlatching. Toggle is
        // cleared from live state, never remembered: hl::drop_caches() (a level
        // transition, a dropped pawn) turns it off, and so does turning the feature off.
        // Nothing else may latch it on.

        bool highlight_enabled = true;
        HighlightMode highlight_mode = HighlightMode::Toggle;
        int highlight_key = 0xA4; // VK_LMENU - left Alt
        bool highlight_gamepad = true;
        // XInput chord (pad::kLeftShoulder | pad::kRightShoulder by default). The
        // triggers are analogue, so they are their own flags rather than mask bits.
        std::uint16_t highlight_pad_mask = 0x0300;
        bool highlight_pad_lt = false;
        bool highlight_pad_rt = false;
        float highlight_radius = 3000.0f; // uu (30 m)
        // Loot plus the people and landmarks worth spotting through a wall. Every category
        // mdb knows is selectable; this is the shipped starting point.
        std::uint32_t highlight_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup) |
            mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Boss) |
            mdb::cat_bit(mdb::Cat::Npc) | mdb::cat_bit(mdb::Cat::Note);
        // Draw collected loot too. Only chests, pickups and hidden items are suppressed by
        // this; shrines, bosses and NPCs are landmarks and show whatever their found state.
        bool highlight_show_found = false;
        int highlight_max_draw = 60;       // nearest first
        float highlight_alpha_near = 1.0f; // at the camera
        float highlight_alpha_far = 0.25f; // at highlight_radius
        float highlight_size = 7.0f;       // glyph radius, screen px
        bool highlight_labels = true;      // name + distance next to the glyph
        // How many drawn glyphs may carry a name. Labels are chosen nearest first, never
        // two for glyphs within highlight_size x 2 of each other, and laid out top to
        // bottom by src/label_layout.hpp so no two boxes overlap.
        int highlight_labels_max = 12;
        bool highlight_edge_arrows = true; // off-screen / behind: an arrow on the rim
        // Item quality colours. Under the x-ray, a marker whose static DB entry carries a
        // rarity tier above 0 is drawn - glyph, label and edge arrow - in that tier's
        // colour instead of its category colour. Tier 0 (every chest, every live-only
        // actor, every ordinary consumable) keeps its category colour. Defaults are the
        // game's own pickup-beam palette (mdb::kDefaultRarityColors).
        bool xray_rarity_colors_enabled = true;
        mdb::Rgb xray_rarity_colors[mdb::kRarityCount] = {
            mdb::kDefaultRarityColors[0], mdb::kDefaultRarityColors[1], mdb::kDefaultRarityColors[2]};
        // The same tint on the minimap / full-map / compass glyphs; those views read as a
        // category map.
        bool markers_rarity_tint = false;
        // How often the game thread re-reads the camera while the highlight is armed. The
        // read is a handful of raw doubles at a cached offset.
        int highlight_camera_hz = 60;
        // The camera reader's rates and discovery bounds (src/highlight.cpp).
        // `..._resolve_ms` - how often APlayerCameraManager is re-found when missing;
        // `..._compass_period_ms` - the slower rate when only the compass wants a heading;
        // `..._getter_period_ms` - paces the ProcessEvent fallback route;
        // `..._pov_scan_bytes` - how far into CameraCachePrivate the POV block is sought;
        // `..._pov_bad_reads` - insane reads in a row that drop the pinned offset.
        int highlight_camera_resolve_ms = 500;
        int highlight_compass_period_ms = 50;
        int highlight_getter_period_ms = 33;
        int highlight_pov_scan_bytes = 192;
        int highlight_pov_bad_reads = 8;

        //==============================================================================
        // The compass strip
        //==============================================================================
        //
        // A heading strip: N/E/S/W plus 15-degree ticks derived from the camera yaw (the
        // pawn's yaw when no camera pose is fresh), with bearing pips for the waypoint and
        // nearby markers of the selected categories. Hidden by the same evaluation as the
        // minimap - no second set of show/hide rules, no second latch.

        bool compass_enabled = true;
        float compass_width = 0.34f;    // fraction of the screen width
        // The filled plate behind the strip. Off = ticks and letters only, each drawn
        // with a one-pixel shadow so they survive on a bright scene.
        bool compass_plate = true;
        // Which edge the strip hangs off, and how far from it. A non-custom
        // `hud_preset` overrides the anchor (top presets -> Top, bottom -> Bottom).
        VAnchor compass_anchor = VAnchor::Top;
        float compass_offset_y = 18.0f; // px from that edge (scaled by ui_scale)
        float compass_height = 26.0f;   // px
        float compass_span_deg = 120.0f; // degrees visible across the strip
        float compass_opacity = 0.9f;
        std::uint32_t compass_categories = mdb::cat_bit(mdb::Cat::Shrine) |
                                           mdb::cat_bit(mdb::Cat::Boss) |
                                           mdb::cat_bit(mdb::Cat::Elite) |
                                           mdb::cat_bit(mdb::Cat::FogGate);
        float compass_marker_distance = 15000.0f; // uu (150 m)
        bool compass_show_waypoint = true;
        // The strip's minor-tick spacing in degrees (a multiple of 45 is a labelled
        // major tick, a multiple of 90 a cardinal letter), and the hard cap on how many
        // marker pips it draws, nearest first.
        float compass_tick_step_deg = 15.0f;
        int compass_max_pips = 32;
        // Pip label: horizontal distance in metres under (or over, on a bottom-anchored
        // strip) the glyph, plus an up/down arrow. Within compass_pip_height_uu the marker
        // counts as being on this floor and no arrow is drawn.
        bool compass_pip_labels = true;
        float compass_pip_height_uu = 300.0f; // 3 m

        //==============================================================================
        // Minimap look
        //==============================================================================
        //
        // Everything the minimap draws that is not the map itself. All live - the next
        // frame uses the new value.

        float minimap_backdrop = 0.86f;    // alpha of the dark disc under the map
        float minimap_backdrop_r = 6.0f;   // and its colour, 0..255
        float minimap_backdrop_g = 9.0f;
        float minimap_backdrop_b = 13.0f;
        float minimap_frame_r = 168.0f;    // the ring / border around the minimap
        float minimap_frame_g = 176.0f;
        float minimap_frame_b = 186.0f;
        float minimap_frame_alpha = 0.85f;
        // The composite fallback merges every storey, so it draws weaker than a slice.
        float minimap_composite_alpha = 0.85f;
        float minimap_min_px = 72.0f;      // floor on the side length, whatever the fraction says
        float minimap_arrow_frac = 0.055f; // player arrow, as a fraction of the side
        float minimap_arrow_min_px = 8.0f;
        // Waypoint glyph radius, as a multiple of markers_size. The waypoint is never
        // culled.
        float waypoint_size_scale = 1.05f;

        //==============================================================================
        // The game-state reader (src/gamestate.cpp, game thread)
        //==============================================================================
        //
        // The rates of the ProcessEvent pump; re-read at most twice a second and live.
        // Lower periods cost game-thread time: the position pump gates the marker scan and
        // the widget test, and the widget sweep is a FindAllOf, a whole object-array walk.

        int reader_position_period_ms = 100;      // 10 Hz: pawn location + yaw
        int reader_resolve_period_ms = 500;       // 2 Hz: FindAllOf for the pawn / controller
        int reader_widget_sweep_period_ms = 250;  // the full menu-widget sweep, fast cadence
        // The sweep is a whole-object-array walk (28-51 ms) and only has to DISCOVER menu
        // roots never seen before; roots already on the watchlist are re-tested on every
        // 10 Hz pump. The period doubles up to this maximum while nothing new is found and
        // drops back to the fast cadence for reader_widget_sweep_warm_ms after any
        // menu-state flip, teleport or world change.
        int reader_widget_sweep_max_period_ms = 2000;
        int reader_widget_sweep_warm_ms = 2000;
        int reader_transition_cooldown_ms = 2000; // no UFunction call for this long after a pawn/world change
        double reader_teleport_jump_uu = 3000.0;  // a position jump this big in one pump is a fast travel
        int reader_chapter_period_ms = 1000;      // how often the streamed level set is named
        int reader_log_throttle_ms = 5000;        // "no pawn" / "rejected class" lines

        // Rounds a live actor may go unseen before it is dropped from the live cache; two
        // stops a marker flickering when a sweep races level streaming. NOT a collected
        // test (see markers_absence_*).
        int markers_live_grace_rounds = 2;

        //==============================================================================
        // Assets and diagnostics
        //==============================================================================

        // How long a retired chapter's height planes stay alive after the pointer to them
        // is cleared, so a render thread already inside a slice (~4 ms) cannot fault.
        int map_asset_retire_grace_ms = 2000;
        // Rate limit on the "hidden because: ..." log line.
        int hide_reason_log_ms = 2000;
        // Size of the SRV descriptor heap (font atlas + map + the four slice buffers).
        // RESTART ONLY: the heap is created once, when the overlay first initialises.
        int srv_heap_size = 64;

        bool debug_readout = false;
        bool debug_show_panel_on_start = false; // main-menu verification aid
        int panel_key = 0x71;                   // VK_F2
        int reload_key = 0x74;                  // VK_F5
        int map_key = 0x4D;                     // 'M' - full map
        int map_recenter_key = 0x52;            // 'R' - recentre the full map on the player
        // Cycles minimap_zoom_presets. 'N' is bound by neither the game, this machine's
        // injected DLLs (RenoDX F6, Steam F12) nor the rest of the mod.
        int zoom_key = 0x4E;                    // 'N' - cycle the minimap zoom
        // Copies the full map to the clipboard. Live only while the full map is open, and
        // map mode swallows the whole keyboard, so a plain letter cannot reach the game.
        int screenshot_key = 0x43;              // 'C' - full map -> clipboard

        // wuchang_minimap_last_stage.txt, rewritten with CreateFileW/WriteFile at every
        // overlay stage transition. The UE4SS log buffer can be lost when the process
        // dies; a file closed after each write cannot be.
        bool crash_breadcrumb = true;
        // How much the mod writes to UE4SS.log and wuchang_minimap.log. `normal` is what a
        // bug report needs; `verbose` adds the running commentary; `trace` adds the
        // per-publish censuses. See the LOGGING block below.
        LogLv log_level = LogLv::Normal;
        // Shrine fast travel. Off until the in-game reflection self-check confirms the
        // call route.
        bool fast_travel_enabled = false;
        // Route 1 of the save-slot ladder CALLS `Get Save Slot Value`. Off until the recon
        // dump shows the parameter is not engine-owned - see saveslot.cpp for the hazard.
        bool saveslot_uuid_call = false;

        //------------------------------------------------------------------------------
        // New fields go at the END of the struct so no existing offset moves, and each
        // stays a POD (see the static_assert below).
        //------------------------------------------------------------------------------

        // Opens the full map from a controller: an XInput button mask, spelled in the
        // config the way `highlight_pad_chord` is ("BACK+Y"), pressed as a chord. `none`
        // disables it.
        std::uint16_t map_pad_open_chord = 0x8020; // pad::kY | pad::kBack

        // The UI font, rasterised at 13 * ui_scale (ImGui's built-in font is a 13 px
        // bitmap). A missing or unreadable file falls back to the built-in ProggyClean and
        // says so in the log. A fixed array, not std::string: see found_profile above.
        char ui_font[192] = "C:\\Windows\\Fonts\\segoeui.ttf";

        // On: `minimap_zoom` and `map_zoom` are multiplied by ui_scale like every other
        // pixel key, so one config shows the same area of the world at 1080p and 2160p.
        // Off: uu/px is literal and the 4K disc shows twice the radius.
        bool zoom_dpi_scaled = true;
    };

    // Config is a value: copied by value onto the render thread every frame, published
    // under a spinlock, compared field by field by operator== below. A std::string member
    // would break all three silently.
    static_assert(std::is_trivially_copyable_v<Config>, "Config is copied by value per frame");

    //==================================================================================
    // Config equality, field by field
    //==================================================================================
    //
    // Written out rather than memcmp'd, and generated from the struct declaration, so it
    // lists every field exactly once. The F2 panel and the full map use it to decide
    // whether to publish a config: a false "changed" is a publish per frame, a false
    // "same" is a panel whose sliders do nothing.
    //
    // The drift guard in tests/markers_test.cpp flips every byte of a Config in turn and
    // requires this to notice, so a field added to the struct and forgotten here fails the
    // build's test step. That is also why this is header-only - the test links
    // markers_db / mapview / compass and not mmstate.cpp.
    //
    // Arrays are compared element by element through the `eq` template.
    namespace detail
    {
        template <typename T, std::size_t N>
        bool eq(const T (&x)[N], const T (&y)[N])
        {
            for (std::size_t i = 0; i < N; ++i)
            {
                if (!(x[i] == y[i]))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace detail

    inline bool operator==(const Config& a, const Config& b)
    {
        return
        a.mod_enabled == b.mod_enabled &&
        a.overlay_enabled == b.overlay_enabled &&
        a.show_minimap == b.show_minimap &&
        a.theme == b.theme &&
        a.palette == b.palette &&
        a.ui_scale_auto == b.ui_scale_auto &&
        a.ui_scale == b.ui_scale &&
        a.hud_preset == b.hud_preset &&
        a.size_frac == b.size_frac &&
        a.zoom_uu_per_px == b.zoom_uu_per_px &&
        detail::eq(a.minimap_zoom_presets, b.minimap_zoom_presets) &&
        a.minimap_zoom_preset_count == b.minimap_zoom_preset_count &&
        a.round == b.round &&
        a.anchor == b.anchor &&
        a.offset_x == b.offset_x &&
        a.offset_y == b.offset_y &&
        a.rotate_with_player == b.rotate_with_player &&
        a.opacity == b.opacity &&
        a.hide_in_menus == b.hide_in_menus &&
        a.require_pawn_view == b.require_pawn_view &&
        a.state_stale_ms == b.state_stale_ms &&
        a.min_visible_after_state_ok_ms == b.min_visible_after_state_ok_ms &&
        a.menu_close_show_delay_ms == b.menu_close_show_delay_ms &&
        a.show_adjacent_floors == b.show_adjacent_floors &&
        a.adjacent_floor_opacity == b.adjacent_floor_opacity &&
        a.floor_z_tolerance == b.floor_z_tolerance &&
        a.floor_fade_uu == b.floor_fade_uu &&
        a.floor_gradient_strength == b.floor_gradient_strength &&
        a.floor_base_r == b.floor_base_r &&
        a.floor_base_g == b.floor_base_g &&
        a.floor_base_b == b.floor_base_b &&
        a.slice_hz == b.slice_hz &&
        a.feet_z_smooth_ms == b.feet_z_smooth_ms &&
        a.player_z_offset == b.player_z_offset &&
        a.fallback_use_composite == b.fallback_use_composite &&
        a.markers_enabled == b.markers_enabled &&
        a.markers_live == b.markers_live &&
        a.markers_filter_chapter == b.markers_filter_chapter &&
        a.markers_rounds_per_sec == b.markers_rounds_per_sec &&
        a.markers_scan_chunk == b.markers_scan_chunk &&
        a.markers_scan_period_ms == b.markers_scan_period_ms &&
        a.markers_categories == b.markers_categories &&
        a.markers_hide_found == b.markers_hide_found &&
        a.markers_found_alpha == b.markers_found_alpha &&
        a.markers_size == b.markers_size &&
        a.markers_clamp_to_edge == b.markers_clamp_to_edge &&
        a.markers_max_draw == b.markers_max_draw &&
        a.markers_absence_marks == b.markers_absence_marks &&
        a.markers_absence_rounds == b.markers_absence_rounds &&
        a.boss_defeat_from_save == b.boss_defeat_from_save &&
        a.markers_absence_categories == b.markers_absence_categories &&
        a.found_tracker == b.found_tracker &&
        a.found_save_debounce_ms == b.found_save_debounce_ms &&
        detail::eq(a.found_profile, b.found_profile) &&
        a.first_run_toast == b.first_run_toast &&
        a.shrine_list == b.shrine_list &&
        detail::eq(a.menu_ignore_roots, b.menu_ignore_roots) &&
        a.map_zoom == b.map_zoom &&
        a.map_zoom_min == b.map_zoom_min &&
        a.map_zoom_max == b.map_zoom_max &&
        a.map_zoom_factor == b.map_zoom_factor &&
        a.map_pan_speed == b.map_pan_speed &&
        a.map_margin == b.map_margin &&
        a.map_backdrop == b.map_backdrop &&
        a.map_marker_size == b.map_marker_size &&
        a.map_markers_max_draw == b.map_markers_max_draw &&
        a.map_floor_step == b.map_floor_step &&
        a.map_show_all_floors == b.map_show_all_floors &&
        a.map_slice_px == b.map_slice_px &&
        a.map_slice_hz == b.map_slice_hz &&
        a.map_gamepad == b.map_gamepad &&
        a.map_gamepad_deadzone == b.map_gamepad_deadzone &&
        a.map_waypoint_persist == b.map_waypoint_persist &&
        a.highlight_enabled == b.highlight_enabled &&
        a.highlight_mode == b.highlight_mode &&
        a.highlight_key == b.highlight_key &&
        a.highlight_gamepad == b.highlight_gamepad &&
        a.highlight_pad_mask == b.highlight_pad_mask &&
        a.highlight_pad_lt == b.highlight_pad_lt &&
        a.highlight_pad_rt == b.highlight_pad_rt &&
        a.highlight_radius == b.highlight_radius &&
        a.highlight_categories == b.highlight_categories &&
        a.highlight_show_found == b.highlight_show_found &&
        a.highlight_max_draw == b.highlight_max_draw &&
        a.highlight_alpha_near == b.highlight_alpha_near &&
        a.highlight_alpha_far == b.highlight_alpha_far &&
        a.highlight_size == b.highlight_size &&
        a.highlight_labels == b.highlight_labels &&
        a.highlight_labels_max == b.highlight_labels_max &&
        a.highlight_edge_arrows == b.highlight_edge_arrows &&
        a.xray_rarity_colors_enabled == b.xray_rarity_colors_enabled &&
        detail::eq(a.xray_rarity_colors, b.xray_rarity_colors) &&
        a.markers_rarity_tint == b.markers_rarity_tint &&
        a.highlight_camera_hz == b.highlight_camera_hz &&
        a.highlight_camera_resolve_ms == b.highlight_camera_resolve_ms &&
        a.highlight_compass_period_ms == b.highlight_compass_period_ms &&
        a.highlight_getter_period_ms == b.highlight_getter_period_ms &&
        a.highlight_pov_scan_bytes == b.highlight_pov_scan_bytes &&
        a.highlight_pov_bad_reads == b.highlight_pov_bad_reads &&
        a.compass_enabled == b.compass_enabled &&
        a.compass_width == b.compass_width &&
        a.compass_plate == b.compass_plate &&
        a.compass_anchor == b.compass_anchor &&
        a.compass_offset_y == b.compass_offset_y &&
        a.compass_height == b.compass_height &&
        a.compass_span_deg == b.compass_span_deg &&
        a.compass_opacity == b.compass_opacity &&
        a.compass_categories == b.compass_categories &&
        a.compass_marker_distance == b.compass_marker_distance &&
        a.compass_show_waypoint == b.compass_show_waypoint &&
        a.compass_tick_step_deg == b.compass_tick_step_deg &&
        a.compass_max_pips == b.compass_max_pips &&
        a.compass_pip_labels == b.compass_pip_labels &&
        a.compass_pip_height_uu == b.compass_pip_height_uu &&
        a.minimap_backdrop == b.minimap_backdrop &&
        a.minimap_backdrop_r == b.minimap_backdrop_r &&
        a.minimap_backdrop_g == b.minimap_backdrop_g &&
        a.minimap_backdrop_b == b.minimap_backdrop_b &&
        a.minimap_frame_r == b.minimap_frame_r &&
        a.minimap_frame_g == b.minimap_frame_g &&
        a.minimap_frame_b == b.minimap_frame_b &&
        a.minimap_frame_alpha == b.minimap_frame_alpha &&
        a.minimap_composite_alpha == b.minimap_composite_alpha &&
        a.minimap_min_px == b.minimap_min_px &&
        a.minimap_arrow_frac == b.minimap_arrow_frac &&
        a.minimap_arrow_min_px == b.minimap_arrow_min_px &&
        a.waypoint_size_scale == b.waypoint_size_scale &&
        a.reader_position_period_ms == b.reader_position_period_ms &&
        a.reader_resolve_period_ms == b.reader_resolve_period_ms &&
        a.reader_widget_sweep_period_ms == b.reader_widget_sweep_period_ms &&
        a.reader_widget_sweep_max_period_ms == b.reader_widget_sweep_max_period_ms &&
        a.reader_widget_sweep_warm_ms == b.reader_widget_sweep_warm_ms &&
        a.reader_transition_cooldown_ms == b.reader_transition_cooldown_ms &&
        a.reader_teleport_jump_uu == b.reader_teleport_jump_uu &&
        a.reader_chapter_period_ms == b.reader_chapter_period_ms &&
        a.reader_log_throttle_ms == b.reader_log_throttle_ms &&
        a.markers_live_grace_rounds == b.markers_live_grace_rounds &&
        a.map_asset_retire_grace_ms == b.map_asset_retire_grace_ms &&
        a.hide_reason_log_ms == b.hide_reason_log_ms &&
        a.srv_heap_size == b.srv_heap_size &&
        a.debug_readout == b.debug_readout &&
        a.debug_show_panel_on_start == b.debug_show_panel_on_start &&
        a.panel_key == b.panel_key &&
        a.reload_key == b.reload_key &&
        a.map_key == b.map_key &&
        a.map_recenter_key == b.map_recenter_key &&
        a.zoom_key == b.zoom_key &&
        a.screenshot_key == b.screenshot_key &&
        a.crash_breadcrumb == b.crash_breadcrumb &&
        a.log_level == b.log_level &&
        a.fast_travel_enabled == b.fast_travel_enabled &&
        a.saveslot_uuid_call == b.saveslot_uuid_call &&
        a.map_pad_open_chord == b.map_pad_open_chord &&
        detail::eq(a.ui_font, b.ui_font) &&
        a.zoom_dpi_scaled == b.zoom_dpi_scaled &&
               true;
    }

    inline bool operator!=(const Config& a, const Config& b)
    {
        return !(a == b);
    }

    // The config lives here and is copied under a spinlock. The loop thread writes it
    // on load / F5; the render thread writes it when the F2 panel is used.
    Config config();
    void set_config(const Config& cfg);

    //==================================================================================
    // The generation-cached config (the ONLY form allowed on a hot path)
    //==================================================================================
    //
    // `config()` takes a spinlock and copies ~1 KB of struct. `cfg_cached()` keeps a
    // per-THREAD copy and refreshes it only when `set_config` has bumped `g_cfg_gen`:
    // steady state is one relaxed atomic load and a reference return.
    //
    // The reference is valid until the next `cfg_cached()` call ON THE SAME THREAD and
    // must never be handed to another thread. Cold paths (file I/O, panel Save, one-off
    // setup) may keep using `config()`.
    extern std::atomic<std::uint32_t> g_cfg_gen;

    const Config& cfg_cached();

    //==================================================================================
    // Per-activity performance counters (perf.hpp)
    //==================================================================================
    //
    // Every periodic activity registers one counter and records how long each invocation
    // took; the F2 debug block prints the table. Recording is lock-free and
    // allocation-free (perf.hpp) - it runs on the game thread inside ProcessEvent.

    // Microseconds from QueryPerformanceCounter, with the frequency read exactly once.
    std::uint64_t qpc_us();

    // `name` must have static storage. Registering the same pointer twice returns the
    // same id, so a `static const int` at a call site is the intended idiom.
    int perf_register(const char* name, perf::Thread thread);

    // Records one invocation that started at `t0_us` (from qpc_us()).
    void perf_record(int id, std::uint64_t t0_us);

    // Records one invocation whose duration the caller measured.
    void perf_record_ms(int id, double ms);

    // The table, for the F2 panel. Read-only by convention.
    const perf::Table& perf_table();
    void perf_reset_peaks();

    // The stall gate, any thread: "the process is not running normally for the next `ms`
    // milliseconds" - a loading screen, a swapchain resize, a reload, a clipboard copy.
    // Every perf_record inside that window counts as a stall instead of setting the peak
    // the F2 table shows, because all three threads measure WALL CLOCK and a cross-thread
    // user32 call blocks for as long as a synchronous load takes.
    //
    // Only externally attributable events call it; a stall is never inferred from a
    // sample's own duration.
    void perf_note_stall(const wchar_t* why, unsigned ms);
    bool perf_in_stall();
    // What the last note said, for the F2 table's footnote. Never null.
    const wchar_t* perf_last_stall();

    // RAII: times the enclosing scope into counter `id`.
    class PerfScope
    {
      public:
        explicit PerfScope(int id) noexcept : id_(id), t0_(qpc_us())
        {
        }
        ~PerfScope()
        {
            perf_record(id_, t0_);
        }
        PerfScope(const PerfScope&) = delete;
        PerfScope& operator=(const PerfScope&) = delete;

      private:
        int id_;
        std::uint64_t t0_;
    };

    //==================================================================================
    // The master switch, as a lock-free flag
    //==================================================================================
    //
    // `mod_active()` is one relaxed atomic load, so a disabled mod costs exactly that per
    // ProcessEvent and per Present.
    //
    // It is NOT `config().mod_enabled`: the switch goes off before the subsystems have
    // finished standing down, so `modswitch` owns and drives the flag and everything else
    // only reads it.
    extern std::atomic<bool> g_mod_active;

    inline bool mod_active()
    {
        return g_mod_active.load(std::memory_order_relaxed);
    }

    // Loop thread. Reads ONLY `mod_enabled` out of the config file, leaving the live
    // config untouched. False when the file cannot be read or carries no such line.
    bool peek_mod_enabled(bool& out);

    // Loop thread. The two config files' FILETIMEs mixed into one uint64, or 0 when
    // neither can be stat()ed. The watcher only parses when this changes, so editing
    // EITHER file reloads both.
    std::uint64_t config_mtime();

    // Loop thread only (plain Win32 file I/O, no iostreams).
    void load_config_file();
    void save_config_file();
    std::wstring config_path();
    // config_wuchang_minimap_dev.txt - the Tier::Dev overlay. Not shipped in the
    // release zip; parsed only when it exists, after the main file.
    std::wstring dev_config_path();
    // "a Save will write the dev file too" - true once a dev file has been read or a Dev
    // key moved off its default, the two conditions save_config_file() uses. Any thread.
    bool dev_config_active();
    std::wstring mod_dir();

    // Every setting as `key` -> its text form, in cfgkeys order (Player, Advanced,
    // Dev). The single source of the VALUES; the layout of the files is owned by
    // config_rewrite.hpp. Any thread (pure).
    std::vector<std::pair<std::string, std::string>> config_kv(const Config& cfg);

    //==================================================================================
    // A BINDING IS A VIRTUAL KEY PLUS AT MOST ONE MODIFIER
    //==================================================================================
    //
    // Every `*_key` config value is an int carrying both halves: the virtual key in bits
    // 0..7 (every VK is <= 0xFF) and the modifier in bits 8..9. Keeps Config trivially
    // copyable.
    //
    // One modifier, not a set. The config file spells it as it reads - `map_key = ctrl+m` -
    // and key_name() writes that spelling back, so a binding always round-trips.
    //
    // A binding with NO modifier does not require the modifiers to be up: the x-ray hold
    // key is Alt, and demanding a clean Alt would kill every other hotkey while it is
    // held. The Bindings tab names that overlap instead.
    enum : int
    {
        kKeyModNone = 0,
        kKeyModCtrl = 1,
        kKeyModShift = 2,
        kKeyModAlt = 3,
    };

    inline constexpr int key_vk(int binding)
    {
        return binding & 0xFF;
    }

    inline constexpr int key_mod(int binding)
    {
        return (binding >> 8) & 0x3;
    }

    inline constexpr int key_make(int vk, int mod)
    {
        return (vk & 0xFF) | ((mod & 0x3) << 8);
    }

    // "F2", "M", "CTRL+M", "TAB", "LALT", ... - the same spelling the config file uses,
    // modifier included. Any thread.
    std::wstring key_name(int binding);

    // "LB+RB", "A", "LT+RT", "none" - the gamepad chord spelling the config file uses.
    // Any thread.
    std::wstring pad_chord_name(std::uint16_t mask, bool lt, bool rt);

    // Can this virtual key be written into the config file and read back? The Bindings
    // tab's capture only accepts keys this returns true for. Any thread (pure).
    bool vk_bindable(int vk);

    // Every bindable virtual key, ascending - what the capture widget scans.
    const std::vector<int>& bindable_vks();

    // Parse a gamepad chord ("LB+RB", "A", "none") into the three fields the config
    // carries. Returns true when any of them changed. Any thread.
    bool set_pad_chord(const std::string& text, std::uint16_t& mask, bool& lt, bool& rt);

    //==================================================================================
    // The waypoint
    //==================================================================================
    //
    // One waypoint at a time, set on the full map and drawn on both the full map and the
    // minimap (edge-clamped, with a distance). It lives in its own file so a waypoint set
    // during play survives without anyone pressing Save.
    //
    // The render thread sets it (a right-click on the map); the loop thread writes the
    // file. The handover is the spinlocked-copy pattern the config uses.

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
    // Panel "Revert": re-read the config files only (no map / marker reload) and
    // publish them, throwing away every unsaved edit made in the panel.
    extern std::atomic<bool> g_revert_config;
    extern std::atomic<bool> g_save_config;     // panel -> loop thread saves
    extern std::atomic<bool> g_panel_drew_frame; // set by the render thread, for the log
    // The Bindings tab is waiting for a key press. While it is set the WndProc hook
    // swallows the whole keyboard, so the key being captured cannot also reach the
    // game. Set and cleared by the render thread; read by the WndProc hook.
    extern std::atomic<bool> g_key_capture;
    extern std::atomic<bool> g_waypoint_dirty;   // render -> loop: write the waypoint file

    //==================================================================================
    // Logging that is safe from any thread
    //==================================================================================
    //
    // Output::send touches fmt and, through it, the C++ locale, which is fatal on the
    // game thread. So every thread except the loop thread only queues text; the loop
    // thread drains the queue in on_update.
    //
    // The queue is bounded and never blocks: a non-loop thread takes a spinlock, pushes
    // one string and leaves; past `kLogQueueMax` entries the line is dropped and counted,
    // and the loop thread reports "log: dropped N line(s)" on its next drain. A log call
    // never waits on the disk (the file buffer has a lock of its own - see mmstate.cpp).
    //
    // Three levels, one key (`log_level`, Advanced tier):
    //   normal   - what a bug report needs: the startup header, config / map / marker /
    //              shrine loads, chapter, level and save-slot changes, hook installation,
    //              every error and warning, every first-occurrence diagnostic, the
    //              watchdog, the breadcrumb, persisted-state changes, a health summary at
    //              most once a minute.
    //   verbose  - the periodic running commentary: `state:` at its real cadence,
    //              per-round marker timings, menu-state transitions, hide/show reasons,
    //              x-ray toggles, panel and full-map open/close.
    //   trace    - everything, including per-publish censuses.
    //
    // A suppressed line costs one relaxed atomic load. The macros wrap the whole call, so
    // neither `std::format` nor the argument expressions run when the level is off.
    // `mm::log` / `mm::logf` are the NORMAL level and always emit.
    //
    //   MM_LOGV(fmt, ...)  / MM_LOGVS(str)   - verbose
    //   MM_LOGT(fmt, ...)  / MM_LOGTS(str)   - trace

    // Set from the config on every load; read by every log macro.
    extern std::atomic<int> g_log_level;

    inline bool log_enabled(LogLv lv) noexcept
    {
        return static_cast<int>(lv) <= g_log_level.load(std::memory_order_relaxed);
    }

    // "normal" / "verbose" / "trace" <-> LogLv, for the config parser, the writer and
    // the F2 panel. Pure; unknown names leave the fallback in place and return false.
    const char* log_level_name(LogLv lv) noexcept;
    bool log_level_from_name(std::string_view name, LogLv& out) noexcept;

    void log(const std::wstring& line);
    void set_loop_thread();
    void drain_log();

    // The mod's own rolling log, `wuchang_minimap.log` in the mod folder, rotated per
    // launch keeping `.1` / `.2` / `.3`. Every line that reaches UE4SS's log goes here too
    // - UE4SS truncates its own log on every launch. Writes are buffered; `modlog_flush()`
    // is safe from any thread and `modlog_tick()` flushes every few seconds from the loop
    // thread.
    //
    // The file is capped at `kModLogMaxBytes` per session: at the cap it writes one "log
    // capped" line and stops. Rotation is unaffected.

    // Compares the game build the shipped marker / map data was dumped from against the
    // build now running. One line, once, at startup; silent at the normal level when the
    // data carries no stamp. Loop thread; plain Win32 file I/O.
    void check_game_build();

    void modlog_flush();
    void modlog_tick(std::uint64_t now_ms);
    std::wstring modlog_path();

    template <typename... Args>
    void logf(std::wformat_string<Args...> fmt, Args&&... args)
    {
        log(std::format(fmt, std::forward<Args>(args)...));
    }

    // Level-checked form for a site where a macro will not do. The format never runs when
    // the level is off, but the ARGUMENTS still do - hence the macros as the default.
    template <typename... Args>
    void logf_at(LogLv lv, std::wformat_string<Args...> fmt, Args&&... args)
    {
        if (log_enabled(lv))
        {
            log(std::format(fmt, std::forward<Args>(args)...));
        }
    }
} // namespace mm

#define MM_LOGV(...)                                                                                 \
    do                                                                                               \
    {                                                                                                \
        if (::mm::log_enabled(::mm::LogLv::Verbose))                                                 \
        {                                                                                            \
            ::mm::logf(__VA_ARGS__);                                                                 \
        }                                                                                            \
    } while (false)

#define MM_LOGVS(line)                                                                               \
    do                                                                                               \
    {                                                                                                \
        if (::mm::log_enabled(::mm::LogLv::Verbose))                                                 \
        {                                                                                            \
            ::mm::log(line);                                                                         \
        }                                                                                            \
    } while (false)

#define MM_LOGT(...)                                                                                 \
    do                                                                                               \
    {                                                                                                \
        if (::mm::log_enabled(::mm::LogLv::Trace))                                                   \
        {                                                                                            \
            ::mm::logf(__VA_ARGS__);                                                                 \
        }                                                                                            \
    } while (false)

#define MM_LOGTS(line)                                                                               \
    do                                                                                               \
    {                                                                                                \
        if (::mm::log_enabled(::mm::LogLv::Trace))                                                   \
        {                                                                                            \
            ::mm::log(line);                                                                         \
        }                                                                                            \
    } while (false)
