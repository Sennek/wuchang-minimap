#pragma once

//
// projection - world -> camera -> NDC -> screen, and nothing else.
//
// WHY IT IS A SEPARATE, PURE HEADER
// ---------------------------------
// The x-ray highlight stands or falls on this arithmetic: if the basis or the FOV
// convention is wrong the labels sit next to the item instead of on it, and the only
// way to notice would be an in-game session - the scarce resource in this project. So
// the math has no dependency on Windows, D3D12, ImGui, UE4SS or the mod's own state,
// and tests/markers_test.cpp checks it against hand-computed screen coordinates.
//
// CONVENTIONS (Unreal, and they are all load bearing)
//   * left-handed world: +X forward/north, +Y right/east, +Z up. The whole mod already
//     draws maps with +X as "up on the image", so this is the same axis set;
//   * FRotator is (pitch, yaw, roll) in DEGREES, and the camera basis is UE's
//     FRotationMatrix - see basis() below for the exact rows;
//   * FMinimalViewInfo::FOV is the HORIZONTAL field of view in degrees when the
//     viewport is wider than it is tall, which is the only case this game ships in.
//     UE builds the projection with XAxisMultiplier = 1 and YAxisMultiplier = w/h in
//     that case, so tan(vfov/2) = tan(hfov/2) / aspect. The portrait branch (aspect
//     < 1) is implemented too, because it is two lines and a wrong guess there would
//     be invisible;
//   * reverse-Z is irrelevant: nothing here produces a depth buffer value, only x/y.
//
// BEHIND THE CAMERA
// -----------------
// A point with camera-space depth <= near is never drawn at a screen position - a
// naive divide would place it mirrored on the opposite side of the screen, which is
// the classic "the marker for the chest behind me sits on the wall in front of me"
// bug. project() flags it instead (`behind = true`, and `sx`/`sy` are left at their defaults) and still
// returns a usable DIRECTION: the raw camera-space right/up components, FOV-scaled but
// never divided by depth, normalised and pushed to NDC length 2 so the caller's edge
// clamp puts the arrow on the rim. Exactly behind the camera (right == up == 0) is the
// one special case and points straight down.
//

#include <cmath>

namespace proj
{
    inline constexpr double kPi = 3.14159265358979323846;
    // Anything closer than this to the camera plane is treated as "behind": at 1 uu the
    // divide is already numerically meaningless and the item is inside the player's head.
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

    // UE's FRotationMatrix rows, spelled out. Getting one sign wrong here is a bug that
    // only shows up when the player looks up or rolls, so the rows are written exactly
    // as the engine has them rather than "derived".
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

    // A camera pose that could not have come from a live game. Used as a hard gate
    // before anything is drawn: a garbage read must produce "no highlight", never a
    // screenful of labels in the wrong place.
    inline bool camera_sane(const Camera& c)
    {
        const auto finite = [](double v) { return std::isfinite(v); };
        if (!finite(c.x) || !finite(c.y) || !finite(c.z) || !finite(c.pitch) || !finite(c.yaw) ||
            !finite(c.roll) || !finite(c.fov_deg))
        {
            return false;
        }
        // Wuchang's world fits in a few hundred thousand uu; 2e6 is generous and still
        // rejects the 1e18 / 1e-300 values a wrong byte offset produces.
        if (std::fabs(c.x) > 2.0e6 || std::fabs(c.y) > 2.0e6 || std::fabs(c.z) > 2.0e6)
        {
            return false;
        }
        // UE clamps camera pitch to +-90 and does NOT wrap yaw on save (a real Chapter-1
        // actor ships yaw 447), so yaw is only bounded loosely.
        if (std::fabs(c.pitch) > 90.5 || std::fabs(c.roll) > 180.5 || std::fabs(c.yaw) > 1.0e4)
        {
            return false;
        }
        return c.fov_deg >= 5.0 && c.fov_deg <= 170.0;
    }

    // The one function. `screen_w` / `screen_h` are the swapchain's, in pixels.
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
            // Behind (or in) the camera plane. A divide by `fwd` here would mirror the
            // point onto the opposite side of the screen - the classic "the chest behind
            // me is labelled on the wall in front of me" bug - so no screen position is
            // produced at all. What IS well defined is the direction the player has to
            // turn: the camera-space right/up components, pushed outside the NDC box so
            // the caller's edge clamp puts the arrow on the rim.
            out.behind = true;
            double ax = rgt * mult_x / tan_half;
            double ay = up * mult_y / tan_half;
            double len = std::sqrt(ax * ax + ay * ay);
            if (!(len > 1e-9))
            {
                // Exactly behind: no side is nearer. Point down, the usual "turn around"
                // convention, rather than picking a side at random.
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
        // A point exactly on the rim counts as visible; the epsilon is there because
        // "exactly" is a floating-point tan/divide away from 1.0.
        constexpr double kEdge = 1.0 + 1e-9;
        out.on_screen = out.ndc_x >= -kEdge && out.ndc_x <= kEdge && out.ndc_y >= -kEdge && out.ndc_y <= kEdge;
        return out;
    }
} // namespace proj
