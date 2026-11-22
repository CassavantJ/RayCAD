// IPostEngine on QuickJS-NG with the two test posts (tests/unit/post/posts): declaration, properties, units, the
// host's motion services seen through a post, the line map, determinism and the golden programs that every OS must
// reproduce byte for byte (tests/golden/gcode/host).
#include "engine.h"
#include "post_test_util.h"

#include <ray/core.h>
#include <ray/post.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;

namespace {

// The program behind the goldens. `full` adds what only iso-test can take: XZ and YZ arcs, cutter compensation and
// tapping.
Program goldenProgram(bool full)
{
    using cam::MoveKind;
    using cam::MoveRole;
    Program p;
    {
        cam::Toolpath& t = p.section("Contour and arcs", "contour2d", 1,
                                     makeTool("10 mm 3FL carbide (rough)", lib::ToolType::FlatEndMill, 10, 3), 9000);
        auto& m = t.moves;
        m.push_back(rapid(mm(20), mm(10), mm(25)));
        m.push_back(rapid(mm(20), mm(10), mm(5)));
        m.push_back(linear(mm(20), mm(10), -mm(2), mmPerMin(300), MoveRole::Plunge));
        m.push_back(linear(mm(30), mm(10), -mm(2), mmPerMin(1200)));
        // Four quarter arcs counter-clockwise about (30, 20), then a clockwise full circle.
        m.push_back(arc(false, mm(40), mm(20), -mm(2), mm(30), mm(20), 0, mmPerMin(1200)));
        m.push_back(arc(false, mm(30), mm(30), -mm(2), mm(30), mm(20), 0, mmPerMin(1200)));
        m.push_back(arc(false, mm(20), mm(20), -mm(2), mm(30), mm(20), 0, mmPerMin(1200)));
        m.push_back(arc(false, mm(30), mm(10), -mm(2), mm(30), mm(20), 0, mmPerMin(1200)));
        m.push_back(arc(true, mm(30), mm(10), -mm(2), mm(30), mm(20), 0, mmPerMin(1000)));
        // Values on rounding ties at 3 decimals of a millimetre: 12.3455 -> 12.346, 12.3445 -> 12.344.
        m.push_back(linear(12'345'500, 4'000'500, -mm(2), mmPerMin(1200)));
        m.push_back(linear(12'344'500, 4'001'500, -mm(2), mmPerMin(1200)));
        m.push_back(linear(mm(25), mm(4), -mm(2), mmPerMin(1200)));
        // A full helical turn about (20, 4), 3 mm down.
        m.push_back(arc(false, mm(25), mm(4), -mm(5), mm(20), mm(4), 0, mmPerMin(700), cam::ArcPlane::XY));
        m.push_back(dwell(250));
        if (full) {
            m.push_back(arc(true, mm(20), mm(4), 0, mm(20), 0, -mm(5), mmPerMin(600), cam::ArcPlane::XZ));
            m.push_back(arc(true, mm(20), mm(9), -mm(5), 0, mm(4), -mm(5), mmPerMin(600), cam::ArcPlane::YZ));
            m.push_back(linear(mm(45), mm(9), -mm(5), mmPerMin(900), MoveRole::LeadIn, 1));
            m.push_back(linear(mm(45), mm(30), -mm(5), mmPerMin(900), MoveRole::Cut, 1));
            m.push_back(linear(mm(50), mm(30), -mm(5), mmPerMin(900), MoveRole::LeadOut, 0));
        }
        m.push_back(rapid(m.back().x, m.back().y, mm(25)));
    }
    {
        cam::Toolpath& t =
            p.section("Drill holes", "drill", 2, makeTool("6.8 mm drill", lib::ToolType::Drill, 6.8, 2), 2500);
        auto& m = t.moves;
        m.push_back(rapid(mm(10), mm(10), mm(25)));
        m.push_back(rapid(mm(10), mm(10), mm(5)));
        m.push_back(cycle(MoveKind::CycleDrill, mm(10), mm(10), -mm(12), mm(2), mmPerMin(150)));
        m.push_back(cycle(MoveKind::CycleDrill, mm(20), mm(10), -mm(12), mm(2), mmPerMin(150)));
        m.push_back(cycle(MoveKind::CycleDrill, mm(30), mm(10), -mm(12), mm(2), mmPerMin(150), 0, 300));
        m.push_back(cycle(MoveKind::CyclePeck, mm(40), mm(10), -mm(20), mm(2), mmPerMin(120), mm(4)));
        m.push_back(cycle(MoveKind::CycleChipBreak, mm(50), mm(10), -mm(15), mm(2), mmPerMin(120), mm(3)));
        m.push_back(cycleCancel());
        m.push_back(cycle(MoveKind::CycleBore, mm(60), mm(10), -mm(8), mm(2), mmPerMin(80), 0, 200));
        m.push_back(cycle(MoveKind::CycleReam, mm(70), mm(10), -mm(10), mm(2), mmPerMin(100)));
        m.push_back(rapid(mm(70), mm(10), mm(25)));
    }
    {
        cam::Toolpath& t =
            p.section("Peck more", "drill", 2, makeTool("6.8 mm drill", lib::ToolType::Drill, 6.8, 2), 2500);
        t.startX = mm(70);
        t.startY = mm(10);
        auto& m = t.moves;
        m.push_back(rapid(mm(10), mm(30), mm(5)));
        m.push_back(cycle(MoveKind::CyclePeck, mm(10), mm(30), -mm(6), mm(2), mmPerMin(120), mm(2)));
        m.push_back(cycle(MoveKind::CyclePeck, mm(20), mm(30), -mm(6), mm(2), mmPerMin(120), mm(2)));
        m.push_back(rapid(mm(20), mm(30), mm(25)));
    }
    if (full) {
        lib::Tool tap = makeTool("M8x1.25 tap", lib::ToolType::Tap, 8, 3);
        tap.threadPitch = 1.25;
        tap.threadSpec = "M8x1.25";
        cam::Toolpath& t = p.section("Tap M8", "drill", 3, tap, 500, cam::CuttingData::Coolant::Off);
        auto& m = t.moves;
        m.push_back(rapid(mm(10), mm(10), mm(5)));
        m.push_back(cycle(MoveKind::CycleTap, mm(10), mm(10), -mm(12), mm(3), mmPerMin(625), 0, 0, 1'250'000));
        m.push_back(cycle(MoveKind::CycleTap, mm(20), mm(10), -mm(12), mm(3), mmPerMin(625), 0, 0, 1'250'000));
        m.push_back(rapid(mm(20), mm(10), mm(25)));
    }
    return p;
}

struct Golden {
    const char* post;
    bool full;
    UnitSystem units;
    const char* file;
};
constexpr Golden kGoldens[] = {
    {"iso-test.post.js", true, UnitSystem::Metric, "iso-test_mm.nc"},
    {"iso-test.post.js", true, UnitSystem::Inch, "iso-test_inch.nc"},
    {"plain-test.post.js", false, UnitSystem::Metric, "plain-test_mm.tap"},
    {"plain-test.post.js", false, UnitSystem::Inch, "plain-test_inch.tap"},
};

Program goldenProgramFor(const Golden& g)
{
    Program p = goldenProgram(g.full);
    p.units = g.units;
    if (g.units == UnitSystem::Inch && g.full) {
        p.properties["includeDate"] = "true"; // and a date the caller gives (PostJob::fixedDate)
    }
    return p;
}

Result<PostOutput> postWith(const std::string& postFile, const Program& program, std::optional<std::string> date = {})
{
    QuickJsPostEngine engine;
    JobContext ctx;
    PostJob job = program.job();
    job.fixedDate = std::move(date);
    return engine.run(postPath(postFile), job, ctx);
}

Result<PostOutput> postGolden(const Golden& g)
{
    return postWith(g.post, goldenProgramFor(g), std::string("2026-01-02"));
}

std::string goldenPath(const char* file)
{
    return std::string(RAY_GOLDEN_GCODE_DIR) + "/" + file;
}

// Whether a G-code line has an axis or centre word outside its comments.
bool hasAxisWord(const std::string& line)
{
    int depth = 0;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        if (depth > 0) {
            continue;
        }
        const char n = line[i + 1];
        const bool startsWord = i == 0 || line[i - 1] == ' ';
        if (startsWord && std::string_view("XYZIJKR").find(c) != std::string_view::npos &&
            ((n >= '0' && n <= '9') || n == '-' || n == '.')) {
            return true;
        }
    }
    return false;
}

std::optional<double> wordValue(const std::string& line, char letter)
{
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == letter && (i == 0 || line[i - 1] == ' ')) {
            return std::strtod(line.c_str() + i + 1, nullptr);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("inspect reads the declaration block", "[post][engine]")
{
    QuickJsPostEngine engine;
    const auto info = engine.inspect(postPath("iso-test.post.js"));
    REQUIRE(info.has_value());
    CHECK(info->id == "iso-test");
    CHECK(info->title == "Host test, ISO words");
    CHECK(info->vendor == "RayCAD tests");
    CHECK(info->dialect == "generic-iso");
    CHECK(info->fileExtension == ".nc");
    CHECK(info->apiVersion == 1);
    CHECK(info->caps.arcsIJK);
    CHECK_FALSE(info->caps.arcsR);
    CHECK(info->caps.helical);
    CHECK(info->caps.cannedCycles);
    CHECK(info->caps.rigidTap);
    CHECK(info->caps.cutterComp);
    CHECK_FALSE(info->caps.toolLengthProbe);
    CHECK(info->caps.maxArcDeg == 180);
    std::vector<std::string> keys;
    for (const auto& p : info->properties) {
        keys.push_back(p.key);
    }
    CHECK(keys == std::vector<std::string>{"numberBlocks", "numberStep", "homeZ", "coolantStyle", "header",
                                           "lineEnding", "includeDate"});
    CHECK(info->properties[0].type == PostProperty::Type::Bool);
    CHECK(info->properties[0].defaultValue == "true");
    CHECK(info->properties[0].group == "Format");
    CHECK(info->properties[1].type == PostProperty::Type::Int);
    CHECK(info->properties[1].defaultValue == "5");
    CHECK(info->properties[2].type == PostProperty::Type::Number);
    CHECK(info->properties[2].defaultValue == "50");
    CHECK(info->properties[2].description == "Z of the final retract");
    CHECK(info->properties[3].enumValues == std::vector<std::string>{"m8", "none"});
    CHECK(info->properties[4].defaultValue == "made by the host test");
    CHECK(info->properties[5].defaultValue == "crlf"); // the host's own
    CHECK(info->properties[6].defaultValue == "false");
    CHECK(engine.lastRun().heapAfterFreeBytes == 0);

    const auto plain = engine.inspect(postPath("plain-test.post.js"));
    REQUIRE(plain.has_value());
    CHECK(plain->id == "plain-test");
    CHECK(plain->vendor.empty());
    CHECK(plain->caps.arcsR);
    CHECK_FALSE(plain->caps.cannedCycles);
    CHECK(plain->caps.maxArcDeg == 90);
    REQUIRE(plain->properties.size() == 3);
    CHECK(plain->properties[0].key == "lineEnding"); // declared by the post, with its own default
    CHECK(plain->properties[0].defaultValue == "lf");
    CHECK(plain->properties[2].key == "includeDate");

    const auto missing = engine.inspect(postPath("no-such.post.js"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("inspect refuses bad declarations and newer API versions", "[post][engine]")
{
    QuickJsPostEngine engine;
    auto declare = [&](const std::string& patch) {
        // patch is inserted into a valid definition and overrides its fields.
        const std::string src = "definePost(Object.assign({ title: 't', dialect: 'fanuc', fileExtension: '.nc', "
                                "apiVersion: 1, capabilities: { arcs: 'IJK', helical: true, maxArcDegrees: 360, "
                                "cannedCycles: [], rigidTap: false, cutterComp: false }, properties: {} }, " +
                                patch + "));";
        return engine.inspectSource(src, "bad.post.js");
    };
    REQUIRE(declare("{}").has_value());
    const auto newer = declare("{ apiVersion: 2 }");
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().code == ErrorCode::FormatTooNew);
    struct Bad {
        const char* patch;
        const char* message;
    };
    const Bad bad[] = {
        {"{ title: 3 }", "title must be a string"},
        {"{ fileExtension: 'nc' }", "fileExtension must start with a dot"},
        {"{ capabilities: { arcs: 'XYZ', helical: true, maxArcDegrees: 360, cannedCycles: [], rigidTap: false, "
         "cutterComp: false } }",
         "capabilities.arcs"},
        {"{ capabilities: { arcs: 'IJK', helical: true, maxArcDegrees: 400, cannedCycles: [], rigidTap: false, "
         "cutterComp: false } }",
         "maxArcDegrees"},
        {"{ capabilities: { arcs: 'IJK', helical: true, maxArcDegrees: 90, cannedCycles: ['thread'], rigidTap: "
         "false, cutterComp: false } }",
         "\"thread\" is not one of"},
        {"{ properties: { a: { type: 'int', title: 'A', default: 1.5 } } }", "whole numbers"},
        {"{ properties: { a: { type: 'enum', title: 'A', default: 'x', values: ['y'] } } }", "not one of its values"},
        {"{ properties: { a: { type: 'bool', title: 'A', default: 'yes' } } }", "must be true or false"},
        {"{ properties: { lineEnding: { type: 'bool', title: 'A', default: true } } }", "read by the host"},
        {"{ onLinear: 5 }", "onLinear must be a function"},
    };
    for (const Bad& b : bad) {
        INFO(b.patch);
        const auto r = declare(b.patch);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::ScriptError);
        CHECK(r.error().message.find(b.message) != std::string::npos);
        CHECK(r.error().message.rfind("bad.post.js: definePost: ", 0) == 0);
        CHECK(engine.lastRun().heapAfterFreeBytes == 0);
    }
}

TEST_CASE("golden programs: byte for byte on every OS", "[post][engine][golden]")
{
    for (const Golden& g : kGoldens) {
        INFO("golden " << g.file);
        const auto out = postGolden(g);
        REQUIRE(out.has_value());
        if (const char* dir = std::getenv("RAYCAD_GOLDEN_PRODUCED")) {
            // Laid out like the suite: tools/update_golden.py --produced <dir> --suite gcode/host.
            const std::filesystem::path folder(dir);
            std::filesystem::create_directories(folder);
            std::ofstream(folder / g.file, std::ios::binary) << out->text;
        }
        const std::string golden = readFile(goldenPath(g.file));
        CHECK_FALSE(golden.empty());
        CHECK(out->text == golden);
        // The same bytes from a second engine in the same process.
        CHECK(postGolden(g)->hash() == out->hash());
    }
}

TEST_CASE("the line map covers every motion line and points at its move", "[post][engine]")
{
    for (const Golden& g : kGoldens) {
        INFO("golden " << g.file);
        const Program program = goldenProgramFor(g);
        const auto out = postGolden(g);
        REQUIRE(out.has_value());
        const auto text = lines(out->text);
        std::set<std::uint32_t> mapped;
        std::uint32_t previous = 0;
        for (std::size_t n = 0; n < out->lineMap.size(); ++n) {
            const GcodeLineMap& e = out->lineMap[n];
            REQUIRE(e.line < text.size());
            REQUIRE(e.sectionIndex < program.paths.size());
            REQUIRE(e.moveIndex < program.paths[e.sectionIndex].moves.size());
            CHECK((n == 0 || e.line > previous)); // one entry per line, in order
            previous = e.line;
            mapped.insert(e.line);
            // A line for a rapid or linear IR move ends where that move ends.
            const cam::Move& m = program.paths[e.sectionIndex].moves[e.moveIndex];
            const bool plain = m.kind == cam::MoveKind::Rapid || m.kind == cam::MoveKind::Linear;
            const std::string& line = text[e.line];
            // (Lines of onSectionStart map to move 0 and show the section's start point instead.)
            if (plain && e.moveIndex > 0) {
                const double unit = g.units == UnitSystem::Inch ? 25.4e6 : 1e6;
                if (const auto x = wordValue(line, 'X')) {
                    CHECK(std::abs(*x - static_cast<double>(m.x) / unit) < 1e-3);
                }
                if (const auto z = wordValue(line, 'Z')) {
                    CHECK(std::abs(*z - static_cast<double>(m.z) / unit) < 1e-3);
                }
            }
        }
        std::size_t motionLines = 0;
        for (std::uint32_t i = 0; i < text.size(); ++i) {
            if (hasAxisWord(text[i]) && text[i].find("G53") == std::string::npos) {
                ++motionLines;
                INFO("line " << i << ": " << text[i]);
                CHECK(mapped.count(i) == 1);
            }
        }
        CHECK(motionLines > 40);
    }
}

TEST_CASE("the host services show in the output", "[post][engine]")
{
    const auto iso = postGolden(kGoldens[0]);
    REQUIRE(iso.has_value());
    const std::string& t = iso->text;
    // Arcs split to 180 degrees: the full circle (move 8) becomes two G2 blocks; quarter arcs stay whole.
    CHECK(t.find(" G2 Y30. I0. J10. F1000.\r\n") != std::string::npos);
    CHECK(t.find(" Y10. I0. J-10.\r\n") != std::string::npos);
    CHECK(t.find(" G17 G3 X40. Y20. I0. J10.\r\n") != std::string::npos);
    CHECK(t.find(" X30. Y30. I-10. J0.\r\n") != std::string::npos);
    // Helix: two halves with the axis interpolated.
    CHECK(t.find("G3 X15. Z-3.5 I-5. J0. F700.") != std::string::npos);
    CHECK(t.find("X25. Z-5. I5. J0.") != std::string::npos);
    // Planes, cutter compensation, ties rounded half to even.
    CHECK(t.find("G18 G2") != std::string::npos);
    CHECK(t.find("G19") != std::string::npos);
    CHECK(t.find("G41 G1 X45. D1 F900.") != std::string::npos);
    CHECK(t.find("G40 X50.") != std::string::npos);
    CHECK(t.find("X12.346 Y4.") != std::string::npos);
    CHECK(t.find("X12.344 Y4.002") != std::string::npos);
    // Declared cycles: modal G81 with the later holes as XY only, G82 with dwell, G83 and G73 with Q, G84 with the
    // pitch in a comment, G80 at the end of each run; bore and ream (not declared) expanded into plain moves.
    CHECK(t.find("G99 G81 X10. Y10. Z-12. R2. F150.") != std::string::npos);
    CHECK(t.find("X20.\r\n") != std::string::npos);
    CHECK(t.find("G82 X30. Y10. Z-12. R2. P0.3 F150.") != std::string::npos);
    CHECK(t.find("G83 X40. Y10. Z-20. R2. Q4. F120.") != std::string::npos);
    CHECK(t.find("G73 X50. Y10. Z-15. R2. Q3. F120.") != std::string::npos);
    CHECK(t.find("(TAP PITCH 1.25 TOP 0.)") != std::string::npos);
    CHECK(t.find("G99 G84 X10. Y10. Z-12. R3. F625.") != std::string::npos);
    CHECK(t.find(" G0 X60. Z2.\r\n") != std::string::npos); // bore, expanded: G85-like, ends at R (G99)
    CHECK(t.find(" G1 Z-8. F80.\r\n") != std::string::npos);
    CHECK(t.find(" G4 P0.2\r\n") != std::string::npos);
    CHECK(t.find("G80") != std::string::npos);
    CHECK(t.find("G4 P0.25") != std::string::npos);
    // Sequence numbers in steps of numberStep (5), the tool change with pre-select of the next tool.
    CHECK(t.find("N10 ") != std::string::npos);
    CHECK(t.find("N15 ") != std::string::npos);
    CHECK(t.find(" T1 M6\r\n") != std::string::npos);
    CHECK(t.find(" T2\r\n") != std::string::npos);
    // Comments sanitised (parentheses inside became spaces), tools listed in first-use order.
    CHECK(t.find("(T1 FLATENDMILL D10. 10 MM 3FL CARBIDE ROUGH)") != std::string::npos);
    CHECK(t.find("(T2 DRILL D6.8 6.8 MM DRILL)\r\n(T3 TAP D8. M8X1.25 TAP)") != std::string::npos);
    // Section 2 keeps the tool: no tool change, a warning.
    REQUIRE(iso->warnings.size() == 1);
    CHECK(iso->warnings[0] == "section 2 keeps tool 2");

    const auto plain = postGolden(kGoldens[2]);
    REQUIRE(plain.has_value());
    const std::string& p = plain->text;
    CHECK(p.find('\r') == std::string::npos); // the post's lineEnding default is lf
    // R arcs of at most 90 degrees; the helix as lines (helical off); cycles expanded.
    CHECK(p.find("G3 X40. Y20. R10.\n") != std::string::npos);
    CHECK(p.find("G2 X20. Y20. R10. F1000.") != std::string::npos);
    CHECK(p.find(" R5.") == std::string::npos);
    CHECK(p.find("G81") == std::string::npos);
    CHECK(p.find("G1 Z-12. F150.") != std::string::npos);
    CHECK(p.find("G4 P300") != std::string::npos);
    CHECK(p.find("(OUT)") != std::string::npos); // bore and ream feed out
    CHECK(p.find("N1 T1 M6") != std::string::npos);
    CHECK(p.find("N2 T2 M6") != std::string::npos);
    CHECK(p.find("N3") == std::string::npos); // section 2 keeps T2
    CHECK(p.find("(Setup Op 1 top, mm work offset 1)") != std::string::npos);
    CHECK(p.find("(10 mm 3FL carbide rough , flatEndMill D10.)") != std::string::npos); // Haas: lower case kept
    // The peck, expanded: re-entry 0.5 mm above the last depth, back to R after each peck.
    CHECK(p.find("G1 Z-2. F120.\nG0 Z2.\nZ-1.5\nG1 Z-6.\n") != std::string::npos);
}

TEST_CASE("units: inch output converts every length and feed", "[post][engine]")
{
    const auto inch = postGolden(kGoldens[1]);
    REQUIRE(inch.has_value());
    const std::string& t = inch->text;
    CHECK(t.find("G20") != std::string::npos);
    CHECK(t.find("G3 X1.5748 Y.7874") == std::string::npos);                  // leading zeros are kept
    CHECK(t.find("G3 X1.5748 Y0.7874 I0. J0.3937\r\n") != std::string::npos); // 40 mm, 20 mm, 10 mm
    CHECK(t.find(" X1.1811 F47.24\r\n") != std::string::npos);                // 30 mm at 1200 mm/min
    CHECK(t.find(" Z0.1969\r\n") != std::string::npos); // 5 mm = 0.19685039 in, on the nanometre grid
    CHECK(t.find("G99 G81 X0.3937 Y0.3937 Z-0.4724 R0.0787 F5.91") != std::string::npos);
    CHECK(t.find("(T2 DRILL D0.268 6.8 MM DRILL)") != std::string::npos);
    CHECK(t.find("(DATE 2026-01-02)") != std::string::npos);
}

TEST_CASE("properties: declared defaults, machine and setup overrides", "[post][engine]")
{
    Program program = goldenProgram(true);
    program.machine.postPropertyOverrides["numberStep"] = "2";
    program.machine.postPropertyOverrides["otherPostsSetting"] = "x"; // not this post's: silently ignored
    program.properties["numberBlocks"] = "true";
    program.properties["header"] = "setup header";
    program.properties["unknownKey"] = "1";
    auto out = postWith("iso-test.post.js", program);
    REQUIRE(out.has_value());
    CHECK(out->text.find("N12 ") != std::string::npos); // 10, 12, 14...
    CHECK(out->text.find("(SETUP HEADER FOR TESTER)") != std::string::npos);
    REQUIRE(out->warnings.size() == 2);
    CHECK(out->warnings[0] == "post property 'unknownKey' is not declared by the post and was ignored");

    program.properties["numberStep"] = "3"; // the setup wins over the machine
    program.properties["numberBlocks"] = "false";
    out = postWith("iso-test.post.js", program);
    REQUIRE(out.has_value());
    CHECK(out->text.find("N1") == std::string::npos);

    program.properties["lineEnding"] = "lf";
    out = postWith("iso-test.post.js", program);
    REQUIRE(out.has_value());
    CHECK(out->text.find('\r') == std::string::npos);

    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{{"numberStep", "1000"},
                                                                                     {"numberStep", "2.5"},
                                                                                     {"homeZ", "abc"},
                                                                                     {"homeZ", ""},
                                                                                     {"coolantStyle", "m7"},
                                                                                     {"numberBlocks", "yes"},
                                                                                     {"lineEnding", "cr"}}) {
        INFO(key << " = " << value);
        Program bad = goldenProgram(true);
        bad.properties[key] = value;
        const auto r = postWith("iso-test.post.js", bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    Program number = goldenProgram(true);
    number.properties["homeZ"] = "1.5e1";
    out = postWith("iso-test.post.js", number);
    REQUIRE(out.has_value());
    CHECK(out->text.find("G53 Z15.") != std::string::npos);
}

TEST_CASE("the date comes only from the job, and only with includeDate", "[post][engine]")
{
    Program program = goldenProgram(true);
    auto out = postWith("iso-test.post.js", program, std::string("2026-05-06"));
    REQUIRE(out.has_value());
    CHECK(out->text.find("2026") == std::string::npos); // includeDate is off by default
    program.properties["includeDate"] = "true";
    out = postWith("iso-test.post.js", program, std::string("2026-05-06"));
    REQUIRE(out.has_value());
    CHECK(out->text.find("(DATE 2026-05-06)") != std::string::npos);
    out = postWith("iso-test.post.js", program);
    REQUIRE(out.has_value());
    CHECK(out->text.find("(DATE") == std::string::npos);
    CHECK(std::any_of(out->warnings.begin(), out->warnings.end(), [](const std::string& w) {
        return w.find("includeDate is on but the post job carries no date") != std::string::npos;
    }));
}

TEST_CASE("jobs the post cannot take are refused", "[post][engine]")
{
    // Cutter compensation and tapping with a post that declares neither.
    Program comp = goldenProgram(false);
    comp.paths[0].moves.insert(comp.paths[0].moves.end() - 1,
                               linear(mm(45), mm(9), -mm(5), mmPerMin(900), cam::MoveRole::LeadIn, 2));
    auto r = postWith("plain-test.post.js", comp);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::PostFailed);
    CHECK(r.error().message.find("cutter compensation") != std::string::npos);
    CHECK(r.error().message.find("section 0 (Contour and arcs): move 14") != std::string::npos);
    Program tap = goldenProgram(true);
    tap.paths[0].moves.resize(14); // no XZ / YZ arcs, no compensation
    tap.paths[0].moves.push_back(rapid(mm(20), mm(4), mm(25)));
    r = postWith("plain-test.post.js", tap);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::PostFailed);
    CHECK(r.error().message.find("tapping cycle") != std::string::npos);
    // A section without a toolpath.
    Program program = goldenProgram(false);
    PostJob job = program.job();
    job.sections[1].toolpath = nullptr;
    QuickJsPostEngine engine;
    JobContext ctx;
    r = engine.run(postPath("iso-test.post.js"), job, ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::PostFailed);
    CHECK(r.error().message.find("has no toolpath") != std::string::npos);
    // An empty program still runs program start and end.
    Program empty;
    const auto e = postWith("iso-test.post.js", empty);
    REQUIRE(e.has_value());
    CHECK(e->text.rfind("%\r\nO1001 (BRACKET)\r\n", 0) == 0);
    CHECK(e->lineMap.empty());
}

TEST_CASE("posting is deterministic across runs and threads", "[post][engine]")
{
    const Golden& g = kGoldens[0];
    const auto first = postGolden(g);
    REQUIRE(first.has_value());
    std::vector<Hash256> hashes(6);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        threads.emplace_back([&, i] {
            const auto r = postGolden(g);
            hashes[i] = r ? r->hash() : Hash256{};
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (const Hash256& h : hashes) {
        CHECK(h == first->hash());
    }
}
