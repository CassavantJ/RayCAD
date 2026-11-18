// Lock file, Windows implementation (PLAN.md 4.3). The POSIX one is lock_posix.cpp.
//
// The owner opens the lock file with CREATE_NEW and share mode FILE_SHARE_READ only, and keeps the handle for the
// session. Another process asking for DELETE access is then refused with a sharing violation for as long as that
// handle exists, which is how "owner provably dead" is detected, and the owner's file cannot be renamed away.
#include "lock.h"

#ifdef _WIN32
#include <windows.h>

#include <cstring>
#include <vector>

namespace ray::store::detail {

class OwnedLock::Held {
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
};

void OwnedLock::HeldDeleter::operator()(Held* held) const noexcept
{
    if (held->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(held->handle);
    }
    delete held;
}

namespace os {
namespace {

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    }
    return out;
}

std::string narrow(const wchar_t* text, std::size_t length)
{
    if (length == 0) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), n, nullptr, nullptr);
    }
    return out;
}

std::int64_t fileTimeToUnixMs(const FILETIME& ft) noexcept
{
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10000) - 11'644'473'600'000LL;
}

std::string winError(const char* what, DWORD code)
{
    return std::string(what) + ": Windows error " + std::to_string(code);
}

Result<std::string> readAll(HANDLE h)
{
    std::string bytes(4096, '\0');
    DWORD got = 0;
    OVERLAPPED at{};
    if (!ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, &at)) {
        const DWORD e = GetLastError();
        if (e != ERROR_HANDLE_EOF) {
            return fail(ErrorCode::IoError, "Cannot read the lock file", winError("ReadFile", e));
        }
        got = 0;
    }
    bytes.resize(got);
    return bytes;
}

void markForDelete(HANDLE h) noexcept
{
    // POSIX semantics remove the name at once, so the next opener can create the lock file immediately even if a
    // reader still has the old one open. Older systems and some servers only support the classic flag.
    FILE_DISPOSITION_INFO_EX ex{};
    ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (!SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof ex)) {
        FILE_DISPOSITION_INFO classic{};
        classic.DeleteFile = TRUE;
        SetFileInformationByHandle(h, FileDispositionInfo, &classic, sizeof classic);
    }
}

} // namespace

Result<CreateOutcome> createLockFile(const std::string& path, std::string_view content, OwnedLock::HeldPtr& out)
{
    const HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) {
            return CreateOutcome::Exists;
        }
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) {
            return CreateOutcome::Retry; // the previous lock file is still being deleted
        }
        return fail(ErrorCode::IoError, "Cannot create the lock file beside the document", winError("CreateFileW", e));
    }
    OwnedLock::HeldPtr held(new OwnedLock::Held);
    held->handle = h;
    // The handle exists from the moment the file does, so a reader that catches the file still empty knows its owner
    // is alive and waits (lock.cpp).
    if (auto st = writeHeld(*held, content); !st) {
        deleteHeld(std::move(held));
        return tl::unexpected<Error>(st.error());
    }
    out = std::move(held);
    return CreateOutcome::Created;
}

Status writeHeld(OwnedLock::Held& held, std::string_view bytes)
{
    OVERLAPPED at{};
    DWORD written = 0;
    if (!WriteFile(held.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, &at) ||
        written != bytes.size()) {
        return fail(ErrorCode::IoError, "Cannot write the lock file", winError("WriteFile", GetLastError()));
    }
    if (!FlushFileBuffers(held.handle)) {
        return fail(ErrorCode::IoError, "Cannot write the lock file", winError("FlushFileBuffers", GetLastError()));
    }
    return {};
}

void deleteHeld(OwnedLock::HeldPtr held) noexcept
{
    if (held && held->handle != INVALID_HANDLE_VALUE) {
        markForDelete(held->handle);
    }
}

Result<LockFileView> readLockFile(const std::string& path)
{
    const std::wstring wide = widen(path);
    const HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_ACCESS_DENIED) {
            // Access denied on an existing name means delete pending: the lock is going away.
            return fail(ErrorCode::NotFound, "No lock file", winError("CreateFileW", e));
        }
        return fail(ErrorCode::IoError, "Cannot read the lock file", winError("CreateFileW", e));
    }
    LockFileView view;
    auto bytes = readAll(h);
    FILETIME written{};
    const BOOL timeOk = GetFileTime(h, nullptr, nullptr, &written);
    CloseHandle(h);
    if (!bytes) {
        return tl::unexpected<Error>(bytes.error());
    }
    view.bytes = std::move(*bytes);
    view.mtimeMs = timeOk ? fileTimeToUnixMs(written) : 0;
    // Owner-alive probe: the owner's handle denies delete sharing, so this open fails while the owner lives.
    const HANDLE probe = CreateFileW(wide.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        view.ownerDead = true;
        CloseHandle(probe);
    }
    return view;
}

std::int64_t serverNowMs(const std::string& path)
{
    const std::wstring probe = widen(path + ".probe-" + randomName());
    const HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return unixNowMs(); // folder not writable: only the local clock is left
    }
    DWORD written = 0;
    const char byte = 'x';
    WriteFile(h, &byte, 1, &written, nullptr);
    FlushFileBuffers(h);
    FILETIME ft{};
    const BOOL ok = GetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok ? fileTimeToUnixMs(ft) : unixNowMs();
}

Status breakLockFile(const std::string& path, const std::function<Status(const LockFileView&)>& mayBreak)
{
    const HANDLE h =
        CreateFileW(widen(path).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return {}; // nothing to break
        }
        if (e == ERROR_SHARING_VIOLATION) {
            // The owner still has the file open: it is alive (or its server has not noticed its death yet).
            std::string who;
            if (auto view = readLockFile(path)) {
                if (auto fields = decodeLock(view->bytes)) {
                    who = " by " + fields->user + "@" + fields->host;
                }
            }
            return fail(ErrorCode::Locked, "The document is open for editing" + who,
                        "the lock owner still has the lock file open");
        }
        return fail(ErrorCode::IoError, "Cannot break the lock", winError("CreateFileW", e));
    }
    LockFileView view;
    view.ownerDead = true; // this handle has DELETE access, so no owner handle is left
    auto bytes = readAll(h);
    FILETIME written{};
    if (GetFileTime(h, nullptr, nullptr, &written)) {
        view.mtimeMs = fileTimeToUnixMs(written);
    }
    if (!bytes) {
        CloseHandle(h);
        return tl::unexpected<Error>(bytes.error());
    }
    view.bytes = std::move(*bytes);
    if (auto verdict = mayBreak(view); !verdict) {
        CloseHandle(h);
        return verdict;
    }
    // Rename through the handle: exactly the file judged above moves, never a lock file created since.
    const std::wstring target = widen(path + ".broken-" + randomName());
    std::vector<std::uint8_t> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, target.data(), target.size() * sizeof(wchar_t));
    if (!SetFileInformationByHandle(h, FileRenameInfo, info, static_cast<DWORD>(buffer.size()))) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        return fail(ErrorCode::IoError, "Cannot break the lock", winError("rename", e));
    }
    markForDelete(h);
    CloseHandle(h);
    return {};
}

Status renameNoReplace(const std::string& from, const std::string& to)
{
    if (!MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) {
            return fail(ErrorCode::InvalidArgument, "A file with this name already exists", to);
        }
        return fail(ErrorCode::IoError, "Cannot create the document file", winError("MoveFileExW", e));
    }
    return {};
}

std::string userName()
{
    wchar_t buffer[257];
    DWORD size = 257;
    if (!GetUserNameW(buffer, &size) || size == 0) {
        return "?";
    }
    return narrow(buffer, size - 1); // size counts the terminating null
}

std::string hostName()
{
    wchar_t buffer[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buffer, &size) && size > 0) {
        return narrow(buffer, size);
    }
    size = 256;
    if (GetComputerNameW(buffer, &size) && size > 0) {
        return narrow(buffer, size);
    }
    return "?";
}

std::int64_t processId() noexcept
{
    return static_cast<std::int64_t>(GetCurrentProcessId());
}

} // namespace os
} // namespace ray::store::detail
#endif
