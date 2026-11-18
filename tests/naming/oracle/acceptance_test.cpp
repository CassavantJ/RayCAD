// The scripted acceptance scenarios of T-081a, judged by the oracle: an unrelated cut, a splitting cut, a unifying
// fuse, a removing cut, and the three failure cases of spike S4 (ordinal swap after an unrelated cut, a hole rim
// broken into two arcs, a slot moved by its own width). Every reference of every scenario must be judged correct
// (or honestly Broken); the specific references each scenario is about must get the expected state.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

using namespace ray;
using namespace ray::naming_oracle;

namespace {

Ent line(double x0, double y0, double x1, double y1, std::uint64_t tag)
{
    Ent e;
    e.k = 'L';
    e.x0 = x0;
    e.y0 = y0;
    e.x1 = x1;
    e.y1 = y1;
    e.tag = tag;
    return e;
}
Ent arc(double x0, double y0, double x1, double y1, double cx, double cy, double r, std::uint64_t tag)
{
    Ent e = line(x0, y0, x1, y1, tag);
    e.k = 'A';
    e.cx = cx;
    e.cy = cy;
    e.r = r;
    return e;
}
// Rectangle: tags 1 bottom (y=y0), 2 right, 3 top, 4 left.
Loop rect(double x0, double y0, double x1, double y1)
{
    return Loop{1,
                {line(x0, y0, x1, y0, 1), line(x1, y0, x1, y1, 2), line(x1, y1, x0, y1, 3), line(x0, y1, x0, y0, 4)}};
}
Loop circle(double cx, double cy, double r)
{
    Ent e;
    e.k = 'C';
    e.cx = cx;
    e.cy = cy;
    e.r = r;
    e.tag = 1;
    e.x0 = e.x1 = cx + r;
    e.y0 = e.y1 = cy;
    return Loop{1, {e}};
}
// Obround: 1 bottom line, 2 right arc, 3 top line, 4 left arc.
Loop obround(double x0, double x1, double cy, double r)
{
    return Loop{1,
                {line(x0, cy - r, x1, cy - r, 1), arc(x1, cy - r, x1, cy + r, x1, cy, r, 2),
                 line(x1, cy + r, x0, cy + r, 3), arc(x0, cy + r, x0, cy - r, x0, cy, r, 4)}};
}
Frame top(double z)
{
    Frame f;
    f.o = {0, 0, z};
    return f;
}
Feature sweep(const std::string& id, BOp op, Frame fr, Loop loop, double dist, const std::string& role = "extrude")
{
    Feature f;
    f.id = id;
    f.type = FType::Sweep;
    f.op = op;
    f.role = role;
    f.frame = fr;
    f.loops = {std::move(loop)};
    f.dist = dist;
    return f;
}

struct Outcome {
    Ref ref;
    naming::Resolved resolved;
    TruthEval expected;
    Verdict verdict;
};

// Builds `base`, records every reference the oracle vouches for, builds `edited`, resolves and judges them all.
std::vector<Outcome> run(const Script& base, const Script& edited)
{
    FeatRefs picks;
    const Builder b0 = buildScript(base, Mode::Author, picks);
    REQUIRE(b0.failed().empty());
    auto s0 = sampleShape(b0.shape());
    REQUIRE(s0.has_value());
    const std::vector<Ref> refs = recordReferences(b0, buildOwners(builtOnly(base, b0)), **s0, nullptr);
    REQUIRE(!refs.empty());
    const Builder b1 = buildScript(edited, Mode::Replay, picks);
    for (const std::string& f : b1.failed())
        std::printf("  replay failure: %s\n", f.c_str());
    auto s1 = sampleShape(b1.shape());
    REQUIRE(s1.has_value());
    const Owners ow1 = buildOwners(builtOnly(edited, b1));
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), b1.map(), b1.shape()}});
    std::vector<Outcome> out;
    for (const Ref& r : refs) {
        const naming::Resolved res = resolver->resolve(r.ref);
        const TruthEval te = evalTruth(r.truth, ow1, **s1);
        std::vector<std::uint32_t> elems;
        for (const SubId& s : res.elements)
            elems.push_back(s.index);
        const auto cong = congruent(r.picked, r.ref.kind, **s1, r.ref.kind == SubKind::Vertex ? 1e-3 : 0.05);
        const Verdict v = judge(answerOf(res.state), elems, te, r.truth.weak, cong.size() == 1 ? long(cong[0]) : -1);
        out.push_back({r, res, te, v});
    }
    return out;
}

// No wrong verdict at all, and no usable Repaired.
void checkNoneWrong(const std::vector<Outcome>& outcomes)
{
    for (const Outcome& o : outcomes) {
        INFO("reference " << o.ref.base.index << " kind " << static_cast<int>(o.ref.ref.kind) << " [" << o.ref.truth.cat
                          << "]: " << verdictName(o.verdict));
        CHECK_FALSE(isWrong(o.verdict));
        if (o.resolved.state == naming::ResolveState::Repaired)
            CHECK_FALSE(o.resolved.usable());
    }
}

// Does the reference's description name this owner (in any of its constraints)?
bool about(const Outcome& o, const std::string& owner)
{
    for (const Constraint& c : o.ref.truth.cons)
        if (std::find(c.alts.begin(), c.alts.end(), owner) != c.alts.end())
            return true;
    return false;
}

// A face reference whose description is exactly the one owner (with any separators).
bool faceOf(const Outcome& o, const std::string& owner)
{
    return o.ref.ref.kind == SubKind::Face && o.ref.truth.cons.size() == 1 &&
           o.ref.truth.cons[0].alts == std::vector<std::string>{owner};
}

std::size_t count(const std::vector<Outcome>& v, const std::function<bool(const Outcome&)>& pred)
{
    return static_cast<std::size_t>(std::count_if(v.begin(), v.end(), pred));
}

Script block(double L, double W, double H)
{
    return {sweep("F01", BOp::New, top(0), rect(0, 0, L, W), H)};
}

} // namespace

TEST_CASE("Acceptance: an unrelated cut leaves every other reference Exact", "[naming][acceptance]")
{
    Script base = block(100, 60, 20);
    base.push_back(sweep("H1", BOp::Cut, top(20), circle(30, 30, 5), 40, "hole"));
    base.push_back(sweep("P1", BOp::Cut, top(20), rect(60, 10, 90, 30), 5));
    Script edited = base;
    // A small pocket in a free corner of the top, early in the tree.
    edited.insert(edited.begin() + 1, sweep("U1", BOp::Cut, top(20), rect(5, 45, 15, 55), 3));
    const auto out = run(base, edited);
    checkNoneWrong(out);
    for (const Outcome& o : out) {
        INFO("reference " << o.ref.base.index << " [" << o.ref.truth.cat << "]");
        CHECK(o.resolved.state == naming::ResolveState::Exact);
        CHECK(o.verdict == Verdict::Correct);
    }
}

TEST_CASE("Acceptance: a cut that splits a referenced face gives Split with both pieces", "[naming][acceptance]")
{
    const Script base = block(100, 60, 20);
    Script edited = base;
    edited.push_back(sweep("N1", BOp::Cut, top(30), rect(40, -5, 60, 10), 40));
    const auto out = run(base, edited);
    checkNoneWrong(out);
    REQUIRE(count(out, [](const Outcome& o) { return faceOf(o, "F01/s1"); }) == 1);
    for (const Outcome& o : out)
        if (faceOf(o, "F01/s1")) {
            CHECK(o.resolved.state == naming::ResolveState::Split);
            CHECK(o.resolved.elements.size() == 2);
            CHECK(o.expected.elems.size() == 2);
            CHECK(o.verdict == Verdict::Correct);
        }
}

TEST_CASE("Acceptance: a fuse that unifies two faces gives Merged", "[naming][acceptance]")
{
    Script base = block(100, 60, 20);
    base.push_back(sweep("B1", BOp::Join, top(20), rect(10, 5, 40, 30), 10));
    Script edited = block(100, 60, 20);
    // The same boss, flush with the block's front face and starting inside it: its front face unifies.
    edited.push_back(sweep("B1", BOp::Join, top(10), rect(10, 0, 40, 30), 20));
    const auto out = run(base, edited);
    checkNoneWrong(out);
    REQUIRE(count(out, [](const Outcome& o) { return faceOf(o, "B1/s1"); }) == 1);
    for (const Outcome& o : out)
        if (faceOf(o, "B1/s1")) {
            CHECK(o.resolved.state == naming::ResolveState::Merged);
            CHECK(o.resolved.elements.size() == 1);
            CHECK(o.expected.merged);
            CHECK(o.verdict == Verdict::Correct);
        }
}

TEST_CASE("Acceptance: a cut that removes a referenced face gives Broken", "[naming][acceptance]")
{
    Script base = block(100, 60, 20);
    base.push_back(sweep("P1", BOp::Cut, top(20), rect(20, 20, 40, 40), 5));
    Script edited = base;
    edited.push_back(sweep("T1", BOp::Cut, top(30), rect(15, 15, 45, 45), 40));
    const auto out = run(base, edited);
    checkNoneWrong(out);
    REQUIRE(count(out, [](const Outcome& o) { return faceOf(o, "P1/c1/1"); }) == 1);
    for (const Outcome& o : out)
        if (faceOf(o, "P1/c1/1")) {
            CHECK(o.resolved.state == naming::ResolveState::Broken);
            CHECK(o.expected.elems.empty());
            CHECK(o.verdict == Verdict::BrokenGone);
        }
}

TEST_CASE("Acceptance: ordinal swap after an unrelated cut (S4 V34) stays Exact on the right piece",
          "[naming][acceptance]")
{
    // A groove along the x=0 side splits it into an upper and a lower piece. A shallow cut added early at the top
    // of that side moves the upper piece's centroid past the lower piece's: ordinals by centroid would swap.
    auto groove = [] {
        Frame f;
        f.o = {0, 0, 0};
        f.xd = {0, -1, 0};
        f.n = {-1, 0, 0};
        return sweep("G1", BOp::Cut, f, rect(-105, 10, 5, 14), 3);
    };
    Script base = block(160, 100, 30);
    base.push_back(groove());
    Script edited = block(160, 100, 30);
    edited.push_back(sweep("X3", BOp::Cut, top(30), rect(-5, 10, 2, 30), 6));
    edited.push_back(groove());
    const auto out = run(base, edited);
    checkNoneWrong(out);
    const auto pieces = [](const Outcome& o) {
        return faceOf(o, "F01/s4") && !o.ref.truth.cons[0].seps.empty();
    };
    REQUIRE(count(out, pieces) == 2);
    for (const Outcome& o : out)
        if (pieces(o)) {
            CHECK(o.resolved.state == naming::ResolveState::Exact);
            CHECK(o.verdict == Verdict::Correct);
        }
}

TEST_CASE("Acceptance: a hole rim broken into two arcs (S4 B36) gives Split with both arcs", "[naming][acceptance]")
{
    // A plate with a hole, then a groove across the top. Moving the hole onto the groove breaks its top rim in two.
    auto script = [](double holeY) {
        Script s = block(100, 60, 10);
        s.push_back(sweep("H1", BOp::Cut, top(10), circle(50, holeY, 5), 20, "hole"));
        s.push_back(sweep("C1", BOp::Cut, top(10), rect(-5, 28, 105, 32), 3));
        return s;
    };
    const auto out = run(script(45), script(30));
    checkNoneWrong(out);
    const auto rim = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && about(o, "H1/s1") && about(o, "F01/c1/1");
    };
    REQUIRE(count(out, rim) == 1);
    for (const Outcome& o : out)
        if (rim(o)) {
            INFO("resolver: " << o.resolved.reason << "; names " << o.ref.names);
            CHECK(o.resolved.state == naming::ResolveState::Split);
            CHECK(o.resolved.elements.size() == 2);
            CHECK(o.verdict == Verdict::Correct);
        }
}

TEST_CASE("Acceptance: a slot moved by its own width (S4 P23) gives Broken, never the other wall",
          "[naming][acceptance]")
{
    // A through slot near the y=0 side, then moved by its own width so its lower wall leaves the part and its upper
    // wall lands exactly where the lower wall was. Everything on the lower wall is gone; the upper wall's edges and
    // vertices now sit, congruent, where the picked ones were.
    auto script = [](double cy) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("S1", BOp::Cut, top(20), obround(30, 70, cy, 4), 40));
        return s;
    };
    const auto out = run(script(8), script(0));
    checkNoneWrong(out);
    const auto lowerWall = [](const Outcome& o) {
        return about(o, "S1/s1");
    };
    REQUIRE(count(out, lowerWall) >= 5);
    for (const Outcome& o : out)
        if (lowerWall(o)) {
            INFO("reference " << o.ref.base.index << " [" << o.ref.truth.cat << "]");
            CHECK(o.expected.elems.empty());
            CHECK(o.resolved.state == naming::ResolveState::Broken);
        }
}
