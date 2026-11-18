// Lock file, POSIX implementation (Linux, macOS) (PLAN.md 4.3). The Windows one is lock_win.cpp.
//
// The owner creates the lock file with O_CREAT | O_EXCL and holds flock(LOCK_EX) on it for the session. flock
// belongs to the open file description, so another process (or another open() in the same process) that gets
// LOCK_SH | LOCK_NB knows the owner's descriptor is closed: the owner is provably dead. The owner writes the
// content only after taking the flock, so a parseable file whose flock is free has a dead owner.
// POSIX cannot stop a rename of an open file, so breaking renames, then checks that the renamed file is the
// very one it judged (same device and inode) and puts it back if it is not.
#include "lock.h"

#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ray::store::detail {

class OwnedLock::Held {
public:
    int fd = -1;
    std::string path;
    dev_t dev = 0;
    ino_t ino = 0;
};

void OwnedLock::HeldDeleter::operator()(Held* held) const noexcept
{
    if (held->fd >= 0) {
        close(held->fd);
    }
    delete held;
}

namespace os {
namespace {

std::int64_t mtimeMs(const struct stat& st) noexcept
{
#if defined(__APPLE__)
    return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1'000'000;
#else
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1'000'000;
#endif
}

std::string posixError(const char* what, int code)
{
    return std::string(what) + ": " + std::strerror(code);
}

Result<std::string> readAll(int fd)
{
    std::string bytes(4096, '\0');
    ssize_t got;
    do {
        got = pread(fd, bytes.data(), bytes.size(), 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return fail(ErrorCode::IoError, "Cannot read the lock file", posixError("pread", errno));
    }
    bytes.resize(static_cast<std::size_t>(got));
    return bytes;
}

// True if no other open file description holds the flock: the owner's descriptor is closed.
bool flockFree(int fd) noexcept
{
    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        flock(fd, LOCK_UN);
        return true;
    }
    return false;
}

} // namespace

Result<CreateOutcome> createLockFile(const std::string& path, OwnedLock::HeldPtr& out)
{
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return CreateOutcome::Exists;
        }
        return fail(ErrorCode::IoError, "Cannot create the lock file beside the document", posixError("open", errno));
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        // Some network file systems have no flock; the lock still works, only "owner provably dead" is lost.
        ray::log(LogLevel::Warn, "store", "flock on the lock file failed: " + std::string(std::strerror(errno)));
    }
    struct stat st{};
    fstat(fd, &st);
    out.reset(new OwnedLock::Held);
    out->fd = fd;
    out->path = path;
    out->dev = st.st_dev;
    out->ino = st.st_ino;
    return CreateOutcome::Created;
}

Status writeHeld(OwnedLock::Held& held, std::string_view bytes)
{
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = pwrite(held.fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(ErrorCode::IoError, "Cannot write the lock file", posixError("pwrite", errno));
        }
        done += static_cast<std::size_t>(n);
    }
    if (fsync(held.fd) != 0) {
        return fail(ErrorCode::IoError, "Cannot write the lock file", posixError("fsync", errno));
    }
    return {};
}

void deleteHeld(OwnedLock::HeldPtr held) noexcept
{
    if (!held || held->fd < 0) {
        return;
    }
    // Unlink only if the name still refers to our file (it may have been broken and replaced meanwhile), and
    // before closing (the deleter closes), so nobody sees our file with its flock already released.
    struct stat st{};
    if (stat(held->path.c_str(), &st) == 0 && st.st_dev == held->dev && st.st_ino == held->ino) {
        unlink(held->path.c_str());
    }
}

Result<LockFileView> readLockFile(const std::string& path)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return fail(ErrorCode::NotFound, "No lock file");
        }
        return fail(ErrorCode::IoError, "Cannot read the lock file", posixError("open", errno));
    }
    LockFileView view;
    auto bytes = readAll(fd);
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        view.mtimeMs = mtimeMs(st);
    }
    view.ownerDead = flockFree(fd);
    close(fd);
    if (!bytes) {
        return tl::unexpected<Error>(bytes.error());
    }
    view.bytes = std::move(*bytes);
    return view;
}

std::int64_t serverNowMs(const std::string& path)
{
    const std::string probe = path + ".probe-" + randomName();
    const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        return unixNowMs(); // folder not writable: only the local clock is left
    }
    const char byte = 'x';
    if (write(fd, &byte, 1) == 1) {
        fsync(fd);
    }
    struct stat st{};
    const bool ok = fstat(fd, &st) == 0;
    close(fd);
    unlink(probe.c_str());
    return ok ? mtimeMs(st) : unixNowMs();
}

Status breakLockFile(const std::string& path, const std::function<Status(const LockFileView&)>& mayBreak)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return {}; // nothing to break
        }
        return fail(ErrorCode::IoError, "Cannot break the lock", posixError("open", errno));
    }
    LockFileView view;
    struct stat judged{};
    auto bytes = readAll(fd);
    const bool statOk = fstat(fd, &judged) == 0;
    view.mtimeMs = statOk ? mtimeMs(judged) : 0;
    view.ownerDead = flockFree(fd);
    close(fd);
    if (!bytes) {
        return tl::unexpected<Error>(bytes.error());
    }
    if (!statOk) {
        return fail(ErrorCode::IoError, "Cannot break the lock", posixError("fstat", errno));
    }
    view.bytes = std::move(*bytes);
    if (auto verdict = mayBreak(view); !verdict) {
        return verdict;
    }
    const std::string broken = path + ".broken-" + randomName();
    if (rename(path.c_str(), broken.c_str()) != 0) {
        if (errno == ENOENT) {
            return {}; // somebody else broke it first
        }
        return fail(ErrorCode::IoError, "Cannot break the lock", posixError("rename", errno));
    }
    struct stat moved{};
    if (lstat(broken.c_str(), &moved) == 0 && moved.st_dev == judged.st_dev && moved.st_ino == judged.st_ino) {
        unlink(broken.c_str());
        return {};
    }
    // We renamed a lock file created after our judgement: give it back to its owner.
    if (link(broken.c_str(), path.c_str()) == 0) {
        unlink(broken.c_str());
    }
    return fail(ErrorCode::Locked, "The document was just opened for editing elsewhere",
                "lock file replaced while breaking");
}

Status renameNoReplace(const std::string& from, const std::string& to)
{
    if (link(from.c_str(), to.c_str()) == 0) {
        unlink(from.c_str());
        return {};
    }
    const int e = errno;
    if (e == EEXIST) {
        return fail(ErrorCode::InvalidArgument, "A file with this name already exists", to);
    }
    // File systems without hard links (SMB without Unix extensions, FAT): check, then rename.
    struct stat st{};
    if (stat(to.c_str(), &st) == 0) {
        return fail(ErrorCode::InvalidArgument, "A file with this name already exists", to);
    }
    if (rename(from.c_str(), to.c_str()) != 0) {
        return fail(ErrorCode::IoError, "Cannot create the document file", posixError("rename", errno));
    }
    return {};
}

std::string userName()
{
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(static_cast<std::size_t>(size > 0 ? size : 16384));
    struct passwd entry{};
    struct passwd* found = nullptr;
    if (getpwuid_r(geteuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found && found->pw_name) {
        return found->pw_name;
    }
    if (const char* env = std::getenv("USER")) {
        return env;
    }
    return "?";
}

std::string hostName()
{
    char buffer[256] = {};
    if (gethostname(buffer, sizeof buffer - 1) == 0 && buffer[0] != '\0') {
        return buffer;
    }
    return "?";
}

std::int64_t processId() noexcept
{
    return static_cast<std::int64_t>(getpid());
}

} // namespace os
} // namespace ray::store::detail
#endif
