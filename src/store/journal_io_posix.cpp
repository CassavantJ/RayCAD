// Recovery journal and migration file primitives, POSIX implementation (Linux, macOS). The Windows one is
// journal_io_win.cpp.
//
// The journal is held with flock(LOCK_EX): flock locks belong to the open file description, so a second open of the
// same file (in another process, or in this one) cannot take the lock while the session's descriptor is open, and
// the kernel drops it when the process dies.
#include "journal_io.h"

#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ray::store::detail::jio {
namespace {

std::string posixError(const char* what, int code)
{
    return std::string(what) + ": " + std::strerror(code);
}

tl::unexpected<Error> ioFail(const char* what)
{
    return fail(ErrorCode::IoError, "Cannot write the recovery data", posixError(what, errno));
}

int syncDescriptor(int fd)
{
#if defined(__linux__)
    return fdatasync(fd);
#else
    // macOS: fsync reaches the drive's cache, as SQLite's default does for the document; F_FULLFSYNC would cost
    // several milliseconds per append.
    return fsync(fd);
#endif
}

} // namespace

struct ExclusiveFile::Impl {
    int fd = -1;
    std::string path;
};

ExclusiveFile::ExclusiveFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ExclusiveFile::~ExclusiveFile()
{
    if (impl_ && impl_->fd >= 0) {
        close(impl_->fd);
    }
}

Result<std::unique_ptr<ExclusiveFile>> ExclusiveFile::open(const std::string& path, bool create)
{
    const int flags = O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0);
    const int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT) {
            return fail(ErrorCode::NotFound, "There is no recovery data", path);
        }
        return fail(ErrorCode::IoError, "Cannot open the recovery data", posixError("open", e));
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        close(fd);
        if (e == EWOULDBLOCK || e == EAGAIN) {
            return fail(ErrorCode::Locked, "The recovery data is in use", path);
        }
        return fail(ErrorCode::IoError, "Cannot lock the recovery data", posixError("flock", e));
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    impl->path = path;
    return std::unique_ptr<ExclusiveFile>(new ExclusiveFile(std::move(impl)));
}

Result<std::string> ExclusiveFile::readAll()
{
    struct stat st{};
    if (fstat(impl_->fd, &st) != 0) {
        return fail(ErrorCode::IoError, "Cannot read the recovery data", posixError("fstat", errno));
    }
    std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const ssize_t got = pread(impl_->fd, bytes.data() + pos, bytes.size() - pos, static_cast<off_t>(pos));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(ErrorCode::IoError, "Cannot read the recovery data", posixError("pread", errno));
        }
        if (got == 0) {
            bytes.resize(pos);
            break;
        }
        pos += static_cast<std::size_t>(got);
    }
    return bytes;
}

Status ExclusiveFile::writeAt(std::uint64_t offset, std::string_view bytes)
{
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t wrote =
            pwrite(impl_->fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ioFail("pwrite");
        }
        done += static_cast<std::size_t>(wrote);
    }
    return {};
}

Status ExclusiveFile::truncate(std::uint64_t size)
{
    if (ftruncate(impl_->fd, static_cast<off_t>(size)) != 0) {
        return ioFail("ftruncate");
    }
    return {};
}

Status ExclusiveFile::flush()
{
    if (syncDescriptor(impl_->fd) != 0) {
        return ioFail("fsync");
    }
    return {};
}

Status ExclusiveFile::removeAndClose()
{
    const int rc = unlink(impl_->path.c_str());
    const int e = errno;
    close(impl_->fd);
    impl_->fd = -1;
    if (rc != 0 && e != ENOENT) {
        return fail(ErrorCode::IoError, "Cannot delete the recovery data", posixError("unlink", e));
    }
    return syncFolderOf(impl_->path);
}

Status replaceFile(const std::string& from, const std::string& to)
{
    if (rename(from.c_str(), to.c_str()) != 0) {
        return fail(ErrorCode::IoError, "Cannot replace a file", posixError("rename", errno));
    }
    return syncFolderOf(to);
}

Status syncFile(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail(ErrorCode::IoError, "Cannot write a file to the disk", posixError("open", errno));
    }
    const int rc = fsync(fd);
    const int e = errno;
    close(fd);
    if (rc != 0) {
        return fail(ErrorCode::IoError, "Cannot write a file to the disk", posixError("fsync", e));
    }
    return {};
}

Status syncFolderOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string folder = slash == std::string::npos ? std::string(".") : path.substr(0, slash == 0 ? 1 : slash);
    const int fd = ::open(folder.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {}; // best effort: some file systems do not open folders
    }
    (void)fsync(fd);
    close(fd);
    return {};
}

std::int64_t modifiedMs(const std::string& path) noexcept
{
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1'000'000;
#else
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1'000'000;
#endif
}

std::string defaultRecoveryDir()
{
    const char* home = std::getenv("HOME");
    const std::string homeDir = home && *home ? home : "/tmp";
#if defined(__APPLE__)
    return homeDir + "/Library/Application Support/RayCAD/recovery";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg == '/') {
        return std::string(xdg) + "/raycad/recovery";
    }
    return homeDir + "/.local/share/raycad/recovery";
#endif
}

} // namespace ray::store::detail::jio
#endif
