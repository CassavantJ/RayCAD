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
using cam::MoveKind;
using cam::MoveRole;

namespace {

constexpr const char* kPosts[] = {"generic-iso", "fanuc", "haas-ngc", "haas-classic"};

std::string shippedPost(const std::string& id)
{
    return std::string(RAY_SHIPPED_POSTS_DIR) + "/" + id + ".post.js";
}

cam::Toolpath goldenToolpath(const std::string& name)
{
    const std::string bytes = readFile(std::string(RAY_TOOLPATH_GOLDEN_DIR) + "/" + name + ".rctp");
    REQUIRE_FALSE(bytes.empty());
    auto p = cam::Toolpath::deserialize(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    REQUIRE(p);
    return *p;
}

lib::Tool endMill(double d, std::string name)
{
    return makeTool(std::move(name), lib::ToolType::FlatEndMill, d, 3);
}

// ---------- the machine every matrix program runs on ----------
lib::MachineDef machineFor(const std::string& post)
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
    } else {
        m.name = "Test VMC (ISO)";
        m.controlDialect = "generic-iso";
    }
    m.model = "";
    return m;
}

// The setup's text: the program number and the header comment saying what the file shows.
Program titled(int number, const std::string& comment)
{
    Program p;
    p.setup.programNumber = number;
    p.setup.programComment = comment;
    return p;
}

cam::Toolpath& millSection(Program& p, const std::string& name, int tool, lib::Tool t, double rpm,
                           cam::CuttingData::Coolant coolant = cam::CuttingData::Coolant::Flood)
{
    cam::Toolpath& path = p.section(name, "contour2d", tool, std::move(t), rpm, coolant);
    return path;
}

cam::Toolpath& drillSection(Program& p, const std::string& name, int tool, lib::Tool t, double rpm,
                            const std::string& golden)
{
    cam::Toolpath& path = p.section(name, "drill", tool, std::move(t), rpm);
    path = goldenToolpath(golden);
    return path;
}

// ---------- the feature programs ----------
// Arcs: four counter-clockwise quarter arcs, one per quadrant, about (30, 20), then the same clockwise, a full circle
// each way, and arcs of 135 and 270 degrees.
Program arcsXY()
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
Program arcsPlanes()
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
Program helical()
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
    m.push_back(dwell(500));                                                    // half a second there (G4)
    m.push_back(linear(mm(20), mm(20), -3'500'000, f));                         // to the centre
    m.push_back(rapid(mm(20), mm(25), mm(5)));
    m.push_back(rapid(mm(40), mm(30), mm(5)));
    m.push_back(linear(mm(40), mm(30), 0, mmPerMin(200), MoveRole::Plunge));
    // XZ helix: a half circle in X and Z about (45, -, 0) while Y moves 2 mm.
    m.push_back(arc(true, mm(50), mm(32), 0, mm(45), 0, 0, f, cam::ArcPlane::XZ));
    m.push_back(rapid(mm(50), mm(32), mm(25)));
    return p;
}

// Three tools, the second used by two operations (no tool change between them), length offset 25 for tool 5.
Program toolChange()
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
        cam::Toolpath& t = p.section("Spot", "drill", 3, makeTool("10 mm spot drill", lib::ToolType::SpotDrill, 10, 2),
                                     2000);
        auto& m = t.moves;
        m.push_back(rapid(mm(20), mm(45), mm(25)));
        m.push_back(rapid(mm(20), mm(45), mm(5)));
        m.push_back(linear(mm(20), mm(45), -mm(1), mmPerMin(100), MoveRole::Plunge));
        m.push_back(rapid(mm(20), mm(45), mm(25)));
    }
    return p;
}

// Cutter compensation in the control: G41 on a linear lead-in, arcs with it on, G40 on a linear lead-out; then G42.
Program cutterComp()
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
Program coolant()
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
Program names()
{
    Program p = titled(42, "Bracket (rev B) \xC3\xB8" "10 bore; 100% [check] \xE2\x80\x94 op 1/2 *top*");
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
Program sequence()
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
Program cycleProgram(int number, const std::string& comment, const std::string& golden, lib::Tool t, int toolNumber,
                     double rpm)
{
    Program p = titled(number, comment);
    drillSection(p, comment.substr(comment.find(':') + 2), toolNumber, std::move(t), rpm, golden);
    return p;
}

lib::Tool drillTool(double d, const std::string& name, lib::ToolType type = lib::ToolType::Drill)
{
    return makeTool(name, type, d, 2);
}

lib::Tool tapTool(double d, double pitch, const std::string& spec)
{
    lib::Tool t = makeTool(spec + " tap", lib::ToolType::Tap, d, 3);
    t.threadPitch = pitch;
    t.threadSpec = spec;
    return t;
}

// Right-hand taps from the golden toolpath, then a left-hand tap (G74) built by hand on its own operation (not for
// the generic post: RS274/NGC has no left-hand tapping cycle).
Program tapping(bool leftHand)
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

// Several tools: face, contour with compensation and arcs, spot, drill, peck, tap and a chamfer.
Program realistic()
{
    Program p = titled(2001, "Realistic: bracket top, six tools");
    p.setup.name = "Op 1 top";
    {
        cam::Toolpath& t = millSection(p, "Face top", 1, makeTool("50 mm face mill", lib::ToolType::FaceMill, 50, 5), 2500);
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
        cam::Toolpath& t =
            millSection(p, "Chamfer top edge", 6, makeTool("10 mm 90 deg chamfer mill", lib::ToolType::ChamferMill, 10, 4),
                        8000);
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
    const char* file;        // golden name without units and extension
    std::function<Program()> make;
    std::map<std::string, std::string> properties; // for every post
};

std::vector<Feature> matrix()
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
             return cycleProgram(1109, "Matrix: spot drilling G81", "drill_spot", drillTool(10, "10 mm spot drill", lib::ToolType::SpotDrill),
                                 3, 1500);
         },
         {}},
        {"10-drill-dwell",
         [] { return cycleProgram(1110, "Matrix: drilling with dwell G82", "drill_drill", drillTool(5, "5 mm drill"), 4, 2000); },
         {}},
        {"11-peck",
         [] {
             return cycleProgram(1111, "Matrix: peck drilling G83", "drill_peck_corpus", drillTool(6.6, "6.6 mm drill"), 4,
                                 2200);
         },
         {}},
        {"12-chip-break",
         [] {
             return cycleProgram(1112, "Matrix: chip-break drilling G73", "drill_chipbreak", drillTool(5, "5 mm drill"), 4,
                                 2400);
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
        {"20-realistic", realistic, {}},
    };
}

// Per post: property overrides a matrix program needs (codes the post leaves empty by default), and the post's own
// extra programs.
std::map<std::string, std::string> postProperties(const std::string& post, const std::string& feature)
{
    std::map<std::string, std::string> out;
    if (feature == "06-coolant") {
        // The codes a post has no default for (FANUC and generic: through and air; Haas: mist), as a shop would set
        // them. They are examples for the golden, not codes from a manual.
        if (post == "fanuc" || post == "generic-iso") {
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

std::vector<Extra> extras()
{
    return {
        {"fanuc", "30-tap-plain-g84", [] { return tapping(true); }, {{"tapping", "plainG84"}}},
        {"fanuc", "31-arcs-r", arcsXY, {{"arcFormat", "R"}}},
        {"fanuc", "32-g53-dwell-x-style-one", helical,
         {{"toolChangeRetract", "G53"}, {"endRetract", "G53"}, {"endPosition", "zThenXY"}, {"decimalStyle", "one"},
          {"dwellUnits", "X-seconds"}}},
        {"generic-iso", "30-arcs-r", arcsXY, {{"arcFormat", "R"}}},
        {"generic-iso", "31-dwell-ms-g28",
         [] {
             Program p = cycleProgram(1131, "ISO: dwell in milliseconds, G28 retracts", "drill_bore",
                                      makeTool("6 mm boring bar", lib::ToolType::BoringBar, 6, 1), 9, 900);
             return p;
         },
         {{"dwellUnits", "milliseconds"}, {"toolChangeRetract", "G28"}, {"endRetract", "G28"}}},
        {"generic-iso", "32-helical-dwell-ms", helical, {{"dwellUnits", "milliseconds"}}},
        {"haas-ngc", "30-g187-smoothing", realistic, {{"smoothing", "medium"}, {"smoothingCorner", "0.05"}}},
        {"haas-ngc", "31-peck-ijk", [] { return cycleProgram(1131, "Haas: G83 with I J K peck reduction", "drill_peck_corpus", drillTool(6.6, "6.6 mm drill"), 4, 2200); },
         {{"peckReduction", "true"}}},
        {"haas-ngc", "32-arcs-r", arcsXY, {{"arcFormat", "R"}}},
        {"haas-classic", "30-g187-smoothing", realistic, {{"smoothing", "finish"}}},
        {"haas-classic", "31-peck-ijk", [] { return cycleProgram(1131, "Haas: G83 with I J K peck reduction", "drill_peck_corpus", drillTool(6.6, "6.6 mm drill"), 4, 2200); },
         {{"peckReduction", "true"}}},
    };
}

struct Run {
    std::string post;
    std::string file; // <name>_<mm|inch>.nc
    Program program;
};

std::vector<Run> goldenRuns()
{
    std::vector<Run> runs;
    for (const char* post : kPosts) {
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

Result<PostOutput> postRun(const std::string& post, const Program& program, std::optional<std::string> date = {})
{
    QuickJsPostEngine engine;
    JobContext ctx;
    PostJob job = program.job();
    job.documentName = "post matrix";
    job.fixedDate = std::move(date);
    return engine.run(shippedPost(post), job, ctx);
}

std::string goldenPath(const std::string& post, const std::string& file)
{
    return std::string(RAY_GOLDEN_GCODE_ROOT) + "/" + post + "/" + file;
}

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
    return std::any_of(w.begin(), w.end(), [](const Word& x) { return std::string("XYZIJKR").find(x.letter) != std::string::npos; });
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
        auto hasG = [&](int code) { return std::find(g.begin(), g.end(), code) != g.end(); };
        auto hasM = [&](int code) { return std::find(m.begin(), m.end(), code) != m.end(); };
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

namespace {

// A pairwise covering set over the declared properties' values: every pair of values of two properties appears in
// at least one combination (greedy, deterministic).
std::vector<std::map<std::string, std::string>> pairwise(const std::vector<std::pair<std::string, std::vector<std::string>>>& params)
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

std::vector<std::string> valuesOf(const PostProperty& p)
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

} // namespace

TEST_CASE("shipped posts: every pairwise property combination passes the structural checks", "[post][shipped]")
{
    for (const char* id : kPosts) {
        QuickJsPostEngine engine;
        const auto info = engine.inspect(shippedPost(id));
        REQUIRE(info.has_value());
        std::vector<std::pair<std::string, std::vector<std::string>>> params;
        for (const PostProperty& p : info->properties) {
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
        const auto combos = pairwise(params);
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
         {}, "on an arc", false},
        {"comp left on",
         [](Program& p) {
             auto& m = p.paths[1].moves;
             m[m.size() - 2].compensation = 1; // the lead-out keeps compensation, the rapid follows
         },
         {}, "cutter compensation", false},
        {"spindle too fast", [](Program& p) { p.ops[1].cutting.spindleRpm = 20000; }, {}, "above the machine's", false},
        {"no spindle speed", [](Program& p) { p.ops[1].cutting.spindleRpm = 0; }, {}, "no spindle speed", false},
        {"mist without a code", [](Program& p) { p.ops[1].cutting.coolant = cam::CuttingData::Coolant::Air; },
         {{"coolantCodes", "flood=M8/M9 mist=M7/M9 through=/ air=/"}}, "coolant mode 'air' has no M code", false},
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
         {}, "-hand tap", false},
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
