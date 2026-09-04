#include "overlay.hpp"

//
// overlay - the DX12 + Dear ImGui side of WuchangMinimap.
//
// HOOK STRATEGY (the hudhook approach)
// ------------------------------------
// The game's IDXGISwapChain and ID3D12CommandQueue are not reachable from a UE4SS mod,
// and the swapchain's command queue cannot be queried back out of the swapchain. So:
//
//   1. create a throwaway D3D12 device + DIRECT command queue + a 64x64 swapchain on a
//      hidden window, purely to read their vtables;
//   2. MinHook the absolute addresses of IDXGISwapChain::Present (vtable slot 8),
//      ResizeBuffers (13), IDXGISwapChain1::Present1 (22) and
//      ID3D12CommandQueue::ExecuteCommandLists (10);
//   3. throw the dummy objects away and wait. The first real Present gives us the
//      swapchain; the first real ExecuteCommandLists gives us the queue.
//
// The dummy objects are created through *our own import table*, i.e. through whatever
// `dxgi.dll` is loaded in the process. On this machine that is **ReShade's** proxy, so
// our dummy swapchain is a ReShade wrapper with exactly the same vtable the game holds
// - which is the point: we hook the same slot the game calls, whoever owns it. The
// module that owns every hooked address is logged, so the log answers the coexistence
// question directly instead of us having to guess. A watchdog complains if no Present
// arrives within a few seconds, which is the signal that the swapchain is wrapped by
// something we did not go through (e.g. a DLSS-FG proxy).
//
// THREADING
// ---------
// Present runs on the RHI / present thread, never the game thread. Nothing in this file
// touches a UObject; the game state arrives through mm::read_snapshot(), which is a
// seqlock. Config file I/O, maps.json parsing and the PNG decode all happen on the
// UE4SS loop thread (on_update) - Present only ever creates D3D12 objects and draws.
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // The shared state declared in overlay_internal.hpp
        //==============================================================================
        float g_ui_scale = 1.0f;          // what the HUD is currently drawn at
        float g_ui_scale_applied = 0.0f;  // what the ImGui style was last built for
        char g_font_loaded[192]{};   // the path the atlas currently holds
        bool g_font_checked = false; // false = the config's path has not been tried yet
        int g_circle_segments = kCircleSegments;
        SrvHeap g_srv_heap;
        int g_pf_frame = -1;    // the whole render prologue + build_ui
        int g_pf_minimap = -1;  // draw_minimap
        int g_pf_markpass = -1; // build_frame_candidates
        int g_pf_slice = -1;    // the minimap height-slice cut (loop thread)
        int g_pf_mslice = -1;   // the full map's cut (loop thread)
        int g_pf_input = -1;    // the hotkey block and the loop thread's file I/O
        int g_pf_pad = -1;      // XInput only, split out of the block above
        spin::Spinlock g_render_lock;
        ID3D12Device* g_device = nullptr;
        std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
        ID3D12GraphicsCommandList* g_cmd_list = nullptr;
        ID3D12DescriptorHeap* g_rtv_heap = nullptr;
        ID3D12Resource* g_backbuffers[kMaxBuffers]{};
        D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kMaxBuffers]{};
        FrameCtx g_frames[kMaxBuffers]{};
        ID3D12Fence* g_fence = nullptr;
        HANDLE g_fence_event = nullptr;
        UINT64 g_fence_value = 0;
        UINT g_buffer_count = 0;
        DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
        UINT g_width = 0;
        UINT g_height = 0;
        HWND g_hwnd = nullptr;
        std::atomic<WNDPROC> g_prev_wndproc{nullptr};
        std::atomic<bool> g_imgui_ready{false};
        std::atomic<bool> g_rt_ready{false};
        std::atomic<bool> g_failed{false};
        MapTexture g_map;
        SliceBuf g_slice[kSliceBufs];
        int g_slice_next = 0;  // the buffer the next update writes (loop thread)
        int g_slice_size = 0;   // side of the currently allocated buffers, px
        std::uint64_t g_slice_last_ms = 0;
        double g_slice_ms = 0.0;     // cost of the last slice, ms (EMA)
        double g_slice_ms_peak = 0.0;
        std::uint64_t g_slice_updates = 0;
        std::uint64_t g_slice_skipped = 0;
        double g_slice_min_y = 0.0;
        double g_slice_max_x = 0.0;
        double g_slice_px_per_uu = 0.0;
        float g_feet_z = 0.0f;
        bool g_feet_z_valid = false;
        std::uint32_t g_slice_opaque = 0;
        std::uint32_t g_slice_dim = 0;
        std::uint32_t g_slice_faint = 0;
        int g_slice_surfaces = 0; // height planes the slicer is reading
        SliceScratch g_slice_scratch;
        std::atomic<bool> g_slicer_pause{false};
        std::atomic<bool> g_slicer_busy{false};
        std::atomic<std::uint32_t> g_slice_gen{0};
        std::atomic<bool> g_slice_copy_pending[kSliceBufs];
        std::atomic<bool> g_mslice_copy_pending[kMapSliceBufs];
        std::atomic<std::uint64_t> g_slice_in_flight[kSliceBufs];
        std::atomic<std::uint64_t> g_mslice_in_flight[kMapSliceBufs];
        std::atomic<int> g_slice_want_px{0};
        std::atomic<std::uint64_t> g_slice_want_ms{0}; // GetTickCount64 of the last request
        spin::Spinlock g_slice_req_lock;
        MapSliceReq g_map_req;
        std::atomic<std::uint64_t> g_map_req_ms{0}; // GetTickCount64 of the last request
        spin::Spinlock g_slice_view_lock;
        SliceView g_slice_view;
        MapSliceView g_mslice_view;
        SliceBuf g_mslice[kMapSliceBufs];
        int g_mslice_next = 0;
        SliceScratch g_mslice_scratch;
        SliceCounts g_mslice_counts{};
        double g_mslice_ms = 0.0;
        double g_mslice_ms_peak = 0.0;
        std::uint64_t g_mslice_updates = 0;
        std::uint64_t g_mslice_skipped = 0;
        std::uint64_t g_mslice_last_ms = 0;
        double g_mr_x0 = 0.0; // south edge
        double g_mr_x1 = 0.0; // north edge
        double g_mr_y0 = 0.0; // west edge
        double g_mr_y1 = 0.0; // east edge
        bool g_mr_valid = false;
        std::atomic<bool> g_map_recut{false};
        int g_mr_w = 0; // the buffer size the cut was made at (a resize is urgent)
        int g_mr_h = 0;
        double g_mr_zoom = 0.0;
        float g_mr_feet = 0.0f;
        double g_mr_px = 0.0; // the player position the cut was made at
        double g_mr_py = 0.0;
        std::string g_mr_chapter;
        mv::View g_mv{};
        bool g_mv_init = false;
        float g_map_floor_off = 0.0f; // uu added to feet Z by the floor adjustment
        bool g_map_help = false;
        bool g_map_was_open = false;
        std::atomic<bool> g_map_recenter{false};
        std::vector<std::pair<std::string, bool>> g_found_override;
        int g_map_markers_drawn = 0;
        int g_map_markers_total = 0;
        IDXGISwapChain* g_swapchain = nullptr;
        IDXGISwapChain3* g_sc3 = nullptr;
        int g_candidates_logged = 0;
        std::atomic<bool> g_readopt{false};
        std::atomic<std::uint64_t> g_readopt_count{0};
        std::atomic<ID3D12CommandQueue*> g_bad_queue{nullptr};
        std::atomic<std::uint64_t> g_present_count{0};
        std::atomic<std::uint64_t> g_resize_count{0};
        std::atomic<const char*> g_render_stage{"no frame yet"};
        std::atomic<unsigned long> g_render_tid{0};
        std::atomic<bool> g_drop_textures{false};
        std::atomic<bool> g_hooks_installed{false};
        bool g_hooks_created = false;                // loop thread only
        std::atomic<bool> g_render_stopped{true};    // render -> loop
        std::atomic<bool> g_watchdog_reported{false};
        std::uint64_t g_hook_install_ms = 0;
        wchar_t g_hide_reason[96] = L"not evaluated yet";
        int g_pf_newframe = -1; // ImGui_ImplWin32_NewFrame - cross-thread user32
        int g_pf_buildui = -1;  // build_ui() - our own drawing
        int g_pf_clip = -1;     // the map -> clipboard hand-off
        int g_pf_save = -1;     // config / waypoint file writes
        int g_pf_reload = -1;   // F5: config + maps + markers
        std::wstring g_hook_report = L"not installed";
        bool g_hooks_from_cache = false;
        wchar_t g_reason_logged[96] = L"";
        std::uint64_t g_reason_log_ms = 0;
        std::uint64_t g_reason_since_ms = 0;
        std::uint64_t g_reason_suppressed = 0;
        PresentFn o_Present = nullptr;
        Present1Fn o_Present1 = nullptr;
        ResizeBuffersFn o_ResizeBuffers = nullptr;
        ExecuteCommandListsFn o_ExecuteCommandLists = nullptr;
        int g_imgui_frames_in_flight = 0;
        std::atomic<std::uint32_t> g_swallow_bits[8]{};
        std::atomic<std::uint64_t> g_swallow_stamp{0};
        spin::Spinlock g_msg_lock;
        PendingMsg g_msg_ring[kMsgRing];
        int g_msg_head = 0;  // oldest unreplayed slot
        int g_msg_count = 0; // slots in use
        std::atomic<std::uint64_t> g_msg_dropped{0};
        std::atomic<bool> g_imgui_want_keyboard{false};
        MiniDebug g_last_mini{};
        std::atomic<int> g_zoom_steps{0};
        std::atomic<bool> g_hud_gate_ever_open{false};
        gly::Palette g_palette = gly::Palette::Default;
        mdb::Rgb g_plate = gly::theme_colors(gly::Theme::Neutral).plate;
        MarkerDrawStats g_marker_draw{};
        std::vector<FrameCand> g_frame_cands; // render thread only, reused every frame
        int g_frame_marker_total = 0;         // rows in the published buffer
        int g_frame_bad_cat = 0;              // rows whose category byte is out of range
        float g_hud_fade = 0.0f;
        std::uint64_t g_hud_fade_ms = 0; // when the current show started
        char g_toast[160]{};
        std::uint64_t g_toast_until = 0;
        ShotStage g_shot_stage = ShotStage::Idle;
        std::atomic<bool> g_shot_request{false};   // loop -> render (the hotkey)
        ID3D12Resource* g_shot_readback = nullptr; // render thread only
        std::uint64_t g_shot_fence = 0;
        UINT g_shot_w = 0;
        UINT g_shot_h = 0;
        UINT g_shot_pitch = 0;
        clipimg::Fmt g_shot_fmt = clipimg::Fmt::Unknown;
        mv::Rect g_shot_canvas{};
        bool g_shot_canvas_valid = false;
        spin::Spinlock g_shot_lock;
        std::vector<std::uint8_t> g_shot_dib;
        std::atomic<bool> g_shot_dib_ready{false};
        std::atomic<bool> g_shot_stage_done{false};
        spin::Spinlock g_toast_lock;
        char g_toast_pending[160]{};
        unsigned g_toast_pending_ms = 2500;
        std::atomic<bool> g_toast_pending_ready{false};
        bool g_shrine_panel = false;
        char g_shrine_selected[shdb::kMaxIdLen]{};
        StatsCache g_stats_cache;
        bool g_stats_page = false; // the full map's Stats panel
        FoundWatch g_found_watch[kFoundWatch]{};
        int g_found_watch_n = 0;
        std::uint64_t g_found_watch_round = 0;
        FoundEvent g_found_events[kFoundEvents]{};
        int g_found_event_head = 0;
        HighlightDebug g_hl_debug{};
        CompassDebug g_compass_debug{};
        std::atomic<std::uint32_t> g_panel_sections{kPanelSectionsDefault};
        std::atomic<bool> g_panel_state_dirty{false};
        std::atomic<bool> g_panel_state_loaded{false};
        int g_capture_row = -1;            // render thread only; -1 = nothing armed
        bool g_capture_wait_release = false;

        //==============================================================================
        // UI SCALE (review-0.9.1 § 1 item 1)
        //==============================================================================
        //
        // ImGui's built-in font is 13 px and nothing here ever touched it, so on a 4K
        // screen every label, tooltip, x-ray name and compass letter was a quarter of
        // its intended physical size - the single biggest legibility problem in the mod.
        //
        // ONE number fixes all of it. `ui_scale = auto` derives it from the back buffer
        // height; a number in the config pins it. It is applied in exactly two places:
        //
        //   apply_ui_scale()  - the ImGui font size and the whole style, on the RENDER
        //                       thread, before the frame's draw lists are built (the
        //                       same rule the F5 texture drop obeys).
        //   ui_scaled()       - every PIXEL config key, multiplied once per frame into
        //                       a copy of the Config the HUD draws from.
        //
        // Nothing else in the drawing code knows about it, which is what stops the
        // scale being applied twice to something or not at all to something else.
        //

        float wanted_ui_scale(const mm::Config& cfg, float screen_h)
        {
            float s = cfg.ui_scale;
            if (cfg.ui_scale_auto)
            {
                // 1080p is the design size, so 1.0 there and 2.0 at 2160p. Never below
                // 1: shrinking the UI on a small screen helps nobody.
                s = screen_h > 0.0f ? screen_h / kBaseScreenHeight : 1.0f;
                s = (std::max)(1.0f, (std::min)(kUiScaleMax, s));
            }
            else
            {
                s = (std::max)(kUiScaleMin, (std::min)(kUiScaleMax, s));
            }
            // Snap to a hundredth: a back buffer of 1081 px must not make the style be
            // rebuilt on the next frame for a difference nobody can see.
            return std::round(s * 100.0f) / 100.0f;
        }

        //==============================================================================
        // THE FONT (review B.11)
        //==============================================================================
        //
        // ImGui's built-in font is ProggyClean, a 13-pixel BITMAP. At 1080p that is the
        // size it was drawn for; at 2160p `style.FontScaleMain = 2` magnifies the bitmap,
        // which is the one part of the HUD a resolution-independent design cannot fake -
        // every number in the panel and every marker label came out soft and blocky.
        //
        // So a real TTF is loaded and rasterised at 13 px, and ImGui 1.92's dynamic atlas
        // re-rasterises it at 13 * ui_scale when the scale changes (the backend declares
        // ImGuiBackendFlags_RendererHasTextures, so there is no atlas of ours to rebuild
        // and no texture of ours to release - which is also why this is the only place
        // that has to react to a scale change at all).
        //

        bool font_path_is_none(const char* path)
        {
            if (path == nullptr || path[0] == 0)
            {
                return true;
            }
            return ::_stricmp(path, "none") == 0 || ::_stricmp(path, "off") == 0;
        }

        void ensure_ui_font(const mm::Config& cfg)
        {
            if (g_font_checked && ::strcmp(g_font_loaded, cfg.ui_font) == 0)
            {
                return; // steady state: one strcmp of a short string per frame
            }
            g_font_checked = true;
            ::strncpy_s(g_font_loaded, sizeof(g_font_loaded), cfg.ui_font, _TRUNCATE);

            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->Clear();
            if (font_path_is_none(cfg.ui_font))
            {
                io.Fonts->AddFontDefault();
                mm::log(L"ui font: the built-in bitmap font (ui_font = none)");
                return;
            }
            // 13 px is the BASE size; style.FontScaleMain multiplies it, so this number
            // stays 13 at every resolution and the scaling lives in one place.
            const ImFont* f = io.Fonts->AddFontFromFileTTF(cfg.ui_font, 13.0f);
            const std::wstring shown(cfg.ui_font, cfg.ui_font + ::strlen(cfg.ui_font));
            if (f == nullptr)
            {
                // A wrong path is one log line and a working mod, never a mod with no
                // text in it.
                io.Fonts->Clear();
                io.Fonts->AddFontDefault();
                mm::logf(L"ui font: could not read '{}' - using the built-in bitmap font", shown);
                return;
            }
            mm::logf(L"ui font: {} at 13 px (x ui scale {:.2f})", shown, static_cast<double>(g_ui_scale));
        }

        //==============================================================================
        // KEYBOARD AND GAMEPAD NAVIGATION (review B.8)
        //==============================================================================
        //
        // Called once from the D3D12 init, straight after CreateContext. The F2 panel was
        // mouse-only: a player on a controller could open it and then not move inside it.
        //
        // NavEnableGamepad makes ImGui read io's gamepad buttons. It does NOT make
        // anything poll XInput here: the backend's own XInput code is compiled out (see
        // xmake.lua) because that runs inside Present, and feed_pad_nav() below hands
        // ImGui the state the loop thread has already sampled.
        //
        // NavEnableSetMousePos is deliberately NOT set - it would warp the OS cursor to
        // the focused widget, and the game owns that cursor.
        void ui_init_io(ImGuiIO& io)
        {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        }

        // RENDER THREAD. The pad state comes from gamepad.cpp on the loop thread; this
        // only translates it into io events.
        //
        // ONLY WHILE THE PANEL IS OPEN. The full map reads the pad directly (it is a
        // canvas, not a widget tree, and it has its own bindings for the sticks and the
        // triggers), and during play the pad belongs to the game - a stick push must
        // never move a focus rectangle nobody can see.
        void feed_pad_nav(const mm::Config& cfg)
        {
            static bool fed_last_frame = false;
            static const ImGuiKey kAllPadKeys[] = {
                ImGuiKey_GamepadFaceDown,   ImGuiKey_GamepadFaceRight,  ImGuiKey_GamepadFaceLeft,
                ImGuiKey_GamepadFaceUp,     ImGuiKey_GamepadDpadUp,     ImGuiKey_GamepadDpadDown,
                ImGuiKey_GamepadDpadLeft,   ImGuiKey_GamepadDpadRight,  ImGuiKey_GamepadL1,
                ImGuiKey_GamepadR1,         ImGuiKey_GamepadStart,      ImGuiKey_GamepadBack,
                ImGuiKey_GamepadLStickUp,   ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft,
                ImGuiKey_GamepadLStickRight};

            const pad::State gp = pad::state();
            const bool want = cfg.map_gamepad && gp.connected &&
                              mm::g_panel_open.load(std::memory_order_relaxed);
            if (!want)
            {
                if (fed_last_frame)
                {
                    // Release everything ONCE. Without this ImGui keeps whatever was
                    // last held for ever, and nav stays stuck in a direction - the same
                    // "nothing latched" rule the rest of the input obeys.
                    ImGuiIO& io = ImGui::GetIO();
                    for (const ImGuiKey k : kAllPadKeys)
                    {
                        io.AddKeyEvent(k, false);
                    }
                    fed_last_frame = false;
                }
                return;
            }
            fed_last_frame = true;
            ImGuiIO& io = ImGui::GetIO();
            const auto btn = [&io, &gp](ImGuiKey key, std::uint16_t bit) {
                io.AddKeyEvent(key, (gp.held & bit) != 0);
            };
            // A activates, B cancels: the Xbox layout, which is what ImGui's own nav
            // key names mean.
            btn(ImGuiKey_GamepadFaceDown, pad::kA);
            btn(ImGuiKey_GamepadFaceRight, pad::kB);
            btn(ImGuiKey_GamepadFaceLeft, pad::kX);
            btn(ImGuiKey_GamepadFaceUp, pad::kY);
            btn(ImGuiKey_GamepadDpadUp, pad::kDpadUp);
            btn(ImGuiKey_GamepadDpadDown, pad::kDpadDown);
            btn(ImGuiKey_GamepadDpadLeft, pad::kDpadLeft);
            btn(ImGuiKey_GamepadDpadRight, pad::kDpadRight);
            btn(ImGuiKey_GamepadL1, pad::kLeftShoulder);
            btn(ImGuiKey_GamepadR1, pad::kRightShoulder);
            btn(ImGuiKey_GamepadStart, pad::kStart);
            btn(ImGuiKey_GamepadBack, pad::kBack);
            // The left stick moves the focus. ImGui wants an analogue value for a
            // directional nav key (0 = not pressed, 1 = fully pushed); the deadzone has
            // already been applied and the range rescaled by gamepad.cpp.
            const auto axis = [&io](ImGuiKey key, float v) {
                const float a = v > 0.0f ? (v > 1.0f ? 1.0f : v) : 0.0f;
                io.AddKeyAnalogEvent(key, a > 0.1f, a);
            };
            axis(ImGuiKey_GamepadLStickRight, gp.lx);
            axis(ImGuiKey_GamepadLStickLeft, -gp.lx);
            axis(ImGuiKey_GamepadLStickUp, gp.ly);
            axis(ImGuiKey_GamepadLStickDown, -gp.ly);
        }

        // Render thread. Rebuilds the ImGui style FROM SCRATCH at the new scale - never
        // ScaleAllSizes on the already-scaled style, which would compound every time.
        void apply_ui_scale(float scale)
        {
            // The font rides along here because this is the one function that runs on
            // the render thread at the top of every frame, before a draw list exists.
            ensure_ui_font(mm::cfg_cached());
            if (scale == g_ui_scale_applied)
            {
                return;
            }
            ImGuiStyle& style = ImGui::GetStyle();
            style = ImGuiStyle{};
            ImGui::StyleColorsDark();
            style.WindowRounding = 4.0f;
            style.ScaleAllSizes(scale);
            style.FontScaleMain = scale;
            g_ui_scale_applied = scale;
            g_ui_scale = scale;
            mm::logf(L"ui scale: {:.2f} (font {:.0f} px, style rebuilt)", scale, 13.0f * scale);
        }

        // Every config key that is a NUMBER OF PIXELS, multiplied once. Fractions of the
        // screen (minimap_size, compass_width, map_margin) are already resolution
        // independent and are deliberately absent.
        mm::Config ui_scaled(const mm::Config& cfg, float s)
        {
            mm::Config out = cfg;
            if (s == 1.0f)
            {
                return out;
            }
            out.markers_size *= s;
            out.map_marker_size *= s;
            out.highlight_size *= s;
            out.compass_height *= s;
            out.compass_offset_y *= s;
            out.offset_x *= s;
            out.offset_y *= s;
            out.minimap_min_px *= s;
            out.minimap_arrow_min_px *= s;
            // THE TWO ZOOM KEYS (review B.12). `zoom_uu_per_px` and `map_zoom` are world
            // units per SCREEN PIXEL, and the disc's SIZE is a fraction of the screen -
            // so leaving them alone means a 4K minimap, twice as many pixels across,
            // shows twice the world radius at the same setting. That is a different view,
            // not a bigger one, and it is not what "a config tuned at 1080p is correct at
            // 4K" promises anywhere else in this function.
            //
            // Coverage is pixels x uu-per-pixel, and the pixels went up by `s`, so the
            // uu per pixel has to come DOWN by `s` to keep the coverage identical: the
            // 4K disc is twice as wide and each of its pixels covers half as much ground,
            // which is the same picture at twice the detail. `zoom_dpi_scaled = 0`
            // restores 1.0.0's literal behaviour for anyone who preferred it.
            //
            // The zoom LADDER (minimap_zoom_presets) is deliberately not touched here: it
            // is the set of values the zoom key writes back into zoom_uu_per_px, i.e. a
            // config value, and scaling it would feed a scaled number into the config
            // file the next time the key was pressed.
            //
            // Only the MINIMAP's key is scaled here. The full map is handed the
            // unscaled config on purpose (its filter chips write back into it, and a
            // scaled number must never reach the config file), so it applies the same
            // factor at the point of use - see `zscale` in draw_full_map.
            if (cfg.zoom_dpi_scaled && s > 0.0f)
            {
                out.zoom_uu_per_px /= s;
            }
            return out;
        }

        //==============================================================================
        // HUD PLACEMENT (review-0.9.1 § 1 item 12)
        //==============================================================================
        //
        // `hud_preset` is one key that moves the WHOLE HUD. `custom` (the shipped
        // default) means "obey minimap_anchor / minimap_offset_* / compass_anchor
        // exactly as written", i.e. v0.9.1 unchanged; any other value puts the minimap
        // in that corner and the compass on the same vertical side, so the two can no
        // longer end up on opposite halves of the screen because one key was edited and
        // the other was not.
        //
        // Both readers are pure functions of the config, evaluated per frame, so a
        // preset change is live and there is nothing to keep in sync.
        mm::Anchor effective_anchor(const mm::Config& cfg)
        {
            switch (cfg.hud_preset)
            {
            case mm::HudPreset::TopLeft:
                return mm::Anchor::TopLeft;
            case mm::HudPreset::TopRight:
                return mm::Anchor::TopRight;
            case mm::HudPreset::BottomLeft:
                return mm::Anchor::BottomLeft;
            case mm::HudPreset::BottomRight:
                return mm::Anchor::BottomRight;
            case mm::HudPreset::Custom:
            default:
                return cfg.anchor;
            }
        }

        bool compass_at_bottom(const mm::Config& cfg)
        {
            switch (cfg.hud_preset)
            {
            case mm::HudPreset::BottomLeft:
            case mm::HudPreset::BottomRight:
                return true;
            case mm::HudPreset::TopLeft:
            case mm::HudPreset::TopRight:
                return false;
            case mm::HudPreset::Custom:
            default:
                return cfg.compass_anchor == mm::VAnchor::Bottom;
            }
        }


        //==============================================================================
        // Small helpers
        //==============================================================================

        std::wstring module_of(const void* addr)
        {
            HMODULE mod = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(addr),
                                     &mod) == 0 ||
                mod == nullptr)
            {
                return L"<unknown>";
            }
            wchar_t path[MAX_PATH * 2]{};
            if (::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path))) == 0)
            {
                return L"<unknown>";
            }
            std::wstring s{path};
            const auto slash = s.find_last_of(L'\\');
            const std::wstring name = slash == std::wstring::npos ? s : s.substr(slash + 1);
            return std::format(L"{}+0x{:X}",
                               name,
                               reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod));
        }

        //==============================================================================
        // WHOSE FUNCTION IS THIS, AND WAS SOMEBODY ALREADY THERE
        //==============================================================================
        //
        // Three overlays live in this process - ReShade (this game's dxgi.dll IS a
        // ReShade proxy), Steam's GameOverlayRenderer64 and us - and all three want
        // IDXGISwapChain::Present. When the Steam FPS counter stops appearing the first
        // question is always "is our hook ON TOP of Steam's, UNDER it, or did we replace
        // it", and that is answerable in one line: read the first bytes of the function
        // BEFORE hooking it. A jmp already sitting there names the module that put it
        // there - which is the proof that our MinHook trampoline chains INTO that module
        // rather than around it.
        //
        // MinHook is a trampoline on the function, never a vtable patch, so a detour
        // installed before ours ends up downstream of ours (its bytes are relocated into
        // our trampoline) and one installed after ours ends up upstream. Either way the
        // chain is intact, and hk_Present / hk_ResizeBuffers / hk_Present1 call the
        // original unconditionally, for every swapchain, ours or not.


        // The three PE fields that identify a BUILD of a DLL. All of them are baked into
        // the file, so they are identical on every launch - which is what makes an RVA
        // captured in one session safe to reuse in the next.
        bool module_identity(HMODULE mod, ModuleId& out)
        {
            if (mod == nullptr)
            {
                return false;
            }
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return false;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return false;
            }
            out.base = mod;
            out.size = nt->OptionalHeader.SizeOfImage;
            out.stamp = nt->FileHeader.TimeDateStamp;
            out.sum = nt->OptionalHeader.CheckSum;

            wchar_t path[MAX_PATH * 2]{};
            if (::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path))) == 0)
            {
                return false;
            }
            std::wstring p{path};
            const auto slash = p.find_last_of(L'\\');
            std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
            for (wchar_t& c : name)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c + 32);
                }
            }
            if (name.size() + 1 >= std::size(out.name))
            {
                return false;
            }
            std::memcpy(out.name, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
            return true;
        }

        bool module_id_of(const void* addr, ModuleId& out)
        {
            HMODULE mod = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(addr),
                                     &mod) == 0 ||
                !module_identity(mod, out))
            {
                return false;
            }
            out.rva = static_cast<std::uint32_t>(reinterpret_cast<const std::uint8_t*>(addr) -
                                                 reinterpret_cast<const std::uint8_t*>(mod));
            return true;
        }

        // "ALREADY DETOURED -> <module>+<offset>" or "no detour", plus the raw bytes.
        std::wstring detour_report(const void* addr)
        {
            if (addr == nullptr)
            {
                return L"<null>";
            }
            const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
            std::wstring bytes;
            for (int i = 0; i < 8; ++i)
            {
                bytes += std::format(L"{:02X} ", p[i]);
            }
            const std::uint8_t* target = nullptr;
            if (p[0] == 0xE9) // jmp rel32 - MinHook's own shape, and Steam's
            {
                std::int32_t rel = 0;
                std::memcpy(&rel, p + 1, sizeof(rel));
                target = p + 5 + rel;
            }
            else if (p[0] == 0xFF && p[1] == 0x25) // jmp [rip+disp32]
            {
                std::int32_t disp = 0;
                std::memcpy(&disp, p + 2, sizeof(disp));
                const void* const* slot = reinterpret_cast<const void* const*>(p + 6 + disp);
                target = reinterpret_cast<const std::uint8_t*>(*slot);
            }
            else if (p[0] == 0x48 && p[1] == 0xB8) // mov rax, imm64 (followed by jmp rax)
            {
                std::uint64_t imm = 0;
                std::memcpy(&imm, p + 2, sizeof(imm));
                target = reinterpret_cast<const std::uint8_t*>(imm);
            }
            if (target != nullptr)
            {
                return std::format(L"[{}] ALREADY DETOURED -> {}", bytes, module_of(target));
            }
            return std::format(L"[{}] no detour", bytes);
        }

        // The overlays sharing this process, with their bases - so "who is here" sits in
        // the log next to the hook report instead of being guessed at.
        void log_overlay_modules()
        {
            static const wchar_t* const names[] = {L"dxgi.dll",
                                                   L"d3d12.dll",
                                                   L"GameOverlayRenderer64.dll",
                                                   L"ReShade64.dll",
                                                   L"nvngx_dlssg.dll",
                                                   L"sl.interposer.dll"};
            for (const wchar_t* name : names)
            {
                const HMODULE mod = ::GetModuleHandleW(name);
                if (mod == nullptr)
                {
                    continue;
                }
                ModuleId id{};
                module_identity(mod, id);
                mm::logf(L"  module {} @ {:p}  size 0x{:X}  stamp 0x{:08X}  sum 0x{:08X}",
                         name,
                         static_cast<void*>(mod),
                         id.size,
                         id.stamp,
                         id.sum);
            }
        }



        //==============================================================================
        // SRV descriptor heap allocator
        //==============================================================================
        //
        // ImGui 1.92's DX12 backend allocates SRV descriptors through callbacks (it
        // needs one per texture now, not just one for the font atlas), so the heap and
        // its free list are ours. A fixed 64-slot heap is plenty: the font atlas plus
        // one map texture.



        void srv_alloc_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            if (!g_srv_heap.alloc(*cpu, *gpu))
            {
                cpu->ptr = 0;
                gpu->ptr = 0;
                // ONCE. ImGui asks for a descriptor when it (re)builds the font atlas,
                // which is not a per-frame event - but this is a render-thread callback
                // and a heap that is full stays full, so it says so once and then stops.
                static bool said = false;
                if (!said)
                {
                    said = true;
                    mm::log(L"SRV heap exhausted - raise srv_heap_size (RESTART) if the "
                            L"overlay is missing textures");
                }
            }
        }

        void srv_free_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            g_srv_heap.free(cpu);
        }

        //==============================================================================
        // Renderer state
        //==============================================================================



        //==============================================================================
        // The height-slice texture
        //==============================================================================
        //
        // A small dynamic RGBA8 texture the CPU slicer refills at slice_hz. Two of
        // them, because the GPU may still be sampling one while we write the next:
        // `in_flight_fence` is the fence value of the last frame that DREW this buffer,
        // so a buffer is only rewritten once GetCompletedValue() has passed it.
        //
        // The upload heap stays mapped for the buffer's whole life (a 512x512 RGBA
        // window is 1 MB, and Map/Unmap per update is pure overhead), and the copy is
        // recorded on the same command list Present already records for ImGui - so
        // there is no extra queue, no PSO and no root signature on ReShade's swapchain.




        // QueryPerformanceFrequency is a constant for the life of the process, and it
        // was being asked for on every slice and every map cut. Once, lazily.
        std::int64_t qpc_freq()
        {
            static const std::int64_t freq = [] {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                return static_cast<std::int64_t>(f.QuadPart);
            }();
            return freq;
        }



        // The window proc that was there before ours, and the one every message is

        // ATOMIC, because all three are written by the RENDER thread and read by other
        // threads: `g_imgui_ready` gates the WndProc hook's whole body on the GAME
        // thread, and the F2 panel / the loop thread read the other two. As plain bools
        // NOT TERMINAL FOR A SWAPCHAIN-LEVEL FAILURE. `g_failed` means "this mod cannot
        // draw and must stop trying": a hook that would not install, a device that is




        //==============================================================================
        // The slicer runs on the LOOP thread
        //==============================================================================
        //
        // `slice_region` is 1-4 ms of pure CPU that writes into a persistently mapped
        // upload heap. Nothing about it needs Present, and running it inside the frame
        // put a 1-4 ms spike into frame time twelve times a second. It now runs from
        // `overlay::on_update` on the UE4SS loop thread; `render()` only records the
        // CopyTextureRegion for a buffer the slicer has finished and stamps the fence of
        // the frame that sampled it.
        //
        // THE OWNERSHIP RULES, which are what keep this safe:
        //   * the RENDER thread owns creation and destruction of every D3D12 object,
        //     including the slice buffers, and publishes what it wants cut;
        //   * the LOOP thread only ever writes into `mapped` memory of a buffer that
        //     already exists, and only while the slicer is not paused;
        //   * a buffer may be written only when the fence of the last frame that
        //     sampled it has completed (`g_slice_in_flight`), exactly as before;
        //   * before creating or destroying any slice buffer the render thread PAUSES
        //     the slicer and waits, with a bound, for it to leave the critical section.
        //     If it cannot, it does not touch the buffers this frame and tries again.
        //   * `g_slice_gen` is bumped by every create/destroy; the slicer re-reads it
        //     after writing and throws the result away if it moved, so a cut can never
        //     be published against a buffer set that no longer exists.






        SliceView slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            return g_slice_view;
        }

        MapSliceView map_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            return g_mslice_view;
        }

        void clear_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            g_slice_view = SliceView{};
        }

        void clear_map_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            g_mslice_view = MapSliceView{};
        }

        // RENDER THREAD. Stop the slicer and wait for it to leave its critical section.
        // Returns false when it did not stop inside `budget_ms` - the caller must then
        // leave every slice buffer alone and try again on the next frame. The slicer's
        // critical section is a few milliseconds of arithmetic and never blocks, so a
        // timeout means something is very wrong and skipping is the safe answer.
        bool slicer_pause_begin(unsigned budget_ms)
        {
            g_slicer_pause.store(true); // seq_cst on purpose: it pairs with the loop's
                                        // "check, mark busy, check again" sequence
            const std::uint64_t deadline = ::GetTickCount64() + budget_ms;
            while (g_slicer_busy.load())
            {
                if (::GetTickCount64() > deadline)
                {
                    g_slicer_pause.store(false);
                    return false;
                }
                ::SwitchToThread();
            }
            return true;
        }

        void slicer_pause_end()
        {
            g_slicer_pause.store(false);
        }

        // Bumped by every create/destroy so a cut in flight can be discarded.
        void note_slice_buffers_changed()
        {
            g_slice_gen.fetch_add(1, std::memory_order_release);
        }

        //==============================================================================
        // The full map (step C1)
        //==============================================================================
        //
        // The same height-sliced asset the minimap draws, at map scale: north-up,
        // pannable, zoomable, with every marker on it. It has its own pair of dynamic
        // textures because its window is both bigger and DECIMATED - one texture pixel
        // covers `step` source pixels - and its own update policy: the minimap re-cuts
        // 12 times a second because the player is always moving, while the map only
        // re-cuts when something actually changed (pan out of the cut region, zoom,
        // floor slice, a big player move), capped at map_slice_hz.



        // The SAME object as g_swapchain, QueryInterface'd once and kept with a
        // reference held, because `GetCurrentBackBufferIndex()` lives only on
        // IDXGISwapChain3 and a QI per frame is a virtual call plus an AddRef/Release

        // RE-ADOPTION. Set when the swapchain or the device we latched onto has stopped
        // being usable - DXGI_ERROR_DEVICE_REMOVED / _RESET out of Present, a GetBuffer
        // or render-target failure, a command queue that turns out to belong to another

        // Drops the captured command queue AND the reference held on it. Only ever
        // called from the render thread's teardown, so no other thread can be inside
        // `o_ExecuteCommandLists(queue, ...)` with our pointer at the same time.
        void safe_release_queue()
        {
            ID3D12CommandQueue* queue = g_queue.exchange(nullptr, std::memory_order_acq_rel);
            if (queue != nullptr)
            {
                queue->Release();
            }
        }

        void request_readoption(const wchar_t* why)
        {
            if (!g_readopt.exchange(true, std::memory_order_release))
            {
                g_readopt_count.fetch_add(1, std::memory_order_relaxed);
                mm::logf(L"the overlay is releasing its D3D12 objects and will adopt the swapchain again "
                         L"on a later Present: {}",
                         why);
            }
        }

        // WHAT THE RENDER THREAD IS DOING, and which thread it is, for the loop

        // The stage names are ASCII literals and the log takes wide strings. An explicit
        // cast loop rather than `std::wstring(a.begin(), a.end())`, which warns (C4244)
        // and this repo is warning-free by policy.
        std::wstring stage_w(const char* s)
        {
            const char* p = s != nullptr ? s : "?";
            std::wstring out;
            for (; *p != '\0'; ++p)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
            }
            return out;
        }



        // Every hide/show transition is logged with its reason, so one line in the log

        void set_hide_reason(const wchar_t* text)
        {
            if (::wcscmp(g_hide_reason, text) == 0)
            {
                return; // unchanged - nothing to record, nothing to log
            }
            ::wcsncpy_s(g_hide_reason, text, std::size(g_hide_reason) - 1);
            const std::uint64_t now = ::GetTickCount64();
            const std::uint64_t held = g_reason_since_ms == 0 ? 0 : now - g_reason_since_ms;
            g_reason_since_ms = now;
            if (::wcscmp(g_reason_logged, text) == 0)
            {
                return; // flapping between two states we already reported
            }
            if (now - g_reason_log_ms < static_cast<std::uint64_t>(mm::cfg_cached().hide_reason_log_ms))
            {
                ++g_reason_suppressed;
                return;
            }
            const bool visible = ::wcscmp(text, L"visible") == 0;
            MM_LOGV(L"minimap {}: {} (previous state held {} ms{})",
                    visible ? L"SHOWN" : L"HIDDEN",
                    text,
                    held,
                    g_reason_suppressed != 0 ? std::format(L", {} change(s) suppressed",
                                                           g_reason_suppressed)
                                             : std::wstring{});
            ::wcsncpy_s(g_reason_logged, text, std::size(g_reason_logged) - 1);
            g_reason_log_ms = now;
            g_reason_suppressed = 0;
        }

        //==============================================================================
        // Original functions
        //==============================================================================



        //==============================================================================
        // Teardown of the swapchain-dependent objects
        //==============================================================================

        void release_render_targets()
        {
            for (UINT i = 0; i < kMaxBuffers; ++i)
            {
                safe_release(g_backbuffers[i]);
                g_rtv[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            safe_release(g_rtv_heap);
            g_rt_ready = false;
        }

        void wait_for_gpu()
        {
            if (g_fence == nullptr || g_fence_event == nullptr)
            {
                return;
            }
            if (g_fence->GetCompletedValue() >= g_fence_value)
            {
                return;
            }
            if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fence_value, g_fence_event)))
            {
                ::WaitForSingleObject(g_fence_event, 1000);
            }
        }

        // How many frames in flight the ImGui DX12 backend was initialised with. A

        // ONE ALLOCATOR PER BACK BUFFER, created for every buffer that has none. They
        // used to be created once, for the count seen at init: after a fullscreen toggle
        // that raised BufferCount from 2 to 3, `render()` called `frame.allocator->Reset()`
        // on a null pointer. `g_buffer_count` is already clamped to kMaxBuffers.
        bool ensure_frame_allocators()
        {
            if (g_device == nullptr)
            {
                return false;
            }
            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (g_frames[i].allocator != nullptr)
                {
                    continue;
                }
                if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&g_frames[i].allocator))))
                {
                    mm::logf(L"CreateCommandAllocator({}) failed", i);
                    return false;
                }
                g_frames[i].fence_value = 0;
            }
            return true;
        }

        bool create_render_targets(IDXGISwapChain* swapchain)
        {
            release_render_targets();

            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(swapchain->GetDesc(&desc)))
            {
                mm::log(L"GetDesc failed - cannot create render targets");
                return false;
            }
            g_buffer_count = (std::min)(desc.BufferCount, static_cast<UINT>(kMaxBuffers));
            if (g_buffer_count == 0)
            {
                g_buffer_count = 2;
            }
            g_format = desc.BufferDesc.Format;
            g_width = desc.BufferDesc.Width;
            g_height = desc.BufferDesc.Height;
            g_hwnd = desc.OutputWindow;

            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = g_buffer_count;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap))))
            {
                mm::log(L"CreateDescriptorHeap(RTV) failed");
                return false;
            }

            const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]))))
                {
                    mm::logf(L"GetBuffer({}) failed", i);
                    release_render_targets();
                    return false;
                }
                g_device->CreateRenderTargetView(g_backbuffers[i], nullptr, handle);
                g_rtv[i] = handle;
                handle.ptr += stride;
            }

            // DOES THE BACK BUFFER BELONG TO THE DEVICE WE TOOK OFF THE QUEUE? The
            // device comes from the first DIRECT command queue seen executing anywhere
            // in the process (IDXGISwapChain::GetDevice does not work through this
            // game's ReShade wrapper), and with frame generation or a second renderer
            // that queue need not belong to the presenting device. `ID3D12Resource::
            // GetDevice` on a back buffer answers authoritatively, and it is the one
            // link from the swapchain to a device that the wrapper does forward.
            // Recording our command list on a queue of a different device is an
            // immediate device removal, so this is a hard reject.
            if (g_backbuffers[0] != nullptr)
            {
                ID3D12Device* owner = nullptr;
                if (SUCCEEDED(g_backbuffers[0]->GetDevice(IID_PPV_ARGS(&owner))) && owner != nullptr)
                {
                    const bool same = owner == g_device;
                    owner->Release();
                    if (!same)
                    {
                        mm::log(L"the back buffers belong to a different ID3D12Device than the captured "
                                L"command queue - dropping the queue so another one can be captured");
                        g_bad_queue.store(g_queue.load(std::memory_order_acquire), std::memory_order_release);
                        release_render_targets();
                        request_readoption(L"the captured command queue belongs to another device");
                        return false;
                    }
                }
            }

            if (!ensure_frame_allocators())
            {
                release_render_targets();
                return false;
            }

            g_rt_ready = true;
            mm::logf(L"render targets: {} buffer(s), {}x{}, {}, hwnd 0x{:X}",
                     g_buffer_count,
                     g_width,
                     g_height,
                     format_name(g_format),
                     reinterpret_cast<std::uintptr_t>(g_hwnd));
            return true;
        }

        //==============================================================================
        // [fix-ui] HOTKEY SWALLOW (review B.13)
        //==============================================================================
        //
        // A key bound to a mod action used to reach the game as well: the loop thread
        // samples it with GetAsyncKeyState and the WndProc hook let the message through,
        // so `M` opened the full map AND did whatever `M` does in the game. The async
        // key state is kernel-side and cannot be denied to a game that polls it
        // (lessons.md) - but the WINDOW MESSAGE can be, and that is how UE reads its
        // keyboard here.
        //
        // The decision has to cost one atomic read, because it runs on the window thread
        // for every key message. So the LOOP thread - the only place that knows which
        // bindings exist, which of them are live at this instant and whether their
        // modifier is held - publishes a 256-bit set of virtual keys, and this tests a
        // bit. publish_swallow_set(), in the hotkey block, is the other half.
        //
        // WHEN THE SET WAS LAST PUBLISHED. The loop thread refreshes it on every 60 Hz

        void swallow_set_clear()
        {
            for (std::atomic<std::uint32_t>& w : g_swallow_bits)
            {
                w.store(0, std::memory_order_relaxed);
            }
        }

        void swallow_set_add(int vk)
        {
            if (vk > 0 && vk < 256)
            {
                g_swallow_bits[vk >> 5].fetch_or(1u << (static_cast<unsigned>(vk) & 31u),
                                                 std::memory_order_relaxed);
            }
        }

        // Key DOWN / UP only. WM_CHAR carries a character rather than a virtual key, so
        // testing it against a VK would be a coincidence, and nothing here reads text.
        bool is_hotkey_message(UINT msg)
        {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        }

        bool hotkey_swallow(WPARAM wparam)
        {
            const unsigned vk = static_cast<unsigned>(wparam);
            if (vk == 0 || vk >= 256)
            {
                return false;
            }
            const std::uint64_t stamp = g_swallow_stamp.load(std::memory_order_relaxed);
            if (stamp == 0 || ::GetTickCount64() - stamp > kSwallowStaleMs)
            {
                return false;
            }
            return (g_swallow_bits[vk >> 5].load(std::memory_order_relaxed) & (1u << (vk & 31u))) != 0;
        }

        //==============================================================================
        // WndProc hook
        //==============================================================================

        bool is_mouse_message(UINT msg)
        {
            return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE ||
                   msg == WM_NCMOUSEMOVE;
        }

        bool is_keyboard_message(UINT msg)
        {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                   msg == WM_CHAR || msg == WM_SETCURSOR;
        }

        // ESCAPE, as a WINDOW MESSAGE. WM_CHAR carries the control character (0x1B), the
        // key messages carry the virtual key - two different numbers that happen to be
        // the same one here, which is worth spelling out rather than relying on.
        bool is_escape_message(UINT msg, WPARAM wparam)
        {
            if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
            {
                return wparam == VK_ESCAPE;
            }
            if (msg == WM_CHAR || msg == WM_SYSCHAR)
            {
                return wparam == 0x1B;
            }
            return false;
        }

        bool is_escape_key_down(UINT msg, WPARAM wparam)
        {
            return (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wparam == VK_ESCAPE;
        }

        // THE TWO CHORDS THE OVERLAY MUST NEVER EAT. ALT+F4 closes the game and ALT+ENTER
        // toggles fullscreen, and both arrive as WM_SYSKEYDOWN / WM_SYSKEYUP (WM_SYSCHAR
        // for the character half) - which `is_keyboard_message` matches, so with the full
        // map open the map branch was returning 1 for them. The consequence was not just
        // "alt+F4 does nothing": WM_CLOSE never arrived, so `crumb::mark_closing()` never
        // ran and the NEXT launch reported the clean exit as a crash.
        bool is_system_chord(UINT msg, WPARAM wparam)
        {
            if (msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP && msg != WM_SYSCHAR)
            {
                return false;
            }
            return wparam == VK_F4 || wparam == VK_RETURN;
        }

        // RAW INPUT. UE reads the mouse through WM_INPUT, not only through WM_MOUSEMOVE,
        // so swallowing the window messages alone still lets the camera turn under an
        // open overlay. One RID_HEADER read says which device a message came from, which
        // is what lets the panel take the mouse and leave the keyboard with the game.
        //

        RawKind raw_kind(UINT msg, LPARAM lparam, bool want_key)
        {
            RawKind out{};
            if (msg != WM_INPUT)
            {
                return out;
            }
            RAWINPUTHEADER hdr{};
            UINT size = sizeof(hdr);
            const UINT got = ::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_HEADER, &hdr, &size,
                                               sizeof(RAWINPUTHEADER));
            if (got != sizeof(RAWINPUTHEADER))
            {
                return out;
            }
            out.mouse = hdr.dwType == RIM_TYPEMOUSE;
            out.keyboard = hdr.dwType == RIM_TYPEKEYBOARD;
            if (out.keyboard && want_key)
            {
                RAWINPUT ri{};
                UINT rsize = sizeof(ri);
                if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &ri, &rsize,
                                      sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                    ri.header.dwType == RIM_TYPEKEYBOARD)
                {
                    out.escape = ri.data.keyboard.VKey == VK_ESCAPE;
                }
            }
            return out;
        }

        bool is_raw_mouse_message(UINT msg, LPARAM lparam)
        {
            return raw_kind(msg, lparam, false).mouse;
        }

        //==============================================================================
        // The game thread's window messages, REPLAYED on the render thread
        //==============================================================================
        //
        // WHY THIS EXISTS AT ALL. `hooked_wndproc` runs on the thread that owns the
        // game's window - the GAME thread - and it used to hand every message straight
        // to `ImGui_ImplWin32_WndProcHandler`. That function mutates the Dear ImGui
        // context: `io.AddKeyEvent` / `AddMousePosEvent` / `AddMouseButtonEvent` all
        // push onto `ImGuiContext::InputEventsQueue`, which is an `ImVector` - a raw
        // pointer, a Size and a Capacity, grown with a realloc and no synchronisation
        // whatsoever. Meanwhile the RENDER thread is inside `ImGui::NewFrame()`, whose
        // `UpdateInputEvents` READS that queue and then `resize(0)`s it, and inside
        // `build_ui()` / `ImGui::Render()`, which read and write the rest of the same
        // context.
        //
        // So two threads were growing and clearing one ImVector. The failure that
        // follows is not a torn read: `push_back` on a stale `Data` pointer writes into
        // a block the other thread has just freed, and a `Size++` that races a
        // `resize(0)` writes one element PAST the capacity. Both land in the CRT heap,
        // and a corrupted free list is a hard, dumpless hang - every thread that then
        // allocates blocks inside the heap lock for ever. That is the exact shape of the
        // 2026-09-03 20:56 freeze: the render thread stopped within a second, the game
        // thread with it, no crash dump, no exception, and the loop thread lived just
        // long enough to flush a log buffer that needed no allocation.
        //
        // THE FIX IS THE ONLY CORRECT ONE: exactly one thread may touch the ImGui
        // context, and that thread is the one that renders. The WndProc hook now only
        // RECORDS the message into a fixed-size ring - no allocation, no ImGui call, a
        // few instructions under a spinlock the render thread holds only for the length
        // of a memcpy - and `replay_imgui_messages()` feeds them to the backend at the
        // top of the frame, before `ImGui_ImplWin32_NewFrame()`. The swallow decision
        // stays on the game thread and no longer reads the context: `WantCaptureKeyboard`
        // is published to an atomic once per frame.
        //
        // WHAT THIS COSTS. Three things in the backend's handler are thread-affine and
        // now answer differently, all of them cosmetic:
        //   * `::GetMessageExtraInfo()` is per-thread and only meaningful while that
        //     thread is dispatching, so every mouse event is reported as a MOUSE rather
        //     than as a pen or a touch. This mod has no pen or touch behaviour.
        //   * `::SetCapture()` / `::GetCapture()` fail on a window owned by another
        //     thread, so a drag that leaves the client area stops being tracked. The
        //     game runs fullscreen and the panel is drawn inside it, so the cursor
        //     cannot leave the client area in the first place.
        //   * `::TrackMouseEvent()` may refuse, in which case WM_MOUSELEAVE never
        //     arrives - but the mouse position keeps coming from the replayed
        //     WM_MOUSEMOVE messages, which is where it comes from today.
        // A one-frame delay on input is the other cost, and it is not observable: the
        // messages are replayed in order, in the same frame the game thread's own
        // dispatch would have been drawn.




        // Does the backend's handler do anything with this message? Recording only what
        // it handles keeps the ring from filling with WM_TIMER / WM_PAINT traffic. The
        // list is `ImGui_ImplWin32_WndProcHandlerEx`'s switch, verbatim.
        bool imgui_handles(UINT msg)
        {
            switch (msg)
            {
            case WM_MOUSEMOVE:
            case WM_NCMOUSEMOVE:
            case WM_MOUSELEAVE:
            case WM_NCMOUSELEAVE:
            case WM_DESTROY:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            case WM_XBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
            case WM_INPUTLANGCHANGE:
            case WM_CHAR:
            case WM_IME_COMPOSITION:
            case WM_IME_CHAR:
            case WM_SETCURSOR:
            case WM_DEVICECHANGE:
                return true;
            default:
                return false;
            }
        }

        // GAME THREAD (the window's owner). Nothing but a bounds check and a 24-byte
        // copy under a spinlock that is never held across anything that can block.
        void record_imgui_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            if (!imgui_handles(msg))
            {
                return;
            }
            spin::SpinGuard guard(g_msg_lock);
            if (g_msg_count >= kMsgRing)
            {
                g_msg_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_msg_ring[(g_msg_head + g_msg_count) % kMsgRing] = PendingMsg{hwnd, msg, wparam, lparam};
            ++g_msg_count;
        }

        // RENDER THREAD, at the top of the frame and BEFORE ImGui_ImplWin32_NewFrame.
        // Bounded by the count read at entry, so a game thread that keeps posting
        // cannot keep this loop alive.
        void replay_imgui_messages()
        {
            int budget = 0;
            {
                spin::SpinGuard guard(g_msg_lock);
                budget = g_msg_count;
            }
            for (int i = 0; i < budget; ++i)
            {
                PendingMsg m{};
                {
                    spin::SpinGuard guard(g_msg_lock);
                    if (g_msg_count == 0)
                    {
                        break;
                    }
                    m = g_msg_ring[g_msg_head];
                    g_msg_head = (g_msg_head + 1) % kMsgRing;
                    --g_msg_count;
                }
                ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wparam, m.lparam);
            }
        }

        LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            // ALT+F4 / the close button: write the terminal crash-breadcrumb stage while
            // there is still a process to write it from. None of the mod's teardown paths
            // run on this route (see the comment on DllMain in dllmain.cpp), so without
            // this every ALT+F4 made the NEXT launch report a crash. Idempotent, and it
            // never swallows the message.
            if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT)
            {
                crumb::mark_closing();
            }
            if (g_imgui_ready)
            {
                // RECORD ONLY. The ImGui context belongs to the render thread; see the
                // comment on record_imgui_message above for what happened when this
                // line called into the backend from here.
                record_imgui_message(hwnd, msg, wparam, lparam);

                // Input is only ever taken away from the game while the F2 panel or
                // the full map is up. With both closed the minimap is a pure overlay
                // and every message goes straight through, so gameplay input is
                // untouched - and because the test is a plain read of the two flags,
                // closing either one hands the input back on the very next message.
                // NOTHING IS LATCHED HERE (lessons.md).
                // ALT+F4 and ALT+ENTER go to the game whatever is open on top of it.
                const bool sys_chord = is_system_chord(msg, wparam);
                if (sys_chord)
                {
                    // Nothing is swallowed and nothing is closed: the game decides. The
                    // message was recorded for ImGui above, which is harmless - the
                    // backend only turns it into a key event.
                }
                else if (mm::g_map_open.load(std::memory_order_relaxed))
                {
                    // The map owns the whole keyboard and mouse: WASD pans it, and a
                    // click on a marker must not also swing the camera. io.WantCapture*
                    // is not enough - it is only true over an ImGui window, and the
                    // canvas deliberately reads raw keys rather than focusing a widget.
                    // The map's own toggle key is sampled with GetAsyncKeyState on the
                    // loop thread, so it still closes the map from here.
                    // ESC. The map already swallows every key, so the game's pause menu
                    // never saw it; what was missing is the other half - the key that a
                    // player expects to CLOSE a full-screen overlay. Closing is a plain
                    // store on the same flag the swallow condition reads, so the input is
                    // back on the very next message (lessons.md: nothing latched here).
                    if (is_escape_key_down(msg, wparam))
                    {
                        mm::g_map_open.store(false);
                        MM_LOGVS(L"full map closed (Esc)");
                    }
                    if (is_mouse_message(msg) || is_keyboard_message(msg) || msg == WM_INPUT)
                    {
                        return 1;
                    }
                }
                else if (mm::g_panel_open.load(std::memory_order_relaxed))
                {
                    // THE PANEL OWNS THE MOUSE, ALL OF IT. io.WantCaptureMouse is only
                    // true over an ImGui window, so with it as the gate every drag that
                    // started a pixel outside the panel turned the game camera while the
                    // player was reading the settings. Raw mouse input is swallowed for
                    // the same reason. The keyboard still goes to the game except while
                    // ImGui wants it (a text field), so the panel key - sampled with
                    // GetAsyncKeyState on the loop thread - always closes it again.
                    // Published by the render thread at the end of its frame - never
                    // read off the context from this thread (see record_imgui_message).
                    const bool want_keys = g_imgui_want_keyboard.load(std::memory_order_relaxed);
                    const bool capturing = mm::g_key_capture.load(std::memory_order_relaxed);

                    // ESC CLOSES THE PANEL, AND THE GAME MUST NOT SEE IT. Without this
                    // the one key everybody presses to dismiss a settings window opened
                    // the game's pause menu on top of it. Esc is therefore swallowed in
                    // all four shapes it can arrive in - WM_KEYDOWN / WM_KEYUP / WM_CHAR
                    // and a raw-input keyboard packet - and the key-DOWN closes the panel.
                    //
                    // While a binding capture is armed, Esc keeps its existing meaning
                    // (cancel the capture, handled on the render thread) and the panel
                    // stays open; the capture already swallows the whole keyboard.
                    const RawKind raw = raw_kind(msg, lparam, true);
                    const bool esc = is_escape_message(msg, wparam) || raw.escape;
                    if (esc && !capturing && is_escape_key_down(msg, wparam))
                    {
                        mm::g_panel_open.store(false);
                        MM_LOGVS(L"settings panel closed (Esc)");
                    }
                    if (is_mouse_message(msg) || msg == WM_SETCURSOR || raw.mouse || esc ||
                        ((want_keys || capturing) && is_keyboard_message(msg)) ||
                        (capturing && msg == WM_INPUT))
                    {
                        return 1;
                    }
                }
                // [fix-ui] hotkey swallow - one atomic read; see g_swallow_bits above.
                if (is_hotkey_message(msg) && hotkey_swallow(wparam))
                {
                    return 1;
                }
            }
            const WNDPROC prev = g_prev_wndproc.load(std::memory_order_acquire);
            if (prev == nullptr)
            {
                return ::DefWindowProcW(hwnd, msg, wparam, lparam);
            }
            return ::CallWindowProcW(prev, hwnd, msg, wparam, lparam);
        }

        // INSTALL. Refuses to hook a window that is ALREADY ours: after a re-adoption
        // (a lost device, a replaced swapchain) `ensure_initialised` runs again, and
        // saving our own proc as the "previous" one would make `hooked_wndproc` call
        // itself for ever on the first message.
        void hook_wndproc()
        {
            if (g_hwnd == nullptr)
            {
                return;
            }
            const auto current = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
            if (current == &hooked_wndproc)
            {
                mm::log(L"the window proc is still ours from an earlier init - not hooking it twice");
                return;
            }
            const auto prev = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hooked_wndproc)));
            g_prev_wndproc.store(prev, std::memory_order_release);
            if (prev == nullptr)
            {
                mm::logf(L"SetWindowLongPtr(GWLP_WNDPROC) failed (error {}) - the F2 panel will get no mouse input",
                         static_cast<unsigned>(::GetLastError()));
            }
        }

        // REMOVE, but only if we are still the outermost proc. Restoring the saved
        // pointer unconditionally UNINSTALLS whoever subclassed the window after us
        // (ReShade, the Steam overlay), which is somebody else's overlay disappearing
        // with no diagnostic. If we are no longer outermost the chain is left exactly as
        // it is: `hooked_wndproc` keeps working because it does nothing at all once
        // `g_imgui_ready` is false, and the saved pointer is deliberately kept so a call
        // still inside it has something to chain to.
        void unhook_wndproc()
        {
            const WNDPROC prev = g_prev_wndproc.load(std::memory_order_acquire);
            if (g_hwnd == nullptr || prev == nullptr)
            {
                return;
            }
            const auto current = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
            if (current != &hooked_wndproc)
            {
                mm::log(L"window proc: somebody else subclassed the window after us, so ours is left in "
                        L"the chain (removing it would uninstall theirs)");
                return;
            }
            ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
        }

        //==============================================================================
        // Map texture upload
        //==============================================================================

        void destroy_texture(MapTexture& t)
        {
            if (t.srv_cpu.ptr != 0)
            {
                g_srv_heap.free(t.srv_cpu);
                t.srv_cpu = D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            safe_release(t.tex);
            safe_release(t.upload);
            t = MapTexture{};
        }

        void destroy_slice_set(SliceBuf* bufs, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (b.upload != nullptr && b.mapped != nullptr)
                {
                    b.upload->Unmap(0, nullptr);
                }
                if (b.srv_cpu.ptr != 0)
                {
                    g_srv_heap.free(b.srv_cpu);
                }
                safe_release(b.tex);
                safe_release(b.upload);
                b = SliceBuf{};
            }
        }

        // RENDER THREAD, and only with the slicer paused.
        void destroy_slice_buffers()
        {
            destroy_slice_set(g_slice, kSliceBufs);
            for (int i = 0; i < kSliceBufs; ++i)
            {
                g_slice_copy_pending[i].store(false);
                g_slice_in_flight[i].store(0);
            }
            g_slice_size = 0;
            g_slice_next = 0;
            clear_slice_view();
            g_slice_last_ms = 0;
            note_slice_buffers_changed();
        }

        // RENDER THREAD, and only with the slicer paused.
        void destroy_map_slice_buffers()
        {
            destroy_slice_set(g_mslice, kMapSliceBufs);
            for (int i = 0; i < kMapSliceBufs; ++i)
            {
                g_mslice_copy_pending[i].store(false);
                g_mslice_in_flight[i].store(0);
            }
            g_mslice_next = 0;
            clear_map_slice_view();
            g_mslice_last_ms = 0;
            g_mr_valid = false;
            note_slice_buffers_changed();
        }

        // RENDER THREAD. Every caller must already have the slicer paused; the two
        // that matter (the F5 texture drop and shutdown_render) do it around
        // wait_for_gpu() as well, so the GPU is idle AND the loop thread is out.
        void destroy_all_map_textures()
        {
            destroy_texture(g_map);
            destroy_slice_buffers();
            destroy_map_slice_buffers();
            g_feet_z_valid = false;
            g_slice_scratch.clear();
            // g_mslice_scratch belongs to the loop thread now, but the slicer is paused
            // here, so clearing it is safe and keeps the memory from a closed map.
            g_mslice_scratch.clear();
            g_slice_want_px.store(0, std::memory_order_relaxed);
            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
            }
        }

        // An upload buffer only has to live until the GPU has run the copy. Releasing
        // them matters now that a chapter uploads ten pictures: keeping them would park
        // ~100 MB of CPU-visible memory for the whole session.
        void release_finished_uploads()
        {
            if (g_fence == nullptr)
            {
                return;
            }
            const UINT64 done = g_fence->GetCompletedValue();
            const auto sweep = [done](MapTexture& t) {
                if (t.upload != nullptr && t.upload_fence != 0 && done >= t.upload_fence)
                {
                    safe_release(t.upload);
                }
            };
            sweep(g_map);
        }

        // Creates the texture and its upload buffer, and records the copy into
        // `list`. Called with the render lock held, from inside a frame.
        bool begin_map_upload(const mapdata::PendingImage& img, ID3D12GraphicsCommandList* list)
        {
            MapTexture& target = g_map;
            const DXGI_FORMAT format = img.channels == 1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            destroy_texture(target);

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(img.width);
            desc.Height = static_cast<UINT>(img.height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&target.tex))))
            {
                mm::logf(L"map texture: CreateCommittedResource({}x{}) failed", img.width, img.height);
                return false;
            }

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total = 0;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.Format = DXGI_FORMAT_UNKNOWN;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&target.upload))))
            {
                mm::logf(L"map texture: upload buffer of {} MB failed", total / (1024 * 1024));
                destroy_texture(target);
                return false;
            }

            void* mapped = nullptr;
            D3D12_RANGE none{0, 0};
            if (FAILED(target.upload->Map(0, &none, &mapped)) || mapped == nullptr)
            {
                mm::log(L"map texture: Map() of the upload buffer failed");
                destroy_texture(target);
                return false;
            }
            const std::size_t src_pitch = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.channels);
            for (UINT row = 0; row < num_rows; ++row)
            {
                std::memcpy(static_cast<std::uint8_t*>(mapped) + layout.Offset + row * layout.Footprint.RowPitch,
                            img.pixels.data() + row * src_pitch,
                            src_pitch);
            }
            target.upload->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = target.tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layout;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.tex;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);

            if (!g_srv_heap.alloc(target.srv_cpu, target.srv_gpu))
            {
                mm::log(L"map texture: no free SRV descriptor");
                destroy_texture(target);
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            // A layer is a single-channel coverage mask, so R is mapped to all four
            // components: ImGui's pixel shader then computes tint * (r, r, r, r), i.e.
            // "the floor colour, at this pixel's coverage". That is what lets the
            // runtime recolour a floor (bright for the current one, dim for the storey
            // below) and what makes an R8 texture - a quarter of the memory of RGBA8 -
            // enough for a full-resolution layer.
            srv.Shader4ComponentMapping =
                img.channels == 1 ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0)
                                  : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(target.tex, &srv, target.srv_cpu);

            target.width = img.width;
            target.height = img.height;
            target.channels = img.channels;
            target.bytes = static_cast<std::size_t>(total);
            target.chapter = img.chapter_key;
            target.ready = true;
            // The fence this frame will signal at the end of Present; once the GPU has
            // passed it, release_finished_uploads() frees the staging buffer.
            target.upload_fence = g_fence_value + 1;

            mm::logf(L"map texture: composite {}x{} uploaded (RGBA8, {} MB), chapter \"{}\"",
                     img.width,
                     img.height,
                     total / (1024 * 1024),
                     std::wstring(img.chapter_key.begin(), img.chapter_key.end()));
            return true;
        }

        //==============================================================================
        // Drawing: the minimap
        //==============================================================================


        UvMap uv_of(const mapdata::Chapter& c)
        {
            return UvMap{c.min_y, c.max_x, c.px_per_uu, c.image_width, c.image_height};
        }


        //==============================================================================
        // WORLD -> MINIMAP OFFSET, and the edge clamp (review B.22)
        //==============================================================================
        //
        // Three copies of this arithmetic lived inside draw_minimap: one in draw_markers,
        // one in the found-ring projector and one in the waypoint block - the same
        // rotate-and-divide, then the same round-or-square limit test, then the same
        // scale-onto-the-rim. Three copies of a coordinate transform is three chances for
        // the waypoint to sit a pixel off the marker it was set on.
        //
        // The three callers differ only in what they want done when the point falls

        MiniOffset mini_offset(const MiniGeom& g, double wx, double wy, bool round, float limit,
                               bool clamp_to_edge)
        {
            const double wdx = wx - g.px;
            const double wdy = wy - g.py;
            const double zz = g.zoom > 0.0001f ? static_cast<double>(g.zoom) : 1.0;
            MiniOffset out{};
            // Screen up is the player's forward (rotate mode) or world +X (north-up);
            // see uv_at() below for the derivation of these two rows.
            out.dx = (-g.sin_yaw * wdx + g.cos_yaw * wdy) / zz;
            out.dy = (-g.cos_yaw * wdx - g.sin_yaw * wdy) / zz;
            const double lim = static_cast<double>(limit);
            if (round)
            {
                const double d2 = out.dx * out.dx + out.dy * out.dy;
                if (d2 <= lim * lim)
                {
                    out.visible = true;
                    return out;
                }
                const double d = std::sqrt(d2);
                if (!clamp_to_edge || d <= 0.0001)
                {
                    return out;
                }
                out.dx = out.dx * lim / d;
                out.dy = out.dy * lim / d;
            }
            else
            {
                if (std::abs(out.dx) <= lim && std::abs(out.dy) <= lim)
                {
                    out.visible = true;
                    return out;
                }
                if (!clamp_to_edge)
                {
                    return out;
                }
                const double sc = lim / (std::max)(std::abs(out.dx), std::abs(out.dy));
                out.dx *= sc;
                out.dy *= sc;
            }
            out.visible = true;
            out.clamped = true;
            return out;
        }

        // Screen offset (dx, dy) in minimap pixels -> texture uv.
        //
        // Screen up is the player's forward when rotate_with_player is on, and world +X
        // (north) otherwise; screen right is the corresponding right vector. In UE, for
        // yaw a, forward = (cos a, sin a) and right = (-sin a, cos a). So
        //     world = player + forward * (-dy * zoom) + right * (dx * zoom)
        // which for yaw = 0 reduces to (X - dy*zoom, Y + dx*zoom): screen right -> +Y
        // (east -> right in the image) and screen up -> +X (north -> up). That is
        // exactly render.py / build_map.py's north-up convention.
        ImVec2 uv_at(const MiniGeom& g, float dx, float dy)
        {
            const double fx = g.cos_yaw;
            const double fy = g.sin_yaw;
            const double rx = -g.sin_yaw;
            const double ry = g.cos_yaw;
            const double wx = g.px + fx * (-dy * g.zoom) + rx * (dx * g.zoom);
            const double wy = g.py + fy * (-dy * g.zoom) + ry * (dx * g.zoom);
            float u = 0.0f;
            float v = 0.0f;
            g.uv.to_uv(wx, wy, u, v);
            return ImVec2{u, v};
        }

        void add_image_circle(ImDrawList* dl, ImTextureRef tex, const MiniGeom& g, ImU32 col)
        {
            dl->PushTexture(tex);
            dl->PrimReserve(g_circle_segments * 3, g_circle_segments + 1);
            const unsigned int base = dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(g.center, uv_at(g, 0.0f, 0.0f), col);
            for (int i = 0; i < g_circle_segments; ++i)
            {
                const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(g_circle_segments);
                const float dx = std::cos(a) * g.half;
                const float dy = std::sin(a) * g.half;
                dl->PrimWriteVtx(ImVec2{g.center.x + dx, g.center.y + dy}, uv_at(g, dx, dy), col);
            }
            for (int i = 0; i < g_circle_segments; ++i)
            {
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + ((i + 1) % g_circle_segments)));
            }
            dl->PopTexture();
        }

        void add_player_arrow(ImDrawList* dl, ImVec2 c, float angle_deg, float size)
        {
            // angle measured clockwise from screen up.
            const float a = angle_deg * kPi / 180.0f;
            const float dirx = std::sin(a);
            const float diry = -std::cos(a);
            const float perpx = std::cos(a);
            const float perpy = std::sin(a);
            const ImVec2 tip{c.x + dirx * size, c.y + diry * size};
            const ImVec2 l{c.x - dirx * size * 0.62f + perpx * size * 0.60f,
                           c.y - diry * size * 0.62f + perpy * size * 0.60f};
            const ImVec2 r{c.x - dirx * size * 0.62f - perpx * size * 0.60f,
                           c.y - diry * size * 0.62f - perpy * size * 0.60f};
            dl->AddTriangleFilled(tip, l, r, IM_COL32(255, 226, 92, 255));
            dl->AddTriangle(tip, l, r, IM_COL32(30, 26, 10, 220), 1.6f);
        }



        //==============================================================================
        // The height slicer
        //==============================================================================
        //
        // WHY THIS REPLACED THE ORDINAL LAYERS (2026-09-02, round 3 of the in-world
        // feedback). The map used to ship one pre-rendered texture per per-pixel
        // surface ORDINAL plus a 640-uu grid of surface bands, and the runtime guessed
        // which ordinal the player's storey was from that grid. The guess is genuinely
        // ambiguous - a band names up to three ordinals - so a temple interior drew
        // several layers blended together and read as noise, exactly the failure mode
        // `CURRENT.md` had written down as the trigger for this rewrite.
        //
        // Now the asset carries the ACTUAL Z of up to four stacked surfaces per pixel
        // (mapdata::HeightMaps) and this code answers the question exactly, per pixel:
        //
        //     |Z - feetZ| <= floor_z_tolerance          -> the floor I am on, opaque
        //     nearest surface below within floor_fade_uu -> dim  (adjacent_floor_opacity)
        //     nearest surface above within floor_fade_uu -> faint (x 0.6)
        //     nothing                                    -> transparent
        //
        // and shades each pixel by (surfaceZ - feetZ) so slopes and staircases inside
        // one storey read as a gentle gradient instead of a flat silhouette. There are
        // no floor ranks, no bands and no per-position grid lookup left - the only
        // hysteresis is the EMA on feetZ.
        //
        // It runs on the CPU, at slice_hz, over only the window the minimap can show
        // (a ~512x512 source region), and uploads that window into a small dynamic
        // texture. That buys the shader path's exact semantics without a custom root
        // signature / PSO / D3DCompile on a ReShade-wrapped DX12 swapchain - see
        // CURRENT.md § Decisions. The shader path stays documented as a later
        // optimisation: it would move this loop to the GPU and drop the 512x512 upload.


        // Creates `count` dynamic RGBA textures of w x h with a persistently mapped
        // upload heap each. Shared by the minimap (square) and the full map
        // (rectangular): the resources, the barriers and the copy are identical, only
        // the size and the update policy differ.
        bool create_slice_set(SliceBuf* bufs, int count, int w, int h, const wchar_t* what)
        {
            destroy_slice_set(bufs, count);
            if (g_device == nullptr || w <= 0 || h <= 0)
            {
                return false;
            }

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(w);
            desc.Height = static_cast<UINT>(h);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total = 0;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.Format = DXGI_FORMAT_UNKNOWN;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&b.tex))))
                {
                    mm::logf(L"slice ({}): CreateCommittedResource({}x{} RGBA) failed", what, w, h);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&b.upload))))
                {
                    mm::logf(L"slice ({}): upload buffer of {} KB failed", what, total / 1024);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                void* mapped = nullptr;
                D3D12_RANGE none{0, 0};
                if (FAILED(b.upload->Map(0, &none, &mapped)) || mapped == nullptr)
                {
                    mm::logf(L"slice ({}): Map() of the upload buffer failed", what);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                b.mapped = static_cast<std::uint8_t*>(mapped);
                if (!g_srv_heap.alloc(b.srv_cpu, b.srv_gpu))
                {
                    mm::logf(L"slice ({}): no free SRV descriptor", what);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(b.tex, &srv, b.srv_cpu);

                b.footprint = layout;
                b.rows = num_rows;
                b.w = w;
                b.h = h;
                b.in_copy_dest = true;
            }
            mm::logf(L"slice ({}): {} dynamic texture(s) of {}x{} RGBA created ({} KB each, {} KB of "
                     L"mapped upload memory)",
                     what,
                     count,
                     w,
                     h,
                     total / 1024,
                     (total * static_cast<UINT64>(count)) / 1024);
            return true;
        }

        // RENDER THREAD, and only with the slicer paused.
        bool create_slice_buffers(int size)
        {
            if (!create_slice_set(g_slice, kSliceBufs, size, size, L"minimap"))
            {
                destroy_slice_buffers();
                return false;
            }
            for (int i = 0; i < kSliceBufs; ++i)
            {
                g_slice_copy_pending[i].store(false);
                g_slice_in_flight[i].store(0);
            }
            g_slice_size = size;
            g_slice_next = 0;
            clear_slice_view();
            note_slice_buffers_changed();
            return true;
        }

        // How many source pixels the minimap can show, including the rotation corners
        // and a margin so the CLAMP sampler never smears an edge into view.
        int slice_size_for(const mm::Config& cfg, const mapdata::HeightMaps& hm, float half_px)
        {
            const double radius_uu = static_cast<double>(half_px) * static_cast<double>(cfg.zoom_uu_per_px) *
                                     1.4143; // the diagonal of the square the disc rotates in
            double want = 2.0 * radius_uu * hm.px_per_uu + 16.0;
            int size = static_cast<int>(std::ceil(want / 128.0)) * 128;
            if (size < kSliceMinPx)
            {
                size = kSliceMinPx;
            }
            if (size > kSliceMaxPx)
            {
                size = kSliceMaxPx;
            }
            return size;
        }

        // One pixel's colour, from the up-to-four surface Z values under it.
        //
        // `state` 3 = the floor the player is on, 2 = the nearest surface below,
        // 1 = the nearest above, 0 = nothing. The gradient is the SAME rule offline

        // Fills `dst` (size*size RGBA8, row pitch `pitch`) with the window whose
        // top-left source pixel is (x0, y0).
        //
        // PLANE-MAJOR, deliberately. The obvious pixel-major loop reads all eight
        // planes at one pixel before moving on - eight addresses ~43 MB apart, i.e. one
        // cache miss per plane per pixel, ~2.1 M misses for a 512x512 window. Walking
        // one plane's window to completion instead touches 512 contiguous uint16 per
        // row, so the whole pass is ~65 k cache lines: the same arithmetic, ~30x fewer
        // misses. The per-pixel decision state lives in ~1.3 MB of scratch, which fits
        // in L2/L3.
        // `sx0` / `sy0` are the source pixel of the destination's top-left CORNER and
        // `src_step` is how many source pixels one destination pixel advances - 1.0 for
        // the minimap (which shows the asset at its own resolution) and > 1 for the full
        // map, which decimates. Sampling is nearest, at the destination pixel's centre;
        // at a decimating step a 1-px corridor can drop out, but at that zoom it is
        // sub-pixel anyway, and the alternative (scanning every source pixel of the
        // region) is ~30x the work for a picture nobody can resolve.
        void slice_region(const mapdata::HeightMaps& hm, double sx0, double sy0, double src_step, int w, int h,
                          std::uint8_t* dst, UINT pitch, float feet, const SliceStyle& st, SliceScratch& sc,
                          SliceCounts& counts)
        {
            counts = SliceCounts{};

            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            if (sc.state.size() != n)
            {
                sc.state.assign(n, 0);
                sc.best_ad.assign(n, 0.0f);
                sc.best_d.assign(n, 0.0f);
            }
            else
            {
                std::memset(sc.state.data(), 0, n);
            }
            std::uint8_t* state = sc.state.data();
            float* best_ad = sc.best_ad.data();
            float* best_d = sc.best_d.data();

            // Destination -> source index, computed once instead of once per plane. It
            // is also where the bounds check lives: -1 means "outside the asset", which
            // comes out transparent.
            if (sc.col_x.size() != static_cast<std::size_t>(w))
            {
                sc.col_x.resize(static_cast<std::size_t>(w));
                sc.gather.resize(static_cast<std::size_t>(w));
            }
            if (sc.row_y.size() != static_cast<std::size_t>(h))
            {
                sc.row_y.resize(static_cast<std::size_t>(h));
            }
            int* col_x = sc.col_x.data();
            int* row_y = sc.row_y.data();
            for (int col = 0; col < w; ++col)
            {
                const int i = static_cast<int>(std::floor(sx0 + (static_cast<double>(col) + 0.5) * src_step));
                col_x[col] = (i >= 0 && i < hm.width) ? i : -1;
            }
            for (int row = 0; row < h; ++row)
            {
                const int i = static_cast<int>(std::floor(sy0 + (static_cast<double>(row) + 0.5) * src_step));
                row_y[row] = (i >= 0 && i < hm.height) ? i : -1;
            }

            const float z0 = hm.z_min;
            const float step = hm.z_step();
            const int planes = hm.count < mapdata::kMaxSurfaces ? hm.count : mapdata::kMaxSurfaces;
            counts.surfaces = planes;

            for (int k = 0; k < planes; ++k)
            {
                if (hm.plane_empty(k))
                {
                    continue;
                }
                std::uint16_t* gathered = sc.gather.data();
                for (int row = 0; row < h; ++row)
                {
                    const int sy = row_y[row];
                    if (sy < 0)
                    {
                        continue;
                    }
                    // Gather the row's codes out of the block store. `false` means
                    // no surface anywhere on this row of this plane, which in the
                    // deeper planes is most rows - and skipping them here is where the
                    // sparse store gives some of the RAM saving back as speed. Columns
                    // outside the asset (col_x < 0) and absent blocks both come back
                    // as code 0, which is exactly what the dense plane held there.
                    if (!hm.gather_row(k, sy, col_x, w, gathered))
                    {
                        continue;
                    }
                    const std::size_t out_base = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                    for (int col = 0; col < w; ++col)
                    {
                        const std::uint16_t code = gathered[col];
                        if (code == 0)
                        {
                            continue; // no surface in this slot here
                        }
                        const float z = z0 + (static_cast<float>(code) - 1.0f) * step;
                        const float d = z - feet;
                        const float ad = d < 0.0f ? -d : d;
                        std::uint8_t cand = 0;
                        if (ad <= st.tol)
                        {
                            cand = 3; // the floor I am standing on
                        }
                        else if (ad <= st.fade)
                        {
                            cand = d < 0.0f ? 2 : 1; // below / above, dimmed
                        }
                        else
                        {
                            continue;
                        }
                        const std::size_t i = out_base + static_cast<std::size_t>(col);
                        // Class first, then "nearest": exactly the offline rule in
                        // tools/navmesh/slice_preview.py (slice_window + shade).
                        if (cand > state[i] || (cand == state[i] && ad < best_ad[i]))
                        {
                            state[i] = cand;
                            best_ad[i] = ad;
                            best_d[i] = d;
                        }
                    }
                }
            }

            const std::uint8_t a_dim = static_cast<std::uint8_t>(st.a_dim * 255.0f + 0.5f);
            const std::uint8_t a_faint = static_cast<std::uint8_t>(st.a_faint * 255.0f + 0.5f);
            for (int row = 0; row < h; ++row)
            {
                std::uint8_t* out = dst + static_cast<std::size_t>(row) * pitch;
                const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                for (int col = 0; col < w; ++col)
                {
                    std::uint8_t* px = out + static_cast<std::size_t>(col) * 4;
                    const std::size_t i = base + static_cast<std::size_t>(col);
                    const std::uint8_t cls = state[i];
                    if (cls == 0)
                    {
                        px[0] = px[1] = px[2] = px[3] = 0;
                        continue;
                    }
                    // lum = 1 + strength * clamp(d / span, -1, +1), span = tol for my own
                    // floor and fade for the dimmed ones - so a ramp or a staircase
                    // inside one storey reads as a gentle gradient, and the dimmed
                    // storeys are shaded by how far away they are.
                    const float span = cls == 3 ? st.tol : st.fade;
                    float t = span > 0.0f ? best_d[i] / span : 0.0f;
                    t = t < -1.0f ? -1.0f : (t > 1.0f ? 1.0f : t);
                    const float lum = 1.0f + st.strength * t;
                    const auto ch = [lum](float v) {
                        const float x = v * lum + 0.5f;
                        return static_cast<std::uint8_t>(x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x));
                    };
                    px[0] = ch(st.base_r);
                    px[1] = ch(st.base_g);
                    px[2] = ch(st.base_b);
                    if (cls == 3)
                    {
                        px[3] = 255;
                        ++counts.opaque;
                    }
                    else if (cls == 2)
                    {
                        px[3] = a_dim;
                        ++counts.dim;
                    }
                    else
                    {
                        px[3] = a_faint;
                        ++counts.faint;
                    }
                }
            }
        }

        // The minimap's window: the asset at 1:1, at an integral source origin.
        void slice_window(const mapdata::HeightMaps& hm, int x0, int y0, int size, std::uint8_t* dst, UINT pitch,
                          float feet, const SliceStyle& st)
        {
            SliceCounts counts{};
            slice_region(hm, static_cast<double>(x0), static_cast<double>(y0), 1.0, size, size, dst, pitch, feet,
                         st, g_slice_scratch, counts);
            g_slice_opaque = counts.opaque;
            g_slice_dim = counts.dim;
            g_slice_faint = counts.faint;
            g_slice_surfaces = counts.surfaces;
        }

        // The slice style, built from the config. Both slicers use it and the loop
        // thread builds its own copy, so it lives in one place.
        SliceStyle style_from(const mm::Config& cfg)
        {
            SliceStyle st{};
            st.base_r = cfg.floor_base_r;
            st.base_g = cfg.floor_base_g;
            st.base_b = cfg.floor_base_b;
            st.strength = cfg.floor_gradient_strength;
            st.tol = cfg.floor_z_tolerance;
            st.fade = cfg.floor_fade_uu;
            st.a_dim = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity : 0.0f;
            st.a_faint = cfg.show_adjacent_floors ? cfg.adjacent_floor_opacity * 0.6f : 0.0f;
            return st;
        }

        // RENDER THREAD. Everything about the minimap slice that must happen inside the
        // frame: size the buffers (creation is the render thread's alone) and tell the
        // slicer the minimap is drawing and how big a window it needs. The CUT itself
        // runs on the loop thread - see slice_minimap_step().
        //
        // Returns true when a buffer is available to draw. The window carries margin,
        // so a cut that has not landed yet is invisible.
        bool plan_slice(const mm::Config& cfg, const mapdata::Chapter& ch, float half_px, std::uint64_t now)
        {
            if (!ch.has_heights())
            {
                g_slice_want_px.store(0, std::memory_order_relaxed);
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;

            const int want = slice_size_for(cfg, hm, half_px);
            if (want != g_slice_size)
            {
                // The buffers are the render thread's to allocate, and the slicer may be
                // writing into the old ones right now. If it will not stand down we
                // simply keep the current size for this frame.
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    wait_for_gpu(); // the old buffers may still be in flight
                    const bool ok = create_slice_buffers(want);
                    slicer_pause_end();
                    if (!ok)
                    {
                        g_slice_want_px.store(0, std::memory_order_relaxed);
                        return false;
                    }
                }
            }
            g_slice_want_px.store(g_slice_size, std::memory_order_relaxed);
            g_slice_want_ms.store(now, std::memory_order_relaxed);
            return slice_view().shown >= 0;
        }

        // LOOP THREAD. The actual cut: pace it, take the next buffer if the GPU is done
        // with it, fill the mapped upload heap and publish the result.
        //
        // The height planes are re-read from `mapdata` on EVERY call and never cached
        // across one: a chapter switch retires the old planes and frees them after a
        // grace period, so a pointer held from the previous slice could be freed memory.
        void slice_minimap_step(std::uint64_t now)
        {
            const int want = g_slice_want_px.load(std::memory_order_relaxed);
            if (want <= 0 || want != g_slice_size)
            {
                return; // the minimap is not drawing, or the render thread is resizing
            }
            // The minimap stopped drawing (the overlay hid, the map opened) and nobody
            // has asked since: stop cutting rather than burning a millisecond forever.
            if (now - g_slice_want_ms.load(std::memory_order_relaxed) > 500)
            {
                return;
            }

            mm::Snapshot snap{};
            if (!mm::read_snapshot(snap) || !snap.has_pawn)
            {
                return;
            }
            const mapdata::Chapter* ch = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (ch == nullptr || !ch->has_heights())
            {
                return;
            }
            const mapdata::HeightMaps& hm = *ch->heights;

            const mm::Config& cfg = mm::cfg_cached();

            // ---- feet Z, EMA-smoothed ------------------------------------------------
            const float raw_feet = static_cast<float>(snap.z) - cfg.player_z_offset;
            static std::uint64_t last_teleport = 0;
            const bool teleported = snap.teleport_ms != 0 && snap.teleport_ms != last_teleport;
            if (teleported)
            {
                last_teleport = snap.teleport_ms;
            }
            if (!g_feet_z_valid || teleported)
            {
                g_feet_z = raw_feet;
                g_feet_z_valid = true;
                g_slice_last_ms = 0; // a teleport must re-slice immediately
            }
            else
            {
                const float tau = cfg.feet_z_smooth_ms > 1 ? static_cast<float>(cfg.feet_z_smooth_ms) : 1.0f;
                // The loop runs at roughly frame rate; the exact dt does not matter for
                // a 100 ms EMA.
                const float a = 16.0f / tau;
                g_feet_z += (raw_feet - g_feet_z) * (a > 1.0f ? 1.0f : a);
            }

            const int period = cfg.slice_hz > 0 ? 1000 / cfg.slice_hz : 80;
            if (g_slice_last_ms != 0 && now - g_slice_last_ms < static_cast<std::uint64_t>(period))
            {
                return; // the previous window is still good enough
            }

            SliceBuf& b = g_slice[g_slice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0)
            {
                return;
            }
            const std::uint64_t in_flight = g_slice_in_flight[g_slice_next].load(std::memory_order_acquire);
            ID3D12Fence* fence = g_fence;
            if (in_flight != 0 && fence != nullptr && fence->GetCompletedValue() < in_flight)
            {
                // The GPU is still sampling this one. Never stall for the map: keep
                // showing the other buffer and try again on the next loop iteration.
                ++g_slice_skipped;
                return;
            }

            double pxc = 0.0;
            double pyc = 0.0;
            hm.to_px(snap.x, snap.y, pxc, pyc);
            const int x0 = static_cast<int>(std::lround(pxc)) - b.w / 2;
            const int y0 = static_cast<int>(std::lround(pyc)) - b.w / 2;

            const SliceStyle st = style_from(cfg);

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            slice_window(hm, x0, y0, b.w, b.mapped + b.footprint.Offset, b.footprint.Footprint.RowPitch,
                         g_feet_z, st);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            double g_slice_last_cut_ms = 0.0;
            if (freq > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq);
                g_slice_last_cut_ms = ms;
                g_slice_ms = g_slice_ms == 0.0 ? ms : g_slice_ms * 0.8 + ms * 0.2;
                if (ms > g_slice_ms_peak)
                {
                    g_slice_ms_peak = ms;
                }
            }

            if (g_pf_slice < 0)
            {
                g_pf_slice = mm::perf_register("minimap slice cut", perf::Thread::Loop);
            }
            mm::perf_record_ms(g_pf_slice, g_slice_last_cut_ms);

            g_slice_copy_pending[g_slice_next].store(true, std::memory_order_release);
            {
                spin::SpinGuard guard(g_slice_view_lock);
                g_slice_view.shown = g_slice_next;
                // The window's own world -> pixel mapping (see mapdata::HeightMaps::to_px):
                //   px_local = px - x0 = (Y - (min_y + x0/s)) * s
                //   py_local = py - y0 = ((max_x - y0/s) - X) * s
                g_slice_view.px_per_uu = hm.px_per_uu;
                g_slice_view.min_y = hm.min_y + static_cast<double>(x0) / hm.px_per_uu;
                g_slice_view.max_x = hm.max_x - static_cast<double>(y0) / hm.px_per_uu;
            }
            g_slice_next = (g_slice_next + 1) % kSliceBufs;
            g_slice_last_ms = now;
            {
                // The first slice is the stage where a map asset, the CPU slicer and a
                // D3D12 upload heap are all live at once - i.e. the first moment a bad
                // manifest or a bad buffer would take the process down.
                static bool first = true;
                if (first)
                {
                    first = false;
                    crumb::stage(crumb::kFirstSlice);
                }
            }
        }

        // MAIN-MENU SELF-TEST. The slicer only ever runs inside draw_minimap, which is
        // gated on a gameplay pawn - so at the main menu neither the D3D12 resources nor
        // the CPU loop is exercised, and a verification run there could only say "it did
        // not crash". This allocates the buffers and slices one window at the chapter's
        // centre so a Lobby log line proves the whole path: texture + mapped upload heap
        // created, the loop ran, and what it cost. It also removes the first-frame hitch
        // in-world, since the buffers already exist.
        // RENDER THREAD, during set-up. It creates the buffers and writes into one of
        // them, so the loop-thread slicer must be held off for its duration.
        void slice_selftest()
        {
            if (!slicer_pause_begin(kSlicerPauseMs))
            {
                return; // the next frame will try again
            }
            struct Resume
            {
                ~Resume()
                {
                    slicer_pause_end();
                }
            } resume;
            const mm::Config cfg = mm::config();
            std::vector<mapdata::Chapter> list = mapdata::chapters();
            const mapdata::Chapter* ch = nullptr;
            for (const mapdata::Chapter& c : list)
            {
                if (c.has_heights())
                {
                    ch = &c;
                    break;
                }
            }
            // Size it for the shipping window so the in-world path needs no realloc.
            const float side = (std::max)(72.0f, cfg.size_frac * static_cast<float>(g_height));
            const int size = ch != nullptr ? slice_size_for(cfg, *ch->heights, side * 0.5f) : 512;
            if (!create_slice_buffers(size))
            {
                return;
            }
            if (ch == nullptr)
            {
                mm::log(L"slice: self-test skipped - no chapter with height maps is loaded");
                return;
            }

            const mapdata::HeightMaps& hm = *ch->heights;
            SliceBuf& b = g_slice[0];
            SliceStyle st{};
            st.base_r = cfg.floor_base_r;
            st.base_g = cfg.floor_base_g;
            st.base_b = cfg.floor_base_b;
            st.strength = cfg.floor_gradient_strength;
            st.tol = cfg.floor_z_tolerance;
            st.fade = cfg.floor_fade_uu;
            st.a_dim = cfg.adjacent_floor_opacity;
            st.a_faint = cfg.adjacent_floor_opacity * 0.6f;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            // Slice a window that actually HAS geometry in it, at a feet Z taken from
            // that geometry - a window over empty map at the mid-Z of the chapter comes
            // out fully transparent and proves nothing about the colour path.
            const int wx0 = (hm.width - b.w) / 2;
            const int wy0 = (hm.height - b.w) / 2;
            float probe_z = (hm.z_min + hm.z_max) * 0.5f;
            int sx0 = wx0;
            int sy0 = wy0;
            {
                // The block store knows where its first lit pixel is, so this no
                // longer has to stride over a 43 MB dense plane to find one.
                int px = 0;
                int py = 0;
                std::uint16_t code = 0;
                if (hm.first_lit(0, px, py, code))
                {
                    probe_z = hm.decode(code);
                    sx0 = px - b.w / 2;
                    sy0 = py - b.w / 2;
                }
            }
            slice_window(hm, sx0, sy0, b.w, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, probe_z, st);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            const double ms = freq > 0 ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                             static_cast<double>(freq)
                                       : 0.0;
            // Deliberately NOT marked needs_copy: nothing may be drawn at the main menu.
            mm::logf(L"slice: self-test sliced a {}x{} window of \"{}\" at source ({}, {}), feet Z "
                     L"{:.0f}, over {} surface(s) in {:.2f} ms (opaque {}, dim {}, faint {}) - the CPU "
                     L"path and the dynamic texture both work",
                     b.w,
                     b.w,
                     std::wstring(ch->key.begin(), ch->key.end()),
                     sx0 + b.w / 2,
                     sy0 + b.w / 2,
                     static_cast<double>(probe_z),
                     hm.count,
                     ms,
                     g_slice_opaque,
                     g_slice_dim,
                     g_slice_faint);
            g_slice_opaque = 0;
            g_slice_dim = 0;
            g_slice_faint = 0;
        }

        // Render thread, inside a frame, after the command list has been reset: record
        // the copy for whichever buffer the CPU just filled.
        void record_slice_copies(ID3D12GraphicsCommandList* list, SliceBuf* bufs,
                                std::atomic<bool>* pending, std::atomic<std::uint64_t>* in_flight,
                                int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                // exchange, not load+store: the slicer may fill this buffer again the
                // instant we clear the flag, and that next fill must not be lost.
                if (b.tex == nullptr || !pending[i].exchange(false))
                {
                    continue;
                }
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = b.tex;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (!b.in_copy_dest)
                {
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    list->ResourceBarrier(1, &barrier);
                }

                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = b.tex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = b.upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = b.footprint;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &barrier);
                b.in_copy_dest = false;
                // The upload heap this copy READS may not be rewritten until the GPU has
                // run it, so claim the fence this frame is about to signal. Without this
                // the loop thread could refill the heap between the recording and the
                // execution of the copy (two buffers, and a stalled frame is all it
                // takes) and the texture would show a window the mapping does not
                // describe. The stamp for a buffer that is merely being SAMPLED happens
                // at the end of the frame, alongside the fence signal.
                in_flight[i].store(g_fence_value + 1, std::memory_order_release);
            }
        }

        void record_slice_copy(ID3D12GraphicsCommandList* list)
        {
            record_slice_copies(list, g_slice, g_slice_copy_pending, g_slice_in_flight, kSliceBufs);
            record_slice_copies(list, g_mslice, g_mslice_copy_pending, g_mslice_in_flight, kMapSliceBufs);
        }

        //==============================================================================
        // Drawing one image (the composite or one floor layer)
        //==============================================================================

        void draw_srv(ImDrawList* dl, D3D12_GPU_DESCRIPTOR_HANDLE srv, const UvMap& uv, MiniGeom g, ImU32 col,
                      bool round, float x0, float y0, float side)
        {
            if (srv.ptr == 0)
            {
                return;
            }
            g.uv = uv;
            const ImTextureRef tex{static_cast<ImTextureID>(srv.ptr)};
            if (round)
            {
                add_image_circle(dl, tex, g, col);
                return;
            }
            const ImVec2 p0{x0, y0};
            const ImVec2 p1{x0 + side, y0 + side};
            dl->AddImageQuad(tex,
                             p0,
                             ImVec2{p1.x, p0.y},
                             p1,
                             ImVec2{p0.x, p1.y},
                             uv_at(g, -g.half, -g.half),
                             uv_at(g, g.half, -g.half),
                             uv_at(g, g.half, g.half),
                             uv_at(g, -g.half, g.half),
                             col);
        }

        void draw_image(ImDrawList* dl, const MapTexture& t, const UvMap& uv, const MiniGeom& g, ImU32 col,
                        bool round, float x0, float y0, float side)
        {
            if (!t.ready)
            {
                return;
            }
            draw_srv(dl, t.srv_gpu, uv, g, col, round, x0, y0, side);
        }

        // The mod's own wide strings (the log is wide) rendered for ImGui, which is
        // UTF-8. Key names, chord names and stage names are pure ASCII, so this is a
        // cast per character - but it has to be an EXPLICIT one:
        // std::string(w.begin(), w.end()) compiles and warns (C4244), and this mod
        // ships warning-free.
        //
        // ONE converter. key_name_ascii() used to be a byte-for-byte copy of this with
        // `mm::key_name(vk)` inlined into it (review B.22).
        std::string wide_to_ascii(const std::wstring& wide)
        {
            std::string out;
            out.reserve(wide.size());
            for (const wchar_t c : wide)
            {
                out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return out;
        }

        // A binding's display name, modifier prefix included ("F2", "CTRL+M").
        std::string key_name_ascii(int binding)
        {
            return wide_to_ascii(mm::key_name(binding));
        }

        // THE KEY HINTS. Built from the CONFIG, never from the defaults, so a rebound key
        // is what the player is told - both in the F2 panel and along the bottom of the
        // full map. One builder, so the two can never drift apart.
        std::string bindings_hint(const mm::Config& cfg)
        {
            std::string s = std::format("{} panel   {} full map ({} recentres)   {} minimap zoom   "
                                        "{} reload",
                                        key_name_ascii(cfg.panel_key),
                                        key_name_ascii(cfg.map_key),
                                        key_name_ascii(cfg.map_recenter_key),
                                        key_name_ascii(cfg.zoom_key),
                                        key_name_ascii(cfg.reload_key));
            if (cfg.highlight_enabled)
            {
                s += std::format("   {} {} x-ray",
                                 cfg.highlight_mode == mm::HighlightMode::Hold ? "hold" : "press",
                                 key_name_ascii(cfg.highlight_key));
                if (cfg.highlight_gamepad &&
                    (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt))
                {
                    s += " (pad " +
                         wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt,
                                                          cfg.highlight_pad_rt)) +
                         ")";
                }
            }
            return s;
        }

        //==============================================================================
        // Drawing: markers
        //==============================================================================
        //
        // Glyphs are drawn with ImDrawList primitives, not from an image atlas: the mod
        // ships no marker art, there is no texture to keep in sync with the categories,
        // and a vector glyph stays sharp at any minimap size. Each category gets a
        // shape AND a colour, because a dimmed "found" marker loses most of its colour
        // contrast and the shape is what still tells it apart.
        //
        // World -> minimap pixels is the inverse of uv_at(): for yaw a,
        // forward = (cos a, sin a) and right = (-sin a, cos a), so
        //     wdx = -s*(dx*z) - c*(dy*z)
        //     wdy =  c*(dx*z) - s*(dy*z)
        // whose inverse (the matrix is a rotation, det = 1) is
        //     dx = (-s*wdx + c*wdy) / z
        //     dy = (-c*wdx - s*wdy) / z
        // At yaw 0 that is dx = wdy/z (east to the right) and dy = -wdx/z (north up),
        // i.e. exactly build_map.py's north-up convention.

        // Defined with the rest of the full map, below - the minimap draws the same
        // glyph, edge-clamped, so the two views agree on what a waypoint looks like.

        // THE LOOK, cached once per frame (build_ui) so nothing on a draw path has to
        // take a config copy to know what colour to be. Render thread only.
        //
        // `g_palette` is the marker hue set and `g_plate` the theme's dark label plate;
        // the theme's other colours are resolved into the ordinary colour config keys at
        // load time (mmstate.cpp's apply_theme_defaults), which is what lets an explicit
        // key in the file override a theme.

        // "THE HUD HAS BEEN ON SCREEN AT LEAST ONCE", published by the render thread the
        // first frame hud_gate() answers "yes" - i.e. the first frame with a validated


        // The hue table lives in the PURE header src/glyphs.hpp, next to the shape
        // table, because "no two categories share a shape and a colour" is a property
        // of the two together and is asserted offline.
        ImU32 marker_color(mdb::Cat cat, int alpha)
        {
            const mdb::Rgb c = gly::marker_rgb(cat, g_palette);
            return IM_COL32(c.r, c.g, c.b, alpha);
        }

        // The dark box behind a label, the compass strip and the waypoint's distance -
        // one colour, from the theme, instead of the same literal in five places.
        ImU32 plate_color(int alpha)
        {
            return IM_COL32(g_plate.r, g_plate.g, g_plate.b, alpha);
        }

        // Category colour, or the ITEM QUALITY colour when the caller has quality
        // colouring switched on and this marker actually has a tier.
        //
        // Tier 0 deliberately falls through to the category colour: it is what every
        // chest, every live-only actor and every ordinary consumable is, so a palette
        // that repainted it would recolour most of the screen to say nothing. See
        // mdb::Rarity for where the tiers and the default palette come from.
        ImU32 marker_color_q(mdb::Cat cat, std::uint8_t rarity, int alpha, bool use_rarity,
                             const mdb::Rgb* palette)
        {
            const int tier = mdb::rarity_clamp(static_cast<int>(rarity));
            if (!use_rarity || tier == 0 || palette == nullptr)
            {
                return marker_color(cat, alpha);
            }
            const mdb::Rgb& c = palette[tier];
            return IM_COL32(c.r, c.g, c.b, alpha);
        }

        // ONE glyph. `hollow` is what a FOUND marker is drawn as: dimming alone takes
        // the colour away first and leaves an unreadable grey blob, while an outline
        // keeps the shape - which is the half of the identity that survives at 6 px.
        //
        // Every glyph gets a dark halo first (the review's `AddCircleFilled(p, r + 1,
        // 0x000000_78)`), so a light glyph still has an edge against the parchment fill
        // and a dark one against a lit scene. Its alpha follows the glyph's, so a
        // faded-out marker does not leave a black dot behind.
        // THE CATEGORIES "found" MEANS SOMETHING FOR. Collecting a chest, a pickup or a

        void draw_marker_glyph(ImDrawList* dl, mdb::Cat cat, ImVec2 p, float r, ImU32 col, ImU32 edge,
                               bool hollow)
        {
            const int ca = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFFu);
            // THE HALO COVERS THE SHAPE (review B.17). It used to be a fixed r + 1
            // circle, which the chest's box CORNERS stuck out of - so the one glyph most
            // often drawn over a bright floor lost its edge exactly where its outline
            // turns. gly::shape_extent() is how far this shape actually reaches; 16
            // segments rather than 12, because a bigger circle shows its facets.
            const gly::Shape shape = gly::shape_of(cat);
            dl->AddCircleFilled(p, r * gly::shape_extent(shape) + 1.0f,
                                IM_COL32(0, 0, 0, (ca * 120) / 255), 16);

            // Below ~7 px the fine detail inside a glyph is a smudge rather than a
            // silhouette, so the three complex shapes have a simplified form.
            const bool simple = r < gly::kSimpleGlyphRadius;
            const float w = hollow ? 1.7f : 1.2f;
            // A filled shape when the marker is live, the same shape as an outline when
            // it is found. Both take the SAME geometry, so the two states are the same
            // glyph and nothing moves when one becomes the other.
            const auto ngon = [&](float rad, int n) {
                if (hollow)
                {
                    dl->AddNgon(p, rad, col, n, w);
                }
                else
                {
                    dl->AddNgonFilled(p, rad, col, n);
                    dl->AddNgon(p, rad, edge, n, w);
                }
            };
            const auto circle = [&](ImVec2 c, float rad, int n) {
                if (hollow)
                {
                    dl->AddCircle(c, rad, col, n, w);
                }
                else
                {
                    dl->AddCircleFilled(c, rad, col, n);
                    dl->AddCircle(c, rad, edge, n, w);
                }
            };
            const auto rect = [&](float hw, float hh) {
                const ImVec2 a{p.x - r * hw, p.y - r * hh};
                const ImVec2 b{p.x + r * hw, p.y + r * hh};
                if (hollow)
                {
                    dl->AddRect(a, b, col, 1.5f, 0, w);
                }
                else
                {
                    dl->AddRectFilled(a, b, col, 1.5f);
                    dl->AddRect(a, b, edge, 1.5f, 0, w);
                }
            };
            const auto tri = [&](float scale) {
                const ImVec2 a{p.x, p.y - r * scale};
                const ImVec2 b{p.x - r * scale * 0.92f, p.y + r * scale * 0.72f};
                const ImVec2 c{p.x + r * scale * 0.92f, p.y + r * scale * 0.72f};
                if (hollow)
                {
                    dl->AddTriangle(a, b, c, col, w);
                }
                else
                {
                    dl->AddTriangleFilled(a, b, c, col);
                    dl->AddTriangle(a, b, c, edge, w);
                }
            };
            // A pip is the dark centre that tells a shrine from a plain diamond. On a
            // hollow glyph it is drawn in the marker's own colour, because there is no
            // fill for it to contrast against.
            const auto pip = [&](float rad) {
                dl->AddCircleFilled(p, r * rad, hollow ? col : edge, 8);
            };

            switch (shape)
            {
            case gly::Shape::Diamond:
                // AddNgon starts at angle 0, so a 4-gon has its vertices on the axes.
                ngon(r * 1.15f, 4);
                pip(0.32f);
                break;
            case gly::Shape::ChestBox:
                rect(0.95f, 0.75f);
                dl->AddLine(ImVec2{p.x - r * 0.95f, p.y}, ImVec2{p.x + r * 0.95f, p.y},
                            hollow ? col : edge, w);
                break;
            case gly::Shape::Dot:
                circle(p, r * 0.72f, 10);
                break;
            case gly::Shape::Triangle:
                tri(1.5f);
                break;
            case gly::Shape::TriangleNotched:
                // The elite's triangle is smaller than the boss's AND wears a bar. The
                // scale difference alone was a three-pixel difference at markers_size
                // 6.5, which is what made the two read as one category.
                tri(1.15f);
                dl->AddLine(ImVec2{p.x - r * 0.62f, p.y + r * 0.30f},
                            ImVec2{p.x + r * 0.62f, p.y + r * 0.30f}, hollow ? col : edge, w + 0.3f);
                break;
            case gly::Shape::DotRing:
                // A small dot with a DETACHED ring: at glyph size the gap is what makes
                // it read as an enemy rather than as a pickup.
                dl->AddCircleFilled(p, r * 0.34f, col, 8);
                dl->AddCircle(p, r * 0.92f, col, 12, w);
                break;
            case gly::Shape::Pentagon:
                ngon(r * 1.05f, 5);
                break;
            case gly::Shape::NotePage:
            {
                // A page with its top-right corner folded away, plus two text rules.
                // The fold is what keeps it apart from the door's plain tall box at
                // glyph size: same family of silhouette, but one corner is missing and
                // the inside is not empty. Wider than the door (0.62 vs 0.55 half-
                // width) and shorter (0.88 vs 0.95) for the same reason.
                const float hw = r * 0.62f;
                const float hh = r * 0.88f;
                const float fold = r * 0.44f; // the 45-degree bite out of the corner
                const ImVec2 pts[5] = {
                    ImVec2{p.x - hw, p.y - hh},
                    ImVec2{p.x + hw - fold, p.y - hh},
                    ImVec2{p.x + hw, p.y - hh + fold},
                    ImVec2{p.x + hw, p.y + hh},
                    ImVec2{p.x - hw, p.y + hh},
                };
                if (hollow)
                {
                    dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, w);
                }
                else
                {
                    dl->AddConvexPolyFilled(pts, 5, col);
                    dl->AddPolyline(pts, 5, edge, ImDrawFlags_Closed, w);
                }
                // The fold itself: the two edges of the turned-down corner.
                const ImU32 ink = hollow ? col : edge;
                dl->AddLine(ImVec2{p.x + hw - fold, p.y - hh}, ImVec2{p.x + hw - fold, p.y - hh + fold},
                            ink, w);
                dl->AddLine(ImVec2{p.x + hw - fold, p.y - hh + fold}, ImVec2{p.x + hw, p.y - hh + fold},
                            ink, w);
                // Two rules of "writing", inset from the edges - dropped in the
                // simplified form, where they are 2 px apart inside a 5 px page and
                // fill it in. The folded corner is the identity and it survives.
                if (!simple)
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        const float y = p.y + r * (i == 0 ? 0.10f : 0.45f);
                        dl->AddLine(ImVec2{p.x - hw * 0.6f, y}, ImVec2{p.x + hw * 0.6f, y}, ink, w * 0.8f);
                    }
                }
                break;
            }
            case gly::Shape::DoorBox:
                rect(0.55f, 0.95f);
                break;
            case gly::Shape::Ladder:
                dl->AddLine(ImVec2{p.x - r * 0.5f, p.y - r}, ImVec2{p.x - r * 0.5f, p.y + r}, col, 1.6f);
                dl->AddLine(ImVec2{p.x + r * 0.5f, p.y - r}, ImVec2{p.x + r * 0.5f, p.y + r}, col, 1.6f);
                // SIMPLIFIED: one rung, not three. At r = 6.5 the three rungs are ~3.5
                // px apart and the 1.2 px lines merge into a filled box - which is the
                // chest's silhouette. One rung keeps the H that says "ladder".
                if (simple)
                {
                    dl->AddLine(ImVec2{p.x - r * 0.5f, p.y}, ImVec2{p.x + r * 0.5f, p.y}, col, 1.2f);
                }
                else
                {
                    for (int i = -1; i <= 1; ++i)
                    {
                        const float y = p.y + static_cast<float>(i) * r * 0.55f;
                        dl->AddLine(ImVec2{p.x - r * 0.5f, y}, ImVec2{p.x + r * 0.5f, y}, col, 1.2f);
                    }
                }
                break;
            case gly::Shape::Lift:
            {
                // SIMPLIFIED: a flatter platform and a taller, narrower arrow, drawn
                // FILLED even when the marker is found. The two shapes are 1 px apart at
                // r = 6.5, and an outlined arrow over an outlined box at that size is a
                // grey blob; the arrow is the whole difference from the chest's box, so
                // it is the part that must stay solid.
                const float box_hh = simple ? 0.36f : 0.5f;
                rect(0.85f, box_hh);
                const float tip = p.y - r * (simple ? 1.2f : 1.35f);
                const float base = p.y - r * (simple ? 0.5f : 0.6f);
                const float half_w = r * (simple ? 0.42f : 0.5f);
                if (hollow && !simple)
                {
                    dl->AddTriangle(ImVec2{p.x, tip}, ImVec2{p.x - half_w, base},
                                    ImVec2{p.x + half_w, base}, col, w);
                }
                else
                {
                    dl->AddTriangleFilled(ImVec2{p.x, tip}, ImVec2{p.x - half_w, base},
                                          ImVec2{p.x + half_w, base}, col);
                }
                break;
            }
            case gly::Shape::RingBar:
                dl->AddCircle(p, r, col, 14, 2.0f);
                dl->AddLine(ImVec2{p.x - r * 0.7f, p.y}, ImVec2{p.x + r * 0.7f, p.y}, col, 1.4f);
                break;
            case gly::Shape::Cross:
                // "X marks the spot" - and, unlike the unfilled 4-gon it replaces, it is
                // not a shrine with the fill switched off.
                dl->AddLine(ImVec2{p.x - r * 0.85f, p.y - r * 0.85f},
                            ImVec2{p.x + r * 0.85f, p.y + r * 0.85f}, col, 2.1f);
                dl->AddLine(ImVec2{p.x - r * 0.85f, p.y + r * 0.85f},
                            ImVec2{p.x + r * 0.85f, p.y - r * 0.85f}, col, 2.1f);
                break;
            case gly::Shape::SmallSquare:
            case gly::Shape::Count:
            default:
                rect(0.55f, 0.55f);
                break;
            }
        }



        //==============================================================================
        // ONE marker pass per frame
        //==============================================================================
        //
        // The minimap, the compass pips and the x-ray highlight each used to walk the
        // whole published marker buffer (700-3 600 entries) with its own loop and its own
        // sqrt. They ask different questions of the same rows, so the walk happens ONCE,
        // here, and each of them filters the result with its own rule.
        //
        // Distances are kept SQUARED: every consumer only needs them to compare and to
        // sort, and the two that want metres take the square root of the handful they
        // actually draw.



        //==============================================================================
        // Animation, toasts, and the "it just became found" event
        //==============================================================================
        //
        // All of it is RENDER-THREAD state derived from what the frame already has. None
        // of it costs the game thread anything, and - the rule that matters - none of it
        // can keep something on screen after the state that allows it went away:
        //
        //   * the HUD fade's TARGET is the hud_gate result. A target of 0 is applied
        //     instantly (hiding is immediate; the gate also stops the draw outright),
        //     and only showing is eased. That is the no-latch rule from lessons.md
        //     applied to an animation instead of to a condition.
        //   * a toast is a string plus a deadline; it says what just happened and then
        //     goes.
        //   * the found ring is driven by comparing the PUBLISHED found flags of the
        //     markers near the player between marker rounds - the game thread does no
        //     extra work for it, and nothing is remembered longer than one round.


        // Eases towards 1 while `target_on`, drops to 0 the instant it is false.
        float hud_fade_step(bool target_on, std::uint64_t now)
        {
            if (!target_on)
            {
                g_hud_fade = 0.0f;
                g_hud_fade_ms = 0;
                return 0.0f;
            }
            if (g_hud_fade_ms == 0)
            {
                g_hud_fade_ms = now;
            }
            const std::uint64_t age = now - g_hud_fade_ms;
            const float t = age >= kHudFadeMs ? 1.0f : static_cast<float>(age) / static_cast<float>(kHudFadeMs);
            // Smoothstep: a linear ramp on an alpha reads as a hard edge at both ends.
            g_hud_fade = t * t * (3.0f - 2.0f * t);
            return g_hud_fade;
        }


        void toast_for(const char* text, unsigned ms)
        {
            ::strncpy_s(g_toast, sizeof(g_toast), text, _TRUNCATE);
            g_toast_until = ::GetTickCount64() + ms;
        }

        void toast(const char* text)
        {
            toast_for(text, 1000);
        }

        //==============================================================================
        // Map -> clipboard
        //==============================================================================
        //
        // The picture we want is exactly what the player is looking at, so the source is
        // the BACK BUFFER, taken from the frame that has just been drawn - not a
        // re-render of the map into an offscreen target (which would need our own RTV,
        // our own pass and would then differ from the screen).
        //
        // THE SEQUENCE, and why it is spread over frames
        //   frame N   : after ImGui's draw call is recorded, the back buffer is
        //               transitioned RENDER_TARGET -> COPY_SOURCE, one
        //               CopyTextureRegion of the canvas rect is recorded into a readback
        //               buffer, and the fence value this frame will signal is remembered.
        //   frame N+k : once GetCompletedValue() has passed that fence the readback is
        //               mapped, unpacked to BGRA8 (clipimg - the back buffer is HDR10
        //               R10G10B10A2 on this game, not R8G8B8A8) and turned into a CF_DIB
        //               payload, which is handed to the LOOP thread.
        //   loop      : OpenClipboard / EmptyClipboard / SetClipboardData / CloseClipboard.
        //
        // Two rules from lessons.md are load-bearing: nothing is mapped before its fence
        // has passed (a readback read early is a garbage picture, not an error), and the
        // clipboard - which is a blocking, window-station-wide, message-pumping API - is
        // never touched from the render thread inside Present.
        //
        // No file is ever written.





        //==============================================================================
        // THE TOAST MAILBOX (review B.22)
        //==============================================================================
        //
        // One slot, one lock, and it is ITS OWN. post_toast() used to take the
        // SCREENSHOT's spinlock and write into a buffer that lived among the screenshot
        // state, so an unrelated notice ("first run: these are your keys", "minimap zoom
        // 26 uu/px") contended with a full-resolution DIB hand-off and read as part of
        // the clipboard machinery. They share nothing but a direction: loop -> render.
        //

        // ANY THREAD. Queues a toast for the render thread to draw.
        void post_toast(const char* text, unsigned ms)
        {
            {
                spin::SpinGuard guard(g_toast_lock);
                ::strncpy_s(g_toast_pending, sizeof(g_toast_pending), text, _TRUNCATE);
                g_toast_pending_ms = ms;
            }
            g_toast_pending_ready.store(true, std::memory_order_release);
        }

        void shot_fail(const char* why)
        {
            post_toast(why, 2500);
        }

        void shot_reset()
        {
            safe_release(g_shot_readback);
            g_shot_stage = ShotStage::Idle;
            g_shot_fence = 0;
            g_shot_w = 0;
            g_shot_h = 0;
            g_shot_pitch = 0;
        }

        // RENDER THREAD. Records the copy into the command list the frame is already
        // building, between ImGui's draw call and the transition back to PRESENT. The
        // back buffer is in RENDER_TARGET state on entry and is left in PRESENT state,
        // i.e. this REPLACES the caller's closing barrier when it returns true.
        bool record_shot_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, UINT index)
        {
            if (g_shot_stage != ShotStage::Idle || !g_shot_request.exchange(false, std::memory_order_acquire))
            {
                return false;
            }
            ID3D12Device* dev = g_device;
            if (dev == nullptr || backbuffer == nullptr)
            {
                shot_fail("screenshot: no device");
                return false;
            }
            const DXGI_FORMAT fmt = g_format;
            g_shot_fmt = clipimg::fmt_from_dxgi(static_cast<unsigned>(fmt));
            if (g_shot_fmt == clipimg::Fmt::Unknown)
            {
                shot_fail("screenshot: back buffer format not supported");
                mm::logf(L"screenshot: DXGI format {} is not one this build can unpack",
                         static_cast<int>(fmt));
                return false;
            }
            // The region: the map canvas as it was laid out this frame, clamped to the
            // back buffer. Without a canvas (the map is not open) there is nothing to
            // copy, and the hotkey should not have fired.
            if (!g_shot_canvas_valid)
            {
                shot_fail("screenshot: the map is not open");
                return false;
            }
            long x0 = static_cast<long>(g_shot_canvas.x0);
            long y0 = static_cast<long>(g_shot_canvas.y0);
            long x1 = static_cast<long>(g_shot_canvas.x1);
            long y1 = static_cast<long>(g_shot_canvas.y1);
            x0 = x0 < 0 ? 0 : x0;
            y0 = y0 < 0 ? 0 : y0;
            x1 = x1 > static_cast<long>(g_width) ? static_cast<long>(g_width) : x1;
            y1 = y1 > static_cast<long>(g_height) ? static_cast<long>(g_height) : y1;
            if (x1 - x0 < 16 || y1 - y0 < 16)
            {
                shot_fail("screenshot: the map canvas is too small");
                return false;
            }
            g_shot_w = static_cast<UINT>(x1 - x0);
            g_shot_h = static_cast<UINT>(y1 - y0);
            // A readback footprint's row pitch must be 256-aligned.
            g_shot_pitch = (g_shot_w * 4u + 255u) & ~255u;

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = static_cast<UINT64>(g_shot_pitch) * g_shot_h;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&g_shot_readback))))
            {
                shot_fail("screenshot: readback buffer allocation failed");
                shot_reset();
                return false;
            }

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = backbuffer;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_shot_readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = fmt;
            dst.PlacedFootprint.Footprint.Width = g_shot_w;
            dst.PlacedFootprint.Footprint.Height = g_shot_h;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = g_shot_pitch;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            D3D12_BOX box{};
            box.left = static_cast<UINT>(x0);
            box.top = static_cast<UINT>(y0);
            box.front = 0;
            box.right = static_cast<UINT>(x1);
            box.bottom = static_cast<UINT>(y1);
            box.back = 1;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &b);
            g_shot_stage = ShotStage::Recorded;
            (void)index;
            return true;
        }

        // RENDER THREAD, top of the frame. Once the GPU is past the fence, map the
        // readback and build the DIB.
        void shot_collect()
        {
            if (g_shot_stage_done.exchange(false, std::memory_order_acquire))
            {
                shot_reset(); // the loop thread is done with the bytes; re-arm
            }
            if (g_shot_stage != ShotStage::Recorded || g_shot_readback == nullptr || g_fence == nullptr)
            {
                return;
            }
            if (g_shot_fence == 0 || g_fence->GetCompletedValue() < g_shot_fence)
            {
                return;
            }
            void* mapped = nullptr;
            D3D12_RANGE range{};
            range.Begin = 0;
            range.End = static_cast<SIZE_T>(g_shot_pitch) * g_shot_h;
            if (FAILED(g_shot_readback->Map(0, &range, &mapped)) || mapped == nullptr)
            {
                shot_fail("screenshot: readback map failed");
                shot_reset();
                return;
            }
            // Unpack row by row into a tight BGRA image, then let clipimg flip it into
            // the bottom-up DIB the clipboard wants.
            std::vector<std::uint8_t> bgra(static_cast<std::size_t>(g_shot_w) * 4u * g_shot_h);
            bool ok = true;
            for (UINT y = 0; y < g_shot_h && ok; ++y)
            {
                const auto* srow = static_cast<const std::uint8_t*>(mapped) +
                                   static_cast<std::size_t>(y) * g_shot_pitch;
                ok = clipimg::unpack_row(g_shot_fmt, srow,
                                         bgra.data() + static_cast<std::size_t>(y) * g_shot_w * 4u,
                                         static_cast<int>(g_shot_w));
            }
            const D3D12_RANGE none{0, 0};
            g_shot_readback->Unmap(0, &none);
            if (!ok)
            {
                shot_fail("screenshot: pixel unpack failed");
                shot_reset();
                return;
            }
            std::vector<std::uint8_t> dib;
            if (!clipimg::build_dib(static_cast<int>(g_shot_w), static_cast<int>(g_shot_h), bgra.data(),
                                    static_cast<std::size_t>(g_shot_w) * 4u, dib))
            {
                shot_fail("screenshot: bitmap build failed");
                shot_reset();
                return;
            }
            {
                spin::SpinGuard guard(g_shot_lock);
                g_shot_dib = std::move(dib);
            }
            g_shot_dib_ready.store(true, std::memory_order_release);
            safe_release(g_shot_readback);
            g_shot_stage = ShotStage::Waiting;
        }

        //==============================================================================
        // The shrine list
        //==============================================================================
        //
        // Every shrine of the current chapter with its in-game name, its distance and
        // whether the save has lit it - and, behind `fast_travel_enabled`, a Travel
        // action per row.
        //
        // Sources, all published snapshots (render thread, no game-thread work):
        //   shr::table()  markers/shrines.json - the id, the localised name, the chapter,
        //                 the actor position and the game's own BirthPosition;
        //   shr::state()  the save's UnlockedFirepoints list, read raw at 1 Hz;
        //   snap          the pawn position, for the distance.
        //
        // Sorted by distance, because "which shrine is near me" is the question a player
        // asks; the chapter filter follows the marker filter so the list and the map
        // agree about what exists.


        void draw_shrine_list(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state,
                              int filter_chapter)
        {
            const std::vector<shdb::Shrine>* table = shr::table();
            if (table == nullptr || table->empty())
            {
                const shr::TableInfo info = shr::table_info();
                ImGui::TextDisabled("no shrine table: %s",
                                    info.error[0] != '\0' ? info.error : "markers\\shrines.json is empty");
                return;
            }
            const shr::State st = shr::state();

            struct Row
            {
                const shdb::Shrine* s;
                double dist;
                bool unlocked;
            };
            // REUSED FRAME TO FRAME (review B.18): the shrine window is open while the
            // player reads it, so this vector was allocated and freed on the render
            // thread at frame rate. Render thread only, like every other static in this
            // file. (`Shrine::label()` returns a reference and allocates nothing.)
            static std::vector<Row> rows;
            rows.clear();
            rows.reserve(table->size());
            for (const shdb::Shrine& sh : *table)
            {
                if (!sh.shrine || !sh.has_pos)
                {
                    continue; // a bossdoor_/Task pseudo-row is not a place
                }
                if (filter_chapter != chid::kNone && sh.chapter >= 0 && sh.chapter != filter_chapter)
                {
                    continue;
                }
                double d = -1.0;
                if (have_state)
                {
                    const double dx = sh.x - snap.x;
                    const double dy = sh.y - snap.y;
                    const double dz = sh.z - snap.z;
                    d = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                rows.push_back(Row{&sh, d, st.valid && shr::is_unlocked(sh.id.c_str())});
            }
            if (rows.empty())
            {
                ImGui::TextDisabled("no shrines in this chapter");
                return;
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if ((a.dist < 0.0) != (b.dist < 0.0))
                {
                    return b.dist < 0.0;
                }
                if (a.dist != b.dist)
                {
                    return a.dist < b.dist;
                }
                return a.s->id < b.s->id;
            });

            if (!st.valid)
            {
                ImGui::TextDisabled("unlocked state: n/a (%s)",
                                    st.route[0] != '\0' ? st.route : "not read yet");
            }
            const shr::TravelState tv = shr::travel_state();
            if (tv.phase == shr::Travel::Refused)
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "travel refused: %s", tv.note);
                ImGui::SameLine();
                if (ImGui::SmallButton("dismiss"))
                {
                    shr::clear_travel();
                }
            }

            if (!ImGui::BeginTable("shrines", cfg.fast_travel_enabled ? 5 : 4,
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f)))
            {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Shrine");
            ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Lit", ImGuiTableColumnFlags_WidthFixed);
            if (cfg.fast_travel_enabled)
            {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            }
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const Row& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const bool selected = ::strcmp(g_shrine_selected, r.s->id.c_str()) == 0;
                // One click sets a waypoint on it, a double-click centres the map there -
                // review item 1. Selectable, so the whole row is the hit target rather
                // than the eight characters of a name.
                if (ImGui::Selectable(r.s->label().c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick))
                {
                    ::strncpy_s(g_shrine_selected, sizeof(g_shrine_selected), r.s->id.c_str(),
                                _TRUNCATE);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        g_mv.cx = r.s->x;
                        g_mv.cy = r.s->y;
                        // The height slice is cut around the view centre, so a jump has
                        // to invalidate it or the map draws the old storey at the new
                        // place until the next scheduled cut.
                        g_map_recut.store(true, std::memory_order_release);
                        toast("centred on the shrine");
                    }
                    else
                    {
                        mv::Waypoint wp{};
                        wp.set = true;
                        wp.x = r.s->x;
                        wp.y = r.s->y;
                        wp.z = r.s->z;
                        mm::set_waypoint(wp);
                        mm::g_waypoint_dirty.store(true, std::memory_order_release);
                        toast("waypoint set on the shrine");
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\nid %s\nclick: waypoint    double-click: centre the map",
                                      r.s->label().c_str(), r.s->id.c_str());
                }
                ImGui::TableNextColumn();
                if (r.s->chapter < 0)
                {
                    ImGui::TextDisabled("-");
                }
                else if (r.s->chapter == 0)
                {
                    ImGui::TextUnformatted("DLC");
                }
                else
                {
                    ImGui::Text("%d", r.s->chapter);
                }
                ImGui::TableNextColumn();
                if (r.dist < 0.0)
                {
                    ImGui::TextDisabled("-");
                }
                else if (r.dist >= 100000.0)
                {
                    ImGui::Text("%.1f km", r.dist / 100000.0);
                }
                else
                {
                    ImGui::Text("%.0f m", r.dist / 100.0);
                }
                ImGui::TableNextColumn();
                if (!st.valid)
                {
                    ImGui::TextDisabled("n/a");
                }
                else if (r.unlocked)
                {
                    ImGui::TextColored(ImVec4{0.55f, 0.85f, 0.55f, 1.0f}, "yes");
                }
                else
                {
                    ImGui::TextDisabled("no");
                }
                if (cfg.fast_travel_enabled)
                {
                    ImGui::TableNextColumn();
                    // Only an id the save says is unlocked: travelling to a locked one is
                    // untested and is exactly the call that can wedge level streaming.
                    const bool can = r.unlocked && tv.phase != shr::Travel::Requested &&
                                     tv.phase != shr::Travel::InFlight;
                    ImGui::BeginDisabled(!can);
                    if (ImGui::SmallButton("Travel"))
                    {
                        shr::request_travel(r.s->id.c_str());
                        toast("fast travel requested");
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!cfg.fast_travel_enabled)
            {
                ImGui::TextDisabled("Fast travel is off (fast_travel_enabled, Advanced tab).");
            }
        }

        //==============================================================================
        // The collection statistics page
        //==============================================================================
        //
        // "Why am I using this mod" on one screen: found/total for every chapter and
        // every category, the shrines the save has lit, and one overall percentage.
        //
        // RENDER THREAD ONLY, and it never asks the game anything. Both sources are
        // published snapshots - `markers::stats()` (a spinlock and a ~1.5 KB copy) and
        // `shr::state()` (~6.5 KB) - so asking per frame would be ~8 KB of copying and a
        // lock round-trip per frame for numbers that change once per SWEEP ROUND. The
        // cache below therefore refreshes when `markers::rounds()` moves, with a 1 s
        // floor so the page still fills in when the live sweep is off entirely (rounds
        // never advance then, and a page that stays empty for ever reads as a bug).




        // THE CATEGORIES THE COLLECTION PAGE COUNTS, and the order they are shown in.

        // Draws into whatever window is current. `compact` drops the per-chapter matrix
        // and keeps the summary, for the F2 panel where vertical space is scarce.
        void draw_collection_stats(std::uint64_t now, bool compact)
        {
            const StatsCache& c = stats_cached(now);
            const markers::Stats& st = c.st;

            if (!st.db_loaded || st.static_markers == 0)
            {
                ImGui::TextDisabled("no markers\\<chapter>.json loaded - live markers only");
                return;
            }

            // ---- the headline ---------------------------------------------------------
            int found_all = 0;
            int total_all = 0;
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                found_all += st.cat[i].found;
                total_all += st.cat[i].total;
            }
            const float pct = total_all > 0 ? 100.0f * static_cast<float>(found_all) /
                                                  static_cast<float>(total_all)
                                            : 0.0f;
            ImGui::Text("%d / %d collected", found_all, total_all);
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1f%% of every chapter)", static_cast<double>(pct));
            ImGui::ProgressBar(total_all > 0 ? static_cast<float>(found_all) /
                                                   static_cast<float>(total_all)
                                             : 0.0f,
                               ImVec2(-1.0f, ImGui::GetTextLineHeight()));

            // ---- shrines lit ----------------------------------------------------------
            //
            // `UnlockedFirepoints` also holds boss-door and task pseudo-points, so the
            // raw count is not "shrines". Only the ids that JOIN to a shrine marker in
            // the static DB are counted; the raw list length is shown beside it so a
            // join that goes wrong is visible rather than silent.
            const int shrine_total = st.cat[static_cast<int>(mdb::Cat::Shrine)].total;
            if (!c.shrines.valid)
            {
                ImGui::TextDisabled("Shrines lit: n/a  (%s)",
                                    c.shrines.route[0] != '\0' ? c.shrines.route : "not read yet");
            }
            else
            {
                int lit = 0;
                const markers::View view = markers::view();
                for (int i = 0; i < c.shrines.id_count; ++i)
                {
                    for (std::size_t m = 0; m < view.count; ++m)
                    {
                        if (view.data[m].cat != static_cast<std::uint8_t>(mdb::Cat::Shrine))
                        {
                            continue;
                        }
                        if (::_stricmp(view.data[m].id, c.shrines.ids[i]) == 0)
                        {
                            ++lit;
                            break;
                        }
                    }
                }
                ImGui::Text("Shrines lit: %d", lit);
                ImGui::SameLine();
                ImGui::TextDisabled("(%d unlocked ids incl. boss doors / tasks; %d shrines in the DB%s)",
                                    c.shrines.unlocked, shrine_total,
                                    c.shrines.truncated ? "; list TRUNCATED" : "");
                if (c.shrines.current[0] != '\0')
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("| last rested at %s", c.shrines.current);
                }
            }

            // ---- per category ---------------------------------------------------------
            ImGui::Spacing();
            if (ImGui::BeginTable("stats_cat", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Category");
                ImGui::TableSetupColumn("Found");
                ImGui::TableSetupColumn("Total");
                ImGui::TableSetupColumn("%");
                ImGui::TableHeadersRow();
                for (const mdb::Cat cat : kStatsCats)
                {
                    const int i = static_cast<int>(cat);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", mdb::cat_label(cat));
                    ImGui::TableNextColumn();
                    if (st.cat[i].total == 0)
                    {
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        continue;
                    }
                    ImGui::Text("%d", st.cat[i].found);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", st.cat[i].total);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%", 100.0 * static_cast<double>(st.cat[i].found) /
                                              static_cast<double>(st.cat[i].total));
                }
                ImGui::EndTable();
            }

            if (compact)
            {
                return;
            }

            // ---- per chapter x category ----------------------------------------------
            //
            // The six collectable categories, always all six, and nothing else: with the
            // full enum this was 15 columns and had to scroll sideways inside a 1080p
            // panel, which made the numbers on the right unreachable in practice.
            ImGui::Spacing();
            ImGui::TextDisabled("per chapter");
            if (ImGui::BeginTable("stats_matrix", kStatsCatCount + 1,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 11.0f)))
            {
                ImGui::TableSetupColumn("Chapter");
                for (const mdb::Cat cat : kStatsCats)
                {
                    ImGui::TableSetupColumn(mdb::cat_label(cat));
                }
                ImGui::TableHeadersRow();

                const auto cell = [](const markers::CatStat& cs) {
                    ImGui::TableNextColumn();
                    if (cs.total == 0)
                    {
                        ImGui::TextDisabled("-");
                    }
                    else if (cs.found >= cs.total)
                    {
                        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%d/%d", cs.found, cs.total);
                    }
                    else
                    {
                        ImGui::Text("%d/%d", cs.found, cs.total);
                    }
                };

                // Row 0 is the bucket for a manifest whose "chapter" is not a number -
                // the DLC one spells it "DLC".
                for (int ch = 0; ch <= 8; ++ch)
                {
                    markers::CatStat row{};
                    for (int i = 0; i < mdb::kCatCount; ++i)
                    {
                        row.total += st.chapter[ch][i].total;
                        row.found += st.chapter[ch][i].found;
                    }
                    if (row.total == 0)
                    {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // The chapter the markers are currently filtered to (the one the
                    // player is standing in) is starred and highlighted.
                    char label[16]{};
                    const bool here = (ch == st.filter_chapter);
                    if (ch == 0)
                    {
                        ::strncpy_s(label, sizeof(label), here ? "DLC*" : "DLC", _TRUNCATE);
                    }
                    else
                    {
                        ::_snprintf_s(label, sizeof(label), _TRUNCATE, here ? "%d*" : "%d", ch);
                    }
                    if (here)
                    {
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", label);
                    }
                    else
                    {
                        ImGui::TextUnformatted(label);
                    }
                    for (const mdb::Cat cat : kStatsCats)
                    {
                        cell(st.chapter[ch][static_cast<int>(cat)]);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("* the chapter the markers are filtered to right now");
        }

        void draw_toast()
        {
            const std::uint64_t now = ::GetTickCount64();
            if (g_toast[0] == '\0' || now >= g_toast_until)
            {
                return;
            }
            const std::uint64_t left = g_toast_until - now;
            const float a = left >= 300 ? 1.0f : static_cast<float>(left) / 300.0f;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            // The one thing that KEEPS the foreground list (review B.20 moved the HUD
            // off it): a toast is a two-second notice about something the player just
            // did, and it has to be readable over the panel and over the full map -
            // "map copied to clipboard" is raised by a key that only works while the map
            // is open.
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(g_toast);
            const float pad = ImGui::GetTextLineHeight() * 0.5f;
            const ImVec2 c{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.72f};
            const ImVec2 tl{c.x - ts.x * 0.5f - pad, c.y - ts.y * 0.5f - pad * 0.5f};
            const ImVec2 br{c.x + ts.x * 0.5f + pad, c.y + ts.y * 0.5f + pad * 0.5f};
            dl->AddRectFilled(tl, br, plate_color(static_cast<int>(220.0f * a)), 4.0f);
            dl->AddRect(tl, br, IM_COL32(150, 158, 168, static_cast<int>(180.0f * a)), 4.0f, 0, 1.2f);
            dl->AddText(ImVec2{c.x - ts.x * 0.5f, c.y - ts.y * 0.5f},
                        IM_COL32(240, 242, 246, static_cast<int>(255.0f * a)), g_toast);
        }

        // ---- "this marker just became found" -----------------------------------------
        //
        // A 400 ms ring where a marker was collected. The event has to come from




        void note_found_event(double x, double y, std::uint64_t now)
        {
            g_found_events[g_found_event_head] = FoundEvent{x, y, now};
            g_found_event_head = (g_found_event_head + 1) % kFoundEvents;
        }

        // Called from build_frame_candidates, once per PUBLISHED ROUND rather than per
        // frame - the flags cannot change in between.
        void update_found_watch(std::uint64_t round, std::uint64_t now)
        {
            if (round == g_found_watch_round)
            {
                return;
            }
            const bool first = g_found_watch_round == 0;
            g_found_watch_round = round;

            FoundWatch next[kFoundWatch]{};
            int n = 0;
            const float radius2 = static_cast<float>(kFoundWatchUu * kFoundWatchUu);
            for (const FrameCand& fc : g_frame_cands)
            {
                if (n >= kFoundWatch)
                {
                    break;
                }
                if (fc.d2_xy > radius2 || fc.m->id[0] == '\0')
                {
                    continue;
                }
                ::strncpy_s(next[n].id, sizeof(next[n].id), fc.m->id, _TRUNCATE);
                next[n].found = fc.found;
                // A marker that was in the previous round's watch as NOT found and is
                // found now is the event. A marker that was not being watched cannot
                // produce one - which is what stops the first round after a load firing
                // a ring for every item the save says is already collected.
                if (!first && next[n].found)
                {
                    for (int j = 0; j < g_found_watch_n; ++j)
                    {
                        if (!g_found_watch[j].found && std::strcmp(g_found_watch[j].id, next[n].id) == 0)
                        {
                            note_found_event(fc.m->x, fc.m->y, now);
                            break;
                        }
                    }
                }
                ++n;
            }
            for (int i = 0; i < n; ++i)
            {
                g_found_watch[i] = next[i];
            }
            g_found_watch_n = n;
        }


        void build_frame_candidates(const mm::Snapshot& snap)
        {
            if (g_pf_markpass < 0)
            {
                g_pf_markpass = mm::perf_register("marker pass", perf::Thread::Render);
            }
            const mm::PerfScope scope(g_pf_markpass);
            g_frame_cands.clear();
            g_frame_marker_total = 0;
            g_frame_bad_cat = 0;

            const markers::View v = markers::view();
            g_frame_marker_total = static_cast<int>(v.count);
            if (v.data == nullptr || v.count == 0)
            {
                return;
            }
            g_frame_cands.reserve(v.count);
            for (std::size_t i = 0; i < v.count; ++i)
            {
                const markers::DrawMarker& m = v.data[i];
                if (static_cast<int>(m.cat) >= mdb::kCatCount)
                {
                    ++g_frame_bad_cat;
                    continue;
                }
                const double dx = m.x - snap.x;
                const double dy = m.y - snap.y;
                const double dz = m.z - snap.z;
                FrameCand c{};
                c.m = &m;
                c.d2_xy = static_cast<float>(dx * dx + dy * dy);
                c.d2_3d = static_cast<float>(dx * dx + dy * dy + dz * dz);
                c.cat = m.cat;
                c.rarity = m.rarity;
                c.flags = m.flags;
                c.found = (m.flags & markers::kFlagFound) != 0;
                g_frame_cands.push_back(c);
            }

            // The found-ring events. Diffed once per published marker round, not per
            // frame - the flags cannot change in between.
            update_found_watch(markers::rounds(), ::GetTickCount64());
        }

        //==============================================================================
        // DECLUTTER: MERGING COINCIDENT GLYPHS (review B.16)
        //==============================================================================
        //
        // Six chests in one room are six glyphs inside one glyph's width: a smear that
        // says "chests" less clearly than a single chest with a 6 next to it. The compass
        // has deduped its pips since 0.9.2; the minimap and the full map never did - they
        // only ever culled by distance and by a hard count.
        //
        // WHAT MERGES. Same CATEGORY only, and same found state. Merging across
        // categories would be a lie - one glyph cannot mean "a chest and an NPC" - and
        // the shape is the half of a marker's identity that survives at 6 px, so it is
        // the half that must not be invented. The nearest member of a cluster is the one
        // drawn, because it is the one the player is walking to.
        //
        // HOW. A uniform grid keyed on (cell, category): anything landing in the same
        // cell as an already-kept glyph of the same category joins it. Cell size is the
        // merge distance, so two glyphs that straddle a cell boundary can stay separate -
        // the same property the compass's 3-pixel columns have, and the same reason: an
        // O(n) grid instead of an O(n x kept) sweep on the render thread, for a

        // The little "and N more like this one" badge. Drawn up and to the right of the
        // glyph, on the plate colour so it reads over both the walkable fill and the
        // dark backdrop, and never for a cluster of one.
        void draw_count_badge(ImDrawList* dl, ImVec2 at, float r, int count, int alpha)
        {
            if (count < 2)
            {
                return;
            }
            char text[8]{};
            if (count > 99)
            {
                (void)std::snprintf(text, sizeof(text), "99+");
            }
            else
            {
                (void)std::snprintf(text, sizeof(text), "%d", count);
            }
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 tp{at.x + r * 0.65f, at.y - r * 0.65f - ts.y * 0.5f};
            dl->AddRectFilled(ImVec2{tp.x - 2.0f, tp.y}, ImVec2{tp.x + ts.x + 2.0f, tp.y + ts.y},
                              plate_color(static_cast<int>(alpha * 0.82f)), 2.0f);
            dl->AddText(tp, IM_COL32(238, 242, 248, alpha), text);
        }

        void draw_markers(const mm::Config& cfg, const MiniGeom& g, bool round, float x0, float y0, float side,
                          ImDrawList* dl)
        {
            g_marker_draw = MarkerDrawStats{};
            if (!cfg.markers_enabled)
            {
                return;
            }
            g_marker_draw.total = g_frame_marker_total;
            g_marker_draw.filtered = g_frame_bad_cat;
            if (g_frame_cands.empty())
            {
                return;
            }

            const float r = cfg.markers_size;
            const float limit = (std::max)(4.0f, g.half - r - 2.0f);

            struct Cand
            {
                float dx = 0.0f;
                float dy = 0.0f;
                float d2 = 0.0f;
                std::uint8_t cat = 0;
                std::uint8_t rarity = 0;
                bool found = false;
                bool clamped = false;
                int count = 1; // how many markers this glyph stands for (review B.16)
                const char* id = nullptr;
            };
            // Render thread only, and reused frame to frame so a full minimap never
            // allocates during Present.
            static std::vector<Cand> cands;
            cands.clear();

            for (const FrameCand& fc : g_frame_cands)
            {
                const markers::DrawMarker& m = *fc.m;
                const mdb::Cat cat = static_cast<mdb::Cat>(fc.cat);
                if (!mdb::cat_enabled(cfg.markers_categories, cat))
                {
                    ++g_marker_draw.filtered;
                    continue;
                }
                const bool found = fc.found;
                if (found && cfg.markers_hide_found)
                {
                    ++g_marker_draw.filtered;
                    continue;
                }

                // The minimap is centred on the position the RENDER side works from,
                // so the on-screen offset is still computed here; the distance used for
                // the cap and the sort comes from the shared pass.
                const MiniOffset off =
                    mini_offset(g, m.x, m.y, round, limit, cfg.markers_clamp_to_edge);
                if (!off.visible)
                {
                    continue;
                }

                Cand cand{};
                cand.dx = static_cast<float>(off.dx);
                cand.dy = static_cast<float>(off.dy);
                cand.d2 = fc.d2_xy;
                cand.cat = fc.cat;
                cand.rarity = fc.rarity;
                cand.found = found;
                cand.clamped = off.clamped;
                cand.id = m.id;
                cands.push_back(cand);
            }

            // ONE sort, nearest first: the cap drops the far ones and the draw loop then
            // walks the array BACKWARDS, so the near ones are painted last (on top).
            // partial_sort already leaves [0, cap) sorted ascending, so after the resize
            // the whole array is sorted - the second full sort this used to do was pure
            // waste on up to 400 candidates every single frame.
            const std::size_t cap = cfg.markers_max_draw > 0
                                        ? static_cast<std::size_t>(cfg.markers_max_draw)
                                        : cands.size();
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            else
            {
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            }

            // ---- declutter -----------------------------------------------------------
            //
            // After the sort, so the glyph kept for a cluster is its NEAREST member, and
            // after the cap, so merging cannot resurrect a marker the cap dropped. A
            // clamped glyph is left out of it: everything on the rim is at the rim by
            // definition and merging those would collapse a whole direction into one
            // number.
            {
                static MergeGrid grid;
                const float merge_r = (std::max)(3.0f, r);
                grid.reset(g.center.x - g.half, g.center.y - g.half, side, side, merge_r);
                std::size_t kept = 0;
                for (std::size_t i = 0; i < cands.size(); ++i)
                {
                    const Cand& c = cands[i];
                    const float sx = g.center.x + c.dx;
                    const float sy = g.center.y + c.dy;
                    const int key = c.clamped ? -1 : grid.find(sx, sy, static_cast<int>(c.cat));
                    if (key >= 0 && cands[static_cast<std::size_t>(key)].found == c.found)
                    {
                        ++cands[static_cast<std::size_t>(key)].count;
                        continue;
                    }
                    if (!c.clamped)
                    {
                        grid.add(sx, sy, static_cast<int>(c.cat), static_cast<int>(kept));
                    }
                    cands[kept++] = c;
                }
                g_marker_draw.merged = static_cast<int>(cands.size() - kept);
                cands.resize(kept);
            }

            const float op = cfg.opacity;
            for (std::size_t ci = cands.size(); ci-- > 0;)
            {
                const Cand& cand = cands[ci];
                const float a = op * (cand.found ? cfg.markers_found_alpha : 1.0f);
                if (a <= 0.01f)
                {
                    continue;
                }
                const int alpha = static_cast<int>((std::min)(1.0f, a) * 255.0f + 0.5f);
                const ImU32 col = marker_color_q(static_cast<mdb::Cat>(cand.cat), cand.rarity, alpha,
                                                 cfg.markers_rarity_tint, cfg.xray_rarity_colors);
                const ImU32 edge = IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f));
                const ImVec2 p{g.center.x + cand.dx, g.center.y + cand.dy};
                draw_marker_glyph(dl, static_cast<mdb::Cat>(cand.cat), p, cand.clamped ? r * 0.72f : r, col, edge,
                                  cand.found);
                draw_count_badge(dl, p, r, cand.count, alpha);
                ++g_marker_draw.drawn;
                g_marker_draw.clamped += cand.clamped ? 1 : 0;
            }
            if (!cands.empty())
            {
                // cands is sorted near -> far, so the FIRST one is the nearest.
                const Cand& near_one = cands.front();
                if (near_one.id != nullptr)
                {
                    ::strncpy_s(g_marker_draw.nearest, sizeof(g_marker_draw.nearest), near_one.id, _TRUNCATE);
                }
                g_marker_draw.nearest_uu = std::sqrt(near_one.d2);
            }
            (void)x0;
            (void)y0;
            (void)side;
        }

        //==============================================================================
        // THE ONE HUD GATE
        //==============================================================================
        //
        // "May anything of ours be on screen right now?" - the minimap, the compass and
        // the x-ray highlight all ask exactly this, so it is evaluated exactly once, in
        // one place, from live state only. It returns nullptr when everything of ours may
        // draw, or the reason it may not, and the caller decides what to do with that:
        // the minimap feeds it to set_hide_reason() (which owns the "hidden because"
        // readout and the transition log lines), the compass and the highlight simply do
        // not draw. Adding a second set of show/hide rules for the compass is exactly the
        // shape of bug lessons.md warns about, so there isn't one.
        //

        void draw_minimap(const mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            g_last_mini = MiniDebug{};

            const std::uint64_t now = ::GetTickCount64();
            if (const wchar_t* blocked = hud_gate(cfg, snap, have_state, now); blocked != nullptr)
            {
                set_hide_reason(blocked);
                return;
            }
            const mapdata::Chapter* chapter_ptr = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (chapter_ptr == nullptr)
            {
                set_hide_reason(L"player is outside every mapped chapter");
                return;
            }
            const mapdata::Chapter& chapter = *chapter_ptr;
            const bool composite_ready = g_map.ready && chapter.key == g_map.chapter;
            if (!chapter.has_heights() && !composite_ready)
            {
                set_hide_reason(L"no height maps and no composite texture loaded");
                return;
            }

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float screen_w = vp->Size.x;
            const float screen_h = vp->Size.y;
            const float side = (std::max)(cfg.minimap_min_px,
                                          (std::min)(cfg.size_frac * screen_h,
                                                     (std::min)(screen_w, screen_h) * 0.9f));

            // A non-custom hud_preset overrides minimap_anchor (and puts the compass on
            // the same vertical side - see draw_compass).
            const mm::Anchor anchor = effective_anchor(cfg);
            float x0 = cfg.offset_x;
            float y0 = cfg.offset_y;
            if (anchor == mm::Anchor::TopRight || anchor == mm::Anchor::BottomRight)
            {
                x0 = screen_w - cfg.offset_x - side;
            }
            if (anchor == mm::Anchor::BottomLeft || anchor == mm::Anchor::BottomRight)
            {
                y0 = screen_h - cfg.offset_y - side;
            }
            x0 += vp->Pos.x;
            y0 += vp->Pos.y;

            MiniGeom g{};
            g.half = side * 0.5f;
            g.center = ImVec2{x0 + g.half, y0 + g.half};
            g.zoom = cfg.zoom_uu_per_px;
            g.uv = uv_of(chapter);
            g.px = snap.x;
            g.py = snap.y;
            const float eff_yaw = cfg.rotate_with_player ? snap.yaw : 0.0f;
            g.cos_yaw = std::cos(eff_yaw * kPi / 180.0f);
            g.sin_yaw = std::sin(eff_yaw * kPi / 180.0f);

            const float op = cfg.opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };
            // A darker, more opaque disc than the MVP had: the first in-world
            // screenshot showed a light grey map over a light grey scene, and the
            // walkable fill needs something dark to sit on.
            const auto ch = [](float v) { return static_cast<int>(v + 0.5f); };
            const ImU32 backdrop = IM_COL32(ch(cfg.minimap_backdrop_r), ch(cfg.minimap_backdrop_g),
                                            ch(cfg.minimap_backdrop_b), alpha(cfg.minimap_backdrop));
            const ImU32 frame = IM_COL32(ch(cfg.minimap_frame_r), ch(cfg.minimap_frame_g), ch(cfg.minimap_frame_b),
                                         alpha(cfg.minimap_frame_alpha));
            const ImU32 inner_ring = IM_COL32(0, 0, 0, alpha(0.55f));

            // The slice texture already carries the floor colour, the height gradient
            // and the per-pixel alpha, so the only tint left is the global opacity.
            const ImU32 tint_slice = IM_COL32(255, 255, 255, alpha(1.0f));
            const ImU32 tint_composite = IM_COL32(255, 255, 255, alpha(cfg.minimap_composite_alpha));

            // THE HUD DRAWS UNDER OUR OWN WINDOWS (review B.20). ImGui renders the
            // background draw list first, then every window, then the foreground list -
            // so a HUD on the FOREGROUND list painted over the centred F2 panel
            // whatever order the calls were made in. The background list is still over
            // the game (everything ImGui draws is), it is just under the panel, the full
            // map and the tooltips. Suppressing the HUD while the panel is open was the
            // other option and it is worse: the panel is where the minimap's own sliders
            // live, and you cannot tune a picture you cannot see.
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            if (cfg.round)
            {
                dl->AddCircleFilled(g.center, g.half, backdrop, g_circle_segments);
            }
            else
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, backdrop, 4.0f);
            }

            // ONE textured quad: the height slice. `update_slice` re-cuts the window
            // around the player at slice_hz and returns true while a window is
            // available to draw (its own or the previous one's - the window carries
            // margin, so a skipped update is invisible).
            const bool slice_ok = plan_slice(cfg, chapter, g.half, now);
            const SliceView sv = slice_view();
            if (slice_ok && sv.shown >= 0)
            {
                const SliceBuf& b = g_slice[sv.shown];
                const UvMap window{sv.min_y, sv.max_x, sv.px_per_uu, b.w, b.h};
                draw_srv(dl, b.srv_gpu, window, g, tint_slice, cfg.round, x0, y0, side);
            }
            else if (composite_ready)
            {
                // No height maps: the Z-shaded composite, which merges every storey.
                draw_image(dl, g_map, uv_of(chapter), g, tint_composite, cfg.round, x0, y0, side);
            }
            else
            {
                set_hide_reason(L"the height slicer has no window yet");
                return;
            }

            if (cfg.round)
            {
                dl->AddCircle(g.center, g.half - 1.0f, inner_ring, g_circle_segments, 2.0f);
                dl->AddCircle(g.center, g.half, frame, g_circle_segments, 2.0f);
            }
            else
            {
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + side, y0 + side}, frame, 4.0f, 0, 2.0f);
            }

            // Markers go over the map and under the frame ring's highlight and the
            // player arrow, so the arrow is never hidden by a glyph standing on it.
            draw_markers(cfg, g, cfg.round, x0, y0, side, dl);

            // A ring where something was just collected - so a pickup taken off screen
            // (or behind the player) still registers on the minimap.
            draw_found_rings(dl, now, (std::max)(4.0f, cfg.markers_size * 1.4f),
                             [&](double wx, double wy, float& sx, float& sy) {
                                 // A ring is never clamped: it says "that was collected
                                 // THERE", and a ring on the rim would be a lie.
                                 const MiniOffset off =
                                     mini_offset(g, wx, wy, cfg.round, g.half - 2.0f, false);
                                 if (!off.visible)
                                 {
                                     return false;
                                 }
                                 sx = g.center.x + static_cast<float>(off.dx);
                                 sy = g.center.y + static_cast<float>(off.dy);
                                 return true;
                             });

            // THE CARDINAL REFERENCE, always drawn (review-0.9.1 item 5). Until 0.9.3
            // the north dot appeared only when `rotate_with_player` was on - so the
            // SHIPPED north-up default had no cardinal reference at all, and nothing on
            // screen said which way the picture was oriented. Now: four ticks on the
            // rim plus the letter N, rotating with the yaw in rotate mode and standing
            // still (N straight up) in north-up mode, where they say "this is north-up"
            // rather than nothing.
            {
                // A point on the rim, `inset` pixels in from it - the disc's circle or
                // the square's edge, so the ticks sit ON the frame either way.
                const auto rim = [&](float ang, float inset) {
                    const float dx = std::sin(ang);
                    const float dy = -std::cos(ang);
                    const float lim = (std::max)(4.0f, g.half - inset);
                    if (cfg.round)
                    {
                        return ImVec2{g.center.x + dx * lim, g.center.y + dy * lim};
                    }
                    const float m = (std::max)(std::abs(dx), std::abs(dy));
                    const float sc = m > 1e-4f ? lim / m : 0.0f;
                    return ImVec2{g.center.x + dx * sc, g.center.y + dy * sc};
                };
                const ImU32 tick_col = IM_COL32(226, 230, 236, alpha(0.6f));
                const ImU32 north_col = IM_COL32(255, 226, 160, alpha(0.95f));
                for (int i = 0; i < 4; ++i)
                {
                    const float ang = (static_cast<float>(i) * 90.0f - eff_yaw) * kPi / 180.0f;
                    const bool north = i == 0;
                    dl->AddLine(rim(ang, 3.0f), rim(ang, north ? 11.0f : 8.0f),
                                north ? north_col : tick_col, north ? 2.2f : 1.4f);
                }
                const float na = -eff_yaw * kPi / 180.0f;
                const ImVec2 np = rim(na, 19.0f);
                const ImVec2 ts = ImGui::CalcTextSize("N");
                const ImVec2 tp{np.x - ts.x * 0.5f, np.y - ts.y * 0.5f};
                // A shadow rather than a plate: a plate at the rim would cover the map,
                // and the letter has to be legible over both the fill and the backdrop.
                dl->AddText(ImVec2{tp.x + 1.0f, tp.y + 1.0f}, IM_COL32(0, 0, 0, alpha(0.8f)), "N");
                dl->AddText(tp, north_col, "N");
            }

            // The waypoint, edge-clamped with its distance. It is the one marker that
            // must never be culled: the whole point of setting one is to be told which
            // way to walk while it is off the map.
            {
                const mv::Waypoint wp = mm::waypoint();
                if (wp.set)
                {
                    const float wr = (std::max)(5.0f, cfg.markers_size * cfg.waypoint_size_scale);
                    const float lim = (std::max)(4.0f, g.half - wr - 3.0f);
                    // ALWAYS clamped: the whole point of setting a waypoint is to be
                    // told which way to walk while it is off the map.
                    const MiniOffset off = mini_offset(g, wp.x, wp.y, cfg.round, lim, true);
                    const ImVec2 wp_pos{g.center.x + static_cast<float>(off.dx),
                                        g.center.y + static_cast<float>(off.dy)};
                    draw_waypoint_glyph(dl, wp_pos, off.clamped ? wr * 0.85f : wr, alpha(1.0f));
                    const double wdx = wp.x - g.px;
                    const double wdy = wp.y - g.py;
                    const double dist_m = std::sqrt(wdx * wdx + wdy * wdy) / 100.0;
                    char label[32]{};
                    if (dist_m >= 1000.0)
                    {
                        (void)std::snprintf(label, sizeof(label), "%.1f km", dist_m / 1000.0);
                    }
                    else
                    {
                        (void)std::snprintf(label, sizeof(label), "%.0f m", dist_m);
                    }
                    const ImVec2 ts = ImGui::CalcTextSize(label);
                    const ImVec2 tp{wp_pos.x - ts.x * 0.5f, wp_pos.y + wr * 1.6f};
                    dl->AddRectFilled(ImVec2{tp.x - 3.0f, tp.y - 1.0f}, ImVec2{tp.x + ts.x + 3.0f, tp.y + ts.y + 1.0f},
                                      plate_color(alpha(0.7f)), 3.0f);
                    dl->AddText(tp, IM_COL32(255, 190, 235, alpha(1.0f)), label);
                }
            }

            add_player_arrow(dl, g.center, snap.yaw - eff_yaw,
                             (std::max)(cfg.minimap_arrow_min_px, side * cfg.minimap_arrow_frac));

            // THE WHEEL OVER THE DISC, and why it is gated on the panel being open.
            // Nothing here swallows the wheel: the WndProc hook only swallows input for
            // a MODE (the full map), so during play a wheel notch reaches the game as
            // well - and stealing the player's weapon / item wheel to zoom a minimap is
            // not a trade anybody asked for. While the F2 panel is up the cursor is
            // already ours and the wheel is already an overlay gesture, so it is safe
            // there. `zoom_key` is the route that works during play.
            if (mm::g_panel_open.load(std::memory_order_relaxed) && !ImGui::GetIO().WantCaptureMouse)
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.MouseWheel != 0.0f)
                {
                    const float mdx = io.MousePos.x - g.center.x;
                    const float mdy = io.MousePos.y - g.center.y;
                    const bool over = cfg.round
                                          ? (mdx * mdx + mdy * mdy) <= g.half * g.half
                                          : (std::abs(mdx) <= g.half && std::abs(mdy) <= g.half);
                    if (over)
                    {
                        // Away from the player = zoom out = a LARGER uu/px, so the sign
                        // matches the full map's wheel.
                        g_zoom_steps.fetch_add(io.MouseWheel > 0.0f ? -1 : 1, std::memory_order_relaxed);
                    }
                }
            }

            set_hide_reason(L"visible");
            g.uv = uv_of(chapter);
            const ImVec2 uv = uv_at(g, 0.0f, 0.0f);
            g_last_mini.visible = true;
            g_last_mini.u = uv.x;
            g_last_mini.v = uv.y;
            g_last_mini.chapter = chapter.key;
            g_last_mini.side = side;
        }

        //==============================================================================
        // Drawing: the HOLD-KEY X-RAY HIGHLIGHT (master plan step 7, v1)
        //==============================================================================
        //
        // While the highlight key (or the pad chord) is held, every marker of an enabled
        // category within highlight_radius of the PLAYER is drawn at its projected screen
        // position: category glyph, name, distance in metres, alpha fading with distance.
        // It is "through walls" for free - the overlay is composited over the finished
        // frame, so there is no occlusion test, no CustomDepth, no material, and nothing
        // that can go wrong with the game's render state.
        //
        // WHAT IT NEEDS AND WHAT IT DOES WITHOUT
        //   * the camera pose comes from hl::camera() (game thread, see highlight.cpp)
        //     and is REQUIRED: no fresh pose, no highlight. Falling back to the pawn's
        //     own position and yaw would put every label a spring-arm's length away from
        //     the truth, which is worse than drawing nothing;
        //   * the screen size comes from the ImGui viewport, which is the swapchain's -
        //     the same rectangle UE built its projection matrix for;
        //   * the radius test is against the player, the projection against the camera,
        //     and the distance shown is the player's. That is what "30 m away" means to
        //     someone deciding whether to walk over.



        // A small outward-pointing triangle at `p`, aimed along (dx, dy) in screen space.
        void add_edge_arrow(ImDrawList* dl, ImVec2 p, float dx, float dy, float r, ImU32 col, ImU32 edge)
        {
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f)
            {
                return;
            }
            const float ux = dx / len;
            const float uy = dy / len;
            const ImVec2 tip{p.x + ux * r, p.y + uy * r};
            const ImVec2 a{p.x - ux * r * 0.6f - uy * r * 0.75f, p.y - uy * r * 0.6f + ux * r * 0.75f};
            const ImVec2 b{p.x - ux * r * 0.6f + uy * r * 0.75f, p.y - uy * r * 0.6f - ux * r * 0.75f};
            dl->AddTriangleFilled(tip, a, b, col);
            dl->AddTriangle(tip, a, b, edge, 1.2f);
        }

        // `const char*`, not std::string (review B.18): every caller of this is inside
        // Present, and the x-ray builds up to twelve of these a frame.
        void draw_label(ImDrawList* dl, ImVec2 at, const char* text, ImU32 col, int alpha)
        {
            if (text == nullptr || text[0] == '\0')
            {
                return;
            }
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 tp{at.x - ts.x * 0.5f, at.y};
            dl->AddRectFilled(ImVec2{tp.x - 4.0f, tp.y - 1.0f}, ImVec2{tp.x + ts.x + 4.0f, tp.y + ts.y + 1.0f},
                              plate_color(static_cast<int>(alpha * 0.62f)), 3.0f);
            dl->AddText(tp, col, text);
        }

        void draw_highlight(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_hl_debug = HighlightDebug{};
            if (!cfg.overlay_enabled || !cfg.highlight_enabled || !gate_ok)
            {
                return;
            }
            if (!hl::held())
            {
                return;
            }
            g_hl_debug.active = true;

            hl::Pose pose{};
            if (!hl::camera(pose))
            {
                return; // the reader has not produced a pose yet
            }
            const std::uint64_t now = ::GetTickCount64();
            g_hl_debug.cam_age_ms = pose.stamp_ms == 0 ? 0 : now - pose.stamp_ms;
            // A pose older than a few frames is a camera that has stopped being read (the
            // game thread stalled, or the pawn went) - draw with it and the labels lag
            // visibly behind the scene, which reads as a bug. 250 ms is ~15 frames.
            if (pose.stamp_ms == 0 || g_hl_debug.cam_age_ms > 250)
            {
                return;
            }
            g_hl_debug.have_camera = true;

            proj::Camera cam{};
            cam.x = pose.x;
            cam.y = pose.y;
            cam.z = pose.z;
            cam.pitch = pose.pitch;
            cam.yaw = pose.yaw;
            cam.roll = pose.roll;
            cam.fov_deg = pose.fov;
            if (!proj::camera_sane(cam))
            {
                return;
            }

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const double screen_w = static_cast<double>(vp->Size.x);
            const double screen_h = static_cast<double>(vp->Size.y);
            if (!(screen_w > 1.0) || !(screen_h > 1.0))
            {
                return;
            }

            if (g_frame_cands.empty())
            {
                return;
            }

            struct Cand
            {
                float d2 = 0.0f; // squared 3D distance from the player
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Cand> cands; // render thread only, reused every frame
            cands.clear();

            // The one per-frame marker pass has already walked the buffer and computed
            // the distances; this only filters. The square root is taken for the handful
            // that are actually drawn, where metres are needed.
            const double radius = static_cast<double>(cfg.highlight_radius);
            const float radius2 = static_cast<float>(radius * radius);
            // ONE GATE, AND IT NAMES ITS REASON. "It is on the minimap and not in the
            // x-ray" has been reported twice (a chest in a house 11 m away; a pre-placed
            // pickup lying on the ground in front of the player) and each guess at which
            // condition did it costs a play session. The conditions now live in the pure,
            // offline-tested mdb::xray_gate() and every rejection is counted, so the
            // per-round line below answers it from the log instead.
            for (const FrameCand& fc : g_frame_cands)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(fc.cat);
                mdb::XrayFacts xf{};
                xf.cat = cat;
                xf.cat_selected = mdb::cat_enabled(cfg.highlight_categories, cat);
                xf.found = fc.found;
                xf.show_found = cfg.highlight_show_found;
                xf.live = (fc.flags & markers::kFlagLive) != 0;
                xf.within_radius = fc.d2_3d <= radius2;
                ++g_hl_debug.gated;
                const mdb::XrayDrop drop = mdb::xray_gate(xf);
                if (drop != mdb::XrayDrop::Drawn)
                {
                    ++g_hl_debug.dropped[static_cast<int>(drop)];
                    continue;
                }
                cands.push_back(Cand{fc.d2_3d, fc.m});
            }
            g_hl_debug.considered = static_cast<int>(cands.size());

            // One sort (nearest first); the draw loop then runs BACKWARDS so the nearest
            // label ends up on top of the pile.
            const std::size_t cap = static_cast<std::size_t>((std::max)(1, cfg.highlight_max_draw));
            if (cands.size() > cap)
            {
                std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                cands.resize(cap);
            }
            else
            {
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            }

            // THE HUD DRAWS UNDER OUR OWN WINDOWS (review B.20). ImGui renders the
            // background draw list first, then every window, then the foreground list -
            // so a HUD on the FOREGROUND list painted over the centred F2 panel
            // whatever order the calls were made in. The background list is still over
            // the game (everything ImGui draws is), it is just under the panel, the full
            // map and the tooltips. Suppressing the HUD while the panel is open was the
            // other option and it is worse: the panel is where the minimap's own sliders
            // live, and you cannot tune a picture you cannot see.
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const float r = cfg.highlight_size;
            const float pad = r * 2.4f;

            // ONE projection pass, nearest first. Everything after it works off this
            // list: the glyphs are drawn from it backwards (far to near, so the nearest
            // ends up on top) and the labels are chosen from it forwards (nearest first,
            // so the cap keeps the ones the player is walking towards).
            struct Shown
            {
                float sx = 0.0f; // screen position, viewport-relative already applied
                float sy = 0.0f;
                float nx = 0.0f; // edge direction, when this one is off screen / behind
                float ny = 0.0f;
                double dist = 0.0; // uu from the player
                int alpha = 0;
                ImU32 col = 0;
                std::uint8_t cat = 0;
                bool on_screen = false;
                bool found = false;
                const markers::DrawMarker* m = nullptr;
            };
            static std::vector<Shown> shown; // render thread only, reused every frame
            shown.clear();

            for (const Cand& cand : cands)
            {
                const double cand_dist = std::sqrt(static_cast<double>(cand.d2));
                const markers::DrawMarker& m = *cand.m;
                const proj::Result pr = proj::project(cam, m.x, m.y, m.z, screen_w, screen_h);
                if (!pr.valid)
                {
                    ++g_hl_debug.no_projection;
                    continue;
                }

                // Fade with distance: fully lit at the camera, highlight_alpha_far at the
                // radius. Linear - a squared falloff makes everything past half the radius
                // look identical.
                const double t = radius > 1.0 ? (cand_dist / radius) : 0.0;
                const double a = static_cast<double>(cfg.highlight_alpha_near) +
                                 (static_cast<double>(cfg.highlight_alpha_far) -
                                  static_cast<double>(cfg.highlight_alpha_near)) *
                                     (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
                const int alpha = static_cast<int>((std::min)(1.0, (std::max)(0.0, a)) * 255.0 + 0.5);
                if (alpha <= 4)
                {
                    ++g_hl_debug.faded_out;
                    continue;
                }
                const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                Shown sh{};
                sh.dist = cand_dist;
                sh.alpha = alpha;
                // THE POINT OF THE FEATURE: while the key is held, quality wins over
                // category, so a weapon and a key item stand out from the consumables.
                sh.col = marker_color_q(cat, m.rarity, alpha, cfg.xray_rarity_colors_enabled,
                                        cfg.xray_rarity_colors);
                sh.cat = m.cat;
                sh.found = (m.flags & markers::kFlagFound) != 0;
                sh.m = &m;

                if (pr.on_screen && !pr.behind)
                {
                    sh.on_screen = true;
                    sh.sx = vp->Pos.x + static_cast<float>(pr.sx);
                    sh.sy = vp->Pos.y + static_cast<float>(pr.sy);
                    shown.push_back(sh);
                    continue;
                }
                if (!cfg.highlight_edge_arrows)
                {
                    ++g_hl_debug.offscreen_no_arrow;
                    continue;
                }
                // Off screen (or behind): an arrow on the rim pointing the way to turn.
                // proj::project() already handed us a direction rather than a mirrored
                // position, so this is just a projection onto the border box.
                double nx = pr.ndc_x;
                double ny = pr.ndc_y;
                const double mag = (std::max)(std::fabs(nx), std::fabs(ny));
                if (mag < 1e-6)
                {
                    continue;
                }
                nx /= mag;
                ny /= mag;
                float sx = static_cast<float>((nx * 0.5 + 0.5) * screen_w);
                float sy = static_cast<float>((0.5 - ny * 0.5) * screen_h);
                sh.sx = vp->Pos.x + (std::max)(pad, (std::min)(static_cast<float>(screen_w) - pad, sx));
                sh.sy = vp->Pos.y + (std::max)(pad, (std::min)(static_cast<float>(screen_h) - pad, sy));
                sh.nx = static_cast<float>(nx);
                sh.ny = static_cast<float>(-ny);
                shown.push_back(sh);
            }

            // Glyphs and arrows, far to near.
            for (std::size_t ci = shown.size(); ci-- > 0;)
            {
                const Shown& sh = shown[ci];
                const mdb::Cat cat = static_cast<mdb::Cat>(sh.cat);
                const ImU32 edge = IM_COL32(10, 12, 16, static_cast<int>(sh.alpha * 0.9f));
                if (sh.on_screen)
                {
                    draw_marker_glyph(dl, cat, ImVec2{sh.sx, sh.sy}, sh.found ? r * 0.75f : r, sh.col, edge,
                                      sh.found);
                    ++g_hl_debug.on_screen;
                }
                else
                {
                    const int dim = static_cast<int>(sh.alpha * 0.8f);
                    add_edge_arrow(dl, ImVec2{sh.sx, sh.sy}, sh.nx, sh.ny, r * 1.15f,
                                   marker_color_q(cat, sh.m->rarity, dim, cfg.xray_rarity_colors_enabled,
                                                  cfg.xray_rarity_colors),
                                   IM_COL32(10, 12, 16, dim));
                    ++g_hl_debug.edge;
                }
                ++g_hl_debug.drawn;
            }

            //--------------------------------------------------------------------------
            // LABELS (review-0.9.1 item 6)
            //--------------------------------------------------------------------------
            //
            // Eight pickups in one room used to produce eight name+distance boxes on the
            // same few pixels. Now: at most `highlight_labels_max` of them get a label
            // at all - chosen NEAREST FIRST, and never two for glyphs within `r * 2` of
            // each other, because two markers a few pixels apart are one thing to the
            // player - and the survivors are laid out top to bottom by the pure
            // lbl::Layout, which pushes each box down until it clears the ones already
            // placed. A box that had to move gets a leader line back to its glyph.
            if (cfg.highlight_labels && !shown.empty())
            {
                static std::vector<std::size_t> labelled; // render thread only
                labelled.clear();
                lbl::Layout layout{};
                const std::size_t label_cap = static_cast<std::size_t>(
                    (std::max)(0, (std::min)(cfg.highlight_labels_max, lbl::Layout::kMaxRects)));
                for (std::size_t i = 0; i < shown.size() && labelled.size() < label_cap; ++i)
                {
                    const Shown& sh = shown[i];
                    if (!sh.on_screen)
                    {
                        continue; // an edge arrow has no room for a name
                    }
                    if (layout.near_labelled(sh.sx, sh.sy, r * 2.0f))
                    {
                        continue;
                    }
                    layout.note_glyph(sh.sx, sh.sy);
                    labelled.push_back(i);
                }
                // Top to bottom, which is what makes pushing DOWN terminate and keeps
                // the arrangement stable from frame to frame.
                std::sort(labelled.begin(), labelled.end(), [](std::size_t a, std::size_t b) {
                    return shown[a].sy < shown[b].sy;
                });
                const float line_h = ImGui::GetTextLineHeight() + 2.0f;
                const float max_push = line_h * 8.0f;
                for (const std::size_t i : labelled)
                {
                    const Shown& sh = shown[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(sh.cat);
                    // NEVER A CLASS NAME. `mdb::display_label` refuses a label that is
                    // one (an enemy's dropped loot used to read `BP_PickupActor_C 1 m`)
                    // and falls back to the category's plain singular word.
                    const char* name = mdb::display_label(cat, sh.m->label);
                    // A STACK BUFFER, not std::format (review B.18): this ran up to
                    // twelve times per frame on the render thread, i.e. twelve heap
                    // allocations inside Present for a string nobody keeps.
                    char text[128]{};
                    (void)std::snprintf(text, sizeof(text), "%s  %.0f m%s", name, sh.dist / 100.0,
                                        sh.found ? "  (found)" : "");
                    const ImVec2 ts = ImGui::CalcTextSize(text);
                    const float want_y = sh.sy + r + 3.0f;
                    float at_y = want_y;
                    if (!layout.place(sh.sx - ts.x * 0.5f - 4.0f, want_y, ts.x + 8.0f, ts.y + 2.0f, max_push,
                                      at_y))
                    {
                        continue; // no room: the glyph speaks for itself
                    }
                    if (at_y - want_y > 2.0f)
                    {
                        // The leader line, so a displaced label still belongs to a glyph.
                        dl->AddLine(ImVec2{sh.sx, sh.sy + r}, ImVec2{sh.sx, at_y},
                                    IM_COL32(200, 206, 214, static_cast<int>(sh.alpha * 0.55f)), 1.0f);
                    }
                    draw_label(dl, ImVec2{sh.sx, at_y}, text, sh.col, sh.alpha);
                    ++g_hl_debug.labels;
                }
            }

            // THE X-RAY CENSUS IS A TRACE LINE, AND EVEN THEN ONLY WHEN IT CHANGES.
            //
            // Every gate has a number, so "the chest is on the minimap but not in the
            // x-ray" is answered by reading the log rather than by another in-game
            // session - but it is a per-Present line about a steady state, and at one
            // line per second it was 1299 of run 5's 2600 lines, half the log saying the
            // same thing. So: `trace` only, at most once per ten seconds, and only if
            // one of the counters actually moved since the last time it was printed.
            // The level check comes FIRST, so at `normal` this whole block is one
            // relaxed atomic load per Present and nothing else.
            if (mm::log_enabled(mm::LogLv::Trace))
            {
                static std::uint64_t last_log = 0;
                static std::uint64_t last_sig = 0;
                std::uint64_t sig = 0;
                for (int i = 0; i < 5; ++i)
                {
                    sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.dropped[i]);
                }
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.gated);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.considered);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.drawn);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.on_screen);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.edge);
                sig = sig * 1000003ull + static_cast<std::uint64_t>(g_hl_debug.labels);
                if (sig != last_sig && now - last_log >= 10000)
                {
                    last_log = now;
                    last_sig = sig;
                    mm::logf(L"x-ray: {} published, dropped {} by category / {} found / {} not live / "
                             L"{} out of radius ({:.0f} m); of {} left, {} drawn ({} on screen, "
                             L"{} rim arrow, {} labelled), skipped {} behind the camera, {} off "
                             L"screen with arrows off, {} faded out",
                             g_hl_debug.gated,
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Category)],
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Found)],
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Live)],
                             g_hl_debug.dropped[static_cast<int>(mdb::XrayDrop::Radius)],
                             radius / 100.0,
                             g_hl_debug.considered,
                             g_hl_debug.drawn,
                             g_hl_debug.on_screen,
                             g_hl_debug.edge,
                             g_hl_debug.labels,
                             g_hl_debug.no_projection,
                             g_hl_debug.offscreen_no_arrow,
                             g_hl_debug.faded_out);
                }
            }
        }

        //==============================================================================
        // Drawing: the COMPASS STRIP
        //==============================================================================
        //
        // A heading strip across the top of the screen. The arithmetic (wrap, bearing,
        // strip position, tick layout) is in src/compass.cpp, where markers_test can
        // reach it; this is the drawing and nothing else.
        //
        // The heading is the CAMERA's yaw when a fresh pose exists, because that is what
        // the player is looking along, and the pawn's yaw otherwise - so the compass
        // still works with highlight_enabled = 0 and during the camera reader's warm-up.
        // Which one is in use is printed in the F2 debug block.



        void draw_compass(const mm::Config& cfg, const mm::Snapshot& snap, bool gate_ok)
        {
            g_compass_debug = CompassDebug{};
            if (!cfg.overlay_enabled || !cfg.compass_enabled || !gate_ok)
            {
                return;
            }

            const std::uint64_t now = ::GetTickCount64();
            double heading = static_cast<double>(snap.yaw);
            bool from_camera = false;
            hl::Pose pose{};
            if (hl::camera(pose) && pose.stamp_ms != 0 && now - pose.stamp_ms <= 1000)
            {
                heading = pose.yaw;
                from_camera = true;
            }
            g_compass_debug.heading = cmp::wrap360(heading);
            g_compass_debug.from_camera = from_camera;

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float screen_w = vp->Size.x;
            const float screen_h = vp->Size.y;
            const float width = (std::max)(120.0f, cfg.compass_width * screen_w);
            const float height = cfg.compass_height;
            const float x0 = vp->Pos.x + (screen_w - width) * 0.5f;
            // compass_offset_y is the distance from whichever edge the strip hangs off,
            // so the key means the same thing in both directions.
            const float y0 = compass_at_bottom(cfg)
                                 ? vp->Pos.y + screen_h - cfg.compass_offset_y - height
                                 : vp->Pos.y + cfg.compass_offset_y;
            const float y1 = y0 + height;

            const float op = cfg.compass_opacity;
            const auto alpha = [op](float a) { return static_cast<int>((std::min)(1.0f, op * a) * 255.0f + 0.5f); };

            // THE HUD DRAWS UNDER OUR OWN WINDOWS (review B.20). ImGui renders the
            // background draw list first, then every window, then the foreground list -
            // so a HUD on the FOREGROUND list painted over the centred F2 panel
            // whatever order the calls were made in. The background list is still over
            // the game (everything ImGui draws is), it is just under the panel, the full
            // map and the tooltips. Suppressing the HUD while the panel is open was the
            // other option and it is worse: the panel is where the minimap's own sliders
            // live, and you cannot tune a picture you cannot see.
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            // THE PLATE. `compass_plate = 0` leaves ticks and letters only, which is what
            // the strip needs to sit lightly over the game's own top-centre HUD; with no
            // plate every glyph gets a one-pixel shadow instead, or a bright scene
            // swallows it.
            if (cfg.compass_plate)
            {
                dl->AddRectFilled(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, plate_color(alpha(0.72f)), 4.0f);
                dl->AddRect(ImVec2{x0, y0}, ImVec2{x0 + width, y1}, IM_COL32(150, 158, 168, alpha(0.55f)),
                            4.0f, 0, 1.2f);
            }
            const bool shadow = !cfg.compass_plate;
            const auto shadowed_line = [&](ImVec2 sa, ImVec2 sb, ImU32 scol, float thick) {
                if (shadow)
                {
                    dl->AddLine(ImVec2{sa.x + 1.0f, sa.y + 1.0f}, ImVec2{sb.x + 1.0f, sb.y + 1.0f},
                                IM_COL32(0, 0, 0, alpha(0.75f)), thick);
                }
                dl->AddLine(sa, sb, scol, thick);
            };
            const auto shadowed_text = [&](ImVec2 at, ImU32 scol, const char* text) {
                if (shadow)
                {
                    dl->AddText(ImVec2{at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, alpha(0.8f)), text);
                }
                dl->AddText(at, scol, text);
            };

            cmp::Strip strip{};
            strip.x0 = static_cast<double>(x0);
            strip.width = static_cast<double>(width);
            strip.heading = heading;
            strip.span = static_cast<double>(cfg.compass_span_deg);

            // Ticks. A cardinal gets the full height and its letter, an intercardinal a
            // shorter line and its two-letter label, everything else a stub.
            cmp::Tick ticks[128]{};
            const int n = cmp::ticks(strip, ticks, static_cast<int>(std::size(ticks)),
                                     static_cast<double>(cfg.compass_tick_step_deg));
            for (int i = 0; i < n; ++i)
            {
                const cmp::Tick& t = ticks[i];
                const float x = static_cast<float>(t.x);
                const float len = t.rank == 2 ? height * 0.5f : (t.rank == 1 ? height * 0.34f : height * 0.22f);
                const int a = t.rank == 2 ? alpha(0.95f) : (t.rank == 1 ? alpha(0.75f) : alpha(0.45f));
                shadowed_line(ImVec2{x, y1 - len}, ImVec2{x, y1 - 2.0f}, IM_COL32(226, 230, 236, a),
                              t.rank == 2 ? 2.0f : 1.2f);
                if (t.label[0] != '\0')
                {
                    const ImVec2 ts = ImGui::CalcTextSize(t.label);
                    shadowed_text(ImVec2{x - ts.x * 0.5f, y0 + 1.0f},
                                  IM_COL32(240, 242, 246, t.rank == 2 ? alpha(1.0f) : alpha(0.8f)),
                                  t.label);
                }
            }

            // The centre reticle: what the player is actually facing.
            {
                const float cx = x0 + width * 0.5f;
                dl->AddTriangleFilled(ImVec2{cx, y1 - 1.0f}, ImVec2{cx - 5.0f, y1 + 7.0f},
                                      ImVec2{cx + 5.0f, y1 + 7.0f}, IM_COL32(255, 236, 180, alpha(0.95f)));
            }

            if (!snap.has_pawn)
            {
                g_compass_debug.visible = true;
                return;
            }

            // Marker pips. Nearest first so the cap keeps what matters, and only inside
            // the strip's span - an off-strip pip clamped to the edge would pile up into
            // a solid block at both ends.
            if (!g_frame_cands.empty())
            {
                struct Pip
                {
                    float d2 = 0.0f; // squared: only ever compared and sorted on
                    double x = 0.0;  // where on the strip it lands, in screen px
                    float dz = 0.0f; // marker Z minus player Z, uu (signed)
                    std::uint8_t cat = 0;
                    std::uint8_t rarity = 0;
                    bool found = false;
                };
                static std::vector<Pip> pips; // render thread only
                pips.clear();
                // The one per-frame marker pass (build_frame_candidates) has already
                // walked the buffer and computed the distances; this only filters.
                const double max_d = static_cast<double>(cfg.compass_marker_distance);
                const float max_d2 = static_cast<float>(max_d * max_d);
                for (const FrameCand& fc : g_frame_cands)
                {
                    if (!mdb::cat_enabled(cfg.compass_categories, static_cast<mdb::Cat>(fc.cat)))
                    {
                        continue;
                    }
                    if (fc.d2_xy > max_d2)
                    {
                        continue;
                    }
                    const markers::DrawMarker& m = *fc.m;
                    double px = 0.0;
                    double rel = 0.0;
                    // Off-strip pips are dropped HERE rather than in the draw loop, so
                    // the cap and the dedupe below both work on pips that will actually
                    // be drawn.
                    if (!cmp::strip_x(strip, cmp::bearing_deg(snap.x, snap.y, m.x, m.y), px, rel))
                    {
                        continue;
                    }
                    Pip p{};
                    p.d2 = fc.d2_xy;
                    p.x = px;
                    p.dz = static_cast<float>(m.z - snap.z);
                    p.cat = fc.cat;
                    p.rarity = fc.rarity;
                    p.found = fc.found;
                    pips.push_back(p);
                }
                // One sort (nearest first), then drawn back to front so the nearest pip
                // ends up on top - the same trick as draw_markers.
                const std::size_t kMaxPips = static_cast<std::size_t>(cfg.compass_max_pips);
                if (pips.size() > kMaxPips)
                {
                    std::partial_sort(pips.begin(), pips.begin() + kMaxPips, pips.end(),
                                      [](const Pip& a, const Pip& b) { return a.d2 < b.d2; });
                    pips.resize(kMaxPips);
                }
                else
                {
                    std::sort(pips.begin(), pips.end(), [](const Pip& a, const Pip& b) { return a.d2 < b.d2; });
                }
                // DEDUPE. Six chests in one room are six pips within a pixel of each
                // other: a solid smear that says "chests" less clearly than one glyph
                // would. The list is sorted NEAREST FIRST, so keeping the first pip in
                // each 3-px column keeps the nearest one of every cluster - and the
                // walk is O(n x kept) over at most compass_max_pips entries.
                constexpr double kDedupePx = 3.0;
                std::size_t kept = 0;
                for (std::size_t i = 0; i < pips.size(); ++i)
                {
                    bool crowded = false;
                    for (std::size_t j = 0; j < kept; ++j)
                    {
                        if (pips[j].cat == pips[i].cat &&
                            std::abs(pips[j].x - pips[i].x) < kDedupePx)
                        {
                            crowded = true;
                            break;
                        }
                    }
                    if (!crowded)
                    {
                        pips[kept++] = pips[i];
                    }
                }
                pips.resize(kept);
                g_compass_debug.deduped = static_cast<int>(pips.size());

                // Drawn back to front, so the nearest pip of a cluster ends up on top -
                // the same trick as draw_markers.
                for (std::size_t pi = pips.size(); pi-- > 0;)
                {
                    const Pip& p = pips[pi];
                    const int a = p.found ? alpha(0.35f) : alpha(1.0f);
                    const ImU32 col = marker_color_q(static_cast<mdb::Cat>(p.cat), p.rarity, a,
                                                     cfg.markers_rarity_tint, cfg.xray_rarity_colors);
                    const ImVec2 at{static_cast<float>(p.x), y1 - height * 0.30f};
                    const float gr = height * 0.22f;
                    draw_marker_glyph(dl, static_cast<mdb::Cat>(p.cat), at, gr, col,
                                      IM_COL32(10, 12, 16, a), p.found);
                    // ABOVE / BELOW. A bearing alone sends the player at a wall when the
                    // chest is on the floor over their head, so a marker further than
                    // compass_pip_height_uu off the player's own Z gets an arrow beside
                    // its glyph. Within that band it is treated as "this floor" and
                    // nothing is drawn - an arrow on every pip would say nothing.
                    const float thr = cfg.compass_pip_height_uu;
                    if (thr > 0.0f && (p.dz > thr || p.dz < -thr))
                    {
                        const float ar = (std::max)(2.5f, height * 0.15f);
                        const float ax = at.x + gr + ar * 0.9f;
                        const float up = p.dz > 0.0f ? -1.0f : 1.0f;
                        const ImU32 acol = IM_COL32(246, 246, 250, a);
                        dl->AddTriangleFilled(ImVec2{ax, at.y + up * ar},
                                              ImVec2{ax - ar * 0.8f, at.y - up * ar * 0.55f},
                                              ImVec2{ax + ar * 0.8f, at.y - up * ar * 0.55f}, acol);
                    }
                    ++g_compass_debug.pips;
                }

                // THE DISTANCE LABELS, nearest first so a crowded strip keeps the ones
                // that matter. They sit OUTSIDE the strip (below it, or above it when
                // the strip is anchored to the bottom edge), where they cannot collide
                // with the ticks and the cardinal letters, and each one reserves its own
                // x range so two labels never overlap.
                if (cfg.compass_pip_labels && !pips.empty())
                {
                    const bool at_bottom = compass_at_bottom(cfg);
                    const float text_h = ImGui::GetTextLineHeight();
                    const float label_y = at_bottom ? y0 - text_h - 1.0f : y1 + 1.0f;
                    static std::vector<std::pair<float, float>> taken; // render thread only
                    taken.clear();
                    for (const Pip& p : pips)
                    {
                        char text[16]{};
                        ::_snprintf_s(text, sizeof(text), _TRUNCATE, "%.0fm",
                                      static_cast<double>(std::sqrt(p.d2)) / 100.0);
                        const float tw = ImGui::CalcTextSize(text).x;
                        const float lx = static_cast<float>(p.x) - tw * 0.5f;
                        const float rx = lx + tw;
                        bool crowded = false;
                        for (const std::pair<float, float>& r : taken)
                        {
                            if (lx < r.second + 2.0f && r.first < rx + 2.0f)
                            {
                                crowded = true;
                                break;
                            }
                        }
                        if (crowded)
                        {
                            continue;
                        }
                        taken.emplace_back(lx, rx);
                        const int la = p.found ? alpha(0.45f) : alpha(0.9f);
                        dl->AddText(ImVec2{lx + 1.0f, label_y + 1.0f}, IM_COL32(0, 0, 0, la), text);
                        dl->AddText(ImVec2{lx, label_y}, IM_COL32(226, 230, 236, la), text);
                    }
                }
            }

            // The waypoint is never culled: being told which way to walk while it is off
            // the strip is the whole point, so it clamps to the edge instead.
            const mv::Waypoint wp = mm::waypoint();
            if (cfg.compass_show_waypoint && wp.set)
            {
                double x = 0.0;
                double rel = 0.0;
                const bool inside = cmp::strip_x(strip, cmp::bearing_deg(snap.x, snap.y, wp.x, wp.y), x, rel);
                const float wx = static_cast<float>(x);
                const ImVec2 at{wx, y1 - height * 0.30f};
                draw_waypoint_glyph(dl, at, height * 0.24f, alpha(1.0f));
                if (!inside)
                {
                    add_edge_arrow(dl, ImVec2{wx + (rel < 0.0 ? -8.0f : 8.0f), at.y}, rel < 0.0 ? -1.0f : 1.0f,
                                   0.0f, 5.0f, IM_COL32(255, 190, 235, alpha(0.9f)),
                                   IM_COL32(10, 12, 16, alpha(0.9f)));
                }
                const double dxw = wp.x - snap.x;
                const double dyw = wp.y - snap.y;
                const double metres = std::sqrt(dxw * dxw + dyw * dyw) / 100.0;
                char text[32]{};
                if (metres >= 1000.0)
                {
                    (void)std::snprintf(text, sizeof(text), "%.1f km", metres / 1000.0);
                }
                else
                {
                    (void)std::snprintf(text, sizeof(text), "%.0f m", metres);
                }
                draw_label(dl, ImVec2{wx, y1 + 9.0f}, text, IM_COL32(255, 190, 235, alpha(1.0f)), alpha(1.0f));
                ++g_compass_debug.pips;
            }

            g_compass_debug.visible = true;
        }

        //==============================================================================
        // Drawing: the FULL MAP (step C1)
        //==============================================================================
        //
        // Toggled by `map_key` (M). While it is open the minimap is hidden, a dark
        // backdrop covers the scene and one ImGui window holds the map: north-up,
        // pannable and zoomable, drawn from the SAME height-sliced asset the minimap
        // uses, with every marker on it.
        //
        // WHAT IS SHARED WITH THE MINIMAP, DELIBERATELY
        //   * the asset (mapdata::HeightMaps) - one copy in RAM, ~327 MB, and the map
        //     adds nothing to it: it cuts a second small dynamic texture out of the same
        //     planes rather than keeping a picture of its own;
        //   * the height-slice rule and its config (floor_z_tolerance / floor_fade_uu /
        //     the gradient / the base colour), so the two never disagree about what a
        //     floor is - the only addition is a floor OFFSET the player can nudge;
        //   * the marker draw buffer, the category filter mask and the glyphs.
        //
        // WHAT IS DIFFERENT
        //   * the cut is DECIMATED (one texture pixel covers `step` source pixels) and
        //     covers the visible viewport plus a 30 % margin, not the whole chapter;
        //   * it only re-cuts when something changed - a pan that leaves the cut region,
        //     a zoom, a floor change, or a big player move - capped at map_slice_hz. An
        //     idle open map costs nothing per frame beyond the draw;
        //   * it is always north-up. A rotating full map is unreadable and every
        //     reference implementation avoids it.
        //
        // NO LATCHES (lessons.md). The map closes itself the moment the state that
        // allows it stops being true - a menu opening, the pawn going away, a level
        // transition - and closing gives the mouse and the keyboard straight back to the
        // game on the same frame, because the swallow condition IS `g_map_open`.

        // The published found flag with the render side's own pending toggle applied.
        // An override is dropped as soon as the published buffer says the same thing,
        // so this can never latch: at worst it holds for one marker round (~1 s).
        bool marker_found_now(const markers::DrawMarker& m)
        {
            const bool published = (m.flags & markers::kFlagFound) != 0;
            for (std::size_t i = 0; i < g_found_override.size(); ++i)
            {
                if (g_found_override[i].first != m.id)
                {
                    continue;
                }
                if (g_found_override[i].second == published)
                {
                    g_found_override.erase(g_found_override.begin() + static_cast<std::ptrdiff_t>(i));
                    return published;
                }
                return g_found_override[i].second;
            }
            return published;
        }

        void toggle_found(const markers::DrawMarker& m)
        {
            if (m.id[0] == '\0')
            {
                return;
            }
            const bool want = !marker_found_now(m);
            for (auto& kv : g_found_override)
            {
                if (kv.first == m.id)
                {
                    kv.second = want;
                    markers::request_toggle_found(m.id, want);
                    return;
                }
            }
            if (g_found_override.size() < 512)
            {
                g_found_override.emplace_back(std::string{m.id}, want);
            }
            markers::request_toggle_found(m.id, want);
        }

        // The full map's texture size for a given canvas. Pure geometry, so both the
        // render thread (which allocates) and the loop thread (which cuts) can derive
        // the same numbers from the same request.
        void map_slice_size(const mm::Config& cfg, const mv::Rect& canvas, int& tw, int& th)
        {
            const double kMargin = kMapSliceMargin;
            const double zoom = g_mv.uu_per_px;
            const double want_w_uu = static_cast<double>(canvas.w()) * zoom * kMargin;
            const double want_h_uu = static_cast<double>(canvas.h()) * zoom * kMargin;

            tw = static_cast<int>(std::lround(static_cast<double>(canvas.w()) * kMargin));
            if (tw > cfg.map_slice_px)
            {
                tw = cfg.map_slice_px;
            }
            tw = (tw / 8) * 8;
            if (tw < 64)
            {
                tw = 64;
            }
            // One step for both axes (a non-square pixel would shear the picture), so
            // the height follows from it rather than from the aspect ratio directly.
            const double step = (want_w_uu * static_cast<double>(tw) > 0.0)
                                    ? (want_w_uu / static_cast<double>(tw))
                                    : 1.0;
            th = step > 0.0 ? static_cast<int>(std::lround(want_h_uu / step)) : 64;
            th = (th / 8) * 8;
            if (th < 64)
            {
                th = 64;
            }
            if (th > cfg.map_slice_px * 2)
            {
                th = (cfg.map_slice_px * 2 / 8) * 8;
            }
        }

        // RENDER THREAD. Size and allocate the full map's buffers and publish what the
        // slicer should cut. The cut runs on the loop thread - see slice_map_step().
        // Returns true while a buffer is available to draw.
        bool plan_map_slice(const mm::Config& cfg, const mapdata::Chapter& ch, const mv::Rect& canvas,
                            float feet, std::uint64_t now)
        {
            if (!ch.has_heights() || canvas.w() < 8.0f || canvas.h() < 8.0f)
            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
                return false;
            }

            int tw = 0;
            int th = 0;
            map_slice_size(cfg, canvas, tw, th);

            if (g_mslice[0].w != tw || g_mslice[0].h != th || g_mslice[0].tex == nullptr)
            {
                // A failing allocation must not retry (and log) once per frame - the
                // same "back a failing blind path off" rule the navmesh scan learned.
                static std::uint64_t create_failed_ms = 0;
                if (create_failed_ms != 0 && now - create_failed_ms < 5000)
                {
                    return false;
                }
                if (!slicer_pause_begin(kSlicerPauseMs))
                {
                    return map_slice_view().shown >= 0; // try again next frame
                }
                wait_for_gpu(); // the old buffers may still be in flight
                const bool ok = create_slice_set(g_mslice, kMapSliceBufs, tw, th, L"full map");
                if (!ok)
                {
                    destroy_map_slice_buffers();
                }
                else
                {
                    for (int i = 0; i < kMapSliceBufs; ++i)
                    {
                        g_mslice_copy_pending[i].store(false);
                        g_mslice_in_flight[i].store(0);
                    }
                    g_mslice_next = 0;
                    clear_map_slice_view();
                    note_slice_buffers_changed();
                }
                slicer_pause_end();
                if (!ok)
                {
                    create_failed_ms = now;
                    return false;
                }
                create_failed_ms = 0;
            }

            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = true;
                g_map_req.cx = g_mv.cx;
                g_map_req.cy = g_mv.cy;
                g_map_req.zoom = g_mv.uu_per_px;
                g_map_req.canvas_w = canvas.w();
                g_map_req.canvas_h = canvas.h();
                g_map_req.feet = feet;
                g_map_req.show_all_floors = cfg.map_show_all_floors;
            }
            g_map_req_ms.store(now, std::memory_order_relaxed);
            return map_slice_view().shown >= 0;
        }

        // LOOP THREAD. Cut the visible region (plus a margin) into the map's own dynamic
        // texture, if anything changed and the next buffer is free.
        void slice_map_step(std::uint64_t now)
        {
            MapSliceReq req{};
            {
                spin::SpinGuard guard(g_slice_req_lock);
                req = g_map_req;
            }
            if (!req.wanted || now - g_map_req_ms.load(std::memory_order_relaxed) > 500)
            {
                return; // the map is closed, or the render thread stopped asking
            }

            mm::Snapshot snap{};
            if (!mm::read_snapshot(snap))
            {
                return;
            }
            // Re-read the planes on EVERY cut: a chapter switch retires them and frees
            // them after a grace period. mapdata's retire runs on this same thread, so a
            // pointer read here cannot be freed while the cut is running.
            const mapdata::Chapter* chp = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (chp == nullptr || !chp->has_heights())
            {
                return;
            }
            const mapdata::Chapter& ch = *chp;
            const mapdata::HeightMaps& hm = *ch.heights;
            if (hm.px_per_uu <= 0.0)
            {
                return;
            }

            if (g_map_recut.exchange(false, std::memory_order_acq_rel))
            {
                g_mr_valid = false; // the map was recentred: the old region says nothing
            }

            const mm::Config& cfg = mm::cfg_cached();
            SliceBuf& b = g_mslice[g_mslice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0 || b.h <= 0)
            {
                return;
            }

            // The step the render thread's sizing implies, recomputed from the buffer
            // that actually exists.
            const double kMargin = kMapSliceMargin;
            const double want_w_uu = static_cast<double>(req.canvas_w) * req.zoom * kMargin;
            const double step = want_w_uu * hm.px_per_uu / static_cast<double>(b.w);
            if (!(step > 0.0))
            {
                return;
            }

            // The world rectangle this cut will cover, derived from the texture so the
            // drawn quad matches the pixels exactly.
            const double half_w = static_cast<double>(b.w) * step / hm.px_per_uu * 0.5;
            const double half_h = static_cast<double>(b.h) * step / hm.px_per_uu * 0.5;

            // Does the visible viewport still sit inside the region we already cut?
            const double view_half_x = static_cast<double>(req.canvas_h) * req.zoom * 0.5;
            const double view_half_y = static_cast<double>(req.canvas_w) * req.zoom * 0.5;
            const bool inside = g_mr_valid && req.cx - view_half_x >= g_mr_x0 &&
                                req.cx + view_half_x <= g_mr_x1 && req.cy - view_half_y >= g_mr_y0 &&
                                req.cy + view_half_y <= g_mr_y1;
            const bool feet_moved = !g_mr_valid || std::abs(req.feet - g_mr_feet) > 20.0f;
            const bool zoomed = !g_mr_valid || req.zoom != g_mr_zoom;
            const bool chapter_changed = g_mr_chapter != ch.key;
            const bool resized = g_mr_w != b.w || g_mr_h != b.h;
            const bool urgent = !inside || zoomed || chapter_changed || resized;
            if (!urgent && !feet_moved)
            {
                return;
            }

            // The rate cap. An urgent cut (the view has left the region, or the zoom
            // changed) still has to wait for the buffer, but not for the clock: showing
            // an empty edge is worse than one extra cut.
            const int period = cfg.map_slice_hz > 0 ? 1000 / cfg.map_slice_hz : 166;
            if (!urgent && g_mslice_last_ms != 0 &&
                now - g_mslice_last_ms < static_cast<std::uint64_t>(period))
            {
                return;
            }

            const std::uint64_t in_flight = g_mslice_in_flight[g_mslice_next].load(std::memory_order_acquire);
            ID3D12Fence* fence = g_fence;
            if (in_flight != 0 && fence != nullptr && fence->GetCompletedValue() < in_flight)
            {
                ++g_mslice_skipped;
                return; // the GPU is still sampling it; keep showing the other buffer
            }

            SliceStyle st = style_from(cfg);
            if (req.show_all_floors)
            {
                // "Show everything": no surface is ever out of range, so the whole
                // chapter's walkable area is on screen with the current storey still
                // picked out at full opacity.
                st.fade = 1.0e9f;
                st.a_dim = 0.34f;
                st.a_faint = 0.24f;
            }

            const double x1 = req.cx + half_h; // north edge
            const double y0 = req.cy - half_w; // west edge
            const double src_x0 = (y0 - hm.min_y) * hm.px_per_uu;
            const double src_y0 = (hm.max_x - x1) * hm.px_per_uu;

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            slice_region(hm, src_x0, src_y0, step, b.w, b.h, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, req.feet, st, g_mslice_scratch, g_mslice_counts);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            double g_mslice_last_cut_ms = 0.0;
            if (freq > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq);
                g_mslice_last_cut_ms = ms;
                g_mslice_ms = g_mslice_ms == 0.0 ? ms : g_mslice_ms * 0.7 + ms * 0.3;
                if (ms > g_mslice_ms_peak)
                {
                    g_mslice_ms_peak = ms;
                }
            }

            g_mr_x0 = req.cx - half_h;
            g_mr_x1 = x1;
            g_mr_y0 = y0;
            g_mr_y1 = req.cy + half_w;
            g_mr_zoom = req.zoom;
            g_mr_feet = req.feet;
            g_mr_chapter = ch.key;
            g_mr_w = b.w;
            g_mr_h = b.h;
            g_mr_valid = true;

            if (g_pf_mslice < 0)
            {
                g_pf_mslice = mm::perf_register("full map cut", perf::Thread::Loop);
            }
            mm::perf_record_ms(g_pf_mslice, g_mslice_last_cut_ms);

            g_mslice_copy_pending[g_mslice_next].store(true, std::memory_order_release);
            {
                spin::SpinGuard guard(g_slice_view_lock);
                g_mslice_view.shown = g_mslice_next;
                g_mslice_view.valid = true;
                g_mslice_view.x0 = g_mr_x0;
                g_mslice_view.x1 = g_mr_x1;
                g_mslice_view.y0 = g_mr_y0;
                g_mslice_view.y1 = g_mr_y1;
            }
            g_mslice_next = (g_mslice_next + 1) % kMapSliceBufs;
            g_mslice_last_ms = now;
            ++g_mslice_updates;
        }

        void draw_waypoint_glyph(ImDrawList* dl, ImVec2 p, float r, int alpha)
        {
            const ImU32 col = IM_COL32(255, 92, 210, alpha);
            // A 1 Hz pulse ring. The waypoint is the one marker that is never culled and
            // never dimmed, and on a screen of static glyphs the eye finds the moving
            // one - which is the whole job of a waypoint. Phase comes from the wall
            // clock, so every view (minimap, full map, compass) pulses together.
            {
                const float phase =
                    static_cast<float>(::GetTickCount64() % 1000) / 1000.0f;
                const float rad = r * (1.2f + 1.1f * phase);
                const int a = static_cast<int>(static_cast<float>(alpha) * 0.55f * (1.0f - phase));
                if (a > 3)
                {
                    dl->AddCircle(p, rad, IM_COL32(255, 92, 210, a), 18, 1.6f);
                }
            }
            const ImU32 edge = IM_COL32(20, 8, 18, static_cast<int>(alpha * 0.9f));
            const ImVec2 tip{p.x, p.y + r * 1.5f};
            const ImVec2 l{p.x - r * 0.75f, p.y + r * 0.25f};
            const ImVec2 rr{p.x + r * 0.75f, p.y + r * 0.25f};
            dl->AddTriangleFilled(l, rr, tip, col);
            dl->AddCircleFilled(ImVec2{p.x, p.y - r * 0.15f}, r * 0.85f, col, 14);
            dl->AddCircle(ImVec2{p.x, p.y - r * 0.15f}, r * 0.85f, edge, 14, 1.4f);
            dl->AddCircleFilled(ImVec2{p.x, p.y - r * 0.15f}, r * 0.3f, edge, 8);
        }

        // Closes the map and says why, exactly once per transition.
        void close_map(const wchar_t* why)
        {
            if (!mm::g_map_open.exchange(false))
            {
                return;
            }
            // The Stats panel belongs to the map mode, so it goes with it - otherwise it
            // would be left drawn over the game with nothing swallowing the input.
            g_stats_page = false;
            g_shrine_panel = false;
            g_shot_canvas_valid = false;
            MM_LOGV(L"full map closed: {}", why);
        }

        void draw_full_map(mm::Config cfg, const mm::Snapshot& snap, bool have_state, float ui_scale)
        {
            const mm::Config before = cfg;
            const std::uint64_t now = ::GetTickCount64();
            ImGuiIO& io = ImGui::GetIO();
            const ImGuiViewport* vp = ImGui::GetMainViewport();

            //--------------------------------------------------------------------------
            // The gate. Every condition is re-evaluated from the live snapshot on every
            // frame and closes the map outright - there is nothing here that can latch.
            //--------------------------------------------------------------------------
            if (!have_state || snap.stamp_ms == 0 ||
                now - snap.stamp_ms > static_cast<std::uint64_t>(cfg.state_stale_ms))
            {
                close_map(L"no fresh game-state snapshot");
                return;
            }
            if (snap.transition)
            {
                close_map(L"a level transition started");
                return;
            }
            if (!snap.has_pawn || !snap.pawn_is_gameplay)
            {
                close_map(L"there is no gameplay pawn");
                return;
            }
            if (cfg.hide_in_menus && snap.menu_open)
            {
                close_map(L"a game menu opened");
                return;
            }

            //--------------------------------------------------------------------------
            // Geometry and the backdrop
            //--------------------------------------------------------------------------
            const float margin = cfg.map_margin * vp->Size.y;
            const mv::Rect frame{vp->Pos.x + margin,
                                 vp->Pos.y + margin,
                                 vp->Pos.x + vp->Size.x - margin,
                                 vp->Pos.y + vp->Size.y - margin};

            ImDrawList* back = ImGui::GetBackgroundDrawList();
            back->AddRectFilled(vp->Pos,
                                ImVec2{vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y},
                                IM_COL32(3, 5, 8, static_cast<int>(cfg.map_backdrop * 255.0f + 0.5f)));

            const mapdata::Chapter* chapter_ptr = mapdata::chapter_ptr_for(snap.x, snap.y);

            //--------------------------------------------------------------------------
            // First frame after opening: centre on the player, reset the zoom and the
            // floor offset, and drop any gamepad edges from while it was closed.
            //--------------------------------------------------------------------------
            // DPI AND THE MAP'S ZOOM (review B.12). This view gets the UNSCALED config -
            // the legend's filter chips write back into it - so the ui-scale factor the
            // minimap's zoom key gets through ui_scaled() is applied here at the point of
            // use instead. Same promise: one config file shows the same area of the world
            // at 1080p and at 2160p.
            // 1/ui_scale, not ui_scale: the canvas is `ui_scale` times as many pixels
            // across, so uu-per-pixel has to come down by the same factor for the view to
            // cover the same ground. See the derivation in ui_scaled().
            const float zscale = (cfg.zoom_dpi_scaled && ui_scale > 0.0f) ? 1.0f / ui_scale : 1.0f;
            if (!g_mv_init)
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_mv.uu_per_px = mv::clamp_zoom(static_cast<double>(cfg.map_zoom * zscale),
                                                static_cast<double>(cfg.map_zoom_min * zscale),
                                                static_cast<double>(cfg.map_zoom_max * zscale));
                g_map_floor_off = 0.0f;
                g_map_recut.store(true, std::memory_order_release);
                g_mv_init = true;
                pad::clear_pressed();
                g_map_recenter.store(false, std::memory_order_relaxed);
            }

            ImGui::SetNextWindowPos(ImVec2{frame.x0, frame.y0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{frame.w(), frame.h()}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.97f);
            constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                ImGuiWindowFlags_NoSavedSettings;
            if (!ImGui::Begin("##wuchang_full_map", nullptr, kFlags))
            {
                ImGui::End();
                return;
            }

            //--------------------------------------------------------------------------
            // Header + the category filter (the SAME mask the minimap and the F2 panel
            // use, so a filter toggled here is toggled everywhere)
            //--------------------------------------------------------------------------
            ImGui::Text("Wuchang map");
            ImGui::SameLine();
            // The floor offset in METRES, and named as a storey delta: `floor +200 uu`
            // was a number in the engine's unit that meant nothing to a player standing
            // one storey up. 1 uu = 1 cm.
            ImGui::TextDisabled("%s   |   %.0f uu/px   |   floor %+.1f m   |   X %.0f  Y %.0f",
                                chapter_ptr != nullptr ? chapter_ptr->key.c_str() : "no chapter here",
                                g_mv.uu_per_px,
                                static_cast<double>(g_map_floor_off) / 100.0,
                                snap.x,
                                snap.y);
            ImGui::SameLine((std::max)(200.0f, ImGui::GetWindowWidth() -
                                                   ImGui::CalcTextSize("FitRecentreClose").x - 90.0f));
            // ZOOM TO FIT. The chapter's bounds are in the manifest, so this is the one
            // view control that cannot be reached by panning and zooming by hand.
            bool want_fit = ImGui::SmallButton("Fit");
            ImGui::SameLine();
            if (ImGui::SmallButton("Stats"))
            {
                g_stats_page = !g_stats_page;
            }
            ImGui::SameLine();
            if (cfg.shrine_list && ImGui::SmallButton("Shrines"))
            {
                g_shrine_panel = !g_shrine_panel;
            }
            if (cfg.shrine_list)
            {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Recentre"))
            {
                g_map_recenter.store(true, std::memory_order_relaxed);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Close"))
            {
                close_map(L"the Close button");
            }

            //--------------------------------------------------------------------------
            // The canvas, with the legend column reserved on its right
            //--------------------------------------------------------------------------
            //
            // The 14 saturated `SmallButton`s that used to sit here (black text on a
            // filled category colour, no wrapping, no counts) are gone: the LEGEND is
            // the filter now. It says what each glyph means, how many of that category
            // this chapter has and how many are found, and clicking a row toggles it -
            // which is three answers from the space one of them used to take.
            const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            // Sized from the text, so it is right at every ui_scale.
            const float legend_w =
                ImGui::CalcTextSize("      Fog gates   9999/9999").x + ImGui::GetStyle().FramePadding.x * 4.0f;
            const ImVec2 csize{(std::max)(64.0f, avail.x - legend_w - ImGui::GetStyle().ItemSpacing.x),
                               (std::max)(64.0f, avail.y - footer_h)};
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##canvas", csize, ImGuiButtonFlags_MouseButtonLeft);
            const bool canvas_hovered = ImGui::IsItemHovered();
            const bool canvas_active = ImGui::IsItemActive();
            const mv::Rect canvas{cpos.x, cpos.y, cpos.x + csize.x, cpos.y + csize.y};
            // What the screenshot key copies. Recorded every frame the map draws, on
            // the same thread that reads it, so no synchronisation is involved - and
            // cleared by close_map, which is what makes "the map is not open" a
            // reportable failure instead of a copy of whatever was there last.
            g_shot_canvas = canvas;
            g_shot_canvas_valid = true;

            //--------------------------------------------------------------------------
            // The legend, which IS the category filter
            //--------------------------------------------------------------------------
            //
            // One row per category: the glyph as it is actually drawn on the map, the
            // name, and `found / total` from markers::stats() - counted for the CHAPTER
            // in force when the marker filter is on, because a whole-DB total would
            // count five chapters the player cannot see. Clicking a row toggles that
            // category in `markers_categories`, the same mask the minimap, the compass
            // and the F2 chips share.
            ImGui::SameLine();
            if (ImGui::BeginChild("##legend", ImVec2{legend_w, csize.y}, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoSavedSettings))
            {
                const markers::Stats lst = markers::stats();
                const int fch = lst.filter_chapter;
                const bool per_chapter = fch >= 0 && fch <= 8;
                ImGui::TextDisabled(per_chapter ? "legend - chapter" : "legend - all chapters");
                ImDrawList* ldl = ImGui::GetWindowDrawList();
                const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.34f);
                for (int i = 0; i < mdb::kCatCount; ++i)
                {
                    const mdb::Cat cat = static_cast<mdb::Cat>(i);
                    const bool on = mdb::cat_enabled(cfg.markers_categories, cat);
                    const markers::CatStat& cs =
                        per_chapter ? lst.chapter[fch][i] : lst.cat[i];
                    const ImVec2 row = ImGui::GetCursorScreenPos();
                    ImGui::PushID(i);
                    // The leading spaces are the glyph's gutter: the glyph is drawn over
                    // the row afterwards, so a Selectable still owns the whole width and
                    // the hit area is the row, not the text.
                    //
                    // CACHED (review B.18). These fourteen strings were fourteen
                    // std::format calls - fourteen heap allocations - on the render
                    // thread on every frame the map was open, for text that changes when
                    // a marker is found (about once a minute) or the chapter changes.
                    // The cache is keyed on exactly what the text is made of.
                    static char row_text[mdb::kCatCount][64]{};
                    static int row_found[mdb::kCatCount]{};
                    static int row_total[mdb::kCatCount]{};
                    static bool row_valid[mdb::kCatCount]{};
                    if (!row_valid[i] || row_found[i] != cs.found || row_total[i] != cs.total)
                    {
                        row_valid[i] = true;
                        row_found[i] = cs.found;
                        row_total[i] = cs.total;
                        if (cs.total > 0)
                        {
                            (void)std::snprintf(row_text[i], sizeof(row_text[i]), "      %s   %d/%d",
                                                mdb::cat_label(cat), cs.found, cs.total);
                        }
                        else
                        {
                            (void)std::snprintf(row_text[i], sizeof(row_text[i]), "      %s",
                                                mdb::cat_label(cat));
                        }
                    }
                    const char* const text = row_text[i];
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          on ? marker_color(cat, 255) : IM_COL32(150, 150, 150, 170));
                    if (ImGui::Selectable(text, on))
                    {
                        cfg.markers_categories ^= mdb::cat_bit(cat);
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    const ImVec2 at{row.x + glyph_r + 4.0f, row.y + ImGui::GetTextLineHeight() * 0.5f};
                    draw_marker_glyph(ldl, cat, at, glyph_r, marker_color(cat, on ? 255 : 90),
                                      IM_COL32(14, 16, 20, on ? 220 : 80));
                }
                ImGui::Spacing();
                if (ImGui::SmallButton("all"))
                {
                    cfg.markers_categories = mdb::kAllCats;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("none"))
                {
                    cfg.markers_categories = 0u;
                }
                ImGui::SameLine();
                bool show_found = !cfg.markers_hide_found;
                if (ImGui::Checkbox("found", &show_found))
                {
                    cfg.markers_hide_found = !show_found;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("show markers already found");
                }
            }
            ImGui::EndChild();

            const double zmin = static_cast<double>(cfg.map_zoom_min * zscale);
            const double zmax = static_cast<double>(cfg.map_zoom_max * zscale);
            g_mv.uu_per_px = mv::clamp_zoom(g_mv.uu_per_px, zmin, zmax);

            //--------------------------------------------------------------------------
            // Input: mouse
            //--------------------------------------------------------------------------
            static float drag_px = 0.0f;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canvas_hovered)
            {
                drag_px = 0.0f;
            }
            if (canvas_active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const ImVec2 d = io.MouseDelta;
                drag_px += std::abs(d.x) + std::abs(d.y);
                // Keep the world point under the cursor under the cursor.
                g_mv.cx += static_cast<double>(d.y) * g_mv.uu_per_px;
                g_mv.cy -= static_cast<double>(d.x) * g_mv.uu_per_px;
            }

            const auto zoom_about = [&](float sx, float sy, double notches) {
                double wx = 0.0;
                double wy = 0.0;
                mv::screen_to_world(g_mv, canvas, sx, sy, wx, wy);
                g_mv.uu_per_px =
                    mv::zoom_by(g_mv.uu_per_px, notches, static_cast<double>(cfg.map_zoom_factor), zmin, zmax);
                // Solve the inverse for the centre so that (wx, wy) lands on (sx, sy)
                // again: it is the same transform, read the other way round.
                g_mv.cx = wx + (static_cast<double>(sy) - static_cast<double>(canvas.cy())) * g_mv.uu_per_px;
                g_mv.cy = wy - (static_cast<double>(sx) - static_cast<double>(canvas.cx())) * g_mv.uu_per_px;
            };

            if (canvas_hovered && io.MouseWheel != 0.0f)
            {
                if (io.KeyCtrl)
                {
                    g_map_floor_off += io.MouseWheel * cfg.map_floor_step;
                }
                else
                {
                    zoom_about(io.MousePos.x, io.MousePos.y, static_cast<double>(io.MouseWheel));
                }
            }

            //--------------------------------------------------------------------------
            // Input: keyboard. ImGui sees these because the WndProc hook feeds it every
            // message BEFORE deciding to swallow it.
            //--------------------------------------------------------------------------
            const float dt = io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : 1.0f / 60.0f;
            const double pan_uu = static_cast<double>(cfg.map_pan_speed) * static_cast<double>(dt) * g_mv.uu_per_px;
            const auto down = [](ImGuiKey a, ImGuiKey b) { return ImGui::IsKeyDown(a) || ImGui::IsKeyDown(b); };
            if (down(ImGuiKey_W, ImGuiKey_UpArrow))
            {
                g_mv.cx += pan_uu; // screen up is world +X (north)
            }
            if (down(ImGuiKey_S, ImGuiKey_DownArrow))
            {
                g_mv.cx -= pan_uu;
            }
            if (down(ImGuiKey_D, ImGuiKey_RightArrow))
            {
                g_mv.cy += pan_uu; // screen right is world +Y (east)
            }
            if (down(ImGuiKey_A, ImGuiKey_LeftArrow))
            {
                g_mv.cy -= pan_uu;
            }
            if (down(ImGuiKey_Equal, ImGuiKey_KeypadAdd))
            {
                zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(dt) * 6.0);
            }
            if (down(ImGuiKey_Minus, ImGuiKey_KeypadSubtract))
            {
                zoom_about(canvas.cx(), canvas.cy(), -static_cast<double>(dt) * 6.0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, true) || ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
            {
                g_map_floor_off += cfg.map_floor_step;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Q, true) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
            {
                g_map_floor_off -= cfg.map_floor_step;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                close_map(L"Escape");
            }
            // Home = Fit, the same action as the header button. Safe as a bare key: the
            // full map swallows the keyboard for as long as it is open (lessons.md), so
            // nothing here can leak into the game.
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
            {
                want_fit = true;
            }
            // F1 / H (and pad Back, below) toggle the controls legend. One long
            // TextDisabled sentence in the footer was unreadable and could not grow.
            //
            // NOT `?`: the footer and the legend both advertised `?` while the code
            // tested ImGuiKey_Slash, i.e. the UNSHIFTED key - so on a keyboard where `?`
            // needs Shift (every US/UK layout) the advertised gesture opened nothing and
            // an undocumented one did. `?` is a CHARACTER, not a key, and ImGui's key
            // enum has no portable name for it, so the honest fix is to bind keys that
            // can be named: F1 (the universal help key) and H. Both are safe bare keys
            // here - the full map is a MODE and swallows the whole keyboard for as long
            // as it is open (lessons.md) - and F1 is not in the F6/F9/F10/F11/F12
            // minefield this machine's other injected DLLs own. `/` stays wired as an
            // unadvertised third route so nobody's muscle memory breaks.
            if (ImGui::IsKeyPressed(ImGuiKey_F1, false) || ImGui::IsKeyPressed(ImGuiKey_H, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Slash, false))
            {
                g_map_help = !g_map_help;
            }
            // Keyboard equivalents of the two mouse actions, at the view centre. They
            // exist because the mouse cursor is the one part of this that depends on
            // what the game does with the cursor while we hold the input - with these
            // (and the gamepad) the map is fully usable if it turns out the cursor
            // cannot be moved freely.
            bool key_waypoint = ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            bool key_toggle = ImGui::IsKeyPressed(ImGuiKey_F, false);

            //--------------------------------------------------------------------------
            // Input: gamepad. The state is polled on the LOOP thread (gamepad.cpp) - as
            // with the keyboard, nothing here touches XInput or the game thread.
            //--------------------------------------------------------------------------
            bool pad_waypoint = false;
            bool pad_toggle = false;
            const pad::State gp = pad::state();
            if (cfg.map_gamepad && gp.connected)
            {
                if (gp.lx != 0.0f || gp.ly != 0.0f)
                {
                    g_mv.cx += static_cast<double>(gp.ly) * pan_uu;
                    g_mv.cy += static_cast<double>(gp.lx) * pan_uu;
                }
                const float trig = gp.rt - gp.lt;
                if (trig > 0.02f || trig < -0.02f)
                {
                    zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(trig * dt) * 8.0);
                }
                if (gp.ry > 0.02f || gp.ry < -0.02f)
                {
                    zoom_about(canvas.cx(), canvas.cy(), static_cast<double>(gp.ry * dt) * 6.0);
                }
                const std::uint16_t pressed = pad::take_pressed();
                if ((pressed & pad::kRightShoulder) != 0)
                {
                    g_map_floor_off += cfg.map_floor_step;
                }
                if ((pressed & pad::kLeftShoulder) != 0)
                {
                    g_map_floor_off -= cfg.map_floor_step;
                }
                if ((pressed & pad::kY) != 0)
                {
                    g_map_recenter.store(true, std::memory_order_relaxed);
                }
                if ((pressed & pad::kA) != 0)
                {
                    pad_waypoint = true;
                }
                if ((pressed & pad::kX) != 0)
                {
                    pad_toggle = true;
                }
                if ((pressed & pad::kB) != 0)
                {
                    close_map(L"the gamepad B button");
                }
                if ((pressed & pad::kBack) != 0)
                {
                    g_map_help = !g_map_help;
                }
            }

            if (g_map_recenter.exchange(false, std::memory_order_relaxed))
            {
                g_mv.cx = snap.x;
                g_mv.cy = snap.y;
                g_map_floor_off = 0.0f;
            }
            // FIT. The chapter's bounds come from maps.json, and mv::fit_zoom() spends
            // world X on the canvas height and world Y on its width (the map is
            // north-up) - crossing those over gives a fit that is right on a square
            // canvas and wrong on a 16:9 one.
            if (want_fit)
            {
                if (chapter_ptr == nullptr)
                {
                    mm::log(L"full map: Fit needs a chapter, and none covers this position");
                }
                else
                {
                    const double z = mv::fit_zoom(chapter_ptr->max_x - chapter_ptr->min_x,
                                                  chapter_ptr->max_y - chapter_ptr->min_y,
                                                  static_cast<double>(csize.x),
                                                  static_cast<double>(csize.y));
                    if (z > 0.0)
                    {
                        g_mv.uu_per_px = mv::clamp_zoom(z, zmin, zmax);
                        g_mv.cx = (chapter_ptr->min_x + chapter_ptr->max_x) * 0.5;
                        g_mv.cy = (chapter_ptr->min_y + chapter_ptr->max_y) * 0.5;
                        g_map_floor_off = 0.0f;
                        g_map_recut.store(true, std::memory_order_release);
                    }
                }
            }
            g_map_floor_off = (std::max)(-20000.0f, (std::min)(20000.0f, g_map_floor_off));

            //--------------------------------------------------------------------------
            // The map picture
            //--------------------------------------------------------------------------
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1}, true);
            dl->AddRectFilled(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1},
                              IM_COL32(9, 12, 16, 255));

            const float feet = static_cast<float>(snap.z) - cfg.player_z_offset + g_map_floor_off;
            bool have_picture = false;
            const bool map_slice_ok =
                chapter_ptr != nullptr && plan_map_slice(cfg, *chapter_ptr, canvas, feet, now);
            const MapSliceView msv = map_slice_view();
            if (map_slice_ok && msv.shown >= 0 && msv.valid)
            {
                const SliceBuf& b = g_mslice[msv.shown];
                float sx0 = 0.0f;
                float sy0 = 0.0f;
                float sx1 = 0.0f;
                float sy1 = 0.0f;
                // Top-left of the cut region is its NORTH-WEST corner (max X, min Y).
                mv::world_to_screen(g_mv, canvas, msv.x1, msv.y0, sx0, sy0);
                mv::world_to_screen(g_mv, canvas, msv.x0, msv.y1, sx1, sy1);
                const ImTextureRef tex{static_cast<ImTextureID>(b.srv_gpu.ptr)};
                dl->AddImage(tex, ImVec2{sx0, sy0}, ImVec2{sx1, sy1}, ImVec2{0.0f, 0.0f}, ImVec2{1.0f, 1.0f},
                             IM_COL32(255, 255, 255, 255));
                have_picture = true;
            }

            //--------------------------------------------------------------------------
            // Markers
            //--------------------------------------------------------------------------
            const markers::View mv_all = markers::view();
            g_map_markers_drawn = 0;
            g_map_markers_total = static_cast<int>(mv_all.count);
            const markers::DrawMarker* hover = nullptr;
            const markers::DrawMarker* centre_marker = nullptr;
            float hover_d2 = 0.0f;
            float centre_d2 = 0.0f;
            // The one pixel key this view owns. `cfg` here is the UNSCALED config (the
            // filter chips write back into it), so the UI scale is applied at the point
            // of use rather than through ui_scaled().
            const float mr = cfg.map_marker_size * ui_scale;
            const float pick_r = (std::max)(8.0f, mr * 1.6f);
            const float kCentrePickR = (std::max)(48.0f, mr * 5.0f);

            if (cfg.markers_enabled && mv_all.data != nullptr)
            {
                //----------------------------------------------------------------------
                // TWO PASSES, NOT ONE (review B.16)
                //----------------------------------------------------------------------
                //
                // 1.0.0 walked the published buffer in DB ORDER and stopped at
                // map_markers_max_draw. Two consequences: with more markers than the cap
                // the ones dropped were whichever the database happened to list last -
                // so zooming out lost markers at random rather than the far ones - and
                // nothing merged, so a room with six chests in it was a smear.
                //
                // So: collect what is on screen, order it by distance from the VIEW
                // CENTRE (which is what the player is looking at, and is also what the
                // Fit button and the recentre key aim), cap that, merge coincident glyphs
                // of the same category, and only then draw. All three buffers are static
                // and reused - this runs inside Present (review B.18).
                struct MapCand
                {
                    float sx = 0.0f;
                    float sy = 0.0f;
                    float cd2 = 0.0f; // squared distance from the canvas centre
                    const markers::DrawMarker* m = nullptr;
                    bool found = false;
                    int count = 1;
                };
                static std::vector<MapCand> cands;
                cands.clear();
                if (cands.capacity() < mv_all.count)
                {
                    cands.reserve(mv_all.count);
                }
                for (std::size_t i = 0; i < mv_all.count; ++i)
                {
                    const markers::DrawMarker& m = mv_all.data[i];
                    const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                    if (static_cast<int>(m.cat) >= mdb::kCatCount ||
                        !mdb::cat_enabled(cfg.markers_categories, cat))
                    {
                        continue;
                    }
                    const bool found = marker_found_now(m);
                    if (found && cfg.markers_hide_found)
                    {
                        continue;
                    }
                    float sx = 0.0f;
                    float sy = 0.0f;
                    mv::world_to_screen(g_mv, canvas, m.x, m.y, sx, sy);
                    if (sx < canvas.x0 - mr || sx > canvas.x1 + mr || sy < canvas.y0 - mr || sy > canvas.y1 + mr)
                    {
                        continue;
                    }
                    MapCand c{};
                    c.sx = sx;
                    c.sy = sy;
                    const float cdx = sx - canvas.cx();
                    const float cdy = sy - canvas.cy();
                    c.cd2 = cdx * cdx + cdy * cdy;
                    c.m = &m;
                    c.found = found;
                    cands.push_back(c);
                }

                // NEAREST THE CENTRE FIRST. partial_sort leaves [0, cap) sorted, which
                // is all the draw order needs.
                const std::size_t cap = cfg.map_markers_max_draw > 0
                                            ? static_cast<std::size_t>(cfg.map_markers_max_draw)
                                            : cands.size();
                const auto nearer = [](const MapCand& a, const MapCand& b) { return a.cd2 < b.cd2; };
                if (cands.size() > cap)
                {
                    std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap),
                                      cands.end(), nearer);
                    cands.resize(cap);
                }
                else
                {
                    std::sort(cands.begin(), cands.end(), nearer);
                }

                // MERGE, same category and same found state only - see MergeGrid.
                {
                    static MergeGrid grid;
                    grid.reset(canvas.x0 - mr, canvas.y0 - mr, canvas.w() + mr * 2.0f,
                               canvas.h() + mr * 2.0f, (std::max)(3.0f, mr));
                    std::size_t kept = 0;
                    for (std::size_t i = 0; i < cands.size(); ++i)
                    {
                        const MapCand& c = cands[i];
                        const int key = grid.find(c.sx, c.sy, static_cast<int>(c.m->cat));
                        if (key >= 0 && cands[static_cast<std::size_t>(key)].found == c.found)
                        {
                            ++cands[static_cast<std::size_t>(key)].count;
                            continue;
                        }
                        grid.add(c.sx, c.sy, static_cast<int>(c.m->cat), static_cast<int>(kept));
                        cands[kept++] = c;
                    }
                    cands.resize(kept);
                }

                // DRAWN FAR TO NEAR, so the marker nearest what the player is looking at
                // ends up on top - the same trick the minimap uses.
                for (std::size_t ci = cands.size(); ci-- > 0;)
                {
                    const MapCand& c = cands[ci];
                    const markers::DrawMarker& m = *c.m;
                    const mdb::Cat cat = static_cast<mdb::Cat>(m.cat);
                    const int alpha =
                        static_cast<int>((c.found ? cfg.markers_found_alpha : 1.0f) * 255.0f + 0.5f);
                    draw_marker_glyph(dl, cat, ImVec2{c.sx, c.sy}, mr,
                                      marker_color_q(cat, m.rarity, alpha, cfg.markers_rarity_tint,
                                                     cfg.xray_rarity_colors),
                                      IM_COL32(14, 16, 20, static_cast<int>(alpha * 0.85f)), c.found);
                    draw_count_badge(dl, ImVec2{c.sx, c.sy}, mr, c.count, alpha);
                    ++g_map_markers_drawn;

                    const float mdx = c.sx - io.MousePos.x;
                    const float mdy = c.sy - io.MousePos.y;
                    const float d2 = mdx * mdx + mdy * mdy;
                    if (canvas_hovered && d2 <= pick_r * pick_r && (hover == nullptr || d2 < hover_d2))
                    {
                        hover = &m;
                        hover_d2 = d2;
                    }
                    // The keyboard / gamepad "toggle found" acts on the marker nearest
                    // the CENTRE of the view - but only within the same radius a mouse
                    // would have to be in, so it can never reach a marker on the far
                    // side of the screen.
                    if (c.cd2 <= kCentrePickR * kCentrePickR && (centre_marker == nullptr || c.cd2 < centre_d2))
                    {
                        centre_marker = &m;
                        centre_d2 = c.cd2;
                    }
                }
            }

            //--------------------------------------------------------------------------
            // The waypoint and the player
            //--------------------------------------------------------------------------
            draw_found_rings(dl, now, (std::max)(5.0f, mr * 1.4f),
                             [&](double wx, double wy, float& sx, float& sy) {
                                 mv::world_to_screen(g_mv, canvas, wx, wy, sx, sy);
                                 return canvas.contains(sx, sy);
                             });
            const mv::Waypoint wp = mm::waypoint();
            if (wp.set)
            {
                float sx = 0.0f;
                float sy = 0.0f;
                mv::world_to_screen(g_mv, canvas, wp.x, wp.y, sx, sy);
                if (canvas.contains(sx, sy))
                {
                    draw_waypoint_glyph(dl, ImVec2{sx, sy}, mr * 1.1f, 255);
                }
            }
            {
                float sx = 0.0f;
                float sy = 0.0f;
                mv::world_to_screen(g_mv, canvas, snap.x, snap.y, sx, sy);
                if (canvas.contains(sx, sy))
                {
                    // The view cone first, so the arrow sits on top of it.
                    const float a = snap.yaw * kPi / 180.0f;
                    const float len = 46.0f;
                    const float half = 32.0f * kPi / 180.0f;
                    const ImVec2 c{sx, sy};
                    const auto dir = [&](float ang) {
                        return ImVec2{c.x + std::sin(ang) * len, c.y - std::cos(ang) * len};
                    };
                    dl->AddTriangleFilled(c, dir(a - half), dir(a + half), IM_COL32(255, 226, 92, 46));
                    add_player_arrow(dl, c, snap.yaw, 11.0f);
                }
            }

            dl->PopClipRect();
            dl->AddRect(ImVec2{canvas.x0, canvas.y0}, ImVec2{canvas.x1, canvas.y1},
                        IM_COL32(120, 130, 145, 160), 0.0f, 0, 1.5f);

            //--------------------------------------------------------------------------
            // Clicks (after the draw, so `hover` is known)
            //--------------------------------------------------------------------------
            if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && drag_px < 5.0f &&
                hover != nullptr)
            {
                toggle_found(*hover);
            }
            // SET, or CLEAR when the same gesture lands on the waypoint that is
            // already there. Clearing used to be possible only from the F2 panel, which
            // means leaving the map to undo something done on the map.
            const auto set_or_clear = [&](double wx, double wy) {
                const mv::Waypoint had = mm::waypoint();
                if (had.set)
                {
                    float hx = 0.0f;
                    float hy = 0.0f;
                    float cx2 = 0.0f;
                    float cy2 = 0.0f;
                    mv::world_to_screen(g_mv, canvas, had.x, had.y, hx, hy);
                    mv::world_to_screen(g_mv, canvas, wx, wy, cx2, cy2);
                    const float ddx = hx - cx2;
                    const float ddy = hy - cy2;
                    if (ddx * ddx + ddy * ddy <= pick_r * pick_r)
                    {
                        mm::set_waypoint(mv::Waypoint{});
                        toast("waypoint cleared");
                        return;
                    }
                }
                mv::Waypoint set{};
                set.set = true;
                set.x = wx;
                set.y = wy;
                set.z = static_cast<double>(feet);
                mm::set_waypoint(set);
                toast("waypoint set");
            };
            if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                double wx = 0.0;
                double wy = 0.0;
                mv::screen_to_world(g_mv, canvas, io.MousePos.x, io.MousePos.y, wx, wy);
                set_or_clear(wx, wy);
            }
            if (pad_waypoint || key_waypoint)
            {
                set_or_clear(g_mv.cx, g_mv.cy);
            }
            if ((pad_toggle || key_toggle) && centre_marker != nullptr)
            {
                toggle_found(*centre_marker);
            }

            //--------------------------------------------------------------------------
            // Hover tooltip
            //--------------------------------------------------------------------------
            if (hover != nullptr)
            {
                ImGui::BeginTooltip();
                const mdb::Cat cat = static_cast<mdb::Cat>(hover->cat);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(marker_color(cat, 255)), "%s",
                                   mdb::display_label(cat, hover->label));
                ImGui::Text("category: %s", mdb::cat_name(cat));
                // The stable id is a path (`Chapter1_DGong_logic/BP_treasurebox_C_12`).
                // It is the join key with the live actors and is exactly what a bug
                // report needs - and it is noise to everyone else, so it lives behind
                // the debug switch now.
                if (cfg.debug_readout)
                {
                    ImGui::TextDisabled("%s", hover->id);
                }
                const double ddx = hover->x - snap.x;
                const double ddy = hover->y - snap.y;
                const double ddz = hover->z - snap.z;
                ImGui::Text("%s   %.0f m away, %+0.0f m up",
                            marker_found_now(*hover) ? "FOUND" : "not found",
                            std::sqrt(ddx * ddx + ddy * ddy) / 100.0,
                            ddz / 100.0);
                ImGui::TextDisabled("left-click toggles found");
                ImGui::EndTooltip();
            }

            //--------------------------------------------------------------------------
            // The shrine list (the `Shrines` button in the header)
            //--------------------------------------------------------------------------
            if (g_shrine_panel && cfg.shrine_list)
            {
                const ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(620.0f * ui_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Shrines", &g_shrine_panel,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    draw_shrine_list(cfg, snap, have_state, markers::stats().filter_chapter);
                    ImGui::Spacing();
                    if (ImGui::Button("Close"))
                    {
                        g_shrine_panel = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The collection statistics panel (the `Stats` button in the header)
            //--------------------------------------------------------------------------
            //
            // A child window over the map rather than a separate top-level one: the map
            // is a MODE that owns the whole screen and swallows the input, so a floating
            // window the player cannot reach with a normal Alt+Tab-style focus change
            // would be a trap. It closes with the same button, with Esc, and with the map.
            if (g_stats_page)
            {
                const ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Appearing,
                                        ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(720.0f * ui_scale, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Collection", &g_stats_page,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
                {
                    draw_collection_stats(::GetTickCount64(), false);
                    ImGui::Spacing();
                    if (ImGui::Button("Close"))
                    {
                        g_stats_page = false;
                    }
                }
                ImGui::End();
            }

            //--------------------------------------------------------------------------
            // The controls legend (`?` / pad Back)
            //--------------------------------------------------------------------------
            //
            // What used to be one long TextDisabled sentence in the footer. Two columns,
            // built from the CONFIG (so a rebound key is what the player is told), with
            // the gamepad column present only while a pad is actually connected -
            // telling a keyboard player about LB/RB is noise, and XInput already knows
            // the answer.
            if (g_map_help)
            {
                struct Row
                {
                    std::string control;
                    std::string action;
                };
                // BUILT ONCE, NOT PER FRAME (review B.18). This is ~28 strings in two
                // vectors, i.e. ~30 heap allocations on the render thread inside Present,
                // for text whose content only changes when a binding changes or a pad is
                // plugged in. The vectors are static (so their capacity survives too) and
                // the signature below is exactly what the text is made of.
                struct HelpKey
                {
                    int panel = 0;
                    int map = 0;
                    int recenter = 0;
                    int zoom = 0;
                    int reload = 0;
                    int shot = 0;
                    int highlight = 0;
                    bool hl_on = false;
                    bool hl_hold = false;
                    bool pad = false;
                    // Defaulted, not memcmp: padding bytes in an aggregate are
                    // unspecified, and a spurious "changed" here would silently put the
                    // per-frame allocations back (the same trap as review B.19).
                    bool operator==(const HelpKey&) const = default;
                };
                const HelpKey want{cfg.panel_key,     cfg.map_key,
                                   cfg.map_recenter_key, cfg.zoom_key,
                                   cfg.reload_key,    cfg.screenshot_key,
                                   cfg.highlight_key, cfg.highlight_enabled,
                                   cfg.highlight_mode == mm::HighlightMode::Hold,
                                   cfg.map_gamepad && gp.connected};
                static std::vector<Row> left;
                static std::vector<Row> right;
                static HelpKey have{};
                static bool built = false;
                const bool rebuild = !built || !(want == have);
                const auto add = [](std::vector<Row>& into, std::string c, std::string a) {
                    into.push_back(Row{std::move(c), std::move(a)});
                };
                if (rebuild)
                {
                built = true;
                have = want;
                left.clear();
                right.clear();
                add(left, "mouse / keyboard", "");
                add(left, "drag, WASD, arrows", "pan");
                add(left, "wheel, + / -", "zoom");
                add(left, "ctrl+wheel, Q / E", "floor down / up");
                add(left, "Home", "zoom to fit the chapter");
                add(left, key_name_ascii(cfg.map_recenter_key), "recentre on the player");
                add(left, "right-click, Space", "set a waypoint (again on it clears)");
                add(left, "left-click, F", "toggle found");
                add(left, "click a legend row", "filter that category");
                add(left, key_name_ascii(cfg.screenshot_key), "copy the map to the clipboard");
                add(left, "Shrines", "shrine list (click = waypoint, double-click = centre)");
                add(left, "Stats", "collection statistics");
                add(left, "F1 or H", "this legend");
                add(left, key_name_ascii(cfg.map_key) + ", Esc", "close the map");
                if (cfg.map_gamepad && gp.connected)
                {
                    add(right, "gamepad", "");
                    add(right, "left stick", "pan");
                    add(right, "triggers, right stick", "zoom");
                    add(right, "LB / RB", "floor down / up");
                    add(right, "A", "set a waypoint (again on it clears)");
                    add(right, "X", "toggle found");
                    add(right, "Y", "recentre");
                    add(right, "Back", "this legend");
                    add(right, "B", "close the map");
                    add(right, "", "");
                }
                add(right, "outside the map", "");
                add(right, key_name_ascii(cfg.panel_key), "settings panel");
                add(right, key_name_ascii(cfg.zoom_key), "cycle the minimap zoom");
                add(right, key_name_ascii(cfg.reload_key), "reload config, maps and markers");
                if (cfg.highlight_enabled)
                {
                    add(right,
                        (cfg.highlight_mode == mm::HighlightMode::Hold ? "hold " : "press ") +
                            key_name_ascii(cfg.highlight_key),
                        "x-ray nearby markers");
                }
                if (cfg.map_gamepad && cfg.map_pad_open_chord != 0)
                {
                    add(right, wide_to_ascii(mm::pad_chord_name(cfg.map_pad_open_chord, false, false)),
                        "open / close the map (pad)");
                }
                } // rebuild

                const float line = ImGui::GetTextLineHeightWithSpacing();
                const float pad_px = ImGui::GetTextLineHeight();
                float ctrl_w[2] = {0.0f, 0.0f};
                float act_w[2] = {0.0f, 0.0f};
                const std::vector<Row>* cols[2] = {&left, &right};
                for (int c = 0; c < 2; ++c)
                {
                    for (const Row& row : *cols[c])
                    {
                        ctrl_w[c] = (std::max)(ctrl_w[c], ImGui::CalcTextSize(row.control.c_str()).x);
                        act_w[c] = (std::max)(act_w[c], ImGui::CalcTextSize(row.action.c_str()).x);
                    }
                }
                const float gap = pad_px;
                const float col_w[2] = {ctrl_w[0] + gap + act_w[0], ctrl_w[1] + gap + act_w[1]};
                const float box_w = col_w[0] + col_w[1] + pad_px * 3.0f;
                const std::size_t rows =
                    (std::max)(left.size(), right.size());
                const float box_h = static_cast<float>(rows) * line + pad_px * 2.0f;
                const ImVec2 tl{canvas.cx() - box_w * 0.5f, canvas.cy() - box_h * 0.5f};
                dl->AddRectFilled(tl, ImVec2{tl.x + box_w, tl.y + box_h}, plate_color(235), 5.0f);
                dl->AddRect(tl, ImVec2{tl.x + box_w, tl.y + box_h}, IM_COL32(150, 158, 168, 200), 5.0f, 0,
                            1.4f);
                for (int c = 0; c < 2; ++c)
                {
                    const float x = tl.x + pad_px + (c == 1 ? col_w[0] + pad_px : 0.0f);
                    float y = tl.y + pad_px;
                    for (const Row& row : *cols[c])
                    {
                        // A row with no action is a heading, and is the only thing in
                        // here drawn bright.
                        const bool heading = row.action.empty();
                        if (!row.control.empty())
                        {
                            dl->AddText(ImVec2{x, y},
                                        heading ? IM_COL32(255, 226, 160, 255) : IM_COL32(226, 230, 236, 235),
                                        row.control.c_str());
                        }
                        if (!heading)
                        {
                            dl->AddText(ImVec2{x + ctrl_w[c] + gap, y}, IM_COL32(180, 186, 196, 220),
                                        row.action.c_str());
                        }
                        y += line;
                    }
                }
            }

            //--------------------------------------------------------------------------
            // Footer
            //--------------------------------------------------------------------------
            if (!have_picture)
            {
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f},
                                   chapter_ptr == nullptr
                                       ? "no chapter covers this position - markers only"
                                       : "no height maps for this chapter - markers only");
            }
            else
            {
                ImGui::TextDisabled("F1 or H (or pad Back) shows the controls   %s or Esc closes the map",
                                    key_name_ascii(cfg.map_key).c_str());
            }
            ImGui::TextDisabled("%d of %d marker(s)   cut %dx%d @ %.2f ms%s", g_map_markers_drawn,
                                g_map_markers_total, g_mslice[0].w, g_mslice[0].h, g_mslice_ms,
                                cfg.map_gamepad && gp.connected ? "   gamepad connected" : "");

            ImGui::End();

            // Field by field (review B.19), not memcmp: a Config is a value, and
            // `mm::operator==` is generated from the struct with a byte-flip drift
            // guard behind it in markers_test.
            if (before != cfg)
            {
                mm::set_config(cfg);
            }
        }

        //==============================================================================
        // Drawing: the F2 panel
        //==============================================================================

        //==============================================================================
        // The per-activity performance table
        //==============================================================================
        //
        // Every periodic activity in this mod records into perf.hpp's counter table;
        // this prints it. The review of v0.9.1 found two costs that had been invisible
        // for weeks (the 4 Hz widget sweep at 28-51 ms, and publish_round, never timed
        // at all) by reading code - this is so the next one is found by looking.
        //
        // The columns: how often it runs, what an average invocation costs, the worst
        // one since the peaks were last reset, the most recent one, and which thread
        // pays. `avg` and `Hz` are over a rolling window (perf::kWindowMs), so they
        // react instead of being diluted by the whole session.
        void draw_perf_table()
        {
            const perf::Table& pt = mm::perf_table();
            if (pt.count == 0)
            {
                return;
            }
            const std::uint64_t now = ::GetTickCount64();
            if (!ImGui::CollapsingHeader("Performance (per activity)"))
            {
                return;
            }
            if (ImGui::SmallButton("reset peaks"))
            {
                mm::perf_reset_peaks();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("a peak from a loading screen otherwise hides every later one");
            // WHAT `peak ms` MEANS. All three of this mod's threads measure wall clock,
            // so a sample taken while the game thread is inside a synchronous load, or
            // while the swapchain is being resized, or while the mod is doing a one-off
            // blocking job, is time spent WAITING - and one of those hides every later
            // regression behind it. Those samples are counted in `stalls` instead, with
            // their own worst case, and nothing is thrown away: hover a peak for the raw
            // one that includes them.
            ImGui::TextDisabled("peak ms = the worst sample OUTSIDE a load / resize / one-off job; "
                                "the rest are counted under stalls (hover a peak for the raw one)");
            {
                char why[128]{};
                ::WideCharToMultiByte(CP_UTF8, 0, mm::perf_last_stall(), -1, why, sizeof(why) - 1, nullptr,
                                      nullptr);
                ImGui::TextDisabled("last stall: %s%s", why, mm::perf_in_stall() ? " (now)" : "");
            }

            if (ImGui::BeginTable("perf", 7,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("activity");
                ImGui::TableSetupColumn("Hz");
                ImGui::TableSetupColumn("avg ms");
                ImGui::TableSetupColumn("peak ms");
                ImGui::TableSetupColumn("stalls");
                ImGui::TableSetupColumn("last ms");
                ImGui::TableSetupColumn("thread");
                ImGui::TableHeadersRow();
                for (int i = 0; i < pt.count; ++i)
                {
                    const perf::Counter& c = pt.c[i];
                    const bool is_idle = perf::idle(c, now);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", c.name != nullptr ? c.name : "?");
                    ImGui::TableNextColumn();
                    if (is_idle)
                    {
                        ImGui::TextDisabled("idle");
                    }
                    else
                    {
                        ImGui::Text("%.1f", c.rate_hz);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", c.avg_ms);
                    ImGui::TableNextColumn();
                    // The one number worth colouring: anything over a millisecond on a
                    // periodic path is a frame-time or game-thread problem.
                    if (c.peak_calm_ms >= 4.0)
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%.3f", c.peak_calm_ms);
                    }
                    else if (c.peak_calm_ms >= 1.0)
                    {
                        ImGui::TextColored(ImVec4{1.0f, 0.85f, 0.4f, 1.0f}, "%.3f", c.peak_calm_ms);
                    }
                    else
                    {
                        ImGui::Text("%.3f", c.peak_calm_ms);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("raw peak including stalls: %.3f ms", c.peak_ms);
                    }
                    ImGui::TableNextColumn();
                    if (c.stalls == 0)
                    {
                        ImGui::TextDisabled("-");
                    }
                    else
                    {
                        ImGui::TextDisabled("%llu / %.0f ms",
                                            static_cast<unsigned long long>(c.stalls),
                                            c.peak_stall_ms);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", c.last_ms);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", perf::thread_name(c.thread));
                }
                ImGui::EndTable();
            }
        }

        //==============================================================================
        // Category chips
        //==============================================================================
        //
        // Three places used to draw fourteen `ImGui::Checkbox`es in a hand-computed grid
        // (the minimap filter, the x-ray filter, the compass filter) - forty-two
        // checkboxes in one window, all identical, none of them telling you what colour
        // the category is on the map.
        //
        // A chip is a SmallButton filled with the category's own glyph colour when the
        // category is on and drawn as a flat outline when it is off, so the row doubles
        // as the legend. `base_id` keeps the three rows apart: ImGui identifies a widget
        // by its label, and three rows of "Chest" in one window would be one widget
        // (that is the PushID rule in lessons.md, and it is why each row passes its own
        // base).
        bool category_chips(std::uint32_t& mask, int base_id, float wrap_width)
        {
            bool changed = false;
            const ImGuiStyle& style = ImGui::GetStyle();
            float x = 0.0f;
            for (int i = 0; i < mdb::kCatCount; ++i)
            {
                const mdb::Cat cat = static_cast<mdb::Cat>(i);
                const bool on = mdb::cat_enabled(mask, cat);
                // A GUTTER FOR THE GLYPH. The chip's own shape is the half of a
                // category's identity that survives at map scale, so the filter shows it
                // rather than only the colour: the button's label is padded on the left
                // and the glyph is drawn into that gap afterwards.
                const float glyph_r = (std::max)(4.0f, ImGui::GetTextLineHeight() * 0.30f);
                const float gutter = glyph_r * 2.0f + 4.0f;
                const char* label = mdb::cat_label(cat);
                const float w = ImGui::CalcTextSize(label).x + gutter + style.FramePadding.x * 4.0f;
                if (i > 0)
                {
                    if (x + w < wrap_width)
                    {
                        ImGui::SameLine();
                    }
                    else
                    {
                        x = 0.0f;
                    }
                }
                x += w + style.ItemSpacing.x;

                const ImU32 col = marker_color(cat, 255);
                const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(col);
                // Black text on a saturated fill, the category's own colour as a thin
                // outline when off. The luminance test is what stops a yellow chip from
                // getting white text nobody can read.
                const float lum = 0.299f * fill.x + 0.587f * fill.y + 0.114f * fill.z;
                const ImVec4 text_col = on ? (lum > 0.55f ? ImVec4{0.06f, 0.06f, 0.06f, 1.0f}
                                                          : ImVec4{1.0f, 1.0f, 1.0f, 1.0f})
                                           : ImVec4{fill.x, fill.y, fill.z, 0.75f};
                const ImVec4 bg = on ? fill : ImVec4{fill.x * 0.18f, fill.y * 0.18f, fill.z * 0.18f, 0.55f};
                ImGui::PushID(base_id + i);
                ImGui::PushStyleColor(ImGuiCol_Button, bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4{fill.x * 0.75f, fill.y * 0.75f, fill.z * 0.75f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
                ImGui::PushStyleColor(ImGuiCol_Text, text_col);
                const std::string padded = std::string(static_cast<std::size_t>(
                                               gutter / (std::max)(1.0f, ImGui::CalcTextSize(" ").x)) + 1,
                                           ' ') + label;
                if (ImGui::SmallButton(padded.c_str()))
                {
                    mask = on ? (mask & ~mdb::cat_bit(cat)) : (mask | mdb::cat_bit(cat));
                    changed = true;
                }
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                draw_marker_glyph(ImGui::GetWindowDrawList(), cat,
                                  ImVec2{rmin.x + style.FramePadding.x + glyph_r,
                                         (rmin.y + rmax.y) * 0.5f},
                                  glyph_r, on ? IM_COL32(20, 22, 26, 235) : marker_color(cat, 210),
                                  on ? IM_COL32(235, 238, 242, 200) : IM_COL32(14, 16, 20, 160));
                ImGui::PopStyleColor(4);
                ImGui::PopID();
            }
            return changed;
        }

        // ONE CATEGORY FILTER, all three of them identical: a title, `all` / `none`, the
        // config key it writes, then the chip grid. Three of these exist (the map and
        // minimap share one mask, the compass has its own, the x-ray has its own) and a
        // player has to be able to tell at a glance which is which - so the title says
        // what the filter is FOR, not what the key is called.
        void category_filter(const char* title, const char* key, std::uint32_t& mask, int base_id,
                             float wrap_width)
        {
            ImGui::PushID(base_id);
            ImGui::TextUnformatted(title);
            ImGui::SameLine();
            if (ImGui::SmallButton("all"))
            {
                mask = mdb::kAllCats;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("none"))
            {
                mask = 0u;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", key);
            ImGui::PopID();
            category_chips(mask, base_id + 1, wrap_width);
        }

        //==============================================================================
        // Player presets
        //==============================================================================
        //
        // Three named starting points, each setting SEVERAL Player keys at once, so the

        void apply_preset(mm::Config& cfg, Preset which)
        {
            const std::uint32_t chest = mdb::cat_bit(mdb::Cat::Chest);
            const std::uint32_t pickup = mdb::cat_bit(mdb::Cat::Pickup);
            const std::uint32_t hidden = mdb::cat_bit(mdb::Cat::Hidden);
            const std::uint32_t shrine = mdb::cat_bit(mdb::Cat::Shrine);
            const std::uint32_t boss = mdb::cat_bit(mdb::Cat::Boss);
            const std::uint32_t elite = mdb::cat_bit(mdb::Cat::Elite);
            const std::uint32_t fog = mdb::cat_bit(mdb::Cat::FogGate);
            const std::uint32_t npc = mdb::cat_bit(mdb::Cat::Npc);
            const std::uint32_t note = mdb::cat_bit(mdb::Cat::Note);
            const std::uint32_t door = mdb::cat_bit(mdb::Cat::Door);
            const std::uint32_t ladder = mdb::cat_bit(mdb::Cat::Ladder);
            const std::uint32_t lift = mdb::cat_bit(mdb::Cat::Lift);

            cfg.overlay_enabled = true;
            cfg.markers_enabled = true;
            cfg.highlight_enabled = true;
            switch (which)
            {
            case Preset::Minimal:
                // As little as possible while still being a map: a small disc, only the
                // landmarks you navigate by, nothing you have already taken, no compass.
                cfg.show_minimap = true;
                cfg.size_frac = 0.16f;
                cfg.zoom_uu_per_px = 30.0f;
                cfg.opacity = 0.85f;
                cfg.markers_categories = shrine | boss | fog;
                cfg.markers_hide_found = true;
                cfg.markers_clamp_to_edge = false;
                cfg.compass_enabled = false;
                cfg.highlight_categories = chest | pickup;
                break;
            case Preset::Loot:
                // Sweeping a level for what is left in it: a close zoom, only loot on
                // the map, found things gone, everything that is off the disc kept on
                // its rim, and the x-ray tuned wide with the quality colours on.
                cfg.show_minimap = true;
                cfg.size_frac = 0.24f;
                cfg.zoom_uu_per_px = 20.0f;
                cfg.opacity = 0.92f;
                cfg.markers_categories = chest | pickup | hidden;
                cfg.markers_hide_found = true;
                cfg.markers_clamp_to_edge = true;
                cfg.compass_enabled = true;
                cfg.compass_categories = chest | pickup;
                cfg.highlight_categories = chest | pickup;
                cfg.highlight_radius = 5000.0f;
                cfg.highlight_labels = true;
                cfg.xray_rarity_colors_enabled = true;
                break;
            case Preset::Exploration:
            default:
                // Learning the level: a wide view, every landmark and every connection
                // (doors, ladders, lifts), found markers still drawn so you can see
                // where you have been. Enemies stay off - the sweep only refreshes them
                // once a second, so they lag while they move.
                cfg.show_minimap = true;
                cfg.size_frac = 0.28f;
                cfg.zoom_uu_per_px = 40.0f;
                cfg.opacity = 0.92f;
                cfg.markers_categories = mdb::kAllCats & ~mdb::cat_bit(mdb::Cat::Enemy);
                cfg.markers_hide_found = false;
                cfg.markers_clamp_to_edge = true;
                cfg.compass_enabled = true;
                cfg.compass_categories = shrine | boss | elite | fog | door | ladder | lift | npc;
                // `note` is in the x-ray set: a readable sign is exactly the thing you
                // want pointed out while you are standing in front of one.
                cfg.highlight_categories = chest | pickup | shrine | boss | npc | note;
                break;
            }
        }

        //==============================================================================
        // The F2 panel: Player / Advanced / Debug
        //==============================================================================
        //
        // Until 0.9.2 this was one flat window with fourteen category checkboxes three
        // times over, scan-chunk sliders, POV offsets and fence counters - a developer
        // console with the player's settings mixed into it. The three tabs below are the
        // §2 classification of the config made visible: the Player tab is the Player
        // tier, the Advanced tab is the Advanced tier, and the Debug tab is the Dev tier
        // plus every read-only diagnostic. NOTHING was dropped in the move; the
        // diagnostics that used to sit inside the Markers / Full map / X-ray / Compass
        // headers are on the Debug tab now.
        //
        // Each tab is its own function for a reason beyond tidiness: MSVC counts nested
        // blocks and C1061'd this file once already (lessons.md).

        // ONE PLAYER-TAB SECTION EACH. They were one 300-line function with
        // SeparatorText between the blocks; the user asked for the Advanced tab's
        // collapsible sections here too, and a CollapsingHeader has to be able to
        // SKIP its contents - which a separator cannot. Splitting the blocks into
        // functions is what makes that possible without wrapping 300 lines in an if.
        //
        // The open state is not persisted, exactly as on the Advanced tab: ImGui's ini
        // file is disabled (io.IniFilename = nullptr), so every header opens at its
        // default - open here, because a player tab that starts collapsed hides the
        // settings it exists for.

        void player_presets(mm::Config& cfg)
        {
            ImGui::TextDisabled("set several of the settings on this tab at once");
            if (ImGui::Button("Minimal HUD"))
            {
                apply_preset(cfg, Preset::Minimal);
            }
            ImGui::SameLine();
            if (ImGui::Button("Loot hunting"))
            {
                apply_preset(cfg, Preset::Loot);
            }
            ImGui::SameLine();
            if (ImGui::Button("Exploration"))
            {
                apply_preset(cfg, Preset::Exploration);
            }
        }

        //--------------------------------------------------------------------------
        // Look: theme and palette
        //--------------------------------------------------------------------------
        //
        // In the FILE a theme only fills in colours the file does not mention; in
        // the PANEL choosing one is an explicit act, so it writes the theme's
        // colours into the five colour keys there and then (they are on the Advanced
        // tab, and the change is visible on the next frame). Anything else would
        // make the combo look broken for a player whose config happens to spell one
        // of those keys out.
        void player_look(mm::Config& cfg)
        {
            int theme_i = static_cast<int>(cfg.theme);
            const char* themes[] = {"neutral", "ink"};
            if (ImGui::Combo("Theme (frame / backdrop / plates / fill)", &theme_i, themes, 2))
            {
                cfg.theme = static_cast<gly::Theme>(theme_i);
                const gly::ThemeColors tc = gly::theme_colors(cfg.theme);
                cfg.minimap_frame_r = static_cast<float>(tc.frame.r);
                cfg.minimap_frame_g = static_cast<float>(tc.frame.g);
                cfg.minimap_frame_b = static_cast<float>(tc.frame.b);
                cfg.minimap_frame_alpha = tc.frame_alpha;
                cfg.minimap_backdrop_r = static_cast<float>(tc.backdrop.r);
                cfg.minimap_backdrop_g = static_cast<float>(tc.backdrop.g);
                cfg.minimap_backdrop_b = static_cast<float>(tc.backdrop.b);
                cfg.minimap_backdrop = tc.backdrop_alpha;
                cfg.floor_base_r = static_cast<float>(tc.floor_base.r);
                cfg.floor_base_g = static_cast<float>(tc.floor_base.g);
                cfg.floor_base_b = static_cast<float>(tc.floor_base.b);
            }
            int pal_i = static_cast<int>(cfg.palette);
            const char* pals[] = {"default", "colorblind"};
            if (ImGui::Combo("Marker palette", &pal_i, pals, 2))
            {
                const gly::Palette was = cfg.palette;
                cfg.palette = static_cast<gly::Palette>(pal_i);
                // The item-quality tiers follow the palette only while they are still
                // the OTHER palette's set - a hand-picked xray_rarity_colors survives.
                bool untouched = true;
                const mdb::Rgb* old_set = gly::rarity_colors(was);
                for (int i = 0; i < mdb::kRarityCount; ++i)
                {
                    untouched = untouched && cfg.xray_rarity_colors[i] == old_set[i];
                }
                if (untouched)
                {
                    const mdb::Rgb* now = gly::rarity_colors(cfg.palette);
                    for (int i = 0; i < mdb::kRarityCount; ++i)
                    {
                        cfg.xray_rarity_colors[i] = now[i];
                    }
                }
            }
            ImGui::TextDisabled("every category has its own glyph shape");
        }

        //--------------------------------------------------------------------------
        // Minimap
        //--------------------------------------------------------------------------
        void player_minimap(mm::Config& cfg)
        {
            // THE ONE LINE THAT ANSWERS "why is the minimap not there", on the tab a
            // PLAYER actually opens. It used to live only on the Debug tab, which since
            // 0.9.2 is hidden unless the unshipped dev config turns it on - so the
            // diagnostic the whole show/hide design exists to produce was invisible to
            // everyone it was written for. Only shown while the minimap is hidden;
            // saying "minimap: shown" over a visible minimap is noise.
            if (!g_last_mini.visible)
            {
                char reason[192]{};
                ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr,
                                      nullptr);
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f}, "hidden because: %s", reason);
            }
            ImGui::Checkbox("Overlay enabled", &cfg.overlay_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show minimap", &cfg.show_minimap);
            ImGui::SliderFloat("Size (fraction of screen height)", &cfg.size_frac, 0.08f, 0.6f, "%.2f");
            ImGui::SliderFloat("Zoom (uu per minimap pixel)", &cfg.zoom_uu_per_px, 4.0f, 200.0f, "%.0f");
            ImGui::SliderFloat("Opacity", &cfg.opacity, 0.15f, 1.0f, "%.2f");

            bool round_shape = cfg.round;
            if (ImGui::Checkbox("Round (off = square)", &round_shape))
            {
                cfg.round = round_shape;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Rotate with player (off = north up)", &cfg.rotate_with_player);
            ImGui::Checkbox("Show the floor below / above (dimmed)", &cfg.show_adjacent_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Hide while a menu is open", &cfg.hide_in_menus);
            ImGui::SliderFloat("Floor Z tolerance (uu)", &cfg.floor_z_tolerance, 20.0f, 800.0f, "%.0f");
        }

        //--------------------------------------------------------------------------
        // Placement and scale
        //--------------------------------------------------------------------------
        void player_placement(mm::Config& cfg)
        {
            // ONE key that moves the whole HUD. `custom` keeps the three placement keys
            // below in force; anything else overrides the minimap's corner and puts the
            // compass on the same vertical side.
            int preset = static_cast<int>(cfg.hud_preset);
            const char* presets[] = {"custom", "top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("HUD placement", &preset, presets, 5))
            {
                cfg.hud_preset = static_cast<mm::HudPreset>(preset);
            }
            ImGui::BeginDisabled(cfg.hud_preset != mm::HudPreset::Custom);
            int anchor = static_cast<int>(cfg.anchor);
            const char* anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            if (ImGui::Combo("Minimap corner", &anchor, anchors, 4))
            {
                cfg.anchor = static_cast<mm::Anchor>(anchor);
            }
            ImGui::EndDisabled();
            ImGui::DragFloat("Offset X", &cfg.offset_x, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::DragFloat("Offset Y", &cfg.offset_y, 1.0f, 0.0f, 2000.0f, "%.0f px");
            ImGui::TextDisabled("in 1080p pixels");

            // UI SCALE. `auto` is a checkbox over the slider rather than a magic value
            // inside the number, so the slider always says what is actually in force.
            bool auto_scale = cfg.ui_scale_auto;
            if (ImGui::Checkbox("Scale the UI automatically", &auto_scale))
            {
                cfg.ui_scale_auto = auto_scale;
                if (!auto_scale)
                {
                    cfg.ui_scale = g_ui_scale; // start from what is on screen right now
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(in force: %.2f)", static_cast<double>(g_ui_scale));
            ImGui::BeginDisabled(cfg.ui_scale_auto);
            ImGui::SliderFloat("UI scale", &cfg.ui_scale, kUiScaleMin, kUiScaleMax, "%.2f");
            ImGui::EndDisabled();
        }

        //--------------------------------------------------------------------------
        // Markers
        //--------------------------------------------------------------------------
        void player_markers(mm::Config& cfg, float wrap)
        {
            ImGui::Checkbox("Show markers", &cfg.markers_enabled);
            ImGui::SameLine();
            // The INVERSE of markers_hide_found. The config key is phrased as "hide",
            // the question a player asks is "show" - and a checkbox whose label is the
            // opposite of what ticking it does is a bug report waiting to happen.
            bool show_found = !cfg.markers_hide_found;
            if (ImGui::Checkbox("Show found markers", &show_found))
            {
                cfg.markers_hide_found = !show_found;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(found ones are drawn hollow)");
            ImGui::SameLine();
            ImGui::Checkbox("Keep out-of-range markers on the rim", &cfg.markers_clamp_to_edge);
            ImGui::SliderFloat("Marker size (px)", &cfg.markers_size, 2.0f, 16.0f, "%.1f");
            // The chips ARE the legend: each one is filled with the colour that category
            // is drawn in on the map.
            category_filter("Map & minimap", "markers_categories", cfg.markers_categories, 1000, wrap);
        }

        //--------------------------------------------------------------------------
        // Collection tracker
        //--------------------------------------------------------------------------
        void player_tracker(mm::Config& cfg)
        {
            ImGui::Checkbox("Remember what I have collected", &cfg.found_tracker);
            ImGui::SameLine();
            ImGui::Checkbox("Mark items whose level is loaded but absent", &cfg.markers_absence_marks);
            const markers::Stats st = markers::stats();
            // WHICH file, and how it was chosen. A per-save tracker that silently picked
            // the wrong save is indistinguishable from a lost collection, so the answer
            // is on screen rather than only in the log.
            ImGui::Text("Profile: %s", st.found_file[0] != '\0' ? st.found_file : "(none yet)");
            ImGui::SameLine();
            ImGui::TextDisabled("(via %s)", st.found_route[0] != '\0' ? st.found_route : "unresolved");
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("found_profile", cfg.found_profile, sizeof(cfg.found_profile)))
            {
                // Free text on purpose: `auto`, `shared`, or a name of the player's own.
                // It takes effect on Save (or F5) - the loop thread owns the file.
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("auto = one file per save slot (recommended)\n"
                                  "shared = one file for every save\n"
                                  "anything else = wuchang_minimap_found_<name>.txt");
            }
            // The whole collection-statistics page, shared with the full map's Stats
            // panel. One function, so the two views can never disagree about a number.
            draw_collection_stats(::GetTickCount64(), false);
        }

        //--------------------------------------------------------------------------
        // Full map
        //--------------------------------------------------------------------------
        void player_fullmap(mm::Config& cfg)
        {
            ImGui::TextDisabled("Press %s in-world.",
                                key_name_ascii(cfg.map_key).c_str());
            ImGui::SliderFloat("Zoom on open (uu per screen px)", &cfg.map_zoom, cfg.map_zoom_min,
                               cfg.map_zoom_max, "%.0f");
            ImGui::SliderFloat("Map marker size (px)", &cfg.map_marker_size, 3.0f, 24.0f, "%.1f");
            ImGui::Checkbox("Show every floor (ignore the height slice)", &cfg.map_show_all_floors);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad (XInput)", &cfg.map_gamepad);
            ImGui::SameLine();
            ImGui::Checkbox("Remember the waypoint", &cfg.map_waypoint_persist);
            const mv::Waypoint wp = mm::waypoint();
            if (wp.set)
            {
                ImGui::Text("waypoint  X %.0f  Y %.0f  Z %.0f", wp.x, wp.y, wp.z);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear waypoint"))
                {
                    mm::set_waypoint(mv::Waypoint{});
                }
            }
            else
            {
                ImGui::TextDisabled("no waypoint - right-click on the full map to set one");
            }
        }

        //--------------------------------------------------------------------------
        // The hold-key x-ray highlight
        //--------------------------------------------------------------------------
        void player_xray(mm::Config& cfg, float wrap)
        {
            std::string hold = key_name_ascii(cfg.highlight_key);
            if (cfg.highlight_gamepad)
            {
                hold += " or pad " + wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                                     cfg.highlight_pad_lt,
                                                                     cfg.highlight_pad_rt));
            }
            // The MODE, next to the key it applies to, because "press or hold?" is the
            // first thing a player asks about the line above.
            int hl_mode = cfg.highlight_mode == mm::HighlightMode::Hold ? 1 : 0;
            ImGui::TextUnformatted("Mode");
            ImGui::SameLine();
            // Both radios must be DRAWN every frame, so neither call may sit behind a
            // short-circuiting || - the second one would disappear on the frame the
            // first was clicked.
            bool hl_mode_changed = ImGui::RadioButton("Toggle", &hl_mode, 0);
            ImGui::SameLine();
            hl_mode_changed = ImGui::RadioButton("Hold", &hl_mode, 1) || hl_mode_changed;
            if (hl_mode_changed)
            {
                cfg.highlight_mode = hl_mode == 1 ? mm::HighlightMode::Hold : mm::HighlightMode::Toggle;
            }
            if (cfg.highlight_mode == mm::HighlightMode::Hold)
            {
                ImGui::TextWrapped("Hold %s in-world to see nearby markers through walls.", hold.c_str());
            }
            else
            {
                ImGui::TextWrapped("Press %s in-world to see nearby markers through walls, and again to "
                                   "hide them. A level transition turns it off.",
                                   hold.c_str());
            }
            ImGui::Checkbox("Enabled##xray", &cfg.highlight_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Gamepad chord", &cfg.highlight_gamepad);
            ImGui::SameLine();
            ImGui::Checkbox("Names + distance", &cfg.highlight_labels);
            ImGui::SliderFloat("Radius (uu)", &cfg.highlight_radius, 200.0f, 20000.0f, "%.0f");
            ImGui::SameLine();
            ImGui::TextDisabled("= %.0f m", static_cast<double>(cfg.highlight_radius) / 100.0);
            ImGui::SliderFloat("Glyph size (px)", &cfg.highlight_size, 2.0f, 24.0f, "%.1f");
            ImGui::Checkbox("Colour by item quality", &cfg.xray_rarity_colors_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Also tint the minimap / map / compass", &cfg.markers_rarity_tint);
            ImGui::TextDisabled("colours pickups by the game's own item-type grouping");
            category_filter("X-ray highlight", "highlight_categories", cfg.highlight_categories, 2000,
                            wrap);
        }

        //--------------------------------------------------------------------------
        // The compass strip
        //--------------------------------------------------------------------------
        void player_compass(mm::Config& cfg, float wrap)
        {
            ImGui::Checkbox("Enabled##compass", &cfg.compass_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show the waypoint bearing", &cfg.compass_show_waypoint);
            ImGui::SliderFloat("Width (fraction of the screen)", &cfg.compass_width, 0.1f, 1.0f, "%.2f");
            // Overridden by a non-custom hud_preset, which is why it goes flat when one
            // is chosen rather than silently doing nothing.
            ImGui::BeginDisabled(cfg.hud_preset != mm::HudPreset::Custom);
            int canchor = cfg.compass_anchor == mm::VAnchor::Bottom ? 1 : 0;
            const char* canchors[] = {"top", "bottom"};
            if (ImGui::Combo("Edge", &canchor, canchors, 2))
            {
                cfg.compass_anchor = canchor == 1 ? mm::VAnchor::Bottom : mm::VAnchor::Top;
            }
            ImGui::EndDisabled();
            ImGui::SliderFloat("Distance from that edge (px)", &cfg.compass_offset_y, 0.0f, 400.0f, "%.0f");
            ImGui::SliderFloat("Degrees across the strip", &cfg.compass_span_deg, 30.0f, 360.0f, "%.0f");
            ImGui::SliderFloat("Compass opacity", &cfg.compass_opacity, 0.1f, 1.0f, "%.2f");
            ImGui::Checkbox("Distance in metres under each pip", &cfg.compass_pip_labels);
            category_filter("Compass", "compass_categories", cfg.compass_categories, 3000, wrap);
        }

        //--------------------------------------------------------------------------
        // Keys
        //--------------------------------------------------------------------------
        // Read from the config by the SAME builder the full map's footer uses, so a
        // rebind cannot make one of the two lie.
        void player_keys(mm::Config& cfg)
        {
            ImGui::TextWrapped("%s", bindings_hint(cfg).c_str());
            ImGui::TextDisabled("rebind them on the Bindings tab");
        }

        //==============================================================================
        // THE PANEL'S OWN STATE FILE (review B.14)
        //==============================================================================
        //
        // Which Player-tab sections are folded up, remembered between sessions.
        //
        // NOT imgui.ini: io.IniFilename is nullptr and stays that way. ImGui's ini is a
        // whole window-layout store - positions, sizes, docking, every window the mod
        // has ever opened - and turning it on would mean the panel's own "come back
        // centred" behaviour stops working, plus a file whose format is ImGui's business
        // and which nobody can hand-edit meaningfully in a bug report.
        //
        // So: one line, one number, in wuchang_minimap_panel.txt beside the config. Also
        // not a config key, because it is not a setting - it is where the player left a
        // window, and it must not appear in the file a Save writes or in the drift test
        // that guards that file.
        //

        std::wstring panel_state_path()
        {
            return mm::mod_dir() + L"\\wuchang_minimap_panel.txt";
        }

        // LOOP THREAD. Plain CreateFileW/ReadFile and a hand-rolled hex parse: no
        // iostreams anywhere in this mod (lessons.md), and this runs before the render
        // thread has drawn a panel.
        void panel_state_load()
        {
            if (g_panel_state_loaded.exchange(true))
            {
                return;
            }
            const HANDLE h = ::CreateFileW(panel_state_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return; // no file yet: every section open, which is what 1.0.0 did
            }
            char buf[256]{};
            DWORD read = 0;
            const bool ok = ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) != 0;
            ::CloseHandle(h);
            if (!ok || read == 0)
            {
                return;
            }
            const char* p = ::strstr(buf, "sections");
            if (p == nullptr)
            {
                return;
            }
            p = ::strchr(p, '=');
            if (p == nullptr)
            {
                return;
            }
            ++p;
            while (*p == ' ' || *p == '\t')
            {
                ++p;
            }
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            {
                base = 16;
                p += 2;
            }
            std::uint32_t v = 0;
            bool any = false;
            for (; *p != '\0'; ++p)
            {
                int d = -1;
                if (*p >= '0' && *p <= '9')
                {
                    d = *p - '0';
                }
                else if (base == 16 && *p >= 'a' && *p <= 'f')
                {
                    d = *p - 'a' + 10;
                }
                else if (base == 16 && *p >= 'A' && *p <= 'F')
                {
                    d = *p - 'A' + 10;
                }
                if (d < 0)
                {
                    break;
                }
                v = v * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(d);
                any = true;
            }
            if (any)
            {
                g_panel_sections.store(v, std::memory_order_relaxed);
                MM_LOGV(L"panel state: sections 0x{:X}", v);
            }
        }

        // LOOP THREAD, and only when the render thread says something changed.
        void panel_state_save()
        {
            char text[256]{};
            const int n = std::snprintf(text, sizeof(text),
                                        "; WuchangMinimap - where you left the F2 panel. Not a setting:\r\n"
                                        "; delete this file to get every section back open.\r\n"
                                        "sections = 0x%X\r\n",
                                        g_panel_sections.load(std::memory_order_relaxed));
            if (n <= 0)
            {
                return;
            }
            const HANDLE h = ::CreateFileW(panel_state_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD written = 0;
            ::WriteFile(h, text, static_cast<DWORD>(n), &written, nullptr);
            ::CloseHandle(h);
        }

        //==============================================================================
        // THE PLAYER TAB (review B.14)
        //==============================================================================
        //
        // Ten sections, all of them DefaultOpen in 1.0.0, in a window with no ini file -
        // so folding one up lasted until the panel was closed, and finding one setting
        // meant scrolling past nine sections you were not looking for.
        //
        // Three things fix that, and they are listed here rather than spread through the
        // section functions:
        //   * the fold state is remembered, in the panel's own state file (see
        //     panel_state_load / panel_state_save - never imgui.ini);
        //   * a filter box hides the sections that have nothing to do with what was
        //     typed. Matching is per SECTION, against its title AND the words its
        //     settings are named with (`kSections` below), not per widget: filtering
        //     individual widgets would mean wrapping every one of the ~120 calls inside
        //     the section functions in a test, and a table of a section's own vocabulary

        // Thin adapters, so every section has the same signature and the table stays a
        // table. (`wrap` is the content width the category-chip rows need.)
        void sec_presets(mm::Config& cfg, float) { player_presets(cfg); }
        void sec_look(mm::Config& cfg, float) { player_look(cfg); }
        void sec_minimap(mm::Config& cfg, float) { player_minimap(cfg); }
        void sec_placement(mm::Config& cfg, float) { player_placement(cfg); }
        void sec_markers(mm::Config& cfg, float wrap) { player_markers(cfg, wrap); }
        void sec_tracker(mm::Config& cfg, float) { player_tracker(cfg); }
        void sec_fullmap(mm::Config& cfg, float) { player_fullmap(cfg); }
        void sec_xray(mm::Config& cfg, float wrap) { player_xray(cfg, wrap); }
        void sec_compass(mm::Config& cfg, float wrap) { player_compass(cfg, wrap); }
        void sec_keys(mm::Config& cfg, float) { player_keys(cfg); }

        constexpr PanelSection kSections[] = {
            {"Presets", "preset hud layout corner placement", &sec_presets},
            {"Look", "theme palette colour color opacity ink neutral colourblind font scale", &sec_look},
            {"Minimap", "minimap shape round square zoom size rotate north floors adjacent", &sec_minimap},
            {"Placement and scale", "anchor offset position ui scale dpi corner", &sec_placement},
            {"Markers", "markers categories glyph size found hide clamp edge rarity quality", &sec_markers},
            {"Collection tracker", "collection tracker found profile save slot absence", &sec_tracker},
            {"Full map", "full map zoom gamepad waypoint shrine list travel", &sec_fullmap},
            {"X-ray highlight", "x-ray xray highlight through walls hold toggle radius labels", &sec_xray},
            {"Compass", "compass strip heading pips width degrees plate", &sec_compass},
            {"Keys", "keys hotkeys bindings rebind", &sec_keys},
        };
        constexpr int kSectionCount = static_cast<int>(std::size(kSections));
        static_assert(kSectionCount <= 32, "one bit per section in g_panel_sections");

        // Case-insensitive substring, both ways round: typing "colour" finds "Look"
        // through its words, and typing "compa" finds "Compass" through its title.
        bool section_matches(const PanelSection& s, const char* needle)
        {
            if (needle == nullptr || needle[0] == '\0')
            {
                return true;
            }
            char low[64]{};
            std::size_t n = 0;
            for (const char* p = needle; *p != '\0' && n + 1 < sizeof(low); ++p)
            {
                low[n++] = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
            }
            if (n == 0)
            {
                return true;
            }
            // Both haystacks are ASCII literals; _stristr does not exist, so lower the
            // needle once (above) and walk the haystacks with a case-insensitive compare.
            const auto contains = [&low, n](const char* hay) {
                for (const char* h = hay; *h != '\0'; ++h)
                {
                    if (::_strnicmp(h, low, n) == 0)
                    {
                        return true;
                    }
                }
                return false;
            };
            return contains(s.title) || contains(s.words);
        }

        void panel_player(mm::Config& cfg)
        {
            const float wrap = ImGui::GetContentRegionAvail().x;

            // ---- the filter ----------------------------------------------------------
            static char filter[64]{};
            ImGui::SetNextItemWidth(220.0f * g_ui_scale);
            ImGui::InputTextWithHint("##filter", "filter settings...", filter, sizeof(filter));
            ImGui::SameLine();
            if (ImGui::SmallButton("clear"))
            {
                filter[0] = '\0';
            }
            const bool filtering = filter[0] != '\0';
            ImGui::SameLine();
            if (filtering)
            {
                ImGui::TextDisabled("matching sections only");
            }
            else
            {
                ImGui::TextDisabled("type a setting's name");
            }

            // ---- the sections --------------------------------------------------------
            std::uint32_t bits = g_panel_sections.load(std::memory_order_relaxed);
            const std::uint32_t before_bits = bits;
            int shown = 0;
            for (int i = 0; i < kSectionCount; ++i)
            {
                const PanelSection& sec = kSections[i];
                if (!section_matches(sec, filter))
                {
                    continue;
                }
                ++shown;
                const std::uint32_t bit = 1u << i;
                // While filtering, everything that matched is forced OPEN - the answer to
                // "where is that setting" must not be a folded header. The stored bit is
                // deliberately not touched by that (`Always` sets the state without
                // asking the header), so clearing the filter restores the fold exactly.
                if (filtering)
                {
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                }
                else
                {
                    ImGui::SetNextItemOpen((bits & bit) != 0, ImGuiCond_Always);
                }
                if (ImGui::CollapsingHeader(sec.title))
                {
                    if (!filtering)
                    {
                        bits |= bit;
                    }
                    sec.draw(cfg, wrap);
                }
                else if (!filtering)
                {
                    bits &= ~bit;
                }
            }
            if (shown == 0)
            {
                ImGui::TextDisabled("nothing matches '%s'", filter);
            }
            if (bits != before_bits)
            {
                g_panel_sections.store(bits, std::memory_order_relaxed);
                g_panel_state_dirty.store(true, std::memory_order_release);
            }

            // ---- reset ---------------------------------------------------------------
            ImGui::Spacing();
            ImGui::Separator();
            // TWO CLICKS. This throws away every tuned value in the struct, and a stray
            // click on a settings panel should not be able to do that. It is armed until
            // the panel is closed or the button is pressed.
            static bool confirm_reset = false;
            if (!confirm_reset)
            {
                if (ImGui::Button("Reset to the shipped defaults"))
                {
                    confirm_reset = true;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Every setting back to what the mod ships with.\n"
                                      "Revert only re-reads the file, so it cannot undo a saved value.");
                }
            }
            else
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "Reset every setting?");
                ImGui::SameLine();
                if (ImGui::Button("Yes, reset"))
                {
                    confirm_reset = false;
                    const bool was_on = cfg.mod_enabled;
                    cfg = mm::Config{};
                    // The master switch is not a preference, it is whether the mod is
                    // running - and it has its own checkbox and its own log line.
                    cfg.mod_enabled = was_on;
                    mm::log(L"config: reset to the shipped defaults from the F2 panel (not saved yet)");
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    confirm_reset = false;
                }
            }
            ImGui::TextDisabled("Nothing is written until Save.");
        }


        void panel_advanced(mm::Config& cfg)
        {
            const float wrap = ImGui::GetContentRegionAvail().x;

            ImGui::TextDisabled("Correct as shipped.");

            if (ImGui::CollapsingHeader("When the overlay is allowed on screen"))
            {
                ImGui::Checkbox("Only when the camera follows the pawn", &cfg.require_pawn_view);
                ImGui::SliderInt("State stale after (ms)", &cfg.state_stale_ms, 100, 5000);
                ImGui::SliderInt("Grace after a valid pawn (ms)", &cfg.min_visible_after_state_ok_ms, 0, 3000);
                ImGui::SliderInt("Delay after a menu closes (ms)", &cfg.menu_close_show_delay_ms, 0, 1000);
            }

            if (ImGui::CollapsingHeader("Height slicing"))
            {
                ImGui::SliderFloat("Adjacent floor opacity", &cfg.adjacent_floor_opacity, 0.0f, 0.6f, "%.2f");
                ImGui::SliderFloat("Below / above fade range (uu)", &cfg.floor_fade_uu, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Height gradient strength", &cfg.floor_gradient_strength, 0.0f, 0.6f, "%.2f");
                float base[3] = {cfg.floor_base_r / 255.0f, cfg.floor_base_g / 255.0f, cfg.floor_base_b / 255.0f};
                if (ImGui::ColorEdit3("Walkable fill colour", base, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.floor_base_r = base[0] * 255.0f;
                    cfg.floor_base_g = base[1] * 255.0f;
                    cfg.floor_base_b = base[2] * 255.0f;
                }
                ImGui::SliderInt("Slice rate (Hz)", &cfg.slice_hz, 2, 30);
                ImGui::SliderInt("Feet Z smoothing (ms)", &cfg.feet_z_smooth_ms, 1, 1000);
                ImGui::SliderFloat("Player Z offset (uu, capsule -> feet)", &cfg.player_z_offset, -200.0f,
                                   200.0f, "%.0f");
            }

            if (ImGui::CollapsingHeader("Marker sweep and tracker"))
            {
                ImGui::Checkbox("Live actor sweep", &cfg.markers_live);
                ImGui::SameLine();
                ImGui::Checkbox("Only this chapter's markers", &cfg.markers_filter_chapter);
                ImGui::SameLine();
                {
                    const markers::Stats fs = markers::stats();
                    if (!cfg.markers_filter_chapter)
                    {
                        ImGui::TextDisabled("(off - all chapters drawn)");
                    }
                    else if (fs.filter_chapter == chid::kNone)
                    {
                        ImGui::TextDisabled("(chapter not detected yet - all drawn)");
                    }
                    else if (fs.filter_chapter == chid::kDlc)
                    {
                        ImGui::TextDisabled("(showing DLC)");
                    }
                    else
                    {
                        ImGui::Text("(showing chapter %d)", fs.filter_chapter);
                    }
                }
                // The two knobs that trade game-thread time for marker freshness. The
                // "scan pump" line on the Debug tab is the read-out that says which way
                // to move them.
                ImGui::SliderInt("Full passes per second", &cfg.markers_rounds_per_sec, 1, 10);
                ImGui::SliderInt("Object slots per pump", &cfg.markers_scan_chunk, scan::kChunkMin,
                                 scan::kChunkMax);
                ImGui::SliderInt("Min ms between pumps", &cfg.markers_scan_period_ms, scan::kPeriodMinMs,
                                 scan::kPeriodMaxMs);
                ImGui::SliderFloat("Found marker opacity", &cfg.markers_found_alpha, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max markers drawn per frame", &cfg.markers_max_draw, 0, 4000);
                ImGui::SliderInt("Found file debounce (ms)", &cfg.found_save_debounce_ms, 200, 20000);
                ImGui::SeparatorText("Absence as evidence of a collect");
                ImGui::SliderInt("Confirming rounds", &cfg.markers_absence_rounds, 1, 30);
                category_filter("Absence rule", "markers_absence_categories",
                                cfg.markers_absence_categories, 4000, wrap);
            }

            if (ImGui::CollapsingHeader("Minimap look"))
            {
                ImGui::SliderFloat("Backdrop opacity", &cfg.minimap_backdrop, 0.0f, 1.0f, "%.2f");
                float back[3] = {cfg.minimap_backdrop_r / 255.0f, cfg.minimap_backdrop_g / 255.0f,
                                 cfg.minimap_backdrop_b / 255.0f};
                if (ImGui::ColorEdit3("Backdrop colour", back, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.minimap_backdrop_r = back[0] * 255.0f;
                    cfg.minimap_backdrop_g = back[1] * 255.0f;
                    cfg.minimap_backdrop_b = back[2] * 255.0f;
                }
                float frame[3] = {cfg.minimap_frame_r / 255.0f, cfg.minimap_frame_g / 255.0f,
                                  cfg.minimap_frame_b / 255.0f};
                if (ImGui::ColorEdit3("Frame colour", frame, ImGuiColorEditFlags_NoInputs))
                {
                    cfg.minimap_frame_r = frame[0] * 255.0f;
                    cfg.minimap_frame_g = frame[1] * 255.0f;
                    cfg.minimap_frame_b = frame[2] * 255.0f;
                }
                ImGui::SliderFloat("Frame alpha", &cfg.minimap_frame_alpha, 0.0f, 1.0f, "%.2f");
                // The zoom ladder is edited in the file (it is a list); the panel shows
                // what is in force and which key steps through it, because "nothing
                // happens when I press N" is otherwise unanswerable from in-game.
                {
                    std::string ladder;
                    for (int i = 0; i < cfg.minimap_zoom_preset_count && i < mv::kMaxZoomPresets; ++i)
                    {
                        ladder += std::format("{}{:.0f}", ladder.empty() ? "" : ", ",
                                              cfg.minimap_zoom_presets[i]);
                    }
                    ImGui::TextDisabled("zoom presets (%s cycles): %s", key_name_ascii(cfg.zoom_key).c_str(),
                                        ladder.empty() ? "none - edit minimap_zoom_presets" : ladder.c_str());
                }
                ImGui::SliderFloat("Minimum side (px)", &cfg.minimap_min_px, 16.0f, 512.0f, "%.0f");
                ImGui::SliderFloat("Player arrow (fraction of the side)", &cfg.minimap_arrow_frac, 0.01f,
                                   0.3f, "%.3f");
                ImGui::SliderFloat("Player arrow minimum (px)", &cfg.minimap_arrow_min_px, 2.0f, 64.0f, "%.0f");
                ImGui::SliderFloat("Waypoint size (x marker size)", &cfg.waypoint_size_scale, 0.2f, 4.0f,
                                   "%.2f");
            }

            if (ImGui::CollapsingHeader("Full map tuning"))
            {
                ImGui::SliderFloat("Zoom limit - closest", &cfg.map_zoom_min, 1.0f, 200.0f, "%.0f");
                ImGui::SliderFloat("Zoom limit - furthest", &cfg.map_zoom_max, 100.0f, 4000.0f, "%.0f");
                ImGui::SliderFloat("Zoom per wheel notch", &cfg.map_zoom_factor, 1.02f, 1.6f, "%.2f");
                ImGui::SliderFloat("Pan speed (screen px per second)", &cfg.map_pan_speed, 100.0f, 4000.0f,
                                   "%.0f");
                ImGui::SliderFloat("Margin (fraction of screen height)", &cfg.map_margin, 0.0f, 0.3f, "%.3f");
                ImGui::SliderFloat("Backdrop opacity##map", &cfg.map_backdrop, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Max markers drawn##map", &cfg.map_markers_max_draw, 0, 20000);
                ImGui::SliderFloat("Floor step (uu)", &cfg.map_floor_step, 20.0f, 2000.0f, "%.0f");
                ImGui::SliderInt("Slice texture width (px)", &cfg.map_slice_px, 256, 2048);
                ImGui::SliderInt("Slice rate cap (Hz)", &cfg.map_slice_hz, 1, 30);
                ImGui::SliderFloat("Gamepad deadzone", &cfg.map_gamepad_deadzone, 0.05f, 0.6f, "%.2f");
            }

            if (ImGui::CollapsingHeader("X-ray tuning"))
            {
                bool hide_loot = !cfg.highlight_show_found;
                if (ImGui::Checkbox("Hide collected loot", &hide_loot))
                {
                    cfg.highlight_show_found = !hide_loot;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(chests, pickups, hidden items)");
                ImGui::SameLine();
                ImGui::Checkbox("Edge arrows (off screen / behind)", &cfg.highlight_edge_arrows);
                ImGui::SliderInt("Max drawn (nearest first)", &cfg.highlight_max_draw, 1, 400);
                ImGui::SliderInt("Max labelled (nearest first)", &cfg.highlight_labels_max, 0, 40);
                ImGui::SameLine();
                ImGui::TextDisabled("names only");
                ImGui::SliderFloat("Alpha at the camera", &cfg.highlight_alpha_near, 0.1f, 1.0f, "%.2f");
                ImGui::SliderFloat("Alpha at the radius", &cfg.highlight_alpha_far, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Camera read rate (Hz)", &cfg.highlight_camera_hz, 5, 240);
                ImGui::SeparatorText("Item quality palette");
                for (int i = 1; i < mdb::kRarityCount; ++i)
                {
                    mdb::Rgb& c = cfg.xray_rarity_colors[i];
                    float rgb[3] = {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                                    static_cast<float>(c.b) / 255.0f};
                    ImGui::PushID(i + 700);
                    if (ImGui::ColorEdit3(mdb::rarity_name(i), rgb,
                                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
                    {
                        const auto to_byte = [](float v) {
                            const float clamped = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
                            return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
                        };
                        c.r = to_byte(rgb[0]);
                        c.g = to_byte(rgb[1]);
                        c.b = to_byte(rgb[2]);
                    }
                    ImGui::PopID();
                    if (i + 1 < mdb::kRarityCount)
                    {
                        ImGui::SameLine();
                    }
                }
            }

            if (ImGui::CollapsingHeader("Diagnostics"))
            {
                // The one Advanced key a bug report cares about. `normal` is what
                // ships; the other two exist so a problem can be reproduced with the
                // running commentary on without editing a file.
                int lv = static_cast<int>(cfg.log_level);
                if (ImGui::Combo("Log detail", &lv, "normal\0verbose\0trace\0"))
                {
                    cfg.log_level = static_cast<mm::LogLv>(lv);
                }
                ImGui::TextWrapped(
                    "normal = what a bug report needs. verbose = the running commentary "
                    "(player state, menu open/close, why the minimap is hidden). trace = "
                    "everything, including a marker census every two seconds. Takes effect "
                    "as soon as you press Save.");
                char logpath[MAX_PATH * 2]{};
                ::WideCharToMultiByte(CP_UTF8, 0, mm::modlog_path().c_str(), -1, logpath,
                                      sizeof(logpath) - 1, nullptr, nullptr);
                ImGui::TextDisabled("Log file: %s", logpath);
            }

            if (ImGui::CollapsingHeader("Compass tuning"))
            {
                ImGui::SliderFloat("Height (px)", &cfg.compass_height, 10.0f, 120.0f, "%.0f");
                ImGui::Checkbox("Filled plate behind the strip", &cfg.compass_plate);
                ImGui::SameLine();
                ImGui::TextDisabled("off = ticks and letters with a shadow");
                ImGui::SliderFloat("Marker bearing range (uu)", &cfg.compass_marker_distance, 500.0f,
                                   60000.0f, "%.0f");
                ImGui::SliderFloat("Minor tick spacing (deg)", &cfg.compass_tick_step_deg, 1.0f, 90.0f, "%.0f");
                ImGui::SliderInt("Max bearing pips", &cfg.compass_max_pips, 0, 256);
                ImGui::SliderFloat("Above / below arrow from (uu)", &cfg.compass_pip_height_uu, 0.0f,
                                   3000.0f, "%.0f");
                ImGui::SameLine();
                ImGui::TextDisabled("= %.1f m", static_cast<double>(cfg.compass_pip_height_uu) / 100.0);
            }
        }

        // The Dev tier (config_wuchang_minimap_dev.txt). Its own function so the Debug
        // tab does not become one more 300-line block.
        void panel_dev_keys(mm::Config& cfg)
        {
            if (!ImGui::CollapsingHeader("Developer settings (config_wuchang_minimap_dev.txt)"))
            {
                return;
            }
            ImGui::TextDisabled("Saved to config_wuchang_minimap_dev.txt.");
            ImGui::Checkbox("Debug readout (this tab)", &cfg.debug_readout);
            ImGui::SameLine();
            ImGui::Checkbox("Open the panel on start", &cfg.debug_show_panel_on_start);
            ImGui::Checkbox("Load the composite fallback picture", &cfg.fallback_use_composite);
            ImGui::SliderFloat("Composite alpha", &cfg.minimap_composite_alpha, 0.0f, 1.0f, "%.2f");
            ImGui::SeparatorText("Game-state reader");
            ImGui::SliderInt("Position period (ms)", &cfg.reader_position_period_ms, 16, 1000);
            ImGui::SliderInt("Resolve period (ms)", &cfg.reader_resolve_period_ms, 100, 10000);
            ImGui::SliderInt("Widget sweep, fast (ms)", &cfg.reader_widget_sweep_period_ms, 50, 5000);
            ImGui::SliderInt("Widget sweep, backed off (ms)", &cfg.reader_widget_sweep_max_period_ms, 50,
                             30000);
            ImGui::SliderInt("Widget sweep warm window (ms)", &cfg.reader_widget_sweep_warm_ms, 0, 60000);
            ImGui::SliderInt("Transition cooldown (ms)", &cfg.reader_transition_cooldown_ms, 0, 30000);
            {
                float jump = static_cast<float>(cfg.reader_teleport_jump_uu);
                if (ImGui::SliderFloat("Teleport jump (uu per pump)", &jump, 200.0f, 20000.0f, "%.0f"))
                {
                    cfg.reader_teleport_jump_uu = static_cast<double>(jump);
                }
            }
            ImGui::SliderInt("Chapter period (ms)", &cfg.reader_chapter_period_ms, 200, 60000);
            ImGui::SliderInt("Reader log throttle (ms)", &cfg.reader_log_throttle_ms, 500, 60000);
            ImGui::SeparatorText("Sweep, assets, diagnostics");
            ImGui::SliderInt("Live grace rounds", &cfg.markers_live_grace_rounds, 1, 30);
            ImGui::SliderInt("Asset retire grace (ms)", &cfg.map_asset_retire_grace_ms, 0, 60000);
            ImGui::SliderInt("Hide-reason log throttle (ms)", &cfg.hide_reason_log_ms, 0, 60000);
            ImGui::SliderInt("SRV heap size (RESTART)", &cfg.srv_heap_size, 16, 1024);
            ImGui::SeparatorText("X-ray camera reader");
            ImGui::SliderInt("Camera resolve (ms)", &cfg.highlight_camera_resolve_ms, 100, 10000);
            ImGui::SliderInt("Compass-only period (ms)", &cfg.highlight_compass_period_ms, 10, 1000);
            ImGui::SliderInt("Getter fallback period (ms)", &cfg.highlight_getter_period_ms, 10, 1000);
            ImGui::SliderInt("POV scan bytes", &cfg.highlight_pov_scan_bytes, 64, 1024);
            ImGui::SliderInt("POV bad reads before re-pin", &cfg.highlight_pov_bad_reads, 1, 64);
        }

        //==============================================================================
        // The Bindings tab
        //==============================================================================
        //
        // Every hotkey a player has, in one place, with a "press a key" capture instead
        // of a text field in a file. Three rules make it safe:
        //   * the capture only accepts a key mm::vk_bindable() says the config file can
        //     spell, so a binding always survives a save and a reload;
        //   * while it is armed mm::g_key_capture makes the WndProc hook swallow the
        //     whole keyboard, so the key being bound cannot also reach the game;
        //   * it waits for every key to be released first, or the click that armed it
        //     would capture whatever the player is still holding down.
        // A change lands in the live config on the same frame (draw_panel diffs the
        // struct and publishes it), and is written to the file by Save like anything
        // else.

        //==============================================================================
        // MODIFIERS, AND THE KEYS THE GAME ITSELF WANTS (review B.13)
        //==============================================================================

        bool is_modifier_vk(int vk)
        {
            switch (vk)
            {
            case VK_SHIFT:
            case VK_CONTROL:
            case VK_MENU:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_LMENU:
            case VK_RMENU:
                return true;
            default:
                return false;
            }
        }

        // Which modifier is physically down, if any. One, not a set: a binding carries
        // one (mm::key_mod), and Ctrl wins over Shift wins over Alt so the answer is
        // deterministic when a player is leaning on two of them.
        int held_modifier()
        {
            if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                return mm::kKeyModCtrl;
            }
            if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
            {
                return mm::kKeyModShift;
            }
            if ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
            {
                return mm::kKeyModAlt;
            }
            return mm::kKeyModNone;
        }

        // THE KEYS SOMETHING ELSE ALREADY OWNS.
        //
        // This list is ADVISORY and it is not read from the game - there is no API for
        // that, and the mod must not pretend otherwise. Two sources, both written down
        // so the next person can judge them:
        //
        //   * the movement / interaction set this genre binds by default (WASD, Space,
        //     Shift, Ctrl, E, F, Q, R, Tab, Esc, 1..5) - a bare letter bound to a mod
        //     action while the HUD is a pure overlay both fires the mod AND does its
        //     game thing, which is what the hotkey swallow now prevents; that makes the
        //     GAME action the casualty instead, so the player has to be told;
        //   * the keys this machine's other injected DLLs own, from lessons.md: F6 is
        //     RenoDX's DLSS 5 toggle (it ignores modifiers and has already caused one
        //     GPU crash), F10 is the UE4SS console, F9 / F11 are engine binds and F12 is
        //     the Steam screenshot key. Those four are refused by the config parser






        void arm_capture(int row)
        {
            g_capture_row = row;
            g_capture_wait_release = true;
            mm::g_key_capture.store(row >= 0, std::memory_order_relaxed);
        }

        void panel_bindings(mm::Config& cfg)
        {
            static const mm::Config kDefaults{};

            // ---- the capture, before anything is drawn --------------------------------
            //
            // A CAPTURE CAN NOW TAKE A MODIFIER (review B.13). Two shapes, and both have
            // to work: `ctrl+m` (hold Ctrl, press M) and a bare modifier (`LALT`, which
            // is the x-ray highlight's shipped default). So a non-modifier key wins
            // immediately and carries whatever modifier is held with it, while a
            // modifier pressed ON ITS OWN is only taken once everything is released -
            // which is also the only way to tell "I am reaching for Ctrl+M" from "I want
            // Ctrl".
            if (g_capture_row >= 0 && g_capture_row < kKeyBindCount)
            {
                bool any_down = false;
                int pressed = 0;      // a real key: bind it now, with the held modifier
                int mod_only = 0;     // a modifier on its own: bind it on release
                for (const int vk : mm::bindable_vks())
                {
                    if ((::GetAsyncKeyState(vk) & 0x8000) == 0)
                    {
                        continue;
                    }
                    any_down = true;
                    if (is_modifier_vk(vk))
                    {
                        if (mod_only == 0)
                        {
                            mod_only = vk;
                        }
                    }
                    else if (pressed == 0)
                    {
                        pressed = vk;
                    }
                }
                static int pending_mod_only = 0;
                if ((::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
                {
                    pending_mod_only = 0;
                    arm_capture(-1);
                }
                else if (g_capture_wait_release)
                {
                    g_capture_wait_release = any_down;
                }
                else
                {
                    if (pressed == 0 && mod_only != 0)
                    {
                        pending_mod_only = mod_only;
                    }
                    const int take = pressed != 0 ? mm::key_make(pressed, held_modifier())
                                     : (!any_down && pending_mod_only != 0)
                                         ? mm::key_make(pending_mod_only, mm::kKeyModNone)
                                         : 0;
                    if (take != 0)
                    {
                        pending_mod_only = 0;
                        cfg.*kKeyBinds[g_capture_row].member = take;
                        mm::logf(L"binding: {} = {}",
                                 std::wstring(kKeyBinds[g_capture_row].key,
                                              kKeyBinds[g_capture_row].key +
                                                  std::strlen(kKeyBinds[g_capture_row].key)),
                                 mm::key_name(take));
                        arm_capture(-1);
                    }
                }
            }
            else if (g_capture_row >= 0)
            {
                arm_capture(-1);
            }

            ImGui::TextDisabled("Click a key to rebind it, then press the new key - hold Ctrl, Shift or "
                                "Alt with it for a modified binding. Esc cancels.");
            ImGui::TextDisabled("A key bound here is taken away from the game while the mod is using it.");

            if (ImGui::BeginTable("bindings", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Action");
                ImGui::TableSetupColumn("Key");
                ImGui::TableSetupColumn("");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (int i = 0; i < kKeyBindCount; ++i)
                {
                    const int vk = cfg.*kKeyBinds[i].member;
                    // CONFLICT. Two actions on one key is not an error the mod can
                    // resolve - both fire - so it is named rather than prevented.
                    const char* clash = nullptr;
                    for (int j = 0; j < kKeyBindCount && clash == nullptr; ++j)
                    {
                        if (j != i && vk != 0 && cfg.*kKeyBinds[j].member == vk)
                        {
                            clash = kKeyBinds[j].label;
                        }
                    }
                    // AND THE UNMODIFIED TWIN. `ctrl+m` and `m` are different bindings
                    // but the same key press: a no-modifier binding deliberately does
                    // not require the modifiers to be up (see mm::key_mod), so pressing
                    // Ctrl+M fires both. That is a choice, not a bug - it is what keeps
                    // every hotkey alive while the x-ray's Alt is held - so it is named
                    // rather than prevented.
                    const char* twin = nullptr;
                    for (int j = 0; j < kKeyBindCount && twin == nullptr; ++j)
                    {
                        const int other = cfg.*kKeyBinds[j].member;
                        if (j != i && vk != 0 && mm::key_vk(other) == mm::key_vk(vk) &&
                            mm::key_mod(other) != mm::key_mod(vk))
                        {
                            twin = kKeyBinds[j].label;
                        }
                    }
                    const char* game = game_bind_clash(vk);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kKeyBinds[i].label);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", kKeyBinds[i].key);

                    ImGui::TableNextColumn();
                    ImGui::PushID(i + 900);
                    const std::string shown = g_capture_row == i
                                                  ? std::string("press a key...")
                                                  : key_name_ascii(vk);
                    if (ImGui::Button(shown.c_str(), ImVec2{130.0f * g_ui_scale, 0.0f}))
                    {
                        arm_capture(g_capture_row == i ? -1 : i);
                    }

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(vk == kDefaults.*kKeyBinds[i].member);
                    if (ImGui::SmallButton("reset"))
                    {
                        cfg.*kKeyBinds[i].member = kDefaults.*kKeyBinds[i].member;
                    }
                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    if (clash != nullptr)
                    {
                        ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "also %s", clash);
                    }
                    else if (game != nullptr)
                    {
                        ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f}, "the game may use it for %s",
                                           game);
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("While the mod is using this key the game does not get it.\n"
                                              "Add Ctrl, Shift or Alt to give it back.");
                        }
                    }
                    else if (twin != nullptr)
                    {
                        ImGui::TextColored(ImVec4{0.80f, 0.80f, 0.55f, 1.0f}, "same key as %s", twin);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (ImGui::Button("Reset every binding"))
            {
                for (int i = 0; i < kKeyBindCount; ++i)
                {
                    cfg.*kKeyBinds[i].member = kDefaults.*kKeyBinds[i].member;
                }
                arm_capture(-1);
            }

            //--------------------------------------------------------------------------
            // The gamepad chord
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Gamepad");
            ImGui::Checkbox("X-ray on a gamepad chord", &cfg.highlight_gamepad);
            static char chord[64]{};
            static bool chord_primed = false;
            const std::string live = wide_to_ascii(
                mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt));
            if (!chord_primed)
            {
                ::strncpy_s(chord, sizeof(chord), live.c_str(), _TRUNCATE);
                chord_primed = true;
            }
            ImGui::SetNextItemWidth(180.0f * g_ui_scale);
            if (ImGui::InputText("highlight_pad_chord", chord, sizeof(chord),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                mm::set_pad_chord(chord, cfg.highlight_pad_mask, cfg.highlight_pad_lt,
                                  cfg.highlight_pad_rt);
                ::strncpy_s(chord, sizeof(chord),
                            wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask,
                                                             cfg.highlight_pad_lt,
                                                             cfg.highlight_pad_rt))
                                .c_str(),
                            _TRUNCATE);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("in force: %s   (LB, RB, LT, RT, A, B, X, Y, BACK, START, LS, RS, "
                                "UP, DOWN, LEFT, RIGHT, joined with +; `none` disables it)",
                                live.c_str());
            if (ImGui::SmallButton("reset the chord"))
            {
                cfg.highlight_pad_mask = kDefaults.highlight_pad_mask;
                cfg.highlight_pad_lt = kDefaults.highlight_pad_lt;
                cfg.highlight_pad_rt = kDefaults.highlight_pad_rt;
                chord_primed = false;
            }
            ImGui::TextDisabled("the full map's own gamepad controls are fixed (left stick pans, "
                                "triggers zoom, LB / RB change floor)");
        }

        void panel_debug(mm::Config& cfg, const mm::Snapshot& snap, bool have_state)
        {
            panel_dev_keys(cfg);

            //--------------------------------------------------------------------------
            // The crash breadcrumb
            //--------------------------------------------------------------------------
            //
            // Where the overlay is now, and - the useful half - what the PREVIOUS session
            // left in wuchang_minimap_last_stage.txt. A non-terminal value there is the
            // only surviving evidence when the process died with UE4SS's log buffer
            // unflushed, so it is called out in colour rather than printed as a fact.
            ImGui::SeparatorText("Stage");
            ImGui::Text("now: %s", crumb::current()[0] != '\0' ? crumb::current() : "(none)");
            ImGui::SameLine();
            ImGui::TextDisabled("(file %s)", cfg.crash_breadcrumb ? "on" : "off - crash_breadcrumb = 0");
            if (crumb::previous_suspicious())
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f},
                                   "last session ended at '%s' - it did NOT shut down cleanly",
                                   crumb::previous());
            }
            else if (crumb::previous()[0] != '\0')
            {
                ImGui::TextDisabled("last session ended at '%s'", crumb::previous());
            }
            else
            {
                ImGui::TextDisabled("no previous session recorded");
            }

            //--------------------------------------------------------------------------
            // The one-press recon dump
            //--------------------------------------------------------------------------
            //
            // context/saveslot-and-teleport-research.md section 3: the four things that
            // cannot be recovered from the cooked assets. It calls nothing and changes
            // nothing - reflection lookups and raw reads only - and writes one file the
            // user can send back.
            ImGui::SeparatorText("Recon dump");
            const recon::Status rc = recon::status();
            ImGui::BeginDisabled(rc.pending);
            if (ImGui::Button("Dump the fast-travel / save-slot recon"))
            {
                recon::request();
            }
            ImGui::EndDisabled();
            if (rc.pending)
            {
                ImGui::TextDisabled("gathering on the next game-thread pump...");
            }
            else if (rc.error[0] != '\0')
            {
                ImGui::TextColored(ImVec4{0.95f, 0.5f, 0.4f, 1.0f}, "%s", rc.error);
            }
            else if (rc.file[0] != '\0')
            {
                ImGui::TextWrapped("wrote %d line(s) to %s", rc.lines, rc.file);
            }

            //--------------------------------------------------------------------------
            // The runtime navmesh dump
            //--------------------------------------------------------------------------
            //
            // A button, not a binding: it scans engine memory and writes JSON, which is
            // never something a player should be able to trigger by leaning on a key.
            // The module ships disabled (config.ini [navmesh] navmesh_dump = 1), and the
            // button says so rather than doing nothing.
            ImGui::SeparatorText("Runtime navmesh dump");
            ImGui::BeginDisabled(!navmesh::enabled());
            if (ImGui::Button("Dump the live navmesh tiles"))
            {
                navmesh::request_dump();
                post_toast("navmesh dump requested", 2500);
            }
            ImGui::EndDisabled();
            if (!navmesh::enabled())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("off - set navmesh_dump = 1 in config.ini and restart");
            }

            draw_perf_table();

            //--------------------------------------------------------------------------
            // Marker sweep
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Markers");
            const markers::Stats st = markers::stats();
            ImGui::Text("db %d marker(s) / %d chapter(s)   found file %d id(s)   published %d   live %d",
                        st.static_markers,
                        st.chapters_loaded,
                        st.found_ids,
                        st.published,
                        st.live_entries);
            // The absence rule (markers_absence_*). Both numbers are diagnostics:
            // `levels loaded` at 0 means the rule can NEVER fire (nothing to match a
            // marker's level against), which is the failure worth seeing at a glance.
            ImGui::Text("absence marks %d   levels loaded %d   (rule %s, %d round(s), %s)",
                        st.absence_marks,
                        st.levels_loaded,
                        cfg.markers_absence_marks ? "on" : "off",
                        cfg.markers_absence_rounds,
                        mdb::format_category_mask(cfg.markers_absence_categories).c_str());
            // Two independent numbers, and they answer different questions.
            //   PUMP  - what one game-thread pump costs. This is the frame-hitch number;
            //           the target is well under 1 ms, and `max` is the worst single pump
            //           since the mod loaded.
            //   ROUND - what a full pass over the object array cost and how many object
            //           slots it visited. This is the freshness number: the marker set is
            //           `slices x period_ms` old at worst.
            // `!` marks the FindAllOf fallback, which is the old 28 ms-per-pump path and
            // only runs when GUObjectArray reports no elements.
            ImGui::Text("scan pump %.3f ms (avg %.3f, peak %.3f, max %.3f)%s",
                        st.scan_slice_ms,
                        st.scan_slice_ms_avg,
                        st.scan_slice_ms_peak,
                        st.scan_slice_ms_max,
                        st.scan_fallback ? "   ! FindAllOf fallback" : "");
            ImGui::Text("round %.1f ms / %d pump(s) / %d object(s) of %d   chunk %d   %llu round(s)",
                        st.scan_round_ms,
                        st.scan_round_slices,
                        st.scan_round_objects,
                        st.scan_total,
                        st.scan_chunk,
                        static_cast<unsigned long long>(st.rounds));
            ImGui::Text("publish %.3f ms (avg %.3f, peak %.3f)",
                        st.publish_ms,
                        st.publish_ms_avg,
                        st.publish_ms_peak);
            ImGui::Text("drawn %d of %d (%d clamped, %d filtered, %d merged)",
                        g_marker_draw.drawn,
                        g_marker_draw.total,
                        g_marker_draw.clamped,
                        g_marker_draw.filtered,
                        g_marker_draw.merged);
            if (g_marker_draw.nearest[0] != 0)
            {
                ImGui::Text("nearest: %s (%.0f uu)", g_marker_draw.nearest, g_marker_draw.nearest_uu);
            }

            //--------------------------------------------------------------------------
            // Full map / gamepad
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Full map and gamepad");
            const pad::State gp = pad::state();
            char padmod[64]{};
            ::WideCharToMultiByte(CP_UTF8, 0, pad::module_name(), -1, padmod, sizeof(padmod) - 1, nullptr,
                                  nullptr);
            // TRUTHFUL, whatever the answer is (review B.1). This line read `none` for
            // ever in 1.0.0 - not because no pad was plugged in, but because XInput was
            // only polled while the full map was open, and the map could only be opened
            // from the keyboard. So it now also says whether anything is ASKING: with
            // map_gamepad off nothing polls, and "none" then means "not looked at".
            ImGui::Text("pad: %s (%s)   sticks %.2f,%.2f / %.2f,%.2f   triggers %.2f/%.2f",
                        gp.connected ? "connected"
                                     : (cfg.map_gamepad ||
                                        (cfg.highlight_enabled && cfg.highlight_gamepad))
                                           ? "none found (polling)"
                                           : "not polled (map_gamepad = 0)",
                        padmod,
                        static_cast<double>(gp.lx),
                        static_cast<double>(gp.ly),
                        static_cast<double>(gp.rx),
                        static_cast<double>(gp.ry),
                        static_cast<double>(gp.lt),
                        static_cast<double>(gp.rt));
            ImGui::Text("map slice %dx%d   %.2f ms (peak %.2f)   %llu cut(s), %llu skipped   "
                        "opaque %u / dim %u / faint %u",
                        g_mslice[0].w,
                        g_mslice[0].h,
                        g_mslice_ms,
                        g_mslice_ms_peak,
                        static_cast<unsigned long long>(g_mslice_updates),
                        static_cast<unsigned long long>(g_mslice_skipped),
                        g_mslice_counts.opaque,
                        g_mslice_counts.dim,
                        g_mslice_counts.faint);

            //--------------------------------------------------------------------------
            // The x-ray camera
            //--------------------------------------------------------------------------
            // WHERE THE CAMERA COMES FROM. This is the block to screenshot if the labels
            // are in the wrong place: it names the route, the pinned offset and how old
            // the pose is.
            ImGui::SeparatorText("X-ray camera");
            const hl::Stats hs = hl::stats();
            const char* route = "none yet";
            switch (hs.route)
            {
            case hl::Route::RawPinned:
                route = "raw POV read (offset calibrated against the getters)";
                break;
            case hl::Route::RawSane:
                route = "raw POV read (offset accepted on sanity ranges only)";
                break;
            case hl::Route::Getters:
                route = "GetCameraLocation / GetCameraRotation / GetFOVAngle per read";
                break;
            case hl::Route::None:
            default:
                break;
            }
            ImGui::Text("camera: %s", route);
            ImGui::Text("manager %s   CameraCachePrivate +%d   POV +%d   %llu read(s), %llu rejected",
                        hs.have_manager ? "yes" : "NO",
                        hs.cache_offset,
                        hs.pov_offset,
                        static_cast<unsigned long long>(hs.reads),
                        static_cast<unsigned long long>(hs.fails));
            hl::Pose pose{};
            if (hl::camera(pose))
            {
                ImGui::Text("pose  X %.0f  Y %.0f  Z %.0f   pitch %.1f  yaw %.1f  roll %.1f   FOV %.1f   "
                            "%llu ms old",
                            pose.x,
                            pose.y,
                            pose.z,
                            pose.pitch,
                            pose.yaw,
                            pose.roll,
                            pose.fov,
                            static_cast<unsigned long long>(
                                pose.stamp_ms == 0 ? 0 : ::GetTickCount64() - pose.stamp_ms));
            }
            else
            {
                ImGui::TextDisabled("no camera pose published yet (hold the key in-world)");
            }
            ImGui::Text("held %s   %d of %d in range drawn (%d on screen, %d on the rim)",
                        g_hl_debug.active ? "YES" : "no",
                        g_hl_debug.drawn,
                        g_hl_debug.considered,
                        g_hl_debug.on_screen,
                        g_hl_debug.edge);
            ImGui::Text("compass: %s   heading %.1f deg from the %s   %d bearing pip(s)",
                        g_compass_debug.visible ? "visible" : "hidden (same gate as the minimap)",
                        g_compass_debug.heading,
                        g_compass_debug.from_camera ? "camera" : "pawn",
                        g_compass_debug.pips);

            //--------------------------------------------------------------------------
            // The game state
            //--------------------------------------------------------------------------
            ImGui::SeparatorText("Game state");
            if (!have_state)
            {
                ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.4f, 1.0f}, "no game-state snapshot yet");
            }
            else
            {
                const std::uint64_t age = ::GetTickCount64() - snap.stamp_ms;
                ImGui::Text("world  X %.1f  Y %.1f  Z %.1f   yaw %.1f deg", snap.x, snap.y, snap.z, snap.yaw);
                ImGui::Text("uv     %.5f, %.5f   chapter '%s'",
                            g_last_mini.u,
                            g_last_mini.v,
                            g_last_mini.chapter.empty() ? "-" : g_last_mini.chapter.c_str());
                ImGui::Text("gameplay pawn %s   transition %s   state-ok age %llu ms",
                            snap.pawn_is_gameplay ? "yes" : "NO",
                            snap.transition ? "YES" : "no",
                            static_cast<unsigned long long>(
                                snap.state_ok_since_ms == 0 ? 0 : ::GetTickCount64() - snap.state_ok_since_ms));
                // The height slicer: what it cut, how much it cost, where it is.
                ImGui::Text("slice  %dx%d px x %d surface(s) @ %.4f px/uu   %.2f ms (peak %.2f)   "
                            "%llu update(s), %llu skipped",
                            g_slice_size,
                            g_slice_size,
                            g_slice_surfaces,
                            g_slice_px_per_uu,
                            g_slice_ms,
                            g_slice_ms_peak,
                            static_cast<unsigned long long>(g_slice_updates),
                            static_cast<unsigned long long>(g_slice_skipped));
                ImGui::Text("       feet Z %.0f (raw %.0f)   tol %.0f  fade %.0f  gradient %.2f   "
                            "opaque %u / dim %u / faint %u",
                            static_cast<double>(g_feet_z),
                            snap.z - static_cast<double>(cfg.player_z_offset),
                            static_cast<double>(cfg.floor_z_tolerance),
                            static_cast<double>(cfg.floor_fade_uu),
                            static_cast<double>(cfg.floor_gradient_strength),
                            g_slice_opaque,
                            g_slice_dim,
                            g_slice_faint);
                ImGui::Text("pawn %s   pawn-view %s   menu %s   state age %llu ms",
                            snap.has_pawn ? "yes" : "no",
                            snap.is_pawn_view ? "yes" : "no",
                            snap.menu_open ? "OPEN" : "no",
                            static_cast<unsigned long long>(age));
                ImGui::Text("widgets seen %u, visible in viewport %u   location via %s",
                            snap.widgets_seen,
                            snap.widgets_visible_in_viewport,
                            snap.loc_from_function ? "K2_GetActorLocation" : "RootComponent");
                // Menu hide/show latency: how long ago the game thread saw the menu state
                // change, and how many roots it re-tests on every pump.
                char holder[128]{};
                ::WideCharToMultiByte(CP_UTF8, 0, snap.menu_holder, -1, holder, sizeof(holder) - 1, nullptr,
                                      nullptr);
                ImGui::Text("menu state changed %llu ms ago   %u cached in-viewport root(s)   "
                            "show delay %d ms   holder '%s'",
                            static_cast<unsigned long long>(
                                snap.menu_change_ms == 0 ? 0 : ::GetTickCount64() - snap.menu_change_ms),
                            snap.menu_roots_cached,
                            cfg.menu_close_show_delay_ms,
                            holder[0] != '\0' ? holder : "-");
                ImGui::Text("teleport %s",
                            snap.teleport_ms == 0
                                ? "never this session"
                                : std::format("{} ms ago", ::GetTickCount64() - snap.teleport_ms).c_str());
                char narrow[256]{};
                ::WideCharToMultiByte(CP_UTF8, 0, snap.level_name, -1, narrow, sizeof(narrow) - 1, nullptr,
                                      nullptr);
                ImGui::TextWrapped("pawn: %s", narrow);
            }

            ImGui::Spacing();
            char reason[192]{};
            ::WideCharToMultiByte(CP_UTF8, 0, g_hide_reason, -1, reason, sizeof(reason) - 1, nullptr, nullptr);
            // THE ONE LINE THAT ANSWERS "why is the minimap not there". It is recomputed
            // from live state every frame - there is no latch anywhere in the show
            // condition - and every change to it is also logged.
            if (g_last_mini.visible)
            {
                ImGui::TextColored(ImVec4{0.55f, 0.9f, 0.6f, 1.0f}, "minimap: %s", reason);
            }
            else
            {
                ImGui::TextColored(ImVec4{1.0f, 0.62f, 0.42f, 1.0f}, "hidden because: %s", reason);
            }
            ImGui::TextDisabled("       this state has held for %llu ms",
                                static_cast<unsigned long long>(
                                    g_reason_since_ms == 0 ? 0 : ::GetTickCount64() - g_reason_since_ms));
            ImGui::Text("backbuffer %ux%u, %u buffer(s), composite %dx%d %s   ui scale %.2f",
                        g_width,
                        g_height,
                        g_buffer_count,
                        g_map.width,
                        g_map.height,
                        g_map.ready ? "ready" : "NOT ready",
                        static_cast<double>(g_ui_scale));
            ImGui::Text("presents %llu, resizes %llu",
                        static_cast<unsigned long long>(g_present_count.load()),
                        static_cast<unsigned long long>(g_resize_count.load()));
        }

        void draw_panel(mm::Config cfg, const mm::Snapshot& snap, bool have_state)
        {
            bool open = true;
            ImGui::SetNextWindowSize(ImVec2{520.0f * g_ui_scale, 620.0f * g_ui_scale},
                                     ImGuiCond_FirstUseEver);
            // CENTRED, every time it opens. `Appearing` rather than `FirstUseEver` so a
            // panel that was dragged to a corner and closed comes back in the middle of
            // the screen; the pivot is the window's own centre, so its size does not
            // change where it lands. Dragging it still works - the position is only
            // written on the frame the window appears.
            const ImGuiViewport* pvp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2{pvp->Pos.x + pvp->Size.x * 0.5f,
                                           pvp->Pos.y + pvp->Size.y * 0.5f},
                                    ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
            if (!ImGui::Begin("Wuchang Minimap  v" WUCHANG_MINIMAP_VERSION
                              "###wuchang_minimap_panel",
                              &open, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::End();
                if (!open)
                {
                    mm::g_panel_open = false;
                }
                return;
            }

            const mm::Config before = cfg;

            // The version and nothing else. "beta" was a note to ourselves and 1.0.0 is
            // not one (lessons.md: a user-visible "not yet verified" line is a note to
            // OURSELVES); version.hpp is the single source of truth for the string.
            ImGui::TextColored(ImVec4{0.62f, 0.68f, 0.78f, 1.0f},
                               "WuchangMinimap v" WUCHANG_MINIMAP_VERSION);

            // The tabs get their own child so the Save / Revert / master-switch row is
            // always at the bottom of the window and never scrolls away with them.
            const float row_h = ImGui::GetFrameHeightWithSpacing() * 3.0f;
            if (ImGui::BeginChild("tabs", ImVec2{0.0f, -row_h}, ImGuiChildFlags_None))
            {
                if (ImGui::BeginTabBar("wuchang_tabs"))
                {
                    if (ImGui::BeginTabItem("Player"))
                    {
                        panel_player(cfg);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Advanced"))
                    {
                        panel_advanced(cfg);
                        ImGui::EndTabItem();
                    }
                    // The Debug tab EXISTS only while debug_readout is on, which is a
                    // Dev key in a file players do not have. So the panel a player sees
                    // has two tabs and no developer surface at all.
                    if (ImGui::BeginTabItem("Bindings"))
                    {
                        panel_bindings(cfg);
                        ImGui::EndTabItem();
                    }
                    if (cfg.debug_readout && ImGui::BeginTabItem("Debug"))
                    {
                        panel_debug(cfg, snap, have_state);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            // The button says WHICH FILE it writes: there are two now, and the panel is
            // the only place that says which of them a setting lives in. And it has to
            // say BOTH when both are written - a Save with a dev file present (or with a
            // Dev dial moved off its default, which is what the Debug tab does) rewrites
            // config_wuchang_minimap_dev.txt as well, and a button that named one file
            // while writing two is exactly the kind of thing a bug report starts with.
            const bool dev_too = mm::dev_config_active();
            if (ImGui::Button(dev_too ? "Save to config_wuchang_minimap.txt + _dev.txt"
                                      : "Save to config_wuchang_minimap.txt"))
            {
                mm::g_save_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(dev_too ? "Write the current settings back to config_wuchang_minimap.txt "
                                            "and the developer dials to config_wuchang_minimap_dev.txt."
                                          : "Write the current settings back to the config file.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert"))
            {
                mm::g_revert_config = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Re-read the config files and discard unsaved changes.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload settings + maps"))
            {
                mm::g_reload_config = true;
            }

            // THE MASTER SWITCH. Unticking it does not stop anything from here - it only
            // writes mod_enabled = 0 into the config file, which the loop thread's 1 Hz
            // watcher then acts on (modswitch.hpp). That keeps the whole shutdown
            // sequence on the one thread that is allowed to run it, and it means the file
            // and the running state can never disagree.
            if (ImGui::Checkbox("Mod enabled (master switch - turns EVERYTHING off)", &cfg.mod_enabled))
            {
                if (!cfg.mod_enabled)
                {
                    mm::set_config(cfg);
                    mm::g_save_config = true;
                    mm::log(L"master switch: turned off from the F2 panel - writing mod_enabled = 0; the "
                            L"mod stops within a second. Edit the config file to turn it back on.");
                }
            }
            if (!cfg.mod_enabled)
            {
                ImGui::TextColored(ImVec4{0.95f, 0.72f, 0.35f, 1.0f},
                                   "The mod is shutting down. Set mod_enabled = 1 in %s to restart it.",
                                   "config_wuchang_minimap.txt");
            }
            // THE OTHER OFF SWITCH. Same shutdown, no file written - for ruling the mod
            // out of a problem without ending up with a config to repair afterwards.
            ImGui::SameLine();
            if (ImGui::Button("Disable for this session"))
            {
                modswitch::request_session_disable();
                mm::log(L"master switch: disable for this session requested from the F2 panel - "
                        L"nothing is written to the config file; save or edit it to turn the mod "
                        L"back on (checked once a second).");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Stop the mod until the config file is saved or edited again.");
            }

            ImGui::End();

            // Field by field (review B.19), not memcmp: a Config is a value, and
            // `mm::operator==` is generated from the struct with a byte-flip drift
            // guard behind it in markers_test.
            if (before != cfg)
            {
                mm::set_config(cfg);
            }
            if (!open)
            {
                mm::g_panel_open = false;
            }
        }

        //==============================================================================
        // Per-frame work
        //==============================================================================

        void build_ui()
        {
            // `raw` is what the panel edits and what Save writes; `cfg` is the same
            // settings with every pixel key multiplied by the UI scale, and it is what
            // the HUD draws from. Keeping them apart is what stops a scaled value ever
            // being written back into the config file.
            const mm::Config& raw = mm::cfg_cached();
            mm::Config cfg = ui_scaled(raw, g_ui_scale);
            // The look, once per frame: every draw path below reads these two globals
            // instead of asking the config what colour it is.
            g_palette = raw.palette;
            g_plate = gly::theme_colors(raw.theme).plate;
            // The disc-drawing helpers take geometry, not the config, so the live
            // roundness is cached here once per frame (render thread only).
            mm::Snapshot snap{};
            const bool have = mm::read_snapshot(snap);

            const bool map_open = mm::g_map_open.load(std::memory_order_relaxed);
            const std::uint64_t frame_now = ::GetTickCount64();

            // THE HUD FADE. Its target is the gate result and nothing else: 0 is applied
            // instantly (and the gate stops the draw anyway, so hiding is immediate),
            // and only showing is eased - 150 ms, so a menu closing does not snap the
            // HUD back on. It is applied by scaling the opacity keys of the per-frame
            // config copy, which is why `cfg` is a mutable copy: nothing downstream has
            // to know the fade exists, and no scaled value can reach the config file.
            const bool gate_open = have && hud_gate(cfg, snap, have, frame_now) == nullptr;
            if (gate_open && !g_hud_gate_ever_open.load(std::memory_order_relaxed))
            {
                // The first frame anything of ours could be seen. The first-run tip on
                // the loop thread is waiting for exactly this (review B.2).
                g_hud_gate_ever_open.store(true, std::memory_order_release);
            }
            const float fade = hud_fade_step(gate_open && cfg.overlay_enabled, frame_now);
            cfg.opacity *= fade;
            cfg.compass_opacity *= fade;
            cfg.highlight_alpha_near *= fade;
            cfg.highlight_alpha_far *= fade;

            // The mouse cursor belongs to whoever is taking the input. Both conditions
            // are plain reads of the live flags - nothing here is remembered, so the
            // frame the map or the panel closes is the frame the game gets the cursor
            // back.
            ImGui::GetIO().MouseDrawCursor = map_open || mm::g_panel_open.load(std::memory_order_relaxed);

            // The pad, into ImGui's own nav (review B.8). Only while the panel is open,
            // and from the state the loop thread sampled - never a poll from here.
            feed_pad_nav(raw);

            // THE ONE MARKER PASS. The minimap, the full map, the compass pips and the
            // x-ray highlight all read the same published buffer; walking it once here
            // and letting each of them filter the result replaces three (four with the
            // map open) full scans plus their square roots.
            if (have)
            {
                build_frame_candidates(snap);
            }
            else
            {
                g_frame_cands.clear();
                g_frame_marker_total = 0;
                g_frame_bad_cat = 0;
            }

            if (mm::g_panel_open.load(std::memory_order_relaxed))
            {
                draw_panel(raw, snap, have);
                mm::g_panel_drew_frame.store(true, std::memory_order_relaxed);
            }
            else if (g_capture_row >= 0)
            {
                // The panel closed with a capture armed. Disarm it here rather than in
                // the close paths: this is the one place that runs on every frame, so
                // the keyboard can never stay swallowed with no panel on screen.
                arm_capture(-1);
            }

            if (map_open)
            {
                draw_full_map(raw, snap, have, g_ui_scale);
            }
            if (g_map_was_open && !mm::g_map_open.load(std::memory_order_relaxed))
            {
                // Closed (by the key, by the gate, or from inside the map): the next
                // open starts centred on the player again.
                g_mv_init = false;
            }
            g_map_was_open = mm::g_map_open.load(std::memory_order_relaxed);

            if (!cfg.overlay_enabled || !cfg.show_minimap)
            {
                set_hide_reason(L"disabled in the config");
            }
            else if (mm::g_map_open.load(std::memory_order_relaxed))
            {
                // The full map replaces the minimap while it is up - two views of the
                // same thing on one screen is just clutter, and the slicer would then be
                // cutting two windows a frame.
                set_hide_reason(L"the full map is open");
            }
            else
            {
                if (g_pf_minimap < 0)
                {
                    g_pf_minimap = mm::perf_register("minimap draw", perf::Thread::Render);
                }
                const mm::PerfScope scope(g_pf_minimap);
                draw_minimap(cfg, snap, have);
            }

            // The compass and the x-ray highlight ask the SAME gate the minimap does -
            // one evaluation, no second set of rules, no second latch - and additionally
            // stand down while the full map is open, because the map is a mode of its own
            // (it swallows the input and covers the scene they would be drawn over).
            const bool hud_ok = gate_open && !mm::g_map_open.load(std::memory_order_relaxed);
            draw_compass(cfg, snap, hud_ok);
            draw_highlight(cfg, snap, hud_ok);

            // The clipboard result comes back from the loop thread as text plus a flag;
            // turn it into a toast here, where toasts live.
            if (g_toast_pending_ready.exchange(false, std::memory_order_acquire))
            {
                char text[160]{};
                unsigned ms = 2500;
                {
                    spin::SpinGuard guard(g_toast_lock);
                    ::strncpy_s(text, sizeof(text), g_toast_pending, _TRUNCATE);
                    ms = g_toast_pending_ms;
                }
                toast_for(text, ms);
            }

            // Toasts last, so they sit over everything they are talking about.
            draw_toast();
        }

        bool ensure_initialised(IDXGISwapChain* swapchain)
        {
            if (g_failed)
            {
                return false;
            }
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            if (queue == nullptr)
            {
                return false; // no ExecuteCommandLists yet; try again next frame
            }
            if (g_imgui_ready)  // NOLINT: the happy path, checked every frame
            {
                return true;
            }

            // The device comes off the CAPTURED QUEUE, not off the swapchain.
            // IDXGISwapChain::GetDevice(ID3D12Device) fails on this game's swapchain -
            // it is a ReShade wrapper (all four hooked addresses live in the 5.6 MB
            // dxgi.dll ReShade drops next to the exe) and its GetDevice does not hand
            // out the D3D12 device. ID3D12CommandQueue::GetDevice always does.
            HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g_device));
            if (FAILED(hr) || g_device == nullptr)
            {
                mm::logf(L"queue->GetDevice failed (0x{:08X}); trying the swapchain", static_cast<unsigned>(hr));
                hr = swapchain->GetDevice(IID_PPV_ARGS(&g_device));
            }
            if (FAILED(hr) || g_device == nullptr)
            {
                mm::logf(L"no ID3D12Device reachable from either the queue or the swapchain (0x{:08X}) - overlay off",
                         static_cast<unsigned>(hr));
                g_failed = true;
                return false;
            }

            // NOT `g_failed`: a swapchain that will not hand out its buffers is a
            // swapchain-level failure (a resize in flight, a device that has just gone,
            // a wrapper being swapped), and those recover. `create_render_targets` has
            // already asked for a re-adoption where it knew the reason.
            if (!create_render_targets(swapchain))
            {
                request_readoption(L"the render targets could not be created");
                return false;
            }

            // One allocator per back buffer, and grown again after any resize that
            // raises BufferCount (see ensure_frame_allocators).
            if (!ensure_frame_allocators())
            {
                g_failed = true;
                return false;
            }
            if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr,
                                                   IID_PPV_ARGS(&g_cmd_list))))
            {
                mm::log(L"CreateCommandList failed");
                g_failed = true;
                return false;
            }
            g_cmd_list->Close();

            if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            {
                mm::log(L"CreateFence failed");
                g_failed = true;
                return false;
            }
            g_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_fence_event == nullptr)
            {
                mm::log(L"CreateEvent failed");
                g_failed = true;
                return false;
            }

            // RESTART-ONLY config key: the heap is created once, here.
            if (!g_srv_heap.create(g_device, mm::config().srv_heap_size))
            {
                mm::log(L"CreateDescriptorHeap(SRV) failed");
                g_failed = true;
                return false;
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // We draw our own software cursor for the panel; never let ImGui fight the
            // game over the OS cursor shape.
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            // [fix-ui] keyboard + gamepad navigation for the F2 panel (review B.8).
            ui_init_io(io);
            ImGui::StyleColorsDark();
            ImGui::GetStyle().WindowRounding = 4.0f;

            if (!ImGui_ImplWin32_Init(g_hwnd))
            {
                mm::log(L"ImGui_ImplWin32_Init failed");
                g_failed = true;
                return false;
            }

            ImGui_ImplDX12_InitInfo info{};
            info.Device = g_device;
            info.CommandQueue = queue;
            info.NumFramesInFlight = static_cast<int>(g_buffer_count);
            info.RTVFormat = g_format;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.SrvDescriptorHeap = g_srv_heap.heap();
            info.SrvDescriptorAllocFn = &srv_alloc_cb;
            info.SrvDescriptorFreeFn = &srv_free_cb;
            if (!ImGui_ImplDX12_Init(&info))
            {
                mm::log(L"ImGui_ImplDX12_Init failed");
                ImGui_ImplWin32_Shutdown();
                g_failed = true;
                return false;
            }

            // WHICH DEVICES THE GAME READS THROUGH WM_INPUT. This decides whether
            // swallowing a key as a window message is enough: a game that registers a
            // raw KEYBOARD also has to have the raw packet filtered (which the panel's
            // Esc path does), and one that registers only the mouse does not. It is one
            // call, once, and it turns "does UE read Esc through raw input?" from a
            // guess into a log line.
            {
                UINT count = 0;
                if (::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == 0 &&
                    count > 0 && count < 64)
                {
                    std::vector<RAWINPUTDEVICE> devs(count);
                    if (::GetRegisteredRawInputDevices(devs.data(), &count, sizeof(RAWINPUTDEVICE)) !=
                        static_cast<UINT>(-1))
                    {
                        std::wstring list;
                        for (UINT i = 0; i < count && i < devs.size(); ++i)
                        {
                            list += std::format(L"{}usage {:#x}/{:#x} flags {:#x}",
                                                list.empty() ? L"" : L", ",
                                                devs[i].usUsagePage,
                                                devs[i].usUsage,
                                                devs[i].dwFlags);
                        }
                        mm::logf(L"raw input: the game has {} device(s) registered ({}). Usage 1/6 is a "
                                 L"KEYBOARD - if it is in that list, keys reach the game through WM_INPUT "
                                 L"as well as WM_KEYDOWN.",
                                 count,
                                 list);
                    }
                }
                else
                {
                    mm::log(L"raw input: the game has no raw-input devices registered - every key and "
                            L"mouse move reaches it as a window message only");
                }
            }

            hook_wndproc();

            g_imgui_frames_in_flight = static_cast<int>(g_buffer_count);
            g_imgui_ready = true;
            crumb::stage(crumb::kImGuiUp);
            mm::logf(L"ImGui {} initialised on the game's swapchain: device {:p}, queue {:p}, {} frames in flight, "
                     L"RTV {}",
                     std::wstring(IMGUI_VERSION, IMGUI_VERSION + std::strlen(IMGUI_VERSION)),
                     static_cast<void*>(g_device),
                     static_cast<void*>(queue),
                     g_buffer_count,
                     format_name(g_format));
            slice_selftest();
            return true;
        }


        // NEVER GUESSES. This used to fall back to a rotating counter when
        // IDXGISwapChain3 was unavailable, which is worse than doing nothing: the index
        // decides which resource the PRESENT -> RENDER_TARGET barrier is issued on, and
        // a barrier declaring the wrong before-state on a resource that is not in it is
        // a device-removal-class error (and, with the wrong RTV, a frame drawn into the
        // buffer the display is scanning out). The interface is QI'd once at adoption
        // and cached; if it is not there, the caller skips the frame.
        UINT current_backbuffer_index(IDXGISwapChain* swapchain)
        {
            if (g_sc3 == nullptr)
            {
                if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&g_sc3))) || g_sc3 == nullptr)
                {
                    g_sc3 = nullptr;
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        mm::log(L"this swapchain does not expose IDXGISwapChain3, so the back-buffer "
                                L"index cannot be known - the overlay will not draw on it (guessing the "
                                L"index would put a resource barrier on the wrong buffer)");
                    }
                    return kNoBackbuffer;
                }
            }
            const UINT index = g_sc3->GetCurrentBackBufferIndex();
            return index < g_buffer_count ? index : kNoBackbuffer;
        }

        // A D3D12 swapchain hands out ID3D12Resource back buffers; a D3D11 one, or a
        // wrapper that does not forward, does not. That is the cheapest reliable test.
        bool is_d3d12_swapchain(IDXGISwapChain* sc)
        {
            ID3D12Resource* buffer = nullptr;
            const HRESULT hr = sc->GetBuffer(0, IID_PPV_ARGS(&buffer));
            safe_release(buffer);
            return SUCCEEDED(hr);
        }

        void log_candidate(IDXGISwapChain* sc, bool d3d12)
        {
            // One line per distinct swapchain, not per Present: the game's decoy 144x8
            // D3D11 swapchain presents every frame.
            static IDXGISwapChain* seen[8]{};
            for (IDXGISwapChain* s : seen)
            {
                if (s == sc)
                {
                    return;
                }
            }
            if (g_candidates_logged >= static_cast<int>(std::size(seen)))
            {
                return;
            }
            seen[g_candidates_logged] = sc;
            ++g_candidates_logged;
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            void** vtable = *reinterpret_cast<void***>(sc);
            mm::logf(L"Present candidate {:p}: {}x{} {} x{} buffers, hwnd 0x{:X}, vtable[8] {} -> {}",
                     static_cast<void*>(sc),
                     desc.BufferDesc.Width,
                     desc.BufferDesc.Height,
                     format_name(desc.BufferDesc.Format),
                     desc.BufferCount,
                     reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                     module_of(vtable[8]),
                     d3d12 ? L"D3D12, taking it" : L"not D3D12, ignored");
        }

        //==============================================================================
        // Render-side teardown for the master switch (RENDER THREAD ONLY)
        //==============================================================================
        //
        // Everything below is a D3D12 object or an ImGui context, and both may only be
        // touched from the thread that created them - which is whichever thread calls
        // Present. So the loop thread clears mm::g_mod_active and this runs inside the
        // next Present, before the hooks are taken out.
        //
        // It leaves the module in exactly the state it had before the first frame:
        // `start()` re-enables the hooks, ExecuteCommandLists re-captures the queue and
        // ensure_initialised() builds everything again.
        //
        // TWO CALLERS, ONE BODY. The master switch (shutdown_render) and a lost device /
        // replaced swapchain (the re-adoption path in render()) release exactly the same
        // objects; the only difference is that the master switch also answers the loop
        // thread's handshake with `g_render_stopped`, which a re-adoption must NOT touch
        // or the loop thread would believe a disable it never asked for had completed.

        void release_device_objects()
        {
            if (g_imgui_ready || g_device != nullptr)
            {
                crumb::stage(crumb::kTeardownBegin);
                // The slicer writes into mapped upload heaps we are about to release.
                // It is a few milliseconds of arithmetic and it re-checks the pause flag
                // on entry, so this always succeeds; if it somehow did not we would
                // rather leak the buffers than free memory under a live writer.
                const bool paused = slicer_pause_begin(1000);
                wait_for_gpu();
                if (!paused)
                {
                    mm::log(L"slice: the loop-thread slicer did not stand down in 1 s - "
                            L"the slice buffers are left allocated on purpose");
                }
                else
                {
                    destroy_all_map_textures();
                }
                slicer_pause_end();
                if (g_imgui_ready)
                {
                    unhook_wndproc();
                    ImGui_ImplDX12_Shutdown();
                    ImGui_ImplWin32_Shutdown();
                    ImGui::DestroyContext();
                    g_imgui_ready = false;
                    // The style went with the context: the next init must rebuild it.
                    g_ui_scale_applied = 0.0f;
                }
                release_render_targets();
                for (UINT i = 0; i < kMaxBuffers; ++i)
                {
                    safe_release(g_frames[i].allocator);
                    g_frames[i].fence_value = 0;
                }
                safe_release(g_cmd_list);
                safe_release(g_fence);
                if (g_fence_event != nullptr)
                {
                    ::CloseHandle(g_fence_event);
                    g_fence_event = nullptr;
                }
                g_fence_value = 0;
                g_srv_heap.destroy();
                safe_release(g_device);
                g_imgui_frames_in_flight = 0;
                mm::log(L"the render thread has released ImGui, the descriptor heaps, "
                        L"the slice buffers and the map textures");
            }
            g_swapchain = nullptr;
            // The cached IDXGISwapChain3 holds a reference on the swapchain we are
            // letting go of; keeping it would pin a dead object and, worse, answer
            // GetCurrentBackBufferIndex for a swapchain we no longer draw on.
            safe_release(g_sc3);
            g_candidates_logged = 0;
            // The queue was captured with a reference held (see hk_ExecuteCommandLists),
            // so dropping it means releasing it.
            safe_release_queue();
            g_failed = false;
            // Everything a re-adoption would have released is gone already.
            g_readopt.store(false, std::memory_order_release);
            crumb::stage(crumb::kTeardownEnd);
        }

        void shutdown_render()
        {
            release_device_objects();
            // THE MASTER SWITCH'S HANDSHAKE, and the one thing a re-adoption must not do.
            g_render_stopped.store(true, std::memory_order_release);
        }

        void render(IDXGISwapChain* swapchain)
        {
            // THE MASTER SWITCH, first statement. One relaxed atomic load per Present
            // while the mod is off - and the one Present that first sees it off does the
            // teardown, because this is the only thread allowed to.
            if (!mm::mod_active())
            {
                if (!g_render_stopped.load(std::memory_order_acquire))
                {
                    spin::SpinGuard guard(g_render_lock);
                    shutdown_render();
                }
                return;
            }

            g_present_count.fetch_add(1, std::memory_order_relaxed);
            g_render_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
            g_render_stage.store("prologue", std::memory_order_relaxed);
            if (g_failed || g_queue.load(std::memory_order_acquire) == nullptr)
            {
                return;
            }

            g_render_stage.store("waiting for the render lock", std::memory_order_relaxed);
            spin::SpinGuard guard(g_render_lock);
            g_render_stage.store("holding the render lock", std::memory_order_relaxed);

            // RE-ADOPTION, and it happens here because this is the only thread that may
            // touch a D3D12 object. Whatever asked for it (a removed device, a swapchain
            // that stopped handing out buffers, a queue from the wrong device) has left
            // this module holding objects that belong to something that no longer
            // exists; releasing them and starting over is what lets the overlay come
            // back by itself after a driver reset or a swapchain swap. `g_failed` is
            // deliberately NOT set: this path is recoverable, that flag is not.
            if (g_readopt.exchange(false, std::memory_order_acquire))
            {
                g_render_stage.store("re-adopting the swapchain", std::memory_order_relaxed);
                mm::perf_note_stall(L"a swapchain / device re-adoption", 2000);
                release_device_objects();
                return; // the next Present adopts whatever is there now
            }

            if (g_swapchain == nullptr)
            {
                const bool d3d12 = is_d3d12_swapchain(swapchain);
                log_candidate(swapchain, d3d12);
                if (!d3d12)
                {
                    return;
                }
                g_swapchain = swapchain;
                crumb::stage(crumb::kSwapchainChosen);
            }
            else if (swapchain != g_swapchain)
            {
                return; // another swapchain (frame generation / ReShade) - not ours
            }

            if (!ensure_initialised(swapchain))
            {
                return;
            }
            if (!g_rt_ready && !create_render_targets(swapchain))
            {
                // A GetBuffer / RTV failure is recoverable (a resize in flight, a device
                // that has gone): hand it to the re-adoption path instead of retrying
                // the same objects every frame for ever.
                request_readoption(L"the render targets could not be rebuilt");
                return;
            }
            // AFTER a resize that raised BufferCount, the ImGui backend still has one
            // set of per-frame buffers per OLD frame in flight and would reuse the
            // vertex, index and descriptor storage of a frame the GPU has not finished.
            // Re-initialising the DX12 backend is the only way to change that count; it
            // happens outside a frame (before NewFrame) and with the GPU idle.
            if (g_imgui_ready && g_imgui_frames_in_flight != 0 &&
                g_imgui_frames_in_flight < static_cast<int>(g_buffer_count))
            {
                mm::logf(L"the swapchain now has {} buffers, ImGui was initialised for {} frames in "
                         L"flight - re-initialising the DX12 backend",
                         g_buffer_count,
                         g_imgui_frames_in_flight);
                wait_for_gpu();
                ImGui_ImplDX12_Shutdown();
                ImGui_ImplDX12_InitInfo info{};
                info.Device = g_device;
                info.CommandQueue = g_queue.load(std::memory_order_acquire);
                info.NumFramesInFlight = static_cast<int>(g_buffer_count);
                info.RTVFormat = g_format;
                info.DSVFormat = DXGI_FORMAT_UNKNOWN;
                info.SrvDescriptorHeap = g_srv_heap.heap();
                info.SrvDescriptorAllocFn = &srv_alloc_cb;
                info.SrvDescriptorFreeFn = &srv_free_cb;
                if (!ImGui_ImplDX12_Init(&info))
                {
                    mm::log(L"ImGui_ImplDX12_Init failed on the re-init after a resize - overlay off");
                    g_failed = true;
                    return;
                }
                g_imgui_frames_in_flight = static_cast<int>(g_buffer_count);
            }

            const UINT index = current_backbuffer_index(swapchain);
            if (index == kNoBackbuffer)
            {
                // No index, no frame. Drawing without knowing which buffer is next means
                // barriering the wrong resource - see current_backbuffer_index.
                return;
            }
            FrameCtx& frame = g_frames[index];
            if (frame.allocator == nullptr)
            {
                // Cannot happen now that the allocators are grown with BufferCount; it
                // stays as the cheap guard that turns the old null-Reset() crash into a
                // dropped frame.
                request_readoption(L"a back buffer has no command allocator");
                return;
            }
            g_render_stage.store("waiting for this frame's fence", std::memory_order_relaxed);
            if (frame.fence_value != 0 && g_fence->GetCompletedValue() < frame.fence_value)
            {
                if (SUCCEEDED(g_fence->SetEventOnCompletion(frame.fence_value, g_fence_event)))
                {
                    ::WaitForSingleObject(g_fence_event, 500);
                }
            }

            // A reload (F5) rebuilds the whole texture set, so the old textures go
            // first - and they may only be released here, on the render thread, and
            // BEFORE the frame's draw lists are built, or this frame would reference
            // an SRV slot we just handed back.
            if (g_drop_textures.load(std::memory_order_acquire))
            {
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    g_drop_textures.store(false, std::memory_order_release);
                    // A full GPU flush plus ~340 MB of releases: a one-off, and one that
                    // must not become the peak every later frame is judged against.
                    mm::perf_note_stall(L"a map texture reload (F5)", 2000);
                    wait_for_gpu();
                    destroy_all_map_textures();
                    slicer_pause_end();
                    mm::log(L"map textures and slice buffers dropped for a reload");
                }
                // else: the request stays pending and the next frame retries. Never
                // release a resource the loop thread may still be writing into.
            }
            release_finished_uploads();

            if (g_pf_frame < 0)
            {
                g_pf_frame = mm::perf_register("render frame (ImGui)", perf::Thread::Render);
            }
            const std::uint64_t frame_t0 = mm::qpc_us();
            // The UI scale, decided from the CURRENT back buffer and applied before the
            // frame's draw lists exist. ResizeBuffers changes g_height, and a config
            // change comes through cfg_cached, so both re-enter here on their own.
            apply_ui_scale(wanted_ui_scale(mm::cfg_cached(), static_cast<float>(g_height)));
            // TWO SUB-COUNTERS, because the 358 ms peak this row showed after 30 minutes
            // had to be attributed to one side or the other. ImGui_ImplWin32_NewFrame is
            // the suspect: it reads and writes the CURSOR and the client rect of a window
            // owned by the GAME thread, and a cross-thread user32 call blocks until that
            // thread pumps messages - which it does not do while it is inside a
            // synchronous level load. build_ui() is our own drawing and touches no OS
            // handle at all. Whichever one carries the peak, the table now says so.
            if (g_pf_newframe < 0)
            {
                g_pf_newframe = mm::perf_register("render NewFrame (win32)", perf::Thread::Render);
                g_pf_buildui = mm::perf_register("render build_ui", perf::Thread::Render);
            }
            {
                const mm::PerfScope nf(g_pf_newframe);
                // The game thread's window messages, in order, on the one thread that
                // is allowed to touch the ImGui context.
                g_render_stage.store("imgui: replaying window messages", std::memory_order_relaxed);
                replay_imgui_messages();
                // CROSS-THREAD USER32, and the render lock is held across it. Everything
                // in here reads or writes the cursor and the client rect of a window
                // owned by the GAME thread, so it is the one place in the frame that can
                // wait on another thread - which is why `hk_ResizeBuffers` (the only
                // other taker of that lock, and a call that can arrive on the game
                // thread) acquires it with a bound instead of spinning for ever.
                g_render_stage.store("imgui: ImplWin32_NewFrame (user32)", std::memory_order_relaxed);
                ImGui_ImplWin32_NewFrame();
            }
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            {
                const mm::PerfScope bu(g_pf_buildui);
                g_render_stage.store("build_ui", std::memory_order_relaxed);
                build_ui();
            }
            ImGui::Render();
            // The game thread's swallow decision reads this instead of the context.
            g_imgui_want_keyboard.store(ImGui::GetIO().WantCaptureKeyboard, std::memory_order_relaxed);
            mm::perf_record(g_pf_frame, frame_t0);

            if (FAILED(frame.allocator->Reset()) || FAILED(g_cmd_list->Reset(frame.allocator, nullptr)))
            {
                return;
            }

            // One image per frame: a nine-layer chapter is resident after ~9 frames
            // instead of stalling a single one with ~100 MB of copies.
            std::unique_ptr<mapdata::PendingImage> pending = mapdata::take_pending();
            if (pending != nullptr)
            {
                begin_map_upload(*pending, g_cmd_list);
            }

            // The height-slice window the CPU filled during build_ui(). Recorded here,
            // i.e. BEFORE ImGui's draw call in the same command list, so the GPU sees
            // the copy complete before it samples the texture - no extra queue, no
            // second submission and no PSO of our own.
            record_slice_copy(g_cmd_list);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = g_backbuffers[index];
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_cmd_list->ResourceBarrier(1, &barrier);

            g_cmd_list->OMSetRenderTargets(1, &g_rtv[index], FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = {g_srv_heap.heap()};
            g_cmd_list->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list);

            // The screenshot copy, if one was asked for: it takes the back buffer from
            // RENDER_TARGET to PRESENT itself (via COPY_SOURCE), so it REPLACES the
            // closing barrier below rather than adding to it.
            const bool shot_recorded = record_shot_copy(g_cmd_list, g_backbuffers[index], index);
            if (!shot_recorded)
            {
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                g_cmd_list->ResourceBarrier(1, &barrier);
            }
            g_cmd_list->Close();

            g_render_stage.store("submitting the command list", std::memory_order_relaxed);
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            ID3D12CommandList* lists[] = {g_cmd_list};
            // The ORIGINAL, so our own submission does not re-enter the hook.
            o_ExecuteCommandLists(queue, 1, lists);
            ++g_fence_value;
            queue->Signal(g_fence, g_fence_value);
            frame.fence_value = g_fence_value;
            if (shot_recorded)
            {
                g_shot_fence = g_fence_value; // the readback may not be mapped before this
            }
            // The buffer this frame sampled may not be rewritten until the GPU is past
            // this fence.
            {
                const int shown = slice_view().shown;
                if (shown >= 0)
                {
                    g_slice_in_flight[shown].store(g_fence_value, std::memory_order_release);
                }
            }
            {
                const int shown = map_slice_view().shown;
                if (shown >= 0)
                {
                    g_mslice_in_flight[shown].store(g_fence_value, std::memory_order_release);
                }
            }

            if (g_map.upload != nullptr && g_map.upload_fence == 0)
            {
                g_map.upload_fence = g_fence_value;
            }
            else if (g_map.upload != nullptr && g_fence->GetCompletedValue() >= g_map.upload_fence)
            {
                safe_release(g_map.upload); // the copy has landed; give the 90 MB back
            }
            g_render_stage.store("between frames", std::memory_order_relaxed);
        }

        //==============================================================================
        // Hooks
        //==============================================================================

        // THE EXCEPTION BARRIER.
        //
        // Everything our frame does - std::format for a panel line, a vector that grows
        // while markers are collected, the ImGui context itself - can throw
        // std::bad_alloc, and a throw here would unwind THROUGH the MinHook trampoline
        // and into DXGI, i.e. through frames that were compiled with no idea our code
        // exists. That is a hard crash inside the graphics driver stack with a call
        // stack that names dxgi.dll and not this mod, which is the worst possible
        // diagnostic for the player who has to report it.
        //
        // So the whole frame is wrapped, once, at the hook boundary: a throw becomes one
        // log line and a dead overlay, and the game keeps presenting.
        //
        // NOT SEH. An access violation is deliberately left to crash: __try cannot live
        // in a function that needs C++ unwinding (MSVC C2712, lessons.md), so it would
        // take a second POD-only trampoline - and swallowing an AV in the middle of our
        // command-list recording would leave the list open and the back buffer stranded
        // between resource states, which the next frames turn into a device removal with
        // no evidence left. The crash breadcrumb plus CrashContext.runtime-xml is the
        // route that has actually diagnosed every fault in this project so far.
        void render_guarded(IDXGISwapChain* sc)
        {
            try
            {
                render(sc);
            }
            catch (const std::exception& e)
            {
                if (!g_failed.exchange(true))
                {
                    const char* what = e.what() != nullptr ? e.what() : "?";
                    mm::logf(L"an exception escaped the overlay's frame ({}) - the overlay is off for "
                             L"the rest of this session; the game is unaffected",
                             stage_w(what));
                }
            }
            catch (...)
            {
                if (!g_failed.exchange(true))
                {
                    mm::log(L"a non-standard exception escaped the overlay's frame - the overlay is off "
                            L"for the rest of this session; the game is unaffected");
                }
            }
        }

        // The screenshot readback: mapped, unpacked and turned into a DIB OUTSIDE the
        // render lock. It used to run inside render(), which holds that lock across the
        // whole frame - and `hk_ResizeBuffers` waits on the same lock from the GAME
        // thread, so a full-canvas unpack (two allocations plus a per-row conversion of
        // up to ~8 MB) sat directly in front of a resize. Same thread, same point in the
        // frame, no lock held: the readback resource and the fence are render-thread-only
        // objects and this is the render thread.
        void collect_guarded()
        {
            try
            {
                shot_collect();
            }
            catch (...)
            {
                mm::log(L"the screenshot readback threw - the map copy is dropped");
            }
        }

        // DID THE PRESENT ITSELF FAIL? The HRESULT used to be discarded, which is how a
        // TDR or a driver reset left the overlay silently dead for the rest of the
        // session: every later frame drew into resources belonging to a device that no
        // longer exists. Both removal codes mean "everything we hold is gone", so the
        // next Present starts over from an empty state.
        void note_present_result(HRESULT hr)
        {
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                request_readoption(hr == DXGI_ERROR_DEVICE_REMOVED ? L"Present returned DEVICE_REMOVED"
                                                                   : L"Present returned DEVICE_RESET");
            }
        }

        HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            // THE ORIGINAL IS ALWAYS CALLED, for every swapchain in the process - see
            // the comment on this hook's install: Present is one dxgi function shared by
            // every swapchain, D3D11 and D3D12 alike, and returning early for "not ours"
            // would stop somebody else's overlay from presenting at all.
            const HRESULT hr = o_Present(sc, sync, flags);
            if (sc == g_swapchain)
            {
                note_present_result(hr);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
        {
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            const HRESULT hr = o_Present1(sc, sync, flags, params);
            if (sc == g_swapchain)
            {
                note_present_result(hr);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                                   UINT flags)
        {
            if (!mm::mod_active())
            {
                return o_ResizeBuffers(sc, count, w, h, format, flags);
            }
            g_resize_count.fetch_add(1, std::memory_order_relaxed);
            // A resize means a device-level stall (a resolution or fullscreen change, or
            // the tail of a level load): the frames around it are wall-clock waits, not
            // this mod's cost, so they go to the stall columns of the F2 table.
            mm::perf_note_stall(L"a swapchain resize", 2000);
            // A BOUNDED ACQUIRE, NOT A SPIN. This call can arrive on the game thread,
            // and the render thread holds this same lock across
            // `ImGui_ImplWin32_NewFrame()`, which reads and writes the cursor and the
            // client rect of a window the GAME thread owns. Spinning here for ever is
            // therefore a two-thread deadlock with no diagnostic; a bound turns the
            // worst case into a named log line and a resize that may fail, which the
            // next Present recovers from by recreating its render targets.
            if (g_render_lock.try_lock_ms(2000))
            {
                // OURS OR NOT, TESTED BEFORE THE LINE IS FORMATTED. Every swapchain in
                // the process comes through this one function, and formatting a log line
                // for each of them (the game presents a decoy 144x8 D3D11 swapchain too)
                // put a wstring allocation and a log write in front of resizes that have
                // nothing to do with this mod.
                if (sc == g_swapchain)
                {
                    mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}) - releasing render targets",
                             count,
                             w,
                             h,
                             format_name(format));
                    // A throw in here would unwind into DXGI through the trampoline, the
                    // same hazard the Present barrier exists for - and this one runs on
                    // the GAME thread, where it would take the game down with it.
                    try
                    {
                        if (g_imgui_ready)
                        {
                            wait_for_gpu();
                        }
                        release_render_targets();
                    }
                    catch (...)
                    {
                        mm::log(L"an exception escaped the ResizeBuffers path - the overlay is off for "
                                L"the rest of this session; the resize itself still happens");
                        g_failed = true;
                    }
                }
                g_render_lock.unlock();
            }
            else if (sc == g_swapchain)
            {
                mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}): the render lock was still held after "
                         L"2000 ms, so our render targets were NOT released. The resize may fail and "
                         L"the next Present will rebuild them. Render thread {} was at '{}'.",
                         count,
                         w,
                         h,
                         format_name(format),
                         g_render_tid.load(std::memory_order_relaxed),
                         stage_w(g_render_stage.load(std::memory_order_relaxed)));
            }
            const HRESULT hr = o_ResizeBuffers(sc, count, w, h, format, flags);
            // The RTVs are recreated lazily on the next Present, once the swapchain has
            // its new buffers.
            if (FAILED(hr))
            {
                mm::logf(L"ResizeBuffers failed (0x{:08X})", static_cast<unsigned>(hr));
            }
            return hr;
        }

        void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                      ID3D12CommandList* const* lists)
        {
            if (mm::mod_active() && g_queue.load(std::memory_order_relaxed) == nullptr && queue != nullptr &&
                queue != g_bad_queue.load(std::memory_order_acquire))
            {
                const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
                if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    ID3D12CommandQueue* expected = nullptr;
                    if (g_queue.compare_exchange_strong(expected, queue))
                    {
                        // A REFERENCE IS HELD FROM HERE UNTIL THE TEARDOWN RELEASES IT.
                        // The pointer was borrowed before: the queue is the game's, and
                        // a game that destroys it (a device reset, a renderer swap) left
                        // this module submitting command lists to freed memory. One
                        // AddRef costs nothing and makes the pointer valid for as long
                        // as we hold it.
                        queue->AddRef();
                        mm::logf(L"captured the game's DIRECT command queue {:p} (priority {}, flags {})",
                                 static_cast<void*>(queue),
                                 desc.Priority,
                                 static_cast<unsigned>(desc.Flags));
                        // WHICH QUEUE, AND WHY IT MAY BE THE WRONG ONE. There is no way
                        // to ask this game's swapchain which queue presented it: it is a
                        // ReShade wrapper and IDXGISwapChain::GetDevice does not even
                        // forward, so "the queue that executed last before Present on
                        // our swapchain" is not derivable here - ExecuteCommandLists is
                        // hooked process-wide and every renderer in the process (the
                        // game, ReShade's own effects, a frame-generation runtime) uses
                        // it. What IS checkable is the device: create_render_targets
                        // compares the back buffer's ID3D12Device with the one this
                        // queue hands out and rejects the queue on a mismatch, which is
                        // the failure that would actually matter.
                    }
                }
            }
            o_ExecuteCommandLists(queue, count, lists);
        }

        //==============================================================================
        // Hook installation via a throwaway device + swapchain
        //==============================================================================

        //==============================================================================
        // THE HOOK-ADDRESS CACHE (and why the Steam overlay wants it)
        //==============================================================================
        //
        // Discovery creates a throwaway D3D12 device, a DIRECT command queue and a 64x64
        // swapchain on a hidden window, reads four vtable slots and destroys all three.
        // That is the hudhook recipe and it is what found the addresses in the first
        // place - but Steam's GameOverlayRenderer64 hooks the device-, queue- and
        // swapchain-creating entry points and re-targets its overlay onto what it sees
        // created. Ours are created LATER than the game's (this runs from
        // on_unreal_init, long after RHI init) and are then destroyed, which is a
        // textbook way to leave the Steam overlay pointed at a dead object - the
        // reported symptom, "the Steam FPS counter stopped rendering with the mod".
        //
        // The addresses, though, are a property of the DLL and not of the session: the
        // first launch writes them down as module + RVA, and every launch after that
        // hooks them directly and creates NOTHING. The cache is keyed to the module's
        // SizeOfImage, TimeDateStamp and CheckSum - all three baked into the file - so a
        // ReShade, driver or Windows update invalidates it and discovery runs once more.
        // If cached addresses ever produce no Present at all, the watchdog deletes the
        // file, so a stale cache costs one launch and heals itself.


        std::wstring hook_cache_path()
        {
            return mm::mod_dir() + L"\\wuchang_minimap_hookaddr.txt";
        }

        bool write_hook_cache(const ModuleId* ids)
        {
            std::wstring text = L"; WuchangMinimap - the DX12 hook addresses found on a previous launch.\n"
                                L"; Deleting this file forces a fresh discovery; it is rewritten by itself\n"
                                L"; whenever one of these modules changes. schema 1\n"
                                L"; <what> = <module> <rva> <SizeOfImage> <TimeDateStamp> <CheckSum>\n";
            for (int i = 0; i < kHookCount; ++i)
            {
                if (ids[i].name[0] == L'\0' || ids[i].rva == 0)
                {
                    return false;
                }
                text += std::format(L"{} = {} 0x{:X} 0x{:X} 0x{:08X} 0x{:08X}\n",
                                    kHookNames[i],
                                    ids[i].name,
                                    ids[i].rva,
                                    ids[i].size,
                                    ids[i].stamp,
                                    ids[i].sum);
            }
            HANDLE h = ::CreateFileW(hook_cache_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            std::string narrow;
            narrow.reserve(text.size());
            for (const wchar_t c : text)
            {
                narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            DWORD wrote = 0;
            const bool ok =
                ::WriteFile(h, narrow.data(), static_cast<DWORD>(narrow.size()), &wrote, nullptr) != 0;
            ::CloseHandle(h);
            return ok;
        }

        // A hex field ("0x1F" or "1F"). False on anything else, so a hand-edited or
        // truncated file is refused rather than half-read.
        bool parse_hex_field(std::string_view t, std::uint64_t& out)
        {
            if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
            {
                t.remove_prefix(2);
            }
            if (t.empty() || t.size() > 16)
            {
                return false;
            }
            std::uint64_t v = 0;
            for (const char c : t)
            {
                int d = -1;
                if (c >= '0' && c <= '9')
                {
                    d = c - '0';
                }
                else if (c >= 'a' && c <= 'f')
                {
                    d = c - 'a' + 10;
                }
                else if (c >= 'A' && c <= 'F')
                {
                    d = c - 'A' + 10;
                }
                if (d < 0)
                {
                    return false;
                }
                v = v * 16 + static_cast<std::uint64_t>(d);
            }
            out = v;
            return true;
        }

        // Resolves every entry against the module loaded RIGHT NOW. Any mismatch refuses
        // the WHOLE cache: a half-valid one would hook an address inside the wrong DLL,
        // which is a crash rather than a missing overlay.
        bool read_hook_cache(void** addr)
        {
            HANDLE h = ::CreateFileW(hook_cache_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            char buf[4096]{};
            DWORD got = 0;
            const bool read_ok = ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) != 0;
            ::CloseHandle(h);
            if (!read_ok || got == 0)
            {
                return false;
            }
            const std::string text{buf, buf + got};

            for (int i = 0; i < kHookCount; ++i)
            {
                addr[i] = nullptr;
            }
            int found = 0;
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t nl = text.find('\n', at);
                if (nl == std::string::npos)
                {
                    nl = text.size();
                }
                std::string_view line{text.data() + at, nl - at};
                at = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                {
                    line.remove_suffix(1);
                }
                if (line.empty() || line.front() == ';')
                {
                    continue;
                }
                const std::size_t eq = line.find(" = ");
                if (eq == std::string_view::npos)
                {
                    continue;
                }
                const std::string_view key = line.substr(0, eq);
                std::string_view rest = line.substr(eq + 3);

                int slot = -1;
                for (int i = 0; i < kHookCount; ++i)
                {
                    std::string want;
                    for (const wchar_t* p = kHookNames[i]; *p != L'\0'; ++p)
                    {
                        want.push_back(static_cast<char>(*p));
                    }
                    if (key == want)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0)
                {
                    continue;
                }

                std::string_view field[5];
                int n = 0;
                while (n < 5 && !rest.empty())
                {
                    const std::size_t sp = rest.find(' ');
                    field[n++] = rest.substr(0, sp);
                    rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
                }
                std::uint64_t rva = 0;
                std::uint64_t size = 0;
                std::uint64_t stamp = 0;
                std::uint64_t sum = 0;
                if (n != 5 || field[0].empty() || field[0].size() + 1 >= 64 ||
                    !parse_hex_field(field[1], rva) || !parse_hex_field(field[2], size) ||
                    !parse_hex_field(field[3], stamp) || !parse_hex_field(field[4], sum))
                {
                    mm::log(L"hook cache: a malformed line - falling back to discovery");
                    return false;
                }

                wchar_t wname[64]{};
                for (std::size_t k = 0; k < field[0].size(); ++k)
                {
                    wname[k] = static_cast<wchar_t>(field[0][k]);
                }
                const HMODULE mod = ::GetModuleHandleW(wname);
                ModuleId live{};
                if (mod == nullptr || !module_identity(mod, live))
                {
                    mm::logf(L"hook cache: '{}' is not loaded - falling back to discovery", wname);
                    return false;
                }
                if (live.size != static_cast<std::uint32_t>(size) ||
                    live.stamp != static_cast<std::uint32_t>(stamp) ||
                    live.sum != static_cast<std::uint32_t>(sum))
                {
                    mm::logf(L"hook cache: {} is a different build now (size 0x{:X} vs 0x{:X}, stamp "
                             L"0x{:08X} vs 0x{:08X}, sum 0x{:08X} vs 0x{:08X}) - falling back to discovery",
                             wname,
                             live.size,
                             static_cast<std::uint32_t>(size),
                             live.stamp,
                             static_cast<std::uint32_t>(stamp),
                             live.sum,
                             static_cast<std::uint32_t>(sum));
                    return false;
                }
                if (rva == 0 || rva >= live.size)
                {
                    return false;
                }
                addr[slot] = reinterpret_cast<std::uint8_t*>(mod) + rva;
                ++found;
            }
            if (found != kHookCount)
            {
                mm::logf(L"hook cache: {} of {} entries resolved - falling back to discovery",
                         found,
                         kHookCount);
                return false;
            }
            return true;
        }

        void delete_hook_cache(const wchar_t* why)
        {
            if (::DeleteFileW(hook_cache_path().c_str()) != 0)
            {
                mm::logf(L"hook cache: deleted ({}). The next launch rediscovers the addresses.", why);
            }
        }

        // Both routes end here: four MH_CreateHook calls, one MH_EnableHook, one report.
        bool create_and_enable(void** addr, const wchar_t* how)
        {
            const MH_STATUS s1 = MH_CreateHook(addr[0], reinterpret_cast<void*>(&hk_Present),
                                               reinterpret_cast<void**>(&o_Present));
            const MH_STATUS s2 = MH_CreateHook(addr[1], reinterpret_cast<void*>(&hk_ResizeBuffers),
                                               reinterpret_cast<void**>(&o_ResizeBuffers));
            const MH_STATUS s3 = MH_CreateHook(addr[2], reinterpret_cast<void*>(&hk_Present1),
                                               reinterpret_cast<void**>(&o_Present1));
            const MH_STATUS s4 = MH_CreateHook(addr[3], reinterpret_cast<void*>(&hk_ExecuteCommandLists),
                                               reinterpret_cast<void**>(&o_ExecuteCommandLists));

            // THE STATUSES ARE CHECKED BEFORE ANYTHING IS ENABLED. MH_EnableHook used to
            // run first, so a partial install (Present created, ExecuteCommandLists not)
            // left LIVE trampolines behind while this function reported failure - and
            // `g_hooks_created` then stayed false, so the master switch's re-enable took
            // the "install from scratch" branch and ran the dummy-device discovery
            // again on top of hooks that were already in place. Either both required
            // hooks exist or nothing of ours is installed at all.
            const MH_STATUS created[kHookCount] = {s1, s2, s3, s4};
            const bool required_ok = s1 == MH_OK && s4 == MH_OK;
            MH_STATUS en = MH_ERROR_NOT_CREATED;
            if (required_ok)
            {
                en = MH_EnableHook(MH_ALL_HOOKS);
            }
            if (!required_ok || en != MH_OK)
            {
                for (int i = 0; i < kHookCount; ++i)
                {
                    if (created[i] == MH_OK)
                    {
                        MH_DisableHook(addr[i]);
                        MH_RemoveHook(addr[i]);
                    }
                }
                mm::log(L"hooks: the required pair (Present, ExecuteCommandLists) did not install, so "
                        L"every trampoline that HAD been created was removed again - nothing of this "
                        L"mod is in the game's call path");
            }

            g_hook_report = std::format(L"{} | Present {} @ {} | ResizeBuffers {} @ {} | Present1 {} @ {} | "
                                        L"ExecuteCommandLists {} @ {} | enable {}",
                                        how,
                                        static_cast<int>(s1),
                                        module_of(addr[0]),
                                        static_cast<int>(s2),
                                        module_of(addr[1]),
                                        static_cast<int>(s3),
                                        module_of(addr[2]),
                                        static_cast<int>(s4),
                                        module_of(addr[3]),
                                        static_cast<int>(en));
            mm::logf(L"hooks: {}", g_hook_report);
            mm::logf(L"hook addresses: Present {:p}  ResizeBuffers {:p}  Present1 {:p}  ExecuteCommandLists {:p}",
                     addr[0],
                     addr[1],
                     addr[2],
                     addr[3]);
            log_overlay_modules();
            const bool ok = required_ok && en == MH_OK;
            if (!ok)
            {
                mm::log(L"at least one required hook did not install - the overlay will not draw");
            }
            return ok;
        }

        bool install_hooks_from_cache()
        {
            void* addr[kHookCount]{};
            if (!read_hook_cache(addr))
            {
                return false;
            }
            // WHO WAS ALREADY THERE, read before we write a byte.
            for (int i = 0; i < kHookCount; ++i)
            {
                mm::logf(L"hook cache: {} -> {} {}", kHookNames[i], module_of(addr[i]),
                         detour_report(addr[i]));
            }
            mm::log(L"hook cache: the addresses came out of wuchang_minimap_hookaddr.txt, so NO dummy "
                    L"device, queue, swapchain or window was created this launch - the Steam overlay has "
                    L"nothing of ours to re-target onto");
            g_hooks_from_cache = true;
            return create_and_enable(addr, L"from the address cache");
        }

        bool install_hooks_by_discovery()
        {
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                mm::logf(L"MH_Initialize failed: {}",
                         std::wstring(MH_StatusToString(init), MH_StatusToString(init) + std::strlen(MH_StatusToString(init))));
                return false;
            }

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WuchangMinimapDummyWnd";
            ::RegisterClassExW(&wc);
            HWND dummy_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"WuchangMinimap", WS_OVERLAPPEDWINDOW, 0, 0, 64,
                                                64, nullptr, nullptr, wc.hInstance, nullptr);
            if (dummy_hwnd == nullptr)
            {
                mm::log(L"could not create the dummy window for vtable discovery");
                ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
                return false;
            }

            ID3D12Device* device = nullptr;
            ID3D12CommandQueue* queue = nullptr;
            IDXGIFactory2* factory = nullptr;
            IDXGISwapChain1* swapchain = nullptr;
            bool ok = false;

            HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            if (SUCCEEDED(hr))
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
            }
            if (SUCCEEDED(hr))
            {
                hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            }
            if (SUCCEEDED(hr))
            {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = 64;
                sd.Height = 64;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.Scaling = DXGI_SCALING_STRETCH;
                sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                hr = factory->CreateSwapChainForHwnd(queue, dummy_hwnd, &sd, nullptr, nullptr, &swapchain);
            }

            if (SUCCEEDED(hr) && swapchain != nullptr && queue != nullptr)
            {
                void** sc_vtable = *reinterpret_cast<void***>(swapchain);
                void** q_vtable = *reinterpret_cast<void***>(queue);

                void* addr[kHookCount] = {
                    sc_vtable[8],  // IDXGISwapChain::Present
                    sc_vtable[13], // IDXGISwapChain::ResizeBuffers
                    sc_vtable[22], // IDXGISwapChain1::Present1
                    q_vtable[10],  // ID3D12CommandQueue::ExecuteCommandLists
                };

                // WHO WAS ALREADY THERE. Read before we write a byte: a jmp in front of
                // Present names the module that installed it, and MinHook relocates
                // those bytes into our trampoline - which is the proof that the other
                // overlay stays in the chain below us instead of being replaced.
                ModuleId ids[kHookCount]{};
                bool all_identified = true;
                for (int i = 0; i < kHookCount; ++i)
                {
                    mm::logf(L"hook discovery: {} -> {} {}",
                             kHookNames[i],
                             module_of(addr[i]),
                             detour_report(addr[i]));
                    all_identified = module_id_of(addr[i], ids[i]) && all_identified;
                }

                ok = create_and_enable(addr, L"by dummy-swapchain discovery");

                // The addresses belong to the DLLs, so the next launch can hook them
                // without creating (and then destroying) a device, a queue and a
                // swapchain that Steam's overlay may have re-targeted itself onto.
                if (ok && all_identified && write_hook_cache(ids))
                {
                    mm::logf(L"hook cache: written to {} - the next launch hooks these addresses directly "
                             L"and creates no dummy objects at all",
                             hook_cache_path());
                }
            }
            else
            {
                mm::logf(L"dummy device/swapchain creation failed (0x{:08X}) - no hooks installed",
                         static_cast<unsigned>(hr));
            }

            safe_release(swapchain);
            safe_release(factory);
            safe_release(queue);
            safe_release(device);
            ::DestroyWindow(dummy_hwnd);
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

            g_hooks_installed = ok;
            g_hook_install_ms = ::GetTickCount64();
            return ok;
        }

        // THE ONE ENTRY POINT. The cache first (it creates nothing), the dummy-swapchain
        // discovery as the fallback that also refreshes the cache.
        bool install_hooks()
        {
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                mm::logf(L"MH_Initialize failed: {}", static_cast<int>(init));
                return false;
            }
            if (install_hooks_from_cache())
            {
                g_hooks_installed = true;
                g_hook_install_ms = ::GetTickCount64();
                return true;
            }
            g_hooks_from_cache = false;
            return install_hooks_by_discovery();
        }

        //==============================================================================
        // Selftest entry points (kept so the third-party objects cannot be dropped)
        //==============================================================================

        using Dx12InitFn = bool (*)(ImGui_ImplDX12_InitInfo*);
        using Win32InitFn = bool (*)(void*);

        const void* const g_entry_points[] = {
            reinterpret_cast<const void*>(static_cast<Dx12InitFn>(&ImGui_ImplDX12_Init)),
            reinterpret_cast<const void*>(static_cast<Win32InitFn>(&ImGui_ImplWin32_Init)),
            reinterpret_cast<const void*>(&MH_Initialize),
            reinterpret_cast<const void*>(&MH_CreateHook),
        };

        auto widen(std::string_view narrow) -> RC::StringType
        {
            return RC::StringType{narrow.begin(), narrow.end()};
        }
    } // namespace ovl

    using namespace ovl;

    auto selftest() -> RC::StringType
    {
        std::size_t linked = 0;
        for (const void* const entry : g_entry_points)
        {
            if (entry != nullptr)
            {
                ++linked;
            }
        }
        return std::format(STR("imgui {} ({}), minhook reachable ({}), {}/{} entry points linked"),
                           widen(IMGUI_VERSION),
                           IMGUI_VERSION_NUM,
                           widen(MH_StatusToString(MH_ERROR_NOT_INITIALIZED)),
                           linked,
                           std::size(g_entry_points));
    }

    void start()
    {
        mm::load_waypoint_file();
        mapdata::load(mm::mod_dir());

        // The render thread is allowed to build its objects again from the next frame.
        g_render_stopped.store(false, std::memory_order_release);

        const mm::Config cfg = mm::config();
        if (!cfg.overlay_enabled)
        {
            mm::log(L"overlay disabled by config (enabled = 0) - no hooks installed");
            mm::drain_log();
            return;
        }

        if (!g_hooks_created)
        {
            g_hooks_created = install_hooks();
            crumb::stage(g_hooks_created ? crumb::kHooksInstalled : "hook install FAILED");
        }
        else
        {
            // Re-enable after a master-switch disable. The trampolines were never
            // removed, so this can never install a second hook on the same address.
            const MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
            g_hooks_installed.store(en == MH_OK, std::memory_order_release);
            g_hook_install_ms = ::GetTickCount64();
            g_watchdog_reported = false;
            mm::logf(L"master switch: the existing DX12 hooks were re-enabled (MH_EnableHook = {})",
                     static_cast<int>(en));
        }

        if (cfg.debug_show_panel_on_start)
        {
            mm::g_panel_open = true;
            mm::log(L"debug_show_panel_on_start = 1: the F2 panel starts open (turn it off for normal play)");
        }
        mm::logf(L"hotkeys: {} settings panel, {} full map ({} recentres it), {} reload "
                 L"config + maps + markers, {} {} (pad {}) for the x-ray highlight [{}]; "
                 L"compass {}",
                 mm::key_name(cfg.panel_key),
                 mm::key_name(cfg.map_key),
                 mm::key_name(cfg.map_recenter_key),
                 mm::key_name(cfg.reload_key),
                 cfg.highlight_mode == mm::HighlightMode::Hold ? L"HOLD" : L"PRESS",
                 mm::key_name(cfg.highlight_key),
                 mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt, cfg.highlight_pad_rt),
                 cfg.highlight_enabled ? L"on" : L"off",
                 cfg.compass_enabled ? L"on" : L"off");
        mm::drain_log();
    }

    //======================================================================================
    // The master switch (loop thread)
    //======================================================================================

    void request_stop()
    {
        // mm::g_mod_active is already false, so the next Present takes the teardown
        // path. If no frame is coming - the game is minimised, or the hooks never fired
        // at all - stop_complete() below answers for it.
        if (!g_hooks_installed.load(std::memory_order_acquire) || g_present_count.load() == 0)
        {
            spin::SpinGuard guard(g_render_lock);
            shutdown_render(); // nothing of ours is in flight; safe from this thread
        }
    }

    bool stop_complete()
    {
        return g_render_stopped.load(std::memory_order_acquire);
    }

    void finish_stop()
    {
        if (!g_hooks_created)
        {
            return;
        }
        const MH_STATUS st = MH_DisableHook(MH_ALL_HOOKS);
        g_hooks_installed.store(false, std::memory_order_release);
        mm::logf(L"master switch: the DX12 hooks were disabled (MH_DisableHook = {}); the trampolines "
                 L"stay created so turning the mod back on cannot double-hook",
                 static_cast<int>(st));
    }


    //======================================================================================
    // THE STALL WATCHDOG (loop thread)
    //======================================================================================
    //
    // WHY. On 2026-09-03 at 20:56 the game hard-hung and had to be killed. Both logs
    // stop inside the same 200 ms, the crash breadcrumb still says "first slice" (a
    // hang, not a crash, so no dump), and there is NOTHING in the process that could
    // say which thread stopped or what it was doing - the whole diagnosis had to be done
    // by reading code. That must never cost a second session.
    //
    // WHAT IT IS. The UE4SS loop thread is the one thread that keeps running when the
    // game thread and the render thread wedge (it did in that incident, long enough to
    // flush a log buffer). So it watches two counters - `g_present_count` for the render
    // thread and `gamestate::pump_calls()` for the game thread - and when either has not
    // moved for `kStallMs` it says so, naming the stage each of them was last seen in.
    //
    // THE FLUSHER MUST NOT DEPEND ON THE STALLED THREAD, and it must not depend on the
    // HEAP either: the leading hypothesis for that freeze is a corrupted CRT heap, in
    // which case `std::format` would hang the last thread still running. So the first
    // thing this does is `crumb::watchdog()`, which is POD-only, allocation-free and
    // WRITE_THROUGH to its own file; only then does it try the ordinary log.
    //
    // FALSE POSITIVES. A synchronous level load blocks the game thread and stops Present
    // too, so the perf stall window (opened by `overlay::on_update` itself whenever
    // there is no validated gameplay pawn, and by ResizeBuffers) suppresses this. What
    // is left is a stall in gameplay, which is exactly the thing being hunted.
    void stall_watchdog(std::uint64_t now)
    {
        // Generous, because a stutter is not a freeze: a 6 s gap in gameplay is already
        // "the game has stopped responding" to a player.
        constexpr std::uint64_t kStallMs = 6000;
        constexpr std::uint64_t kRepeatMs = 5000;

        static std::uint64_t present_seen = 0;
        static std::uint64_t present_at = 0;
        static std::uint64_t pump_seen = 0;
        static std::uint64_t pump_at = 0;
        static std::uint64_t last_shout = 0;
        static bool shouting = false;

        const std::uint64_t presents = g_present_count.load(std::memory_order_relaxed);
        const std::uint64_t pumps = gamestate::pump_calls();
        if (present_at == 0)
        {
            present_at = now;
            pump_at = now;
            present_seen = presents;
            pump_seen = pumps;
            return;
        }
        if (presents != present_seen)
        {
            present_seen = presents;
            present_at = now;
        }
        if (pumps != pump_seen)
        {
            pump_seen = pumps;
            pump_at = now;
        }

        // Nothing to watch: the mod is off, the hooks are not in, no frame has ever
        // arrived (the no-Present watchdog above owns that case), or the game is
        // legitimately not producing frames or ticks.
        if (!mm::mod_active() || !g_hooks_installed.load(std::memory_order_acquire) || presents == 0 ||
            mm::perf_in_stall())
        {
            present_at = now;
            pump_at = now;
            return;
        }

        const std::uint64_t render_ms = now - present_at;
        const std::uint64_t game_ms = now - pump_at;
        if (render_ms < kStallMs && game_ms < kStallMs)
        {
            if (shouting)
            {
                shouting = false;
                mm::log(L"WATCHDOG: the stall is over - both threads are moving again");
            }
            return;
        }
        if (last_shout != 0 && now - last_shout < kRepeatMs)
        {
            return;
        }
        last_shout = now;
        shouting = true;

        const char* rstage = g_render_stage.load(std::memory_order_relaxed);
        const char* gstage = gamestate::pump_stage();
        char note[192]{};
        ::_snprintf_s(note, std::size(note), _TRUNCATE,
                      "rtid=%lu presents=%llu pumps=%llu pause=%d busy=%d panel=%d map=%d msgdrop=%llu",
                      g_render_tid.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(presents),
                      static_cast<unsigned long long>(pumps),
                      g_slicer_pause.load() ? 1 : 0,
                      g_slicer_busy.load() ? 1 : 0,
                      mm::g_panel_open.load() ? 1 : 0,
                      mm::g_map_open.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_msg_dropped.load(std::memory_order_relaxed)));
        // POD FIRST. If the heap is the thing that is wedged, everything below this line
        // never returns - and the line is already on disk.
        crumb::watchdog(static_cast<unsigned long>(render_ms), static_cast<unsigned long>(game_ms),
                        rstage, gstage, note);
        mm::modlog_flush();

        mm::logf(L"WATCHDOG: {} has not moved for {} ms (render {} ms at '{}', game {} ms at '{}'); "
                 L"{}. A line is also in wuchang_minimap_watchdog.txt, which is written without "
                 L"allocating in case the heap is what is stuck.",
                 render_ms >= kStallMs && game_ms >= kStallMs
                     ? L"NEITHER the render thread NOR the game thread"
                     : (render_ms >= kStallMs ? L"the RENDER thread (no Present)"
                                              : L"the GAME thread (no ProcessEvent pump)"),
                 (std::max)(render_ms, game_ms),
                 render_ms,
                 stage_w(rstage),
                 game_ms,
                 stage_w(gstage),
                 stage_w(note));
        mm::drain_log();
        mm::modlog_flush();
    }

    //==================================================================================
    // ONE DEBOUNCE PER BINDING
    //==================================================================================
    //
    // Until 1.0.1 the four toggles shared a single `last_key` timestamp, so a press of
    // the map key within 250 ms of the panel key was DROPPED - two unrelated actions
    // debouncing each other. The debounce exists to swallow a contact bounce and a key
    // repeat of the SAME key, which is a property of one binding, so it lives with the
    // binding: `Edge` is the level plus the last accepted time of exactly one hotkey.
    constexpr std::uint64_t kEdgeDebounceMs = 250;

    struct Edge
    {
        bool down = false;
        std::uint64_t last_ms = 0;
    };

    // True exactly once on the rising edge of `now_down`, and never twice inside
    // kEdgeDebounceMs. The level is recorded whatever the answer, so a key held down
    // through a gate closing cannot fire when the gate opens again.
    bool edge_fired(Edge& e, bool now_down, std::uint64_t now)
    {
        const bool fire = now_down && !e.down && (e.last_ms == 0 || now - e.last_ms > kEdgeDebounceMs);
        if (fire)
        {
            e.last_ms = now;
        }
        e.down = now_down;
        return fire;
    }

    void on_update()
    {
        // UE4SS EVENT-LOOP THREAD. No D3D12, no UObjects.
        static Edge panel_edge{};
        static Edge reload_edge{};
        static bool logged_first_present = false;

        const mm::Config& cfg = mm::cfg_cached();
        const std::uint64_t now = ::GetTickCount64();

        // THE HEIGHT SLICER. 1-4 ms of CPU that used to run inside Present twelve times
        // a second; it writes into a persistently mapped upload heap and needs nothing
        // from the frame. render() now only records the CopyTextureRegion.
        //
        // The guard sequence is the loop-thread half of the pause handshake: check,
        // mark busy, check AGAIN. The render thread sets `pause` and then waits for
        // `busy`, so whichever order the two interleave in, one of them backs off.
        if (mm::mod_active() && !g_slicer_pause.load())
        {
            g_slicer_busy.store(true);
            if (g_slicer_pause.load())
            {
                g_slicer_busy.store(false);
            }
            else
            {
                const std::uint32_t gen = g_slice_gen.load(std::memory_order_acquire);
                slice_minimap_step(now);
                slice_map_step(now);
                if (g_slice_gen.load(std::memory_order_acquire) != gen)
                {
                    // The buffer set was recreated while we were cutting. Whatever was
                    // just published describes buffers that no longer exist, so drop it;
                    // the next iteration cuts into the new ones.
                    clear_slice_view();
                    clear_map_slice_view();
                }
                g_slicer_busy.store(false);
            }
        }

        // THE HOTKEY BLOCK, gated to ~60 Hz. UE4SS spins this loop far faster than
        // that, and every iteration used to cost a GetForegroundWindow +
        // GetWindowThreadProcessId pair plus five GetAsyncKeyState calls. A key press
        // lasts tens of milliseconds, so nothing is missed - and the pad poll and the
        // hold key ride along with it, which is exactly the cadence they want.
        static std::uint64_t last_input_ms = 0;
        if (now - last_input_ms < 16)
        {
            return;
        }
        last_input_ms = now;
        // ONE COUNTER CANNOT ANSWER TWO QUESTIONS (lessons.md), and this row had to be
        // told twice. It reaches to the end of the function, so it started out timing the
        // hotkey samples together with the gamepad poll (32 ms peak, 2026-09-03) - which
        // got its own row - and then read a 366 ms PEAK against a ~0 ms average, which
        // was the other things sharing the scope: the map screenshot's clipboard hand-off
        // (a full-resolution DIB through GlobalAlloc + SetClipboardData, which takes a
        // window-station-wide lock), `mm::save_config_file()` (a ~28 KB rewrite) and the
        // F5 reload (`mapdata::load` re-decodes up to ~340 MB of PNG). All three now have
        // their own rows and all three declare a stall, so this row is the ~8
        // GetAsyncKeyState calls and nothing else. Every one of them is on the LOOP
        // thread, where a stall costs no frame and no game tick.
        if (g_pf_input < 0)
        {
            g_pf_input = mm::perf_register("loop input (hotkeys)", perf::Thread::Loop);
        }
        const mm::PerfScope input_scope(g_pf_input);

        // The foreground answer changes only when the player alt-tabs, so it is worth
        // 250 ms of cache: two user32 round-trips saved per sample.
        static std::uint64_t fg_checked_ms = 0;
        static bool fg_cached = false;
        if (fg_checked_ms == 0 || now - fg_checked_ms >= 250)
        {
            fg_checked_ms = now;
            const HWND fg = ::GetForegroundWindow();
            DWORD pid = 0;
            if (fg != nullptr)
            {
                ::GetWindowThreadProcessId(fg, &pid);
            }
            fg_cached = (pid == ::GetCurrentProcessId());
        }
        const bool foreground = fg_cached;

        // A LOADING SCREEN IS A STALL, and this is the cheapest honest place to notice
        // one: the snapshot is a seqlock read, it is already published for the render
        // thread, and "no validated gameplay pawn" is exactly the state the game is in
        // while it blocks its own thread loading a level. The window is generous (1.5 s)
        // because the frames on either side of a load are wall-clock waits too, and it
        // is refreshed on every 60 Hz pass for as long as the condition holds.
        {
            mm::Snapshot snap{};
            const bool have = mm::read_snapshot(snap);
            if (!have || !snap.has_pawn || !snap.pawn_is_gameplay || snap.transition ||
                snap.state_ok_since_ms == 0)
            {
                mm::perf_note_stall(L"a loading screen / no gameplay pawn", 1500);
            }
        }

        // THE BINDINGS, sampled as a LEVEL with their modifier (review B.13). A binding
        // carries its virtual key in the low byte and one modifier in bits 8..9
        // (mm::key_vk / mm::key_mod), so `map_key = ctrl+m` is one int and one sample.
        //
        // A binding with NO modifier does not require the modifiers to be up: the x-ray
        // hold key is Alt by default, and demanding a clean Alt would have made every
        // other hotkey dead for as long as the x-ray is held. The Bindings tab names
        // that overlap rather than the code inventing a rule about it.
        const auto mod_held = [](int mod) {
            switch (mod)
            {
            case mm::kKeyModCtrl:
                return (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            case mm::kKeyModShift:
                return (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            case mm::kKeyModAlt:
                return (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            default:
                return true;
            }
        };
        const auto key_down = [&mod_held](int binding) {
            const int vk = mm::key_vk(binding);
            if (vk == 0)
            {
                return false; // `none` - deliberately unbound
            }
            return (::GetAsyncKeyState(vk) & 0x8000) != 0 && mod_held(mm::key_mod(binding));
        };

        // WHAT THE WINDOW THREAD MAY SWALLOW, recomputed on every 60 Hz pass (review
        // B.13). A key is in the set only while the action it is bound to can actually
        // fire, so `C` is the game's again the moment the full map closes, and the
        // modifier has to be held for a modified binding - `ctrl+m` never costs the game
        // a bare `m`.
        //
        // Three keys are NEVER swallowed however they are bound: Alt+F4, Alt+Enter and
        // Alt+Tab are the player's way out of a game that is misbehaving, and a mod that
        // eats them is a mod nobody can quit. (Review B.4 is the same bug in the map's
        // blanket swallow.)
        {
            const bool map_open_now = mm::g_map_open.load(std::memory_order_relaxed);
            swallow_set_clear();
            // Sampled once, not once per binding: this block runs 60 times a second and
            // every GetAsyncKeyState is a syscall-ish read.
            const bool alt_now = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            const auto arm = [&](int binding, bool live) {
                const int vk = mm::key_vk(binding);
                if (!live || vk == 0 || !mod_held(mm::key_mod(binding)))
                {
                    return;
                }
                if (alt_now && (vk == VK_F4 || vk == VK_RETURN || vk == VK_TAB))
                {
                    return;
                }
                swallow_set_add(vk);
            };
            g_swallow_stamp.store(now, std::memory_order_relaxed);
            if (mm::mod_active() && foreground)
            {
                arm(cfg.panel_key, true);
                arm(cfg.reload_key, true);
                arm(cfg.map_key, true);
                arm(cfg.zoom_key, !map_open_now);
                arm(cfg.screenshot_key, map_open_now);
                arm(cfg.map_recenter_key, map_open_now);
                arm(cfg.highlight_key, cfg.highlight_enabled);
            }
        }

        if (edge_fired(panel_edge, key_down(cfg.panel_key), now) && foreground)
        {
            const bool open = !mm::g_panel_open.load();
            mm::g_panel_open = open;
            MM_LOGV(L"settings panel {}", open ? L"opened" : L"closed");
        }

        if (edge_fired(reload_edge, key_down(cfg.reload_key), now) && foreground)
        {
            mm::g_reload_config = true;
        }

        // The full map. GetAsyncKeyState rather than a WndProc test on purpose: while
        // the map is open the WndProc hook swallows every key, so the message-based
        // route could not close it again.
        static Edge map_edge{};
        if (edge_fired(map_edge, key_down(cfg.map_key), now) && foreground)
        {
            const bool open = !mm::g_map_open.load();
            mm::g_map_open = open;
            MM_LOGV(L"full map {}", open ? L"opened" : L"closed");
        }

        // THE FULL MAP ON A GAMEPAD (review B.1). Every pad control inside the map was
        // unreachable for a controller-only player, because nothing opened the map: the
        // key is a keyboard key and `want_pad` only polled XInput once the map was
        // already open. The chord is a press of ALL its buttons at once, taken from the
        // held mask rather than from the edge accumulator, so the order they go down in
        // does not matter; `map_pad_open_chord = none` disables it.
        static bool pad_chord_down = false;
        if (cfg.map_gamepad && cfg.map_pad_open_chord != 0)
        {
            const pad::State gp_open = pad::state();
            const bool chord_now = gp_open.connected &&
                                   (gp_open.held & cfg.map_pad_open_chord) == cfg.map_pad_open_chord;
            if (chord_now && !pad_chord_down)
            {
                const bool open = !mm::g_map_open.load();
                mm::g_map_open = open;
                mm::logf(L"full map {} (pad {})", open ? L"opened" : L"closed",
                         mm::pad_chord_name(cfg.map_pad_open_chord, false, false));
            }
            pad_chord_down = chord_now;
        }
        else
        {
            pad_chord_down = false;
        }

        // THE MINIMAP ZOOM LADDER. `zoom_key` is a press, the wheel gesture arrives as
        // accumulated steps from the render thread, and both are applied here - the loop
        // thread is the only one allowed to publish a config. Skipped while the full map
        // is open: it swallows the keyboard and owns its own zoom.
        static Edge zoom_edge{};
        if (edge_fired(zoom_edge, key_down(cfg.zoom_key), now) && foreground &&
            !mm::g_map_open.load())
        {
            g_zoom_steps.fetch_add(1, std::memory_order_relaxed);
        }
        if (const int steps = g_zoom_steps.exchange(0, std::memory_order_relaxed); steps != 0)
        {
            mm::Config edit = mm::config();
            const int n = (std::min)(edit.minimap_zoom_preset_count, mv::kMaxZoomPresets);
            const int dir = steps > 0 ? 1 : -1;
            for (int i = 0, taken = steps > 0 ? steps : -steps; i < taken && i < 64; ++i)
            {
                edit.zoom_uu_per_px =
                    mv::step_zoom_preset(edit.minimap_zoom_presets, n, edit.zoom_uu_per_px, dir);
            }
            if (edit.zoom_uu_per_px != cfg.zoom_uu_per_px)
            {
                mm::set_config(edit);
                // ON SCREEN, not only in the log (review B.9). The zoom key was the one
                // in-play gesture whose only feedback was a log line: the picture does
                // change, but at 13 -> 26 uu/px on a small disc that is not obviously
                // "I changed a setting" rather than "the map moved". One second is long
                // enough to read and short enough not to sit over the game.
                const int rung =
                    mv::zoom_preset_index(edit.minimap_zoom_presets, n, edit.zoom_uu_per_px);
                char note[64]{};
                if (rung >= 0)
                {
                    (void)std::snprintf(note, sizeof(note), "minimap zoom  %.0f uu/px  (%d of %d)",
                                        static_cast<double>(edit.zoom_uu_per_px), rung + 1, n);
                }
                else
                {
                    (void)std::snprintf(note, sizeof(note), "minimap zoom  %.0f uu/px",
                                        static_cast<double>(edit.zoom_uu_per_px));
                }
                post_toast(note, 1000);
                mm::logf(L"minimap zoom: {:.0f} uu/px (cycled with {} over {} preset(s))",
                         edit.zoom_uu_per_px, mm::key_name(edit.zoom_key), n);
            }
            else if (n <= 0)
            {
                post_toast("minimap_zoom_presets is empty - nothing to cycle", 1500);
                mm::log(L"minimap zoom: minimap_zoom_presets is empty - nothing to cycle through");
            }
        }

        // THE FIRST-RUN TIP. Once per install: a 10-second toast naming the keys that
        // are actually bound, because "the mod does nothing" is almost always "I did not
        // know which key opens it". The sentinel is a file next to the config, so
        // reinstalling into a clean folder shows it again and a config reload does not.
        //
        // IT WAITS FOR THE HUD (review B.2). It used to fire on the first pass of this
        // function - at process start, over the splash screen and the main menu, where
        // nothing of ours draws - and it wrote the sentinel there too, so the one tip a
        // player ever gets was spent on a screen that never showed it. The gate is the
        // render thread's own "the HUD may be on screen" answer, published the first
        // time it opens, i.e. the first frame with a validated gameplay pawn.
        static bool first_run_checked = false;
        if (!first_run_checked && cfg.first_run_toast && g_hud_gate_ever_open.load(std::memory_order_acquire))
        {
            first_run_checked = true;
            const std::wstring sentinel = mm::mod_dir() + L"\\wuchang_minimap_firstrun.txt";
            if (::GetFileAttributesW(sentinel.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                const std::string text =
                    std::format("{} settings   {} map   hold {} to see items through walls",
                                key_name_ascii(cfg.panel_key), key_name_ascii(cfg.map_key),
                                key_name_ascii(cfg.highlight_key));
                post_toast(text.c_str(), 10000);
                const HANDLE h = ::CreateFileW(sentinel.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    const char* note =
                        "This file only records that the first-run tip has been shown.\r\n"
                        "Delete it to see the tip again on the next launch.\r\n";
                    DWORD written = 0;
                    ::WriteFile(h, note, static_cast<DWORD>(std::strlen(note)), &written, nullptr);
                    ::CloseHandle(h);
                }
                mm::logf(L"first run: showing the key tip - {}",
                         std::wstring(text.begin(), text.end()));
            }
        }

        // MAP -> CLIPBOARD. Only while the full map is open, which is also what makes a
        // plain letter safe as the default: the map mode swallows every keyboard message
        // (lessons.md), so `C` cannot reach the game while this can fire.
        static Edge shot_edge{};
        if (edge_fired(shot_edge, key_down(cfg.screenshot_key), now) && foreground &&
            mm::g_map_open.load())
        {
            g_shot_request.store(true, std::memory_order_release);
        }

        // The finished bitmap, handed over by the render thread. The clipboard API opens
        // a window-station-wide lock and can block; it belongs here and nowhere near
        // Present.
        if (g_shot_dib_ready.exchange(false, std::memory_order_acquire))
        {
            if (g_pf_clip < 0)
            {
                g_pf_clip = mm::perf_register("map -> clipboard", perf::Thread::Loop);
            }
            mm::perf_note_stall(L"the map -> clipboard hand-off", 500);
            const mm::PerfScope clip_scope(g_pf_clip);
            std::vector<std::uint8_t> dib;
            {
                spin::SpinGuard guard(g_shot_lock);
                dib.swap(g_shot_dib);
            }
            const char* why = nullptr;
            if (dib.size() <= clipimg::kHeaderSize)
            {
                why = "map copy failed: empty bitmap";
            }
            else if (::OpenClipboard(nullptr) == 0)
            {
                why = "map copy failed: clipboard is busy";
            }
            else
            {
                // GMEM_MOVEABLE is required: the clipboard takes OWNERSHIP of the handle
                // on success, so it must not be freed afterwards - and must be freed by
                // us on failure, which is the only branch that calls GlobalFree.
                HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, dib.size());
                void* dst = mem != nullptr ? ::GlobalLock(mem) : nullptr;
                if (dst != nullptr)
                {
                    std::memcpy(dst, dib.data(), dib.size());
                    ::GlobalUnlock(mem);
                    ::EmptyClipboard();
                    if (::SetClipboardData(CF_DIB, mem) == nullptr)
                    {
                        ::GlobalFree(mem);
                        why = "map copy failed: SetClipboardData";
                    }
                }
                else
                {
                    if (mem != nullptr)
                    {
                        ::GlobalFree(mem);
                    }
                    why = "map copy failed: out of memory";
                }
                ::CloseClipboard();
            }
            post_toast(why != nullptr ? why : "map copied to clipboard", 2500);
            g_shot_stage_done.store(true, std::memory_order_release);
            mm::logf(L"screenshot: {} ({} bytes)",
                     why != nullptr ? std::wstring(why, why + std::strlen(why))
                                    : std::wstring{L"copied to the clipboard"},
                     dib.size());
        }

        static Edge recenter_edge{};
        if (edge_fired(recenter_edge, key_down(cfg.map_recenter_key), now) && foreground &&
            mm::g_map_open.load())
        {
            g_map_recenter.store(true, std::memory_order_relaxed);
        }

        // XInput, on THIS thread - the same place the keyboard is sampled, and never on
        // the game thread (lessons.md).
        //
        // WHY THIS IS NOT GATED ON THE MAP BEING OPEN ANY MORE (review B.1). It was
        // `map_gamepad && map_open`, and that is a deadlock in the shape of a condition:
        // the only thing that could open the map was a keyboard key, so a controller-only
        // player could never reach any of the pad controls inside it, and the Debug tab's
        // "gamepad connected" line said `false` for ever because nothing had ever asked.
        //
        // The cost that gate existed to avoid is polling an EMPTY slot, and gamepad.cpp
        // already handles that itself: with no pad found it probes the four slots once a
        // second and returns, and once a slot answers it follows that one at whatever
        // rate it is called (a connected-slot XInputGetState is a handful of
        // microseconds). So "poll whenever the feature is switched on" is a ~1 Hz probe
        // when nothing is plugged in and full rate as soon as something is - which is
        // exactly what the map, the open chord and the x-ray chord all need.
        const bool want_pad = cfg.map_gamepad ||
                              (cfg.highlight_enabled && cfg.highlight_gamepad &&
                               (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt));
        // Its own row: `XInputGetState` on an empty slot costs about a millisecond, and
        // the first call also pays a `LoadLibraryW("xinput1_4.dll")`. The enumeration is
        // already behind a 1 Hz probe with the found slot pinned, so this should read
        // ~0.00 ms average with a one-off peak - and if it does not, the number says so
        // instead of hiding inside the block above.
        if (g_pf_pad < 0)
        {
            g_pf_pad = mm::perf_register("gamepad poll", perf::Thread::Loop);
        }
        {
            const mm::PerfScope pad_scope(g_pf_pad);
            pad::poll(want_pad, cfg.map_gamepad_deadzone);
        }

        // THE X-RAY HIGHLIGHT'S KEY. The key and the pad chord are always sampled as a
        // LEVEL (`down` below); what `highlight_mode` decides is what that level means.
        //
        //   hold   - the original: on while down. No debounce, no "turn it off" path.
        //   toggle - the default the user asked for: the RISING EDGE of the level flips
        //            hl's latch. The latch is the one piece of latched input state in
        //            the mod, so it follows the rule that goes with that (lessons.md):
        //            it is cleared from live state by hl::drop_caches() - every level
        //            transition and every dropped pawn - and by turning the feature off.
        //
        // Either way the DEMAND handed to hl needs the window in the foreground, or
        // alt-tabbing would leave the game thread reading the camera for nothing.
        bool down = cfg.highlight_enabled && key_down(cfg.highlight_key);
        if (!down && cfg.highlight_enabled && cfg.highlight_gamepad)
        {
            const pad::State gp = pad::state();
            const bool chord = (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt) &&
                               (gp.held & cfg.highlight_pad_mask) == cfg.highlight_pad_mask &&
                               (!cfg.highlight_pad_lt || gp.lt > 0.5f) && (!cfg.highlight_pad_rt || gp.rt > 0.5f);
            down = gp.connected && chord;
        }
        static bool xray_down = false;
        bool held = false;
        if (!cfg.highlight_enabled)
        {
            hl::xray_latch_clear(L"the highlight was turned off");
        }
        else if (cfg.highlight_mode == mm::HighlightMode::Hold)
        {
            // Leaving hold mode armed would strand the latch on; clearing it here is
            // also what makes switching the mode in the panel take effect at once.
            hl::xray_latch_clear(L"switched to hold mode");
            held = down;
        }
        else
        {
            if (down && !xray_down && foreground)
            {
                hl::xray_latch_flip();
            }
            held = hl::xray_latched();
        }
        xray_down = down;
        held = held && foreground;
        // This is what makes the game thread read the camera at all: with neither the
        // highlight held nor the compass on, highlight.cpp costs one atomic load a pump.
        hl::set_demand(held, cfg.overlay_enabled && cfg.compass_enabled);

        // THE FILE WRITES. All of them are on this thread and none of them is anywhere
        // near Present: the render thread only ever raises a flag (a waypoint drag, the
        // panel's Save button, the screenshot request) and this is where the flag turns
        // into a write. They share one row, because "the mod wrote a file" is one
        // question, and they declare a stall, because a ~28 KB rewrite through
        // CreateFile can block on a virus scanner for as long as it likes.
        const bool wp_dirty = mm::g_waypoint_dirty.exchange(false);
        const bool cfg_dirty = mm::g_save_config.load();
        if (wp_dirty || cfg_dirty)
        {
            if (g_pf_save < 0)
            {
                g_pf_save = mm::perf_register("config / waypoint save", perf::Thread::Loop);
            }
            mm::perf_note_stall(L"a config / waypoint file write", 500);
        }
        if (wp_dirty)
        {
            if (cfg.map_waypoint_persist)
            {
                const mm::PerfScope save_scope(g_pf_save);
                mm::save_waypoint_file();
            }
        }

        panel_state_load();
        if (g_panel_state_dirty.exchange(false, std::memory_order_acquire))
        {
            // Tiny (one line), and on the same thread as every other write this mod
            // does. It shares the file-write perf row above by design.
            panel_state_save();
        }

        if (mm::g_reload_config.exchange(false))
        {
            if (g_pf_reload < 0)
            {
                g_pf_reload = mm::perf_register("reload (config+maps+markers)", perf::Thread::Loop);
            }
            // Hundreds of milliseconds by design: `mapdata::load` re-decodes the
            // chapter's height PNGs. A one-off, and it must not set the peak every
            // later sample of every other row is judged against.
            mm::perf_note_stall(L"an F5 reload", 4000);
            const mm::PerfScope reload_scope(g_pf_reload);
            mm::log(L"reloading config + maps + markers");
            mm::load_config_file();
            mm::load_waypoint_file();
            g_drop_textures.store(true, std::memory_order_release);
            mapdata::load(mm::mod_dir());
            markers::reload();
        }
        if (mm::g_save_config.exchange(false))
        {
            const mm::PerfScope save_scope(g_pf_save);
            mm::save_config_file();
        }
        // REVERT: re-read the config files and publish them, throwing away every
        // unsaved edit made in the panel. Deliberately not a maps / markers reload -
        // "undo what I just fiddled with" should not cost a 340 MB asset swap.
        if (mm::g_revert_config.exchange(false))
        {
            mm::log(L"config: reverting to what is on disk");
            mm::load_config_file();
        }

        if (!logged_first_present && g_present_count.load() > 0)
        {
            logged_first_present = true;
            mm::logf(L"first Present seen; the hook is live (count {})", g_present_count.load());
        }
        // The panel renders every frame, so this is throttled hard: once when it first
        // draws, then at most one line every 10 s. (Without the throttle a single
        // main-menu verification run wrote 2 100 identical lines.)
        static std::uint64_t last_panel_log = 0;
        if (mm::g_panel_drew_frame.exchange(false) && (last_panel_log == 0 || now - last_panel_log > 10000))
        {
            last_panel_log = now;
            mm::log(L"the settings panel is rendering");
        }
        if (!g_watchdog_reported.load() && g_hooks_installed.load() && g_hook_install_ms != 0 &&
            now - g_hook_install_ms > 8000 && g_present_count.load() == 0)
        {
            g_watchdog_reported = true;
            if (g_hooks_from_cache)
            {
                delete_hook_cache(L"8 s with the cached addresses and no Present at all");
            }
            mm::log(L"WATCHDOG: 8 s after installing the hooks not a single Present has arrived. The game's "
                    L"swapchain is behind a proxy we did not create ours through (a DLSS frame-generation wrapper "
                    L"is the likely candidate). Loaded graphics modules follow:");
            static const wchar_t* const suspects[] = {L"dxgi.dll",       L"d3d12.dll",  L"ReShade64.dll",
                                                      L"nvngx_dlssg.dll", L"sl.interposer.dll", L"sl.dlss_g.dll",
                                                      L"amd_fidelityfx_dx12.dll"};
            for (const wchar_t* name : suspects)
            {
                const HMODULE mod = ::GetModuleHandleW(name);
                if (mod != nullptr)
                {
                    wchar_t path[MAX_PATH * 2]{};
                    ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
                    mm::logf(L"  loaded: {} -> {}", name, path);
                }
            }
        }
        // NAMES THE STALLED THREAD, so the next freeze does not have to be diagnosed by
        // reading code (see the comment on stall_watchdog).
        stall_watchdog(now);

        mm::drain_log();
    }
} // namespace overlay
