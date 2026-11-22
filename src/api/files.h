// Paths, the per-user data folder and the recent-files list, private to src/api. Every path is UTF-8.
#pragma once

#include <ray/api.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::detail {

std::filesystem::path toFsPath(std::string_view utf8);
std::string fromFsPath(const std::filesystem::path& path);
// An absolute, normalised form for comparing two paths to one file (case-folded on Windows).
std::string pathKey(const std::string& utf8);
// The file name with or without its extension; the extension in lower case with the dot (".step").
std::string fileName(const std::string& utf8);
std::string fileStem(const std::string& utf8);
std::string lowerExtension(const std::string& utf8);
bool fileExists(const std::string& utf8);

// AppOptions::userDataDir resolved: the explicit value, else $RAYCAD_USER_DATA_DIR, else the platform folder.
// "" when nothing can be found (no home directory) or the caller asked for no persistence.
std::string resolveUserDataDir(const std::optional<std::string>& option);

std::int64_t unixNowMs();

// The folder of the running executable (UTF-8), or "" when the platform does not say (program_dir.cpp).
std::string programDir();

// The recent-files list in <dir>/recent-files.json: [{"openedMs":n,"path":"..."}], most recent first.
// Thread-safe. A missing or unreadable file is an empty list; write errors are logged, never fatal.
class RecentFiles {
public:
    static constexpr std::size_t kMax = 20;
    explicit RecentFiles(std::string dir);
    std::vector<IApp::RecentFile> list() const;
    void add(const std::string& path);
    // Removes the entry for `path` (compared by pathKey); false if it is not listed.
    bool remove(const std::string& path);

private:
    void load();
    void store() const;
    std::string file_;
    mutable std::mutex mutex_;
    std::vector<IApp::RecentFile> items_;
};

} // namespace ray::api::detail
