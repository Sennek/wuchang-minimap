#pragma once

//
// scriptmap - PURE. Decodes a UE 5.1 `FScriptMap` header and its element buffer into the
// keys of its live entries. No Windows, no UE4SS, no D3D12: the caller supplies bytes it
// has already copied out of the game with mem::read, so tests/markers_test.cpp exercises
// the exact decode the game thread runs.
//
// The only map the mod reads is `TMap<int32, int32>` - a pickup's `{item id -> amount}`.
//
// FScriptMap is FScriptSet is TSparseArray, 80 bytes:
//
//      0   TArray Data          { void* Data; int32 Num; int32 Max }
//     16   TBitArray AllocationFlags
//              16   uint32 InlineWords[4]      (TInlineAllocator<4>)
//              32   void*  SecondaryWords      (null while the words fit inline)
//              40   int32  NumBits
//              44   int32  MaxBits
//     48   int32 FirstFreeIndex
//     52   int32 NumFreeIndices
//     56   Hash                 { int32 InlineHash; pad; void* SecondaryHash }  (TInlineAllocator<1>)
//     72   int32 HashSize
//
// `Data` is a sparse array: a slot is a live `TSetElement<TPair<int32,int32>>`
//
//     { int32 Key; int32 Value; int32 HashNextId; int32 HashIndex }     stride 16
//
// only when its AllocationFlags bit is set. A free slot unions that with the free-list
// link, so its first eight bytes are two indices that read as a perfectly plausible
// key/amount pair - which is why the bit, not the bytes, decides.
//

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace smap
{
    // sizeof(FScriptMap) in this engine build. A property whose reflected element size is
    // not this is not a map the decoder understands, and is rejected rather than guessed at.
    inline constexpr int kScriptMapSize = 80;

    // sizeof(TSetElement<TPair<int32, int32>>).
    inline constexpr int kElementStride = 16;

    // The most slots the decoder walks - it bounds the caller's stack buffers. A pickup
    // grants a handful of items; anything wider is a misread, not a generous drop.
    inline constexpr int kMaxSlots = 64;

    struct Header
    {
        std::uint64_t data = 0;  // the element buffer
        std::int32_t num = 0;    // slots in use, LIVE AND FREE
        std::int32_t max = 0;
        std::uint32_t inline_words[4]{}; // allocation bits while they fit inline
        std::uint64_t words_ptr = 0;     // allocation bits once they do not
        std::int32_t num_bits = 0;
        std::int32_t max_bits = 0;
        std::int32_t first_free = 0;
        std::int32_t num_free = 0;
        std::int32_t hash_size = 0;

        // TInlineAllocator keeps its secondary pointer null for as long as the inline
        // storage is the live one.
        bool words_are_inline() const
        {
            return words_ptr == 0;
        }

        int live_count() const
        {
            return num - num_free;
        }

        // uint32 words of allocation bits the slots need.
        int word_count() const
        {
            return (num + 31) / 32;
        }
    };

    // Bits the inline storage holds: 4 words.
    inline constexpr int kInlineBits = 128;

    // Reads an 80-byte FScriptMap and validates it against its own invariants: the bit
    // array counts exactly the sparse array's slots, free slots are a subset of them, a
    // populated map has a buffer, and the hash is a power-of-two bucket count. `out` is
    // meaningful only when this returns true.
    inline bool parse(const unsigned char* raw, std::size_t n, Header& out)
    {
        if (raw == nullptr || n < static_cast<std::size_t>(kScriptMapSize))
        {
            return false;
        }
        Header h{};
        std::memcpy(&h.data, raw + 0, sizeof(h.data));
        std::memcpy(&h.num, raw + 8, sizeof(h.num));
        std::memcpy(&h.max, raw + 12, sizeof(h.max));
        std::memcpy(h.inline_words, raw + 16, sizeof(h.inline_words));
        std::memcpy(&h.words_ptr, raw + 32, sizeof(h.words_ptr));
        std::memcpy(&h.num_bits, raw + 40, sizeof(h.num_bits));
        std::memcpy(&h.max_bits, raw + 44, sizeof(h.max_bits));
        std::memcpy(&h.first_free, raw + 48, sizeof(h.first_free));
        std::memcpy(&h.num_free, raw + 52, sizeof(h.num_free));
        std::memcpy(&h.hash_size, raw + 72, sizeof(h.hash_size));

        if (h.num < 0 || h.max < h.num || h.max > 0x10000)
        {
            return false;
        }
        // TSparseArray keeps one allocation bit per slot, always.
        if (h.num_bits != h.num || h.max_bits < h.num_bits || h.max_bits > 0x10000)
        {
            return false;
        }
        if (h.num_free < 0 || h.num_free > h.num)
        {
            return false;
        }
        if (h.num > 0 && h.data == 0)
        {
            return false;
        }
        if (h.max_bits > kInlineBits && h.words_ptr == 0)
        {
            return false;
        }
        if (h.hash_size < 0 || (h.hash_size & (h.hash_size - 1)) != 0)
        {
            return false;
        }
        out = h;
        return true;
    }

    // Is slot `i` a live entry? `words` is `word_count()` uint32s - the header's inline
    // words, or what the caller copied from `words_ptr`.
    inline bool slot_live(const Header& h, const std::uint32_t* words, int word_count, int i)
    {
        if (words == nullptr || i < 0 || i >= h.num)
        {
            return false;
        }
        const int w = i / 32;
        if (w >= word_count)
        {
            return false;
        }
        return (words[w] & (1u << (i % 32))) != 0;
    }

    // The keys of the live entries, in slot order. `elems` is `h.num * kElementStride`
    // bytes copied from `h.data`. Returns how many keys landed in `out_keys`.
    inline int live_keys(const Header& h, const std::uint32_t* words, int word_count,
                         const unsigned char* elems, std::size_t elems_bytes,
                         std::int32_t* out_keys, int cap)
    {
        if (elems == nullptr || out_keys == nullptr || cap <= 0)
        {
            return 0;
        }
        int found = 0;
        for (int i = 0; i < h.num && found < cap; ++i)
        {
            const std::size_t at = static_cast<std::size_t>(i) * kElementStride;
            if (at + kElementStride > elems_bytes)
            {
                break;
            }
            if (!slot_live(h, words, word_count, i))
            {
                continue;
            }
            std::int32_t key = 0;
            std::memcpy(&key, elems + at, sizeof(key));
            out_keys[found++] = key;
        }
        return found;
    }
} // namespace smap
