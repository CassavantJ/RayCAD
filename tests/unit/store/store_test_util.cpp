// Shared pieces of the store tests and their helper process.
#include "store_test_util.h"

#include "lock.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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

#ifndef RAY_STORE_HELPER
#error RAY_STORE_HELPER must name the helper executable
#endif

namespace storetest {
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
    const std::string name = std::string(reinterpret_cast<const char*>(u8"raycad store tests ü 測試 ")) + tag;
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

std::string TempDir::path(std::string_view name) const
{
    return utf8Of(fsPath(dir_) / fsPath(name));
}

Process Process::spawnHelper(const std::vector<std::string>& args)
{
    Process p;
#ifdef _WIN32
    std::wstring line;
    appendQuoted(line, widen(RAY_STORE_HELPER));
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
    all.push_back(RAY_STORE_HELPER);
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

std::uintmax_t fileSize(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(fsPath(path), ec);
    return ec ? 0 : size;
}

std::string readFile(const std::string& path)
{
    std::ifstream in(fsPath(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, std::string_view bytes)
{
    std::ofstream out(fsPath(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeFileAtomic(const std::string& path, std::string_view bytes)
{
    writeFile(path + ".tmp", bytes);
    std::error_code ec;
    std::filesystem::rename(fsPath(path + ".tmp"), fsPath(path), ec);
}

void removeFile(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(fsPath(path), ec);
}

void removeDocument(const std::string& path)
{
    removeFile(path);
    removeFile(path + "-journal");
    removeFile(path + ".lock");
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

std::string randomHex(std::mt19937_64& rng, std::size_t length)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(length, '0');
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (i % 16 == 0) {
            bits = rng();
        }
        out[i] = kHex[bits & 15];
        bits >>= 4;
    }
    return out;
}

std::vector<std::byte> randomBytes(std::mt19937_64& rng, std::size_t length)
{
    std::vector<std::byte> out(length);
    std::size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        const std::uint64_t v = rng();
        for (int k = 0; k < 8; ++k) {
            out[i + static_cast<std::size_t>(k)] = static_cast<std::byte>(v >> (8 * k));
        }
    }
    for (; i < length; ++i) {
        out[i] = static_cast<std::byte>(rng());
    }
    return out;
}

ray::Hash256 hashOf(std::span<const std::byte> bytes)
{
    ray::Hasher hasher;
    hasher.bytes(bytes);
    return hasher.finish();
}

ray::doc::DocState makeState(std::mt19937_64& rng, int features, std::size_t payloadChars)
{
    ray::doc::DocState state;
    state.settings.units = ray::UnitSystem::Metric;
    state.settings.decimals = 3;
    state.parameters.push_back(
        {ray::ParamId{ray::Uuid::generate()}, "width", "4 in", ray::doc::Parameter::Kind::Length, ""});
    for (int i = 0; i < features; ++i) {
        ray::doc::Feature f;
        f.id = ray::FeatureId{ray::Uuid::generate()};
        f.type = "extrude";
        f.name = std::string(reinterpret_cast<const char*>(u8"Extrüde ")) + std::to_string(i + 1);
        f.propsJson = "{\"distance\":\"width / 2\",\"payload\":\"" + randomHex(rng, payloadChars) + "\"}";
        state.features.push_back(std::move(f));
    }
    state.bodies.push_back(
        {ray::BodyId{ray::Uuid::generate()}, "Part", ray::doc::Body::Role::Part, {0.7f, 0.7f, 0.75f, 1.0f}, true, ""});
    return state;
}

Acks readAcks(const std::string& path)
{
    Acks acks;
    const std::string text = readFile(path);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            break; // a line cut short by the kill
        }
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.size() < 3 || line[1] != ' ') {
            continue;
        }
        const std::int64_t value = std::stoll(line.substr(2));
        if (line[0] == 'S') {
            acks.start = value;
        } else if (line[0] == 'B') {
            acks.begun = value;
        } else if (line[0] == 'A') {
            acks.acked = value;
        }
    }
    return acks;
}

std::int64_t saveNumber(const std::string& message)
{
    if (message.rfind("save ", 0) != 0) {
        return -1;
    }
    try {
        return std::stoll(message.substr(5));
    } catch (...) {
        return -1;
    }
}

void useTestLockTiming()
{
    ray::store::detail::setLockTiming({kTestHeartbeatMs, kTestStaleMs});
}

} // namespace storetest
