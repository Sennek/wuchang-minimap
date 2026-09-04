#pragma once

//
// Guarded raw-memory reads, for walking non-reflected engine structs by pointer
// arithmetic. Two layers:
//
//   1. mem::readable() - VirtualQuery, rejecting decommitted / guard / no-access
//      pages before any dereference.
//   2. mem::copy()     - SEH-guarded memcpy, turning a lost-page race into `false`.
//
// Nothing here throws, allocates or logs. Callers use mem::read<T>().
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mem
{
    // SEH-guarded memcpy. Out of line: a function containing __try must not need C++
    // object unwinding (MSVC C2712).
    bool copy(const void* src, void* dst, std::size_t n) noexcept;

    // SEH-guarded call into foreign code, e.g. a trampoline issuing a
    // UObject::ProcessEvent. `fn` must be a plain function needing no C++ unwinding
    // (MSVC C2712). False when the call faulted.
    using GuardedFn = void (*)(void*, void*, void*);
    bool guarded_call(GuardedFn fn, void* a, void* b, void* c) noexcept;

    // True when [p, p+n) is entirely committed and readable. Caches the last accepted
    // region; the tile scan asks tens of thousands of times and a VirtualQuery is ~1 us.
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

    // A pointer-sized read that also rejects null, low, non-canonical and unaligned
    // values.
    bool read_ptr(const void* p, void*& out) noexcept;

    // Stronger than readable(): the target must sit in a committed, writable, PRIVATE
    // region - a heap allocation, not an image section, mapped file or read-only data -
    // whose remaining size from `p` is inside [min_size, max_size]. Engine structs such
    // as FPImplRecastNavMesh / dtNavMesh are always private RW heap blocks, so this is
    // what bounds the pointer chase in the navmesh discovery scan.
    bool region_ok(const void* p, std::size_t min_size, std::size_t max_size) noexcept;

    inline bool plausible_ptr(const void* p) noexcept
    {
        const auto v = reinterpret_cast<std::uintptr_t>(p);
        // User-mode x64: below 64 KiB is never mapped, above the 47-bit user range is
        // kernel space. Engine allocations are at least 4-byte aligned.
        return v >= 0x10000u && v < 0x7FFFFFFFFFFFull && (v & 0x3u) == 0u;
    }
} // namespace mem
