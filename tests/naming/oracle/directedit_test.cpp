// The scripted acceptance scenarios of T-081b: references followed through the direct edits of T-080 (kernel
// deleteFaces, offsetFaces, resizeHole, unifySameDomain), judged by the full oracle (oracle.h; direct edits are
// modelled on its owners, EditSpec). Each scenario builds a scripted part, records every reference the oracle vouches
// for, appends one direct edit (or edits a script that has one), replays, and resolves and judges every reference:
// no answer may be wrong, none unjudged, every mutant of the oracle caught, no Repaired usable; and the references
// each scenario is about must get the state the plan asks for:
//  - deleting an unrelated fillet leaves every other reference Exact;
//  - resizing a hole that a drilling operation references keeps that reference Exact (postable without review);
//  - pulling a face so that a neighbour comes apart gives Split with both pieces;
//  - pushing a face until it unifies with another gives Merged;
//  - deleting a referenced face gives Broken.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"

#include <naming_testing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
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
Frame top(double z)
{
    Frame f;
    f.o = {0, 0, z};
    return f;
}
// The front face of a block (y = 0, outward -y): sketch x along x, sketch y along z; a Cut goes in along +y.
Frame front()
{
    Frame f;
    f.o = {0, 0, 0};
    f.n = {0, -1, 0};
    f.xd = {1, 0, 0};
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
Feature directEdit(const std::string& id, EKind kind, std::vector<std::string> targets, double amount = 0)
{
    Feature f;
    f.id = id;
    f.type = FType::Edit;
    f.role = kind == EKind::Delete   ? "delete_face"
             : kind == EKind::Offset ? "offset_face"
             : kind == EKind::Resize ? "resize_hole"
                                     : "unify";
    f.edit.kind = kind;
    f.edit.targets = std::move(targets);
    f.edit.amount = amount;
    return f;
}
Script block(double L, double W, double H)
{
    return {sweep("F01", BOp::New, top(0), rect(0, 0, L, W), H)};
}

struct Outcome {
    Ref ref;
    naming::Resolved resolved;
    RefResult result;
    Verdict verdict = Verdict::Correct;
};

struct Run {
    std::vector<Outcome> out;
    std::vector<PickOutcome> picks; // the replayed direct edits' own picks, judged
    std::vector<std::string> failed;
    std::shared_ptr<const Samples> samples; // the edited shape's, for probing answers (judgeAnswer)
    std::shared_ptr<const Owners> owners;
};

// Builds `base`, records every reference the oracle vouches for, builds `edited`, resolves and judges them all.
Run run(const Script& base, const Script& edited)
{
    FeatRefs picks;
    const Builder b0 = buildScript(base, Mode::Author, picks);
    for (const std::string& f : b0.failed())
        std::printf("  baseline failure: %s\n", f.c_str());
    REQUIRE(b0.failed().empty());
    auto s0 = sampleShape(b0.shape());
    REQUIRE(s0.has_value());
    const auto ow0 = std::make_shared<const Owners>(buildOwners(b0.builtScript()));
    const std::vector<Ref> refs = recordReferences(b0, ow0, *s0, nullptr);
    REQUIRE(!refs.empty());
    const Builder b1 = buildScript(edited, Mode::Replay, picks);
    Run r;
    r.failed = b1.failed();
    for (const std::string& f : b1.failed())
        std::printf("  replay failure: %s\n", f.c_str());
    auto s1 = sampleShape(b1.shape());
    REQUIRE(s1.has_value());
    const Owners owners = buildOwners(b1.builtScript());
    r.samples = *s1;
    r.owners = std::make_shared<const Owners>(owners);
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), b1.map(), b1.shape()}});
    const bool verbose = std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr;
    const std::string edges = verbose ? dumpElements(b1, **s1, SubKind::Edge) : std::string();
    for (const Ref& ref : refs) {
        const naming::Resolved res = resolver->resolve(ref.ref);
        RefResult rr = judgeReference(ref.record, owners, **s1, res, "");
        const Verdict v = rr.judgement.verdict;
        // RAYCAD_NAMING_VERBOSE: the names of an edge that was missed (Broken, though it exists), and of what exists.
        if (verbose && v == Verdict::Missed && ref.ref.kind == SubKind::Edge) {
            std::printf("  missed edge %u: picked names [%s]\n", ref.record.base.index, ref.names.c_str());
            for (const std::uint32_t e : rr.expected) {
                const std::string tag = "    edge " + std::to_string(e) + " ";
                const auto at = edges.find(tag);
                if (at != std::string::npos)
                    std::printf("  now %s\n", edges.substr(at, edges.find('\n', at) - at).c_str());
            }
        }
        r.out.push_back({ref, res, std::move(rr), v});
    }
    r.picks = b1.picks();
    return r;
}

const char* stateText(naming::ResolveState s)
{
    switch (s) {
    case naming::ResolveState::Exact:
        return "Exact";
    case naming::ResolveState::Split:
        return "Split";
    case naming::ResolveState::Merged:
        return "Merged";
    case naming::ResolveState::Repaired:
        return "Repaired";
    case naming::ResolveState::Broken:
        return "Broken";
    }
    return "?";
}

// No wrong verdict at all, none unjudged, no unreviewed acceptance, every mutant caught, and no usable Repaired; the
// same for the replayed edits' own picks. Prints the states, for the report.
void checkNoneWrong(const Run& r, const char* title)
{
    std::array<int, 5> states{};
    for (const Outcome& o : r.out) {
        ++states[static_cast<std::size_t>(answerOf(o.resolved.state))];
        INFO("reference " << o.ref.record.base.index << " kind " << static_cast<int>(o.ref.ref.kind) << " ["
                          << o.ref.record.truth.cat << "]: " << verdictName(o.verdict) << "; resolver "
                          << stateText(o.resolved.state) << " " << o.resolved.reason
                          << "; accepted: " << o.result.judgement.accepted);
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
    for (const PickOutcome& p : r.picks) {
        INFO("pick of " << p.feature << " face " << p.edge << ": " << verdictName(p.result.judgement.verdict));
        CHECK_FALSE(isWrong(p.result.judgement.verdict));
        CHECK(p.result.judgement.verdict != Verdict::Unjudged);
    }
    std::printf("%s: %zu references: Exact %d, Split %d, Merged %d, Repaired %d, Broken %d; %zu replayed picks\n",
                title, r.out.size(), states[0], states[1], states[2], states[3], states[4], r.picks.size());
    // RAYCAD_NAMING_VERBOSE: every answer that is not Exact, with the oracle's verdict and the resolver's reason.
    if (std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr)
        for (const Outcome& o : r.out)
            if (o.resolved.state != naming::ResolveState::Exact)
                std::printf("    %s %u [%s] %s: %s; %s\n", o.ref.ref.kind == SubKind::Face ? "face" : "edge/vertex",
                            o.ref.record.base.index, o.ref.record.truth.cat.c_str(), stateText(o.resolved.state),
                            verdictName(o.verdict), o.resolved.reason.c_str());
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

// The common part: a block with a blind hole and a pocket, both away from its back right corner.
Script holeAndPocket()
{
    Script s = block(100, 60, 20);
    s.push_back(sweep("H1", BOp::Cut, top(20), circle(30, 30, 5), 12, "hole"));
    s.push_back(sweep("P1", BOp::Cut, top(20), rect(10, 40, 40, 52), 5));
    return s;
}

} // namespace

TEST_CASE("Direct edit: deleting an unrelated fillet leaves every other reference Exact", "[naming][directedit]")
{
    // The fillet rounds the block's back right vertical edge, away from the hole and the pocket.
    Script base = holeAndPocket();
    base.push_back(blendOn("R1", true, Target{"F01/s2", "F01/s3", true, true}, 4.0));
    Script edited = base;
    edited.push_back(directEdit("E1", EKind::Delete, {"R1/b0"}));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    checkNoneWrong(r, "delete an unrelated fillet");
    REQUIRE(count(r.out, [](const Outcome& o) { return faceOf(o, "R1/b0"); }) == 1);
    for (const Outcome& o : r.out) {
        INFO("reference " << o.ref.record.base.index << " [" << o.ref.record.truth.cat << "] " << o.ref.names << ": "
                          << stateText(o.resolved.state) << " " << o.resolved.reason);
        if (about(o, "R1/b0"))
            CHECK(o.resolved.state == naming::ResolveState::Broken);
        else
            CHECK(o.resolved.state == naming::ResolveState::Exact);
    }
}

TEST_CASE("Direct edit: resizing a hole that a drilling operation references keeps the reference Exact",
          "[naming][directedit]")
{
    // A drilling operation's geometry is the bore wall (cam/templates.h); it posts only what is postable without
    // review.
    for (const double radius : {7.0, 3.5}) {
        DYNAMIC_SECTION("to radius " << radius)
        {
            const Script base = holeAndPocket();
            Script edited = base;
            edited.push_back(directEdit("E1", EKind::Resize, {"H1/s1"}, radius));
            const Run r = run(base, edited);
            REQUIRE(r.failed.empty());
            checkNoneWrong(r, radius > 5 ? "resize a hole larger" : "resize a hole smaller");
            REQUIRE(count(r.out, [](const Outcome& o) { return faceOf(o, "H1/s1"); }) == 1);
            for (const Outcome& o : r.out)
                if (faceOf(o, "H1/s1")) {
                    CHECK(o.resolved.state == naming::ResolveState::Exact);
                    CHECK(o.resolved.postableWithoutReview());
                    CHECK(o.resolved.elements.size() == 1);
                    CHECK(o.verdict == Verdict::Correct);
                }
            // Nothing else about the hole is lost: its floor stays Exact too.
            for (const Outcome& o : r.out)
                if (faceOf(o, "H1/c1/1"))
                    CHECK(o.resolved.state == naming::ResolveState::Exact);
        }
    }
}

TEST_CASE("Direct edit: pulling a face so that a neighbour comes apart gives Split with both pieces",
          "[naming][directedit]")
{
    // A notch in the front face, open at the bottom, leaves a bridge of the front face above it (z 15 to 20). Pulling
    // the top face in by 7 mm takes the bridge away: the front face is in two pieces either side of the notch.
    Script base = block(100, 60, 20);
    base.push_back(sweep("N1", BOp::Cut, front(), rect(40, -5, 60, 15), 10));
    Script edited = base;
    edited.push_back(directEdit("E1", EKind::Offset, {"F01/c1/1"}, -7.0));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    checkNoneWrong(r, "pull a face so that a neighbour splits");
    REQUIRE(count(r.out, [](const Outcome& o) { return faceOf(o, "F01/s1"); }) == 1);
    for (const Outcome& o : r.out)
        if (faceOf(o, "F01/s1")) {
            CHECK(o.resolved.state == naming::ResolveState::Split);
            CHECK(o.resolved.elements.size() == 2);
            CHECK(o.result.expected.size() == 2);
            CHECK(o.verdict == Verdict::Correct);
        }
    // The top face itself moved and stays itself; the notch's ceiling is gone.
    for (const Outcome& o : r.out) {
        if (faceOf(o, "F01/c1/1"))
            CHECK(o.resolved.state == naming::ResolveState::Exact);
        if (faceOf(o, "N1/s3"))
            CHECK(o.resolved.state == naming::ResolveState::Broken);
    }
}

TEST_CASE("Direct edit: pushing a face until it unifies with another gives Merged", "[naming][directedit]")
{
    // A step 8 mm down at the right end; pushing its floor up 8 mm makes it flush with the top: one face.
    Script base = block(100, 60, 20);
    base.push_back(sweep("C1", BOp::Cut, top(20), rect(60, -5, 105, 65), 8));
    Script edited = base;
    edited.push_back(directEdit("E1", EKind::Offset, {"C1/c1/1"}, 8.0));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    checkNoneWrong(r, "push a face until two faces unify");
    REQUIRE(count(r.out, [](const Outcome& o) { return faceOf(o, "C1/c1/1"); }) == 1);
    for (const Outcome& o : r.out) {
        if (faceOf(o, "C1/c1/1")) {
            CHECK(o.resolved.state == naming::ResolveState::Merged);
            CHECK(o.resolved.elements.size() == 1);
            CHECK(o.result.merged);
            CHECK(o.verdict == Verdict::Correct);
        }
        // The riser between the two levels is gone.
        if (faceOf(o, "C1/s4"))
            CHECK(o.resolved.state == naming::ResolveState::Broken);
    }
}

TEST_CASE("Direct edit: deleting a referenced face gives Broken", "[naming][directedit]")
{
    // A hole is removed by any face of it (the kernel takes the whole hole); a pocket by all its faces (its floor alone
    // would be removed and its walls run on through the part: a through pocket, whose walls are still the walls).
    const Script base = holeAndPocket();
    const std::vector<std::vector<std::string>> picks{{"H1/s1"}, {"P1/c1/1", "P1/s1", "P1/s2", "P1/s3", "P1/s4"}};
    for (const std::vector<std::string>& targets : picks) {
        const std::string target = targets.front();
        DYNAMIC_SECTION("delete " << target)
        {
            const std::string feature = target.substr(0, 2);
            Script edited = base;
            edited.push_back(directEdit("E1", EKind::Delete, targets));
            const Run r = run(base, edited);
            REQUIRE(r.failed.empty());
            checkNoneWrong(r, target[0] == 'H' ? "delete a hole" : "delete a pocket");
            REQUIRE(count(r.out, [&](const Outcome& o) { return faceOf(o, target); }) == 1);
            for (const Outcome& o : r.out) {
                INFO("reference " << o.ref.record.base.index << " [" << o.ref.record.truth.cat << "] " << o.ref.names
                                  << ": " << stateText(o.resolved.state) << " " << o.resolved.reason);
                if (faceOf(o, target)) {
                    CHECK(o.resolved.state == naming::ResolveState::Broken);
                    CHECK(o.result.expected.empty());
                    CHECK(o.verdict == Verdict::BrokenGone);
                }
                // Every face of the removed feature goes with it.
                bool ofFeature = false;
                for (const Constraint& c : o.ref.record.truth.cons)
                    for (const std::string& a : c.alts)
                        ofFeature = ofFeature || a.compare(0, feature.size() + 1, feature + "/") == 0;
                // And nothing else is lost.
                CHECK(o.resolved.state == (ofFeature ? naming::ResolveState::Broken : naming::ResolveState::Exact));
            }
        }
    }
}

TEST_CASE("Oracle (round 3): a hole opened up over a smaller one at the same depth merges their floors",
          "[naming][directedit]")
{
    // The independent reviewer's adversarial case for the "flat bottom widens" rule (EditSpec::held): a block 100 x 60
    // x 20, hole H1 r 5 at (30, 30) and hole H2 r 1 at (37.5, 30), both 12 deep; H1 opened up to r 9 swallows H2, and
    // the one floor left holds both floors' ground. H1's floor answered Exact on it is a silent merge (WRONG); the
    // floor's growth over the ring takes in only ground nobody held, never H2's floor.
    Script base = block(100, 60, 20);
    base.push_back(sweep("H1", BOp::Cut, top(20), circle(30, 30, 5), 12, "hole"));
    base.push_back(sweep("H2", BOp::Cut, top(20), circle(37.5, 30, 1), 12, "hole"));
    Script edited = base;
    edited.push_back(directEdit("E1", EKind::Resize, {"H1/s1"}, 9.0));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    REQUIRE(r.samples != nullptr);
    // The one face on the floor plane (z 8, facing up) that now holds H1's and H2's floors.
    const auto& F = r.samples->of(SubKind::Face);
    std::vector<std::uint32_t> floors;
    for (std::uint32_t i = 0; i < F.size(); ++i)
        if (F[i].type == static_cast<int>(kernel::SurfaceType::Plane) && F[i].nrm.z > 0.99 &&
            std::abs(F[i].boxMin.z - 8.0) < 1e-6 && std::abs(F[i].boxMax.z - 8.0) < 1e-6)
            floors.push_back(i);
    REQUIRE(floors.size() == 1);
    const std::uint32_t floor = floors.front();
    REQUIRE(F[floor].boxMax.x > 38.0); // it reaches over H2's old floor
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (!faceOf(o, "H1/c1/1") && !faceOf(o, "H2/c1/1"))
            continue;
        ++probed;
        INFO("reference to " << o.ref.record.truth.cat << " " << o.ref.names << ": resolver "
                             << stateText(o.resolved.state) << ", " << verdictName(o.verdict));
        const Expectation x = expect(o.ref.record, *r.owners, *r.samples);
        INFO(
            "H1's floor: " << faceOf(o, "H1/c1/1") << "; E " << x.expected.elems.size() << " merged "
                           << x.expected.merged << " merge unknown " << x.mergeUnknown << " weak " << x.weak
                           << " no anchor '" << x.noAnchor << "'; Exact judged "
                           << verdictName(
                                  judgeAnswer(x, o.ref.record, *r.owners, *r.samples, Answer::Exact, {floor}).verdict));
        // Exact on the joined floor: a silent merge, for either floor.
        CHECK(isWrong(judgeAnswer(x, o.ref.record, *r.owners, *r.samples, Answer::Exact, {floor}).verdict));
        // Flagged Merged on it: right.
        CHECK(judgeAnswer(x, o.ref.record, *r.owners, *r.samples, Answer::Merged, {floor}).verdict == Verdict::Correct);
    }
    CHECK(probed == 2);
}

TEST_CASE("Feature edit (round 3, macOS 8:18): a hole moved by its own width is not repaired onto its other crossing",
          "[naming][directedit]")
{
    // The randomised block's 8:18 as macOS built it, scripted: a pocket's wall (x 30, z 5..20) through the axis of a
    // blind hole bored along y (r 2 at z 4) meets the hole's wall along one line (z 6; the other crossing, z 2, is
    // below the pocket's floor), named alone. The hole moved up by its width (4 mm) meets the wall along two lines, z 6
    // and z 10, siblings told apart only by ordinal: the picked name is gone, and the hole's lower line lies where the
    // picked one was. The reference must not be repaired onto it (a congruence repair onto another element of the
    // pick's own feature after the feature moved): Broken, or Exact on the line at z 10.
    Script base = block(100, 60, 20);
    base.push_back(sweep("P1", BOp::Cut, top(20), rect(30, -1, 60, 61), 15));
    base.push_back(
        sweep("H1", BOp::Cut, front(), circle(30, 4, 2), 30, "hole")); // blind: the wall is notched, not cut in two
    Script edited = base;
    edited[2].frame.o = add(edited[2].frame.o, Vec3{0, 0, 4});
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    std::size_t lines = 0;
    for (const Outcome& o : r.out) {
        if (o.ref.ref.kind != SubKind::Edge || !about(o, "H1/s1") || !about(o, "P1/s4") ||
            o.ref.record.truth.cons.size() != 2)
            continue;
        ++lines;
        INFO("reference " << o.ref.record.base.index << " " << o.ref.names << ": " << stateText(o.resolved.state) << " "
                          << o.resolved.reason << "; " << verdictName(o.verdict));
        CHECK(o.resolved.state != naming::ResolveState::Repaired);
        CHECK_FALSE(isWrong(o.verdict));
    }
    CHECK(lines >= 1);
}

TEST_CASE("Direct edit: an edit in the history follows its face through an upstream edit", "[naming][directedit]")
{
    // The hole is opened up by a direct edit; then the hole itself is moved upstream. The edit's own face reference
    // resolves Exact on the replay, the edit runs again, and the opened-up wall is still the referenced wall.
    Script base = holeAndPocket();
    base.push_back(directEdit("E1", EKind::Resize, {"H1/s1"}, 7.0));
    Script edited = base;
    edited[1].frame.o = add(edited[1].frame.o, Vec3{8, -6, 0});
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    checkNoneWrong(r, "a direct edit replayed after an upstream move");
    REQUIRE(r.picks.size() == 1);
    CHECK(r.picks.front().resolved.state == naming::ResolveState::Exact);
    REQUIRE(count(r.out, [](const Outcome& o) { return faceOf(o, "H1/s1"); }) == 1);
    for (const Outcome& o : r.out)
        if (faceOf(o, "H1/s1")) {
            CHECK(o.resolved.state == naming::ResolveState::Exact);
            CHECK(o.verdict == Verdict::Correct);
        }
}

namespace {

// The element of `kind` of `r`'s edited shape nearest point p (its samples' polyline, mesh or point), or -1.
long elementNear(const Run& r, SubKind kind, const Vec3& p)
{
    const auto& N = r.samples->of(kind);
    long best = -1;
    double dBest = 1e-2;
    for (std::uint32_t e = 0; e < N.size(); ++e) {
        double d = 1e300;
        if (kind == SubKind::Face) {
            if (N[e].mesh)
                d = N[e].mesh->distanceWithin(p, 1e-2, true);
        } else if (kind == SubKind::Edge) {
            for (std::size_t k = 1; k < N[e].nodes.size(); ++k) {
                const Vec3 a = N[e].nodes[k - 1], ab = sub(N[e].nodes[k], a);
                const double l2 = dot(ab, ab);
                const double s = l2 > 0 ? std::clamp(dot(sub(p, a), ab) / l2, 0.0, 1.0) : 0.0;
                d = std::min(d, norm(sub(p, add(a, mul(ab, s)))));
            }
        } else if (!N[e].nodes.empty()) {
            d = norm(sub(p, N[e].nodes.front()));
        }
        if (d < dBest) {
            dBest = d;
            best = static_cast<long>(e);
        }
    }
    return best;
}

Verdict judgeAs(const Run& r, const Outcome& o, Answer answer, std::vector<std::uint32_t> elements)
{
    const Expectation x = expect(o.ref.record, *r.owners, *r.samples);
    return judgeAnswer(x, o.ref.record, *r.owners, *r.samples, answer, std::move(elements)).verdict;
}

} // namespace

TEST_CASE("Structural check (round 4): an edge on a line that stayed, the pick's stretch gone, is not the other end",
          "[naming][directedit]")
{
    // The reviewer's holdout1 4:33 edge 9 and holdout2 1:3 edge 42, scripted: a pocket open at the front (y -10..20,
    // seen from y 0) is moved along its own walls by 60 mm (y 50..80, seen to y 60). Its right wall stays on the plane
    // x 60, and its edge with the top on the line x 60, z 20; the picked stretch (y 0..20) is top face now, the moved
    // image (y 60..80) is outside the part, and the edge there (y 50..60) holds neither: Broken (the resolver: another
    // stretch of a line that stayed), the expectation gone, and Exact on it WRONG (the structural check on edges).
    Script base = block(100, 60, 20);
    base.push_back(sweep("P1", BOp::Cut, top(20), rect(40, -10, 60, 20), 10));
    Script edited = base;
    edited[1].frame.o = add(edited[1].frame.o, Vec3{0, 60, 0});
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    const long other = elementNear(r, SubKind::Edge, Vec3{60, 55, 20});
    REQUIRE(other >= 0);
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (o.ref.ref.kind != SubKind::Edge || !about(o, "P1/s2") || !about(o, "F01/c1/1") ||
            o.ref.record.truth.cons.size() != 2)
            continue;
        ++probed;
        INFO("reference " << o.ref.record.base.index << " " << o.ref.names << ": " << stateText(o.resolved.state) << " "
                          << o.resolved.reason << "; " << verdictName(o.verdict));
        CHECK(o.resolved.state == naming::ResolveState::Broken);
        CHECK(o.verdict == Verdict::BrokenGone);
        CHECK(isWrong(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(other)})));
        CHECK(isWrong(judgeAs(r, o, Answer::Merged, {static_cast<std::uint32_t>(other)})));
    }
    CHECK(probed == 1);
}

TEST_CASE("Resize (round 4, the lead's decision): a bore opened past a counterbore took the counterbore's ground",
          "[naming][directedit]")
{
    // Review holdout 18:33 face 2, scripted: a bore r 2, 15 deep, under a counterbore r 4, 4 deep; the bore opened up
    // to
    // r 5 runs on to the top, through the heights the counterbore's floor and wall held. The bore's reference is the
    // bore
    // and more: Merged (the resolver), Exact on it a silent merge (WRONG), Merged on it right.
    Script base = block(100, 60, 20);
    base.push_back(sweep("H1", BOp::Cut, top(20), circle(30, 30, 2), 15, "hole"));
    base.push_back(sweep("C1", BOp::Cut, top(20), circle(30, 30, 4), 4, "hole"));
    Script edited = base;
    edited.push_back(directEdit("E1", EKind::Resize, {"H1/s1"}, 5.0));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    const long bore = elementNear(r, SubKind::Face, Vec3{35, 30, 10});
    REQUIRE(bore >= 0);
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (!faceOf(o, "H1/s1"))
            continue;
        ++probed;
        INFO("reference " << o.ref.names << ": " << stateText(o.resolved.state) << " " << o.resolved.reason << "; "
                          << verdictName(o.verdict));
        CHECK(o.resolved.state == naming::ResolveState::Merged);
        CHECK(o.verdict == Verdict::Correct);
        const Expectation x = expect(o.ref.record, *r.owners, *r.samples);
        INFO("E " << x.expected.elems.size() << " merged " << x.expected.merged << " partial "
                  << x.expected.partial.size() << " merge unknown " << x.mergeUnknown << " weak " << x.weak << " moved "
                  << x.moved << "; Exact judged "
                  << verdictName(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(bore)})));
        CHECK(isWrong(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(bore)})));
        CHECK(judgeAs(r, o, Answer::Merged, {static_cast<std::uint32_t>(bore)}) == Verdict::Correct);
    }
    CHECK(probed == 1);
}

TEST_CASE("Sibling match (round 4): a picked piece that came apart is all its pieces, not the one at its rank",
          "[naming][directedit]")
{
    // Review holdout 15:10 edge 8, scripted: a slot across the front top edge leaves it in two pieces, and a chamfer on
    // that logical edge has two pieces, each with an edge on the top (siblings). The chamfer made larger slides those
    // edges; a groove inserted across the right piece cuts its top edge in two. The right piece's top edge is now both
    // stretches: Split with both is right, either stretch alone WRONG (incomplete); the left piece's edge goes on
    // alone, as the sibling's continuation.
    Script base = block(100, 60, 20);
    base.push_back(sweep("S1", BOp::Cut, top(20), rect(40, -1, 50, 10), 5));
    base.push_back(blendOn("C1", false, Target{"F01/c1/1", "F01/s1", true, true}, 0.6));
    Script edited = base;
    edited[2].size = 0.9;
    edited.push_back(sweep("X1", BOp::Cut, top(20), rect(70, -1, 72, 61), 3));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    const long a = elementNear(r, SubKind::Edge, Vec3{60, 0.9, 20});
    const long b = elementNear(r, SubKind::Edge, Vec3{85, 0.9, 20});
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    REQUIRE(a != b);
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (o.ref.ref.kind != SubKind::Edge || !about(o, "F01/c1/1") || !about(o, "C1/b0") ||
            o.ref.record.picked().boxMin.x < 45)
            continue;
        ++probed;
        INFO("reference " << o.ref.record.base.index << " " << o.ref.names << ": " << stateText(o.resolved.state) << " "
                          << o.resolved.reason << "; " << verdictName(o.verdict));
        const auto ua = static_cast<std::uint32_t>(a), ub = static_cast<std::uint32_t>(b);
        CHECK_FALSE(isWrong(o.verdict));
        CHECK(judgeAs(r, o, Answer::Split, {ua, ub}) == Verdict::Correct);
        CHECK(isWrong(judgeAs(r, o, Answer::Exact, {ua})));
        CHECK(isWrong(judgeAs(r, o, Answer::Exact, {ub})));
    }
    CHECK(probed == 1);
}

TEST_CASE("Structural check (round 4): an edge moved along its line with its feature holds the moved pick",
          "[naming][directedit]")
{
    // The counterpart of the test above (tuned 10:17 edge 45, holdout1 10:5 edge 8): a pocket open at the front (y
    // -10..10,
    // seen from y 0) moved along its own walls by 25 mm (y 15..35). Its right wall's edge with the top lies on the same
    // line, nowhere where it was picked (y 0..10), but the pick moved with the pocket (y 25..35) lies on it: the
    // pick moved with its feature, Exact and judged correct (the oracle's structural check follows the sweep's move,
    // its
    // own map projecting the crossing of a moved wall with the top that stayed; the resolver follows the pocket's
    // placement).
    Script base = block(100, 60, 20);
    base.push_back(sweep("P1", BOp::Cut, top(20), rect(40, -10, 60, 10), 10));
    Script edited = base;
    edited[1].frame.o = add(edited[1].frame.o, Vec3{0, 25, 0});
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    const long moved = elementNear(r, SubKind::Edge, Vec3{60, 30, 20});
    REQUIRE(moved >= 0);
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (o.ref.ref.kind != SubKind::Edge || !about(o, "P1/s2") || !about(o, "F01/c1/1") ||
            o.ref.record.truth.cons.size() != 2)
            continue;
        ++probed;
        INFO("reference " << o.ref.record.base.index << " " << o.ref.names << ": " << stateText(o.resolved.state) << " "
                          << o.resolved.reason << "; " << verdictName(o.verdict));
        CHECK(o.resolved.state == naming::ResolveState::Exact);
        CHECK(o.verdict == Verdict::Correct);
        CHECK(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(moved)}) == Verdict::Correct);
    }
    CHECK(probed == 1);
}

TEST_CASE(
    "Rotation (round 5, the reviewer's case): a boss turned 90 degrees about a far point is followed, its twin is "
    "not",
    "[naming][directedit]")
{
    // The reviewer's round-3 adversarial case (rotation-exact): two equal bosses 10 x 6 on a block; B1 turned 90
    // degrees about a point away from it (its sketch frame's x along y, its origin moved from (15, 25) to (35, 15)), so
    // that its top lands elsewhere, B2 left. The turned top is B1's top: Exact on it right; Exact on B2's top (a
    // congruent twin) WRONG, and on the block's top too. With rotation-exact off, the turned feature's map counts as
    // exact, the structural check finds the right answer holds none of the mapped pick, and calls it a ghost.
    Script base = block(100, 60, 10);
    Frame f1 = top(10);
    f1.o = {15, 25, 10};
    Frame f2 = top(10);
    f2.o = {45, 25, 10};
    base.push_back(sweep("B1", BOp::Join, f1, rect(-5, -3, 5, 3), 5));
    base.push_back(sweep("B2", BOp::Join, f2, rect(-5, -3, 5, 3), 5));
    Script edited = base;
    edited[1].frame.xd = {0, 1, 0};
    edited[1].frame.o = {35, 15, 10};
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    const long turned = elementNear(r, SubKind::Face, Vec3{35, 15, 15});
    const long twin = elementNear(r, SubKind::Face, Vec3{45, 25, 15});
    const long plate = elementNear(r, SubKind::Face, Vec3{80, 50, 10});
    REQUIRE(turned >= 0);
    REQUIRE(twin >= 0);
    REQUIRE(plate >= 0);
    REQUIRE(turned != twin);
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (!faceOf(o, "B1/c1/1"))
            continue;
        ++probed;
        INFO("reference " << o.ref.names << ": " << stateText(o.resolved.state) << " " << o.resolved.reason << "; "
                          << verdictName(o.verdict));
        CHECK_FALSE(isWrong(o.verdict));
        CHECK(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(turned)}) == Verdict::Correct);
        CHECK(isWrong(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(twin)})));
        CHECK(isWrong(judgeAs(r, o, Answer::Exact, {static_cast<std::uint32_t>(plate)})));
    }
    CHECK(probed == 1);
}

TEST_CASE("Signatures (round 6): near-twin vertices cannot be reviewed, every edge has a real signature",
          "[naming][directedit]")
{
    // The reviewer's collision scan (round 5): vertex pairs 5..19 um apart had signatures sameAnswer cannot tell apart,
    // and every edge without samples read edge(0 0 0 0). Scripted: a pocket whose wall stands 0.01 mm inside the
    // block's left face puts two top vertices 10 um apart, (0, 0, 20) and (0.01, 0, 20). Both are near twins: an answer
    // on either is never counted as reviewed (signatureAmbiguous), whatever the list says. A vertex far from any other
    // is not. Every edge's signature carries its end points and a length.
    Script base = block(100, 60, 20);
    base.push_back(sweep("P1", BOp::Cut, top(20), rect(0.01, -1, 30, 61), 5));
    const Run r = run(base, base);
    REQUIRE(r.failed.empty());
    const auto& V = r.samples->of(SubKind::Vertex);
    long a = -1, b = -1, far = -1;
    for (std::uint32_t i = 0; i < V.size(); ++i) {
        if (V[i].nodes.empty())
            continue;
        const Vec3 p = V[i].nodes.front();
        if (norm(sub(p, Vec3{0, 0, 20})) < 1e-6)
            a = i;
        if (norm(sub(p, Vec3{0.01, 0, 20})) < 1e-6)
            b = i;
        if (norm(sub(p, Vec3{100, 60, 0})) < 1e-6)
            far = i;
    }
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    REQUIRE(far >= 0);
    const auto ua = static_cast<std::uint32_t>(a), ub = static_cast<std::uint32_t>(b),
               uf = static_cast<std::uint32_t>(far);
    const std::string sa = answerSignature(Answer::Exact, {SubId{SubKind::Vertex, ua}}, *r.samples);
    const std::string sb = answerSignature(Answer::Exact, {SubId{SubKind::Vertex, ub}}, *r.samples);
    INFO(sa << " / " << sb);
    CHECK(sameAnswer(sa, sb));                                           // the signature cannot tell them apart ...
    CHECK(signatureAmbiguous({SubId{SubKind::Vertex, ua}}, *r.samples)); // ... so neither is ever reviewed
    CHECK(signatureAmbiguous({SubId{SubKind::Vertex, ub}}, *r.samples));
    CHECK_FALSE(signatureAmbiguous({SubId{SubKind::Vertex, uf}}, *r.samples));
    const auto& E = r.samples->of(SubKind::Edge);
    for (std::uint32_t i = 0; i < E.size(); ++i) {
        INFO("edge " << i);
        CHECK(E[i].measure > 0);
        CHECK(norm(sub(E[i].end0, E[i].end1)) + E[i].measure > 0);
    }
}

TEST_CASE("Held in place (round 6, the reviewer's negative): a hole's top rim covered by a boss is not held",
          "[naming][directedit]")
{
    // A through hole H1 (r 5) in a 20 mm block, then a boss B3 joined over it: the top rim, where the block's top
    // F01/c1/1 met the hole's wall, is now the concave corner of a blind hole's ceiling (B3's base) on the same circle.
    // Both owners are on their carriers (the top plane is still there around the boss), so HELD IN PLACE took the
    // corner for the rim; but the face beside it now faces down, not up: it is not bounded as the rim was, and Exact
    // on it must not be judged correct (with the bounding test off, held-bounds, it is).
    Script base = block(100, 60, 20);
    base.push_back(sweep("H1", BOp::Cut, top(20), circle(30, 30, 5), 25, "hole"));
    Script edited = base;
    edited.push_back(sweep("B3", BOp::Join, top(20), rect(20, 20, 40, 40), 5));
    const Run r = run(base, edited);
    REQUIRE(r.failed.empty());
    // Every edge on the rim's circle now (the corner may be in pieces).
    std::vector<std::uint32_t> corners;
    const auto& E = r.samples->of(SubKind::Edge);
    for (std::uint32_t i = 0; i < E.size(); ++i) {
        bool on = !E[i].nodes.empty();
        for (const Vec3& p : E[i].nodes)
            on = on && std::abs(p.z - 20) < 1e-3 && std::abs(norm(sub(Vec3{p.x, p.y, 0}, Vec3{30, 30, 0})) - 5) < 1e-3;
        if (on)
            corners.push_back(i);
    }
    REQUIRE(!corners.empty());
    std::size_t probed = 0;
    for (const Outcome& o : r.out) {
        if (o.ref.ref.kind != SubKind::Edge || !about(o, "H1/s1") || !about(o, "F01/c1/1") ||
            o.ref.record.truth.cons.size() != 2)
            continue;
        ++probed;
        INFO("reference " << o.ref.names << ": " << stateText(o.resolved.state) << " " << o.resolved.reason << "; "
                          << verdictName(o.verdict));
        CHECK_FALSE(isWrong(o.verdict));
        for (const std::uint32_t c : corners) {
            INFO("corner edge " << c);
            CHECK(judgeAs(r, o, Answer::Exact, {c}) != Verdict::Correct);
            CHECK(judgeAs(r, o, Answer::Merged, {c}) != Verdict::Correct);
        }
        if (corners.size() >= 2)
            CHECK(judgeAs(r, o, Answer::Split, corners) != Verdict::Correct);
    }
    CHECK(probed >= 1);
}

TEST_CASE("Post-condition pin (round 8, the macOS holdout3 4:7 family): a name on the same line running the other way "
          "from the picked end is not the pick",
          "[naming][directedit]")
{
    // macOS holdout3 4:7 edge 26 (T-081a; the macOS generator's sequence, not built on Windows): a boss moved by its
    // own width, its other wall standing exactly where the trailing wall was; the pick's name survived on an edge of
    // the same curve running the other way from the picked end, sharing only that point with it: Exact there, with and
    // without placements. Scripted here: a cut from the front face whose profile is a Z (its left wall on x 40 at
    // z 5..10, its right wall on x 50 at z 10..15), moved by -10 along x: the right wall stands on x 40 at z 10..15
    // now, beside where the picked edge (the left wall's crossing with the front face, z 5..10) was, sharing its end
    // (40, 0, 10). This kernel names the two apart, so the macOS outcome is simulated: the reference's names, and the
    // names of the faces around it, are replaced by that edge's (its evidence kept). The post-condition must not answer
    // Exact there: with the cut's placement the moved pick lands on the moved left wall's edge (x 30); without (an old
    // reference, the move unknown) the picked end lands on that edge's end, which runs from it the other way. The
    // oracle judges Exact there WRONG; with the post-condition off the resolver answers it (the pin fails).
    auto zCut = [](double dx) {
        const double p[8][2] = {{40, 5}, {55, 5}, {55, 10}, {50, 10}, {50, 15}, {35, 15}, {35, 10}, {40, 10}};
        Loop l{1, {}};
        for (std::uint64_t i = 0; i < 8; ++i) {
            const auto& a = p[i];
            const auto& b = p[(i + 1) % 8];
            l.ents.push_back(line(a[0] + dx, a[1], b[0] + dx, b[1], i + 1));
        }
        Script s = block(100, 60, 20);
        s.push_back(sweep("C1", BOp::Cut, front(), l, 10));
        return s;
    };
    FeatRefs picks;
    const Builder b0 = buildScript(zCut(0), Mode::Author, picks);
    REQUIRE(b0.failed().empty());
    auto s0 = sampleShape(b0.shape());
    REQUIRE(s0.has_value());
    const auto ow0 = std::make_shared<const Owners>(buildOwners(b0.builtScript()));
    const std::vector<Ref> refs = recordReferences(b0, ow0, *s0, nullptr);
    const Builder b1 = buildScript(zCut(-10), Mode::Replay, picks);
    REQUIRE(b1.failed().empty());
    auto s1 = sampleShape(b1.shape());
    REQUIRE(s1.has_value());
    const Owners owners = buildOwners(b1.builtScript());
    // The edges of the edited shape by their ends.
    auto edgeFrom = [&](const Vec3& a, const Vec3& b) -> std::optional<SubId> {
        for (std::uint32_t i = 0; i < b1.shape().count(SubKind::Edge); ++i) {
            const auto c = kernel::curveInfo(b1.shape(), SubId{SubKind::Edge, i});
            if (c && c->type == kernel::CurveType::Line &&
                ((norm(sub(c->start, a)) < 1e-6 && norm(sub(c->end, b)) < 1e-6) ||
                 (norm(sub(c->start, b)) < 1e-6 && norm(sub(c->end, a)) < 1e-6)))
                return SubId{SubKind::Edge, i};
        }
        return std::nullopt;
    };
    const auto other = edgeFrom(Vec3{40, 0, 10}, Vec3{40, 0, 15});
    const auto moved = edgeFrom(Vec3{30, 0, 5}, Vec3{30, 0, 10});
    REQUIRE(other);
    REQUIRE(moved);
    const Ref* picked = nullptr;
    for (const Ref& r : refs) {
        const naming::Fingerprint& h = r.ref.hint;
        if (r.ref.kind == SubKind::Edge && std::abs(h.qCx * 1e-4 - 40) < 1e-3 && std::abs(h.qCy * 1e-4) < 1e-3 &&
            std::abs(h.qCz * 1e-4 - 7.5) < 1e-3 && std::abs(h.qArea * 1e-4 - 5) < 1e-3)
            picked = &r;
    }
    REQUIRE(picked != nullptr);
    REQUIRE_FALSE(picked->ref.samples.placements.empty()); // the cut's placement is known
    // The macOS naming: the pick's names on the other edge.
    naming::TopoRef forged = picked->ref;
    forged.names.clear();
    forged.families.clear();
    forged.ambiguous.clear();
    forged.splitParents.clear();
    for (const Hash256& n : b1.map().namesOf(*other)) {
        forged.names.push_back(n);
        const naming::TopoName* record = b1.map().record(n);
        forged.families.push_back(record != nullptr ? record->family : Hash256{});
        forged.ambiguous.push_back(false);
    }
    REQUIRE(!forged.names.empty());
    // (And the faces around it as the surviving name had them, so that the names alone agree with the element.)
    forged.neighbourNames.clear();
    for (const SubId& f : kernel::adjacent(b1.shape(), *other, SubKind::Face))
        for (const Hash256& n : b1.map().namesOf(f))
            forged.neighbourNames.push_back(n);
    std::sort(forged.neighbourNames.begin(), forged.neighbourNames.end());
    forged.neighbourNames.erase(std::unique(forged.neighbourNames.begin(), forged.neighbourNames.end()),
                                forged.neighbourNames.end());
    // The same without the placements (an old reference: the move not known, the samples where they were picked).
    naming::TopoRef unplaced = forged;
    unplaced.placements.clear();
    for (naming::SampleCarrier& c : unplaced.samples.carriers)
        c.owner = -1;
    unplaced.samples.neighbourOwners.clear();
    const auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), b1.map(), b1.shape()}});
    const Expectation x = expect(picked->record, owners, **s1);
    // The oracle: Exact on the other edge is WRONG, Exact on the moved edge is right.
    CHECK(isWrong(judgeAnswer(x, picked->record, owners, **s1, Answer::Exact, {other->index}).verdict));
    CHECK(judgeAnswer(x, picked->record, owners, **s1, Answer::Exact, {moved->index}).verdict == Verdict::Correct);
    for (const naming::TopoRef* ref : {&forged, &unplaced}) {
        const naming::Resolved res = resolver->resolve(*ref);
        INFO((ref == &forged ? "with placements: " : "without placements: ")
             << static_cast<int>(res.state) << " " << res.reason);
        CHECK_FALSE((res.state == naming::ResolveState::Exact && res.elements == std::vector<SubId>{*other}));
        CHECK_FALSE(isWrong(judgeReference(picked->record, owners, **s1, res, "").judgement.verdict));
        // The mutation check: without the post-condition the resolver answers Exact on the other edge.
        naming::testing::setPostconditionEnabled(false);
        const naming::Resolved off = resolver->resolve(*ref);
        naming::testing::setPostconditionEnabled(true);
        CHECK(off.state == naming::ResolveState::Exact);
        CHECK(off.elements == std::vector<SubId>{*other});
    }
}
