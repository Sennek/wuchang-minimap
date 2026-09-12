//
// overlay_slice - the map texture upload and the two height slicers.
//
// The slicers run on the LOOP thread and hand the render thread a finished RGBA buffer;
// the upload and the image draw are the render thread's half of that exchange.
//

#include "overlay_internal.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // Map texture upload
        //==============================================================================

        void destroy_texture(MapTexture& t)
        {
            if (t.srv_cpu.ptr != 0)
            {
                g_srv_heap.free(t.srv_cpu);
                t.srv_cpu = D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            safe_release(t.tex);
            safe_release(t.upload);
            t = MapTexture{};
        }

        void destroy_slice_set(SliceBuf* bufs, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (b.upload != nullptr && b.mapped != nullptr)
                {
                    b.upload->Unmap(0, nullptr);
                }
                if (b.srv_cpu.ptr != 0)
                {
                    g_srv_heap.free(b.srv_cpu);
                }
                safe_release(b.tex);
                safe_release(b.upload);
                b = SliceBuf{};
            }
        }

        // Render thread, and only with the slicer paused.
        void destroy_slice_buffers()
        {
            destroy_slice_set(g_slice, kSliceBufs);
            for (int i = 0; i < kSliceBufs; ++i)
            {
                g_slice_copy_pending[i].store(false);
                g_slice_in_flight[i].store(0);
            }
            g_slice_size = 0;
            g_slice_next = 0;
            clear_slice_view();
            g_slice_last_ms = 0;
            g_slice_range.valid = false;
            note_slice_buffers_changed();
        }

        // Render thread, and only with the slicer paused.
        void destroy_map_slice_buffers()
        {
            destroy_slice_set(g_mslice, kMapSliceBufs);
            for (int i = 0; i < kMapSliceBufs; ++i)
            {
                g_mslice_copy_pending[i].store(false);
                g_mslice_in_flight[i].store(0);
            }
            g_mslice_next = 0;
            clear_map_slice_view();
            g_mslice_last_ms = 0;
            g_mr_valid = false;
            note_slice_buffers_changed();
        }

        // Render thread. Every caller must already have the slicer paused; the F5
        // texture drop and shutdown_render also do it around wait_for_gpu(), so the GPU
        // is idle and the loop thread is out.
        void destroy_all_map_textures()
        {
            destroy_texture(g_map);
            destroy_slice_buffers();
            destroy_map_slice_buffers();
            g_feet_z_valid = false;
            g_slice_scratch.clear();
            // g_mslice_scratch belongs to the loop thread; the slicer is paused here,
            // so clearing it is safe and gives back a closed map's memory.
            g_mslice_scratch.clear();
            g_slice_want_px.store(0, std::memory_order_relaxed);
            {
                spin::SpinGuard guard(g_slice_req_lock);
                g_map_req.wanted = false;
            }
        }

        // An upload buffer only has to live until the GPU has run the copy, and a
        // chapter uploads ten pictures - keeping them parks ~100 MB of CPU-visible
        // memory for the session.
        void release_finished_uploads()
        {
            if (g_fence == nullptr)
            {
                return;
            }
            const UINT64 done = g_fence->GetCompletedValue();
            const auto sweep = [done](MapTexture& t) {
                if (t.upload != nullptr && t.upload_fence != 0 && done >= t.upload_fence)
                {
                    safe_release(t.upload);
                }
            };
            sweep(g_map);
        }

        // Creates the texture and its upload buffer, and records the copy into
        // `list`. Called with the render lock held, from inside a frame.
        bool begin_map_upload(const mapdata::PendingImage& img, ID3D12GraphicsCommandList* list)
        {
            MapTexture& target = g_map;
            const DXGI_FORMAT format = img.channels == 1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            destroy_texture(target);

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(img.width);
            desc.Height = static_cast<UINT>(img.height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&target.tex))))
            {
                mm::logf(L"map texture: CreateCommittedResource({}x{}) failed", img.width, img.height);
                return false;
            }

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total = 0;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.Format = DXGI_FORMAT_UNKNOWN;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&target.upload))))
            {
                mm::logf(L"map texture: upload buffer of {} MB failed", total / (1024 * 1024));
                destroy_texture(target);
                return false;
            }

            void* mapped = nullptr;
            D3D12_RANGE none{0, 0};
            if (FAILED(target.upload->Map(0, &none, &mapped)) || mapped == nullptr)
            {
                mm::log(L"map texture: Map() of the upload buffer failed");
                destroy_texture(target);
                return false;
            }
            const std::size_t src_pitch = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.channels);
            for (UINT row = 0; row < num_rows; ++row)
            {
                std::memcpy(static_cast<std::uint8_t*>(mapped) + layout.Offset + row * layout.Footprint.RowPitch,
                            img.pixels.data() + row * src_pitch,
                            src_pitch);
            }
            target.upload->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = target.tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layout;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.tex;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);

            if (!g_srv_heap.alloc(target.srv_cpu, target.srv_gpu))
            {
                mm::log(L"map texture: no free SRV descriptor");
                destroy_texture(target);
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            // A layer is a single-channel coverage mask, so R is mapped to all four
            // components: ImGui's pixel shader then computes tint * (r, r, r, r), i.e.
            // "the floor colour, at this pixel's coverage". That is what lets the
            // runtime recolour a floor (bright for the current one, dim for the storey
            // below) and what makes an R8 texture - a quarter of the memory of RGBA8 -
            // enough for a full-resolution layer.
            srv.Shader4ComponentMapping =
                img.channels == 1 ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0)
                                  : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(target.tex, &srv, target.srv_cpu);

            target.width = img.width;
            target.height = img.height;
            target.channels = img.channels;
            target.bytes = static_cast<std::size_t>(total);
            target.chapter = img.chapter_key;
            target.ready = true;
            // The fence this frame will signal at the end of Present; once the GPU has
            // passed it, release_finished_uploads() frees the staging buffer.
            target.upload_fence = g_fence_value + 1;

            mm::logf(L"map texture: composite {}x{} uploaded (RGBA8, {} MB), chapter \"{}\"",
                     img.width,
                     img.height,
                     total / (1024 * 1024),
                     std::wstring(img.chapter_key.begin(), img.chapter_key.end()));
            return true;
        }

        //==============================================================================
        // The height slicer
        //==============================================================================
        //
        // The asset carries the actual Z of up to eight stacked surfaces per pixel plus
        // a reachable bit (mapdata::HeightMaps); the per-pixel rule is
        // srule::accumulate / alpha_for in src/slicerule.hpp:
        //
        //     a surface within floor_z_tolerance of the feet wins - the nearest one -
        //     and everything above that pixel is a ceiling nobody draws
        //     with none, the LOWEST surface up to shade_above_band_uu over the feet: a
        //     ledge or a piece of upper terrain, at shade_above_alpha
        //     with none of those, the highest surface below, at shade_below_alpha
        //
        // and `map_unreachable` decides whether a surface the reachability flood never
        // reached is dropped, drawn one rung dimmer, or drawn like any other.
        //
        // Colour is absolute height on the shade_lo_color -> shade_hi_color ramp - one
        // ramp for every class - so slopes, staircases and the storey below all read by
        // their Z rather than by a flat silhouette. Both slicers stretch the ramp between
        // percentiles of the Z their own cut drew (shade_range_pct_lo); the minimap eases
        // that span through srule::ease_range, the full map takes it as measured. The
        // only other hysteresis is the EMA on feetZ.
        //
        // A storey step is then drawn as a seam - srule::seam_factor darkens a pixel whose
        // left or up neighbour is more than kSeamStepUu away in Z - because two abutting
        // flat slabs are two flat tones and the ramp puts no edge between them.
        //
        // It runs on the CPU, at slice_hz, over only the window the minimap can show
        // (a ~512x512 source region), and uploads that window into a small dynamic
        // texture - the shader path's semantics without a custom root signature / PSO /
        // D3DCompile on a ReShade-wrapped DX12 swapchain.

        // Creates `count` dynamic RGBA textures of w x h, each with a persistently
        // mapped upload heap. Shared by the minimap (square) and the full map
        // (rectangular): resources, barriers and copy are identical, only the size and
        // the update policy differ.
        bool create_slice_set(SliceBuf* bufs, int count, int w, int h, const wchar_t* what)
        {
            destroy_slice_set(bufs, count);
            if (g_device == nullptr || w <= 0 || h <= 0)
            {
                return false;
            }

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(w);
            desc.Height = static_cast<UINT>(h);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total = 0;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.Format = DXGI_FORMAT_UNKNOWN;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&b.tex))))
                {
                    mm::logf(L"slice ({}): CreateCommittedResource({}x{} RGBA) failed", what, w, h);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&b.upload))))
                {
                    mm::logf(L"slice ({}): upload buffer of {} KB failed", what, total / 1024);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                void* mapped = nullptr;
                D3D12_RANGE none{0, 0};
                if (FAILED(b.upload->Map(0, &none, &mapped)) || mapped == nullptr)
                {
                    mm::logf(L"slice ({}): Map() of the upload buffer failed", what);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                b.mapped = static_cast<std::uint8_t*>(mapped);
                if (!g_srv_heap.alloc(b.srv_cpu, b.srv_gpu))
                {
                    mm::logf(L"slice ({}): no free SRV descriptor", what);
                    destroy_slice_set(bufs, count);
                    return false;
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(b.tex, &srv, b.srv_cpu);

                b.footprint = layout;
                b.rows = num_rows;
                b.w = w;
                b.h = h;
                b.in_copy_dest = true;
            }
            mm::logf(L"slice ({}): {} dynamic texture(s) of {}x{} RGBA created ({} KB each, {} KB of "
                     L"mapped upload memory)",
                     what,
                     count,
                     w,
                     h,
                     total / 1024,
                     (total * static_cast<UINT64>(count)) / 1024);
            return true;
        }

        // Render thread, and only with the slicer paused.
        bool create_slice_buffers(int size)
        {
            if (!create_slice_set(g_slice, kSliceBufs, size, size, L"minimap"))
            {
                destroy_slice_buffers();
                return false;
            }
            for (int i = 0; i < kSliceBufs; ++i)
            {
                g_slice_copy_pending[i].store(false);
                g_slice_in_flight[i].store(0);
            }
            g_slice_size = size;
            g_slice_next = 0;
            clear_slice_view();
            note_slice_buffers_changed();
            return true;
        }

        // How many source pixels the minimap can show, including the rotation corners
        // and a margin so the CLAMP sampler never smears an edge into view.
        int slice_size_for(const mm::Config& cfg, const mapdata::HeightMaps& hm, float half_px)
        {
            const double radius_uu = static_cast<double>(half_px) * static_cast<double>(cfg.zoom_uu_per_px) *
                                     1.4143; // the diagonal of the square the disc rotates in
            double want = 2.0 * radius_uu * hm.px_per_uu + 16.0;
            int size = static_cast<int>(std::ceil(want / 128.0)) * 128;
            if (size < kSliceMinPx)
            {
                size = kSliceMinPx;
            }
            if (size > kSliceMaxPx)
            {
                size = kSliceMaxPx;
            }
            return size;
        }

        // `state` holds srule's rank - class * 2 + reachable - so a reachable surface
        // beats an unreachable one of the same class, and height settles the rest.

        // Fills `dst` (size*size RGBA8, row pitch `pitch`) with the window whose
        // top-left source pixel is (x0, y0).
        //
        // Plane-major, deliberately: a pixel-major loop reads eight planes ~43 MB
        // apart at every pixel (~2.1 M cache misses for a 512x512 window), while
        // walking one plane's window to completion touches 512 contiguous uint16 per
        // row - the same arithmetic, ~30x fewer misses. The per-pixel decision state is
        // ~1.3 MB of scratch and fits in L2/L3.
        // `sx0` / `sy0` are the source pixel of the destination's top-left CORNER and
        // `src_step` is how many source pixels one destination pixel advances - 1.0 for
        // the minimap (the asset at its own resolution) and > 1 for the full map, which
        // decimates. Sampling is nearest, at the destination pixel's centre, so a 1-px
        // corridor can drop out at a decimating step - sub-pixel at that zoom anyway.
        //
        // The ramp's ends are always measured from what this cut drew - percentiles of
        // the surviving surfaces' Z. `range` non-null eases them into `*range` over
        // `dt_ms` and paints with the eased span (the minimap, which moves with the
        // player); null paints with the measured span as it stands (the full map, one
        // cut over the whole visible chapter).
        void slice_region(const mapdata::HeightMaps& hm, double sx0, double sy0, double src_step, int w, int h,
                          std::uint8_t* dst, UINT pitch, float feet, const SliceStyle& st, SliceScratch& sc,
                          SliceCounts& counts, srule::RangeState* range, float dt_ms)
        {
            counts = SliceCounts{};

            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            if (sc.state.size() != n)
            {
                sc.state.assign(n, 0);
                sc.best_d.assign(n, 0.0f);
            }
            else
            {
                std::memset(sc.state.data(), 0, n);
            }
            std::uint8_t* state = sc.state.data();
            float* best_d = sc.best_d.data();

            // Destination -> source index, computed once instead of once per plane, and
            // where the bounds check lives: -1 means outside the asset, i.e. transparent.
            if (sc.col_x.size() != static_cast<std::size_t>(w))
            {
                sc.col_x.resize(static_cast<std::size_t>(w));
                sc.gather.resize(static_cast<std::size_t>(w));
            }
            if (sc.row_y.size() != static_cast<std::size_t>(h))
            {
                sc.row_y.resize(static_cast<std::size_t>(h));
            }
            int* col_x = sc.col_x.data();
            int* row_y = sc.row_y.data();
            for (int col = 0; col < w; ++col)
            {
                const int i = static_cast<int>(std::floor(sx0 + (static_cast<double>(col) + 0.5) * src_step));
                col_x[col] = (i >= 0 && i < hm.width) ? i : -1;
            }
            for (int row = 0; row < h; ++row)
            {
                const int i = static_cast<int>(std::floor(sy0 + (static_cast<double>(row) + 0.5) * src_step));
                row_y[row] = (i >= 0 && i < hm.height) ? i : -1;
            }

            const float z0 = hm.z_min;
            const float step = hm.z_step();
            // Hoisted: every code in the window shares this one asset-wide answer.
            const bool has_reach = hm.has_reachability;
            const int planes = hm.count < mapdata::kMaxSurfaces ? hm.count : mapdata::kMaxSurfaces;
            counts.surfaces = planes;

            for (int k = 0; k < planes; ++k)
            {
                if (hm.plane_empty(k))
                {
                    continue;
                }
                std::uint16_t* gathered = sc.gather.data();
                for (int row = 0; row < h; ++row)
                {
                    const int sy = row_y[row];
                    if (sy < 0)
                    {
                        continue;
                    }
                    // Gather the row's codes out of the block store. `false` means no
                    // surface anywhere on this row of this plane - most rows in the
                    // deeper planes, and skipping them is where the sparse store pays
                    // back in speed. Columns outside the asset (col_x < 0) and absent
                    // blocks both come back as code 0.
                    if (!hm.gather_row(k, sy, col_x, w, gathered))
                    {
                        continue;
                    }
                    const std::size_t out_base = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                    for (int col = 0; col < w; ++col)
                    {
                        const std::uint16_t raw = gathered[col];
                        const std::uint16_t code = mapdata::z_code(raw);
                        if (code == 0)
                        {
                            continue; // no surface in this slot here
                        }
                        const bool reachable = !has_reach || (raw & mapdata::kReachableBit) != 0;
                        const float z = z0 + (static_cast<float>(code) - 1.0f) * step;
                        const std::size_t i = out_base + static_cast<std::size_t>(col);
                        srule::accumulate(state[i], best_d[i], z - feet, reachable, st);
                    }
                }
            }

            // The ramp this cut paints with: percentiles of the Z of the pixels that
            // survived the pass above, so the culled ceiling, one deep pit and one high
            // gallery are all outside the ends. Measured before any of it is coloured.
            SliceStyle style = st;
            srule::ZHistogram hist;
            hist.reset(hm.z_min, hm.z_max);
            for (std::size_t i = 0; i < n; ++i)
            {
                if (state[i] != 0)
                {
                    hist.add(feet + best_d[i]);
                }
            }
            // The full map equalises: `t` is the cut's own CDF, so the ramp is spent in
            // proportion to the area at each height. The minimap leaves it null and
            // stays linear between the percentile ends.
            const srule::ZHistogram* eq = nullptr;
            if (st.equalize && hist.total > 0)
            {
                hist.build_cdf();
                eq = &hist;
            }
            float raw_lo = 0.0f;
            float raw_hi = 0.0f;
            // An equalised ramp is bounded by the drawn set itself, so its ends are that
            // set's extremes rather than the trimmed percentiles: p0..p100 is then what
            // the read-out reports, which is the span the picture really covers.
            const float pct = st.equalize ? 0.0f : st.range_pct_lo;
            const bool measured = srule::hist_range(hist, pct, raw_lo, raw_hi);
            bool have_ramp = false;
            if (range != nullptr)
            {
                if (measured)
                {
                    srule::ease_range(*range, raw_lo, raw_hi, dt_ms, st);
                }
                if (range->valid)
                {
                    style.z_lo = range->lo;
                    style.z_hi = range->hi;
                    have_ramp = true;
                }
            }
            else if (measured)
            {
                if (!st.equalize)
                {
                    srule::widen_range(raw_lo, raw_hi, st.min_range_uu);
                }
                style.z_lo = raw_lo;
                style.z_hi = raw_hi;
                have_ramp = true;
            }
            if (!have_ramp)
            {
                // The cut drew nothing and there is no carried range to keep - the first
                // cut of a session onto empty ground. Fall back to the asset's own Z
                // range: SliceStyle's 0..1 would be a one-uu ramp over a whole chapter,
                // and the read-out would report it as the truth.
                style.z_lo = hm.z_min;
                style.z_hi = hm.z_max;
            }
            counts.z_lo = style.z_lo;
            counts.z_hi = style.z_hi;

            for (int row = 0; row < h; ++row)
            {
                std::uint8_t* out = dst + static_cast<std::size_t>(row) * pitch;
                const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                for (int col = 0; col < w; ++col)
                {
                    std::uint8_t* px = out + static_cast<std::size_t>(col) * 4;
                    const std::size_t i = base + static_cast<std::size_t>(col);
                    const std::uint8_t cls = srule::rank_class(state[i]);
                    if (cls == srule::kClassNone)
                    {
                        px[0] = px[1] = px[2] = px[3] = 0;
                        continue;
                    }
                    const bool reachable = srule::rank_reachable(state[i]);
                    float r = 0.0f;
                    float g = 0.0f;
                    float b = 0.0f;
                    const float z = feet + best_d[i];
                    srule::class_rgb(z, cls, style, r, g, b, eq);
                    // The seam, off the two neighbours this scan has already decided:
                    // `state` and `best_d` hold the whole window, so it costs two loads
                    // and no second pass over the height planes.
                    const bool has_left = col > 0;
                    const bool has_up = row > 0;
                    const std::size_t left = i - 1;
                    const std::size_t up = i - static_cast<std::size_t>(w);
                    const float seam =
                        srule::seam_factor(z,
                                           has_left ? feet + best_d[left] : z,
                                           has_left && state[left] != 0,
                                           has_up ? feet + best_d[up] : z,
                                           has_up && state[up] != 0);
                    r *= seam;
                    g *= seam;
                    b *= seam;
                    const auto ch = [](float v) {
                        const float x = v + 0.5f;
                        return static_cast<std::uint8_t>(x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x));
                    };
                    px[0] = ch(r);
                    px[1] = ch(g);
                    px[2] = ch(b);
                    px[3] = static_cast<std::uint8_t>(srule::alpha_for(cls, reachable, style) * 255.0f + 0.5f);
                    if (cls == srule::kClassFloor)
                    {
                        ++counts.opaque;
                    }
                    else if (cls == srule::kClassBelow)
                    {
                        ++counts.dim;
                    }
                    else
                    {
                        ++counts.faint;
                    }
                    if (!reachable)
                    {
                        ++counts.unreachable;
                    }
                }
            }
        }

        // The minimap's window: the asset at 1:1, at an integral source origin. Its
        // ramp follows the window, so `dt_ms` - how long since the previous cut - paces
        // the easing.
        void slice_window(const mapdata::HeightMaps& hm, int x0, int y0, int size, std::uint8_t* dst, UINT pitch,
                          float feet, const SliceStyle& st, float dt_ms)
        {
            SliceCounts counts{};
            slice_region(hm, static_cast<double>(x0), static_cast<double>(y0), 1.0, size, size, dst, pitch, feet,
                         st, g_slice_scratch, counts, &g_slice_range, dt_ms);
            g_slice_opaque = counts.opaque;
            g_slice_dim = counts.dim;
            g_slice_faint = counts.faint;
            g_slice_unreach = counts.unreachable;
            g_slice_surfaces = counts.surfaces;
            g_slice_z_lo = counts.z_lo;
            g_slice_z_hi = counts.z_hi;
        }

        // The slice style, built from the config. Both slicers use it and the loop
        // thread builds its own copy, so it lives in one place.
        SliceStyle style_from(const mm::Config& cfg)
        {
            SliceStyle st{};
            st.lo_r = cfg.shade_lo_r;
            st.lo_g = cfg.shade_lo_g;
            st.lo_b = cfg.shade_lo_b;
            st.hi_r = cfg.shade_hi_r;
            st.hi_g = cfg.shade_hi_g;
            st.hi_b = cfg.shade_hi_b;
            st.gamma = cfg.shade_gamma;
            st.tol = cfg.floor_z_tolerance;
            st.above_band = cfg.show_adjacent_floors ? cfg.shade_above_band_uu : 0.0f;
            st.a_below = cfg.show_adjacent_floors ? cfg.shade_below_alpha : 0.0f;
            st.a_above = cfg.show_adjacent_floors ? cfg.shade_above_alpha : 0.0f;
            st.range_pct_lo = cfg.shade_range_pct_lo;
            st.min_range_uu = cfg.shade_min_range_uu;
            st.range_smooth_ms = static_cast<float>(cfg.shade_range_smooth_ms);
            st.unreachable = cfg.map_unreachable;
            return st;
        }

        // Render thread. The part of the minimap slice that must happen inside the
        // frame: size the buffers (creation is the render thread's alone) and tell the
        // slicer the minimap is drawing and how big a window it needs. The cut itself
        // runs on the loop thread - see slice_minimap_step().
        //
        // Returns true when a buffer is available to draw. The window carries margin,
        // so a cut that has not landed yet is invisible.
        bool plan_slice(const mm::Config& cfg, const mapdata::Chapter& ch, float half_px, std::uint64_t now)
        {
            if (!ch.has_heights())
            {
                g_slice_want_px.store(0, std::memory_order_relaxed);
                return false;
            }
            const mapdata::HeightMaps& hm = *ch.heights;

            const int want = slice_size_for(cfg, hm, half_px);
            if (want != g_slice_size)
            {
                // The buffers are the render thread's to allocate and the slicer may be
                // writing into the old ones; if it will not stand down, the current size
                // is kept for this frame.
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    wait_for_gpu(); // the old buffers may still be in flight
                    const bool ok = create_slice_buffers(want);
                    slicer_pause_end();
                    if (!ok)
                    {
                        g_slice_want_px.store(0, std::memory_order_relaxed);
                        return false;
                    }
                }
            }
            g_slice_want_px.store(g_slice_size, std::memory_order_relaxed);
            g_slice_want_ms.store(now, std::memory_order_relaxed);
            return slice_view().shown >= 0;
        }

        // Loop thread. The actual cut: pace it, take the next buffer if the GPU is done
        // with it, fill the mapped upload heap and publish the result.
        //
        // The height planes are re-read from `mapdata` on every call and never cached
        // across one: a chapter switch retires the old planes and frees them after a
        // grace period, so a pointer held from the previous slice could be freed memory.
        void slice_minimap_step(std::uint64_t now)
        {
            const int want = g_slice_want_px.load(std::memory_order_relaxed);
            if (want <= 0 || want != g_slice_size)
            {
                return; // the minimap is not drawing, or the render thread is resizing
            }
            // The minimap stopped drawing (the overlay hid, the map opened) and nobody
            // has asked since: stop cutting rather than burning a millisecond a loop.
            if (now - g_slice_want_ms.load(std::memory_order_relaxed) > 500)
            {
                return;
            }

            mm::Snapshot snap{};
            if (!mm::read_snapshot(snap) || !snap.has_pawn)
            {
                return;
            }
            const mapdata::Chapter* ch = mapdata::chapter_ptr_for(snap.x, snap.y);
            if (ch == nullptr || !ch->has_heights())
            {
                return;
            }
            const mapdata::HeightMaps& hm = *ch->heights;

            const mm::Config& cfg = mm::cfg_cached();

            // ---- feet Z, EMA-smoothed ------------------------------------------------
            const float raw_feet = static_cast<float>(snap.z) - cfg.player_z_offset;
            static std::uint64_t last_teleport = 0;
            const bool teleported = snap.teleport_ms != 0 && snap.teleport_ms != last_teleport;
            if (teleported)
            {
                last_teleport = snap.teleport_ms;
            }
            if (!g_feet_z_valid || teleported)
            {
                g_feet_z = raw_feet;
                g_feet_z_valid = true;
                g_slice_last_ms = 0;        // a teleport must re-slice immediately
                g_slice_range.valid = false; // and land on the new storey's ramp at once
            }
            else
            {
                const float tau = cfg.feet_z_smooth_ms > 1 ? static_cast<float>(cfg.feet_z_smooth_ms) : 1.0f;
                // The loop runs at roughly frame rate; the exact dt does not matter for
                // a 100 ms EMA.
                const float a = 16.0f / tau;
                g_feet_z += (raw_feet - g_feet_z) * (a > 1.0f ? 1.0f : a);
            }

            const int period = cfg.slice_hz > 0 ? 1000 / cfg.slice_hz : 80;
            if (g_slice_last_ms != 0 && now - g_slice_last_ms < static_cast<std::uint64_t>(period))
            {
                return; // the previous window is still good enough
            }

            SliceBuf& b = g_slice[g_slice_next];
            if (b.tex == nullptr || b.mapped == nullptr || b.w <= 0)
            {
                return;
            }
            const std::uint64_t in_flight = g_slice_in_flight[g_slice_next].load(std::memory_order_acquire);
            ID3D12Fence* fence = g_fence;
            if (in_flight != 0 && fence != nullptr && fence->GetCompletedValue() < in_flight)
            {
                // The GPU is still sampling this one. Never stall for the map: keep
                // showing the other buffer and try again on the next iteration.
                ++g_slice_skipped;
                return;
            }

            double pxc = 0.0;
            double pyc = 0.0;
            hm.to_px(snap.x, snap.y, pxc, pyc);
            const int x0 = static_cast<int>(std::lround(pxc)) - b.w / 2;
            const int y0 = static_cast<int>(std::lround(pyc)) - b.w / 2;

            const SliceStyle st = style_from(cfg);
            const float dt_ms = g_slice_last_ms == 0
                                    ? 0.0f
                                    : static_cast<float>(now - g_slice_last_ms);

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            slice_window(hm, x0, y0, b.w, b.mapped + b.footprint.Offset, b.footprint.Footprint.RowPitch,
                         g_feet_z, st, dt_ms);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            double g_slice_last_cut_ms = 0.0;
            if (freq > 0)
            {
                const double ms =
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq);
                g_slice_last_cut_ms = ms;
                g_slice_ms = g_slice_ms == 0.0 ? ms : g_slice_ms * 0.8 + ms * 0.2;
                if (ms > g_slice_ms_peak)
                {
                    g_slice_ms_peak = ms;
                }
            }

            if (g_pf_slice < 0)
            {
                g_pf_slice = mm::perf_register("minimap slice cut", perf::Thread::Loop);
            }
            mm::perf_record_ms(g_pf_slice, g_slice_last_cut_ms);

            g_slice_copy_pending[g_slice_next].store(true, std::memory_order_release);
            {
                spin::SpinGuard guard(g_slice_view_lock);
                g_slice_view.shown = g_slice_next;
                // The window's own world -> pixel mapping (see mapdata::HeightMaps::to_px):
                //   px_local = px - x0 = (Y - (min_y + x0/s)) * s
                //   py_local = py - y0 = ((max_x - y0/s) - X) * s
                g_slice_view.px_per_uu = hm.px_per_uu;
                g_slice_view.min_y = hm.min_y + static_cast<double>(x0) / hm.px_per_uu;
                g_slice_view.max_x = hm.max_x - static_cast<double>(y0) / hm.px_per_uu;
            }
            g_slice_next = (g_slice_next + 1) % kSliceBufs;
            g_slice_last_ms = now;
            {
                // The first slice is where a map asset, the CPU slicer and a D3D12
                // upload heap are first all live at once.
                static bool first = true;
                if (first)
                {
                    first = false;
                    crumb::stage(crumb::kFirstSlice);
                }
            }
        }

        // Main-menu self-test: the slicer otherwise only runs inside draw_minimap,
        // which is gated on a gameplay pawn. This allocates the buffers and slices one
        // window at the chapter's centre, so a Lobby log line proves the whole path
        // (texture + mapped upload heap created, the loop ran, what it cost) and the
        // in-world first frame has no allocation hitch.
        //
        // Render thread, during set-up. It creates the buffers and writes into one of
        // them, so the loop-thread slicer must be held off for its duration.
        void slice_selftest()
        {
            if (!slicer_pause_begin(kSlicerPauseMs))
            {
                return; // the next frame will try again
            }
            struct Resume
            {
                ~Resume()
                {
                    slicer_pause_end();
                }
            } resume;
            const mm::Config cfg = mm::config();
            std::vector<mapdata::Chapter> list = mapdata::chapters();
            const mapdata::Chapter* ch = nullptr;
            for (const mapdata::Chapter& c : list)
            {
                if (c.has_heights())
                {
                    ch = &c;
                    break;
                }
            }
            // Size it for the shipping window so the in-world path needs no realloc.
            const float side = (std::max)(72.0f, cfg.size_frac * static_cast<float>(g_height));
            const int size = ch != nullptr ? slice_size_for(cfg, *ch->heights, side * 0.5f) : 512;
            if (!create_slice_buffers(size))
            {
                return;
            }
            if (ch == nullptr)
            {
                mm::log(L"slice: self-test skipped - no chapter with height maps is loaded");
                return;
            }

            const mapdata::HeightMaps& hm = *ch->heights;
            SliceBuf& b = g_slice[0];
            const SliceStyle st = style_from(cfg);

            LARGE_INTEGER t0{};
            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t0);
            // A window that has geometry in it, at a feet Z taken from that geometry -
            // a window over empty map comes out transparent and proves nothing about
            // the colour path.
            const int wx0 = (hm.width - b.w) / 2;
            const int wy0 = (hm.height - b.w) / 2;
            float probe_z = (hm.z_min + hm.z_max) * 0.5f;
            int sx0 = wx0;
            int sy0 = wy0;
            {
                // The block store knows where its first lit pixel is.
                int px = 0;
                int py = 0;
                std::uint16_t code = 0;
                if (hm.first_lit(0, px, py, code))
                {
                    probe_z = hm.decode(code);
                    sx0 = px - b.w / 2;
                    sy0 = py - b.w / 2;
                }
            }
            g_slice_range.valid = false; // the probe window is nobody's storey
            slice_window(hm, sx0, sy0, b.w, b.mapped + b.footprint.Offset,
                         b.footprint.Footprint.RowPitch, probe_z, st, 0.0f);
            ::QueryPerformanceCounter(&t1);
            const std::int64_t freq = qpc_freq();
            const double ms = freq > 0 ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                             static_cast<double>(freq)
                                       : 0.0;
            // Deliberately not marked needs_copy: nothing may be drawn at the main menu.
            mm::logf(L"slice: self-test sliced a {}x{} window of \"{}\" at source ({}, {}), feet Z "
                     L"{:.0f}, over {} surface(s) in {:.2f} ms (opaque {}, dim {}, faint {}) - the CPU "
                     L"path and the dynamic texture both work",
                     b.w,
                     b.w,
                     std::wstring(ch->key.begin(), ch->key.end()),
                     sx0 + b.w / 2,
                     sy0 + b.w / 2,
                     static_cast<double>(probe_z),
                     hm.count,
                     ms,
                     g_slice_opaque,
                     g_slice_dim,
                     g_slice_faint);
            g_slice_opaque = 0;
            g_slice_dim = 0;
            g_slice_faint = 0;
            g_slice_unreach = 0;
            g_slice_range.valid = false;
        }

        // Render thread, inside a frame, after the command list has been reset: record
        // the copy for whichever buffer the CPU just filled.
        void record_slice_copies(ID3D12GraphicsCommandList* list, SliceBuf* bufs,
                                std::atomic<bool>* pending, std::atomic<std::uint64_t>* in_flight,
                                int count)
        {
            for (int i = 0; i < count; ++i)
            {
                SliceBuf& b = bufs[i];
                // exchange, not load+store: the slicer may fill this buffer again the
                // instant the flag is cleared, and that next fill must not be lost.
                if (b.tex == nullptr || !pending[i].exchange(false))
                {
                    continue;
                }
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = b.tex;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (!b.in_copy_dest)
                {
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    list->ResourceBarrier(1, &barrier);
                }

                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = b.tex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = b.upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = b.footprint;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &barrier);
                b.in_copy_dest = false;
                // The upload heap this copy reads may not be rewritten until the GPU
                // has run it, so it claims the fence this frame is about to signal -
                // otherwise the loop thread could refill the heap between the recording
                // and the execution of the copy and the texture would show a window the
                // mapping does not describe. A buffer that is merely SAMPLED is stamped
                // at the end of the frame, alongside the fence signal.
                in_flight[i].store(g_fence_value + 1, std::memory_order_release);
            }
        }

        void record_slice_copy(ID3D12GraphicsCommandList* list)
        {
            record_slice_copies(list, g_slice, g_slice_copy_pending, g_slice_in_flight, kSliceBufs);
            record_slice_copies(list, g_mslice, g_mslice_copy_pending, g_mslice_in_flight, kMapSliceBufs);
        }

        //==============================================================================
        // Drawing one image (the composite or one floor layer)
        //==============================================================================

        void draw_srv(ImDrawList* dl, D3D12_GPU_DESCRIPTOR_HANDLE srv, const UvMap& uv, MiniGeom g, ImU32 col,
                      bool round, float x0, float y0, float side)
        {
            if (srv.ptr == 0)
            {
                return;
            }
            g.uv = uv;
            const ImTextureRef tex{static_cast<ImTextureID>(srv.ptr)};
            if (round)
            {
                add_image_circle(dl, tex, g, col);
                return;
            }
            const ImVec2 p0{x0, y0};
            const ImVec2 p1{x0 + side, y0 + side};
            dl->AddImageQuad(tex,
                             p0,
                             ImVec2{p1.x, p0.y},
                             p1,
                             ImVec2{p0.x, p1.y},
                             uv_at(g, -g.half, -g.half),
                             uv_at(g, g.half, -g.half),
                             uv_at(g, g.half, g.half),
                             uv_at(g, -g.half, g.half),
                             col);
        }

        void draw_image(ImDrawList* dl, const MapTexture& t, const UvMap& uv, const MiniGeom& g, ImU32 col,
                        bool round, float x0, float y0, float side)
        {
            if (!t.ready)
            {
                return;
            }
            draw_srv(dl, t.srv_gpu, uv, g, col, round, x0, y0, side);
        }

        // The mod's own wide strings (the log is wide) rendered for ImGui, which is
        // UTF-8. Key names, chord names and stage names are pure ASCII, so this is one
        // explicit cast per character - std::string(w.begin(), w.end()) warns (C4244)
        // and this mod ships warning-free.
        std::string wide_to_ascii(const std::wstring& wide)
        {
            std::string out;
            out.reserve(wide.size());
            for (const wchar_t c : wide)
            {
                out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
            }
            return out;
        }

        // A binding's display name, modifier prefix included ("F2", "CTRL+M").
        std::string key_name_ascii(int binding)
        {
            return wide_to_ascii(mm::key_name(binding));
        }

        // The key hints, built from the config and never from the defaults, so a
        // rebound key is what the player is told. One builder for both the F2 panel and
        // the full map's footer.
        std::string bindings_hint(const mm::Config& cfg)
        {
            std::string s = std::format("{} panel   {} full map ({} recentres)   {} minimap zoom   "
                                        "{} reload",
                                        key_name_ascii(cfg.panel_key),
                                        key_name_ascii(cfg.map_key),
                                        key_name_ascii(cfg.map_recenter_key),
                                        key_name_ascii(cfg.zoom_key),
                                        key_name_ascii(cfg.reload_key));
            if (cfg.highlight_enabled)
            {
                s += std::format("   {} {} x-ray",
                                 cfg.highlight_mode == mm::HighlightMode::Hold ? "hold" : "press",
                                 key_name_ascii(cfg.highlight_key));
                if (cfg.highlight_gamepad &&
                    (cfg.highlight_pad_mask != 0 || cfg.highlight_pad_lt || cfg.highlight_pad_rt))
                {
                    s += " (pad " +
                         wide_to_ascii(mm::pad_chord_name(cfg.highlight_pad_mask, cfg.highlight_pad_lt,
                                                          cfg.highlight_pad_rt)) +
                         ")";
                }
            }
            return s;
        }
    } // namespace ovl
} // namespace overlay
