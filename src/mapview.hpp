#pragma once

//
// mapview - the PURE half of the full map (step C1).
//
// Everything in here is plain C++ over doubles and strings: the full map's viewport
// transform and its exact inverse, the zoom clamp / zoom step, and the waypoint file
// round-trip. Like markers_db.hpp it has NO dependency on Windows, UE4SS, Dear ImGui
// or mm::log, which is what lets tests/markers_test.cpp link it into a console exe and
// check the world<->screen inverse, the zoom limits and the waypoint file without the
// game running. Everything that needs D3D12 or ImGui lives in overlay.cpp.
//
// THE TRANSFORM
// -------------
// The full map is always NORTH-UP - it never rotates with the player - so the mapping
// is the minimap's own mapping at yaw 0, which is also build_map.py / render.py's
// convention:
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
// The two are tested against each other over the whole viewport in markers_test.
//

#include <string>
#include <string_view>

namespace mv
{
    //==================================================================================
    // Screen rectangle
    //==================================================================================

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

    //==================================================================================
    // The view
    //==================================================================================

    struct View
    {
        double cx = 0.0;          // world X at the centre of the rectangle
        double cy = 0.0;          // world Y at the centre of the rectangle
        double uu_per_px = 55.0;  // zoom: world units per screen pixel
    };

    void world_to_screen(const View& v, const Rect& r, double wx, double wy, float& sx, float& sy);
    void screen_to_world(const View& v, const Rect& r, float sx, float sy, double& wx, double& wy);

    // Zoom limits. `lo` / `hi` are taken in either order, and a non-finite or
    // non-positive input falls back to the low limit - a hand-edited config must never
    // be able to produce a divide-by-zero zoom.
    double clamp_zoom(double z, double lo, double hi);

    // One or more wheel notches. POSITIVE notches zoom IN, i.e. reduce uu_per_px, which
    // is what a mouse wheel scrolled away from the user means everywhere else.
    // `factor` is the multiplier per notch (1.15 ships); values <= 1 are ignored.
    double zoom_by(double z, double notches, double factor, double lo, double hi);

    //==================================================================================
    // Minimap zoom presets
    //==================================================================================
    //
    // Until 0.9.3 the minimap's zoom could only be changed by opening the F2 panel or
    // by editing a file, which is the one setting a player wants to change while
    // walking. `minimap_zoom_presets` is the ladder and `zoom_key` steps through it.
    //
    // Both halves are here because both are pure: parsing a hand-edited list, and
    // deciding which rung comes next.

    constexpr int kMaxZoomPresets = 8;

    // "13, 26, 52" -> `out`, ascending, duplicates and out-of-range values dropped.
    // Returns how many presets were stored (0 when the text carries none usable, in
    // which case `out` is untouched and the caller keeps whatever it had). Malformed
    // tokens are appended to `rejected` (comma separated) for the loader to log; unlike
    // a colour list a bad token here does NOT consume a slot, because a zoom ladder is
    // a set, not a tier-indexed table.
    int parse_zoom_presets(std::string_view text, float out[kMaxZoomPresets], std::string* rejected = nullptr);

    // One rung along the ladder: `dir > 0` the next rung above `current`, `dir < 0` the
    // next one below, both wrapping round. `presets` must be ascending
    // (parse_zoom_presets sorts it). Returns `current` unchanged when there is nothing
    // to cycle through, so no caller has to special-case an empty ladder.
    //
    // "Above" and "below" are deliberately fuzzy (a 0.1 % margin): the current zoom
    // usually IS one of the presets, and an exact comparison would then be answered by
    // that same rung - the key would appear to do nothing.
    float step_zoom_preset(const float* presets, int count, float current, int dir);

    // step_zoom_preset(..., +1): what `zoom_key` does.
    float next_zoom_preset(const float* presets, int count, float current);

    //==================================================================================
    // The waypoint - wuchang_minimap_waypoint.txt
    //==================================================================================
    //
    // One waypoint at a time, stored next to the config file as three `key = value`
    // lines so it stays hand-editable and diffable like every other file this mod
    // writes. It is deliberately NOT part of config_wuchang_minimap.txt: the config
    // file is rewritten wholesale by the F2 panel's Save button, and a waypoint set
    // during play must survive without anybody pressing Save.

    struct Waypoint
    {
        bool set = false;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    std::string waypoint_serialize(const Waypoint& wp);

    // Returns false (and leaves `out` untouched) when the text carries no usable
    // waypoint. A BOM, CRLF, comments (`;` / `#`) and blank lines are all tolerated,
    // exactly like the config and found-tracker readers.
    bool waypoint_parse(std::string_view text, Waypoint& out);
} // namespace mv
