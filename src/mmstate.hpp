#pragma once

// mmstate - state shared across the mod's three threads: the UE4SS event-loop thread
// (on_update: hotkeys, config file I/O, maps.json + PNG decoding, log draining), the game
// thread (ProcessEvent pre-callback: reads the pawn, the view target and the widgets, and
// publishes a Snapshot) and the render thread (the hooked IDXGISwapChain::Present).
//
// Invariants:
//   * no std::mutex anywhere (it faults against the process's MSVCP140 from the game
//     thread) - a Snapshot goes through a seqlock, everything else is a plain atomic;
//   * no iostreams / locale off the loop thread - file I/O is CreateFileW + ReadFile;
//   * the render thread never touches a UObject, the game thread never touches D3D12.

#include <atomic>
#include <cstddef>
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
    // Published by the game thread. Trivially copyable: the seqlock memcpy's it.
    struct Snapshot
    {
        double x = 0.0; // world location, uu
        double y = 0.0;
        double z = 0.0;
        float yaw = 0.0f; // degrees, UE convention (0 = +X = the image's "north")

        // Defaults mean "hidden": an unfilled snapshot must never show the minimap.
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
        std::uint32_t menu_watch_count = 0; // all re-tested on every pump)
        std::uint32_t widget_sweep_period_ms = 0;
        std::uint64_t menu_change_ms = 0; // GetTickCount64() when menu_open last changed
        std::uint64_t stamp_ms = 0; // GetTickCount64() at publish
        // GetTickCount64() since which a validated gameplay pawn has been held continuously;
        // 0 = it is not. The overlay requires a minimum age here.
        std::uint64_t state_ok_since_ms = 0;
        // GetTickCount64() of the last teleport (a position jump inside one pump). The render
        // side drops its position-derived caches on it.
        std::uint64_t teleport_ms = 0;

        wchar_t pawn_name[96]{}; // pawn class / object name
        wchar_t level_name[160]{}; // pawn full name (carries the world + level path)
        // The in-viewport Visible root widget holding menu_open true, or empty.
        wchar_t menu_holder[64]{};
    };

    // Writer (game thread) and reader (render thread). A torn read is retried, never
    // blocked - the render thread must never wait on the game thread.
    void publish(const Snapshot& snap);
    bool read_snapshot(Snapshot& out);

    //=== Configuration - config_wuchang_minimap.txt in the mod folder ==============

    enum class Anchor : int
    {
        TopLeft = 0,
        TopRight = 1,
        BottomLeft = 2,
        BottomRight = 3,
    };

    // HUD placement. `Custom` obeys minimap_anchor / minimap_offset_* / compass_anchor; any
    // other value puts the minimap in that corner and the compass on the same vertical side.
    enum class HudPreset : int
    {
        Custom = 0,
        TopLeft = 1,
        TopRight = 2,
        BottomLeft = 3,
        BottomRight = 4,
    };

    // Toggle: one press on, the next off. Hold: on only while the key / pad chord is down.
    enum class HighlightMode : int
    {
        Toggle = 0,
        Hold = 1,
    };

    // Which edge the compass strip hangs off; `compass_offset_y` measures from that edge.
    enum class VAnchor : int
    {
        Top = 0,
        Bottom = 1,
    };

    enum class LogLv : int
    {
        Normal = 0,
        Verbose = 1,
        Trace = 2,
    };

    struct Config
    {
        //=== The master switch =====================================================
        // `mod_enabled = 0` stops all measurable work: no DX12 hooks, the ProcessEvent callback
        // returns on its first statement, no object-array scan, height maps freed, no XInput. A
        // 1 Hz stat() of the config file on the loop thread is what lets `mod_enabled = 1` turn
        // it back on without restarting the game.
        bool mod_enabled = true;

        // The ladder, in order of how much they stop: mod_enabled (the whole mod) >
        // overlay_enabled (anything drawn) > show_minimap (just the minimap disc).
        bool overlay_enabled = true;
        bool show_minimap = true;

        //=== UI scale, HUD placement, theme and palette ============================
        // `ui_scale = auto` derives the factor from the back buffer's height
        // (clamp(h / 1080, 1, 4)). Every PIXEL key below is multiplied by it at read time;
        // minimap_size and compass_width are fractions and are not.
        // `theme` is the chrome, `palette` the marker hue set - independent axes; a colour key
        // written in the config file wins over the theme.
        gly::Theme theme = gly::Theme::Neutral;
        gly::Palette palette = gly::Palette::Default;

        bool ui_scale_auto = true;
        float ui_scale = 1.0f; // only consulted when ui_scale_auto is false
        HudPreset hud_preset = HudPreset::Custom;
        float size_frac = 0.24f;      // minimap side as a fraction of screen height
        float zoom_uu_per_px = 26.0f; // world uu per minimap pixel (smaller = closer)
        // The zoom ladder `zoom_key` steps through and wraps (mapview: parse/next_zoom_preset).
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
        // A menu closing is not a level transition: re-showing waits only this long, not
        int menu_close_show_delay_ms = 150; // min_visible_after_state_ok_ms. Hiding is immediate.

        //=== Height slicing (what the minimap actually draws) ======================
        // Four 16-bit PNGs hold the Z of up to four stacked walkable surfaces per pixel. Every
        // ~80 ms the overlay slices the window around the player on the CPU:
        //     |surfaceZ - feetZ| <= floor_z_tolerance -> opaque, shaded by the gradient
        //     nearest below within floor_fade_uu      -> adjacent_floor_opacity
        //     nearest above within floor_fade_uu      -> adjacent_floor_opacity x 0.6
        //     nothing                                 -> transparent
        // The only smoothing is feet_z_smooth_ms.

        bool show_adjacent_floors = true;     // draw the surfaces below / above, dimmed
        float adjacent_floor_opacity = 0.25f; // below; above uses 0.6 x this
        float floor_z_tolerance = 200.0f;     // uu: |Z - feetZ| within this = my floor
        float floor_fade_uu = 800.0f;         // uu: how far below / above is still shown
        // lum = 1 + strength * clamp((surfaceZ - feetZ) / span, -1, 1), span = tolerance (current
        // floor) or fade (dimmed). The same formula runs offline in tools/navmesh/slice_preview.py.
        float floor_gradient_strength = 0.18f;
        float floor_base_r = 214.0f; // walkable fill colour, 0..255
        float floor_base_g = 208.0f;
        float floor_base_b = 196.0f;
        int slice_hz = 12;           // CPU re-slices per second (2..30)
        int feet_z_smooth_ms = 100;  // EMA time constant on feet Z
        // The pawn's location is its capsule centre, ~90 uu above the navmesh under it;
        // subtracted to get feet Z.
        float player_z_offset = 90.0f;
        // Load the Z-shaded composite PNG too (+82 MB VRAM for Chapter 1): it merges every storey
        // and is the fallback for a chapter whose height maps are missing.
        bool fallback_use_composite = false;

        //=== Markers ===============================================================
        // Static database: markers/<chapter>.json (schema `wuchang-minimap-markers/1`). Live half:
        // a class sweep on the game thread reading each marker actor's own state flag.

        bool markers_enabled = true;
        bool markers_live = true;     // run the game-thread class sweep
        // Draw only the chapter the player is in. The static DB is one flat set of all six
        // chapters whose world bounds overlap heavily, so off paints other chapters on top.
        bool markers_filter_chapter = true;
        // The live sweep walks GUObjectArray in slices, one slice per game-thread pump
        // (src/scan_sched.hpp); the chunk and period set how much game-thread time one pump costs.
        int markers_rounds_per_sec = 1;   // full passes over the object array per second
        int markers_scan_chunk = scan::kChunkDefault;        // object slots per pump
        int markers_scan_period_ms = scan::kPeriodDefaultMs; // min ms between pumps
        std::uint32_t markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
        bool markers_hide_found = true;  // hide instead of dimming a found marker
        float markers_found_alpha = 0.3f;
        float markers_size = 6.5f;         // glyph radius in minimap pixels
        bool markers_clamp_to_edge = false; // keep out-of-range markers on the rim
        int markers_max_draw = 400;         // hard cap per frame, nearest first

        //=== Absence as evidence of a collect ======================================
        // An item collected before the mod was installed has a DB entry and no live actor at all.
        // With this on, a marker whose owning level is loaded and that no full object-array round
        // has seen since that level streamed in is marked collected after this many rounds.
        bool markers_absence_marks = true;
        int markers_absence_rounds = 2;
        // A boss killed before the mod was installed never spawns again. With this on, a boss
        // marker also counts as defeated when the `bossdoor_*` firepoint the level script names
        // for it is in the save's `UnlockedFirepoints`. Derived on every publish.
        bool boss_defeat_from_save = true;
        // Chests keep a `Used` flag and pickups a `dying` flag, so absence means "taken" only for
        // those two; shrines, doors and fog gates are not in the default set.
        std::uint32_t markers_absence_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup);

        // The found tracker: wuchang_minimap_found.txt, one stable id per line.
        bool found_tracker = true;
        int found_save_debounce_ms = 2000;
        // `auto` runs the save-slot ladder in src/saveslot.hpp and writes
        // wuchang_minimap_found_<key>.txt; `shared` pins the global file; anything else is the key.
        char found_profile[32] = "auto";
        bool first_run_toast = true;
        bool shrine_list = true;
        // Extra widget class-name prefixes that are NOT menus, on top of `kNonMenuRoots` in
        // scan_sched.hpp - "a menu is open" means "an in-viewport widget Visibility is Visible",
        // which a combat subtitle also satisfies. Comma-separated, matched as a prefix.
        char menu_ignore_roots[192] = "";

        //=== The full map ==========================================================
        // The height-sliced asset at map scale, with every marker on it. While it is open the
        // minimap is hidden and ImGui takes the mouse and keyboard, never latched.
        // Zoom is world units per SCREEN pixel: 30 uu/px shows ~270 m across a 900 px canvas.

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
        // (width x height x surfaces) plane reads - 5-10 ms - so it runs only on a change.
        int map_slice_px = 768;
        int map_slice_hz = 6;        // hard cap on re-cuts per second
        bool map_gamepad = true;     // poll XInput while the map is open
        float map_gamepad_deadzone = 0.22f;
        bool map_waypoint_persist = true; // write wuchang_minimap_waypoint.txt

        //=== X-ray highlight =======================================================
        // While on, every marker of an enabled category within `highlight_radius` is drawn at its
        // projected screen position, fading with distance. No occlusion test.
        // Toggle state lives in live state only: hl::drop_caches() (a level transition, a dropped
        // pawn) turns it off, and so does turning the feature off. Nothing else may latch it on.

        bool highlight_enabled = true;
        HighlightMode highlight_mode = HighlightMode::Toggle;
        int highlight_key = 0x09; // VK_TAB
        bool highlight_gamepad = true;
        // XInput chord (pad::kLeftShoulder | pad::kRightShoulder). The triggers are analogue, so
        // they are their own flags rather than mask bits.
        std::uint16_t highlight_pad_mask = 0x0300;
        bool highlight_pad_lt = false;
        bool highlight_pad_rt = false;
        float highlight_radius = 3000.0f; // uu (30 m)
        // Every category mdb knows is selectable; this is the shipped starting point.
        std::uint32_t highlight_categories =
            mdb::cat_bit(mdb::Cat::Chest) | mdb::cat_bit(mdb::Cat::Pickup) |
            mdb::cat_bit(mdb::Cat::Shrine) | mdb::cat_bit(mdb::Cat::Boss) |
            mdb::cat_bit(mdb::Cat::Npc) | mdb::cat_bit(mdb::Cat::Note);
        // Draw collected loot too. Only chests, pickups and hidden items are suppressed by this;
        // shrines, bosses and NPCs are landmarks and show whatever their found state.
        bool highlight_show_found = false;
        int highlight_max_draw = 60;       // nearest first
        float highlight_alpha_near = 1.0f; // at the camera
        float highlight_alpha_far = 0.25f; // at highlight_radius
        float highlight_size = 7.0f;       // glyph radius, screen px
        bool highlight_labels = true;      // name + distance next to the glyph
        // How many drawn glyphs may carry a name. Chosen nearest first, never two for glyphs within
        // highlight_size x 2, laid out by src/label_layout.hpp so no two boxes overlap.
        int highlight_labels_max = 12;
        bool highlight_edge_arrows = true; // off-screen / behind: an arrow on the rim
        // Under the x-ray, a marker whose DB entry carries a rarity tier above 0 is drawn in that
        // tier's colour instead of its category colour. Tier 0 keeps the category colour.
        bool xray_rarity_colors_enabled = true;
        mdb::Rgb xray_rarity_colors[mdb::kRarityCount] = {
            mdb::kDefaultRarityColors[0], mdb::kDefaultRarityColors[1], mdb::kDefaultRarityColors[2]};
        bool markers_rarity_tint = false;
        int highlight_camera_hz = 60;
        // Camera reader bounds (src/highlight.cpp): `resolve_ms` how often APlayerCameraManager is
        // re-found, `compass_period_ms` the rate when only the compass wants a heading,
        // `pov_scan_bytes` how far into CameraCachePrivate the POV block is sought.
        int highlight_camera_resolve_ms = 500;
        int highlight_compass_period_ms = 50;
        int highlight_getter_period_ms = 33;
        int highlight_pov_scan_bytes = 192;
        int highlight_pov_bad_reads = 8;

        //=== The compass strip =====================================================
        // N/E/S/W plus 15-degree ticks from the camera yaw (the pawn's yaw when no camera pose is
        // fresh). Hidden by the same evaluation as the minimap - no second latch.

        bool compass_enabled = true;
        float compass_width = 0.34f;    // fraction of the screen width
        // Filled plate behind the strip. Off = ticks and letters only, each with a 1 px shadow.
        bool compass_plate = true;
        // A non-custom `hud_preset` overrides this anchor (top presets -> Top, bottom -> Bottom).
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
        // Minor-tick spacing in degrees (a multiple of 45 is a labelled major tick, of 90 a
        // cardinal letter), and the cap on marker pips, nearest first.
        float compass_tick_step_deg = 15.0f;
        int compass_max_pips = 32;
        // Pip label: horizontal distance in metres plus an up/down arrow. Within
        // compass_pip_height_uu the marker counts as being on this floor and no arrow is drawn.
        bool compass_pip_labels = true;
        float compass_pip_height_uu = 300.0f; // 3 m

        //=== Minimap look ==========================================================

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
        float minimap_min_px = 72.0f;      // floor on the side length, px
        float minimap_arrow_frac = 0.055f; // player arrow, as a fraction of the side
        float minimap_arrow_min_px = 8.0f;
        // Waypoint glyph radius, as a multiple of markers_size. The waypoint is never culled.
        float waypoint_size_scale = 1.05f;

        //=== The game-state reader (src/gamestate.cpp, game thread) ================
        // ProcessEvent pump rates, live. Lower periods cost game-thread time: the position pump
        // gates the marker scan and the widget test, and the widget sweep is a FindAllOf.

        int reader_position_period_ms = 100;      // 10 Hz: pawn location + yaw
        int reader_resolve_period_ms = 500;       // 2 Hz: FindAllOf for the pawn / controller
        int reader_widget_sweep_period_ms = 250;  // the full menu-widget sweep, fast cadence
        // The sweep is a whole-object-array walk (28-51 ms) and only has to DISCOVER roots never
        // seen before; known roots are re-tested on every 10 Hz pump. The period doubles up to this
        // maximum while nothing new is found, then drops back after a menu flip, teleport or world change.
        int reader_widget_sweep_max_period_ms = 2000;
        int reader_widget_sweep_warm_ms = 2000;
        int reader_transition_cooldown_ms = 2000; // no UFunction call for this long after a pawn/world change
        double reader_teleport_jump_uu = 3000.0;  // a position jump this big in one pump is a fast travel
        int reader_chapter_period_ms = 1000;      // how often the streamed level set is named
        int reader_log_throttle_ms = 5000;        // "no pawn" / "rejected class" lines

        // Rounds a live actor may go unseen before it is dropped from the live cache; two stops a
        // marker flickering when a sweep races level streaming. NOT a collected test.
        int markers_live_grace_rounds = 2;

        //=== Assets and diagnostics ================================================

        // How long a retired chapter's height planes stay alive after the pointer to them is
        // cleared, so a render thread already inside a slice (~4 ms) cannot fault.
        int map_asset_retire_grace_ms = 2000;
        int hide_reason_log_ms = 2000;
        // Size of the SRV descriptor heap (font atlas + map + the four slice buffers). RESTART
        // ONLY: the heap is created once, when the overlay first initialises.
        int srv_heap_size = 64;

        bool debug_readout = false;
        bool debug_show_panel_on_start = false; // main-menu verification aid
        int panel_key = 0x71;                   // VK_F2
        int reload_key = 0x74;                  // VK_F5
        int map_key = 0x4D;                     // 'M' - full map
        int map_recenter_key = 0x52;            // 'R' - recentre the full map on the player
        int zoom_key = 0x4E;                    // 'N' - cycle the minimap zoom
        int screenshot_key = 0x43;              // 'C' - full map -> clipboard
        int waypoint_nearest_key = 0x47;        // 'G' - waypoint the nearest unfound marker

        // wuchang_minimap_last_stage.txt, rewritten at every overlay stage transition. The UE4SS
        // log buffer can be lost when the process dies; a file closed after each write cannot be.
        bool crash_breadcrumb = true;
        // `normal` is what a bug report needs; `verbose` adds the running commentary; `trace` adds
        // the per-publish censuses. See the LOGGING block below.
        LogLv log_level = LogLv::Normal;
        // Route 1 of the save-slot ladder CALLS `Get Save Slot Value`. Off until a recon dump shows
        // the parameter is not engine-owned - see saveslot.cpp for the hazard.
        bool saveslot_uuid_call = false;

        // New fields go at the END of the struct so no existing offset moves, and each stays a POD
        // (see the static_assert below).

        // Opens the full map from a controller: an XInput button mask spelled the way
        // `highlight_pad_chord` is ("BACK+Y"), pressed as a chord. `none` disables it.
        std::uint16_t map_pad_open_chord = 0x8020; // pad::kY | pad::kBack

        // The UI font, rasterised at 13 * ui_scale. A missing or unreadable file falls back to the
        // built-in ProggyClean and says so in the log. A fixed array, not std::string.
        char ui_font[192] = "C:\\Windows\\Fonts\\segoeui.ttf";

        // On: `minimap_zoom` and `map_zoom` are scaled by ui_scale like every other pixel key, so
        // one config shows the same area of world at 1080p and 2160p. Off: uu/px is literal.
        bool zoom_dpi_scaled = true;
    };

    // Config is a value: copied by value onto the render thread every frame, published under a
    // spinlock, compared field by field below. A std::string member would break all three.
    static_assert(std::is_trivially_copyable_v<Config>, "Config is copied by value per frame");

    //=== Config equality, field by field ===========================================
    // Generated from the struct declaration, so it lists every field exactly once. The F2 panel
    // and the full map use it to decide whether to publish a config.
    // The drift guard in tests/markers_test.cpp flips every byte of a Config in turn and requires
    // this to notice, which is also why this is header-only: the test does not link mmstate.cpp.
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
        a.waypoint_nearest_key == b.waypoint_nearest_key &&
        a.crash_breadcrumb == b.crash_breadcrumb &&
        a.log_level == b.log_level &&
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

    // The marker filters, the one definition of the set: what the full map's legend and the F2
    // chips edit, and the only keys the automatic filter save writes. All four are Tier::Player.
    inline bool filters_differ(const Config& a, const Config& b)
    {
        return a.markers_categories != b.markers_categories ||
               a.markers_hide_found != b.markers_hide_found ||
               a.highlight_categories != b.highlight_categories ||
               a.compass_categories != b.compass_categories;
    }

    // The keys `filters_differ` compares, for save_config_keys().
    inline const char* const kFilterKeys[] = {"markers_categories", "markers_hide_found",
                                              "highlight_categories", "compass_categories"};

    // The config lives here and is copied under a spinlock. The loop thread writes it on load /
    // F5; the render thread writes it when the F2 panel is used.
    Config config();
    void set_config(const Config& cfg);

    //=== The generation-cached config (the ONLY form allowed on a hot path) ========
    // `config()` takes a spinlock and copies ~1 KB of struct. `cfg_cached()` keeps a per-THREAD
    // copy, refreshed only when `set_config` has bumped `g_cfg_gen`. The reference is valid
    // until the next call ON THE SAME THREAD and must never be handed to another thread.
    extern std::atomic<std::uint32_t> g_cfg_gen;

    const Config& cfg_cached();

    //=== Per-activity performance counters (perf.hpp) ==============================
    // Recording is lock-free and allocation-free - it runs inside ProcessEvent.

    // Microseconds from QueryPerformanceCounter, with the frequency read exactly once.
    std::uint64_t qpc_us();

    // `name` must have static storage; registering the same pointer twice returns the same id.
    int perf_register(const char* name, perf::Thread thread);

    void perf_record(int id, std::uint64_t t0_us);

    void perf_record_ms(int id, double ms);

    // The table, for the F2 panel. Read-only by convention.
    const perf::Table& perf_table();
    void perf_reset_peaks();

    // The stall gate, any thread: "the process is not running normally for the next `ms` ms" -
    // a loading screen, a swapchain resize, a reload. Every perf_record inside that window
    // counts as a stall instead of setting a peak; a stall is never inferred from a sample.
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

    //=== The master switch, as a lock-free flag ====================================
    // One relaxed atomic load, so a disabled mod costs exactly that per ProcessEvent and per
    // Present. NOT `config().mod_enabled`: the switch goes off before the subsystems have
    // finished standing down, so `modswitch` owns the flag and everything else only reads it.
    extern std::atomic<bool> g_mod_active;

    inline bool mod_active()
    {
        return g_mod_active.load(std::memory_order_relaxed);
    }

    // Loop thread. Reads ONLY `mod_enabled` out of the config file, leaving the live config
    // untouched. False when the file cannot be read or carries no such line.
    bool peek_mod_enabled(bool& out);

    // Loop thread. The two config files' FILETIMEs mixed into one uint64, or 0 when neither can
    // be stat()ed. The watcher only parses when this changes, so editing EITHER reloads both.
    std::uint64_t config_mtime();

    // Loop thread only (plain Win32 file I/O, no iostreams).
    void load_config_file();
    void save_config_file();
    // A partial save: only the named keys get their live value written into
    // config_wuchang_minimap.txt. Every other key, including a panel edit not yet saved, keeps
    // whatever is on disk. `keys` must be Player or Advanced keys - a Dev key is dropped, since
    // this only ever writes the player file. With no file to rewrite it falls back to
    // save_config_file(), which owns the pristine text a fresh file is made of.
    void save_config_keys(const char* const* keys, std::size_t count);
    std::wstring config_path();
    // config_wuchang_minimap_dev.txt - the Tier::Dev overlay, parsed after the main file.
    std::wstring dev_config_path();
    // "a Save will write the dev file too", the condition save_config_file() uses. Any thread.
    bool dev_config_active();
    std::wstring mod_dir();

    // Every setting as `key` -> its text form, in cfgkeys order. The single source of the
    // VALUES; the layout of the files is owned by config_rewrite.hpp. Any thread (pure).
    std::vector<std::pair<std::string, std::string>> config_kv(const Config& cfg);

    //=== A BINDING IS A VIRTUAL KEY PLUS AT MOST ONE MODIFIER ======================
    // Every `*_key` config value is an int carrying both halves: the virtual key in bits 0..7
    // (every VK is <= 0xFF) and the modifier in bits 8..9. Keeps Config trivially copyable.
    // One modifier, not a set; key_name() writes back the config file's spelling, so a binding
    // round-trips. A binding with NO modifier does not require the modifiers to be up.
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

    // "F2", "M", "CTRL+M", "TAB", "LALT" - the config file spelling, modifier included.
    std::wstring key_name(int binding);

    // "LB+RB", "A", "LT+RT", "none" - the gamepad chord spelling the config file uses.
    std::wstring pad_chord_name(std::uint16_t mask, bool lt, bool rt);

    // Can this virtual key be written into the config file and read back? Any thread (pure).
    bool vk_bindable(int vk);

    const std::vector<int>& bindable_vks();

    // Parse a gamepad chord ("LB+RB", "A", "none") into the three fields the config carries.
    // Returns true when any of them changed. Any thread.
    bool set_pad_chord(const std::string& text, std::uint16_t& mask, bool& lt, bool& rt);

    //=== The waypoints =============================================================
    // Up to mv::kMaxWaypoints of them, set on the full map and drawn on the full map, the
    // minimap and the compass. They live in their own file so one set during play survives
    // without a Save. The render thread sets them; the loop thread writes the file, via
    // the spinlocked-copy pattern.

    mv::WaypointSet waypoints();
    void set_waypoints(const mv::WaypointSet& set); // any thread; marks the file dirty
    // False when the set is already mv::kMaxWaypoints long.
    bool add_waypoint(const mv::Waypoint& wp);
    void remove_waypoint(std::size_t index);
    void clear_waypoints();
    void load_waypoint_file();                 // loop thread
    void save_waypoint_file();                 // loop thread
    std::wstring waypoint_path();

    //=== Cross-thread flags ========================================================

    extern std::atomic<bool> g_panel_open;      // F2
    extern std::atomic<bool> g_map_open;        // M - full map (reserved)
    extern std::atomic<bool> g_reload_config;   // F5 -> loop thread reloads
    // Panel "Revert": re-read the config files only and publish them, dropping unsaved edits.
    extern std::atomic<bool> g_revert_config;
    extern std::atomic<bool> g_save_config;     // panel -> loop thread saves
    // A category filter changed in the UI. Render thread -> loop thread, which writes the four
    // filter keys ~750 ms after the last change, so a run of legend clicks costs one write.
    extern std::atomic<bool> g_save_filters;
    extern std::atomic<bool> g_panel_drew_frame; // set by the render thread, for the log
    // The Bindings tab is waiting for a key press. While it is set the WndProc hook swallows
    // the whole keyboard. Set and cleared by the render thread; read by the WndProc hook.
    extern std::atomic<bool> g_key_capture;
    extern std::atomic<bool> g_waypoint_dirty;   // render -> loop: write the waypoint file

    //=== Logging that is safe from any thread ======================================
    // Output::send touches fmt and, through it, the C++ locale, which is fatal on the game
    // thread: every thread except the loop thread only queues text, and the loop thread drains
    // the queue in on_update. The queue is bounded and never blocks - past `kLogQueueMax`
    // entries a line is dropped and counted - and never waits on the disk.
    //
    // Three levels, one key (`log_level`, Advanced tier):
    //   normal   - startup header, config / map / marker / save-slot loads and changes, hook
    //              installation, every error, warning and first-occurrence diagnostic.
    //   verbose  - the running commentary: `state:`, marker timings, menu transitions, x-ray.
    //   trace    - everything, including per-publish censuses.
    //
    // The macros wrap the whole call, so neither `std::format` nor the arguments run when the
    // level is off. MM_LOGV / MM_LOGVS are verbose, MM_LOGT / MM_LOGTS trace; `mm::log` and
    // `mm::logf` are NORMAL and always emit.

    // Set from the config on every load; read by every log macro.
    extern std::atomic<int> g_log_level;

    inline bool log_enabled(LogLv lv) noexcept
    {
        return static_cast<int>(lv) <= g_log_level.load(std::memory_order_relaxed);
    }

    // "normal" / "verbose" / "trace" <-> LogLv. Pure; an unknown name leaves the fallback in
    // place and returns false.
    const char* log_level_name(LogLv lv) noexcept;
    bool log_level_from_name(std::string_view name, LogLv& out) noexcept;

    void log(const std::wstring& line);
    void set_loop_thread();
    void drain_log();

    // The mod's own rolling log, `wuchang_minimap.log`, rotated per launch keeping `.1` / `.2` /
    // `.3`; every line that reaches UE4SS's log goes here too. Writes are buffered and capped
    // at `kModLogMaxBytes` per session. `modlog_flush()` is safe from any thread.

    // Compares the game build the shipped marker / map data was dumped from against the build
    // now running. One line, once, at startup. Loop thread; plain Win32 file I/O.
    void check_game_build();

    void modlog_flush();
    void modlog_tick(std::uint64_t now_ms);
    std::wstring modlog_path();

    template <typename... Args>
    void logf(std::wformat_string<Args...> fmt, Args&&... args)
    {
        log(std::format(fmt, std::forward<Args>(args)...));
    }

    // Level-checked form for a site where a macro will not do. The format never runs when the
    // level is off, but the ARGUMENTS still do - hence the macros as the default.
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
