#pragma once

//
// Guarded raw-memory reads.
//
// The navmesh dumper walks non-reflected engine structs by pointer arithmetic, so
// every single read has to survive a wrong guess. Two layers protect it:
//
//   1. mem::readable() - VirtualQuery, so a pointer into a decommitted / guard /
//      no-access region is rejected before it is ever dereferenced.
//   2. mem::copy()     - an SEH-guarded memcpy, so even a race (the page went away
//      between the query and the read) turns into `false` instead of a crash.
//
// Nothing in here throws, allocates or logs. mem::read<T>() is the only function
// the rest of the dumper should use.
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mem
{
    // SEH-guarded memcpy. Lives in mem.cpp because a function containing __try
    // must not need C++ object unwinding (MSVC C2712).
    bool copy(const void* src, void* dst, std::size_t n) noexcept;

    // SEH-guarded call into foreign code. `fn` must be a plain function (no C++
    // unwinding needed at the call site, MSVC C2712) that does the risky work with
    // the three opaque arguments - e.g. a trampoline that issues a
    // UObject::ProcessEvent. Returns false when the call faulted, which is how a
    // UObject that died under us stops being a crash and becomes "no data".
    using GuardedFn = void (*)(void*, void*, void*);
    bool guarded_call(GuardedFn fn, void* a, void* b, void* c) noexcept;

    // True when [p, p+n) is entirely committed and readable. Caches the last
    // accepted region, which matters: the tile scan asks this tens of thousands
    // of times and a VirtualQuery is ~1 us.
    bool readable(const void* p, std::size_t n) noexcept;

    // Belt and braces: query, then guarded copy.
    template <typename T>
    inline bool read(const void* p, T& out) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "mem::read is for POD only");
        if (!readable(p, sizeof(T)))
        {
            return false;
        }
        return copy(p, &out, sizeof(T));
    }

    template <typename T>
    inline bool read_at(const void* base, std::size_t byte_offset, T& out) noexcept
    {
        return read<T>(static_cast<const std::uint8_t*>(base) + byte_offset, out);
    }

    // A pointer-sized read that additionally rejects the obviously-bogus values a
    // misidentified field produces (null, low addresses, non-canonical, unaligned).
    bool read_ptr(const void* p, void*& out) noexcept;

    // Stronger than readable(): the target must sit in a committed, readable,
    // *writable and private* region (i.e. a heap allocation, not an image section,
    // not a mapped file, not read-only data) whose remaining size from `p` is inside
    // [min_size, max_size].
    //
    // This is what bounds the depth-2 pointer chase in the navmesh discovery scan.
    // readable() alone accepts thousands of pointers into loaded images, mapped paks
    // and thread stacks; following those is pure waste and it is exactly where a
    // guard page or a concurrently-freed block would be met. An engine struct such as
    // FPImplRecastNavMesh / dtNavMesh is always a private RW heap block.
    bool region_ok(const void* p, std::size_t min_size, std::size_t max_size) noexcept;

    inline bool plausible_ptr(const void* p) noexcept
    {
        const auto v = reinterpret_cast<std::uintptr_t>(p);
        // User-mode x64: below 64 KiB is never mapped, above the 47-bit user range
        // is kernel space. Engine allocations are always at least 4-byte aligned.
        return v >= 0x10000u && v < 0x7FFFFFFFFFFFull && (v & 0x3u) == 0u;
    }
} // namespace mem
