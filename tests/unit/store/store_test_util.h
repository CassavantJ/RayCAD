// Shared pieces of the store tests and their helper process: temp folders, child processes, sample documents.
#pragma once
#include <ray/document.h>
#include <ray/store.h>

#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace storetest {

// A new folder under the system temp folder whose name contains spaces and non-ASCII characters, so every test
// exercises UTF-8 paths. Removed with its contents at destruction.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& dir() const { return dir_; }
    std::string path(std::string_view name) const; // UTF-8

private:
    std::string dir_;
};

// A child process running the test helper (store_helper.cpp). Killed at destruction if still running.
class Process {
public:
    static Process spawnHelper(const std::vector<std::string>& args); // UTF-8 arguments
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    ~Process();
    void kill(); // hard kill (TerminateProcess / SIGKILL), then wait
    int wait();  // exit code
    bool running();
    std::int64_t pid() const { return pid_; }

private:
    Process() = default;
    void* handle_ = nullptr; // Windows process handle
    std::int64_t pid_ = 0;
    bool done_ = true;
    int exitCode_ = 0;
};

// Files, by UTF-8 path.
bool exists(const std::string& path);
std::uintmax_t fileSize(const std::string& path);
std::string readFile(const std::string& path);
void writeFile(const std::string& path, std::string_view bytes);
// Writes beside the target, then renames, so a reader sees all of it or nothing.
void writeFileAtomic(const std::string& path, std::string_view bytes);
void removeFile(const std::string& path);
void removeDocument(const std::string& path); // the document, its -journal and its lock file
void sleepMs(int ms);
std::int64_t nowMs(); // steady clock

// Random hex text of `length` characters (compressible about 2:1, like real JSON payloads).
std::string randomHex(std::mt19937_64& rng, std::size_t length);
std::vector<std::byte> randomBytes(std::mt19937_64& rng, std::size_t length);
ray::Hash256 hashOf(std::span<const std::byte> bytes);

// A document with `features` features, each with a props payload of `payloadChars` random hex characters.
ray::doc::DocState makeState(std::mt19937_64& rng, int features, std::size_t payloadChars);

// Lines "S <seq>", "B <seq>", "A <seq>" written by the helper's writer: the head it found at open, the save it
// began, the save that returned. Only complete lines count.
struct Acks {
    std::int64_t start = INT64_MIN, begun = INT64_MIN, acked = INT64_MIN;
};
Acks readAcks(const std::string& path);

// "save <n>" -> n; -1 if the message has another form.
std::int64_t saveNumber(const std::string& message);

// The lock timing the tests use: heartbeat 200 ms, stale after 1.5 s. Set in this process and passed to helpers.
inline constexpr std::int64_t kTestHeartbeatMs = 200;
inline constexpr std::int64_t kTestStaleMs = 1500;
void useTestLockTiming();

// Breaks the stale lock a killed helper left on `doc`, retrying for up to 5 s; false if it stays locked.
bool breakLockOfDeadWriter(const std::string& doc);

// ---------- Recovery journal tests ----------
// A random version 4 UUID string.
std::string randomUuid(std::mt19937_64& rng);
// A random command that is valid for `state`: feature.add, feature.edit, feature.remove, feature.reorder or
// feature.suppress, with props of up to `payloadChars` random characters.
ray::doc::Command randomCommand(std::mt19937_64& rng, const ray::doc::DocState& state, std::size_t payloadChars = 200);

enum class Step { Execute, Undo, Redo, Save };
// One random step on `doc`, recorded in `journal` as the app would: mostly an executed command, sometimes an undo or a
// redo, and, with a `file`, `savePercent` percent of the time a save followed by markSaved and markClean.
// `before(state)` runs after the step and before its journal record (for a save: before saving). Throws
// std::runtime_error on any failure.
Step randomStep(std::mt19937_64& rng, ray::doc::Document& doc, ray::store::IRecoveryJournal& journal,
                ray::store::IDocumentFile* file, int savePercent,
                const std::function<void(const ray::doc::DocStatePtr&)>& before = {});

} // namespace storetest
