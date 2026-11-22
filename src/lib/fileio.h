// File primitives of the library folders. Private to src/lib; every path is UTF-8.
//
// Libraries live on network shares, so nothing here takes a file lock: readers open files with every sharing mode
// (on Windows FILE_SHARE_DELETE too, so a reader never blocks another process's rename-over), and a write is a new
// temporary file beside the target, flushed, then renamed over it, so a reader sees the old file or the new one
// and a killed writer leaves at most a stray temporary file (its name starts with '.', which the loader ignores).
#pragma once
#include <ray/core.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ray::lib::detail::fio {

std::filesystem::path toFsPath(std::string_view utf8);
std::string fromFsPath(const std::filesystem::path& path);
std::string join(const std::string& dir, std::string_view name);

// Size and modification time as the file system reports them (the time in its own units: 100 ns on Windows,
// nanoseconds on POSIX; SMB and FAT round it to as much as 2 s). Only ever compared with another Stamp.
struct Stamp {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool operator==(const Stamp&) const = default;
};
struct DirEntry {
    std::string name; // UTF-8 file name
    Stamp stamp;
};

bool isFolder(const std::string& path) noexcept;
// The regular files of a folder, sorted by name. NotFound if the folder is missing.
Result<std::vector<DirEntry>> listFolder(const std::string& dir);
// The whole file. NotFound if it is missing, IoError otherwise.
Result<std::string> readFile(const std::string& path);
// Replaces (or creates) `path` with `bytes` atomically and durably: a temporary ".<name>.<random>.tmp" in the same
// folder, flushed to the disk, renamed over `path` (retried for a moment while another process holds the target
// open without delete sharing), then the folder entry is flushed. Creates the folder if it is missing.
Status replaceAtomically(const std::string& path, std::string_view bytes);
// NotFound if the file is missing.
Status removeFile(const std::string& path);
// Removes temporary files of replaceAtomically in `dir` older than an hour by this machine's clock (left by a
// killed writer). Best effort; errors are ignored.
void removeStaleTemporaries(const std::string& dir) noexcept;
// True for names replaceAtomically uses for its temporary files.
bool isTemporaryName(std::string_view name) noexcept;

// Test hook: called between flushing the temporary file and renaming it (the kill test widens that window).
void setBeforeRenameHook(void (*hook)());

} // namespace ray::lib::detail::fio
