// Determinism of the adaptive engine (task T-060; PLAN.md section 8): byte-identical output over repeats in one
// process, over 1, 2, 16 and 32 concurrent runs (the 32 under a time limit: upstream hung once and crashed once there),
// over 20 processes, and against the integer goldens in tests/golden/adaptive2d, which the Linux job compares too.
//
// GOLDENS. tests/golden/adaptive2d/p2_20.txt is the whole toText dump of S5's p2 at 20% load; hashes.txt holds, per
// S5 pocket and load, the BLAKE3 of the dump and its size. To make them again after a reviewed change: set
// RAY_UPDATE_ADAPTIVE_GOLDENS=1 and run ray_cam_adaptive_tests "[golden]" (quick cases) and "[golden][slow]".
#include "adaptive_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::cam::adaptive2d;
using namespace ray::cam::adaptive2d::test;

namespace {

std::string goldenPath(const std::string& name)
{
    return std::string(RAY_ADAPTIVE_GOLDEN_DIR) + "/" + name;
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_ADAPTIVE_GOLDENS");
    return v != nullptr && std::string(v) == "1";
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string textOf(const std::string& id, double stepover)
{
    auto out = runCase(id, stepover);
    REQUIRE(out);
    return toText(*out);
}

std::string loadName(double stepover)
{
    return std::to_string(int(stepover * 100 + 0.5));
}

// hashes.txt: one line "<case> <load%> <blake3> <bytes>" per run, sorted.
std::map<std::string, std::string> readHashes()
{
    std::map<std::string, std::string> m;
    std::istringstream in(readFile(goldenPath("hashes.txt")));
    std::string id, load, hash, bytes;
    while (in >> id >> load >> hash >> bytes) {
        m[id + " " + load] = hash + " " + bytes;
    }
    return m;
}

void checkHashes(const std::vector<std::string>& ids)
{
    std::map<std::string, std::string> golden = readHashes();
    for (const std::string& id : ids) {
        for (double s : {0.1, 0.2}) {
            const std::string text = textOf(id, s);
            const std::string key = id + " " + loadName(s);
            const std::string value = hashText(text) + " " + std::to_string(text.size());
            if (updating()) {
                golden[key] = value;
            } else {
                INFO("golden hash of " << key);
                CHECK(golden[key] == value);
            }
        }
    }
    if (updating()) {
        std::ofstream out(goldenPath("hashes.txt"), std::ios::binary | std::ios::trunc);
        for (const auto& [k, v] : golden) {
            out << k << " " << v << "\n";
        }
    }
}

// Each of `jobs` threads runs every case; all results must equal the single-threaded ones.
void concurrent(int jobs, const std::vector<std::string>& ids, double stepover,
                const std::vector<std::string>& expected, double limitSeconds)
{
    std::vector<std::vector<std::string>> got(jobs);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < jobs; t++) {
        threads.emplace_back([&, t] {
            for (const std::string& id : ids) {
                auto out = runCase(id, stepover);
                got[t].push_back(out ? hashText(toText(*out)) : "error: " + out.error().message);
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN(jobs << " concurrent runs of " << ids.size() << " pockets took " << seconds << " s");
    CHECK(seconds < limitSeconds);
    for (int t = 0; t < jobs; t++) {
        INFO("job " << t << " of " << jobs);
        CHECK(got[t] == expected);
    }
}

} // namespace

TEST_CASE("Twenty runs in one process give the same bytes", "[adaptive][determinism]")
{
    for (const char* id : {"p2"}) {
        const std::string first = textOf(id, 0.2);
        for (int i = 1; i < 20; i++) {
            INFO(id << " repeat " << i);
            REQUIRE(textOf(id, 0.2) == first);
        }
    }
}

TEST_CASE("1, 2, 16 and 32 concurrent runs give the same bytes; 32 finish in time",
          "[adaptive][determinism][concurrency]")
{
#ifdef NDEBUG
    const std::vector<std::string> ids = {"p2", "p5"};
#else
    // MSVC Debug: its debug heap serialises the runs' allocations (16 runs of three pockets took 1400 s, 52 times one
    // run), so one small pocket.
    const std::vector<std::string> ids = {"p2"};
#endif
    std::vector<std::string> expected;
    for (const std::string& id : ids) {
        expected.push_back(hashText(textOf(id, 0.2)));
    }
    // The limit is a guard against a hang (S5: one of 32 upstream instances never finished), not a budget: alone in
    // Release the 32 runs take seconds; MSVC Debug minutes.
    for (int jobs : {1, 2, 16, 32}) {
        concurrent(jobs, ids, 0.2, expected, 1200.0);
    }
}

TEST_CASE("Twenty runs in one process give the same bytes: the other S5 pockets", "[adaptive][determinism][slow]")
{
    for (const char* id : {"p1", "p3", "p5", "p6"}) {
        const std::string first = textOf(id, 0.2);
        for (int i = 1; i < 20; i++) {
            INFO(id << " repeat " << i);
            REQUIRE(textOf(id, 0.2) == first);
        }
    }
}

TEST_CASE("Thirty-two concurrent runs of every S5 pocket and load give the same bytes", "[adaptive][determinism][slow]")
{
    const std::vector<std::string> ids = {"p1", "p2", "p3", "p5", "p6"};
    for (double s : {0.2, 0.1}) {
        std::vector<std::string> expected;
        for (const std::string& id : ids) {
            expected.push_back(hashText(textOf(id, s)));
        }
        concurrent(32, ids, s, expected, 1200.0);
    }
}

#ifdef RAY_ADAPTIVE_PROBE
TEST_CASE("Twenty processes give the same bytes as this one", "[adaptive][determinism]")
{
    const std::string expected = hashText(textOf("p2", 0.2));
    const std::string cmd = std::string("\"") + RAY_ADAPTIVE_PROBE + "\" hash p2 0.2";
    for (int i = 0; i < 20; i++) {
#ifdef _WIN32
        FILE* pipe = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        REQUIRE(pipe != nullptr);
        char buf[256] = {};
        std::string line = std::fgets(buf, sizeof buf, pipe) ? std::string(buf) : std::string();
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        INFO("process " << i);
        CHECK(line == expected);
    }
}
#endif

TEST_CASE("golden: the whole output of the circular pocket at 20% load", "[adaptive][golden]")
{
    const std::string text = textOf("p2", 0.2);
    if (updating()) {
        std::ofstream(goldenPath("p2_20.txt"), std::ios::binary | std::ios::trunc) << text;
        return;
    }
    const std::string golden = readFile(goldenPath("p2_20.txt"));
    INFO("golden p2_20.txt differs (" << golden.size() << " bytes golden, " << text.size() << " produced)");
    CHECK(golden == text);
}

TEST_CASE("golden: hashes of the circular and the open pocket at 10% and 20% load", "[adaptive][golden]")
{
    checkHashes({"p2", "p5"});
}

TEST_CASE("golden: hashes of the other S5 pockets at 10% and 20% load", "[adaptive][golden][slow]")
{
    checkHashes({"p1", "p3", "p6"});
}
