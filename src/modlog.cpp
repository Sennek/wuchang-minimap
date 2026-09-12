//
// modlog - every line the mod says, and the file it says it into.
//
// TWO THREADS, TWO LOCKS. `log()` on the loop thread writes straight through; from any
// other thread the line is stamped with the wall clock and queued under `g_log_lock`,
// bounded and never blocking, for `drain_log()` to write later. The file itself sits
// behind `g_modlog_lock` - a lock of its own, so enqueueing from the game thread never
// waits on a file syscall.
//
// Flat Win32 over a hand-built UTF-8 buffer: no iostreams and no C++ locale, because
// the game thread must not touch MSVCP140's stream code. The level this file honours is
// `g_log_level`, which the config loader writes and `mm::log_enabled()` reads.
//

#include "mmstate.hpp"

#include "spinlock.hpp"

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

using namespace RC;

namespace mm
{
    namespace
    {
        spin::Spinlock g_log_lock;
        // A line logged off the loop thread waits here until the loop thread drains it,
        // which can be a whole frame later - so the wall clock is taken when the line is
        // MADE, not when it is written. The stamp is POD beside the string, so it costs
        // the queue entry one SYSTEMTIME and no allocation.
        struct LogEntry
        {
            std::wstring line;
            SYSTEMTIME at{};
        };
        std::vector<LogEntry> g_log_queue;
        DWORD g_loop_thread = 0;
        // Dropped, never blocked and never grown: the game thread must not wait on the loop thread.
        constexpr std::size_t kLogQueueMax = 4096;
        std::size_t g_log_dropped = 0;
    } // namespace

    std::atomic<int> g_log_level{static_cast<int>(LogLv::Normal)};

    const char* log_level_name(LogLv lv) noexcept
    {
        switch (lv)
        {
        case LogLv::Verbose:
            return "verbose";
        case LogLv::Trace:
            return "trace";
        case LogLv::Normal:
        default:
            return "normal";
        }
    }

    bool log_level_from_name(std::string_view name, LogLv& out) noexcept
    {
        if (name == "normal")
        {
            out = LogLv::Normal;
            return true;
        }
        if (name == "verbose")
        {
            out = LogLv::Verbose;
            return true;
        }
        if (name == "trace")
        {
            out = LogLv::Trace;
            return true;
        }
        return false;
    }

    void set_loop_thread()
    {
        g_loop_thread = ::GetCurrentThreadId();
    }

    // The mod's own rolling log - wuchang_minimap.log, keeping the last four sessions through its own
    // `.1` / `.2` / `.3` rotation, because UE4SS truncates `UE4SS.log` on every launch.
    // Flat `CreateFileW` / `WriteFile` over a hand-built UTF-8 buffer: the game thread must never touch
    // C++ iostreams or the C++ locale. Writing is BUFFERED (8 KB) and flushed when the buffer fills, on
    // every crash breadcrumb write, and every few seconds from the loop thread.
    // `modlog_line()` runs on the loop thread only, but `modlog_flush()` is called from any thread, so the
    // buffer and the handle sit behind a spinlock of their OWN (g_modlog_lock), never the log queue's.
    // Nothing in here allocates while holding it.

    namespace
    {
        constexpr std::size_t kModLogFlushAt = 8192;
        // The longest line written verbatim. The buffer flushes at kModLogFlushAt and is reserved at twice that,
        // so buffer + line + cap notice never reach the reserve - `append` cannot reallocate under the lock.
        constexpr std::size_t kModLogMaxLine = 4096;
        // Per-session cap. At the cap one line says so and writing stops; the rotation is untouched.
        constexpr std::uint64_t kModLogMaxBytes = 20ull * 1024ull * 1024ull;
        constexpr const wchar_t* kModLogName = L"\\wuchang_minimap.log";

        // A lock of its own, not the log QUEUE's, so enqueueing a line from the game thread never waits on a file
        // syscall. `drain_log` swaps the queue out under `g_log_lock`, releases it, then writes.
        spin::Spinlock g_modlog_lock;
        HANDLE g_modlog = INVALID_HANDLE_VALUE;
        std::string g_modlog_buf;
        bool g_modlog_opened = false;
        bool g_modlog_capped = false;
        std::uint64_t g_modlog_bytes = 0;
        std::uint64_t g_modlog_last_flush_ms = 0;

        std::string utf8_of(const std::wstring& w)
        {
            if (w.empty())
            {
                return {};
            }
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                                   nullptr, 0, nullptr, nullptr);
            if (need <= 0)
            {
                return {};
            }
            std::string out;
            out.resize(static_cast<std::size_t>(need));
            ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                                  nullptr, nullptr);
            return out;
        }

        // Rotate, then create: `.3` is dropped, everything else shifts up one, and the live file is always `wuchang_minimap.log`.
        void modlog_rotate(const std::wstring& base)
        {
            const std::wstring p1 = base + L".1";
            const std::wstring p2 = base + L".2";
            const std::wstring p3 = base + L".3";
            ::DeleteFileW(p3.c_str());
            ::MoveFileExW(p2.c_str(), p3.c_str(), MOVEFILE_REPLACE_EXISTING);
            ::MoveFileExW(p1.c_str(), p2.c_str(), MOVEFILE_REPLACE_EXISTING);
            ::MoveFileExW(base.c_str(), p1.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        // Loop thread, lazily on the first line. FILE_SHARE_READ so the file can be read while the game is still running.
        void modlog_open_locked()
        {
            if (g_modlog_opened)
            {
                return;
            }
            g_modlog_opened = true; // one attempt per session, success or not
            const std::wstring base = state_dir() + kModLogName;
            modlog_rotate(base);
            g_modlog = ::CreateFileW(base.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_modlog == INVALID_HANDLE_VALUE)
            {
                return;
            }
            g_modlog_buf.reserve(kModLogFlushAt * 2);
        }

        void modlog_write_locked()
        {
            if (g_modlog == INVALID_HANDLE_VALUE || g_modlog_buf.empty())
            {
                return;
            }
            DWORD written = 0;
            ::WriteFile(g_modlog, g_modlog_buf.data(), static_cast<DWORD>(g_modlog_buf.size()), &written,
                        nullptr);
            g_modlog_bytes += g_modlog_buf.size();
            g_modlog_buf.clear();
        }

        // `at` is when the line was made. Null means now, which is right for every line
        // written from the loop thread itself.
        void modlog_line(const std::wstring& line, const SYSTEMTIME* at = nullptr)
        {
            SYSTEMTIME st{};
            if (at != nullptr)
            {
                st = *at;
            }
            else
            {
                ::GetLocalTime(&st);
            }
            wchar_t stamp[32]{};
            ::_snwprintf_s(stamp, std::size(stamp), _TRUNCATE, L"%02u:%02u:%02u.%03u ",
                           static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                           static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
            std::string text = utf8_of(std::wstring{stamp} + line);
            // The buffer must not reallocate under g_modlog_lock: modlog_flush() takes that lock from the crash
            // breadcrumb and the stall watchdog. Truncating on a UTF-8 character boundary keeps buffer + line +
            // cap notice inside the 2 x kModLogFlushAt bytes reserved once at open time.
            if (text.size() > kModLogMaxLine)
            {
                std::size_t cut = kModLogMaxLine;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
                {
                    --cut;
                }
                text.resize(cut);
                text += " [line truncated]";
            }
            spin::SpinGuard guard(g_modlog_lock);
            modlog_open_locked();
            if (g_modlog == INVALID_HANDLE_VALUE)
            {
                return;
            }
            if (g_modlog_capped)
            {
                return;
            }
            g_modlog_buf.append(text);
            g_modlog_buf.append("\r\n");
            if (g_modlog_bytes + g_modlog_buf.size() >= kModLogMaxBytes)
            {
                g_modlog_buf.append("--- log capped at 20 MB for this session; nothing more is written "
                                    "to this file (the .1 / .2 / .3 rotation is unaffected) ---\r\n");
                modlog_write_locked();
                g_modlog_capped = true;
                return;
            }
            if (g_modlog_buf.size() >= kModLogFlushAt)
            {
                modlog_write_locked();
            }
        }
    } // namespace

    void modlog_flush()
    {
        spin::SpinGuard guard(g_modlog_lock);
        modlog_write_locked();
        if (g_modlog != INVALID_HANDLE_VALUE)
        {
            ::FlushFileBuffers(g_modlog);
        }
    }

    void modlog_tick(std::uint64_t now_ms)
    {
        if (now_ms - g_modlog_last_flush_ms < 3000)
        {
            return;
        }
        g_modlog_last_flush_ms = now_ms;
        modlog_flush();
    }

    std::wstring modlog_path()
    {
        return state_dir() + kModLogName;
    }

    void log(const std::wstring& line)
    {
        if (g_loop_thread == 0 || ::GetCurrentThreadId() != g_loop_thread)
        {
            // Outside the lock: GetLocalTime is a read of shared kernel data and this is
            // the render thread's own timestamp, not something the drain needs to serialise.
            SYSTEMTIME at{};
            ::GetLocalTime(&at);
            spin::SpinGuard guard(g_log_lock);
            if (g_log_queue.size() < kLogQueueMax)
            {
                g_log_queue.push_back(LogEntry{line, at});
            }
            else
            {
                ++g_log_dropped;
            }
            return;
        }
        Output::send<LogLevel::Verbose>(STR("[minimap] {}\n"), line);
        modlog_line(line);
    }

    void drain_log()
    {
        std::vector<LogEntry> lines;
        std::size_t dropped = 0;
        {
            spin::SpinGuard guard(g_log_lock);
            if (g_log_queue.empty() && g_log_dropped == 0)
            {
                return;
            }
            lines.swap(g_log_queue);
            dropped = g_log_dropped;
            g_log_dropped = 0;
        }
        if (dropped != 0)
        {
            const std::wstring note =
                std::format(L"log: dropped {} line(s) - the queue was full (the loop thread was not "
                            L"draining, or something is logging far too fast)",
                            dropped);
            Output::send<LogLevel::Warning>(STR("[minimap] {}\n"), note);
            modlog_line(note);
        }
        for (const LogEntry& e : lines)
        {
            Output::send<LogLevel::Verbose>(STR("[minimap] {}\n"), e.line);
            modlog_line(e.line, &e.at);
        }
    }
} // namespace mm
