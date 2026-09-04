#pragma once

//
// mapview - the pure half of the full map: the viewport transform and its exact
// inverse, the zoom clamp / zoom step, and the waypoint file round-trip. Plain C++ over
// doubles and strings, with no dependency on Windows, UE4SS, Dear ImGui or mm::log, so
// tests/markers_test.cpp links it into a console exe. Anything needing D3D12 or ImGui
// lives in overlay.cpp.
//
// The full map is always north-up, so the mapping is the minimap's at yaw 0, which is
// also build_map.py / render.py's convention:
//
//     screen right  ->  world +Y  (east)
//     screen up     ->  world +X  (north)
//
// With the view centred on (cx, cy) at `uu_per_px` world units per screen pixel and
// the map occupying the screen rectangle `r`:
//
//     sx = r.cx() + (wy - cy) / uu_per_px
//     sy = r.cy() - (wx - cx) / uu_per_px
//
// and the inverse, exactly:
//
//     wx = cx - (sy - r.cy()) * uu_per_px
//     wy = cy + (sx - r.cx()) * uu_per_px
//

#include <string>
#include <string_view>

namespace mv
{
    struct Rect
    {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;

        float cx() const
        {
            return (x0 + x1) * 0.5f;
        }
        float cy() const
        {
            return (y0 + y1) * 0.5f;
        }
        float w() const
        {
            return x1 - x0;
        }
        float h() const
        {
            return y1 - y0;
        }
        bool contains(float x, float y) const
        {
            return x >= x0 && x <= x1 && y >= y0 && y <= y1;
        }
    };

    struct View
    {
        double cx = 0.0;          // world X at the centre of the rectangle
        double cy = 0.0;          // world Y at the centre of the rectangle
        double uu_per_px = 55.0;  // zoom: world units per screen pixel
    };

    void world_to_screen(const View& v, const Rect& r, double wx, double wy, float& sx, float& sy);
    void screen_to_world(const View& v, const Rect& r, float sx, float sy, double& wx, double& wy);

    // Zoom limits. `lo` / `hi` are taken in either order; a non-finite or non-positive
    // input falls back to the low limit.
    double clamp_zoom(double z, double lo, double hi);

    // One or more wheel notches. Positive notches zoom in, reducing uu_per_px.
    // `factor` is the multiplier per notch; values <= 1 are ignored.
    double zoom_by(double z, double notches, double factor, double lo, double hi);

    // The zoom at which a world rectangle exactly fills a canvas. North-up, so world X
    // (north-south) is spent on the canvas height and world Y (east-west) on its width.
    // Returns 0 when a span or a canvas side is unusable. `margin` (0.02 = 2 %) keeps
    // the fitted rectangle inside the canvas.
    double fit_zoom(double span_x_uu, double span_y_uu, double canvas_w_px, double canvas_h_px,
                    double margin = 0.02);

    //==================================================================================
    // Minimap zoom presets - `minimap_zoom_presets` is the ladder, `zoom_key` steps it
    //==================================================================================

    constexpr int kMaxZoomPresets = 8;

    // "13, 26, 52" -> `out`, ascending, duplicates and out-of-range values dropped.
    // Returns how many presets were stored; 0 leaves `out` untouched. Malformed tokens
    // are appended to `rejected` (comma separated) and consume no slot.
    int parse_zoom_presets(std::string_view text, float out[kMaxZoomPresets], std::string* rejected = nullptr);

    // One rung along the ladder: `dir > 0` up, `dir < 0` down, both wrapping round.
    // `presets` must be ascending. Returns `current` when there is nothing to cycle.
    // "Above" and "below" carry a 0.1 % margin, so a zoom sitting exactly on a rung
    // moves off it.
    float step_zoom_preset(const float* presets, int count, float current, int dir);

    // step_zoom_preset(..., +1): what `zoom_key` does.
    float next_zoom_preset(const float* presets, int count, float current);

    // Which rung the current zoom stands on, 0-based, or -1 for none. Same 0.1 % margin
    // as step_zoom_preset.
    int zoom_preset_index(const float* presets, int count, float current);

    //==================================================================================
    // The waypoint - wuchang_minimap_waypoint.txt
    //==================================================================================
    //
    // One waypoint at a time, next to the config file as three `key = value` lines. Kept
    // out of config_wuchang_minimap.txt, which the F2 panel's Save button rewrites
    // wholesale; a waypoint set during play survives without a Save.

    struct Waypoint
    {
        bool set = false;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    std::string waypoint_serialize(const Waypoint& wp);

    // False (and `out` untouched) when the text carries no usable waypoint. A BOM, CRLF,
    // comments (`;` / `#`) and blank lines are tolerated.
    bool waypoint_parse(std::string_view text, Waypoint& out);
} // namespace mv
