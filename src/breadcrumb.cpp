//
// breadcrumb - see breadcrumb.hpp. Everything in here is deliberately primitive.
//

#include "breadcrumb.hpp"

#include <Windows.h>

#include <cstring>

namespace crumb
{
    namespace
    {
        constexpr const wchar_t* kFileName = L"\\wuchang_minimap_last_stage.txt";

        wchar_t g_path[MAX_PATH]{};
        bool g_enabled = false;
        bool g_have_path = false;
        char g_previous[64]{};
        bool g_had_previous = false;
        char g_current[64]{};

        // strlen without <string>, so this file can stay free of anything that reaches
        // into the host CRT beyond the flat Win32 API.
        std::size_t len(const char* s)
        {
            std::size_t n = 0;
            while (s != nullptr && s[n] != '\0' && n < 512)
            {
                ++n;
            }
            return n;
        }

        void append(char* buf, std::size_t cap, std::size_t& at, const char* text)
        {
            const std::size_t n = len(text);
            for (std::size_t i = 0; i < n && at + 1 < cap; ++i)
            {
                buf[at++] = text[i];
            }
            buf[at] = '\0';
        }

        void append_u32(char* buf, std::size_t cap, std::size_t& at, unsigned long v, int width)
        {
            char digits[16]{};
            int n = 0;
            do
            {
                digits[n++] = static_cast<char>('0' + (v % 10u));
                v /= 10u;
            } while (v != 0u && n < 15);
            while (n < width)
            {
                digits[n++] = '0';
            }
            while (n > 0 && at + 1 < cap)
            {
                buf[at++] = digits[--n];
            }
            buf[at] = '\0';
        }

        void read_previous()
        {
            const HANDLE h = ::CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            char buf[128]{};
            DWORD read = 0;
            if (::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) != 0 && read > 0)
            {
                buf[read] = '\0';
                // The stage name is everything up to the first tab - the rest of the line
                // is the timestamp and the thread id.
                std::size_t n = 0;
                while (n + 1 < sizeof(g_previous) && buf[n] != '\0' && buf[n] != '\t' && buf[n] != '\r' &&
                       buf[n] != '\n')
                {
                    g_previous[n] = buf[n];
                    ++n;
                }
                g_previous[n] = '\0';
                g_had_previous = n > 0;
            }
            ::CloseHandle(h);
        }
    } // namespace

    void init(const wchar_t* dir, bool enabled)
    {
        g_enabled = enabled;
        g_have_path = false;
        g_previous[0] = '\0';
        g_had_previous = false;
        if (dir == nullptr || dir[0] == L'\0')
        {
            return;
        }
        const std::size_t dn = ::wcsnlen(dir, MAX_PATH);
        const std::size_t fn = ::wcslen(kFileName);
        if (dn + fn + 1 >= MAX_PATH)
        {
            return;
        }
        ::memcpy(g_path, dir, dn * sizeof(wchar_t));
        ::memcpy(g_path + dn, kFileName, (fn + 1) * sizeof(wchar_t));
        g_have_path = true;
        read_previous();
    }

    void stage(const char* name)
    {
        if (name == nullptr)
        {
            return;
        }
        // The in-memory copy is kept even when the FILE is switched off, so the F2 Debug
        // tab can still say where the overlay is.
        std::size_t at = 0;
        g_current[0] = '\0';
        append(g_current, sizeof(g_current), at, name);
        if (!g_enabled || !g_have_path)
        {
            return;
        }

        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        char line[192]{};
        std::size_t n = 0;
        append(line, sizeof(line), n, name);
        append(line, sizeof(line), n, "\t");
        append_u32(line, sizeof(line), n, st.wHour, 2);
        append(line, sizeof(line), n, ":");
        append_u32(line, sizeof(line), n, st.wMinute, 2);
        append(line, sizeof(line), n, ":");
        append_u32(line, sizeof(line), n, st.wSecond, 2);
        append(line, sizeof(line), n, ".");
        append_u32(line, sizeof(line), n, st.wMilliseconds, 3);
        append(line, sizeof(line), n, "\ttid ");
        append_u32(line, sizeof(line), n, ::GetCurrentThreadId(), 1);
        append(line, sizeof(line), n, "\r\n");

        // CREATE_ALWAYS: the file is one line and is rewritten, never appended to - a
        // growing log is what UE4SS's own log is for. FILE_FLAG_WRITE_THROUGH plus the
        // close is what makes the content survive a process that dies immediately after.
        const HANDLE h = ::CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        ::WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
        ::FlushFileBuffers(h);
        ::CloseHandle(h);
    }

    const char* previous()
    {
        return g_previous;
    }

    const char* current()
    {
        return g_current;
    }

    bool previous_suspicious()
    {
        if (!g_had_previous)
        {
            return false; // a first run is not a crash
        }
        return ::strcmp(g_previous, kCleanExit) != 0 && ::strcmp(g_previous, kTeardownEnd) != 0;
    }
} // namespace crumb
