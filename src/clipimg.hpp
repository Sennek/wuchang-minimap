#pragma once

//
// clipimg - the PURE half of "copy the map to the clipboard": back-buffer pixel
// formats in, a CF_DIB payload out.
//
// WHY IT IS ITS OWN HEADER
// ------------------------
// The interesting part of a screenshot feature is not the D3D12 readback (which is
// ~40 lines of barrier + CopyTextureRegion) but the two things that are silently wrong
// if they are wrong: the pixel unpack, and the DIB layout. Wuchang's back buffer is
// `R10G10B10A2_UNORM` (HDR10 - lessons.md), not the `R8G8B8A8` every screenshot example
// assumes, so a naive `memcpy` produces a picture with the channels shifted and a
// two-bit alpha; and a DIB with the wrong row order or the wrong header size pastes
// upside down or not at all. Both are pure functions of bytes, so both are tested
// offline instead of by pasting into Paint after a play session.
//
// No Windows, no D3D12, no allocation beyond the output vector.
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace clipimg
{
    // Only the formats this game's swapchain can actually present. Anything else is
    // refused with a named reason rather than guessed at.
    enum class Fmt
    {
        Unknown = 0,
        R10G10B10A2,   // DXGI_FORMAT_R10G10B10A2_UNORM = 24  (Wuchang's real back buffer)
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

    // Every supported format is 4 bytes per pixel; kept as a function so a 16-bit
    // float back buffer (which would need tone mapping, not unpacking) cannot be added
    // by accident.
    inline int bytes_per_pixel(Fmt f)
    {
        return f == Fmt::Unknown ? 0 : 4;
    }

    // 10-bit channel -> 8-bit, rounded rather than shifted: `v >> 2` loses a quarter of
    // a level at the top and makes white 252, which is visible as a grey cast on a
    // screenshot of a white UI.
    inline std::uint8_t from10(std::uint32_t v)
    {
        return static_cast<std::uint8_t>((v * 255u + 511u) / 1023u);
    }

    // Unpacks `px` pixels into BGRA8 (the byte order a Windows 32bpp DIB wants: blue
    // first, then green, red, alpha). Alpha is forced OPAQUE: the back buffer's alpha
    // is whatever the game left there (2 bits of it, in the HDR10 case) and a
    // half-transparent screenshot is never what was asked for.
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
    // BITMAPFILEHEADER (that is only for a .bmp on disk, and including it is the classic
    // way to get a clipboard image that nothing can paste).
    //
    // 32 bpp / BI_RGB, and BOTTOM-UP with a positive height. A negative height (top-down)
    // is legal in the DIB format and is mishandled by enough applications that it is not
    // worth the saved memcpy; rows are reversed on the way in instead.

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

    // `bgra` is `h` rows of `w` BGRA pixels, TOP-DOWN, `src_pitch` bytes apart (a D3D12
    // readback footprint pitch is 256-aligned, so it is never simply w*4).
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
        // 4 bytes per pixel means every row is already DWORD-aligned, so there is no
        // padding to add - which is the only reason 32 bpp is worth the memory here.
        const std::size_t pixels = row * static_cast<std::size_t>(h);
        // ~500 MB would be a 11000x11000 map; refuse rather than try.
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
