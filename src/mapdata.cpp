#include "mapdata.hpp"

#include <Windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "mapmanifest.hpp"
#include "mmstate.hpp"

namespace mapdata
{
    namespace
    {
        // The manifest parse itself lives in mapmanifest.hpp - pure, header-only and
        // exercised by tests/markers_test.cpp on the build machine. Everything left in
        // this file is what genuinely needs Windows: the file read, the WIC decode and
        // the residency state machine.

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

        //==============================================================================
        // ONE CHAPTER AT A TIME
        //==============================================================================
        //
        // All of this is loop-thread state except the two atomics. The published
        // chapter list is never freed (see publish_chapters), so a render thread that
        // is holding a `const Chapter*` keeps a valid object across a reload; what it
        // may find inside it is a null `heights`, which every caller already tests for.

        // Parsed manifest, kept in step (index for index) with the published chapter
        // vector. It carries the per-chapter z_min / z_max and file list that decoding
        // needs, so `Chapter` does not have to grow fields the render side never reads.
        mapmanifest::Manifest g_manifest{};
        std::wstring g_maps_dir;

        // The published vector, non-const, for the one thread allowed to mutate the
        // `heights` pointers.
        std::vector<Chapter>* g_chapters_mut = nullptr;

        // Index of the resident chapter, published for chapter_ptr_for(). -1 = none.
        std::atomic<int> g_active{-1};

        // Written by the game thread (gamestate), read by the loop thread.
        std::atomic<int> g_detected{chid::kNone};

        int g_pending_chapter = -1;          // index whose planes still have to be decoded
        int g_last_logged = chid::kNone - 1; // so the first detection always logs

        // Planes whose pointer has been cleared but which may still be under a render
        // thread's eyes. Freed once kRetireGraceMs has passed - and always BEFORE the
        // incoming chapter is decoded, so the peak is one chapter, not two.
        HeightMaps* g_retired = nullptr;
        std::uint64_t g_retire_at = 0;

        std::wstring png_path(const std::string& rel)
        {
            std::wstring path = g_maps_dir + L"\\" + widen(rel);
            std::replace(path.begin(), path.end(), L'/', L'\\');
            return path;
        }

        // Decodes one chapter's 16-bit height planes. Returns nullptr when nothing
        // usable came back; it logs why.
        HeightMaps* decode_heights(const Chapter& ch, const mapmanifest::Entry& e)
        {
            auto hm = std::make_unique<HeightMaps>();
            hm->min_x = ch.min_x;
            hm->min_y = ch.min_y;
            hm->max_x = ch.max_x;
            hm->max_y = ch.max_y;
            hm->px_per_uu = ch.px_per_uu;
            hm->z_min = static_cast<float>(e.z_min);
            hm->z_max = static_cast<float>(e.z_max);

            if (hm->z_max <= hm->z_min)
            {
                mm::logf(L"maps: chapter \"{}\" has no usable z_min/z_max in maps.json "
                         L"({:.0f}..{:.0f}) - the height maps are unusable",
                         widen(ch.key),
                         static_cast<double>(hm->z_min),
                         static_cast<double>(hm->z_max));
                return nullptr;
            }

            for (std::size_t k = 0; k < ch.height_files.size() && k < kMaxSurfaces; ++k)
            {
                int w = 0;
                int h = 0;
                std::vector<std::uint8_t> raw;
                if (!decode_raw(png_path(ch.height_files[k]), 2, w, h, raw))
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
            }

            if (hm->count == 0)
            {
                mm::logf(L"maps: NO height plane decoded for \"{}\" - build them with "
                         L"tools/navmesh/build_map.py and re-run deploy.ps1",
                         widen(ch.key));
                return nullptr;
            }

            // SANITY CHECK ON THE BYTE ORDER. PNG stores 16-bit samples big-endian;
            // WIC's 16bppGray converter hands them back in native (little-endian)
            // order, but a decoder that did not would produce codes that are
            // byte-swapped garbage - and the only symptom would be a map that looks
            // like noise. So decode plane 0's actual Z range and log it: it must land
            // inside [z_min, z_max] and be broad. A swapped buffer shows up immediately
            // as a range that fills the whole span with a nonsense distribution.
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
                     hm->count,
                     ch.height_files.size(),
                     widen(ch.key),
                     hm->width,
                     hm->height,
                     static_cast<double>(hm->z_min),
                     static_cast<double>(hm->z_max),
                     static_cast<double>(hm->z_step()),
                     hm->bytes() / (1024 * 1024));
            return hm.release();
        }

        // Clears the active chapter's `heights` pointer and parks the planes for a
        // delayed free. Loop thread.
        void retire_active(std::uint64_t now)
        {
            const int active = g_active.exchange(-1, std::memory_order_release);
            if (g_chapters_mut == nullptr || active < 0 ||
                active >= static_cast<int>(g_chapters_mut->size()))
            {
                return;
            }
            Chapter& ch = (*g_chapters_mut)[static_cast<std::size_t>(active)];
            const HeightMaps* planes = ch.heights;
            ch.heights = nullptr;
            if (planes == nullptr)
            {
                return;
            }
            // Only one retirement can ever be in flight, because a switch never starts
            // while one is pending. Free defensively in case that changes.
            delete g_retired;
            g_retired = const_cast<HeightMaps*>(planes);
            // `map_asset_retire_grace_ms` (default kRetireGraceMs), read here rather
            // than baked in: it is the one number that decides whether a render thread
            // still inside a slice can be handed freed memory.
            const std::uint64_t grace = static_cast<std::uint64_t>(mm::config().map_asset_retire_grace_ms);
            g_retire_at = now + grace;
            mm::logf(L"maps: chapter \"{}\" unloaded ({} MB freed in {} ms)",
                     widen(ch.key),
                     g_retired->bytes() / (1024 * 1024),
                     grace);
        }
    } // namespace

    void load(const std::wstring& mod_dir)
    {
        g_maps_dir = mod_dir + L"\\maps";
        const std::wstring manifest_path = g_maps_dir + L"\\maps.json";

        std::string text;
        if (!read_whole_file(manifest_path, text))
        {
            mm::logf(L"maps: {} not found - the overlay will run with no map background. Build it with "
                     L"tools/navmesh/build_map.py and re-run deploy.ps1.",
                     manifest_path);
            return;
        }

        mapmanifest::Manifest parsed_manifest{};
        std::vector<std::string> problems;
        const bool ok = mapmanifest::parse(text, parsed_manifest, problems);
        for (const std::string& problem : problems)
        {
            mm::logf(L"maps: {}", widen(problem));
        }
        if (!ok)
        {
            return;
        }
        if (parsed_manifest.schema != mapmanifest::kSchema)
        {
            // Not fatal: every field this build reads was already in schema /3 and a
            // newer writer is expected to stay additive. Say so once, loudly.
            mm::logf(L"maps: manifest schema is \"{}\", this build was written for \"{}\" - "
                     L"reading it anyway",
                     widen(parsed_manifest.schema),
                     widen(mapmanifest::kSchema));
        }
        if (parsed_manifest.chapters.empty())
        {
            mm::log(L"maps: no usable chapter in the manifest");
            return;
        }

        // An F5 reload publishes a fresh chapter list; the planes hanging off the old
        // one have to be retired here or they are leaked with it (327 MB a press).
        const std::uint64_t now = ::GetTickCount64();
        retire_active(now);
        pending_clear(); // an F5 reload must not upload the previous composite

        std::vector<Chapter> parsed;
        parsed.reserve(parsed_manifest.chapters.size());
        for (const mapmanifest::Entry& e : parsed_manifest.chapters)
        {
            Chapter ch{};
            ch.key = e.key;
            ch.image = e.image;
            ch.chapter = e.chapter;
            ch.image_width = e.image_width;
            ch.image_height = e.image_height;
            ch.min_x = e.min_x;
            ch.min_y = e.min_y;
            ch.max_x = e.max_x;
            ch.max_y = e.max_y;
            ch.px_per_uu = e.px_per_uu;
            ch.height_files = e.height_maps;
            if (e.height_maps_guessed)
            {
                mm::logf(L"maps: chapter \"{}\" has no \"height_maps\" key - guessing {} file(s) "
                         L"from the composite name",
                         widen(ch.key),
                         ch.height_files.size());
            }
            const std::size_t resident_mb =
                (static_cast<std::size_t>(e.image_width) * static_cast<std::size_t>(e.image_height) * 2u *
                 ch.height_files.size()) /
                (1024u * 1024u);
            mm::logf(L"maps: chapter \"{}\" (chapter {}) {} {}x{} px @ {:.4f} px/uu, world X {:.0f}..{:.0f} "
                     L"Y {:.0f}..{:.0f}, Z {:.0f}..{:.0f}, {} height plane(s), {} MB when resident",
                     widen(ch.key),
                     ch.chapter,
                     widen(ch.image),
                     ch.image_width,
                     ch.image_height,
                     ch.px_per_uu,
                     ch.min_x,
                     ch.max_x,
                     ch.min_y,
                     ch.max_y,
                     e.z_min,
                     e.z_max,
                     ch.height_files.size(),
                     resident_mb);
            parsed.push_back(std::move(ch));
        }

        g_manifest = std::move(parsed_manifest);
        publish_chapters(std::move(parsed));
        g_chapters_mut = const_cast<std::vector<Chapter>*>(g_chapters.load(std::memory_order_acquire));
        g_loaded = true;

        // Start on the chapter the detection has already named, if it has; otherwise on
        // the lowest-numbered one, which reproduces the single-chapter build's start-up
        // (chapter 1 resident at the main menu, so the slicer self-test has an asset).
        const int detected = g_detected.load(std::memory_order_relaxed);
        const int want = detected == chid::kNone ? -1 : g_manifest.index_of_number(detected);
        g_pending_chapter = want >= 0 ? want : g_manifest.default_index();
        mm::logf(L"maps: {} chapter(s) in the manifest, ONE resident at a time; starting with \"{}\"",
                 g_manifest.chapters.size(),
                 g_pending_chapter >= 0
                     ? widen(g_manifest.chapters[static_cast<std::size_t>(g_pending_chapter)].key)
                     : std::wstring{L"(none)"});

        // Decode it now rather than on the next tick, so a cold start (and F5) behaves
        // exactly as the single-chapter build did: when load() returns, the map is there.
        on_update();
    }

    void unload()
    {
        // LOOP THREAD, master switch only, and only AFTER overlay::stop_complete():
        // with the render side torn down nobody can be inside a height slice, so the
        // planes are freed here and now rather than through the kRetireGraceMs path.
        const std::uint64_t now = ::GetTickCount64();
        retire_active(now);
        delete g_retired;
        g_retired = nullptr;
        g_retire_at = 0;
        g_pending_chapter = -1;
        pending_clear();
        // Nothing is detected while the mod is off, and the next enable must log the
        // chapter again rather than assume the player never moved.
        g_detected.store(chid::kNone, std::memory_order_relaxed);
        g_last_logged = chid::kNone - 1;
    }

    void set_detected_chapter(int chapter)
    {
        g_detected.store(chapter, std::memory_order_relaxed);
    }

    int detected_chapter()
    {
        return g_detected.load(std::memory_order_relaxed);
    }

    std::string active_chapter_key()
    {
        const std::vector<Chapter>* list = g_chapters.load(std::memory_order_acquire);
        const int active = g_active.load(std::memory_order_acquire);
        if (list == nullptr || active < 0 || active >= static_cast<int>(list->size()))
        {
            return {};
        }
        return (*list)[static_cast<std::size_t>(active)].key;
    }

    void on_update()
    {
        if (g_chapters_mut == nullptr)
        {
            return;
        }
        const std::uint64_t now = ::GetTickCount64();

        // ---- 1. free a retired chapter, once no render thread can still be in it ----
        if (g_retired != nullptr)
        {
            if (now < g_retire_at)
            {
                return; // never decode the incoming chapter while the old one is alive
            }
            delete g_retired;
            g_retired = nullptr;
        }

        // ---- 2. act on the detection ------------------------------------------------
        const int detected = g_detected.load(std::memory_order_relaxed);
        if (detected != g_last_logged)
        {
            g_last_logged = detected;
            mm::logf(L"maps: the player is in chapter {}",
                     detected == chid::kNone  ? std::wstring{L"?"}
                     : detected == chid::kDlc ? std::wstring{L"DLC"}
                                              : std::to_wstring(detected));
        }
        if (detected != chid::kNone && g_pending_chapter < 0)
        {
            const int want = g_manifest.index_of_number(detected);
            const int active = g_active.load(std::memory_order_acquire);
            if (want != active)
            {
                // want < 0 means "this chapter has no map asset" - the DLC, whose
                // navmesh the paks do not carry at all. Unloading is the RIGHT answer
                // there: the chapters' world bounds overlap, so keeping the old one
                // resident would draw chapter 3's geometry under a DLC player.
                const std::wstring from =
                    active >= 0 ? widen((*g_chapters_mut)[static_cast<std::size_t>(active)].key)
                                : std::wstring{L"(none)"};
                const std::wstring to =
                    want >= 0 ? widen(g_manifest.chapters[static_cast<std::size_t>(want)].key)
                              : std::wstring{L"(no map asset for this chapter)"};
                mm::logf(L"maps: chapter switch {} -> {}", from, to);
                retire_active(now);
                g_pending_chapter = want;
                return;
            }
        }

        // ---- 3. decode the incoming chapter -----------------------------------------
        if (g_pending_chapter < 0 || g_pending_chapter >= static_cast<int>(g_chapters_mut->size()))
        {
            g_pending_chapter = -1;
            return;
        }
        const int index = g_pending_chapter;
        g_pending_chapter = -1;
        Chapter& ch = (*g_chapters_mut)[static_cast<std::size_t>(index)];
        const mapmanifest::Entry& entry = g_manifest.chapters[static_cast<std::size_t>(index)];
        HeightMaps* planes = decode_heights(ch, entry);
        ch.heights = planes;
        g_active.store(index, std::memory_order_release);

        // The composite is only the no-height-map fallback and it is off by default
        // (`fallback_use_composite = 0`). Note it IS re-decoded on a chapter switch, but
        // the upload still happens inside overlay.cpp's own frame path - this side only
        // queues the pixels.
        if (mm::config().fallback_use_composite || planes == nullptr)
        {
            auto img = std::make_unique<PendingImage>();
            img->chapter_key = ch.key;
            img->channels = 4;
            if (decode_png(png_path(ch.image), *img))
            {
                if (img->width != ch.image_width || img->height != ch.image_height)
                {
                    mm::logf(L"maps: {} is {}x{} but maps.json says {}x{} - trusting the PNG",
                             widen(ch.image),
                             img->width,
                             img->height,
                             ch.image_width,
                             ch.image_height);
                    ch.image_width = img->width;
                    ch.image_height = img->height;
                }
                mm::logf(L"maps: decoded the composite {} -> {}x{} RGBA ({} MB){}",
                         widen(ch.image),
                         img->width,
                         img->height,
                         img->pixels.size() / (1024 * 1024),
                         planes == nullptr ? L" - the only thing there is to draw" : L"");
                pending_push(std::move(img));
            }
        }
        else
        {
            mm::log(L"maps: composite texture not loaded (fallback_use_composite = 0); the height "
                    L"slicer does not need it");
        }
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

        // A chapter is resident: it is the only possible answer. The five chapters'
        // world bounds overlap (chapter 4 covers nearly all of chapter 1), so a bounds
        // scan here would hand back a different chapter's map the moment the player
        // stepped into an overlap - which is the bug this whole mechanism exists to
        // remove.
        const int active = g_active.load(std::memory_order_acquire);
        if (active >= 0 && active < static_cast<int>(list->size()))
        {
            const Chapter& ch = (*list)[static_cast<std::size_t>(active)];
            return ch.contains(wx, wy) ? &ch : nullptr;
        }

        // Nothing resident yet (main menu, or the detection has not landed): the old
        // behaviour, preferring a chapter that actually has planes decoded.
        for (const Chapter& ch : *list)
        {
            if (ch.has_heights() && ch.contains(wx, wy))
            {
                return &ch;
            }
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
