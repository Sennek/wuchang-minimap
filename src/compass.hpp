#pragma once

//
// compass - the pure arithmetic behind the heading strip at the top of the screen.
//
// Same split as markers_db / mapview / scan_sched: everything that does not need the
// engine, D3D12 or ImGui lives here and is covered by tests/markers_test.cpp, so a
// wrong wrap or a mirrored bearing is caught on the build machine instead of in a play
// session. The drawing itself (ImDrawList calls, colours, glyphs) stays in overlay.cpp.
//
// HEADINGS. Unreal yaw is degrees about +Z, 0 = +X. This mod already renders maps with
// +X as "up on the image" (north) and +Y as east, so:
//
//     yaw    0 = N        90 = E       180 = S       270 = W
//
// and the bearing from the player to a marker is atan2(dy, dx) in the same units,
// which is why bearing_deg() takes (dx = north, dy = east) in that order.
//

namespace cmp
{
    inline constexpr double kPi = 3.14159265358979323846;

    // Fold any angle into (-180, 180].
    double wrap180(double deg);

    // Fold into [0, 360).
    double wrap360(double deg);

    // Compass bearing from (from_x, from_y) to (to_x, to_y) in world uu, in the same
    // degrees as the player's yaw. Returns 0 for a zero-length delta.
    double bearing_deg(double from_x, double from_y, double to_x, double to_y);

    // The strip: `width` pixels showing `span_deg` degrees centred on `heading_deg`.
    struct Strip
    {
        double x0 = 0.0;      // left edge, screen px
        double width = 0.0;   // px
        double heading = 0.0; // degrees, what sits in the middle
        double span = 120.0;  // degrees across the whole strip
    };

    // Where a bearing lands on the strip.
    //   returns false  - outside the strip's span (x is then the clamped edge position,
    //                    which is what an "off to the left/right" arrow uses)
    //   rel            - signed degrees from the centre, negative = left
    bool strip_x(const Strip& s, double bearing_deg_value, double& x_out, double& rel_out);

    // The cardinal / intercardinal ticks. `step_deg` is the spacing of MINOR ticks
    // (15 by default); a tick whose bearing is a multiple of 45 is major and carries a
    // label, and a multiple of 90 gets the cardinal letter.
    struct Tick
    {
        double bearing = 0.0; // 0..360
        double x = 0.0;       // screen px
        double rel = 0.0;     // signed degrees from the centre
        int rank = 0;         // 0 = minor, 1 = intercardinal (45), 2 = cardinal (90)
        const char* label = ""; // "N", "NE", ... or "" for a minor tick
    };

    // Fills `out` with every tick inside the strip, left to right. Returns how many.
    int ticks(const Strip& s, Tick* out, int cap, double step_deg = 15.0);

    // The cardinal label for a bearing that is a multiple of 45 ("" otherwise).
    const char* cardinal_label(double bearing);
} // namespace cmp
