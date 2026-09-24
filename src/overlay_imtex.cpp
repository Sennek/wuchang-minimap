//
// overlay_imtex - ImGui's own textures (the dynamic font atlas): created, updated and
// destroyed on the frame's command list, with nothing ever waiting for the GPU.
//
// RENDER thread, with `g_render_lock` held, inside the game's Present. ImGui 1.92 asks for
// texture work through `ImDrawData::Textures`. The stock DX12 backend answers each request on a
// command list of its own and then blocks on the queue, which from inside Present holds the game
// until every overlay frame in flight has finished on the GPU. Here the copies are recorded into
// the frame's list ahead of ImGui's draw call; a staging buffer is reused once `g_fence` passes
// the frame that read it, and a texture ImGui drops is released the same way. RenderDrawData
// then gets a draw data with no texture list, which the backend documents as "handled
// elsewhere".
//
// Every ImGui texture is ours: `BackendUserData` points at an ImTex, never at the backend's own
// struct, so `destroy_imgui_textures()` has to run before `ImGui_ImplDX12_Shutdown`.
//
// A request that cannot be met this frame (no staging buffer free, a failed allocation) records
// nothing and stays pending for the next one. A texture still waiting for its first upload
// cannot be sampled, so such a frame skips ImGui's draw call.
//

#include "overlay_internal.hpp"

#include "texupload.hpp"

namespace overlay
{
    namespace ovl
    {
        namespace
        {
            static_assert(texup::kPitchAlign == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            static_assert(texup::kPlaceAlign == D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

            struct ImTex
            {
                ID3D12Resource* res = nullptr;
                D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
                D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
            };

            // Mapped for life. `fence` is the frame that last read it; 0 means free. A buffer
            // above kStagingKeep (a whole atlas on creation) is released once free rather than
            // parked for the session.
            struct Staging
            {
                ID3D12Resource* buf = nullptr;
                std::uint8_t* mapped = nullptr;
                UINT64 size = 0;
                UINT64 fence = 0;
            };
            constexpr int kStaging = 8;
            constexpr UINT64 kStagingMin = 256ull << 10;
            constexpr UINT64 kStagingKeep = 1ull << 20;
            Staging g_staging[kStaging]{};

            // A texture ImGui dropped, and its SRV slot, until the last frame that could sample
            // it has finished.
            struct Retired
            {
                ID3D12Resource* res = nullptr;
                D3D12_CPU_DESCRIPTOR_HANDLE srv{};
                UINT64 fence = 0;
            };
            constexpr int kRetired = 8;
            Retired g_retired[kRetired]{};

            std::vector<ImTextureRect> g_rects;
            std::vector<texup::Placed> g_placed;

            void release_staging(Staging& s)
            {
                if (s.buf != nullptr && s.mapped != nullptr)
                {
                    s.buf->Unmap(0, nullptr);
                }
                safe_release(s.buf);
                s = Staging{};
            }

            void release_retired(Retired& r)
            {
                if (r.srv.ptr != 0)
                {
                    g_srv_heap.free(r.srv);
                }
                safe_release(r.res);
                r = Retired{};
            }

            void sweep(UINT64 done)
            {
                for (Staging& s : g_staging)
                {
                    if (s.fence != 0 && done >= s.fence)
                    {
                        s.fence = 0;
                        if (s.size > kStagingKeep)
                        {
                            release_staging(s);
                        }
                    }
                }
                for (Retired& r : g_retired)
                {
                    if (r.fence != 0 && done >= r.fence)
                    {
                        release_retired(r);
                    }
                }
            }

            // A free buffer of at least `bytes`, claimed for the frame signalling `fence`.
            // Reuses one that fits, else fills an empty slot, else replaces a free one.
            Staging* acquire_staging(UINT64 bytes, UINT64 fence)
            {
                Staging* fit = nullptr;
                Staging* empty = nullptr;
                Staging* spare = nullptr;
                for (Staging& s : g_staging)
                {
                    if (s.fence != 0)
                    {
                        continue;
                    }
                    if (s.buf != nullptr && s.size >= bytes && fit == nullptr)
                    {
                        fit = &s;
                    }
                    else if (s.buf == nullptr && empty == nullptr)
                    {
                        empty = &s;
                    }
                    else if (spare == nullptr)
                    {
                        spare = &s;
                    }
                }
                Staging* pick = fit != nullptr ? fit : (empty != nullptr ? empty : spare);
                if (pick == nullptr)
                {
                    return nullptr;
                }
                if (pick != fit)
                {
                    release_staging(*pick);
                    const UINT64 size = ((bytes > kStagingMin ? bytes : kStagingMin) + 0xFFFF) & ~0xFFFFull;
                    D3D12_HEAP_PROPERTIES heap{};
                    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                    D3D12_RESOURCE_DESC desc{};
                    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    desc.Width = size;
                    desc.Height = 1;
                    desc.DepthOrArraySize = 1;
                    desc.MipLevels = 1;
                    desc.Format = DXGI_FORMAT_UNKNOWN;
                    desc.SampleDesc.Count = 1;
                    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    void* mapped = nullptr;
                    const D3D12_RANGE none{0, 0};
                    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                 IID_PPV_ARGS(&pick->buf)))
                        || FAILED(pick->buf->Map(0, &none, &mapped)) || mapped == nullptr)
                    {
                        release_staging(*pick);
                        return nullptr;
                    }
                    pick->mapped = static_cast<std::uint8_t*>(mapped);
                    pick->size = size;
                }
                pick->fence = fence;
                return pick;
            }

            bool create_texture(ImTextureData* tex)
            {
                ImTex* t = new ImTex{};
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = static_cast<UINT64>(tex->Width);
                desc.Height = static_cast<UINT>(tex->Height);
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&t->res))))
                {
                    delete t;
                    return false;
                }
                if (!g_srv_heap.alloc(t->srv_cpu, t->srv_gpu))
                {
                    safe_release(t->res);
                    delete t;
                    return false;
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(t->res, &srv, t->srv_cpu);
                tex->SetTexID(static_cast<ImTextureID>(t->srv_gpu.ptr));
                tex->BackendUserData = t;
                mm::logf(L"imgui texture #{}: {}x{} created; its uploads ride the overlay's own command list",
                         tex->UniqueID,
                         tex->Width,
                         tex->Height);
                return true;
            }

            // Records the copies one request needs. `fresh` is the first upload: the whole
            // texture, into a resource still in COPY_DEST. False records nothing.
            bool record_upload(ImTextureData* tex, bool fresh, ID3D12GraphicsCommandList* list, UINT64 fence)
            {
                g_rects.clear();
                if (fresh)
                {
                    g_rects.push_back(ImTextureRect{0, 0, static_cast<unsigned short>(tex->Width),
                                                    static_cast<unsigned short>(tex->Height)});
                }
                else if (!tex->Updates.empty())
                {
                    for (const ImTextureRect& r : tex->Updates)
                    {
                        if (r.w != 0 && r.h != 0)
                        {
                            g_rects.push_back(r);
                        }
                    }
                }
                else if (tex->UpdateRect.w != 0 && tex->UpdateRect.h != 0)
                {
                    g_rects.push_back(tex->UpdateRect);
                }
                if (g_rects.empty())
                {
                    return true;
                }
                const int n = static_cast<int>(g_rects.size());
                g_placed.resize(g_rects.size());
                const UINT64 bytes = texup::lay_out(g_rects.data(), n, tex->BytesPerPixel, g_placed.data());
                Staging* s = acquire_staging(bytes, fence);
                if (s == nullptr)
                {
                    return false;
                }
                const std::size_t row_bytes_px = static_cast<std::size_t>(tex->BytesPerPixel);
                for (int i = 0; i < n; ++i)
                {
                    const ImTextureRect& r = g_rects[static_cast<std::size_t>(i)];
                    const texup::Placed& p = g_placed[static_cast<std::size_t>(i)];
                    for (int y = 0; y < r.h; ++y)
                    {
                        std::memcpy(s->mapped + p.offset + static_cast<UINT64>(y) * p.pitch,
                                    tex->GetPixelsAt(r.x, r.y + y),
                                    r.w * row_bytes_px);
                    }
                }

                ImTex* t = static_cast<ImTex*>(tex->BackendUserData);
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = t->res;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (!fresh)
                {
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    list->ResourceBarrier(1, &barrier);
                }
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = t->res;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = s->buf;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                src.PlacedFootprint.Footprint.Depth = 1;
                for (int i = 0; i < n; ++i)
                {
                    const ImTextureRect& r = g_rects[static_cast<std::size_t>(i)];
                    const texup::Placed& p = g_placed[static_cast<std::size_t>(i)];
                    src.PlacedFootprint.Offset = p.offset;
                    src.PlacedFootprint.Footprint.Width = r.w;
                    src.PlacedFootprint.Footprint.Height = r.h;
                    src.PlacedFootprint.Footprint.RowPitch = p.pitch;
                    list->CopyTextureRegion(&dst, r.x, r.y, 0, &src, nullptr);
                }
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &barrier);
                return true;
            }

            // False when there is no room to retire it yet; the request stays pending.
            bool retire_texture(ImTextureData* tex, UINT64 fence)
            {
                if (ImTex* t = static_cast<ImTex*>(tex->BackendUserData))
                {
                    Retired* slot = nullptr;
                    for (Retired& r : g_retired)
                    {
                        if (r.fence == 0 && r.res == nullptr)
                        {
                            slot = &r;
                            break;
                        }
                    }
                    if (slot == nullptr)
                    {
                        return false;
                    }
                    slot->res = t->res;
                    slot->srv = t->srv_cpu;
                    slot->fence = fence;
                    delete t;
                }
                tex->SetTexID(ImTextureID_Invalid);
                tex->BackendUserData = nullptr;
                tex->SetStatus(ImTextureStatus_Destroyed);
                return true;
            }

            void say_once(bool& said, const wchar_t* what)
            {
                if (!said)
                {
                    said = true;
                    mm::log(what);
                }
            }
        } // namespace

        ImTexFrame record_imgui_textures(ImDrawData* dd, ID3D12GraphicsCommandList* list)
        {
            ImTexFrame out{};
            if (dd == nullptr || dd->Textures == nullptr)
            {
                out.drawable = dd != nullptr;
                return out;
            }
            ImVector<ImTextureData*>& all = *dd->Textures;
            dd->Textures = nullptr;
            // The value this frame signals: the copies below are read by it, and a texture
            // dropped now may still be sampled by any frame up to it.
            const UINT64 fence = g_fence_value + 1;
            sweep(g_fence->GetCompletedValue());
            static bool said_format = false;
            static bool said_create = false;
            static bool said_staging = false;
            for (ImTextureData* tex : all)
            {
                const ImTextureStatus status = tex->Status;
                if (status == ImTextureStatus_OK || status == ImTextureStatus_Destroyed)
                {
                    continue;
                }
                out.any = true;
                if (status == ImTextureStatus_WantDestroy)
                {
                    (void)retire_texture(tex, fence);
                    continue;
                }
                if (tex->Format != ImTextureFormat_RGBA32)
                {
                    say_once(said_format, L"imgui texture: a format other than RGBA32 was asked for and is not "
                                          L"uploaded - the overlay's text will be missing");
                    continue;
                }
                const bool fresh = status == ImTextureStatus_WantCreate;
                if (fresh && tex->BackendUserData == nullptr && !create_texture(tex))
                {
                    say_once(said_create, L"imgui texture: the font atlas could not be created on the GPU - "
                                          L"the overlay draws no ImGui content until it can be");
                    continue;
                }
                if (!record_upload(tex, fresh, list, fence))
                {
                    say_once(said_staging, L"imgui texture: no staging buffer this frame - the upload waits "
                                           L"for the next one");
                    continue;
                }
                tex->SetStatus(ImTextureStatus_OK);
            }
            out.drawable = true;
            for (ImTextureData* tex : all)
            {
                if (tex->Status == ImTextureStatus_WantCreate)
                {
                    out.drawable = false;
                }
            }
            return out;
        }

        void destroy_imgui_textures()
        {
            for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
            {
                if (ImTex* t = static_cast<ImTex*>(tex->BackendUserData))
                {
                    if (t->srv_cpu.ptr != 0)
                    {
                        g_srv_heap.free(t->srv_cpu);
                    }
                    safe_release(t->res);
                    delete t;
                    tex->SetTexID(ImTextureID_Invalid);
                    tex->BackendUserData = nullptr;
                    tex->SetStatus(ImTextureStatus_Destroyed);
                }
            }
            for (Staging& s : g_staging)
            {
                release_staging(s);
            }
            for (Retired& r : g_retired)
            {
                release_retired(r);
            }
        }
    } // namespace ovl
} // namespace overlay
