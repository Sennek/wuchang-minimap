#pragma once

//
// clipimg - the pure half of "copy the map to the clipboard": BGRA8 pixels in, a
// CF_DIB payload out.
//
// There is one source format and it is a constant: the overlay draws into render
// targets of its own, created `B8G8R8A8_UNORM` by overlay_dcomp.cpp because that is
// what a DirectComposition surface is composed in. The game's own back buffer - HDR10
// on this title - is never read.
//
// No Windows, no D3D12, no allocation beyond the output vector.
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace clipimg
{
    // Copies `px` BGRA8 pixels into the byte order a Windows 32bpp DIB wants, which is
    // the same one - so this is a copy with the alpha forced opaque. The target's alpha
    // is the OVERLAY's coverage, transparent wherever nothing was drawn, and a clipboard
    // bitmap of the map is wanted opaque.
    inline bool to_dib_row(const std::uint8_t* src, std::uint8_t* dst_bgra, int px)
    {
        if (src == nullptr || dst_bgra == nullptr || px <= 0)
        {
            return false;
        }
        for (int i = 0; i < px; ++i)
        {
            dst_bgra[i * 4 + 0] = src[i * 4 + 0];
            dst_bgra[i * 4 + 1] = src[i * 4 + 1];
            dst_bgra[i * 4 + 2] = src[i * 4 + 2];
            dst_bgra[i * 4 + 3] = 0xFFu;
        }
        return true;
    }

    //==================================================================================
    // The CF_DIB payload
    //==================================================================================
    //
    // A clipboard DIB is a BITMAPINFOHEADER immediately followed by the pixels - no
    // BITMAPFILEHEADER, which belongs only to a .bmp on disk.
    //
    // 32 bpp / BI_RGB, bottom-up with a positive height. Top-down (negative height) is
    // legal but mishandled by enough applications that rows are reversed on the way in.

    inline constexpr std::size_t kHeaderSize = 40; // sizeof(BITMAPINFOHEADER)

    inline void put_u16(std::uint8_t* p, std::uint16_t v)
    {
        p[0] = static_cast<std::uint8_t>(v & 0xFFu);
        p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    }

    inline void put_u32(std::uint8_t* p, std::uint32_t v)
    {
        p[0] = static_cast<std::uint8_t>(v & 0xFFu);
        p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    }

    inline std::uint32_t get_u32(const std::uint8_t* p)
    {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    inline std::int32_t get_i32(const std::uint8_t* p)
    {
        return static_cast<std::int32_t>(get_u32(p));
    }

    // `bgra` is `h` rows of `w` BGRA pixels, top-down, `src_pitch` bytes apart. A D3D12
    // readback footprint pitch is 256-aligned, so it is never simply w*4.
    inline bool build_dib(int w, int h, const std::uint8_t* bgra, std::size_t src_pitch,
                          std::vector<std::uint8_t>& out)
    {
        out.clear();
        if (w <= 0 || h <= 0 || bgra == nullptr)
        {
            return false;
        }
        const std::size_t row = static_cast<std::size_t>(w) * 4u;
        if (src_pitch < row)
        {
            return false;
        }
        // 4 bytes per pixel: every row is already DWORD-aligned, no padding to add.
        const std::size_t pixels = row * static_cast<std::size_t>(h);
        // ~500 MB, about an 11000x11000 map.
        if (pixels > (512u << 20))
        {
            return false;
        }
        out.resize(kHeaderSize + pixels);
        std::uint8_t* p = out.data();
        put_u32(p + 0, static_cast<std::uint32_t>(kHeaderSize)); // biSize
        put_u32(p + 4, static_cast<std::uint32_t>(w));           // biWidth
        put_u32(p + 8, static_cast<std::uint32_t>(h));           // biHeight (bottom-up)
        put_u16(p + 12, 1);                                      // biPlanes
        put_u16(p + 14, 32);                                     // biBitCount
        put_u32(p + 16, 0);                                      // biCompression = BI_RGB
        put_u32(p + 20, static_cast<std::uint32_t>(pixels));     // biSizeImage
        put_u32(p + 24, 0);                                      // biXPelsPerMeter
        put_u32(p + 28, 0);                                      // biYPelsPerMeter
        put_u32(p + 32, 0);                                      // biClrUsed
        put_u32(p + 36, 0);                                      // biClrImportant
        std::uint8_t* dst = out.data() + kHeaderSize;
        for (int y = 0; y < h; ++y)
        {
            // Bottom-up: DIB row 0 is the BOTTOM of the image.
            const std::uint8_t* srow = bgra + static_cast<std::size_t>(h - 1 - y) * src_pitch;
            std::memcpy(dst + static_cast<std::size_t>(y) * row, srow, row);
        }
        return true;
    }
} // namespace clipimg
