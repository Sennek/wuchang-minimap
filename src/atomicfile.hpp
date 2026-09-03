//
// atomicfile.hpp - whole-file reads and CRASH-SAFE whole-file writes.
//
// Every file this mod owns (the found tracker, the config files, the waypoint) used to
// be written with CREATE_ALWAYS + WriteFile straight over the live file: the moment the
// handle opens, the player's data is gone, and a crash or a power cut anywhere in the
// next few milliseconds leaves a truncated file behind. The found tracker is the one
// that hurts - it is the player's collection progress and nothing else in the game
// carries it.
//
// So every write goes:
//
//     write <path>.tmp  ->  FlushFileBuffers  ->  keep <path> as <path>.bak
//                       ->  MoveFileExW(<path>.tmp -> <path>,
//                                       REPLACE_EXISTING | WRITE_THROUGH)
//
// The rename is the commit point: after it the file is either wholly the old contents
// or wholly the new ones, never half of each. `.bak` is the previous generation, kept
// for the found tracker only (a config file the player can retype; a 300-id collection
// they cannot).
//
// Header-only and Win32-only on purpose: it is used by the DLL and by the offline test
// exe, and neither may pull in iostreams (lessons.md - the game thread must not touch
// MSVCP140's stream/locale code).
//
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <windows.h>

namespace mmfile
{
    // Why a read did not produce data. The distinction is load-bearing: "the file is
    // not there" means a fresh install, and "I could not read the file that IS there"
    // means something else has it open (a virus scanner, OneDrive, a text editor) and
    // the mod must NOT then write an empty set over it.
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

    // <path>.tmp / <path>.bak. Pure, so the naming is testable.
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
    // GetLastError() from whichever step failed, so the caller can say WHY in its log
    // line. A failed write leaves `<path>` exactly as it was.
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
        // The bytes have to be on the disk BEFORE the rename, or a power cut can commit
        // a name pointing at nothing.
        if (::FlushFileBuffers(h) == 0)
        {
            const unsigned e = static_cast<unsigned>(::GetLastError());
            ::CloseHandle(h);
            return fail(e);
        }
        ::CloseHandle(h);

        if (keep_backup)
        {
            // A copy, not a move: a move would leave `<path>` missing for the width of
            // the second rename. A failure here is not fatal - the backup is a courtesy,
            // the commit below is the guarantee.
            ::CopyFileW(path.c_str(), bak_path(path).c_str(), FALSE);
        }

        if (::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
        {
            // Deliberately NOT deleting the temp file: it holds the only copy of the
            // data that was meant to be saved, and the caller logs the path.
            return fail(static_cast<unsigned>(::GetLastError()));
        }
        if (err != nullptr)
        {
            *err = 0;
        }
        return true;
    }
} // namespace mmfile
