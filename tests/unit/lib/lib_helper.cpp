// The helper process of the library tests.
//   writer <root> <acks file> <seed>   saves three tools in a loop until killed. Before each save it appends
//                                       "B <tool> <n>" to the acks file and after it "A <tool> <n>" (flushed), where
//                                       the tool's name becomes "save <n>". Sizes vary from save to save.
//   rewrite <file> <from> <to> <keep>   replaces the first <from> in the file with <to> in place (no rename, as an
//                                       editor that overwrites does); with <keep> = 1 the old modification time is
//                                       put back, as on a file system whose time stamps are too coarse to differ.
#include "fileio.h"
#include "lib_test_util.h"

#include <ray/libraries.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h> // after windows.h
#endif

namespace {

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

std::mt19937_64 gRng;

void jitter()
{
    std::this_thread::sleep_for(std::chrono::microseconds(gRng() % 3000));
}

int writer(const std::string& root, const std::string& acksPath, std::uint64_t seed)
{
    gRng.seed(seed);
    ray::lib::detail::fio::setBeforeRenameHook(&jitter);
    auto library = ray::lib::ILibrary::open(root);
    if (!library) {
        std::cerr << "open: " << library.error().message << "\n";
        return 2;
    }
    if (!(*library)->issues().empty()) {
        std::cerr << "the library has issues at start: " << (*library)->issues().front().file << ": "
                  << (*library)->issues().front().message << "\n";
        return 3;
    }
    std::ofstream acks(ray::lib::detail::fio::toFsPath(acksPath), std::ios::binary | std::ios::app);
    if (!acks) {
        return 4;
    }
    for (std::int64_t n = 1;; ++n) {
        const int which = static_cast<int>(gRng() % 3);
        ray::lib::Tool t = libtest::sampleTool(100 + which);
        t.name = "save " + std::to_string(n);
        const int presets = 1 + static_cast<int>(gRng() % 200);
        t.presets.clear();
        for (int i = 0; i < presets; ++i) {
            t.presets.push_back({ray::MaterialId{libtest::fixedId(1000 + i)}, "rough", 100.0 + i, 0.05, 0.1, 1, 2});
        }
        acks << "B " << which << " " << n << "\n" << std::flush;
        auto saved = (*library)->save(t);
        if (!saved) {
            std::cerr << "save " << n << ": " << saved.error().message << "\n";
            return 5;
        }
        acks << "A " << which << " " << n << "\n" << std::flush;
    }
}

int rewrite(const std::string& file, const std::string& from, const std::string& to, bool keepTime)
{
    const auto path = ray::lib::detail::fio::toFsPath(file);
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    std::string bytes = libtest::readFile(file);
    const std::size_t at = bytes.find(from);
    if (at == std::string::npos) {
        return 2;
    }
    bytes.replace(at, from.size(), to);
    {
        std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(0);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::resize_file(path, bytes.size(), ec);
    if (keepTime) {
        std::filesystem::last_write_time(path, time, ec);
        if (ec) {
            return 3;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args = utf8Arguments(argc, argv);
    const std::string mode = args.size() > 1 ? args[1] : "";
    if (mode == "writer" && args.size() == 5) {
        return writer(args[2], args[3], std::stoull(args[4]));
    }
    if (mode == "rewrite" && args.size() == 6) {
        return rewrite(args[2], args[3], args[4], args[5] == "1");
    }
    std::cerr << "usage: lib_helper writer <root> <acks> <seed> | rewrite <file> <from> <to> <keep>\n";
    return 1;
}
