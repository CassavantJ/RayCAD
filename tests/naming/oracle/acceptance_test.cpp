// The scripted acceptance scenarios of T-081a, judged by the full oracle (oracle.h): an unrelated cut, a splitting
// cut, a unifying fuse, a removing cut, and the three failure cases of spike S4 (ordinal swap after an unrelated
// cut, a hole rim broken into two arcs, a slot moved by its own width). Every reference of every scenario must be
// judged correct (or honestly Broken); the specific references each scenario is about must get the expected state.
// Then the strict-judge scenarios of the T-081a review: cases where a separate element shares the picked element's
// faces or carrier, in which a wrong answer must be judged wrong by the oracle and never given by the resolver.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

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
    RefResult result;
    TruthEval expected; // E (elems, merged)
    Verdict verdict = Verdict::Correct;
};

struct Run {
    std::vector<Outcome> out;
    std::shared_ptr<const Samples> now;
    std::shared_ptr<const Owners> owners;
};

// Builds `base`, records every reference the oracle vouches for, builds `edited`, resolves and judges them all.
Run run(const Script& base, const Script& edited)
{
    FeatRefs picks;
    const Builder b0 = buildScript(base, Mode::Author, picks);
    REQUIRE(b0.failed().empty());
    auto s0 = sampleShape(b0.shape());
    REQUIRE(s0.has_value());
    const auto ow0 = std::make_shared<const Owners>(buildOwners(builtOnly(base, b0)));
    const std::vector<Ref> refs = recordReferences(b0, ow0, *s0, nullptr);
    REQUIRE(!refs.empty());
    const Builder b1 = buildScript(edited, Mode::Replay, picks);
    for (const std::string& f : b1.failed())
        std::printf("  replay failure: %s\n", f.c_str());
    auto s1 = sampleShape(b1.shape());
    REQUIRE(s1.has_value());
    Run r;
    r.now = *s1;
    r.owners = std::make_shared<const Owners>(buildOwners(builtOnly(edited, b1)));
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), b1.map(), b1.shape()}});
    for (const Ref& ref : refs) {
        const naming::Resolved res = resolver->resolve(ref.ref);
        RefResult rr = judgeReference(ref.record, *r.owners, *r.now, res, "");
        TruthEval te;
        te.elems = rr.expected;
        te.merged = rr.merged;
        const Verdict v = rr.judgement.verdict;
        r.out.push_back({ref, res, std::move(rr), te, v});
    }
    return r;
}

// The oracle's verdict on another answer to the same reference (the full judge, new-territory rule included).
Judgement judgeOther(const Run& r, const Outcome& o, Answer answer, const std::vector<std::uint32_t>& elements)
{
    const Expectation x = expect(o.ref.record, *r.owners, *r.now);
    return judgeAnswer(x, o.ref.record, *r.owners, *r.now, answer, elements);
}

// Every element of the edited shape on the reference's logical element (its description without separators).
std::vector<std::uint32_t> logicalElements(const Run& r, const Outcome& o)
{
    return expect(o.ref.record, *r.owners, *r.now).logical;
}

// No wrong verdict at all, none unjudged, no unreviewed acceptance, every mutant caught, and no usable Repaired.
void checkNoneWrong(const Run& r)
{
    for (const Outcome& o : r.out) {
        INFO("reference " << o.ref.record.base.index << " kind " << static_cast<int>(o.ref.ref.kind) << " ["
                          << o.ref.record.truth.cat << "]: " << verdictName(o.verdict) << "; resolver "
                          << o.resolved.reason << "; accepted: " << o.result.judgement.accepted);
        CHECK_FALSE(isWrong(o.verdict));
        CHECK(o.verdict != Verdict::Unjudged);
        CHECK(o.result.judgement.accepted.empty());
        CHECK(o.result.selfCheck.empty());
        for (const RefResult::Mutant& m : o.result.mutants) {
            INFO("mutant " << m.cls << ": " << m.detail);
            CHECK(m.caught);
        }
        if (o.resolved.state == naming::ResolveState::Repaired)
            CHECK_FALSE(o.resolved.usable());
    }
}

// Does the reference's description name this owner (in any of its constraints)?
bool about(const Outcome& o, const std::string& owner)
{
    for (const Constraint& c : o.ref.record.truth.cons)
        if (std::find(c.alts.begin(), c.alts.end(), owner) != c.alts.end())
            return true;
    return false;
}

// A face reference whose description is exactly the one owner (with any separators).
bool faceOf(const Outcome& o, const std::string& owner)
{
    return o.ref.ref.kind == SubKind::Face && o.ref.record.truth.cons.size() == 1 &&
           o.ref.record.truth.cons[0].alts == std::vector<std::string>{owner};
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
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto& out = r.out;
    for (const Outcome& o : out) {
        INFO("reference " << o.ref.record.base.index << " [" << o.ref.record.truth.cat << "]");
        CHECK(o.resolved.state == naming::ResolveState::Exact);
        CHECK(o.verdict == Verdict::Correct);
    }
}

TEST_CASE("Acceptance: a cut that splits a referenced face gives Split with both pieces", "[naming][acceptance]")
{
    const Script base = block(100, 60, 20);
    Script edited = base;
    edited.push_back(sweep("N1", BOp::Cut, top(30), rect(40, -5, 60, 10), 40));
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto& out = r.out;
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
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto& out = r.out;
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
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto& out = r.out;
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
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto& out = r.out;
    const auto pieces = [](const Outcome& o) {
        return faceOf(o, "F01/s4") && !o.ref.record.truth.cons[0].seps.empty();
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
    const Run r = run(script(45), script(30));
    checkNoneWrong(r);
    const auto& out = r.out;
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
    const Run r = run(script(8), script(0));
    checkNoneWrong(r);
    const auto& out = r.out;
    const auto lowerWall = [](const Outcome& o) {
        return about(o, "S1/s1");
    };
    REQUIRE(count(out, lowerWall) >= 5);
    for (const Outcome& o : out)
        if (lowerWall(o)) {
            INFO("reference " << o.ref.record.base.index << " [" << o.ref.record.truth.cat << "]");
            CHECK(o.expected.elems.empty());
            CHECK(o.resolved.state == naming::ResolveState::Broken);
        }
}

// ---------------------------------------------------------------- strict-judge scenarios (T-081a review)
namespace {

std::vector<std::uint32_t> without(const std::vector<std::uint32_t>& all, const std::vector<std::uint32_t>& some)
{
    std::vector<std::uint32_t> out;
    std::set_difference(all.begin(), all.end(), some.begin(), some.end(), std::back_inserter(out));
    return out;
}

std::string text(const std::vector<std::uint32_t>& v)
{
    std::string s;
    for (const std::uint32_t e : v)
        s += " " + std::to_string(e);
    return s;
}

// Every reference about `owner`, in words (for a failing count).
std::string refsAbout(const Run& r, const std::string& owner)
{
    std::string s;
    for (const Outcome& o : r.out) {
        if (!about(o, owner))
            continue;
        s += "\n  kind " + std::to_string(static_cast<int>(o.ref.ref.kind)) + " #" +
             std::to_string(o.ref.record.base.index) + " [" + o.ref.record.truth.cat + "]";
        for (const Constraint& c : o.ref.record.truth.cons) {
            s += " (";
            for (const std::string& a : c.alts)
                s += a + " ";
            s += std::to_string(c.seps.size()) + " seps)";
        }
    }
    return s;
}

// The resolver's answer lies inside the oracle's expectation (or is Broken), and the answers that take the separate
// parts of the logical element are judged wrong.
void checkSeparateParts(const Run& r, const Outcome& o)
{
    const std::vector<std::uint32_t> all = logicalElements(r, o);
    const std::vector<std::uint32_t> others = without(all, o.expected.elems);
    INFO("reference " << o.ref.record.base.index << " [" << o.ref.record.truth.cat << "]: expected"
                      << text(o.expected.elems) << ", logical" << text(all) << "; resolver " << o.resolved.reason);
    REQUIRE_FALSE(o.expected.elems.empty());
    REQUIRE_FALSE(others.empty());
    CHECK(isWrong(judgeOther(r, o, Answer::Split, all).verdict));
    for (const std::uint32_t e : others)
        CHECK(isWrong(judgeOther(r, o, Answer::Exact, {e}).verdict));
    for (const SubId& e : o.resolved.elements)
        CHECK(std::binary_search(o.expected.elems.begin(), o.expected.elems.end(), e.index));
}

} // namespace

TEST_CASE("Strict judge: a cross hole lengthened through a bore; its exit loop is not a piece of the entry loop",
          "[naming][acceptance]")
{
    // A bore of 20 along x through a 60 x 40 x 40 block, and a cross hole of 8 from the top that ends inside the
    // bore; then the cross hole is lengthened through the bottom. The cross hole now meets the bore twice.
    auto script = [](double depth) {
        Script s = block(60, 40, 40);
        Frame side;
        side.o = {61, 0, 0};
        side.n = {1, 0, 0};
        side.xd = {0, 1, 0};
        s.push_back(sweep("B1", BOp::Cut, side, circle(20, 20, 10), 62, "hole"));
        s.push_back(sweep("X1", BOp::Cut, top(40), circle(30, 20, 4), depth, "hole"));
        return s;
    };
    const Run r = run(script(15), script(45));
    checkNoneWrong(r);
    const auto entry = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && about(o, "B1/s1") && about(o, "X1/s1");
    };
    REQUIRE(count(r.out, entry) >= 1);
    for (const Outcome& o : r.out)
        if (entry(o))
            checkSeparateParts(r, o);
    // The cross hole's wall: its new lower part is not connected to the picked upper part.
    const auto wall = [](const Outcome& o) {
        return faceOf(o, "X1/s1");
    };
    REQUIRE(count(r.out, wall) == 1);
    for (const Outcome& o : r.out)
        if (wall(o))
            checkSeparateParts(r, o);
}

TEST_CASE("Strict judge: a hole breaking out of a side as two lines; moved, each line keeps its own",
          "[naming][acceptance]")
{
    // A 100 x 60 x 20 block, a through hole of 10 at y = 4: it breaks out of the y = 0 side as two vertical lines.
    auto script = [](double x, double y) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("H1", BOp::Cut, top(20), circle(x, y, 5), 40, "hole"));
        return s;
    };
    for (const auto& [x, y] : {std::pair{52.0, 4.0}, std::pair{50.0, 2.0}}) {
        INFO("hole moved to " << x << ", " << y);
        const Run r = run(script(50, 4), script(x, y));
        checkNoneWrong(r);
        const auto line = [](const Outcome& o) {
            return o.ref.ref.kind == SubKind::Edge && about(o, "H1/s1") && about(o, "F01/s1") &&
                   o.ref.record.truth.cons.size() == 2;
        };
        INFO("references about the hole:" << refsAbout(r, "H1/s1"));
        REQUIRE(count(r.out, line) == 2);
        for (const Outcome& o : r.out)
            if (line(o)) {
                CHECK(o.expected.elems.size() == 1);
                checkSeparateParts(r, o);
            }
    }
}

TEST_CASE("Strict judge: where a slot floor meets a fillet torus in two arcs, one arc is not the other",
          "[naming][acceptance]")
{
    // A plate with a boss of radius 10 and a concave fillet of 3 at its foot (a torus), then a slot across the boss
    // whose floor cuts the fillet: two arcs, one each side of the boss. The slot is then made shallower.
    auto script = [](double floor) {
        Script s = block(100, 100, 10);
        s.push_back(sweep("B1", BOp::Join, top(10), circle(50, 50, 10), 10));
        Feature f;
        f.id = "R1";
        f.type = FType::Fillet;
        f.role = "fillet";
        f.targets = {Target{"B1/s1", "F01/c1/1", false, true}};
        f.size = 3;
        s.push_back(f);
        s.push_back(sweep("S1", BOp::Cut, top(30), rect(20, 47, 80, 53), 30 - floor));
        return s;
    };
    const Run r = run(script(11.5), script(12));
    checkNoneWrong(r);
    const auto arc = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && about(o, "S1/c1/1") && about(o, "R1/b0");
    };
    INFO("references about the slot floor:" << refsAbout(r, "S1/c1/1"));
    REQUIRE(count(r.out, arc) >= 2);
    for (const Outcome& o : r.out)
        if (arc(o))
            checkSeparateParts(r, o);
}

TEST_CASE("Strict judge: a hole wall cut in two by a cross slot, hole moved: each piece is its own",
          "[naming][acceptance]")
{
    // A through hole and a slot through the block from the y = 0 side at mid-height: the hole wall is in an upper
    // and a lower piece. The hole then moves along the slot.
    auto script = [](double x) {
        Script s = block(100, 60, 30);
        s.push_back(sweep("H1", BOp::Cut, top(30), circle(x, 30, 5), 40, "hole"));
        Frame side;
        side.o = {0, 0, 0};
        side.n = {0, -1, 0};
        side.xd = {1, 0, 0};
        s.push_back(sweep("C1", BOp::Cut, side, rect(30, 12, 70, 18), 70));
        return s;
    };
    const Run r = run(script(50), script(53));
    checkNoneWrong(r);
    const auto piece = [](const Outcome& o) {
        return faceOf(o, "H1/s1");
    };
    REQUIRE(count(r.out, piece) == 2);
    for (const Outcome& o : r.out)
        if (piece(o)) {
            CHECK(o.expected.elems.size() == 1);
            checkSeparateParts(r, o);
        }
}

TEST_CASE("Strict judge: a face that a lengthened slot cuts in two needs both pieces", "[naming][acceptance]")
{
    // A pocket open to the y = 0 side that stops short of the far side; lengthened, it cuts the top face in two.
    auto script = [](double y1) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("S1", BOp::Cut, top(20), rect(40, -5, 60, y1), 5));
        return s;
    };
    const Run r = run(script(40), script(65));
    checkNoneWrong(r);
    const auto topFace = [](const Outcome& o) {
        return faceOf(o, "F01/c1/1");
    };
    REQUIRE(count(r.out, topFace) == 1);
    for (const Outcome& o : r.out)
        if (topFace(o)) {
            REQUIRE(o.expected.elems.size() == 2);
            for (const std::uint32_t e : o.expected.elems)
                CHECK(isWrong(judgeOther(r, o, Answer::Exact, {e}).verdict));
            CHECK(judgeOther(r, o, Answer::Split, o.expected.elems).verdict == Verdict::Correct);
            CHECK(
                (o.resolved.state == naming::ResolveState::Split || o.resolved.state == naming::ResolveState::Broken));
        }
}

// ---------------------------------------------------------------- regressions of the Linux findings (T-081a)
// Sequences 6:6, 6:21 and 7:28 of the randomised test gave wrong-element answers on Linux only (B-rep results differ
// in the last bits between platforms, so the random paths differ). The geometry of 7:28 (a resized boss's wall/cap
// edge appearing in a second, separate piece) is rebuilt below; without the resolver's Split check
// (piecesLieAsPicked) the resolver answers Split with the separate piece, on every platform.
namespace {

// A rounded rectangle: tags 1 bottom (y = y0), 2 right, 3 top, 4 left as rect(), corner arcs 5 to 8.
Loop roundedRect(double x0, double y0, double x1, double y1, double r)
{
    return Loop{1,
                {line(x0 + r, y0, x1 - r, y0, 1), arc(x1 - r, y0, x1, y0 + r, x1 - r, y0 + r, r, 5),
                 line(x1, y0 + r, x1, y1 - r, 2), arc(x1, y1 - r, x1 - r, y1, x1 - r, y1 - r, r, 6),
                 line(x1 - r, y1, x0 + r, y1, 3), arc(x0 + r, y1, x0, y1 - r, x0 + r, y1 - r, r, 7),
                 line(x0, y1 - r, x0, y0 + r, 4), arc(x0, y0 + r, x0 + r, y0, x0 + r, y0 + r, r, 8)}};
}

} // namespace

TEST_CASE("A fillet that now runs on round tangent corners keeps only its own face", "[naming][acceptance]")
{
    // The fillet of the block's front top edge is one face. With rounded corners in the block's profile, the same
    // fillet runs on along the tangent corners: the faces it makes there (tori, and the other sides' cylinders) are
    // not pieces of the picked face, which lies on the front edge's cylinder. (The kind of answer Linux sequence 6:21
    // gave: the family rule split a fillet face's name over faces of another surface. Its exact geometry depends on
    // the last bits of the B-rep and could not be rebuilt here; the cross-hole scenario above fails without the
    // check that fixed it, on every platform.)
    auto script = [](double corner) {
        Script s{
            sweep("F01", BOp::New, top(0), corner > 0 ? roundedRect(0, 0, 100, 60, corner) : rect(0, 0, 100, 60), 20)};
        Feature f;
        f.id = "R1";
        f.type = FType::Fillet;
        f.role = "fillet";
        f.targets = {Target{"F01/s1", "F01/c1/1", true, true}};
        if (corner > 0)
            for (const char* side : {"F01/s5", "F01/s8"})
                f.targets.push_back(Target{side, "F01/c1/1", true, false});
        f.size = 2;
        s.push_back(f);
        return s;
    };
    const Run r = run(script(0), script(10));
    checkNoneWrong(r);
    const auto fillet = [](const Outcome& o) {
        return faceOf(o, "R1/b0");
    };
    REQUIRE(count(r.out, fillet) == 1);
    for (const Outcome& o : r.out)
        if (fillet(o)) {
            INFO("resolver " << o.resolved.reason);
            for (const SubId& e : o.resolved.elements)
                CHECK(std::binary_search(o.expected.elems.begin(), o.expected.elems.end(), e.index));
        }
}

TEST_CASE("Regression (Linux 7:28): a boss widened over a second opening; the cap edge there is not a piece",
          "[naming][acceptance]")
{
    // A block with two pockets in its x = 0 side, and a boss on that side over the first one: where the boss's
    // bottom cap spans the opening, its lower wall meets the cap in an edge. Widened over the second opening, the
    // same wall and cap meet there too, in a separate edge that was never part of the picked one.
    auto script = [](double width) {
        Script s = block(100, 60, 40);
        Frame side;
        side.o = {0, 0, 0};
        side.xd = {0, -1, 0};
        side.n = {-1, 0, 0};
        s.push_back(sweep("P1", BOp::Cut, side, rect(-20, 10, -10, 30), 20));
        s.push_back(sweep("P2", BOp::Cut, side, rect(-50, 10, -40, 30), 20));
        s.push_back(sweep("B1", BOp::Join, side, rect(-5 - width, 15, -5, 25), 10));
        return s;
    };
    const Run r = run(script(20), script(50));
    checkNoneWrong(r);
    const auto capEdge = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && about(o, "B1/s1") && about(o, "B1/c0/1");
    };
    INFO("references about the boss's lower wall:" << refsAbout(r, "B1/s1"));
    REQUIRE(count(r.out, capEdge) == 1);
    for (const Outcome& o : r.out)
        if (capEdge(o))
            checkSeparateParts(r, o);
}

// ---------------------------------------------------------------- regressions of the T-081a gate (naming errors)
// Each scenario rebuilds, on a block, the kind of wrong-element answer the randomised test found on a corpus part, and
// fails without the naming rule that fixed it: the oracle judges the old answer wrong, and the resolver must not give
// it.
namespace {

Feature blendOn(const std::string& id, bool fillet, Target t, double size)
{
    Feature f;
    f.id = id;
    f.type = fillet ? FType::Fillet : FType::Chamfer;
    f.role = fillet ? "fillet" : "chamfer";
    f.targets = {std::move(t)};
    f.size = size;
    return f;
}

// The wrong answer `bad` is judged wrong by the oracle, the resolver's answer is not, and it is not `bad`.
void checkNotAnswered(const Run& r, const Outcome& o, Answer bad, const std::vector<std::uint32_t>& badElements)
{
    INFO("reference " << o.ref.record.base.index << " [" << o.ref.record.truth.cat << "]: resolver "
                      << static_cast<int>(o.resolved.state) << " (" << o.resolved.reason << "), oracle expects"
                      << text(o.expected.elems));
    CHECK(isWrong(judgeOther(r, o, bad, badElements).verdict));
    CHECK_FALSE(isWrong(o.verdict));
    std::vector<std::uint32_t> got;
    for (const SubId& e : o.resolved.elements)
        got.push_back(e.index);
    CHECK_FALSE((answerOf(o.resolved.state) == bad && got == badElements));
}

} // namespace

TEST_CASE("Regression (random 11:12): a Split piece beyond a pocket that bounded the picked face is not a piece",
          "[naming][acceptance][t081a-gate]")
{
    // The chamfer of the block's front top edge, cut short at x = 90 by a pocket over the right end. The pocket then
    // moves to x = 80..96: the chamfer is in two pieces, and the piece beyond the pocket (x 96..100) lies where the
    // pocket was when the chamfer was picked. Without the rule that a Split piece must not lie beyond a face of a
    // feature that bounded the picked element (facePiecesStayInside), the resolver answers Split with both pieces.
    auto script = [](double x0) {
        Script s = block(100, 60, 20);
        s.push_back(blendOn("R1", false, Target{"F01/s1", "F01/c1/1", true, true}, 2));
        s.push_back(sweep("P1", BOp::Cut, top(20), rect(x0, -5, x0 + 16, 10), 5));
        return s;
    };
    const Run r = run(script(90), script(80));
    checkNoneWrong(r);
    const auto chamfer = [](const Outcome& o) {
        return faceOf(o, "R1/b0");
    };
    REQUIRE(count(r.out, chamfer) == 1);
    for (const Outcome& o : r.out)
        if (chamfer(o)) {
            const std::vector<std::uint32_t> all = logicalElements(r, o);
            REQUIRE(all.size() == 2);
            checkNotAnswered(r, o, Answer::Split, all);
        }
}

TEST_CASE("Regression (random 10:34): a piece whose face is whole again round a notch where the piece was is not "
          "Merged into it",
          "[naming][acceptance][t081a-gate]")
{
    // A slot through the block's right end splits its x = 100 side into a piece y 0..38 and a piece y 40..60. With the
    // slot suppressed and a notch over the second piece's place (y 39..61, from the top down to 0.3 mm above the
    // bottom), the side is one face again, L-shaped: its box still holds the second piece's centre (y 50, z 10), the
    // face does not, and only a strip 0.3 mm high of the piece's place is left on it. Without the outline check in
    // canContain, the split-parent rule answers Merged with the whole side.
    Script base = block(100, 60, 20);
    base.push_back(sweep("A1", BOp::Cut, top(20), rect(90, 38, 105, 40), 25));
    Script edited = base;
    edited[1].suppressed = true;
    edited.push_back(sweep("N1", BOp::Cut, top(20), rect(90, 39, 105, 61), 19.7));
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto middle = [&](const Outcome& o) {
        if (!faceOf(o, "F01/s2"))
            return false;
        const ElemSamples& p = o.ref.record.picked();
        return p.boxMin.y > 39;
    };
    INFO("references about the right side:" << refsAbout(r, "F01/s2"));
    REQUIRE(count(r.out, middle) == 1);
    for (const Outcome& o : r.out)
        if (middle(o)) {
            const std::vector<std::uint32_t> side = logicalElements(r, o);
            REQUIRE(side.size() == 1);
            checkNotAnswered(r, o, Answer::Merged, side);
        }
}

TEST_CASE("Regression (random 6:21): an edge piece that only touches the picked edge's end is not a piece of it",
          "[naming][acceptance][t081a-gate]")
{
    // The block's front top edge, cut short at x = 90 by a pocket over its right end, is one edge. The pocket then
    // moves to x = 74..90: the edge is in two pieces, and the right one starts exactly where the picked edge ended.
    // Without the check that a piece shares a stretch of the picked segment (onHintCarrier), the resolver answers
    // Split with both pieces.
    auto script = [](double x0) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("P1", BOp::Cut, top(20), rect(x0, -5, x0 + 16, 10), 5));
        return s;
    };
    const Run r = run(script(90), script(74));
    checkNoneWrong(r);
    const auto edge = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && o.ref.record.truth.cons.size() == 2 && about(o, "F01/s1") &&
               about(o, "F01/c1/1");
    };
    INFO("references about the front top edge:" << refsAbout(r, "F01/s1"));
    REQUIRE(count(r.out, edge) == 1);
    for (const Outcome& o : r.out)
        if (edge(o)) {
            const std::vector<std::uint32_t> all = logicalElements(r, o);
            REQUIRE(all.size() == 2);
            checkNotAnswered(r, o, Answer::Split, all);
        }
}

TEST_CASE("Regression (holdout 18:5): an arc on the far side of a moved hole's rim is not a piece of the picked arc",
          "[naming][acceptance][t081a-gate]")
{
    // A groove across the top (y 25.5..27.5) cuts the south of a hole's top rim (radius 5, centre y 31) off: the rim
    // is one arc of 268 degrees. The hole then moves to y 29.8 and the groove widens to y 29.8: the rim is a north arc
    // (0..180 degrees), where the picked arc went, and a south arc (239..301), where the picked arc had its gap; the
    // two are together shorter than the picked arc and lie within the circle's diameter of each other. Without the
    // check that the pieces of a moved arc fit in an arc no longer than the picked one (piecesLieAsPicked), the
    // resolver answers Split with both.
    auto script = [](double cy, double groove) {
        Script s = block(100, 60, 20);
        // Sketched with its x axis along -y, so the hole wall's seam faces south, inside the groove: the north arc is
        // one edge.
        Frame south = top(20);
        south.xd = {0, -1, 0};
        s.push_back(sweep("H1", BOp::Cut, south, circle(-cy, 50, 5), 25, "hole"));
        s.push_back(sweep("G1", BOp::Cut, top(20), rect(-5, 25.5, 105, groove), 3));
        return s;
    };
    const Run r = run(script(31, 27.5), script(29.8, 29.8));
    checkNoneWrong(r);
    const auto rim = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && about(o, "H1/s1") && about(o, "F01/c1/1");
    };
    REQUIRE(count(r.out, rim) == 1);
    for (const Outcome& o : r.out)
        if (rim(o)) {
            const std::vector<std::uint32_t> all = logicalElements(r, o);
            REQUIRE(all.size() >= 2); // the south arc may be in two at the seam
            checkNotAnswered(r, o, Answer::Split, all);
        }
}

TEST_CASE("Regression (random 5:43): a micron stub left of a cut-away edge is not the edge",
          "[naming][acceptance][t081a-gate]")
{
    // A cut removes the block's front right corner up to 0.0008 mm below the top: of the vertical corner edge only a
    // stub of 0.0008 mm is left, and it keeps the edge's name. Without the stub check in the Exact stage, the
    // resolver answers Exact with the stub.
    Script base = block(100, 60, 20);
    Script edited = base;
    edited.push_back(sweep("X1", BOp::Cut, top(20 - 0.0008), rect(90, -5, 105, 10), 25));
    const Run r = run(base, edited);
    checkNoneWrong(r);
    const auto corner = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && o.ref.record.truth.cons.size() == 2 && about(o, "F01/s1") &&
               about(o, "F01/s2");
    };
    REQUIRE(count(r.out, corner) == 1);
    for (const Outcome& o : r.out)
        if (corner(o)) {
            INFO("resolver " << o.resolved.reason);
            CHECK(o.resolved.state == naming::ResolveState::Broken);
            // What is left on the corner is a sliver to the oracle too (no sample 0.02 mm inside it).
            for (const std::uint32_t e : logicalElements(r, o))
                CHECK(isSliver(r.now->of(SubKind::Edge)[e], SubKind::Edge));
        }
}

TEST_CASE("Oracle: a face that took in a coplanar face of another feature is merged (known naming blind spot)",
          "[naming][acceptance][oracle-gate]")
{
    // A boss under the block and a slot cut up into it to the block's bottom: inside the slot, the block's bottom
    // plane shows as the slot's ceiling, a face of the slot. With the boss suppressed, the slot cuts nothing and the
    // bottom face runs on over the ceiling's place: it took in a face that existed when it was picked, and the oracle
    // expects it merged (Exact on it is a silent merge, Merged is right). The resolver cannot tell this from a
    // dimension change that makes the face larger (which must stay Exact, T-096) without the picked face's territory
    // in the reference: it answers Exact here (T-081a random 6:29 and 6:22, holdout2 13:4). Only the oracle is checked.
    Script base = block(100, 60, 20);
    Frame under; // sketch y runs along -y
    under.n = {0, 0, -1};
    base.push_back(sweep("B1", BOp::Join, under, rect(30, -40, 70, -20), 10));
    base.push_back(sweep("S1", BOp::Cut, top(0), rect(40, 25, 60, 35), 15));
    Script edited = base;
    edited[1].suppressed = true;
    const Run r = run(base, edited);
    const auto bottom = [](const Outcome& o) {
        return faceOf(o, "F01/c0/1");
    };
    INFO("references about the bottom:" << refsAbout(r, "F01/c0/1"));
    REQUIRE(count(r.out, bottom) >= 1);
    for (const Outcome& o : r.out)
        if (bottom(o) && o.expected.merged) {
            CHECK(isWrong(judgeOther(r, o, Answer::Exact, o.expected.elems).verdict));
            CHECK(judgeOther(r, o, Answer::Merged, o.expected.elems).verdict == Verdict::Correct);
        }
    CHECK(count(r.out, [&](const Outcome& o) { return bottom(o) && o.expected.merged; }) >= 1);
}

TEST_CASE("Regression (random 9:41): the pieces of an edge trade territory when the slot between them moves",
          "[naming][acceptance][t081a-gate]")
{
    // A slot across the block's front top edge splits it into a left and a right piece, told apart by the slot walls
    // they end on. Moved left by more than its width, the slot leaves the left piece short and gives most of its old
    // stretch to the right piece; each piece still ends on the same wall. Without the rule that an edge among
    // siblings is trusted only while its geometry is unchanged (needsHint), both resolve Exact: one incomplete, one a
    // silent merge.
    auto script = [](double x0) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("S1", BOp::Cut, top(20), rect(x0, -5, x0 + 10, 10), 5));
        return s;
    };
    const Run r = run(script(40), script(10));
    checkNoneWrong(r);
    const auto piece = [](const Outcome& o) {
        return o.ref.ref.kind == SubKind::Edge && o.ref.record.truth.cons.size() == 2 && about(o, "F01/s1") &&
               about(o, "F01/c1/1");
    };
    INFO("references about the front top edge:" << refsAbout(r, "F01/s1"));
    REQUIRE(count(r.out, piece) == 2);
    for (const Outcome& o : r.out)
        if (piece(o)) {
            INFO("reference " << o.ref.record.base.index << ": resolver " << o.resolved.reason);
            CHECK(o.resolved.state != naming::ResolveState::Exact);
            for (const SubId& e : o.resolved.elements)
                CHECK(isWrong(judgeOther(r, o, Answer::Exact, {e.index}).verdict));
        }
}

// ---------------------------------------------------------------- oracle capabilities of the T-081a gate
// Each new judging rule of the oracle on a scripted case: the answer the rule now accepts is judged correct, and a
// wrong answer in the same situation is still judged wrong.

TEST_CASE("Oracle: a face that runs on over empty space is not merged; over a sibling's territory it is",
          "[naming][acceptance][oracle-gate]")
{
    // New territory (expect, ownTerritory): the top face with a hole in it, the hole then suppressed: the face runs on
    // over the hole's place, which was nothing when it was picked. Exact is right.
    {
        Script base = block(100, 60, 20);
        base.push_back(sweep("H1", BOp::Cut, top(20), circle(50, 30, 5), 40, "hole"));
        Script edited = base;
        edited[1].suppressed = true;
        const Run r = run(base, edited);
        checkNoneWrong(r);
        const auto topFace = [](const Outcome& o) {
            return faceOf(o, "F01/c1/1");
        };
        REQUIRE(count(r.out, topFace) == 1);
        for (const Outcome& o : r.out)
            if (topFace(o)) {
                REQUIRE(o.expected.elems.size() == 1);
                CHECK_FALSE(o.expected.merged);
                CHECK(judgeOther(r, o, Answer::Exact, o.expected.elems).verdict == Verdict::Correct);
                CHECK(judgeOther(r, o, Answer::Merged, o.expected.elems).verdict == Verdict::Correct);
                // Negative: any other face is wrong.
                const std::uint32_t other = o.expected.elems[0] == 0 ? 1 : 0;
                CHECK(isWrong(judgeOther(r, o, Answer::Exact, {other}).verdict));
            }
    }
    // Negative: a slot across the top splits it into a left and a right piece; with the slot suppressed the left
    // piece's face also covers the right piece's territory. Exact on it is a silent merge; Merged is right.
    {
        Script base = block(100, 60, 20);
        base.push_back(sweep("S1", BOp::Cut, top(20), rect(40, -5, 60, 65), 5));
        Script edited = base;
        edited[1].suppressed = true;
        const Run r = run(base, edited);
        checkNoneWrong(r);
        const auto leftPiece = [](const Outcome& o) {
            return faceOf(o, "F01/c1/1") && o.ref.record.picked().boxMax.x < 41;
        };
        REQUIRE(count(r.out, leftPiece) == 1);
        for (const Outcome& o : r.out)
            if (leftPiece(o)) {
                REQUIRE(o.expected.elems.size() == 1);
                CHECK(o.expected.merged);
                CHECK(judgeOther(r, o, Answer::Exact, o.expected.elems).verdict == Verdict::WrongSilentMerge);
                CHECK(judgeOther(r, o, Answer::Merged, o.expected.elems).verdict == Verdict::Correct);
            }
    }
}

TEST_CASE("Oracle: a resized pocket carries its walls' meeting lines with it", "[naming][acceptance][oracle-gate]")
{
    // The edit map of a sweep scaled about its sketch origin (mapElement, profileScale): the line where a straight wall
    // of a rounded pocket meets its corner arc, tangent, cannot be found by projecting onto the two walls; scaled with
    // the pocket it is. The edge at the same wall's other end is still wrong.
    auto script = [](double k) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("P1", BOp::Cut, top(20), roundedRect(30 * k, 20 * k, 70 * k, 40 * k, 5 * k), 5));
        return s;
    };
    const Run r = run(script(1.0), script(0.8));
    checkNoneWrong(r);
    const auto tangent = [](const Outcome& o, const char* arc) {
        return o.ref.ref.kind == SubKind::Edge && o.ref.record.truth.cons.size() == 2 && about(o, "P1/s1") &&
               about(o, arc);
    };
    REQUIRE(count(r.out, [&](const Outcome& o) { return tangent(o, "P1/s5"); }) == 1);
    REQUIRE(count(r.out, [&](const Outcome& o) { return tangent(o, "P1/s8"); }) == 1);
    std::vector<std::uint32_t> e5, e8;
    for (const Outcome& o : r.out) {
        if (tangent(o, "P1/s5"))
            e5 = o.expected.elems;
        if (tangent(o, "P1/s8"))
            e8 = o.expected.elems;
    }
    REQUIRE(e5.size() == 1);
    REQUIRE(e8.size() == 1);
    CHECK(e5 != e8);
    for (const Outcome& o : r.out)
        if (tangent(o, "P1/s5")) {
            CHECK(o.result.noAnchor.empty());
            CHECK(judgeOther(r, o, Answer::Exact, e5).verdict == Verdict::Correct);
            CHECK(isWrong(judgeOther(r, o, Answer::Exact, e8).verdict));
        }
}

TEST_CASE("Oracle: a small face unified into a large one is continued by it, merged",
          "[naming][acceptance][oracle-gate]")
{
    // An element that holds the mapped pick continues it whatever the 2 % floor of evalTruth (expect, containers): a
    // boss 2 x 2 mm on the top, 1 mm behind the front face, moved flush with it: its front face (20 mm^2) is unified
    // into the block's front face (2,000 mm^2). Merged is right; Exact (it is larger) and any other face are wrong.
    auto script = [](double y0) {
        Script s = block(100, 60, 20);
        s.push_back(sweep("B1", BOp::Join, top(20), rect(10, y0, 12, y0 + 2), 10));
        return s;
    };
    const Run r = run(script(1), script(0));
    checkNoneWrong(r);
    const auto front = [](const Outcome& o) {
        return faceOf(o, "B1/s1");
    };
    REQUIRE(count(r.out, front) == 1);
    for (const Outcome& o : r.out)
        if (front(o)) {
            REQUIRE(o.expected.elems.size() == 1);
            CHECK(o.expected.merged);
            CHECK(judgeOther(r, o, Answer::Merged, o.expected.elems).verdict == Verdict::Correct);
            CHECK(isWrong(judgeOther(r, o, Answer::Exact, o.expected.elems).verdict));
            const std::uint32_t other = o.expected.elems[0] == 0 ? 1 : 0;
            CHECK(isWrong(judgeOther(r, o, Answer::Merged, {other}).verdict));
        }
}

TEST_CASE("Oracle: the exposed start face of a boss moved along the face it stands on moves with the boss",
          "[naming][acceptance][oracle-gate]")
{
    // A face whose every owner is one sweep that moved moves with that sweep, though its own carrier did not change
    // (mapElement): a boss on the front face that hangs 4 mm below the block shows its start face below the block;
    // moved 20 mm along x, the start face shows 20 mm further on, and is the continuation. The front face of the
    // block, on the same plane, is still wrong.
    auto script = [](double x0) {
        Script s = block(100, 60, 20);
        Frame front; // sketch on the front face, extruded along -y; sketch y runs along +z
        front.n = {0, -1, 0};
        front.xd = {1, 0, 0};
        s.push_back(sweep("B1", BOp::Join, front, rect(x0, -4, x0 + 10, 6), 8));
        return s;
    };
    const Run r = run(script(20), script(40));
    checkNoneWrong(r);
    const auto cap = [](const Outcome& o) {
        return faceOf(o, "B1/c0/1");
    };
    REQUIRE(count(r.out, cap) == 1);
    for (const Outcome& o : r.out)
        if (cap(o)) {
            CHECK(o.result.noAnchor.empty());
            REQUIRE(o.expected.elems.size() == 1);
            CHECK(judgeOther(r, o, Answer::Exact, o.expected.elems).verdict == Verdict::Correct);
            for (const std::uint32_t e : logicalElements(r, o))
                if (e != o.expected.elems[0])
                    CHECK(isWrong(judgeOther(r, o, Answer::Exact, {e}).verdict));
        }
}
