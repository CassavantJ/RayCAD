// Hand-built toolpaths, tools and machines for the post host tests (T-039). The toolpath IR is cam.h's Toolpath and
// Move; nothing here needs src/cam (T-032) beyond the header's types.
#pragma once
#include "engine.h"

#include <ray/cam.h>
#include <ray/libraries.h>
#include <ray/post.h>

#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ray::post::test {

constexpr Nm mm(std::int64_t v)
{
    return v * 1'000'000;
}
constexpr Nm um(std::int64_t v)
{
    return v * 1'000;
}
constexpr std::int32_t mmPerMin(std::int32_t v)
{
    return v * 1000;
}

inline cam::Move rapid(Nm x, Nm y, Nm z)
{
    cam::Move m{};
    m.kind = cam::MoveKind::Rapid;
    m.role = cam::MoveRole::Link;
    m.x = x;
    m.y = y;
    m.z = z;
    return m;
}
inline cam::Move linear(Nm x, Nm y, Nm z, std::int32_t feed, cam::MoveRole role = cam::MoveRole::Cut,
                        std::uint8_t comp = 0)
{
    cam::Move m = rapid(x, y, z);
    m.kind = cam::MoveKind::Linear;
    m.role = role;
    m.feedMicronPerMin = feed;
    m.compensation = comp;
    return m;
}
// Arc to (x, y, z) about the absolute centre (i, j, k); only the in-plane coordinates of the centre matter.
inline cam::Move arc(bool cw, Nm x, Nm y, Nm z, Nm i, Nm j, Nm k, std::int32_t feed,
                     cam::ArcPlane plane = cam::ArcPlane::XY)
{
    cam::Move m = linear(x, y, z, feed);
    m.kind = cw ? cam::MoveKind::ArcCW : cam::MoveKind::ArcCCW;
    m.i = i;
    m.j = j;
    m.k = k;
    m.plane = plane;
    return m;
}
inline cam::Move cycle(cam::MoveKind kind, Nm x, Nm y, Nm bottom, Nm retract, std::int32_t feed, Nm peck = 0,
                       std::int32_t dwellMs = 0, std::int32_t pitchNm = 0)
{
    cam::Move m = rapid(x, y, bottom);
    m.kind = kind;
    m.role = cam::MoveRole::Plunge;
    m.feedMicronPerMin = feed;
    m.cycleRetract = retract;
    m.cyclePeck = peck;
    m.cycleDwellMs = dwellMs;
    m.threadPitchNm = pitchNm;
    return m;
}
inline cam::Move dwell(std::int32_t ms)
{
    cam::Move m{};
    m.kind = cam::MoveKind::Dwell;
    m.cycleDwellMs = ms;
    return m;
}
inline cam::Move cycleCancel()
{
    cam::Move m{};
    m.kind = cam::MoveKind::CycleCancel;
    return m;
}

inline lib::Tool makeTool(std::string name, lib::ToolType type, double diameter, int flutes)
{
    lib::Tool t{};
    t.name = std::move(name);
    t.type = type;
    t.diameter = diameter;
    t.flutes = flutes;
    t.fluteLength = 3 * diameter;
    return t;
}

inline lib::MachineDef makeMachine()
{
    lib::MachineDef m{};
    m.name = "Test mill";
    m.vendor = "RayCAD tests";
    m.model = "TM-1";
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', 0, 762, 25400, 16500, 5000}, lib::AxisDef{'Y', 0, 406, 25400, 16500, 5000},
              lib::AxisDef{'Z', -508, 0, 25400, 16500, 5000}};
    m.maxRpm = 8100;
    m.toolCapacity = 20;
    m.controlDialect = "generic-iso";
    m.rigidTapping = true;
    m.maxBlockNumber = 99999;
    return m;
}

// A program under construction: add sections, then job().
struct Program {
    cam::Setup setup{};
    lib::MachineDef machine = makeMachine();
    std::deque<cam::Operation> ops;
    std::deque<lib::Tool> tools;
    std::deque<lib::Holder> holders;
    std::deque<cam::Toolpath> paths;
    UnitSystem units = UnitSystem::Metric;
    std::map<std::string, std::string> properties;

    Program()
    {
        setup.name = "Op 1 top";
        setup.programNumber = 1001;
        setup.programComment = "host test part (rev A)";
        setup.wcs.workOffset = 1;
    }

    cam::Toolpath& section(std::string name, std::string type, int toolNumber, lib::Tool tool, double rpm = 10000,
                           cam::CuttingData::Coolant coolant = cam::CuttingData::Coolant::Flood)
    {
        cam::Operation op{};
        op.name = std::move(name);
        op.type = std::move(type);
        op.tool.toolNumber = toolNumber;
        op.tool.lengthOffset = toolNumber;
        op.tool.diameterOffset = toolNumber;
        op.tool.stickOut = 30;
        op.cutting.spindleRpm = rpm;
        op.cutting.clockwise = true;
        op.cutting.coolant = coolant;
        op.heights = cam::Heights{25, 5, 2, 0, -10};
        ops.push_back(std::move(op));
        tools.push_back(std::move(tool));
        holders.push_back(lib::Holder{{}, "CAT40 ER32", "", {}, 60});
        paths.emplace_back();
        cam::Toolpath& p = paths.back();
        p.startX = 0;
        p.startY = 0;
        p.startZ = mm(25);
        return p;
    }

    PostJob job() const
    {
        PostJob j;
        j.setup = &setup;
        j.machine = &machine;
        j.outputUnits = units;
        j.properties = properties;
        j.documentName = "bracket";
        j.userName = "tester";
        for (std::size_t i = 0; i < ops.size(); ++i) {
            j.sections.push_back(PostSection{&ops[i], &paths[i], &tools[i], &holders[i]});
        }
        return j;
    }
};

inline std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

inline std::string postPath(const std::string& name)
{
    return std::string(RAY_POST_TEST_DIR) + "/posts/" + name;
}
inline std::string scriptPath(const std::string& name)
{
    return std::string(RAY_POST_TEST_DIR) + "/scripts/" + name;
}

// Splits text into lines (CRLF or LF).
inline std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from < text.size()) {
        std::size_t end = text.find('\n', from);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(from, end - from);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
        from = end + 1;
    }
    return out;
}

// The line number the error message names ("name:LINE: ..."), or -1.
inline int errorLine(const std::string& message, const std::string& scriptName)
{
    const std::string key = scriptName + ":";
    if (message.rfind(key, 0) != 0) {
        return -1;
    }
    int line = 0;
    std::size_t p = key.size();
    if (p >= message.size() || message[p] < '0' || message[p] > '9') {
        return -1;
    }
    for (; p < message.size() && message[p] >= '0' && message[p] <= '9'; ++p) {
        line = line * 10 + (message[p] - '0');
    }
    return line;
}

// The line holding the /*HERE*/ marker, 1-based, or -1.
inline int markerLine(const std::string& source)
{
    const std::size_t p = source.find("/*HERE*/");
    if (p == std::string::npos) {
        return -1;
    }
    int line = 1;
    for (std::size_t i = 0; i < p; ++i) {
        line += source[i] == '\n' ? 1 : 0;
    }
    return line;
}

// A minimal post definition around the given handlers, appended after a test script (function declarations are
// hoisted, so the script's own line numbers are unchanged).
inline const char* kTestPostTail = R"(
function testPost(handlers) {
  const base = { title: "test", dialect: "generic-iso", fileExtension: ".nc", apiVersion: 1,
    capabilities: { arcs: "IJK", helical: true, maxArcDegrees: 360, cannedCycles: [], rigidTap: false, cutterComp: false },
    properties: {} };
  return Object.assign(base, handlers);
}
)";

// A short toolpath of linear moves (for scripts that need handler calls).
inline Program linearProgram(std::size_t moves)
{
    Program p;
    cam::Toolpath& path = p.section("lines", "contour2d", 7, makeTool("6 mm end mill", lib::ToolType::FlatEndMill, 6, 3));
    for (std::size_t i = 0; i < moves; ++i) {
        path.moves.push_back(linear(um(static_cast<std::int64_t>(i) * 10), um(static_cast<std::int64_t>(i % 7)), -mm(1),
                                    mmPerMin(1200)));
    }
    return p;
}

} // namespace ray::post::test
