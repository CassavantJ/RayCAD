// T-043: the interpreter's own rules on small hand-written programs, and its independence from src/post.
#include "gcode_interp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

rtgc::Dialect dialect(rtgc::Family f, bool classic = false)
{
    rtgc::Dialect d;
    d.family = f;
    d.haasClassic = classic;
    rtgc::parseCoolantCodes(
        f == rtgc::Family::Haas ? "flood=M8/M9 through=M88/M89 air=M83/M84" : "flood=M8/M9 mist=M7/M9", d.coolant);
    return d;
}

// A program around `body`: units, safe start, a tool with its length offset, the spindle on, at X0 Y0 Z10.
std::string program(const std::string& body, const char* units = "G21")
{
    return std::string("%\n") + units +
           "\nG17 G40 G49 G80 G90 G94\nG0 G53 Z0.\nT1 M6\nS1000 M3\nG0 G54 X0. Y0.\nG43 Z10. H1\n" + body +
           "G0 Z10.\nG0 G53 Z0.\nM30\n%\n";
}

bool hasCode(const std::vector<rtgc::Issue>& v, const std::string& code)
{
    for (const rtgc::Issue& i : v) {
        if (i.code == code) {
            return true;
        }
    }
    return false;
}

std::vector<rtgc::Segment> feeds(const rtgc::Trace& t)
{
    std::vector<rtgc::Segment> out;
    for (const rtgc::Segment& s : t.segs) {
        if (s.kind != rtgc::SegKind::Rapid) {
            out.push_back(s);
        }
    }
    return out;
}

std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

TEST_CASE("interpreter: a clean program has no issue", "[post][roundtrip][interp]")
{
    const auto t = rtgc::interpret(program("G1 Z-1. F100.\nX10.\n"), dialect(rtgc::Family::Ngc));
    CHECK(t.errors.empty());
    CHECK(t.safety.empty());
    REQUIRE(feeds(t).size() == 2);
    CHECK(feeds(t)[1].to.x == 10.0);
    CHECK(feeds(t)[1].feed == 100.0);
}

TEST_CASE("interpreter: numbers without a decimal point, per control", "[post][roundtrip][interp]")
{
    // [N 3.3.2.1] RS274/NGC: X10 is 10. FANUC / Haas: 10 least increments [ASSUMPTION].
    const auto ngc = rtgc::interpret(program("G1 Z-1. F100.\nX10\n"), dialect(rtgc::Family::Ngc));
    CHECK(feeds(ngc).back().to.x == 10.0);
    const auto fanuc = rtgc::interpret(program("G1 Z-1. F100.\nX10\n"), dialect(rtgc::Family::Fanuc));
    CHECK(std::fabs(feeds(fanuc).back().to.x - 0.010) < 1e-12);
    const auto inch = rtgc::interpret(program("G1 Z-0.1 F10.\nX10\n", "G20"), dialect(rtgc::Family::Haas));
    CHECK(std::fabs(feeds(inch).back().to.x - 0.001 * 25.4) < 1e-12);
}

TEST_CASE("interpreter: dwell units per control", "[post][roundtrip][interp]")
{
    auto dwellOf = [](const std::string& block, rtgc::Family f, bool ms = false) {
        rtgc::Dialect d = dialect(f);
        d.ngcDwellMilliseconds = ms;
        const auto t = rtgc::interpret(program(block + "\n"), d);
        for (const rtgc::Segment& s : t.segs) {
            if (s.kind == rtgc::SegKind::Dwell) {
                return t.errors.empty() ? s.dwell : -1.0;
            }
        }
        return -2.0;
    };
    CHECK(dwellOf("G4 P0.5", rtgc::Family::Ngc) == 0.5);                     // [N 3.5.4] seconds
    CHECK(dwellOf("G4 P500", rtgc::Family::Ngc, true) == 0.5);               // a control set to milliseconds
    CHECK(dwellOf("G4 P0.5", rtgc::Family::Ngc, true) == -2.0);              // rejected
    CHECK(dwellOf("G4 P500", rtgc::Family::Fanuc) == 0.5);                   // integer milliseconds [F1, ASSUMPTION]
    CHECK(dwellOf("G4 X0.5", rtgc::Family::Fanuc) == 0.5);                   // X seconds
    CHECK(dwellOf("G4 P0.5", rtgc::Family::Fanuc) == -2.0);                  // rejected
    CHECK(dwellOf("G04 P0.500", rtgc::Family::Haas) == 0.5);                 // [H1 p.48] a fraction: seconds
    CHECK(dwellOf("G04 P500", rtgc::Family::Haas) == 0.5);                   // none: milliseconds
    CHECK(std::fabs(dwellOf("G04 P500.", rtgc::Family::Haas) - 500) < 1e-9); // 500 seconds
}

TEST_CASE("interpreter: R arcs take the arc R describes, not its complement", "[post][roundtrip][interp]")
{
    // [N 3.5.3.1] From (0, 0) clockwise to (5, 5): R5 is the 90-degree arc about (5, 0), R-5 the 270-degree one.
    const auto t =
        rtgc::interpret(program("G1 Z-1. F100.\nG2 X5. Y5. R5.\nG2 X0. Y0. R-5.\n"), dialect(rtgc::Family::Ngc));
    REQUIRE(t.errors.empty());
    const auto f = feeds(t);
    REQUIRE(f.size() == 3);
    CHECK(std::fabs(f[1].centre.x - 5) < 1e-9);
    CHECK(std::fabs(f[1].centre.y - 0) < 1e-9);
    CHECK(std::fabs(f[1].sweep - 3.14159265358979 / 2) < 1e-9);
    CHECK(std::fabs(f[2].sweep - 3 * 3.14159265358979 / 2) < 1e-9);
    // A full circle in centre format is one block [N 3.5.3.2].
    const auto c = rtgc::interpret(program("G1 Z-1. F100.\nG3 X0. Y0. I5. J0.\n"), dialect(rtgc::Family::Fanuc));
    CHECK(std::fabs(feeds(c)[1].sweep - 2 * 3.14159265358979) < 1e-9);
    // G18: counter-clockwise is the positive rotation about +Y (Z towards X).
    const auto x = rtgc::interpret(program("G1 Z0. F100.\nG18 G3 X10. Z0. I5. K0.\nG17\n"), dialect(rtgc::Family::Ngc));
    REQUIRE(x.errors.empty());
    CHECK(feeds(x)[1].plane == 18);
}

TEST_CASE("interpreter: peck cycles", "[post][roundtrip][interp]")
{
    // FANUC G83: from R2 down to -5 in pecks of 3: feeds end at -1, -4, -5; between pecks rapid to R, then to 0.5
    // above the last depth.
    const auto t = rtgc::interpret(program("G99 G83 X0. Y0. Z-5. R2. Q3. F100.\nG80\n"), dialect(rtgc::Family::Fanuc));
    REQUIRE(t.errors.empty());
    const auto f = feeds(t);
    REQUIRE(f.size() == 3);
    CHECK(f[0].to.z == -1.0);
    CHECK(f[1].from.z == -0.5);
    CHECK(f[1].to.z == -4.0);
    CHECK(f[2].to.z == -5.0);
    CHECK(t.segs.back().kind == rtgc::SegKind::Rapid);
    // Haas G83 I J K: pecks 3, 2, 1, 1 [H1 p.77 per haas-ngc.md].
    const auto h =
        rtgc::interpret(program("G83 G99 X0. Y0. Z-5. I3. J1. K1. R2. F100.\nG80\n"), dialect(rtgc::Family::Haas));
    REQUIRE(h.errors.empty());
    const auto hf = feeds(h);
    REQUIRE(hf.size() == 4);
    CHECK(hf[0].to.z == -1.0);
    CHECK(hf[1].to.z == -3.0);
    CHECK(hf[2].to.z == -4.0);
    CHECK(hf[3].to.z == -5.0);
}

TEST_CASE("interpreter: G98 return level per control", "[post][roundtrip][interp]")
{
    // A G99 hole, then a G98 hole. FANUC / Haas: G98 goes back to the initial level (Z10). RS274/NGC: to the Z the
    // block started at (the R plane of the hole before) [N 3.5.16, 3.5.20].
    const std::string body = "G99 G81 X0. Y0. Z-1. R2. F100.\nG98 X5.\nG80\n";
    auto lastZ = [&](rtgc::Family f) {
        const auto t = rtgc::interpret(program(body), dialect(f));
        double z = 0;
        for (const rtgc::Segment& s : t.segs) {
            if (s.cycleReturn) {
                z = s.to.z;
            }
        }
        return z;
    };
    CHECK(lastZ(rtgc::Family::Fanuc) == 10.0);
    CHECK(lastZ(rtgc::Family::Haas) == 10.0);
    CHECK(lastZ(rtgc::Family::Ngc) == 2.0);
}

TEST_CASE("interpreter: unknown words, codes and forms are rejected with the line", "[post][roundtrip][interp]")
{
    auto errs = [](const std::string& block, rtgc::Family f, bool classic = false) {
        return rtgc::interpret(program(block + "\n"), dialect(f, classic)).errors;
    };
    CHECK(hasCode(errs("G1 X1. L2 F10.", rtgc::Family::Ngc), "unknown-word"));
    CHECK(hasCode(errs("G61", rtgc::Family::Fanuc), "unknown-g"));
    CHECK(hasCode(errs("G73 X0. Y0. Z-1. R1. Q1. F10.", rtgc::Family::Ngc), "unknown-g")); // no G73 in [N]
    CHECK(hasCode(errs("M7", rtgc::Family::Haas), "unknown-m"));                           // no mist code by default
    CHECK(hasCode(errs("M29 S100", rtgc::Family::Haas), "unknown-m"));
    CHECK(hasCode(errs("G187 P2", rtgc::Family::Haas, true), "word-context")); // classic: E only [H1 p.17]
    CHECK(errs("G187 P2 E0.05", rtgc::Family::Haas).empty());
    CHECK(hasCode(errs("G1 X1. #1=2", rtgc::Family::Ngc), "unknown-character"));
    CHECK(hasCode(errs("H2", rtgc::Family::Fanuc), "word-context"));
    CHECK(hasCode(errs("G1 X1. (open", rtgc::Family::Fanuc), "comment-unclosed"));
    const auto t = rtgc::interpret(program("G1 X1. L2 F10.\n"), dialect(rtgc::Family::Ngc));
    REQUIRE_FALSE(t.errors.empty());
    CHECK(rtgc::describe(t.errors.front(), t.lines).rfind("line 9 (G1 X1. L2 F10.)", 0) == 0);
}

TEST_CASE("interpreter: machine-level safety facts", "[post][roundtrip][interp]")
{
    auto safety = [](const std::string& text, rtgc::Family f = rtgc::Family::Fanuc) {
        return rtgc::interpret(text, dialect(f)).safety;
    };
    CHECK(hasCode(safety(program("M5\nG1 Z-1. F100.\n")), "safety-feed-spindle-stopped"));
    CHECK(hasCode(safety(program("G1 Z-1. F100.\nG0 X10.\n")), "safety-xy-rapid-in-cut"));
    CHECK(hasCode(safety(program("G1 Z-1. F100.\nG41 G2 X10. Y0. I5. J0. D1\nG40 G1 X12.\n")), "safety-comp-switch"));
    CHECK(hasCode(safety(program("G1 Z-1. F100.\nG41 X5. D1\n")), "safety-comp-on"));
    CHECK(hasCode(safety(program("G99 G81 X0. Y0. Z-1. R2. F100.\nG0 X5.\n")), "safety-g80-missing"));
    CHECK(hasCode(safety("%\nG21\nG17 G40 G49 G80 G90 G94\nT1 M6\nS100 M3\nG0 X0. Y0.\nZ5.\nG0 G53 Z0.\nM30\n%\n"),
                  "safety-no-length-offset"));
    CHECK(hasCode(safety("%\nG21\nG90\nG0 G53 Z0.\nM30\n"), "safety-framing"));
    CHECK(hasCode(safety("%\nG21\nG90\nG0 G53 Z0.\n%\n"), "safety-no-m30"));
    // Rigid tapping after M29 drives the spindle itself: no "spindle stopped" there.
    CHECK_FALSE(
        hasCode(safety(program("M5\nM29 S500\nG99 G84 X0. Y0. Z-5. R2. F500.\nG80\n")), "safety-feed-spindle-stopped"));
}

TEST_CASE("interpreter: Mazak forms, read both ways", "[post][roundtrip][interp]")
{
    auto mazak = [](int reading) {
        rtgc::Dialect d = dialect(rtgc::Family::Mazak);
        d.mazakReading = reading;
        return d;
    };
    // T<n> T<m> M6: reading 0 mounts T<n>; reading 1 takes the last T word [N 3.7.3].
    const std::string twoT = "%\nG21\nG17 G40 G49 G80 G90 G94\nG91 G28 Z0.\nG90\nT5 T3 M6\nS1000 M3\nG0 G54 X0. Y0.\n"
                             "G43 Z10. H5\nG1 Z-1. F100.\nG91 G28 Z0.\nG90\nM30\n%\n";
    const auto a = rtgc::interpret(twoT, mazak(0));
    const auto b = rtgc::interpret(twoT, mazak(1));
    CHECK(a.errors.empty());
    CHECK(feeds(a).front().state.tool == 5);
    CHECK(feeds(b).front().state.tool == 3);
    // G84.2: synchronous tapping drives the spindle (no M3 needed); G84.3 turns the other way.
    const auto tap = rtgc::interpret(program("M5\nG99 G84.2 X0. Y0. Z-5. R2. F500.\nG80\n"), mazak(0));
    CHECK(tap.errors.empty());
    CHECK_FALSE(hasCode(tap.safety, "safety-feed-spindle-stopped"));
    CHECK(feeds(tap).front().state.spindle == 1);
    // No-motion modes; G05 needs P2 or P0; G54.1 P1-P48.
    CHECK(rtgc::interpret(program("G61.1\nG64\nG05 P2\nG05 P0\n"), mazak(0)).errors.empty());
    CHECK(hasCode(rtgc::interpret(program("G05 P1\n"), mazak(0)).errors, "bad-g05"));
    CHECK(rtgc::interpret(program("G54.1 P48\n"), mazak(0)).errors.empty());
    CHECK(hasCode(rtgc::interpret(program("G54.1 P49\n"), mazak(0)).errors, "bad-wcs"));
    // Dwell P in the unit the shop states (dwellUnits).
    rtgc::Dialect sec = mazak(0);
    sec.mazakDwellMilliseconds = false;
    CHECK(rtgc::interpret(program("G4 P300\n"), mazak(0)).segs.size() > 0);
    CHECK(hasCode(rtgc::interpret(program("G4 P0.3\n"), mazak(0)).errors, "dwell-format"));
    CHECK(rtgc::interpret(program("G4 P0.3\n"), sec).errors.empty());
    // Integers: reading 0 counts increments, reading 1 takes the number.
    CHECK(std::fabs(feeds(rtgc::interpret(program("G1 Z-1. F100.\nX10\n"), mazak(0))).back().to.x - 0.01) < 1e-12);
    CHECK(feeds(rtgc::interpret(program("G1 Z-1. F100.\nX10\n"), mazak(1))).back().to.x == 10.0);
    // M6 retracts Z in reading 1 only.
    auto zAfterM6 = [&](int reading) {
        const auto t = rtgc::interpret("%\nG21\nG90\nG0 G53 Z0.\nT1 M6\nS100 M3\nG0 X0. Y0.\nG43 Z5. H1\nT2 M6\n"
                                       "G0 G53 Z0.\nM30\n%\n",
                                       mazak(reading));
        for (const rtgc::Segment& s : t.segs) {
            if (s.line == 9) {
                return s.to.z;
            }
        }
        return 5.0;
    };
    CHECK(zAfterM6(0) == 5.0);
    CHECK(zAfterM6(1) >= rtgc::kHomeZ);
}
TEST_CASE("the interpreter shares no code with src/post", "[post][roundtrip][interp]")
{
    // gcode_interp.* include the standard library and the interpreter's own header only; roundtrip_compare.* add the
    // IR headers of include/ray. Neither names anything of src/post. (CMake also builds ray_gcode_interp without any
    // ray target or src/post include directory.)
    const std::string dir = RAY_ROUNDTRIP_DIR;
    struct File {
        const char* name;
        std::vector<std::string> allowedQuoted;
        std::vector<std::string> allowedRay;
    };
    const std::vector<File> files = {
        {"gcode_interp.h", {}, {}},
        {"gcode_interp.cpp", {"gcode_interp.h"}, {}},
        {"roundtrip_compare.h", {"gcode_interp.h"}, {"ray/cam.h", "ray/libraries.h"}},
        {"roundtrip_compare.cpp", {"roundtrip_compare.h"}, {}},
    };
    for (const File& f : files) {
        INFO(f.name);
        const std::string text = readText(dir + "/" + f.name);
        REQUIRE_FALSE(text.empty());
        std::istringstream in(text);
        std::string line;
        int includes = 0;
        while (std::getline(in, line)) {
            if (line.rfind("#include", 0) != 0) {
                continue;
            }
            ++includes;
            INFO(line);
            const std::size_t q = line.find('"');
            if (q != std::string::npos) {
                const std::string name = line.substr(q + 1, line.find('"', q + 1) - q - 1);
                CHECK(std::find(f.allowedQuoted.begin(), f.allowedQuoted.end(), name) != f.allowedQuoted.end());
                continue;
            }
            const std::string name = line.substr(line.find('<') + 1, line.find('>') - line.find('<') - 1);
            if (name.rfind("ray/", 0) == 0) {
                CHECK(std::find(f.allowedRay.begin(), f.allowedRay.end(), name) != f.allowedRay.end());
            } else {
                CHECK(name.find('/') == std::string::npos); // a standard header
                CHECK(name.find('.') == std::string::npos);
            }
        }
        CHECK(includes > 0);
        for (const char* banned :
             {"ray::post", "ray/post.h", "formatNumber", "engine.h", "engine_motion", "writer.h", "PostOutput"}) {
            INFO(banned);
            CHECK(text.find(banned) == std::string::npos);
        }
    }
}
