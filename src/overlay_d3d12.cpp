//
// overlay_d3d12 - the D3D12 device objects, the swapchain hooks and the per-frame
// render entry point.
//
// Everything here runs on the RENDER thread except the slice request the loop thread
// leaves behind: the SRV heap, the map and height-slice textures, the ImGui backends,
// the Present / Present1 / ResizeBuffers / ExecuteCommandLists hooks and the hook
// address cache they are installed from.
//
// HOOK STRATEGY (the hudhook approach)
// ------------------------------------
// The game's IDXGISwapChain and ID3D12CommandQueue are not reachable from a UE4SS mod,
// and the swapchain's command queue cannot be queried back out of the swapchain. So:
//
//   1. create a throwaway D3D12 device + DIRECT command queue + a 64x64 swapchain on a
//      hidden window, purely to read their vtables;
//   2. MinHook the absolute addresses of IDXGISwapChain::Present (vtable slot 8),
//      ResizeBuffers (13), IDXGISwapChain1::Present1 (22) and
//      ID3D12CommandQueue::ExecuteCommandLists (10);
//   3. throw the dummy objects away and wait. The first real Present gives us the
//      swapchain; the DIRECT queue that executed last before it gives us the queue.
//
// The dummy objects go through our own import table, i.e. through whatever `dxgi.dll`
// is loaded - on this machine ReShade's proxy, so the dummy swapchain is a ReShade
// wrapper with the same vtable the game holds and we hook the slot the game calls. The
// module owning every hooked address is logged. A watchdog complains when no Present
// arrives within a few seconds: the swapchain is then wrapped by something we did not
// go through (e.g. a DLSS-FG proxy).
//

#include "overlay_internal.hpp"

#include "imgui_caret.hpp"

namespace overlay
{
    namespace ovl
    {
        //==============================================================================
        // SRV descriptor heap allocator
        //==============================================================================
        //
        // ImGui 1.92's DX12 backend allocates SRV descriptors through callbacks (it
        // needs one per texture now, not just one for the font atlas), so the heap and
        // its free list are ours. A fixed 64-slot heap is plenty: the font atlas plus
        // one map texture.

        void srv_alloc_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            if (!g_srv_heap.alloc(*cpu, *gpu))
            {
                cpu->ptr = 0;
                gpu->ptr = 0;
                // Once: a full heap stays full, and this is a render-thread callback.
                static bool said = false;
                if (!said)
                {
                    said = true;
                    mm::log(L"SRV heap exhausted - raise srv_heap_size (RESTART) if the "
                            L"overlay is missing textures");
                }
            }
        }

        void srv_free_cb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            g_srv_heap.free(cpu);
        }

        //==============================================================================
        // Renderer state
        //==============================================================================

        // RENDER THREAD ONLY, like `g_swapchain` itself.
        //
        // `g_adopted_present_ms` is GetTickCount64() of the last Present on the adopted
        // swapchain; 0 means "no reference point yet" and disables the check below. A
        // game that RECREATES its swapchain (an HDR toggle, a fullscreen mode change)
        // leaves the adopted one presenting nothing while a new one presents every
        // frame, and the plain "not ours" filter would drop the new one for the rest of
        // the session. So a D3D12 swapchain presenting while ours has been silent for
        // kSwapchainSilentMs asks for the ordinary re-adoption.
        //
        // `g_imgui_rtv_format` is the RTV format ImGui's DX12 backend was initialised
        // for. Its pipeline state bakes that format in, so a back-buffer format change
        // needs the backend rebuilt exactly as a raised buffer count does.
        static std::uint64_t g_adopted_present_ms = 0;
        static DXGI_FORMAT g_imgui_rtv_format = DXGI_FORMAT_UNKNOWN;
        constexpr std::uint64_t kSwapchainSilentMs = 2000;

        //==============================================================================
        // The height-slice texture
        //==============================================================================
        //
        // A small dynamic RGBA8 texture the CPU slicer refills at slice_hz. Two of them,
        // because the GPU may still be sampling one while we write the next:
        // `in_flight_fence` is the fence value of the last frame that drew this buffer,
        // and a buffer is only rewritten once GetCompletedValue() has passed it.
        //
        // The upload heap stays mapped for the buffer's whole life (a 512x512 RGBA
        // window is 1 MB, and Map/Unmap per update is pure overhead), and the copy is
        // recorded on the same command list Present already records for ImGui - so
        // there is no extra queue, no PSO and no root signature on ReShade's swapchain.

        // QueryPerformanceFrequency is constant for the life of the process: asked once,
        // lazily.
        std::int64_t qpc_freq()
        {
            static const std::int64_t freq = [] {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                return static_cast<std::int64_t>(f.QuadPart);
            }();
            return freq;
        }

        //==============================================================================
        // The slicer runs on the LOOP thread
        //==============================================================================
        //
        // `slice_region` is 1-4 ms of pure CPU that writes into a persistently mapped
        // upload heap and needs nothing from Present, so it runs from
        // `overlay::on_update` on the UE4SS loop thread; `render()` only records the
        // CopyTextureRegion for a buffer the slicer has finished and stamps the fence of
        // the frame that sampled it.
        //
        // THE OWNERSHIP RULES, which are what keep this safe:
        //   * the RENDER thread owns creation and destruction of every D3D12 object,
        //     including the slice buffers, and publishes what it wants cut;
        //   * the LOOP thread only ever writes into `mapped` memory of a buffer that
        //     already exists, and only while the slicer is not paused;
        //   * a buffer may be written only when the fence of the last frame that
        //     sampled it has completed (`g_slice_in_flight`);
        //   * before creating or destroying any slice buffer the render thread PAUSES
        //     the slicer and waits, with a bound, for it to leave the critical section.
        //     If it cannot, it does not touch the buffers this frame and tries again.
        //   * `g_slice_gen` is bumped by every create/destroy; the slicer re-reads it
        //     after writing and throws the result away if it moved, so a cut can never
        //     be published against a buffer set that no longer exists.

        SliceView slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            return g_slice_view;
        }

        MapSliceView map_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            return g_mslice_view;
        }

        void clear_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            g_slice_view = SliceView{};
        }

        void clear_map_slice_view()
        {
            spin::SpinGuard guard(g_slice_view_lock);
            g_mslice_view = MapSliceView{};
        }

        // Render thread. Stops the slicer and waits for it to leave its critical
        // section. False when it did not stop inside `budget_ms`: the caller must then
        // leave every slice buffer alone and try again next frame.
        bool slicer_pause_begin(unsigned budget_ms)
        {
            g_slicer_pause.store(true); // seq_cst on purpose: it pairs with the loop's
                                        // "check, mark busy, check again" sequence
            const std::uint64_t deadline = ::GetTickCount64() + budget_ms;
            while (g_slicer_busy.load())
            {
                if (::GetTickCount64() > deadline)
                {
                    g_slicer_pause.store(false);
                    return false;
                }
                ::SwitchToThread();
            }
            return true;
        }

        void slicer_pause_end()
        {
            g_slicer_pause.store(false);
        }

        // Bumped by every create/destroy so a cut in flight can be discarded.
        void note_slice_buffers_changed()
        {
            g_slice_gen.fetch_add(1, std::memory_order_release);
        }

        //==============================================================================
        // The full map
        //==============================================================================
        //
        // The same height-sliced asset the minimap draws, at map scale: north-up,
        // pannable, zoomable, with every marker on it. Its own pair of dynamic textures,
        // because its window is bigger and decimated - one texture pixel covers `step`
        // source pixels - and its own update policy: it re-cuts only when something
        // changed (pan out of the cut region, zoom, floor slice, a big player move),
        // capped at map_slice_hz, where the minimap re-cuts 12 times a second.

        // Drops the captured command queue and the reference held on it. The render
        // thread's teardown is what calls it, so no other thread can be inside
        // `o_ExecuteCommandLists(queue, ...)` with our pointer at the same time. The loop
        // thread reaches it on a stop taken before any Present arrived or with the hooks
        // already disabled; a queue can only be captured inside a Present, so in the
        // first case there is nothing to release, and in the second nothing can be
        // submitting through a disabled trampoline.
        void safe_release_queue()
        {
            ID3D12CommandQueue* queue = g_queue.exchange(nullptr, std::memory_order_acq_rel);
            if (queue != nullptr)
            {
                queue->Release();
            }
        }

        // Asks only; the expiry is a separate sweep, so this is safe to call from inside
        // a log line or a scoring loop.
        bool queue_is_bad(ID3D12CommandQueue* queue)
        {
            for (const BadQueue& slot : g_bad_queues)
            {
                if (slot.queue.load(std::memory_order_acquire) == queue)
                {
                    return true;
                }
            }
            return false;
        }

        // RENDER THREAD. Drops the refusals that have run out: the address may by now be
        // a queue that has nothing to do with the one refused, so it is offered again and
        // judged on its own device.
        void sweep_bad_queues()
        {
            const std::uint64_t now = g_present_count.load(std::memory_order_relaxed);
            for (BadQueue& slot : g_bad_queues)
            {
                if (slot.queue.load(std::memory_order_acquire) != nullptr
                    && now >= slot.expires_at_present.load(std::memory_order_acquire))
                {
                    slot.queue.store(nullptr, std::memory_order_release);
                }
            }
        }

        // True when the refusal is remembered. False means every slot is taken, and the
        // caller uses that to keep from logging the same refusal on every Present.
        bool mark_queue_bad(ID3D12CommandQueue* queue)
        {
            if (queue == nullptr || queue_is_bad(queue))
            {
                return false;
            }
            const std::uint64_t until = g_present_count.load(std::memory_order_relaxed) + kBadQueueTtlPresents;
            for (BadQueue& slot : g_bad_queues)
            {
                ID3D12CommandQueue* empty = nullptr;
                if (slot.queue.compare_exchange_strong(empty, queue, std::memory_order_acq_rel))
                {
                    slot.expires_at_present.store(until, std::memory_order_release);
                    return true;
                }
            }
            return false;
        }

        //==============================================================================
        // Which queue presents (see the ring's comment in overlay_internal.hpp)
        //==============================================================================

        // RENDER THREAD ONLY. `g_queue_seq_mark` is `g_exec_seq` as it stood at the
        // previous Present of the adopted swapchain, so the search window is exactly
        // "submitted since the last frame".
        static std::uint64_t g_queue_seq_mark = 0;
        // The horizon, render thread only: for each ring slot, the command lists its queue
        // submitted in each of the last kQueueScoreHorizon informative windows, and which
        // queue those columns belong to. A slot whose occupant changes starts its row
        // again, and a refusal or an eviction clears the row, so a queue that lands on a
        // freed address inherits nothing.
        static std::uint64_t g_slot_history[kQueueRing][kQueueScoreHorizon]{};
        static ID3D12CommandQueue* g_slot_owner[kQueueRing]{};
        static int g_history_at = 0;
        // When the last informative window closed. A horizon older than
        // kQueueScoreStaleMs describes a renderer that has moved on - a menu, a pause, a
        // level load - so it is thrown away rather than believed.
        static std::uint64_t g_history_ms = 0;
        static std::uint64_t g_queue_attempts = 0;
        // Whether the adapter / output / presenting trio has been written for the current
        // adoption. One set of lines per adopted swapchain, not per resize.
        static bool g_display_logged = false;
        // The next attempt count at which the indecision report repeats, doubling each
        // time: often enough that a log covering a session says the overlay never
        // decided, rarely enough not to become the log.
        static std::uint64_t g_queue_indecision_at = 240;

        // Any thread. Hands `queue` to the render thread to Release; false when there is
        // no room, which leaves the caller holding it.
        bool defer_queue_release(ID3D12CommandQueue* queue)
        {
            for (std::atomic<ID3D12CommandQueue*>& slot : g_pending_queue_release)
            {
                ID3D12CommandQueue* empty = nullptr;
                if (slot.compare_exchange_strong(empty, queue, std::memory_order_acq_rel))
                {
                    return true;
                }
            }
            return false;
        }

        void scrub_window_winner(ID3D12CommandQueue* queue);

        void drain_pending_queue_releases()
        {
            for (std::atomic<ID3D12CommandQueue*>& slot : g_pending_queue_release)
            {
                ID3D12CommandQueue* q = slot.exchange(nullptr, std::memory_order_acq_rel);
                if (q != nullptr)
                {
                    // The address is about to become anyone's, so the windows it won go
                    // with it.
                    scrub_window_winner(q);
                    q->Release();
                }
            }
        }

        void note_direct_queue(ID3D12CommandQueue* queue)
        {
            // Recording is only worth anything while the adoption is open. Once the queue
            // is captured this hook is one relaxed load and a return for the rest of the
            // process's life; the teardown's `safe_release_queue` nulls `g_queue` and so
            // re-arms it.
            if (g_queue.load(std::memory_order_relaxed) != nullptr)
            {
                return;
            }
            // The hot path: a queue already in the ring only needs its recency and its
            // submission count bumped.
            for (QueueSlot& slot : g_queue_ring)
            {
                if (slot.queue.load(std::memory_order_relaxed) == queue)
                {
                    slot.submits.fetch_add(1, std::memory_order_relaxed);
                    slot.seq.store(g_exec_seq.fetch_add(1, std::memory_order_relaxed) + 1,
                                   std::memory_order_release);
                    return;
                }
            }
            // Not in the ring, so GetDesc() is asked once per queue rather than once per
            // submission. Only DIRECT queues are remembered: a copy or compute queue can
            // never be the one a swapchain presents from.
            if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                return;
            }
            spin::SpinGuard guard(g_queue_ring_lock);
            if (g_queue.load(std::memory_order_acquire) != nullptr)
            {
                // Captured while this thread was on its way here, and the render thread
                // has already emptied the ring under this same lock - an entry now would
                // be a reference nothing ever comes back for.
                return;
            }
            // Its own slot if another thread has just inserted it, else a free one, else
            // the least recently used.
            QueueSlot* pick = nullptr;
            for (QueueSlot& slot : g_queue_ring)
            {
                ID3D12CommandQueue* held = slot.queue.load(std::memory_order_relaxed);
                if (held == queue || held == nullptr)
                {
                    pick = &slot;
                    break;
                }
                if (pick == nullptr ||
                    slot.seq.load(std::memory_order_relaxed) < pick->seq.load(std::memory_order_relaxed))
                {
                    pick = &slot;
                }
            }
            if (pick == nullptr)
            {
                return;
            }
            ID3D12CommandQueue* evicted = pick->queue.load(std::memory_order_relaxed);
            if (evicted != queue)
            {
                if (evicted != nullptr && !defer_queue_release(evicted))
                {
                    return; // nowhere to hand the eviction to, so nothing is evicted
                }
                // A reference for as long as the entry lives - the ring's comment says
                // why. The counter restarts with it and is published BEFORE the pointer,
                // so a reader cannot attribute the old queue's total to the new one. A
                // bump already in flight for the old occupant can still land on the fresh
                // counter, which costs one submission out of one window.
                queue->AddRef();
                pick->submits.store(0, std::memory_order_relaxed);
                pick->queue.store(queue, std::memory_order_release);
            }
            pick->submits.fetch_add(1, std::memory_order_relaxed);
            pick->seq.store(g_exec_seq.fetch_add(1, std::memory_order_relaxed) + 1,
                            std::memory_order_release);
        }

        // RENDER THREAD. Forgets everything a queue submitted, so a refused queue stops
        // leading and a released address cannot pass its score to whatever lands there.
        void scrub_window_winner(ID3D12CommandQueue* queue)
        {
            if (queue == nullptr)
            {
                return;
            }
            for (int i = 0; i < kQueueRing; ++i)
            {
                if (g_slot_owner[i] == queue)
                {
                    g_slot_owner[i] = nullptr;
                    for (std::uint64_t& cell : g_slot_history[i])
                    {
                        cell = 0;
                    }
                }
            }
        }

        // RENDER THREAD. What one slot submitted across the whole horizon.
        std::uint64_t slot_score(int slot)
        {
            std::uint64_t sum = 0;
            for (const std::uint64_t cell : g_slot_history[slot])
            {
                sum += cell;
            }
            return sum;
        }

        // RENDER THREAD. The two best-scoring slots of the horizon.
        void score_horizon(QueuePick& out)
        {
            for (int i = 0; i < kQueueRing; ++i)
            {
                ID3D12CommandQueue* q = g_slot_owner[i];
                if (q == nullptr)
                {
                    continue;
                }
                const std::uint64_t score = slot_score(i);
                if (score == 0)
                {
                    continue;
                }
                if (score > out.score)
                {
                    out.runner_up = out.queue;
                    out.runner_score = out.score;
                    out.queue = q;
                    out.score = score;
                }
                else if (score > out.runner_score)
                {
                    out.runner_up = q;
                    out.runner_score = score;
                }
            }
        }

        QueuePick pick_presenting_queue(std::uint64_t after)
        {
            // The window baselines: what each slot's counter stood at when the previous
            // window closed, and which queue that reading belonged to.
            static std::uint64_t base[kQueueRing]{};
            static ID3D12CommandQueue* base_queue[kQueueRing]{};
            std::uint64_t window[kQueueRing]{};
            bool informative = false;
            for (int i = 0; i < kQueueRing; ++i)
            {
                const QueueSlot& slot = g_queue_ring[i];
                ID3D12CommandQueue* q = slot.queue.load(std::memory_order_acquire);
                // `submits` is read BEFORE `seq` because a submitting thread publishes them
                // the other way round. Reading them in that order could pair a new count
                // with the old sequence, which would drop the slot from this window and
                // still take its baseline forward - losing those submissions from the next
                // window as well. This way round the pairing errs the other way and the
                // count simply lands in the next window.
                const std::uint64_t submits = slot.submits.load(std::memory_order_acquire);
                const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
                // A slot whose queue changed inside this window restarted its counter with
                // it, so the whole reading belongs to the window.
                const bool same = q == base_queue[i] && submits >= base[i];
                const std::uint64_t count = same ? submits - base[i] : submits;
                base[i] = submits;
                base_queue[i] = q;
                if (q != g_slot_owner[i])
                {
                    g_slot_owner[i] = q; // a new occupant inherits nothing from the last
                    for (std::uint64_t& cell : g_slot_history[i])
                    {
                        cell = 0;
                    }
                }
                // A refused queue is no answer, so its work is not counted either: left in
                // the horizon it would keep the leader from ever reaching the margin.
                if (q == nullptr || seq <= after || count == 0 || queue_is_bad(q))
                {
                    continue;
                }
                window[i] = count;
                informative = true;
            }
            QueuePick out{};
            const std::uint64_t now = ::GetTickCount64();
            if (g_history_ms != 0 && now - g_history_ms > kQueueScoreStaleMs)
            {
                for (int i = 0; i < kQueueRing; ++i)
                {
                    for (std::uint64_t& cell : g_slot_history[i])
                    {
                        cell = 0;
                    }
                }
                g_history_at = 0;
            }
            if (!informative)
            {
                // Nothing unrefused submitted between the last two Presents: a generated
                // frame, a paused game. The horizon does not move, and the scores are
                // reported as they stand.
                score_horizon(out);
                return out;
            }
            g_history_ms = now;
            out.informative = true;
            for (int i = 0; i < kQueueRing; ++i)
            {
                g_slot_history[i][g_history_at] = window[i];
            }
            g_history_at = (g_history_at + 1) % kQueueScoreHorizon;
            score_horizon(out);
            for (int i = 0; i < kQueueRing; ++i)
            {
                if (out.queue != nullptr && g_slot_owner[i] == out.queue)
                {
                    out.window_count = window[i];
                }
            }
            if (out.queue != nullptr && out.score >= kQueueScoreMin
                && out.score >= kQueueScoreMargin * out.runner_score)
            {
                out.decided = true;
            }
            else if (out.queue != nullptr && out.score > out.runner_score
                     && g_queue_attempts >= kQueueDecideByPresents)
            {
                // The margin is not coming. A queue of this device that does not present
                // costs ordering, not the device, and an overlay that never appears with
                // nothing in the log to explain it is the worse outcome.
                out.decided = true;
                out.by_plurality = true;
            }
            return out;
        }

        // The caller holds `g_queue_ring_lock` and gets the pointers back to Release once
        // it has let go of it. A final Release runs the destructor of whatever wraps the
        // queue - Streamline, ReShade, a frame-generation proxy - and a wrapper that
        // submits from its destructor would come back through hk_ExecuteCommandLists into
        // this same non-recursive lock. So no Release ever happens under it.
        void take_queue_ring_locked(ID3D12CommandQueue* (&out)[kQueueRing])
        {
            for (int i = 0; i < kQueueRing; ++i)
            {
                out[i] = g_queue_ring[i].queue.exchange(nullptr, std::memory_order_acq_rel);
                g_queue_ring[i].submits.store(0, std::memory_order_relaxed);
                scrub_window_winner(out[i]);
            }
        }

        void release_taken_queues(ID3D12CommandQueue* (&taken)[kQueueRing])
        {
            for (ID3D12CommandQueue*& q : taken)
            {
                if (q != nullptr)
                {
                    q->Release();
                    q = nullptr;
                }
            }
        }

        void release_queue_ring()
        {
            ID3D12CommandQueue* taken[kQueueRing]{};
            {
                spin::SpinGuard guard(g_queue_ring_lock);
                take_queue_ring_locked(taken);
            }
            release_taken_queues(taken);
            drain_pending_queue_releases();
        }

        void reset_queue_adoption(bool on_render_thread)
        {
            g_queue_attempts = 0;
            g_queue_indecision_at = 240;
            for (int i = 0; i < kQueueRing; ++i)
            {
                g_slot_owner[i] = nullptr;
                for (std::uint64_t& cell : g_slot_history[i])
                {
                    cell = 0;
                }
            }
            g_history_at = 0;
            g_history_ms = 0;
            g_display_logged = false;
            // `pick_presenting_queue`'s per-slot baselines are deliberately left alone:
            // each is compared against the slot's current occupant, so a stale reading
            // cannot be attributed to a new one.
            if (on_render_thread)
            {
                release_queue_ring();
            }
            // Only submissions made from here on may be adopted: a ring entry from before
            // a teardown belongs to a device this module no longer knows anything about.
            g_queue_seq_mark = g_exec_seq.load(std::memory_order_acquire);
        }

        // The scoring is not converging: two DIRECT queues are winning windows evenly, so
        // neither leads by the margin, and "no overlay and no explanation" is the worst of
        // the possible outcomes. Repeated at each doubling of the attempt count, so a log
        // covering a whole session says so more than once without becoming the log.
        // Nothing is given up - the scoring goes on with every Present.
        void log_queue_indecision()
        {
            std::wstring ring;
            for (const QueueSlot& slot : g_queue_ring)
            {
                ID3D12CommandQueue* q = slot.queue.load(std::memory_order_acquire);
                if (q == nullptr)
                {
                    continue;
                }
                ring += std::format(L"{}{:p}@{}x{}{}",
                                    ring.empty() ? L"" : L", ",
                                    static_cast<void*>(q),
                                    slot.seq.load(std::memory_order_acquire),
                                    slot.submits.load(std::memory_order_acquire),
                                    queue_is_bad(q) ? L" (refused)" : L"");
            }
            QueuePick score{};
            score_horizon(score);
            mm::logf(L"no presenting queue after {} Presents of the adopted swapchain: no DIRECT queue "
                     L"has submitted {} command list(s) across the last {} windows and {}x the "
                     L"runner-up's, so nothing of the overlay is submitted. Drawing on a queue that does "
                     L"not present this swapchain puts the overlay out of order with the flip - torn, a "
                     L"frame late or invisible - which is why this waits rather than guesses. DIRECT "
                     L"queues seen (pointer@last submission x total submissions): {}. Leading {:p} with "
                     L"{}, runner-up {:p} with {}. After {} Presents the leader is taken anyway.",
                     g_queue_attempts,
                     kQueueScoreMin,
                     kQueueScoreHorizon,
                     kQueueScoreMargin,
                     ring.empty() ? std::wstring{L"none"} : ring,
                     static_cast<void*>(score.queue),
                     score.score,
                     static_cast<void*>(score.runner_up),
                     score.runner_score,
                     kQueueDecideByPresents);
            set_hide_reason(L"no presenting DIRECT command queue identified yet");
        }

        // A colour space is how the swapchain's numbers reach the display, and it is the
        // only honest answer to "is HDR on?" - the game's own menu says what was asked
        // for, not what the display is doing.
        const wchar_t* colour_space_name(DXGI_COLOR_SPACE_TYPE cs)
        {
            switch (cs)
            {
            case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
                return L"sRGB / SDR";
            case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                return L"HDR10 (PQ, Rec.2020)";
            case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
                return L"scRGB (linear, HDR)";
            case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
                return L"HDR10 studio range";
            default:
                return L"another colour space";
            }
        }

        // RENDER THREAD, once per adoption. Three questions a bug report about this mod
        // always needs and can never answer from the outside: what the GPU and its driver
        // are, what the display is doing with the frames, and how the game is presenting
        // them. All of it read from objects this module already holds; nothing is asked of
        // the player and nothing is read out of the game's own settings, which describe
        // what was requested rather than what is happening.
        void log_display_environment(IDXGISwapChain* swapchain)
        {
            if (g_device == nullptr || swapchain == nullptr)
            {
                // Not silent: this used to be called before the device existed, and a
                // guard that returns without a word is how three lines went missing.
                mm::logf(L"  the display environment cannot be read yet (device {:p}, swapchain {:p})",
                         static_cast<void*>(g_device),
                         static_cast<void*>(swapchain));
                return;
            }

            // The adapter, by LUID, because that is the one identity a D3D12 device
            // exposes. CheckInterfaceSupport on IID_IDXGIDevice answers with the
            // user-mode driver's version - the number a driver release is known by.
            IDXGIFactory4* factory = nullptr;
            if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            {
                IDXGIAdapter1* adapter = nullptr;
                if (SUCCEEDED(factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))
                    && adapter != nullptr)
                {
                    DXGI_ADAPTER_DESC1 ad{};
                    LARGE_INTEGER umd{};
                    const bool have_desc = SUCCEEDED(adapter->GetDesc1(&ad));
                    const bool have_umd = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                    mm::logf(L"  adapter \"{}\" vendor 0x{:04X} device 0x{:04X}, {} MB dedicated video "
                             L"memory, user-mode driver {}.{}.{}.{}",
                             have_desc ? ad.Description : L"(unnamed)",
                             have_desc ? ad.VendorId : 0u,
                             have_desc ? ad.DeviceId : 0u,
                             have_desc ? static_cast<std::uint64_t>(ad.DedicatedVideoMemory / (1024 * 1024))
                                       : 0ull,
                             have_umd ? HIWORD(umd.HighPart) : 0,
                             have_umd ? LOWORD(umd.HighPart) : 0,
                             have_umd ? HIWORD(umd.LowPart) : 0,
                             have_umd ? LOWORD(umd.LowPart) : 0);
                    safe_release(adapter);
                }
                safe_release(factory);
            }

            // The output, for the colour space. GetContainingOutput has no answer while
            // the game is windowed on some drivers, which is itself worth saying.
            IDXGIOutput* output = nullptr;
            if (SUCCEEDED(swapchain->GetContainingOutput(&output)) && output != nullptr)
            {
                IDXGIOutput6* out6 = nullptr;
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out6))) && out6 != nullptr)
                {
                    DXGI_OUTPUT_DESC1 od{};
                    if (SUCCEEDED(out6->GetDesc1(&od)))
                    {
                        mm::logf(L"  output \"{}\" {}x{}, {} bit(s) per colour, {}, {} nit peak",
                                 od.DeviceName,
                                 od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                                 od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                                 od.BitsPerColor,
                                 colour_space_name(od.ColorSpace),
                                 static_cast<int>(od.MaxLuminance));
                    }
                    safe_release(out6);
                }
                safe_release(output);
            }
            else
            {
                mm::log(L"  the swapchain reports no containing output, which a windowed game can do");
            }

            // How the game presents: the flip model and whether it is synchronised. The
            // sync interval is the game's own argument to Present, recorded by the hook -
            // 0 means it is not waiting for the display.
            BOOL fullscreen = FALSE;
            const bool have_fs = SUCCEEDED(swapchain->GetFullscreenState(&fullscreen, nullptr));
            const unsigned sync = g_present_sync.load(std::memory_order_relaxed);
            const unsigned flags = g_present_flags.load(std::memory_order_relaxed);
            mm::logf(L"  presenting {}, sync interval {}{}, present flags 0x{:X}{}",
                     !have_fs ? L"in an unknown mode"
                              : (fullscreen ? L"in exclusive fullscreen" : L"windowed or borderless"),
                     sync,
                     sync == 0 ? L" (not waiting for the display)" : L"",
                     flags,
                     (flags & DXGI_PRESENT_ALLOW_TEARING) != 0 ? L" (tearing allowed)" : L"");
        }

        bool adopt_presenting_queue(IDXGISwapChain* swapchain)
        {
            // A ring entry holds a reference, so a pointer read out of the ring below is
            // alive for the rest of this function - as long as nothing releases an
            // eviction underneath it. Which is why the pending list is drained FIRST.
            drain_pending_queue_releases();
            sweep_bad_queues();

            // The first report is loud enough to be found in a log and rare enough not to
            // be noise: ~4 s at 60 Hz. Then every doubling.
            if (++g_queue_attempts >= g_queue_indecision_at)
            {
                log_queue_indecision();
                g_queue_indecision_at *= 2;
            }
            const std::uint64_t window_from = g_queue_seq_mark;
            g_queue_seq_mark = g_exec_seq.load(std::memory_order_acquire);
            const QueuePick pick = pick_presenting_queue(window_from);
            if (!pick.informative)
            {
                // No unrefused queue submitted between the last two Presents: a generated
                // frame, a paused game, a window whose only entrant is already refused.
                // That says nothing, so no score moves and nothing here changes.
                return false;
            }
            if (!pick.decided)
            {
                return false; // a leader, but not yet by the margin
            }
            ID3D12CommandQueue* last = pick.queue;

            // The leader holds. Its type is asked again: the ring's pointer match is what
            // the recency bump goes through, and a queue is only worth submitting on if it
            // still calls itself DIRECT.
            if (last->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                if (mark_queue_bad(last))
                {
                    mm::logf(L"the queue {:p} leading the window scoring no longer reports a DIRECT type "
                             L"- it is refused and another one is waited for",
                             static_cast<void*>(last));
                }
                scrub_window_winner(last);
                return false;
            }

            // The remaining question is the device: ID3D12Resource::GetDevice on back
            // buffer 0 is the one link from the swapchain to a device that every wrapper
            // forwards, and recording our command list on a queue of any other device is
            // an immediate device removal.
            ID3D12Resource* buffer = nullptr;
            if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))) || buffer == nullptr)
            {
                return false; // a resize in flight; ask again on the next Present
            }
            ID3D12Device* sc_device = nullptr;
            ID3D12Device* q_device = nullptr;
            const bool have_sc = SUCCEEDED(buffer->GetDevice(IID_PPV_ARGS(&sc_device))) && sc_device != nullptr;
            const bool have_q = SUCCEEDED(last->GetDevice(IID_PPV_ARGS(&q_device))) && q_device != nullptr;
            const bool same = have_sc && have_q && sc_device == q_device;
            safe_release(buffer);
            if (!same)
            {
                if (mark_queue_bad(last))
                {
                    mm::logf(L"the DIRECT queue {:p} leads the scoring with {} command list(s) across "
                             L"the last {} windows, but its ID3D12Device {:p} is not the device {:p} "
                             L"that owns the back buffers - the queue is refused and another one is "
                             L"waited for",
                             static_cast<void*>(last),
                             pick.score,
                             kQueueScoreHorizon,
                             static_cast<void*>(q_device),
                             static_cast<void*>(sc_device));
                }
                scrub_window_winner(last);
                safe_release(sc_device);
                safe_release(q_device);
                return false;
            }

            // A reference is held from here until the teardown releases it: the queue is
            // the game's, and a game that destroys it (a device reset, a renderer swap)
            // would leave this module submitting command lists to freed memory.
            //
            // The store and the ring's release go together under the ring's own lock: a
            // submitting thread that is already past its "is anything captured?" check
            // must not be able to put an entry into a ring nothing will empty again.
            last->AddRef();
            ID3D12CommandQueue* taken[kQueueRing]{};
            {
                spin::SpinGuard guard(g_queue_ring_lock);
                g_queue.store(last, std::memory_order_release);
                // The question is answered, so the ring's remaining references - a
                // frame-generation queue, another overlay's - are dropped here rather than
                // held for the session. The store and the emptying happen together under
                // the lock so a submitting thread cannot put an entry into a ring that is
                // about to be abandoned; the Releases themselves happen after it.
                take_queue_ring_locked(taken);
            }
            release_taken_queues(taken);
            drain_pending_queue_releases();
            // The re-adoption cap counts attempts that got nowhere. This one arrived, so
            // the count starts again: a player cycling the video settings is not a cycle
            // of failures.
            g_readopt_count.store(0, std::memory_order_relaxed);
            const D3D12_COMMAND_QUEUE_DESC desc = last->GetDesc();
            mm::logf(L"captured the PRESENTING DIRECT command queue {:p} (priority {}, flags {}): it "
                     L"submitted {} command list(s) across the last {} windows before a Present of the "
                     L"adopted swapchain against a runner-up's {}, {} in the window that decided it, "
                     L"and its ID3D12Device {:p} is the one that owns the back buffers",
                     static_cast<void*>(last),
                     desc.Priority,
                     static_cast<unsigned>(desc.Flags),
                     pick.score,
                     kQueueScoreHorizon,
                     pick.runner_score,
                     pick.window_count,
                     static_cast<void*>(q_device));
            if (pick.by_plurality)
            {
                mm::logf(L"it was taken on a plurality, not on the {}x margin: after {} Presents two "
                         L"DIRECT queues of this device were still sharing the work. If the overlay "
                         L"tears, appears a frame late or does not appear at all, this is the choice to "
                         L"doubt first.",
                         kQueueScoreMargin,
                         kQueueDecideByPresents);
            }
            if (pick.runner_up != nullptr)
            {
                // The one thing a bug report cannot reconstruct: which other DIRECT queue
                // of this device was close. A wrong choice here does not remove the
                // device - it leaves the overlay unordered against the flip, so this is
                // the line to read when the minimap tears or never appears.
                mm::logf(L"the runner-up was the DIRECT queue {:p} with {} command list(s) across the "
                         L"same windows; if the overlay tears or does not appear, this is the queue that "
                         L"was not taken",
                         static_cast<void*>(pick.runner_up),
                         pick.runner_score);
            }
            safe_release(sc_device);
            safe_release(q_device);
            return true;
        }

        void engage_terminal_off(const wchar_t* why);

        void request_readoption(const wchar_t* why)
        {
            if (g_readopt.exchange(true, std::memory_order_release))
            {
                return; // one is already pending; the render thread will service it
            }
            const std::uint64_t n = g_readopt_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n > kMaxReadoptions)
            {
                // Release and re-adopt, over and over, is not a recovery: each cycle costs
                // the game a stall and gets the overlay no further.
                g_readopt.store(false, std::memory_order_release);
                mm::logf(L"the overlay has re-adopted the swapchain {} times this session and this "
                         L"Present asked for another ({}), which is a cycle rather than a recovery",
                         n - 1,
                         why);
                engage_terminal_off(L"the overlay is off for the rest of this session: re-adopting the "
                                    L"swapchain stopped getting anywhere.");
                return;
            }
            mm::logf(L"the overlay is releasing its D3D12 objects and will adopt the swapchain again "
                     L"on a later Present (re-adoption {} of at most {} this session): {}",
                     n,
                     kMaxReadoptions,
                     why);
        }

        //==============================================================================
        // A REMOVED DEVICE IS TERMINAL
        //==============================================================================
        //
        // Every D3D12 object this module holds belongs to one device. Once that device is
        // removed they are all dead, and building them again is worse than doing nothing:
        // ImGui's DX12 backend uploads its font atlas behind a fence wait with no
        // timeout, so a re-init on a dead device wedges the render thread and takes the
        // game's own device-removed recovery down with it. So the state is one-way -
        // `g_device_removed` is set here, released from once inside the next Present, and
        // cleared only by `overlay::start()` or the master switch's `finish_stop()`.

        // GetDeviceRemovedReason answers with one of these, and the bare number is
        // unreadable in a bug report.
        const wchar_t* removed_reason_name(HRESULT hr)
        {
            switch (hr)
            {
            case S_OK:
                return L"S_OK";
            case DXGI_ERROR_DEVICE_HUNG:
                return L"DXGI_ERROR_DEVICE_HUNG";
            case DXGI_ERROR_DEVICE_REMOVED:
                return L"DXGI_ERROR_DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_RESET:
                return L"DXGI_ERROR_DEVICE_RESET";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                return L"DXGI_ERROR_DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL:
                return L"DXGI_ERROR_INVALID_CALL";
            default:
                return L"an HRESULT outside the removal family";
            }
        }

        // The terminal off state. `why` opens the line; everything after it is the same
        // whichever way the overlay got here.
        void engage_terminal_off(const wchar_t* why)
        {
            if (g_device_removed.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            mm::logf(L"{} Nothing of this mod is submitted again. Its objects are released inside the "
                     L"next Present IF the game presents again; a game that never does keeps them "
                     L"allocated until it exits, which is deliberate - releasing them is the render "
                     L"thread's job and no other thread may do it. If the game does not recover on its "
                     L"own, set overlay_hooks = 0 in config_wuchang_minimap.txt and restart - the mod "
                     L"then never touches DirectX.",
                     why);
        }

        void engage_device_removal()
        {
            engage_terminal_off(L"the overlay is off for the rest of this session: the D3D12 device it "
                                L"adopted has been removed.");
        }

        bool device_alive(const wchar_t* what)
        {
            if (g_device_removed.load(std::memory_order_acquire))
            {
                return false;
            }
            if (g_device == nullptr)
            {
                return true; // nothing adopted yet, so nothing of ours can be dead
            }
            const HRESULT reason = g_device->GetDeviceRemovedReason();
            if (reason == S_OK)
            {
                return true;
            }
            mm::logf(L"the adopted D3D12 device {:p} reports {} (0x{:08X}) at {}",
                     static_cast<void*>(g_device),
                     removed_reason_name(reason),
                     static_cast<unsigned>(reason),
                     what);
            engage_device_removal();
            return false;
        }

        // The stage names are ASCII literals and the log takes wide strings. An explicit
        // cast loop rather than `std::wstring(a.begin(), a.end())`, which warns (C4244).
        std::wstring stage_w(const char* s)
        {
            const char* p = s != nullptr ? s : "?";
            std::wstring out;
            for (; *p != '\0'; ++p)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
            }
            return out;
        }

        // Every hide/show transition is logged with its reason, rate-limited by
        // hide_reason_log_ms.
        void set_hide_reason(const wchar_t* text)
        {
            if (::wcscmp(g_hide_reason, text) == 0)
            {
                return; // unchanged - nothing to record, nothing to log
            }
            ::wcsncpy_s(g_hide_reason, text, std::size(g_hide_reason) - 1);
            const std::uint64_t now = ::GetTickCount64();
            const std::uint64_t held = g_reason_since_ms == 0 ? 0 : now - g_reason_since_ms;
            g_reason_since_ms = now;
            if (::wcscmp(g_reason_logged, text) == 0)
            {
                return; // flapping between two states we already reported
            }
            if (now - g_reason_log_ms < static_cast<std::uint64_t>(mm::cfg_cached().hide_reason_log_ms))
            {
                ++g_reason_suppressed;
                return;
            }
            const bool visible = ::wcscmp(text, L"visible") == 0;
            MM_LOGV(L"minimap {}: {} (previous state held {} ms{})",
                    visible ? L"SHOWN" : L"HIDDEN",
                    text,
                    held,
                    g_reason_suppressed != 0 ? std::format(L", {} change(s) suppressed",
                                                           g_reason_suppressed)
                                             : std::wstring{});
            ::wcsncpy_s(g_reason_logged, text, std::size(g_reason_logged) - 1);
            g_reason_log_ms = now;
            g_reason_suppressed = 0;
        }

        //==============================================================================
        // Original functions
        //==============================================================================

        // The probe frame's state, declared here because releasing the render targets
        // resets it. THE PROBE FRAME below is what it is for.
        enum class Probe
        {
            Pending,   // the device objects exist, nothing has been submitted yet
            Submitted, // the barrier-only list is on the queue; the next Present judges it
            Passed,    // the GPU ran it and the device still answers S_OK
        };
        static Probe g_probe = Probe::Pending;
        // The fence value the probe's submission signals, and how many Presents have gone
        // by waiting for it. Zero means the Signal itself failed, so nothing can say the
        // barrier pair ever ran.
        static std::uint64_t g_probe_fence = 0;
        static int g_probe_waits = 0;
        // How long a probe may stay in flight before the overlay stops believing in it:
        // one barrier pair is a fraction of a frame, so a second of Presents is generous.
        constexpr int kProbeWaitPresents = 60;

        //==============================================================================
        // Teardown of the swapchain-dependent objects
        //==============================================================================

        void release_render_targets()
        {
            for (UINT i = 0; i < kMaxBuffers; ++i)
            {
                safe_release(g_backbuffers[i]);
                g_rtv[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            safe_release(g_rtv_heap);
            g_rt_ready = false;
            // The probe barrier named a back buffer that no longer exists, so whatever it
            // proved it no longer proves: the next set of render targets gets its own
            // probe frame. Without this a resize between the submit and the next Present
            // promotes an untested assumption to Passed.
            g_probe = Probe::Pending;
            g_probe_fence = 0;
            g_probe_waits = 0;
        }

        void wait_for_gpu()
        {
            if (g_fence == nullptr || g_fence_event == nullptr)
            {
                return;
            }
            if (g_fence->GetCompletedValue() >= g_fence_value)
            {
                return;
            }
            if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fence_value, g_fence_event)))
            {
                ::WaitForSingleObject(g_fence_event, 1000);
            }
        }

        // One allocator per back buffer, created for every buffer that has none - a
        // fullscreen toggle can raise BufferCount after init. `g_buffer_count` is
        // already clamped to kMaxBuffers.
        bool ensure_frame_allocators()
        {
            if (g_device == nullptr)
            {
                return false;
            }
            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (g_frames[i].allocator != nullptr)
                {
                    continue;
                }
                if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&g_frames[i].allocator))))
                {
                    mm::logf(L"CreateCommandAllocator({}) failed", i);
                    return false;
                }
                g_frames[i].fence_value = 0;
            }
            return true;
        }

        bool create_render_targets(IDXGISwapChain* swapchain)
        {
            release_render_targets();

            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(swapchain->GetDesc(&desc)))
            {
                mm::log(L"GetDesc failed - cannot create render targets");
                return false;
            }
            g_buffer_count = (std::min)(desc.BufferCount, static_cast<UINT>(kMaxBuffers));
            if (g_buffer_count == 0)
            {
                g_buffer_count = 2;
            }
            g_format = desc.BufferDesc.Format;
            g_width = desc.BufferDesc.Width;
            g_height = desc.BufferDesc.Height;
            g_hwnd = desc.OutputWindow;

            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = g_buffer_count;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap))))
            {
                mm::log(L"CreateDescriptorHeap(RTV) failed");
                return false;
            }

            const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g_buffer_count; ++i)
            {
                if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]))))
                {
                    mm::logf(L"GetBuffer({}) failed", i);
                    release_render_targets();
                    return false;
                }
                g_device->CreateRenderTargetView(g_backbuffers[i], nullptr, handle);
                g_rtv[i] = handle;
                handle.ptr += stride;
            }

            // Does the back buffer belong to the device taken off the queue? The queue
            // was already matched against the back buffers' device at adoption, but the
            // device this module holds comes off the queue and the swapchain can be
            // replaced under us (IDXGISwapChain::GetDevice does not work through this
            // game's ReShade wrapper). `ID3D12Resource::GetDevice` on a back buffer
            // answers authoritatively and is the one link from the swapchain to a device
            // the wrapper does forward. Recording our command list on a queue of another
            // device is an immediate device removal: hard reject.
            if (g_backbuffers[0] != nullptr)
            {
                ID3D12Device* owner = nullptr;
                if (SUCCEEDED(g_backbuffers[0]->GetDevice(IID_PPV_ARGS(&owner))) && owner != nullptr)
                {
                    const bool same = owner == g_device;
                    owner->Release();
                    if (!same)
                    {
                        mm::log(L"the back buffers belong to a different ID3D12Device than the captured "
                                L"command queue - dropping the queue so another one can be captured");
                        mark_queue_bad(g_queue.load(std::memory_order_acquire));
                        release_render_targets();
                        request_readoption(L"the captured command queue belongs to another device");
                        return false;
                    }
                }
            }

            if (!ensure_frame_allocators())
            {
                release_render_targets();
                return false;
            }

            g_rt_ready = true;
            // The reasoning, once a session; the per-creation line only names the
            // assumption, because render targets are rebuilt on every resize.
            static bool assumption_explained = false;
            if (!assumption_explained)
            {
                assumption_explained = true;
                mm::log(L"the overlay assumes a back buffer is in D3D12_RESOURCE_STATE_PRESENT when the "
                        L"game calls Present: D3D12 cannot be asked what state a resource is in, and "
                        L"PRESENT is the same state as COMMON, which is what a swapchain buffer has to "
                        L"be in for the flip. The probe frame is what tests that assumption before "
                        L"anything else of the overlay exists.");
            }
            mm::logf(L"render targets: {} buffer(s), {}x{}, {}, hwnd 0x{:X}, swapchain flags 0x{:X}, "
                     L"swap effect {}. The back buffers are assumed to be in "
                     L"D3D12_RESOURCE_STATE_PRESENT at Present.",
                     g_buffer_count,
                     g_width,
                     g_height,
                     format_name(g_format),
                     reinterpret_cast<std::uintptr_t>(g_hwnd),
                     static_cast<unsigned>(desc.Flags),
                     static_cast<int>(desc.SwapEffect));
            return true;
        }

        //==============================================================================
        // Per-frame work
        //==============================================================================

        void build_ui()
        {
            // `raw` is what the panel edits and what Save writes; `cfg` is the same
            // settings with every pixel key multiplied by the UI scale and is what the
            // HUD draws from. Keeping them apart stops a scaled value ever being written
            // back into the config file.
            const mm::Config& raw = mm::cfg_cached();
            mm::Config cfg = ui_scaled(raw, g_ui_scale);
            // The look, once per frame: every draw path below reads these two globals
            // instead of asking the config what colour it is.
            g_palette = raw.palette;
            g_plate = gly::theme_colors(raw.theme).plate;
            // The disc-drawing helpers take geometry, not the config, so the live
            // roundness is cached here once per frame (render thread only).
            mm::Snapshot snap{};
            const bool have = mm::read_snapshot(snap);

            const bool map_open = mm::g_map_open.load(std::memory_order_relaxed);
            const std::uint64_t frame_now = ::GetTickCount64();

            // THE HUD FADE. Its target is the gate result and nothing else: 0 applies
            // instantly (and the gate stops the draw anyway), only showing is eased over
            // 150 ms. It is applied by scaling the opacity keys of the per-frame config
            // copy, so nothing downstream has to know the fade exists.
            const bool gate_open = have && hud_gate(cfg, snap, have, frame_now) == nullptr;
            if (gate_open && !g_hud_gate_ever_open.load(std::memory_order_relaxed))
            {
                // The first frame anything of ours could be seen; the loop thread's
                // first-run tip waits for it.
                g_hud_gate_ever_open.store(true, std::memory_order_release);
            }
            const float fade = hud_fade_step(gate_open, frame_now);
            cfg.opacity *= fade;
            cfg.compass_opacity *= fade;
            cfg.highlight_alpha_near *= fade;
            cfg.highlight_alpha_far *= fade;

            // The mouse cursor belongs to whoever is taking the input. Both conditions
            // are plain reads of the live flags, so the frame the map or the panel
            // closes is the frame the game gets the cursor back.
            ImGui::GetIO().MouseDrawCursor = map_open || mm::g_panel_open.load(std::memory_order_relaxed);

            // The pad, into ImGui's own nav. Only while the panel is open, and from the
            // state the loop thread sampled - never a poll from here.
            feed_pad_nav(raw);

            // THE ONE MARKER PASS. The minimap, the full map, the compass pips and the
            // x-ray highlight all read the same published buffer: it is walked once here
            // and each of them filters the result.
            if (have)
            {
                build_frame_candidates(snap);
            }
            else
            {
                g_frame_cands.clear();
                g_frame_marker_total = 0;
                g_frame_bad_cat = 0;
            }

            // NEAREST UNFOUND (the waypoint_nearest_key hotkey). The pick lives here
            // because the frame's candidate list is what knows both distance and the
            // category mask in force. It has no radius: the whole chapter's published
            // buffer is a candidate.
            if (g_nearest_request.exchange(false, std::memory_order_acquire))
            {
                const FrameCand* best = nullptr;
                if (cfg.markers_enabled)
                {
                    for (const FrameCand& c : g_frame_cands)
                    {
                        if (c.found ||
                            !mdb::cat_enabled(cfg.markers_categories, static_cast<mdb::Cat>(c.cat)))
                        {
                            continue;
                        }
                        if (best == nullptr || c.d2_3d < best->d2_3d)
                        {
                            best = &c;
                        }
                    }
                }
                char note[160]{};
                if (best == nullptr)
                {
                    (void)std::snprintf(note, sizeof(note),
                                        cfg.markers_enabled
                                            ? "nothing unfound in the categories you have on"
                                            : "markers are turned off");
                }
                else
                {
                    mv::Waypoint wp{};
                    wp.set = true;
                    wp.x = best->m->x;
                    wp.y = best->m->y;
                    wp.z = best->m->z;
                    const char* name = mdb::display_label(static_cast<mdb::Cat>(best->cat), best->m->label);
                    if (mm::add_waypoint(wp))
                    {
                        (void)std::snprintf(note, sizeof(note), "waypoint: %s, %.0f m away", name,
                                            std::sqrt(static_cast<double>(best->d2_3d)) / 100.0);
                    }
                    else
                    {
                        (void)std::snprintf(note, sizeof(note), "%zu waypoints already - clear one first",
                                            mv::kMaxWaypoints);
                    }
                }
                toast_for(note, 3500);
            }

            if (mm::g_panel_open.load(std::memory_order_relaxed))
            {
                draw_panel(raw, snap, have);
                mm::g_panel_drew_frame.store(true, std::memory_order_relaxed);
            }
            else if (g_capture_row >= 0)
            {
                // The panel closed with a capture armed. Disarmed here rather than in the
                // close paths - this runs every frame, so the keyboard cannot stay
                // swallowed with no panel on screen.
                arm_capture(-1);
            }

            if (map_open)
            {
                draw_full_map(raw, snap, have, g_ui_scale);
            }
            if (g_map_was_open && !mm::g_map_open.load(std::memory_order_relaxed))
            {
                // Closed (by the key, by the pad chord, by the gate, or from inside the
                // map): the next open starts centred on the player again, with the map
                // mode's panels and search box reset. close_map has already done that
                // for its own routes; this is the edge the map never sees.
                //
                // The zoom the player left the map at becomes the zoom it opens with.
                // The view holds a DPI-scaled number (draw_full_map's `zscale`), so it is
                // un-scaled on the way back into the config, which is always literal.
                if (g_mv_init)
                {
                    const float zscale =
                        (raw.zoom_dpi_scaled && g_ui_scale > 0.0f) ? 1.0f / g_ui_scale : 1.0f;
                    mm::Config edit = mm::config();
                    edit.map_zoom = static_cast<float>(g_mv.uu_per_px) / zscale;
                    if (edit.map_zoom != raw.map_zoom)
                    {
                        mm::set_config(edit);
                        mm::g_save_config_soon = true;
                    }
                }
                g_mv_init = false;
                reset_map_mode();
            }
            g_map_was_open = mm::g_map_open.load(std::memory_order_relaxed);

            if (!cfg.show_minimap)
            {
                set_hide_reason(L"the minimap is switched off");
            }
            else if (mm::g_map_open.load(std::memory_order_relaxed))
            {
                // The full map replaces the minimap while it is up, so the slicer never
                // cuts two windows a frame.
                set_hide_reason(L"the full map is open");
            }
            else
            {
                if (g_pf_minimap < 0)
                {
                    g_pf_minimap = mm::perf_register("minimap draw", perf::Thread::Render);
                }
                const mm::PerfScope scope(g_pf_minimap);
                draw_minimap(cfg, snap, have);
            }

            // The compass and the x-ray highlight ask the same gate the minimap does -
            // one evaluation, no second set of rules, no second latch - and additionally
            // stand down while the full map is open.
            const bool hud_ok = gate_open && !mm::g_map_open.load(std::memory_order_relaxed);
            draw_compass(cfg, snap, hud_ok);
            draw_highlight(cfg, snap, hud_ok);

            // The clipboard result comes back from the loop thread as text plus a flag;
            // turn it into a toast here, where toasts live.
            if (g_toast_pending_ready.exchange(false, std::memory_order_acquire))
            {
                char text[160]{};
                unsigned ms = 2500;
                {
                    spin::SpinGuard guard(g_toast_lock);
                    ::strncpy_s(text, sizeof(text), g_toast_pending, _TRUNCATE);
                    ms = g_toast_pending_ms;
                }
                toast_for(text, ms);
            }

            // Toasts last, so they sit over everything they are talking about.
            draw_toast();
        }

        // The D3D12 half of the start-up: the device, the render targets, one allocator
        // per back buffer, the command list and the fence. Nothing here draws and nothing
        // here is ImGui, which is what lets the probe frame run between the two halves.
        //
        // Called on every Present until ImGui is up - the probe frame costs at least one -
        // so every object is created only where it does not exist yet.
        bool ensure_device_objects(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
        {
            if (g_device == nullptr)
            {
                // The device comes off the captured QUEUE, not off the swapchain:
                // IDXGISwapChain::GetDevice(ID3D12Device) fails on this game's ReShade
                // wrapper. ID3D12CommandQueue::GetDevice always works.
                HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g_device));
                if (FAILED(hr) || g_device == nullptr)
                {
                    mm::logf(L"queue->GetDevice failed (0x{:08X}); trying the swapchain",
                             static_cast<unsigned>(hr));
                    hr = swapchain->GetDevice(IID_PPV_ARGS(&g_device));
                }
                if (FAILED(hr) || g_device == nullptr)
                {
                    mm::logf(L"no ID3D12Device reachable from either the queue or the swapchain "
                             L"(0x{:08X}) - overlay off",
                             static_cast<unsigned>(hr));
                    g_failed = true;
                    return false;
                }
            }

            // Not `g_failed`: a swapchain that will not hand out its buffers is a
            // swapchain-level failure (a resize in flight, a device that has just gone,
            // a wrapper being swapped) and those recover. `create_render_targets` has
            // already asked for a re-adoption where it knew the reason.
            if (!g_rt_ready && !create_render_targets(swapchain))
            {
                request_readoption(L"the render targets could not be created");
                return false;
            }

            // One allocator per back buffer, and grown again after any resize that
            // raises BufferCount (see ensure_frame_allocators).
            if (!ensure_frame_allocators())
            {
                g_failed = true;
                return false;
            }
            if (g_cmd_list == nullptr)
            {
                if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       g_frames[0].allocator, nullptr,
                                                       IID_PPV_ARGS(&g_cmd_list))))
                {
                    mm::log(L"CreateCommandList failed");
                    g_failed = true;
                    return false;
                }
                g_cmd_list->Close();
            }

            if (g_fence == nullptr &&
                FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            {
                mm::log(L"CreateFence failed");
                g_failed = true;
                return false;
            }
            if (g_fence_event == nullptr)
            {
                g_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
            if (g_fence_event == nullptr)
            {
                mm::log(L"CreateEvent failed");
                g_failed = true;
                return false;
            }
            return true;
        }

        //==============================================================================
        // THE PROBE FRAME
        //==============================================================================
        //
        // D3D12 has no way to ask a resource what state it is in, so the closing barrier
        // of the game's own frame is taken on trust: a swapchain buffer must be in COMMON
        // (which is the same state as PRESENT) for the flip, so PRESENT is the honest
        // assumption for the barrier that takes it to RENDER_TARGET. A frame-generation
        // proxy presenting a buffer it left in another state makes that barrier a
        // wrong-state transition, and a wrong-state transition on a real swapchain buffer
        // removes the device.
        //
        // So the FIRST thing submitted on a newly adopted queue is a command list that
        // contains only the barrier pair and no draw at all. If the assumption is wrong,
        // the Present that follows returns DEVICE_REMOVED, note_present_result logs the
        // reason and the terminal state engages - and it does so before ImGui, the font
        // atlas or a single map texture has been created, i.e. before the mod has spent
        // anything or waited on any fence that could hang.
        // True once the command list HAS BEEN SUBMITTED, not once the frame succeeded:
        // everything after o_ExecuteCommandLists is in flight and must never be Reset
        // again, so a fence failure is reported and still counts as submitted.
        bool submit_probe_frame(IDXGISwapChain* swapchain)
        {
            const UINT index = current_backbuffer_index(swapchain);
            if (index == kNoBackbuffer || g_backbuffers[index] == nullptr)
            {
                return false; // no index, no barrier - see current_backbuffer_index
            }
            FrameCtx& frame = g_frames[index];
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            if (frame.allocator == nullptr || g_cmd_list == nullptr || queue == nullptr)
            {
                return false;
            }
            if (FAILED(frame.allocator->Reset()) || FAILED(g_cmd_list->Reset(frame.allocator, nullptr)))
            {
                return false;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = g_backbuffers[index];
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_cmd_list->ResourceBarrier(1, &barrier);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_cmd_list->ResourceBarrier(1, &barrier);
            if (FAILED(g_cmd_list->Close()))
            {
                return false;
            }
            ID3D12CommandList* lists[] = {g_cmd_list};
            o_ExecuteCommandLists(queue, 1, lists);
            // From here the list and the allocator are IN FLIGHT, whatever else fails:
            // the caller's state moves to Submitted on this `true` and nothing Resets
            // either of them again.
            ++g_fence_value;
            const HRESULT sig = queue->Signal(g_fence, g_fence_value);
            // Stored whether or not the Signal took. A value the fence will never reach
            // cannot be waited out, so it buys one bounded wait (500 ms in the frame path,
            // 1 s in wait_for_gpu) and no more; what makes that safe is the size of this
            // command list - two barriers, microseconds - and the re-adoption the failure
            // asks for, which tears the allocator down rather than reusing it.
            frame.fence_value = g_fence_value;
            g_probe_fence = SUCCEEDED(sig) ? g_fence_value : 0;
            g_probe_waits = 0;
            mm::logf(L"probe frame: back buffer {} of {}, one PRESENT -> RENDER_TARGET -> PRESENT barrier "
                     L"pair and no draw, submitted on queue {:p} to signal fence {}. Nothing else of the "
                     L"overlay is created until the GPU has run this frame and the device still answers "
                     L"S_OK.",
                     index,
                     g_buffer_count,
                     static_cast<void*>(queue),
                     g_fence_value);
            if (FAILED(sig))
            {
                // The submission stands; only the fence does not. `g_fence_value` is left
                // raised on purpose - wait_for_gpu then waits out its 1 s bound instead
                // of concluding the GPU is idle while the probe is still running.
                mm::logf(L"probe frame: ID3D12CommandQueue::Signal returned 0x{:08X}, so nothing can say "
                         L"whether the barrier pair ran - the swapchain is adopted again rather than "
                         L"trusted",
                         static_cast<unsigned>(sig));
            }
            return true;
        }

        // The ImGui half: our SRV heap, the context, both backends and the wndproc chain.
        // Only ever reached once the probe frame has been presented.
        bool ensure_imgui(ID3D12CommandQueue* queue)
        {
            // RESTART-ONLY config key: the heap is created once, here.
            if (g_srv_heap.heap() == nullptr && !g_srv_heap.create(g_device, mm::config().srv_heap_size))
            {
                mm::log(L"CreateDescriptorHeap(SRV) failed");
                g_failed = true;
                return false;
            }

            // The last gate before ImGui exists at all: the device has been created,
            // the render targets are up and a command list has been submitted on it, so
            // if the adoption was going to remove the device it has happened by now.
            if (!device_alive(L"the ImGui context and DX12 backend creation"))
            {
                return false;
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // The panel draws its own software cursor; ImGui must not fight the game
            // over the OS cursor shape.
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            // Keyboard + gamepad navigation for the F2 panel.
            ui_init_io(io);
            ImGui::StyleColorsDark();
            ImGui::GetStyle().WindowRounding = 4.0f;

            if (!ImGui_ImplWin32_Init(g_hwnd))
            {
                mm::log(L"ImGui_ImplWin32_Init failed");
                g_failed = true;
                return false;
            }

            ImGui_ImplDX12_InitInfo info{};
            info.Device = g_device;
            info.CommandQueue = queue;
            info.NumFramesInFlight = static_cast<int>(g_buffer_count);
            info.RTVFormat = g_format;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.SrvDescriptorHeap = g_srv_heap.heap();
            info.SrvDescriptorAllocFn = &srv_alloc_cb;
            info.SrvDescriptorFreeFn = &srv_free_cb;
            if (!ImGui_ImplDX12_Init(&info))
            {
                mm::log(L"ImGui_ImplDX12_Init failed");
                ImGui_ImplWin32_Shutdown();
                g_failed = true;
                return false;
            }

            // Which devices the game reads through WM_INPUT, logged once. It decides
            // whether swallowing a key as a window message is enough: a game that
            // registers a raw KEYBOARD also needs the raw packet filtered (the panel's
            // Esc path), one that registers only the mouse does not.
            {
                UINT count = 0;
                if (::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == 0 &&
                    count > 0 && count < 64)
                {
                    std::vector<RAWINPUTDEVICE> devs(count);
                    if (::GetRegisteredRawInputDevices(devs.data(), &count, sizeof(RAWINPUTDEVICE)) !=
                        static_cast<UINT>(-1))
                    {
                        std::wstring list;
                        for (UINT i = 0; i < count && i < devs.size(); ++i)
                        {
                            list += std::format(L"{}usage {:#x}/{:#x} flags {:#x}",
                                                list.empty() ? L"" : L", ",
                                                devs[i].usUsagePage,
                                                devs[i].usUsage,
                                                devs[i].dwFlags);
                        }
                        mm::logf(L"raw input: the game has {} device(s) registered ({}). Usage 1/6 is a "
                                 L"KEYBOARD - if it is in that list, keys reach the game through WM_INPUT "
                                 L"as well as WM_KEYDOWN.",
                                 count,
                                 list);
                    }
                }
                else
                {
                    mm::log(L"raw input: the game has no raw-input devices registered - every key and "
                            L"mouse move reaches it as a window message only");
                }
            }

            hook_wndproc();

            g_imgui_frames_in_flight = static_cast<int>(g_buffer_count);
            g_imgui_rtv_format = g_format;
            g_imgui_ready = true;
            crumb::stage(crumb::kImGuiUp);
            mm::logf(L"ImGui {} initialised on the game's swapchain: device {:p}, queue {:p}, {} frames in flight, "
                     L"RTV {}",
                     std::wstring(IMGUI_VERSION, IMGUI_VERSION + std::strlen(IMGUI_VERSION)),
                     static_cast<void*>(g_device),
                     static_cast<void*>(queue),
                     g_buffer_count,
                     format_name(g_format));
            slice_selftest();
            return true;
        }

        bool ensure_initialised(IDXGISwapChain* swapchain)
        {
            if (g_failed)
            {
                return false;
            }
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            if (queue == nullptr)
            {
                return false; // no presenting queue chosen yet; try again next frame
            }
            if (g_imgui_ready)  // NOLINT: the happy path, checked every frame
            {
                return true;
            }
            // Nothing is created on a device that is not answering S_OK. The ImGui DX12
            // backend is the reason the check is here and not only at the Present that
            // reports the removal: its font-atlas upload waits on a fence with no
            // timeout, which on a dead device never completes.
            if (!device_alive(L"the overlay's first initialisation"))
            {
                return false;
            }
            if (!ensure_device_objects(swapchain, queue))
            {
                return false;
            }
            if (!g_display_logged)
            {
                // Here rather than at the capture: the adapter is reached through
                // `g_device`, which `ensure_device_objects` is what creates. Once per
                // adoption, so a resize does not reprint it.
                g_display_logged = true;
                log_display_environment(swapchain);
            }
            if (g_probe == Probe::Pending)
            {
                if (submit_probe_frame(swapchain))
                {
                    g_probe = Probe::Submitted;
                }
                return false; // this Present carries the probe and nothing else
            }
            if (g_probe == Probe::Submitted)
            {
                if (g_probe_fence == 0)
                {
                    // The Signal failed, so "the device still answers S_OK" says nothing
                    // about the barrier pair: it may never have reached the GPU. The queue
                    // is refused so the next adoption looks elsewhere instead of coming
                    // back to the one queue that has already failed to fence.
                    mark_queue_bad(queue);
                    request_readoption(L"the probe frame could not be fenced, so nothing can say whether "
                                       L"its barrier pair ran");
                    return false;
                }
                if (g_fence == nullptr || g_fence->GetCompletedValue() < g_probe_fence)
                {
                    // Still in flight. Waiting here would block the game's render thread,
                    // so the answer is simply asked again on the next Present.
                    if (++g_probe_waits >= kProbeWaitPresents)
                    {
                        mark_queue_bad(queue);
                        mm::logf(L"probe frame: its fence has not completed in {} Presents, so the queue "
                                 L"{:p} it was submitted on is not running our work - it is refused and "
                                 L"the swapchain is adopted again",
                                 kProbeWaitPresents,
                                 static_cast<void*>(queue));
                        request_readoption(L"the probe frame's fence never completed");
                    }
                    return false;
                }
                // The GPU ran the barrier pair and the device is still answering, which is
                // the one piece of evidence that the back buffer really is in PRESENT when
                // the game calls Present.
                g_probe = Probe::Passed;
                mm::log(L"probe frame: the GPU ran it and the device still answers S_OK, so the back "
                        L"buffer is in D3D12_RESOURCE_STATE_PRESENT when the game calls Present. "
                        L"Building the overlay's own objects now.");
            }
            return ensure_imgui(queue);
        }

        // Never guesses. The index decides which resource the PRESENT -> RENDER_TARGET
        // barrier is issued on, and a barrier declaring the wrong before-state is a
        // device-removal-class error (and, with the wrong RTV, a frame drawn into the
        // buffer being scanned out). IDXGISwapChain3 is QI'd once at adoption and
        // cached; without it the caller skips the frame.
        UINT current_backbuffer_index(IDXGISwapChain* swapchain)
        {
            if (g_sc3 == nullptr)
            {
                if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&g_sc3))) || g_sc3 == nullptr)
                {
                    g_sc3 = nullptr;
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        mm::log(L"this swapchain does not expose IDXGISwapChain3, so the back-buffer "
                                L"index cannot be known - the overlay will not draw on it (guessing the "
                                L"index would put a resource barrier on the wrong buffer)");
                    }
                    return kNoBackbuffer;
                }
            }
            const UINT index = g_sc3->GetCurrentBackBufferIndex();
            return index < g_buffer_count ? index : kNoBackbuffer;
        }

        // A D3D12 swapchain hands out ID3D12Resource back buffers; a D3D11 one, or a
        // wrapper that does not forward, does not. That is the cheapest reliable test.
        bool is_d3d12_swapchain(IDXGISwapChain* sc)
        {
            ID3D12Resource* buffer = nullptr;
            const HRESULT hr = sc->GetBuffer(0, IID_PPV_ARGS(&buffer));
            safe_release(buffer);
            return SUCCEEDED(hr);
        }

        void log_candidate(IDXGISwapChain* sc, bool d3d12)
        {
            // One line per distinct swapchain, not per Present: the game's decoy 144x8
            // D3D11 swapchain presents every frame.
            static IDXGISwapChain* seen[8]{};
            for (IDXGISwapChain* s : seen)
            {
                if (s == sc)
                {
                    return;
                }
            }
            if (g_candidates_logged >= static_cast<int>(std::size(seen)))
            {
                return;
            }
            seen[g_candidates_logged] = sc;
            ++g_candidates_logged;
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            void** vtable = *reinterpret_cast<void***>(sc);
            mm::logf(L"Present candidate {:p}: {}x{} {} x{} buffers, hwnd 0x{:X}, vtable[8] {} -> {}",
                     static_cast<void*>(sc),
                     desc.BufferDesc.Width,
                     desc.BufferDesc.Height,
                     format_name(desc.BufferDesc.Format),
                     desc.BufferCount,
                     reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                     module_of(vtable[8]),
                     d3d12 ? L"D3D12, taking it" : L"not D3D12, ignored");
        }

        //==============================================================================
        // Render-side teardown for the master switch (RENDER THREAD ONLY)
        //==============================================================================
        //
        // Everything below is a D3D12 object or an ImGui context, and both may only be
        // touched from the thread that created them - whichever thread calls Present. So
        // the loop thread clears mm::g_mod_active and this runs inside the next Present,
        // before the hooks are taken out. It leaves the module in the state it had
        // before the first frame: `start()` re-enables the hooks, ExecuteCommandLists
        // re-captures the queue and ensure_initialised() builds everything again.
        //
        // Two callers, one body. The master switch (shutdown_render) and a lost device /
        // replaced swapchain (the re-adoption path in render()) release the same
        // objects; only the master switch also answers the loop thread's handshake with
        // `g_render_stopped`, which a re-adoption must not touch or the loop thread
        // believes a disable it never asked for has completed.
        //
        // The invariant is the LOCK, not a thread id: whoever holds `g_render_lock` may
        // release these objects. Present arrives on whichever thread the game or a
        // frame-generation proxy presents from, so the thread that built them and the one
        // that releases them need not be the same - only serialised.

        void release_device_objects(bool on_render_thread)
        {
            if (g_imgui_ready || g_device != nullptr)
            {
                crumb::stage(crumb::kTeardownBegin);
                // The slicer writes into mapped upload heaps about to be released. On a
                // timeout the buffers are leaked rather than freed under a live writer.
                const bool paused = slicer_pause_begin(1000);
                wait_for_gpu();
                if (!paused)
                {
                    mm::log(L"slice: the loop-thread slicer did not stand down in 1 s - "
                            L"the slice buffers are left allocated on purpose");
                }
                else
                {
                    destroy_all_map_textures();
                }
                slicer_pause_end();
                if (g_imgui_ready)
                {
                    unhook_wndproc();
                    ImGui_ImplDX12_Shutdown();
                    ImGui_ImplWin32_Shutdown();
                    ImGui::DestroyContext();
                    g_imgui_ready = false;
                    // The style went with the context: the next init must rebuild it.
                    g_ui_scale_applied = 0.0f;
                }
                release_render_targets();
                for (UINT i = 0; i < kMaxBuffers; ++i)
                {
                    safe_release(g_frames[i].allocator);
                    g_frames[i].fence_value = 0;
                }
                safe_release(g_cmd_list);
                safe_release(g_fence);
                if (g_fence_event != nullptr)
                {
                    ::CloseHandle(g_fence_event);
                    g_fence_event = nullptr;
                }
                g_fence_value = 0;
                g_srv_heap.destroy();
                safe_release(g_device);
                g_imgui_frames_in_flight = 0;
                g_imgui_rtv_format = DXGI_FORMAT_UNKNOWN;
                mm::log(L"the render thread has released ImGui, the descriptor heaps, "
                        L"the slice buffers and the map textures");
            }
            g_swapchain = nullptr;
            // Nothing is adopted, so there is no silence to measure until the next
            // Present on a newly adopted swapchain.
            g_adopted_present_ms = 0;
            // The cached IDXGISwapChain3 holds a reference on the swapchain being let
            // go; keeping it would pin a dead object and answer
            // GetCurrentBackBufferIndex for a swapchain we no longer draw on.
            safe_release(g_sc3);
            g_candidates_logged = 0;
            // The queue was captured with a reference held (see adopt_presenting_queue),
            // so dropping it means releasing it, and the scoring starts over.
            safe_release_queue();
            reset_queue_adoption(on_render_thread);
            // The next adoption gets its own probe frame: a new swapchain can be behind a
            // different proxy than the one that was just let go.
            g_probe = Probe::Pending;
            g_probe_fence = 0;
            g_probe_waits = 0;
            g_failed = false;
            // Everything a re-adoption would have released is gone already.
            g_readopt.store(false, std::memory_order_release);
            crumb::stage(crumb::kTeardownEnd);
        }

        void shutdown_render(bool on_render_thread)
        {
            release_device_objects(on_render_thread);
            // The master switch's handshake, and the one thing a re-adoption must skip.
            g_render_stopped.store(true, std::memory_order_release);
        }

        void render(IDXGISwapChain* swapchain)
        {
            // The master switch, first statement: one relaxed atomic load per Present
            // while the mod is off, and the first Present to see it off does the
            // teardown, because this is the only thread allowed to.
            if (!mm::mod_active())
            {
                if (!g_render_stopped.load(std::memory_order_acquire))
                {
                    spin::SpinGuard guard(g_render_lock);
                    shutdown_render(true);
                }
                return;
            }

            g_present_count.fetch_add(1, std::memory_order_relaxed);
            g_render_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
            g_render_stage.store("prologue", std::memory_order_relaxed);
            // TERMINAL. The adopted device is gone: release what is left of ours once,
            // on this thread because it is the only one allowed to, and then return from
            // every later Present before touching anything at all.
            if (g_device_removed.load(std::memory_order_acquire))
            {
                if (!g_removal_released.exchange(true, std::memory_order_acq_rel))
                {
                    g_render_stage.store("releasing after a device removal", std::memory_order_relaxed);
                    spin::SpinGuard guard(g_render_lock);
                    release_device_objects(true);
                }
                g_render_stage.store("off: the adopted device was removed", std::memory_order_relaxed);
                return;
            }
            g_render_stage.store("waiting for the render lock", std::memory_order_relaxed);
            spin::SpinGuard guard(g_render_lock);
            g_render_stage.store("holding the render lock", std::memory_order_relaxed);

            // A Present of the adopted swapchain failed on some thread. Asking the device
            // why is this thread's job, and it decides between the terminal state and the
            // re-adoption immediately below. It runs BEFORE the `g_failed` return: the
            // reason a device was removed is the single most useful line in a bug report,
            // and an unrelated earlier failure must not be what swallows it.
            handle_present_failure();
            // A start after a stop that never reached a Present: whatever survived it is
            // released before anything is built on it.
            if (g_verify_on_start.exchange(false, std::memory_order_acq_rel) && g_device != nullptr)
            {
                request_readoption(L"the overlay is starting again while the objects of the device it "
                                   L"had adopted are still allocated");
            }
            if (g_device_removed.load(std::memory_order_acquire))
            {
                return; // the next Present does the one release
            }
            if (g_failed)
            {
                return;
            }

            // RE-ADOPTION, here because this is the only thread that may touch a D3D12
            // object. Whatever asked for it (a removed device, a swapchain that stopped
            // handing out buffers, a queue from the wrong device) left this module
            // holding objects of something that no longer exists; releasing them and
            // starting over is what brings the overlay back after a driver reset or a
            // swapchain swap. `g_failed` is not set - this path is recoverable.
            if (g_readopt.exchange(false, std::memory_order_acquire))
            {
                g_render_stage.store("re-adopting the swapchain", std::memory_order_relaxed);
                mm::perf_note_stall(L"a swapchain / device re-adoption", 2000);
                release_device_objects(true);
                return; // the next Present adopts whatever is there now
            }

            if (g_swapchain == nullptr)
            {
                const bool d3d12 = is_d3d12_swapchain(swapchain);
                log_candidate(swapchain, d3d12);
                if (!d3d12)
                {
                    return;
                }
                g_swapchain = swapchain;
                crumb::stage(crumb::kSwapchainChosen);
            }
            else if (swapchain != g_swapchain)
            {
                // Another swapchain - frame generation, ReShade, the game's decoy D3D11
                // one - and normally not ours. It IS ours when the game has recreated
                // its swapchain: the adopted one then presents nothing while this one
                // presents every frame. Both tests have to hold before that is believed
                // - our swapchain silent for kSwapchainSilentMs AND this one a D3D12
                // swapchain - because the decoy presents right through a level load,
                // when the game's own swapchain is legitimately quiet.
                const std::uint64_t now = ::GetTickCount64();
                if (g_adopted_present_ms != 0 && now - g_adopted_present_ms > kSwapchainSilentMs &&
                    is_d3d12_swapchain(swapchain))
                {
                    // The ordinary re-adoption: release everything on this thread and
                    // let the next Present adopt whatever is presenting now.
                    g_adopted_present_ms = 0; // one attempt per adopted swapchain
                    request_readoption(L"the adopted swapchain has not presented for 2 s while another "
                                       L"D3D12 swapchain has - the game has replaced it");
                }
                return;
            }

            // This Present is on the adopted swapchain: the reference point the check
            // above measures silence against.
            g_adopted_present_ms = ::GetTickCount64();

            // THE PRESENTING QUEUE, decided here because this Present is what closes a
            // window and scores its winner.
            if (g_queue.load(std::memory_order_acquire) == nullptr)
            {
                g_render_stage.store("choosing the presenting queue", std::memory_order_relaxed);
                if (!adopt_presenting_queue(swapchain))
                {
                    return; // nothing to submit on yet
                }
            }

            if (!ensure_initialised(swapchain))
            {
                return;
            }
            if (!g_rt_ready && !create_render_targets(swapchain))
            {
                // A GetBuffer / RTV failure is recoverable (a resize in flight, a device
                // that has gone), so it goes to the re-adoption path instead of retrying
                // the same objects every frame.
                request_readoption(L"the render targets could not be rebuilt");
                return;
            }
            // After a resize that raised BufferCount the ImGui backend still has one set
            // of per-frame buffers per OLD frame in flight and would reuse the vertex,
            // index and descriptor storage of a frame the GPU has not finished.
            // Re-initialising the DX12 backend is the only way to change that count; it
            // happens outside a frame (before NewFrame) and with the GPU idle. A changed
            // back-buffer FORMAT (an HDR toggle) needs the same rebuild: the backend's
            // pipeline state bakes the RTV format in, and drawing it against a render
            // target of another format is a debug-layer error and a wrong-looking frame.
            if (g_imgui_ready && g_imgui_frames_in_flight != 0 &&
                (g_imgui_frames_in_flight < static_cast<int>(g_buffer_count) ||
                 g_imgui_rtv_format != g_format))
            {
                mm::logf(L"the swapchain now has {} buffer(s) in {}, ImGui was initialised for {} frames "
                         L"in flight in {} - re-initialising the DX12 backend",
                         g_buffer_count,
                         format_name(g_format),
                         g_imgui_frames_in_flight,
                         format_name(g_imgui_rtv_format));
                // ImGui_ImplDX12_Init uploads the font atlas behind a fence wait with no
                // timeout, so it may only ever run on a device that answers S_OK.
                if (!device_alive(L"the ImGui DX12 backend re-init after a resize"))
                {
                    return;
                }
                wait_for_gpu();
                ImGui_ImplDX12_Shutdown();
                ImGui_ImplDX12_InitInfo info{};
                info.Device = g_device;
                info.CommandQueue = g_queue.load(std::memory_order_acquire);
                info.NumFramesInFlight = static_cast<int>(g_buffer_count);
                info.RTVFormat = g_format;
                info.DSVFormat = DXGI_FORMAT_UNKNOWN;
                info.SrvDescriptorHeap = g_srv_heap.heap();
                info.SrvDescriptorAllocFn = &srv_alloc_cb;
                info.SrvDescriptorFreeFn = &srv_free_cb;
                if (!ImGui_ImplDX12_Init(&info))
                {
                    mm::log(L"ImGui_ImplDX12_Init failed on the re-init after a resize - overlay off");
                    g_failed = true;
                    return;
                }
                g_imgui_frames_in_flight = static_cast<int>(g_buffer_count);
                g_imgui_rtv_format = g_format;
            }

            const UINT index = current_backbuffer_index(swapchain);
            if (index == kNoBackbuffer)
            {
                // No index, no frame. Drawing without knowing which buffer is next means
                // barriering the wrong resource - see current_backbuffer_index.
                return;
            }
            FrameCtx& frame = g_frames[index];
            if (frame.allocator == nullptr)
            {
                // A cheap guard: a missing allocator drops the frame instead of faulting
                // in Reset().
                request_readoption(L"a back buffer has no command allocator");
                return;
            }
            g_render_stage.store("waiting for this frame's fence", std::memory_order_relaxed);
            if (frame.fence_value != 0 && g_fence->GetCompletedValue() < frame.fence_value)
            {
                if (SUCCEEDED(g_fence->SetEventOnCompletion(frame.fence_value, g_fence_event)))
                {
                    ::WaitForSingleObject(g_fence_event, 500);
                }
            }

            // A reload (F5) rebuilds the whole texture set. The old textures may only be
            // released here, on the render thread and BEFORE the frame's draw lists are
            // built, or the frame references an SRV slot just handed back.
            if (g_drop_textures.load(std::memory_order_acquire))
            {
                if (slicer_pause_begin(kSlicerPauseMs))
                {
                    g_drop_textures.store(false, std::memory_order_release);
                    // A full GPU flush plus ~340 MB of releases: a one-off that must not
                    // become the peak every later frame is judged against.
                    mm::perf_note_stall(L"a map texture reload (F5)", 2000);
                    wait_for_gpu();
                    destroy_all_map_textures();
                    slicer_pause_end();
                    mm::log(L"map textures and slice buffers dropped for a reload");
                }
                // else: the request stays pending and the next frame retries. Never
                // release a resource the loop thread may still be writing into.
            }
            release_finished_uploads();

            if (g_pf_frame < 0)
            {
                g_pf_frame = mm::perf_register("render frame (ImGui)", perf::Thread::Render);
            }
            const std::uint64_t frame_t0 = mm::qpc_us();
            // The UI scale, decided from the current back buffer and applied before the
            // frame's draw lists exist. ResizeBuffers changes g_height and a config
            // change comes through cfg_cached, so both re-enter here on their own.
            apply_ui_scale(wanted_ui_scale(mm::cfg_cached(), static_cast<float>(g_height)));
            // Two sub-counters, because the two halves fail differently:
            // ImGui_ImplWin32_NewFrame reads and writes the cursor and the client rect
            // of a window owned by the GAME thread, and that cross-thread user32 call
            // blocks until that thread pumps messages - which it does not do inside a
            // synchronous level load. build_ui() is our own drawing and touches no OS
            // handle.
            if (g_pf_newframe < 0)
            {
                g_pf_newframe = mm::perf_register("render NewFrame (win32)", perf::Thread::Render);
                g_pf_buildui = mm::perf_register("render build_ui", perf::Thread::Render);
            }
            {
                const mm::PerfScope nf(g_pf_newframe);
                // The game thread's window messages, in order, on the one thread that
                // is allowed to touch the ImGui context.
                g_render_stage.store("imgui: replaying window messages", std::memory_order_relaxed);
                replay_imgui_messages();
                // Cross-thread user32 with the render lock held: everything in here
                // reads or writes the cursor and the client rect of a window owned by
                // the GAME thread, so it is the one place in the frame that can wait on
                // another thread. That is why `hk_ResizeBuffers` - the only other taker
                // of that lock, and a call that can arrive on the game thread - acquires
                // it with a bound instead of spinning for ever.
                g_render_stage.store("imgui: ImplWin32_NewFrame (user32)", std::memory_order_relaxed);
                ImGui_ImplWin32_NewFrame();
            }
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            {
                const mm::PerfScope bu(g_pf_buildui);
                g_render_stage.store("build_ui", std::memory_order_relaxed);
                build_ui();
            }
            ImGui::Render();
            // The game thread's swallow decision reads this instead of the context.
            g_imgui_want_keyboard.store(ImGui::GetIO().WantCaptureKeyboard, std::memory_order_relaxed);
            // tgate::text_active() rather than io.WantTextInput: that flag is a frame
            // behind the caret, and the frame a click into a text box lands in is
            // exactly the frame the loop thread is reading while the first letter of
            // the word goes down.
            g_imgui_want_text.store(tgate::text_active(), std::memory_order_relaxed);
            mm::perf_record(g_pf_frame, frame_t0);

            if (FAILED(frame.allocator->Reset()) || FAILED(g_cmd_list->Reset(frame.allocator, nullptr)))
            {
                return;
            }

            // One image per frame: a nine-layer chapter is resident after ~9 frames
            // instead of stalling a single one with ~100 MB of copies.
            std::unique_ptr<mapdata::PendingImage> pending = mapdata::take_pending();
            if (pending != nullptr)
            {
                begin_map_upload(*pending, g_cmd_list);
            }

            // The height-slice window the CPU filled during build_ui(), recorded BEFORE
            // ImGui's draw call in the same command list, so the GPU sees the copy
            // complete before it samples the texture - no extra queue, no second
            // submission, no PSO of our own.
            record_slice_copy(g_cmd_list);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = g_backbuffers[index];
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_cmd_list->ResourceBarrier(1, &barrier);

            g_cmd_list->OMSetRenderTargets(1, &g_rtv[index], FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = {g_srv_heap.heap()};
            g_cmd_list->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list);

            // The screenshot copy, if one was asked for: it takes the back buffer from
            // RENDER_TARGET to PRESENT itself (via COPY_SOURCE), so it replaces the
            // closing barrier below rather than adding to it.
            const bool shot_recorded = record_shot_copy(g_cmd_list, g_backbuffers[index], index);
            if (!shot_recorded)
            {
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                g_cmd_list->ResourceBarrier(1, &barrier);
            }
            g_cmd_list->Close();

            g_render_stage.store("submitting the command list", std::memory_order_relaxed);
            ID3D12CommandQueue* queue = g_queue.load(std::memory_order_acquire);
            ID3D12CommandList* lists[] = {g_cmd_list};
            // The original, so our own submission does not re-enter the hook.
            // ExecuteCommandLists returns void: the only report a bad submission gives is
            // the fence Signal that follows it and the device's own removal reason, both
            // checked below.
            o_ExecuteCommandLists(queue, 1, lists);
            ++g_fence_value;
            const HRESULT sig = queue->Signal(g_fence, g_fence_value);
            if (FAILED(sig))
            {
                static bool said = false;
                if (!said)
                {
                    said = true;
                    mm::logf(L"ID3D12CommandQueue::Signal failed (0x{:08X}) on queue {:p} right after the "
                             L"overlay's command list was submitted - the queue or its device is gone",
                             static_cast<unsigned>(sig),
                             static_cast<void*>(queue));
                }
            }
            frame.fence_value = g_fence_value;
            if (shot_recorded)
            {
                g_shot_fence = g_fence_value; // the readback may not be mapped before this
            }
            // The buffer this frame sampled may not be rewritten until the GPU is past
            // this fence.
            {
                const int shown = slice_view().shown;
                if (shown >= 0)
                {
                    g_slice_in_flight[shown].store(g_fence_value, std::memory_order_release);
                }
            }
            {
                const int shown = map_slice_view().shown;
                if (shown >= 0)
                {
                    g_mslice_in_flight[shown].store(g_fence_value, std::memory_order_release);
                }
            }

            if (g_map.upload != nullptr && g_map.upload_fence == 0)
            {
                g_map.upload_fence = g_fence_value;
            }
            else if (g_map.upload != nullptr && g_fence->GetCompletedValue() >= g_map.upload_fence)
            {
                safe_release(g_map.upload); // the copy has landed; give the 90 MB back
            }
            g_render_stage.store("between frames", std::memory_order_relaxed);
            // A removal our own submission caused does not have to wait for a Present to
            // report it: the device answers directly, and this is the shortest path from
            // cause to log line.
            (void)device_alive(L"the end of a frame the overlay submitted");
        }

        //==============================================================================
        // Hooks
        //==============================================================================

        // THE EXCEPTION BARRIER.
        //
        // Anything in the frame can throw std::bad_alloc, and a throw would unwind
        // through the MinHook trampoline into DXGI - frames compiled with no idea this
        // code exists, i.e. a crash inside the driver stack whose call stack names
        // dxgi.dll. So the whole frame is wrapped once, at the hook boundary: a throw
        // becomes one log line and a dead overlay, and the game keeps presenting.
        //
        // Not SEH: an access violation is left to crash. `__try` cannot live in a
        // function needing C++ unwinding (MSVC C2712), and swallowing an AV mid
        // command-list recording leaves the list open and the back buffer stranded
        // between resource states, which later frames turn into a device removal with no
        // evidence. The crash breadcrumb plus CrashContext.runtime-xml is the diagnostic
        // route here.
        void render_guarded(IDXGISwapChain* sc)
        {
            try
            {
                render(sc);
            }
            catch (const std::exception& e)
            {
                if (!g_failed.exchange(true))
                {
                    const char* what = e.what() != nullptr ? e.what() : "?";
                    mm::logf(L"an exception escaped the overlay's frame ({}) - the overlay is off for "
                             L"the rest of this session; the game is unaffected",
                             stage_w(what));
                }
            }
            catch (...)
            {
                if (!g_failed.exchange(true))
                {
                    mm::log(L"a non-standard exception escaped the overlay's frame - the overlay is off "
                            L"for the rest of this session; the game is unaffected");
                }
            }
        }

        // The screenshot readback: mapped, unpacked and turned into a DIB OUTSIDE the
        // render lock, because a full-canvas unpack is two allocations plus a per-row
        // conversion of up to ~8 MB and `hk_ResizeBuffers` waits on that same lock from
        // the GAME thread. Still the render thread, which is what owns the readback
        // resource and the fence.
        void collect_guarded()
        {
            try
            {
                shot_collect();
            }
            catch (...)
            {
                mm::log(L"the screenshot readback threw - the map copy is dropped");
            }
        }

        // Did the Present itself fail? A removal is reported for EVERY swapchain in the
        // process, because the swapchain that reports it first need not be the one we
        // adopted and the reason code is what a bug report is read for. Only a removal on
        // our own device is terminal; anything else is one log line.
        //
        // ANY THREAD - frame generation presents from its own. So nothing here touches
        // `g_device`: the pointer is printed, never dereferenced, and asking it for its
        // removal reason is left to the render thread, which is the only one that may be
        // holding it (see g_present_failed and the check at the top of render()).
        void note_present_result(HRESULT hr, IDXGISwapChain* sc)
        {
            if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET)
            {
                return;
            }
            const bool ours = sc == g_swapchain;
            // Once per removal. It repeats on every Present of every swapchain from here
            // on, and the first line is the one worth having.
            if (!g_removal_logged.exchange(true, std::memory_order_acq_rel))
            {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (sc != nullptr)
                {
                    sc->GetDesc(&desc);
                }
                mm::logf(L"DEVICE REMOVED: Present returned 0x{:08X} on {} swapchain {:p} ({}x{} {} "
                         L"x{} buffers, flags 0x{:X}, swap effect {}, hwnd 0x{:X}); our device {:p}, "
                         L"our queue {:p}, present count {}. GetDeviceRemovedReason() is asked and "
                         L"logged by the render thread, which is the only thread allowed to touch the "
                         L"device.",
                         static_cast<unsigned>(hr),
                         ours ? L"OUR adopted" : L"another",
                         static_cast<void*>(sc),
                         desc.BufferDesc.Width,
                         desc.BufferDesc.Height,
                         format_name(desc.BufferDesc.Format),
                         desc.BufferCount,
                         static_cast<unsigned>(desc.Flags),
                         static_cast<int>(desc.SwapEffect),
                         reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                         static_cast<void*>(g_device),
                         static_cast<void*>(g_queue.load(std::memory_order_acquire)),
                         g_present_count.load(std::memory_order_relaxed));
            }
            if (!ours)
            {
                return; // another renderer's device: reported, never acted on
            }
            // The render thread decides: it asks the device for its reason and either
            // engages the terminal state or asks for a re-adoption.
            g_present_failed.store(true, std::memory_order_release);
        }

        // RENDER THREAD, from render(). The other half of note_present_result: a Present
        // of the adopted swapchain failed with DEVICE_REMOVED / DEVICE_RESET and the
        // device has to be asked what happened, on the one thread that may ask it.
        void handle_present_failure()
        {
            if (!g_present_failed.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            // device_alive() logs the reason and engages the terminal state when the
            // device has stopped answering S_OK.
            if (!device_alive(L"a Present of the adopted swapchain that returned DEVICE_REMOVED"))
            {
                return;
            }
            // Nothing adopted yet, or a device that still answers S_OK: the recoverable
            // recoverable path - release and adopt again on a later Present.
            request_readoption(L"a Present of the adopted swapchain returned DEVICE_REMOVED or "
                               L"DEVICE_RESET while the device still answers S_OK");
        }

        // The game's own Present arguments, kept for `log_display_environment`. Recorded
        // for the adopted swapchain only, so a decoy or a frame-generation chain cannot
        // put its own pacing in the log.
        void note_present_args(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            if (sc == g_swapchain)
            {
                g_present_sync.store(sync, std::memory_order_relaxed);
                g_present_flags.store(flags, std::memory_order_relaxed);
            }
        }

        HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            note_present_args(sc, sync, flags);
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            // The original is always called, for every swapchain in the process: Present
            // is one dxgi function shared by every swapchain, D3D11 and D3D12 alike, and
            // returning early for "not ours" would stop another overlay presenting.
            const HRESULT hr = o_Present(sc, sync, flags);
            if (sc == g_swapchain || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                note_present_result(hr, sc);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
        {
            note_present_args(sc, sync, flags);
            render_guarded(sc);
            if (sc == g_swapchain)
            {
                collect_guarded();
            }
            const HRESULT hr = o_Present1(sc, sync, flags, params);
            if (sc == g_swapchain || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                note_present_result(hr, sc);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                                   UINT flags)
        {
            if (!mm::mod_active())
            {
                return o_ResizeBuffers(sc, count, w, h, format, flags);
            }
            g_resize_count.fetch_add(1, std::memory_order_relaxed);
            // A resize means a device-level stall (a resolution or fullscreen change, or
            // the tail of a level load): the frames around it are wall-clock waits, not
            // this mod's cost, so they go to the stall columns of the F2 table.
            mm::perf_note_stall(L"a swapchain resize", 2000);
            // A bounded acquire, not a spin: this call can arrive on the game thread and
            // the render thread holds the same lock across `ImGui_ImplWin32_NewFrame()`,
            // which waits on that very thread. Spinning for ever is a two-thread
            // deadlock; the bound turns the worst case into a named log line and a
            // resize that may fail, which the next Present recovers from by recreating
            // its render targets.
            if (g_render_lock.try_lock_ms(2000))
            {
                // Ours or not, tested before the line is formatted: every swapchain in
                // the process comes through this function, the decoy 144x8 D3D11 one
                // included, and formatting for each would put a wstring allocation and a
                // log write in front of resizes that have nothing to do with this mod.
                if (sc == g_swapchain)
                {
                    mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}) - releasing render targets",
                             count,
                             w,
                             h,
                             format_name(format));
                    // A throw here would unwind into DXGI through the trampoline - the
                    // same hazard the Present barrier exists for, and on the GAME
                    // thread, where it takes the game down with it.
                    try
                    {
                        // The fence, not ImGui: the probe frame is submitted while
                        // `g_imgui_ready` is still false, and releasing the back buffers
                        // out from under it is exactly the case this wait exists for.
                        if (g_fence != nullptr)
                        {
                            wait_for_gpu();
                        }
                        release_render_targets();
                    }
                    catch (...)
                    {
                        mm::log(L"an exception escaped the ResizeBuffers path - the overlay is off for "
                                L"the rest of this session; the resize itself still happens");
                        g_failed = true;
                    }
                }
                g_render_lock.unlock();
            }
            else if (sc == g_swapchain)
            {
                mm::logf(L"ResizeBuffers({} buffers, {}x{}, {}): the render lock was still held after "
                         L"2000 ms, so our render targets were NOT released. The resize may fail and "
                         L"the next Present will rebuild them. Render thread {} was at '{}'.",
                         count,
                         w,
                         h,
                         format_name(format),
                         g_render_tid.load(std::memory_order_relaxed),
                         stage_w(g_render_stage.load(std::memory_order_relaxed)));
            }
            const HRESULT hr = o_ResizeBuffers(sc, count, w, h, format, flags);
            // The RTVs are recreated lazily on the next Present, once the swapchain has
            // its new buffers.
            if (FAILED(hr))
            {
                mm::logf(L"ResizeBuffers failed (0x{:08X})", static_cast<unsigned>(hr));
            }
            return hr;
        }

        void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                      ID3D12CommandList* const* lists)
        {
            // This hook only REMEMBERS, and only while the adoption is open: once the
            // queue is captured `note_direct_queue` returns on its first statement, so
            // what is left in the game's submission path is a relaxed load. Which of the
            // process's DIRECT queues presents the game's swapchain is decided on the
            // render thread, at the Present that closes a window and scores it - see
            // adopt_presenting_queue().
            if (queue != nullptr && mm::mod_active() && !g_device_removed.load(std::memory_order_relaxed))
            {
                note_direct_queue(queue);
            }
            o_ExecuteCommandLists(queue, count, lists);
        }

        //==============================================================================
        // Hook installation via a throwaway device + swapchain, on every launch
        //==============================================================================
        //
        // Discovery creates a throwaway D3D12 device, a DIRECT command queue and a 64x64
        // swapchain on a hidden window through our own import table, reads four vtable
        // slots and destroys all three. The addresses it finds are a property of the
        // DLLs, so they could be written down and reused - and reusing them is what makes
        // such a launch a different launch. On a machine stacking ReShade's dxgi proxy,
        // an addon, Streamline's interposer and the Steam overlay, these creation calls
        // are what drives every one of those layers through its own interposer before
        // MinHook writes a byte; hooking the addresses straight out of a file skips that
        // and intermittently costs the frame - a black screen from launch, with this mod
        // presenting happily and nothing in any log. Discovery costs ~60 ms once per
        // launch, which is not worth a file to skip.
        //
        // The price is that Steam's GameOverlayRenderer64 hooks the same creation entry
        // points and re-targets its overlay onto what it sees created, so its FPS counter
        // can end up pointing at objects we destroyed again.

        // wuchang_minimap_hookaddr.txt is where older builds cached the addresses.
        void remove_stale_hook_cache()
        {
            const std::wstring path = mm::mod_dir() + L"\\wuchang_minimap_hookaddr.txt";
            if (::DeleteFileW(path.c_str()) != 0)
            {
                mm::logf(L"deleted {} - an older build's address cache. The addresses are discovered "
                         L"fresh on every launch.",
                         path);
            }
        }

        // Four MH_CreateHook calls, one MH_EnableHook, one report.
        bool create_and_enable(void** addr, const wchar_t* how)
        {
            const MH_STATUS s1 = MH_CreateHook(addr[0], reinterpret_cast<void*>(&hk_Present),
                                               reinterpret_cast<void**>(&o_Present));
            const MH_STATUS s2 = MH_CreateHook(addr[1], reinterpret_cast<void*>(&hk_ResizeBuffers),
                                               reinterpret_cast<void**>(&o_ResizeBuffers));
            const MH_STATUS s3 = MH_CreateHook(addr[2], reinterpret_cast<void*>(&hk_Present1),
                                               reinterpret_cast<void**>(&o_Present1));
            const MH_STATUS s4 = MH_CreateHook(addr[3], reinterpret_cast<void*>(&hk_ExecuteCommandLists),
                                               reinterpret_cast<void**>(&o_ExecuteCommandLists));

            // The statuses are checked before anything is enabled: either both required
            // hooks exist or nothing of ours is installed at all. A partial install
            // would leave live trampolines behind while reporting failure, and the
            // master switch's re-enable would then run discovery on top of them.
            const MH_STATUS created[kHookCount] = {s1, s2, s3, s4};
            const bool required_ok = s1 == MH_OK && s4 == MH_OK;
            MH_STATUS en = MH_ERROR_NOT_CREATED;
            if (required_ok)
            {
                en = MH_EnableHook(MH_ALL_HOOKS);
            }
            if (!required_ok || en != MH_OK)
            {
                for (int i = 0; i < kHookCount; ++i)
                {
                    if (created[i] == MH_OK)
                    {
                        MH_DisableHook(addr[i]);
                        MH_RemoveHook(addr[i]);
                    }
                }
                mm::log(L"hooks: the required pair (Present, ExecuteCommandLists) did not install, so "
                        L"every trampoline that HAD been created was removed again - nothing of this "
                        L"mod is in the game's call path");
            }

            g_hook_report = std::format(L"{} | Present {} @ {} | ResizeBuffers {} @ {} | Present1 {} @ {} | "
                                        L"ExecuteCommandLists {} @ {} | enable {}",
                                        how,
                                        static_cast<int>(s1),
                                        module_of(addr[0]),
                                        static_cast<int>(s2),
                                        module_of(addr[1]),
                                        static_cast<int>(s3),
                                        module_of(addr[2]),
                                        static_cast<int>(s4),
                                        module_of(addr[3]),
                                        static_cast<int>(en));
            mm::logf(L"hooks: {}", g_hook_report);
            mm::logf(L"hook addresses: Present {:p}  ResizeBuffers {:p}  Present1 {:p}  ExecuteCommandLists {:p}",
                     addr[0],
                     addr[1],
                     addr[2],
                     addr[3]);
            log_overlay_modules();
            const bool ok = required_ok && en == MH_OK;
            if (!ok)
            {
                mm::log(L"at least one required hook did not install - the overlay will not draw");
            }
            return ok;
        }

        bool install_hooks()
        {
            remove_stale_hook_cache();
            const std::uint64_t began = ::GetTickCount64();
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                mm::logf(L"MH_Initialize failed: {}",
                         std::wstring(MH_StatusToString(init), MH_StatusToString(init) + std::strlen(MH_StatusToString(init))));
                return false;
            }

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WuchangMinimapDummyWnd";
            ::RegisterClassExW(&wc);
            HWND dummy_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"WuchangMinimap", WS_OVERLAPPEDWINDOW, 0, 0, 64,
                                                64, nullptr, nullptr, wc.hInstance, nullptr);
            if (dummy_hwnd == nullptr)
            {
                mm::log(L"could not create the dummy window for vtable discovery");
                ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
                return false;
            }

            ID3D12Device* device = nullptr;
            ID3D12CommandQueue* queue = nullptr;
            IDXGIFactory2* factory = nullptr;
            IDXGISwapChain1* swapchain = nullptr;
            bool ok = false;

            HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            if (SUCCEEDED(hr))
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
            }
            if (SUCCEEDED(hr))
            {
                hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            }
            if (SUCCEEDED(hr))
            {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = 64;
                sd.Height = 64;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.Scaling = DXGI_SCALING_STRETCH;
                sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                hr = factory->CreateSwapChainForHwnd(queue, dummy_hwnd, &sd, nullptr, nullptr, &swapchain);
            }

            if (SUCCEEDED(hr) && swapchain != nullptr && queue != nullptr)
            {
                void** sc_vtable = *reinterpret_cast<void***>(swapchain);
                void** q_vtable = *reinterpret_cast<void***>(queue);

                void* addr[kHookCount] = {
                    sc_vtable[8],  // IDXGISwapChain::Present
                    sc_vtable[13], // IDXGISwapChain::ResizeBuffers
                    sc_vtable[22], // IDXGISwapChain1::Present1
                    q_vtable[10],  // ID3D12CommandQueue::ExecuteCommandLists
                };

                // Who was already there, read before a byte is written: a jmp in front of
                // Present names the module that installed it, and MinHook relocates
                // those bytes into our trampoline, so that overlay stays in the chain.
                for (int i = 0; i < kHookCount; ++i)
                {
                    mm::logf(L"hook discovery: {} -> {} {}",
                             kHookNames[i],
                             module_of(addr[i]),
                             detour_report(addr[i]));
                }

                ok = create_and_enable(addr, L"by dummy-swapchain discovery");
            }
            else
            {
                mm::logf(L"dummy device/swapchain creation failed (0x{:08X}) - no hooks installed",
                         static_cast<unsigned>(hr));
            }

            safe_release(swapchain);
            safe_release(factory);
            safe_release(queue);
            safe_release(device);
            ::DestroyWindow(dummy_hwnd);
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

            g_hooks_installed = ok;
            g_hook_install_ms = ::GetTickCount64();
            // What the throwaway objects cost, and the window in which Steam's overlay
            // could have re-targeted itself onto them.
            mm::logf(L"hook discovery: {} ms from the dummy window to the four hooks; the dummy device, "
                     L"queue, swapchain and window are destroyed again",
                     g_hook_install_ms - began);
            return ok;
        }

    } // namespace ovl
} // namespace overlay
