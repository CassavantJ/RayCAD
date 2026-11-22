// File output of posted programs (src/post/output.h, task T-047): naming patterns and the names they make, the
// overwrite rules, atomic writes on a local folder, on a loopback SMB path when this machine has one, and through a
// file-system fake for what a real share cannot be made to do on demand (vanish mid-write, fill up, lock a file
// half-way), crash safety with a helper process killed mid-write and just before the rename, and that every program
// is written byte for byte as the post produced it (every golden program, CRLF and LF).
//
// SMB: the [smb] cases use the loopback administrative share of the drive the system temp folder is on
// (\\localhost\C$\...), which Windows has by default; nothing is shared, created or changed. They skip, saying why,
// when it cannot be reached (another OS, admin shares off). RAY_TEST_SMB_DIR names another share folder to use.
#include "output.h"
#include "post_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ray;
using namespace ray::post;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
fs::path native(const std::string& utf8Path)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
}

std::string readAll(const std::string& path)
{
    std::ifstream in(native(path), std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
void writeAll(const std::string& path, std::string_view bytes)
{
    std::ofstream out(native(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::vector<std::string> listDir(const std::string& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(native(dir), ec))
        out.push_back(utf8(e.path().filename()));
    std::sort(out.begin(), out.end());
    return out;
}

// A new folder under the system temp folder, removed at the end. `ascii`: no spaces or non-ASCII in its name (the
// helper process takes it on its command line).
class TempDir {
public:
    explicit TempDir(bool ascii = false)
    {
        std::random_device rd;
        const std::string tag = std::to_string(rd()) + std::to_string(rd());
        const std::string name = ascii ? "raycad-t047-" + tag : "raycad t047 \xC3\xBC " + tag;
        path_ = fs::temp_directory_path() / native(name);
        fs::create_directories(path_);
        dir_ = utf8(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const std::string& dir() const { return dir_; }
    std::string file(const std::string& name) const { return joinPath(dir_, name); }

private:
    fs::path path_;
    std::string dir_;
};

// A program with CRLF line ends and every byte the posts write (and a non-ASCII one, which no post writes but the
// writer must not touch either).
std::string sampleProgram(std::size_t lines = 40)
{
    std::string s = "%\r\nO01001 (BRACKET)\r\n";
    for (std::size_t i = 0; i < lines; ++i)
        s += "N" + std::to_string(10 * (i + 1)) + " G1 X" + std::to_string(i) + ".25 Y-1. F500.\r\n";
    s += "(caf\xC3\xA9)\r\nM30\r\n%\r\n";
    return s;
}

NameFields fields()
{
    NameFields f;
    f.programNumber = 1001;
    f.programComment = "rev A";
    f.setupName = "Op 1 - top";
    f.documentName = "Bracket";
    f.partName = "Bracket body";
    f.postId = "haas-ngc";
    f.machineName = "Haas VF-2";
    f.extension = ".nc";
    return f;
}

std::string nameOf(std::string_view pattern, const NameFields& f = fields(), int limit = 0)
{
    auto n = fileNameFor(pattern, f, limit);
    INFO(std::string(pattern) << ": " << (n ? std::string() : n.error().message));
    REQUIRE(n);
    return n->name;
}

// ---------- an in-memory file system with faults on demand ----------
struct FakeFs final : public IOutputFs {
    std::set<std::string> folders;
    std::map<std::string, std::string> files; // committed contents by path
    std::set<std::string> locked;             // paths another program holds open (replace fails)
    // Faults.
    std::optional<OutputProblem> folderProblem;
    std::size_t failWriteAfter = std::string::npos; // bytes, then the write fails with writeProblem
    OutputProblem writeProblem = OutputProblem::DiskFull;
    bool failFlush = false;
    bool vanishOnWrite = false;    // the share goes away during the write: nothing works after that
    std::string appearOnRename;    // a file another program creates just before the rename
    int tempCollisions = 0;        // the first temp names are taken
    std::function<void()> onWrite; // called before each write
    bool gone = false;
    int creates = 0, removes = 0, renames = 0;

    struct File final : public IOutputFile {
        FakeFs& fs;
        std::string path;
        bool open = true;
        File(FakeFs& f, std::string p) : fs(f), path(std::move(p)) {}
        Status write(std::string_view bytes) override
        {
            if (fs.onWrite)
                fs.onWrite();
            if (fs.gone)
                return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "share gone"));
            std::string& data = fs.files[path];
            if (data.size() + bytes.size() > fs.failWriteAfter) {
                data.append(bytes.substr(0, fs.failWriteAfter - std::min(fs.failWriteAfter, data.size())));
                if (fs.vanishOnWrite)
                    fs.gone = true;
                return tl::unexpected(outputError(ErrorCode::IoError, fs.writeProblem,
                                                  fs.writeProblem == OutputProblem::DiskFull ? "The disk is full"
                                                                                             : "The share went away"));
            }
            data.append(bytes);
            return std::monostate{};
        }
        Status flush() override
        {
            if (fs.failFlush || fs.gone)
                return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "flush failed"));
            return std::monostate{};
        }
        Status close() override
        {
            open = false;
            return std::monostate{};
        }
    };

    static std::string folderOf(const std::string& p) { return p.substr(0, p.find_last_of("/\\")); }
    Status checkFolder(const std::string& dir) override
    {
        if (gone || folderProblem == OutputProblem::Unreachable)
            return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable,
                                              "The folder " + dir + " cannot be reached"));
        if (folders.count(dir) == 0)
            return tl::unexpected(outputError(ErrorCode::NotFound, OutputProblem::FolderMissing,
                                              "The folder " + dir + " does not exist"));
        return std::monostate{};
    }
    Result<bool> exists(const std::string& path) override
    {
        if (gone)
            return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "gone"));
        return files.count(path) != 0;
    }
    Result<std::unique_ptr<IOutputFile>> createNew(const std::string& path) override
    {
        if (gone)
            return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "gone"));
        if (tempCollisions > 0) {
            --tempCollisions;
            files[path] = "someone else's temp";
            return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Exists, "exists"));
        }
        if (files.count(path) != 0)
            return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Exists, "exists"));
        ++creates;
        files[path] = std::string();
        return std::unique_ptr<IOutputFile>(std::make_unique<File>(*this, path));
    }
    Status replace(const std::string& from, const std::string& to) override
    {
        ++renames;
        if (gone)
            return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "gone"));
        if (locked.count(to) != 0)
            return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Locked,
                                              to + " is open in another program; the old file is unchanged"));
        files[to] = files[from];
        files.erase(from);
        return std::monostate{};
    }
    Status moveNoReplace(const std::string& from, const std::string& to) override
    {
        ++renames;
        if (gone)
            return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "gone"));
        if (!appearOnRename.empty()) {
            files[appearOnRename] = "theirs";
            appearOnRename.clear();
        }
        if (files.count(to) != 0)
            return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Exists, "exists"));
        files[to] = files[from];
        files.erase(from);
        return std::monostate{};
    }
    Status remove(const std::string& path) override
    {
        if (gone)
            return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Unreachable, "gone"));
        ++removes;
        files.erase(path);
        return std::monostate{};
    }
    // The files whose name a control could load (not our temporary names).
    std::vector<std::string> programs() const
    {
        std::vector<std::string> out;
        for (const auto& [p, data] : files) {
            const std::string name = p.substr(p.find_last_of("/\\") + 1);
            if (!isTemporaryOutputName(name))
                out.push_back(name);
        }
        return out;
    }
    std::size_t temporaries() const { return files.size() - programs().size(); }
};

constexpr const char* kShare = "\\\\server\\cnc";

OutputTarget target(OverwriteMode mode, const std::string& name = "O1001.nc", const std::string& dir = kShare)
{
    OutputTarget t;
    t.directory = dir;
    t.fileName = name;
    t.mode = mode;
    return t;
}

// ---------- a child process to kill ----------
class Child {
public:
    explicit Child(const std::vector<std::string>& args)
    {
#ifdef _WIN32
        std::wstring cmd;
        for (const std::string& a : args) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), w.data(), n);
            cmd += (cmd.empty() ? L"\"" : L" \"") + w + L"\"";
        }
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
        CloseHandle(pi.hThread);
        handle_ = pi.hProcess;
#else
        pid_ = fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            std::vector<char*> argv;
            for (const std::string& a : args)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
#endif
    }
    ~Child() { kill(); }
    void kill()
    {
#ifdef _WIN32
        if (handle_ != nullptr) {
            TerminateProcess(handle_, 9);
            WaitForSingleObject(handle_, INFINITE);
            CloseHandle(handle_);
            handle_ = nullptr;
        }
#else
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = 0;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    pid_t pid_ = 0;
#endif
};

bool waitForFile(const std::string& path, int ms)
{
    for (int i = 0; i < ms / 20; ++i) {
        std::error_code ec;
        if (fs::exists(native(path), ec))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// The folder of `local` as the loopback SMB share sees it (\\localhost\C$\...), or why there is none.
std::optional<std::string> smbFolder(const std::string& local, std::string* why)
{
    if (const char* forced = std::getenv("RAY_TEST_SMB_DIR"); forced != nullptr && *forced != '\0')
        return std::string(forced);
#ifdef _WIN32
    if (local.size() < 3 || local[1] != ':' || (local[2] != '\\' && local[2] != '/')) {
        *why = "the temp folder " + local + " is not on a drive letter";
        return std::nullopt;
    }
    const std::string unc = std::string("\\\\localhost\\") + local[0] + "$" + local.substr(2);
    std::error_code ec;
    if (!fs::is_directory(native(unc), ec)) {
        *why = "the loopback administrative share " + unc.substr(0, 14) + " cannot be reached on this machine";
        return std::nullopt;
    }
    return unc;
#else
    (void)local;
    *why = "no loopback SMB share on this OS (set RAY_TEST_SMB_DIR to a mounted share folder)";
    return std::nullopt;
#endif
}

} // namespace

// ======================================================================================================== names
TEST_CASE("output: naming patterns expand every field", "[post][output]")
{
    CHECK(nameOf(kDefaultFileNamePattern) == "Bracket_Op 1 - top.nc");
    CHECK(nameOf("O{program:05}") == "O01001.nc");
    CHECK(nameOf("O{program:4}") == "O1001.nc");
    CHECK(nameOf("{program}") == "1001.nc");
    CHECK(nameOf("{part}") == "Bracket body.nc");
    CHECK(nameOf("{doc} {comment}") == "Bracket rev A.nc");
    CHECK(nameOf("{post}-{machine}") == "haas-ngc-Haas VF-2.nc");
    // {ext} places the extension; it is then not appended again.
    CHECK(nameOf("{program}{ext}") == "1001.nc");
    CHECK(nameOf("{program}{ext}.bak") == "1001.nc.bak");
    // Literal braces (which are then made safe like any other character).
    CHECK(nameOf("{{x}}") == "_x_.nc");
    NameFields small = fields();
    small.programNumber = 7;
    CHECK(nameOf("O{program:5}", small) == "O00007.nc");
    small.extension = "";
    CHECK(nameOf("{program}", small) == "7");
    small.extension = ".T AP";
    CHECK(nameOf("{program}", small) == "7.T AP");
}

TEST_CASE("output: a bad pattern is refused naming its problem", "[post][output]")
{
    const auto error = [](std::string_view pattern) {
        auto n = fileNameFor(pattern, fields());
        REQUIRE_FALSE(n);
        CHECK(n.error().code == ErrorCode::InvalidArgument);
        return n.error().message;
    };
    CHECK_THAT(error("{nope}"), ContainsSubstring("unknown field {nope}"));
    CHECK_THAT(error("{setup:3}"), ContainsSubstring("{setup:3}"));
    CHECK_THAT(error("{program:0}"), ContainsSubstring("N from 1 to 9"));
    CHECK_THAT(error("{program:x}"), ContainsSubstring("{program:x}"));
    CHECK_THAT(error("{doc"), ContainsSubstring("'{' without its '}'"));
    CHECK_THAT(error("doc}"), ContainsSubstring("'}' without its '{'"));
}

TEST_CASE("output: names are made valid on Windows, Linux and macOS", "[post][output]")
{
    NameFields f = fields();
    const auto named = [&](std::string setupName) {
        f.setupName = std::move(setupName);
        return nameOf("{setup}", f);
    };
    CHECK(named("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j.nc");
    CHECK(named(std::string("tab\there\x01") + "\x7f") == "tab_here__.nc");
    CHECK(named("Geh\xC3\xA4use") == "Geh_use.nc");              // one '_' per character, not per byte
    CHECK(named("rocket \xF0\x9F\x9A\x80") == "rocket _.nc");     // a 4-byte character
    CHECK(named("  ..hidden") == "hidden.nc");                    // no leading dots or spaces
    CHECK(named("trailing. . ") == "trailing.nc");                // Windows drops trailing dots and spaces
    CHECK(named("") == "program.nc");
    CHECK(named("...") == "program.nc");
    for (const char* device : {"CON", "con", "Prn", "AUX", "NUL", "COM1", "com9", "LPT1", "LPT9", "COM0", "nul.txt"})
        CHECK(named(device) == "_" + std::string(device) + ".nc");
    CHECK(named("CONSOLE") == "CONSOLE.nc"); // only the exact device names
    CHECK(named("COM10") == "COM10.nc");
    // A very long name is cut, within every file system's limit.
    const std::string longName = nameOf("{setup}", [&] {
        NameFields l = fields();
        l.setupName = std::string(400, 'x');
        return l;
    }());
    CHECK(longName.size() == kMaxStemBytes + 3);
    // The notes say what was changed.
    f.setupName = "a/b";
    auto n = fileNameFor("{setup}", f);
    REQUIRE(n);
    REQUIRE(n->notes.size() == 1);
    CHECK_THAT(n->notes[0], ContainsSubstring("made safe"));
    f.setupName = "ab";
    n = fileNameFor("{setup}", f);
    REQUIRE(n);
    CHECK(n->notes.empty());
}

TEST_CASE("output: every name made from any text is valid everywhere (10,000 random names)", "[post][output]")
{
    std::mt19937 rng(47);
    std::uniform_int_distribution<int> len(0, 300), byte(0, 255);
    const std::string kept = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.()+,=#~";
    for (int i = 0; i < 10000; ++i) {
        std::string text(static_cast<std::size_t>(len(rng)), '\0');
        for (char& c : text)
            c = static_cast<char>(byte(rng));
        if (i % 3 == 0 && text.size() > 3)
            text.replace(0, 3, "COM"); // device-name prefixes now and then
        const std::string s = sanitizeFileName(text);
        INFO("input " << i);
        REQUIRE_FALSE(s.empty());
        CHECK(s.size() <= kMaxStemBytes + 1);
        CHECK(s.find_first_not_of(kept) == std::string::npos);
        CHECK(s.front() != ' ');
        CHECK(s.front() != '.');
        CHECK(s.back() != ' ');
        CHECK(s.back() != '.');
        std::string upper = s.substr(0, s.find('.'));
        std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
        });
        for (const char* device : {"CON", "PRN", "AUX", "NUL", "COM1", "LPT1"})
            CHECK(upper != device);
        CHECK(sanitizeFileName(s) == s); // stable: a safe name stays as it is
    }
}

TEST_CASE("output: the machine's name length limit cuts the stem, with a note", "[post][output]")
{
    auto n = fileNameFor("{doc}_{setup}", fields(), 8);
    REQUIRE(n);
    CHECK(n->name == "Bracket_.nc");
    CHECK(n->stem == "Bracket_");
    CHECK(n->extension == ".nc");
    REQUIRE_FALSE(n->notes.empty());
    CHECK_THAT(n->notes.back(), ContainsSubstring("cut to 8 characters"));
    n = fileNameFor("O{program:05}", fields(), 8);
    REQUIRE(n);
    CHECK(n->name == "O01001.nc"); // fits: unchanged, no note
    CHECK(n->notes.empty());
    // A cut never ends in a dot or a space.
    n = fileNameFor("{setup}", fields(), 3);
    REQUIRE(n);
    CHECK(n->stem == "Op");
}

// ======================================================================================================== local folder
TEST_CASE("output: a program is written to a local folder byte for byte, and nothing else is left", "[post][output]")
{
    TempDir dir;
    for (const bool crlf : {true, false}) {
        std::string text = sampleProgram();
        if (!crlf) {
            std::string lf;
            for (char c : text)
                if (c != '\r')
                    lf.push_back(c);
            text = lf;
        }
        const std::string name = crlf ? "crlf.nc" : "lf.nc";
        auto w = writeProgramFile(text, target(OverwriteMode::Refuse, name, dir.dir()), localOutputFs());
        REQUIRE(w);
        CHECK(w->path == dir.file(name));
        CHECK(w->fileName == name);
        CHECK_FALSE(w->replaced);
        CHECK(readAll(w->path) == text);
    }
    CHECK(listDir(dir.dir()) == std::vector<std::string>{"crlf.nc", "lf.nc"});
}

TEST_CASE("output: overwrite rules on a local folder: refuse, replace, new numbered name, ask", "[post][output]")
{
    TempDir dir;
    const std::string first = sampleProgram(5), second = sampleProgram(9);
    REQUIRE(writeProgramFile(first, target(OverwriteMode::Refuse, "O1001.nc", dir.dir()), localOutputFs()));

    // Ask: the caller learns the file is there before anything is written.
    auto there = targetExists(target(OverwriteMode::Refuse, "O1001.nc", dir.dir()), localOutputFs());
    REQUIRE(there);
    CHECK(*there);
    there = targetExists(target(OverwriteMode::Refuse, "O1002.nc", dir.dir()), localOutputFs());
    REQUIRE(there);
    CHECK_FALSE(*there);

    // Refuse: an error naming the file; the old program is unchanged.
    auto refused = writeProgramFile(second, target(OverwriteMode::Refuse, "O1001.nc", dir.dir()), localOutputFs());
    REQUIRE_FALSE(refused);
    CHECK(problemOf(refused.error()) == OutputProblem::Exists);
    CHECK(refused.error().code == ErrorCode::Locked);
    CHECK_THAT(refused.error().message, ContainsSubstring("O1001.nc already exists"));
    CHECK(readAll(dir.file("O1001.nc")) == first);

    // New name: the next free numbered one, each time.
    auto renamed = writeProgramFile(second, target(OverwriteMode::NewName, "O1001.nc", dir.dir()), localOutputFs());
    REQUIRE(renamed);
    CHECK(renamed->fileName == "O1001_2.nc");
    renamed = writeProgramFile(second, target(OverwriteMode::NewName, "O1001.nc", dir.dir()), localOutputFs());
    REQUIRE(renamed);
    CHECK(renamed->fileName == "O1001_3.nc");
    CHECK(readAll(dir.file("O1001_3.nc")) == second);
    CHECK(readAll(dir.file("O1001.nc")) == first);
    // Within the machine's name limit: the stem gives way to the number.
    OutputTarget limited = target(OverwriteMode::NewName, "O1001.nc", dir.dir());
    limited.maxStemLength = 5;
    renamed = writeProgramFile(second, limited, localOutputFs());
    REQUIRE(renamed);
    CHECK(renamed->fileName == "O10_2.nc");

    // Replace: atomically over the old one.
    auto replaced = writeProgramFile(second, target(OverwriteMode::Replace, "O1001.nc", dir.dir()), localOutputFs());
    REQUIRE(replaced);
    CHECK(replaced->replaced);
    CHECK(readAll(dir.file("O1001.nc")) == second);
    CHECK(listDir(dir.dir()) == std::vector<std::string>{"O1001.nc", "O1001_2.nc", "O1001_3.nc", "O10_2.nc"});
}

TEST_CASE("output: clear messages for a missing folder, a file path and a bad name", "[post][output]")
{
    TempDir dir;
    auto missing = writeProgramFile("x", target(OverwriteMode::Replace, "a.nc", dir.file("not here")), localOutputFs());
    REQUIRE_FALSE(missing);
    CHECK(problemOf(missing.error()) == OutputProblem::FolderMissing);
    CHECK_THAT(missing.error().message, ContainsSubstring("does not exist"));
    CHECK_THAT(missing.error().message, ContainsSubstring("not here"));
    writeAll(dir.file("plain"), "x");
    auto notFolder = writeProgramFile("x", target(OverwriteMode::Replace, "a.nc", dir.file("plain")), localOutputFs());
    REQUIRE_FALSE(notFolder);
    CHECK_THAT(notFolder.error().message, ContainsSubstring("is a file, not a folder"));
    auto bad = writeProgramFile("x", target(OverwriteMode::Replace, "sub/a.nc", dir.dir()), localOutputFs());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    auto none = writeProgramFile("x", target(OverwriteMode::Replace, "a.nc", ""), localOutputFs());
    REQUIRE_FALSE(none);
    CHECK_THAT(none.error().message, ContainsSubstring("No output folder"));
}

#ifdef _WIN32
TEST_CASE("output: a destination held open by another program is reported as locked; the old file stays",
          "[post][output]")
{
    TempDir dir;
    const std::string path = dir.file("O1001.nc");
    writeAll(path, "old");
    // A DNC sender or an editor with the file open and no delete sharing.
    std::wstring wide = native(path).wstring();
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    auto w = writeProgramFile("new", target(OverwriteMode::Replace, "O1001.nc", dir.dir()), localOutputFs());
    CloseHandle(h);
    REQUIRE_FALSE(w);
    CHECK(problemOf(w.error()) == OutputProblem::Locked);
    CHECK_THAT(w.error().message, ContainsSubstring("open in another program"));
    CHECK_THAT(w.error().message, ContainsSubstring("old file is unchanged"));
    CHECK(readAll(path) == "old");
    CHECK(listDir(dir.dir()) == std::vector<std::string>{"O1001.nc"}); // the temporary file was removed
    // Closed again: it goes through.
    REQUIRE(writeProgramFile("new", target(OverwriteMode::Replace, "O1001.nc", dir.dir()), localOutputFs()));
    CHECK(readAll(path) == "new");
}
#endif

TEST_CASE("output: cancel before the rename writes nothing; a cancel after the commit is too late", "[post][output]")
{
    FakeFs f;
    f.folders.insert(kShare);
    f.files[std::string(kShare) + "\\O1001.nc"] = "old";
    CancelSource source;
    CancelToken token = source.token();
    f.onWrite = [&] { source.cancel(); };
    const std::string big(std::size_t(3) << 20, 'G'); // several chunks
    auto w = writeProgramFile(big, target(OverwriteMode::Replace), f, &token);
    REQUIRE_FALSE(w);
    CHECK(w.error().code == ErrorCode::Cancelled);
    CHECK(problemOf(w.error()) == OutputProblem::Cancelled);
    CHECK(f.files.at(std::string(kShare) + "\\O1001.nc") == "old");
    CHECK(f.temporaries() == 0);
    // The commit closes the token: a later cancel changes nothing.
    CancelSource late;
    CancelToken lateToken = late.token();
    FakeFs g;
    g.folders.insert(kShare);
    auto ok = writeProgramFile("new", target(OverwriteMode::Replace), g, &lateToken);
    REQUIRE(ok);
    CHECK_FALSE(late.cancel());
    CHECK(g.files.at(std::string(kShare) + "\\O1001.nc") == "new");
}

// ======================================================================================================== faults (fake)
TEST_CASE("output: a full disk mid-write leaves no partial program and the old one intact", "[post][output][fake]")
{
    FakeFs f;
    f.folders.insert(kShare);
    const std::string old = sampleProgram(3);
    f.files[std::string(kShare) + "\\O1001.nc"] = old;
    f.failWriteAfter = 100;
    f.writeProblem = OutputProblem::DiskFull;
    auto w = writeProgramFile(sampleProgram(200), target(OverwriteMode::Replace), f);
    REQUIRE_FALSE(w);
    CHECK(problemOf(w.error()) == OutputProblem::DiskFull);
    CHECK(f.files.at(std::string(kShare) + "\\O1001.nc") == old);
    CHECK(f.temporaries() == 0); // the partial temporary file was removed
    CHECK(f.renames == 0);
    // A new file: nothing at all appears.
    FakeFs g;
    g.folders.insert(kShare);
    g.failWriteAfter = 10;
    REQUIRE_FALSE(writeProgramFile(sampleProgram(200), target(OverwriteMode::Refuse), g));
    CHECK(g.files.empty());
}

TEST_CASE("output: a share that vanishes mid-write: a clear message, nothing loadable left", "[post][output][fake]")
{
    FakeFs f;
    f.folders.insert(kShare);
    f.files[std::string(kShare) + "\\O1001.nc"] = "old";
    f.failWriteAfter = 64;
    f.writeProblem = OutputProblem::Unreachable;
    f.vanishOnWrite = true;
    auto w = writeProgramFile(sampleProgram(200), target(OverwriteMode::Replace), f);
    REQUIRE_FALSE(w);
    CHECK(problemOf(w.error()) == OutputProblem::Unreachable);
    // The temporary file could not be removed (the share is gone), but it is a .tmp no control loads, and the old
    // program is untouched.
    CHECK(f.programs() == std::vector<std::string>{"O1001.nc"});
    CHECK(f.files.at(std::string(kShare) + "\\O1001.nc") == "old");
    CHECK(f.renames == 0);
    // Gone before the start: said so at once.
    FakeFs g;
    g.folderProblem = OutputProblem::Unreachable;
    auto unreachable = writeProgramFile("x", target(OverwriteMode::Replace), g);
    REQUIRE_FALSE(unreachable);
    CHECK(problemOf(unreachable.error()) == OutputProblem::Unreachable);
    CHECK_THAT(unreachable.error().message, ContainsSubstring("cannot be reached"));
}

TEST_CASE("output: a failed flush and a locked destination leave the old program", "[post][output][fake]")
{
    FakeFs f;
    f.folders.insert(kShare);
    const std::string path = std::string(kShare) + "\\O1001.nc";
    f.files[path] = "old";
    f.failFlush = true;
    REQUIRE_FALSE(writeProgramFile("new", target(OverwriteMode::Replace), f));
    CHECK(f.files.at(path) == "old");
    CHECK(f.temporaries() == 0);
    f.failFlush = false;
    f.locked.insert(path);
    auto w = writeProgramFile("new", target(OverwriteMode::Replace), f);
    REQUIRE_FALSE(w);
    CHECK(problemOf(w.error()) == OutputProblem::Locked);
    CHECK(f.files.at(path) == "old");
    CHECK(f.temporaries() == 0);
}

TEST_CASE("output: a file that appears while writing is never replaced unless the rule says so", "[post][output][fake]")
{
    const std::string path = std::string(kShare) + "\\O1001.nc";
    FakeFs f;
    f.folders.insert(kShare);
    f.appearOnRename = path;
    auto refused = writeProgramFile("mine", target(OverwriteMode::Refuse), f);
    REQUIRE_FALSE(refused);
    CHECK(problemOf(refused.error()) == OutputProblem::Exists);
    CHECK_THAT(refused.error().message, ContainsSubstring("appeared"));
    CHECK(f.files.at(path) == "theirs");
    CHECK(f.temporaries() == 0);
    FakeFs g;
    g.folders.insert(kShare);
    g.appearOnRename = path;
    auto renamed = writeProgramFile("mine", target(OverwriteMode::NewName), g);
    REQUIRE(renamed);
    CHECK(renamed->fileName == "O1001_2.nc");
    CHECK(g.files.at(path) == "theirs");
    CHECK(g.files.at(std::string(kShare) + "\\O1001_2.nc") == "mine");
    // A temporary name someone else holds is skipped.
    FakeFs h;
    h.folders.insert(kShare);
    h.tempCollisions = 2;
    REQUIRE(writeProgramFile("mine", target(OverwriteMode::Replace), h));
    CHECK(h.files.at(path) == "mine");
}

// ======================================================================================================== bytes
TEST_CASE("output: writeProgram writes every golden program byte for byte (OutputRule, both overwrite settings)",
          "[post][output][golden]")
{
    TempDir dir;
    int checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(native(RAY_GOLDEN_GCODE_ROOT))) {
        if (!entry.is_regular_file() || entry.path().extension() != ".nc")
            continue;
        const std::string golden = readAll(utf8(entry.path()));
        REQUIRE_FALSE(golden.empty());
        PostOutput out;
        out.text = golden;
        PostInfo info{};
        info.id = utf8(entry.path().parent_path().filename());
        info.fileExtension = ".nc";
        cam::Setup setup{};
        setup.name = utf8(entry.path().stem());
        setup.programNumber = 1001;
        PostJob job{};
        job.setup = &setup;
        job.documentName = "golden";
        const OutputRule rule{dir.dir(), "{post}_{setup}", false};
        auto path = writeProgram(out, info, job, rule);
        REQUIRE(path);
        CHECK(readAll(*path) == golden);
        // Again: refused while overwrite is false, replaced (same bytes) when true.
        CHECK_FALSE(writeProgram(out, info, job, rule));
        auto again = writeProgram(out, info, job, OutputRule{dir.dir(), "{post}_{setup}", true});
        REQUIRE(again);
        CHECK(*again == *path);
        CHECK(readAll(*again) == golden);
        ++checked;
    }
    CHECK(checked > 100);
    for (const std::string& name : listDir(dir.dir()))
        CHECK_FALSE(isTemporaryOutputName(name));
}

TEST_CASE("output: a posted program's file equals PostOutput::text with either line ending property",
          "[post][output]")
{
    TempDir dir;
    for (const char* ending : {"crlf", "lf"}) {
        test::Program p = test::linearProgram(50);
        p.properties["lineEnding"] = ending;
        QuickJsPostEngine engine;
        JobContext ctx;
        auto out = engine.run(test::postPath("iso-test.post.js"), p.job(), ctx);
        REQUIRE(out);
        REQUIRE_FALSE(out->text.empty());
        CHECK((out->text.find("\r\n") != std::string::npos) == (std::string(ending) == "crlf"));
        auto info = engine.inspect(test::postPath("iso-test.post.js"));
        REQUIRE(info);
        const PostJob job = p.job();
        auto path = writeProgram(*out, *info, job, OutputRule{dir.dir(), std::string("{program}_") + ending, false});
        REQUIRE(path);
        CHECK(readAll(*path) == out->text);
        CHECK(fs::file_size(native(*path)) == out->text.size());
    }
}

// ======================================================================================================== crash
TEST_CASE("output: a writer killed mid-write or just before the rename leaves the old program whole",
          "[post][output][kill]")
{
    TempDir dir(true);
    for (const char* mode : {"write", "rename"}) {
        INFO("killed at " << mode);
        const std::string name = std::string("kill_") + mode + ".nc";
        const std::string old = sampleProgram(10);
        writeAll(dir.file(name), old);
        const std::string marker = dir.file(std::string("marker_") + mode);
        Child child({RAY_POST_OUTPUT_HELPER, mode, dir.dir(), name, marker, std::to_string(std::size_t(2) << 20)});
        REQUIRE(waitForFile(marker, 30000));
        child.kill();
        CHECK(readAll(dir.file(name)) == old);
        // Only the temporary file may be left, and its name is no program's.
        for (const std::string& f : listDir(dir.dir())) {
            if (f == name || f.rfind("marker_", 0) == 0 || f.rfind("kill_", 0) == 0)
                continue;
            CHECK(isTemporaryOutputName(f));
            CHECK(f.substr(f.size() - 4) == ".tmp");
        }
    }
}

// ======================================================================================================== SMB
TEST_CASE("output: on a loopback SMB share: write, atomic replace, exists, new name, locked file, vanished share",
          "[post][output][smb]")
{
    TempDir local(true);
    std::string why;
    const auto share = smbFolder(local.dir(), &why);
    if (!share)
        SKIP("SMB not exercised: " << why);
    INFO("share folder " << *share);
    const std::string text = sampleProgram(500);
    auto w = writeProgramFile(text, target(OverwriteMode::Refuse, "O1001.nc", *share), localOutputFs());
    REQUIRE(w);
    CHECK(readAll(local.file("O1001.nc")) == text); // the same file, seen locally
    CHECK(readAll(w->path) == text);
    // Exists, refused; ask sees it.
    auto there = targetExists(target(OverwriteMode::Refuse, "O1001.nc", *share), localOutputFs());
    REQUIRE(there);
    CHECK(*there);
    auto refused = writeProgramFile("x", target(OverwriteMode::Refuse, "O1001.nc", *share), localOutputFs());
    REQUIRE_FALSE(refused);
    CHECK(problemOf(refused.error()) == OutputProblem::Exists);
    // New name.
    auto renamed = writeProgramFile("second", target(OverwriteMode::NewName, "O1001.nc", *share), localOutputFs());
    REQUIRE(renamed);
    CHECK(renamed->fileName == "O1001_2.nc");
    // Atomic replace: a reader polling the file meanwhile only ever sees the whole old or the whole new program.
    const std::string next = sampleProgram(900);
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0}, torn{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const std::string seen = readAll(local.file("O1001.nc"));
            if (!seen.empty() && seen != text && seen != next)
                ++torn;
            ++reads;
        }
    });
    for (int i = 0; i < 20; ++i) {
        auto r = writeProgramFile(i % 2 == 0 ? next : text, target(OverwriteMode::Replace, "O1001.nc", *share),
                                  localOutputFs());
        if (!r)
            CHECK(problemOf(r.error()) == OutputProblem::Locked); // the reader held it at that instant
    }
    stop = true;
    reader.join();
    CHECK(torn == 0);
    CHECK(reads > 0);
#ifdef _WIN32
    // A destination held open over the share without delete sharing: locked, the old file stays.
    const std::string before = readAll(local.file("O1001.nc"));
    HANDLE h = CreateFileW(native(joinPath(*share, "O1001.nc")).wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    auto locked = writeProgramFile("new", target(OverwriteMode::Replace, "O1001.nc", *share), localOutputFs());
    CloseHandle(h);
    REQUIRE_FALSE(locked);
    CHECK(problemOf(locked.error()) == OutputProblem::Locked);
    CHECK_THAT(locked.error().message, ContainsSubstring("open in another program"));
    CHECK(readAll(local.file("O1001.nc")) == before);
    // A share that is not there (the server answers, the share name does not exist): unreachable, said plainly.
    auto vanished = writeProgramFile(
        "x", target(OverwriteMode::Replace, "O1001.nc", "\\\\localhost\\RayCadNoSuchShare$\\cnc"), localOutputFs());
    REQUIRE_FALSE(vanished);
    CHECK(problemOf(vanished.error()) == OutputProblem::Unreachable);
    CHECK_THAT(vanished.error().message, ContainsSubstring("cannot be reached"));
#endif
    for (const std::string& f : listDir(local.dir()))
        CHECK_FALSE(isTemporaryOutputName(f));
}

TEST_CASE("output: on a loopback SMB share a writer killed mid-write leaves the old program whole",
          "[post][output][smb][kill]")
{
    TempDir local(true);
    std::string why;
    const auto share = smbFolder(local.dir(), &why);
    if (!share)
        SKIP("SMB not exercised: " << why);
    for (const char* mode : {"write", "rename"}) {
        INFO("killed at " << mode);
        const std::string name = std::string("smb_") + mode + ".nc";
        const std::string old = sampleProgram(10);
        writeAll(local.file(name), old);
        const std::string marker = local.file(std::string("marker_") + mode);
        Child child({RAY_POST_OUTPUT_HELPER, mode, *share, name, marker, std::to_string(std::size_t(2) << 20)});
        REQUIRE(waitForFile(marker, 30000));
        child.kill();
        CHECK(readAll(local.file(name)) == old);
        for (const std::string& f : listDir(local.dir())) {
            if (f.rfind("smb_", 0) == 0 || f.rfind("marker_", 0) == 0)
                continue;
            CHECK(isTemporaryOutputName(f));
        }
    }
}
