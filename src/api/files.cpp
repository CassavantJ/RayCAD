#include "files.h"

#include "json.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace ray::api::detail {

std::filesystem::path toFsPath(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string fromFsPath(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string pathKey(const std::string& utf8)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(toFsPath(utf8), ec);
    if (ec) {
        p = toFsPath(utf8);
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(p, ec);
    std::string key = fromFsPath(ec ? p.lexically_normal() : canonical);
#ifdef _WIN32
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (c == '/') {
            c = '\\';
        }
    }
#endif
    return key;
}

std::string fileName(const std::string& utf8)
{
    return fromFsPath(toFsPath(utf8).filename());
}

std::string fileStem(const std::string& utf8)
{
    return fromFsPath(toFsPath(utf8).stem());
}

std::string lowerExtension(const std::string& utf8)
{
    std::string ext = fromFsPath(toFsPath(utf8).extension());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

bool fileExists(const std::string& utf8)
{
    std::error_code ec;
    return std::filesystem::exists(toFsPath(utf8), ec) && !ec;
}

namespace {
std::optional<std::string> environment(const char* name)
{
#ifdef _WIN32
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wide.c_str());
    if (value == nullptr || *value == 0) {
        return std::nullopt;
    }
    return fromFsPath(std::filesystem::path(std::wstring(value)));
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == 0) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}
} // namespace

std::string resolveUserDataDir(const std::optional<std::string>& option)
{
    if (option) {
        return *option;
    }
    if (auto overridden = environment("RAYCAD_USER_DATA_DIR")) {
        return *overridden;
    }
#if defined(_WIN32)
    if (auto local = environment("LOCALAPPDATA")) {
        return fromFsPath(toFsPath(*local) / "RayCAD");
    }
    return {};
#elif defined(__APPLE__)
    if (auto home = environment("HOME")) {
        return fromFsPath(toFsPath(*home) / "Library" / "Application Support" / "RayCAD");
    }
    return {};
#else
    if (auto data = environment("XDG_DATA_HOME")) {
        return fromFsPath(toFsPath(*data) / "raycad");
    }
    if (auto home = environment("HOME")) {
        return fromFsPath(toFsPath(*home) / ".local" / "share" / "raycad");
    }
    return {};
#endif
}

std::int64_t unixNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------- Recent files ----------
RecentFiles::RecentFiles(std::string dir)
{
    if (!dir.empty()) {
        file_ = fromFsPath(toFsPath(dir) / "recent-files.json");
        load();
    }
}

std::vector<IApp::RecentFile> RecentFiles::list() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

void RecentFiles::add(const std::string& path)
{
    if (path.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = pathKey(path);
    items_.erase(
        std::remove_if(items_.begin(), items_.end(), [&](const IApp::RecentFile& f) { return pathKey(f.path) == key; }),
        items_.end());
    items_.insert(items_.begin(), IApp::RecentFile{path, unixNowMs(), {}});
    if (items_.size() > kMax) {
        items_.resize(kMax);
    }
    store();
}

void RecentFiles::load()
{
    std::ifstream in(toFsPath(file_), std::ios::binary);
    if (!in) {
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = json::parse(text);
    if (!parsed || !parsed->isArray()) {
        log(LogLevel::Warn, "api", "The recent-files list is unreadable and was ignored: " + file_);
        return;
    }
    for (const json::Value& item : parsed->asArray()) {
        const json::Value* path = item.find("path");
        const json::Value* opened = item.find("openedMs");
        if (path && path->isString() && !path->asString().empty() && opened && opened->isInteger()) {
            items_.push_back(IApp::RecentFile{path->asString(), static_cast<std::int64_t>(opened->asNumber()), {}});
        }
        if (items_.size() >= kMax) {
            break;
        }
    }
}

void RecentFiles::store() const
{
    if (file_.empty()) {
        return;
    }
    json::Value list = json::Value::array();
    for (const IApp::RecentFile& f : items_) {
        json::Value item = json::Value::object();
        item.set("path", json::Value::string(f.path));
        item.set("openedMs", json::Value::integer(f.openedMs));
        list.push(std::move(item));
    }
    const std::filesystem::path target = toFsPath(file_);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << json::canonical(list) << '\n';
        if (!out) {
            log(LogLevel::Warn, "api", "Cannot write the recent-files list: " + file_);
            return;
        }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        log(LogLevel::Warn, "api", "Cannot replace the recent-files list: " + file_);
    }
}

} // namespace ray::api::detail
