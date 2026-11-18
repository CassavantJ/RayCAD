// The <name>.raycad.lock file (PLAN.md 4.3). Private to src/store.
//
// Opening for write creates the lock file with create-new semantics and keeps its handle open for the whole
// session: on Windows without delete sharing, on POSIX with flock(LOCK_EX) held, so a reader can tell at once
// that the owner process is gone. The file holds user, host, process id, start time, a heartbeat rewritten every
// 30 s and the save counter. Its age is judged on the file server's clock: the modification time of a probe file
// created beside it, minus the lock file's modification time. A stale lock is broken by renaming it away.
//
// The file is a fixed 512-byte block of "key=value" lines ending in a checksum line, rewritten in place and
// flushed, so a reader never sees a short file and detects a torn read.
#pragma once
#include "file_util.h"

#include <ray/store.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace ray::store::detail {

// Heartbeat interval and stale limit. The plan's values; tests shorten them in-process (and pass them to their
// helper processes), the product never changes them.
struct LockTiming {
    std::int64_t heartbeatMs = 30'000;
    std::int64_t staleMs = 120'000;
};
void setLockTiming(LockTiming timing) noexcept;
LockTiming lockTiming() noexcept;

std::string lockPathOf(const std::string& docPath);

struct LockFields {
    std::string user, host;
    std::int64_t pid = 0;
    std::int64_t sinceUnixMs = 0;     // writer's clock, for display only
    std::int64_t heartbeatUnixMs = 0; // writer's clock, for display only; staleness uses the server's clock
    std::int64_t beat = 0;
    bool saving = false;        // save counter: true while a write transaction may be changing the file
    std::int64_t saveCount = 0; // incremented after every write transaction
};
inline constexpr std::size_t kLockFileBytes = 512;
std::string encodeLock(const LockFields& fields);
std::optional<LockFields> decodeLock(std::string_view bytes);

// Read from the operating system at run time.
std::string currentUserName();
std::string currentHostName();
std::int64_t currentProcessId() noexcept;

// What another process sees of a lock file.
struct LockState {
    LockFields fields;
    bool readable = false;  // false: the content could not be parsed (being written, or damaged)
    double ageSec = 0;      // server clock: probe file mtime minus lock file mtime
    bool ownerDead = false; // the owner's handle is closed, so the owner process is gone
    bool stale = false;     // older than the stale limit, or the owner is provably dead
};
// NotFound if there is no lock file.
Result<LockState> readLock(const std::string& docPath);
LockInfo toLockInfo(const LockState& state);

// The lock this process holds on one document.
class OwnedLock {
public:
    // ErrorCode::Locked if the lock file exists already.
    static Result<std::unique_ptr<OwnedLock>> acquire(const std::string& docPath);
    ~OwnedLock(); // stops the heartbeat and deletes the lock file
    OwnedLock(const OwnedLock&) = delete;
    OwnedLock& operator=(const OwnedLock&) = delete;

    // The save counter around every write transaction: "saving", then "idle, n + 1".
    void beginWrite();
    void endWrite();
    LockFields fields() const;

    class Held; // the open handle, per platform
    // Closes the handle without deleting the file (lock_win.cpp, lock_posix.cpp).
    struct HeldDeleter {
        void operator()(Held* held) const noexcept;
    };
    using HeldPtr = std::unique_ptr<Held, HeldDeleter>;

private:
    explicit OwnedLock(HeldPtr file, LockFields fields);
    void write(); // caller holds mutex_
    void heartbeatLoop();

    HeldPtr file_;
    mutable std::mutex mutex_;
    LockFields fields_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread heartbeat_;
};

// breakStaleLock (store.h).
Status breakLock(const std::string& docPath);

// ---------- Platform layer: lock_win.cpp and lock_posix.cpp ----------
namespace os {

enum class CreateOutcome { Created, Exists, Retry };
// Create-new, with `content` written and flushed before the file can be judged by anyone else (POSIX: before it
// appears under its name, with the owner's flock already held). Retry: try again in a moment (Windows: the name is
// still being deleted by its previous owner; POSIX without hard links: a reader held the new file's flock).
Result<CreateOutcome> createLockFile(const std::string& path, std::string_view content, OwnedLock::HeldPtr& out);
// Rewrites the file from offset 0 and flushes it to the server.
Status writeHeld(OwnedLock::Held& held, std::string_view bytes);
// Deletes the file through the handle (so a successor's lock file is never deleted), then closes it.
void deleteHeld(OwnedLock::HeldPtr held) noexcept;

struct LockFileView {
    std::string bytes;
    std::int64_t mtimeMs = 0; // the file server's clock, ms since 1970
    bool ownerDead = false;
};
// NotFound if the file does not exist.
Result<LockFileView> readLockFile(const std::string& path);
// The file server's current time: the modification time of a probe file created and removed beside `path`.
// Falls back to the local clock if the folder is not writable.
std::int64_t serverNowMs(const std::string& path);
// Opens the lock file and, if `mayBreak` accepts what it read through that same handle, renames exactly that file
// away (never a successor's) and deletes it. Missing file: success. `mayBreak` returns an error to refuse. A lock
// whose owner still has the file open is refused without asking `mayBreak` (ErrorCode::Locked), on both platforms.
// Of several processes breaking the same file, exactly one breaks it.
Status breakLockFile(const std::string& path, const std::function<Status(const LockFileView&)>& mayBreak);

// Moves `from` to `to`, failing if `to` exists.
Status renameNoReplace(const std::string& from, const std::string& to);

std::string userName();
std::string hostName();
std::int64_t processId() noexcept;

} // namespace os
} // namespace ray::store::detail
