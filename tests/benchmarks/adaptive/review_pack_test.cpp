// The adaptive roughing quality review pack (task T-062, PLAN.md M4): docs/reports/adaptive.md and its images in
// docs/reports/adaptive/, rebuilt from the repository alone.
//
// WHAT IT DOES. The adaptive-roughed features of the benchmark parts BP1 to BP3 (PLAN.md 12.3) are modelled as solids
// with the kernel (the dimensions the plan gives; the ones it does not give are chosen here and listed in the report).
// Each feature is roughed twice, by the adaptive2d operation and by the plain pocket2d operation, with the same tool
// from the starter library at the parameters each would sensibly use: feeds from the project's own feeds calculator
// (cam.h computeFeeds) on the starter library's cutting data, stepdowns from the same data. For each toolpath:
//   - engagement, air cutting, gouge, uncut area and link removal from the independent sweep checker
//     (tests/tools/sweepcheck), fed from the toolpath's own moves (what the machine would run, arcs and links included)
//     level by level, against pocket outlines written here from the drawing's dimensions (not from the generators);
//   - the cycle-time estimate (cam.h estimateCycleTime) on the machine definition the plan assigns to the part;
//   - path lengths by kind of motion and feed moves per millimetre from the moves;
//   - a top view of the toolpath (SVG) and, per feature, an engagement histogram of both strategies (SVG).
// Plus spike S5's islands pocket at 10 % load (s5_reference.h), for the report's known issues.
// The values fill the template tests/benchmarks/adaptive/report.md.in ({{name}} placeholders).
//
// THE TESTS. "[adaptive_review]" builds the whole pack and fails when a placeholder has no value, when the template
// lacks a required field, or when docs/reports/adaptive.md or any image differs from what it builds (numbers with a
// decimal point may differ by one unit of their last digit, for floating point that differs between compilers;
// everything else must match exactly). To rebuild the files after a change:
//     ray_bench_adaptive_review "[write]"
// (a hidden test case). "[timing]" (hidden) times each generation alone; the report does not take its numbers from it.
#include "adaptive2d_test_util.h"
#include "s5_reference.h"
#include "sweepcheck.h"

#include <ray/cam.h>
#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;
namespace sw = ray::test::sweep;
namespace fs = std::filesystem;

namespace {

constexpr double kIn = 25.4;
double inch(double v)
{
    return v * kIn;
}

// ---------- Text ----------

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
void writeFile(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// Fixed decimals, no negative zero.
std::string num(double v, int decimals)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    std::string s = b;
    if (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos) {
        s.erase(0, 1);
    }
    return s;
}
std::string pct(double v, int decimals = 1)
{
    return num(v, decimals) + " %";
}
// A share that must be shown when it is not zero, however small.
std::string smallPct(double v)
{
    return v > 0 && v < 0.005 ? num(v, 3) + " %" : pct(v, 2);
}
std::string deg(double v)
{
    return num(v, 1) + "°";
}

// ---------- The starter library ----------

const std::string kLibrary = RAY_LIBRARY_DIR;

lib::Tool libTool(const std::string& id)
{
    auto t = lib::toolFromJson(readFile(kLibrary + "/tools/" + id + ".tool.json"));
    INFO("tool " << id);
    REQUIRE(t);
    return *t;
}
lib::Material libMaterial(const std::string& id)
{
    auto m = lib::materialFromJson(readFile(kLibrary + "/materials/" + id + ".material.json"));
    INFO("material " << id);
    REQUIRE(m);
    return *m;
}
lib::MachineDef libMachine(const std::string& id)
{
    auto m = lib::machineFromJson(readFile(kLibrary + "/machines/" + id + ".machine.json"));
    INFO("machine " << id);
    REQUIRE(m);
    return *m;
}
const lib::ToolPreset& presetOf(const lib::Tool& t, const lib::Material& m, const std::string& opClass)
{
    const auto it = std::find_if(t.presets.begin(), t.presets.end(),
                                 [&](const lib::ToolPreset& p) { return p.material == m.id && p.opClass == opClass; });
    INFO("no " << opClass << " preset of " << t.name << " for " << m.name);
    REQUIRE(it != t.presets.end());
    return *it;
}

// ---------- The checker's outlines (mm), written from the dimensions, not from any generator ----------

// An arc from a0 to a1 (radians, either direction) as points no farther than 0.5 um from the circle, both ends in.
void arcPts(sw::Loop& l, double cx, double cy, double r, double a0, double a1)
{
    const double step = 2 * std::acos(1 - 0.0005 / r);
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(a1 - a0) / step)));
    for (int k = 0; k <= n; ++k) {
        const double a = a0 + (a1 - a0) * k / n;
        l.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
}
sw::Loop rectLoop(double x0, double y0, double x1, double y1)
{
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}
sw::Loop roundedRectLoop(double x0, double y0, double x1, double y1, double r)
{
    sw::Loop l;
    arcPts(l, x1 - r, y0 + r, r, -sw::kPi / 2, 0);
    arcPts(l, x1 - r, y1 - r, r, 0, sw::kPi / 2);
    arcPts(l, x0 + r, y1 - r, r, sw::kPi / 2, sw::kPi);
    arcPts(l, x0 + r, y0 + r, r, sw::kPi, 3 * sw::kPi / 2);
    return l;
}
sw::Loop circleLoop2(double cx, double cy, double r)
{
    sw::Loop l;
    arcPts(l, cx, cy, r, 0, 2 * sw::kPi);
    l.pop_back(); // closed: the last point is the first
    return l;
}
sw::Loop obroundLoop(double x0, double y0, double x1, double y1)
{
    const double r = (y1 - y0) / 2, cy = (y0 + y1) / 2;
    sw::Loop l;
    arcPts(l, x1 - r, cy, r, -sw::kPi / 2, sw::kPi / 2);
    arcPts(l, x0 + r, cy, r, sw::kPi / 2, 3 * sw::kPi / 2);
    return l;
}
sw::Loops grownBox(const sw::Loops& loops, double by)
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const sw::Loop& l : loops) {
        for (const sw::Pt& p : l) {
            x0 = std::min(x0, p.first);
            y0 = std::min(y0, p.second);
            x1 = std::max(x1, p.first);
            y1 = std::max(y1, p.second);
        }
    }
    return {rectLoop(x0 - by, y0 - by, x1 + by, y1 + by)};
}

// ---------- The features ----------

struct Feature {
    std::string key, part, title;
    std::string modelled; // what was modelled and assumed, one sentence for the report
    double airLimit = 15; // air cutting allowed, percent: 10 on BP1's and BP2's pockets, 15 elsewhere (T-063)
    double depthMm = 0;
    std::shared_ptr<kernel::Shape> shape;
    std::vector<kernel::SubId> faces;
    lib::Tool tool;
    const lib::Material* material = nullptr;
    const lib::MachineDef* machine = nullptr;
    sw::Loops pocket, stock; // the checker's area to clear and stock outline at the feature's depths
};

struct Library {
    lib::Tool halfInch = libTool("3fb7624c-615b-4f07-8f57-ecc8432f07df");    // 1/2 in 3FL carbide flat end mill
    lib::Tool quarterInch = libTool("c8747aea-1ac5-4908-b062-c177d47b8a76"); // 1/4 in 3FL carbide flat end mill
    lib::Material al6061 = libMaterial("b72c5171-c258-44d2-91ae-ee049b636621");
    lib::Material steel4140ph = libMaterial("52267d99-5eb1-4530-9ef3-9a9165fb6421");
    lib::MachineDef haas = libMachine("b903ede7-5123-4c26-bc15-466e92fba4c6");  // Haas NGC style (VF-2 class)
    lib::MachineDef mazak = libMachine("1647afea-3110-4e8e-a439-fa1082f98a5d"); // Mazak VC-Ez 20 class
    lib::MachineDef fanuc = libMachine("79733809-d5a1-4b1d-9502-d9ccd73a1777"); // Fanuc ROBODRILL class
};

// The ring between the stock outline and a part outline (both counter-clockwise, the part inside), cut from z0 up.
kernel::Shape cutRing(const kernel::Shape& s, double sx0, double sy0, double sx1, double sy1, kernel::ProfileLoop part,
                      double z0, double z1)
{
    return cut(s, prism({roundedRect(sx0 - 1, sy0 - 1, sx1 + 1, sy1 + 1, 0), std::move(part)}, z0, z1 + 1));
}

// A block of stock: the part origin at the finished part's corner, the stock overhanging 0.125 in on every side.
struct Block {
    double sx0, sy0, sx1, sy1, t;
    sw::Loop stock() const { return rectLoop(sx0, sy0, sx1, sy1); }
};
const Block kBp1{inch(-0.125), inch(-0.125), inch(6.125), inch(4.125), inch(0.75)};
const Block kBp2{inch(-0.125), inch(-0.125), inch(3.125), inch(3.125), inch(2.1)};

// The models. Each feature is roughed on a model with only what it needs: on the whole part, the outside's operation
// also clears BP2's open step, which is open to the outside's area (the depth rule of pocket2d.h DEPTHS, which
// adaptive2d shares: area_depths.h; the report's first section and `whole` in buildPack below).
kernel::Shape bp1Model(bool ring, bool pockets)
{
    const Block& b = kBp1;
    kernel::Shape s = box(Vec3{b.sx0, b.sy0, 0}, Vec3{b.sx1, b.sy1, b.t});
    if (ring) {
        s = cutRing(s, b.sx0, b.sy0, b.sx1, b.sy1, roundedRect(0, 0, inch(6), inch(4), 0), b.t - inch(0.65), b.t);
    }
    if (pockets) {
        const double pz = b.t - inch(0.375);
        s = cut(s, prism({roundedRect(inch(0.75), inch(1.25), inch(2.75), inch(2.75), inch(0.25))}, pz, b.t + 1));
        s = cut(s, prism({roundedRect(inch(3.25), inch(1.25), inch(5.25), inch(2.75), inch(0.25))}, pz, b.t + 1));
    }
    return s;
}
// BP2 with the stock top at t: the outside `ringDepth` in deep (0: none), the step, the circular pocket.
kernel::Shape bp2Model(double t, double ringDepth, bool step, bool circle)
{
    const Block& b = kBp2;
    kernel::Shape s = box(Vec3{b.sx0, b.sy0, 0}, Vec3{b.sx1, b.sy1, t});
    if (ringDepth > 0) {
        s = cutRing(s, b.sx0, b.sy0, b.sx1, b.sy1, roundedRect(0, 0, inch(3), inch(3), 0), t - inch(ringDepth), t);
    }
    if (step) {
        s = cut(s, box(Vec3{b.sx0 - 1, b.sy0 - 1, t - inch(0.5)}, Vec3{b.sx1 + 1, inch(0.5), t + 1}));
    }
    if (circle) {
        s = cut(s, prism({circleLoop(inch(1.5), inch(1.5), inch(0.75))}, t - inch(0.75), t + 1));
    }
    return s;
}

std::vector<Feature> features(const Library& lib)
{
    std::vector<Feature> out;
    // BP1 fixture plate: 6061-T6, saw-cut 6.25 x 4.25 x 0.75 in, finished 6.00 x 4.00. Each feature on a model of its
    // own (bp1Model says why).
    {
        Feature f;
        f.part = "bp1";
        f.tool = lib.halfInch;
        f.material = &lib.al6061;
        f.machine = &lib.haas;

        auto s = std::make_shared<kernel::Shape>(bp1Model(true, false));
        f.shape = s;
        f.key = "bp1_perimeter";
        f.title = "BP1 perimeter (outside of the plate)";
        f.modelled =
            "the 0.125 in of stock round the 6.00 x 4.00 in plate, 0.65 in deep from the saw-cut top (past the "
            "0.625 in finished thickness; the second setup faces the rest off), square corners";
        f.airLimit = 15;
        f.depthMm = inch(0.65);
        f.faces = {upFace(*s, kBp1.t - inch(0.65))};
        f.stock = {kBp1.stock()};
        f.pocket = {kBp1.stock(), rectLoop(0, 0, inch(6), inch(4))};
        out.push_back(f);

        s = std::make_shared<kernel::Shape>(bp1Model(false, true));
        f.shape = s;
        f.key = "bp1_pockets";
        f.title = "BP1 two closed pockets (one operation)";
        f.modelled = "two 2.00 x 1.50 in pockets with 0.25 in corner radii, 0.375 in deep, centred at x 1.75 and "
                     "4.25 in, y 2.00 in (the plan gives depth and corner radius only)";
        f.airLimit = 10;
        f.depthMm = inch(0.375);
        const double pz = kBp1.t - inch(0.375);
        f.faces = {upFace(*s, pz, 0, 0, inch(3), inch(4)), upFace(*s, pz, inch(3), 0, inch(6), inch(4))};
        f.pocket = {roundedRectLoop(inch(0.75), inch(1.25), inch(2.75), inch(2.75), inch(0.25)),
                    roundedRectLoop(inch(3.25), inch(1.25), inch(5.25), inch(2.75), inch(0.25))};
        f.stock = grownBox(f.pocket, 10);
        out.push_back(f);
    }
    // BP2 manifold block, first setup: 6061-T6, 3.25 x 3.25 x 2.1 in, the outside to 3.00 square 1.25 deep, a step
    // 0.5 deep, a circular pocket 1.500 in across and 0.75 deep.
    {
        Feature f;
        f.part = "bp2";
        f.tool = lib.halfInch;
        f.material = &lib.al6061;
        f.machine = &lib.mazak;

        auto s = std::make_shared<kernel::Shape>(bp2Model(kBp2.t, 1.25, false, false));
        f.shape = s;
        f.key = "bp2_outside1";
        f.title = "BP2 outside, first setup (1.25 in deep)";
        f.modelled = "the 0.125 in of stock round the 3.00 in square, 1.25 in deep, square corners";
        f.airLimit = 15;
        f.depthMm = inch(1.25);
        f.faces = {upFace(*s, kBp2.t - inch(1.25))};
        f.stock = {kBp2.stock()};
        f.pocket = {kBp2.stock(), rectLoop(0, 0, inch(3), inch(3))};
        out.push_back(f);

        s = std::make_shared<kernel::Shape>(bp2Model(kBp2.t, 1.25, true, false));
        f.shape = s;
        f.key = "bp2_step";
        f.title = "BP2 open-sided step (0.5 in deep)";
        f.modelled = "a step 0.50 in wide along the whole front edge, 0.5 in deep, open on three sides (the plan gives "
                     "the depth only); modelled after the outside, but roughed from the saw-cut stock, so it also "
                     "takes the outside's stock in front of it";
        f.airLimit = 15;
        f.depthMm = inch(0.5);
        f.faces = {upFace(*s, kBp2.t - inch(0.5))};
        f.pocket = {rectLoop(kBp2.sx0, kBp2.sy0, kBp2.sx1, inch(0.5))};
        out.push_back(f);

        s = std::make_shared<kernel::Shape>(bp2Model(kBp2.t, 0, false, true));
        f.shape = s;
        f.key = "bp2_circle";
        f.title = "BP2 circular pocket (1.500 in across, 0.75 in deep)";
        f.modelled = "a 1.500 in circle at the block's centre, 0.75 in deep, entered by a helix";
        f.airLimit = 10;
        f.depthMm = inch(0.75);
        f.faces = {upFace(*s, kBp2.t - inch(0.75))};
        f.pocket = {circleLoop2(inch(1.5), inch(1.5), inch(0.75))};
        f.stock = grownBox(f.pocket, 10);
        out.push_back(f);

        // the second setup: flipped, faced to 2.000 in; the remaining 0.75 in of the outside
        s = std::make_shared<kernel::Shape>(bp2Model(inch(2.0), 0.75, false, false));
        f.shape = s;
        f.key = "bp2_outside2";
        f.title = "BP2 outside, second setup (the remaining 0.75 in)";
        f.modelled = "the same ring as the first setup, 0.75 in deep from the faced top; modelled with a floor at that "
                     "depth (on the real part the first setup has already cut below it, which changes nothing in "
                     "what this operation cuts)";
        f.airLimit = 15;
        f.depthMm = inch(0.75);
        f.faces = {upFace(*s, inch(2.0) - inch(0.75))};
        f.stock = {kBp2.stock()};
        f.pocket = {kBp2.stock(), rectLoop(0, 0, inch(3), inch(3))};
        out.push_back(f);
    }
    // BP3 steel bracket: 4140 pre-hard, 4.25 x 2.25 x 1.0 in; a stepped profile 4.00 x 2.00 in narrowing to 1.25 in
    // over its last 1.50 in, and a 0.375 in wide through slot, both 0.90 in deep from the saw-cut top (the second
    // setup faces the back to 0.875 in thickness, which opens them).
    {
        const double t = inch(1.0), sx0 = inch(-0.125), sy0 = inch(-0.125), sx1 = inch(4.125), sy1 = inch(2.125);
        const double z = t - inch(0.9), fr = inch(0.3125);
        auto s = std::make_shared<kernel::Shape>(box(Vec3{sx0, sy0, 0}, Vec3{sx1, sy1, t}));
        *s = cutRing(*s, sx0, sy0, sx1, sy1, roundedRect(0, 0, inch(4), inch(2), 0), z, t);
        *s = cut(*s, prism({roundedRect(inch(2.5), inch(-0.5), inch(4.5), inch(0.375), fr)}, z, t + 1));
        *s = cut(*s, prism({roundedRect(inch(2.5), inch(1.625), inch(4.5), inch(2.5), fr)}, z, t + 1));
        *s = cut(*s, prism({obround(inch(0.5), inch(0.8125), inch(2.0), inch(1.1875))}, z, t + 1));
        Feature f;
        f.part = "bp3";
        f.shape = s;
        f.material = &lib.steel4140ph;
        f.machine = &lib.fanuc;

        f.key = "bp3_profile";
        f.title = "BP3 stepped profile (0.90 in deep)";
        f.modelled = "the stock round a 4.00 x 2.00 in outline whose last 1.50 in narrows to 1.25 in (inside corners "
                     "0.3125 in radius), 0.90 in deep from the saw-cut top (the plan names a stepped profile without "
                     "dimensions)";
        f.airLimit = 15;
        f.depthMm = inch(0.9);
        f.tool = lib.halfInch;
        const kernel::SubId slotFace = upFace(*s, z, inch(0.4), inch(0.7), inch(2.1), inch(1.3));
        for (const kernel::SubId& face : upFaces(*s, z)) {
            if (!(face.index == slotFace.index)) {
                f.faces.push_back(face);
            }
        }
        REQUIRE(f.faces.size() == 1);
        f.stock = {rectLoop(sx0, sy0, sx1, sy1)};
        sw::Loop outline = {{0, 0}, {inch(2.5), 0}};
        arcPts(outline, inch(2.8125), inch(0.0625), fr, sw::kPi, sw::kPi / 2);
        outline.push_back({inch(4), inch(0.375)});
        outline.push_back({inch(4), inch(1.625)});
        arcPts(outline, inch(2.8125), inch(1.9375), fr, -sw::kPi / 2, -sw::kPi);
        outline.push_back({inch(2.5), inch(2)});
        outline.push_back({0, inch(2)});
        f.pocket = {rectLoop(sx0, sy0, sx1, sy1), outline};
        out.push_back(f);

        f.key = "bp3_slot";
        f.title = "BP3 through slot (0.375 in wide, 0.90 in deep)";
        f.modelled = "a 0.375 x 1.50 in slot with round ends, centred at x 1.25, y 1.00 in, 0.90 in deep; roughed "
                     "with a 1/4 in end mill, since the 1/2 in one does not fit";
        f.airLimit = 15; // a narrow slot: T-063 allows it 15 %
        f.tool = lib.quarterInch;
        f.faces = {slotFace};
        f.pocket = {obroundLoop(inch(0.5), inch(0.8125), inch(2.0), inch(1.1875))};
        f.stock = grownBox(f.pocket, 10);
        out.push_back(f);
    }
    return out;
}

// ---------- The two strategies ----------

struct Strategy {
    bool adaptive = true;
    double stepover = 0;   // fraction of the diameter (adaptive: the load)
    double stepdownMm = 0; // the parameter; the operation takes equal steps no deeper
    int levels = 0;
    double stepMm = 0; // the equal step it takes
    std::string presetClass;
    double surfaceSpeed = 0, chipLoad = 0;
    double rpm = 0, feed = 0, rampFeed = 0, plungeFeed = 0, rampAngle = 0;
    double chipThinning = 1, powerKw = 0;
    bool limited = false;
    std::string params;
};

const char* kAdaptiveLoadNote = "10 % of the diameter in aluminium (the operation's default), 8 % in 4140 pre-hard "
                                "(the starter library's largest radial cut for that tool and material)";

Strategy strategyFor(const Feature& f, bool adaptive)
{
    Strategy s;
    s.adaptive = adaptive;
    const lib::Tool& t = f.tool;
    const lib::ToolPreset& rough = presetOf(t, *f.material, "rough");
    const lib::ToolPreset& slot = presetOf(t, *f.material, "slot");
    const lib::ToolPreset& p = adaptive ? rough : slot;
    s.presetClass = p.opClass;
    s.surfaceSpeed = p.surfaceSpeedMMin;
    s.chipLoad = p.chipLoadMm;
    s.rampAngle = rough.rampAngleDeg;
    s.stepover = adaptive ? std::min(0.10, rough.maxRadialFraction) : 0.45;
    s.stepdownMm = std::min(p.maxAxialFraction * t.diameter, t.fluteLength);
    s.levels = static_cast<int>(std::ceil(f.depthMm / s.stepdownMm - 1e-9));
    s.stepMm = f.depthMm / s.levels;
    // The cutting feed: adaptive at its load and step (radial chip thinning raises the feed); plain pocketing at a
    // full-width cut, the engagement it meets in its first ring and in every corner.
    const double ae = adaptive ? s.stepover * t.diameter : t.diameter;
    auto feeds = computeFeeds(
        FeedsInput{t, *f.material, *f.machine, ae, s.stepMm, adaptive ? "adaptive2d" : "pocket2d", p.opClass});
    INFO(f.key << (adaptive ? " adaptive" : " pocket") << ": " << (feeds ? "" : feeds.error().message));
    REQUIRE(feeds);
    s.rpm = feeds->rpm;
    s.feed = feeds->feedMmMin;
    s.chipThinning = feeds->chipThinningFactor;
    s.powerKw = feeds->powerKw;
    s.limited = feeds->limitedByRpm || feeds->limitedByPower || feeds->limitedByFeed || feeds->limitedByTorque;
    // Helices and ramps, the same for both: the full-width (slot) feed at the plain pocket's step, half that to plunge.
    auto slotFeeds =
        computeFeeds(FeedsInput{t, *f.material, *f.machine, t.diameter,
                                std::min(slot.maxAxialFraction * t.diameter, t.fluteLength), "pocket2d", "slot"});
    REQUIRE(slotFeeds);
    s.rampFeed = slotFeeds->feedMmMin;
    s.plungeFeed = std::round(slotFeeds->feedMmMin / 2);
    char b[256];
    std::snprintf(b, sizeof b, R"({"stepover":%g,"stepdown":%.4f,"rampAngle":%g})", s.stepover * 100, s.stepdownMm,
                  s.rampAngle);
    s.params = b;
    return s;
}

Operation operationFor(const Strategy& s)
{
    Operation op = s.adaptive ? adaptiveOp(s.params, s.rpm, s.feed) : pocketOp(s.params, s.rpm, s.feed);
    op.cutting.feedRamp = s.rampFeed;
    op.cutting.feedPlunge = s.plungeFeed;
    op.cutting.coolant = CuttingData::Coolant::Flood;
    return op;
}

// ---------- Moves ----------

bool isFeedXY(const Move& m)
{
    return m.kind == MoveKind::Linear || m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW;
}
bool isArc(const Move& m)
{
    return m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW;
}
struct P3 {
    Nm x = 0, y = 0, z = 0;
};
sw::Pt mm(Nm x, Nm y)
{
    return {static_cast<double>(x) / kNmPerMm, static_cast<double>(y) / kNmPerMm};
}
// The swept angle of an arc move from `a` (radians, positive), a full turn when it ends where it starts.
double arcSweep(const P3& a, const Move& m)
{
    const double ax = static_cast<double>(a.x - m.i), ay = static_cast<double>(a.y - m.j);
    const double bx = static_cast<double>(m.x - m.i), by = static_cast<double>(m.y - m.j);
    if (a.x == m.x && a.y == m.y) {
        return 2 * sw::kPi;
    }
    double s = std::atan2(ax * by - ay * bx, ax * bx + ay * by);
    if (m.kind == MoveKind::ArcCW) {
        s = -s;
    }
    while (s <= 0) {
        s += 2 * sw::kPi;
    }
    return s;
}
// The XY points of a move after its start, arcs within 0.5 um.
void appendXY(std::vector<sw::Pt>& out, const P3& a, const Move& m)
{
    if (!isArc(m)) {
        out.push_back(mm(m.x, m.y));
        return;
    }
    const double r = std::hypot(static_cast<double>(a.x - m.i), static_cast<double>(a.y - m.j)) / kNmPerMm;
    const double sweep = arcSweep(a, m);
    const double a0 = std::atan2(static_cast<double>(a.y - m.j), static_cast<double>(a.x - m.i));
    const double step = r > 0.001 ? 2 * std::acos(std::max(-1.0, 1 - 0.0005 / r)) : sweep;
    const int n = std::max(1, static_cast<int>(std::ceil(sweep / step)));
    const double dir = m.kind == MoveKind::ArcCCW ? 1 : -1;
    const sw::Pt c = mm(m.i, m.j);
    for (int k = 1; k < n; ++k) {
        const double ang = a0 + dir * sweep * k / n;
        out.push_back({c.first + r * std::cos(ang), c.second + r * std::sin(ang)});
    }
    out.push_back(mm(m.x, m.y));
}
double moveLength(const P3& a, const Move& m)
{
    const double dz = static_cast<double>(m.z - a.z) / kNmPerMm;
    if (isArc(m)) {
        const double r = std::hypot(static_cast<double>(a.x - m.i), static_cast<double>(a.y - m.j)) / kNmPerMm;
        return std::hypot(r * arcSweep(a, m), dz);
    }
    const double dx = static_cast<double>(m.x - a.x) / kNmPerMm, dy = static_cast<double>(m.y - a.y) / kNmPerMm;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------- The toolpath, level by level, as the sweep checker's input ----------
//
// A level is a Z at which the toolpath has cutting moves. At each level the checker gets, region by region:
//   an entry: a run of helix, ramp or plunge moves (roles Ramp, Plunge) that stays at or above the level and reaches
//     it; its arcs (a helix) are swept at once and not measured, as the checker sweeps a helix; its straight legs (a
//     ramp's zigzag) are not swept, because the pass at the level that follows runs along the same line and cuts what
//     is left there (a 2D sweep cannot tell the zigzag's depths apart);
//   paths: feed moves at the level, cuts (roles Cut, LeadIn, LeadOut) measured, links (Link, Retract, Approach)
//     swept as tool-down links; a cut at less than the toolpath's cutting feed carries the reduced-feed flag;
//   whatever leaves the level (a rapid, a lifted link) is a retract: not swept, the tool reappears where it comes
//     back down.
// Every level starts from full material, which is right for these straight-walled features.

struct Level {
    Nm z = 0;
    sw::Input in;
    std::string signature;
    int count = 1;      // levels with the same input (the same XY moves)
    int firstSame = -1; // index of the first level with the same signature, or -1
    sw::Report report;
};

std::string signatureOf(const sw::Input& in)
{
    Hasher h;
    auto pt = [&](const sw::Pt& p) {
        h.f64(p.first).f64(p.second);
    };
    for (const sw::Region& r : in.regions) {
        h.str("region");
        pt(r.helixCentre);
        h.f64(r.helixRadius);
        pt(r.start);
        for (const sw::Pt& p : r.ramp) {
            pt(p);
        }
        for (const sw::Path& p : r.paths) {
            h.i64(static_cast<int>(p.kind)).i64(static_cast<std::int64_t>(p.points.size()));
            for (const sw::Pt& q : p.points) {
                pt(q);
            }
            for (unsigned char f : p.flags) {
                h.i64(f);
            }
        }
    }
    return h.finish().hex();
}

std::vector<Nm> cutLevels(const Toolpath& tp)
{
    std::set<Nm> zs;
    P3 p{tp.startX, tp.startY, tp.startZ};
    for (const Move& m : tp.moves) {
        if (isFeedXY(m) && m.role == MoveRole::Cut && m.z == p.z && (m.x != p.x || m.y != p.y)) {
            zs.insert(m.z);
        }
        p = {m.x, m.y, m.z};
    }
    return {zs.rbegin(), zs.rend()}; // top down
}

std::int32_t cuttingFeed(const Toolpath& tp)
{
    std::int32_t f = 0;
    for (const Move& m : tp.moves) {
        if (m.role == MoveRole::Cut && isFeedXY(m)) {
            f = std::max(f, m.feedMicronPerMin);
        }
    }
    return f;
}

sw::Input levelInput(const Toolpath& tp, Nm L, const sw::Input& base)
{
    sw::Input in = base;
    const std::int32_t feedCut = cuttingFeed(tp);
    sw::Region* cur = nullptr;
    bool away = true;
    std::vector<std::size_t> run;            // indices of an entry run
    std::vector<std::pair<P3, P3>> rampLegs; // the straight legs of the last entry (a ramp's zigzag)
    std::vector<P3> from(tp.moves.size());
    // on a ramp leg: within 2 um of one, in XY
    auto onRamp = [&](Nm x, Nm y) {
        for (const auto& [a, b] : rampLegs) {
            const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
            const double px = static_cast<double>(x - a.x), py = static_cast<double>(y - a.y);
            const double t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0, 1.0);
            if (std::hypot(px - t * dx, py - t * dy) <= 2000) {
                return true;
            }
        }
        return false;
    };
    {
        P3 p{tp.startX, tp.startY, tp.startZ};
        for (std::size_t i = 0; i < tp.moves.size(); ++i) {
            from[i] = p;
            p = {tp.moves[i].x, tp.moves[i].y, tp.moves[i].z};
        }
    }
    auto newRegion = [&]() -> sw::Region& {
        in.regions.emplace_back();
        cur = &in.regions.back();
        return *cur;
    };
    auto flushRun = [&] {
        if (run.empty()) {
            return;
        }
        bool atOrAbove = true, reaches = false;
        for (std::size_t i : run) {
            atOrAbove = atOrAbove && std::min(from[i].z, tp.moves[i].z) >= L;
            reaches = reaches || tp.moves[i].z == L;
        }
        if (atOrAbove && reaches) {
            std::vector<sw::Pt> helix;
            rampLegs.clear();
            for (std::size_t i : run) {
                if (!isArc(tp.moves[i]) && (tp.moves[i].x != from[i].x || tp.moves[i].y != from[i].y)) {
                    rampLegs.push_back({from[i], P3{tp.moves[i].x, tp.moves[i].y, tp.moves[i].z}});
                }
                if (isArc(tp.moves[i])) {
                    if (helix.empty()) {
                        helix.push_back(mm(from[i].x, from[i].y));
                    }
                    appendXY(helix, from[i], tp.moves[i]);
                }
            }
            const Move& last = tp.moves[run.back()];
            sw::Region& r = newRegion();
            if (helix.empty()) {
                r.start = mm(last.x, last.y);
            } else {
                r.start = helix.front();
                r.ramp.assign(helix.begin() + 1, helix.end());
                r.ramp.push_back(mm(last.x, last.y));
            }
            away = false;
        } else {
            rampLegs.clear();
            away = true;
        }
        run.clear();
    };
    for (std::size_t i = 0; i < tp.moves.size(); ++i) {
        const Move& m = tp.moves[i];
        const P3& p = from[i];
        if (isFeedXY(m) && (m.role == MoveRole::Ramp || m.role == MoveRole::Plunge)) {
            run.push_back(i);
            continue;
        }
        flushRun();
        if (!(isFeedXY(m) && p.z == L && m.z == L)) {
            away = true;
            continue;
        }
        if (m.x == p.x && m.y == p.y) {
            continue;
        }
        // the cut along a ramp's line at the level, straight after the ramp: part of the entry (swept, not measured),
        // as the engine's acceptance counts it
        if (cur != nullptr && !away && cur->paths.empty() && !rampLegs.empty() && m.kind == MoveKind::Linear &&
            m.role == MoveRole::Cut && onRamp(p.x, p.y) && onRamp(m.x, m.y)) {
            cur->ramp.push_back(mm(m.x, m.y));
            continue;
        }
        rampLegs.clear();
        const bool link = m.role == MoveRole::Link || m.role == MoveRole::Retract || m.role == MoveRole::Approach;
        const sw::Kind kind = link ? sw::Kind::LinkDown : sw::Kind::Cut;
        if (cur == nullptr) {
            newRegion().start = mm(p.x, p.y);
            away = false;
        }
        if (away) {
            sw::Path back;
            back.kind = sw::Kind::Retract;
            back.points = {mm(p.x, p.y)};
            cur->paths.push_back(back);
            away = false;
        }
        if (cur->paths.empty() || cur->paths.back().kind != kind) {
            sw::Path np;
            np.kind = kind;
            np.points = {mm(p.x, p.y)};
            np.flags = {0};
            cur->paths.push_back(np);
        }
        sw::Path& path = cur->paths.back();
        appendXY(path.points, p, m);
        const unsigned char flag = kind == sw::Kind::Cut && m.feedMicronPerMin < feedCut ? 1 : 0;
        path.flags.resize(path.points.size(), flag);
    }
    flushRun();
    return in;
}

// Which level each move belongs to (for the pictures): the level of the next cutting move at a level, or the last.
std::vector<int> moveLevels(const Toolpath& tp, const std::vector<Level>& levels)
{
    std::vector<int> own(tp.moves.size(), -1);
    std::map<Nm, int> index;
    for (std::size_t k = 0; k < levels.size(); ++k) {
        index[levels[k].z] = static_cast<int>(k);
    }
    P3 p{tp.startX, tp.startY, tp.startZ};
    std::vector<int> at(tp.moves.size(), -1);
    for (std::size_t i = 0; i < tp.moves.size(); ++i) {
        const Move& m = tp.moves[i];
        if (isFeedXY(m) && m.role == MoveRole::Cut && m.z == p.z && index.count(m.z) != 0) {
            at[i] = index[m.z];
        }
        p = {m.x, m.y, m.z};
    }
    int next = -1;
    for (std::size_t i = tp.moves.size(); i-- > 0;) {
        if (at[i] >= 0) {
            next = at[i];
        }
        own[i] = next;
    }
    int last = -1;
    for (std::size_t i = 0; i < tp.moves.size(); ++i) {
        if (own[i] < 0) {
            own[i] = last;
        }
        last = own[i];
    }
    return own;
}

// The estimated time of one toolpath without its tool change, s.
double estimatedSec(const Toolpath& tp, const lib::MachineDef& machine)
{
    const CycleTimeOperation op{&tp, 1};
    const CycleTime ct = estimateCycleTime(std::span<const CycleTimeOperation>(&op, 1), machine);
    return ct.totalSec - ct.toolChangeSec;
}

// ---------- Measures of one toolpath ----------

struct Sample {
    double deg = 0, fwd = 0, len = 0;
    bool flagged = false;
};
double percentile(std::vector<Sample> s, double p, bool forward = false)
{
    if (s.empty()) {
        return 0;
    }
    const auto key = [&](const Sample& x) {
        return forward ? x.fwd : x.deg;
    };
    std::sort(s.begin(), s.end(), [&](const Sample& a, const Sample& b) { return key(a) < key(b); });
    double tot = 0;
    for (const Sample& x : s) {
        tot += x.len;
    }
    double acc = 0;
    for (const Sample& x : s) {
        acc += x.len;
        if (acc >= p * tot) {
            return key(x);
        }
    }
    return key(s.back());
}
double pctWhere(const std::vector<Sample>& s, const std::function<bool(const Sample&)>& f)
{
    double tot = 0, a = 0;
    for (const Sample& x : s) {
        tot += x.len;
        if (f(x)) {
            a += x.len;
        }
    }
    return tot > 0 ? 100 * a / tot : 0;
}

struct Measures {
    double targetDeg = 0; // adaptive: the straight-cut engagement of the load; plain: of its stepover
    int levels = 0, distinctLevels = 0;
    double median = 0, p99 = 0, fwdP99 = 0, maxDeg = 0;
    double aboveTarget10 = 0, above1_5x = 0, above90 = 0, air = 0;
    double unflaggedFull = 0, unflaggedFwd = 0, flagged = 0;
    std::array<double, 18> hist{};
    std::array<double, 6> groups{}; // under 2, 2-30, 30-50, 50-90, 90-135, 135-180
    double uncutMm2 = 0, gougeUm = 0, linkRemovedMm2 = 0;
    double cutM = 0, linkM = 0, rapidM = 0; // path length by kind of motion, m
    double movesPerMm = 0;
    double cornersPerM = 0; // corners sharper than 10 degrees per metre of feed path at a level
    double totalSec = 0, cutSec = 0, rapidSec = 0;
    std::vector<std::string> warnings;
};

Measures measure(const Feature& f, const Strategy& s, const Toolpath& tp, std::vector<Level>& levels)
{
    Measures out;
    const double r = f.tool.diameter / 2;
    out.targetDeg = sw::straightEngagementDeg(s.stepover * f.tool.diameter, r);
    sw::Input base;
    base.pocket = f.pocket;
    base.stock = f.stock;
    base.toolDiameter = f.tool.diameter;
    base.targetDeg = out.targetDeg;
    base.recordAboveDeg = -1; // every measured sample, for the statistics over all levels
    std::vector<Sample> samples;
    for (Nm z : cutLevels(tp)) {
        Level lv;
        lv.z = z;
        lv.in = levelInput(tp, z, base);
        lv.signature = signatureOf(lv.in);
        for (std::size_t k = 0; k < levels.size(); ++k) {
            if (levels[k].signature == lv.signature) {
                lv.firstSame = static_cast<int>(k);
                levels[k].count++;
                break;
            }
        }
        if (lv.firstSame < 0) {
            lv.report = sw::check(lv.in);
            lv.in.regions.clear(); // not needed any more
        }
        levels.push_back(std::move(lv));
    }
    out.levels = static_cast<int>(levels.size());
    for (const Level& lv : levels) {
        if (lv.firstSame >= 0) {
            continue;
        }
        out.distinctLevels++;
        for (const sw::Hot& h : lv.report.hot) {
            for (int c = 0; c < lv.count; ++c) {
                samples.push_back({h.deg, h.forwardDeg, h.length, h.flagged});
            }
        }
        out.uncutMm2 = std::max(out.uncutMm2, lv.report.uncutReachable);
        out.gougeUm = std::max(out.gougeUm, 1000 * lv.report.gougeAnalytic);
        out.linkRemovedMm2 += lv.count * lv.report.linkRemovedMm2;
    }
    const double t = out.targetDeg;
    out.median = percentile(samples, 0.5);
    out.p99 = percentile(samples, 0.99);
    out.fwdP99 = percentile(samples, 0.99, true);
    out.maxDeg = percentile(samples, 1.0);
    out.aboveTarget10 = pctWhere(samples, [&](const Sample& x) { return x.deg > t + 10; });
    out.above1_5x = pctWhere(samples, [&](const Sample& x) { return x.deg > 1.5 * t; });
    out.above90 = pctWhere(samples, [&](const Sample& x) { return x.deg > 90; });
    out.air = pctWhere(samples, [&](const Sample& x) { return x.deg <= 2; });
    out.unflaggedFull = pctWhere(samples, [&](const Sample& x) { return !x.flagged && x.deg > 1.25 * t; });
    out.unflaggedFwd = pctWhere(samples, [&](const Sample& x) { return !x.flagged && x.fwd > 1.25 * t; });
    out.flagged = pctWhere(samples, [&](const Sample& x) { return x.flagged; });
    for (int b = 0; b < 18; ++b) {
        out.hist[static_cast<std::size_t>(b)] =
            pctWhere(samples, [&](const Sample& x) { return std::min(17, static_cast<int>(x.deg / 10)) == b; });
    }
    const double edges[] = {-1, 2, 30, 50, 90, 135, 1e9};
    for (std::size_t g = 0; g < 6; ++g) {
        out.groups[g] = pctWhere(samples, [&](const Sample& x) { return x.deg > edges[g] && x.deg <= edges[g + 1]; });
    }
    // path lengths
    P3 p{tp.startX, tp.startY, tp.startZ};
    for (const Move& m : tp.moves) {
        const double len = moveLength(p, m) / 1000;
        if (m.kind == MoveKind::Rapid) {
            out.rapidM += len;
        } else if (isFeedXY(m)) {
            const bool link = m.role == MoveRole::Link || m.role == MoveRole::Retract || m.role == MoveRole::Approach;
            (link ? out.linkM : out.cutM) += len;
        }
        p = {m.x, m.y, m.z};
    }
    // corners: junctions of two feed moves at one level whose directions differ by more than the cycle-time model's
    // 10 degrees (CycleTimeOptions::cornerStopDeg), where the machine slows to a stop
    {
        const double cosLimit = std::cos(CycleTimeOptions{}.cornerStopDeg * sw::kPi / 180);
        auto tangent = [](const P3& at, const Move& mv, bool atEnd) {
            double tx, ty;
            if (!isArc(mv)) {
                tx = static_cast<double>(mv.x - at.x);
                ty = static_cast<double>(mv.y - at.y);
            } else {
                const double px = static_cast<double>((atEnd ? mv.x : at.x) - mv.i);
                const double py = static_cast<double>((atEnd ? mv.y : at.y) - mv.j);
                tx = mv.kind == MoveKind::ArcCCW ? -py : py;
                ty = mv.kind == MoveKind::ArcCCW ? px : -px;
            }
            const double n = std::hypot(tx, ty);
            return std::pair{tx / n, ty / n};
        };
        P3 q{tp.startX, tp.startY, tp.startZ};
        bool havePrev = false;
        std::pair<double, double> prevEnd{0, 0};
        Nm prevZ = 0;
        int corners = 0;
        double lengthMm = 0;
        for (const Move& mv : tp.moves) {
            const bool flat = isFeedXY(mv) && mv.z == q.z && (mv.x != q.x || mv.y != q.y || isArc(mv));
            if (flat) {
                const auto start = tangent(q, mv, false);
                if (havePrev && prevZ == mv.z &&
                    start.first * prevEnd.first + start.second * prevEnd.second < cosLimit) {
                    ++corners;
                }
                prevEnd = tangent(q, mv, true);
                prevZ = mv.z;
                havePrev = true;
                lengthMm += moveLength(q, mv);
            } else if (!(mv.x == q.x && mv.y == q.y && mv.z == q.z)) {
                havePrev = false;
            }
            q = {mv.x, mv.y, mv.z};
        }
        out.cornersPerM = lengthMm > 0 ? corners / (lengthMm / 1000) : 0;
    }
    out.movesPerMm = cutDensity(tp).perMm();
    const CycleTimeOperation op{&tp, 1}; // one operation: its tool change is left out below
    const CycleTime ct = estimateCycleTime(std::span<const CycleTimeOperation>(&op, 1), *f.machine);
    out.cutSec = ct.cutSec;
    out.rapidSec = ct.rapidSec;
    out.totalSec = ct.totalSec - ct.toolChangeSec;
    out.warnings = tp.warnings;
    return out;
}

// ---------- Pictures ----------

// Nanometres to the pictures' unit of 10 um, rounded half away from zero (integers only: the same on every OS).
long long u(Nm v)
{
    return v >= 0 ? (v + 5000) / 10000 : -((-v + 5000) / 10000);
}
long long u(double mmv)
{
    return u(toNm(mmv));
}

const char* kBlue = "#2a78d6";
const char* kOrange = "#eb6834";
const char* kAqua = "#1baf7a";
const char* kViolet = "#6250d6";
const char* kInk = "#0b0b0b";
const char* kInk2 = "#52514e";
const char* kSurface = "#fcfcfb";

enum class Ink { Cut, Slow, Link, Entry, Rapid };

std::string toolpathSvg(const Feature& f, const Strategy& s, const Toolpath& tp, const std::vector<Level>& levels)
{
    // frame: the stock outline grown by the tool diameter and 2 mm
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const sw::Loops* ls : {&f.stock, &f.pocket}) {
        for (const sw::Loop& l : *ls) {
            for (const sw::Pt& q : l) {
                x0 = std::min(x0, q.first);
                y0 = std::min(y0, q.second);
                x1 = std::max(x1, q.first);
                y1 = std::max(y1, q.second);
            }
        }
    }
    // and every XY point of the feed moves (an open area's passes run out past the stock)
    for (const Move& mv : tp.moves) {
        if (isFeedXY(mv)) {
            const sw::Pt q = mm(mv.x, mv.y);
            x0 = std::min(x0, q.first);
            y0 = std::min(y0, q.second);
            x1 = std::max(x1, q.first);
            y1 = std::max(y1, q.second);
        }
    }
    const double m = f.tool.diameter / 2 + 2;
    const long long fx0 = u(x0 - m), fy0 = u(y0 - m), fx1 = u(x1 + m), fy1 = u(y1 + m);
    const double widthPx = 720;
    const double scale = widthPx / static_cast<double>(fx1 - fx0);
    const double heightPx = std::round(static_cast<double>(fy1 - fy0) * scale);
    const int top = 44, legend = 40;
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << static_cast<int>(widthPx) + 20 << "\" height=\""
      << static_cast<int>(heightPx) + top + legend << "\" font-family=\"system-ui, sans-serif\">\n";
    o << "<rect width=\"100%\" height=\"100%\" fill=\"" << kSurface << "\"/>\n";
    int shown = 0;
    for (const Level& lv : levels) {
        shown += lv.firstSame < 0 ? 1 : 0;
    }
    o << "<text x=\"10\" y=\"20\" font-size=\"15\" fill=\"" << kInk << "\">" << f.title << ": "
      << (s.adaptive ? "adaptive roughing" : "plain pocketing") << "</text>\n";
    o << "<text x=\"10\" y=\"37\" font-size=\"12\" fill=\"" << kInk2
      << "\">Top view of the path of the cutter's centre. " << levels.size()
      << (levels.size() == 1 ? " level" : " levels") << " of cut";
    if (shown < static_cast<int>(levels.size())) {
        o << ", repeated levels drawn once (" << shown << " drawn)";
    }
    o << ".</text>\n";
    // y up: the drawing's y is -y
    o << "<g transform=\"translate(10 " << top << ") scale(" << num(scale, 6) << ") translate(" << -fx0 << " " << fy1
      << ")\">\n";
    auto loopPath = [&](const sw::Loops& ls) {
        std::string d;
        for (const sw::Loop& l : ls) {
            for (std::size_t k = 0; k < l.size(); ++k) {
                d += (k == 0 ? "M" : "L") + std::to_string(u(l[k].first)) + " " + std::to_string(-u(l[k].second));
            }
            d += "Z";
        }
        return d;
    };
    o << "<path d=\"" << loopPath(f.stock) << "\" fill=\"#c9c8c2\"/>\n";
    o << "<path d=\"" << loopPath(f.pocket) << "\" fill=\"#f3efe6\" fill-rule=\"evenodd\"/>\n";
    const std::vector<int> own = moveLevels(tp, levels);
    const std::int32_t feedCut = cuttingFeed(tp);
    auto inkOf = [&](const Move& mv) {
        if (mv.kind == MoveKind::Rapid) {
            return Ink::Rapid;
        }
        if (mv.role == MoveRole::Ramp || mv.role == MoveRole::Plunge) {
            return Ink::Entry;
        }
        if (mv.role == MoveRole::Link || mv.role == MoveRole::Retract || mv.role == MoveRole::Approach) {
            return Ink::Link;
        }
        return mv.feedMicronPerMin < feedCut ? Ink::Slow : Ink::Cut;
    };
    auto style = [&](Ink k) {
        switch (k) {
        case Ink::Cut:
            return std::string("stroke=\"") + kBlue + "\" stroke-width=\"1.1\"";
        case Ink::Slow:
            return std::string("stroke=\"") + kOrange + "\" stroke-width=\"1.8\"";
        case Ink::Link:
            return std::string("stroke=\"") + kAqua + "\" stroke-width=\"1.1\"";
        case Ink::Entry:
            return std::string("stroke=\"") + kViolet + "\" stroke-width=\"1.4\"";
        case Ink::Rapid:
            return std::string("stroke=\"") + kInk2 + "\" stroke-width=\"0.8\" stroke-dasharray=\"4 3\"";
        }
        return std::string();
    };
    P3 p{tp.startX, tp.startY, tp.startZ};
    std::string d;
    Ink curInk = Ink::Rapid;
    bool open = false;
    auto flush = [&] {
        if (open && !d.empty()) {
            o << "<path d=\"" << d << "\" fill=\"none\" " << style(curInk)
              << " vector-effect=\"non-scaling-stroke\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n";
        }
        d.clear();
        open = false;
    };
    for (std::size_t i = 0; i < tp.moves.size(); ++i) {
        const Move& mv = tp.moves[i];
        const P3 a = p;
        p = {mv.x, mv.y, mv.z};
        if (mv.kind != MoveKind::Rapid && !isFeedXY(mv)) {
            continue;
        }
        if (mv.x == a.x && mv.y == a.y && !isArc(mv)) {
            continue;
        }
        if (own[i] >= 0 && levels[static_cast<std::size_t>(own[i])].firstSame >= 0) {
            flush();
            continue;
        }
        const Ink k = inkOf(mv);
        if (!open || k != curInk) {
            flush();
            curInk = k;
            open = true;
            d = "M" + std::to_string(u(a.x)) + " " + std::to_string(-u(a.y));
        }
        if (!isArc(mv)) {
            d += "L" + std::to_string(u(mv.x)) + " " + std::to_string(-u(mv.y));
            continue;
        }
        const double rr = std::sqrt(static_cast<double>(a.x - mv.i) * static_cast<double>(a.x - mv.i) +
                                    static_cast<double>(a.y - mv.j) * static_cast<double>(a.y - mv.j));
        const long long rad = u(static_cast<Nm>(std::llround(rr)));
        const int sweepFlag = mv.kind == MoveKind::ArcCCW ? 1 : 0; // y is flipped
        auto arcTo = [&](Nm ex, Nm ey, bool large) {
            d += "A" + std::to_string(rad) + " " + std::to_string(rad) + " 0 " + (large ? "1 " : "0 ") +
                 std::to_string(sweepFlag) + " " + std::to_string(u(ex)) + " " + std::to_string(-u(ey));
        };
        if (mv.x == a.x && mv.y == a.y) { // a full turn: two halves
            arcTo(2 * mv.i - a.x, 2 * mv.j - a.y, false);
            arcTo(mv.x, mv.y, false);
        } else {
            // the radius and the part are far under 3e9 nm, so the products fit an int64
            const std::int64_t cross = (a.x - mv.i) * (mv.y - mv.j) - (a.y - mv.j) * (mv.x - mv.i);
            const bool ccwSmall = cross >= 0;
            arcTo(mv.x, mv.y, mv.kind == MoveKind::ArcCCW ? !ccwSmall : ccwSmall);
        }
    }
    flush();
    o << "</g>\n";
    // legend
    const int ly = static_cast<int>(heightPx) + top + 24;
    struct Item {
        Ink k;
        const char* label;
    };
    const Item items[] = {{Ink::Cut, "cutting"},
                          {Ink::Slow, "cutting at reduced feed"},
                          {Ink::Entry, "helix, ramp or plunge"},
                          {Ink::Link, "link at feed"},
                          {Ink::Rapid, "rapid"}};
    int lx = 10;
    for (const Item& it : items) {
        if (it.k == Ink::Slow && !s.adaptive) {
            continue;
        }
        o << "<line x1=\"" << lx << "\" y1=\"" << ly - 4 << "\" x2=\"" << lx + 22 << "\" y2=\"" << ly - 4 << "\" "
          << style(it.k) << "/>\n";
        o << "<text x=\"" << lx + 28 << "\" y=\"" << ly << "\" font-size=\"12\" fill=\"" << kInk2 << "\">" << it.label
          << "</text>\n";
        lx += 40 + static_cast<int>(std::string(it.label).size()) * 6;
    }
    o << "</svg>\n";
    return o.str();
}

std::string histogramSvg(const Feature& f, const Measures& a, const Measures& p)
{
    const int W = 740, H = 300, left = 52, right = 16, top = 52, bottom = 46;
    const double plotW = W - left - right, plotH = H - top - bottom;
    double ymax = 0;
    for (std::size_t b = 0; b < 18; ++b) {
        ymax = std::max({ymax, a.hist[b], p.hist[b]});
    }
    const double yStep = ymax <= 20 ? 5 : ymax <= 50 ? 10 : 20;
    const int yTicks = std::max(1, static_cast<int>(std::ceil(ymax / yStep)));
    const double yTop = yStep * yTicks;
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H
      << "\" font-family=\"system-ui, sans-serif\">\n";
    o << "<rect width=\"100%\" height=\"100%\" fill=\"" << kSurface << "\"/>\n";
    o << "<text x=\"10\" y=\"20\" font-size=\"15\" fill=\"" << kInk << "\">" << f.title
      << ": engagement, percent of cutting length</text>\n";
    // legend
    o << "<rect x=\"10\" y=\"30\" width=\"12\" height=\"12\" rx=\"2\" fill=\"" << kBlue << "\"/>";
    o << "<text x=\"27\" y=\"40\" font-size=\"12\" fill=\"" << kInk2 << "\">adaptive (target " << num(a.targetDeg, 1)
      << "°)</text>\n";
    o << "<rect x=\"190\" y=\"30\" width=\"12\" height=\"12\" rx=\"2\" fill=\"" << kOrange << "\"/>";
    o << "<text x=\"207\" y=\"40\" font-size=\"12\" fill=\"" << kInk2 << "\">plain pocketing</text>\n";
    // grid and axes
    for (int k = 0; k <= yTicks; ++k) {
        const double v = yStep * k;
        const double y = top + plotH - plotH * v / yTop;
        o << "<line x1=\"" << left << "\" x2=\"" << W - right << "\" y1=\"" << num(y, 1) << "\" y2=\"" << num(y, 1)
          << "\" stroke=\"#e4e3df\" stroke-width=\"1\"/>";
        o << "<text x=\"" << left - 6 << "\" y=\"" << num(y + 4, 1) << "\" font-size=\"11\" fill=\"" << kInk2
          << "\" text-anchor=\"end\">" << num(v, 0) << " %</text>\n";
    }
    const double bw = plotW / 18;
    for (int b = 0; b <= 18; b += 3) {
        const double x = left + bw * b;
        o << "<text x=\"" << num(x, 1) << "\" y=\"" << top + plotH + 16 << "\" font-size=\"11\" fill=\"" << kInk2
          << "\" text-anchor=\"middle\">" << b * 10 << "°</text>\n";
    }
    o << "<text x=\"" << left + plotW / 2 << "\" y=\"" << H - 8 << "\" font-size=\"12\" fill=\"" << kInk2
      << "\" text-anchor=\"middle\">engagement angle (0° no cut, 180° full-width slot)</text>\n";
    // bars: two per 10 degree bin, 2 px apart
    for (std::size_t b = 0; b < 18; ++b) {
        const double x = left + bw * static_cast<double>(b);
        const double w = (bw - 6) / 2;
        for (int k = 0; k < 2; ++k) {
            const double v = k == 0 ? a.hist[b] : p.hist[b];
            if (v <= 0) {
                continue;
            }
            const double h = std::max(1.0, plotH * v / yTop);
            o << "<rect x=\"" << num(x + 2 + k * (w + 2), 1) << "\" y=\"" << num(top + plotH - h, 1) << "\" width=\""
              << num(w, 1) << "\" height=\"" << num(h, 1) << "\" rx=\"2\" fill=\"" << (k == 0 ? kBlue : kOrange)
              << "\"><title>" << b * 10 << "-" << b * 10 + 10 << "°: " << num(v, 1) << " %</title></rect>\n";
        }
    }
    // the adaptive target and its limit
    const double xt = left + plotW * a.targetDeg / 180;
    o << "<line x1=\"" << num(xt, 1) << "\" x2=\"" << num(xt, 1) << "\" y1=\"" << top << "\" y2=\"" << top + plotH
      << "\" stroke=\"" << kInk << "\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>";
    o << "<text x=\"" << num(xt + 4, 1) << "\" y=\"" << top + 10 << "\" font-size=\"11\" fill=\"" << kInk
      << "\">adaptive target</text>\n";
    o << "<line x1=\"" << left << "\" x2=\"" << W - right << "\" y1=\"" << top + plotH << "\" y2=\"" << top + plotH
      << "\" stroke=\"" << kInk2 << "\" stroke-width=\"1\"/>\n";
    o << "</svg>\n";
    return o.str();
}

// ---------- One run, and the whole pack ----------

struct Run {
    Strategy s;
    Toolpath tp;
    std::vector<Level> levels;
    Measures m;
};

Toolpath generate(const Feature& f, const Operation& op)
{
    ResolvedGeometry g = geometryOn(*f.shape, f.faces);
    JobContext ctx;
    const GenerateInput in{op, f.tool, nullptr, *f.machine, g};
    auto type = op.type == "adaptive2d" ? ops::makeAdaptive2dOperationType() : ops::makePocket2dOperationType();
    auto tp = type->generate(in, ctx);
    INFO(f.key << " " << op.type << ": " << (tp ? "" : tp.error().message));
    REQUIRE(tp);
    return *tp;
}

Run runOne(const Feature& f, bool adaptive)
{
    Run r;
    r.s = strategyFor(f, adaptive);
    r.tp = generate(f, operationFor(r.s));
    r.m = measure(f, r.s, r.tp, r.levels);
    return r;
}

struct Limits {
    double medianTol = 2, p99Over = 6, over10Pct = 0.5, over1_5xPct = 0.1, movesPerMm = 1, rapidTimePct = 10;
    double gougeUm = 10, uncutMm2 = 0.005, linkMm2 = 0.005;
};

struct Row {
    const Feature* f;
    Run a, p;
    std::vector<std::string> shortOf; // the adaptive figures that fall short, in words
};

double airLimit(const Feature& f)
{
    return f.airLimit;
}

std::vector<std::string> shortfalls(const Row& row)
{
    const Limits L;
    const Measures& m = row.a.m;
    std::vector<std::string> s;
    const double t = m.targetDeg;
    if (std::fabs(m.median - t) > L.medianTol) {
        s.push_back("median engagement " + deg(m.median) + " against target " + deg(t));
    }
    if (m.p99 > t + L.p99Over) {
        s.push_back("99th-percentile engagement " + deg(m.p99) + " against " + deg(t + L.p99Over));
    }
    if (m.aboveTarget10 > L.over10Pct) {
        s.push_back(pct(m.aboveTarget10, 2) + " of cutting more than 10° over target (limit 0.5 %)");
    }
    if (m.above1_5x > L.over1_5xPct) {
        s.push_back(pct(m.above1_5x, 2) + " of cutting above 1.5 x target (limit 0.1 %)");
    }
    if (m.unflaggedFwd > 0.0) {
        s.push_back(smallPct(m.unflaggedFwd) + " of cutting above 1.25 x target without reduced feed (forward half)");
    }
    if (m.air > airLimit(*row.f)) {
        s.push_back("air cutting " + pct(m.air) + " (limit " + num(airLimit(*row.f), 0) + " %)");
    }
    if (m.movesPerMm > L.movesPerMm) {
        s.push_back(num(m.movesPerMm, 2) + " moves per mm (limit 1)");
    }
    if (100 * m.rapidSec / std::max(1e-9, m.totalSec) > L.rapidTimePct) {
        s.push_back("rapids " + pct(100 * m.rapidSec / m.totalSec) + " of the time (guide 10 %)");
    }
    if (m.totalSec >= row.p.m.totalSec) {
        s.push_back("estimated time " + num(m.totalSec, 1) + " s, not below plain pocketing's " +
                    num(row.p.m.totalSec, 1) + " s");
    }
    if (m.uncutMm2 > L.uncutMm2) {
        s.push_back("reachable area left uncut " + num(m.uncutMm2, 2) + " mm²");
    }
    if (m.gougeUm > L.gougeUm) {
        s.push_back("gouge " + num(m.gougeUm, 1) + " µm (limit 10 µm)");
    }
    if (m.linkRemovedMm2 > L.linkMm2) {
        s.push_back("tool-down links remove " + num(m.linkRemovedMm2, 2) + " mm²");
    }
    return s;
}

std::string mark(bool isShort)
{
    return isShort ? " **short**" : "";
}

std::string seconds(double s)
{
    return num(s, 1) + " s";
}

std::string partName(const std::string& part)
{
    if (part == "bp1") {
        return "BP1 fixture plate (6061-T6, Haas VF-2 class machine)";
    }
    if (part == "bp2") {
        return "BP2 manifold block (6061-T6, Mazak VC-Ez 20 class machine)";
    }
    return "BP3 steel bracket (4140 pre-hard, Fanuc ROBODRILL class machine)";
}

std::string machineNotes(const lib::MachineDef& m)
{
    Toolpath t;
    Move mv{};
    mv.kind = MoveKind::Linear;
    mv.role = MoveRole::Cut;
    mv.feedMicronPerMin = 1'000'000;
    mv.x = 10'000'000;
    t.moves.push_back(mv);
    mv.kind = MoveKind::Rapid;
    mv.feedMicronPerMin = 0;
    mv.x = 0;
    t.moves.push_back(mv);
    const CycleTimeOperation op{&t, 1};
    const CycleTime ct = estimateCycleTime(std::span<const CycleTimeOperation>(&op, 1), m);
    std::string s;
    for (const std::string& n : ct.notes) {
        if (n.find("tool change") != std::string::npos) {
            continue; // the tool change is left out of every figure here
        }
        s += (s.empty() ? "" : " ") + n;
    }
    return s.empty() ? "none" : s;
}

struct Pack {
    std::string report;
    std::map<std::string, std::string> files; // path under docs/reports -> content
    std::vector<std::string> missing;         // placeholders without a value
};

const std::vector<std::string> kRequiredPerFeature = {
    "time_a",       "time_p",     "time_diff",  "p99_a", "p99_limit", "median_a", "air_a",    "air_p",   "rapid_time_a",
    "rapid_time_p", "link_len_a", "moves_mm_a", "img_a", "img_p",     "img_hist", "levels_a", "levels_p"};

Pack buildPack(const std::string& tmpl)
{
    Library lib;
    const std::vector<Feature> feats = features(lib);
    std::vector<Row> rows;
    std::map<std::string, std::string> v;
    Pack pack;
    for (const Feature& f : feats) {
        Row row;
        row.f = &f;
        row.a = runOne(f, true);
        row.p = runOne(f, false);
        row.shortOf = shortfalls(row);
        rows.push_back(std::move(row));
    }
    const Limits L;
    std::string overview, params, sections, features_;
    overview += "| Feature | Estimated time, adaptive | Estimated time, plain | Difference | Engagement p99 "
                "(limit) | Air cutting, adaptive | Rapids, share of time | Moves per mm | Figures short |\n";
    overview += "|-|-|-|-|-|-|-|-|-|\n";
    params += "| Feature | Strategy | Tool | Load or stepover | Stepdown (levels) | Cutting data used | rpm | Feed "
              "mm/min | Helix and ramp feed | Ramp angle |\n";
    params += "|-|-|-|-|-|-|-|-|-|-|\n";
    features_ += "| Feature | Depth | Modelled as |\n|-|-|-|\n";
    std::map<std::string, std::pair<double, double>> partTime;
    std::map<std::string, std::vector<std::string>> partShort;
    std::map<std::string, int> partCount;
    for (const Row& row : rows) {
        const Feature& f = *row.f;
        const Measures& a = row.a.m;
        const Measures& p = row.p.m;
        const std::string k = f.key;
        const std::string imgA = "adaptive/" + k + "_adaptive.svg", imgP = "adaptive/" + k + "_pocket.svg",
                          imgH = "adaptive/" + k + "_engagement.svg";
        pack.files[imgA] = toolpathSvg(f, row.a.s, row.a.tp, row.a.levels);
        pack.files[imgP] = toolpathSvg(f, row.p.s, row.p.tp, row.p.levels);
        pack.files[imgH] = histogramSvg(f, a, p);
        const double diff = 100 * (a.totalSec - p.totalSec) / p.totalSec;
        const double limit = a.targetDeg + L.p99Over;
        const double rapidA = 100 * a.rapidSec / a.totalSec, rapidP = 100 * p.rapidSec / p.totalSec;
        auto put = [&](const std::string& name, const std::string& val) {
            v[k + "." + name] = val;
        };
        put("title", f.title);
        put("time_a", seconds(a.totalSec) + mark(a.totalSec >= p.totalSec));
        put("time_p", seconds(p.totalSec));
        put("time_diff", (diff <= 0 ? num(-diff, 0) + " % less" : num(diff, 0) + " % more"));
        put("p99_a", deg(a.p99) + mark(a.p99 > limit));
        put("p99_fwd_a", deg(a.fwdP99) + mark(a.fwdP99 > limit));
        put("p99_p", deg(p.p99));
        put("p99_limit", deg(limit));
        put("target", deg(a.targetDeg));
        put("median_a", deg(a.median) + mark(std::fabs(a.median - a.targetDeg) > L.medianTol));
        put("median_p", deg(p.median));
        put("max_a", deg(a.maxDeg));
        put("max_p", deg(p.maxDeg));
        put("over10_a", pct(a.aboveTarget10, 2) + mark(a.aboveTarget10 > L.over10Pct));
        put("over15x_a", pct(a.above1_5x, 2) + mark(a.above1_5x > L.over1_5xPct));
        put("over90_a", pct(a.above90, 2));
        put("over90_p", pct(p.above90, 2));
        put("air_a", pct(a.air) + mark(a.air > airLimit(f)));
        put("air_p", pct(p.air));
        put("air_limit", num(airLimit(f), 0) + " %");
        put("unflagged_full_a", smallPct(a.unflaggedFull));
        put("unflagged_fwd_a", smallPct(a.unflaggedFwd) + mark(a.unflaggedFwd > 0));
        put("flagged_a", pct(a.flagged, 2));
        put("moves_mm_a", num(a.movesPerMm, 2) + mark(a.movesPerMm > L.movesPerMm));
        put("moves_mm_p", num(p.movesPerMm, 2));
        put("corners_a", num(a.cornersPerM, 1));
        put("corners_p", num(p.cornersPerM, 1));
        put("rapid_time_a", pct(rapidA) + mark(rapidA > L.rapidTimePct));
        put("rapid_time_p", pct(rapidP));
        put("cut_len_a", num(a.cutM, 2) + " m");
        put("cut_len_p", num(p.cutM, 2) + " m");
        put("link_len_a", num(a.linkM, 2) + " m");
        put("link_len_p", num(p.linkM, 2) + " m");
        put("rapid_len_a", num(a.rapidM, 2) + " m");
        put("rapid_len_p", num(p.rapidM, 2) + " m");
        put("noncut_a", pct(100 * (a.linkM + a.rapidM) / (a.cutM + a.linkM + a.rapidM)));
        put("noncut_p", pct(100 * (p.linkM + p.rapidM) / (p.cutM + p.linkM + p.rapidM)));
        put("levels_a", std::to_string(a.levels));
        put("levels_p", std::to_string(p.levels));
        put("uncut_a", num(a.uncutMm2, 2) + " mm²" + mark(a.uncutMm2 > L.uncutMm2));
        put("uncut_p", num(p.uncutMm2, 2) + " mm²");
        put("gouge_a", num(a.gougeUm, 1) + " µm" + mark(a.gougeUm > L.gougeUm));
        put("gouge_p", num(p.gougeUm, 1) + " µm");
        put("linkcut_a", num(a.linkRemovedMm2, 2) + " mm²" + mark(a.linkRemovedMm2 > L.linkMm2));
        put("linkcut_p", num(p.linkRemovedMm2, 2) + " mm²");
        put("img_a", imgA);
        put("img_p", imgP);
        put("img_hist", imgH);
        std::string groups = "| Engagement band | under 2° (air) | 2-30° | 30-50° | 50-90° | 90-135° | 135-180° |\n"
                             "|-|-|-|-|-|-|-|\n";
        for (int s = 0; s < 2; ++s) {
            const Measures& m = s == 0 ? a : p;
            groups += s == 0 ? "| adaptive |" : "| plain pocketing |";
            for (double g : m.groups) {
                groups += " " + pct(g) + " |";
            }
            groups += "\n";
        }
        put("bands", groups);
        std::string warn;
        for (const Run* r : {&row.a, &row.p}) {
            const std::string who = r == &row.a ? "Adaptive" : "Plain pocketing";
            if (r->m.warnings.empty()) {
                warn += "- " + who + ": no warnings.\n";
            }
            for (const std::string& w : r->m.warnings) {
                warn += "- " + who + ": " + w + "\n";
            }
        }
        put("warnings", warn);
        std::string sh;
        for (const std::string& s : row.shortOf) {
            sh += (sh.empty() ? "" : "; ") + s;
        }
        put("short", sh.empty() ? "none" : sh);
        overview += "| " + f.title + " | " + v[k + ".time_a"] + " | " + v[k + ".time_p"] + " | " + v[k + ".time_diff"] +
                    " | " + v[k + ".p99_a"] + " (" + v[k + ".p99_limit"] + ") | " + v[k + ".air_a"] + " | " +
                    v[k + ".rapid_time_a"] + " | " + v[k + ".moves_mm_a"] + " | " + std::to_string(row.shortOf.size()) +
                    " |\n";
        features_ += "| " + f.title + " | " + num(f.depthMm / kIn, 3) + " in (" + num(f.depthMm, 2) + " mm) | " +
                     f.modelled + " |\n";
        for (const Run* r : {&row.a, &row.p}) {
            const Strategy& s = r->s;
            params += "| " + f.title + " | " + (s.adaptive ? "adaptive" : "plain pocketing") + " | " + f.tool.name +
                      " | " + num(100 * s.stepover, 0) + " % (" + num(s.stepover * f.tool.diameter, 2) + " mm) | " +
                      num(s.stepMm, 2) + " mm (" + std::to_string(s.levels) + ") | " + f.material->name + ", '" +
                      s.presetClass + "': " + num(s.surfaceSpeed, 0) + " m/min, " + num(s.chipLoad, 4) + " mm/tooth" +
                      (s.chipThinning > 1.0001 ? ", chip thinning x" + num(s.chipThinning, 2) : "") +
                      (s.limited ? ", held by a machine limit" : "") + " | " + num(s.rpm, 0) + " | " + num(s.feed, 0) +
                      " | " + num(s.rampFeed, 0) + " | " + num(s.rampAngle, 1) + "° |\n";
        }
        partTime[f.part].first += a.totalSec;
        partTime[f.part].second += p.totalSec;
        partCount[f.part]++;
        for (const std::string& s : row.shortOf) {
            partShort[f.part].push_back(f.title + ": " + s);
        }
    }
    v["overview_table"] = overview;
    v["params_table"] = params;
    v["features_table"] = features_;
    for (const auto& [part, t] : partTime) {
        const double d = 100 * (t.second - t.first) / t.second;
        std::string line = "**" + partName(part) + "**, " + std::to_string(partCount[part]) +
                           (partCount[part] == 1 ? " feature" : " features") + ": adaptive roughing estimated at " +
                           seconds(t.first) + " against " + seconds(t.second) + " for plain pocketing (" +
                           (d >= 0 ? num(d, 0) + " % less" : num(-d, 0) + " % more") + "); ";
        const auto& sh = partShort[part];
        if (sh.empty()) {
            line += "every figure within what good looks like.";
        } else {
            line += std::to_string(sh.size()) + (sh.size() == 1 ? " figure" : " figures") + " short: ";
            for (std::size_t i = 0; i < sh.size(); ++i) {
                line += (i ? "; " : "") + sh[i];
            }
            line += ".";
        }
        v["verdict_" + part] = line;
    }
    // the worst reduced-feed reading over the parts' features (known issue b)
    double unfFull = 0, unfFwd = 0, minAir = 100;
    for (const Row& row : rows) {
        unfFull = std::max(unfFull, row.a.m.unflaggedFull);
        unfFwd = std::max(unfFwd, row.a.m.unflaggedFwd);
        minAir = std::min(minAir, row.p.m.air);
    }
    // the outsides of BP1 and BP2 on the whole part (bp1Model): levels and estimated time
    auto whole = [&](const std::string& name, const Feature& iso, const kernel::Shape& shape, double floorZ) {
        Feature f = iso;
        f.shape = std::make_shared<kernel::Shape>(shape);
        f.faces = {upFace(*f.shape, floorZ)};
        for (bool adaptive : {true, false}) {
            const Toolpath tp = generate(f, operationFor(strategyFor(f, adaptive)));
            const std::string suffix = adaptive ? "_a" : "_p";
            v[name + "_levels" + suffix] = std::to_string(cutLevels(tp).size());
            v[name + "_time" + suffix] = seconds(estimatedSec(tp, *f.machine));
        }
    };
    whole("whole_bp1", feats[0], bp1Model(true, true), kBp1.t - inch(0.65));
    whole("whole_bp2", feats[2], bp2Model(kBp2.t, 1.25, true, true), kBp2.t - inch(1.25));
    v["bp_unflagged_full_max"] = pct(unfFull, 2);
    v["bp_unflagged_fwd_max"] = smallPct(unfFwd);
    v["machine_notes_haas"] = machineNotes(lib.haas);
    v["machine_notes_mazak"] = machineNotes(lib.mazak);
    v["machine_notes_fanuc"] = machineNotes(lib.fanuc);
    const ray::bench::adaptive::S5Reference s5 = ray::bench::adaptive::measureS5Islands();
    v["s5_target"] = deg(s5.targetDeg);
    v["s5_limit"] = deg(s5.targetDeg + L.p99Over);
    v["s5_p99"] = deg(s5.p99Deg);
    v["s5_fwd_p99"] = deg(s5.forwardP99Deg);
    v["s5_median"] = deg(s5.medianDeg);
    v["s5_unflagged_full"] = pct(s5.unflaggedFullPct, 2);
    v["s5_unflagged_fwd"] = smallPct(s5.unflaggedForwardPct);
    v["s5_flagged"] = pct(s5.flaggedPct, 2);
    v["adaptive_load_note"] = kAdaptiveLoadNote;

    // render
    std::string outText;
    std::size_t at = 0;
    while (true) {
        const std::size_t open = tmpl.find("{{", at);
        if (open == std::string::npos) {
            outText += tmpl.substr(at);
            break;
        }
        const std::size_t close = tmpl.find("}}", open);
        REQUIRE(close != std::string::npos);
        outText += tmpl.substr(at, open - at);
        const std::string name = tmpl.substr(open + 2, close - open - 2);
        const auto it = v.find(name);
        if (it == v.end()) {
            pack.missing.push_back(name);
        } else {
            std::string val = it->second;
            if (!val.empty() && val.back() == '\n' && close + 2 < tmpl.size() && tmpl[close + 2] == '\n') {
                val.pop_back(); // a block placeholder on a line of its own
            }
            outText += val;
        }
        at = close + 2;
    }
    pack.report = outText;
    // every required field of every feature is in the template
    for (const Feature& f : feats) {
        for (const std::string& r : kRequiredPerFeature) {
            if (tmpl.find("{{" + f.key + "." + r + "}}") == std::string::npos) {
                pack.missing.push_back("(not in the template) " + f.key + "." + r);
            }
        }
    }
    for (const char* r : {"verdict_bp1", "verdict_bp2", "verdict_bp3", "s5_p99", "s5_fwd_p99", "s5_unflagged_full",
                          "s5_unflagged_fwd", "overview_table", "params_table"}) {
        if (tmpl.find(std::string("{{") + r + "}}") == std::string::npos) {
            pack.missing.push_back(std::string("(not in the template) ") + r);
        }
    }
    return pack;
}

// ---------- Comparing with the committed files ----------

// Equal, except that a number with a decimal point may differ by one unit of its last digit. Empty when equal, else
// where they first differ.
std::string difference(const std::string& want, const std::string& got)
{
    static const std::regex token(R"(-?\d+\.\d+|-?\d+|[^\d-]+|-)");
    auto tokens = [](const std::string& s) {
        std::vector<std::string> t;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), token); it != std::sregex_iterator(); ++it) {
            t.push_back(it->str());
        }
        return t;
    };
    const auto a = tokens(want), b = tokens(got);
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] == b[i]) {
            continue;
        }
        const auto dot = a[i].find('.');
        if (dot != std::string::npos && b[i].find('.') != std::string::npos &&
            a[i].size() - dot == b[i].size() - b[i].find('.')) {
            const double unit = std::pow(10.0, -static_cast<double>(a[i].size() - dot - 1));
            if (std::fabs(std::stod(a[i]) - std::stod(b[i])) <= unit * 1.000001) {
                continue;
            }
        }
        std::string ctx;
        for (std::size_t k = i >= 6 ? i - 6 : 0; k < std::min(b.size(), i + 6); ++k) {
            ctx += b[k];
        }
        return "committed '" + a[i] + "', built '" + b[i] + "' in: ..." + ctx + "...";
    }
    if (a.size() != b.size()) {
        return "the texts differ in length (" + std::to_string(a.size()) + " and " + std::to_string(b.size()) +
               " tokens)";
    }
    return {};
}

const fs::path kSource = RAY_SOURCE_DIR;
const fs::path kTemplate = kSource / "tests/benchmarks/adaptive/report.md.in";
const fs::path kReports = kSource / "docs/reports";

} // namespace

TEST_CASE("The adaptive review pack: docs/reports/adaptive.md and its images are what the generator builds",
          "[adaptive_review]")
{
    const Pack pack = buildPack(readFile(kTemplate));
    for (const std::string& m : pack.missing) {
        FAIL_CHECK("field missing: " << m);
    }
    const std::string committed = readFile(kReports / "adaptive.md");
    const std::string diff = difference(committed, pack.report);
    INFO("docs/reports/adaptive.md is out of date (rebuild it with ray_bench_adaptive_review \"[write]\"): " << diff);
    CHECK(diff.empty());
    for (const auto& [path, text] : pack.files) {
        INFO(path << " is out of date or missing");
        REQUIRE(fs::exists(kReports / path));
        const std::string d = difference(readFile(kReports / path), text);
        INFO(d);
        CHECK(d.empty());
        CHECK(text.size() < 300'000);
    }
}

TEST_CASE("Write the adaptive review pack", "[.][write]")
{
    const Pack pack = buildPack(readFile(kTemplate));
    for (const std::string& m : pack.missing) {
        WARN("field missing: " << m);
    }
    REQUIRE(pack.missing.empty());
    writeFile(kReports / "adaptive.md", pack.report);
    for (const auto& [path, text] : pack.files) {
        writeFile(kReports / path, text);
        WARN(path << ": " << text.size() << " bytes");
    }
}

// The outsides on the whole parts take the levels of the outside on its own (found by T-062, known issue (g), fixed
// since: the depth rule shared by pocket2d and adaptive2d, area_depths.h, once added a level of cut at the floor of
// every pocket inside the part, which the outside's area cannot reach, and ran the same passes again there). BP1:
// one 0.65 in level for adaptive at a 25.4 mm stepdown, three for plain pocketing at 6.35 mm, with the two pockets
// (0.375 in deep) in the plate. BP2: the step (0.5 in down, open to the outside's area) is one more level for adaptive,
// whose two 0.625 in levels pass it; plain pocketing's 0.25 in levels include its depth; the circular pocket (0.75 in,
// closed inside the block) adds none. Run in the quick tier (ray_bench_adaptive_review_depths; regions only, no
// engine).
TEST_CASE("The outside on the whole part takes no level of cut at a pocket floor inside the part", "[depths]")
{
    Library lib;
    const std::vector<Feature> feats = features(lib);
    const Feature& bp1 = feats[0];
    const Feature& bp2 = feats[2];
    REQUIRE(bp1.key == "bp1_perimeter");
    REQUIRE(bp2.key == "bp2_outside1");
    const kernel::Shape wholeBp1 = bp1Model(true, true);
    const kernel::Shape wholeBp2 = bp2Model(kBp2.t, 1.25, true, true);
    struct Want {
        const Feature* iso;
        const kernel::Shape* shape;
        double floorZ;
        int extraAdaptive, extraPocket; // levels over what the stepdown needs
    };
    for (const Want& w :
         {Want{&bp1, &wholeBp1, kBp1.t - inch(0.65), 0, 0}, Want{&bp2, &wholeBp2, kBp2.t - inch(1.25), 1, 0}}) {
        const ResolvedGeometry g = geometryOn(*w.shape, {upFace(*w.shape, w.floorZ)});
        for (bool adaptive : {true, false}) {
            const Strategy s = strategyFor(*w.iso, adaptive);
            const Operation op = operationFor(s);
            const GenerateInput in{op, w.iso->tool, nullptr, *w.iso->machine, g};
            JobContext ctx;
            std::size_t depths = 0;
            if (adaptive) {
                auto plan = ops::planAdaptive2d(in, ctx, ops::Adaptive2dStage::Regions);
                REQUIRE(plan);
                depths = plan->areas.at(0).depths.size();
            } else {
                auto plan = ops::planPocket2d(in, ctx, ops::PlanStage::Regions);
                REQUIRE(plan);
                depths = plan->pockets.at(0).depths.size();
            }
            INFO(w.iso->key << " on the whole part, " << (adaptive ? "adaptive2d" : "pocket2d") << ": " << depths
                            << " depths, the stepdown needs " << s.levels);
            CHECK(depths == static_cast<std::size_t>(s.levels + (adaptive ? w.extraAdaptive : w.extraPocket)));
        }
    }
}

TEST_CASE("Time each generation of the adaptive review pack, alone", "[.][timing]")
{
    using Clock = std::chrono::steady_clock;
    Library lib;
    for (const Feature& f : features(lib)) {
        for (bool adaptive : {true, false}) {
            const Strategy s = strategyFor(f, adaptive);
            const Operation op = operationFor(s);
            double best = 1e300;
            for (int i = 0; i < 3; ++i) {
                const auto t0 = Clock::now();
                (void)generate(f, op);
                best = std::min(best, std::chrono::duration<double>(Clock::now() - t0).count());
            }
            WARN(f.key << (adaptive ? " adaptive" : " pocket2d") << ": best of 3 " << best << " s");
        }
    }
}
