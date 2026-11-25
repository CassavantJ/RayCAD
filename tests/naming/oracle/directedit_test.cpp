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

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
    // A drilling operation's geometry is the bore wall (cam/templates.h); it posts only what is postable without review.
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
