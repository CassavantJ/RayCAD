// Section view and draft analysis (task T-023, PLAN.md 13 T-023 acceptance), rendered offscreen:
//   - reference images of sectioned parts with and without a cap (a box with a through hole and the filleted body,
//     both cut through their holes so the inside shows), within the tile tolerance of viewport_reference_test.cpp;
//     a preset without section caps (Low) draws none even when asked
//   - no cap and no artefact on an open shell or a lone face; closed bodies are capped, hidden ones are not
//   - picks through the cut return the inner face, never a face that was cut away; a cap answers as its body
//   - draft colours match the angles of a test wedge and, across a curved face, the class boundary lies where the
//     computed angle crosses the threshold (within a few pixels); draft analysis overrides a body's own colour
//   - moving the plane re-renders without uploading any mesh again; idle renders nothing; section and draft
//     survive theme and preset switches; the glass backdrop is blurred again from the sectioned image
// To regenerate the reference images: RAY_UPDATE_RENDER_REFERENCES=1 ray_render_tests "[section][reference]".
#include "viewport_scenes.h"
#include "viewport_testing.h"

#include <section.h>
#include <shape_testing.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 120;
// The tolerances of viewport_reference_test.cpp.
constexpr double kMaxTileColour = 40.0;
constexpr double kMaxMeanColour = 3.0;
constexpr double kMaxTileCoverage = 0.30;
constexpr double kCoverageAbsolute = 0.002;
constexpr double kCoverageRelative = 0.08;

constexpr std::uint8_t kFaces = std::uint8_t(PickFilter::Faces);
constexpr std::uint8_t kBodies = std::uint8_t(PickFilter::Bodies);

const MeshStyle kModel{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};

std::string referencePath(const std::string& scene)
{
    return std::string(RAY_RENDER_REFERENCE_DIR) + "/" + scene + ".png";
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_RENDER_REFERENCES");
    return v && std::string(v) == "1";
}

std::unique_ptr<IOffscreenViewport> viewportOrSkip(int w, int h, bool software = false)
{
    std::string whyNot;
    auto vp = makeViewport(w, h, software, &whyNot);
    if (!vp)
        SKIP("no RHI backend could be created: " << whyNot);
    return vp;
}

bool within(const Comparison& c)
{
    return c.worstBlockColour <= kMaxTileColour && c.meanBlockColour <= kMaxMeanColour &&
           c.worstBlockCoverage <= kMaxTileCoverage &&
           std::abs(c.coverageA - c.coverageB) <= kCoverageAbsolute + kCoverageRelative * c.coverageB;
}

// A camera looking from `from` (a direction) at the box, framing it.
Camera lookingFrom(Vec3 from, const Box3& box, double aspect, bool perspective)
{
    const Vec3 c{(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2, (box.min.z + box.max.z) / 2};
    Camera cam{};
    cam.target = c;
    cam.eye = Vec3{c.x + from.x * 500, c.y + from.y * 500, c.z + from.z * 500};
    cam.up = Vec3{0, 0, 1};
    cam.perspective = perspective;
    cam.fovDeg = 30;
    cam.orthoHeight = 100;
    return fitCamera(cam, box, aspect);
}

struct SectionScene {
    std::string name;
    bool cap;
};

const std::vector<SectionScene>& sectionScenes()
{
    static const std::vector<SectionScene> list = {{"section_cut_body_cap", true},
                                                   {"section_cut_body_nocap", false},
                                                   {"section_filleted_cap", true},
                                                   {"section_filleted_nocap", false}};
    return list;
}

// The box with a through hole (100 x 60 x 20, radius 8 at (50, 30)) or the filleted body (80 x 50 x 20, radius 6
// holes at (20, 25) and (60, 25)), cut at the holes' axes with the +Y half removed, seen from the +Y side above.
void buildSectionScene(const std::string& name, IViewport& vp, bool cap, double aspect)
{
    const bool cutBody = name.rfind("section_cut_body", 0) == 0;
    const Box3 box = cutBody ? Box3{{0, 0, 0}, {100, 60, 20}} : Box3{{0, 0, 0}, {80, 50, 20}};
    vp.addMesh(meshOf(cutBody ? kernel::testing::makeCutBody() : kernel::testing::makeFilletedBody()),
               Xform::identity(), kModel);
    const double y = cutBody ? 30 : 25;
    vp.setSection(SectionPlane{Vec3{0, y, 0}, Vec3{0, 1, 0}, cap});
    // Framing what is left of the part.
    vp.setCamera(lookingFrom(Vec3{0.45, 1.0, 0.75}, Box3{box.min, Vec3{box.max.x, y, box.max.z}}, aspect, true));
}

// Closedness of the meshes for the caps is worked out on a worker (ViewportInfo::closedChecksPending); until then a
// body is drawn uncapped. Tests that look at caps wait for it.
void waitForClosedChecks(IViewport& vp)
{
    auto& rv = dynamic_cast<IRenderViewport&>(vp);
    REQUIRE(processEventsUntil([&] { return rv.info().closedChecksPending == 0; }, 30000));
    processEventsFor(1); // the wake-up the last check posted
}

Image draw(IOffscreenViewport& vp, int w = kWidth, int h = kHeight)
{
    waitForClosedChecks(vp);
    return capture(vp, w, h);
}

bool identical(const Image& a, const Image& b)
{
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}

// The mesh without the faces whose vertices all satisfy `drop` (an open shell).
std::shared_ptr<const kernel::Mesh> withoutFaces(const kernel::Mesh& in, const std::function<bool(Vec3)>& drop)
{
    kernel::Mesh out = in;
    out.indices.clear();
    out.faces.clear();
    for (const kernel::FaceRange& f : in.faces) {
        bool all = true;
        for (std::uint32_t i = 0; i < f.indexCount; ++i) {
            const std::uint32_t v = in.indices[f.firstIndex + i];
            all = all && drop(Vec3{in.positions[v * 3], in.positions[v * 3 + 1], in.positions[v * 3 + 2]});
        }
        if (all)
            continue;
        kernel::FaceRange g = f;
        g.firstIndex = std::uint32_t(out.indices.size());
        out.indices.insert(out.indices.end(), in.indices.begin() + f.firstIndex,
                           in.indices.begin() + f.firstIndex + f.indexCount);
        out.faces.push_back(g);
    }
    return std::make_shared<const kernel::Mesh>(std::move(out));
}

// Kernel index of the face whose vertices all satisfy `pred`; -1 if none.
std::int64_t faceWhere(const kernel::Mesh& m, const std::function<bool(Vec3)>& pred)
{
    for (const kernel::FaceRange& f : m.faces) {
        bool all = f.indexCount > 0;
        for (std::uint32_t i = 0; i < f.indexCount && all; ++i) {
            const std::uint32_t v = m.indices[f.firstIndex + i];
            all = pred(Vec3{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]});
        }
        if (all)
            return f.face.index;
    }
    return -1;
}

std::optional<PickHit> pickAt(IViewport& vp, int x, int y, std::uint8_t mask)
{
    std::optional<std::optional<PickHit>> got;
    vp.pick(x, y, mask, [&](std::optional<PickHit> h) { got = h; });
    REQUIRE(processEventsUntil([&] { return got.has_value(); }, 30000));
    return *got;
}

double srgbToLinear(int v)
{
    const double e = v / 255.0;
    return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}

// The draft class whose theme colour has the chromaticity closest to the pixel's (lighting scales all three
// channels alike, so chromaticity survives it).
DraftClass classOfPixel(const Image& img, int x, int y, const Theme& theme, double* distance = nullptr)
{
    const auto rgb = rgbAt(img, x, y);
    const double r = srgbToLinear(rgb[0]), g = srgbToLinear(rgb[1]), b = srgbToLinear(rgb[2]);
    const double s = std::max(r + g + b, 1e-6);
    DraftClass best = DraftClass::Vertical;
    double bestD = 1e9;
    for (const DraftClass c : {DraftClass::Positive, DraftClass::Vertical, DraftClass::Negative}) {
        const Color k = theme.colors[std::size_t(draftToken(c))];
        const double ks = std::max(double(k.r + k.g + k.b), 1e-6);
        const double d =
            std::hypot(r / s - k.r / ks, g / s - k.g / ks) + std::abs(b / s - k.b / ks); // chromaticity distance
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    if (distance)
        *distance = bestD;
    return best;
}

const char* className(DraftClass c)
{
    return c == DraftClass::Positive ? "positive" : c == DraftClass::Negative ? "negative" : "vertical";
}

} // namespace

// ---------- CPU half ----------
TEST_CASE("closed meshes are told from open shells and lone faces", "[render][section][unit]")
{
    using namespace kernel::testing;
    CHECK(ray::render::detail::isClosedMesh(*meshOf(kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 30, 20}))));
    CHECK(ray::render::detail::isClosedMesh(*meshOf(kernel::makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 15, 40))));
    CHECK(ray::render::detail::isClosedMesh(*meshOf(makeCutBody())));
    CHECK(ray::render::detail::isClosedMesh(*meshOf(makeFilletedBody())));
    CHECK(ray::render::detail::isClosedMesh(*meshOf(makeSphere(Vec3{0, 0, 0}, 10))));
    CHECK(ray::render::detail::isClosedMesh(*meshOf(makeTorus(20, 5))));
    // Open: a box without its top, a lone face, a body with a face BRepMesh left without triangles.
    const auto box = meshOf(kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 30, 20}));
    CHECK_FALSE(ray::render::detail::isClosedMesh(*withoutFaces(*box, [](Vec3 p) { return p.z > 19.99; })));
    CHECK_FALSE(ray::render::detail::isClosedMesh(*meshOf(makePlanarFace())));
    CHECK_FALSE(ray::render::detail::isClosedMesh(*meshOf(makeBodyWithUnmeshableFace())));
    CHECK_FALSE(ray::render::detail::isClosedMesh(kernel::Mesh{}));
}

TEST_CASE("the cap quad covers the cut of every visible body", "[render][section][unit]")
{
    const Box3 box{{0, 0, 0}, {100, 60, 20}};
    const auto plane = ray::render::detail::planeEquation(SectionPlane{Vec3{0, 30, 0}, Vec3{0, 2, 0}, true});
    REQUIRE(plane.has_value());
    CHECK(plane->ny == 1.0);
    CHECK(plane->distance(Vec3{5, 40, 5}) == 10.0);
    const auto quad = ray::render::detail::capQuad(*plane, box);
    REQUIRE(quad.has_value());
    for (const Vec3& p : *quad)
        CHECK(std::abs(plane->distance(p)) < 1e-9);
    // Every corner of the box's cross-section lies inside the quad (it spans x and z far beyond the box).
    double lo = 1e9, hi = -1e9;
    for (const Vec3& p : *quad) {
        lo = std::min({lo, p.x, p.z});
        hi = std::max({hi, p.x, p.z});
    }
    CHECK(lo < -50);
    CHECK(hi > 120);
    CHECK_FALSE(
        ray::render::detail::capQuad(*plane, Box3{{0, 100, 0}, {10, 110, 10}}).has_value()); // the plane misses it
    CHECK_FALSE(ray::render::detail::planeEquation(SectionPlane{Vec3{0, 0, 0}, Vec3{0, 0, 0}, true}).has_value());
}

TEST_CASE("draft classes follow the angle to the pull direction", "[render][draft][unit]")
{
    const DraftAnalysis d{Vec3{0, 0, 2}, 1.0};
    const auto at = [](double deg) {
        const double a = deg * kPi / 180;
        return Vec3{0, -std::cos(a), std::sin(a)};
    };
    CHECK(draftClassOf(at(30), d) == DraftClass::Positive);
    CHECK(draftClassOf(at(1.2), d) == DraftClass::Positive);
    CHECK(draftClassOf(at(0.8), d) == DraftClass::Vertical);
    CHECK(draftClassOf(at(-0.8), d) == DraftClass::Vertical);
    CHECK(draftClassOf(at(-1.2), d) == DraftClass::Negative);
    CHECK(draftClassOf(Vec3{0, 0, -1}, d) == DraftClass::Negative);
    CHECK(draftToken(DraftClass::Positive) == Token::DraftPositive);
    CHECK(draftToken(DraftClass::Negative) == Token::DraftNegative);
    CHECK(draftToken(DraftClass::Vertical) == Token::DraftVertical);
}

// ---------- rendering ----------
TEST_CASE("sectioned parts match the reference images, with and without a cap", "[render][section][reference]")
{
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            auto bg = viewportOrSkip(kWidth, kHeight, software);
            const auto dark = theme("graphite-dark");
            bg->setTheme(dark);
            const Image background = draw(*bg);
            std::cout << "[section] " << bg->info().backend << " on " << bg->info().device << "\n";

            std::map<std::string, Image> images;
            for (const SectionScene& scene : sectionScenes()) {
                auto vp = viewportOrSkip(kWidth, kHeight, software);
                vp->setTheme(dark);
                vp->setPreset(presetFor(Tier::Medium));
                buildSectionScene(scene.name, *vp, scene.cap, double(kWidth) / kHeight);
                const Image actual = draw(*vp);
                CHECK(vp->info().sectionCaps == (scene.cap ? 1u : 0u));
                images[scene.name] = actual;
                const std::string path = referencePath(scene.name);
                if (updating() && software) {
                    REQUIRE(writePng(path, actual).has_value());
                    WARN("wrote reference " << path);
                    continue;
                }
                auto reference = readPng(path);
                INFO("scene " << scene.name << ", reference " << path);
                REQUIRE(reference.has_value());
                const Comparison c = compare(actual, *reference, background);
                const std::string actualPath =
                    (std::filesystem::temp_directory_path() / ("ray_render_" + scene.name + "_actual.png")).string();
                std::cout << "[section] " << scene.name << ": tile colour worst " << c.worstBlockColour << " mean "
                          << c.meanBlockColour << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                          << c.coverageA << " vs " << c.coverageB << "\n";
                INFO("tile colour worst " << c.worstBlockColour << " mean " << c.meanBlockColour
                                          << ", tile coverage worst " << c.worstBlockCoverage
                                          << "; this run's image: " << actualPath);
                if (!within(c))
                    static_cast<void>(writePng(actualPath, actual));
                CHECK(c.coverageA > 0.02);
                CHECK(within(c));
                // Strict enough to tell a cap from no cap, and one part from the other.
                for (const SectionScene& other : sectionScenes()) {
                    if (other.name == scene.name)
                        continue;
                    auto otherRef = readPng(referencePath(other.name));
                    REQUIRE(otherRef.has_value());
                    INFO("against " << other.name);
                    CHECK_FALSE(within(compare(actual, *otherRef, background)));
                }
            }
            if (updating())
                continue;

            // The cap only changes the cut: the parts' outside looks the same with and without it.
            for (const char* part : {"section_cut_body", "section_filleted"}) {
                const Image& capped = images[std::string(part) + "_cap"];
                const Image& open = images[std::string(part) + "_nocap"];
                const auto a = coverage(capped, background), b = coverage(open, background);
                int differ = 0;
                for (std::size_t i = 0; i < a.size(); ++i)
                    differ += a[i] != b[i] ? 1 : 0;
                INFO(part);
                CHECK(differ < int(a.size()) / 50); // same silhouette (a cap never pokes out of its body)
                CHECK_FALSE(identical(capped, open));
            }

            // A preset without section caps (Low) draws none, even when the section asks for one.
            for (const char* part : {"section_cut_body", "section_filleted"}) {
                auto withCap = viewportOrSkip(kWidth, kHeight, software);
                auto without = viewportOrSkip(kWidth, kHeight, software);
                for (auto* vp : {withCap.get(), without.get()}) {
                    vp->setTheme(dark);
                    vp->setPreset(presetFor(Tier::Low));
                }
                buildSectionScene(std::string(part) + "_cap", *withCap, true, double(kWidth) / kHeight);
                buildSectionScene(std::string(part) + "_nocap", *without, false, double(kWidth) / kHeight);
                const Image a = draw(*withCap);
                const Image b = draw(*without);
                INFO(part << " on the Low preset");
                CHECK(identical(a, b));
                CHECK(withCap->info().sectionCaps == 0);
                CHECK(withCap->section().has_value());
                CHECK(withCap->section()->cap);
            }
        }
    }
}

TEST_CASE("open shells, lone faces and hidden bodies get no cap and no artefact", "[render][section][caps]")
{
    auto vp = viewportOrSkip(kWidth, kHeight);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::High));
    const Box3 box{{0, 0, 0}, {40, 30, 20}};
    const auto closed = meshOf(kernel::makeBox(box.min, box.max));
    const auto open = withoutFaces(*closed, [](Vec3 p) { return p.z > 19.99; });
    const auto face = meshOf(kernel::testing::makePlanarFace()); // 10 x 20 at z = 5
    vp->setCamera(lookingFrom(Vec3{1.0, -0.6, 0.8}, box, double(kWidth) / kHeight, true));

    const auto imageWith = [&](const std::shared_ptr<const kernel::Mesh>& mesh, bool cap, std::uint32_t* caps) {
        const ItemId id = vp->addMesh(mesh, Xform::identity(), kModel);
        vp->setSection(SectionPlane{Vec3{5, 0, 0}, Vec3{1, 0, 0}, cap});
        const Image img = draw(*vp);
        if (caps)
            *caps = vp->info().sectionCaps;
        vp->remove(id);
        return img;
    };
    std::uint32_t caps = 99;
    SECTION("an open box")
    {
        const Image capped = imageWith(open, true, &caps);
        CHECK(caps == 0);
        CHECK(identical(capped, imageWith(open, false, nullptr)));
    }
    SECTION("a lone face")
    {
        const Image capped = imageWith(face, true, &caps);
        CHECK(caps == 0);
        CHECK(identical(capped, imageWith(face, false, nullptr)));
    }
    SECTION("a closed box is capped; hidden, it is not")
    {
        const Image capped = imageWith(closed, true, &caps);
        CHECK(caps == 1);
        CHECK_FALSE(identical(capped, imageWith(closed, false, nullptr)));
        const ItemId id = vp->addMesh(closed, Xform::identity(), kModel);
        vp->setSection(SectionPlane{Vec3{5, 0, 0}, Vec3{1, 0, 0}, true});
        vp->setVisible(id, false);
        draw(*vp);
        CHECK(vp->info().sectionCaps == 0);
        vp->setVisible(id, true);
        draw(*vp);
        CHECK(vp->info().sectionCaps == 1);
        // A see-through body (transparency on in this preset) gets none either.
        vp->setStyle(id, MeshStyle{Token::ModelFace, Token::ModelEdge, 0.5f, true, false});
        draw(*vp);
        CHECK(vp->info().sectionCaps == 0);
    }
}

TEST_CASE("picks through the cut find the inner face, never a face that was cut away", "[render][section][pick]")
{
    constexpr int w = 320, h = 240;
    auto vp = viewportOrSkip(w, h);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    const auto mesh = meshOf(kernel::testing::makeCutBody()); // 100 x 60 x 20, radius 8 hole at (50, 30)
    const ItemId id = vp->addMesh(mesh, Xform::identity(), kModel);
    const auto isNear = [](double a, double b) {
        return std::abs(a - b) < 1e-3;
    };
    const std::int64_t back = faceWhere(*mesh, [&](Vec3 p) { return isNear(p.y, 60); }); // cut away
    const std::int64_t front = faceWhere(*mesh, [&](Vec3 p) { return isNear(p.y, 0); }); // the far wall, kept
    const std::int64_t hole =
        faceWhere(*mesh, [&](Vec3 p) { return std::abs(std::hypot(p.x - 50, p.y - 30) - 8) < 0.05; });
    REQUIRE(back >= 0);
    REQUIRE(front >= 0);
    REQUIRE(hole >= 0);

    // Straight at the cut from +Y, orthographic: screen right is -X, up is +Z, 3 px per mm around (50, *, 10).
    Camera cam{};
    cam.target = Vec3{50, 30, 10};
    cam.eye = Vec3{50, 300, 10};
    cam.up = Vec3{0, 0, 1};
    cam.perspective = false;
    cam.fovDeg = 30;
    cam.orthoHeight = 80;
    vp->setCamera(cam);
    const auto px = [&](double x) {
        return int(std::floor(w / 2.0 - (x - 50) * 3.0));
    };
    const auto py = [&](double z) {
        return int(std::floor(h / 2.0 - (z - 10) * 3.0));
    };

    // Without a section the material pixel shows the +Y face.
    auto hit = pickAt(*vp, px(20), py(10), kFaces);
    REQUIRE(hit.has_value());
    CHECK(std::int64_t(hit->subIndex) == back);

    for (const bool cap : {false, true}) {
        DYNAMIC_SECTION((cap ? "with a cap" : "without a cap"))
        {
            vp->setSection(SectionPlane{Vec3{0, 30, 0}, Vec3{0, 1, 0}, cap});
            waitForClosedChecks(*vp);
            // Through the hole: its far half.
            hit = pickAt(*vp, px(50), py(10), kFaces);
            REQUIRE(hit.has_value());
            CHECK(std::int64_t(hit->subIndex) == hole);
            CHECK(hit->point.y < 30.01);
            // Through the material: the cap (its body), or without one the far wall seen from inside.
            hit = pickAt(*vp, px(20), py(10), kFaces);
            if (cap) {
                CHECK_FALSE(hit.has_value());
                const auto body = pickAt(*vp, px(20), py(10), kBodies);
                REQUIRE(body.has_value());
                CHECK(body->item == id);
                CHECK(body->kind == kernel::SubKind::Solid);
                CHECK(std::abs(body->point.y - 30) < 0.5);
            } else {
                REQUIRE(hit.has_value());
                CHECK(std::int64_t(hit->subIndex) == front);
            }
            // Nowhere on the part is the cut-away face pickable, and every point lies on the kept side.
            int hits = 0;
            for (double x = 2; x <= 98; x += 6) {
                for (double z = 2; z <= 18; z += 4) {
                    const auto h2 = pickAt(*vp, px(x), py(z), kFaces);
                    if (!h2)
                        continue;
                    ++hits;
                    INFO("x " << x << " z " << z);
                    CHECK(std::int64_t(h2->subIndex) != back);
                    CHECK(h2->point.y < 30.5);
                }
            }
            CHECK(hits > (cap ? 5 : 40));
        }
    }
}

TEST_CASE("draft colours match the angles of a test wedge", "[render][draft]")
{
    constexpr int w = 360, h = 120;
    auto vp = viewportOrSkip(w, h);
    const auto dark = theme("graphite-dark");
    vp->setTheme(dark);
    vp->setPreset(presetFor(Tier::Medium));
    // Square faces in a row, each tilted so that its normal makes a known draft angle with +Z, facing the camera.
    const std::vector<double> angles{30, 5, 2, 0.5, 0, -0.5, -2, -5, -30};
    kernel::Mesh mesh;
    for (std::size_t i = 0; i < angles.size(); ++i) {
        const double a = angles[i] * kPi / 180;
        const Vec3 c{double(i) * 12.0, 0, 0};
        const Vec3 n{0, -std::cos(a), std::sin(a)};
        const Vec3 v{0, std::sin(a), std::cos(a)};
        const std::uint32_t base = std::uint32_t(mesh.positions.size() / 3);
        for (const auto& [su, sv] : std::vector<std::pair<double, double>>{{-5, -5}, {5, -5}, {5, 5}, {-5, 5}}) {
            mesh.positions.insert(mesh.positions.end(),
                                  {float(c.x + su), float(c.y + sv * v.y), float(c.z + sv * v.z)});
            mesh.normals.insert(mesh.normals.end(), {float(n.x), float(n.y), float(n.z)});
        }
        kernel::FaceRange f{};
        f.face = kernel::SubId{kernel::SubKind::Face, std::uint32_t(i)};
        f.firstIndex = std::uint32_t(mesh.indices.size());
        f.indexCount = 6;
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        mesh.faces.push_back(f);
    }
    // Its own colour, which draft analysis must override.
    MeshStyle style{Token::ModelFace, Token::ModelEdge, 1.0f, false, false};
    style.faceColor = Color{0.05f, 0.1f, 0.9f, 1.0f};
    vp->addMesh(std::make_shared<const kernel::Mesh>(std::move(mesh)), Xform::identity(), style);
    Camera cam{};
    cam.target = Vec3{48, 0, 0};
    cam.eye = Vec3{48, -300, 0};
    cam.up = Vec3{0, 0, 1};
    cam.perspective = false;
    cam.fovDeg = 30;
    cam.orthoHeight = 36; // 3.33 px per mm
    vp->setCamera(cam);
    const double scale = h / cam.orthoHeight;
    const auto centreOf = [&](std::size_t i) {
        return int(std::floor(w / 2.0 + (double(i) * 12.0 - 48) * scale));
    };

    const Image plain = draw(*vp, w, h);
    const auto blue = rgbAt(plain, centreOf(4), h / 2);
    CHECK(blue[2] > blue[0] + 60); // the body's own colour without the analysis

    for (const auto& [pull, threshold] : std::vector<std::pair<Vec3, double>>{
             {Vec3{0, 0, 1}, 1.0}, {Vec3{0, 0, 1}, 3.0}, {Vec3{0, 0, -1}, 1.0}, {Vec3{1, 0, 0}, 1.0}}) {
        const DraftAnalysis d{pull, threshold};
        vp->setDraftAnalysis(d);
        const Image img = draw(*vp, w, h);
        for (std::size_t i = 0; i < angles.size(); ++i) {
            const double a = angles[i] * kPi / 180;
            const DraftClass expected = draftClassOf(Vec3{0, -std::cos(a), std::sin(a)}, d);
            double dist = 0;
            const DraftClass got = classOfPixel(img, centreOf(i), h / 2, *dark, &dist);
            INFO("pull " << pull.x << "," << pull.y << "," << pull.z << " threshold " << threshold << " angle "
                         << angles[i] << ": expected " << className(expected) << ", got " << className(got)
                         << " (chromaticity distance " << dist << ")");
            CHECK(got == expected);
            CHECK(dist < 0.08);
        }
    }
    CHECK(vp->draftAnalysis().has_value());
    vp->setDraftAnalysis(std::nullopt);
    CHECK(identical(draw(*vp, w, h), plain));
}

TEST_CASE("draft analysis puts the class boundary of a curved face where the angle crosses the threshold",
          "[render][draft]")
{
    constexpr int w = 200, h = 240;
    auto vp = viewportOrSkip(w, h);
    const auto dark = theme("graphite-dark");
    vp->setTheme(dark);
    vp->setPreset(presetFor(Tier::Medium));
    // A cylinder of radius 40 along X: seen from -Y, a surface point at height z has the draft angle asin(z / 40).
    constexpr double r = 40;
    const auto mesh = meshOf(kernel::makeCylinder(Vec3{-60, 0, 0}, Vec3{1, 0, 0}, r, 120), 0.01, 3 * kPi / 180);
    vp->addMesh(mesh, Xform::identity(), MeshStyle{Token::ModelFace, Token::ModelEdge, 1.0f, false, false});
    Camera cam{};
    cam.target = Vec3{0, 0, 0};
    cam.eye = Vec3{0, -400, 0};
    cam.up = Vec3{0, 0, 1};
    cam.perspective = false;
    cam.fovDeg = 30;
    cam.orthoHeight = 100;
    vp->setCamera(cam);
    const double scale = h / cam.orthoHeight; // px per mm
    for (const double threshold : {20.0, 45.0}) {
        vp->setDraftAnalysis(DraftAnalysis{Vec3{0, 0, 1}, threshold});
        const Image img = draw(*vp, w, h);
        // Scan the middle column from top to bottom: positive, then near-vertical, then negative.
        const int x = w / 2;
        int firstVertical = -1, firstNegative = -1;
        const int top = int(std::ceil(h / 2.0 - (r - 1) * scale)), bottom = int(std::floor(h / 2.0 + (r - 1) * scale));
        for (int y = top; y <= bottom; ++y) {
            const DraftClass c = classOfPixel(img, x, y, *dark);
            if (c == DraftClass::Vertical && firstVertical < 0)
                firstVertical = y;
            if (c == DraftClass::Negative && firstNegative < 0)
                firstNegative = y;
        }
        const double zBoundary = r * std::sin(threshold * kPi / 180);
        const double expectedTop = h / 2.0 - zBoundary * scale;    // positive -> vertical
        const double expectedBottom = h / 2.0 + zBoundary * scale; // vertical -> negative
        INFO("threshold " << threshold << ": boundaries at rows " << firstVertical << " and " << firstNegative
                          << ", expected " << expectedTop << " and " << expectedBottom);
        CHECK(classOfPixel(img, x, top + 2, *dark) == DraftClass::Positive);
        CHECK(classOfPixel(img, x, h / 2, *dark) == DraftClass::Vertical);
        CHECK(classOfPixel(img, x, bottom - 2, *dark) == DraftClass::Negative);
        CHECK(std::abs(firstVertical - expectedTop) <= 3.0);
        CHECK(std::abs(firstNegative - expectedBottom) <= 3.0);
    }
}

TEST_CASE("moving the section plane re-renders without uploading meshes again", "[render][section][counter]")
{
    auto vp = viewportOrSkip(kWidth, kHeight);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    buildSectionScene("section_filleted_cap", *vp, true, double(kWidth) / kHeight);
    waitForClosedChecks(*vp);
    REQUIRE(vp->renderNow().has_value());
    const ViewportInfo before = vp->info();
    CHECK(before.meshUploads == 1);
    CHECK(before.sectionCaps == 1);
    for (int i = 0; i < 20; ++i) {
        vp->setSection(SectionPlane{Vec3{0, 5.0 + 2.0 * i, 0}, Vec3{0, 1, 0}, true});
        REQUIRE(vp->renderNow().has_value());
    }
    vp->setDraftAnalysis(DraftAnalysis{Vec3{0, 0, 1}, 2.0});
    REQUIRE(vp->renderNow().has_value());
    const ViewportInfo after = vp->info();
    CHECK(after.meshUploads == before.meshUploads);
    CHECK(after.framesRendered == before.framesRendered + 21);
    CHECK(after.sectionCaps == 1);

    // Idle: nothing is drawn.
    processEventsFor(300);
    const std::uint64_t frames = vp->info().framesRendered;
    processEventsFor(500);
    CHECK(vp->info().framesRendered == frames);
    // The same plane again is no change at all.
    vp->setSection(vp->section());
    processEventsFor(200);
    CHECK(vp->info().framesRendered == frames);
}

TEST_CASE("section and draft analysis survive theme and preset switches", "[render][section][draft][state]")
{
    auto vp = viewportOrSkip(kWidth, kHeight);
    const auto dark = theme("graphite-dark");
    vp->setTheme(dark);
    vp->setPreset(presetFor(Tier::Medium));
    buildSectionScene("section_cut_body_cap", *vp, true, double(kWidth) / kHeight);
    const DraftAnalysis draft{Vec3{0, 0, 1}, 3.0};
    vp->setDraftAnalysis(draft);
    const Image before = draw(*vp);
    CHECK(vp->info().sectionCaps == 1);

    vp->setTheme(theme("graphite-light"));
    const Image light = draw(*vp);
    CHECK_FALSE(identical(light, before));
    CHECK(vp->info().sectionCaps == 1);
    vp->setPreset(presetFor(Tier::Low));
    draw(*vp);
    CHECK(vp->info().sectionCaps == 0); // Low: no caps, the section itself stays
    vp->setPreset(presetFor(Tier::High));
    draw(*vp);
    vp->setPreset(presetFor(Tier::Medium));
    vp->setTheme(dark);
    const Image after = draw(*vp);
    CHECK(identical(after, before));
    REQUIRE(vp->section().has_value());
    CHECK(vp->section()->origin.y == 30);
    CHECK(vp->section()->cap);
    REQUIRE(vp->draftAnalysis().has_value());
    CHECK(vp->draftAnalysis()->thresholdDeg == 3.0);
}

TEST_CASE("the glass backdrop is blurred again from the sectioned image", "[render][section][backdrop]")
{
    auto vp = viewportOrSkip(kWidth, kHeight);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    buildSectionScene("section_cut_body_cap", *vp, true, double(kWidth) / kHeight);
    waitForClosedChecks(*vp); // known from here on: the capped frame below is the first and only one
    vp->setSection(std::nullopt);
    vp->setBackdropEnabled(true);
    REQUIRE(vp->renderNow().has_value());
    auto whole = vp->readBackdrop();
    REQUIRE(whole.has_value());
    const std::uint64_t blurs = vp->info().backdropBlurs;
    // Only the section changes: that frame redraws the picture and blurs it again.
    vp->setSection(SectionPlane{Vec3{0, 30, 0}, Vec3{0, 1, 0}, true});
    REQUIRE(processEventsUntil([&] { return vp->info().backdropBlurs == blurs + 1; }, 10000));
    auto cut = vp->readBackdrop();
    REQUIRE(cut.has_value());
    CHECK_FALSE(identical(*whole, *cut));
    // The blur of a sectioned image equals the blur another viewport makes of the same section from the start.
    auto other = viewportOrSkip(kWidth, kHeight);
    other->setTheme(theme("graphite-dark"));
    other->setPreset(presetFor(Tier::Medium));
    buildSectionScene("section_cut_body_cap", *other, true, double(kWidth) / kHeight);
    waitForClosedChecks(*other);
    other->setBackdropEnabled(true);
    REQUIRE(other->renderNow().has_value());
    auto expected = other->readBackdrop();
    REQUIRE(expected.has_value());
    CHECK(identical(*cut, *expected));
}

TEST_CASE("cost of section caps on a 500k-triangle body", "[render][section][cost]")
{
    const auto mesh = meshOf(kernel::testing::makeSphere(Vec3{0, 0, 0}, 50), 0.0008, 1.0 * kPi / 180);
    REQUIRE(mesh->indices.size() / 3 >= 500'000);
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    CHECK(ray::render::detail::isClosedMesh(*mesh));
    const double closedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    auto vp = viewportOrSkip(1920, 1080);
    vp->setTheme(theme("graphite-dark"));
    vp->setPreset(presetFor(Tier::Medium));
    vp->addMesh(mesh, Xform::identity(), kModel);
    vp->setCamera(standardViewCamera(StandardView::Iso, Box3{Vec3{-50, -50, -50}, Vec3{50, 50, 50}}, 16.0 / 9.0, true));
    vp->setSection(SectionPlane{Vec3{0, 0, 0}, Vec3{0, 0, 1}, true});
    // The closedness check runs on a worker: the render thread never waits for it.
    REQUIRE(vp->renderNow().has_value());
    const std::uint32_t capsBeforeCheck = vp->info().sectionCaps;
    const auto t1 = Clock::now();
    waitForClosedChecks(*vp);
    const double workerMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
    REQUIRE(vp->renderNow().has_value());
    CHECK(vp->info().sectionCaps == 1);
    std::cout << "[section] the first capped frame came before the worker's check: " << (capsBeforeCheck == 0)
              << " (check finished " << workerMs << " ms later)\n";
    // Interleaved, so GPU clock changes hit both alike: no section / a section without cap / a capped section.
    std::vector<double> plainMs, cutMs, cappedMs;
    for (int i = 0; i < 30; ++i) {
        const auto one = [&](std::optional<SectionPlane> s, std::vector<double>& out) {
            vp->setSection(s);
            REQUIRE(vp->renderNow().has_value());
            out.push_back(vp->lastFrameStats().cpuMs); // offscreen: the whole frame, GPU wait included
        };
        one(std::nullopt, plainMs);
        one(SectionPlane{Vec3{0, 0, 0.01 * i}, Vec3{0, 0, 1}, false}, cutMs);
        one(SectionPlane{Vec3{0, 0, 0.01 * i}, Vec3{0, 0, 1}, true}, cappedMs);
    }
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const double plain = median(plainMs);
    const double cut = median(cutMs);
    const double capped = median(cappedMs);
    std::cout << "[section] 500k triangles: section without cap " << cut << " ms\n";
    std::cout << "[section] 500k triangles: closed-mesh check " << closedMs << " ms (once per mesh); frame " << plain
              << " ms without a section, " << capped << " ms with a capped section (" << vp->info().backend << " on "
              << vp->info().device << ", 1920x1080)\n";
}
