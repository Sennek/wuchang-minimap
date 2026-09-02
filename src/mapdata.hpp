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
#include <vector>

namespace mapdata
{
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
    };

    // A decoded texture waiting to be uploaded by the render thread.
    struct PendingImage
    {
        std::string chapter_key;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba; // width * height * 4, top-down
    };

    // Loop thread: read maps.json, then decode the first chapter's PNG. Safe to call
    // repeatedly; a second call re-reads everything (that is what F5 does).
    void load(const std::wstring& mod_dir);

    // Render thread: take ownership of the decoded pixels, if any are ready.
    std::unique_ptr<PendingImage> take_pending();

    // Both threads: the chapter list is written once at load and then only read.
    // A copy is returned so the render thread never walks a container being rebuilt.
    std::vector<Chapter> chapters();

    // Chapter whose world bounds contain (wx, wy), or nullopt-ish (empty key).
    Chapter chapter_for(double wx, double wy);

    bool loaded();
} // namespace mapdata
