// Picking and highlight (task T-015, PLAN.md 13 T-015 acceptance). Scripted picks at known pixels are checked against
// a CPU ray cast through the same display meshes (the oracle) and against analytic points; edge and vertex tolerance,
// filters, pickRect, the ID-buffer render counter, and hover and selection highlights in the rendered image.
#include "viewport_scenes.h"
#include "viewport_testing.h"

#include <pick.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;
using Catch::Matchers::WithinAbs;
using kernel::SubId;
using kernel::SubKind;
namespace rd = ray::render::detail;

namespace {

using Clock = std::chrono::steady_clock;

Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 scale(Vec3 a, double k)
{
    return {a.x * k, a.y * k, a.z * k};
}
double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double len(Vec3 v)
{
    return std::sqrt(dot(v, v));
}
Vec3 unit(Vec3 v)
{
    return scale(v, 1.0 / len(v));
}

// Where a world point lands on screen in pixels (continuous, pixel centres at +0.5), as the renderer projects it.
Vec2 project(const Camera& cam, Vec3 p, int w, int h)
{
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

struct Pixel {
    int x, y;
};
Pixel pixelOf(Vec2 p)
{
    return {int(std::floor(p.x)), int(std::floor(p.y))};
}

// One body or line set in a test scene, as the oracle sees it.
struct Body {
    ItemId id = 0;
    std::shared_ptr<const kernel::Mesh> mesh;
    Xform xf = Xform::identity();
    kernel::Shape shape;
};

Vec3 worldVertex(const Body& b, std::uint32_t v)
{
    return rd::transformPoint(b.xf, &b.mesh->positions[std::size_t(v) * 3]);
}

struct OracleHit {
    ItemId item = 0;
    std::uint32_t face = 0;
    double t = 0;
    Vec3 point{};
};

// Nearest triangle along the ray (Moeller-Trumbore), with the kernel face it belongs to.
std::optional<OracleHit> rayCast(const std::vector<Body>& bodies, const Ray& ray)
{
    std::optional<OracleHit> best;
    for (const Body& b : bodies) {
        for (const kernel::FaceRange& f : b.mesh->faces) {
            for (std::uint32_t i = 0; i + 2 < f.indexCount; i += 3) {
                const Vec3 a = worldVertex(b, b.mesh->indices[f.firstIndex + i]);
                const Vec3 e1 = sub(worldVertex(b, b.mesh->indices[f.firstIndex + i + 1]), a);
                const Vec3 e2 = sub(worldVertex(b, b.mesh->indices[f.firstIndex + i + 2]), a);
                const Vec3 p = cross(ray.dir, e2);
                const double det = dot(e1, p);
                if (std::abs(det) < 1e-14)
                    continue;
                const Vec3 s = sub(ray.origin, a);
                const double u = dot(s, p) / det;
                if (u < 0 || u > 1)
                    continue;
                const Vec3 q = cross(s, e1);
                const double v = dot(ray.dir, q) / det;
                if (v < 0 || u + v > 1)
                    continue;
                const double t = dot(e2, q) / det;
                if (t <= 0 || (best && t >= best->t))
                    continue;
                best = OracleHit{b.id, f.face.index, t, add(ray.origin, scale(ray.dir, t))};
            }
        }
    }
    return best;
}

std::optional<OracleHit> oracleAt(const std::vector<Body>& bodies, const Camera& cam, int x, int y, int w, int h)
{
    return rayCast(bodies, cameraRay(cam, x + 0.5, y + 0.5, w, h));
}

// The oracle at a pixel and at its neighbours within `r` agree: the pixel is not near a face boundary.
bool interior(const std::vector<Body>& bodies, const Camera& cam, int x, int y, int w, int h, int r)
{
    // Also for empty pixels: next to a silhouette the edge line covers background pixels.
    const auto at = oracleAt(bodies, cam, x, y, w, h);
    for (int dy = -r; dy <= r; dy += r) {
        for (int dx = -r; dx <= r; dx += r) {
            const auto n = oracleAt(bodies, cam, x + dx, y + dy, w, h);
            if (n.has_value() != at.has_value() || (n && (n->item != at->item || n->face != at->face)))
                return false;
        }
    }
    return true;
}

std::unique_ptr<IOffscreenViewport> viewportOrSkip(int w, int h, bool software = false)
{
    std::string whyNot;
    auto vp = makeViewport(w, h, software, &whyNot);
    if (!vp)
        SKIP("no RHI backend could be created: " << whyNot);
    return vp;
}

constexpr std::uint8_t kFaces = std::uint8_t(PickFilter::Faces);
constexpr std::uint8_t kEdges = std::uint8_t(PickFilter::Edges);
constexpr std::uint8_t kVertices = std::uint8_t(PickFilter::Vertices);
constexpr std::uint8_t kBodies = std::uint8_t(PickFilter::Bodies);
constexpr std::uint8_t kAll = kFaces | kEdges | kVertices | kBodies;

std::optional<SubKind> kindOf(const std::optional<PickHit>& h)
{
    return h ? std::optional<SubKind>(h->kind) : std::nullopt;
}
std::int64_t indexOf(const std::optional<PickHit>& h)
{
    return h ? std::int64_t(h->subIndex) : -1;
}
ItemId itemOf(const std::optional<PickHit>& h)
{
    return h ? h->item : 0;
}

// A pick through the public API, waiting for the asynchronous answer. `ms`: request to answer, measured inside the
// callback.
std::optional<PickHit> pickAt(IViewport& vp, int x, int y, std::uint8_t mask, double* ms = nullptr)
{
    std::optional<std::optional<PickHit>> got;
    Clock::time_point answered{};
    const Clock::time_point asked = Clock::now();
    vp.pick(x, y, mask, [&](std::optional<PickHit> h) {
        answered = Clock::now();
        got = h;
    });
    CHECK(!got.has_value()); // never synchronous
    REQUIRE(processEventsUntil([&] { return got.has_value(); }, 30000));
    if (ms)
        *ms = std::chrono::duration<double, std::milli>(answered - asked).count();
    return *got;
}

std::vector<PickHit> pickRectAt(IViewport& vp, int x0, int y0, int x1, int y1, std::uint8_t mask)
{
    std::optional<std::vector<PickHit>> got;
    vp.pickRect(x0, y0, x1, y1, mask, [&](std::vector<PickHit> h) { got = std::move(h); });
    REQUIRE(processEventsUntil([&] { return got.has_value(); }, 30000));
    return *got;
}

const MeshStyle kModel{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};

Body addBody(IViewport& vp, Result<kernel::Shape> shape, const Xform& xf = Xform::identity())
{
    Body b;
    b.shape = take(std::move(shape));
    JobContext ctx;
    b.mesh = std::make_shared<const kernel::Mesh>(
        take(kernel::tessellate(b.shape, kernel::MeshOptions{0.05, 10 * kPi / 180}, ctx)));
    b.xf = xf;
    b.id = vp.addMesh(b.mesh, xf, kModel);
    REQUIRE(b.id != 0);
    return b;
}

struct FaceInfo {
    std::uint32_t index;
    Vec3 centre, normal;
};

// Centre (mean of its distinct vertices) and normal of each face of a body with planar faces.
std::vector<FaceInfo> planarFaces(const Body& b)
{
    std::vector<FaceInfo> out;
    for (const kernel::FaceRange& f : b.mesh->faces) {
        std::set<std::uint32_t> verts(b.mesh->indices.begin() + f.firstIndex,
                                      b.mesh->indices.begin() + f.firstIndex + f.indexCount);
        Vec3 c{};
        for (const std::uint32_t v : verts)
            c = add(c, worldVertex(b, v));
        c = scale(c, 1.0 / double(verts.size()));
        const std::uint32_t v0 = *verts.begin();
        const Vec3 n{b.mesh->normals[v0 * 3], b.mesh->normals[v0 * 3 + 1], b.mesh->normals[v0 * 3 + 2]};
        out.push_back(FaceInfo{f.face.index, c, unit(n)});
    }
    return out;
}

bool facesCamera(const Camera& cam, const FaceInfo& f)
{
    const Vec3 toEye = cam.perspective ? sub(cam.eye, f.centre) : sub(cam.eye, cam.target);
    return dot(f.normal, toEye) > 1e-6;
}

const kernel::EdgeRange* edgeRange(const kernel::Mesh& mesh, std::uint32_t edge)
{
    for (const kernel::EdgeRange& e : mesh.edges) {
        if (e.edge.index == edge)
            return &e;
    }
    return nullptr;
}

Vec3 edgePoint(const Body& b, const kernel::EdgeRange& e, std::uint32_t k)
{
    return rd::transformPoint(b.xf, &b.mesh->edgePositions[(std::size_t(e.firstVertex) + k) * 3]);
}

std::set<std::uint32_t> adjacentIndices(const kernel::Shape& s, SubId of, SubKind wanted)
{
    std::set<std::uint32_t> out;
    for (const SubId id : kernel::adjacent(s, of, wanted))
        out.insert(id.index);
    return out;
}

// The kernel vertex a vertex PickHit names (the T-020 route): an end of edge vertexEdge(code), at `point`.
std::optional<std::uint32_t> kernelVertexOf(const kernel::Shape& s, const PickHit& h)
{
    for (const SubId v : kernel::adjacent(s, SubId{SubKind::Edge, vertexEdge(h.subIndex)}, SubKind::Vertex)) {
        const auto p = kernel::vertexPoint(s, v);
        if (p && len(sub(*p, h.point)) < 1e-3)
            return v.index;
    }
    return std::nullopt;
}

// A box seen from the iso direction, large enough that its faces are far wider than the tolerance.
struct BoxScene {
    std::unique_ptr<IOffscreenViewport> vp;
    Body box;
    Camera cam;
    int w = 0, h = 0;
    std::vector<FaceInfo> faces, visible;
};

BoxScene boxScene(int w, int h, Tier tier, bool software = false)
{
    BoxScene s;
    s.w = w;
    s.h = h;
    s.vp = viewportOrSkip(w, h, software);
    s.vp->setTheme(theme("graphite-dark"));
    s.vp->setPreset(presetFor(tier));
    s.box = addBody(*s.vp, kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 30, 20}));
    s.cam = standardViewCamera(StandardView::Iso, Box3{Vec3{0, 0, 0}, Vec3{40, 30, 20}}, double(w) / h, true);
    s.vp->setCamera(s.cam);
    s.faces = planarFaces(s.box);
    for (const FaceInfo& f : s.faces) {
        if (facesCamera(s.cam, f))
            s.visible.push_back(f);
    }
    REQUIRE(s.visible.size() == 3);
    return s;
}

double planeT(const Ray& r, Vec3 origin, Vec3 normal)
{
    return dot(sub(origin, r.origin), normal) / dot(r.dir, normal);
}

} // namespace

// ---------- CPU side, no GPU ----------
TEST_CASE("mesh index welds edge ends into the box's vertices", "[render][pick][unit]")
{
    const auto mesh = meshOf(kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 30, 20}));
    const auto index = rd::buildMeshIndex(*mesh);
    CHECK(index->vertices.size() == 8);
    for (const auto& v : index->vertices) {
        CHECK(v.edges.size() == 3);
        // The code names an end of one of the vertex's own edges, the smallest one.
        CHECK(std::binary_search(v.edges.begin(), v.edges.end(), vertexEdge(v.code)));
        CHECK(vertexEdge(v.code) == v.edges.front());
    }
    for (const kernel::EdgeRange& e : mesh->edges) {
        for (const bool last : {false, true}) {
            const auto* v = index->vertexOfCode(vertexCode(e.edge.index, last));
            REQUIRE(v);
            const float* p = &mesh->edgePositions[(std::size_t(e.firstVertex) + (last ? e.vertexCount - 1 : 0)) * 3];
            CHECK(v->pos[0] == p[0]);
            CHECK(v->pos[1] == p[1]);
            CHECK(v->pos[2] == p[2]);
        }
    }
    for (const kernel::FaceRange& f : mesh->faces)
        CHECK(mesh->faces[std::size_t(index->facePosition(f.face.index))].face.index == f.face.index);
    CHECK(index->facePosition(9999) == -1);
}

TEST_CASE("pick search: priority, tolerance and filters on a synthetic ID window", "[render][pick][unit]")
{
    // 40 x 40 target, identity camera; item 7 is a face everywhere with a vertical edge line at x = 20.
    rd::PickView view;
    view.width = view.height = 40;
    for (int i = 0; i < 16; i += 5)
        view.viewProj[i] = view.invViewProj[i] = 1.0;
    view.items.push_back(rd::PickItem{7, false, Xform::identity(), nullptr, nullptr});
    rd::IdWindow win;
    win.width = win.height = 40;
    win.pixels.resize(1600);
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            rd::IdPixel& p = win.pixels[std::size_t(y * 40 + x)];
            p.item = 7;
            p.kind = x == 20 ? rd::IdKind::Edge : rd::IdKind::Face;
            p.index = x == 20 ? 3 : (x < 20 ? 0 : 1);
            p.depth = 0.5;
        }
    }
    const auto hit = [&](int x, int y, std::uint8_t mask) {
        return rd::resolvePick(view, win, x, y, 6.0, mask);
    };
    CHECK(kindOf(hit(15, 10, kFaces | kEdges)) == SubKind::Edge); // 5 px away: the edge wins
    CHECK(indexOf(hit(15, 10, kFaces | kEdges)) == 3);
    CHECK(kindOf(hit(14, 10, kFaces | kEdges)) == SubKind::Edge); // 6 px: still inside
    CHECK(kindOf(hit(13, 10, kFaces | kEdges)) == SubKind::Face); // 7 px: outside
    CHECK(!hit(10, 10, kEdges).has_value());
    CHECK(kindOf(hit(15, 10, kFaces)) == SubKind::Face);
    CHECK(indexOf(hit(15, 10, kFaces)) == 0);
    // On the line itself a face-only pick takes the face next to it, never the edge.
    CHECK(kindOf(hit(20, 10, kFaces)) == SubKind::Face);
    CHECK(kindOf(hit(20, 10, kBodies)) == SubKind::Solid);
    CHECK(itemOf(hit(20, 10, kBodies)) == 7);
    CHECK(!hit(-1, 10, kAll).has_value());
    // The point: identity matrices, so NDC x and y of the pixel centre and NDC z = 2 * depth - 1 = 0.
    const auto ch = hit(30, 9, kFaces);
    REQUIRE(ch);
    const PickHit c = *ch;
    CHECK_THAT(c.point.x, WithinAbs(2.0 * 30.5 / 40 - 1, 1e-12));
    CHECK_THAT(c.point.y, WithinAbs(1 - 2.0 * 9.5 / 40, 1e-12));
    CHECK_THAT(c.point.z, WithinAbs(0, 1e-12));
    // pickRect: every element once, sorted.
    const auto rect = rd::resolvePickRect(view, win, 18, 0, 25, 39, kFaces | kEdges | kBodies);
    REQUIRE(rect.size() == 4);
    CHECK(rect[0].kind == SubKind::Edge);
    CHECK(rect[1].kind == SubKind::Face);
    CHECK(rect[1].subIndex == 0);
    CHECK(rect[2].subIndex == 1);
    CHECK(rect[3].kind == SubKind::Solid);
}

// ---------- GPU ----------
TEST_CASE("picks return the face under each visible face's centre, with its 3D point", "[render][pick][faces]")
{
    for (const Tier tier : {Tier::Low, Tier::High}) {
        BoxScene s = boxScene(400, 300, tier);
        REQUIRE(s.vp->renderNow().has_value());
        for (const FaceInfo& f : s.visible) {
            const Pixel px = pixelOf(project(s.cam, f.centre, s.w, s.h));
            INFO("tier " << int(tier) << ", face " << f.index << " at " << px.x << "," << px.y);
            const auto hit = pickAt(*s.vp, px.x, px.y, kFaces);
            REQUIRE(hit.has_value());
            CHECK(hit->item == s.box.id);
            CHECK(hit->kind == SubKind::Face);
            CHECK(hit->subIndex == f.index);
            CHECK(hit->moveIndex == 0);
            // Analytic point: the pixel's ray meets the face's plane.
            const Ray ray = cameraRay(s.cam, px.x + 0.5, px.y + 0.5, s.w, s.h);
            const Vec3 expect = add(ray.origin, scale(ray.dir, planeT(ray, f.centre, f.normal)));
            CHECK(len(sub(hit->point, expect)) < 0.02);
        }
        // Pick coordinates are logical pixels: at device pixel ratio 2 the same faces are at half the coordinates.
        s.vp->setDevicePixelRatio(2.0);
        for (const FaceInfo& f : s.visible) {
            const Pixel px = pixelOf(project(s.cam, f.centre, s.w, s.h));
            const auto hit = pickAt(*s.vp, px.x / 2, px.y / 2, kFaces);
            CHECK(indexOf(hit) == f.index);
        }
        s.vp->setDevicePixelRatio(1.0);
        // Background misses.
        CHECK(!pickAt(*s.vp, 2, 2, kAll).has_value());
        CHECK(!pickAt(*s.vp, -5, 10, kAll).has_value());
        CHECK(!pickAt(*s.vp, 10, 400, kAll).has_value());
    }
}

TEST_CASE("picks match a CPU ray cast on the box, cylinder and two-body scenes", "[render][pick][oracle]")
{
    constexpr int w = 240, h = 180;
    for (const std::string name : {"box", "cylinder", "two_bodies"}) {
        auto vp = viewportOrSkip(w, h);
        vp->setTheme(theme("graphite-dark"));
        vp->setPreset(presetFor(Tier::Medium));
        std::vector<Body> bodies;
        Box3 bounds{};
        bool perspective = true;
        if (name == "box") {
            bodies.push_back(addBody(*vp, kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 30, 20})));
            bounds = {Vec3{0, 0, 0}, Vec3{40, 30, 20}};
        } else if (name == "cylinder") {
            bodies.push_back(addBody(*vp, kernel::makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 15, 40)));
            bounds = {Vec3{-15, -15, 0}, Vec3{15, 15, 40}};
            perspective = false;
        } else {
            // A big box and, in front of it towards the eye, a small one placed by a transform: the nearer one
            // occludes part of the farther.
            bodies.push_back(addBody(*vp, kernel::makeBox(Vec3{0, 0, 0}, Vec3{60, 40, 10})));
            bounds = {Vec3{0, 0, 0}, Vec3{60, 40, 10}};
            const Camera c = standardViewCamera(StandardView::Iso, bounds, double(w) / h, true);
            const Vec3 front = add(Vec3{30, 20, 10}, scale(unit(sub(c.eye, Vec3{30, 20, 10})), 25));
            bodies.push_back(
                addBody(*vp, kernel::makeBox(Vec3{-6, -6, -6}, Vec3{6, 6, 6}), translation(front.x, front.y, front.z)));
        }
        const Camera cam = standardViewCamera(StandardView::Iso, bounds, double(w) / h, perspective);
        vp->setCamera(cam);
        std::map<ItemId, int> hitsPerItem;
        int checked = 0;
        for (int y = 3; y < h; y += 7) {
            for (int x = 3; x < w; x += 7) {
                const auto expect = oracleAt(bodies, cam, x, y, w, h);
                if (!interior(bodies, cam, x, y, w, h, 3))
                    continue; // near a face boundary, where 1 px edge lines and rasterisation rules decide
                const auto hit = pickAt(*vp, x, y, kFaces);
                INFO(name << " pixel " << x << "," << y);
                REQUIRE(hit.has_value() == expect.has_value());
                ++checked;
                if (!expect)
                    continue;
                CHECK(hit->item == expect->item);
                CHECK(hit->subIndex == expect->face);
                CHECK(len(sub(hit->point, expect->point)) < 0.02);
                ++hitsPerItem[hit->item];
            }
        }
        std::cout << "[pick] " << name << ": " << checked << " scripted picks agree with the ray cast\n";
        CHECK(checked > 300);
        for (const Body& b : bodies)
            CHECK(hitsPerItem[b.id] > 5);
        if (name == "cylinder") {
            // Analytic points: the top cap centre, and the side facing the camera at mid height.
            const Vec2 top = project(cam, Vec3{0, 0, 40}, w, h);
            const Pixel pt = pixelOf(top);
            const auto capHit = pickAt(*vp, pt.x, pt.y, kFaces);
            REQUIRE(capHit);
            const Ray capRay = cameraRay(cam, pt.x + 0.5, pt.y + 0.5, w, h);
            CHECK(len(sub(capHit->point, add(capRay.origin, scale(capRay.dir, planeT(capRay, Vec3{0, 0, 40},
                                                                                     Vec3{0, 0, 1}))))) < 0.02);
            const Vec3 toEye = unit(Vec3{cam.eye.x - cam.target.x, cam.eye.y - cam.target.y, 0});
            const Pixel ps = pixelOf(project(cam, Vec3{15 * toEye.x, 15 * toEye.y, 20}, w, h));
            const auto sideHit = pickAt(*vp, ps.x - 2, ps.y, kFaces);
            REQUIRE(sideHit);
            CHECK(sideHit->subIndex != capHit->subIndex);
            // On the true cylinder within the display mesh's chord tolerance (0.05 mm), at mid height.
            CHECK_THAT(std::hypot(sideHit->point.x, sideHit->point.y), WithinAbs(15, 0.06));
            CHECK(std::abs(sideHit->point.z - 20) < 3);
        }
        if (name == "two_bodies") {
            // The small body's centre pixel picks it, not the big one behind it; the body filter returns the item.
            const Xform& xf = bodies[1].xf;
            const Pixel pc = pixelOf(project(cam, Vec3{xf.m[3], xf.m[7], xf.m[11]}, w, h));
            const auto nearer = pickAt(*vp, pc.x, pc.y, kBodies);
            REQUIRE(nearer);
            CHECK(nearer->item == bodies[1].id);
            CHECK(nearer->kind == SubKind::Solid);
            CHECK(nearer->subIndex == 0);
            const auto behind = rayCast({bodies[0]}, cameraRay(cam, pc.x + 0.5, pc.y + 0.5, w, h));
            REQUIRE(behind); // the big box is really behind it
            CHECK(dot(sub(nearer->point, cam.eye), sub(nearer->point, cam.eye)) <
                  dot(sub(behind->point, cam.eye), sub(behind->point, cam.eye)));
        }
    }
}

TEST_CASE("edges and vertices are pickable within 6 px and not at 10 px", "[render][pick][tolerance]")
{
    for (const Tier tier : {Tier::Low, Tier::High}) {
        BoxScene s = boxScene(400, 300, tier);
        INFO("tier " << int(tier));
        // The edge between two visible faces, and the vertex the three visible faces share.
        const auto edgesA = adjacentIndices(s.box.shape, SubId{SubKind::Face, s.visible[0].index}, SubKind::Edge);
        const auto edgesB = adjacentIndices(s.box.shape, SubId{SubKind::Face, s.visible[1].index}, SubKind::Edge);
        std::vector<std::uint32_t> shared;
        std::set_intersection(edgesA.begin(), edgesA.end(), edgesB.begin(), edgesB.end(), std::back_inserter(shared));
        REQUIRE(shared.size() == 1);
        const std::uint32_t edge = shared[0];
        const kernel::EdgeRange* range = edgeRange(*s.box.mesh, edge);
        REQUIRE(range);
        const Vec3 a = edgePoint(s.box, *range, 0);
        const Vec3 b = edgePoint(s.box, *range, range->vertexCount - 1);
        const Vec2 sa = project(s.cam, a, s.w, s.h);
        const Vec2 sb = project(s.cam, b, s.w, s.h);
        const Vec2 mid{0.5 * (sa.x + sb.x), 0.5 * (sa.y + sb.y)};
        const double dl = std::hypot(sb.x - sa.x, sb.y - sa.y);
        REQUIRE(dl > 60);
        const Vec2 n{-(sb.y - sa.y) / dl, (sb.x - sa.x) / dl};
        const auto at = [&](Vec2 p, double dist, double side) {
            return pixelOf(Vec2{p.x + side * dist * n.x, p.y + side * dist * n.y});
        };
        for (const double side : {1.0, -1.0}) {
            // 4 px from the line: with the pixel grid, the nearest line pixel is at most about 5.2 px away.
            const Pixel near = at(mid, 4, side);
            const auto e5 = pickAt(*s.vp, near.x, near.y, kEdges);
            INFO("edge " << edge << ", side " << side);
            REQUIRE(e5.has_value());
            CHECK(e5->kind == SubKind::Edge);
            CHECK(e5->subIndex == edge);
            CHECK(e5->item == s.box.id);
            // The point lies on the edge's line.
            const Vec3 dir = unit(sub(b, a));
            const Vec3 off = sub(e5->point, a);
            CHECK(len(sub(off, scale(dir, dot(off, dir)))) < 0.5);
            // Priority: edge over face when both are enabled; a face-only pick never returns an edge.
            CHECK(kindOf(pickAt(*s.vp, near.x, near.y, kFaces | kEdges)) == SubKind::Edge);
            CHECK(kindOf(pickAt(*s.vp, near.x, near.y, kFaces)) == SubKind::Face);
            const Pixel far = at(mid, 10, side);
            CHECK(!pickAt(*s.vp, far.x, far.y, kEdges).has_value());
            CHECK(kindOf(pickAt(*s.vp, far.x, far.y, kFaces | kEdges)) == SubKind::Face);
            CHECK(itemOf(pickAt(*s.vp, far.x, far.y, kBodies)) == s.box.id);
        }
        // Right on the edge line: a face-only pick still answers a face.
        const Pixel on = pixelOf(mid);
        const auto onFace = pickAt(*s.vp, on.x, on.y, kFaces);
        REQUIRE(onFace);
        CHECK(onFace->kind == SubKind::Face);
        CHECK((onFace->subIndex == s.visible[0].index || onFace->subIndex == s.visible[1].index));

        // The vertex.
        std::set<std::uint32_t> common =
            adjacentIndices(s.box.shape, {SubKind::Face, s.visible[0].index}, SubKind::Vertex);
        for (int k = 1; k < 3; ++k) {
            const auto next =
                adjacentIndices(s.box.shape, {SubKind::Face, s.visible[std::size_t(k)].index}, SubKind::Vertex);
            std::set<std::uint32_t> keep;
            std::set_intersection(common.begin(), common.end(), next.begin(), next.end(),
                                  std::inserter(keep, keep.end()));
            common = keep;
        }
        REQUIRE(common.size() == 1);
        const Vec3 corner = take(kernel::vertexPoint(s.box.shape, SubId{SubKind::Vertex, *common.begin()}));
        const Vec2 sv = project(s.cam, corner, s.w, s.h);
        // Step from the vertex into the middle of one visible face, away from the edges.
        const Vec2 sf = project(s.cam, s.visible[2].centre, s.w, s.h);
        const double fl = std::hypot(sf.x - sv.x, sf.y - sv.y);
        const Vec2 into{(sf.x - sv.x) / fl, (sf.y - sv.y) / fl};
        const Pixel v5 = pixelOf(Vec2{sv.x + 5 * into.x, sv.y + 5 * into.y});
        const auto vh = pickAt(*s.vp, v5.x, v5.y, kVertices);
        REQUIRE(vh.has_value());
        CHECK(vh->kind == SubKind::Vertex);
        CHECK(vh->item == s.box.id);
        CHECK(len(sub(vh->point, corner)) < 1e-3);
        CHECK(kernelVertexOf(s.box.shape, *vh) == *common.begin());
        // Priority: vertex over edge and face.
        const auto all5 = pickAt(*s.vp, v5.x, v5.y, kAll);
        REQUIRE(all5);
        CHECK(all5->kind == SubKind::Vertex);
        CHECK(all5->subIndex == vh->subIndex);
        const Pixel v10 = pixelOf(Vec2{sv.x + 10 * into.x, sv.y + 10 * into.y});
        CHECK(!pickAt(*s.vp, v10.x, v10.y, kVertices).has_value());
        CHECK(kindOf(pickAt(*s.vp, v10.x, v10.y, kFaces)) == SubKind::Face);
        // Face-only and edge-only filters never return a vertex.
        CHECK(kindOf(pickAt(*s.vp, v5.x, v5.y, kFaces)) == SubKind::Face);
        const auto e = pickAt(*s.vp, v5.x, v5.y, kEdges);
        CHECK((!e || e->kind == SubKind::Edge));
    }
}

TEST_CASE("pickRect returns the visible faces, edges, vertices and bodies in the rectangle", "[render][pick][rect]")
{
    BoxScene s = boxScene(400, 300, Tier::Medium);
    std::set<std::uint32_t> faces, edges, vertices;
    for (const FaceInfo& f : s.visible) {
        faces.insert(f.index);
        const auto e = adjacentIndices(s.box.shape, SubId{SubKind::Face, f.index}, SubKind::Edge);
        edges.insert(e.begin(), e.end());
        const auto v = adjacentIndices(s.box.shape, SubId{SubKind::Face, f.index}, SubKind::Vertex);
        vertices.insert(v.begin(), v.end());
    }
    REQUIRE(edges.size() == 9);
    REQUIRE(vertices.size() == 7);

    const auto all = pickRectAt(*s.vp, 0, 0, s.w - 1, s.h - 1, kAll);
    std::set<std::uint32_t> gotFaces, gotEdges, gotVertices;
    int bodies = 0;
    for (const PickHit& h : all) {
        CHECK(h.item == s.box.id);
        if (h.kind == SubKind::Face)
            gotFaces.insert(h.subIndex);
        if (h.kind == SubKind::Edge)
            gotEdges.insert(h.subIndex);
        if (h.kind == SubKind::Vertex) {
            const auto v = kernelVertexOf(s.box.shape, h);
            REQUIRE(v);
            gotVertices.insert(*v);
        }
        bodies += h.kind == SubKind::Solid ? 1 : 0;
    }
    CHECK(gotFaces == faces);
    CHECK(gotEdges == edges);
    CHECK(gotVertices == vertices);
    CHECK(bodies == 1);
    CHECK(std::is_sorted(all.begin(), all.end(), [](const PickHit& a, const PickHit& b) {
        return std::tie(a.item, a.kind, a.subIndex) < std::tie(b.item, b.kind, b.subIndex);
    }));

    // Filters apply to rectangles too.
    for (const PickHit& h : pickRectAt(*s.vp, 0, 0, s.w - 1, s.h - 1, kFaces))
        CHECK(h.kind == SubKind::Face);
    CHECK(pickRectAt(*s.vp, 0, 0, s.w - 1, s.h - 1, kEdges).size() == 9);
    // A small rectangle inside one face holds just that face; corners may come in any order.
    const Pixel c = pixelOf(project(s.cam, s.visible[0].centre, s.w, s.h));
    const auto one = pickRectAt(*s.vp, c.x + 3, c.y + 3, c.x - 3, c.y - 3, kAll);
    REQUIRE(one.size() == 2);
    CHECK(one[0].kind == SubKind::Face);
    CHECK(one[0].subIndex == s.visible[0].index);
    CHECK(one[1].kind == SubKind::Solid);
    // Empty space.
    CHECK(pickRectAt(*s.vp, 0, 0, 5, 5, kAll).empty());
}

TEST_CASE("the ID buffer is drawn again only when the camera or the scene changed", "[render][pick][counter]")
{
    for (const bool software : {false, true}) {
        BoxScene s = boxScene(640, 480, Tier::Medium, software);
        REQUIRE(s.vp->renderNow().has_value());
        CHECK(s.vp->info().pickBufferRenders == 0); // drawing frames does not draw the ID buffer
        const Pixel c = pixelOf(project(s.cam, s.visible[0].centre, s.w, s.h));
        double first = 0;
        REQUIRE(pickAt(*s.vp, c.x, c.y, kAll, &first));
        const std::uint64_t base = s.vp->info().pickBufferRenders;
        CHECK(base == 1);
        const std::uint64_t framesBefore = s.vp->info().framesRendered;
        std::vector<double> ms;
        for (int i = 0; i < 100; ++i) {
            double t = 0;
            const auto hit = pickAt(*s.vp, c.x + (i % 11) - 5, c.y + (i % 7) - 3, kAll, &t);
            CHECK(hit.has_value());
            ms.push_back(t);
        }
        CHECK(s.vp->info().pickBufferRenders == base); // 100 picks, no new ID render
        CHECK(s.vp->info().framesRendered - framesBefore <= 100);
        std::sort(ms.begin(), ms.end());
        const ViewportInfo info = s.vp->info();
        std::cout << "[pick] latency, " << info.backend << " on " << info.device
                  << " at 640x480: first pick (ID render) " << first << " ms; cached: median " << ms[50]
                  << " ms, 95th percentile " << ms[95] << " ms\n";

        // Highlight, theme and preset changes do not invalidate it; camera, transform, visibility, style and
        // size changes do.
        s.vp->setHighlight({}, PickHit{s.box.id, SubKind::Face, s.visible[0].index, {}, 0});
        s.vp->setTheme(theme("graphite-light"));
        s.vp->setPreset(presetFor(Tier::High));
        pickAt(*s.vp, c.x, c.y, kAll);
        CHECK(s.vp->info().pickBufferRenders == base);
        s.vp->setCamera(orbitCamera(s.cam, 0.5, 0, s.cam.target));
        pickAt(*s.vp, c.x, c.y, kAll);
        CHECK(s.vp->info().pickBufferRenders == base + 1);
        s.vp->setCamera(s.cam);
        double changed = 0;
        pickAt(*s.vp, c.x, c.y, kAll, &changed);
        CHECK(s.vp->info().pickBufferRenders == base + 2);
        s.vp->setTransform(s.box.id, translation(0, 0, 0));
        pickAt(*s.vp, c.x, c.y, kAll);
        CHECK(s.vp->info().pickBufferRenders == base + 3);
        s.vp->setStyle(s.box.id, MeshStyle{Token::ModelFace, Token::ModelEdge, 1.0f, false, false});
        pickAt(*s.vp, c.x, c.y, kAll);
        CHECK(s.vp->info().pickBufferRenders == base + 4);
        s.vp->resize(320, 240);
        pickAt(*s.vp, 10, 10, kAll);
        CHECK(s.vp->info().pickBufferRenders == base + 5);
        std::cout << "[pick] latency after a camera change (" << info.backend << "): " << changed << " ms\n";
    }
}

TEST_CASE("pick latency on a 500k-triangle mesh at 1920x1080", "[render][pick][latency]")
{
    const auto mesh = meshOf(kernel::testing::makeSphere(Vec3{0, 0, 0}, 50), 0.0008, 1.0 * kPi / 180);
    REQUIRE(mesh->indices.size() / 3 >= 500'000);
    for (const bool software : {false, true}) {
        auto vp = viewportOrSkip(1920, 1080, software);
        vp->setTheme(theme("graphite-dark"));
        vp->setPreset(presetFor(Tier::Medium));
        const ItemId id = vp->addMesh(mesh, Xform::identity(), kModel);
        Camera cam =
            standardViewCamera(StandardView::Iso, Box3{Vec3{-50, -50, -50}, Vec3{50, 50, 50}}, 16.0 / 9.0, true);
        vp->setCamera(cam);
        REQUIRE(vp->renderNow().has_value());
        REQUIRE(pickAt(*vp, 960, 540, kAll)); // pipelines and ID buffer made
        std::vector<double> cached, fresh;
        for (int i = 0; i < 20; ++i) {
            double t = 0;
            const auto hit = pickAt(*vp, 940 + i, 530 + i, kAll, &t);
            CHECK(itemOf(hit) == id);
            cached.push_back(t);
        }
        for (int i = 0; i < 10; ++i) {
            cam = orbitCamera(cam, 1, 0, cam.target);
            vp->setCamera(cam);
            REQUIRE(vp->renderNow().has_value()); // the frame the camera change asked for
            double t = 0;
            CHECK(itemOf(pickAt(*vp, 960, 540, kAll, &t)) == id);
            fresh.push_back(t);
        }
        std::sort(cached.begin(), cached.end());
        std::sort(fresh.begin(), fresh.end());
        const ViewportInfo info = vp->info();
        std::cout << "[pick] 500k triangles at 1920x1080, " << info.backend << " on " << info.device
                  << ": cached ID buffer median " << cached[10] << " ms (worst " << cached.back()
                  << "), after a camera change (ID render + read-back) median " << fresh[5] << " ms (worst "
                  << fresh.back() << ")\n";
    }
}

TEST_CASE("pick results arrive asynchronously on the UI thread; empty scenes miss", "[render][pick][async]")
{
    auto vp = viewportOrSkip(64, 48);
    vp->setTheme(theme("graphite-dark"));
    const std::thread::id ui = std::this_thread::get_id();
    std::optional<std::optional<PickHit>> hit;
    std::optional<std::vector<PickHit>> hits;
    std::thread::id where{};
    vp->pick(10, 10, kAll, [&](std::optional<PickHit> h) {
        where = std::this_thread::get_id();
        hit = h;
    });
    vp->pickRect(0, 0, 63, 47, kAll, [&](std::vector<PickHit> h) { hits = std::move(h); });
    CHECK(!hit.has_value());
    CHECK(!hits.has_value());
    REQUIRE(processEventsUntil([&] { return hit.has_value() && hits.has_value(); }, 10000));
    CHECK(where == ui);
    CHECK(!hit->has_value());
    CHECK(hits->empty());
    // A null callback is ignored.
    vp->pick(1, 1, kAll, nullptr);
    vp->pickRect(0, 0, 1, 1, kAll, nullptr);
    REQUIRE(vp->renderNow().has_value());
}

TEST_CASE("segments of line items are pickable as edges", "[render][pick][lines]")
{
    constexpr int w = 320, h = 240;
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-dark"));
    const std::vector<Vec3> segs{{0, 0, 0}, {50, 0, 0}, {0, 0, 0}, {0, 50, 0}};
    const ItemId lines = vp->addLines(segs, Token::AxisX, 3.0f, false);
    const Camera cam =
        standardViewCamera(StandardView::Top, Box3{Vec3{0, 0, 0}, Vec3{50, 50, 1}}, double(w) / h, false);
    vp->setCamera(cam);
    const Pixel onY = pixelOf(project(cam, Vec3{0, 30, 0}, w, h));
    const auto hit = pickAt(*vp, onY.x + 4, onY.y, kEdges);
    REQUIRE(hit);
    CHECK(hit->item == lines);
    CHECK(hit->kind == SubKind::Edge);
    CHECK(hit->subIndex == 1);
    CHECK(!pickAt(*vp, onY.x + 4, onY.y, kFaces).has_value());
    CHECK(itemOf(pickAt(*vp, onY.x + 4, onY.y, kBodies)) == lines);
}

TEST_CASE("hover and selection highlights change the image where expected, in theme colours",
          "[render][pick][highlight]")
{
    constexpr int w = 300, h = 225;
    // Highlight tokens set to colours the model never uses, so the test can tell them apart. Only tokens change.
    Theme custom = *theme("graphite-dark");
    custom.colors[std::size_t(Token::Selection)] = Color{0.0f, 1.0f, 0.0f, 1.0f};
    custom.colors[std::size_t(Token::SelectionFill)] = Color{0.0f, 1.0f, 0.0f, 0.5f};
    custom.colors[std::size_t(Token::Hover)] = Color{1.0f, 0.0f, 0.0f, 1.0f};
    const auto tokens = std::make_shared<const Theme>(custom);
    for (const Tier tier : {Tier::Low, Tier::High}) {
        BoxScene s = boxScene(w, h, tier);
        INFO("tier " << int(tier));
        s.vp->setTheme(tokens);
        // The very first MSAA frame on the device can differ from later identical ones in a few edge pixels (seen
        // on an NVIDIA GPU: 6 pixels by 3/255), so the reference is the second capture.
        static_cast<void>(capture(*s.vp, w, h));
        const Image base = capture(*s.vp, w, h);
        std::vector<Body> bodies{s.box};
        // Per pixel, which face the ray cast sees.
        std::vector<int> faceAt(std::size_t(w) * h, -1);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (const auto o = oracleAt(bodies, s.cam, x, y, w, h))
                    faceAt[std::size_t(y) * w + std::size_t(x)] = int(o->face);
            }
        }
        const auto nearFace = [&](int x, int y, int face, int r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h && faceAt[std::size_t(yy) * w + std::size_t(xx)] == face)
                        return true;
                }
            }
            return false;
        };
        const auto changedPixels = [&](const Image& img, int threshold) {
            std::vector<Pixel> out;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (maxChannelDiff(rgbAt(img, x, y), rgbAt(base, x, y)) > threshold)
                        out.push_back({x, y});
                }
            }
            return out;
        };

        // Selected face: its pixels turn green, nothing else changes.
        const FaceInfo& f = s.visible[0];
        const PickHit faceHit{s.box.id, SubKind::Face, f.index, f.centre, 0};
        s.vp->setHighlight(std::span<const PickHit>(&faceHit, 1), std::nullopt);
        const Image sel = capture(*s.vp, w, h);
        int inside = 0, insideChanged = 0, outsideChanged = 0, greener = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const auto a = rgbAt(base, x, y);
                const auto b = rgbAt(sel, x, y);
                const bool changed = maxChannelDiff(a, b) > 4;
                if (faceAt[std::size_t(y) * w + std::size_t(x)] == int(f.index) && !nearFace(x, y, -1, 2) &&
                    nearFace(x, y, int(f.index), 0)) {
                    ++inside;
                    insideChanged += changed;
                    greener += changed && b[1] - a[1] > 0 && b[0] < a[0] && b[2] < a[2];
                } else if (changed && !nearFace(x, y, int(f.index), 2)) {
                    ++outsideChanged;
                }
            }
        }
        CHECK(inside > 1000);
        CHECK(insideChanged > inside * 95 / 100);
        CHECK(greener > insideChanged * 95 / 100);
        CHECK(outsideChanged == 0);

        // Hover on an edge: red pixels along that edge only.
        const auto edges = adjacentIndices(s.box.shape, SubId{SubKind::Face, f.index}, SubKind::Edge);
        const std::uint32_t edge = *edges.begin();
        const kernel::EdgeRange* range = edgeRange(*s.box.mesh, edge);
        REQUIRE(range);
        const Vec2 ea = project(s.cam, edgePoint(s.box, *range, 0), w, h);
        const Vec2 eb = project(s.cam, edgePoint(s.box, *range, range->vertexCount - 1), w, h);
        s.vp->setHighlight({}, PickHit{s.box.id, SubKind::Edge, edge, {}, 0});
        const Image hov = capture(*s.vp, w, h);
        const auto hovChanged = changedPixels(hov, 8);
        int offEdge = 0, red = 0;
        for (const Pixel& p : hovChanged) {
            // Distance from the pixel centre to the projected segment.
            const double vx = eb.x - ea.x, vy = eb.y - ea.y;
            const double t =
                std::clamp(((p.x + 0.5 - ea.x) * vx + (p.y + 0.5 - ea.y) * vy) / (vx * vx + vy * vy), 0.0, 1.0);
            const double d = std::hypot(p.x + 0.5 - (ea.x + t * vx), p.y + 0.5 - (ea.y + t * vy));
            offEdge += d > 5 ? 1 : 0;
            const auto c = rgbAt(hov, p.x, p.y);
            red += c[0] > c[1] && c[0] > c[2] ? 1 : 0;
        }
        CHECK(double(hovChanged.size()) > std::hypot(eb.x - ea.x, eb.y - ea.y) * 1.5);
        CHECK(offEdge == 0);
        CHECK(red > int(hovChanged.size()) * 8 / 10);

        // Hover on a vertex: a red marker around it only.
        const auto corners = adjacentIndices(s.box.shape, SubId{SubKind::Edge, edge}, SubKind::Vertex);
        const Vec3 corner = take(kernel::vertexPoint(s.box.shape, SubId{SubKind::Vertex, *corners.begin()}));
        const Vec2 sv = project(s.cam, corner, w, h);
        const Pixel pv = pixelOf(sv);
        const auto vertexHit = pickAt(*s.vp, pv.x, pv.y, kVertices);
        REQUIRE(vertexHit);
        s.vp->setHighlight({}, vertexHit);
        const auto vChanged = changedPixels(capture(*s.vp, w, h), 8);
        CHECK(vChanged.size() > 12);
        for (const Pixel& p : vChanged)
            CHECK(std::hypot(p.x + 0.5 - sv.x, p.y + 0.5 - sv.y) < 9);

        // Selected body: every visible face pixel changes.
        const PickHit body{s.box.id, SubKind::Solid, 0, {}, 0};
        s.vp->setHighlight(std::span<const PickHit>(&body, 1), std::nullopt);
        const Image whole = capture(*s.vp, w, h);
        int covered = 0, bodyChanged = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (faceAt[std::size_t(y) * w + std::size_t(x)] < 0 || nearFace(x, y, -1, 2))
                    continue;
                ++covered;
                bodyChanged += maxChannelDiff(rgbAt(whole, x, y), rgbAt(base, x, y)) > 4 ? 1 : 0;
            }
        }
        CHECK(bodyChanged > covered * 95 / 100);

        // Clearing the highlight gives the original image back.
        s.vp->setHighlight({}, std::nullopt);
        const Image back = capture(*s.vp, w, h);
        const auto left = changedPixels(back, 1);
        int worst = 0;
        for (const Pixel& p : left)
            worst = std::max(worst, maxChannelDiff(rgbAt(back, p.x, p.y), rgbAt(base, p.x, p.y)));
        INFO(left.size() << " pixels differ, by up to " << worst
                         << (left.empty()
                                 ? ""
                                 : " first at " + std::to_string(left[0].x) + "," + std::to_string(left[0].y)));
        CHECK(left.empty());
    }
}
