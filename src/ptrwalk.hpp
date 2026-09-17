// ptrwalk - a bounded breadth-first walk over a pointer graph. PURE: no Windows, no
// UE4SS, no D3D12, no allocation, no logging. Header-only, so the offline tests link it.
//
// The walk exists because a chain of C++ objects can be crossed without knowing a single
// offset. Given a root and two callbacks - read a block of memory, judge a candidate -
// it looks at every aligned pointer inside a window of each object, breadth first, and
// stops at the first candidate the judge accepts. A candidate is judged when it is
// discovered, and only one the judge calls an object is descended into, so the queue
// holds the shape of the object graph rather than every word that looked like a pointer.
//
// Depth, window and node count are all bounded, and every address already seen is
// remembered, so a cycle costs one visit and a wrong root costs the budget and nothing
// else. The caller owns every unsafe decision: `read` says what memory may be touched,
// `probe` says what an object is. This file only decides where to look next.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pw
{
    // What the caller's probe makes of a candidate.
    enum class Verdict
    {
        Skip,   // not an object of interest and not worth descending into
        Follow, // an object: look at the pointers inside it
        Accept  // the thing we came for; the walk stops here
    };

    struct Limits
    {
        int max_depth = 4;         // hops from the root; the root itself is depth 0
        int window_bytes = 0x200;  // how far into an object its pointer fields are looked for
        int max_nodes = 3000;      // candidates judged before the walk gives up
    };

    struct Result
    {
        const void* object = nullptr; // what the probe accepted, or null
        int depth = 0;                // how many hops from the root it sat
        int nodes = 0;                // candidates judged
        int follows = 0;              // objects whose insides were read
        bool budget_hit = false;      // the walk stopped because it ran out, not because it finished
    };

    // The walk allocates nothing, so its two tables are the caller's: one instance,
    // reused, owned by the thread that searches. `kSeenSlots` is a power of two and at
    // least twice `kNodeCap`, which keeps the open-addressing set from degrading.
    constexpr int kNodeCap = 4096;
    constexpr int kSeenSlots = 8192;

    struct Arena
    {
        struct Node
        {
            const void* p;
            int depth;
        };

        Node queue[kNodeCap];
        std::uintptr_t seen[kSeenSlots];

        void reset() { std::memset(seen, 0, sizeof(seen)); }

        // True if `v` was already there. A full table reports everything as seen, which
        // ends the walk rather than looping.
        bool mark(std::uintptr_t v)
        {
            std::uintptr_t h = v;
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDull;
            h ^= h >> 29;
            std::size_t i = static_cast<std::size_t>(h) & (kSeenSlots - 1);
            for (int step = 0; step < kSeenSlots; ++step)
            {
                if (seen[i] == 0)
                {
                    seen[i] = v;
                    return false;
                }
                if (seen[i] == v)
                {
                    return true;
                }
                i = (i + 1) & (kSeenSlots - 1);
            }
            return true;
        }
    };

    // `read`  : bool(const void* addr, void* out, std::size_t bytes) - false if that
    //           memory may not be touched. Never called with bytes == 0.
    // `probe` : Verdict(const void* candidate) - called once per distinct address. The
    //           root is not probed; it is followed by definition.
    template <class Read, class Probe>
    Result search(const void* root, const Limits& lim, Read read, Probe probe, Arena& arena)
    {
        Result out{};
        if (root == nullptr)
        {
            return out;
        }

        const int max_nodes = lim.max_nodes < 1 ? 1 : (lim.max_nodes > kNodeCap ? kNodeCap : lim.max_nodes);
        const int max_depth = lim.max_depth < 0 ? 0 : lim.max_depth;
        const std::size_t window = lim.window_bytes < 0 ? 0 : static_cast<std::size_t>(lim.window_bytes);

        arena.reset();
        int head = 0;
        int tail = 0;
        arena.mark(reinterpret_cast<std::uintptr_t>(root));
        arena.queue[tail++] = {root, 0};

        // Eight pointers at a time: one guarded read per chunk instead of one per slot,
        // and a chunk that straddles the end of a commit simply drops out.
        const void* buf[8];

        while (head < tail)
        {
            const Arena::Node node = arena.queue[head++];
            if (node.depth >= max_depth)
            {
                continue;
            }
            ++out.follows;

            const auto* base = static_cast<const unsigned char*>(node.p);
            for (std::size_t off = 0; off + sizeof(void*) <= window; off += sizeof(buf))
            {
                const std::size_t left = window - off;
                const std::size_t want = left < sizeof(buf) ? left & ~(sizeof(void*) - 1) : sizeof(buf);
                if (!read(base + off, buf, want))
                {
                    continue;
                }
                const std::size_t slots = want / sizeof(void*);
                for (std::size_t i = 0; i < slots; ++i)
                {
                    const void* q = buf[i];
                    const auto v = reinterpret_cast<std::uintptr_t>(q);
                    // A pointer, not a small integer, a tag or a misaligned field.
                    if (v < 0x10000 || v >= 0x7FFFFFFFFFFFull || (v & 7) != 0)
                    {
                        continue;
                    }
                    if (arena.mark(v))
                    {
                        continue;
                    }

                    ++out.nodes;
                    const Verdict verdict = probe(q);
                    if (verdict == Verdict::Accept)
                    {
                        out.object = q;
                        out.depth = node.depth + 1;
                        return out;
                    }
                    if (out.nodes >= max_nodes)
                    {
                        out.budget_hit = true;
                        return out;
                    }
                    if (verdict != Verdict::Follow || node.depth + 1 >= max_depth)
                    {
                        continue;
                    }
                    if (tail >= kNodeCap)
                    {
                        out.budget_hit = true;
                        return out;
                    }
                    arena.queue[tail++] = {q, node.depth + 1};
                }
            }
        }

        return out;
    }
} // namespace pw
