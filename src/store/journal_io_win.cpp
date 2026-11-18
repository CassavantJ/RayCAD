// Recovery journal and migration file primitives, Windows implementation. The POSIX one is journal_io_posix.cpp.
//
// The journal is opened with share mode FILE_SHARE_READ only: anyone else asking for write or delete access gets a
// sharing violation for as long as the handle exists, so a journal in use is never read as a crashed one, never
// deleted and never written by two sessions.
#include "journal_io.h"

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#include <system_error>
#include <vector>

namespace ray::store::detail::jio {
namespace {

std::wstring widen(std::string_view utf8)
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

std::string winError(const char* what, DWORD code)
{
    return std::string(what) + ": Windows error " + std::to_string(code);
}

tl::unexpected<Error> ioFail(const char* what)
{
    return fail(ErrorCode::IoError, "Cannot write the recovery data", winError(what, GetLastError()));
}

OVERLAPPED at(std::uint64_t offset)
{
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(offset & 0xffffffffu);
    o.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return o;
}

} // namespace

struct ExclusiveFile::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
};

ExclusiveFile::ExclusiveFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ExclusiveFile::~ExclusiveFile()
{
    if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
    }
}

Result<std::unique_ptr<ExclusiveFile>> ExclusiveFile::open(const std::string& path, bool create)
{
    const std::wstring wide = widen(path);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
                           create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED) {
            // Access denied is also what a file that is being deleted reports.
            return fail(ErrorCode::Locked, "The recovery data is in use", winError("CreateFileW", e));
        }
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return fail(ErrorCode::NotFound, "There is no recovery data", path);
        }
        return fail(ErrorCode::IoError, "Cannot open the recovery data", winError("CreateFileW", e));
    }
    auto impl = std::make_unique<Impl>();
    impl->handle = h;
    return std::unique_ptr<ExclusiveFile>(new ExclusiveFile(std::move(impl)));
}

Result<std::string> ExclusiveFile::readAll()
{
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(impl_->handle, &size)) {
        return fail(ErrorCode::IoError, "Cannot read the recovery data", winError("GetFileSizeEx", GetLastError()));
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::uint64_t pos = 0;
    while (pos < bytes.size()) {
        const std::uint64_t left = bytes.size() - pos;
        const DWORD want = static_cast<DWORD>(left < (1u << 30) ? left : (1u << 30));
        DWORD got = 0;
        OVERLAPPED o = at(pos);
        if (!ReadFile(impl_->handle, bytes.data() + pos, want, &got, &o) || got == 0) {
            const DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF || got == 0) {
                bytes.resize(static_cast<std::size_t>(pos));
                break;
            }
            return fail(ErrorCode::IoError, "Cannot read the recovery data", winError("ReadFile", e));
        }
        pos += got;
    }
    return bytes;
}

Status ExclusiveFile::writeAt(std::uint64_t offset, std::string_view bytes)
{
    std::uint64_t done = 0;
    while (done < bytes.size()) {
        const std::uint64_t left = bytes.size() - done;
        const DWORD want = static_cast<DWORD>(left < (1u << 30) ? left : (1u << 30));
        DWORD wrote = 0;
        OVERLAPPED o = at(offset + done);
        if (!WriteFile(impl_->handle, bytes.data() + done, want, &wrote, &o) || wrote == 0) {
            return ioFail("WriteFile");
        }
        done += wrote;
    }
    return {};
}

Status ExclusiveFile::truncate(std::uint64_t size)
{
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(impl_->handle, FileEndOfFileInfo, &eof, sizeof eof)) {
        return ioFail("SetFileInformationByHandle(EndOfFile)");
    }
    return {};
}

Status ExclusiveFile::flush()
{
    if (!FlushFileBuffers(impl_->handle)) {
        return ioFail("FlushFileBuffers");
    }
    return {};
}

Status ExclusiveFile::removeAndClose()
{
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    const BOOL ok = SetFileInformationByHandle(impl_->handle, FileDispositionInfo, &disposition, sizeof disposition);
    const DWORD e = ok ? 0 : GetLastError();
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
    if (!ok) {
        return fail(ErrorCode::IoError, "Cannot delete the recovery data", winError("SetFileInformationByHandle", e));
    }
    return {};
}

Status replaceFile(const std::string& from, const std::string& to)
{
    if (!MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail(ErrorCode::IoError, "Cannot replace a file", winError("MoveFileExW", GetLastError()));
    }
    return {};
}

Status syncFile(const std::string& path)
{
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return fail(ErrorCode::IoError, "Cannot write a file to the disk", winError("CreateFileW", GetLastError()));
    }
    const BOOL ok = FlushFileBuffers(h);
    const DWORD e = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        return fail(ErrorCode::IoError, "Cannot write a file to the disk", winError("FlushFileBuffers", e));
    }
    return {};
}

Status syncFolderOf(const std::string&)
{
    return {}; // NTFS journals directory changes; MOVEFILE_WRITE_THROUGH covers renames.
}

std::int64_t modifiedMs(const std::string& path) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10000) - 11'644'473'600'000LL;
}

std::string defaultRecoveryDir()
{
    std::filesystem::path base;
    for (const wchar_t* name : {L"LOCALAPPDATA", L"USERPROFILE"}) {
        std::vector<wchar_t> buffer(32768);
        const DWORD n = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n > 0 && n < buffer.size()) {
            base = std::filesystem::path(std::wstring(buffer.data(), n));
            if (std::wstring(name) == L"USERPROFILE") {
                base = base / L"AppData" / L"Local";
            }
            break;
        }
    }
    if (base.empty()) {
        std::error_code ec;
        base = std::filesystem::temp_directory_path(ec);
    }
    return fromFsPath(base / L"RayCAD" / L"recovery");
}

} // namespace ray::store::detail::jio
#endif
