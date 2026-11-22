// Toolpath backplot, polylines and tool glyph (task T-038, PLAN.md 12 T-038 acceptance):
//   - reference images in both themes of a hand-made toolpath over a simple part (every move type, a range-scrubbed
//     case, a tool glyph case), and a check that each move type shows in its own token colour;
//   - thin lines meet 3:1 in light mode, measured on the rendered image against the rendered background;
//   - rapids are dashed in screen space with a phase that does not move when the range changes;
//   - picking a segment returns its moveIndex, also past 24 bits; scrubbing changes no buffer;
//   - 2 M segments: frame time on the software rasteriser against the 500k-triangle baseline scene, and GPU memory
//     per segment ([backplot][cost], its own ctest, run alone).
//
// To regenerate the reference images after an intended change: RAY_UPDATE_RENDER_REFERENCES=1 and run
//   ray_render_tests "[backplot][reference]"
// The software-rasteriser pass writes the PNGs into tests/unit/render/reference/; review them before committing.
#include "viewport_testing.h"

#include <lines.h>
#include <shape_testing.h>

#include <ray/cam.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;
using cam::MoveKind;
using cam::MoveRole;
using Catch::Matchers::WithinAbs;
namespace rd = ray::render::detail;

namespace {

constexpr std::uint8_t kToolpath = std::uint8_t(PickFilter::Toolpath);
constexpr std::uint8_t kEdges = std::uint8_t(PickFilter::Edges);
constexpr std::uint8_t kFaces = std::uint8_t(PickFilter::Faces);
constexpr std::uint8_t kBodies = std::uint8_t(PickFilter::Bodies);
const MeshStyle kModel{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};

std::unique_ptr<IOffscreenViewport> viewportOrSkip(int w, int h, bool software = false)
{
    std::string whyNot;
    auto vp = makeViewport(w, h, software, &whyNot);
    if (!vp)
        SKIP("no RHI backend could be created: " << whyNot);
    return vp;
}

Xform translation(double x, double y, double z)
{
    Xform xf = Xform::identity();
    xf.m[3] = x;
    xf.m[7] = y;
    xf.m[11] = z;
    return xf;
}

// ---------- hand-made toolpaths (written against include/ray/cam.h) ----------
Nm nm(double mm)
{
    return static_cast<Nm>(std::llround(mm * 1e6));
}

cam::Move blank(MoveKind kind, MoveRole role)
{
    cam::Move m{};
    m.kind = kind;
    m.role = role;
    m.plane = cam::ArcPlane::XY;
    return m;
}

cam::Move rapid(double x, double y, double z, MoveRole role)
{
    cam::Move m = blank(MoveKind::Rapid, role);
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    return m;
}

cam::Move feed(double x, double y, double z, MoveRole role, double mmPerMin = 800)
{
    cam::Move m = blank(MoveKind::Linear, role);
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    m.feedMicronPerMin = std::int32_t(std::llround(mmPerMin * 1000));
    return m;
}

// The demo part: a 60 x 40 x 20 mm block.
const Box3 kPart{Vec3{0, 0, 0}, Vec3{60, 40, 20}};
const Box3 kDemoBounds{Vec3{-20, -20, 0}, Vec3{65, 55, 40}};

// A contour around the block and a drilled hole on top, with every move type. Move indices are fixed: the tests
// below name them.
cam::Toolpath demoToolpath()
{
    cam::Toolpath tp{};
    tp.startX = nm(-20), tp.startY = nm(-20), tp.startZ = nm(40);
    std::vector<cam::Move>& m = tp.moves;
    m.push_back(rapid(-20, 20, 40, MoveRole::Link));                // 0
    m.push_back(rapid(-20, 20, 22, MoveRole::Approach));            // 1
    m.push_back(feed(-20, 20, 10, MoveRole::Plunge));               // 2
    m.push_back(feed(-5, 20, 10, MoveRole::LeadIn));                // 3
    m.push_back(feed(-5, 45, 10, MoveRole::Cut));                   // 4
    m.push_back(feed(65, 45, 10, MoveRole::Cut));                   // 5
    m.push_back(feed(65, -5, 10, MoveRole::Cut));                   // 6
    m.push_back(feed(-5, -5, 10, MoveRole::Cut));                   // 7
    m.push_back(feed(-5, 20, 10, MoveRole::Cut));                   // 8
    cam::Move leadOut = blank(MoveKind::ArcCCW, MoveRole::LeadOut); // 9: a quarter circle, centre (-15, 20)
    leadOut.x = nm(-15), leadOut.y = nm(30), leadOut.z = nm(10);
    leadOut.i = nm(-15), leadOut.j = nm(20), leadOut.k = nm(10);
    leadOut.feedMicronPerMin = 800'000;
    m.push_back(leadOut);
    m.push_back(feed(-15, 55, 10, MoveRole::Link));                  // 10: stay-down link at feed
    m.push_back(feed(40, 55, 4, MoveRole::Ramp));                    // 11
    m.push_back(feed(65, 55, 4, MoveRole::Cut));                     // 12
    m.push_back(rapid(65, 55, 40, MoveRole::Retract));               // 13
    cam::Move drill = blank(MoveKind::CycleDrill, MoveRole::Plunge); // 14: G81 at (30, 20), R 23, bottom 12
    drill.x = nm(30), drill.y = nm(20), drill.z = nm(12);
    drill.cycleRetract = nm(23);
    drill.feedMicronPerMin = 200'000;
    m.push_back(drill);
    m.push_back(rapid(30, 20, 40, MoveRole::Retract)); // 15
    return tp;
}

std::shared_ptr<const sim::Backplot> demoBackplot()
{
    static const auto bp =
        std::make_shared<const sim::Backplot>(sim::buildBackplot(demoToolpath(), Xform::identity(), 0.01));
    return bp;
}

// A backplot written directly: `start`, then one vertex per entry, each its own move (moveFirstVertex 1, 2, ...).
// moveIndex is the entry's `move` (not necessarily its position: the picking tests use large values).
struct HandVertex {
    double x, y, z;
    MoveKind kind;
    MoveRole role;
    std::uint32_t move;
};
std::shared_ptr<const sim::Backplot> handBackplot(Vec3 start, const std::vector<HandVertex>& vertices)
{
    auto bp = std::make_shared<sim::Backplot>();
    bp->lineVertices.push_back(
        sim::BackplotVertex{float(start.x), float(start.y), float(start.z), 0, 0, 0, 0.0f, 0xFFFFFFFFu});
    for (const HandVertex& v : vertices) {
        bp->lineVertices.push_back(sim::BackplotVertex{float(v.x), float(v.y), float(v.z), std::uint8_t(v.role),
                                                       std::uint8_t(v.kind), 0,
                                                       v.kind == MoveKind::Rapid ? 0.0f : 500.0f, v.move});
    }
    for (std::size_t i = 1; i <= vertices.size(); ++i)
        bp->moveFirstVertex.push_back(std::uint32_t(i));
    bp->moveFirstVertex.push_back(std::uint32_t(bp->lineVertices.size()));
    bp->bounds = *rd::backplotBounds(sim::Backplot{bp->lineVertices, {}, Box3{Vec3{1, 1, 1}, Vec3{0, 0, 0}}});
    return bp;
}

// The kind and role that give a colour slot.
std::pair<MoveKind, MoveRole> slotMove(rd::PathSlot slot)
{
    switch (slot) {
    case rd::PathSlot::Cut:
        return {MoveKind::Linear, MoveRole::Cut};
    case rd::PathSlot::Rapid:
        return {MoveKind::Rapid, MoveRole::Link};
    case rd::PathSlot::Plunge:
        return {MoveKind::Linear, MoveRole::Plunge};
    case rd::PathSlot::Ramp:
        return {MoveKind::Linear, MoveRole::Ramp};
    case rd::PathSlot::Lead:
        return {MoveKind::Linear, MoveRole::LeadIn};
    default:
        return {MoveKind::Linear, MoveRole::Link};
    }
}

const char* slotName(rd::PathSlot slot)
{
    static const char* names[] = {"cut", "rapid", "plunge", "ramp", "lead", "link"};
    return names[std::size_t(slot)];
}

constexpr int kSlots = int(rd::PathSlot::Count);

// A bull-nose end mill from tool dimensions (lib::Tool: diameter, corner radius, flute length, shank diameter,
// overall length), as (radius, height) from the tip up.
std::vector<Vec2> endMillProfile(double diameter, double cornerRadius, double fluteLength, double shankDiameter,
                                 double overallLength)
{
    std::vector<Vec2> p;
    const double r = diameter / 2;
    p.push_back({0, 0});
    p.push_back({r - cornerRadius, 0});
    for (int i = 1; i <= 8; ++i) {
        const double a = kPi / 2 * i / 8;
        p.push_back({r - cornerRadius + cornerRadius * std::sin(a), cornerRadius - cornerRadius * std::cos(a)});
    }
    p.push_back({r, fluteLength});
    p.push_back({shankDiameter / 2, fluteLength + 1.5});
    p.push_back({shankDiameter / 2, overallLength});
    return p;
}

// ---------- camera and pixels ----------
// Looking straight down at the XY plane, `mmHigh` mm across the viewport's height, the origin in the middle.
Camera topCamera(double mmHigh)
{
    return Camera{Vec3{0, 0, 200}, Vec3{0, 0, 0}, Vec3{0, 1, 0}, false, 30, mmHigh};
}

// Where a world point lands (pixel centres at +0.5), as pick_test.cpp projects.
Vec2 project(const Camera& cam, Vec3 p, int w, int h)
{
    const auto sub = [](Vec3 a, Vec3 b) {
        return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
    };
    const auto dot = [](Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const auto cross = [](Vec3 a, Vec3 b) {
        return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto unit = [&](Vec3 v) {
        const double l = std::sqrt(dot(v, v));
        return Vec3{v.x / l, v.y / l, v.z / l};
    };
    const Vec3 f = unit(sub(cam.target, cam.eye));
    const Vec3 r = unit(cross(f, unit(cam.up)));
    const Vec3 u = cross(r, f);
    const Vec3 d = sub(p, cam.eye);
    const double aspect = double(w) / h;
    double ndcX = 0, ndcY = 0;
    if (cam.perspective) {
        const double t = std::tan(cam.fovDeg * kPi / 360.0);
        ndcX = dot(d, r) / (dot(d, f) * t * aspect);
        ndcY = dot(d, u) / (dot(d, f) * t);
    } else {
        ndcX = dot(d, r) / (cam.orthoHeight * 0.5 * aspect);
        ndcY = dot(d, u) / (cam.orthoHeight * 0.5);
    }
    return {(ndcX + 1) * 0.5 * w, (1 - ndcY) * 0.5 * h};
}

std::array<int, 3> srgbOf(const Color& c)
{
    return {encodeSrgb8(c.r), encodeSrgb8(c.g), encodeSrgb8(c.b)};
}

double decodeSrgb(int v)
{
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

Color linearOf(const std::array<int, 3>& rgb)
{
    return Color{float(decodeSrgb(rgb[0])), float(decodeSrgb(rgb[1])), float(decodeSrgb(rgb[2])), 1.0f};
}

// Pixels within `tolerance` (per channel) of a colour; with `without`, only those that differ from it (pixels the
// item drawn on top of `without` changed, so a shade of a face that happens to match does not count).
int countColour(const Image& img, const std::array<int, 3>& want, int tolerance = 8, const Image* without = nullptr)
{
    int n = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const auto px = rgbAt(img, x, y);
            if (without && maxChannelDiff(px, rgbAt(*without, x, y)) <= tolerance)
                continue;
            n += maxChannelDiff(px, want) <= tolerance ? 1 : 0;
        }
    }
    return n;
}

int countDifferent(const Image& a, const Image& b, int threshold = 0)
{
    int n = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x)
            n += maxChannelDiff(rgbAt(a, x, y), rgbAt(b, x, y)) > threshold ? 1 : 0;
    }
    return n;
}

std::optional<PickHit> pickAt(IViewport& vp, int x, int y, std::uint8_t mask)
{
    std::optional<std::optional<PickHit>> got;
    vp.pick(x, y, mask, [&](std::optional<PickHit> h) { got = h; });
    REQUIRE(processEventsUntil([&] { return got.has_value(); }, 30000));
    return *got;
}

std::vector<PickHit> pickRectAt(IViewport& vp, int x0, int y0, int x1, int y1, std::uint8_t mask)
{
    std::optional<std::vector<PickHit>> got;
    vp.pickRect(x0, y0, x1, y1, mask, [&](std::vector<PickHit> h) { got = std::move(h); });
    REQUIRE(processEventsUntil([&] { return got.has_value(); }, 30000));
    return *got;
}

// Waits for the viewport's levels of detail, then draws a frame with them. (The frame the viewport asks for when they
// are ready may come after the wait returns, and renderNow alone keeps the last picture when nothing else changed.)
void waitForLods(IOffscreenViewport& vp)
{
    REQUIRE(processEventsUntil([&] { return vp.info().pathLodsPending == 0; }, 60000));
    vp.requestFrame();
    REQUIRE(vp.renderNow().has_value());
}

std::shared_ptr<const Theme> withMetric(const std::shared_ptr<const Theme>& base, const char* name, double value)
{
    auto copy = std::make_shared<Theme>(*base);
    copy->metrics[name] = value;
    return copy;
}

// ---------- the reference scenes ----------
struct SceneDef {
    std::string name; // without the theme suffix
    Tier preset;
};
const std::vector<SceneDef>& sceneDefs()
{
    static const std::vector<SceneDef> list = {
        {"backplot", Tier::Low}, {"backplot_range", Tier::Low}, {"tool_glyph", Tier::Medium}};
    return list;
}

// The demo part and toolpath; "backplot_range" shows moves 2 to 6 only, "tool_glyph" moves 0 to 7 with the tool at
// the end of move 7.
struct DemoScene {
    ItemId part = 0, path = 0, glyph = 0;
    Camera camera{};
};
DemoScene buildDemo(const std::string& name, IViewport& vp, double aspect)
{
    DemoScene s;
    s.part = vp.addMesh(meshOf(kernel::makeBox(kPart.min, kPart.max)), Xform::identity(), kModel);
    s.path = vp.addBackplot(demoBackplot());
    REQUIRE(s.path != 0);
    if (name == "backplot_range") {
        vp.setBackplotRange(s.path, 2, 6);
    } else if (name == "tool_glyph") {
        vp.setBackplotRange(s.path, 0, 7);
        const std::vector<Vec2> profile = endMillProfile(10, 1, 22, 10, 45);
        s.glyph = vp.addToolGlyph(profile, Token::FixtureFace);
        REQUIRE(s.glyph != 0);
        vp.setTransform(s.glyph, translation(-5, -5, 10));
    } else {
        REQUIRE(name == "backplot");
    }
    s.camera = standardViewCamera(StandardView::Iso, kDemoBounds, aspect, false);
    vp.setCamera(s.camera);
    return s;
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_RENDER_REFERENCES");
    return v && std::string(v) == "1";
}

std::string referencePath(const std::string& name)
{
    return std::string(RAY_RENDER_REFERENCE_DIR) + "/" + name + ".png";
}

// Tolerances of viewport_reference_test.cpp (same rasterisers, same tile comparison).
constexpr double kMaxTileColour = 40.0;
constexpr double kMaxMeanColour = 3.0;
constexpr double kMaxTileCoverage = 0.30;
constexpr double kCoverageAbsolute = 0.002;
constexpr double kCoverageRelative = 0.08;
constexpr int kRefWidth = 160;
constexpr int kRefHeight = 120;

bool matches(const Comparison& c)
{
    return c.worstBlockColour <= kMaxTileColour && c.meanBlockColour <= kMaxMeanColour &&
           c.worstBlockCoverage <= kMaxTileCoverage &&
           std::abs(c.coverageA - c.coverageB) <= kCoverageAbsolute + kCoverageRelative * c.coverageB;
}

const char* const kThemes[2] = {"graphite-dark", "graphite-light"};
std::string themeSuffix(const char* themeName)
{
    return std::string(themeName) == "graphite-dark" ? "_dark" : "_light";
}

} // namespace

// ---------- CPU rules (lines.h) ----------
TEST_CASE("move types map to their colour tokens", "[render][backplot]")
{
    using rd::PathSlot;
    // Rapid motion is PathRapid whatever the role (a cycle's retract is a rapid); feeds by role.
    for (int role = 0; role < 8; ++role)
        CHECK(rd::pathSlot(std::uint8_t(role), std::uint8_t(MoveKind::Rapid)) == PathSlot::Rapid);
    const auto slotOf = [](MoveRole r, MoveKind k = MoveKind::Linear) {
        return rd::pathSlot(std::uint8_t(r), std::uint8_t(k));
    };
    CHECK(slotOf(MoveRole::Cut) == PathSlot::Cut);
    CHECK(slotOf(MoveRole::Cut, MoveKind::ArcCW) == PathSlot::Cut);
    CHECK(slotOf(MoveRole::Plunge) == PathSlot::Plunge);
    CHECK(slotOf(MoveRole::Ramp, MoveKind::ArcCCW) == PathSlot::Ramp);
    CHECK(slotOf(MoveRole::LeadIn) == PathSlot::Lead);
    CHECK(slotOf(MoveRole::LeadOut) == PathSlot::Lead);
    CHECK(slotOf(MoveRole::Link) == PathSlot::Link);
    CHECK(slotOf(MoveRole::Retract) == PathSlot::Link); // no token of its own
    CHECK(slotOf(MoveRole::Approach) == PathSlot::Link);
    CHECK(rd::pathToken(PathSlot::Cut) == Token::PathCut);
    CHECK(rd::pathToken(PathSlot::Rapid) == Token::PathRapid);
    CHECK(rd::pathToken(PathSlot::Plunge) == Token::PathPlunge);
    CHECK(rd::pathToken(PathSlot::Ramp) == Token::PathRamp);
    CHECK(rd::pathToken(PathSlot::Lead) == Token::PathLead);
    CHECK(rd::pathToken(PathSlot::Link) == Token::PathLink);
    CHECK(rd::pathDashed(std::uint8_t(MoveKind::Rapid)));
    CHECK_FALSE(rd::pathDashed(std::uint8_t(MoveKind::Linear)));
    // In both themes the six colours are far enough apart to be told apart in a picture (and by the tests below).
    for (const char* name : kThemes) {
        const auto t = theme(name);
        for (int a = 0; a < kSlots; ++a) {
            for (int b = a + 1; b < kSlots; ++b) {
                INFO(name << ": " << slotName(PathSlot(a)) << " against " << slotName(PathSlot(b)));
                CHECK(maxChannelDiff(srgbOf(t->colors[std::size_t(rd::pathToken(PathSlot(a)))]),
                                     srgbOf(t->colors[std::size_t(rd::pathToken(PathSlot(b)))])) > 24);
            }
        }
    }
}

TEST_CASE("backplot ranges are found from moveFirstVertex", "[render][backplot]")
{
    const sim::Backplot& bp = *demoBackplot();
    REQUIRE(rd::validateBackplot(bp).has_value());
    const std::uint64_t segments = rd::backplotSegmentCount(bp);
    const std::uint32_t moves = std::uint32_t(bp.moveFirstVertex.size() - 1);
    REQUIRE(moves == demoToolpath().moves.size());
    // All moves: every segment. One move: exactly the segments whose end vertex carries its index.
    const rd::SegmentRange all = rd::backplotMoveSegments(bp, 0, 0xFFFFFFFFu);
    CHECK(all.first == 0);
    CHECK(all.end == segments);
    for (std::uint32_t m = 0; m < moves; ++m) {
        const rd::SegmentRange r = rd::backplotMoveSegments(bp, m, m);
        REQUIRE(!r.empty());
        for (std::uint64_t s = r.first; s < r.end; ++s)
            CHECK(bp.lineVertices[std::size_t(s + 1)].moveIndex == m);
        if (r.first > 0)
            CHECK(bp.lineVertices[std::size_t(r.first)].moveIndex != m);
        if (r.end < segments)
            CHECK(bp.lineVertices[std::size_t(r.end + 1)].moveIndex != m);
    }
    // Inclusive at both ends; clamped; empty when reversed or past the end.
    CHECK(rd::backplotMoveSegments(bp, 2, 6).count() ==
          rd::backplotMoveSegments(bp, 2, 2).count() + rd::backplotMoveSegments(bp, 3, 6).count());
    CHECK(rd::backplotMoveSegments(bp, 3, 1000).end == segments);
    CHECK(rd::backplotMoveSegments(bp, 6, 2).empty());
    CHECK(rd::backplotMoveSegments(bp, moves, moves + 5).empty());

    // Display decimation keeps the strip's ends and maps ranges outward.
    CHECK(rd::backplotStride(2'000'000, 2'000'000) == 1);
    CHECK(rd::backplotStride(2'000'001, 2'000'000) == 2);
    CHECK(rd::backplotStride(32'000'000, 2'000'000) == 16);
    CHECK(rd::backplotStride(10, 0) == 1);
    const auto dec = rd::decimateBackplot(bp, 4);
    CHECK(dec.size() == rd::strideSegmentCount(segments, 4) + 1);
    CHECK(dec.front().moveIndex == bp.lineVertices.front().moveIndex);
    CHECK(dec.back().x == bp.lineVertices.back().x);
    const rd::SegmentRange r = rd::strideRange(rd::SegmentRange{5, 9}, 4, rd::strideSegmentCount(segments, 4));
    CHECK(r.first == 1);
    CHECK(r.end == 3);

    // What the renderer refuses.
    sim::Backplot bad = bp;
    bad.moveFirstVertex.back() += 1;
    CHECK_FALSE(rd::validateBackplot(bad).has_value());
    bad = bp;
    bad.moveFirstVertex[0] = 0;
    CHECK_FALSE(rd::validateBackplot(bad).has_value());
    bad = bp;
    std::swap(bad.moveFirstVertex[3], bad.moveFirstVertex[4]);
    CHECK_FALSE(rd::validateBackplot(bad).has_value());
    bad = bp;
    bad.lineVertices[3].x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(rd::validateBackplot(bad).has_value());
}

TEST_CASE("tool glyph: a revolved profile with crisp corners and closed ends", "[render][backplot][glyph]")
{
    // Flat end mill: tip disc, side, shank. Normals are unit length; the side band is horizontal, the tip faces down.
    const std::vector<Vec2> flat{{0, 0}, {5, 0}, {5, 20}, {4, 21}, {4, 40}};
    const kernel::Mesh mesh = *rd::revolveProfile(flat, 16);
    REQUIRE(mesh.positions.size() == mesh.normals.size());
    REQUIRE(!mesh.indices.empty());
    REQUIRE(mesh.faces.size() == 1);
    CHECK(mesh.faces[0].indexCount == mesh.indices.size());
    double maxR = 0, minZ = 1e9, maxZ = -1e9;
    for (std::size_t v = 0; v < mesh.positions.size() / 3; ++v) {
        const float* p = &mesh.positions[v * 3];
        const float* n = &mesh.normals[v * 3];
        CHECK_THAT(std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]), WithinAbs(1, 1e-5));
        maxR = std::max(maxR, std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1]));
        minZ = std::min(minZ, double(p[2]));
        maxZ = std::max(maxZ, double(p[2]));
    }
    CHECK_THAT(maxR, WithinAbs(5, 1e-5));
    CHECK(minZ == 0);
    CHECK(maxZ == 40);
    for (const std::uint32_t i : mesh.indices)
        CHECK(i < mesh.positions.size() / 3);
    // 4 bands of 16 facets (the tip band loses its triangles on the axis) and a top disc (the bottom starts on the
    // axis, so it needs none).
    CHECK(mesh.indices.size() / 3 == 16 * 1 + 3 * 16 * 2 + 16);

    // A corner radius in 8 steps shades round: the bands share their normals across the arc.
    const std::vector<Vec2> bull = endMillProfile(10, 2, 20, 10, 40);
    const kernel::Mesh round = *rd::revolveProfile(bull, 16);
    CHECK(round.indices.size() > mesh.indices.size());

    // Refused: too few points, a negative radius, a NaN.
    CHECK_FALSE(rd::revolveProfile(std::vector<Vec2>{{1, 0}}).has_value());
    CHECK_FALSE(rd::revolveProfile(std::vector<Vec2>{{1, 0}, {1, 0}}).has_value());
    CHECK_FALSE(rd::revolveProfile(std::vector<Vec2>{{0, 0}, {-1, 5}}).has_value());
    CHECK_FALSE(rd::revolveProfile(std::vector<Vec2>{{0, 0}, {1, std::nan("")}}).has_value());
}

// ---------- reference images ----------
TEST_CASE("backplot reference images in both themes", "[render][backplot][reference]")
{
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            for (const char* themeName : kThemes) {
                const auto t = theme(themeName);
                auto bgView = viewportOrSkip(kRefWidth, kRefHeight, software);
                bgView->setTheme(t);
                const Image background = capture(*bgView, kRefWidth, kRefHeight);
                std::cout << "[backplot] " << bgView->info().backend << " on " << bgView->info().device << "\n";
                std::map<std::string, Image> rendered;
                for (const SceneDef& def : sceneDefs()) {
                    const std::string name = def.name + themeSuffix(themeName);
                    auto vp = viewportOrSkip(kRefWidth, kRefHeight, software);
                    vp->setTheme(t);
                    vp->setPreset(presetFor(def.preset));
                    buildDemo(def.name, *vp, double(kRefWidth) / kRefHeight);
                    const Image actual = capture(*vp, kRefWidth, kRefHeight);
                    rendered[name] = actual;
                    if (updating() && software) {
                        REQUIRE(writePng(referencePath(name), actual).has_value());
                        WARN("wrote reference " << referencePath(name));
                        continue;
                    }
                    auto reference = readPng(referencePath(name));
                    INFO("scene " << name << ", reference " << referencePath(name));
                    REQUIRE(reference.has_value());
                    const Comparison c = compare(actual, *reference, background);
                    std::cout << "[backplot] " << name << ": tile colour worst " << c.worstBlockColour << " mean "
                              << c.meanBlockColour << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                              << c.coverageA << " vs " << c.coverageB << "\n";
                    const std::string actualPath =
                        (std::filesystem::temp_directory_path() / ("ray_render_" + name + "_actual.png")).string();
                    if (!matches(c))
                        static_cast<void>(writePng(actualPath, actual));
                    INFO("tile colour worst " << c.worstBlockColour << " mean " << c.meanBlockColour
                                              << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                                              << c.coverageA << " vs " << c.coverageB
                                              << "; this run's image: " << actualPath);
                    CHECK(c.coverageA > 0.02);
                    CHECK(matches(c));
                }
                if (updating() && software)
                    continue;
                // The comparison tells the three scenes of a theme apart.
                for (const SceneDef& a : sceneDefs()) {
                    for (const SceneDef& b : sceneDefs()) {
                        if (a.name == b.name)
                            continue;
                        auto other = readPng(referencePath(b.name + themeSuffix(themeName)));
                        REQUIRE(other.has_value());
                        INFO(a.name << " against the reference of " << b.name << " (" << themeName << ")");
                        CHECK_FALSE(matches(compare(rendered[a.name + themeSuffix(themeName)], *other, background)));
                    }
                }
                // Every move type shows in its own token colour: the reference scene at its own size, counting the
                // pixels the backplot changed against the part alone. The move types the range leaves out are gone
                // from the range-scrubbed one.
                auto partView = viewportOrSkip(kRefWidth, kRefHeight, software);
                partView->setTheme(t);
                partView->setPreset(presetFor(Tier::Low));
                partView->addMesh(meshOf(kernel::makeBox(kPart.min, kPart.max)), Xform::identity(), kModel);
                partView->setCamera(
                    standardViewCamera(StandardView::Iso, kDemoBounds, double(kRefWidth) / kRefHeight, false));
                const Image partOnly = capture(*partView, kRefWidth, kRefHeight);
                const Image& full = rendered["backplot" + themeSuffix(themeName)];
                const Image& range = rendered["backplot_range" + themeSuffix(themeName)];
                for (int s = 0; s < kSlots; ++s) {
                    const auto colour = srgbOf(t->colors[std::size_t(rd::pathToken(rd::PathSlot(s)))]);
                    const int inFull = countColour(full, colour, 8, &partOnly);
                    const int inRange = countColour(range, colour, 8, &partOnly);
                    INFO(themeName << " " << slotName(rd::PathSlot(s)) << ": " << inFull << " pixels, " << inRange
                                   << " with moves 2 to 6");
                    std::cout << "[backplot] " << themeName << " " << slotName(rd::PathSlot(s)) << ": " << inFull
                              << " pixels in the token colour (" << inRange << " with moves 2 to 6)\n";
                    CHECK(inFull >= 6);
                    // Moves 2 to 6: plunge, lead-in and cuts stay; the rapids, the ramp and the links go.
                    const rd::PathSlot slot = rd::PathSlot(s);
                    if (slot == rd::PathSlot::Rapid || slot == rd::PathSlot::Ramp || slot == rd::PathSlot::Link)
                        CHECK(inRange <= 1);
                    else
                        CHECK(inRange >= 3);
                }
            }
        }
    }
}

// ---------- contrast, dashes ----------
TEST_CASE("thin path lines meet 3:1 in light mode, measured on the image", "[render][backplot][contrast]")
{
    // Every move type as a line 1.5 px wide (the theme's thin line) over the bare viewport, at a shallow angle (so the
    // centre line passes through every sub-pixel offset) and at 30 degrees. Per pixel column along each line the
    // pixel of most contrast against the background image at the same place must reach 3:1 (WCAG 2), for every
    // column: the line reads at 3:1 along its whole length. Rapids: every column inside a dash.
    constexpr int w = 480, h = 360;
    constexpr double mmHigh = 180; // 2 px per mm
    const auto light = theme("graphite-light");
    REQUIRE(light->metrics.at("line.thin.px") == 1.5);
    REQUIRE(light->metrics.count("line.path.px") == 0);
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            auto bgView = viewportOrSkip(w, h, software);
            bgView->setTheme(light);
            bgView->setCamera(topCamera(mmHigh));
            const Image background = capture(*bgView, w, h);
            for (const double angleDeg : {1.7, 30.0}) {
                auto vp = viewportOrSkip(w, h, software);
                vp->setTheme(light);
                vp->setPreset(presetFor(Tier::Low));
                const Camera cam = topCamera(mmHigh);
                vp->setCamera(cam);
                const double t = std::tan(angleDeg * kPi / 180);
                struct Line {
                    Vec3 a, b;
                };
                std::vector<Line> lines;
                for (int s = 0; s < kSlots; ++s) {
                    const double y0 = (angleDeg > 10 ? 30 : 60) - 20.0 * s;
                    const Line l{Vec3{-40, y0, 0}, Vec3{40, y0 + 80 * t, 0}};
                    const auto [kind, role] = slotMove(rd::PathSlot(s));
                    REQUIRE(vp->addBackplot(handBackplot(l.a, {{l.b.x, l.b.y, l.b.z, kind, role, 0}})) != 0);
                    lines.push_back(l);
                }
                const Image img = capture(*vp, w, h);
                for (int s = 0; s < kSlots; ++s) {
                    const Vec2 pa = project(cam, lines[std::size_t(s)].a, w, h);
                    const Vec2 pb = project(cam, lines[std::size_t(s)].b, w, h);
                    double worst = 1e9;
                    int columns = 0, inked = 0;
                    std::vector<int> ink; // per column: 1 when the line shows there
                    std::vector<double> best;
                    for (int x = int(pa.x) + 6; x <= int(pb.x) - 6; ++x) {
                        const double yc = pa.y + (pb.y - pa.y) * ((x + 0.5) - pa.x) / (pb.x - pa.x);
                        double most = 1.0;
                        int diff = 0;
                        for (int y = int(yc) - 4; y <= int(yc) + 4; ++y) {
                            if (y < 0 || y >= h)
                                continue;
                            const auto px = rgbAt(img, x, y);
                            const auto bg = rgbAt(background, x, y);
                            diff = std::max(diff, maxChannelDiff(px, bg));
                            most = std::max(most, contrastRatio(linearOf(px), linearOf(bg)));
                        }
                        ink.push_back(diff > 3 ? 1 : 0);
                        best.push_back(most);
                    }
                    // A rapid's dash ends are cut square across the line; at 30 degrees a column there sees only
                    // part of the line, so columns next to a gap are left out for it.
                    const bool dashed = rd::PathSlot(s) == rd::PathSlot::Rapid;
                    for (std::size_t i = 0; i < best.size(); ++i) {
                        ++columns;
                        if (dashed) {
                            const bool inside = ink[i] && (i == 0 || ink[i - 1]) && (i + 1 == ink.size() || ink[i + 1]);
                            if (!inside)
                                continue;
                        }
                        ++inked;
                        worst = std::min(worst, best[i]);
                    }
                    const Color token = light->colors[std::size_t(rd::pathToken(rd::PathSlot(s)))];
                    const double tokenContrast =
                        std::min(contrastRatio(token, light->colors[std::size_t(Token::ViewportBgTop)]),
                                 contrastRatio(token, light->colors[std::size_t(Token::ViewportBgBottom)]));
                    INFO(slotName(rd::PathSlot(s)) << " at " << angleDeg << " degrees: worst column " << worst
                                                   << ":1 over " << inked << " of " << columns << " columns");
                    std::cout << "[backplot] light, " << (software ? "software" : "adapter") << ", "
                              << slotName(rd::PathSlot(s)) << " at " << angleDeg << " deg: worst column contrast "
                              << worst << ":1 (the token itself " << tokenContrast << ":1)\n";
                    CHECK(worst >= 3.0);
                    CHECK(inked >= (dashed ? columns / 3 : columns));
                }
            }
        }
    }
}

TEST_CASE("rapids are dashed in screen space and keep their phase when the range changes", "[render][backplot]")
{
    constexpr int w = 480, h = 240;
    auto vp = viewportOrSkip(w, h);
    const auto dark = theme("graphite-dark");
    vp->setTheme(dark);
    const Camera cam = topCamera(120); // 2 px per mm
    vp->setCamera(cam);
    // Move 0: a cut along y = 40; move 1: a long horizontal rapid along y = 0; move 2: a cut back up.
    const ItemId id =
        vp->addBackplot(handBackplot(Vec3{-100, 40, 0}, {{-100, 40, 0, MoveKind::Linear, MoveRole::Cut, 0},
                                                         {90, 40, 0, MoveKind::Linear, MoveRole::Cut, 1},
                                                         {90, 0, 0, MoveKind::Linear, MoveRole::Link, 2},
                                                         {-90, 0, 0, MoveKind::Rapid, MoveRole::Link, 3}}));
    REQUIRE(id != 0);
    const Image all = capture(*vp, w, h);
    const int row = int(project(cam, Vec3{0, 0, 0}, w, h).y);
    const auto rapidColour = srgbOf(dark->colors[std::size_t(Token::PathRapid)]);
    // Runs of ink along the rapid's row: about one dash period ("line.dash.px", default 8 px) apart, 60 % ink.
    std::vector<int> starts;
    int inkColumns = 0, columns = 0;
    bool previous = false;
    for (int x = int(project(cam, Vec3{-80, 0, 0}, w, h).x); x < int(project(cam, Vec3{80, 0, 0}, w, h).x); ++x) {
        const bool on = maxChannelDiff(rgbAt(all, x, row), rapidColour) <= 40;
        if (on && !previous)
            starts.push_back(x);
        inkColumns += on ? 1 : 0;
        ++columns;
        previous = on;
    }
    INFO("dashes: " << starts.size() << ", ink " << inkColumns << " of " << columns << " columns");
    REQUIRE(starts.size() >= 20);
    const double period = double(starts.back() - starts.front()) / double(starts.size() - 1);
    CHECK_THAT(period, WithinAbs(8.0, 0.75));
    CHECK_THAT(double(inkColumns) / columns, WithinAbs(0.6, 0.1));

    // Scrubbing: showing only the rapid, or the rapid and the link, leaves every pixel of its rows as it was.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges{{3, 3}, {2, 3}};
    for (const auto& [first, last] : ranges) {
        vp->setBackplotRange(id, first, last);
        const Image some = capture(*vp, w, h);
        int changed = 0;
        for (int y = row - 4; y <= row + 4; ++y) {
            for (int x = int(project(cam, Vec3{-85, 0, 0}, w, h).x); x < int(project(cam, Vec3{85, 0, 0}, w, h).x); ++x)
                changed += rgbAt(some, x, y) != rgbAt(all, x, y) ? 1 : 0;
        }
        INFO("range " << first << " to " << last);
        CHECK(changed == 0);
        // ...while the cut along y = 40 is gone.
        CHECK(countDifferent(some, all) > 100);
    }
}

// ---------- scrubbing, decimation, chunks ----------
TEST_CASE("setBackplotRange changes the draw range only", "[render][backplot][range]")
{
    auto vp = viewportOrSkip(320, 240);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Low));
    const DemoScene s = buildDemo("backplot", *vp, 320.0 / 240.0);
    const sim::Backplot& bp = *demoBackplot();
    REQUIRE(vp->renderNow().has_value());
    const ViewportInfo before = vp->info();
    CHECK(before.pathSegments == rd::backplotSegmentCount(bp));
    CHECK(before.pathGpuBytes == bp.lineVertices.size() * sizeof(sim::BackplotVertex)); // 24 bytes a segment
    const IViewport::FrameStats stats = vp->lastFrameStats();
    Image previous = capture(*vp, 320, 240);
    struct Case {
        std::uint32_t first, last;
    };
    for (const Case c : {Case{0, 0}, Case{2, 6}, Case{3, 1000}, Case{6, 2}, Case{100, 200}, Case{0, 0xFFFFFFFFu},
                         Case{5, 5}, Case{14, 14}}) {
        vp->setBackplotRange(s.path, c.first, c.last);
        REQUIRE(vp->renderNow().has_value());
        const ViewportInfo info = vp->info();
        INFO("range " << c.first << " to " << c.last);
        CHECK(info.pathSegments == rd::backplotMoveSegments(bp, c.first, c.last).count());
        // No upload, no new GPU resource, the same number of draw calls.
        CHECK(info.meshUploads == before.meshUploads);
        CHECK(info.liveRhiResources == before.liveRhiResources);
        CHECK(vp->lastFrameStats().drawCalls <= stats.drawCalls);
    }
    // The picture follows the range.
    vp->setBackplotRange(s.path, 0, 3);
    const Image partial = capture(*vp, 320, 240);
    CHECK(countDifferent(partial, previous, 10) > 50);
    // A backplot that does not hold together is refused; an empty toolpath draws nothing.
    sim::Backplot bad = bp;
    bad.moveFirstVertex.pop_back();
    CHECK(vp->addBackplot(std::make_shared<const sim::Backplot>(bad)) == 0);
    CHECK(vp->addBackplot(nullptr) == 0);
    const ItemId empty = vp->addBackplot(
        std::make_shared<const sim::Backplot>(sim::buildBackplot(cam::Toolpath{}, Xform::identity(), 0.01)));
    CHECK(empty != 0);
    REQUIRE(vp->renderNow().has_value());
}

TEST_CASE("backplots over the display limit are decimated, large ones split into chunks", "[render][backplot][range]")
{
    // A fine zigzag of 2^21 + 1000 segments (two GPU chunks), each its own move.
    constexpr std::uint32_t segments = (1u << 21) + 1000;
    auto bp = std::make_shared<sim::Backplot>();
    bp->lineVertices.reserve(segments + 1);
    bp->lineVertices.push_back(sim::BackplotVertex{-50, -50, 0, 0, 0, 0, 0, 0xFFFFFFFFu});
    for (std::uint32_t i = 0; i < segments; ++i) {
        const float x = -50.0f + 100.0f * float(i) / float(segments);
        const float y = -50.0f + 0.05f * float(i % 2);
        bp->lineVertices.push_back(
            sim::BackplotVertex{x, y, 0, std::uint8_t(MoveRole::Cut), std::uint8_t(MoveKind::Linear), 0, 500.0f, i});
    }
    bp->moveFirstVertex.resize(segments + 1);
    for (std::uint32_t m = 0; m <= segments; ++m)
        bp->moveFirstVertex[m] = m + 1;
    bp->bounds = Box3{Vec3{-50, -50, 0}, Vec3{50, 50, 0}};

    auto vp = viewportOrSkip(128, 96);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium)); // 8 M segments: no decimation
    vp->setCamera(topCamera(0.5));          // close enough for full detail (no level of detail fits)
    const ItemId id = vp->addBackplot(bp);
    REQUIRE(id != 0);
    waitForLods(*vp);
    CHECK(vp->info().pathSegments == segments);
    // Two chunks share one vertex; the levels of detail (one chunk each here) come on top.
    std::uint64_t lodBytes = 0;
    for (const rd::PathLod& lod : rd::buildPathLods(*bp))
        lodBytes += lod.vertices.size() * 24;
    CHECK(vp->info().pathGpuBytes == (std::uint64_t(segments) + 2) * 24 + lodBytes);
    // A range across the chunk boundary.
    vp->setBackplotRange(id, (1u << 21) - 10, (1u << 21) + 9);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().pathSegments == 20);
    vp->setBackplotRange(id, 0, 0xFFFFFFFFu);

    // A lower display limit decimates (an upload of the path, and of its levels of detail again), the range still
    // maps.
    TierPreset limited = presetFor(Tier::Medium);
    limited.maxBackplotSegments = 100'000;
    const std::uint64_t uploads = vp->info().meshUploads;
    vp->setPreset(limited);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().meshUploads == uploads + 2);
    const std::uint32_t stride = rd::backplotStride(segments, 100'000);
    CHECK(vp->info().pathSegments == rd::strideSegmentCount(segments, stride));
    CHECK(vp->info().pathSegments <= 100'000);
    vp->setPreset(presetFor(Tier::Medium));
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().pathSegments == segments);
}

// ---------- picking and highlights ----------
TEST_CASE("picking a segment returns its moveIndex", "[render][backplot][pick]")
{
    constexpr int w = 400, h = 200;
    const Camera cam = topCamera(100); // 2 px per mm
    const auto px = [&](double x, double y) {
        const Vec2 p = project(cam, Vec3{x, y, 0}, w, h);
        return std::pair<int, int>{int(std::floor(p.x)), int(std::floor(p.y))};
    };
    // Five moves in one strip; their move indices are not their positions and run past 24 bits.
    const std::uint32_t A = 7, B = 0x00FFFFFFu, C = 0x01234567u, D = 0x7FFFFFFFu, E = 0xFEDCBA98u;
    const auto bp = handBackplot(Vec3{-80, 30, 0}, {{80, 30, 0, MoveKind::Linear, MoveRole::Cut, A},
                                                    {80, 0, 0, MoveKind::Linear, MoveRole::Link, B},
                                                    {-80, 0, 0, MoveKind::Linear, MoveRole::Cut, C},
                                                    {-80, -30, 0, MoveKind::Rapid, MoveRole::Link, D},
                                                    {80, -30, 0, MoveKind::Linear, MoveRole::Ramp, E}});
    for (const double width : {1.5, 1.0}) {
        DYNAMIC_SECTION("line width " << width << " px")
        {
            auto vp = viewportOrSkip(w, h);
            vp->setTheme(withMetric(theme("graphite-dark"), "line.path.px", width));
            vp->setCamera(cam);
            const ItemId id = vp->addBackplot(bp);
            REQUIRE(id != 0);
            struct Probe {
                double x, y;
                int dy; // pixels off the line
                std::uint32_t move;
            };
            for (const Probe p :
                 {Probe{0, 30, 0, A}, Probe{10, 30, 4, A}, Probe{80, 15, 0, B}, Probe{-20, 0, 0, C},
                  Probe{30, 0, -5, C}, Probe{-80, -15, 0, D}, Probe{40, -30, 0, E}, Probe{-60, -30, 3, E}}) {
                const auto [x, y] = px(p.x, p.y);
                const std::optional<PickHit> hit = pickAt(*vp, x, y + p.dy, kToolpath);
                INFO("probe at (" << p.x << ", " << p.y << ") + " << p.dy << " px");
                REQUIRE(hit.has_value());
                CHECK(hit->item == id);
                CHECK(hit->kind == kernel::SubKind::Edge);
                CHECK(hit->moveIndex == p.move);
                CHECK(hit->subIndex == p.move);
                CHECK_THAT(hit->point.z, WithinAbs(0, 0.5));
            }
            // Only the Toolpath filter finds a backplot; nothing is found beyond the tolerance.
            const auto [x, y] = px(0, 30);
            CHECK_FALSE(pickAt(*vp, x, y, kEdges | kFaces | kBodies).has_value());
            CHECK_FALSE(pickAt(*vp, x, y + 12, kToolpath).has_value());
            // A rectangle over the middle finds A, C, E (and not B, D at the ends).
            const auto [x0, y0] = px(-20, 40);
            const auto [x1, y1] = px(20, -40);
            const std::vector<PickHit> hits = pickRectAt(*vp, x0, y0, x1, y1, kToolpath);
            std::vector<std::uint32_t> moves;
            for (const PickHit& hh : hits)
                moves.push_back(hh.moveIndex);
            std::sort(moves.begin(), moves.end());
            CHECK(moves == std::vector<std::uint32_t>{A, C, E});
            // Hidden moves are not pickable.
            vp->setBackplotRange(id, 0, 1); // A and B
            const auto [cx, cy] = px(-20, 0);
            CHECK_FALSE(pickAt(*vp, cx, cy, kToolpath).has_value());
            CHECK(pickAt(*vp, x, y, kToolpath).has_value());
        }
    }
}

TEST_CASE("picks on the demo toolpath name the move under the cursor", "[render][backplot][pick]")
{
    constexpr int w = 480, h = 400;
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-light"));
    vp->addMesh(meshOf(kernel::makeBox(kPart.min, kPart.max)), Xform::identity(), kModel);
    const ItemId path = vp->addBackplot(demoBackplot());
    const Camera cam = standardViewCamera(StandardView::Top, kDemoBounds, double(w) / h, false);
    vp->setCamera(cam);
    // Mid-points of moves seen from above, each away from the others.
    struct Probe {
        Vec3 at;
        std::uint32_t move;
    };
    for (const Probe p :
         {Probe{{-12.5, 20, 10}, 3}, Probe{{-5, 32, 10}, 4}, Probe{{30, 45, 10}, 5}, Probe{{65, 20, 10}, 6},
          Probe{{30, -5, 10}, 7}, Probe{{-15, 45, 10}, 10}, Probe{{12.5, 55, 7}, 11}, Probe{{55, 55, 4}, 12}}) {
        const Vec2 s = project(cam, p.at, w, h);
        const std::optional<PickHit> hit = pickAt(*vp, int(s.x), int(s.y), kToolpath | kFaces);
        INFO("move " << p.move);
        REQUIRE(hit.has_value());
        CHECK(hit->item == path);
        CHECK(hit->moveIndex == p.move);
        CHECK_THAT(hit->point.x, WithinAbs(p.at.x, 1.0));
        CHECK_THAT(hit->point.y, WithinAbs(p.at.y, 1.0));
    }
    // A face of the part under no path: the face, not the toolpath.
    const Vec2 top = project(cam, Vec3{30, 30, 20}, w, h);
    const std::optional<PickHit> face = pickAt(*vp, int(top.x), int(top.y), kToolpath | kFaces);
    REQUIRE(face.has_value());
    CHECK(face->kind == kernel::SubKind::Face);
    CHECK(face->moveIndex == 0);
}

TEST_CASE("a highlighted move is drawn in PathSelected", "[render][backplot][pick]")
{
    constexpr int w = 480, h = 400;
    auto vp = viewportOrSkip(w, h);
    const auto light = theme("graphite-light");
    vp->setTheme(light);
    const ItemId path = vp->addBackplot(demoBackplot());
    const Camera cam = standardViewCamera(StandardView::Top, kDemoBounds, double(w) / h, false);
    vp->setCamera(cam);
    const auto selected = srgbOf(light->colors[std::size_t(Token::PathSelected)]);
    const auto hover = srgbOf(light->colors[std::size_t(Token::Hover)]);
    const auto cut = srgbOf(light->colors[std::size_t(Token::PathCut)]);
    const Image plain = capture(*vp, w, h);
    CHECK(countColour(plain, selected, 4) == 0);
    // As T-047 would: a G-code line's move becomes a hit on the backplot, outside the range shown as well.
    vp->setBackplotRange(path, 0, 4);
    const PickHit five{path, kernel::SubKind::Edge, 5, Vec3{}, 5};
    const PickHit seven{path, kernel::SubKind::Edge, 7, Vec3{}, 7};
    vp->setHighlight(std::span<const PickHit>(&five, 1), seven);
    const Image lit = capture(*vp, w, h);
    const Vec2 a = project(cam, Vec3{30, 45, 10}, w, h); // move 5
    const Vec2 b = project(cam, Vec3{30, -5, 10}, w, h); // move 7 (hover)
    const Vec2 c = project(cam, Vec3{-5, 32, 10}, w, h); // move 4, drawn as a cut
    CHECK(maxChannelDiff(rgbAt(lit, int(a.x), int(a.y)), selected) <= 8);
    CHECK(maxChannelDiff(rgbAt(lit, int(b.x), int(b.y)), hover) <= 8);
    CHECK(maxChannelDiff(rgbAt(lit, int(c.x), int(c.y)), cut) <= 8);
    CHECK(countColour(lit, selected, 4) > 60);
}

// ---------- tool glyph in the viewport ----------
TEST_CASE("the tool glyph is a shaded mesh that moves without an upload", "[render][backplot][glyph]")
{
    constexpr int w = 320, h = 240;
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    const ItemId path = vp->addBackplot(demoBackplot());
    const std::vector<Vec2> profile = endMillProfile(10, 1, 22, 10, 45);
    const ItemId glyph = vp->addToolGlyph(profile, Token::FixtureFace);
    REQUIRE(glyph != 0);
    CHECK(vp->pickMode(glyph) == PickMode::Never);
    CHECK(vp->addToolGlyph(std::vector<Vec2>{{1, 0}}, Token::FixtureFace) == 0);
    CHECK(vp->addToolGlyph(profile, Token::Count) == 0);
    const Camera cam = standardViewCamera(StandardView::Iso, kDemoBounds, double(w) / h, false);
    vp->setCamera(cam);
    vp->setTransform(glyph, translation(-5, -5, 10));
    REQUIRE(vp->renderNow().has_value());
    const std::uint64_t triangles = vp->lastFrameStats().triangles;
    CHECK(triangles == rd::revolveProfile(profile)->indices.size() / 3);
    const ViewportInfo before = vp->info();
    const Image here = capture(*vp, w, h);
    // Moving it every frame along move 7: no upload, no new resource, a different picture.
    for (int i = 0; i <= 10; ++i) {
        vp->setTransform(glyph, translation(65 - 3.5 * i, -5, 10)); // ends at x = 30
        REQUIRE(vp->renderNow().has_value());
    }
    CHECK(vp->info().meshUploads == before.meshUploads);
    CHECK(vp->info().liveRhiResources == before.liveRhiResources);
    const Image there = capture(*vp, w, h);
    CHECK(countDifferent(here, there, 10) > 100);
    // It never hides the toolpath from a pick: seen from above, the tool stands on move 7.
    const Camera top = standardViewCamera(StandardView::Top, kDemoBounds, double(w) / h, false);
    vp->setCamera(top);
    vp->setTransform(glyph, translation(30, -5, 10));
    const Vec2 s = project(top, Vec3{30, -5, 10}, w, h);
    const std::optional<PickHit> hit = pickAt(*vp, int(s.x), int(s.y), kToolpath | kFaces | kBodies);
    REQUIRE(hit.has_value());
    CHECK(hit->item == path);
    CHECK(hit->moveIndex == 7);
}

// ---------- 2 M segments ----------
namespace {

// `segments` in the shape of an adaptive clearing: `levels` Z levels of a spiral from r = 48 mm in to r = 2 mm with
// 1 mm between turns (2 M segments in 20 levels, or 200k in 2: chords 0.07 mm long); a plunge and a lead per level
// and rapids between them. About 8 segments per move (flattened arcs).
std::shared_ptr<const sim::Backplot> spiralBackplot(std::uint32_t segments, int levels = 20)
{
    auto bp = std::make_shared<sim::Backplot>();
    bp->lineVertices.reserve(segments + 1);
    bp->moveFirstVertex.reserve(segments / 8 + 2);
    bp->lineVertices.push_back(sim::BackplotVertex{48, 0, 30, 0, 0, 0, 0, 0xFFFFFFFFu});
    const std::uint32_t perLevel = segments / levels;
    std::uint32_t move = 0;
    const auto push = [&](float x, float y, float z, MoveRole role, MoveKind kind, bool newMove) {
        if (newMove || bp->moveFirstVertex.empty()) {
            bp->moveFirstVertex.push_back(std::uint32_t(bp->lineVertices.size()));
            move = std::uint32_t(bp->moveFirstVertex.size() - 1);
        }
        bp->lineVertices.push_back(sim::BackplotVertex{x, y, z, std::uint8_t(role), std::uint8_t(kind), 0,
                                                       kind == MoveKind::Rapid ? 0.0f : 900.0f, move});
    };
    for (int level = 0; level < levels && bp->lineVertices.size() <= segments; ++level) {
        const float z = 20.0f - float(level);
        push(48, 0, 22, MoveRole::Approach, MoveKind::Rapid, true);
        push(48, 0, z, MoveRole::Plunge, MoveKind::Linear, true);
        const std::uint32_t chords = perLevel - 4;
        for (std::uint32_t i = 0; i < chords; ++i) {
            const double t = double(i) / chords;
            const double turns = 46.0; // 1 mm apart from r 48 to r 2
            const double a = 2 * kPi * turns * t;
            const double r = 48 - 46 * t;
            push(float(r * std::cos(a)), float(r * std::sin(a)), z, i < 200 ? MoveRole::LeadIn : MoveRole::Cut,
                 MoveKind::ArcCCW, i % 8 == 0);
        }
        push(2, 0, 25, MoveRole::Retract, MoveKind::Rapid, true);
        push(48, 0, 25, MoveRole::Link, MoveKind::Rapid, true);
    }
    while (bp->lineVertices.size() < std::size_t(segments) + 1)
        push(48, 0, 25, MoveRole::Link, MoveKind::Rapid, true);
    bp->moveFirstVertex.push_back(std::uint32_t(bp->lineVertices.size()));
    bp->bounds = Box3{Vec3{-48, -48, 0}, Vec3{48, 48, 25}};
    return bp;
}

double percentile(std::vector<double> v, double p)
{
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, std::size_t(double(v.size()) * p))];
}

} // namespace

TEST_CASE("levels of detail stay within their tolerance and keep colours apart", "[render][backplot][lod]")
{
    const auto bp = spiralBackplot(200'000, 2);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<rd::PathLod> lods = rd::buildPathLods(*bp);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(lods.size() >= 3);
    std::cout << "[backplot] levels of detail of 200k segments in " << ms << " ms:";
    for (const rd::PathLod& lod : lods)
        std::cout << " " << lod.vertices.size() - 1 << " (" << lod.toleranceMm << " mm)";
    std::cout << "\n";
    const auto dist = [](const sim::BackplotVertex& p, const sim::BackplotVertex& a, const sim::BackplotVertex& b) {
        const double abx = double(b.x) - a.x, aby = double(b.y) - a.y, abz = double(b.z) - a.z;
        const double apx = double(p.x) - a.x, apy = double(p.y) - a.y, apz = double(p.z) - a.z;
        const double l2 = abx * abx + aby * aby + abz * abz;
        const double t = l2 > 0 ? std::clamp((apx * abx + apy * aby + apz * abz) / l2, 0.0, 1.0) : 0.0;
        const double dx = apx - t * abx, dy = apy - t * aby, dz = apz - t * abz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    std::size_t previous = bp->lineVertices.size();
    for (const rd::PathLod& lod : lods) {
        REQUIRE(lod.vertices.size() == lod.source.size());
        CHECK(lod.vertices.size() * 5 <= previous * 4); // each level a real saving
        previous = lod.vertices.size();
        CHECK(lod.source.front() == 0);
        CHECK(lod.source.back() == bp->lineVertices.size() - 1);
        double worst = 0;
        bool sameSlot = true, sameVertex = true;
        for (std::size_t q = 0; q + 1 < lod.source.size(); ++q) {
            const std::uint32_t a = lod.source[q], b = lod.source[q + 1];
            REQUIRE(a < b);
            sameVertex = sameVertex && std::memcmp(&lod.vertices[q + 1], &bp->lineVertices[b], 24) == 0;
            const rd::PathSlot slot = rd::pathSlot(lod.vertices[q + 1].role, lod.vertices[q + 1].kind);
            for (std::uint32_t i = a + 1; i <= b; ++i) {
                worst = std::max(worst, dist(bp->lineVertices[i], bp->lineVertices[a], bp->lineVertices[b]));
                sameSlot = sameSlot && rd::pathSlot(bp->lineVertices[i].role, bp->lineVertices[i].kind) == slot;
            }
        }
        INFO("level of " << lod.vertices.size() << " vertices, tolerance " << lod.toleranceMm << " mm");
        CHECK(worst <= lod.toleranceMm + 1e-6);
        CHECK(sameSlot);   // a chord never merges two colours (nor a rapid with a feed)
        CHECK(sameVertex); // kept vertices are the originals, attributes and all
    }
    // Ranges split into full-path ends and level segments that cover them exactly.
    const sim::Backplot& full = *bp;
    const std::uint64_t moves = full.moveFirstVertex.size() - 1;
    for (std::uint64_t k = 0; k < 200; ++k) {
        const std::uint32_t a = std::uint32_t((k * 7919) % moves), b = std::uint32_t(std::min(moves - 1, a + k * 97));
        const rd::SegmentRange r = rd::backplotMoveSegments(full, a, b);
        for (const rd::PathLod& lod : lods) {
            const rd::LodPieces p = rd::lodPieces(lod, r);
            if (p.level.empty()) {
                CHECK(p.head.first == r.first);
                CHECK(p.head.end == r.end);
                continue;
            }
            CHECK(p.head.first == r.first);
            CHECK(p.head.end == lod.source[std::size_t(p.level.first)]);
            CHECK(p.tail.first == lod.source[std::size_t(p.level.end)]);
            CHECK(p.tail.end == r.end);
        }
    }
    // The level for a scale: the coarsest whose tolerance is within half a pixel.
    CHECK(rd::pathLodLevel(lods, 1e6) == 0);
    CHECK(rd::pathLodLevel(lods, 0.5 / lods.back().toleranceMm) == int(lods.size()));
    CHECK(rd::pathLodLevel(lods, 0.5 / lods.front().toleranceMm) >= 1);
    CHECK(rd::buildPathLods(*demoBackplot()).empty()); // too short to need any
}

TEST_CASE("the viewport draws a level of detail when zoomed out and every segment when close",
          "[render][backplot][lod]")
{
    const auto bp = spiralBackplot(200'000, 2);
    auto vp = viewportOrSkip(320, 240);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Low));
    const ItemId id = vp->addBackplot(bp);
    vp->setCamera(standardViewCamera(StandardView::Iso, bp->bounds, 320.0 / 240.0, true));
    waitForLods(*vp);
    const ViewportInfo far = vp->info();
    INFO("segments drawn zoomed out: " << far.pathSegments);
    CHECK(far.pathSegments * 4 < 200'000);
    // Picks still name a move near the cursor: the outermost turn of the top level, seen from above.
    const Camera top = standardViewCamera(StandardView::Top, bp->bounds, 320.0 / 240.0, false);
    vp->setCamera(top);
    const Vec2 s = project(top, Vec3{0, -47.9, 20}, 320, 240);
    const std::optional<PickHit> hit = pickAt(*vp, int(s.x), int(s.y), kToolpath);
    REQUIRE(hit.has_value());
    CHECK(hit->item == id);
    CHECK(hit->moveIndex < 12'600); // a move of the top level, nearest the eye (about 12.5k moves a level)
    // Scrubbing stays exact: a range of a few moves is drawn from the full path at its ends.
    vp->setBackplotRange(id, 100, 103);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().pathSegments <= rd::backplotMoveSegments(*bp, 100, 103).count());
    vp->setBackplotRange(id, 0, 0xFFFFFFFFu);
    // Close up (half a millimetre high): every segment.
    vp->setCamera(Camera{Vec3{0, -48, 120}, Vec3{0, -48, 20}, Vec3{0, 1, 0}, false, 30, 0.5});
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().pathSegments == 200'000);
}

TEST_CASE("a level of detail looks like the full path", "[render][backplot][lod]")
{
    // The 200k-segment spiral zoomed out, once as one backplot (drawn at a level of detail) and once cut into
    // backplots too short to get any (every segment drawn), compared with the reference images' tile comparison.
    const auto bp = spiralBackplot(200'000, 2);
    constexpr int w = 640, h = 360;
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            const auto light = theme("graphite-light");
            const Camera cam = standardViewCamera(StandardView::Iso, bp->bounds, double(w) / h, true);
            auto full = viewportOrSkip(w, h, software);
            full->setTheme(light);
            full->setPreset(presetFor(Tier::Low));
            const std::size_t piece = rd::kLodMinVertices - 2;
            for (std::size_t first = 0; first + 1 < bp->lineVertices.size(); first += piece) {
                auto part = std::make_shared<sim::Backplot>();
                const std::size_t last = std::min(first + piece, bp->lineVertices.size() - 1);
                part->lineVertices.assign(bp->lineVertices.begin() + std::ptrdiff_t(first),
                                          bp->lineVertices.begin() + std::ptrdiff_t(last) + 1);
                for (std::size_t i = 1; i <= last - first; ++i)
                    part->moveFirstVertex.push_back(std::uint32_t(i));
                part->moveFirstVertex.push_back(std::uint32_t(part->lineVertices.size()));
                part->bounds = bp->bounds;
                REQUIRE(full->addBackplot(part) != 0);
            }
            full->setCamera(cam);
            const Image fullDetail = capture(*full, w, h);
            REQUIRE(full->info().pathSegments == 200'000);
            auto vp = viewportOrSkip(w, h, software);
            vp->setTheme(light);
            vp->setPreset(presetFor(Tier::Low));
            vp->addBackplot(bp);
            vp->setCamera(cam);
            waitForLods(*vp);
            const Image lod = capture(*vp, w, h);
            const std::uint64_t drawn = vp->info().pathSegments;
            auto bgView = viewportOrSkip(w, h, software);
            bgView->setTheme(light);
            const Image background = capture(*bgView, w, h);
            const Comparison c = compare(lod, fullDetail, background);
            std::cout << "[backplot] level of detail (" << drawn << " of 200000 segments drawn) against full detail, "
                      << (software ? "software" : "adapter") << ": tile colour worst " << c.worstBlockColour << " mean "
                      << c.meanBlockColour << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                      << c.coverageA << " vs " << c.coverageB << "\n";
            CHECK(drawn * 4 < 200'000);
            CHECK(matches(c));
        }
    }
}

TEST_CASE("2 M backplot segments against the 500k-triangle baseline", "[render][backplot][cost][wallclock]")
{
    // PLAN.md 9 and 12 T-038: 2 M segments at the Low budget (30 FPS) on the reference machine or, without one, under
    // the baseline frame time on WARP. The baseline is the Low tier's own test scene (PLAN.md 6.1, the section 9
    // viewport row): 500k triangles, 1920x1080, the Low preset, orbiting, measured here on the same device in the same
    // run with frames interleaved, so a busy machine slows both alike. The frame time is lastFrameStats().cpuMs of
    // the offscreen viewport: the whole frame on the calling thread, waiting for the GPU included.
    constexpr std::uint32_t kSegments = 2'000'000;
    const auto bp = spiralBackplot(kSegments);
    REQUIRE(rd::validateBackplot(*bp).has_value());
    REQUIRE(rd::backplotSegmentCount(*bp) == kSegments);
    // The same mesh as ray_render_frame_stats (viewport_test.cpp): OpenCASCADE's sphere meshed to 500k triangles, with
    // its per-face vertices and its edges. A UV sphere made directly draws in about 60 % of the time on WARP and would
    // be a harder bar than the plan's baseline scene; meshing this one takes most of this test's time.
    const auto sphere = meshOf(kernel::testing::makeSphere(Vec3{0, 0, 0}, 50), 0.0008, 1.0 * kPi / 180);
    REQUIRE(sphere->indices.size() / 3 >= 500'000);
    const TierPreset low = presetFor(Tier::Low);
    REQUIRE(low.maxBackplotSegments >= int(kSegments)); // uploaded in full, not decimated
    const auto lodStart = std::chrono::steady_clock::now();
    const auto lods = rd::buildPathLods(*bp); // what the viewport's worker makes too
    std::cout << "[backplot] levels of detail of 2 M segments built in "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lodStart).count()
              << " ms (one worker thread)\n";
    REQUIRE(!lods.empty());
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            auto paths = viewportOrSkip(1920, 1080, software);
            auto mesh = viewportOrSkip(1920, 1080, software);
            for (IOffscreenViewport* vp : {paths.get(), mesh.get()}) {
                vp->setTheme(theme("graphite-dark"));
                vp->setPreset(low);
            }
            paths->addBackplot(bp);
            mesh->addMesh(sphere, Xform::identity(), kModel);
            waitForLods(*paths);
            Camera pathCam = standardViewCamera(StandardView::Iso, bp->bounds, 16.0 / 9.0, true);
            Camera meshCam =
                standardViewCamera(StandardView::Iso, Box3{Vec3{-50, -50, -50}, Vec3{50, 50, 50}}, 16.0 / 9.0, true);
            paths->setCamera(pathCam);
            mesh->setCamera(meshCam);
            REQUIRE(paths->renderNow().has_value()); // uploads
            REQUIRE(mesh->renderNow().has_value());
            std::vector<double> pathMs, meshMs, pathGpu, meshGpu;
            for (int i = 0; i < 24; ++i) {
                pathCam = orbitCamera(pathCam, 2, 0, pathCam.target);
                meshCam = orbitCamera(meshCam, 2, 0, meshCam.target);
                paths->setCamera(pathCam);
                mesh->setCamera(meshCam);
                REQUIRE(paths->renderNow().has_value());
                REQUIRE(mesh->renderNow().has_value());
                if (i < 4)
                    continue; // warm-up
                pathMs.push_back(paths->lastFrameStats().cpuMs);
                meshMs.push_back(mesh->lastFrameStats().cpuMs);
                pathGpu.push_back(paths->lastFrameStats().gpuMs);
                meshGpu.push_back(mesh->lastFrameStats().gpuMs);
            }
            const ViewportInfo info = paths->info();
            // GPU memory: the path itself (24 bytes a segment) and its levels of detail.
            std::uint64_t lodBytes = 0;
            for (const rd::PathLod& lod : lods)
                lodBytes += lod.vertices.size() * 24;
            CHECK(info.pathGpuBytes == (std::uint64_t(kSegments) + 1) * 24 + lodBytes);
            std::cout << "[backplot] " << info.backend << " on " << info.device
                      << (info.softwareRenderer ? " (software)" : "") << ", 1920x1080, Low preset: " << kSegments
                      << " segments (" << info.pathSegments << " drawn at the level of detail chosen): frame median "
                      << percentile(pathMs, 0.5) << " ms, 95th percentile " << percentile(pathMs, 0.95)
                      << " ms, GPU median " << percentile(pathGpu, 0.5) << " ms; baseline 500k triangles: median "
                      << percentile(meshMs, 0.5) << " ms, 95th percentile " << percentile(meshMs, 0.95)
                      << " ms, GPU median " << percentile(meshGpu, 0.5) << " ms; backplot GPU memory "
                      << info.pathGpuBytes << " bytes: 24 per segment for the path and " << double(lodBytes) / kSegments
                      << " per segment for its " << lods.size() << " levels of detail\n";
            if (info.softwareRenderer) {
                // The fallback acceptance: under the baseline frame time on WARP (llvmpipe on Linux).
                CHECK(percentile(pathMs, 0.5) <= percentile(meshMs, 0.5));
                CHECK(percentile(pathMs, 0.95) <= percentile(meshMs, 0.95));
            } else {
                // A hardware adapter here is no Low-tier machine; reported against the Low budget, 30 FPS.
                std::cout << "[backplot] Low budget 33.3 ms: 95th percentile "
                          << (percentile(pathMs, 0.95) <= 1000.0 / 30 ? "within" : "OVER") << " on this adapter\n";
            }
        }
    }
}
