#include "compass.hpp"

#include <cmath>

namespace cmp
{
    double wrap180(double deg)
    {
        if (!std::isfinite(deg))
        {
            return 0.0;
        }
        // The half-open interval is (-180, 180]: due south must read as +180, not -180,
        // so that a strip whose span is a full 360 degrees does not fold its right edge
        // onto its left one.
        double v = std::fmod(deg + 180.0, 360.0);
        if (v <= 0.0)
        {
            v += 360.0;
        }
        return v - 180.0;
    }

    double wrap360(double deg)
    {
        if (!std::isfinite(deg))
        {
            return 0.0;
        }
        double v = std::fmod(deg, 360.0);
        if (v < 0.0)
        {
            v += 360.0;
        }
        return v;
    }

    double bearing_deg(double from_x, double from_y, double to_x, double to_y)
    {
        const double dx = to_x - from_x;
        const double dy = to_y - from_y;
        if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0 && dy == 0.0))
        {
            return 0.0;
        }
        return wrap360(std::atan2(dy, dx) * 180.0 / kPi);
    }

    bool strip_x(const Strip& s, double bearing_deg_value, double& x_out, double& rel_out)
    {
        const double span = (s.span > 1.0 && s.span <= 360.0) ? s.span : 120.0;
        const double rel = wrap180(bearing_deg_value - s.heading);
        rel_out = rel;
        const double half = span * 0.5;
        const double centre = s.x0 + s.width * 0.5;
        if (rel < -half || rel > half)
        {
            // Clamped to the edge on the side it is actually on, so an arrow drawn
            // there points the way the player has to turn.
            x_out = rel < 0.0 ? s.x0 : s.x0 + s.width;
            return false;
        }
        x_out = centre + (rel / span) * s.width;
        return true;
    }

    const char* cardinal_label(double bearing)
    {
        const double b = wrap360(bearing);
        const int idx = static_cast<int>(std::lround(b / 45.0)) % 8;
        // Only a true multiple of 45 gets a label; anything else is a minor tick.
        if (std::fabs(wrap180(b - static_cast<double>(idx) * 45.0)) > 0.001)
        {
            return "";
        }
        static const char* const kNames[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        return kNames[idx];
    }

    int ticks(const Strip& s, Tick* out, int cap, double step_deg)
    {
        if (out == nullptr || cap <= 0)
        {
            return 0;
        }
        const double step = (step_deg >= 1.0 && step_deg <= 90.0) ? step_deg : 15.0;
        const double span = (s.span > 1.0 && s.span <= 360.0) ? s.span : 120.0;
        const double half = span * 0.5;

        // Walk the ticks around the heading rather than 0..360: the strip never shows
        // more than `span` degrees, so this is span/step iterations regardless of where
        // the player is looking (and it cannot loop forever on a silly heading).
        const double first = std::ceil((s.heading - half) / step) * step;
        int n = 0;
        for (int i = 0; i < 4096 && n < cap; ++i)
        {
            const double bearing = first + static_cast<double>(i) * step;
            if (bearing > s.heading + half + 1e-9)
            {
                break;
            }
            Tick t{};
            t.bearing = wrap360(bearing);
            // The walk itself already produced a relative angle inside the strip, so use
            // it directly. Going back through strip_x() would re-wrap it, and at a full
            // 360-degree span that maps both ends onto the same edge.
            t.rel = bearing - s.heading;
            t.x = s.x0 + s.width * 0.5 + (t.rel / span) * s.width;
            const double from45 = std::fabs(wrap180(t.bearing - static_cast<double>(std::lround(t.bearing / 45.0)) * 45.0));
            const bool on45 = from45 <= 0.001;
            const double from90 = std::fabs(wrap180(t.bearing - static_cast<double>(std::lround(t.bearing / 90.0)) * 90.0));
            const bool on90 = from90 <= 0.001;
            t.rank = on90 ? 2 : (on45 ? 1 : 0);
            t.label = t.rank > 0 ? cardinal_label(t.bearing) : "";
            out[n++] = t;
        }
        return n;
    }
} // namespace cmp
