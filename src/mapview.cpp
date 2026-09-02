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

        // std::format is available, but this file is deliberately dependency-light and
        // is also linked into the test exe; snprintf with an explicit format is enough
        // and round-trips a double exactly at 17 significant digits.
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
        // Positive notches zoom IN, i.e. fewer world units per pixel.
        return clamp_zoom(z * std::pow(factor, -notches), lo, hi);
    }

    std::string waypoint_serialize(const Waypoint& wp)
    {
        std::string out;
        out += "; WuchangMinimap waypoint. Written when you set one on the full map;\n";
        out += "; delete this file (or set `set = 0`) to clear it.\n";
        out += "set = ";
        out += wp.set ? "1" : "0";
        out += "\n";
        out += "x = " + num(wp.x) + "\n";
        out += "y = " + num(wp.y) + "\n";
        out += "z = " + num(wp.z) + "\n";
        return out;
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
            wp.set = true; // a hand-written file with just x/y/z means "here"
        }
        out = wp;
        return true;
    }
} // namespace mv
