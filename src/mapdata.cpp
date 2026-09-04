#include "mapdata.hpp"

#include <Windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "mapmanifest.hpp"
#include "pngdecode.hpp"
#include "breadcrumb.hpp"
#include "mmstate.hpp"

namespace mapdata
{
    namespace
    {
        // The manifest parse lives in mapmanifest.hpp - pure, header-only and exercised by
        // tests/markers_test.cpp. What is left here needs Windows: the file read, the WIC
        // decode and the residency state machine.

        //==============================================================================
        // State
        //==============================================================================

        std::atomic<bool> g_loaded{false};

        // Decoded images waiting for the render thread. A chapter decodes ten pictures
        // (nine layers plus, optionally, the composite). std::mutex is banned in this mod,
        // so the critical section - one push_back or pop_front of a pointer - is guarded by
        // an atomic_flag spinlock and never does I/O.
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
        // vector, then swaps the pointer in, so the render thread reads it with no lock
        // while an F5 reload rebuilds it. The old vector is leaked on purpose (bounded by
        // the number of reloads) because a reader may still be walking it.
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
        // PNG -> pixels
        //==============================================================================
        //
        // The WIC decode lives in pngdecode.hpp - pure Windows, no UE4SS - so
        // tests/markers_test.cpp can put a SHIPPED PNG through exactly this path on the
        // build machine. Runs on the loop thread, so the decode never stalls Present.

        // Returns the wall time the decode took, in ms, or a negative value on failure.
        double decode_raw_ms(const std::wstring& path, int channels, int& out_w, int& out_h,
                             std::vector<std::uint8_t>& out_pixels)
        {
            const std::uint64_t t0 = ::GetTickCount64();
            const pngdec::Result r = pngdec::decode(path.c_str(), channels, out_pixels);
            if (!r.ok())
            {
                mm::logf(L"maps: PNG decode of {} failed (0x{:08X})", path,
                         static_cast<unsigned>(r.hr));
                return -1.0;
            }
            out_w = r.width;
            out_h = r.height;
            return static_cast<double>(::GetTickCount64() - t0);
        }

        std::wstring widen(std::string_view narrow)
        {
            return std::wstring{narrow.begin(), narrow.end()};
        }

        //==============================================================================
        // ONE CHAPTER AT A TIME
        //==============================================================================
        //
        // All loop-thread state except the two atomics. The published chapter list is never
        // freed (see publish_chapters), so a render thread holding a `const Chapter*` keeps
        // a valid object across a reload; what it may find inside is a null `heights`,
        // which every caller already tests for.

        // Parsed manifest, kept in step (index for index) with the published chapter
        // vector. It carries the per-chapter z_min / z_max and file list that decoding
        // needs.
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

        // Decodes one chapter's height planes into the sparse block store. Returns nullptr
        // when nothing usable came back; it logs why.
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
            hm->z_code_max = e.z_code_max;

            if (hm->z_max <= hm->z_min)
            {
                mm::logf(L"maps: chapter \"{}\" has no usable z_min/z_max in maps.json "
                         L"({:.0f}..{:.0f}) - the height maps are unusable",
                         widen(ch.key),
                         static_cast<double>(hm->z_min),
                         static_cast<double>(hm->z_max));
                return nullptr;
            }

            // Every plane and the composite are timed, and the per-plane line carries what
            // the block store allocated for it - the only place the 26 % occupancy is
            // visible on a player's machine.
            double total_ms = 0.0;
            std::vector<std::uint8_t> raw;
            for (std::size_t k = 0; k < ch.height_files.size() && k < kMaxSurfaces; ++k)
            {
                int w = 0;
                int h = 0;
                const double ms = decode_raw_ms(png_path(ch.height_files[k]), 2, w, h, raw);
                if (ms < 0.0)
                {
                    break; // a gap in the planes would misorder the surfaces
                }
                total_ms += ms;
                if (hm->width == 0)
                {
                    hm->width = w;
                    hm->height = h;
                }
                else if (w != hm->width || h != hm->height)
                {
                    mm::logf(L"maps: height plane {} is {}x{} but the first is {}x{} - stopping here",
                             k,
                             w,
                             h,
                             hm->width,
                             hm->height);
                    break;
                }
                if (raw.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 2u)
                {
                    mm::logf(L"maps: height plane {} decoded {} byte(s) for {}x{} - refusing it",
                             k,
                             raw.size(),
                             w,
                             h);
                    break;
                }
                const std::uint64_t t0 = ::GetTickCount64();
                build_plane(hm->layer[k], reinterpret_cast<const std::uint16_t*>(raw.data()),
                            w, h);
                const double tile_ms = static_cast<double>(::GetTickCount64() - t0);
                total_ms += tile_ms;
                hm->count = static_cast<int>(k + 1);
                mm::logf(L"maps:   plane {} {}x{} decoded in {:.0f} ms, tiled in {:.0f} ms -> "
                         L"{}/{} tile(s) of {} px, {} KB (dense would be {} KB)",
                         k,
                         w,
                         h,
                         ms,
                         tile_ms,
                         hm->layer[k].tiles,
                         hm->layer[k].tile.size(),
                         kTilePx,
                         hm->layer[k].bytes() / 1024,
                         (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 2u) / 1024);
            }
            raw.clear();
            raw.shrink_to_fit();

            if (hm->count == 0)
            {
                mm::logf(L"maps: NO height plane decoded for \"{}\" - build them with "
                         L"tools/navmesh/build_map.py and re-run deploy.ps1",
                         widen(ch.key));
                return nullptr;
            }

            // SANITY CHECK ON THE BYTE ORDER AND THE QUANTISATION. PNG stores 16-bit
            // samples big-endian; WIC's 16bppGray converter hands them back in native
            // (little-endian) order, and a decoder that did not would produce byte-swapped
            // garbage whose only symptom is a map that looks like noise. Codes only go up
            // to `z_code_max` (4095) and 4095 byte-swapped is 65295, so ONE swapped pixel
            // puts the maximum out of range. Decode plane 0's Z range and lit count and log
            // both.
            float lo = hm->z_max;
            float hi = hm->z_min;
            std::size_t lit = 0;
            int code_hi = 0;
            const HeightPlane& p0 = hm->layer[0];
            for (std::int32_t idx : p0.tile)
            {
                if (idx < 0)
                {
                    continue;
                }
                const std::uint16_t* b = p0.data.data() + static_cast<std::size_t>(idx) * kTileCells;
                for (int i = 0; i < kTileCells; ++i)
                {
                    const std::uint16_t code = b[i];
                    if (code == 0)
                    {
                        continue;
                    }
                    code_hi = code > code_hi ? code : code_hi;
                    const float z = hm->decode(code);
                    lo = z < lo ? z : lo;
                    hi = z > hi ? z : hi;
                    ++lit;
                }
            }
            const std::size_t px_total =
                static_cast<std::size_t>(hm->width) * static_cast<std::size_t>(hm->height);
            mm::logf(L"maps: height plane 0 has {} lit pixel(s) ({}%), codes up to {} of {}, "
                     L"decoded Z {:.0f}..{:.0f} (manifest says {:.0f}..{:.0f}) - a byte-swapped "
                     L"decode would not fit this",
                     lit,
                     px_total == 0 ? 0 : (lit * 100) / px_total,
                     code_hi,
                     hm->z_code_max,
                     static_cast<double>(lo),
                     static_cast<double>(hi),
                     static_cast<double>(hm->z_min),
                     static_cast<double>(hm->z_max));
            if (code_hi > hm->z_code_max)
            {
                mm::logf(L"maps: plane 0 carries code {} but maps.json says the maximum is {} - "
                         L"this asset is not the format this build reads; refusing it",
                         code_hi,
                         hm->z_code_max);
                return nullptr;
            }
            const std::size_t dense = hm->dense_bytes();
            const std::size_t sparse = hm->bytes();
            mm::logf(L"maps: decoded {}/{} height plane(s) of \"{}\" in {:.0f} ms ({}x{}, Z "
                     L"{:.0f}..{:.0f} in steps of {:.2f} uu), {} tile(s) of {} px = {} MB of RAM "
                     L"instead of {} MB dense, 0 MB of VRAM",
                     hm->count,
                     ch.height_files.size(),
                     widen(ch.key),
                     total_ms,
                     hm->width,
                     hm->height,
                     static_cast<double>(hm->z_min),
                     static_cast<double>(hm->z_max),
                     static_cast<double>(hm->z_step()),
                     hm->tiles(),
                     kTilePx,
                     sparse / (1024 * 1024),
                     dense / (1024 * 1024));
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
            // Only one retirement is ever in flight, because a switch never starts while
            // one is pending. Free defensively in case that changes.
            // A chapter swap - a ~340 MB free followed by a ~340 MB decode - gets its own
            // pair of breadcrumb stages, so a crash on a loading screen is placed.
            crumb::stage(crumb::kChapterSwapStart);
            delete g_retired;
            g_retired = const_cast<HeightMaps*>(planes);
            // `map_asset_retire_grace_ms` (default kRetireGraceMs), read here rather than
            // baked in: it decides whether a render thread still inside a slice can be
            // handed freed memory.
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
        // There is no "schema mismatch, read it anyway" branch: mapmanifest::parse()
        // refuses a manifest whose schema is not exactly kSchema. The /3 and /4 height
        // encodings differ by a factor of sixteen in one scale, so reading the wrong one
        // draws a map that looks empty rather than reporting a version error.
        if (parsed_manifest.chapters.empty())
        {
            mm::log(L"maps: no usable chapter in the manifest");
            return;
        }

        // An F5 reload publishes a fresh chapter list; the planes hanging off the old one
        // are retired here or leaked with it (327 MB a press).
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
                     L"Y {:.0f}..{:.0f}, Z {:.0f}..{:.0f} in {}-bit steps of {:.2f} uu, "
                     L"{} height plane(s), {} MB when resident",
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
                     e.z_bits,
                     e.z_step_uu(),
                     ch.height_files.size(),
                     resident_mb);
            parsed.push_back(std::move(ch));
        }

        g_manifest = std::move(parsed_manifest);
        publish_chapters(std::move(parsed));
        g_chapters_mut = const_cast<std::vector<Chapter>*>(g_chapters.load(std::memory_order_acquire));
        g_loaded = true;

        // Start on the chapter the detection has already named, if it has; otherwise on the
        // lowest-numbered one, so chapter 1 is resident at the main menu and the slicer
        // self-test has an asset.
        const int detected = g_detected.load(std::memory_order_relaxed);
        const int want = detected == chid::kNone ? -1 : g_manifest.index_of_number(detected);
        g_pending_chapter = want >= 0 ? want : g_manifest.default_index();
        mm::logf(L"maps: {} chapter(s) in the manifest, ONE resident at a time; starting with \"{}\"",
                 g_manifest.chapters.size(),
                 g_pending_chapter >= 0
                     ? widen(g_manifest.chapters[static_cast<std::size_t>(g_pending_chapter)].key)
                     : std::wstring{L"(none)"});

        // Decode now rather than on the next tick, so the map is there when load() returns.
        on_update();
    }

    void unload()
    {
        // LOOP THREAD, master switch only, and only AFTER overlay::stop_complete(): with
        // the render side torn down nobody can be inside a height slice, so the planes are
        // freed here and now rather than through the kRetireGraceMs path.
        const std::uint64_t now = ::GetTickCount64();
        retire_active(now);
        delete g_retired;
        g_retired = nullptr;
        g_retire_at = 0;
        g_pending_chapter = -1;
        pending_clear();
        // Nothing is detected while the mod is off; the next enable logs the chapter
        // again rather than assuming the player never moved.
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
                // want < 0 means "this chapter has no map asset" - the DLC, whose navmesh
                // the paks do not carry. Unloading is right: the chapters' world bounds
                // overlap, so keeping the old one resident would draw chapter 3's geometry
                // under a DLC player.
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
        // Wall clock for the whole chapter, composite included. Individual planes are
        // timed inside decode_heights().
        const std::uint64_t load_t0 = ::GetTickCount64();
        HeightMaps* planes = decode_heights(ch, entry);
        ch.heights = planes;
        g_active.store(index, std::memory_order_release);
        crumb::stage(crumb::kChapterSwapEnd);

        // The composite is only the no-height-map fallback and it is off by default
        // (`fallback_use_composite = 0`). It IS re-decoded on a chapter switch; the upload
        // happens in overlay.cpp's frame path, this side only queues the pixels.
        if (mm::config().fallback_use_composite || planes == nullptr)
        {
            auto img = std::make_unique<PendingImage>();
            img->chapter_key = ch.key;
            img->channels = 4;
            int cw = 0;
            int chh = 0;
            const double composite_ms =
                decode_raw_ms(png_path(ch.image), img->channels, cw, chh, img->pixels);
            if (composite_ms >= 0.0)
            {
                img->width = cw;
                img->height = chh;
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
                mm::logf(L"maps: decoded the composite {} -> {}x{} RGBA ({} MB) in {:.0f} ms{}",
                         widen(ch.image),
                         img->width,
                         img->height,
                         img->pixels.size() / (1024 * 1024),
                         composite_ms,
                         planes == nullptr ? L" - the only thing there is to draw" : L"");
                pending_push(std::move(img));
            }
        }
        else
        {
            mm::log(L"maps: composite texture not loaded (fallback_use_composite = 0); the height "
                    L"slicer does not need it");
        }
        mm::logf(L"maps: chapter \"{}\" ready in {} ms ({} height plane(s) + {}, {} MB resident)",
                 widen(ch.key),
                 ::GetTickCount64() - load_t0,
                 planes != nullptr ? planes->count : 0,
                 mm::config().fallback_use_composite || planes == nullptr ? L"the composite"
                                                                        : L"no composite",
                 planes != nullptr ? planes->bytes() / (1024 * 1024) : std::size_t{0});
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
        // publish_chapters), so a reader can keep using it across a reload.
        const std::vector<Chapter>* list = g_chapters.load(std::memory_order_acquire);
        if (list == nullptr)
        {
            return nullptr;
        }

        // A chapter is resident: it is the only possible answer. The five chapters' world
        // bounds overlap (chapter 4 covers nearly all of chapter 1), so a bounds scan would
        // hand back a different chapter's map inside an overlap.
        const int active = g_active.load(std::memory_order_acquire);
        if (active >= 0 && active < static_cast<int>(list->size()))
        {
            const Chapter& ch = (*list)[static_cast<std::size_t>(active)];
            return ch.contains(wx, wy) ? &ch : nullptr;
        }

        // Nothing resident yet (main menu, or the detection has not landed): prefer a
        // chapter that actually has planes decoded.
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
