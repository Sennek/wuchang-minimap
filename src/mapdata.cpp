#include "mapdata.hpp"

#include <Windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "mmstate.hpp"

namespace mapdata
{
    namespace
    {
        //==============================================================================
        // A tiny JSON reader
        //==============================================================================
        //
        // maps.json is written by tools/navmesh/build_map.py, but it is a text file in
        // the user's game folder, so it still has to survive being hand-edited into
        // nonsense. This is a complete (if minimal) recursive-descent parser rather
        // than a pattern match: numbers, strings, bools, null, arrays and objects, with
        // a depth cap. Only the subset we need is ever read out of the tree.

        struct JValue;
        using JObject = std::vector<std::pair<std::string, JValue>>;
        using JArray = std::vector<JValue>;

        struct JValue
        {
            enum class Kind
            {
                Null,
                Bool,
                Number,
                String,
                Array,
                Object
            } kind = Kind::Null;

            bool b = false;
            double num = 0.0;
            std::string str;
            std::shared_ptr<JArray> arr;
            std::shared_ptr<JObject> obj;

            const JValue* find(std::string_view key) const
            {
                if (kind != Kind::Object || !obj)
                {
                    return nullptr;
                }
                for (const auto& kv : *obj)
                {
                    if (kv.first == key)
                    {
                        return &kv.second;
                    }
                }
                return nullptr;
            }

            double number_or(double fallback) const
            {
                return kind == Kind::Number ? num : fallback;
            }

            std::string string_or(std::string fallback) const
            {
                return kind == Kind::String ? str : fallback;
            }
        };

        class JParser
        {
          public:
            explicit JParser(std::string_view text) : t_(text) {}

            bool parse(JValue& out)
            {
                skip();
                return value(out, 0) && (skip(), p_ >= t_.size());
            }

          private:
            std::string_view t_;
            std::size_t p_ = 0;

            void skip()
            {
                while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\r' || t_[p_] == '\n'))
                {
                    ++p_;
                }
            }

            bool lit(std::string_view s)
            {
                if (t_.compare(p_, s.size(), s) == 0)
                {
                    p_ += s.size();
                    return true;
                }
                return false;
            }

            bool value(JValue& out, int depth)
            {
                if (depth > 32 || p_ >= t_.size())
                {
                    return false;
                }
                switch (t_[p_])
                {
                case '{':
                    return object(out, depth);
                case '[':
                    return array(out, depth);
                case '"':
                    out.kind = JValue::Kind::String;
                    return string(out.str);
                case 't':
                    out.kind = JValue::Kind::Bool;
                    out.b = true;
                    return lit("true");
                case 'f':
                    out.kind = JValue::Kind::Bool;
                    out.b = false;
                    return lit("false");
                case 'n':
                    out.kind = JValue::Kind::Null;
                    return lit("null");
                default:
                    return number(out);
                }
            }

            bool string(std::string& out)
            {
                if (p_ >= t_.size() || t_[p_] != '"')
                {
                    return false;
                }
                ++p_;
                out.clear();
                while (p_ < t_.size() && t_[p_] != '"')
                {
                    if (t_[p_] == '\\' && p_ + 1 < t_.size())
                    {
                        ++p_;
                        switch (t_[p_])
                        {
                        case 'n':
                            out.push_back('\n');
                            break;
                        case 't':
                            out.push_back('\t');
                            break;
                        case 'r':
                            out.push_back('\r');
                            break;
                        case 'u':
                            // Not needed for this manifest; skip the code point.
                            p_ += 4;
                            out.push_back('?');
                            break;
                        default:
                            out.push_back(t_[p_]);
                            break;
                        }
                        ++p_;
                        continue;
                    }
                    out.push_back(t_[p_++]);
                }
                if (p_ >= t_.size())
                {
                    return false;
                }
                ++p_; // closing quote
                return true;
            }

            bool number(JValue& out)
            {
                const std::size_t start = p_;
                while (p_ < t_.size() && (std::strchr("+-.eE0123456789", t_[p_]) != nullptr))
                {
                    ++p_;
                }
                if (p_ == start)
                {
                    return false;
                }
                const std::string text{t_.substr(start, p_ - start)};
                char* end = nullptr;
                const double v = std::strtod(text.c_str(), &end);
                if (end == text.c_str())
                {
                    return false;
                }
                out.kind = JValue::Kind::Number;
                out.num = v;
                return true;
            }

            bool array(JValue& out, int depth)
            {
                ++p_; // '['
                out.kind = JValue::Kind::Array;
                out.arr = std::make_shared<JArray>();
                skip();
                if (p_ < t_.size() && t_[p_] == ']')
                {
                    ++p_;
                    return true;
                }
                for (;;)
                {
                    skip();
                    JValue element{};
                    if (!value(element, depth + 1))
                    {
                        return false;
                    }
                    out.arr->push_back(std::move(element));
                    skip();
                    if (p_ < t_.size() && t_[p_] == ',')
                    {
                        ++p_;
                        continue;
                    }
                    break;
                }
                if (p_ >= t_.size() || t_[p_] != ']')
                {
                    return false;
                }
                ++p_;
                return true;
            }

            bool object(JValue& out, int depth)
            {
                ++p_; // '{'
                out.kind = JValue::Kind::Object;
                out.obj = std::make_shared<JObject>();
                skip();
                if (p_ < t_.size() && t_[p_] == '}')
                {
                    ++p_;
                    return true;
                }
                for (;;)
                {
                    skip();
                    std::string key;
                    if (!string(key))
                    {
                        return false;
                    }
                    skip();
                    if (p_ >= t_.size() || t_[p_] != ':')
                    {
                        return false;
                    }
                    ++p_;
                    skip();
                    JValue v{};
                    if (!value(v, depth + 1))
                    {
                        return false;
                    }
                    out.obj->emplace_back(std::move(key), std::move(v));
                    skip();
                    if (p_ < t_.size() && t_[p_] == ',')
                    {
                        ++p_;
                        continue;
                    }
                    break;
                }
                if (p_ >= t_.size() || t_[p_] != '}')
                {
                    return false;
                }
                ++p_;
                return true;
            }
        };

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

        // `channels`: 4 -> 32bpp RGBA (the Z-shaded composite), 1 -> 8bpp gray (a
        // floor layer's coverage mask, uploaded as R8_UNORM and swizzled to RGBA in
        // the SRV, which is what keeps nine full-resolution layers inside ~104 MB).
        bool decode_png(const std::wstring& path, PendingImage& out)
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
                hr = converter->Initialize(frame,
                                           out.channels == 1 ? GUID_WICPixelFormat8bppGray
                                                             : GUID_WICPixelFormat32bppRGBA,
                                           WICBitmapDitherTypeNone,
                                           nullptr,
                                           0.0,
                                           WICBitmapPaletteTypeCustom);
            }
            if (SUCCEEDED(hr))
            {
                const UINT stride = w * static_cast<UINT>(out.channels);
                out.width = static_cast<int>(w);
                out.height = static_cast<int>(h);
                out.pixels.resize(static_cast<std::size_t>(stride) * h);
                hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.pixels.size()), out.pixels.data());
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

            // ---- the per-floor layers -------------------------------------------
            auto layers = std::make_shared<std::vector<Layer>>();
            const JValue* jlayers = c.find("layers");
            if (jlayers != nullptr && jlayers->kind == JValue::Kind::Array && jlayers->arr)
            {
                for (const JValue& jl : *jlayers->arr)
                {
                    if (jl.kind != JValue::Kind::Object)
                    {
                        continue;
                    }
                    const auto ln = [&jl](const char* key, double fallback) {
                        const JValue* v = jl.find(key);
                        return v != nullptr ? v->number_or(fallback) : fallback;
                    };
                    Layer l{};
                    const JValue* li = jl.find("image");
                    l.image = li != nullptr ? li->string_or("") : "";
                    l.floor = static_cast<int>(ln("floor", -1.0));
                    l.image_width = static_cast<int>(ln("image_width", 0.0));
                    l.image_height = static_cast<int>(ln("image_height", 0.0));
                    l.min_x = ln("min_x", 0.0);
                    l.min_y = ln("min_y", 0.0);
                    l.max_x = ln("max_x", 0.0);
                    l.max_y = ln("max_y", 0.0);
                    l.px_per_uu = ln("px_per_uu", 0.0);
                    l.z_min = ln("z_min", 0.0);
                    l.z_max = ln("z_max", 0.0);
                    l.poly_count = static_cast<int>(ln("poly_count", 0.0));
                    if (l.image.empty() || l.floor < 0 || l.image_width <= 0 || l.image_height <= 0 ||
                        l.px_per_uu <= 0.0 || l.max_x <= l.min_x || l.max_y <= l.min_y)
                    {
                        mm::logf(L"maps: chapter \"{}\" has an incomplete layer entry - skipped", widen(ch.key));
                        continue;
                    }
                    if (layers->size() >= static_cast<std::size_t>(kMaxLayers))
                    {
                        mm::logf(L"maps: chapter \"{}\" has more than {} layers - the rest are ignored",
                                 widen(ch.key),
                                 kMaxLayers);
                        break;
                    }
                    layers->push_back(std::move(l));
                }
            }
            ch.layers = layers;

            // ---- the surface-band grid ------------------------------------------
            //
            // Row layout, from build_map.py:
            //   [gx, gy, band_count, (zmin, zmax, floor_count, floor...) * band_count]
            // Anything malformed drops that one row, never the whole grid: a broken
            // grid only means "fall back", not "no map".
            auto grid = std::make_shared<FloorGrid>();
            grid->cell_uu = n("floor_grid_cell_uu", 0.0);
            std::size_t bands_total = 0;
            std::size_t rows_bad = 0;
            const JValue* jgrid = c.find("floor_grid");
            if (grid->cell_uu > 0.0 && jgrid != nullptr && jgrid->kind == JValue::Kind::Array && jgrid->arr)
            {
                for (const JValue& jrow : *jgrid->arr)
                {
                    if (jrow.kind != JValue::Kind::Array || !jrow.arr || jrow.arr->size() < 3)
                    {
                        ++rows_bad;
                        continue;
                    }
                    const JArray& row = *jrow.arr;
                    const auto num = [&row](std::size_t i) {
                        return row[i].kind == JValue::Kind::Number ? row[i].num : 0.0;
                    };
                    const int gx = static_cast<int>(num(0));
                    const int gy = static_cast<int>(num(1));
                    const int nbands = static_cast<int>(num(2));
                    if (nbands <= 0 || nbands > 64)
                    {
                        ++rows_bad;
                        continue;
                    }
                    std::vector<Band> bands;
                    bands.reserve(static_cast<std::size_t>(nbands));
                    std::size_t p = 3;
                    bool ok = true;
                    for (int b = 0; b < nbands && ok; ++b)
                    {
                        if (p + 3 > row.size())
                        {
                            ok = false;
                            break;
                        }
                        Band band{};
                        band.z_min = static_cast<float>(num(p));
                        band.z_max = static_cast<float>(num(p + 1));
                        const int nf = static_cast<int>(num(p + 2));
                        p += 3;
                        if (nf <= 0 || nf > 32 || p + static_cast<std::size_t>(nf) > row.size())
                        {
                            ok = false;
                            break;
                        }
                        for (int f = 0; f < nf; ++f)
                        {
                            if (band.floor_count < kMaxBandFloors)
                            {
                                band.floors[band.floor_count++] = static_cast<int>(num(p + static_cast<std::size_t>(f)));
                            }
                        }
                        p += static_cast<std::size_t>(nf);
                        if (band.z_max < band.z_min)
                        {
                            std::swap(band.z_min, band.z_max);
                        }
                        bands.push_back(band);
                    }
                    if (!ok || bands.empty())
                    {
                        ++rows_bad;
                        continue;
                    }
                    bands_total += bands.size();
                    grid->cells.emplace(FloorGrid::key(gx, gy), std::move(bands));
                }
            }
            ch.grid = grid;

            mm::logf(L"maps: chapter \"{}\" {} {}x{} px @ {:.4f} px/uu, world X {:.0f}..{:.0f} Y {:.0f}..{:.0f}",
                     widen(ch.key),
                     widen(ch.image),
                     ch.image_width,
                     ch.image_height,
                     ch.px_per_uu,
                     ch.min_x,
                     ch.max_x,
                     ch.min_y,
                     ch.max_y);
            mm::logf(L"maps: chapter \"{}\" {} floor layer(s), grid {} cell(s) of {:.0f} uu with {} band(s){}",
                     widen(ch.key),
                     layers->size(),
                     grid->cells.size(),
                     grid->cell_uu,
                     bands_total,
                     rows_bad != 0 ? std::format(L", {} malformed row(s) dropped", rows_bad) : std::wstring{});
            parsed.push_back(std::move(ch));
        }

        if (parsed.empty())
        {
            mm::log(L"maps: no usable chapter in the manifest");
            return;
        }

        // MVP: one chapter's textures at a time - the first one in the manifest.
        // Multi-chapter streaming comes with the rest of the chapters' assets.
        Chapter& first = parsed.front();
        const auto png_path = [&maps_dir](const std::string& rel) {
            std::wstring p = maps_dir + L"\\" + widen(rel);
            std::replace(p.begin(), p.end(), L'/', L'\\');
            return p;
        };

        pending_clear(); // an F5 reload must not upload the previous set

        // The layers first: they are what is drawn. The composite is only the
        // off-grid fallback and is skipped entirely unless the config asks for it -
        // it is another 82 MB of VRAM on top of the layers' ~104 MB.
        std::size_t layer_bytes = 0;
        std::size_t layers_ok = 0;
        if (first.layers)
        {
            for (std::size_t i = 0; i < first.layers->size(); ++i)
            {
                const Layer& l = (*first.layers)[i];
                auto img = std::make_unique<PendingImage>();
                img->chapter_key = first.key;
                img->layer_index = static_cast<int>(i);
                img->floor = l.floor;
                img->channels = 1;
                if (!decode_png(png_path(l.image), *img))
                {
                    continue;
                }
                if (img->width != l.image_width || img->height != l.image_height)
                {
                    mm::logf(L"maps: layer f{} is {}x{} but maps.json says {}x{} - skipped (the bounds would not match)",
                             l.floor,
                             img->width,
                             img->height,
                             l.image_width,
                             l.image_height);
                    continue;
                }
                layer_bytes += img->pixels.size();
                ++layers_ok;
                pending_push(std::move(img));
            }
            mm::logf(L"maps: decoded {}/{} floor layer(s) of \"{}\", {} MB of R8 texture memory",
                     layers_ok,
                     first.layers->size(),
                     widen(first.key),
                     layer_bytes / (1024 * 1024));
        }

        if (want_composite)
        {
            auto img = std::make_unique<PendingImage>();
            img->chapter_key = first.key;
            img->layer_index = -1;
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
                mm::logf(L"maps: decoded the composite {} -> {}x{} RGBA ({} MB)",
                         widen(first.image),
                         img->width,
                         img->height,
                         img->pixels.size() / (1024 * 1024));
                pending_push(std::move(img));
            }
        }
        else
        {
            mm::log(L"maps: composite texture not loaded (fallback_use_composite = 0); the off-grid "
                    L"fallback draws every floor layer at once instead");
        }

        if (layers_ok == 0 && !want_composite)
        {
            mm::log(L"maps: no layer decoded and no composite requested - the minimap will have nothing to draw");
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

    const std::vector<Band>* FloorGrid::at(double wx, double wy) const
    {
        if (cell_uu <= 0.0 || cells.empty())
        {
            return nullptr;
        }
        const int gx = static_cast<int>(std::floor(wx / cell_uu));
        const int gy = static_cast<int>(std::floor(wy / cell_uu));
        const auto it = cells.find(key(gx, gy));
        return it == cells.end() ? nullptr : &it->second;
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
