#include "mapdata.hpp"

#include <Windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "json.hpp"
#include "mmstate.hpp"

namespace mapdata
{
    namespace
    {
        // The JSON reader lives in json.hpp: markers.cpp parses its own manifest with
        // the same parser, and the offline test target links it without UE4SS.
        using mjson::JParser;
        using mjson::JValue;

        //==============================================================================
        // State
        //==============================================================================

        std::atomic<bool> g_loaded{false};

        // Decoded images waiting for the render thread. A chapter now decodes ten
        // pictures (nine layers plus, optionally, the composite), so this is a queue
        // rather than a single slot. std::mutex is banned in this mod (lessons.md), so
        // the critical section - a push_back or a pop_front of a pointer - is guarded
        // by an atomic_flag spinlock and never does I/O.
        std::atomic_flag g_pending_lock = ATOMIC_FLAG_INIT;
        std::vector<PendingImage*> g_pending;

        void pending_lock()
        {
            for (int spin = 0; g_pending_lock.test_and_set(std::memory_order_acquire); ++spin)
            {
                if ((spin & 0x3F) == 0x3F)
                {
                    ::SwitchToThread();
                }
                else
                {
                    YieldProcessor();
                }
            }
        }

        void pending_unlock()
        {
            g_pending_lock.clear(std::memory_order_release);
        }

        void pending_push(std::unique_ptr<PendingImage> img)
        {
            pending_lock();
            g_pending.push_back(img.release());
            pending_unlock();
        }

        void pending_clear()
        {
            std::vector<PendingImage*> old;
            pending_lock();
            old.swap(g_pending);
            pending_unlock();
            for (PendingImage* p : old)
            {
                delete p;
            }
        }

        // Published as an immutable snapshot pointer: the loop thread builds a fresh
        // vector, then swaps the pointer in. The render thread can therefore read it
        // with no lock while an F5 reload is rebuilding it. The old vector is leaked on
        // purpose (a handful of bytes, bounded by the number of reloads) because a
        // reader may still be walking it - there is no safe point to free it and no
        // std::mutex allowed in this mod.
        std::atomic<const std::vector<Chapter>*> g_chapters{nullptr};

        void publish_chapters(std::vector<Chapter>&& list)
        {
            g_chapters.store(new std::vector<Chapter>(std::move(list)), std::memory_order_release);
        }

        bool read_whole_file(const std::wstring& path, std::string& out)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size) == 0 || size.QuadPart < 0 || size.QuadPart > (16 << 20))
            {
                ::CloseHandle(h);
                return false;
            }
            out.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            const bool ok = out.empty() || (::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 &&
                                            read == out.size());
            ::CloseHandle(h);
            return ok;
        }

        //==============================================================================
        // PNG -> RGBA8 via WIC
        //==============================================================================
        //
        // WIC rather than a vendored stb_image: it is part of Windows, needs no new
        // third-party code, and handles the 8-bit RGBA PNGs build_map.py writes
        // directly. This runs on the loop thread, so the ~90 MB decode never stalls
        // Present.

        // `channels`: 4 -> 32bpp RGBA (the Z-shaded composite), 2 -> 16bpp gray (one
        // height plane; WIC gives native-endian uint16 per pixel, i.e. little-endian
        // in memory on x64, which is what `HeightMaps::plane` wants), 1 -> 8bpp gray.
        bool decode_raw(const std::wstring& path, int channels, int& out_w, int& out_h,
                        std::vector<std::uint8_t>& out_pixels)
        {
            // The loop thread may or may not already have COM up; RPC_E_CHANGED_MODE
            // just means someone else picked the other apartment model, which is fine
            // for WIC. Deliberately never uninitialised - we do not own this thread.
            const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(co) && co != RPC_E_CHANGED_MODE)
            {
                mm::logf(L"maps: CoInitializeEx failed (0x{:08X})", static_cast<unsigned>(co));
                return false;
            }

            IWICImagingFactory* factory = nullptr;
            IWICBitmapDecoder* decoder = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* converter = nullptr;
            bool ok = false;

            HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr))
            {
                hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
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
            if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > 16384 || h > 16384))
            {
                mm::logf(L"maps: refusing a {}x{} image", w, h);
                hr = E_FAIL;
            }
            if (SUCCEEDED(hr))
            {
                hr = factory->CreateFormatConverter(&converter);
            }
            if (SUCCEEDED(hr))
            {
                // GUID_WICPixelFormat32bppRGBA, not BGRA: the ImGui DX12 backend's
                // sampler and our DXGI_FORMAT_R8G8B8A8_UNORM texture both want RGBA.
                const WICPixelFormatGUID want = channels == 1   ? GUID_WICPixelFormat8bppGray
                                                : channels == 2 ? GUID_WICPixelFormat16bppGray
                                                                : GUID_WICPixelFormat32bppRGBA;
                hr = converter->Initialize(frame,
                                           want,
                                           WICBitmapDitherTypeNone,
                                           nullptr,
                                           0.0,
                                           WICBitmapPaletteTypeCustom);
            }
            if (SUCCEEDED(hr))
            {
                const UINT stride = w * static_cast<UINT>(channels);
                out_w = static_cast<int>(w);
                out_h = static_cast<int>(h);
                out_pixels.resize(static_cast<std::size_t>(stride) * h);
                hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out_pixels.size()),
                                           out_pixels.data());
                ok = SUCCEEDED(hr);
            }
            if (!ok)
            {
                mm::logf(L"maps: PNG decode of {} failed (0x{:08X})", path, static_cast<unsigned>(hr));
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
            return ok;
        }

        bool decode_png(const std::wstring& path, PendingImage& out)
        {
            return decode_raw(path, out.channels, out.width, out.height, out.pixels);
        }

        std::wstring widen(std::string_view narrow)
        {
            return std::wstring{narrow.begin(), narrow.end()};
        }
    } // namespace

    void load(const std::wstring& mod_dir)
    {
        const std::wstring maps_dir = mod_dir + L"\\maps";
        const std::wstring manifest = maps_dir + L"\\maps.json";

        std::string text;
        if (!read_whole_file(manifest, text))
        {
            mm::logf(L"maps: {} not found - the overlay will run with no map background. Build it with "
                     L"tools/navmesh/build_map.py and re-run deploy.ps1.",
                     manifest);
            return;
        }

        JValue root{};
        if (!JParser{text}.parse(root) || root.kind != JValue::Kind::Object)
        {
            mm::logf(L"maps: {} is not valid JSON", manifest);
            return;
        }

        const JValue* chapters = root.find("chapters");
        if (chapters == nullptr || chapters->kind != JValue::Kind::Object || !chapters->obj)
        {
            mm::logf(L"maps: {} has no \"chapters\" object", manifest);
            return;
        }

        const bool want_composite = mm::config().fallback_use_composite;

        std::vector<Chapter> parsed;
        for (const auto& kv : *chapters->obj)
        {
            const JValue& c = kv.second;
            Chapter ch{};
            ch.key = kv.first;
            const JValue* image = c.find("image");
            ch.image = image != nullptr ? image->string_or("") : "";
            const auto n = [&c](const char* key, double fallback) {
                const JValue* v = c.find(key);
                return v != nullptr ? v->number_or(fallback) : fallback;
            };
            ch.image_width = static_cast<int>(n("image_width", 0.0));
            ch.image_height = static_cast<int>(n("image_height", 0.0));
            ch.min_x = n("min_x", 0.0);
            ch.min_y = n("min_y", 0.0);
            ch.max_x = n("max_x", 0.0);
            ch.max_y = n("max_y", 0.0);
            ch.px_per_uu = n("px_per_uu", 0.0);

            if (ch.image.empty() || ch.image_width <= 0 || ch.image_height <= 0 || ch.px_per_uu <= 0.0 ||
                ch.max_x <= ch.min_x || ch.max_y <= ch.min_y)
            {
                mm::logf(L"maps: chapter \"{}\" is incomplete - skipped", widen(ch.key));
                continue;
            }

            // ---- the multi-surface height map (schema wuchang-minimap-maps/3) ----
            //
            // `height_maps` is an array of PNG paths, lowest surface first. Everything
            // else about them is the chapter's own bounds / px_per_uu, so there is one
            // mapping for the whole asset - no per-layer crops and no per-layer scales
            // any more.
            auto hm = std::make_shared<HeightMaps>();
            hm->min_x = ch.min_x;
            hm->min_y = ch.min_y;
            hm->max_x = ch.max_x;
            hm->max_y = ch.max_y;
            hm->px_per_uu = n("px_per_uu", 0.0);
            hm->z_min = static_cast<float>(n("z_min", 0.0));
            hm->z_max = static_cast<float>(n("z_max", 0.0));
            const int declared = static_cast<int>(n("max_surfaces", static_cast<double>(kMaxSurfaces)));

            std::vector<std::string> height_files;
            const JValue* jh = c.find("height_maps");
            if (jh != nullptr && jh->kind == JValue::Kind::Array && jh->arr)
            {
                for (const JValue& e : *jh->arr)
                {
                    // Either a bare filename or {"image": "..."}; accept both so a
                    // manifest tweak cannot silently produce a mapless overlay.
                    std::string rel = e.string_or("");
                    if (rel.empty())
                    {
                        const JValue* img2 = e.find("image");
                        rel = img2 != nullptr ? img2->string_or("") : "";
                    }
                    if (!rel.empty() && height_files.size() < static_cast<std::size_t>(kMaxSurfaces))
                    {
                        height_files.push_back(rel);
                    }
                }
            }
            if (height_files.empty())
            {
                // Fallback for a manifest written before the key existed: the shipped
                // naming is <chapter>/small_z<k>.png next to the composite.
                std::string stem = ch.image;
                const std::size_t dot = stem.rfind('.');
                if (dot != std::string::npos)
                {
                    stem = stem.substr(0, dot);
                }
                const int want = declared > 0 && declared <= kMaxSurfaces ? declared : kMaxSurfaces;
                for (int k = 0; k < want; ++k)
                {
                    height_files.push_back(stem + "_z" + std::to_string(k) + ".png");
                }
                mm::logf(L"maps: chapter \"{}\" has no \"height_maps\" key - guessing {} file(s) "
                         L"from the composite name",
                         widen(ch.key),
                         height_files.size());
            }
            ch.height_files = height_files;
            ch.heights = hm;

            mm::logf(L"maps: chapter \"{}\" {} {}x{} px @ {:.4f} px/uu, world X {:.0f}..{:.0f} "
                     L"Y {:.0f}..{:.0f}, Z {:.0f}..{:.0f}, {} height plane(s)",
                     widen(ch.key),
                     widen(ch.image),
                     ch.image_width,
                     ch.image_height,
                     ch.px_per_uu,
                     ch.min_x,
                     ch.max_x,
                     ch.min_y,
                     ch.max_y,
                     static_cast<double>(hm->z_min),
                     static_cast<double>(hm->z_max),
                     height_files.size());
            parsed.push_back(std::move(ch));
        }

        if (parsed.empty())
        {
            mm::log(L"maps: no usable chapter in the manifest");
            return;
        }

        // MVP: one chapter's asset at a time - the first one in the manifest.
        Chapter& first = parsed.front();
        const auto png_path = [&maps_dir](const std::string& rel) {
            std::wstring p = maps_dir + L"\\" + widen(rel);
            std::replace(p.begin(), p.end(), L'/', L'\\');
            return p;
        };

        pending_clear(); // an F5 reload must not upload the previous set

        // ---- the height planes: CPU-side source data for the slicer ---------------
        //
        // These are NOT GPU textures. The render thread slices a window out of them
        // into a small dynamic texture every ~80 ms, so they live in ordinary RAM and
        // cost no VRAM at all (the previous ordinal-layer scheme cost 109 MB of it).
        std::size_t planes_ok = 0;
        if (first.heights)
        {
            // `heights` is shared as const once published, so fill it while we are
            // still the only owner.
            auto* hm = const_cast<HeightMaps*>(first.heights.get());
            for (std::size_t k = 0; k < first.height_files.size(); ++k)
            {
                int w = 0;
                int h = 0;
                std::vector<std::uint8_t> raw;
                if (!decode_raw(png_path(first.height_files[k]), 2, w, h, raw))
                {
                    break; // a gap in the planes would misorder the surfaces
                }
                if (hm->width == 0)
                {
                    hm->width = w;
                    hm->height = h;
                }
                else if (w != hm->width || h != hm->height)
                {
                    mm::logf(L"maps: height plane z{} is {}x{} but z0 is {}x{} - stopping here",
                             k,
                             w,
                             h,
                             hm->width,
                             hm->height);
                    break;
                }
                const std::size_t n_px = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
                hm->plane[k].resize(n_px);
                std::memcpy(hm->plane[k].data(), raw.data(), n_px * sizeof(std::uint16_t));
                hm->count = static_cast<int>(k + 1);
                ++planes_ok;
            }
            if (hm->z_max <= hm->z_min)
            {
                mm::logf(L"maps: chapter \"{}\" has no usable z_min/z_max in maps.json "
                         L"({:.0f}..{:.0f}) - the height maps are unusable",
                         widen(first.key),
                         static_cast<double>(hm->z_min),
                         static_cast<double>(hm->z_max));
                hm->count = 0;
                planes_ok = 0;
            }
            if (planes_ok > 0)
            {
                // SANITY CHECK ON THE BYTE ORDER. PNG stores 16-bit samples
                // big-endian; WIC's 16bppGray converter hands them back in native
                // (little-endian) order, but a decoder that did not would produce codes
                // that are byte-swapped garbage - and the only symptom would be a map
                // that looks like noise. So decode plane 0's actual Z range and log it:
                // it must land inside [z_min, z_max] and be broad. A swapped buffer
                // shows up immediately as a range that fills the whole span with a
                // nonsense distribution.
                float lo = hm->z_max;
                float hi = hm->z_min;
                std::size_t lit = 0;
                for (std::uint16_t code : hm->plane[0])
                {
                    if (code == 0)
                    {
                        continue;
                    }
                    const float z = hm->decode(code);
                    lo = z < lo ? z : lo;
                    hi = z > hi ? z : hi;
                    ++lit;
                }
                mm::logf(L"maps: height plane z0 has {} lit pixel(s) ({}%), decoded Z {:.0f}..{:.0f} "
                         L"(manifest says {:.0f}..{:.0f}) - a byte-swapped decode would not fit this",
                         lit,
                         hm->plane[0].empty() ? 0 : (lit * 100) / hm->plane[0].size(),
                         static_cast<double>(lo),
                         static_cast<double>(hi),
                         static_cast<double>(hm->z_min),
                         static_cast<double>(hm->z_max));

                mm::logf(L"maps: decoded {}/{} height plane(s) of \"{}\" ({}x{}, Z {:.0f}..{:.0f} "
                         L"in steps of {:.1f} uu), {} MB of RAM, 0 MB of VRAM",
                         planes_ok,
                         first.height_files.size(),
                         widen(first.key),
                         hm->width,
                         hm->height,
                         static_cast<double>(hm->z_min),
                         static_cast<double>(hm->z_max),
                         static_cast<double>(hm->z_step()),
                         hm->bytes() / (1024 * 1024));
            }
            else
            {
                mm::logf(L"maps: NO height plane decoded for \"{}\" - build them with "
                         L"tools/navmesh/build_map.py and re-run deploy.ps1",
                         widen(first.key));
            }
        }

        // ---- the composite, only as the no-height-map fallback --------------------
        if (want_composite || planes_ok == 0)
        {
            auto img = std::make_unique<PendingImage>();
            img->chapter_key = first.key;
            img->channels = 4;
            if (decode_png(png_path(first.image), *img))
            {
                if (img->width != first.image_width || img->height != first.image_height)
                {
                    mm::logf(L"maps: {} is {}x{} but maps.json says {}x{} - trusting the PNG",
                             widen(first.image),
                             img->width,
                             img->height,
                             first.image_width,
                             first.image_height);
                    first.image_width = img->width;
                    first.image_height = img->height;
                }
                mm::logf(L"maps: decoded the composite {} -> {}x{} RGBA ({} MB){}",
                         widen(first.image),
                         img->width,
                         img->height,
                         img->pixels.size() / (1024 * 1024),
                         planes_ok == 0 ? L" - the only thing there is to draw" : L"");
                pending_push(std::move(img));
            }
        }
        else
        {
            mm::log(L"maps: composite texture not loaded (fallback_use_composite = 0); the height "
                    L"slicer does not need it");
        }

        publish_chapters(std::move(parsed));
        g_loaded = true;
    }

    std::unique_ptr<PendingImage> take_pending()
    {
        PendingImage* out = nullptr;
        pending_lock();
        if (!g_pending.empty())
        {
            out = g_pending.front();
            g_pending.erase(g_pending.begin());
        }
        pending_unlock();
        return std::unique_ptr<PendingImage>{out};
    }

    std::vector<Chapter> chapters()
    {
        const std::vector<Chapter>* list = g_chapters.load(std::memory_order_acquire);
        return list == nullptr ? std::vector<Chapter>{} : *list;
    }

    Chapter chapter_for(double wx, double wy)
    {
        const Chapter* ch = chapter_ptr_for(wx, wy);
        return ch == nullptr ? Chapter{} : *ch;
    }

    const Chapter* chapter_ptr_for(double wx, double wy)
    {
        // Safe to hand out a pointer: the published vector is never freed (see
        // publish_chapters), precisely so a reader can keep using it across a reload.
        const std::vector<Chapter>* list = g_chapters.load(std::memory_order_acquire);
        if (list == nullptr)
        {
            return nullptr;
        }
        for (const Chapter& ch : *list)
        {
            if (ch.contains(wx, wy))
            {
                return &ch;
            }
        }
        return nullptr;
    }

    bool loaded()
    {
        return g_loaded.load(std::memory_order_acquire);
    }
} // namespace mapdata
