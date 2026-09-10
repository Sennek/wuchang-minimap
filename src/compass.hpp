#pragma once

//
// compass - the pure arithmetic behind the heading strip. Drawing stays in overlay.cpp.
//
// Unreal yaw is degrees about +Z with 0 = +X, north-up (src/mapview.hpp), so yaw 0 = N,
// 90 = E, 180 = S, 270 = W. A bearing is atan2(dy, dx) in the same units, hence
// bearing_deg()'s (x = north, y = east) argument order.
//

namespace cmp
{
    inline constexpr double kPi = 3.14159265358979323846;

    // Fold any angle into (-180, 180].
    double wrap180(double deg);

    // Fold into [0, 360).
    double wrap360(double deg);

    // Bearing from (from_x, from_y) to (to_x, to_y) in world uu, in yaw degrees.
    // 0 for a zero-length delta.
    double bearing_deg(double from_x, double from_y, double to_x, double to_y);

    // The strip: `width` pixels showing `span_deg` degrees centred on `heading_deg`.
    struct Strip
    {
        double x0 = 0.0;      // left edge, screen px
        double width = 0.0;   // px
        double heading = 0.0; // degrees, what sits in the middle
        double span = 120.0;  // degrees across the whole strip
    };

    // Where a bearing lands on the strip. False when it is outside the span, and `x` is
    // then the clamped edge position. `rel` is signed degrees from the centre.
    bool strip_x(const Strip& s, double bearing_deg_value, double& x_out, double& rel_out);

    // `step_deg` is the minor-tick spacing; multiples of 45 are major and labelled,
    // multiples of 90 carry the cardinal letter.
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
