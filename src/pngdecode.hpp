#pragma once

//
// pngdecode - the WIC PNG decode the map assets go through, on its own.
//
// Split out of mapdata.cpp for the same reason mapmanifest.hpp was: it is the half of
// the loader that has no dependency on UE4SS, on the mod's log or on the residency
// state machine, so tests/markers_test.cpp can decode a SHIPPED PNG through exactly
// this code on the build machine. An in-game session is the scarce resource; "does
// the asset we are about to ship decode to the numbers we think it does" must not
// need one.
//
// WIC rather than a vendored stb_image: it is part of Windows, needs no new
// third-party code, and it is what makes the palette composite free. A schema-/4
// composite is PNG colour type 3 (8-bit indexed) with a tRNS ARRAY, which WIC's PNG
// decoder presents as GUID_WICPixelFormat8bppIndexed with the frame's palette
// carrying the per-index alpha; IWICFormatConverter::Initialize(frame, 32bppRGBA)
// then expands it using that palette. So the caller asks for RGBA and gets RGBA
// whether the file on disk is RGBA8 or indexed - one palette lookup per pixel more
// than before, on the loop thread, once per chapter load.
//
// This runs on the UE4SS event-loop thread, never inside Present.
//

#include <Windows.h>

#include <wincodec.h>

#include <cstdint>
#include <vector>

namespace pngdec
{
    // The number of BYTES per pixel the caller wants back, which also picks the WIC
    // target format:
    //   4 -> GUID_WICPixelFormat32bppRGBA  (the Z-shaded composite; RGBA and not
    //        BGRA because the ImGui DX12 backend's sampler and our
    //        DXGI_FORMAT_R8G8B8A8_UNORM texture both want RGBA)
    //   2 -> GUID_WICPixelFormat16bppGray  (one height plane; WIC hands 16-bit
    //        samples back in native - little-endian - order, which is what the
    //        height codes want. PNG itself stores them big-endian.)
    //   1 -> GUID_WICPixelFormat8bppGray
    constexpr int kRgba = 4;
    constexpr int kGray16 = 2;
    constexpr int kGray8 = 1;

    // Refuse anything absurd before allocating for it. The biggest shipped asset is
    // 3029x7342; a D3D12 texture cannot exceed 16384 either.
    constexpr UINT kMaxDim = 16384;

    struct Result
    {
        HRESULT hr = E_FAIL;
        int width = 0;
        int height = 0;
        bool ok() const
        {
            return SUCCEEDED(hr) && width > 0 && height > 0;
        }
    };

    // Decodes `path` into `out_pixels` (width * height * channels bytes, top-down,
    // tightly packed). Never throws; the HRESULT in the result says what went wrong
    // so the caller can log it in its own voice.
    //
    // COM: the loop thread may or may not already have an apartment, and
    // RPC_E_CHANGED_MODE just means someone else picked the other model - which is
    // fine for WIC. Deliberately never uninitialised: we do not own this thread.
    inline Result decode(const wchar_t* path, int channels, std::vector<std::uint8_t>& out_pixels)
    {
        Result r{};
        if (path == nullptr || (channels != kRgba && channels != kGray16 && channels != kGray8))
        {
            r.hr = E_INVALIDARG;
            return r;
        }

        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(co) && co != RPC_E_CHANGED_MODE)
        {
            r.hr = co;
            return r;
        }

        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;

        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr))
        {
            hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
        }
        if (SUCCEEDED(hr))
        {
            hr = decoder->GetFrame(0, &frame);
        }
        UINT w = 0;
        UINT h = 0;
        if (SUCCEEDED(hr))
        {
            hr = frame->GetSize(&w, &h);
        }
        if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim))
        {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr))
        {
            hr = factory->CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(hr))
        {
            const WICPixelFormatGUID want = channels == kGray8    ? GUID_WICPixelFormat8bppGray
                                            : channels == kGray16 ? GUID_WICPixelFormat16bppGray
                                                                  : GUID_WICPixelFormat32bppRGBA;
            // WICBitmapPaletteTypeCustom + a null palette: the palette argument is
            // for the DESTINATION (RGB -> indexed), and we only ever go the other
            // way, where the converter uses the SOURCE frame's own palette.
            hr = converter->Initialize(frame, want, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr))
        {
            const UINT stride = w * static_cast<UINT>(channels);
            out_pixels.resize(static_cast<std::size_t>(stride) * h);
            hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out_pixels.size()),
                                       out_pixels.data());
            if (SUCCEEDED(hr))
            {
                r.width = static_cast<int>(w);
                r.height = static_cast<int>(h);
            }
        }

        if (converter != nullptr)
        {
            converter->Release();
        }
        if (frame != nullptr)
        {
            frame->Release();
        }
        if (decoder != nullptr)
        {
            decoder->Release();
        }
        if (factory != nullptr)
        {
            factory->Release();
        }
        r.hr = hr;
        return r;
    }
} // namespace pngdec
