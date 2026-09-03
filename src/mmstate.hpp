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
#include <string_view>
#include <utility>
#include <vector>

#include "glyphs.hpp"
#include "mapview.hpp"
#include "markers_db.hpp"
#include "perf.hpp"
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
        std::uint32_t menu_roots_cached = 0; // in-viewport roots open right now
        // Widgets on the menu WATCHLIST (every root ever confirmed in the viewport this
        // session; all of them are re-tested on every pump) and the period the FindAllOf
        // discovery sweep has currently backed off to.
        std::uint32_t menu_watch_count = 0;
        std::uint32_t widget_sweep_period_ms = 0;
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

    // HUD PLACEMENT PRESET. `Custom` is the shipped default and means "obey
    // minimap_anchor / minimap_offset_* / compass_anchor exactly as written" - i.e.
    // v0.9.1's behaviour, unchanged. Any other value puts the minimap in that corner
    // AND moves the compass to the same vertical side, so one key relocates the whole
    // HUD instead of three that can disagree with each other.
    enum class HudPreset : int
    {
        Custom = 0,
        TopLeft = 1,
        TopRight = 2,
        BottomLeft = 3,
        BottomRight = 4,
    };

    // How the x-ray highlight is armed. Toggle is the default: one press turns it on,
    // the next turns it off. Hold is the original behaviour - it is only on while the
    // key (or the pad chord) is physically down.
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

    // How much the mod says. Declared here because it is a Config field; the whole
    // logging contract, and the macros that make a suppressed line free, are in the
    // LOGGING block near the bottom of this file.
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
        // `mod_enabled = 0` stops the mod doing ANY measurable work: the DX12 hooks are
        // not installed (or are disabled again, after the render thread has torn its own
        // objects down), the ProcessEvent game-thread callback returns on its first
        // statement, no object-array scan runs, the chapter's height maps are freed and
        // XInput is never polled. The one thing that keeps running is a 1 Hz stat() of
        // this file on the loop thread, which is what lets `mod_enabled = 1` turn it
        // back on without restarting the game (F5 is unavailable while it is off -
        // nothing samples the keyboard).
        //
        // It is deliberately SEPARATE from `enabled` below: `enabled` is "draw the
        // overlay", and it still leaves the reader, the marker sweep and the map asset
        // running.
        bool mod_enabled = true;

        // Renamed from `enabled` in 0.9.2 (the old name is still accepted, with one
        // warning). The three-switch ladder, in order of how much they stop:
        //   mod_enabled     = the whole mod (no hook, no scans, no map in memory)
        //   overlay_enabled = anything we draw (the reader and the sweep keep running)
        //   show_minimap    = just the minimap disc
        bool overlay_enabled = true;
        bool show_minimap = true;     // draw the minimap window

        //==============================================================================
        // UI scale and HUD placement
        //==============================================================================
        //
        // ImGui draws at 13 px by default, which is a quarter of the intended physical
        // size on a 4K screen. `ui_scale = auto` derives the factor from the back
        // buffer's height (clamp(h / 1080, 1, 4)); a number overrides it. The font
        // size and the ImGui style are scaled on the render thread, and every PIXEL
        // config key below is multiplied by the same factor at read time - so a config
        // tuned at 1080p is correct at 4K without being re-tuned.
        //
        // Scaled: markers_size, map_marker_size, highlight_size, compass_height,
        // compass_offset_y, minimap_offset_x/y, minimap_min_px, minimap_arrow_min_px.
        // NOT scaled: minimap_size and compass_width, which are fractions already.
        //==============================================================================
        // Theme and palette (review-0.9.1 items 7 and 8)
        //==============================================================================
        //
        // TWO AXES, deliberately separate. `theme` is the CHROME - the minimap frame,
        // the disc backdrop, the dark plate behind every label and the walkable fill -
        // and `palette` is the MARKER HUE SET. They are independent because a player who
        // needs the colour-blind hues does not thereby want a different frame.
        //
        // PRECEDENCE: a theme only supplies a colour key the config file does not
        // mention. `minimap_frame_color` written out in the file wins over every theme,
        // for ever; the same holds for `minimap_backdrop_color`, `minimap_frame_alpha`,
        // `minimap_backdrop`, `floor_base_color` and `xray_rarity_colors`. That is
        // resolved once, in load_config_file(), and is order-independent - the theme
        // line may sit anywhere in the file.
        gly::Theme theme = gly::Theme::Neutral;
        gly::Palette palette = gly::Palette::Default;

        bool ui_scale_auto = true;
        float ui_scale = 1.0f; // only consulted when ui_scale_auto is false
        HudPreset hud_preset = HudPreset::Custom;
        float size_frac = 0.24f;      // minimap side as a fraction of screen height
        float zoom_uu_per_px = 26.0f; // world uu per minimap pixel (smaller = closer)
        // THE ZOOM LADDER. `zoom_key` steps through these in order and wraps; it is the
        // one HUD setting a player wants to change while walking, and until 0.9.3 it
        // could only be changed by opening the panel or editing this file. Parsed and
        // stepped by pure functions in mapview (parse_zoom_presets / next_zoom_preset).
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
        // Draw only the markers of the chapter the player is in. The static DB is one
        // flat set of all six chapters and their world bounds overlap heavily (chapter
        // 4 covers nearly all of chapter 1), so with this off the map paints other
        // chapters' markers on top of the current one. Off is a debugging setting.
        bool markers_filter_chapter = true;
        // The live sweep walks GUObjectArray in slices, one slice per game-thread pump
        // (src/scan_sched.hpp). `markers_rounds_per_sec` caps how often a FULL pass is
        // started; `markers_scan_chunk` and `markers_scan_period_ms` decide how finely
        // that pass is sliced, i.e. how much game-thread time a single pump can cost.
        // Raise the chunk for a fresher marker set, lower it if the frame time suffers.
        int markers_rounds_per_sec = 1;   // full passes over the object array per second
        int markers_scan_chunk = scan::kChunkDefault;        // object slots per pump
        int markers_scan_period_ms = scan::kPeriodDefaultMs; // min ms between pumps
        std::uint32_t markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        // Found markers are HIDDEN by default (0.9.3): the map exists to show what is
        // still out there, and a screen of dimmed already-collected glyphs is what the
        // player has to read past to find it. The F2 Player tab offers the inverse of
        // this as a "Show found markers" checkbox, so the setting reads the way the
        // question is asked.
        bool markers_hide_found = true;  // hide instead of dimming a found marker
        float markers_found_alpha = 0.3f;
        float markers_size = 6.5f;         // glyph radius in minimap pixels
        bool markers_clamp_to_edge = false; // keep out-of-range markers on the rim
        int markers_max_draw = 400;         // hard cap per frame, nearest first

        //==============================================================================
        // Absence as evidence of a collect
        //==============================================================================
        //
        // An item collected before the mod was installed leaves a static DB entry and no
        // live actor at all (the level saver parked it at (0,0,0) at load time and a GC
        // freed it), so neither `dying` nor the (0,0,0) test can speak for it and the
        // marker stayed drawn for ever. With this on, a marker whose OWNING LEVEL the
        // game reports as loaded, that no full object-array round has seen since that
        // level streamed in, is marked collected after `markers_absence_rounds`
        // consecutive confirming rounds. A marker whose level cannot be matched is never
        // marked. The predicate is pure and tested - mdb::absence_marks().
        bool markers_absence_marks = true;
        int markers_absence_rounds = 2;
        // A boss the player killed BEFORE installing the mod never spawns again, so the
        // health-based defeat rule can never fire for it. With this on, a boss marker
        // also counts as defeated when the `bossdoor_*` firepoint the game's own level
        // script names for it is in the save's `UnlockedFirepoints`
        // (mdb::boss_found_from_save). Derived on every publish and deliberately NOT
        // written to the found file - what the id means exactly ("cleared" vs "fought
        // and respawned here") is an open question, so the mark has to be undoable by
        // flipping this key.
        bool boss_defeat_from_save = true;
        // Chests keep their `Used` flag and pickups their `dying`, so those two are the
        // categories where absence really does mean "already taken". Shrines, doors and
        // fog gates are deliberately not in the default set.
        std::uint32_t markers_absence_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup);

        // The found tracker: wuchang_minimap_found.txt, one stable id per line.
        bool found_tracker = true;
        int found_save_debounce_ms = 2000;
        // WHICH found file. `auto` runs the save-slot ladder in src/saveslot.hpp and
        // writes wuchang_minimap_found_<key>.txt; `shared` pins the old global file;
        // anything else is used verbatim as the key. A fixed array, not std::string:
        // Config is copied by value onto the render thread every frame.
        char found_profile[32] = "auto";
        // The once-per-install toast that names the actual hotkeys. The sentinel file
        // next to the config is what makes it once.
        bool first_run_toast = true;
        // The shrine list panel on the full map (names, chapter, distance, travel).
        bool shrine_list = true;
        // EXTRA WIDGET CLASS-NAME PREFIXES THAT ARE NOT MENUS, on top of the built-in
        // table in scan_sched.hpp (`kNonMenuRoots`). "A menu is open" == "an in-viewport
        // widget's Visibility is Visible", and on 2026-09-03 a combat SUBTITLE
        // (`WB_ZiMu_C`) satisfied it and hid the minimap for 2.4 s. The built-in table
        // covers the furniture we know about; this is how the next one is silenced from
        // the config file rather than from a rebuild - every root the mod discovers for
        // the first time is logged by name, so the name to put here is in the log.
        // Comma-separated, matched case-insensitively as a PREFIX. A fixed array, not
        // std::string: Config is copied by value onto the render thread every frame.
        char menu_ignore_roots[192] = "";

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
        // While the highlight is ON, every marker of an enabled category within
        // `highlight_radius` is drawn at its projected screen position - glyph, name and
        // distance in metres - fading with distance. The overlay draws over the scene, so
        // "through walls" costs nothing extra; there is no occlusion test and no render
        // state anywhere near the game's.
        //
        // `highlight_mode` says how it is armed. HOLD is the original: on only while the
        // key is physically down, which needs no unlatching. TOGGLE is the default the
        // user asked for, and it obeys the same rule lessons.md sets for every latched
        // input state - it is cleared from LIVE state, not remembered: hl::drop_caches()
        // (a level transition, a dropped pawn) turns it off, and so does turning the
        // feature off. Nothing else may latch it on.

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
        // Loot plus the people and landmarks worth spotting through a wall. Everything
        // mdb knows about is selectable; this is only the shipped starting point.
        // `Note` is in here because a readable sign is exactly the kind of thing you
        // want the x-ray to point at while you are standing in front of it - the user
        // asked for it after round 4.
        std::uint32_t highlight_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup) |
            mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Boss) |
            mdb::cat_bit(mdb::Cat::Npc) | mdb::cat_bit(mdb::Cat::Note);
        // Draw collected loot too. Only chests, pickups and hidden items are ever
        // suppressed by this: a lit shrine, a beaten boss or an NPC you have met is
        // still a landmark, and is highlighted whatever its found state.
        bool highlight_show_found = false;
        int highlight_max_draw = 60;       // nearest first
        float highlight_alpha_near = 1.0f; // at the camera
        float highlight_alpha_far = 0.25f; // at highlight_radius
        float highlight_size = 7.0f;       // glyph radius, screen px
        bool highlight_labels = true;      // name + distance next to the glyph
        // How many of the drawn glyphs may carry a NAME, as opposed to how many glyphs
        // are drawn at all (highlight_max_draw, 60). The two are separate because sixty
        // names do not fit on a screen and never did: the labels are chosen nearest
        // first, never two for glyphs within highlight_size x 2 of each other, and laid
        // out top to bottom by src/label_layout.hpp so no two boxes overlap.
        int highlight_labels_max = 12;
        bool highlight_edge_arrows = true; // off-screen / behind: an arrow on the rim
        // ITEM QUALITY COLOURS. While the x-ray key is held, a marker whose static DB
        // entry carries a rarity tier above 0 is drawn - glyph, label and edge arrow -
        // in that tier's colour instead of its category colour, so "there is a weapon
        // in that room" reads at a glance. Tier 0 (every chest, every live-only actor,
        // every ordinary consumable) keeps the category colour it has always had, so
        // turning this on never repaints the things the palette has nothing to say
        // about. Defaults are the game's own pickup-beam palette
        // (mdb::kDefaultRarityColors).
        bool xray_rarity_colors_enabled = true;
        mdb::Rgb xray_rarity_colors[mdb::kRarityCount] = {
            mdb::kDefaultRarityColors[0], mdb::kDefaultRarityColors[1], mdb::kDefaultRarityColors[2]};
        // The same tint on the minimap / full-map / compass glyphs. Off by default:
        // those views are read as a category map, and recolouring a third of the pickups
        // there costs more legibility than it buys.
        bool markers_rarity_tint = false;
        // How often the game thread re-reads the camera while the key is held. The read
        // is a handful of raw doubles at a cached offset, so this is cheap; it only has
        // to keep up with how fast the player can swing the camera.
        int highlight_camera_hz = 60;
        // The camera reader's own rates and its discovery bounds (src/highlight.cpp).
        // `highlight_camera_resolve_ms` is how often the APlayerCameraManager is
        // re-found when it is missing; `highlight_compass_period_ms` is the slower rate
        // used when only the compass wants a heading; `highlight_getter_period_ms`
        // paces the ProcessEvent fallback route. `highlight_pov_scan_bytes` is how far
        // into CameraCachePrivate the POV block is looked for, and
        // `highlight_pov_bad_reads` how many insane reads in a row drop the pinned
        // offset and re-discover it.
        int highlight_camera_resolve_ms = 500;
        int highlight_compass_period_ms = 50;
        int highlight_getter_period_ms = 33;
        int highlight_pov_scan_bytes = 192;
        int highlight_pov_bad_reads = 8;

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
        // 0.34 rather than 0.42 (0.9.2): the strip sits where the game's own HUD lives,
        // and a narrower strip both fights it less and spreads the same span over fewer
        // pixels, which reads as MORE precise rather than less.
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
        // A pip on its own says "there is a chest that way"; the two keys below make it
        // say "there is a chest 42 m that way, one floor up". The label is the
        // horizontal distance in metres under (or over, on a bottom-anchored strip) the
        // glyph, and the arrow appears only once the height difference is worth walking
        // to - within compass_pip_height_uu the marker is treated as being on this
        // floor and no arrow is drawn.
        bool compass_pip_labels = true;
        float compass_pip_height_uu = 300.0f; // 3 m

        //==============================================================================
        // Minimap look
        //==============================================================================
        //
        // Everything the minimap draws that is not the map itself. These used to be
        // literals inside draw_minimap(); they are here so a legibility complaint can
        // be answered by editing a file instead of by a rebuild. All of them are live -
        // the next frame uses the new value.

        float minimap_backdrop = 0.86f;    // alpha of the dark disc under the map
        float minimap_backdrop_r = 6.0f;   // and its colour, 0..255
        float minimap_backdrop_g = 9.0f;
        float minimap_backdrop_b = 13.0f;
        float minimap_frame_r = 168.0f;    // the ring / border around the minimap
        float minimap_frame_g = 176.0f;
        float minimap_frame_b = 186.0f;
        float minimap_frame_alpha = 0.85f;
        // The Z-shaded composite fallback is a merged picture of every storey, so it is
        // drawn a little weaker than a height slice.
        float minimap_composite_alpha = 0.85f;
        float minimap_min_px = 72.0f;      // floor on the side length, whatever the fraction says
        float minimap_arrow_frac = 0.055f; // player arrow, as a fraction of the side
        float minimap_arrow_min_px = 8.0f;
        // The waypoint glyph's radius, as a multiple of markers_size. It is deliberately
        // a touch bigger than a marker: it is the one thing that is never culled.
        float waypoint_size_scale = 1.05f;

        //==============================================================================
        // The game-state reader (src/gamestate.cpp, game thread)
        //==============================================================================
        //
        // The rates of the ProcessEvent pump. They are read from the config at
        // most twice a second and are live. LOWER PERIODS COST GAME-THREAD TIME: the
        // position pump is what gates the marker scan and the widget test, and the
        // widget sweep is a FindAllOf, i.e. a whole object-array walk.

        int reader_position_period_ms = 100;      // 10 Hz: pawn location + yaw
        int reader_resolve_period_ms = 500;       // 2 Hz: FindAllOf for the pawn / controller
        int reader_widget_sweep_period_ms = 250;  // the full menu-widget sweep, fast cadence
        // The sweep is a whole-object-array walk (28-51 ms). It only ever has to
        // DISCOVER a menu root that has never been seen: every root already on the
        // watchlist is re-tested on every 10 Hz pump. So the period doubles from
        // reader_widget_sweep_period_ms up to this maximum while nothing new is found,
        // and drops back to the fast cadence for reader_widget_sweep_warm_ms after any
        // menu-state flip, teleport or world change.
        int reader_widget_sweep_max_period_ms = 2000;
        int reader_widget_sweep_warm_ms = 2000;
        int reader_transition_cooldown_ms = 2000; // no UFunction call for this long after a pawn/world change
        double reader_teleport_jump_uu = 3000.0;  // a position jump this big in one pump is a fast travel
        int reader_chapter_period_ms = 1000;      // how often the streamed level set is named
        int reader_log_throttle_ms = 5000;        // "no pawn" / "rejected class" lines

        //==============================================================================
        // Marker sweep internals
        //==============================================================================
        //
        // The absence grace of the live half. `markers_live_grace_rounds` is the number
        // of rounds a live actor may go unseen before it is dropped from the live cache - two rounds is what stops a marker flickering whenever a sweep
        // races level streaming. It is NOT a collected test (see markers_absence_*).

        int markers_live_grace_rounds = 2;

        //==============================================================================
        // Assets and diagnostics
        //==============================================================================

        // How long a retired chapter's height planes stay alive after the pointer to
        // them was cleared, so a render thread already inside a slice cannot fault. A
        // slice is ~4 ms; this is three orders of magnitude more.
        int map_asset_retire_grace_ms = 2000;
        // Rate limit on the "hidden because: ..." log line, so a flapping condition
        // cannot flood the log.
        int hide_reason_log_ms = 2000;
        // Size of our SRV descriptor heap (font atlas + map + the four slice buffers).
        // RESTART ONLY: the heap is created once, when the overlay first initialises.
        int srv_heap_size = 64;

        bool debug_readout = false;
        bool debug_show_panel_on_start = false; // main-menu verification aid
        int panel_key = 0x71;                   // VK_F2
        int reload_key = 0x74;                  // VK_F5
        int map_key = 0x4D;                     // 'M' - full map
        int map_recenter_key = 0x52;            // 'R' - recentre the full map on the player
        // Cycles minimap_zoom_presets. 'N' is not bound by the game, by this machine's
        // injected DLLs (RenoDX F6, Steam F12) or by the rest of the mod (F2 / M / R /
        // F5 / LALT), and it is next to M - the other map key.
        int zoom_key = 0x4E;                    // 'N' - cycle the minimap zoom
        // Copies the full map to the clipboard. Only live WHILE THE FULL MAP IS OPEN,
        // and the map mode swallows the whole keyboard (lessons.md), so a plain letter
        // cannot reach the game - which is why this can be 'C' for copy instead of yet
        // another F-key in a minefield of them.
        int screenshot_key = 0x43;              // 'C' - full map -> clipboard

        //------------------------------------------------------------------------------
        // Extras (0.9.4)
        //------------------------------------------------------------------------------
        // wuchang_minimap_last_stage.txt, rewritten with CreateFileW/WriteFile at every
        // overlay stage transition. The UE4SS log buffer can be lost when the process
        // dies; a file closed after each write cannot be.
        bool crash_breadcrumb = true;
        // How much the mod writes to UE4SS.log and wuchang_minimap.log. `normal` is
        // what a bug report needs; `verbose` adds the running commentary; `trace` adds
        // the per-publish censuses. See the LOGGING block further down this file.
        LogLv log_level = LogLv::Normal;
        // Shrine fast travel. OFF until the in-game reflection self-check in
        // context/extras-test-instructions.md has confirmed the call route.
        bool fast_travel_enabled = false;
        // Route 1 of the save-slot ladder actually CALLS `Get Save Slot Value`. Off
        // until the recon dump shows the parameter is not engine-owned - see
        // saveslot.cpp for the hazard.
        bool saveslot_uuid_call = false;
        // The one-press in-game recon dump (research §3). 0 = unbound.
    };

    // The config lives here and is copied under a spinlock. The loop thread writes it
    // on load / F5; the render thread writes it when the F2 panel is used.
    Config config();
    void set_config(const Config& cfg);

    //==================================================================================
    // The generation-cached config (the ONLY form allowed on a hot path)
    //==================================================================================
    //
    // `config()` takes a spinlock and copies ~1 KB of struct. The game thread used to do
    // that 1 000 times a second, the loop thread once per UE4SS iteration and the render
    // thread once per frame, all for a struct that changes when the user presses Save,
    // F5, or once a second at most (the config-file mtime watcher).
    //
    // `cfg_cached()` instead keeps a per-THREAD copy and refreshes it only when
    // `set_config` has bumped `g_cfg_gen`. Steady state is one relaxed atomic load and a
    // reference return; a real change costs exactly one spinlocked copy per thread that
    // asks. Every writer goes through `set_config`, so F5 and the 1 Hz mtime reload keep
    // working unchanged.
    //
    // The reference is valid until the next `cfg_cached()` call ON THE SAME THREAD, and
    // must never be handed to another thread. Cold paths (file I/O, panel Save, one-off
    // setup) may keep using `config()`.
    extern std::atomic<std::uint32_t> g_cfg_gen;

    const Config& cfg_cached();

    //==================================================================================
    // Per-activity performance counters (perf.hpp)
    //==================================================================================
    //
    // Every periodic activity in this mod registers one counter and records how long
    // each invocation took; the F2 debug block prints the table. The instance lives
    // here because mmstate is the one module every other one already links against.
    //
    // Recording is lock-free and allocation-free (see perf.hpp) - it is called from the
    // game thread inside ProcessEvent, so it has to be.

    // Microseconds from QueryPerformanceCounter, with the frequency read exactly once.
    std::uint64_t qpc_us();

    // `name` must have static storage. Registering the same pointer twice returns the
    // same id, so a `static const int` at a call site is the intended idiom.
    int perf_register(const char* name, perf::Thread thread);

    // Records one invocation that started at `t0_us` (from qpc_us()).
    void perf_record(int id, std::uint64_t t0_us);

    // Records one invocation whose duration was measured by the caller (the two
    // slicers already time themselves for the F2 slice line).
    void perf_record_ms(int id, double ms);

    // The table, for the F2 panel. Read-only by convention.
    const perf::Table& perf_table();
    void perf_reset_peaks();

    // THE STALL GATE. Any thread. `perf_note_stall` says "the process is not running
    // normally for the next `ms` milliseconds" - a loading screen, a swapchain resize, a
    // reload, a clipboard copy - and every perf_record taken inside that window is
    // counted as a stall instead of setting the peak the F2 table shows. It exists
    // because all three of this mod's threads measure WALL CLOCK: while the game thread
    // is inside a synchronous load, a cross-thread user32 call (which is most of what
    // ImGui_ImplWin32_NewFrame and GetForegroundWindow do) blocks for as long as the
    // load takes, and the resulting 350 ms "peak" is not this mod's cost at all.
    //
    // It deliberately does NOT infer a stall from the sample's own duration - "this was
    // slow so it must have been a stall" is circular. Only externally attributable
    // events call it.
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
    // `config()` copies the whole struct under a spinlock, which is far too much for the
    // very first statement of a callback the engine fires thousands of times a second.
    // `mod_active()` is one relaxed atomic load and allocates nothing, so a disabled mod
    // costs exactly that per ProcessEvent and per Present.
    //
    // It is NOT simply `config().mod_enabled`: the switch is turned off before the
    // subsystems have finished standing down, so `modswitch` owns the flag and drives
    // it, and everything else only reads it.
    extern std::atomic<bool> g_mod_active;

    inline bool mod_active()
    {
        return g_mod_active.load(std::memory_order_relaxed);
    }

    // Loop thread. Reads ONLY `mod_enabled` out of the config file, without touching
    // the live config - the 1 Hz watcher uses it to answer "did the master switch flip?"
    // while everything else in the file stays untouched until the mod is running again.
    // False when the file cannot be read or carries no `mod_enabled` line.
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
    std::wstring mod_dir();

    // Every setting as `key` -> its text form, in cfgkeys order (Player, Advanced,
    // Dev). The single source of the VALUES; the layout of the files is owned by
    // config_rewrite.hpp. Any thread (pure).
    std::vector<std::pair<std::string, std::string>> config_kv(const Config& cfg);

    // "F2", "M", "TAB", "LALT", ... - the same spelling the config file uses. Any thread.
    std::wstring key_name(int vk);

    // "LB+RB", "A", "LT+RT", "none" - the gamepad chord spelling the config file uses.
    // Any thread.
    std::wstring pad_chord_name(std::uint16_t mask, bool lt, bool rt);

    // Can this virtual key be written into the config file and read back? The F2
    // Bindings tab's "press a key" capture only accepts keys this returns true for, so
    // a captured binding always round-trips through key_name(). Any thread (pure).
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
    // THE QUEUE IS BOUNDED AND NEVER BLOCKS. A thread that is not the loop thread takes
    // a spinlock, pushes one string and leaves; past `kLogQueueMax` entries the line is
    // DROPPED and counted, and the loop thread prints "log: dropped N line(s)" the next
    // time it drains. A log call can therefore never grow without bound and can never
    // wait on the disk (the file buffer is behind a lock of its own - see mmstate.cpp).
    //
    // THREE LEVELS, ONE KEY (`log_level`, Advanced tier).
    //
    //   normal   (default) - what a bug report needs and nothing else: the startup
    //                        header, config / map / marker / shrine loads, chapter,
    //                        level and save-slot changes, hook installation, every
    //                        error and warning, every FIRST-OCCURRENCE diagnostic (the
    //                        visibility / item-name / health routes, the not-a-menu
    //                        list), the watchdog, the breadcrumb, persisted-state
    //                        changes, and a health summary at most once a minute.
    //   verbose            - the periodic running commentary: the `state:` line at its
    //                        real cadence, the per-round marker timings, menu-state
    //                        transitions, minimap hide/show reasons, x-ray toggles,
    //                        panel and full-map open/close.
    //   trace              - everything, including per-publish censuses.
    //
    // A SUPPRESSED LINE COSTS ONE RELAXED ATOMIC LOAD. Use the macros: they wrap the
    // whole call, so neither `std::format` nor the argument expressions run when the
    // level is off. `mm::log` / `mm::logf` with no macro around them are the NORMAL
    // level and always emit - which keeps the default level's call sites unadorned.
    //
    //   MM_LOGV(fmt, ...)  / MM_LOGVS(str)   - verbose
    //   MM_LOGT(fmt, ...)  / MM_LOGTS(str)   - trace

    // Set from the config on every load; read by every log macro. Relaxed because a
    // line logged one microsecond either side of a level change is not a correctness
    // question.
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

    // The mod's OWN rolling log, `wuchang_minimap.log` in the mod folder, rotated per
    // launch keeping `.1` / `.2` / `.3`. Every line that reaches UE4SS's log goes here
    // too - UE4SS truncates its own log on every launch, so without this the evidence
    // from an in-game session is gone as soon as the game is started again. Writes are
    // buffered; `modlog_flush()` is safe from any thread (the crash breadcrumb calls it,
    // so the log and the breadcrumb always agree about the last thing that happened) and
    // `modlog_tick()` flushes it every few seconds from the loop thread.
    //
    // The file is CAPPED at `kModLogMaxBytes` per session: at the cap it writes one
    // "log capped" line and stops writing, so a runaway diagnostic cannot fill a disk.
    // The `.1` / `.2` / `.3` rotation is unaffected.
    void modlog_flush();
    void modlog_tick(std::uint64_t now_ms);
    std::wstring modlog_path();

    template <typename... Args>
    void logf(std::wformat_string<Args...> fmt, Args&&... args)
    {
        log(std::format(fmt, std::forward<Args>(args)...));
    }

    // Level-checked forms, for the rare site where a macro will not do (inside a
    // lambda argument list, say). The check is inside, so the format never runs - but
    // the ARGUMENTS still do, which is why the macros are the default.
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
