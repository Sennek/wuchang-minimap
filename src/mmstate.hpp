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

        wchar_t pawn_name[96]{}; // pawn class / object name, for the debug readout
        wchar_t level_name[160]{}; // pawn full name (carries the world + level path)
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
        // Floor (Z) awareness
        //==============================================================================
        //
        // The map ships one pre-rendered layer per floor plus a per-cell table of
        // walkable surface bands (maps.json `floor_grid`). Every frame the overlay
        // picks the band the player is standing in and draws that band's layer(s).

        bool show_adjacent_floors = true;     // dim the storey below / above
        float adjacent_floor_opacity = 0.25f; // below; above uses 0.6 x this
        float floor_z_tolerance = 150.0f;     // uu of slack around a band's Z range
        float floor_hysteresis = 100.0f;      // uu the new band must win by, on stairs
        // The pawn's location is its capsule centre, ~90 uu above the navmesh it is
        // standing on. Subtracted before the band lookup.
        float player_z_offset = 90.0f;
        int floor_fallback_hold_ms = 3000; // keep the last floor this long when off-grid
        // After that: draw every layer at once (0, the default - costs no extra VRAM)
        // or the Z-shaded composite texture (1, +82 MB of VRAM for Chapter 1).
        bool fallback_use_composite = false;

        bool debug_readout = true;
        bool debug_show_panel_on_start = false; // main-menu verification aid
        int panel_key = 0x71;                   // VK_F2
        int reload_key = 0x74;                  // VK_F5
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

    //==================================================================================
    // Cross-thread flags
    //==================================================================================

    extern std::atomic<bool> g_panel_open;      // F2
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
