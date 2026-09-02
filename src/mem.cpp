#include "mem.hpp"

#include <Windows.h>

namespace mem
{
    namespace
    {
        // Last region VirtualQuery said was fine. Single-threaded use (the UE4SS
        // update thread), so no locking; a stale entry can only ever cause an
        // extra VirtualQuery, never a wrong "readable" answer for a new region.
        thread_local std::uintptr_t g_cache_begin = 0;
        thread_local std::uintptr_t g_cache_end = 0;
    } // namespace

    bool copy(const void* src, void* dst, std::size_t n) noexcept
    {
        __try
        {
            std::memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool readable(const void* p, std::size_t n) noexcept
    {
        if (p == nullptr || n == 0)
        {
            return false;
        }

        const auto begin = reinterpret_cast<std::uintptr_t>(p);
        if (begin < 0x10000u)
        {
            return false;
        }
        const auto end = begin + n;
        if (end < begin) // wrapped
        {
            return false;
        }

        if (begin >= g_cache_begin && end <= g_cache_end)
        {
            return true;
        }

        std::uintptr_t cursor = begin;
        std::uintptr_t region_begin = 0;
        std::uintptr_t region_end = 0;

        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            {
                return false;
            }
            if (mbi.State != MEM_COMMIT)
            {
                return false;
            }
            constexpr DWORD unreadable = PAGE_NOACCESS | PAGE_EXECUTE;
            if ((mbi.Protect & unreadable) != 0 || (mbi.Protect & PAGE_GUARD) != 0 || mbi.Protect == 0)
            {
                return false;
            }

            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto stop = base + mbi.RegionSize;
            if (stop <= cursor) // no forward progress: bail rather than spin
            {
                return false;
            }
            if (region_end == 0)
            {
                region_begin = base;
            }
            region_end = stop;
            cursor = stop;
        }

        // Cache only the contiguous run we just validated.
        g_cache_begin = region_begin;
        g_cache_end = region_end;
        return true;
    }

    bool read_ptr(const void* p, void*& out) noexcept
    {
        void* v = nullptr;
        if (!read<void*>(p, v))
        {
            return false;
        }
        if (!plausible_ptr(v))
        {
            return false;
        }
        out = v;
        return true;
    }
} // namespace mem
