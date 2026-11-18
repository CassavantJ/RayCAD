// Shared helpers of the api tests: a headless app with its own user-data folder, temporary folders, data files.
#pragma once

#include <ray/api.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace ray::test {

inline std::string utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

inline std::filesystem::path fsPath(const std::string& utf8Path)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
}

// A fresh folder under the system temp folder, removed again at the end of the test.
class TempDir {
public:
    TempDir()
    {
        path_ = std::filesystem::temp_directory_path() / "raycad-api-tests" / ray::Uuid::generate().toString();
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string file(const std::string& name) const { return utf8(path_ / name); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::string kernelData(const std::string& name)
{
    return std::string(RAY_KERNEL_DATA_DIR) + "/" + name;
}

inline std::string corpusFile(const std::string& name)
{
    return std::string(RAY_CORPUS_DIR) + "/" + name;
}

inline std::unique_ptr<api::HeadlessApp> makeTestApp(const std::string& userDataDir, int workers = 4)
{
    api::AppOptions options;
    options.workerThreads = workers;
    options.userDataDir = userDataDir;
    auto app = api::makeHeadlessApp(std::move(options));
    REQUIRE(app != nullptr);
    return app;
}

// A JSON string literal (quotes included) for building argument objects with paths.
inline std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

inline constexpr std::chrono::milliseconds kWait = std::chrono::minutes(5);

inline void writeBytes(const std::string& path, const std::string& bytes)
{
    std::ofstream out(fsPath(path), std::ios::binary | std::ios::trunc);
    out << bytes;
}

} // namespace ray::test
