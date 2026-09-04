//
// overlay - the DX12 + Dear ImGui side of WuchangMinimap.
//
// The module is one namespace, `overlay::ovl`, spread over the translation units below;
// everything they share is declared in overlay_internal.hpp.
//
//   overlay.cpp         the shared state, the UI scale, the font, the HUD placement,
//                       the module identification helpers, and the loop-thread side:
//                       start / stop, the stall watchdog and the hotkey debounce
//   overlay_d3d12.cpp   the device objects, the swapchain hooks, the render entry point
//   overlay_input.cpp   the WndProc hook and the message replay
//   overlay_slice.cpp   the map texture upload and the two height slicers
//   overlay_hud.cpp     the minimap, its markers, the toasts, the x-ray, the compass
//   overlay_extras.cpp  the shrine list and the collection statistics page
//   overlay_fullmap.cpp the pannable, zoomable full map
//   overlay_panel.cpp   the F2 settings panel
//
// THREADING
// ---------
// Present runs on the RHI / present thread, never the game thread. Nothing in these files
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
