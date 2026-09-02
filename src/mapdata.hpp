#pragma once

//
// mapdata - loads maps/maps.json and the chapter PNGs the offline pipeline produced
// (tools/navmesh/build_map.py, schema `wuchang-minimap-maps/3`).
//
// WHAT THE ASSET IS (changed 2026-09-02, round 3 of the in-world feedback)
// ------------------------------------------------------------------------
// The chapter ships a MULTI-SURFACE HEIGHT MAP: `max_surfaces` (8) 16-bit grayscale
// PNGs of identical size and bounds, where plane k at pixel (px, py) holds the Z of
// the k-th walkable surface at that spot, lowest first, quantised over the chapter's
// [z_min, z_max] into 1..65535 - and 0 means "no surface here".
//
// EIGHT, not four: four slots hold 93 % of the chapter's lit pixels but only 50 % of
// them in the Digong-spiral / Hanguang-temple block, where a pixel can carry up to
// eleven surfaces. Sliced at the temple's feet Z, four slots left 5 655 opaque pixels
// against 17 807 at eight - two thirds of the floor missing, which is the fragmented
// look this whole rewrite exists to remove. Eight is within 2 % of sixteen. The top
// slot is the OVERFLOW slot and holds the highest Z where a stack is deeper than
// eight, so the top of a deep stack is never what gets dropped.
//
// Sampling must be POINT/nearest: interpolating a quantised height code across a
// storey boundary invents a floor halfway between two real ones. (The RGBA slice the
// runtime produces from it is a colour, so the GPU may filter that one freely.)
//
// It replaces the per-pixel surface-ORDINAL layer scheme (8 R8 coverage masks + a
// 640-uu band lookup grid). Ordinals were exactly separable but unreadable in-world:
// the runtime could only guess which ordinal the player's storey was, so a temple
// interior came out as several blended layers. With a height map the runtime knows
// the actual Z of every pixel and slices `|Z - feetZ| <= tolerance` on the CPU, which
// is the exact semantics the shader path would have given - without a custom PSO on
// a ReShade-wrapped swapchain. See CURRENT.md for the full decision.
//
// Threading: everything here runs on the UE4SS event-loop thread - parsing, file
// reads and the WIC PNG decode. The height planes are then published as an immutable
// shared_ptr the render thread reads directly (no per-frame copy), and the composite
// is handed over as a decoded RGBA buffer through an atomic queue so Present never
// blocks on the decode.
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mapdata
{
    // How many stacked walkable surfaces one pixel can carry. Must match
    // build_map.py's --max-surfaces (shipped: 8). Fewer planes than this in the
    // manifest is fine - `HeightMaps::count` is what the slicer reads.
    constexpr int kMaxSurfaces = 8;

    //==================================================================================
    // The multi-surface height map
    //==================================================================================

    struct HeightMaps
    {
        int width = 0;
        int height = 0;
        int count = 0; // planes actually decoded, <= kMaxSurfaces
        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        double px_per_uu = 0.0;
        float z_min = 0.0f;
        float z_max = 0.0f;

        // plane[k][py * width + px]; 0 = no surface, else 1 + round(t * 65534).
        std::vector<std::uint16_t> plane[kMaxSurfaces];

        // uu per quantisation step - reported once, so a "the gradient is banded"
        // report can be checked against the asset instead of the renderer.
        float z_step() const
        {
            return count > 0 ? (z_max - z_min) / 65534.0f : 0.0f;
        }

        float decode(std::uint16_t code) const
        {
            return z_min + (static_cast<float>(code) - 1.0f) * z_step();
        }

        // World -> source pixel, the north-up mapping build_map.py / render.py write:
        //   px = (Y - min_y) * px_per_uu ;  py = (max_x - X) * px_per_uu
        void to_px(double wx, double wy, double& px, double& py) const
        {
            px = (wy - min_y) * px_per_uu;
            py = (max_x - wx) * px_per_uu;
        }

        std::size_t bytes() const
        {
            std::size_t n = 0;
            for (int i = 0; i < kMaxSurfaces; ++i)
            {
                n += plane[i].size() * sizeof(std::uint16_t);
            }
            return n;
        }

        bool ready() const
        {
            return count > 0 && width > 0 && height > 0 && z_max > z_min;
        }
    };

    struct Chapter
    {
        std::string key;   // "chapter1"
        std::string image; // "chapter1/small.png", relative to the maps dir
        int image_width = 0;
        int image_height = 0;
        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        double px_per_uu = 0.0;

        bool contains(double wx, double wy) const
        {
            return wx >= min_x && wx <= max_x && wy >= min_y && wy <= max_y;
        }

        // The north-up mapping, in normalised uv.
        void to_uv(double wx, double wy, float& u, float& v) const
        {
            const double upx = (wy - min_y) * px_per_uu;
            const double vpx = (max_x - wx) * px_per_uu;
            u = image_width > 0 ? static_cast<float>(upx / image_width) : 0.0f;
            v = image_height > 0 ? static_cast<float>(vpx / image_height) : 0.0f;
        }

        // The height-plane PNG paths from maps.json, lowest surface first.
        std::vector<std::string> height_files;

        // Immutable once published, so the render thread reads it every frame without
        // copying anything.
        std::shared_ptr<const HeightMaps> heights;

        bool has_heights() const
        {
            return heights && heights->ready();
        }
    };

    // A decoded texture waiting to be uploaded by the render thread. Only the chapter
    // composite goes through here now (the height planes stay on the CPU).
    struct PendingImage
    {
        std::string chapter_key;
        int width = 0;
        int height = 0;
        int channels = 4;                 // 4 = RGBA8
        std::vector<std::uint8_t> pixels; // width * height * channels, top-down
    };

    // Loop thread: read maps.json, decode the first chapter's height planes (and its
    // composite, if `mm::Config::fallback_use_composite` is on). Safe to call
    // repeatedly; a second call re-reads everything (that is what F5 does).
    void load(const std::wstring& mod_dir);

    // Render thread: take ownership of one decoded image, if any is ready.
    std::unique_ptr<PendingImage> take_pending();

    // Both threads: the chapter list is written once at load and then only read.
    std::vector<Chapter> chapters();

    // Chapter whose world bounds contain (wx, wy) - empty key if none.
    Chapter chapter_for(double wx, double wy);

    // Same, but a pointer into the published (and deliberately never freed) chapter
    // list, so the render thread can read the height planes every frame without a
    // copy. nullptr if nothing matches.
    const Chapter* chapter_ptr_for(double wx, double wy);

    bool loaded();
} // namespace mapdata
