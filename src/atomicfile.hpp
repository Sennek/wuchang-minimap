//
// atomicfile.hpp - whole-file reads and crash-safe whole-file writes.
//
//     write <path>.tmp  ->  FlushFileBuffers  ->  keep <path> as <path>.bak
//                       ->  MoveFileExW(<path>.tmp -> <path>,
//                                       REPLACE_EXISTING | WRITE_THROUGH)
//
// The rename is the commit point: afterwards the file is wholly old or wholly new,
// never half of each. `.bak` is the previous generation, kept for the found tracker.
//
// Header-only and Win32-only: shared by the DLL and the offline test exe, and the game
// thread must not touch MSVCP140's stream/locale code.
//
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <windows.h>

namespace mmfile
{
    // Why a read produced no data. NotFound is a fresh install; Failed means something
    // else holds the file open, and the mod must not write an empty set over it.
    enum class ReadStatus
    {
        Ok,
        NotFound,
        Failed,
    };

    struct ReadInfo
    {
        ReadStatus status = ReadStatus::Failed;
        unsigned error = 0;                 // GetLastError() at the failing call
        bool too_big = false;               // the size cap rejected it
        unsigned long long size = 0;        // the size the file reported
    };

    // <path>.tmp / <path>.bak.
    inline std::wstring tmp_path(std::wstring_view path)
    {
        return std::wstring{path} + L".tmp";
    }

    inline std::wstring bak_path(std::wstring_view path)
    {
        return std::wstring{path} + L".bak";
    }

    inline ReadInfo read_whole_file(const std::wstring& path, std::string& out, unsigned long long max_bytes)
    {
        ReadInfo info{};
        out.clear();
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            info.error = static_cast<unsigned>(::GetLastError());
            info.status = (info.error == ERROR_FILE_NOT_FOUND || info.error == ERROR_PATH_NOT_FOUND)
                              ? ReadStatus::NotFound
                              : ReadStatus::Failed;
            return info;
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(h, &size) == 0 || size.QuadPart < 0)
        {
            info.error = static_cast<unsigned>(::GetLastError());
            ::CloseHandle(h);
            return info;
        }
        info.size = static_cast<unsigned long long>(size.QuadPart);
        if (info.size > max_bytes)
        {
            info.too_big = true;
            ::CloseHandle(h);
            return info;
        }
        out.resize(static_cast<std::size_t>(info.size));
        DWORD read = 0;
        const bool ok = out.empty() ||
                        (::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 &&
                         read == out.size());
        if (!ok)
        {
            info.error = static_cast<unsigned>(::GetLastError());
            out.clear();
            ::CloseHandle(h);
            return info;
        }
        ::CloseHandle(h);
        info.status = ReadStatus::Ok;
        return info;
    }

    // The temp-file-plus-rename write described at the top of this file. `err` receives
    // GetLastError() from the failing step. A failed write leaves `<path>` untouched.
    inline bool write_whole_file_atomic(const std::wstring& path, std::string_view data, bool keep_backup,
                                        unsigned* err = nullptr)
    {
        const auto fail = [&](unsigned e) {
            if (err != nullptr)
            {
                *err = e;
            }
            return false;
        };

        const std::wstring tmp = tmp_path(path);
        const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return fail(static_cast<unsigned>(::GetLastError()));
        }
        DWORD written = 0;
        if (!data.empty() &&
            (::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) == 0 ||
             written != data.size()))
        {
            const unsigned e = static_cast<unsigned>(::GetLastError());
            ::CloseHandle(h);
            return fail(e);
        }
        // The bytes must reach the disk before the rename, or a power cut commits a
        // name pointing at nothing.
        if (::FlushFileBuffers(h) == 0)
        {
            const unsigned e = static_cast<unsigned>(::GetLastError());
            ::CloseHandle(h);
            return fail(e);
        }
        ::CloseHandle(h);

        if (keep_backup)
        {
            // A copy, not a move: a move leaves `<path>` missing across the second
            // rename. Failure here is not fatal; the commit below is the guarantee.
            ::CopyFileW(path.c_str(), bak_path(path).c_str(), FALSE);
        }

        if (::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
        {
            // The temp file stays: it holds the only copy of the data to be saved.
            return fail(static_cast<unsigned>(::GetLastError()));
        }
        if (err != nullptr)
        {
            *err = 0;
        }
        return true;
    }
} // namespace mmfile
