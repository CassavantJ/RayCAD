// Simulation stock display (task T-073, PLAN.md 12 T-073 acceptance; ray_render/viewport.h "Simulation stock"):
//   - updating a dirty region uploads only that region: the byte counter (ViewportInfo::stockUploadBytes) moves by
//     exactly the bytes of the tiles the dirty mesh brings, far below a full upload;
//   - reference images in both themes (a cut stock with a collision mark, and the same in a capped section);
//   - what the acceptance needs to mean anything: an incrementally updated stock looks like the stock meshed afresh;
//     the walls that close each tile inside the material never show (against the same stock as ONE solid without
//     them, from several sides, close up, in sections through and exactly on a tile seam, and see-through); the cut,
//     uncut and marked colours; hide, show, recolour and marks upload nothing; picks name the tile;
//   - the CPU rules of stock.h (pieces, the allocator, the shader block) and the theme colours;
//   - [cost], its own ctest: a 2 M-triangle display of the benchmark stock at 0.05 mm, a one-move update against a full
//     upload, and the frame time.
//
// To regenerate the reference images after an intended change: RAY_UPDATE_RENDER_REFERENCES=1 and run
//   ray_render_tests "[stock][reference]"
// The software-rasteriser pass writes the PNGs into tests/unit/render/reference/; review them before committing.
// Screenshots for review: RAY_T073_SHOTS=<folder> and run ray_render_tests "[.shots]".
#include "viewport_testing.h"

#include <stock.h>

#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;
namespace rd = ray::render::detail;

namespace {

constexpr std::uint8_t kFaces = std::uint8_t(PickFilter::Faces);

std::unique_ptr<IOffscreenViewport> viewportOrSkip(int w, int h, bool software = false)
{
    std::string whyNot;
    auto vp = makeViewport(w, h, software, &whyNot);
    if (!vp)
        SKIP("no RHI backend could be created: " << whyNot);
    return vp;
}

// ---------- stock models (written against include/ray/sim.h) ----------
class NoDispatcher final : public IUiDispatcher {
public:
    void post(std::function<void()> f) override { f(); }
};

IJobSystem& jobs()
{
    static NoDispatcher dispatcher;
    static std::unique_ptr<IJobSystem> js = makeJobSystem(dispatcher, 8);
    return *js;
}

sim::ToolShape flatTool(double diameter, double flute = 30)
{
    const double r = diameter / 2;
    sim::ToolShape t;
    t.cuttingProfile = {{0, 0}, {r, 0}, {r, flute}};
    t.shankProfile = {{r, flute}, {r, flute + 40}};
    return t;
}

sim::ToolShape ballTool(double diameter, double flute = 30)
{
    const double r = diameter / 2;
    sim::ToolShape t;
    for (int i = 0; i <= 12; ++i) {
        const double a = kPi / 2 * i / 12;
        t.cuttingProfile.push_back({r * std::sin(a), r - r * std::cos(a)});
    }
    t.cuttingProfile.push_back({r, flute});
    t.shankProfile = {{r, flute}, {r, flute + 40}};
    return t;
}

std::unique_ptr<sim::IStockModel> blockStock(const Box3& box, double cell)
{
    std::unique_ptr<sim::IStockModel> m = sim::makeTriDexelCpu(jobs());
    m->reset({}, box, sim::StockResolution{cell});
    return m;
}

sim::IStockModel::CutMove line(Vec3 from, Vec3 to)
{
    return sim::IStockModel::CutMove{from, to, false};
}

// The demo stock: 60 x 40 x 15 mm, top at z = 0, cells of 0.25 mm, so tiles (64 cells) of 16 mm: 4 x 3 tiles with
// seams at x = 16, 32, 48 and y = 16, 32.
const Box3 kDemo{Vec3{0, 0, -15}, Vec3{60, 40, 0}};
constexpr double kDemoCell = 0.25;
constexpr double kTileMm = 64 * kDemoCell;

// Cut `step` (1 to 5) of the demo: a pocket across two seams, a ball groove, a hole through on a tile corner, a slot
// along a seam and a step on the right side.
void cutStep(sim::IStockModel& m, int step)
{
    if (step == 1) {
        std::vector<sim::IStockModel::CutMove> pocket;
        Vec3 at{8, 8, 5};
        pocket.push_back(line(at, {8, 8, -4}));
        at = {8, 8, -4};
        for (double y = 8; y <= 30.01; y += 4) {
            const double x = (int(std::lround((y - 8) / 4)) % 2 == 0) ? 30 : 8;
            pocket.push_back(line(at, {at.x, y, -4}));
            pocket.push_back(line({at.x, y, -4}, {x, y, -4}));
            at = {x, y, -4};
        }
        std::vector<double> removed(pocket.size());
        m.cutBatch(flatTool(8), pocket, removed);
    } else if (step == 2) {
        m.cut(ballTool(6), {36, 6, -2}, {56, 34, -3.5}, false);
    } else if (step == 3) {
        m.cut(flatTool(5), {32, 16, 5}, {32, 16, -16}, false);
    } else if (step == 4) {
        m.cut(flatTool(6), {5, 32, -2.5}, {28, 32, -2.5}, false);
    } else if (step == 5) {
        m.cut(flatTool(10), {62, -5, -6}, {62, 45, -6}, false);
    }
}

void cutDemo(sim::IStockModel& m, int steps = 5)
{
    for (int step = 1; step <= steps; ++step)
        cutStep(m, step);
}

// The demo stock, cut.
std::unique_ptr<sim::IStockModel> demoStock()
{
    auto m = blockStock(kDemo, kDemoCell);
    cutDemo(*m);
    return m;
}

std::shared_ptr<const kernel::Mesh> share(kernel::Mesh m)
{
    return std::make_shared<const kernel::Mesh>(std::move(m));
}

// The display mesh with every vertex normal replaced by its facet's (the column mesh's vertices belong to one face
// each), so its shading can be compared with a mesh triangulated otherwise.
kernel::Mesh withFacetNormals(kernel::Mesh m)
{
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const float* a = &m.positions[3 * std::size_t(m.indices[t])];
        const float* b = &m.positions[3 * std::size_t(m.indices[t + 1])];
        const float* c = &m.positions[3 * std::size_t(m.indices[t + 2])];
        const double u[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
        const double v[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
        double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 0)
            continue;
        for (int k = 0; k < 3; ++k) {
            float* out = &m.normals[3 * std::size_t(m.indices[t + std::size_t(k)])];
            out[0] = float(n[0] / len), out[1] = float(n[1] / len), out[2] = float(n[2] / len);
        }
    }
    return m;
}

// IStockModel::toSolid as a display mesh: the same surface as toMesh at full resolution, but one closed solid with no
// walls between tiles. Unwelded, facet normals, one face range.
kernel::Mesh solidMesh(const kernel::TriSoup& s)
{
    kernel::Mesh m;
    for (const std::uint32_t i : s.indices) {
        for (int k = 0; k < 3; ++k)
            m.positions.push_back(float(s.positions[3 * std::size_t(i) + std::size_t(k)]));
        m.indices.push_back(std::uint32_t(m.indices.size()));
    }
    m.normals.assign(m.positions.size(), 0.0f);
    m.faces.push_back({kernel::SubId{kernel::SubKind::Face, 0}, 0, std::uint32_t(m.indices.size())});
    return withFacetNormals(std::move(m));
}

// Bytes the renderer uploads for every face range of `mesh` (stock.h: 28 per vertex, 12 per triangle).
std::uint64_t pieceBytes(const std::shared_ptr<const kernel::Mesh>& mesh, std::uint64_t* ranges = nullptr)
{
    std::uint64_t bytes = 0;
    const auto table = rd::piecesOf(mesh);
    for (const auto& p : table->pieces)
        bytes += p->vertexBytes() + p->indexBytes();
    if (ranges)
        *ranges = table->pieces.size();
    return bytes;
}

Camera lookAt(Vec3 target, double azimuthDeg, double elevationDeg, double distance, bool perspective = true)
{
    const double az = azimuthDeg * kPi / 180, el = elevationDeg * kPi / 180;
    Camera c{};
    c.target = target;
    c.eye = {target.x + distance * std::cos(el) * std::cos(az), target.y + distance * std::cos(el) * std::sin(az),
             target.z + distance * std::sin(el)};
    c.up = {0, 0, 1};
    c.perspective = perspective;
    c.fovDeg = 30;
    c.orthoHeight = distance * 0.5;
    return c;
}

Vec3 centreOf(const Box3& b)
{
    return {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2};
}

// Pixels whose colours differ by more than `threshold` in some channel; with `diffPath`, writes them out (red on the
// first image, darkened) for a look.
int differingPixels(const Image& a, const Image& b, int threshold, const std::string& diffPath = {})
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    int count = 0;
    Image diff = a;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const bool differs = maxChannelDiff(rgbAt(a, x, y), rgbAt(b, x, y)) > threshold;
            count += differs ? 1 : 0;
            const std::size_t i = (std::size_t(y) * std::size_t(a.width) + std::size_t(x)) * 4;
            diff.rgba[i] = differs ? 255 : std::uint8_t(diff.rgba[i] / 3);
            diff.rgba[i + 1] = differs ? 0 : std::uint8_t(diff.rgba[i + 1] / 3);
            diff.rgba[i + 2] = differs ? 0 : std::uint8_t(diff.rgba[i + 2] / 3);
        }
    }
    if (!diffPath.empty() && count > 0)
        static_cast<void>(writePng(diffPath, diff));
    return count;
}

std::string tempPng(const std::string& name)
{
    return (std::filesystem::temp_directory_path() / ("ray_render_" + name + ".png")).string();
}

// The pixel (centre) of a world point in an orthographic view from above (cameraRay's origin maps linearly).
std::pair<int, int> pixelOfTopView(const Camera& cam, int w, int h, double x, double y)
{
    const Ray r0 = cameraRay(cam, 0, 0, w, h);
    const Ray rx = cameraRay(cam, w, 0, w, h);
    const Ray ry = cameraRay(cam, 0, h, w, h);
    // origin(px, py) = r0 + px / w * (rx - r0) + py / h * (ry - r0), solved for (px, py) in x and y.
    const double ax = (rx.origin.x - r0.origin.x) / w, ay = (rx.origin.y - r0.origin.y) / w;
    const double bx = (ry.origin.x - r0.origin.x) / h, by = (ry.origin.y - r0.origin.y) / h;
    const double det = ax * by - ay * bx;
    const double dx = x - r0.origin.x, dy = y - r0.origin.y;
    const double px = (dx * by - dy * bx) / det, py = (ax * dy - ay * dx) / det;
    return {int(std::floor(px)), int(std::floor(py))};
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

// sRGB 8-bit to CIELAB (D65), for the colour distances of the theme test.
std::array<double, 3> lab(const Color& linear)
{
    const double r = linear.r, g = linear.g, b = linear.b;
    const double X = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double Z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    const auto f = [](double t) {
        return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116;
    };
    return {116 * f(Y) - 16, 500 * (f(X) - f(Y)), 200 * (f(Y) - f(Z))};
}

double deltaE(const Color& a, const Color& b)
{
    const auto p = lab(a), q = lab(b);
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

// A mesh of `faces` face ranges, face f a grid of quads (4 vertices each) of size `quads[f]`, side by side in x.
std::shared_ptr<const kernel::Mesh> quadFaces(const std::vector<std::uint32_t>& faceNumbers,
                                              const std::vector<int>& quads)
{
    kernel::Mesh m;
    for (std::size_t f = 0; f < faceNumbers.size(); ++f) {
        const std::uint32_t first = std::uint32_t(m.indices.size());
        const float x0 = float(faceNumbers[f]) * 2.0f;
        for (int q = 0; q < quads[f]; ++q) {
            const std::uint32_t base = std::uint32_t(m.positions.size() / 3);
            const float y0 = float(q) * 0.01f;
            for (const auto& [dx, dy] : {std::pair{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.01f}, {0.0f, 0.01f}}) {
                m.positions.insert(m.positions.end(), {x0 + dx, y0 + dy, 0.0f});
                m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            }
            m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        m.faces.push_back(
            {kernel::SubId{kernel::SubKind::Face, faceNumbers[f]}, first, std::uint32_t(m.indices.size()) - first});
    }
    return share(std::move(m));
}

} // namespace

// ---------- CPU rules (stock.h) ----------
TEST_CASE("RangeAllocator places first fit and merges what is freed", "[render][stock]")
{
    rd::RangeAllocator a(100);
    CHECK(a.allocate(30) == 0u);
    CHECK(a.allocate(30) == 30u);
    CHECK(a.allocate(30) == 60u);
    CHECK(!a.allocate(11).has_value());
    CHECK(a.allocate(10) == 90u);
    CHECK(a.used() == 100);
    a.free(30, 30);
    a.free(0, 30); // merges with the range after it
    CHECK(a.freeRanges() == 1);
    CHECK(a.largestFree() == 60);
    CHECK(a.allocate(50) == 0u); // first fit
    a.free(60, 30);              // merges with the 10 left at 50
    CHECK(a.largestFree() == 40);
    a.free(0, 50);
    a.free(90, 10);
    CHECK(a.used() == 0);
    CHECK(a.freeRanges() == 1);
    CHECK(a.largestFree() == 100);
    CHECK(!a.allocate(0).has_value());
}

TEST_CASE("a dirty update replaces face ranges by face number", "[render][stock]")
{
    const auto first = quadFaces({0, 1, 2, 3}, {2, 3, 4, 5});
    const auto table = rd::piecesOf(first);
    REQUIRE(table->pieces.size() == 4);
    CHECK(table->triangles == 2 * (2 + 3 + 4 + 5));
    // Face 1 again, larger; face 2 emptied; 0 and 3 not named.
    kernel::Mesh patch = *quadFaces({1}, {7});
    patch.faces.push_back({kernel::SubId{kernel::SubKind::Face, 2}, std::uint32_t(patch.indices.size()), 0});
    const auto next = rd::patchPieces(table, share(patch), Box3{{2, -1, -1}, {6, 1, 1}});
    REQUIRE(next->pieces.size() == 3);
    CHECK(next->pieces[0] == table->pieces[0]); // kept as it was: the same piece, nothing to upload
    CHECK(next->pieces[1]->face() == 1);
    CHECK(next->pieces[1]->triangles() == 14);
    CHECK(next->pieces[1] != table->pieces[1]);
    CHECK(next->pieces[2] == table->pieces[3]);
    CHECK(next->triangles == 2 * (2 + 7 + 5));
    // A face whose bounds lie inside the dirty box and that the new mesh leaves out is gone; one that only touches the
    // box (its neighbour's side) stays.
    const auto inBox = rd::patchPieces(table, share(kernel::Mesh{}), Box3{{3.9, -1, -1}, {5.5, 1, 1}});
    REQUIRE(inBox->pieces.size() == 3);
    CHECK(inBox->pieces[2]->face() == 3);
    const auto touching = rd::patchPieces(table, share(kernel::Mesh{}), Box3{{4.5, -1, -1}, {5.5, 1, 1}});
    CHECK(touching->pieces.size() == 4);
    // Nothing changed (IStockModel's empty mesh and empty box): the same pieces.
    const auto none = rd::patchPieces(table, share(kernel::Mesh{}), Box3{{1, 1, 1}, {0, 0, 0}});
    CHECK(none->pieces == table->pieces);
    // A full replacement makes every piece new.
    const auto full = rd::piecesOf(first);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(full->pieces[i] != table->pieces[i]);
    REQUIRE(table->bounds.has_value());
    CHECK(table->bounds->min.x == 0);
    CHECK(table->bounds->max.x == 7);
}

TEST_CASE("a piece packs once, rebases its indices and lets go of its source", "[render][stock]")
{
    auto mesh = std::make_shared<kernel::Mesh>(*quadFaces({4, 9}, {1, 2}));
    std::weak_ptr<const kernel::Mesh> weak = mesh;
    auto table = rd::piecesOf(mesh);
    mesh.reset();
    REQUIRE(table->pieces.size() == 2);
    const rd::MeshPiece& p = *table->pieces[1];
    CHECK(p.face() == 9);
    CHECK(p.vertexCount() == 8);
    CHECK(p.vertexBytes() == 8 * 28);
    CHECK(p.indexBytes() == 4 * 12);
    CHECK(!weak.expired()); // not packed yet: the source is needed
    const rd::PackedPiece& packed = p.packed();
    REQUIRE(packed.vertices.size() == 8 * 28);
    REQUIRE(packed.indices.size() == 12);
    CHECK(*std::max_element(packed.indices.begin(), packed.indices.end()) == 7);
    std::uint32_t id = 0;
    std::memcpy(&id, packed.vertices.data() + 24, 4);
    CHECK(id == 10); // face number + 1
    CHECK(&p.packed() == &packed);
    static_cast<void>(table->pieces[0]->packed());
    CHECK(weak.expired()); // every piece packed: the source mesh is freed

    // A range that uses scattered vertices of a shared pool is gathered.
    kernel::Mesh sparse;
    for (int i = 0; i < 200; ++i) {
        sparse.positions.insert(sparse.positions.end(), {float(i), 0.0f, float(i % 3)});
        sparse.normals.insert(sparse.normals.end(), {0.0f, 0.0f, 1.0f});
    }
    sparse.indices = {0, 100, 199, 199, 100, 50};
    sparse.faces.push_back({kernel::SubId{kernel::SubKind::Face, 0}, 0, 6});
    const auto sparseTable = rd::piecesOf(share(sparse));
    const rd::MeshPiece& s = *sparseTable->pieces[0];
    CHECK(s.vertexCount() == 4);
    const rd::PackedPiece& sp = s.packed();
    CHECK(sp.indices == std::vector<std::uint32_t>{0, 2, 3, 3, 2, 1});
    float x = 0;
    std::memcpy(&x, sp.vertices.data() + 3 * 28, 4);
    CHECK(x == 199.0f);
    CHECK(s.bounds().max.z == 2.0);
}

TEST_CASE("stock marks and the uncut box reach the shader block", "[render][stock]")
{
    StockShading shading;
    shading.uncut = Box3{{0, 0, -15}, {60, 40, 0}};
    for (int i = 0; i < kMaxStockMarks + 3; ++i)
        shading.marks.push_back(
            {Box3{{double(i), 0, 0}, {double(i) + 1, 1, 1}}, i == 1 ? Token::SimGouge : Token::Accent});
    const rd::StockUniforms u = rd::stockUniforms(shading, std::nullopt);
    CHECK(u.stock[0] == 1.0f);
    CHECK(u.stock[1] == float(kMaxStockMarks)); // the rest are ignored
    CHECK(u.stock[2] >= 1e-3f);
    CHECK(u.stock[2] < 1e-2f);
    CHECK(u.uncutMin[3] == 1.0f);
    CHECK(u.uncutMax[1] == 40.0f);
    CHECK(u.markMin[1][3] == float(int(rd::StockMarkSlot::Gouge)));
    CHECK(u.markMin[2][3] == float(int(rd::StockMarkSlot::Collision))); // not a simulation token: as SimCollision
    CHECK(u.markMax[7][0] == 8.0f);
    const rd::StockUniforms none = rd::stockUniforms(StockShading{}, Box3{{0, 0, 0}, {1000, 1000, 1000}});
    CHECK(none.uncutMin[3] == 0.0f);
    CHECK(none.stock[1] == 0.0f);
    CHECK(none.stock[2] > 1e-2f); // the tolerance grows with the stock
    CHECK(rd::stockMarkToken(rd::StockMarkSlot::Leftover) == Token::SimLeftover);
}

TEST_CASE("stock colours stand apart in every shipped theme", "[render][stock][theme]")
{
    // Uncut stock, cut faces and collision marks must be told apart at a glance, also from the part (ModelFace), the
    // leftover and selection colours, and from the viewport background. CIELAB distance (dE76) between the tokens.
    for (const char* name : {"graphite-dark", "graphite-light", "print/setupsheet-print"}) {
        const auto t = theme(name);
        const auto c = [&](Token k) {
            return t->colors[std::size_t(k)];
        };
        INFO("theme " << name);
        const double cutUncut = deltaE(c(Token::StockCutFace), c(Token::StockFace));
        const double collisionUncut = deltaE(c(Token::SimCollision), c(Token::StockFace));
        const double collisionCut = deltaE(c(Token::SimCollision), c(Token::StockCutFace));
        const double cutModel = deltaE(c(Token::StockCutFace), c(Token::ModelFace));
        const double cutLeftover = deltaE(c(Token::StockCutFace), c(Token::SimLeftover));
        const double cutSelection = deltaE(c(Token::StockCutFace), c(Token::Selection));
        const double cutBg = std::min(deltaE(c(Token::StockCutFace), c(Token::ViewportBgTop)),
                                      deltaE(c(Token::StockCutFace), c(Token::ViewportBgBottom)));
        std::cout << "[stock] " << name << ": dE cut/uncut " << cutUncut << ", collision/uncut " << collisionUncut
                  << ", collision/cut " << collisionCut << ", cut/model " << cutModel << ", cut/leftover "
                  << cutLeftover << ", cut/selection " << cutSelection << ", cut/background " << cutBg
                  << "; contrast cut/uncut " << contrastRatio(c(Token::StockCutFace), c(Token::StockFace))
                  << ", collision/cut " << contrastRatio(c(Token::SimCollision), c(Token::StockCutFace)) << "\n";
        CHECK(cutUncut >= 25);
        CHECK(collisionUncut >= 40);
        CHECK(collisionCut >= 40);
        CHECK(cutModel >= 10);
        CHECK(cutLeftover >= 25);
        CHECK(cutSelection >= 25);
        CHECK(cutBg >= 20);
        // Light and dark both: some luminance step between cut and uncut, not hue alone.
        CHECK(contrastRatio(c(Token::StockCutFace), c(Token::StockFace)) >= 1.2);
    }
}

// ---------- GPU ----------
TEST_CASE("updating a dirty region uploads only that region", "[render][stock][upload]")
{
    // PLAN.md 12 T-073 acceptance. The benchmark stock (6 x 4 x 1 in) at 0.2 mm: 12 x 8 tiles of 12.8 mm. One 20 mm
    // move of a 12.7 mm end mill, the dirty mesh the model returns for it, and the bytes the viewport hands the GPU.
    const Box3 box{{0, 0, 0}, {152.4, 101.6, 25.4}};
    auto model = blockStock(box, 0.2);
    model->cut(flatTool(12.7), {20, 20, 30}, {20, 20, 20}, false); // something cut before
    const std::uint32_t budget = 2'000'000;
    Box3 all{};
    const auto full = share(model->toMesh(0, &all, budget));
    std::uint64_t fullRanges = 0;
    const std::uint64_t fullBytes = pieceBytes(full, &fullRanges);
    std::uint32_t shown = model->version();

    auto vp = viewportOrSkip(320, 240);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    const ItemId stock = vp->addStock(full, Xform::identity(), stockStyle(), StockShading{box, {}});
    REQUIRE(stock != 0);
    vp->setCamera(standardViewCamera(StandardView::Iso, box, 320.0 / 240, true));
    REQUIRE(vp->renderNow().has_value());
    const ViewportInfo before = vp->info();
    CHECK(before.stockUploadBytes == fullBytes);
    CHECK(before.stockTileUploads == fullRanges);
    CHECK(fullRanges == 12 * 8);
    CHECK(vp->triangleCount(stock) == full->indices.size() / 3);
    CHECK(vp->lastFrameStats().triangles >= full->indices.size() / 3);

    model->cut(flatTool(12.7), {60, 50, 21}, {80, 50, 21}, false); // the one move: 20 mm, 4.4 mm deep
    Box3 dirty{};
    const auto patch = share(model->toMesh(shown, &dirty, budget));
    shown = model->version();
    std::uint64_t patchRanges = 0;
    const std::uint64_t patchBytes = pieceBytes(patch, &patchRanges);
    REQUIRE(patchRanges > 0);
    vp->updateMesh(stock, patch, dirty);
    REQUIRE(vp->renderNow().has_value());
    const ViewportInfo after = vp->info();
    const std::uint64_t uploaded = after.stockUploadBytes - before.stockUploadBytes;
    std::cout << "[stock] 0.2 mm, " << fullRanges << " tiles: full upload " << fullBytes << " bytes ("
              << full->indices.size() / 3 << " triangles); one 20 mm move: " << patchRanges << " tiles, " << uploaded
              << " bytes (" << patch->indices.size() / 3 << " triangles), " << 100.0 * double(uploaded) / fullBytes
              << " % of the full upload\n";
    CHECK(uploaded == patchBytes);                                          // exactly the tiles of the dirty mesh
    CHECK(after.stockTileUploads - before.stockTileUploads == patchRanges); // and no other tile
    CHECK(after.stockRepacks == 0);
    CHECK(uploaded * 10 < fullBytes);
    // Drawing again, moving the camera or capturing uploads nothing.
    vp->setCamera(standardViewCamera(StandardView::Top, box, 320.0 / 240, false));
    REQUIRE(vp->renderNow().has_value());
    static_cast<void>(capture(*vp, 160, 120));
    CHECK(vp->info().stockUploadBytes == after.stockUploadBytes);
    // "Nothing changed": the empty mesh and empty box toMesh returns then.
    Box3 none{};
    const auto empty = share(model->toMesh(shown, &none, budget));
    CHECK(empty->faces.empty());
    vp->updateMesh(stock, empty, none);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().stockUploadBytes == after.stockUploadBytes);
}

TEST_CASE("an incrementally updated stock looks like the stock meshed afresh", "[render][stock]")
{
    constexpr int w = 480, h = 360;
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            auto model = blockStock(kDemo, kDemoCell);
            auto vp = viewportOrSkip(w, h, software);
            vp->setTheme(theme("graphite-dark"));
            vp->setPreset(presetFor(Tier::Medium));
            const ItemId stock = vp->addStock(share(model->toMesh(0, nullptr, 50'000'000)), Xform::identity(),
                                              stockStyle(), StockShading{kDemo, {}});
            const Camera cam = standardViewCamera(StandardView::Iso, kDemo, double(w) / h, true);
            vp->setCamera(cam);
            REQUIRE(vp->renderNow().has_value());
            std::uint32_t shown = model->version();
            std::uint64_t uploads = vp->info().stockTileUploads;
            for (int step = 1; step <= 5; ++step) {
                cutStep(*model, step);
                Box3 dirty{};
                const auto patch = share(model->toMesh(shown, &dirty, 50'000'000));
                shown = model->version();
                vp->updateMesh(stock, patch, dirty);
                REQUIRE(vp->renderNow().has_value());
                const std::uint64_t now = vp->info().stockTileUploads;
                INFO("step " << step);
                CHECK(now - uploads ==
                      patch->faces.size() -
                          std::size_t(std::count_if(patch->faces.begin(), patch->faces.end(),
                                                    [](const kernel::FaceRange& f) { return f.indexCount == 0; })));
                CHECK(patch->faces.size() < 12); // a dirty region, not the whole stock
                uploads = now;
            }
            const Image incremental = capture(*vp, w, h);
            auto fresh = viewportOrSkip(w, h, software);
            fresh->setTheme(theme("graphite-dark"));
            fresh->setPreset(presetFor(Tier::Medium));
            fresh->addStock(share(model->toMesh(0, nullptr, 50'000'000)), Xform::identity(), stockStyle(),
                            StockShading{kDemo, {}});
            fresh->setCamera(cam);
            const Image afresh = capture(*fresh, w, h);
            const int differing = differingPixels(incremental, afresh, 24, tempPng("stock_incremental_diff"));
            std::cout << "[stock] incremental against fresh (" << (software ? "software" : "default")
                      << "): " << differing << " pixels differ\n";
            // The default adapter: the run-to-run noise of the NVIDIA adapter at creases (see the seam tests).
            CHECK(differing <= (software ? 4 : 16));
        }
    }
}

namespace {

// The views of the seam tests: every side, close up on tile edges, capped sections through a tile and exactly on a
// seam in x and in y (also with SectionPlane::cap false: a stock is capped anyway), and see-through.
struct SeamView {
    std::string name;
    Camera camera;
    std::optional<SectionPlane> section;
    float opacity = 1.0f;
};
std::vector<SeamView> seamViews(double aspect)
{
    const Vec3 c = centreOf(kDemo);
    return {
        {"iso", standardViewCamera(StandardView::Iso, kDemo, aspect, true), std::nullopt},
        {"top", standardViewCamera(StandardView::Top, kDemo, aspect, false), std::nullopt},
        {"grazing", lookAt(c, 200, 6, 130), std::nullopt},
        {"corner", lookAt({32, 16, -3}, 230, 35, 28), std::nullopt},
        {"seam_closeup", lookAt({16, 16, -4}, 210, 30, 20), std::nullopt},
        {"below", lookAt(c, 120, -35, 130), std::nullopt},
        {"section_x30", lookAt({30, 20, -7}, 30, 25, 110), SectionPlane{{30, 0, 0}, {1, 0, 0}, true}},
        {"section_seam_x32", lookAt({32, 20, -7}, 30, 25, 110), SectionPlane{{kTileMm * 2, 0, 0}, {1, 0, 0}, true}},
        {"section_seam_y16", lookAt({30, 16, -7}, 110, 30, 110), SectionPlane{{0, kTileMm, 0}, {0, 1, 0}, true}},
        {"section_seam_uncapped", lookAt({32, 20, -7}, 30, 25, 110),
         SectionPlane{{kTileMm * 2, 0, 0}, {1, 0, 0}, false}},
        {"see_through", standardViewCamera(StandardView::Iso, kDemo, aspect, true), std::nullopt, 0.5f},
    };
}

// Sets an environment variable of this process for the lifetime of the object (the renderer's test hooks).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) { put(value); }
    ~ScopedEnv() { put(""); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void put(const char* value)
    {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        if (*value)
            setenv(name_.c_str(), value, 1);
        else
            unsetenv(name_.c_str());
#endif
    }
    std::string name_;
};

// Pixels with any magenta in them (the test hook's colour, mixed into even one MSAA sample): green clearly below both
// red and blue. The stock's own colours (sand, steel blue, their shades) all have green between red and blue.
int magentaPixels(const Image& img)
{
    int n = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const auto c = rgbAt(img, x, y);
            n += c[1] + 12 < std::min(c[0], c[2]) ? 1 : 0;
        }
    }
    return n;
}

} // namespace

TEST_CASE("the walls inside the stock never show", "[render][stock][seams]")
{
    // Each tile of IStockModel::toMesh is closed by walls where its edge cuts material; neighbouring tiles' walls lie
    // back to back inside the stock. The renderer's test hook RAY_RENDER_SHOW_STOCK_WALLS paints every triangle it
    // counts as such a wall (stock.h PackedPiece's third part) magenta: in a stock cut so that no REAL wall falls on a
    // tile edge (flat floors across the seams, straight walls crossing them), not one pixel may show magenta from any
    // side, close up or seen through, and at most two single pixels in a section (through a tile or exactly on a seam).
    // Before the depth push (stock.h "DEPTH") these walls drew lines of hundreds of pixels along the seams wherever
    // they met an outer face, and before the parts were drawn apart, single MSAA samples along the seams on the NVIDIA
    // adapter.
    auto model = blockStock(kDemo, kDemoCell);
    {
        // A pocket 24 mm square, floor at -4 across the seams x = 16 and y = 16: a loop round the boundary (straight
        // walls at x = 4, 28 and y = 4, 28 cross the seams; the rounded corners lie clear of them) and a raster.
        std::vector<sim::IStockModel::CutMove> pocket = {line({8, 8, -4}, {24, 8, -4}), line({24, 8, -4}, {24, 24, -4}),
                                                         line({24, 24, -4}, {8, 24, -4}),
                                                         line({8, 24, -4}, {8, 8, -4})};
        for (double y = 12; y <= 20.01; y += 4)
            pocket.push_back(line({8, y, -4}, {24, y, -4}));
        model->cutBatch(flatTool(8), pocket, {});
        model->cut(flatTool(6), {36, 10, -3}, {56, 10, -3}, false);  // a slot across the seam x = 48
        model->cut(flatTool(10), {62, -5, -6}, {62, 45, -6}, false); // a step on the right
    }
    const auto mesh = share(model->toMesh(0, nullptr, 100'000'000));
    REQUIRE(mesh->faces.size() == 12);
    constexpr int w = 480, h = 360;
    const ScopedEnv hook("RAY_RENDER_SHOW_STOCK_WALLS", "1");
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            for (const SeamView& v : seamViews(double(w) / h)) {
                auto vp = viewportOrSkip(w, h, software);
                vp->setTheme(theme("graphite-dark"));
                vp->setPreset(presetFor(Tier::Medium));
                MeshStyle style = stockStyle();
                style.opacity = v.opacity;
                REQUIRE(vp->addStock(mesh, Xform::identity(), style, StockShading{kDemo, {}}) != 0);
                vp->setCamera(v.camera);
                vp->setSection(v.section);
                const Image img = capture(*vp, w, h);
                const int magenta = magentaPixels(img);
                std::cout << "[stock] walls inside, " << (software ? "software " : "default ") << v.name << ": "
                          << magenta << " pixels show one\n";
                INFO(v.name << ": " << magenta << " pixels show a wall inside the stock; image "
                            << tempPng("stock_walls_" + v.name));
                if (magenta > 0)
                    static_cast<void>(writePng(tempPng("stock_walls_" + v.name), img));
                // In a section, a single pixel can catch one where the plane, an outer face and a wall inside meet
                // (seen: one pixel in each section view, the same on both rasterisers); never a line.
                CHECK(magenta <= (v.section ? 2 : 0));
                if (v.section)
                    CHECK(vp->info().sectionCaps == 1); // a stock is capped, whatever the section asks
            }
        }
    }
    // The hook itself works: the first tile alone, whose walls on the seams x = 16 and y = 16 nothing hides now.
    kernel::Mesh first = *mesh;
    first.faces.resize(1);
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    REQUIRE(vp->addStock(share(std::move(first)), Xform::identity(), stockStyle(), StockShading{kDemo, {}}) != 0);
    vp->setCamera(lookAt({8, 8, -7}, 45, 30, 60));
    const int shown = magentaPixels(capture(*vp, w, h));
    INFO("the first tile's own walls on the seams: " << shown << " magenta pixels");
    CHECK(shown > 1000);
}

TEST_CASE("the stock in tiles looks like the same stock in one piece", "[render][stock][seams]")
{
    // The same stock as ONE solid (toSolid: the same surface at full resolution, without the walls that close each
    // tile) is the reference: drawn alike (both as a stock, facet normals on both, since the two triangulations
    // differ), the pictures must agree but for rasterisation noise at creases, where two real faces meet and the draw
    // order settles a tie (the step's corner: about 11 pixels here). The default adapter is held to a looser count:
    // the NVIDIA adapter of the development machine draws even the single solid differently from run to run (up to
    // about 70 of 960,000 pixels at 1200 x 800 under MSAA), the software rasteriser never does.
    auto model = demoStock();
    const auto tiled = share(withFacetNormals(model->toMesh(0, nullptr, 100'000'000)));
    const auto solid = share(solidMesh(model->toSolid()));
    REQUIRE(tiled->faces.size() == 12);
    constexpr int w = 480, h = 360;
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            for (const char* themeName : kThemes) {
                for (const SeamView& v : seamViews(double(w) / h)) {
                    Image img[2];
                    for (int which = 0; which < 2; ++which) {
                        auto vp = viewportOrSkip(w, h, software);
                        vp->setTheme(theme(themeName));
                        vp->setPreset(presetFor(Tier::Medium));
                        MeshStyle style = stockStyle();
                        style.opacity = v.opacity;
                        REQUIRE(vp->addStock(which == 0 ? tiled : solid, Xform::identity(), style,
                                             StockShading{kDemo, {}}) != 0);
                        vp->setCamera(v.camera);
                        vp->setSection(v.section);
                        img[which] = capture(*vp, w, h);
                    }
                    const std::string name = "stock_seams_" + v.name + themeSuffix(themeName);
                    const int differing = differingPixels(img[0], img[1], 24, tempPng(name + "_diff"));
                    std::cout << "[stock] tiles against one piece, " << (software ? "software " : "default ") << v.name
                              << themeSuffix(themeName) << ": " << differing << " of " << w * h << " pixels differ\n";
                    INFO(name << ": " << differing << " pixels differ; diff image " << tempPng(name + "_diff"));
                    CHECK(differing <= (software ? 16 : 40));
                    if (differing > 16) {
                        static_cast<void>(writePng(tempPng(name + "_tiled"), img[0]));
                        static_cast<void>(writePng(tempPng(name + "_solid"), img[1]));
                    }
                }
            }
        }
    }
}

TEST_CASE("stock colours: uncut faces, cut faces and marks", "[render][stock][colour]")
{
    // From above (orthographic), the uncut top and the pocket floor face the same way, so they are shaded alike and
    // their pixels differ only by the colour the stock gives them. Each is compared with the stock drawn wholly in one
    // token (no uncut box: every face in MeshStyle::face).
    auto model = demoStock();
    const auto mesh = share(model->toMesh(0, nullptr, 50'000'000));
    constexpr int w = 360, h = 240;
    const Camera cam = standardViewCamera(StandardView::Top, kDemo, double(w) / h, false);
    const Box3 markBox{{9, 20, -16}, {29, 31, 1}}; // over the upper part of the pocket
    for (const char* themeName : kThemes) {
        INFO("theme " << themeName);
        const auto render = [&](MeshStyle style, StockShading shading) {
            auto vp = viewportOrSkip(w, h);
            vp->setTheme(theme(themeName));
            vp->setPreset(presetFor(Tier::Medium));
            REQUIRE(vp->addStock(mesh, Xform::identity(), style, std::move(shading)) != 0);
            vp->setCamera(cam);
            return capture(*vp, w, h);
        };
        const Image real = render(stockStyle(), StockShading{kDemo, {}});
        const Image marked = render(stockStyle(), StockShading{kDemo, {{markBox, Token::SimCollision}}});
        const Image allUncut = render(MeshStyle{Token::StockFace, Token::StockCutFace, 1.0f, false, false}, {});
        const Image allCut = render(MeshStyle{Token::StockCutFace, Token::StockCutFace, 1.0f, false, false}, {});
        const Image allCollision = render(MeshStyle{Token::SimCollision, Token::SimCollision, 1.0f, false, false}, {});
        const auto [tx, ty] = pixelOfTopView(cam, w, h, 45, 36);   // uncut top
        const auto [fx, fy] = pixelOfTopView(cam, w, h, 15, 12);   // pocket floor, outside the mark
        const auto [mx, my] = pixelOfTopView(cam, w, h, 20, 26);   // pocket floor, inside the mark
        const auto [sx, sy] = pixelOfTopView(cam, w, h, 58.5, 20); // the step's floor on the right (cut)
        for (const auto& [x, y] : {std::pair{tx, ty}, {fx, fy}, {mx, my}, {sx, sy}}) {
            REQUIRE(x > 0);
            REQUIRE(y > 0);
            REQUIRE(x < w - 1);
            REQUIRE(y < h - 1);
        }
        CHECK(maxChannelDiff(rgbAt(real, tx, ty), rgbAt(allUncut, tx, ty)) <= 2);
        CHECK(maxChannelDiff(rgbAt(real, fx, fy), rgbAt(allCut, fx, fy)) <= 2);
        CHECK(maxChannelDiff(rgbAt(real, sx, sy), rgbAt(allCut, sx, sy)) <= 2);
        CHECK(maxChannelDiff(rgbAt(real, fx, fy), rgbAt(allUncut, fx, fy)) >= 20);
        CHECK(maxChannelDiff(rgbAt(marked, mx, my), rgbAt(allCollision, mx, my)) <= 2);
        CHECK(maxChannelDiff(rgbAt(marked, mx, my), rgbAt(real, mx, my)) >= 20);
        CHECK(maxChannelDiff(rgbAt(marked, fx, fy), rgbAt(real, fx, fy)) <= 2); // outside the mark: as before
        CHECK(maxChannelDiff(rgbAt(marked, tx, ty), rgbAt(real, tx, ty)) <= 2);
    }
}

TEST_CASE("hiding, showing, recolouring and marking the stock upload nothing", "[render][stock]")
{
    auto model = blockStock(kDemo, kDemoCell);
    auto vp = viewportOrSkip(240, 180);
    vp->setTheme(theme("graphite-light"));
    vp->setPreset(presetFor(Tier::Medium));
    const ItemId stock = vp->addStock(share(model->toMesh(0, nullptr, 50'000'000)), Xform::identity(), stockStyle(),
                                      StockShading{kDemo, {}});
    vp->setCamera(standardViewCamera(StandardView::Iso, kDemo, 240.0 / 180, true));
    REQUIRE(vp->renderNow().has_value());
    const std::uint64_t base = vp->info().stockUploadBytes;
    const Image shownImage = capture(*vp, 240, 180);
    vp->setVisible(stock, false);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->lastFrameStats().triangles == 0);
    vp->setVisible(stock, true);
    vp->setStyle(stock, MeshStyle{Token::PathStale, Token::PathStale, 0.6f, false, false});
    vp->setStockShading(stock, StockShading{kDemo, {{Box3{{0, 0, -15}, {20, 20, 0}}, Token::SimGouge}}});
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().stockUploadBytes == base);
    const Image recoloured = capture(*vp, 240, 180);
    CHECK(differingPixels(recoloured, shownImage, 24) > 1000);
    vp->setStyle(stock, stockStyle());
    vp->setStockShading(stock, StockShading{kDemo, {}});
    // Updates while hidden wait until it is shown again.
    vp->setVisible(stock, false);
    std::uint32_t shown = model->version();
    cutStep(*model, 1);
    Box3 dirty{};
    const auto patch = share(model->toMesh(shown, &dirty, 50'000'000));
    vp->updateMesh(stock, patch, dirty);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().stockUploadBytes == base);
    vp->setVisible(stock, true);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().stockUploadBytes == base + pieceBytes(patch));
    // Removed: its buffers go.
    CHECK(vp->info().stockGpuBytes > 0);
    vp->remove(stock);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().stockGpuBytes == 0);
    CHECK(vp->triangleCount(stock) == 0);
}

TEST_CASE("a pick on the stock names the tile, and a highlight draws it", "[render][stock][pick]")
{
    auto model = demoStock();
    constexpr int w = 360, h = 240;
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    const ItemId stock = vp->addStock(share(model->toMesh(0, nullptr, 50'000'000)), Xform::identity(), stockStyle(),
                                      StockShading{kDemo, {}});
    const Camera cam = standardViewCamera(StandardView::Top, kDemo, double(w) / h, false);
    vp->setCamera(cam);
    const auto [px, py] = pixelOfTopView(cam, w, h, 40, 25); // tile (2, 1) = 2 + 1 * 4
    std::optional<std::optional<PickHit>> got;
    vp->pick(px, py, kFaces, [&got](std::optional<PickHit> hit) { got = hit; });
    REQUIRE(processEventsUntil([&got] { return got.has_value(); }, 30000));
    REQUIRE(got->has_value());
    const PickHit hit = **got;
    CHECK(hit.item == stock);
    CHECK(hit.kind == kernel::SubKind::Face);
    CHECK(hit.subIndex == 6u);
    CHECK(std::abs(hit.point.z) < 0.05);
    const Image plain = capture(*vp, w, h);
    const PickHit highlighted[1] = {hit};
    vp->setHighlight(highlighted, std::nullopt);
    const Image lit = capture(*vp, w, h);
    const auto [ix, iy] = pixelOfTopView(cam, w, h, 40, 25); // in the tile
    const auto [ox, oy] = pixelOfTopView(cam, w, h, 56, 38); // in another tile
    CHECK(maxChannelDiff(rgbAt(lit, ix, iy), rgbAt(plain, ix, iy)) >= 10);
    CHECK(maxChannelDiff(rgbAt(lit, ox, oy), rgbAt(plain, ox, oy)) <= 2);
}

TEST_CASE("updateMesh with a dirty box moves an ordinary mesh to tiled storage", "[render][stock]")
{
    // Any mesh item: the first update with a dirty box uploads it per face range once; later ones only their ranges.
    auto model = blockStock(kDemo, kDemoCell);
    constexpr int w = 320, h = 240;
    const MeshStyle style{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    const ItemId item = vp->addMesh(share(model->toMesh(0, nullptr, 50'000'000)), Xform::identity(), style);
    const Camera cam = standardViewCamera(StandardView::Iso, kDemo, double(w) / h, true);
    vp->setCamera(cam);
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().stockTileUploads == 0);
    std::uint32_t shown = model->version();
    for (int step = 1; step <= 3; ++step) {
        cutStep(*model, step);
        Box3 dirty{};
        const auto patch = share(model->toMesh(shown, &dirty, 50'000'000));
        shown = model->version();
        const std::uint64_t before = vp->info().stockTileUploads;
        vp->updateMesh(item, patch, dirty);
        REQUIRE(vp->renderNow().has_value());
        const std::uint64_t uploaded = vp->info().stockTileUploads - before;
        INFO("step " << step);
        if (step == 1)
            CHECK(uploaded == 12); // the move to tiled storage: every range once
        else
            CHECK(uploaded == patch->faces.size());
    }
    const Image patched = capture(*vp, w, h);
    auto fresh = viewportOrSkip(w, h);
    fresh->setTheme(theme("graphite-dark"));
    fresh->setPreset(presetFor(Tier::Medium));
    fresh->addMesh(share(model->toMesh(0, nullptr, 50'000'000)), Xform::identity(), style);
    fresh->setCamera(cam);
    const Image afresh = capture(*fresh, w, h);
    CHECK(differingPixels(patched, afresh, 24, tempPng("stock_converted_diff")) <= 16); // adapter noise, as above
}

TEST_CASE("pages full of holes are repacked", "[render][stock]")
{
    // Pieces that change size move out of their places. While the pages hold no more than twice what is in use the
    // holes are filled by later pieces; beyond that (and above a few MB) every piece goes into new pages. Drawn the
    // same as a stock shown afresh.
    std::vector<std::uint32_t> faces;
    for (std::uint32_t f = 0; f < 150; ++f)
        faces.push_back(f);
    const Box3 all{{-1, -1, -1}, {301, 11, 1}};
    const Camera cam = standardViewCamera(StandardView::Top, Box3{{0, 0, 0}, {300, 10, 1}}, 200.0 / 150, false);
    auto vp = viewportOrSkip(200, 150);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    const ItemId item =
        vp->addStock(quadFaces(faces, std::vector<int>(faces.size(), 1000)), Xform::identity(), stockStyle(), {});
    vp->setCamera(cam);
    REQUIRE(vp->renderNow().has_value());
    const std::uint64_t big = vp->info().stockGpuBytes;
    // Every other face grows: their old places are holes too small for them, so they go to a new page.
    std::vector<int> sizes(faces.size(), 1000);
    for (std::size_t f = 1; f < faces.size(); f += 2)
        sizes[f] = 1100;
    vp->updateMesh(item, quadFaces(faces, sizes), all);
    REQUIRE(vp->renderNow().has_value());
    std::cout << "[stock] repack: " << big << " GPU bytes at first, " << vp->info().stockGpuBytes
              << " after half the faces grew (" << vp->info().stockDrawCalls << " draws)\n";
    CHECK(vp->info().stockRepacks == 0);
    CHECK(vp->info().stockGpuBytes > big);
    // Every face shrinks to a tenth: the pages would be nearly empty, so they are replaced.
    std::fill(sizes.begin(), sizes.end(), 100);
    vp->updateMesh(item, quadFaces(faces, sizes), all);
    REQUIRE(vp->renderNow().has_value());
    const ViewportInfo info = vp->info();
    std::cout << "[stock] repack: " << info.stockGpuBytes << " GPU bytes after every face shrank, " << info.stockRepacks
              << " repack, " << info.stockDrawCalls << " draws\n";
    CHECK(info.stockRepacks == 1);
    CHECK(info.stockGpuBytes * 5 < big);
    CHECK(info.stockDrawCalls == 1); // one page, every piece one after the other
    CHECK(vp->triangleCount(item) == 150u * 200u);
    const Image after = capture(*vp, 200, 150);
    auto fresh = viewportOrSkip(200, 150);
    fresh->setTheme(theme("graphite-dark"));
    fresh->setPreset(presetFor(Tier::Medium));
    fresh->addStock(quadFaces(faces, sizes), Xform::identity(), stockStyle(), {});
    fresh->setCamera(cam);
    CHECK(differingPixels(after, capture(*fresh, 200, 150), 24) <= 16); // adapter noise, as above
}

// ---------- reference images ----------
namespace {

// "stock": the demo stock from the iso view with a collision mark over the hole on the tile corner; "stock_section":
// the same in a section through the pocket, the hole and the slot (capped). The mesh's own normals (smoothed tops).
void buildStockScene(const std::string& name, IRenderViewport& vp, double aspect)
{
    static const auto mesh = share(demoStock()->toMesh(0, nullptr, 50'000'000));
    StockShading shading{kDemo, {{Box3{{28, 12, -16}, {36, 20, 1}}, Token::SimCollision}}};
    REQUIRE(vp.addStock(mesh, Xform::identity(), stockStyle(), shading) != 0);
    if (name == "stock") {
        vp.setCamera(standardViewCamera(StandardView::Iso, kDemo, aspect, true));
    } else {
        REQUIRE(name == "stock_section");
        vp.setCamera(lookAt({24, 20, -7}, 20, 30, 105));
        vp.setSection(SectionPlane{{24, 0, 0}, {1, 0, 0}, true});
    }
}

} // namespace

TEST_CASE("stock reference images in both themes", "[render][stock][reference]")
{
    const std::vector<std::string> names = {"stock", "stock_section"};
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            for (const char* themeName : kThemes) {
                const auto t = theme(themeName);
                auto bgView = viewportOrSkip(kRefWidth, kRefHeight, software);
                bgView->setTheme(t);
                const Image background = capture(*bgView, kRefWidth, kRefHeight);
                std::map<std::string, Image> rendered;
                for (const std::string& scene : names) {
                    const std::string name = scene + themeSuffix(themeName);
                    auto vp = viewportOrSkip(kRefWidth, kRefHeight, software);
                    vp->setTheme(t);
                    vp->setPreset(presetFor(Tier::Medium));
                    buildStockScene(scene, *vp, double(kRefWidth) / kRefHeight);
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
                    std::cout << "[stock] " << name << " (" << (software ? "software" : "default")
                              << "): tile colour worst " << c.worstBlockColour << " mean " << c.meanBlockColour
                              << ", tile coverage worst " << c.worstBlockCoverage << ", coverage " << c.coverageA
                              << " vs " << c.coverageB << "\n";
                    const std::string actualPath = tempPng(name + "_actual");
                    if (!matches(c))
                        static_cast<void>(writePng(actualPath, actual));
                    INFO("this run's image: " << actualPath);
                    CHECK(c.coverageA > 0.1);
                    CHECK(matches(c));
                }
                if (updating() && software)
                    continue;
                // The comparison tells the scenes and the themes apart.
                auto other = readPng(referencePath("stock_section" + themeSuffix(themeName)));
                REQUIRE(other.has_value());
                CHECK_FALSE(matches(compare(rendered["stock" + themeSuffix(themeName)], *other, background)));
                const char* otherTheme = std::string(themeName) == "graphite-dark" ? "graphite-light" : "graphite-dark";
                auto otherThemed = readPng(referencePath("stock" + themeSuffix(otherTheme)));
                REQUIRE(otherThemed.has_value());
                CHECK_FALSE(matches(compare(rendered["stock" + themeSuffix(themeName)], *otherThemed, background)));
            }
        }
    }
}

TEST_CASE("stock screenshots for review", "[.shots]")
{
    // RAY_T073_SHOTS=<folder>: larger pictures of the stock for a person to look at (not compared with anything).
    const char* dir = std::getenv("RAY_T073_SHOTS");
    if (!dir || !*dir)
        SKIP("RAY_T073_SHOTS is not set");
    std::filesystem::create_directories(dir);
    constexpr int w = 1200, h = 800;
    const double aspect = double(w) / h;
    auto model = demoStock();
    const auto mesh = share(model->toMesh(0, nullptr, 50'000'000));
    const StockShading marked{kDemo, {{Box3{{28, 12, -16}, {36, 20, 1}}, Token::SimCollision}}};
    const auto shot = [&](const std::string& name, const char* themeName,
                          const std::function<void(IOffscreenViewport&)>& build) {
        auto vp = viewportOrSkip(w, h);
        vp->setTheme(theme(themeName));
        vp->setPreset(presetFor(Tier::Medium));
        build(*vp);
        const Image img = capture(*vp, w, h);
        const std::string path =
            (std::filesystem::path(dir) / ("t073_" + name + themeSuffix(themeName) + ".png")).string();
        REQUIRE(writePng(path, img).has_value());
        std::cout << "[stock] wrote " << path << "\n";
    };
    for (const char* t : kThemes) {
        shot("stock_iso", t, [&](IOffscreenViewport& vp) {
            vp.addStock(mesh, Xform::identity(), stockStyle(), marked);
            vp.setCamera(standardViewCamera(StandardView::Iso, kDemo, aspect, true));
        });
        shot("stock_section_seam", t, [&](IOffscreenViewport& vp) {
            vp.addStock(mesh, Xform::identity(), stockStyle(), StockShading{kDemo, {}});
            vp.setCamera(lookAt({32, 20, -7}, 30, 25, 100));
            vp.setSection(SectionPlane{{kTileMm * 2, 0, 0}, {1, 0, 0}, true});
        });
        shot("stock_corner_closeup", t, [&](IOffscreenViewport& vp) {
            vp.addStock(mesh, Xform::identity(), stockStyle(), StockShading{kDemo, {}});
            vp.setCamera(lookAt({32, 16, -3}, 230, 35, 28));
        });
        shot("stock_see_through", t, [&](IOffscreenViewport& vp) {
            MeshStyle s = stockStyle();
            s.opacity = 0.45f;
            vp.addStock(mesh, Xform::identity(), s, StockShading{kDemo, {}});
            vp.addMesh(meshOf(kernel::makeCylinder(Vec3{32, 16, -15}, Vec3{0, 0, 1}, 1.5, 15)), Xform::identity(),
                       MeshStyle{Token::ModelFace, Token::ModelEdge, 1.0f, true, false});
            vp.setCamera(standardViewCamera(StandardView::Iso, kDemo, aspect, true));
        });
    }
    // Mixed detail: the whole benchmark-like stock coarse, one move's tiles at full resolution beside them.
    const Box3 big{{0, 0, 0}, {152.4, 101.6, 25.4}};
    auto bench = blockStock(big, 0.1);
    for (int i = 0; i < 6; ++i)
        bench->cut(flatTool(12.7), {10, 10.0 + 14 * i, 22}, {140, 10.0 + 14 * i, 22}, false);
    bench->cut(ballTool(6.35), {20, 20, 21}, {130, 80, 18}, false);
    const auto coarse = share(bench->toMesh(0, nullptr, 400'000));
    std::uint32_t shown = bench->version();
    bench->cut(flatTool(12.7), {60, 50, 19}, {80, 50, 19}, false);
    Box3 dirty{};
    const auto fine = share(bench->toMesh(shown, &dirty, 2'000'000));
    for (const char* t : kThemes) {
        shot("stock_mixed_detail", t, [&](IOffscreenViewport& vp) {
            const ItemId id = vp.addStock(coarse, Xform::identity(), stockStyle(), StockShading{big, {}});
            vp.updateMesh(id, fine, dirty);
            vp.setCamera(lookAt({70, 50, 19}, 240, 40, 120));
        });
    }
}

// ---------- cost ----------
TEST_CASE("a 2 M-triangle stock: one-move upload against a full upload, and the frame time",
          "[render][stock][cost][wallclock]")
{
    // The benchmark stock (6 x 4 x 1 in) at 0.05 mm (High tier), cut by a facing pass, a pocket and ball scallops,
    // shown under a 2 M-triangle budget (columns merged until it fits); then one 20 mm move and its dirty region, at
    // the same budget (so its tiles come back finer than their neighbours). Frame time at 1920x1080, Medium preset,
    // orbiting, against the same triangles as one plain mesh: what the tiles cost in draws.
    const Box3 box{{0, 0, 0}, {152.4, 101.6, 25.4}};
    auto model = blockStock(box, 0.05);
    const auto cutStart = std::chrono::steady_clock::now();
    {
        std::vector<sim::IStockModel::CutMove> facing, pocket, scallops;
        for (double y = -2; y <= 104; y += 9)
            facing.push_back(line({-10, y, 24.4}, {165, y, 24.4}));
        for (double y = 20; y <= 80; y += 6)
            pocket.push_back(line({20, y, 18}, {80, y, 18}));
        for (double y = 15; y <= 85; y += 1.5)
            scallops.push_back(
                line({95, y, 20.0 + 2.0 * std::sin(y * 0.2)}, {145, y, 20.0 + 2.0 * std::cos(y * 0.15)}));
        model->cutBatch(flatTool(12.7), facing, {});
        model->cutBatch(flatTool(12.7), pocket, {});
        model->cutBatch(ballTool(6.35), scallops, {});
    }
    std::cout << "[stock] cost: cutting took "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cutStart).count()
              << " ms\n";
    const std::uint32_t budget = 2'000'000;
    const auto meshStart = std::chrono::steady_clock::now();
    const auto full = share(model->toMesh(0, nullptr, budget));
    const double meshMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - meshStart).count();
    std::uint32_t shown = model->version();
    model->cut(flatTool(12.7), {30, 95, 22}, {50, 95, 22}, false); // 20 mm, 2.4 mm deep, beside the pocket
    Box3 dirty{};
    const auto dirtyStart = std::chrono::steady_clock::now();
    const auto patch = share(model->toMesh(shown, &dirty, budget));
    const double dirtyMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - dirtyStart).count();
    std::cout << "[stock] cost: full mesh " << full->indices.size() / 3 << " triangles in " << full->faces.size()
              << " tiles (" << meshMs << " ms); one move's dirty region " << patch->indices.size() / 3
              << " triangles in " << patch->faces.size() << " tiles (" << dirtyMs << " ms)\n";
    // The same dirty region asked for at the display's level of detail: a budget in proportion to its share of the
    // tiles (what T-074 can do to keep the display under its budget, ray_render/viewport.h).
    Box3 dirtySameDetail{};
    const std::uint32_t proportional = std::uint32_t(1.25 * double(full->indices.size() / 3) *
                                                     double(patch->faces.size()) / double(full->faces.size()));
    const auto patchSameDetail =
        share(model->toMesh(shown, &dirtySameDetail, std::max<std::uint32_t>(proportional, 1)));
    std::cout << "[stock] cost: bytes to upload: full " << pieceBytes(full) << "; one move at full detail "
              << pieceBytes(patch) << " (" << 100.0 * double(pieceBytes(patch)) / double(pieceBytes(full))
              << " %); one move at the display's detail (budget " << proportional << ": "
              << patchSameDetail->indices.size() / 3 << " triangles) " << pieceBytes(patchSameDetail) << " ("
              << 100.0 * double(pieceBytes(patchSameDetail)) / double(pieceBytes(full)) << " %)\n";
    CHECK(pieceBytes(patchSameDetail) * 10 < pieceBytes(full));
    for (const bool software : {false, true}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            auto vp = viewportOrSkip(1920, 1080, software);
            auto plain = viewportOrSkip(1920, 1080, software);
            for (IOffscreenViewport* v : {vp.get(), plain.get()}) {
                v->setTheme(theme("graphite-dark"));
                v->setPreset(presetFor(Tier::Medium));
            }
            const ItemId stock = vp->addStock(full, Xform::identity(), stockStyle(), StockShading{box, {}});
            Camera cam = standardViewCamera(StandardView::Iso, box, 16.0 / 9.0, true);
            vp->setCamera(cam);
            auto start = std::chrono::steady_clock::now();
            REQUIRE(vp->renderNow().has_value());
            const double fullUploadMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const ViewportInfo first = vp->info();
            vp->updateMesh(stock, patch, dirty);
            start = std::chrono::steady_clock::now();
            REQUIRE(vp->renderNow().has_value());
            const double patchUploadMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const ViewportInfo second = vp->info();
            const std::uint64_t oneMove = second.stockUploadBytes - first.stockUploadBytes;
            CHECK(oneMove == pieceBytes(patch));
            CHECK(second.stockRepacks == 0);
            std::cout << "[stock] cost (" << second.backend << " on " << second.device << "): full upload "
                      << first.stockUploadBytes << " bytes (frame with it " << fullUploadMs << " ms); one move "
                      << oneMove << " bytes, " << 100.0 * double(oneMove) / double(first.stockUploadBytes)
                      << " % (frame with it " << patchUploadMs << " ms); GPU buffers " << second.stockGpuBytes
                      << " bytes; shown " << vp->triangleCount(stock) << " triangles\n";
            // For scale: the stock after the move meshed afresh as one plain mesh (one draw) under the same number of
            // triangles, which toMesh fills at one level of detail (fewer triangles than the mixed display).
            const auto onePiece = share(model->toMesh(0, nullptr, std::uint32_t(vp->triangleCount(stock))));
            plain->addMesh(onePiece, Xform::identity(),
                           MeshStyle{Token::StockFace, Token::StockCutFace, 1.0f, false, false});
            plain->setCamera(cam);
            REQUIRE(plain->renderNow().has_value());
            std::vector<double> stockMs, plainMs;
            std::uint64_t draws = 0, triangles = 0;
            for (int i = 0; i < 24; ++i) {
                cam = orbitCamera(cam, 2, 0, cam.target);
                vp->setCamera(cam);
                plain->setCamera(cam);
                REQUIRE(vp->renderNow().has_value());
                REQUIRE(plain->renderNow().has_value());
                if (i < 4)
                    continue;
                stockMs.push_back(vp->lastFrameStats().cpuMs);
                plainMs.push_back(plain->lastFrameStats().cpuMs);
                draws = vp->info().stockDrawCalls;
                triangles = vp->lastFrameStats().triangles;
            }
            std::sort(stockMs.begin(), stockMs.end());
            std::sort(plainMs.begin(), plainMs.end());
            std::cout << "[stock] cost (" << (software ? "software" : "default") << "): " << triangles
                      << " stock triangles in " << draws << " draws: frame median " << stockMs[stockMs.size() / 2]
                      << " ms, 95th percentile " << stockMs[stockMs.size() * 95 / 100] << " ms; one plain mesh of "
                      << onePiece->indices.size() / 3 << " triangles: median " << plainMs[plainMs.size() / 2]
                      << " ms\n";
            CHECK(oneMove > 0);
            CHECK(triangles >= 500'000);
            CHECK(vp->info().stockUploadBytes == second.stockUploadBytes); // orbiting uploads nothing
            // The whole stock at one finer level (a 3 M budget: columns of 4 cells here), for a frame time nearer
            // 2 M triangles and more.
            auto finer = viewportOrSkip(1920, 1080, software);
            finer->setTheme(theme("graphite-dark"));
            finer->setPreset(presetFor(Tier::Medium));
            const auto fine = share(model->toMesh(0, nullptr, 3'000'000));
            finer->addStock(fine, Xform::identity(), stockStyle(), StockShading{box, {}});
            Camera cam2 = standardViewCamera(StandardView::Iso, box, 16.0 / 9.0, true);
            finer->setCamera(cam2);
            REQUIRE(finer->renderNow().has_value());
            std::vector<double> fineMs;
            for (int i = 0; i < 24; ++i) {
                cam2 = orbitCamera(cam2, 2, 0, cam2.target);
                finer->setCamera(cam2);
                REQUIRE(finer->renderNow().has_value());
                if (i >= 4)
                    fineMs.push_back(finer->lastFrameStats().cpuMs);
            }
            std::sort(fineMs.begin(), fineMs.end());
            std::cout << "[stock] cost (" << (software ? "software" : "default") << "): " << fine->indices.size() / 3
                      << " triangles in " << fine->faces.size() << " tiles, " << finer->info().stockDrawCalls
                      << " draws: frame median " << fineMs[fineMs.size() / 2] << " ms, 95th percentile "
                      << fineMs[fineMs.size() * 95 / 100] << " ms; GPU buffers " << finer->info().stockGpuBytes
                      << " bytes\n";
        }
    }
}
