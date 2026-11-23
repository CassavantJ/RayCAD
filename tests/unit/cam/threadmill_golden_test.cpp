// Golden toolpaths of the thread milling operation (task T-056): an internal single-form thread in radial passes with a
// spring pass, an internal multi-form thread in steps with an overlap, an external left-hand thread on a pin, and an
// internal thread cut conventionally (top down) in the counterbored holes of a corpus part. Each is
// tests/golden/toolpaths/threadmill_<name>.rctp with its .txt, passes the full toolpath invariant checker (the part
// left by drilling as the stock, cutting allowed only inside the thread's cylinder, climb or conventional claimed), and
// keeps the pitch exact per revolution. threadmill_internal and threadmill_external are also the inputs of the shipped
// posts' feature matrix (tests/unit/post/shipped_matrix.h "18-thread-milling").
#include "engrave_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::ops;
using namespace ray::cam::test;

namespace {

// threadmill_test.cpp's block: T1 (15, 20) 6.8 through; T2 (40, 20) 10.2 blind to Z 4 (118 degree point); top Z 20.
const kernel::Shape& block()
{
    static const kernel::Shape s = [] {
        kernel::Shape b = box(Vec3{0, 0, 0}, Vec3{60, 40, 20});
        b = cut(b, revolved(15, 20, {{0, 21}, {3.4, 21}, {3.4, -1}, {0, -1}}));
        b = cut(b, revolved(40, 20, {{0, 21}, {5.1, 21}, {5.1, 4}, {0, 4 - 5.1 * kTip118}}));
        return b;
    }();
    return s;
}

// The Z change of every whole revolution (two consecutive cut arcs from (x + R, y) back to it) is `pitch`.
void pitchExact(const Toolpath& p, Nm pitch)
{
    std::size_t revolutions = 0;
    for (std::size_t i = 1; i + 1 < p.moves.size(); ++i) {
        const Move& prev = p.moves[i - 1];
        const Move& a = p.moves[i];
        const Move& b = p.moves[i + 1];
        const bool cut = a.role == MoveRole::Cut && b.role == MoveRole::Cut && a.kind != MoveKind::Linear &&
                         b.kind != MoveKind::Linear && a.kind != MoveKind::Rapid;
        if (cut && prev.y == a.j && b.y == a.j && b.x == prev.x && a.x == 2 * a.i - prev.x && prev.x != a.x) {
            ++revolutions;
            CHECK(std::abs(b.z - prev.z) == pitch);
            CHECK(b.i == a.i);
            CHECK(b.j == a.j);
        }
    }
    CHECK(revolutions > 0);
}

} // namespace

TEST_CASE("golden: thread milling M8x1.25 inside, single-form, radial passes and a spring pass",
          "[cam][threadmill][golden]")
{
    OpCase c = threadCase(block(), {holeFace(block(), 15, 20)},
                          R"({"pitch":1.25,"diameter":8,"radialPasses":2,"springPass":true})", threadMill(5));
    c.op.cutting.feedLeadIn = 100;
    c.op.cutting.feedLeadOut = 150;
    const Toolpath p = c.path([](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts.push_back(invariants::AllowedCut{15, 20, 4.01, -1, 30});
        k.direction = invariants::Direction::Climb;
    });
    matchesToolpathGolden("threadmill_internal", p);
    pitchExact(p, 1'250'000);
}

TEST_CASE("golden: thread milling M12x1.75 inside, multi-form in steps with an overlap", "[cam][threadmill][golden]")
{
    OpCase c = threadCase(block(), {holeFace(block(), 40, 20)},
                          R"({"form":"multi","pitch":1.75,"diameter":12,"overlap":20,"radialPasses":2})",
                          threadMill(5.5, 1.75, 10, 30));
    const Toolpath p = c.path([](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts.push_back(invariants::AllowedCut{40, 20, 6.01, 3.99, 40});
        k.direction = invariants::Direction::Climb;
    });
    matchesToolpathGolden("threadmill_multi", p);
    pitchExact(p, 1'750'000);
}

TEST_CASE("golden: thread milling M12x1.75 left-hand outside a pin", "[cam][threadmill][golden]")
{
    const kernel::Shape pin = pinPlate(12, 38);
    OpCase c = threadCase(pin, cylinderFaces(pin, 30, 20, 6), R"({"location":"external","pitch":1.75,"leftHand":true})",
                          threadMill(6));
    const Toolpath p = c.path([](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts.push_back(invariants::AllowedCut{30, 20, 6.5, 3.99, 30});
        k.direction = invariants::Direction::Climb;
    });
    matchesToolpathGolden("threadmill_external", p);
    pitchExact(p, 1'750'000);
}

TEST_CASE("golden: thread milling the counterbored holes of a corpus part, conventional",
          "[cam][threadmill][golden][corpus]")
{
    // plate_counterbore_1: four 6.6 mm through holes from Z 8.5 to Z 0 under 11 mm counterbores from Z 15
    // (tests/unit/cam/holes_labels.txt). An M8x1.25 thread: the 6.6 mm bore is under its minor diameter (a warning).
    JobContext ctx;
    auto imported =
        kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/plate_counterbore_1.stp", {}, ctx);
    REQUIRE(imported);
    const kernel::Shape part = imported->bodies.at(0).shape;
    std::vector<kernel::SubId> faces;
    std::vector<invariants::AllowedCut> allowed;
    for (const RecognizedHole& h : holesOf(part)) {
        faces.push_back(h.axisSub);
        allowed.push_back(invariants::AllowedCut{toMm(h.x), toMm(h.y), 4.01, -1, 9}); // the counterbore above
    }
    REQUIRE(faces.size() == 4);
    OpCase c = threadCase(part, faces, R"({"pitch":1.25,"diameter":8,"direction":"conventional"})", threadMill(5));
    auto warnings = c.check();
    REQUIRE(warnings);
    CHECK(warnings->size() == 4); // one per hole: the bore is under the minor diameter
    const Toolpath p = c.path([&](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts = allowed;
        k.direction = invariants::Direction::Conventional;
    });
    matchesToolpathGolden("threadmill_corpus", p);
    pitchExact(p, 1'250'000);
    // Conventional, right hand, inside: clockwise and down.
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Cut) {
            CHECK(m.kind == MoveKind::ArcCW);
        }
    }
}

TEST_CASE("thread milling gives the same bytes every time", "[cam][threadmill][golden]")
{
    const auto make = [] {
        return threadCase(block(), {holeFace(block(), 15, 20), holeFace(block(), 40, 20)},
                          R"({"pitch":1.25,"diameter":8,"radialPasses":3})", threadMill(5))
            .generate();
    };
    const auto first = make();
    // T2 (10.2 mm) is not smaller than an 8 mm thread: refused, the same way each time.
    REQUIRE_FALSE(first);
    const auto good = [] {
        return threadCase(block(), {holeFace(block(), 15, 20)}, R"({"pitch":1,"diameter":8,"radialPasses":3})",
                          threadMill(5))
            .generate();
    };
    const auto a = good();
    REQUIRE(a);
    for (int i = 0; i < 3; ++i) {
        const auto b = good();
        REQUIRE(b);
        CHECK(b->serialize() == a->serialize());
        CHECK(make().error().message == first.error().message);
    }
}
