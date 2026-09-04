#pragma once

//
// projection - world -> camera -> NDC -> screen. No Windows, D3D12, ImGui, UE4SS or
// mod state, so the arithmetic is tested offline.
//
// Unreal conventions, all load bearing:
//   * left-handed world: +X forward/north, +Y right/east, +Z up;
//   * FRotator is (pitch, yaw, roll) in DEGREES; the camera basis is UE's
//     FRotationMatrix, spelled out in basis();
//   * FMinimalViewInfo::FOV is HORIZONTAL in degrees when the viewport is wider than
//     tall. UE then uses XAxisMultiplier = 1, YAxisMultiplier = w/h, so
//     tan(vfov/2) = tan(hfov/2) / aspect. The portrait branch is implemented too;
//   * reverse-Z is irrelevant: nothing here produces a depth value, only x/y.
//
// A point with camera-space depth <= near gets no screen position (a divide would
// mirror it to the opposite side of the screen). project() sets `behind` and returns a
// DIRECTION instead: camera-space right/up, FOV-scaled but not divided by depth,
// normalised to NDC length 2 so the caller's edge clamp puts an arrow on the rim.
// Exactly behind the camera points straight down.
//

#include <cmath>

namespace proj
{
    inline constexpr double kPi = 3.14159265358979323846;
    // Closer than this to the camera plane counts as "behind".
    inline constexpr double kNearUu = 1.0;

    struct Camera
    {
        double x = 0.0; // world location, uu
        double y = 0.0;
        double z = 0.0;
        double pitch = 0.0; // degrees, UE FRotator
        double yaw = 0.0;
        double roll = 0.0;
        double fov_deg = 90.0; // horizontal, degrees
    };

    struct Vec3
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct Basis
    {
        Vec3 forward;
        Vec3 right;
        Vec3 up;
    };

    // UE's FRotationMatrix rows, spelled out exactly as the engine has them.
    inline Basis basis(double pitch_deg, double yaw_deg, double roll_deg)
    {
        const double p = pitch_deg * kPi / 180.0;
        const double y = yaw_deg * kPi / 180.0;
        const double r = roll_deg * kPi / 180.0;
        const double sp = std::sin(p);
        const double cp = std::cos(p);
        const double sy = std::sin(y);
        const double cy = std::cos(y);
        const double sr = std::sin(r);
        const double cr = std::cos(r);

        Basis b{};
        b.forward = Vec3{cp * cy, cp * sy, sp};
        b.right = Vec3{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
        b.up = Vec3{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
        return b;
    }

    inline double dot(const Vec3& a, double x, double y, double z)
    {
        return a.x * x + a.y * y + a.z * z;
    }

    struct Result
    {
        bool valid = false;     // the camera and the screen size were usable at all
        bool behind = false;    // camera-space depth <= kNearUu
        bool on_screen = false; // in front AND inside the NDC box
        double depth = 0.0;     // uu along the camera forward axis (signed)
        double dist = 0.0;      // straight-line distance camera -> point, uu
        // NDC, -1..1, x right and y UP. When `behind` these are a direction only.
        double ndc_x = 0.0;
        double ndc_y = 0.0;
        // Screen pixels, origin top-left. Only meaningful when !behind.
        double sx = 0.0;
        double sy = 0.0;
    };

    // Hard gate before anything is drawn: a garbage read must produce no highlight.
    inline bool camera_sane(const Camera& c)
    {
        const auto finite = [](double v) { return std::isfinite(v); };
        if (!finite(c.x) || !finite(c.y) || !finite(c.z) || !finite(c.pitch) || !finite(c.yaw) ||
            !finite(c.roll) || !finite(c.fov_deg))
        {
            return false;
        }
        // Wuchang's world fits in a few hundred thousand uu; 2e6 still rejects the
        // 1e18 / 1e-300 values a wrong byte offset produces.
        if (std::fabs(c.x) > 2.0e6 || std::fabs(c.y) > 2.0e6 || std::fabs(c.z) > 2.0e6)
        {
            return false;
        }
        // UE clamps camera pitch to +-90 and does not wrap yaw on save (a Chapter-1
        // actor ships yaw 447), so yaw is bounded only loosely.
        if (std::fabs(c.pitch) > 90.5 || std::fabs(c.roll) > 180.5 || std::fabs(c.yaw) > 1.0e4)
        {
            return false;
        }
        return c.fov_deg >= 5.0 && c.fov_deg <= 170.0;
    }

    // `screen_w` / `screen_h` are the swapchain's, in pixels.
    inline Result project(const Camera& c, double wx, double wy, double wz, double screen_w, double screen_h)
    {
        Result out{};
        if (!(screen_w > 1.0) || !(screen_h > 1.0) || !camera_sane(c))
        {
            return out;
        }
        if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz))
        {
            return out;
        }
        out.valid = true;

        const Basis b = basis(c.pitch, c.yaw, c.roll);
        const double dx = wx - c.x;
        const double dy = wy - c.y;
        const double dz = wz - c.z;
        const double fwd = dot(b.forward, dx, dy, dz);
        const double rgt = dot(b.right, dx, dy, dz);
        const double up = dot(b.up, dx, dy, dz);

        out.depth = fwd;
        out.dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        const double tan_half = std::tan(c.fov_deg * 0.5 * kPi / 180.0);
        if (!(tan_half > 1e-6))
        {
            out.valid = false;
            return out;
        }
        const double aspect = screen_w / screen_h;
        // UE's SceneView: the wider axis keeps the given FOV, the other is scaled.
        const double mult_x = aspect >= 1.0 ? 1.0 : (screen_h / screen_w);
        const double mult_y = aspect >= 1.0 ? aspect : 1.0;

        if (fwd <= kNearUu)
        {
            // Behind (or in) the camera plane: no screen position, only the direction to
            // turn, pushed outside the NDC box for the caller's edge clamp.
            out.behind = true;
            double ax = rgt * mult_x / tan_half;
            double ay = up * mult_y / tan_half;
            double len = std::sqrt(ax * ax + ay * ay);
            if (!(len > 1e-9))
            {
                // Exactly behind: no side is nearer, so point down.
                ax = 0.0;
                ay = -1.0;
                len = 1.0;
            }
            out.ndc_x = ax / len * 2.0;
            out.ndc_y = ay / len * 2.0;
            return out;
        }

        out.ndc_x = (rgt / fwd) * mult_x / tan_half;
        out.ndc_y = (up / fwd) * mult_y / tan_half;
        out.sx = (out.ndc_x * 0.5 + 0.5) * screen_w;
        out.sy = (0.5 - out.ndc_y * 0.5) * screen_h;
        // A point on the rim counts as visible; the epsilon absorbs the tan/divide.
        constexpr double kEdge = 1.0 + 1e-9;
        out.on_screen = out.ndc_x >= -kEdge && out.ndc_x <= kEdge && out.ndc_y >= -kEdge && out.ndc_y <= kEdge;
        return out;
    }
} // namespace proj
