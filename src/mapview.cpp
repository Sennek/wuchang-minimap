#include "mapview.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mv
{
    namespace
    {
        bool finite(double v)
        {
            return v == v && v > -1e300 && v < 1e300;
        }

        std::string trim(std::string_view v)
        {
            std::size_t a = 0;
            std::size_t b = v.size();
            const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (a < b && space(v[a]))
            {
                ++a;
            }
            while (b > a && space(v[b - 1]))
            {
                --b;
            }
            return std::string{v.substr(a, b - a)};
        }

        bool parse_double(const std::string& text, double& out)
        {
            if (text.empty())
            {
                return false;
            }
            char* end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || !finite(v))
            {
                return false;
            }
            out = v;
            return true;
        }

        // 17 significant digits round-trips a double exactly.
        std::string num(double v)
        {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%.17g", v);
            return std::string{buf};
        }
    } // namespace

    void world_to_screen(const View& v, const Rect& r, double wx, double wy, float& sx, float& sy)
    {
        const double z = v.uu_per_px > 1e-9 ? v.uu_per_px : 1e-9;
        sx = static_cast<float>(static_cast<double>(r.cx()) + (wy - v.cy) / z);
        sy = static_cast<float>(static_cast<double>(r.cy()) - (wx - v.cx) / z);
    }

    void screen_to_world(const View& v, const Rect& r, float sx, float sy, double& wx, double& wy)
    {
        const double z = v.uu_per_px > 1e-9 ? v.uu_per_px : 1e-9;
        wx = v.cx - (static_cast<double>(sy) - static_cast<double>(r.cy())) * z;
        wy = v.cy + (static_cast<double>(sx) - static_cast<double>(r.cx())) * z;
    }

    double clamp_zoom(double z, double lo, double hi)
    {
        if (!finite(lo) || lo <= 0.0)
        {
            lo = 1.0;
        }
        if (!finite(hi) || hi <= 0.0)
        {
            hi = lo;
        }
        if (hi < lo)
        {
            const double t = lo;
            lo = hi;
            hi = t;
        }
        if (!finite(z) || z <= 0.0)
        {
            return lo;
        }
        return z < lo ? lo : (z > hi ? hi : z);
    }

    double zoom_by(double z, double notches, double factor, double lo, double hi)
    {
        z = clamp_zoom(z, lo, hi);
        if (!finite(notches) || notches == 0.0 || !finite(factor) || factor <= 1.0)
        {
            return z;
        }
        // Positive notches zoom in, i.e. fewer world units per pixel.
        return clamp_zoom(z * std::pow(factor, -notches), lo, hi);
    }

    double fit_zoom(double span_x_uu, double span_y_uu, double canvas_w_px, double canvas_h_px,
                    double margin)
    {
        if (!finite(span_x_uu) || !finite(span_y_uu) || !finite(canvas_w_px) || !finite(canvas_h_px))
        {
            return 0.0;
        }
        if (span_x_uu <= 0.0 || span_y_uu <= 0.0 || canvas_w_px <= 1.0 || canvas_h_px <= 1.0)
        {
            return 0.0;
        }
        const double m = (margin > 0.0 && margin < 0.5) ? margin : 0.0;
        const double h = canvas_h_px * (1.0 - m);
        const double w = canvas_w_px * (1.0 - m);
        const double by_x = span_x_uu / h; // north-south spans the canvas height
        const double by_y = span_y_uu / w; // east-west spans the canvas width
        return by_x > by_y ? by_x : by_y;  // the bigger uu/px is the one that fits both
    }

    int parse_zoom_presets(std::string_view text, float out[kMaxZoomPresets], std::string* rejected)
    {
        float found[kMaxZoomPresets]{};
        int n = 0;
        std::string token;
        const auto flush = [&]() {
            const std::string t = trim(token);
            token.clear();
            if (t.empty())
            {
                return;
            }
            double v = 0.0;
            // The same clamps the zoom itself obeys (mmstate's clamp_config).
            if (!parse_double(t, v) || v < 2.0 || v > 400.0)
            {
                if (rejected != nullptr)
                {
                    if (!rejected->empty())
                    {
                        *rejected += ", ";
                    }
                    *rejected += t;
                }
                return;
            }
            const float f = static_cast<float>(v);
            for (int i = 0; i < n; ++i)
            {
                if (found[i] == f)
                {
                    return; // duplicate rung
                }
            }
            if (n < kMaxZoomPresets)
            {
                found[n++] = f;
            }
        };
        for (const char c : text)
        {
            if (c == ',' || c == ';' || c == ' ' || c == '\t')
            {
                flush();
            }
            else
            {
                token.push_back(c);
            }
        }
        flush();

        if (n == 0)
        {
            return 0;
        }
        // Ascending, so next_zoom_preset() walks the array and wraps at the end.
        for (int i = 1; i < n; ++i)
        {
            const float key = found[i];
            int j = i - 1;
            while (j >= 0 && found[j] > key)
            {
                found[j + 1] = found[j];
                --j;
            }
            found[j + 1] = key;
        }
        for (int i = 0; i < n; ++i)
        {
            out[i] = found[i];
        }
        return n;
    }

    float step_zoom_preset(const float* presets, int count, float current, int dir)
    {
        if (presets == nullptr || count <= 0 || dir == 0)
        {
            return current;
        }
        if (dir > 0)
        {
            for (int i = 0; i < count; ++i)
            {
                if (presets[i] > current * 1.001f)
                {
                    return presets[i];
                }
            }
            return presets[0]; // past the top rung: wrap to the most zoomed-in
        }
        for (int i = count; i-- > 0;)
        {
            if (presets[i] < current * 0.999f)
            {
                return presets[i];
            }
        }
        return presets[count - 1];
    }

    float next_zoom_preset(const float* presets, int count, float current)
    {
        return step_zoom_preset(presets, count, current, 1);
    }

    int zoom_preset_index(const float* presets, int count, float current)
    {
        if (presets == nullptr || count <= 0)
        {
            return -1;
        }
        for (int i = 0; i < count; ++i)
        {
            const float lo = presets[i] * 0.999f;
            const float hi = presets[i] * 1.001f;
            if (current >= lo && current <= hi)
            {
                return i;
            }
        }
        return -1;
    }

    bool waypoint_parse(std::string_view text, Waypoint& out)
    {
        std::size_t pos = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            pos = 3;
        }

        Waypoint wp{};
        bool have_set = false;
        bool have_x = false;
        bool have_y = false;
        while (pos <= text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            std::string_view raw = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
            pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

            const std::size_t comment = raw.find_first_of(";#");
            if (comment != std::string_view::npos)
            {
                raw = raw.substr(0, comment);
            }
            const std::size_t eq = raw.find('=');
            if (eq == std::string_view::npos)
            {
                continue;
            }
            const std::string key = trim(raw.substr(0, eq));
            const std::string value = trim(raw.substr(eq + 1));
            if (key == "set")
            {
                wp.set = !(value == "0" || value == "false" || value == "no" || value == "off");
                have_set = true;
            }
            else if (key == "x")
            {
                have_x = parse_double(value, wp.x);
            }
            else if (key == "y")
            {
                have_y = parse_double(value, wp.y);
            }
            else if (key == "z")
            {
                parse_double(value, wp.z);
            }
        }

        if (!have_x || !have_y)
        {
            return false;
        }
        if (!have_set)
        {
            wp.set = true; // just x/y/z means "here"
        }
        out = wp;
        return true;
    }

    std::string waypoints_serialize(const WaypointSet& set)
    {
        std::string out;
        out += "; WuchangMinimap waypoints. Written when you set one on the full map;\n";
        out += "; delete this file, or delete a line, to drop them. One line each:\n";
        out += ";   waypoint = <x> <y> <z>\n";
        for (std::size_t i = 0; i < set.count && i < kMaxWaypoints; ++i)
        {
            const Waypoint& wp = set.items[i];
            if (!wp.set)
            {
                continue;
            }
            out += "waypoint = " + num(wp.x) + " " + num(wp.y) + " " + num(wp.z) + "\n";
        }
        return out;
    }

    bool waypoints_parse(std::string_view text, WaypointSet& out)
    {
        std::size_t pos = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            pos = 3;
        }

        WaypointSet set{};
        bool any = false;
        bool saw_key = false; // a `waypoint =` line, however unreadable its value
        while (pos <= text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            std::string_view raw = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
            pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

            const std::size_t comment = raw.find_first_of(";#");
            if (comment != std::string_view::npos)
            {
                raw = raw.substr(0, comment);
            }
            const std::size_t eq = raw.find('=');
            if (eq == std::string_view::npos || trim(raw.substr(0, eq)) != "waypoint")
            {
                continue;
            }
            saw_key = true;
            // `x y z`, also tolerating commas between the three.
            std::string value = trim(raw.substr(eq + 1));
            for (char& c : value)
            {
                if (c == ',' || c == '\t')
                {
                    c = ' ';
                }
            }
            double v[3] = {0.0, 0.0, 0.0};
            int got = 0;
            std::size_t at = 0;
            while (got < 3 && at < value.size())
            {
                while (at < value.size() && value[at] == ' ')
                {
                    ++at;
                }
                if (at >= value.size())
                {
                    break;
                }
                const std::size_t end = value.find(' ', at);
                const std::string token =
                    value.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = (end == std::string::npos) ? value.size() : end;
                if (!parse_double(token, v[got]))
                {
                    got = 0;
                    break;
                }
                ++got;
            }
            if (got < 2 || set.count >= kMaxWaypoints)
            {
                continue;
            }
            Waypoint& wp = set.items[set.count++];
            wp.set = true;
            wp.x = v[0];
            wp.y = v[1];
            wp.z = v[2];
            any = true;
        }

        if (!any)
        {
            if (saw_key)
            {
                return false; // waypoint lines that carry no coordinates
            }
            // The older format: one waypoint as a `set` / `x` / `y` / `z` block. Absent
            // too, the file is the empty set the Clear all button writes.
            Waypoint one{};
            if (waypoint_parse(text, one) && one.set)
            {
                set.count = 1;
                set.items[0] = one;
            }
        }
        out = set;
        return true;
    }

    int nearest_waypoint(const WaypointSet& set, double x, double y)
    {
        int best = -1;
        double best_d2 = 0.0;
        for (std::size_t i = 0; i < set.count && i < kMaxWaypoints; ++i)
        {
            const double dx = set.items[i].x - x;
            const double dy = set.items[i].y - y;
            const double d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < best_d2)
            {
                best = static_cast<int>(i);
                best_d2 = d2;
            }
        }
        return best;
    }

    WaypointToggleResult waypoint_toggle_at(const WaypointSet& set, double x, double y, double z,
                                            double eps)
    {
        for (std::size_t i = 0; i < set.count && i < kMaxWaypoints; ++i)
        {
            const Waypoint& w = set.items[i];
            if (std::fabs(w.x - x) <= eps && std::fabs(w.y - y) <= eps && std::fabs(w.z - z) <= eps)
            {
                return WaypointToggleResult{WaypointToggle::Remove, static_cast<int>(i)};
            }
        }
        return WaypointToggleResult{set.count >= kMaxWaypoints ? WaypointToggle::Full
                                                              : WaypointToggle::Add,
                                    -1};
    }
} // namespace mv
