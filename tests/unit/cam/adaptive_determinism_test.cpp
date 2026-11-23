// Determinism of the adaptive engine (task T-060; PLAN.md section 8): byte-identical output over repeats in one
// process, over 1, 2, 16 and 32 concurrent runs (the 32 under a time limit: upstream hung once and crashed once there),
// over 20 processes, and against the integer goldens in tests/golden/adaptive2d, which the Linux job compares too.
//
// GOLDENS. tests/golden/adaptive2d/p2_20.txt is the whole toText dump of S5's p2 at 20% load; hashes.txt holds, per
// S5 pocket and load, the BLAKE3 of the dump and its size. To make them again after a reviewed change: set
// RAY_UPDATE_ADAPTIVE_GOLDENS=1 and run ray_cam_adaptive_tests "[golden]" (quick cases) and "[golden][slow]".
// T-063: plan_p4_20.txt is the entry planner's dump of the slot at 20% load (entry.h, a ramp), and hashes.txt's
// "plan-p4" and "plan-p4hole" rows the planner's on the slot (a ramp; a pre-drilled hole).
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
namespace geom2d = ray::cam::geom2d;
using ray::cam::EntryKind;

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

// T-063: the entry planner's output (entry.h). "p4" is S5's slot entered by a ramp; "p4hole" the same slot entered at
// a pre-drilled hole of 14.3 mm at the centre of its end radius.
Result<Output> planOf(const std::string& id, double stepover)
{
    EntryOptions o;
    if (id == "p4hole") {
        o.order = {EntryKind::PreDrill, EntryKind::Helix, EntryKind::Ramp};
        o.preDrills = {PreDrillHole{geom2d::toPt({7.25, 7.25}), toNm(14.3)}};
        return planCase("p4", stepover, o);
    }
    return planCase(id, stepover, o);
}

std::string planTextOf(const std::string& id, double stepover)
{
    auto out = planOf(id, stepover);
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

// plan: the entry planner's output, keyed "plan-<id>" (T-063)
void checkHashes(const std::vector<std::string>& ids, bool plan = false)
{
    std::map<std::string, std::string> golden = readHashes();
    for (const std::string& id : ids) {
        for (double s : {0.1, 0.2}) {
            const std::string text = plan ? planTextOf(id, s) : textOf(id, s);
            const std::string key = (plan ? "plan-" : "") + id + " " + loadName(s);
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

// Each of `jobs` threads runs every case; all results must equal the single-threaded ones. An id "plan-<id>" runs the
// entry planner (planOf).
Result<Output> runAny(const std::string& id, double stepover)
{
    return id.rfind("plan-", 0) == 0 ? planOf(id.substr(5), stepover) : runCase(id, stepover);
}

void concurrent(int jobs, const std::vector<std::string>& ids, double stepover,
                const std::vector<std::string>& expected, double limitSeconds)
{
    std::vector<std::vector<std::string>> got(jobs);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < jobs; t++) {
        threads.emplace_back([&, t] {
            for (const std::string& id : ids) {
                auto out = runAny(id, stepover);
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

TEST_CASE("Twenty runs of the entry planner in one process give the same bytes", "[adaptive][determinism]")
{
    const std::string first = planTextOf("p4", 0.2);
    for (int i = 1; i < 20; i++) {
        INFO("repeat " << i);
        REQUIRE(planTextOf("p4", 0.2) == first);
    }
}

TEST_CASE("1, 2, 16 and 32 concurrent runs give the same bytes; 32 finish in time",
          "[adaptive][determinism][concurrency]")
{
#ifdef NDEBUG
    // the engine on the circle and the open pocket, and the entry planner on the slot (two engine runs, a ramp)
    const std::vector<std::string> ids = {"p2", "p5", "plan-p4"};
#else
    // MSVC Debug: its debug heap serialises the runs' allocations (T-060: 16 runs of three pockets took 1400 s, 52
    // times one run; 1, 2, 16 and 32 runs of the circle about 1,100 s). So the smallest case only, the slot through the
    // entry planner (two engine runs per case, one of which only finds the regions): every count of concurrent runs
    // still, 32 included, on the same engine and planner code, with less work per run.
    const std::vector<std::string> ids = {"plan-p4"};
#endif
    std::vector<std::string> expected;
    for (const std::string& id : ids) {
        auto out = runAny(id, 0.2);
        REQUIRE(out);
        expected.push_back(hashText(toText(*out)));
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
    const std::vector<std::string> ids = {"p1", "p2", "p3", "p5", "p6", "plan-p4", "plan-p4hole"};
    for (double s : {0.2, 0.1}) {
        std::vector<std::string> expected;
        for (const std::string& id : ids) {
            auto out = runAny(id, s);
            REQUIRE(out);
            expected.push_back(hashText(toText(*out)));
        }
        concurrent(32, ids, s, expected, 1200.0);
    }
}

#ifdef RAY_ADAPTIVE_PROBE
TEST_CASE("Twenty processes give the same bytes as this one", "[adaptive][determinism]")
{
    // the engine on the circle, and (T-063) the entry planner on the slot
    for (const auto& [mode, expected] : {std::pair{std::string("hash p2 0.2"), hashText(textOf("p2", 0.2))},
                                         std::pair{std::string("plan-hash p4 0.2"), hashText(planTextOf("p4", 0.2))}}) {
        const std::string cmd = std::string("\"") + RAY_ADAPTIVE_PROBE + "\" " + mode;
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
            INFO(mode << ", process " << i);
            CHECK(line == expected);
        }
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

TEST_CASE("golden: the whole output of the entry planner on the slot at 20% load", "[adaptive][golden]")
{
    const std::string text = planTextOf("p4", 0.2);
    if (updating()) {
        std::ofstream(goldenPath("plan_p4_20.txt"), std::ios::binary | std::ios::trunc) << text;
        return;
    }
    const std::string golden = readFile(goldenPath("plan_p4_20.txt"));
    INFO("golden plan_p4_20.txt differs (" << golden.size() << " bytes golden, " << text.size() << " produced)");
    CHECK(golden == text);
}

TEST_CASE("golden: hashes of the circular and the open pocket at 10% and 20% load", "[adaptive][golden]")
{
    checkHashes({"p2", "p5"});
    checkHashes({"p4"}, true); // T-063: the entry planner on the slot (a ramp)
}

TEST_CASE("golden: hashes of the entry planner on the slot entered at a pre-drilled hole", "[adaptive][golden][slow]")
{
    checkHashes({"p4hole"}, true);
}

TEST_CASE("golden: hashes of the other S5 pockets at 10% and 20% load", "[adaptive][golden][slow]")
{
    checkHashes({"p1", "p3", "p6"});
}
