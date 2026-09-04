#pragma once

//
// clipimg - the pure half of "copy the map to the clipboard": back-buffer pixel
// formats in, a CF_DIB payload out.
//
// Wuchang's back buffer is `R10G10B10A2_UNORM` (HDR10), not the `R8G8B8A8` every
// screenshot example assumes.
//
// No Windows, no D3D12, no allocation beyond the output vector.
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace clipimg
{
    // The formats this game's swapchain can present. Anything else is refused.
    enum class Fmt
    {
        Unknown = 0,
        R10G10B10A2,   // DXGI_FORMAT_R10G10B10A2_UNORM = 24  (Wuchang's back buffer)
        R8G8B8A8,      // 28 (_UNORM) / 29 (_UNORM_SRGB)
        B8G8R8A8,      // 87 (_UNORM) / 91 (_UNORM_SRGB)
    };

    inline Fmt fmt_from_dxgi(unsigned dxgi_format)
    {
        switch (dxgi_format)
        {
        case 24: // DXGI_FORMAT_R10G10B10A2_UNORM
            return Fmt::R10G10B10A2;
        case 28: // DXGI_FORMAT_R8G8B8A8_UNORM
        case 29: // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            return Fmt::R8G8B8A8;
        case 87: // DXGI_FORMAT_B8G8R8A8_UNORM
        case 91: // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            return Fmt::B8G8R8A8;
        default:
            return Fmt::Unknown;
        }
    }

    inline const char* fmt_name(Fmt f)
    {
        switch (f)
        {
        case Fmt::R10G10B10A2:
            return "R10G10B10A2_UNORM";
        case Fmt::R8G8B8A8:
            return "R8G8B8A8";
        case Fmt::B8G8R8A8:
            return "B8G8R8A8";
        case Fmt::Unknown:
        default:
            return "unsupported";
        }
    }

    // Every supported format is 4 bytes per pixel. A function, so a 16-bit float back
    // buffer (which needs tone mapping, not unpacking) cannot slip in.
    inline int bytes_per_pixel(Fmt f)
    {
        return f == Fmt::Unknown ? 0 : 4;
    }

    // 10-bit channel -> 8-bit, rounded not shifted: `v >> 2` makes white 252, a visible
    // grey cast.
    inline std::uint8_t from10(std::uint32_t v)
    {
        return static_cast<std::uint8_t>((v * 255u + 511u) / 1023u);
    }

    // Unpacks `px` pixels into BGRA8, the byte order a Windows 32bpp DIB wants. Alpha is
    // forced opaque; the back buffer's alpha is whatever the game left there.
    inline bool unpack_row(Fmt f, const std::uint8_t* src, std::uint8_t* dst_bgra, int px)
    {
        if (src == nullptr || dst_bgra == nullptr || px <= 0)
        {
            return false;
        }
        switch (f)
        {
        case Fmt::R10G10B10A2:
            for (int i = 0; i < px; ++i)
            {
                std::uint32_t v = 0;
                std::memcpy(&v, src + static_cast<std::size_t>(i) * 4u, 4);
                // Little-endian packing: R in bits 0-9, G in 10-19, B in 20-29, A in 30-31.
                dst_bgra[i * 4 + 0] = from10((v >> 20) & 0x3FFu); // B
                dst_bgra[i * 4 + 1] = from10((v >> 10) & 0x3FFu); // G
                dst_bgra[i * 4 + 2] = from10(v & 0x3FFu);         // R
                dst_bgra[i * 4 + 3] = 0xFFu;
            }
            return true;
        case Fmt::R8G8B8A8:
            for (int i = 0; i < px; ++i)
            {
                dst_bgra[i * 4 + 0] = src[i * 4 + 2];
                dst_bgra[i * 4 + 1] = src[i * 4 + 1];
                dst_bgra[i * 4 + 2] = src[i * 4 + 0];
                dst_bgra[i * 4 + 3] = 0xFFu;
            }
            return true;
        case Fmt::B8G8R8A8:
            for (int i = 0; i < px; ++i)
            {
                dst_bgra[i * 4 + 0] = src[i * 4 + 0];
                dst_bgra[i * 4 + 1] = src[i * 4 + 1];
                dst_bgra[i * 4 + 2] = src[i * 4 + 2];
                dst_bgra[i * 4 + 3] = 0xFFu;
            }
            return true;
        case Fmt::Unknown:
        default:
            return false;
        }
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
