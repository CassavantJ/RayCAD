// Child process for the store tests: things that must run in a process the test can kill.
//
//   writer <doc> <acks> <pauseMs> <bigPercent> <seed>   save in a loop, logging "S/B/A <seq>" lines to <acks>
//   hold   <doc> <ready> <heartbeat 0|1>                  open for writing, write "rw"/"ro" to <ready>, then wait
//   race   <doc> <go> <result> <done>                     wait for <go>, break the stale lock, open for writing,
//                                                         write "rw", "ro" or "err ..." to <result>, wait for <done>
// All paths are UTF-8. Lock timing is the tests' (store_test_util.h).
#include "lock.h"
#include "store_test_util.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h> // after windows.h
#endif

using namespace storetest;
using ray::store::IDocumentFile;
using ray::store::OpenInfo;
using ray::store::OpenMode;

namespace {

std::FILE* openAppend(const std::string& path)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), n);
    return _wfopen(wide.c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

void say(std::FILE* f, char tag, std::int64_t seq)
{
    std::fprintf(f, "%c %lld\n", tag, static_cast<long long>(seq));
    std::fflush(f); // reaches the OS, which keeps it through a kill
}

// Opens the document for writing, creating it if needed and breaking a lock whose owner is dead.
std::unique_ptr<IDocumentFile> openForWrite(const std::string& doc)
{
    for (int attempt = 0; attempt < 400; ++attempt) {
        // A dead writer's lock is broken first (also one left by a writer killed while creating the file), so the
        // open goes straight to the hot-journal rollback.
        if (auto lock = ray::store::detail::readLock(doc); lock && lock->stale) {
            (void)ray::store::breakStaleLock(doc);
        }
        if (!exists(doc)) {
            auto created = IDocumentFile::create(doc);
            if (created) {
                return std::move(*created);
            }
        } else {
            OpenInfo info{};
            auto opened = IDocumentFile::open(doc, OpenMode::ReadWrite, &info);
            if (opened && !info.openedReadOnlyBecauseLocked) {
                return std::move(*opened);
            }
            if (opened) {
                opened->reset();
                if (info.lock && info.lock->stale) {
                    (void)ray::store::breakStaleLock(doc);
                }
            } else {
                std::fprintf(stderr, "helper: open failed: %s\n", opened.error().message.c_str());
                if (opened.error().code != ray::ErrorCode::Locked) {
                    std::exit(4);
                }
            }
        }
        sleepMs(25);
    }
    std::fprintf(stderr, "helper: could not open for writing\n");
    std::exit(3);
}

int writer(const std::vector<std::string>& args)
{
    const std::string doc = args.at(0);
    std::FILE* acks = openAppend(args.at(1));
    const int pauseMs = std::atoi(args.at(2).c_str());
    const int bigPercent = std::atoi(args.at(3).c_str());
    std::mt19937_64 rng(std::strtoull(args.at(4).c_str(), nullptr, 10));
    auto file = openForWrite(doc);

    ray::doc::DocState state;
    std::int64_t seq = -1;
    if (auto head = file->loadHead()) {
        state = **head;
        auto log = file->log("main", 1);
        if (!log || log->empty()) {
            return 5;
        }
        seq = saveNumber(log->front().message);
    } else {
        state = makeState(rng, 16, 64);
    }
    say(acks, 'S', seq);
    ray::JobContext ctx;
    for (;;) {
        IDocumentFile::SaveRequest request;
        const int changes = 1 + static_cast<int>(rng() % 4);
        for (int i = 0; i < changes; ++i) {
            auto& feature = state.features[rng() % state.features.size()];
            feature.propsJson = "{\"payload\":\"" + randomHex(rng, 512 + rng() % (16 * 1024)) + "\"}";
        }
        std::size_t size = 1024 + rng() % (256 * 1024);
        if (bigPercent > 0 && static_cast<int>(rng() % 100) < bigPercent) {
            // Larger than SQLite's page cache: forces database writes before COMMIT (a hot journal on a kill).
            size = (2u << 20) + rng() % (4u << 20);
        }
        auto blob = randomBytes(rng, size);
        ray::Hash256 input;
        for (auto& b : input.bytes) {
            b = static_cast<std::uint8_t>(rng());
        }
        request.derivedIndex[input] = hashOf(blob);
        request.derived.emplace_back(ray::store::ObjectKind::Mesh, std::move(blob));
        request.state = std::make_shared<const ray::doc::DocState>(state);
        request.message = "save " + std::to_string(seq + 1);
        request.author = "store test";
        say(acks, 'B', seq + 1);
        auto saved = file->save(request, ctx);
        if (!saved) {
            std::fprintf(stderr, "helper: save failed: %s\n", saved.error().message.c_str());
            return 6;
        }
        ++seq;
        say(acks, 'A', seq);
        if (pauseMs > 0) {
            sleepMs(pauseMs);
        }
    }
}

int hold(const std::vector<std::string>& args)
{
    const std::string doc = args.at(0);
    const bool beat = args.at(2) == "1";
    if (!beat) {
        ray::store::detail::setLockTiming({1'000'000'000, kTestStaleMs});
    }
    OpenInfo info{};
    auto file = IDocumentFile::open(doc, OpenMode::ReadWrite, &info);
    if (!file) {
        writeFileAtomic(args.at(1), "err " + file.error().message);
        return 4;
    }
    writeFileAtomic(args.at(1), info.openedReadOnlyBecauseLocked ? "ro" : "rw");
    for (;;) {
        sleepMs(1000);
    }
}

int race(const std::vector<std::string>& args)
{
    const std::string doc = args.at(0);
    while (!exists(args.at(1))) {
        sleepMs(1);
    }
    std::string result;
    auto broken = ray::store::breakStaleLock(doc);
    OpenInfo info{};
    auto file = IDocumentFile::open(doc, OpenMode::ReadWrite, &info);
    if (!file) {
        result = "err " + file.error().message;
    } else {
        result = info.openedReadOnlyBecauseLocked ? "ro" : "rw";
    }
    result += broken ? " broke" : " locked";
    writeFileAtomic(args.at(2), result);
    while (!exists(args.at(3))) {
        sleepMs(5);
    }
    return 0;
}

std::vector<std::string> utf8Arguments(int argc, char** argv)
{
    std::vector<std::string> out;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; i < count; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), n, nullptr, nullptr);
        }
        out.push_back(std::move(arg));
    }
    LocalFree(wide);
#else
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i]);
    }
#endif
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args = utf8Arguments(argc, argv);
    if (args.size() < 2) {
        return 2;
    }
    useTestLockTiming();
    const std::string command = args[1];
    const std::vector<std::string> rest(args.begin() + 2, args.end());
    try {
        if (command == "writer") {
            return writer(rest);
        }
        if (command == "hold") {
            return hold(rest);
        }
        if (command == "race") {
            return race(rest);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "helper: %s\n", e.what());
        return 9;
    }
    return 2;
}
