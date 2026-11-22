// Shared pieces of the library tests and their helper process.
#include "lib_test_util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef RAY_LIB_HELPER
#error RAY_LIB_HELPER must name the helper executable
#endif

namespace libtest {
namespace {

std::filesystem::path fsPath(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8Of(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

#ifdef _WIN32
std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// Quoting for CommandLineToArgvW / the MSVC runtime.
void appendQuoted(std::wstring& line, const std::wstring& arg)
{
    line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        backslashes = 0;
        line.push_back(c);
    }
    line.append(backslashes * 2, L'\\');
    line.push_back(L'"');
}
#endif

} // namespace

TempDir::TempDir()
{
    std::random_device device;
    const auto tag = std::to_string(device()) + std::to_string(device());
    const std::string name = std::string(reinterpret_cast<const char*>(u8"raycad lib tests ü 測試 ")) + tag;
    const auto dir = std::filesystem::temp_directory_path() / fsPath(name);
    std::filesystem::create_directories(dir);
    dir_ = utf8Of(dir);
}

TempDir::~TempDir()
{
    std::error_code ec;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::filesystem::remove_all(fsPath(dir_), ec);
        if (!ec) {
            return;
        }
        sleepMs(50); // a killed child's handles may close a moment later
    }
}

std::string TempDir::path(std::string_view relative) const
{
    return utf8Of((fsPath(dir_) / fsPath(relative)).lexically_normal());
}

Process Process::spawnHelper(const std::vector<std::string>& args)
{
    Process p;
#ifdef _WIN32
    std::wstring line;
    appendQuoted(line, widen(RAY_LIB_HELPER));
    for (const std::string& arg : args) {
        line.push_back(L' ');
        appendQuoted(line, widen(arg));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("CreateProcessW failed: " + std::to_string(GetLastError()));
    }
    CloseHandle(pi.hThread);
    p.handle_ = pi.hProcess;
    p.pid_ = static_cast<std::int64_t>(pi.dwProcessId);
#else
    std::vector<std::string> all;
    all.push_back(RAY_LIB_HELPER);
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : all) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, all[0].c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        throw std::runtime_error("posix_spawn failed");
    }
    p.pid_ = pid;
#endif
    p.done_ = false;
    return p;
}

Process::Process(Process&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), done_(other.done_), exitCode_(other.exitCode_)
{
    other.handle_ = nullptr;
    other.done_ = true;
}

Process& Process::operator=(Process&& other) noexcept
{
    if (this != &other) {
        kill();
        handle_ = other.handle_;
        pid_ = other.pid_;
        done_ = other.done_;
        exitCode_ = other.exitCode_;
        other.handle_ = nullptr;
        other.done_ = true;
    }
    return *this;
}

Process::~Process()
{
    kill();
}

void Process::kill()
{
    if (done_) {
        return;
    }
#ifdef _WIN32
    TerminateProcess(static_cast<HANDLE>(handle_), 99);
#else
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
    wait();
}

int Process::wait()
{
    if (done_) {
        return exitCode_;
    }
#ifdef _WIN32
    WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    exitCode_ = static_cast<int>(code);
#else
    int status = 0;
    while (waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    done_ = true;
    return exitCode_;
}

bool Process::running()
{
    if (done_) {
        return false;
    }
#ifdef _WIN32
    if (WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_OBJECT_0) {
        wait();
        return false;
    }
    return true;
#else
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == static_cast<pid_t>(pid_)) {
        exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        done_ = true;
        return false;
    }
    return true;
#endif
}

bool exists(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::exists(fsPath(path), ec);
}

std::string readFile(const std::string& path)
{
    std::ifstream in(fsPath(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, std::string_view bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(fsPath(path).parent_path(), ec);
    std::ofstream out(fsPath(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void removeFile(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(fsPath(path), ec);
}

std::vector<std::string> listFiles(const std::string& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(fsPath(dir), ec)) {
        if (entry.is_regular_file()) {
            out.push_back(utf8Of(entry.path().filename()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

ray::Uuid fixedId(int n)
{
    char text[40];
    std::snprintf(text, sizeof text, "0000%04x-1111-4222-8333-%012x", static_cast<unsigned>(n),
                  static_cast<unsigned>(n));
    return ray::Uuid::fromString(text);
}

ray::lib::Tool sampleTool(int n, ray::lib::ToolType type, double diameter)
{
    ray::lib::Tool t{};
    t.id = ray::ToolId{fixedId(n)};
    t.name = "Tool " + std::to_string(n);
    t.vendor = "";
    t.productId = "";
    t.type = type;
    t.flutes = 3;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = 2 * diameter;
    t.shoulderLength = 2.5 * diameter;
    t.overallLength = 8 * diameter;
    t.bodyMaterial = "carbide";
    if (type == ray::lib::ToolType::Tap) {
        t.threadPitch = 1;
        t.threadSpec = "M6x1";
    }
    t.presets.push_back({ray::MaterialId{fixedId(900)}, "rough", 250, 0.05, 0.1, 2, 2});
    t.presets.push_back({ray::MaterialId{fixedId(900)}, "finish", 300, 0.04, 0.05, 2, 2});
    return t;
}

ray::lib::Holder sampleHolder(int n)
{
    ray::lib::Holder h{};
    h.id = ray::HolderId{fixedId(n)};
    h.name = "Holder " + std::to_string(n);
    h.gaugeLength = 70;
    h.segments = {{15.9, 63.5, 63.5}, {54.1, 50, 50}};
    return h;
}

ray::lib::Material sampleMaterial(int n)
{
    ray::lib::Material m{};
    m.id = ray::MaterialId{fixedId(n)};
    m.name = "6061-T6";
    m.group = "Aluminium";
    m.hardnessHb = 95;
    m.specificCuttingForceNmm2 = 700;
    m.densityGcm3 = 2.7;
    return m;
}

ray::lib::MachineDef sampleMachine(int n)
{
    ray::lib::MachineDef m{};
    m.id = ray::MachineId{fixedId(n)};
    m.name = "Machine " + std::to_string(n);
    m.kind = ray::lib::MachineDef::Kind::Mill3Axis;
    m.axes = {{'X', 0, 762, 25400, 16500, 2500}, {'Y', 0, 406, 25400, 16500, 2500}, {'Z', -508, 0, 25400, 16500, 2500}};
    m.maxRpm = 8100;
    m.toolCapacity = 20;
    m.toolChangeSec = 4.2;
    m.controlDialect = "haas-ngc";
    m.defaultPost = "haas-ngc";
    m.rigidTapping = true;
    m.maxBlockNumber = 99999;
    m.maxProgramNameLength = 8;
    m.postPropertyOverrides = {{"useG187", "true"}, {"comment", "\"x\""}};
    return m;
}

ray::lib::OpTemplate sampleTemplate(int n)
{
    ray::lib::OpTemplate t{};
    t.id = fixedId(n);
    t.name = "Template " + std::to_string(n);
    t.opType = "drill";
    t.featureKind = "hole.tapped";
    t.toolQueryJson = R"({"type":"tap","threadSpec":"M6x1"})";
    t.paramsJson = R"({"cycle":"tap","depth":"feature.threadDepth + 2 * tool.threadPitch"})";
    return t;
}

std::string toolFile(const std::string& root, const ray::Uuid& id)
{
    return utf8Of(fsPath(root) / "tools" / fsPath(id.toString() + ".tool.json"));
}

} // namespace libtest
