// File primitives of the recovery journal and of migration (PLAN.md 4.2, 4.4). Private to src/store.
// journal_io_win.cpp and journal_io_posix.cpp implement them; every path is UTF-8.
#pragma once
#include "file_util.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ray::store::detail::jio {

// A file held open by one process at a time: on Windows without write or delete sharing, on POSIX with flock(LOCK_EX)
// held. A second opener (in this or another process) gets ErrorCode::Locked, which is how a journal still in use
// by a running session is told apart from one a crash left behind.
class ExclusiveFile {
public:
    // `create`: create the file if it is missing; otherwise NotFound when it is missing.
    static Result<std::unique_ptr<ExclusiveFile>> open(const std::string& path, bool create);
    ~ExclusiveFile(); // closes; the file stays
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;

    Result<std::string> readAll();
    Status writeAt(std::uint64_t offset, std::string_view bytes);
    Status truncate(std::uint64_t size);
    // Data and size on the disk (FlushFileBuffers; fdatasync on Linux, fsync elsewhere).
    Status flush();
    // Deletes the file through this handle and closes it.
    Status removeAndClose();

    struct Impl;

private:
    explicit ExclusiveFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Moves `from` over `to` (replacing it) atomically and durably.
Status replaceFile(const std::string& from, const std::string& to);
// Flushes a closed file's data to the disk.
Status syncFile(const std::string& path);
// Makes a new or renamed directory entry durable (POSIX: fsync of the folder; Windows: nothing to do).
Status syncFolderOf(const std::string& path);
// Last modification time, ms since 1970; 0 if unknown.
std::int64_t modifiedMs(const std::string& path) noexcept;
// <user data dir>/recovery (store.h): LOCALAPPDATA, ~/Library/Application Support or XDG_DATA_HOME.
std::string defaultRecoveryDir();

} // namespace ray::store::detail::jio
