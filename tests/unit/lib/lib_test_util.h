// Shared pieces of the library tests and their helper process: temp folders, child processes, sample items.
// (The process and folder helpers follow tests/unit/store/store_test_util.*, which links the document model.)
#pragma once
#include <ray/libraries.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace libtest {

// A new folder under the system temp folder whose name has spaces and non-ASCII characters, so every test uses
// UTF-8 paths. Removed with its contents at destruction.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& dir() const { return dir_; }
    std::string path(std::string_view relative) const; // UTF-8; '/' separates folders

private:
    std::string dir_;
};

// A child process running the test helper (lib_helper.cpp). Killed at destruction if still running.
class Process {
public:
    static Process spawnHelper(const std::vector<std::string>& args);
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    ~Process();
    void kill(); // hard kill (TerminateProcess / SIGKILL), then wait
    int wait();  // exit code
    bool running();

private:
    Process() = default;
    void* handle_ = nullptr;
    std::int64_t pid_ = 0;
    bool done_ = true;
    int exitCode_ = 0;
};

bool exists(const std::string& path);
std::string readFile(const std::string& path);
void writeFile(const std::string& path, std::string_view bytes); // creates the folder
void removeFile(const std::string& path);
std::vector<std::string> listFiles(const std::string& dir); // file names, sorted
void sleepMs(int ms);
std::int64_t nowMs(); // steady clock

// Fixed ids for tests (version 4 form), and sample items with every field set.
ray::Uuid fixedId(int n);
ray::lib::Tool sampleTool(int n, ray::lib::ToolType type = ray::lib::ToolType::FlatEndMill, double diameter = 6);
ray::lib::Holder sampleHolder(int n);
ray::lib::Material sampleMaterial(int n);
ray::lib::MachineDef sampleMachine(int n);
ray::lib::OpTemplate sampleTemplate(int n);

// The file a sample item is saved to: "<root>/tools/<uuid>.tool.json".
std::string toolFile(const std::string& root, const ray::Uuid& id);

} // namespace libtest
