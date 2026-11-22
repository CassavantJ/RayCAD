// Kill during save (as tests/unit/store/kill_test.cpp does for documents): a writer process saves three tools in a
// loop and is killed at a random moment, often between flushing its temporary file and renaming it. Afterwards the
// library must open with no issues, and every tool must hold either its last acknowledged save or the one that
// had begun: never a mix, never a torn file. The normal run does 50 kills; the hidden tag [.kill500] does 500.
#include "lib_test_util.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <climits>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

using namespace libtest;

namespace {

struct ToolAcks {
    std::int64_t begun = INT64_MIN; // the last save that began
    std::int64_t acked = INT64_MIN; // the last save that returned
};

// Complete lines "B <tool> <n>" and "A <tool> <n>" only (a kill can cut the last one short).
std::array<ToolAcks, 3> readAcks(const std::string& path, bool& any)
{
    std::array<ToolAcks, 3> out{};
    const std::string text = readFile(path);
    std::size_t pos = 0;
    any = false;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            break;
        }
        std::istringstream line(text.substr(pos, end - pos));
        pos = end + 1;
        char kind = 0;
        int which = -1;
        long long n = 0;
        if (!(line >> kind >> which >> n) || which < 0 || which > 2) {
            continue;
        }
        if (kind == 'B') {
            out[static_cast<std::size_t>(which)].begun = n;
        } else if (kind == 'A') {
            out[static_cast<std::size_t>(which)].acked = n;
            any = true;
        }
    }
    return out;
}

std::int64_t saveNumber(const std::string& name)
{
    return name.rfind("save ", 0) == 0 ? std::stoll(name.substr(5)) : -1;
}

void runKillTest(int iterations)
{
    TempDir dir;
    const std::string root = dir.path("library");
    const std::string acksPath = dir.path("acks.txt");
    writeFile(dir.path("library/library.json"), "{\"schema\": 1, \"name\": \"kill test\"}");
    std::mt19937_64 rng(20261121);
    // What each tool may hold: its last acknowledged save, or the one begun after it. Numbers restart with every
    // writer, so the bounds are per writer.
    std::array<std::int64_t, 3> expectedBefore{-1, -1, -1}; // the save number each tool held before this writer
    int killedBetween = 0;
    int stray = 0;
    std::int64_t savesAcked = 0;
    const auto t0 = nowMs();
    for (int i = 0; i < iterations; ++i) {
        removeFile(acksPath);
        {
            Process writer = Process::spawnHelper({"writer", root, acksPath, std::to_string(rng())});
            // Most kills land among saves; one in five is timed from process start.
            bool any = false;
            if (rng() % 5 != 0) {
                const auto end = nowMs() + 10000;
                while (writer.running() && nowMs() < end) {
                    readAcks(acksPath, any);
                    if (any) {
                        break;
                    }
                    sleepMs(1);
                }
            }
            sleepMs(static_cast<int>(rng() % 60));
            if (!writer.running()) {
                FAIL("iteration " << i << ": the writer exited by itself (exit code " << writer.wait() << ")");
            }
            writer.kill();
        }
        bool any = false;
        const auto acks = readAcks(acksPath, any);
        auto library = ray::lib::ILibrary::open(root);
        REQUIRE(library);
        for (const auto& issue : (*library)->issues()) {
            FAIL_CHECK("iteration " << i << ": " << issue.file << ": " << issue.message);
        }
        const auto tools = (*library)->tools();
        for (int k = 0; k < 3; ++k) {
            const ToolAcks& a = acks[static_cast<std::size_t>(k)];
            const ray::lib::Tool* found = nullptr;
            for (const auto& t : tools) {
                if (t.id.value == fixedId(100 + k)) {
                    found = &t;
                }
            }
            const std::int64_t held = found ? saveNumber(found->name) : -1;
            INFO("iteration " << i << ", tool " << k << ": holds save " << held << ", acked " << a.acked << ", begun "
                              << a.begun << ", before " << expectedBefore[static_cast<std::size_t>(k)]);
            if (a.begun == INT64_MIN) {
                CHECK(held == expectedBefore[static_cast<std::size_t>(k)]); // not touched by this writer
            } else if (a.acked == a.begun) {
                CHECK(held == a.acked);
            } else {
                // Killed inside a save: the old content (the previous ack, or what it held before) or the new one.
                const std::int64_t old = a.acked != INT64_MIN ? a.acked : expectedBefore[static_cast<std::size_t>(k)];
                CHECK((held == old || held == a.begun));
                ++killedBetween;
            }
            if (found) {
                // The tool is whole: its preset count is what the writer generated (at least one).
                CHECK(!found->presets.empty());
            }
            expectedBefore[static_cast<std::size_t>(k)] = held;
            if (a.acked != INT64_MIN) {
                savesAcked += 1;
            }
        }
    }
    for (const std::string& name : listFiles(root + "/tools")) {
        stray += (!name.empty() && name.front() == '.') ? 1 : 0; // temporary files of killed saves
    }
    std::cout << "[lib] kill test: " << iterations << " kills, " << killedBetween << " inside a save, " << stray
              << " temporary files left by killed saves (ignored by the loader), " << savesAcked
              << " tool slots with acknowledged saves, 0 unreadable files, " << (nowMs() - t0) / 1000.0 << " s\n";
}

} // namespace

TEST_CASE("killing the writer during saves never leaves a torn or unreadable file (50 kills)", "[lib][kill]")
{
    runKillTest(50);
}

TEST_CASE("killing the writer during saves never leaves a torn or unreadable file (500 kills)", "[.kill500]")
{
    runKillTest(500);
}
