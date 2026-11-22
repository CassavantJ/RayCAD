// File primitives of the library folders (see fileio.h): Windows and POSIX.
#include "fileio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ray::lib::detail::fio {
namespace {

std::atomic<void (*)()> gBeforeRename{nullptr};

tl::unexpected<Error> fail(ErrorCode code, std::string message, std::string context)
{
    return tl::unexpected<Error>(Error{code, std::move(message), {std::move(context)}});
}

std::string temporaryName(std::string_view name)
{
    const std::string random = Uuid::generate().toString();
    return "." + std::string(name) + "." + random.substr(0, 8) + random.substr(9, 4) + ".tmp";
}

std::pair<std::string, std::string> splitPath(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return {".", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

#ifdef _WIN32
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

std::string narrow(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    }
    return out;
}

std::string winError(const char* what, DWORD code)
{
    return std::string(what) + ": Windows error " + std::to_string(code);
}

Stamp stampOf(DWORD sizeHigh, DWORD sizeLow, const FILETIME& written)
{
    Stamp s;
    s.size = (static_cast<std::uint64_t>(sizeHigh) << 32) | sizeLow;
    s.mtime =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(written.dwHighDateTime) << 32) | written.dwLowDateTime);
    return s;
}
#else
std::string posixError(const char* what, int code)
{
    return std::string(what) + ": " + std::strerror(code);
}

void syncFolder(const std::string& dir)
{
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}
#endif

} // namespace

std::filesystem::path toFsPath(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string fromFsPath(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string join(const std::string& dir, std::string_view name)
{
    return fromFsPath(toFsPath(dir) / toFsPath(name));
}

bool isFolder(const std::string& path) noexcept
{
    try {
        std::error_code ec;
        return std::filesystem::is_directory(toFsPath(path), ec);
    } catch (...) {
        return false;
    }
}

bool isTemporaryName(std::string_view name) noexcept
{
    return name.size() > 5 && name.front() == '.' && name.substr(name.size() - 4) == ".tmp";
}

void setBeforeRenameHook(void (*hook)())
{
    gBeforeRename.store(hook);
}

#ifdef _WIN32

Result<std::vector<DirEntry>> listFolder(const std::string& dir)
{
    std::vector<DirEntry> out;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = widen(dir) + L"\\*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            return out; // an empty folder (no "." entry on some file systems)
        }
        if (e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY) {
            return fail(ErrorCode::NotFound, "The library folder is missing", dir);
        }
        return fail(ErrorCode::IoError, "Cannot list a library folder", winError("FindFirstFileExW", e) + ": " + dir);
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            out.push_back(
                {narrow(data.cFileName), stampOf(data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime)});
        }
    } while (FindNextFileW(h, &data));
    const DWORD e = GetLastError();
    FindClose(h);
    if (e != ERROR_NO_MORE_FILES) {
        return fail(ErrorCode::IoError, "Cannot list a library folder", winError("FindNextFileW", e) + ": " + dir);
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return out;
}

Result<std::string> readFile(const std::string& path)
{
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return fail(ErrorCode::NotFound, "The file is missing", path);
        }
        return fail(ErrorCode::IoError, "Cannot read a library file", winError("CreateFileW", e) + ": " + path);
    }
    std::string bytes;
    char buffer[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buffer, sizeof buffer, &got, nullptr)) {
            const DWORD e = GetLastError();
            CloseHandle(h);
            return fail(ErrorCode::IoError, "Cannot read a library file", winError("ReadFile", e) + ": " + path);
        }
        if (got == 0) {
            break;
        }
        bytes.append(buffer, got);
        if (bytes.size() > 64u * 1024 * 1024) {
            CloseHandle(h);
            return fail(ErrorCode::FormatError, "The file is too large for a library item (over 64 MiB)", path);
        }
    }
    CloseHandle(h);
    return bytes;
}

Status replaceAtomically(const std::string& path, std::string_view bytes)
{
    const auto [dir, name] = splitPath(path);
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(dir), ec);
    const std::string temp = join(dir, temporaryName(name));
    const std::wstring wideTemp = widen(temp);
    HANDLE h = CreateFileW(wideTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return fail(ErrorCode::IoError, "Cannot write to the library folder",
                    winError("CreateFileW", GetLastError()) + ": " + temp);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t left = bytes.size() - done;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
            const DWORD e = GetLastError();
            CloseHandle(h);
            DeleteFileW(wideTemp.c_str());
            return fail(ErrorCode::IoError, "Cannot write to the library folder",
                        winError("WriteFile", e) + ": " + temp);
        }
        done += wrote;
    }
    if (!FlushFileBuffers(h)) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        DeleteFileW(wideTemp.c_str());
        return fail(ErrorCode::IoError, "Cannot write to the library folder",
                    winError("FlushFileBuffers", e) + ": " + temp);
    }
    CloseHandle(h);
    if (auto hook = gBeforeRename.load()) {
        hook();
    }
    const std::wstring wideTarget = widen(path);
    DWORD e = 0;
    // Another process reading the file without FILE_SHARE_DELETE (an editor, a virus scanner) makes the rename fail
    // with a sharing or access error for as long as it holds it: retry for up to about two seconds.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (MoveFileExW(wideTemp.c_str(), wideTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return {};
        }
        e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    DeleteFileW(wideTemp.c_str());
    if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) {
        return fail(ErrorCode::Locked, "Another program has the library file open",
                    winError("MoveFileExW", e) + ": " + path);
    }
    return fail(ErrorCode::IoError, "Cannot replace a library file", winError("MoveFileExW", e) + ": " + path);
}

Status removeFile(const std::string& path)
{
    if (DeleteFileW(widen(path).c_str())) {
        return {};
    }
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
        return fail(ErrorCode::NotFound, "The file is missing", path);
    }
    if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) {
        return fail(ErrorCode::Locked, "Another program has the library file open",
                    winError("DeleteFileW", e) + ": " + path);
    }
    return fail(ErrorCode::IoError, "Cannot delete a library file", winError("DeleteFileW", e) + ": " + path);
}

#else

Result<std::vector<DirEntry>> listFolder(const std::string& dir)
{
    std::vector<DirEntry> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        const int e = errno;
        if (e == ENOENT || e == ENOTDIR) {
            return fail(ErrorCode::NotFound, "The library folder is missing", dir);
        }
        return fail(ErrorCode::IoError, "Cannot list a library folder", posixError("opendir", e) + ": " + dir);
    }
    while (const dirent* entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        struct stat st{};
        if (::stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue; // vanished meanwhile, or not a regular file
        }
        Stamp s;
        s.size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
        s.mtime = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
        s.mtime = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
        out.push_back({name, s});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return out;
}

Result<std::string> readFile(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT || e == ENOTDIR) {
            return fail(ErrorCode::NotFound, "The file is missing", path);
        }
        return fail(ErrorCode::IoError, "Cannot read a library file", posixError("open", e) + ": " + path);
    }
    std::string bytes;
    char buffer[65536];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int e = errno;
            close(fd);
            return fail(ErrorCode::IoError, "Cannot read a library file", posixError("read", e) + ": " + path);
        }
        if (got == 0) {
            break;
        }
        bytes.append(buffer, static_cast<std::size_t>(got));
        if (bytes.size() > 64u * 1024 * 1024) {
            close(fd);
            return fail(ErrorCode::FormatError, "The file is too large for a library item (over 64 MiB)", path);
        }
    }
    close(fd);
    return bytes;
}

Status replaceAtomically(const std::string& path, std::string_view bytes)
{
    const auto [dir, name] = splitPath(path);
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(dir), ec);
    const std::string temp = dir + "/" + temporaryName(name);
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        return fail(ErrorCode::IoError, "Cannot write to the library folder", posixError("open", errno) + ": " + temp);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t wrote = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            const int e = errno;
            close(fd);
            unlink(temp.c_str());
            return fail(ErrorCode::IoError, "Cannot write to the library folder", posixError("write", e) + ": " + temp);
        }
        done += static_cast<std::size_t>(wrote);
    }
    if (fsync(fd) != 0) {
        const int e = errno;
        close(fd);
        unlink(temp.c_str());
        return fail(ErrorCode::IoError, "Cannot write to the library folder", posixError("fsync", e) + ": " + temp);
    }
    close(fd);
    if (auto hook = gBeforeRename.load()) {
        hook();
    }
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        const int e = errno;
        unlink(temp.c_str());
        return fail(ErrorCode::IoError, "Cannot replace a library file", posixError("rename", e) + ": " + path);
    }
    syncFolder(dir);
    return {};
}

Status removeFile(const std::string& path)
{
    if (::unlink(path.c_str()) == 0) {
        syncFolder(splitPath(path).first);
        return {};
    }
    const int e = errno;
    if (e == ENOENT) {
        return fail(ErrorCode::NotFound, "The file is missing", path);
    }
    return fail(ErrorCode::IoError, "Cannot delete a library file", posixError("unlink", e) + ": " + path);
}

#endif

void removeStaleTemporaries(const std::string& dir) noexcept
{
    try {
        std::error_code ec;
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto& entry : std::filesystem::directory_iterator(toFsPath(dir), ec)) {
            const std::string name = fromFsPath(entry.path().filename());
            if (!isTemporaryName(name)) {
                continue;
            }
            std::error_code timeError;
            const auto written = entry.last_write_time(timeError);
            if (!timeError && now - written > std::chrono::hours(1)) {
                std::error_code removeError;
                std::filesystem::remove(entry.path(), removeError);
            }
        }
    } catch (...) {
    }
}

} // namespace ray::lib::detail::fio
