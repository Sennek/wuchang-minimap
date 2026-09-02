#pragma once

//
// mapdata - loads maps/maps.json and the chapter PNGs the offline pipeline produced
// (tools/navmesh/build_map.py).
//
// Everything here runs on the UE4SS event-loop thread: parsing, file reads and the
// WIC PNG decode. The result is a heap buffer of RGBA8 pixels handed to the render
// thread through an atomic, so Present never blocks on a 90 MB decode.
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mapdata
{
    // Hard cap on floor layers per chapter. Chapter 1 has 9; the cap keeps every
    // runtime array a fixed size and bounds the texture budget.
    constexpr int kMaxLayers = 16;

    // The maximum number of floor layers one surface band can name (build_map.py
    // writes at most 4).
    constexpr int kMaxBandFloors = 4;

    //==================================================================================
    // One walkable surface at one XY grid cell
    //==================================================================================
    //
    // `build_map.py` splits each cell's polygons by Z at gaps > floor_band_gap, so a
    // band is "the storey you are standing on at this spot". It names the floor
    // layer(s) that painted it - usually one, sometimes two adjacent global ranks that
    // the clustering smoothed to nearly the same height.

    struct Band
    {
        float z_min = 0.0f;
        float z_max = 0.0f;
        int floor_count = 0;
        int floors[kMaxBandFloors]{};

        // 0 while `z` is inside the band, else the distance to the nearer edge.
        double distance(double z) const
        {
            if (z < z_min)
            {
                return z_min - z;
            }
            if (z > z_max)
            {
                return z - z_max;
            }
            return 0.0;
        }
    };

    // Sparse (gx, gy) -> bands, low Z first. Immutable once published.
    struct FloorGrid
    {
        double cell_uu = 0.0;
        std::unordered_map<std::uint64_t, std::vector<Band>> cells;

        static std::uint64_t key(int gx, int gy)
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 32) |
                   static_cast<std::uint32_t>(gy);
        }

        const std::vector<Band>* at(double wx, double wy) const;
    };

    //==================================================================================
    // One pre-rendered floor layer (an 8-bit coverage mask, cropped to its own bbox)
    //==================================================================================

    struct Layer
    {
        int floor = 0;
        std::string image;
        int image_width = 0;
        int image_height = 0;
        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        double px_per_uu = 0.0;
        double z_min = 0.0;
        double z_max = 0.0;
        int poly_count = 0;

        void to_uv(double wx, double wy, float& u, float& v) const
        {
            const double upx = (wy - min_y) * px_per_uu;
            const double vpx = (max_x - wx) * px_per_uu;
            u = image_width > 0 ? static_cast<float>(upx / image_width) : 0.0f;
            v = image_height > 0 ? static_cast<float>(vpx / image_height) : 0.0f;
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

        // The north-up mapping from build_map.py / render.py, in normalised uv:
        //   u_px = (Y - min_y) * px_per_uu ;  v_px = (max_x - X) * px_per_uu
        void to_uv(double wx, double wy, float& u, float& v) const
        {
            const double upx = (wy - min_y) * px_per_uu;
            const double vpx = (max_x - wx) * px_per_uu;
            u = image_width > 0 ? static_cast<float>(upx / image_width) : 0.0f;
            v = image_height > 0 ? static_cast<float>(vpx / image_height) : 0.0f;
        }

        // The per-floor layers, low floor first, and the surface-band grid. Both are
        // shared_ptr so the copy `chapters()` hands out is cheap.
        std::shared_ptr<const std::vector<Layer>> layers;
        std::shared_ptr<const FloorGrid> grid;

        int layer_count() const
        {
            return layers ? static_cast<int>(layers->size()) : 0;
        }

        // Index into `*layers` for a floor number, or -1.
        int layer_of_floor(int floor) const
        {
            if (!layers)
            {
                return -1;
            }
            for (std::size_t i = 0; i < layers->size(); ++i)
            {
                if ((*layers)[i].floor == floor)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }
    };

    // A decoded texture waiting to be uploaded by the render thread.
    struct PendingImage
    {
        std::string chapter_key;
        int layer_index = -1; // -1 = the chapter composite, else index into layers
        int floor = -1;
        int width = 0;
        int height = 0;
        int channels = 4; // 4 = RGBA8 (composite), 1 = R8 coverage mask (layer)
        std::vector<std::uint8_t> pixels; // width * height * channels, top-down
    };

    // Loop thread: read maps.json, then decode the first chapter's layer PNGs (and
    // its composite, if `mm::Config::fallback_use_composite` is on). Safe to call
    // repeatedly; a second call re-reads everything (that is what F5 does).
    void load(const std::wstring& mod_dir);

    // Render thread: take ownership of one decoded image, if any is ready. Called once
    // per frame, so a nine-layer chapter uploads over nine frames instead of stalling
    // one.
    std::unique_ptr<PendingImage> take_pending();

    // Both threads: the chapter list is written once at load and then only read.
    // A copy is returned so the render thread never walks a container being rebuilt.
    std::vector<Chapter> chapters();

    // Chapter whose world bounds contain (wx, wy), or nullopt-ish (empty key).
    Chapter chapter_for(double wx, double wy);

    // Same, but a pointer into the published (and deliberately never freed) chapter
    // list - so the render thread can read the layers and the band grid every frame
    // without copying them. nullptr if nothing matches.
    const Chapter* chapter_ptr_for(double wx, double wy);

    bool loaded();
} // namespace mapdata
