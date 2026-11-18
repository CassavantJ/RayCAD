// Lock file, POSIX implementation (Linux, macOS) (PLAN.md 4.3). The Windows one is lock_win.cpp.
//
// The owner holds flock(LOCK_EX) on its lock file for the whole session. flock belongs to the open file description,
// not to the process, so it is released only when the owner's own descriptor closes (the owner exits or dies), and
// closing some other descriptor of the same file in the owner's process (readLock() in the owner's own process, for
// example) does not drop it. fcntl() record locks would not do: closing any descriptor of a file drops all of a
// process's fcntl locks on it, which is also why this code never opens the document file itself (that would drop
// SQLite's lock).
//
// Create-new: the owner builds the complete file under a private name, takes the flock, writes the content and only
// then publishes it with link(), which fails if the lock file exists. So the lock file never exists without content
// and without its owner's flock, and "flock free" really means "the owner is dead". File systems without hard links
// fall back to O_CREAT | O_EXCL on the lock file itself, followed by the flock and the content; a reader can catch
// such a file empty for a moment, which lock.cpp's grace period for unreadable files covers.
//
// Breaking: the breaker opens the lock file and takes the same flock with LOCK_EX. It gets it only if the owner's
// descriptor is closed and no other breaker is working on that file, so of two racers judging the same stale file
// exactly one gets to break it. The other waits for it, then finds the name no longer refers to the file it holds and
// judges whatever the name refers to now (the winner's new, live lock file). While the breaker holds the flock nobody
// else can move the file, so the rename takes exactly the file judged. The breaker keeps its descriptor open until it
// is done, so the file's inode number cannot be reused by a new lock file meanwhile (the device and inode comparisons
// below would otherwise be fooled). A lock whose owner still holds its flock is never broken, however old its
// heartbeat: as on Windows (whose owner handle denies delete sharing), a live owner on the same server keeps its lock.
// Only on a file system without flock does age alone decide.
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

// How long a creator or a breaker waits for a flock somebody holds only briefly (a reader probing it, another breaker
// judging the file).
constexpr int kFlockRetryMs = 5;
constexpr int kCreateFlockAttempts = 400; // 2 s, the grace period for an unreadable lock file
constexpr int kBreakFlockAttempts = 100;  // 0.5 s
constexpr int kBreakRounds = 5;           // the name moved to a new file this often while breaking: give up

// Closes a descriptor when it goes out of scope.
class Fd {
public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return fd_; }

private:
    int fd_;
};

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

void sleepMs(int ms) noexcept
{
    usleep(static_cast<useconds_t>(ms) * 1000);
}

bool sameFile(const struct stat& a, const struct stat& b) noexcept
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// True if `path` currently names the file `st` describes.
bool namesFile(const std::string& path, const struct stat& st) noexcept
{
    struct stat now{};
    return lstat(path.c_str(), &now) == 0 && sameFile(now, st);
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

enum class Flock { Taken, Busy, Unsupported };

Flock tryFlock(int fd, int operation) noexcept
{
    for (;;) {
        if (flock(fd, operation | LOCK_NB) == 0) {
            return Flock::Taken;
        }
        if (errno == EINTR) {
            continue;
        }
        // EWOULDBLOCK: somebody holds it. Anything else (ENOLCK, EOPNOTSUPP, EBADF for an exclusive lock through a
        // read-only descriptor on NFS, ...): this file system cannot tell.
        return errno == EWOULDBLOCK || errno == EAGAIN ? Flock::Busy : Flock::Unsupported;
    }
}

Flock flockWithRetry(int fd, int operation, int attempts) noexcept
{
    Flock got = tryFlock(fd, operation);
    for (int i = 1; i < attempts && got == Flock::Busy; ++i) {
        sleepMs(kFlockRetryMs);
        got = tryFlock(fd, operation);
    }
    return got;
}

// True if no other open file description holds the flock: the owner's descriptor is closed.
bool flockFree(int fd) noexcept
{
    if (tryFlock(fd, LOCK_SH) == Flock::Taken) {
        flock(fd, LOCK_UN);
        return true;
    }
    return false;
}

// link() errors that mean "this file system has no hard links" rather than a real failure.
bool linkUnsupported(int e) noexcept
{
    return e == EPERM || e == EOPNOTSUPP || e == ENOTSUP || e == ENOSYS || e == EMLINK || e == EXDEV;
}

void fillIdentity(OwnedLock::Held& held) noexcept
{
    struct stat st{};
    if (fstat(held.fd, &st) == 0) {
        held.dev = st.st_dev;
        held.ino = st.st_ino;
    }
}

// Removes `path` only if it still names the file `held` has open.
void unlinkIfOurs(const OwnedLock::Held& held) noexcept
{
    struct stat st{};
    if (stat(held.path.c_str(), &st) == 0 && st.st_dev == held.dev && st.st_ino == held.ino) {
        unlink(held.path.c_str());
    }
}

// Fallback for file systems without hard links: create-new on the lock file itself, then flock, then the content.
Result<CreateOutcome> createInPlace(const std::string& path, std::string_view content, OwnedLock::HeldPtr& out)
{
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return CreateOutcome::Exists;
        }
        return fail(ErrorCode::IoError, "Cannot create the lock file beside the document", posixError("open", errno));
    }
    OwnedLock::HeldPtr held(new OwnedLock::Held);
    held->fd = fd;
    held->path = path;
    fillIdentity(*held);
    // A reader probing the flock, or a breaker looking at the still empty file, can hold it for a moment.
    const Flock locked = flockWithRetry(fd, LOCK_EX, kCreateFlockAttempts);
    if (locked == Flock::Busy) {
        unlinkIfOurs(*held);
        return CreateOutcome::Retry;
    }
    if (locked == Flock::Unsupported) {
        // The lock still works, only "owner provably dead" is lost.
        ray::log(LogLevel::Warn, "store", "flock on the lock file failed: " + std::string(std::strerror(errno)));
    }
    if (auto st = writeHeld(*held, content); !st) {
        unlinkIfOurs(*held);
        return tl::unexpected<Error>(st.error());
    }
    out = std::move(held);
    return CreateOutcome::Created;
}

} // namespace

Result<CreateOutcome> createLockFile(const std::string& path, std::string_view content, OwnedLock::HeldPtr& out)
{
    const std::string temp = path + ".new-" + randomName();
    const int fd = open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        return fail(ErrorCode::IoError, "Cannot create the lock file beside the document", posixError("open", errno));
    }
    OwnedLock::HeldPtr held(new OwnedLock::Held);
    held->fd = fd;
    held->path = path;
    // Nobody else knows the private name, so the flock is free unless the file system has none.
    const bool flocked = tryFlock(fd, LOCK_EX) == Flock::Taken;
    const int flockErrno = errno;
    if (auto st = writeHeld(*held, content); !st) {
        unlink(temp.c_str());
        return tl::unexpected<Error>(st.error());
    }
    if (link(temp.c_str(), path.c_str()) == 0) {
        unlink(temp.c_str());
        if (!flocked) {
            ray::log(LogLevel::Warn, "store",
                     "flock on the lock file failed: " + std::string(std::strerror(flockErrno)));
        }
        fillIdentity(*held);
        out = std::move(held);
        return CreateOutcome::Created;
    }
    const int e = errno;
    unlink(temp.c_str());
    held.reset();
    if (e == EEXIST) {
        return CreateOutcome::Exists;
    }
    if (linkUnsupported(e)) {
        return createInPlace(path, content, out);
    }
    return fail(ErrorCode::IoError, "Cannot create the lock file beside the document", posixError("link", e));
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
    // Unlink only if the name still refers to our file, and before closing (the deleter closes), so nobody sees our
    // file with its flock already released. While we hold the flock no breaker can move the file, so the name cannot
    // change between the check and the unlink.
    unlinkIfOurs(*held);
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
    const Fd guard(fd);
    LockFileView view;
    auto bytes = readAll(fd);
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        view.mtimeMs = mtimeMs(st);
    }
    view.ownerDead = flockFree(fd);
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
    for (int round = 0; round < kBreakRounds; ++round) {
        // Read-write where allowed: NFS implements flock with fcntl locks, and an exclusive one needs write access.
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
            fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            if (errno == ENOENT) {
                return {}; // nothing to break
            }
            return fail(ErrorCode::IoError, "Cannot break the lock", posixError("open", errno));
        }
        // Kept open until the end: the inode number stays taken, so a new lock file cannot get it.
        const Fd guard(fd);
        const Flock token = flockWithRetry(fd, LOCK_EX, kBreakFlockAttempts);
        struct stat judged{};
        if (fstat(fd, &judged) != 0) {
            return fail(ErrorCode::IoError, "Cannot break the lock", posixError("fstat", errno));
        }
        if (!namesFile(path, judged)) {
            continue; // broken or released by somebody else meanwhile: judge what the name refers to now
        }
        auto bytes = readAll(fd);
        if (!bytes) {
            return tl::unexpected<Error>(bytes.error());
        }
        if (token == Flock::Busy) {
            // The owner still holds its flock: it is alive, however old its heartbeat.
            std::string who;
            if (auto fields = decodeLock(*bytes)) {
                who = " by " + fields->user + "@" + fields->host;
            }
            return fail(ErrorCode::Locked, "The document is open for editing" + who,
                        "the lock owner still has the lock file open");
        }
        LockFileView view;
        view.bytes = std::move(*bytes);
        view.mtimeMs = mtimeMs(judged);
        view.ownerDead = token == Flock::Taken;
        if (auto verdict = mayBreak(view); !verdict) {
            return verdict;
        }
        const std::string broken = path + ".broken-" + randomName();
        if (rename(path.c_str(), broken.c_str()) != 0) {
            if (errno == ENOENT) {
                continue; // without flock support, somebody else broke it first
            }
            return fail(ErrorCode::IoError, "Cannot break the lock", posixError("rename", errno));
        }
        struct stat moved{};
        if (lstat(broken.c_str(), &moved) == 0 && sameFile(moved, judged)) {
            unlink(broken.c_str());
            return {};
        }
        // Only without flock support: the name was given to a new lock file between the check and the rename, and
        // that file was renamed instead. Give it back to its owner.
        if (link(broken.c_str(), path.c_str()) == 0) {
            unlink(broken.c_str());
        } else {
            (void)rename(broken.c_str(), path.c_str());
        }
        return fail(ErrorCode::Locked, "The document was just opened for editing elsewhere",
                    "lock file replaced while breaking");
    }
    return fail(ErrorCode::Locked, "The document is being opened elsewhere", "the lock file keeps changing");
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
