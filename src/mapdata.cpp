#include "mapdata.hpp"

#include <Windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cstring>

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
        std::atomic<PendingImage*> g_pending{nullptr};

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
                hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom);
            }
            if (SUCCEEDED(hr))
            {
                const UINT stride = w * 4;
                out.width = static_cast<int>(w);
                out.height = static_cast<int>(h);
                out.rgba.resize(static_cast<std::size_t>(stride) * h);
                hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.rgba.size()), out.rgba.data());
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
            parsed.push_back(std::move(ch));
        }

        if (parsed.empty())
        {
            mm::log(L"maps: no usable chapter in the manifest");
            return;
        }

        // MVP: one texture, the first chapter. Multi-chapter streaming comes with the
        // rest of the chapters' assets.
        const Chapter& first = parsed.front();
        std::wstring png = maps_dir + L"\\" + widen(first.image);
        std::replace(png.begin(), png.end(), L'/', L'\\');

        auto pending = std::make_unique<PendingImage>();
        pending->chapter_key = first.key;
        if (!decode_png(png, *pending))
        {
            publish_chapters(std::move(parsed));
            g_loaded = true;
            return;
        }
        if (pending->width != first.image_width || pending->height != first.image_height)
        {
            mm::logf(L"maps: {} is {}x{} but maps.json says {}x{} - trusting the PNG",
                     png,
                     pending->width,
                     pending->height,
                     first.image_width,
                     first.image_height);
            for (Chapter& ch : parsed)
            {
                if (ch.key == first.key)
                {
                    ch.image_width = pending->width;
                    ch.image_height = pending->height;
                }
            }
        }
        mm::logf(L"maps: decoded {} -> {}x{} RGBA ({} MB), waiting for the render thread",
                 png,
                 pending->width,
                 pending->height,
                 pending->rgba.size() / (1024 * 1024));

        publish_chapters(std::move(parsed));
        g_loaded = true;

        // Hand the pixels over; drop any previous undelivered decode.
        PendingImage* old = g_pending.exchange(pending.release());
        delete old;
    }

    std::unique_ptr<PendingImage> take_pending()
    {
        return std::unique_ptr<PendingImage>{g_pending.exchange(nullptr)};
    }

    std::vector<Chapter> chapters()
    {
        const std::vector<Chapter>* list = g_chapters.load(std::memory_order_acquire);
        return list == nullptr ? std::vector<Chapter>{} : *list;
    }

    Chapter chapter_for(double wx, double wy)
    {
        const std::vector<Chapter>* list = g_chapters.load(std::memory_order_acquire);
        if (list == nullptr)
        {
            return {};
        }
        for (const Chapter& ch : *list)
        {
            if (ch.contains(wx, wy))
            {
                return ch;
            }
        }
        return {};
    }

    bool loaded()
    {
        return g_loaded.load(std::memory_order_acquire);
    }
} // namespace mapdata
