#pragma once

//
// mapdata - loads maps/maps.json and the chapter PNGs the offline pipeline produced
// (tools/navmesh/build_map.py, schema `wuchang-minimap-maps/3`).
//
// THE ASSET
// ---------
// A chapter ships a MULTI-SURFACE HEIGHT MAP: `max_surfaces` (8) 16-bit grayscale PNGs of
// identical size and bounds, where plane k at pixel (px, py) holds the Z of the k-th
// walkable surface at that spot, lowest first, quantised over the chapter's [z_min, z_max]
// into 1..`z_code_max`; 0 means "no surface here".
//
// Only 12 bits of the 16-bit sample are used (schema /4), for a Z step of 3.98..12.44 uu
// depending on the chapter's span. The slicer's floor tolerance is 200 uu and its fade
// 800 uu, so the worst error (+/- 6.22 uu) is 3 % of the decision it feeds. The divisor
// comes from the manifest (`z_code_max`), and the schema string is checked for EXACT
// equality: a /3 plane read here would put every surface sixteen times too low and look
// like an empty map rather than a version error. See src/mapmanifest.hpp.
//
// Eight slots, not four: four hold 93 % of a chapter's lit pixels but only 50 % of them in
// the Digong-spiral / Hanguang-temple block, where a pixel can carry up to eleven
// surfaces. Eight is within 2 % of sixteen. The top slot is the OVERFLOW slot and holds
// the highest Z where a stack is deeper than eight, so the top of a deep stack is never
// what gets dropped.
//
// Sampling must be POINT/nearest: interpolating a quantised height code across a storey
// boundary invents a floor halfway between two real ones. (The RGBA slice the runtime
// produces from it is a colour, so the GPU may filter that one freely.)
//
// The runtime knows the actual Z of every pixel and slices `|Z - feetZ| <= tolerance` on
// the CPU - the exact semantics a shader path would give, without a custom PSO on a
// ReShade-wrapped swapchain.
//
// ONE CHAPTER AT A TIME
// ---------------------
// Five chapters ship and each costs 320-340 MB of RAM, so only ONE is ever resident.
// `gamestate` names the chapter the player is in from the streamed `B<N>EX0_...` cell
// packages (chapterid.hpp) and calls `set_detected_chapter()` from the game thread;
// `on_update()` runs the swap on the loop thread. The swap is RETIRE-THEN-LOAD, never
// load-then-retire, so the peak is one chapter and not two:
//
//   1. the outgoing chapter's `heights` pointer is cleared (readers see "no height maps"
//      from the very next frame),
//   2. the planes are freed only after `kRetireGraceMs` - a render thread that had already
//      dereferenced the pointer finishes its ~4 ms slice a hundred times over inside that
//      window,
//   3. only then are the incoming chapter's planes decoded (~1-3 s of WIC).
//
// A chapter change happens at a loading screen, where the overlay is hidden by the
// transition cooldown.
//
// Threading: everything here runs on the UE4SS event-loop thread - parsing, file reads and
// the WIC PNG decode - except `set_detected_chapter()` (game thread, one atomic store) and
// the two reader functions the render thread uses. The height planes are published as a
// raw `const HeightMaps*` the render thread reads directly (no per-frame copy, no
// shared_ptr refcount traffic in the frame path); the pointer is a single aligned 8-byte
// slot, so a reader sees either the old planes or none, never a torn value. The composite
// is handed over as a decoded RGBA buffer through an atomic queue so Present never blocks
// on the decode.
//

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mapmanifest.hpp"

namespace mapdata
{
    // How many stacked walkable surfaces one pixel can carry. Must match build_map.py's
    // --max-surfaces (shipped: 8). Fewer planes in the manifest is fine -
    // `HeightMaps::count` is what the slicer reads.
    constexpr int kMaxSurfaces = mapmanifest::kMaxSurfaces;

    // How long a retired chapter's height planes stay alive after the pointer to them is
    // cleared. A slice is ~4 ms; this is three orders of magnitude more.
    constexpr std::uint64_t kRetireGraceMs = 2000;

    //==================================================================================
    // The multi-surface height map
    //==================================================================================

    // How big a block the sparse store allocates in. 128 px measured best of the sizes
    // tried: chapter 1's eight planes light 2 766 of 10 608 blocks (26.1 %). The lit
    // pixels are scattered through every building on the map, not clustered.
    constexpr int kTilePx = 128;
    constexpr int kTileShift = 7; // 1 << 7 == kTilePx
    constexpr int kTileMask = kTilePx - 1;
    constexpr int kTileCells = kTilePx * kTilePx;

    // ONE SURFACE LAYER, STORED SPARSELY. A chapter's walkable area is a quarter of its
    // bounding box and the deeper surfaces are rarer still (plane 0 lights 62 % of the
    // blocks, plane 7 lights 3 %), so only the non-empty blocks are allocated and `tile`
    // is a block-resolution index into them: 86 MB for chapter 1, 47..74 MB for the rest.
    //
    // An absent block reads as code 0, the same answer a dense plane held there. A row of
    // the picture crosses several blocks, so there is no row pointer to hand out and
    // readers gather a row through `HeightMaps::gather_row()`.
    struct HeightPlane
    {
        static constexpr std::int32_t kEmpty = -1;

        // tile[ty * ntx + tx] = the block's index in `data`, or kEmpty.
        std::vector<std::int32_t> tile;
        // kTileCells codes per present block, in `tile`'s index order.
        std::vector<std::uint16_t> data;
        int ntx = 0;
        int nty = 0;
        int tiles = 0; // present blocks

        bool empty() const
        {
            return tiles == 0;
        }

        std::size_t bytes() const
        {
            return data.size() * sizeof(std::uint16_t) + tile.size() * sizeof(std::int32_t);
        }

        const std::uint16_t* block(int tx, int ty) const
        {
            if (tx < 0 || ty < 0 || tx >= ntx || ty >= nty)
            {
                return nullptr;
            }
            const std::int32_t idx = tile[static_cast<std::size_t>(ty) * ntx + tx];
            return idx < 0 ? nullptr
                           : data.data() + static_cast<std::size_t>(idx) * kTileCells;
        }
    };

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
        // Highest height code the asset uses; 0 always means "no surface". From
        // maps.json (`z_code_max`), 4095 since schema /4.
        int z_code_max = mapmanifest::kZCodeMax;

        // The sparse planes, lowest surface first. Read through gather_row().
        HeightPlane layer[kMaxSurfaces];

        // uu per quantisation step - reported once, so a "the gradient is banded"
        // report can be checked against the asset instead of the renderer.
        float z_step() const
        {
            return count > 0 && z_code_max > 1
                       ? (z_max - z_min) / static_cast<float>(z_code_max - 1)
                       : 0.0f;
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

        bool plane_empty(int k) const
        {
            return k < 0 || k >= kMaxSurfaces || layer[k].empty();
        }

        // GATHER ONE DESTINATION ROW of surface `k`: `dst[i]` becomes the height code at
        // source pixel (`col_x[i]`, `sy`), or 0 where `col_x[i]` is negative (the caller's
        // "outside the asset" marker) or the block is absent.
        //
        // The block store's answer to `plane + sy * width`, at the same memory traffic:
        // `n` codes per plane per row, one index lookup per block crossed. `col_x` need
        // not be monotonic; the inner run only requires consecutive columns in the same
        // block to be consecutive in `col_x`.
        //
        // Returns false when the row contributed no surface at all (every code 0), which
        // lets the caller skip the whole row.
        bool gather_row(int k, int sy, const int* col_x, int n, std::uint16_t* dst) const
        {
            if (dst == nullptr || col_x == nullptr || n <= 0 || plane_empty(k) || sy < 0 ||
                sy >= height)
            {
                return false;
            }
            const HeightPlane& p = layer[k];
            const int ty = sy >> kTileShift;
            if (ty >= p.nty)
            {
                return false;
            }
            const std::int32_t* trow = p.tile.data() + static_cast<std::size_t>(ty) * p.ntx;
            const std::size_t row_off = static_cast<std::size_t>(sy & kTileMask) * kTilePx;
            bool any = false;
            int i = 0;
            while (i < n)
            {
                const int sx = col_x[i];
                if (sx < 0)
                {
                    dst[i] = 0;
                    ++i;
                    continue;
                }
                const int tx = sx >> kTileShift;
                const std::int32_t idx = tx < p.ntx ? trow[tx] : HeightPlane::kEmpty;
                if (idx < 0)
                {
                    do
                    {
                        dst[i] = 0;
                        ++i;
                    } while (i < n && col_x[i] >= 0 && (col_x[i] >> kTileShift) == tx);
                    continue;
                }
                const std::uint16_t* src =
                    p.data.data() + static_cast<std::size_t>(idx) * kTileCells + row_off;
                do
                {
                    const std::uint16_t code = src[col_x[i] & kTileMask];
                    dst[i] = code;
                    any = any || code != 0;
                    ++i;
                } while (i < n && col_x[i] >= 0 && (col_x[i] >> kTileShift) == tx);
            }
            return any;
        }

        // One code, for the odd single lookup (diagnostics, the self-test). Not the
        // per-pixel path - gather_row() amortises the index lookup.
        std::uint16_t code_at(int k, int px, int py) const
        {
            if (plane_empty(k) || px < 0 || py < 0 || px >= width || py >= height)
            {
                return 0;
            }
            const std::uint16_t* b = layer[k].block(px >> kTileShift, py >> kTileShift);
            return b == nullptr
                       ? std::uint16_t{0}
                       : b[static_cast<std::size_t>(py & kTileMask) * kTilePx + (px & kTileMask)];
        }

        // The first lit pixel of surface `k`, for a caller that needs a window with
        // geometry in it (the slicer self-test). False when the plane is empty.
        bool first_lit(int k, int& out_px, int& out_py, std::uint16_t& out_code) const
        {
            if (plane_empty(k))
            {
                return false;
            }
            const HeightPlane& p = layer[k];
            for (int ty = 0; ty < p.nty; ++ty)
            {
                for (int tx = 0; tx < p.ntx; ++tx)
                {
                    const std::uint16_t* b = p.block(tx, ty);
                    if (b == nullptr)
                    {
                        continue;
                    }
                    for (int i = 0; i < kTileCells; ++i)
                    {
                        if (b[i] == 0)
                        {
                            continue;
                        }
                        const int px = (tx << kTileShift) + (i & kTileMask);
                        const int py = (ty << kTileShift) + (i >> kTileShift);
                        if (px >= width || py >= height)
                        {
                            continue; // the block's padding past the image edge
                        }
                        out_px = px;
                        out_py = py;
                        out_code = b[i];
                        return true;
                    }
                }
            }
            return false;
        }

        int tiles() const
        {
            int n = 0;
            for (int i = 0; i < kMaxSurfaces; ++i)
            {
                n += layer[i].tiles;
            }
            return n;
        }

        std::size_t bytes() const
        {
            std::size_t n = 0;
            for (int i = 0; i < kMaxSurfaces; ++i)
            {
                n += layer[i].bytes();
            }
            return n;
        }

        // What the dense planes would cost, for the log line. Nothing allocates this.
        std::size_t dense_bytes() const
        {
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                   sizeof(std::uint16_t) * static_cast<std::size_t>(count > 0 ? count : 0);
        }

        bool ready() const
        {
            return count > 0 && width > 0 && height > 0 && z_max > z_min;
        }
    };

    // Fills one sparse plane from a DENSE decoded buffer (`w * h` height codes,
    // row-major), allocating only the 128-px blocks that carry a surface.
    //
    // Inline and here rather than in mapdata.cpp so tests/markers_test.cpp can build a
    // plane with THIS function and compare gather_row() against the dense buffer it came
    // from. The dense buffer is transient - the caller frees it per plane - so a chapter
    // load peaks at the sparse set so far plus one dense plane (~43 MB).
    inline void build_plane(HeightPlane& out, const std::uint16_t* src, int w, int h)
    {
        out = HeightPlane{};
        if (src == nullptr || w <= 0 || h <= 0)
        {
            return;
        }
        out.ntx = (w + kTilePx - 1) / kTilePx;
        out.nty = (h + kTilePx - 1) / kTilePx;
        out.tile.assign(static_cast<std::size_t>(out.ntx) * static_cast<std::size_t>(out.nty),
                        HeightPlane::kEmpty);

        // Two passes: mark the non-empty blocks, then allocate exactly that many and copy.
        // One pass with push_back would reallocate a ~50 MB vector on the loop thread.
        for (int ty = 0; ty < out.nty; ++ty)
        {
            const int y0 = ty << kTileShift;
            const int y1 = (y0 + kTilePx) < h ? (y0 + kTilePx) : h;
            for (int tx = 0; tx < out.ntx; ++tx)
            {
                const int x0 = tx << kTileShift;
                const int x1 = (x0 + kTilePx) < w ? (x0 + kTilePx) : w;
                bool lit = false;
                for (int y = y0; y < y1 && !lit; ++y)
                {
                    const std::uint16_t* row = src + static_cast<std::size_t>(y) * w;
                    for (int x = x0; x < x1; ++x)
                    {
                        if (row[x] != 0)
                        {
                            lit = true;
                            break;
                        }
                    }
                }
                if (lit)
                {
                    out.tile[static_cast<std::size_t>(ty) * out.ntx + tx] = out.tiles++;
                }
            }
        }
        if (out.tiles == 0)
        {
            return;
        }
        // Zero-initialised, so the padding of an edge block - and any hole inside a
        // block - reads as "no surface".
        out.data.assign(static_cast<std::size_t>(out.tiles) * kTileCells, 0);
        for (int ty = 0; ty < out.nty; ++ty)
        {
            const int y0 = ty << kTileShift;
            const int y1 = (y0 + kTilePx) < h ? (y0 + kTilePx) : h;
            for (int tx = 0; tx < out.ntx; ++tx)
            {
                const std::int32_t idx = out.tile[static_cast<std::size_t>(ty) * out.ntx + tx];
                if (idx < 0)
                {
                    continue;
                }
                const int x0 = tx << kTileShift;
                const int x1 = (x0 + kTilePx) < w ? (x0 + kTilePx) : w;
                std::uint16_t* dst = out.data.data() + static_cast<std::size_t>(idx) * kTileCells;
                for (int y = y0; y < y1; ++y)
                {
                    std::memcpy(dst + static_cast<std::size_t>(y - y0) * kTilePx,
                                src + static_cast<std::size_t>(y) * w + x0,
                                static_cast<std::size_t>(x1 - x0) * sizeof(std::uint16_t));
                }
            }
        }
    }

    struct Chapter
    {
        std::string key;   // "chapter1"
        std::string image; // "chapter1/small.png", relative to the maps dir
        // The chapter NUMBER this asset is for (chid::kDlc for the DLC, chid::kNone when
        // the manifest neither states it nor spells it in the key). The runtime's chapter
        // detection matches against this.
        int chapter = chid::kNone;
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

        // The resident height planes, or nullptr when this chapter is not the active one.
        // Owned by mapdata (see kRetireGraceMs); a raw pointer rather than a shared_ptr so
        // the render thread's per-frame read is one aligned load with no refcount traffic,
        // and so a chapter switch cannot be kept alive by a copy a reader holds.
        const HeightMaps* heights = nullptr;

        bool has_heights() const
        {
            return heights && heights->ready();
        }
    };

    // A decoded texture waiting to be uploaded by the render thread. Only the chapter
    // composite goes through here (the height planes stay on the CPU).
    struct PendingImage
    {
        std::string chapter_key;
        int width = 0;
        int height = 0;
        int channels = 4;                 // 4 = RGBA8
        std::vector<std::uint8_t> pixels; // width * height * channels, top-down
    };

    // Loop thread: read maps.json and decode the DEFAULT chapter's height planes (the
    // lowest chapter number in the manifest) plus its composite, if
    // `mm::Config::fallback_use_composite` is on. Safe to call repeatedly.
    void load(const std::wstring& mod_dir);

    // Loop thread, master switch (see modswitch.hpp): free the resident chapter's height
    // planes and every queued image immediately, and forget the detection. Legal only once
    // the overlay's render side has stopped - it skips the kRetireGraceMs delay.
    void unload();

    // Loop thread: drives the retire/decode state machine. Call it every tick.
    void on_update();

    // GAME THREAD: the chapter the player is in, as a chapter NUMBER (chid::kDlc for the
    // DLC, chid::kNone when it cannot be determined). One relaxed atomic store.
    void set_detected_chapter(int chapter);
    int detected_chapter();

    // The chapter whose height planes are resident right now, or an empty string.
    std::string active_chapter_key();

    // Render thread: take ownership of one decoded image, if any is ready.
    std::unique_ptr<PendingImage> take_pending();

    // Both threads: the chapter list is written once at load and then only read.
    std::vector<Chapter> chapters();

    // Chapter whose world bounds contain (wx, wy) - empty key if none.
    Chapter chapter_for(double wx, double wy);

    // THE MAP THE OVERLAY SHOULD DRAW at (wx, wy). A pointer into the published (and
    // never freed) chapter list, so the render thread reads the height planes every frame
    // without a copy. nullptr if nothing matches.
    //
    // Once a chapter is ACTIVE this answers with that chapter and no other: the chapters'
    // world bounds overlap (chapter 4 covers nearly all of chapter 1), so a bounds test
    // would hand back the wrong map. Before anything is known it falls back to "first
    // chapter whose bounds contain the point".
    const Chapter* chapter_ptr_for(double wx, double wy);

    bool loaded();
} // namespace mapdata
