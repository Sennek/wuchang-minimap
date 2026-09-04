//
// spinlock.hpp - the mod's only lock. `std::mutex` is unusable in this process:
// `std::unique_lock<std::mutex>::try_lock` faults on the game thread against whichever
// MSVCP140 the game already loaded.
//
// Properties the call sites rely on:
//   * header-only and allocation-free - no CRT, no heap, safe inside ProcessEvent;
//   * constant-initialised, so a namespace-scope `Spinlock` is ready before any other
//     translation unit runs;
//   * `YieldProcessor()` for the first spins, `SwitchToThread()` every 64th.
//
// The render thread, the UE4SS event-loop thread and the game thread all take these
// locks, so critical sections must stay short and must not allocate.
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

        // Bounded acquire: returns false without the lock once `budget_ms` runs out.
        // The deadline is tested on every backoff, so the wait is budget + one timeslice.
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
