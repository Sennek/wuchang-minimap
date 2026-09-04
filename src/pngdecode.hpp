#pragma once

//
// pngdecode - the WIC PNG decode the map assets go through. No UE4SS, no mod log, no
// residency state machine, so tests decode a shipped PNG through exactly this code.
//
// A schema-/4 composite is PNG colour type 3 (8-bit indexed) with a tRNS array, which
// WIC presents as GUID_WICPixelFormat8bppIndexed with per-index alpha in the frame's
// palette; IWICFormatConverter::Initialize(frame, 32bppRGBA) expands it. So the caller
// asks for RGBA and gets RGBA whether the file is RGBA8 or indexed.
//
// Runs on the UE4SS event-loop thread, never inside Present.
//

#include <Windows.h>

#include <wincodec.h>

#include <cstdint>
#include <vector>

namespace pngdec
{
    // Bytes per pixel wanted back, which also picks the WIC target format:
    //   4 -> GUID_WICPixelFormat32bppRGBA  (the Z-shaded composite; RGBA, since the
    //        ImGui DX12 backend and the R8G8B8A8_UNORM texture both want RGBA)
    //   2 -> GUID_WICPixelFormat16bppGray  (one height plane; WIC returns 16-bit
    //        samples in native little-endian order, which is what the height codes
    //        want. PNG itself stores them big-endian.)
    //   1 -> GUID_WICPixelFormat8bppGray
    constexpr int kRgba = 4;
    constexpr int kGray16 = 2;
    constexpr int kGray8 = 1;

    // Refused before allocating. The biggest shipped asset is 3029x7342 and a D3D12
    // texture cannot exceed 16384.
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
    // tightly packed). Never throws; the result's HRESULT says what went wrong.
    //
    // COM: RPC_E_CHANGED_MODE means the thread already picked the other apartment
    // model, which is fine for WIC. Never uninitialised - the mod does not own this
    // thread.
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
            // WICBitmapPaletteTypeCustom + null palette: that argument is for a
            // DESTINATION palette (RGB -> indexed). Going the other way, the converter
            // uses the source frame's own palette.
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
