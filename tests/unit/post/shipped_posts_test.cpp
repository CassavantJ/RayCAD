// The shipped posts (resources/library/posts, tasks T-040 and T-041): generic ISO, FANUC, Haas NGC and Haas classic.
//  - Every post loads in the sandbox with the default limits, declares its capabilities and properties, and frees its
//    runtime to zero bytes.
//  - The feature matrix: one small program per feature, in millimetres and inches, byte for byte against the goldens
//    in tests/golden/gcode/<post>/ (every OS runs this test). The inputs are the drilling operation's golden toolpaths
//    (tests/golden/toolpaths/drill_*.rctp) and hand-built toolpaths for the milling features.
//  - The line map covers every line with an axis word.
//  - Every combination of a pairwise set of property values posts the realistic program, and the result passes the
//    structural checks: the safe start block first, the end block last, no feed move without the spindle turning, no
//    canned cycle or cutter compensation left on at a tool change or the end, Z retracted before the end.
//  - Each refusal and warning names its cause.
// The goldens are NOT reviewed by the owner yet (PLAN.md T-040): docs/post_review/<post>.md lists what to check.
// To write the produced programs: set RAYCAD_GOLDEN_PRODUCED=<dir>, run "[shipped][golden]", then
// tools/update_golden.py --produced <dir>/<post> --suite gcode/<post> for each post.
#include "engine.h"
#include "post_test_util.h"
#include "shipped_matrix.h"

#include <ray/cam.h>
#include <ray/core.h>
#include <ray/post.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;
using namespace ray::post::test::shipped;

namespace {

// ---------- reading G-code back, for the structural checks (this is not the T-043 interpreter) ----------
struct Word {
    char letter;
    std::string value;
};
// The words of a line outside comments; N numbers dropped.
std::vector<Word> wordsOf(const std::string& line)
{
    std::vector<Word> out;
    int depth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '(') {
            ++depth;
            continue;
        }
        if (c == ')') {
            --depth;
            continue;
        }
        if (depth > 0 || c == ' ' || c == '%') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            std::size_t j = i + 1;
            while (j < line.size() && line[j] != ' ' && line[j] != '(' && !(line[j] >= 'A' && line[j] <= 'Z')) {
                ++j;
            }
            if (c != 'N') {
                out.push_back(Word{c, line.substr(i + 1, j - i - 1)});
            }
            i = j - 1;
        }
    }
    return out;
}

bool hasWord(const std::vector<Word>& w, char letter, const std::string& value)
{
    return std::any_of(w.begin(), w.end(), [&](const Word& x) { return x.letter == letter && x.value == value; });
}
bool hasLetter(const std::vector<Word>& w, char letter)
{
    return std::any_of(w.begin(), w.end(), [&](const Word& x) { return x.letter == letter; });
}
bool hasAxis(const std::vector<Word>& w)
{
    return std::any_of(w.begin(), w.end(),
                       [](const Word& x) { return std::string("XYZIJKR").find(x.letter) != std::string::npos; });
}
// G codes as numbers times ten: "G1" 10, "G54.1" 541, "G0" 0.
std::vector<int> gCodes(const std::vector<Word>& w)
{
    std::vector<int> out;
    for (const Word& x : w) {
        if (x.letter == 'G') {
            const double v = std::strtod(x.value.c_str(), nullptr);
            out.push_back(static_cast<int>(v * 10 + 0.5));
        }
    }
    return out;
}
std::vector<int> mCodes(const std::vector<Word>& w)
{
    std::vector<int> out;
    for (const Word& x : w) {
        if (x.letter == 'M') {
            out.push_back(std::atoi(x.value.c_str()));
        }
    }
    return out;
}

// The structural checks of PLAN.md T-040 requirement 5. Returns the problems found (empty when the program is fine).
std::vector<std::string> structuralProblems(const std::string& post, const std::string& text,
                                            const std::set<int>& coolantOff)
{
    std::vector<std::string> problems;
    const auto all = lines(text);
    if (all.size() < 4) {
        return {"the program has fewer than four lines"};
    }
    auto problem = [&](std::size_t n, const std::string& what) {
        problems.push_back("line " + std::to_string(n + 1) + " (" + all[n] + "): " + what);
    };
    if (all.front() != "%") {
        problem(0, "the program does not start with %");
    }
    if (all.back() != "%") {
        problem(all.size() - 1, "the program does not end with %");
    }
    const bool haas = post.rfind("haas", 0) == 0;
    bool safeSeen = false;
    bool spindle = false;
    bool rigid = false; // M29 given, until G80
    bool cycle = false;
    int comp = 0;
    int motion = 0;
    bool m30 = false;
    bool zHomeSinceLastCut = false;
    for (std::size_t n = 0; n < all.size(); ++n) {
        const auto w = wordsOf(all[n]);
        if (w.empty()) {
            continue;
        }
        if (m30) {
            problem(n, "a block after M30");
        }
        const auto g = gCodes(w);
        const auto m = mCodes(w);
        auto hasG = [&](int code) {
            return std::find(g.begin(), g.end(), code) != g.end();
        };
        auto hasM = [&](int code) {
            return std::find(m.begin(), m.end(), code) != m.end();
        };
        if (m.size() > 1) {
            problem(n, "more than one M code in a block");
        }
        if (hasG(400) && hasG(490) && hasG(800) && hasG(170) && hasG(900)) {
            safeSeen = true;
        }
        const bool moves = hasAxis(w) || std::any_of(g.begin(), g.end(), [](int c) { return c == 280 || c == 530; });
        if (!safeSeen && (moves || hasM(6))) {
            problem(n, "motion or a tool change before the safe start block (G17 G40 G49 G80 G90)");
        }
        for (const int c : g) {
            if (c == 0 || c == 10 || c == 20 || c == 30) {
                motion = c;
            }
            if (c == 730 || c == 740 || (c >= 810 && c <= 890)) {
                motion = c;
                cycle = true;
            }
            if (c == 800) {
                cycle = false;
                rigid = false;
                motion = 800;
            }
            if (c == 410 || c == 420) {
                comp = c / 10;
            }
            if (c == 400) {
                comp = 0;
            }
        }
        if (hasM(29)) {
            rigid = true;
        }
        const bool homing = hasG(280) || hasG(530);
        if (homing && hasLetter(w, 'Z')) {
            zHomeSinceLastCut = true;
        }
        if (!homing && hasAxis(w)) {
            const bool feed = motion == 10 || motion == 20 || motion == 30 || (motion >= 730 && motion != 800);
            const bool tapCycle = motion == 840 || motion == 740;
            if (feed && !spindle && !(tapCycle && (rigid || haas))) {
                problem(n, "a feed move with the spindle stopped");
            }
            if (motion != 0 || hasLetter(w, 'Z')) {
                zHomeSinceLastCut = false;
            }
        }
        if (hasM(3) || hasM(4)) {
            spindle = true;
        }
        if (hasM(5) || hasM(6) || hasM(30)) {
            spindle = false;
        }
        if (hasM(6) || hasM(30)) {
            if (cycle) {
                problem(n, "a canned cycle still active");
            }
            if (comp != 0) {
                problem(n, "cutter compensation still on");
            }
        }
        if (hasM(30)) {
            m30 = true;
            if (!zHomeSinceLastCut) {
                problem(n, "M30 without a Z retract (G28 / G53 Z) after the last move");
            }
        }
        (void)coolantOff;
    }
    if (!m30) {
        problems.push_back("no M30");
    }
    if (!safeSeen) {
        problems.push_back("no safe start block");
    }
    return problems;
}

} // namespace

TEST_CASE("shipped posts load in the sandbox and declare capabilities and properties", "[post][shipped]")
{
    for (const char* id : kPosts) {
        INFO(id);
        QuickJsPostEngine engine; // the default limits (post.h)
        const auto info = engine.inspect(shippedPost(id));
        REQUIRE(info.has_value());
        CHECK(info->id == id);
        CHECK(info->dialect == id);
        CHECK(info->apiVersion == 1);
        CHECK(info->fileExtension == ".nc");
        CHECK(info->caps.arcsIJK);
        CHECK(info->caps.helical);
        CHECK(info->caps.cannedCycles);
        CHECK(info->caps.cutterComp);
        CHECK(info->caps.rigidTap);
        CHECK((info->caps.maxArcDeg == 360 || info->caps.maxArcDeg == 180));
        CHECK(info->properties.size() >= 10);
        for (const PostProperty& p : info->properties) {
            INFO(p.key);
            CHECK_FALSE(p.title.empty());
            if (p.type == PostProperty::Type::Enum) {
                CHECK(p.enumValues.size() >= 2);
            }
        }
        CHECK(engine.lastRun().heapAfterFreeBytes == 0);
        // And a run with the default limits frees everything too.
        Program p = realistic();
        p.machine = machineFor(id);
        JobContext ctx;
        const auto out = engine.run(shippedPost(id), p.job(), ctx);
        REQUIRE(out.has_value());
        CHECK(engine.lastRun().heapAfterFreeBytes == 0);
        CHECK(engine.lastRun().movesPosted > 50);
    }
}

TEST_CASE("shipped posts: the feature matrix matches the goldens byte for byte", "[post][shipped][golden]")
{
    const char* produced = std::getenv("RAYCAD_GOLDEN_PRODUCED");
    for (const Run& r : goldenRuns()) {
        INFO(r.post << "/" << r.file);
        const auto out = postRun(r.post, r.program);
        if (!out) {
            FAIL(out.error().message);
        }
        // The warnings, one per line, beside the program when there are any (<name>.warnings.txt): a reviewer reads
        // them with the program, and a new or lost warning fails the test like a changed line.
        std::string warnings;
        for (const std::string& w : out->warnings) {
            warnings += w + "\n";
        }
        const std::string warningsFile = r.file.substr(0, r.file.size() - 3) + ".warnings.txt";
        if (produced != nullptr) {
            const std::filesystem::path folder = std::filesystem::path(produced) / r.post;
            std::filesystem::create_directories(folder);
            std::ofstream(folder / r.file, std::ios::binary) << out->text;
            if (!warnings.empty()) {
                std::ofstream(folder / warningsFile, std::ios::binary) << warnings;
            }
        }
        const std::string golden = readFile(goldenPath(r.post, r.file));
        CHECK_FALSE(golden.empty());
        CHECK(out->text == golden);
        CHECK(readFile(goldenPath(r.post, warningsFile)) == warnings);
    }
}

TEST_CASE("shipped posts: the line map covers every line with an axis word", "[post][shipped]")
{
    for (const Run& r : goldenRuns()) {
        INFO(r.post << "/" << r.file);
        const auto out = postRun(r.post, r.program);
        REQUIRE(out.has_value());
        const auto text = lines(out->text);
        std::set<std::uint32_t> mapped;
        for (const GcodeLineMap& e : out->lineMap) {
            REQUIRE(e.line < text.size());
            REQUIRE(e.sectionIndex < r.program.paths.size());
            CHECK(e.moveIndex < r.program.paths[e.sectionIndex].moves.size());
            mapped.insert(e.line);
        }
        for (std::uint32_t i = 0; i < text.size(); ++i) {
            if (hasAxis(wordsOf(text[i]))) {
                INFO("line " << i + 1 << ": " << text[i]);
                CHECK(mapped.count(i) == 1);
            }
        }
    }
}

TEST_CASE("shipped posts: the matrix programs pass the structural checks", "[post][shipped]")
{
    for (const Run& r : goldenRuns()) {
        INFO(r.post << "/" << r.file);
        const auto out = postRun(r.post, r.program);
        REQUIRE(out.has_value());
        for (const std::string& p : structuralProblems(r.post, out->text, {})) {
            FAIL_CHECK(p);
        }
    }
}

TEST_CASE("shipped posts: every pairwise property combination passes the structural checks", "[post][shipped]")
{
    for (const char* id : kPosts) {
        QuickJsPostEngine engine;
        const auto info = engine.inspect(shippedPost(id));
        REQUIRE(info.has_value());
        const auto combos = pairwiseCombinations(*info);
        INFO(id << ": " << combos.size() << " combinations");
        CHECK(combos.size() >= 4);
        CHECK(combos.size() <= 80);
        for (const auto& combo : combos) {
            std::string label;
            for (const auto& [k, v] : combo) {
                label += k + "=" + v + " ";
            }
            INFO(id << " " << label);
            Program p = realistic();
            p.machine = machineFor(id);
            p.properties = combo;
            const auto out = postRun(id, p, std::string("2026-11-22"));
            if (!out) {
                FAIL_CHECK(out.error().message);
                continue;
            }
            for (const std::string& problem : structuralProblems(id, out->text, {})) {
                FAIL_CHECK(problem);
            }
        }
    }
}

TEST_CASE("shipped posts: refusals and warnings name their cause", "[post][shipped]")
{
    struct Case {
        const char* what;
        std::function<void(Program&)> change;
        std::map<std::string, std::string> properties;
        const char* message; // in the error, or in a warning when `warning`
        bool warning;
    };
    const std::vector<Case> cases = {
        {"program number 0", [](Program& p) { p.setup.programNumber = 0; }, {}, "program number 0", false},
        {"comp starts on an arc",
         [](Program& p) {
             auto& m = p.paths[1].moves;
             m[5].compensation = 1; // the first arc, while the lead-in had compensation... already on: change lead-in
             m[3].compensation = 0;
             m[4].compensation = 0;
         },
         {},
         "on an arc",
         false},
        {"comp left on",
         [](Program& p) {
             auto& m = p.paths[1].moves;
             m[m.size() - 2].compensation = 1; // the lead-out keeps compensation, the rapid follows
         },
         {},
         "cutter compensation",
         false},
        {"spindle too fast", [](Program& p) { p.ops[1].cutting.spindleRpm = 20000; }, {}, "above the machine's", false},
        {"no spindle speed", [](Program& p) { p.ops[1].cutting.spindleRpm = 0; }, {}, "no spindle speed", false},
        {"mist without a code",
         [](Program& p) { p.ops[1].cutting.coolant = cam::CuttingData::Coolant::Air; },
         {{"coolantCodes", "flood=M8/M9 mist=M7/M9 through=/ air=/"}},
         "coolant mode 'air' has no M code",
         false},
        {"bad coolant codes", [](Program&) {}, {{"coolantCodes", "flood=M8"}}, "post property coolantCodes", false},
        {"tap feed not pitch x rpm", [](Program& p) { p.ops[5].cutting.spindleRpm = 600; }, {}, "pitch x rpm", false},
        {"left-hand tap, spindle clockwise",
         [](Program& p) {
             for (cam::Move& m : p.paths[5].moves) {
                 if (m.kind == MoveKind::CycleTap) {
                     m.cycleLeftHand = 1;
                 }
             }
         },
         {},
         "-hand tap",
         false},
        {"peck clearance in the IR", [](Program&) {}, {}, "peck clearance", true}, // drill_chipbreak has 0.3 mm
    };
    for (const char* id : kPosts) {
        for (const Case& c : cases) {
            INFO(id << ": " << c.what);
            Program p = realistic();
            p.machine = machineFor(id);
            p.properties = c.properties;
            c.change(p);
            const auto out = postRun(id, p);
            if (c.warning) {
                REQUIRE(out.has_value());
                if (std::string(id) == "generic-iso") {
                    continue; // its chip break is expanded by the host, which uses the IR's peck clearance
                }
                CHECK(std::any_of(out->warnings.begin(), out->warnings.end(),
                                  [&](const std::string& w) { return w.find(c.message) != std::string::npos; }));
                continue;
            }
            if (std::string(c.what) == "left-hand tap, spindle clockwise" && std::string(id) == "generic-iso") {
                // RS274/NGC has no G74: the generic post refuses any left-hand tap.
                REQUIRE_FALSE(out.has_value());
                CHECK(out.error().message.find("left-hand") != std::string::npos);
                continue;
            }
            REQUIRE_FALSE(out.has_value());
            CHECK(out.error().message.find(c.message) != std::string::npos);
        }
    }
}
