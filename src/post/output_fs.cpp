// The real file system of program output (output.h IOutputFs): Win32 on Windows (local disks and SMB shares by UNC
// or mapped drive), POSIX elsewhere (local disks and mounted shares). Every error names what failed and maps the
// system's code to an OutputProblem, so the user is told "the share cannot be reached" rather than a number.
#include "output.h"

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ray::post {

namespace {

// A target held open by another program (a DNC sender, an editor, a virus scanner) can block the rename for a
// moment: retried this many times, this far apart, before it is reported as locked.
constexpr int kLockRetries = 8;
constexpr std::chrono::milliseconds kLockRetryWait{60};

std::string folderOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string nameOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
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

std::string winDetail(const char* what, DWORD code, const std::string& path)
{
    return std::string(what) + ": Windows error " + std::to_string(code) + ": " + path;
}

bool networkError(DWORD e)
{
    switch (e) {
    case ERROR_BAD_NETPATH:           // 53
    case ERROR_NETWORK_BUSY:          // 54
    case ERROR_DEV_NOT_EXIST:         // 55
    case ERROR_UNEXP_NET_ERR:         // 59
    case ERROR_BAD_NET_RESP:          // 58
    case ERROR_NETNAME_DELETED:       // 64
    case ERROR_NETWORK_ACCESS_DENIED: // 65
    case ERROR_BAD_NET_NAME:          // 67
    case ERROR_SEM_TIMEOUT:           // 121
    case ERROR_NO_NET_OR_BAD_PATH:    // 1203
    case ERROR_NOT_CONNECTED:         // 2250
    case ERROR_CONNECTION_ABORTED:    // 1236
    case ERROR_NETWORK_UNREACHABLE:   // 1231
    case ERROR_HOST_UNREACHABLE:      // 1232
    case ERROR_CONNECTION_REFUSED:    // 1225
    case ERROR_NOT_READY:             // 21: a removable drive or a disconnected mapped drive
    case ERROR_DEVICE_NOT_CONNECTED:  // 1167
        return true;
    default:
        return false;
    }
}

bool diskFull(DWORD e)
{
    return e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL || e == ERROR_DISK_QUOTA_EXCEEDED;
}

bool lockedError(DWORD e)
{
    return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ||
           e == ERROR_USER_MAPPED_FILE;
}

// The common mapping of a failed call on `path` (what the user is told, by the kind of failure).
Error mapped(DWORD e, const char* what, const std::string& path, const std::string& doing)
{
    const std::string detail = winDetail(what, e, path);
    if (networkError(e)) {
        return outputError(ErrorCode::IoError, OutputProblem::Unreachable,
                           "The folder " + folderOf(path) + " cannot be reached (" + doing +
                               "): the network share is not available or the connection was lost",
                           detail);
    }
    if (diskFull(e)) {
        return outputError(ErrorCode::IoError, OutputProblem::DiskFull,
                           "The disk of " + folderOf(path) + " is full (" + doing + ")", detail);
    }
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME || e == ERROR_DIRECTORY) {
        return outputError(ErrorCode::NotFound, OutputProblem::FolderMissing,
                           "The folder " + folderOf(path) + " does not exist (" + doing + ")", detail);
    }
    if (e == ERROR_ACCESS_DENIED || e == ERROR_WRITE_PROTECT) {
        return outputError(ErrorCode::IoError, OutputProblem::Denied,
                           "No permission to write in " + folderOf(path) + " (" + doing + ")", detail);
    }
    if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) {
        return outputError(ErrorCode::Locked, OutputProblem::Exists, nameOf(path) + " already exists", detail);
    }
    return outputError(ErrorCode::IoError, OutputProblem::Other,
                       "Could not write " + nameOf(path) + " in " + folderOf(path) + " (" + doing + ")", detail);
}

class WinFile final : public IOutputFile {
public:
    WinFile(HANDLE h, std::string path) : h_(h), path_(std::move(path)) {}
    ~WinFile() override { (void)close(); }
    Status write(std::string_view bytes) override
    {
        while (!bytes.empty()) {
            const DWORD n = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::size_t(1) << 24));
            DWORD done = 0;
            if (!WriteFile(h_, bytes.data(), n, &done, nullptr)) {
                return tl::unexpected(mapped(GetLastError(), "WriteFile", path_, "writing the program"));
            }
            if (done == 0) {
                return tl::unexpected(mapped(ERROR_DISK_FULL, "WriteFile", path_, "writing the program"));
            }
            bytes.remove_prefix(done);
        }
        return std::monostate{};
    }
    Status flush() override
    {
        if (!FlushFileBuffers(h_)) {
            return tl::unexpected(mapped(GetLastError(), "FlushFileBuffers", path_, "flushing the program to disk"));
        }
        return std::monostate{};
    }
    Status close() override
    {
        if (h_ == INVALID_HANDLE_VALUE) {
            return std::monostate{};
        }
        const BOOL ok = CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        if (!ok) {
            return tl::unexpected(mapped(GetLastError(), "CloseHandle", path_, "closing the program"));
        }
        return std::monostate{};
    }

private:
    HANDLE h_;
    std::string path_;
};

class WinFs final : public IOutputFs {
public:
    Status checkFolder(const std::string& dir) override
    {
        if (dir.empty()) {
            return tl::unexpected(
                outputError(ErrorCode::InvalidArgument, OutputProblem::FolderMissing, "No output folder is set"));
        }
        const DWORD a = GetFileAttributesW(widen(dir).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            const DWORD e = GetLastError();
            if (networkError(e)) {
                return tl::unexpected(
                    outputError(ErrorCode::IoError, OutputProblem::Unreachable,
                                "The folder " + dir + " cannot be reached: the network share is not available",
                                winDetail("GetFileAttributesW", e, dir)));
            }
            if (e == ERROR_ACCESS_DENIED) {
                return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Denied,
                                                  "No permission to open the folder " + dir,
                                                  winDetail("GetFileAttributesW", e, dir)));
            }
            return tl::unexpected(outputError(ErrorCode::NotFound, OutputProblem::FolderMissing,
                                              "The folder " + dir + " does not exist",
                                              winDetail("GetFileAttributesW", e, dir)));
        }
        if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return tl::unexpected(
                outputError(ErrorCode::NotFound, OutputProblem::FolderMissing, dir + " is a file, not a folder"));
        }
        return std::monostate{};
    }
    Result<bool> exists(const std::string& path) override
    {
        const DWORD a = GetFileAttributesW(widen(path).c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return tl::unexpected(mapped(e, "GetFileAttributesW", path, "looking for the file"));
    }
    Result<std::unique_ptr<IOutputFile>> createNew(const std::string& path) override
    {
        HANDLE h =
            CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return tl::unexpected(mapped(GetLastError(), "CreateFileW", path, "making the temporary file"));
        }
        return std::unique_ptr<IOutputFile>(std::make_unique<WinFile>(h, path));
    }
    Status replace(const std::string& from, const std::string& to) override
    {
        const std::wstring f = widen(from), t = widen(to);
        DWORD e = 0;
        for (int i = 0; i < kLockRetries; ++i) {
            if (MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return std::monostate{};
            }
            e = GetLastError();
            if (!lockedError(e)) {
                break;
            }
            std::this_thread::sleep_for(kLockRetryWait);
        }
        if (lockedError(e)) {
            return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Locked,
                                              nameOf(to) + " in " + folderOf(to) +
                                                  " could not be replaced: it is open in another program (perhaps "
                                                  "being sent to the machine) or read-only. The old file is "
                                                  "unchanged; close it and post again",
                                              winDetail("MoveFileExW", e, to)));
        }
        return tl::unexpected(mapped(e, "MoveFileExW", to, "putting the program in place"));
    }
    Status moveNoReplace(const std::string& from, const std::string& to) override
    {
        if (MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_WRITE_THROUGH)) {
            return std::monostate{};
        }
        return tl::unexpected(mapped(GetLastError(), "MoveFileExW", to, "putting the program in place"));
    }
    Status remove(const std::string& path) override
    {
        if (DeleteFileW(widen(path).c_str())) {
            return std::monostate{};
        }
        return tl::unexpected(mapped(GetLastError(), "DeleteFileW", path, "removing the temporary file"));
    }
};

#else

std::string posixDetail(const char* what, int code, const std::string& path)
{
    return std::string(what) + ": " + std::strerror(code) + ": " + path;
}

bool networkErrno(int e)
{
    return e == EIO || e == ENOTCONN || e == ETIMEDOUT || e == EHOSTDOWN || e == EHOSTUNREACH || e == ENETDOWN ||
           e == ENETUNREACH || e == ECONNABORTED || e == ECONNRESET || e == ESTALE;
}

Error mapped(int e, const char* what, const std::string& path, const std::string& doing)
{
    const std::string detail = posixDetail(what, e, path);
    if (networkErrno(e)) {
        return outputError(ErrorCode::IoError, OutputProblem::Unreachable,
                           "The folder " + folderOf(path) + " cannot be reached (" + doing +
                               "): the network share is not available or the connection was lost",
                           detail);
    }
    if (e == ENOSPC || e == EDQUOT) {
        return outputError(ErrorCode::IoError, OutputProblem::DiskFull,
                           "The disk of " + folderOf(path) + " is full (" + doing + ")", detail);
    }
    if (e == ENOENT || e == ENOTDIR) {
        return outputError(ErrorCode::NotFound, OutputProblem::FolderMissing,
                           "The folder " + folderOf(path) + " does not exist (" + doing + ")", detail);
    }
    if (e == EACCES || e == EPERM || e == EROFS) {
        return outputError(ErrorCode::IoError, OutputProblem::Denied,
                           "No permission to write in " + folderOf(path) + " (" + doing + ")", detail);
    }
    if (e == EEXIST) {
        return outputError(ErrorCode::Locked, OutputProblem::Exists, nameOf(path) + " already exists", detail);
    }
    if (e == EBUSY || e == ETXTBSY) {
        return outputError(ErrorCode::Locked, OutputProblem::Locked,
                           nameOf(path) + " in " + folderOf(path) +
                               " could not be replaced: it is in use. The old file is unchanged",
                           detail);
    }
    return outputError(ErrorCode::IoError, OutputProblem::Other,
                       "Could not write " + nameOf(path) + " in " + folderOf(path) + " (" + doing + ")", detail);
}

void syncFolder(const std::string& dir)
{
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        ::close(fd);
    }
}

class PosixFile final : public IOutputFile {
public:
    PosixFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~PosixFile() override { (void)close(); }
    Status write(std::string_view bytes) override
    {
        while (!bytes.empty()) {
            const ssize_t n = ::write(fd_, bytes.data(), bytes.size());
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return tl::unexpected(mapped(errno, "write", path_, "writing the program"));
            }
            if (n == 0) {
                return tl::unexpected(mapped(ENOSPC, "write", path_, "writing the program"));
            }
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
        return std::monostate{};
    }
    Status flush() override
    {
        if (fsync(fd_) != 0) {
            return tl::unexpected(mapped(errno, "fsync", path_, "flushing the program to disk"));
        }
        return std::monostate{};
    }
    Status close() override
    {
        if (fd_ < 0) {
            return std::monostate{};
        }
        const int r = ::close(fd_);
        fd_ = -1;
        if (r != 0 && errno != EINTR) {
            return tl::unexpected(mapped(errno, "close", path_, "closing the program"));
        }
        return std::monostate{};
    }

private:
    int fd_;
    std::string path_;
};

class PosixFs final : public IOutputFs {
public:
    Status checkFolder(const std::string& dir) override
    {
        if (dir.empty()) {
            return tl::unexpected(
                outputError(ErrorCode::InvalidArgument, OutputProblem::FolderMissing, "No output folder is set"));
        }
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0) {
            const int e = errno;
            if (networkErrno(e)) {
                return tl::unexpected(
                    outputError(ErrorCode::IoError, OutputProblem::Unreachable,
                                "The folder " + dir + " cannot be reached: the network share is not available",
                                posixDetail("stat", e, dir)));
            }
            if (e == EACCES) {
                return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Denied,
                                                  "No permission to open the folder " + dir,
                                                  posixDetail("stat", e, dir)));
            }
            return tl::unexpected(outputError(ErrorCode::NotFound, OutputProblem::FolderMissing,
                                              "The folder " + dir + " does not exist", posixDetail("stat", e, dir)));
        }
        if (!S_ISDIR(st.st_mode)) {
            return tl::unexpected(
                outputError(ErrorCode::NotFound, OutputProblem::FolderMissing, dir + " is a file, not a folder"));
        }
        return std::monostate{};
    }
    Result<bool> exists(const std::string& path) override
    {
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0) {
            return true;
        }
        if (errno == ENOENT) {
            return false;
        }
        return tl::unexpected(mapped(errno, "lstat", path, "looking for the file"));
    }
    Result<std::unique_ptr<IOutputFile>> createNew(const std::string& path) override
    {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            return tl::unexpected(mapped(errno, "open", path, "making the temporary file"));
        }
        return std::unique_ptr<IOutputFile>(std::make_unique<PosixFile>(fd, path));
    }
    Status replace(const std::string& from, const std::string& to) override
    {
        int e = 0;
        for (int i = 0; i < kLockRetries; ++i) {
            if (::rename(from.c_str(), to.c_str()) == 0) {
                syncFolder(folderOf(to));
                return std::monostate{};
            }
            e = errno;
            if (e != EBUSY && e != ETXTBSY) {
                break;
            }
            std::this_thread::sleep_for(kLockRetryWait);
        }
        return tl::unexpected(mapped(e, "rename", to, "putting the program in place"));
    }
    Status moveNoReplace(const std::string& from, const std::string& to) override
    {
        if (::link(from.c_str(), to.c_str()) == 0) {
            (void)::unlink(from.c_str());
            syncFolder(folderOf(to));
            return std::monostate{};
        }
        const int e = errno;
        if (e == EEXIST) {
            return tl::unexpected(mapped(e, "link", to, "putting the program in place"));
        }
        // File systems without hard links (SMB without Unix extensions, FAT, some NAS mounts): check, then rename.
        // The name could be taken between the two; nothing better exists there.
        if (e == EPERM || e == ENOTSUP || e == EOPNOTSUPP || e == EMLINK || e == ENOSYS || e == EXDEV || e == EACCES) {
            auto there = exists(to);
            if (!there) {
                return tl::unexpected(there.error());
            }
            if (*there) {
                return tl::unexpected(mapped(EEXIST, "lstat", to, "putting the program in place"));
            }
            if (::rename(from.c_str(), to.c_str()) == 0) {
                syncFolder(folderOf(to));
                return std::monostate{};
            }
            return tl::unexpected(mapped(errno, "rename", to, "putting the program in place"));
        }
        return tl::unexpected(mapped(e, "link", to, "putting the program in place"));
    }
    Status remove(const std::string& path) override
    {
        if (::unlink(path.c_str()) == 0) {
            return std::monostate{};
        }
        return tl::unexpected(mapped(errno, "unlink", path, "removing the temporary file"));
    }
};
#endif

} // namespace

IOutputFs& localOutputFs()
{
#ifdef _WIN32
    static WinFs fs;
#else
    static PosixFs fs;
#endif
    return fs;
}

} // namespace ray::post
