#pragma once

//
// texupload - PURE. Where each rectangle of one texture update sits in a single staging
// buffer: rows padded to the copy pitch, every rectangle starting on a placement boundary.
//
// No Windows, no D3D12, no ImGui. The two alignments restate D3D12's, and overlay_imtex.cpp
// checks them against the SDK headers with a static_assert.
//

#include <cstdint>

namespace texup
{
    constexpr std::uint32_t kPitchAlign = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
    constexpr std::uint64_t kPlaceAlign = 512; // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT

    struct Placed
    {
        std::uint64_t offset = 0; // from the start of the buffer
        std::uint32_t pitch = 0;  // bytes per row in the buffer
    };

    inline std::uint32_t row_pitch(int width, int bytes_per_pixel)
    {
        const std::uint32_t raw = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(bytes_per_pixel);
        return (raw + kPitchAlign - 1) / kPitchAlign * kPitchAlign;
    }

    // Lays `n` rectangles (anything with `w` and `h`) out back to back into `out`. Returns the
    // bytes the buffer needs.
    template <class Rect>
    std::uint64_t lay_out(const Rect* rects, int n, int bytes_per_pixel, Placed* out)
    {
        std::uint64_t at = 0;
        for (int i = 0; i < n; ++i)
        {
            at = (at + kPlaceAlign - 1) / kPlaceAlign * kPlaceAlign;
            out[i].offset = at;
            out[i].pitch = row_pitch(rects[i].w, bytes_per_pixel);
            at += static_cast<std::uint64_t>(out[i].pitch) * static_cast<std::uint64_t>(rects[i].h);
        }
        return at;
    }
} // namespace texup
