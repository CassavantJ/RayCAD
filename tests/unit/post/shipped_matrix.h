// The shipped posts' feature matrix (tasks T-040, T-041): the programs every shipped post is posted with, their
// properties, and the golden file each one is compared with. Shared by shipped_posts_test.cpp (the goldens byte for
// byte, the structural checks) and roundtrip/ (T-043: the independent G-code interpreter reads the same goldens back
// and compares them with these toolpaths). Moved here unchanged from shipped_posts_test.cpp.
#pragma once
#include "engine.h"
#include "post_test_util.h"

#include <ray/cam.h>
#include <ray/core.h>
#include <ray/post.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ray::post::test::shipped {

using cam::MoveKind;
using cam::MoveRole;
// The shipped posts. The T-043 interpreter (roundtrip/) has a dialect for every one of them and iterates these
// (kPosts, goldenRuns()); shipped_posts_test.cpp iterates allPosts() and goldenRuns(true), the same set.
inline constexpr const char* kPosts[] = {"generic-iso",  "fanuc",        "haas-ngc",
                                         "haas-classic", "mazak-smooth", "mazak-matrix"};

inline std::vector<std::string> allPosts()
{
    return std::vector<std::string>(std::begin(kPosts), std::end(kPosts));
}

inline bool isMazak(const std::string& post)
{
    return post.rfind("mazak", 0) == 0;
}

inline std::string shippedPost(const std::string& id)
{
    return std::string(RAY_SHIPPED_POSTS_DIR) + "/" + id + ".post.js";
}

inline cam::Toolpath goldenToolpath(const std::string& name)
{
    const std::string bytes = readFile(std::string(RAY_TOOLPATH_GOLDEN_DIR) + "/" + name + ".rctp");
    REQUIRE_FALSE(bytes.empty());
    auto p = cam::Toolpath::deserialize(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    REQUIRE(p);
    return *p;
}

inline lib::Tool endMill(double d, std::string name)
{
    return makeTool(std::move(name), lib::ToolType::FlatEndMill, d, 3);
}

// ---------- the machine every matrix program runs on ----------
inline lib::MachineDef machineFor(const std::string& post)
{
    lib::MachineDef m = makeMachine();
    m.maxRpm = 12000;
    m.rigidTapping = true;
    m.maxBlockNumber = 0; // the posts' own limits apply
    if (post == "fanuc") {
        m.name = "Test VMC (FANUC 0i-MF)";
        m.controlDialect = "fanuc";
    } else if (post == "haas-ngc") {
        m.name = "Test VMC (Haas NGC)";
        m.controlDialect = "haas-ngc";
    } else if (post == "haas-classic") {
        m.name = "Test VMC (Haas classic)";
        m.controlDialect = "haas-classic";
    } else if (post == "mazak-smooth") {
        m.name = "Test VMC (Mazak Smooth)";
        m.controlDialect = "mazak-smooth";
    } else if (post == "mazak-matrix") {
        m.name = "Test VMC (Mazak Matrix)";
        m.controlDialect = "mazak-matrix";
    } else {
        m.name = "Test VMC (ISO)";
        m.controlDialect = "generic-iso";
    }
    m.model = "";
    return m;
}

// The setup's text: the program number and the header comment saying what the file shows.
inline Program titled(int number, const std::string& comment)
{
    Program p;
    p.setup.programNumber = number;
    p.setup.programComment = comment;
    return p;
}

inline cam::Toolpath& millSection(Program& p, const std::string& name, int tool, lib::Tool t, double rpm,
                                  cam::CuttingData::Coolant coolant = cam::CuttingData::Coolant::Flood)
{
    cam::Toolpath& path = p.section(name, "contour2d", tool, std::move(t), rpm, coolant);
    return path;
}

inline cam::Toolpath& drillSection(Program& p, const std::string& name, int tool, lib::Tool t, double rpm,
                                   const std::string& golden)
{
    cam::Toolpath& path = p.section(name, "drill", tool, std::move(t), rpm);
    path = goldenToolpath(golden);
    return path;
}

// ---------- the feature programs ----------
// Arcs: four counter-clockwise quarter arcs, one per quadrant, about (30, 20), then the same clockwise, a full circle
// each way, and arcs of 135 and 270 degrees.
inline Program arcsXY()
{
    Program p = titled(1101, "Matrix: XY arcs in four quadrants, full circles, 135 and 270 degrees");
    cam::Toolpath& t = millSection(p, "XY arcs", 1, endMill(10, "10 mm 3FL end mill"), 6000);
    auto& m = t.moves;
    const std::int32_t f = mmPerMin(800);
    m.push_back(rapid(mm(30), mm(10), mm(25)));
    m.push_back(rapid(mm(30), mm(10), mm(5)));
    m.push_back(linear(mm(30), mm(10), -mm(2), mmPerMin(200), MoveRole::Plunge));
    m.push_back(arc(false, mm(40), mm(20), -mm(2), mm(30), mm(20), 0, f)); // quadrant IV -> I
    m.push_back(arc(false, mm(30), mm(30), -mm(2), mm(30), mm(20), 0, f)); // I -> II
    m.push_back(arc(false, mm(20), mm(20), -mm(2), mm(30), mm(20), 0, f)); // II -> III
    m.push_back(arc(false, mm(30), mm(10), -mm(2), mm(30), mm(20), 0, f)); // III -> IV
    m.push_back(arc(true, mm(20), mm(20), -mm(2), mm(30), mm(20), 0, f));
    m.push_back(arc(true, mm(30), mm(30), -mm(2), mm(30), mm(20), 0, f));
    m.push_back(arc(true, mm(40), mm(20), -mm(2), mm(30), mm(20), 0, f));
    m.push_back(arc(true, mm(30), mm(10), -mm(2), mm(30), mm(20), 0, f));
    m.push_back(arc(false, mm(30), mm(10), -mm(2), mm(30), mm(20), 0, f)); // full circle CCW
    m.push_back(arc(true, mm(30), mm(10), -mm(2), mm(30), mm(20), 0, f));  // full circle CW
    // 135 degrees CCW from (30, 10) to (37.071068, 27.071068); then 270 degrees CW about the same centre.
    m.push_back(arc(false, 37'071'068, 27'071'068, -mm(2), mm(30), mm(20), 0, f));
    m.push_back(arc(true, 22'928'932, 27'071'068, -mm(2), mm(30), mm(20), 0, f));
    m.push_back(rapid(22'928'932, 27'071'068, mm(25)));
    return p;
}

// Arcs in the XZ (G18) and YZ (G19) planes: a ball end mill cutting half circles both ways.
inline Program arcsPlanes()
{
    Program p = titled(1102, "Matrix: arcs in the XZ (G18) and YZ (G19) planes");
    cam::Toolpath& t =
        millSection(p, "XZ and YZ arcs", 2, makeTool("6 mm ball end mill", lib::ToolType::BallEndMill, 6, 2), 8000);
    auto& m = t.moves;
    const std::int32_t f = mmPerMin(600);
    m.push_back(rapid(mm(10), mm(10), mm(25)));
    m.push_back(rapid(mm(10), mm(10), mm(2)));
    m.push_back(linear(mm(10), mm(10), 0, mmPerMin(200), MoveRole::Plunge));
    // XZ: centre (20, -, 0); CW (G2) seen from +Y from (10, 0) over the top... here downwards: below Z 0.
    m.push_back(arc(true, mm(30), mm(10), 0, mm(20), 0, 0, f, cam::ArcPlane::XZ));
    m.push_back(arc(false, mm(10), mm(10), 0, mm(20), 0, 0, f, cam::ArcPlane::XZ));
    // YZ: centre (-, 20, 0).
    m.push_back(arc(true, mm(10), mm(30), 0, 0, mm(20), 0, f, cam::ArcPlane::YZ));
    m.push_back(arc(false, mm(10), mm(10), 0, 0, mm(20), 0, f, cam::ArcPlane::YZ));
    m.push_back(rapid(mm(10), mm(10), mm(25)));
    return p;
}

// Helices: two full turns down 1.5 mm each about (20, 20), a quarter turn, and a helix in the XZ plane.
inline Program helical()
{
    Program p = titled(1103, "Matrix: helical arcs (full turns, a quarter turn, an XZ helix)");
    cam::Toolpath& t = millSection(p, "Helical entry", 3, endMill(8, "8 mm 3FL end mill"), 7000);
    auto& m = t.moves;
    const std::int32_t f = mmPerMin(500);
    m.push_back(rapid(mm(25), mm(20), mm(25)));
    m.push_back(rapid(mm(25), mm(20), mm(1)));
    m.push_back(linear(mm(25), mm(20), 0, mmPerMin(200), MoveRole::Plunge));
    m.push_back(arc(false, mm(25), mm(20), -1'500'000, mm(20), mm(20), 0, f));
    m.push_back(arc(false, mm(25), mm(20), -mm(3), mm(20), mm(20), 0, f));
    m.push_back(arc(false, mm(20), mm(25), -3'500'000, mm(20), mm(20), 0, f));
    m.push_back(arc(false, mm(20), mm(25), -3'500'000, mm(20), mm(20), 0, f)); // flat full circle at the bottom
    m.push_back(dwell(500));                                                   // half a second there (G4)
    m.push_back(linear(mm(20), mm(20), -3'500'000, f));                        // to the centre
    m.push_back(rapid(mm(20), mm(20), mm(5))); // straight up out of the bore, as an operation retracts
    m.push_back(rapid(mm(40), mm(30), mm(5)));
    m.push_back(linear(mm(40), mm(30), 0, mmPerMin(200), MoveRole::Plunge));
    // XZ helix: a half circle in X and Z about (45, -, 0) while Y moves 2 mm.
    m.push_back(arc(true, mm(50), mm(32), 0, mm(45), 0, 0, f, cam::ArcPlane::XZ));
    m.push_back(rapid(mm(50), mm(32), mm(25)));
    return p;
}

// Three tools, the second used by two operations (no tool change between them), length offset 25 for tool 5.
inline Program toolChange()
{
    Program p = titled(1104, "Matrix: tool changes with pre-select, G43 length offsets, a repeated tool");
    {
        cam::Toolpath& t = millSection(p, "Face", 1, makeTool("50 mm face mill", lib::ToolType::FaceMill, 50, 5), 3000);
        auto& m = t.moves;
        m.push_back(rapid(-mm(30), mm(10), mm(25)));
        m.push_back(rapid(-mm(30), mm(10), mm(2)));
        m.push_back(linear(-mm(30), mm(10), 0, mmPerMin(300), MoveRole::Plunge));
        m.push_back(linear(mm(110), mm(10), 0, mmPerMin(1500)));
        m.push_back(rapid(mm(110), mm(10), mm(25)));
    }
    for (int k = 0; k < 2; ++k) {
        cam::Toolpath& t = millSection(p, k == 0 ? "Slot A" : "Slot B", 5, endMill(6, "6 mm 2FL end mill"), 9000);
        p.ops.back().tool.lengthOffset = 25;
        p.ops.back().tool.diameterOffset = 25;
        auto& m = t.moves;
        const Nm y = mm(20 + 10 * k);
        m.push_back(rapid(mm(10), y, mm(25)));
        m.push_back(rapid(mm(10), y, mm(2)));
        m.push_back(linear(mm(10), y, -mm(3), mmPerMin(150), MoveRole::Plunge));
        m.push_back(linear(mm(60), y, -mm(3), mmPerMin(700)));
        m.push_back(rapid(mm(60), y, mm(k == 0 ? 5 : 25)));
    }
    {
        cam::Toolpath& t =
            p.section("Spot", "drill", 3, makeTool("10 mm spot drill", lib::ToolType::SpotDrill, 10, 2), 2000);
        auto& m = t.moves;
        m.push_back(rapid(mm(20), mm(45), mm(25)));
        m.push_back(rapid(mm(20), mm(45), mm(5)));
        m.push_back(linear(mm(20), mm(45), -mm(1), mmPerMin(100), MoveRole::Plunge));
        m.push_back(rapid(mm(20), mm(45), mm(25)));
    }
    return p;
}

// Cutter compensation in the control: G41 on a linear lead-in, arcs with it on, G40 on a linear lead-out; then G42.
inline Program cutterComp()
{
    Program p = titled(1105, "Matrix: cutter compensation G41 and G42 with linear leads");
    cam::Toolpath& t = millSection(p, "Contour with comp", 4, endMill(12, "12 mm 4FL end mill"), 5000);
    p.ops.back().tool.diameterOffset = 14;
    auto& m = t.moves;
    const std::int32_t f = mmPerMin(600);
    m.push_back(rapid(-mm(10), -mm(10), mm(25)));
    m.push_back(rapid(-mm(10), -mm(10), mm(2)));
    m.push_back(linear(-mm(10), -mm(10), -mm(5), mmPerMin(200), MoveRole::Plunge));
    m.push_back(linear(0, -mm(5), -mm(5), f, MoveRole::LeadIn, 1)); // G41 on
    m.push_back(linear(0, mm(40), -mm(5), f, MoveRole::Cut, 1));
    m.push_back(arc(true, mm(10), mm(50), -mm(5), mm(10), mm(40), 0, f));
    m.back().compensation = 1;
    m.push_back(linear(mm(50), mm(50), -mm(5), f, MoveRole::Cut, 1));
    m.push_back(linear(mm(50), 0, -mm(5), f, MoveRole::Cut, 1));
    m.push_back(linear(-mm(5), 0, -mm(5), f, MoveRole::Cut, 1));
    m.push_back(linear(-mm(10), mm(10), -mm(5), f, MoveRole::LeadOut, 0)); // G40 off
    m.push_back(rapid(-mm(10), mm(10), mm(2)));
    // A second contour the other way round, compensation right.
    m.push_back(rapid(mm(70), -mm(10), mm(2)));
    m.push_back(linear(mm(70), -mm(10), -mm(5), mmPerMin(200), MoveRole::Plunge));
    m.push_back(linear(mm(60), 0, -mm(5), f, MoveRole::LeadIn, 2)); // G42 on
    m.push_back(linear(mm(60), mm(30), -mm(5), f, MoveRole::Cut, 2));
    m.push_back(linear(mm(80), mm(30), -mm(5), f, MoveRole::Cut, 2));
    m.push_back(linear(mm(90), mm(40), -mm(5), f, MoveRole::LeadOut, 0)); // G40 off
    m.push_back(rapid(mm(90), mm(40), mm(25)));
    return p;
}

// Coolant: one tool through five operations with flood, mist, through-spindle, air and none, then a second tool.
inline Program coolant()
{
    Program p = titled(1106, "Matrix: coolant modes flood, mist, through-spindle, air and off");
    using C = cam::CuttingData::Coolant;
    const C modes[] = {C::Flood, C::Mist, C::ThroughTool, C::Air, C::Off};
    const char* names[] = {"Flood", "Mist", "Through spindle", "Air", "No coolant"};
    for (int k = 0; k < 5; ++k) {
        cam::Toolpath& t = millSection(p, names[k], 6, endMill(8, "8 mm through-coolant end mill"), 6000, modes[k]);
        auto& m = t.moves;
        const Nm x = mm(10 * (k + 1));
        m.push_back(rapid(x, mm(10), mm(25)));
        m.push_back(rapid(x, mm(10), mm(2)));
        m.push_back(linear(x, mm(10), -mm(2), mmPerMin(200), MoveRole::Plunge));
        m.push_back(linear(x, mm(30), -mm(2), mmPerMin(800)));
        m.push_back(rapid(x, mm(30), mm(10)));
    }
    cam::Toolpath& t = millSection(p, "Flood, next tool", 7, endMill(4, "4 mm end mill"), 10000, C::Flood);
    t.moves.push_back(rapid(mm(70), mm(10), mm(25)));
    t.moves.push_back(rapid(mm(70), mm(10), mm(2)));
    t.moves.push_back(linear(mm(70), mm(10), -mm(1), mmPerMin(100), MoveRole::Plunge));
    t.moves.push_back(rapid(mm(70), mm(10), mm(25)));
    return p;
}

// Program number and comment sanitising: an odd program number, characters no control accepts, lower case, a
// parenthesis inside a comment, non-ASCII text and a long name.
inline Program names()
{
    Program p = titled(42, "Bracket (rev B) \xC3\xB8"
                           "10 bore; 100% [check] \xE2\x80\x94 op 1/2 *top*");
    p.setup.name = "Op 1 (top) \xE2\x80\x93 vise";
    cam::Toolpath& t = millSection(p, "Pocket (rough) \xE2\x80\x94 \"A\" side", 8,
                                   endMill(10, "10mm (3FL) end-mill, ALTiN coated \xC2\xB5m"), 6000);
    t.moves.push_back(rapid(mm(10), mm(10), mm(25)));
    t.moves.push_back(rapid(mm(10), mm(10), mm(2)));
    t.moves.push_back(linear(mm(10), mm(10), -mm(1), mmPerMin(200), MoveRole::Plunge));
    t.moves.push_back(rapid(mm(10), mm(10), mm(25)));
    return p;
}

// Sequence numbers on every block (the property), over two tools.
inline Program sequence()
{
    Program p = titled(1108, "Matrix: sequence numbers on every block");
    for (int k = 0; k < 2; ++k) {
        cam::Toolpath& t = millSection(p, k == 0 ? "First tool" : "Second tool", 1 + k,
                                       endMill(k == 0 ? 10 : 6, k == 0 ? "10 mm end mill" : "6 mm end mill"), 6000);
        t.moves.push_back(rapid(mm(10), mm(10 + 20 * k), mm(25)));
        t.moves.push_back(rapid(mm(10), mm(10 + 20 * k), mm(2)));
        t.moves.push_back(linear(mm(10), mm(10 + 20 * k), -mm(1), mmPerMin(200), MoveRole::Plunge));
        t.moves.push_back(linear(mm(40), mm(10 + 20 * k), -mm(1), mmPerMin(800)));
        t.moves.push_back(rapid(mm(40), mm(10 + 20 * k), mm(25)));
    }
    return p;
}

// One canned cycle each, from the drilling operation's golden toolpaths.
inline Program cycleProgram(int number, const std::string& comment, const std::string& golden, lib::Tool t,
                            int toolNumber, double rpm)
{
    Program p = titled(number, comment);
    drillSection(p, comment.substr(comment.find(':') + 2), toolNumber, std::move(t), rpm, golden);
    return p;
}

inline lib::Tool drillTool(double d, const std::string& name, lib::ToolType type = lib::ToolType::Drill)
{
    return makeTool(name, type, d, 2);
}

inline lib::Tool tapTool(double d, double pitch, const std::string& spec)
{
    lib::Tool t = makeTool(spec + " tap", lib::ToolType::Tap, d, 3);
    t.threadPitch = pitch;
    t.threadSpec = spec;
    return t;
}

// Right-hand taps from the golden toolpath, then a left-hand tap (G74) built by hand on its own operation (not for
// the generic post: RS274/NGC has no left-hand tapping cycle).
inline Program tapping(bool leftHand)
{
    Program p = titled(1115, leftHand ? "Matrix: tapping M6x1, right hand (G84) and left hand (G74)"
                                      : "Matrix: tapping M6x1, right hand (G84)");
    drillSection(p, "Tap M6x1", 15, tapTool(6, 1, "M6x1"), 500, "drill_tap");
    if (!leftHand) {
        return p;
    }
    cam::Toolpath& t = p.section("Tap M6x1 LH", "drill", 16, tapTool(6, 1, "M6x1 LH"), 500);
    p.ops.back().cutting.clockwise = false;
    t.startX = mm(35);
    t.startY = mm(15);
    t.startZ = mm(35);
    t.moves.push_back(rapid(mm(35), mm(15), mm(35)));
    cam::Move h = levels(cycle(MoveKind::CycleTap, mm(35), mm(15), mm(8), mm(22), mmPerMin(500), 0, 0, 1'000'000),
                         mm(20), mm(35), cam::CycleReturn::Initial);
    h.cycleLeftHand = 1;
    t.moves.push_back(h);
    t.moves.push_back(cycleCancel());
    return p;
}

// One run of drilling holes whose cycle code changes: G81, then G82 (a dwell), then G81 again with a deeper hole and
// another R plane. The code change restarts the cycle from the initial level; the depth and R change do not.
inline Program cycleChange()
{
    Program p = titled(1116, "Matrix: cycle code and values changing inside one run");
    cam::Toolpath& t = p.section("Drill, mixed", "drill", 4, drillTool(5, "5 mm drill"), 2000);
    t.startX = mm(10);
    t.startY = mm(10);
    t.startZ = mm(30);
    auto& m = t.moves;
    m.push_back(rapid(mm(10), mm(10), mm(30)));
    const auto hole = [](Nm x, Nm bottom, Nm r, std::int32_t dwellMs, cam::CycleReturn ret) {
        return levels(cycle(MoveKind::CycleDrill, x, mm(10), bottom, r, mmPerMin(150), 0, dwellMs), mm(20), mm(30),
                      ret);
    };
    m.push_back(hole(mm(10), mm(12), mm(22), 0, cam::CycleReturn::RPlane));
    m.push_back(hole(mm(20), mm(12), mm(22), 200, cam::CycleReturn::RPlane));
    m.push_back(hole(mm(30), mm(8), mm(23), 200, cam::CycleReturn::RPlane));
    m.push_back(hole(mm(40), mm(8), mm(23), 0, cam::CycleReturn::Initial));
    m.push_back(cycleCancel());
    return p;
}

// Several tools: face, contour with compensation and arcs, spot, drill, peck, tap and a chamfer.
inline Program realistic()
{
    Program p = titled(2001, "Realistic: bracket top, six tools");
    p.setup.name = "Op 1 top";
    {
        cam::Toolpath& t =
            millSection(p, "Face top", 1, makeTool("50 mm face mill", lib::ToolType::FaceMill, 50, 5), 2500);
        auto& m = t.moves;
        m.push_back(rapid(-mm(30), mm(12), mm(25)));
        m.push_back(rapid(-mm(30), mm(12), mm(22)));
        m.push_back(linear(-mm(30), mm(12), mm(20), mmPerMin(400), MoveRole::Plunge));
        m.push_back(linear(mm(110), mm(12), mm(20), mmPerMin(1200)));
        m.push_back(linear(mm(110), mm(38), mm(20), mmPerMin(1200)));
        m.push_back(linear(-mm(30), mm(38), mm(20), mmPerMin(1200)));
        m.push_back(rapid(-mm(30), mm(38), mm(35)));
    }
    {
        cam::Toolpath& t = millSection(p, "Contour outside", 2, endMill(10, "10 mm 3FL end mill"), 6000);
        auto& m = t.moves;
        const std::int32_t f = mmPerMin(900);
        m.push_back(rapid(-mm(10), -mm(10), mm(35)));
        m.push_back(rapid(-mm(10), -mm(10), mm(22)));
        m.push_back(linear(-mm(10), -mm(10), mm(12), mmPerMin(300), MoveRole::Plunge));
        m.push_back(linear(0, -mm(5), mm(12), f, MoveRole::LeadIn, 1));
        m.push_back(linear(0, mm(45), mm(12), f, MoveRole::Cut, 1));
        m.push_back(arc(true, mm(5), mm(50), mm(12), mm(5), mm(45), 0, f));
        m.back().compensation = 1;
        m.push_back(linear(mm(75), mm(50), mm(12), f, MoveRole::Cut, 1));
        m.push_back(arc(true, mm(80), mm(45), mm(12), mm(75), mm(45), 0, f));
        m.back().compensation = 1;
        m.push_back(linear(mm(80), mm(5), mm(12), f, MoveRole::Cut, 1));
        m.push_back(arc(true, mm(75), 0, mm(12), mm(75), mm(5), 0, f));
        m.back().compensation = 1;
        m.push_back(linear(-mm(5), 0, mm(12), f, MoveRole::Cut, 1));
        m.push_back(linear(-mm(10), mm(10), mm(12), f, MoveRole::LeadOut, 0));
        m.push_back(rapid(-mm(10), mm(10), mm(35)));
    }
    drillSection(p, "Spot holes", 3, drillTool(10, "10 mm 90 deg spot drill", lib::ToolType::SpotDrill), 1500,
                 "drill_spot");
    drillSection(p, "Drill 5 mm with dwell", 4, drillTool(5, "5 mm drill"), 2000, "drill_drill");
    drillSection(p, "Chip-break drill", 4, drillTool(5, "5 mm drill"), 2400, "drill_chipbreak");
    drillSection(p, "Tap M6x1", 5, tapTool(6, 1, "M6x1"), 500, "drill_tap");
    {
        cam::Toolpath& t = millSection(p, "Chamfer top edge", 6,
                                       makeTool("10 mm 90 deg chamfer mill", lib::ToolType::ChamferMill, 10, 4), 8000);
        auto& m = t.moves;
        const std::int32_t f = mmPerMin(700);
        m.push_back(rapid(mm(40), -mm(8), mm(35)));
        m.push_back(rapid(mm(40), -mm(8), mm(22)));
        m.push_back(linear(mm(40), -mm(8), mm(19), mmPerMin(300), MoveRole::Plunge));
        m.push_back(arc(false, mm(40), -mm(8), mm(19), mm(40), mm(25), 0, f)); // a full circle round the part
        m.push_back(rapid(mm(40), -mm(8), mm(35)));
    }
    return p;
}

struct Feature {
    const char* file; // golden name without units and extension
    std::function<Program()> make;
    std::map<std::string, std::string> properties; // for every post
};

inline std::vector<Feature> matrix()
{
    return {
        {"01-arcs-xy", arcsXY, {}},
        {"02-arcs-planes", arcsPlanes, {}},
        {"03-helical", helical, {}},
        {"04-tool-change", toolChange, {}},
        {"05-cutter-comp", cutterComp, {}},
        {"06-coolant", coolant, {}},
        {"07-names", names, {}},
        {"08-sequence-numbers", sequence, {{"sequenceNumbers", "every"}}},
        {"09-spot-drill",
         [] {
             return cycleProgram(1109, "Matrix: spot drilling G81", "drill_spot",
                                 drillTool(10, "10 mm spot drill", lib::ToolType::SpotDrill), 3, 1500);
         },
         {}},
        {"10-drill-dwell",
         [] {
             return cycleProgram(1110, "Matrix: drilling with dwell G82", "drill_drill", drillTool(5, "5 mm drill"), 4,
                                 2000);
         },
         {}},
        {"11-peck",
         [] {
             return cycleProgram(1111, "Matrix: peck drilling G83", "drill_peck_corpus", drillTool(6.6, "6.6 mm drill"),
                                 4, 2200);
         },
         {}},
        {"12-chip-break",
         [] {
             return cycleProgram(1112, "Matrix: chip-break drilling G73", "drill_chipbreak", drillTool(5, "5 mm drill"),
                                 4, 2400);
         },
         {}},
        {"13-bore",
         [] {
             return cycleProgram(1113, "Matrix: boring with dwell G89", "drill_bore",
                                 makeTool("6 mm boring bar", lib::ToolType::BoringBar, 6, 1), 9, 900);
         },
         {}},
        {"14-ream",
         [] {
             return cycleProgram(1114, "Matrix: reaming G85", "drill_ream",
                                 makeTool("6.6 mm reamer", lib::ToolType::Reamer, 6.6, 6), 10, 400);
         },
         {}},
        {"15-tap", [] { return tapping(true); }, {}},
        {"16-cycle-change", cycleChange, {}},
        {"20-realistic", realistic, {}},
    };
}

// Per post: property overrides a matrix program needs (codes the post leaves empty by default), and the post's own
// extra programs.
inline std::map<std::string, std::string> postProperties(const std::string& post, const std::string& feature)
{
    std::map<std::string, std::string> out;
    if (feature == "06-coolant") {
        // The codes a post has no default for (FANUC and generic: through and air; Haas: mist), as a shop would set
        // them. They are examples for the golden, not codes from a manual.
        if (post == "fanuc" || post == "generic-iso" || isMazak(post)) {
            out["coolantCodes"] = "flood=M8/M9 mist=M7/M9 through=M88/M89 air=M83/M84";
        } else {
            out["coolantCodes"] = "flood=M08/M09 mist=M07/M09 through=M88/M89 air=M83/M84";
        }
    }
    return out;
}

struct Extra {
    const char* post;
    const char* file;
    std::function<Program()> make;
    std::map<std::string, std::string> properties;
};

inline std::vector<Extra> extras()
{
    return {
        {"fanuc", "30-tap-plain-g84", [] { return tapping(true); }, {{"tapping", "plainG84"}}},
        {"fanuc", "31-arcs-r", arcsXY, {{"arcFormat", "R"}}},
        {"fanuc",
         "32-g53-dwell-x-style-one",
         helical,
         {{"toolChangeRetract", "G53"},
          {"endRetract", "G53"},
          {"endPosition", "zThenXY"},
          {"decimalStyle", "one"},
          {"dwellUnits", "X-seconds"}}},
        {"generic-iso", "30-arcs-r", arcsXY, {{"arcFormat", "R"}}},
        {"generic-iso",
         "31-dwell-ms-g28",
         [] {
             Program p = cycleProgram(1131, "ISO: dwell in milliseconds, G28 retracts", "drill_bore",
                                      makeTool("6 mm boring bar", lib::ToolType::BoringBar, 6, 1), 9, 900);
             return p;
         },
         {{"dwellUnits", "milliseconds"}, {"toolChangeRetract", "G28"}, {"endRetract", "G28"}}},
        {"generic-iso", "32-helical-dwell-ms", helical, {{"dwellUnits", "milliseconds"}}},
        {"haas-ngc", "30-g187-smoothing", realistic, {{"smoothing", "medium"}, {"smoothingCorner", "0.05"}}},
        {"haas-ngc",
         "31-peck-ijk",
         [] {
             return cycleProgram(1131, "Haas: G83 with I J K peck reduction", "drill_peck_corpus",
                                 drillTool(6.6, "6.6 mm drill"), 4, 2200);
         },
         {{"peckReduction", "true"}}},
        {"haas-ngc", "32-arcs-r", arcsXY, {{"arcFormat", "R"}}},
        // Mazak (T-042): the tapping forms, the high-speed mode codes, the pre-select forms and the dwell unit, all
        // properties because none could be read in a Mazak document.
        {"mazak-smooth", "30-tap-g84-2", [] { return tapping(true); }, {{"tapping", "G84.2"}}},
        {"mazak-smooth", "31-tap-plain-g84", [] { return tapping(true); }, {{"tapping", "plainG84"}}},
        {"mazak-smooth", "32-high-speed-g61-1", arcsXY, {{"highSpeedMode", "G61.1"}}},
        {"mazak-smooth",
         "34-dwell-seconds",
         [] {
             return cycleProgram(1134, "Mazak: dwell P in seconds", "drill_drill", drillTool(5, "5 mm drill"), 4, 2000);
         },
         {{"dwellUnits", "seconds"}}},
        {"mazak-matrix", "30-tap-g84-2", [] { return tapping(true); }, {{"tapping", "G84.2"}}},
        {"mazak-matrix", "31-tap-plain-g84", [] { return tapping(true); }, {{"tapping", "plainG84"}}},
        {"mazak-matrix", "32-high-speed-g05-p2", arcsXY, {{"highSpeedMode", "G05P2"}}},
        {"mazak-matrix", "33-preselect-separate-t", toolChange, {{"toolPreselect", "separateT"}}},
        // The classic control documents G187 with E only: E is written, the level is warned about and not posted.
        {"haas-classic", "30-g187-smoothing", realistic, {{"smoothing", "finish"}, {"smoothingCorner", "0.05"}}},
        {"haas-classic",
         "31-peck-ijk",
         [] {
             return cycleProgram(1131, "Haas: G83 with I J K peck reduction", "drill_peck_corpus",
                                 drillTool(6.6, "6.6 mm drill"), 4, 2200);
         },
         {{"peckReduction", "true"}}},
    };
}

struct Run {
    std::string post;
    std::string file; // <name>_<mm|inch>.nc
    Program program;
};

// Every golden run of the posts with an interpreter dialect (kPosts), or of every shipped post (allPosts()).
inline std::vector<Run> goldenRuns(bool allShipped = false)
{
    std::vector<Run> runs;
    std::vector<std::string> posts(std::begin(kPosts), std::end(kPosts));
    if (allShipped) {
        posts = allPosts();
    }
    for (const std::string& postId : posts) {
        const char* post = postId.c_str();
        auto add = [&](const std::string& name, const std::function<Program()>& make,
                       const std::map<std::string, std::string>& props) {
            for (const UnitSystem u : {UnitSystem::Metric, UnitSystem::Inch}) {
                Run r{post, name + (u == UnitSystem::Inch ? "_inch.nc" : "_mm.nc"), make()};
                r.program.units = u;
                r.program.machine = machineFor(post);
                r.program.properties = props;
                for (const auto& [k, v] : postProperties(post, name)) {
                    r.program.properties[k] = v;
                }
                runs.push_back(std::move(r));
            }
        };
        for (const Feature& f : matrix()) {
            if (std::string(post) == "generic-iso" && std::string(f.file) == "15-tap") {
                add(f.file, [] { return tapping(false); }, f.properties);
                continue;
            }
            add(f.file, f.make, f.properties);
        }
        for (const Extra& e : extras()) {
            if (std::string(e.post) == post) {
                add(e.file, e.make, e.properties);
            }
        }
    }
    return runs;
}

inline Result<PostOutput> postRun(const std::string& post, const Program& program, std::optional<std::string> date = {})
{
    QuickJsPostEngine engine;
    JobContext ctx;
    PostJob job = program.job();
    job.documentName = "post matrix";
    job.fixedDate = std::move(date);
    return engine.run(shippedPost(post), job, ctx);
}

inline std::string goldenPath(const std::string& post, const std::string& file)
{
    return std::string(RAY_GOLDEN_GCODE_ROOT) + "/" + post + "/" + file;
}

// A pairwise covering set over the declared properties' values: every pair of values of two properties appears in
// at least one combination (greedy, deterministic).
inline std::vector<std::map<std::string, std::string>>
pairwise(const std::vector<std::pair<std::string, std::vector<std::string>>>& params)
{
    struct Pair {
        std::size_t a, va, b, vb;
        bool operator<(const Pair& o) const { return std::tie(a, va, b, vb) < std::tie(o.a, o.va, o.b, o.vb); }
    };
    std::set<Pair> open;
    for (std::size_t a = 0; a < params.size(); ++a) {
        for (std::size_t b = a + 1; b < params.size(); ++b) {
            for (std::size_t va = 0; va < params[a].second.size(); ++va) {
                for (std::size_t vb = 0; vb < params[b].second.size(); ++vb) {
                    open.insert(Pair{a, va, b, vb});
                }
            }
        }
    }
    std::vector<std::map<std::string, std::string>> out;
    while (!open.empty()) {
        std::vector<std::size_t> pick(params.size(), 0);
        // Seed with the first open pair, then choose each other value to cover the most open pairs.
        const Pair seed = *open.begin();
        pick[seed.a] = seed.va;
        pick[seed.b] = seed.vb;
        for (std::size_t p = 0; p < params.size(); ++p) {
            if (p == seed.a || p == seed.b) {
                continue;
            }
            std::size_t best = 0, bestCount = 0;
            for (std::size_t v = 0; v < params[p].second.size(); ++v) {
                std::size_t count = 0;
                for (std::size_t q = 0; q < p; ++q) {
                    const Pair k = q < p ? Pair{q, pick[q], p, v} : Pair{p, v, q, pick[q]};
                    count += open.count(k);
                }
                if (count > bestCount) {
                    best = v;
                    bestCount = count;
                }
            }
            pick[p] = best;
        }
        for (std::size_t a = 0; a < params.size(); ++a) {
            for (std::size_t b = a + 1; b < params.size(); ++b) {
                open.erase(Pair{a, pick[a], b, pick[b]});
            }
        }
        std::map<std::string, std::string> combo;
        for (std::size_t p = 0; p < params.size(); ++p) {
            combo[params[p].first] = params[p].second[pick[p]];
        }
        out.push_back(std::move(combo));
    }
    return out;
}

inline std::vector<std::string> valuesOf(const PostProperty& p)
{
    switch (p.type) {
    case PostProperty::Type::Bool:
        return {"false", "true"};
    case PostProperty::Type::Enum:
        return p.enumValues;
    case PostProperty::Type::String:
        return {p.defaultValue};
    case PostProperty::Type::Int:
    case PostProperty::Type::Number:
        break;
    }
    return {p.defaultValue};
}

// The property combinations a post is checked with: every pair of values of two of its declared properties.
inline std::vector<std::map<std::string, std::string>> pairwiseCombinations(const PostInfo& info)
{
    std::vector<std::pair<std::string, std::vector<std::string>>> params;
    for (const PostProperty& p : info.properties) {
        params.emplace_back(p.key, valuesOf(p));
    }
    // Numbers at their limits too, where the post gives them (the test knows which make sense to vary).
    for (auto& [key, values] : params) {
        if (key == "sequenceIncrement") {
            values = {"10", "1", "5"};
        } else if (key == "commentMaxLength") {
            values = {"0", "12"};
        } else if (key == "smoothingCorner") {
            values = {"0", "0.05", "0.2"};
        } else if (key == "peckReductionJ" || key == "peckMinimumK") {
            values = {"0.25", "0.5"};
        }
    }
    auto combos = pairwise(params);
    // toolChangeRetract = none is refused unless m6LeavesZRetracted asserts that the machine's M6 leaves Z up (FANUC,
    // generic): such combinations carry the assertion. The refusal itself is a case of the refusal test.
    for (auto& c : combos) {
        const auto retract = c.find("toolChangeRetract");
        if (retract != c.end() && retract->second == "none" && c.count("m6LeavesZRetracted") == 1) {
            c["m6LeavesZRetracted"] = "true";
        }
    }
    return combos;
}

} // namespace ray::post::test::shipped
