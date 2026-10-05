//==============================================================================
// overlay_present.cpp - how the overlay's finished frame reaches the screen
//==============================================================================
//
// The overlay draws into textures of its own (overlay_d3d12.cpp). This module puts the
// newest finished one onto the game's frame: one full-screen premultiplied-alpha blend into
// the back buffer of the LOWEST swapchain, recorded and submitted from inside that
// swapchain's own Present, on the queue that swapchain was created with.
//
// The lowest swapchain is the one that actually flips. Under NVIDIA Streamline the engine
// holds a proxy and `slGetNativeInterface` names what is beneath it (DXGI's own, or a
// ReShade wrapper); with no interposer it is the engine's swapchain itself. Its Present runs
// once per DISPLAYED frame - frame generation's frames included, on frame generation's own
// pacer thread - so the overlay is on every frame the player sees and is never interpolated.
//
// The queue is the whole question. With DLSS-G loaded the back buffers belong to frame
// generation and may only be written from its present queue: a write from the game's own
// render queue is refused with DXGI_ERROR_ACCESS_DENIED and the device goes with it (1.0.x;
// measured again in WM-105). The queue a swapchain was created with sits in the swapchain
// object, so it is found the way hudhook finds it: every queue the process submits to is
// noted by a hook on ExecuteCommandLists, and the one whose pointer appears in the first
// words of the swapchain object is that swapchain's.
//
// Nothing is composed beside the game's frame, so DWM has nothing of ours to compose: the
// game keeps its independent flip whether or not the display offers an overlay plane.
//
// THREADS. `comp_create`, `comp_bind_targets`, `comp_unbind_targets`, `comp_release`,
// `comp_pick_target` and `comp_publish` belong to whoever holds `g_render_lock`.
// `comp_present` runs on whichever thread presents the lowest swapchain - the render
// thread with no interposer, DLSS-G's pacer with one - and touches nothing but this
// module's own objects, the render fence and the targets it was handed. It never waits:
// a busy lock, an unfinished frame or a busy slot means this present goes out without the
// overlay and the next one tries again. The render-lock side takes the same lock to take
// anything away from it, and that wait is bounded by one recording.
//

#include "overlay_internal.hpp"

#include <d3dcompiler.h>

#include "mem.hpp"

namespace overlay
{
    namespace ovl
    {
        namespace
        {
            using EclFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
            using GetNativeFn = int(__cdecl*)(void*, void**);

            // BGRA because every surface tool reads it, premultiplied because ImGui's blend
            // state over the frame's transparent-black clear already emits it.
            constexpr DXGI_FORMAT kCompFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            constexpr int kSlots = 8;               // blend recordings in flight
            constexpr int kMaxQueues = 16;          // queues this process submits to
            constexpr std::size_t kScanWords = 512; // hudhook's reach into the swapchain object
            constexpr unsigned kLockMs = 500;
            constexpr DWORD kBlendWaitMs = 1000;

            //------------------------------------------------------------------
            // Hooks. ExecuteCommandLists is a MinHook detour, created once per
            // process; the master switch's enable/disable of MH_ALL_HOOKS covers
            // it. The lowest swapchain's methods are its VTABLE slots instead: an
            // overlay that inline-hooks DXGI's Present (RTSS does) rewrites the
            // function's first bytes over ours and can leave us out of the chain,
            // while every caller of the swapchain still goes through the vtable.
            //------------------------------------------------------------------
            EclFn o_ecl = nullptr;
            void* g_ecl_addr = nullptr;

            // One vtable slot of ours. `installed` stays true for as long as our detour
            // is anywhere in the chain - also when someone patched the slot after us and
            // it can no longer be given back.
            struct VtSlot
            {
                void** slot = nullptr;
                void* original = nullptr;
                bool installed = false;
            };
            VtSlot g_vt_present;
            VtSlot g_vt_present1;
            VtSlot g_vt_colour_space;
            PresentFn o_low_present = nullptr;
            Present1Fn o_low_present1 = nullptr;

            struct QueueSeen
            {
                std::atomic<ID3D12CommandQueue*> q{nullptr};
                std::atomic<std::uint64_t> calls{0};
            };
            QueueSeen g_queues[kMaxQueues];

            //------------------------------------------------------------------
            // Objects. Created and released under the render lock; used by the
            // consumer only inside `g_present_lock` while `g_live`.
            //------------------------------------------------------------------
            ID3D12Device* g_dev = nullptr; // not owned: the overlay's adopted device
            ID3D12CommandQueue* g_comp_queue = nullptr;
            ID3D12RootSignature* g_root = nullptr;
            ID3DBlob* g_vs = nullptr;
            ID3DBlob* g_ps = nullptr;
            ID3D12PipelineState* g_pso = nullptr;
            DXGI_FORMAT g_pso_format = DXGI_FORMAT_UNKNOWN;
            // The pipeline a format change replaced, kept until the blend fence passes the
            // last blend recorded with it.
            ID3D12PipelineState* g_pso_retired = nullptr;
            std::uint64_t g_pso_retired_at = 0;
            ID3D12DescriptorHeap* g_blend_srv = nullptr;
            ID3D12DescriptorHeap* g_blend_rtv = nullptr;
            UINT g_blend_srv_step = 0;
            UINT g_blend_rtv_step = 0;
            ID3D12CommandAllocator* g_alloc[kSlots] = {};
            std::uint64_t g_slot_value[kSlots] = {};
            ID3D12GraphicsCommandList* g_list = nullptr;
            ID3D12Fence* g_blend_fence = nullptr;
            HANDLE g_blend_event = nullptr;
            std::uint64_t g_blend_value = 0;
            std::uint64_t g_recordings = 0;
            ID3D12Resource* g_bound[kTargets] = {}; // not owned: overlay_d3d12's targets
            UINT g_blend_w = 0;
            UINT g_blend_h = 0;

            spin::Spinlock g_present_lock;
            bool g_live = false; // under g_present_lock
            std::atomic<void*> g_low_sc{nullptr};
            bool g_low_is_engine = false;
            // Whether the swapchain that flips was found beneath a frame generator's proxy,
            // which may make a new one on a resize: then each resize looks for it again.
            bool g_low_beneath_proxy = false;
            bool g_low_just_found = false; // comp_create's own first bind needs no second look
            ID3D12CommandQueue* g_present_queue = nullptr; // not owned; under g_present_lock
            int g_current = -1;                            // under g_present_lock
            // What the back buffer is, for the shader's mode. The colour space is what the
            // game last set on the swapchain that flips, -1 until a SetColorSpace1 of ours
            // has seen one; before that, a 10-bit buffer on an output in HDR mode is taken
            // as HDR10, which is how this engine presents HDR.
            std::atomic<int> g_colour_space{-1};
            bool g_output_hdr = false;
            float g_white_nits = 200.0f;
            int g_mode_logged = -1; // under g_present_lock
            int blend_mode(DXGI_FORMAT format);
            bool g_queue_missing_logged = false;
            bool g_format_logged = false;

            //------------------------------------------------------------------
            // The target ring. A target is the render thread's when it is not
            // busy (neither on screen nor in either mailbox slot) and its last
            // blend has completed on the GPU.
            //
            // Two slots, because a present that follows the publish at once - the
            // render thread's own, with frame generation off - always finds the
            // newest frame unfinished on the GPU: `g_mailbox` holds the newest,
            // `g_waiting` the one it displaced, and a present takes the newest of
            // the two that has finished. A slot changes hands only by exchange or
            // compare-exchange, so whoever takes a frame out of one owns it.
            //------------------------------------------------------------------
            constexpr int kMailboxIndexBits = 3;
            constexpr std::uint64_t kMailboxIndexMask = (1ull << kMailboxIndexBits) - 1ull;
            std::atomic<std::uint64_t> g_mailbox{0};
            std::atomic<std::uint64_t> g_waiting{0};
            std::atomic<bool> g_target_busy[kTargets]{};
            std::atomic<std::uint64_t> g_target_blend[kTargets]{};
            // What the frame in a target covers: nothing (skip the blend) or a rect.
            std::atomic<bool> g_target_empty[kTargets]{};
            std::atomic<std::uint64_t> g_target_rect[kTargets]{};

            std::atomic<int> g_phase{fc::Full};
            std::atomic<std::uint64_t> g_skipped{0};
            std::atomic<std::uint64_t> g_dropped{0};
            std::atomic<std::uint64_t> g_blends{0};
            std::atomic<std::uint64_t> g_gpu_waits{0};
            // The present side's own account, read every 10 s: how many presents of the
            // swapchain that flips came through, and what each one that went out without the
            // overlay lacked.
            std::uint64_t g_presents = 0;   // under g_present_lock
            std::uint64_t g_adoptions = 0;  // under g_present_lock
            std::uint64_t g_adopted_waiting = 0; // under g_present_lock: of those, the older slot's
            std::uint64_t g_no_frame = 0;   // under g_present_lock: nothing adopted yet
            std::uint64_t g_empty = 0;      // under g_present_lock: the frame drew nothing
            std::atomic<std::uint64_t> g_lock_busy{0};
            std::uint64_t g_report_ms = 0;  // under g_present_lock
            // When the render thread first published a frame that drew something, and the
            // engine's present count then; 0 until then.
            std::atomic<std::uint64_t> g_first_drawn_ms{0};
            std::atomic<std::uint64_t> g_first_drawn_presents{0};
            // The once-per-adoption account of the path, written by whichever side gets there
            // first: the present side when its presents arrive, the render thread when they
            // do not.
            std::atomic<bool> g_path_reported{false};
            constexpr std::uint64_t kPathReportMs = 10000;
            // Every call our hooks on the lowest swapchain's Present and Present1 see, and of
            // those the ones on the swapchain that flips, with the last thread that made one.
            std::atomic<std::uint64_t> g_low_calls{0};
            std::atomic<std::uint64_t> g_low1_calls{0};
            std::atomic<std::uint64_t> g_low_on_ours{0};
            std::atomic<unsigned long> g_low_tid{0};
            int g_pf_blend = -1; // registered by the presenting thread itself

            const char* const kVs =
                "float4 main(uint id : SV_VertexID) : SV_Position {"
                "  float2 uv = float2((id << 1) & 2, id & 2);"
                "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
            // Load, not Sample: the target and the back buffer are the same size, pixel for
            // pixel. The overlay is SDR sRGB; what it becomes depends on the back buffer, the
            // mapping DWM made for the composition surface:
            //   0 - an SDR buffer: as it is;
            //   1 - scRGB (FP16, linear, 1.0 = 80 nits): to linear, SDR white at `white` nits;
            //   2 - HDR10 (ST.2084, BT.2020): to linear, to BT.2020, SDR white at `white`
            //       nits, PQ-encoded.
            // Premultiplied in and out: the colour is converted unpremultiplied.
            const char* const kPs =
                "Texture2D<float4> t : register(t0);"
                "cbuffer c : register(b0) { uint mode; float white; };"
                "float3 lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }"
                "float3 pq(float3 l) {"
                "  float3 y = pow(saturate(l), 0.1593017578125);"
                "  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375); }"
                "float4 main(float4 p : SV_Position) : SV_Target {"
                "  float4 c = t.Load(int3(p.xy, 0));"
                "  if (mode == 0 || c.a <= 0) { return c; }"
                "  float3 l = lin(saturate(c.rgb / c.a));"
                "  if (mode == 1) { return float4(l * (white / 80.0) * c.a, c.a); }"
                "  float3 w = mul(float3x3(0.6274040, 0.3292820, 0.0433136,"
                "                          0.0690970, 0.9195400, 0.0113612,"
                "                          0.0163916, 0.0880132, 0.8955950), l);"
                "  return float4(pq(w * (white / 10000.0)) * c.a, c.a); }";

            std::uint64_t pack_rect(const D3D12_RECT& r)
            {
                return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(r.left)) << 48)
                       | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(r.top)) << 32)
                       | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(r.right)) << 16)
                       | static_cast<std::uint64_t>(static_cast<std::uint16_t>(r.bottom));
            }

            D3D12_RECT unpack_rect(std::uint64_t v)
            {
                return D3D12_RECT{static_cast<LONG>((v >> 48) & 0xFFFF), static_cast<LONG>((v >> 32) & 0xFFFF),
                                  static_cast<LONG>((v >> 16) & 0xFFFF), static_cast<LONG>(v & 0xFFFF)};
            }

            void note_queue(ID3D12CommandQueue* q)
            {
                for (QueueSeen& s : g_queues)
                {
                    ID3D12CommandQueue* cur = s.q.load(std::memory_order_acquire);
                    if (cur == nullptr)
                    {
                        ID3D12CommandQueue* expected = nullptr;
                        if (s.q.compare_exchange_strong(expected, q, std::memory_order_acq_rel))
                        {
                            cur = q;
                        }
                        else
                        {
                            cur = expected;
                        }
                    }
                    if (cur == q)
                    {
                        s.calls.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            }

            void STDMETHODCALLTYPE hk_ecl(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
            {
                note_queue(q);
                o_ecl(q, n, lists);
            }

            std::wstring describe_queue(ID3D12CommandQueue* q)
            {
                const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
                std::uint64_t calls = 0;
                for (const QueueSeen& s : g_queues)
                {
                    if (s.q.load(std::memory_order_acquire) == q)
                    {
                        calls = s.calls.load(std::memory_order_relaxed);
                    }
                }
                return std::format(L"{:p} type {} priority {} ({} submissions seen)",
                                   static_cast<void*>(q),
                                   static_cast<int>(d.Type),
                                   d.Priority,
                                   calls);
            }

            // The queue the swapchain was created with: one this process has submitted to,
            // whose pointer sits in the swapchain object. Reads only, through mem::read_ptr.
            ID3D12CommandQueue* swapchain_queue(const void* sc, std::size_t& offset)
            {
                for (std::size_t i = 0; i < kScanWords; ++i)
                {
                    void* p = nullptr;
                    if (!mem::read_ptr(static_cast<const std::uint8_t*>(sc) + i * sizeof(void*), p))
                    {
                        continue;
                    }
                    for (const QueueSeen& s : g_queues)
                    {
                        if (s.q.load(std::memory_order_acquire) == p)
                        {
                            offset = i * sizeof(void*);
                            return static_cast<ID3D12CommandQueue*>(p);
                        }
                    }
                }
                return nullptr;
            }

            bool ensure_pso(DXGI_FORMAT format)
            {
                if (g_pso != nullptr && g_pso_format == format)
                {
                    return true;
                }
                if (g_pso_retired != nullptr && g_blend_fence->GetCompletedValue() >= g_pso_retired_at)
                {
                    safe_release(g_pso_retired);
                }
                if (g_pso != nullptr)
                {
                    if (g_pso_retired != nullptr)
                    {
                        return false; // the one before is still in a blend; a later present retries
                    }
                    // Blends recorded with it may still be on the queue: it goes once they are.
                    g_pso_retired = g_pso;
                    g_pso_retired_at = g_blend_value;
                    g_pso = nullptr;
                }
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = g_root;
                pd.VS = {g_vs->GetBufferPointer(), g_vs->GetBufferSize()};
                pd.PS = {g_ps->GetBufferPointer(), g_ps->GetBufferSize()};
                D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
                b.BlendEnable = TRUE;
                b.SrcBlend = D3D12_BLEND_ONE;
                b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                b.BlendOp = D3D12_BLEND_OP_ADD;
                // The back buffer's alpha is the game's and stays the game's.
                b.SrcBlendAlpha = D3D12_BLEND_ZERO;
                b.DestBlendAlpha = D3D12_BLEND_ONE;
                b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.RasterizerState.DepthClipEnable = TRUE;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = format;
                pd.SampleDesc.Count = 1;
                const HRESULT hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
                if (FAILED(hr) || g_pso == nullptr)
                {
                    g_pso = nullptr;
                    g_pso_format = DXGI_FORMAT_UNKNOWN;
                    mm::logf(L"present: the blend pipeline for a {} back buffer could not be created (0x{:08X})",
                             format_name(format),
                             static_cast<unsigned>(hr));
                    return false;
                }
                g_pso_format = format;
                return true;
            }

            void release_slot(std::uint64_t slot)
            {
                if (slot != 0)
                {
                    g_target_busy[slot & kMailboxIndexMask].store(false, std::memory_order_release);
                }
            }

            // Takes `slot` out of `box` if it is still there and its render fence has
            // completed. True when this present now owns it.
            bool take_finished(std::atomic<std::uint64_t>& box, std::uint64_t done)
            {
                std::uint64_t slot = box.load(std::memory_order_acquire);
                if (slot == 0 || done < (slot >> kMailboxIndexBits))
                {
                    return false;
                }
                if (!box.compare_exchange_strong(slot, 0, std::memory_order_acq_rel))
                {
                    return false; // the render thread moved it; the next present looks again
                }
                if (g_current >= 0)
                {
                    g_target_busy[g_current].store(false, std::memory_order_release);
                }
                g_current = static_cast<int>(slot & kMailboxIndexMask);
                ++g_adoptions;
                return true;
            }

            // The newest finished frame goes on screen: the newest published if the GPU is
            // done with it, else the one it displaced. An unfinished frame stays for a later
            // present - nothing waits for it.
            void adopt_newest()
            {
                if (g_fence == nullptr)
                {
                    return;
                }
                const std::uint64_t done = g_fence->GetCompletedValue();
                if (take_finished(g_mailbox, done))
                {
                    // Older than what is now on screen.
                    release_slot(g_waiting.exchange(0, std::memory_order_acq_rel));
                }
                else if (take_finished(g_waiting, done))
                {
                    ++g_adopted_waiting;
                }
            }

            void find_queue(IDXGISwapChain* sc)
            {
                std::size_t off = 0;
                g_present_queue = swapchain_queue(sc, off);
                if (g_present_queue == nullptr)
                {
                    if (!g_queue_missing_logged && g_recordings == 0 && g_blends.load() == 0)
                    {
                        g_queue_missing_logged = true;
                        mm::log(L"present: no queue the game submits to sits in the swapchain object yet - "
                                L"the overlay is not drawn until one does");
                    }
                    return;
                }
                mm::logf(L"present: the swapchain {:p} holds queue {} at +0x{:X}; the overlay is blended "
                         L"into its back buffer on that queue",
                         static_cast<void*>(sc),
                         describe_queue(g_present_queue),
                         off);
            }

            void blend(IDXGISwapChain* sc)
            {
                const int slot = static_cast<int>(g_recordings % kSlots);
                if (g_blend_fence->GetCompletedValue() < g_slot_value[slot])
                {
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                IDXGISwapChain3* sc3 = nullptr;
                if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || sc3 == nullptr)
                {
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                ID3D12Resource* bb = nullptr;
                const HRESULT hb = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
                sc3->Release();
                if (FAILED(hb) || bb == nullptr)
                {
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const D3D12_RESOURCE_DESC bd = bb->GetDesc();
                if (!ensure_pso(bd.Format))
                {
                    bb->Release();
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const UINT w = (std::min)(g_blend_w, static_cast<UINT>(bd.Width));
                const UINT h = (std::min)(g_blend_h, bd.Height);
                D3D12_RECT r = unpack_rect(g_target_rect[g_current].load(std::memory_order_acquire));
                r.right = (std::min)(r.right, static_cast<LONG>(w));
                r.bottom = (std::min)(r.bottom, static_cast<LONG>(h));
                if (r.right <= r.left || r.bottom <= r.top)
                {
                    bb->Release();
                    return;
                }

                const mm::PerfScope pf(g_pf_blend);
                ID3D12Resource* target = g_bound[g_current];
                g_alloc[slot]->Reset();
                g_list->Reset(g_alloc[slot], g_pso);

                D3D12_RESOURCE_BARRIER in[2]{};
                in[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                in[0].Transition.pResource = target;
                in[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                in[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                in[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                in[1] = in[0];
                in[1].Transition.pResource = bb;
                in[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                in[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                g_list->ResourceBarrier(2, in);

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_blend_rtv->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += static_cast<SIZE_T>(slot) * g_blend_rtv_step;
                g_dev->CreateRenderTargetView(bb, nullptr, rtv);
                g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                g_list->SetGraphicsRootSignature(g_root);
                ID3D12DescriptorHeap* heaps[] = {g_blend_srv};
                g_list->SetDescriptorHeaps(1, heaps);
                D3D12_GPU_DESCRIPTOR_HANDLE srv = g_blend_srv->GetGPUDescriptorHandleForHeapStart();
                srv.ptr += static_cast<UINT64>(g_current) * g_blend_srv_step;
                g_list->SetGraphicsRootDescriptorTable(0, srv);
                const int mode = blend_mode(bd.Format);
                if (mode != g_mode_logged)
                {
                    g_mode_logged = mode;
                    mm::logf(L"present: blending into a {} back buffer {}",
                             format_name(bd.Format),
                             mode == 1   ? L"as scRGB"
                             : mode == 2 ? L"as HDR10 (ST.2084, BT.2020)"
                                         : L"as it is (SDR)");
                }
                const float white = g_white_nits;
                UINT consts[2] = {static_cast<UINT>(mode), 0};
                std::memcpy(&consts[1], &white, sizeof(float));
                g_list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
                const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
                g_list->RSSetViewports(1, &vp);
                g_list->RSSetScissorRects(1, &r);
                g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_list->DrawInstanced(3, 1, 0, 0);

                D3D12_RESOURCE_BARRIER out[2] = {in[0], in[1]};
                std::swap(out[0].Transition.StateBefore, out[0].Transition.StateAfter);
                std::swap(out[1].Transition.StateBefore, out[1].Transition.StateAfter);
                g_list->ResourceBarrier(2, out);
                g_list->Close();

                ID3D12CommandList* lists[] = {g_list};
                o_ecl(g_present_queue, 1, lists);
                g_present_queue->Signal(g_blend_fence, ++g_blend_value);
                g_slot_value[slot] = g_blend_value;
                g_target_blend[g_current].store(g_blend_value, std::memory_order_release);
                ++g_recordings;
                g_blends.fetch_add(1, std::memory_order_relaxed);
                bb->Release();
            }

            void present_body(IDXGISwapChain* sc)
            {
                if (!g_live || g_device_removed.load(std::memory_order_acquire))
                {
                    return;
                }
                if (g_present_queue == nullptr)
                {
                    find_queue(sc);
                    if (g_present_queue == nullptr)
                    {
                        return;
                    }
                }
                ++g_presents;
                adopt_newest();
                const std::uint64_t now = ::GetTickCount64();
                // Every 10 s at verbose; once at any level 10 s after the overlay first drew
                // something, so a player's log says whether the frames reach this present.
                const std::uint64_t drew = g_first_drawn_ms.load(std::memory_order_acquire);
                const bool first = drew != 0 && now - drew >= kPathReportMs
                                   && !g_path_reported.load(std::memory_order_relaxed)
                                   && !g_path_reported.exchange(true, std::memory_order_acq_rel);
                if (first || now - g_report_ms >= 10000)
                {
                    g_report_ms = now;
                    if (first || mm::log_enabled(mm::LogLv::Verbose))
                    {
                        mm::logf(L"present: {} present(s), {} blended, {} new overlay frame(s) taken ({} of them "
                                 L"the older, the newest still unfinished); without the overlay: {} before the "
                                 L"first frame, {} empty, {} slot busy, {} lock busy; {} GPU wait(s) of our queue "
                                 L"for a target still being read",
                                 g_presents,
                                 g_blends.load(std::memory_order_relaxed),
                                 g_adoptions,
                                 g_adopted_waiting,
                                 g_no_frame,
                                 g_empty,
                                 g_dropped.load(std::memory_order_relaxed),
                                 g_lock_busy.load(std::memory_order_relaxed),
                                 g_gpu_waits.load(std::memory_order_relaxed));
                    }
                }
                if (g_current < 0)
                {
                    ++g_no_frame;
                    return;
                }
                if (g_target_empty[g_current].load(std::memory_order_acquire))
                {
                    ++g_empty;
                    return;
                }
                if (g_phase.load(std::memory_order_relaxed) >= fc::NoCompose)
                {
                    return;
                }
                blend(sc);
            }

            int blend_mode(DXGI_FORMAT format)
            {
                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    return 1;
                }
                if (format != DXGI_FORMAT_R10G10B10A2_UNORM)
                {
                    return 0;
                }
                const int cs = g_colour_space.load(std::memory_order_relaxed);
                if (cs >= 0)
                {
                    return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0;
                }
                return g_output_hdr ? 2 : 0;
            }

            using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
            SetColorSpace1Fn o_set_colour_space = nullptr;

            HRESULT STDMETHODCALLTYPE hk_set_colour_space(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs)
            {
                const HRESULT hr = o_set_colour_space(sc, cs);
                if (SUCCEEDED(hr) && static_cast<void*>(sc) == g_low_sc.load(std::memory_order_acquire))
                {
                    g_colour_space.store(static_cast<int>(cs), std::memory_order_relaxed);
                }
                return hr;
            }

            // The user's "SDR content brightness" for the output, in nits - what DWM maps SDR
            // white to on an HDR display, and so what the overlay's white has to be to look as
            // it did when DWM composed it. 0 when the display configuration does not answer.
            float sdr_white_nits(const wchar_t* gdi_name)
            {
                UINT32 np = 0;
                UINT32 nm = 0;
                if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
                {
                    return 0.0f;
                }
                std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
                std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
                if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr)
                    != ERROR_SUCCESS)
                {
                    return 0.0f;
                }
                for (UINT32 i = 0; i < np; ++i)
                {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src.header.size = sizeof(src);
                    src.header.adapterId = paths[i].sourceInfo.adapterId;
                    src.header.id = paths[i].sourceInfo.id;
                    if (::DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS
                        || std::wcscmp(src.viewGdiDeviceName, gdi_name) != 0)
                    {
                        continue;
                    }
                    DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
                    wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                    wl.header.size = sizeof(wl);
                    wl.header.adapterId = paths[i].targetInfo.adapterId;
                    wl.header.id = paths[i].targetInfo.id;
                    if (::DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS)
                    {
                        return static_cast<float>(wl.SDRWhiteLevel) / 1000.0f * 80.0f;
                    }
                }
                return 0.0f;
            }

            // RENDER THREAD, once per adoption: whether the output is in HDR mode, and its SDR
            // white.
            void read_output(IDXGISwapChain* sc)
            {
                g_output_hdr = false;
                g_white_nits = 200.0f;
                IDXGIOutput* out = nullptr;
                if (FAILED(sc->GetContainingOutput(&out)) || out == nullptr)
                {
                    return;
                }
                IDXGIOutput6* out6 = nullptr;
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&out6))) && out6 != nullptr
                    && SUCCEEDED(out6->GetDesc1(&d)))
                {
                    g_output_hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    const float w = sdr_white_nits(d.DeviceName);
                    if (w > 0.0f)
                    {
                        g_white_nits = w;
                    }
                }
                safe_release(out6);
                safe_release(out);
                mm::logf(L"present: the output is in {} mode; SDR white {:.0f} nits",
                         g_output_hdr ? L"HDR" : L"SDR",
                         g_white_nits);
            }

            HRESULT STDMETHODCALLTYPE hk_low_present(IDXGISwapChain* sc, UINT sync, UINT flags)
            {
                g_low_calls.fetch_add(1, std::memory_order_relaxed);
                comp_present(sc);
                return o_low_present(sc, sync, flags);
            }

            HRESULT STDMETHODCALLTYPE hk_low_present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                      const DXGI_PRESENT_PARAMETERS* params)
            {
                g_low1_calls.fetch_add(1, std::memory_order_relaxed);
                comp_present(sc);
                return o_low_present1(sc, sync, flags, params);
            }

            // Puts `detour` into `vt[index]`; `original` gets what the slot held before the
            // slot changes, so a call arriving the moment it does already has somewhere to go.
            bool vt_patch(VtSlot& v, void** vt, int index, void* detour, void** original, const wchar_t* name)
            {
                void** slot = vt + index;
                if (v.installed)
                {
                    if (v.slot == slot)
                    {
                        return true;
                    }
                    mm::logf(L"present: {} moved from vtable slot {:p} to {:p} - the old patch stays, the new "
                             L"slot is not patched",
                             name,
                             static_cast<void*>(v.slot),
                             static_cast<void*>(slot));
                    return false;
                }
                void* cur = nullptr;
                DWORD old = 0;
                if (!mem::read(slot, cur) || cur == nullptr
                    || !::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                {
                    mm::logf(L"present: {}: vtable slot {:p} could not be made writable",
                             name,
                             static_cast<void*>(slot));
                    return false;
                }
                *original = cur;
                v.slot = slot;
                v.original = cur;
                ::InterlockedExchangePointer(slot, detour);
                ::VirtualProtect(slot, sizeof(void*), old, &old);
                v.installed = true;
                mm::logf(L"present: patched {} - slot {} of its vtable in {}, which pointed into {}",
                         name,
                         index,
                         module_of(vt),
                         module_of(cur));
                return true;
            }

            // Gives the slot back if it still holds our detour. If someone patched it after us,
            // their chain runs through ours and the patch stays, passing every call through.
            void vt_unpatch(VtSlot& v, void* detour)
            {
                if (!v.installed)
                {
                    return;
                }
                void* cur = nullptr;
                DWORD old = 0;
                if (!mem::read(v.slot, cur) || cur != detour
                    || !::VirtualProtect(v.slot, sizeof(void*), PAGE_READWRITE, &old))
                {
                    return;
                }
                ::InterlockedExchangePointer(v.slot, v.original);
                ::VirtualProtect(v.slot, sizeof(void*), old, &old);
                v.installed = false;
            }

            bool hook_once(void*& installed, void* target, void* detour, void** original, const wchar_t* name)
            {
                if (installed == target)
                {
                    return true;
                }
                if (installed != nullptr)
                {
                    mm::logf(L"present: {} moved from {:p} to {:p} - the old hook stays, the new address is "
                             L"not hooked",
                             name,
                             installed,
                             target);
                    return false;
                }
                const MH_STATUS c = MH_CreateHook(target, detour, original);
                const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
                mm::logf(L"present: hooked {} @ {:p} ({}) - create {} enable {}",
                         name,
                         target,
                         module_of(target),
                         static_cast<int>(c),
                         static_cast<int>(e));
                if (c != MH_OK || e != MH_OK)
                {
                    if (c == MH_OK)
                    {
                        MH_RemoveHook(target);
                    }
                    return false;
                }
                installed = target;
                return true;
            }

            // Where a call to `at` ends up: through `jmp rel32` and `jmp [rip+disp32]`, the two
            // forms an inline hook and MinHook's relay take, at most four hops.
            const void* follow_jumps(const void* at)
            {
                for (int hop = 0; hop < 4 && at != nullptr; ++hop)
                {
                    std::uint8_t op[6]{};
                    if (!mem::read(at, op))
                    {
                        break;
                    }
                    const auto* p = static_cast<const std::uint8_t*>(at);
                    if (op[0] == 0xE9)
                    {
                        std::int32_t rel = 0;
                        std::memcpy(&rel, op + 1, sizeof(rel));
                        at = p + 5 + rel;
                    }
                    else if (op[0] == 0xFF && op[1] == 0x25)
                    {
                        std::int32_t disp = 0;
                        std::memcpy(&disp, op + 2, sizeof(disp));
                        void* next = nullptr;
                        if (!mem::read(p + 6 + disp, next))
                        {
                            break;
                        }
                        at = next;
                    }
                    else
                    {
                        break;
                    }
                }
                return at;
            }

            // Whether `p` lies in a module named dxgi.dll: Windows' own, or a proxy that took its
            // name (ReShade), which forwards every present it is given.
            bool in_dxgi(const void* p)
            {
                HMODULE mod = nullptr;
                wchar_t path[MAX_PATH * 2]{};
                if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                             | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         static_cast<LPCWSTR>(p),
                                         &mod)
                        == 0
                    || ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path))) == 0)
                {
                    return false;
                }
                const wchar_t* slash = std::wcsrchr(path, L'\\');
                return ::_wcsicmp(slash != nullptr ? slash + 1 : path, L"dxgi.dll") == 0;
            }

            // Whether `p` lies in any loaded image - what a vtable of a C++ object does.
            bool in_image(const void* p)
            {
                HMODULE mod = nullptr;
                return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            static_cast<LPCWSTR>(p),
                                            &mod)
                       != 0;
            }

            // Whether the first words of `obj` hold `value`. Reads only.
            bool holds_word(const void* obj, const void* value)
            {
                for (std::size_t i = 0; i < kScanWords; ++i)
                {
                    void* w = nullptr;
                    if (mem::read(static_cast<const std::uint8_t*>(obj) + i * sizeof(void*), w) && w == value)
                    {
                        return true;
                    }
                }
                return false;
            }

            // Whether `p` can be taken for a DXGI swapchain of `window` before anything of it is
            // called: its vtable's IUnknown slots and Present all in dxgi.dll - dxgi.dll has C++
            // classes of its own that are not COM objects, and calling one of those through
            // slot 0 runs whatever is there - and the window's handle among its first words, as
            // a swapchain keeps it.
            bool looks_like_dxgi_swapchain(const void* p, void* const* vt, HWND window)
            {
                for (int slot : {0, 1, 2, 8})
                {
                    void* f = nullptr;
                    if (!mem::read(vt + slot, f) || !in_dxgi(f))
                    {
                        return false;
                    }
                }
                return in_dxgi(vt) && holds_word(p, window);
            }

            // The DXGI swapchain for `window` among the objects `obj` points at in its first
            // words, looking `depth` objects deep. Only an object `looks_like_dxgi_swapchain`
            // accepts is ever called; every other object is only read. `path` collects the
            // offsets.
            void* find_dxgi_swapchain(const void* obj, HWND window, int depth, int& budget, std::wstring& path)
            {
                for (std::size_t i = 0; i < kScanWords; ++i)
                {
                    void* p = nullptr;
                    void** vt = nullptr;
                    void* present = nullptr;
                    if (!mem::read_ptr(static_cast<const std::uint8_t*>(obj) + i * sizeof(void*), p) || p == obj
                        || !mem::read(p, vt) || !mem::read(vt + 8, present) || !in_image(vt))
                    {
                        continue;
                    }
                    const auto here = [i] { return std::format(L"+0x{:X}", i * sizeof(void*)); };
                    if (looks_like_dxgi_swapchain(p, vt, window))
                    {
                        IDXGISwapChain* sc = nullptr;
                        if (FAILED(static_cast<IUnknown*>(p)->QueryInterface(IID_PPV_ARGS(&sc))) || sc == nullptr)
                        {
                            continue;
                        }
                        DXGI_SWAP_CHAIN_DESC d{};
                        const bool same_window = SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow == window;
                        sc->Release();
                        if (same_window)
                        {
                            path += here();
                            return sc;
                        }
                        continue;
                    }
                    // Another wrapper in between (OptiScaler's own, under its FSR3 proxy): an
                    // object with a vtable in some loaded image is looked into, within budget.
                    if (depth > 0 && budget > 0)
                    {
                        --budget;
                        void* found = find_dxgi_swapchain(p, window, depth - 1, budget, path);
                        if (found != nullptr)
                        {
                            path = here() + L" (" + module_of(vt) + L") " + path;
                            return found;
                        }
                    }
                }
                return nullptr;
            }

            // A swapchain object that is not DXGI's - a frame generator's proxy, such as FSR3's
            // under OptiScaler - presents its generated frames to a DXGI swapchain it holds,
            // directly or through another wrapper, and only that one is on every displayed
            // frame. Found the way the queue is: by reading the proxy's first words.
            void* swapchain_inside(void* outer, IDXGISwapChain* engine)
            {
                DXGI_SWAP_CHAIN_DESC want{};
                if (FAILED(engine->GetDesc(&want)))
                {
                    return nullptr;
                }
                int budget = 128;
                std::wstring path;
                void* sc = find_dxgi_swapchain(outer, want.OutputWindow, 1, budget, path);
                if (sc != nullptr)
                {
                    mm::logf(L"present: the swapchain {:p} (vtable in {}) presents through DXGI swapchain {:p}, "
                             L"held at {}; the overlay goes into that one",
                             outer,
                             module_of(*static_cast<void**>(outer)),
                             sc,
                             path);
                    return sc;
                }
                mm::logf(L"present: the swapchain {:p} (vtable in {}) is not DXGI's and holds no DXGI swapchain "
                         L"for the window that could be found - the overlay goes into it, and frames it "
                         L"generates itself go out without the overlay",
                         outer,
                         module_of(*static_cast<void**>(outer)));
                return nullptr;
            }

            // What the engine's swapchain sits on: Streamline's native interface, or itself.
            void* native_of(IDXGISwapChain* engine)
            {
                HMODULE sl = ::GetModuleHandleW(L"sl.interposer.dll");
                auto get_native = sl != nullptr ? reinterpret_cast<GetNativeFn>(reinterpret_cast<void*>(
                                                      ::GetProcAddress(sl, "slGetNativeInterface")))
                                                : nullptr;
                if (get_native == nullptr)
                {
                    return engine;
                }
                void* native = nullptr;
                const int r = get_native(engine, &native);
                if (r != 0 || native == nullptr)
                {
                    mm::logf(L"present: slGetNativeInterface answered {} - the engine's swapchain is taken as "
                             L"the one that flips",
                             r);
                    return engine;
                }
                // Whether that answer carries a reference is not documented; a second call
                // tells. AddRef/Release in balance is the only thing done to the object.
                const ULONG before = static_cast<IUnknown*>(native)->AddRef();
                static_cast<IUnknown*>(native)->Release();
                void* again = nullptr;
                if (get_native(engine, &again) != 0 || again == nullptr)
                {
                    return native;
                }
                const ULONG after = static_cast<IUnknown*>(native)->AddRef();
                static_cast<IUnknown*>(native)->Release();
                if (after == before + 1)
                {
                    static_cast<IUnknown*>(native)->Release();
                    static_cast<IUnknown*>(again)->Release();
                }
                return native;
            }

            // The swapchain that actually flips: beneath any interposer the engine's sits on,
            // and beneath a frame generator's proxy when there is one.
            void* lowest_swapchain(IDXGISwapChain* engine)
            {
                void* low = native_of(engine);
                void** vt = nullptr;
                if (!mem::read(low, vt) || in_dxgi(vt))
                {
                    return low;
                }
                void* inner = swapchain_inside(low, engine);
                g_low_beneath_proxy = inner != nullptr;
                return inner != nullptr ? inner : low;
            }

            // UNDER g_present_lock, on a rebind after a resize: the proxy's DXGI swapchain
            // again, and if it is a new one, it becomes the one that flips - its queue found
            // afresh by the next present. A new one of another class keeps the old patch.
            void follow_proxy()
            {
                void* low = lowest_swapchain(g_swapchain);
                if (low == g_low_sc.load(std::memory_order_acquire))
                {
                    return;
                }
                void** vt = *static_cast<void***>(low);
                if (!vt_patch(g_vt_present, vt, 8, reinterpret_cast<void*>(&hk_low_present),
                              reinterpret_cast<void**>(&o_low_present), L"the lowest swapchain's Present"))
                {
                    return;
                }
                vt_patch(g_vt_present1, vt, 22, reinterpret_cast<void*>(&hk_low_present1),
                         reinterpret_cast<void**>(&o_low_present1), L"the lowest swapchain's Present1");
                vt_patch(g_vt_colour_space, vt, 38, reinterpret_cast<void*>(&hk_set_colour_space),
                         reinterpret_cast<void**>(&o_set_colour_space), L"the lowest swapchain's SetColorSpace1");
                mm::logf(L"present: after the resize the swapchain that flips is {:p}", low);
                g_low_sc.store(low, std::memory_order_release);
                g_present_queue = nullptr;
            }

            bool create_pipeline()
            {
                ID3DBlob* err = nullptr;
                HRESULT hr = ::D3DCompile(kVs, std::strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                          &g_vs, &err);
                safe_release(err);
                if (SUCCEEDED(hr))
                {
                    hr = ::D3DCompile(kPs, std::strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                      &g_ps, &err);
                    if (FAILED(hr) && err != nullptr)
                    {
                        mm::logf(L"present: the blend shader did not compile: {}",
                                 std::wstring(static_cast<const char*>(err->GetBufferPointer()),
                                              static_cast<const char*>(err->GetBufferPointer())
                                                  + err->GetBufferSize()));
                    }
                    safe_release(err);
                }
                if (FAILED(hr))
                {
                    return false;
                }
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                range.NumDescriptors = 1;
                D3D12_ROOT_PARAMETER params[2]{};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[0].DescriptorTable.NumDescriptorRanges = 1;
                params[0].DescriptorTable.pDescriptorRanges = &range;
                params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[1].Constants.Num32BitValues = 2;
                params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC rd{};
                rd.NumParameters = 2;
                rd.pParameters = params;
                ID3DBlob* sig = nullptr;
                hr = ::D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
                safe_release(err);
                if (SUCCEEDED(hr))
                {
                    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                    IID_PPV_ARGS(&g_root));
                }
                safe_release(sig);
                return SUCCEEDED(hr) && g_root != nullptr;
            }

            bool create_objects()
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_comp_queue))))
                {
                    return false;
                }
                for (ID3D12CommandAllocator*& a : g_alloc)
                {
                    if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                    {
                        return false;
                    }
                }
                if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                                    IID_PPV_ARGS(&g_list))))
                {
                    return false;
                }
                g_list->Close();
                if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_blend_fence))))
                {
                    return false;
                }
                if (g_blend_event == nullptr)
                {
                    g_blend_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                }
                D3D12_DESCRIPTOR_HEAP_DESC hd{};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                hd.NumDescriptors = kSlots;
                if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_blend_rtv))))
                {
                    return false;
                }
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                hd.NumDescriptors = kTargets;
                hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_blend_srv))))
                {
                    return false;
                }
                g_blend_rtv_step = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                g_blend_srv_step = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                return g_blend_event != nullptr && create_pipeline();
            }

            // Teardown only: the last blend reads the targets and runs on a queue that is
            // not ours, so nothing it touches goes before it has.
            void wait_for_blends()
            {
                if (g_blend_fence == nullptr || g_blend_value == 0
                    || g_blend_fence->GetCompletedValue() >= g_blend_value)
                {
                    return;
                }
                if (SUCCEEDED(g_blend_fence->SetEventOnCompletion(g_blend_value, g_blend_event))
                    && ::WaitForSingleObject(g_blend_event, kBlendWaitMs) != WAIT_OBJECT_0)
                {
                    mm::logf(L"present: the last blend had still not run after {} ms; the teardown goes on",
                             kBlendWaitMs);
                }
            }
        } // namespace

        DXGI_FORMAT comp_format()
        {
            return kCompFormat;
        }

        ID3D12CommandQueue* comp_queue()
        {
            return g_comp_queue;
        }

        bool comp_ready()
        {
            return g_comp_queue != nullptr && g_root != nullptr;
        }

        std::uint64_t comp_skipped_frames()
        {
            return g_skipped.load(std::memory_order_relaxed);
        }

        std::uint64_t comp_dropped_frames()
        {
            return g_dropped.load(std::memory_order_relaxed);
        }

        void comp_reset_counters()
        {
            g_skipped.store(0, std::memory_order_relaxed);
            g_dropped.store(0, std::memory_order_relaxed);
        }

        void comp_set_phase(int phase)
        {
            g_phase.store(phase, std::memory_order_relaxed);
        }

        int comp_pick_target()
        {
            // A target off screen and out of the mailbox may still be read by a blend the
            // present queue has not run yet - it runs a frame or two behind under frame
            // generation. The render thread does not wait for that: our own queue does, on
            // the GPU, ahead of the frame that overwrites it. Of the free targets the one
            // whose last blend is oldest is taken, so that wait is rarely there at all.
            int best = -1;
            std::uint64_t best_blend = 0;
            for (int i = 0; i < static_cast<int>(kTargets); ++i)
            {
                const std::uint64_t b = g_target_blend[i].load(std::memory_order_acquire);
                if (!g_target_busy[i].load(std::memory_order_acquire) && (best < 0 || b < best_blend))
                {
                    best = i;
                    best_blend = b;
                }
            }
            if (best < 0)
            {
                // One on screen and both mailbox slots full: the older waiting frame is
                // taken back, unless a present has just put it on screen.
                const std::uint64_t w = g_waiting.exchange(0, std::memory_order_acq_rel);
                if (w == 0)
                {
                    g_skipped.fetch_add(1, std::memory_order_relaxed);
                    return -1;
                }
                best = static_cast<int>(w & kMailboxIndexMask);
                best_blend = g_target_blend[best].load(std::memory_order_acquire);
                g_target_busy[best].store(false, std::memory_order_release);
            }
            if (g_blend_fence != nullptr && g_comp_queue != nullptr && best_blend > g_blend_fence->GetCompletedValue())
            {
                g_comp_queue->Wait(g_blend_fence, best_blend);
                g_gpu_waits.fetch_add(1, std::memory_order_relaxed);
            }
            return best;
        }

        void comp_publish(int index, std::uint64_t fence_value, const ImDrawData* dd)
        {
            if (index < 0 || index >= static_cast<int>(kTargets))
            {
                return;
            }
            // What the frame covers, so a blend touches only that: the union of every draw
            // command's clip rect. Nothing drawn at all is the overlay hidden, and costs no
            // blend.
            bool empty = true;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            if (dd != nullptr)
            {
                for (const ImDrawList* l : dd->CmdLists)
                {
                    for (const ImDrawCmd& c : l->CmdBuffer)
                    {
                        if (c.ElemCount == 0)
                        {
                            continue;
                        }
                        empty = false;
                        x0 = (std::min)(x0, c.ClipRect.x);
                        y0 = (std::min)(y0, c.ClipRect.y);
                        x1 = (std::max)(x1, c.ClipRect.z);
                        y1 = (std::max)(y1, c.ClipRect.w);
                    }
                }
            }
            D3D12_RECT r{0, 0, 0, 0};
            if (!empty)
            {
                const ImVec2 o = dd->DisplayPos;
                const ImVec2 s = dd->FramebufferScale;
                const auto clampx = [](float v, UINT lim) {
                    return static_cast<LONG>((std::max)(0.0f, (std::min)(v, static_cast<float>(lim))));
                };
                r.left = clampx(std::floor((x0 - o.x) * s.x), g_blend_w);
                r.top = clampx(std::floor((y0 - o.y) * s.y), g_blend_h);
                r.right = clampx(std::ceil((x1 - o.x) * s.x), g_blend_w);
                r.bottom = clampx(std::ceil((y1 - o.y) * s.y), g_blend_h);
            }
            if (!empty && g_first_drawn_ms.load(std::memory_order_relaxed) == 0)
            {
                g_first_drawn_presents.store(g_present_count.load(std::memory_order_relaxed),
                                             std::memory_order_relaxed);
                g_first_drawn_ms.store(::GetTickCount64(), std::memory_order_release);
            }
            g_target_empty[index].store(empty, std::memory_order_release);
            g_target_rect[index].store(pack_rect(r), std::memory_order_release);
            g_target_busy[index].store(true, std::memory_order_release);
            const std::uint64_t slot = (fence_value << kMailboxIndexBits) | static_cast<std::uint64_t>(index);
            const std::uint64_t prev = g_mailbox.exchange(slot, std::memory_order_acq_rel);
            if (prev != 0)
            {
                // The displaced frame waits beside the new one in case the GPU finishes it
                // first; what it displaces in turn was never adopted, so never blended, and
                // is free at once.
                release_slot(g_waiting.exchange(prev, std::memory_order_acq_rel));
            }
        }

        void comp_present(IDXGISwapChain* sc)
        {
            if (sc != g_low_sc.load(std::memory_order_acquire))
            {
                return;
            }
            g_low_on_ours.fetch_add(1, std::memory_order_relaxed);
            g_low_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
            if (!g_present_lock.try_lock())
            {
                g_lock_busy.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (g_pf_blend < 0)
            {
                g_pf_blend = mm::perf_register("present blend", perf::Thread::Present);
            }
            try
            {
                present_body(sc);
            }
            catch (...)
            {
                g_live = false;
                mm::log(L"present: an exception escaped the blend - the overlay stops reaching the screen");
            }
            g_present_lock.unlock();
        }

        void comp_report_quiet_path()
        {
            const std::uint64_t drew = g_first_drawn_ms.load(std::memory_order_acquire);
            // The present side has the first say; this is for when its presents never come.
            if (drew == 0 || ::GetTickCount64() - drew < kPathReportMs + 2000
                || g_path_reported.load(std::memory_order_relaxed)
                || g_path_reported.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            std::wstring where = L"not patched";
            if (g_vt_present.installed)
            {
                void* slot = nullptr;
                (void)mem::read(g_vt_present.slot, slot);
                const void* at = g_vt_present.original;
                std::uint8_t head[8]{};
                (void)mem::read(at, head);
                where = std::format(L"{}; the function behind it starts {:02X} {:02X} {:02X} {:02X} {:02X} "
                                    L"{:02X}, which lands in {}",
                                    slot == reinterpret_cast<void*>(&hk_low_present)
                                        ? std::wstring{L"ours"}
                                        : L"in " + module_of(slot),
                                    head[0], head[1], head[2], head[3], head[4], head[5],
                                    module_of(follow_jumps(at)));
            }
            mm::logf(L"present: the overlay has drawn for {} s and the swapchain that flips has not "
                     L"blended once: the engine presented {} time(s) since; our hook on the lowest "
                     L"Present ran {} time(s), Present1 {}, {} of them on that swapchain (last on thread "
                     L"{}). Its vtable slot is {}",
                     (::GetTickCount64() - drew) / 1000,
                     g_present_count.load(std::memory_order_relaxed)
                         - g_first_drawn_presents.load(std::memory_order_relaxed),
                     g_low_calls.load(std::memory_order_relaxed),
                     g_low1_calls.load(std::memory_order_relaxed),
                     g_low_on_ours.load(std::memory_order_relaxed),
                     g_low_tid.load(std::memory_order_relaxed),
                     where);
        }

        bool comp_create(ID3D12Device* device, HWND)
        {
            if (device == nullptr || g_swapchain == nullptr)
            {
                return false;
            }
            g_dev = device;
            read_output(g_swapchain);
            if (!create_objects())
            {
                mm::log(L"present: the blend's command objects or pipeline could not be created - the "
                        L"overlay does not start. The rest of the mod keeps running.");
                comp_release();
                return false;
            }
            void* low = lowest_swapchain(g_swapchain);
            void** engine_vt = *reinterpret_cast<void***>(g_swapchain);
            void** low_vt = *reinterpret_cast<void***>(low);
            g_low_is_engine = low == g_swapchain || low_vt == engine_vt;
            void** queue_vt = *reinterpret_cast<void***>(g_comp_queue);
            bool ok = hook_once(g_ecl_addr, queue_vt[10], reinterpret_cast<void*>(&hk_ecl),
                                reinterpret_cast<void**>(&o_ecl), L"ExecuteCommandLists");
            if (ok)
            {
                // The game's own colour space, from here on; a SetColorSpace1 before this
                // install is what `read_output` stands in for.
                vt_patch(g_vt_colour_space, low_vt, 38, reinterpret_cast<void*>(&hk_set_colour_space),
                         reinterpret_cast<void**>(&o_set_colour_space), L"the lowest swapchain's SetColorSpace1");
            }
            if (ok && !g_low_is_engine)
            {
                ok = vt_patch(g_vt_present, low_vt, 8, reinterpret_cast<void*>(&hk_low_present),
                              reinterpret_cast<void**>(&o_low_present), L"the lowest swapchain's Present");
                vt_patch(g_vt_present1, low_vt, 22, reinterpret_cast<void*>(&hk_low_present1),
                         reinterpret_cast<void**>(&o_low_present1), L"the lowest swapchain's Present1");
            }
            if (!ok)
            {
                mm::log(L"present: the hooks the blend needs did not install - the overlay does not start");
                comp_release();
                return false;
            }
            mm::logf(L"present: the swapchain that flips is {:p}{}, its Present in {}",
                     low,
                     g_low_is_engine ? L" (the engine's own)" : L" (beneath the engine's)",
                     module_of(g_low_is_engine ? low_vt[8] : g_vt_present.original));
            g_low_sc.store(low, std::memory_order_release);
            g_low_just_found = true;
            return true;
        }

        bool comp_bind_targets(ID3D12Resource* const* targets, UINT width, UINT height)
        {
            if (g_blend_srv == nullptr || targets == nullptr || width == 0 || height == 0)
            {
                return false;
            }
            if (!g_present_lock.try_lock_ms(kLockMs))
            {
                return false;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_blend_srv->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < kTargets; ++i)
            {
                g_bound[i] = targets[i];
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = kCompFormat;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                g_dev->CreateShaderResourceView(targets[i], &sd, h);
                h.ptr += g_blend_srv_step;
                g_target_busy[i].store(false, std::memory_order_release);
                g_target_blend[i].store(0, std::memory_order_release);
                g_target_empty[i].store(true, std::memory_order_release);
            }
            g_blend_w = width;
            g_blend_h = height;
            g_current = -1;
            g_mailbox.store(0, std::memory_order_release);
            g_waiting.store(0, std::memory_order_release);
            if (g_low_beneath_proxy && !g_low_just_found && g_swapchain != nullptr)
            {
                follow_proxy();
            }
            g_low_just_found = false;
            g_live = true;
            g_present_lock.unlock();
            if (!g_format_logged)
            {
                g_format_logged = true;
                mm::logf(L"present: the overlay draws into {} target(s) of its own - {}x{} {}, premultiplied "
                         L"alpha - and each displayed frame gets the newest finished one blended into its back "
                         L"buffer. Nothing is composed beside the game's frame.",
                         kTargets,
                         width,
                         height,
                         format_name(kCompFormat));
            }
            return true;
        }

        bool comp_unbind_targets()
        {
            if (!g_present_lock.try_lock_ms(kLockMs))
            {
                mm::logf(L"present: the presenting thread held the blend for over {} ms - the targets are "
                         L"left as they are",
                         kLockMs);
                return false;
            }
            g_live = false;
            g_current = -1;
            g_mailbox.store(0, std::memory_order_release);
            g_waiting.store(0, std::memory_order_release);
            wait_for_blends();
            for (UINT i = 0; i < kTargets; ++i)
            {
                g_bound[i] = nullptr;
                g_target_busy[i].store(false, std::memory_order_release);
                g_target_blend[i].store(0, std::memory_order_release);
            }
            g_present_lock.unlock();
            return true;
        }

        void comp_release()
        {
            (void)comp_unbind_targets();
            spin::SpinGuard guard(g_present_lock);
            g_low_sc.store(nullptr, std::memory_order_release);
            g_present_queue = nullptr;
            wait_for_blends();
            safe_release(g_pso);
            safe_release(g_pso_retired);
            g_pso_format = DXGI_FORMAT_UNKNOWN;
            safe_release(g_root);
            safe_release(g_vs);
            safe_release(g_ps);
            safe_release(g_list);
            for (ID3D12CommandAllocator*& a : g_alloc)
            {
                safe_release(a);
            }
            for (std::uint64_t& v : g_slot_value)
            {
                v = 0;
            }
            safe_release(g_blend_fence);
            g_blend_value = 0;
            g_recordings = 0;
            safe_release(g_blend_rtv);
            safe_release(g_blend_srv);
            safe_release(g_comp_queue);
            g_dev = nullptr;
            g_queue_missing_logged = false;
            g_mode_logged = -1;
            g_blends.store(0, std::memory_order_relaxed);
            g_gpu_waits.store(0, std::memory_order_relaxed);
            g_lock_busy.store(0, std::memory_order_relaxed);
            g_presents = 0;
            g_adoptions = 0;
            g_adopted_waiting = 0;
            g_path_reported.store(false, std::memory_order_relaxed);
            g_first_drawn_ms.store(0, std::memory_order_relaxed);
            g_first_drawn_presents.store(0, std::memory_order_relaxed);
            g_low_calls.store(0, std::memory_order_relaxed);
            g_low1_calls.store(0, std::memory_order_relaxed);
            g_low_on_ours.store(0, std::memory_order_relaxed);
            g_no_frame = 0;
            g_empty = 0;
            comp_reset_counters();
            // The vtable slots go back; a slot someone patched after us stays ours and, with
            // `g_low_sc` cleared, passes every call straight through - as the ExecuteCommandLists
            // detour does, which MinHook keeps for the process and the master switch disables.
            // A call already inside a detour finishes on the original it read. The blend event
            // is not closed: a handle value is recycled by the kernel.
            vt_unpatch(g_vt_present, reinterpret_cast<void*>(&hk_low_present));
            vt_unpatch(g_vt_present1, reinterpret_cast<void*>(&hk_low_present1));
            vt_unpatch(g_vt_colour_space, reinterpret_cast<void*>(&hk_set_colour_space));
        }

    } // namespace ovl
} // namespace overlay
