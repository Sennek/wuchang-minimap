//
// spinlock.hpp - the mod's only lock.
//
// `std::mutex` is unusable here: compiled against MSVC 14.40's STL,
// `std::unique_lock<std::mutex>::try_lock` faults with an access violation on the
// first call from this game's game thread, against whichever MSVCP140 the process
// already loaded (see lessons.md and context/crash-gamethread-mutex-CrashContext.xml).
// So every critical section in the mod is held by this header-only spinlock instead.
//
// Properties the call sites rely on:
//   * header-only and allocation-free - no CRT, no heap, safe inside ProcessEvent;
//   * constant-initialised - a `Spinlock g_lock;` at namespace scope needs no dynamic
//     initialiser, so it is ready before any other translation unit runs;
//   * `YieldProcessor()` for the first spins, `SwitchToThread()` every 64th, so a
//     contended lock does not burn a core while the holder waits for a timeslice;
//   * `try_lock_ms()` for callers that must never wait for ever.
//
// Critical sections under it must stay short and must not allocate: the render thread,
// the UE4SS event-loop thread and the game thread all take these locks.
//

#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace spin
{
    class Spinlock
    {
      public:
        void lock() noexcept
        {
            for (int spin = 0; flag_.test_and_set(std::memory_order_acquire); ++spin)
            {
                if ((spin & 0x3F) == 0x3F)
                {
                    ::SwitchToThread();
                }
                else
                {
                    YieldProcessor();
                }
            }
        }

        // A BOUNDED acquire, for callers that must never wait for ever - a render-thread
        // path that can be entered from a thread the render thread is itself waiting on
        // would turn a stall into a deadlock. Returns false without holding the lock when
        // the budget runs out; the deadline is tested on every backoff, so the wait is
        // bounded by `budget_ms` plus one timeslice.
        bool try_lock_ms(unsigned budget_ms) noexcept
        {
            const std::uint64_t deadline = ::GetTickCount64() + budget_ms;
            for (int spin = 0; flag_.test_and_set(std::memory_order_acquire); ++spin)
            {
                if ((spin & 0x3F) == 0x3F)
                {
                    if (::GetTickCount64() >= deadline)
                    {
                        return false;
                    }
                    ::SwitchToThread();
                }
                else
                {
                    YieldProcessor();
                }
            }
            return true;
        }

        // One attempt, no spinning at all.
        bool try_lock() noexcept
        {
            return !flag_.test_and_set(std::memory_order_acquire);
        }

        void unlock() noexcept
        {
            flag_.clear(std::memory_order_release);
        }

      private:
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };

    class SpinGuard
    {
      public:
        explicit SpinGuard(Spinlock& l) noexcept : lock_(l)
        {
            lock_.lock();
        }
        ~SpinGuard()
        {
            lock_.unlock();
        }
        SpinGuard(const SpinGuard&) = delete;
        SpinGuard& operator=(const SpinGuard&) = delete;

      private:
        Spinlock& lock_;
    };
} // namespace spin
