// Conflicting, redundant and partially redundant constraints are identified by id (task T-090): PlaneGCS reports int
// tags, the adapter maps them back to the sketch's Uuids.
#include "sketch_test_util.h"

#include "solver_internal.h"

#include <catch2/catch_test_macros.hpp>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;
using CT = ConstraintType;

namespace {

SolveResult solveIt(const Sketch& s)
{
    JobContext ctx;
    return take(makePlaneGcsSolver()->solve(s, ctx));
}

bool contains(const std::vector<Uuid>& v, const Uuid& id)
{
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

TEST_CASE("Two different radii on one circle conflict, and both are named", "[sketch][diagnosis]")
{
    Builder b;
    const auto c = b.circle(b.point(0, 0), 5);
    const auto fix = b.constrain(CT::Fix, {b.s.entities.front().id});
    const auto r5 = b.constrain(CT::Radius, {c}, 5);
    const auto r6 = b.constrain(CT::Radius, {c}, 6);
    const SolveResult r = solveIt(b.s);
    CHECK(r.status == SolveStatus::Conflicting);
    CHECK(contains(r.conflicting, r5));
    CHECK(contains(r.conflicting, r6));
    CHECK_FALSE(contains(r.conflicting, fix));
    CHECK(r.redundant.empty());
    // A conflicting sketch is returned as it was, and cannot be dragged.
    CHECK(coordinates(r.solved) == coordinates(b.s));
    JobContext ctx;
    auto drag = makePlaneGcsSolver()->beginDrag(b.s, b.s.entities.front().id, ctx);
    REQUIRE_FALSE(drag.has_value());
    CHECK(drag.error().code == ErrorCode::Overconstrained);
}

TEST_CASE("A redundant horizontal is named and the sketch still solves", "[sketch][diagnosis]")
{
    // Spike S7's case: a horizontal on a line that is already parallel to a horizontal line. PlaneGCS diagnoses at the
    // given coordinates (rank of the Jacobian there), so the geometry is the solved one, as a document stores it.
    Builder b;
    const auto a = b.point(0, 0), c = b.point(10, 0), d = b.point(0.3, 5), e = b.point(10, 5);
    const auto bottom = b.line(a, c), top = b.line(d, e);
    b.constrain(CT::Fix, {a});
    const auto h1 = b.constrain(CT::Horizontal, {bottom});
    const auto par = b.constrain(CT::Parallel, {top, bottom});
    const auto h2 = b.constrain(CT::Horizontal, {top});
    const SolveResult r = solveIt(b.s);
    CHECK(r.status == SolveStatus::OverConstrained);
    REQUIRE(r.redundant.size() == 1);
    CHECK((r.redundant[0] == h2 || r.redundant[0] == par || r.redundant[0] == h1));
    CHECK(r.redundant[0] == h2); // PlaneGCS proposes the most recently added of equals (canonical order)
    CHECK(r.conflicting.empty());
    CHECK(r.partiallyRedundant.empty());
    CHECK(maxError(r.solved, b.s) <= 1e-9);
}

TEST_CASE("A fix that is only partly redundant is reported as partially redundant", "[sketch][diagnosis]")
{
    // Two fixed lines share a point: two of the second fix's four rows repeat the first fix.
    Builder b;
    const auto pa = b.point(0, 0), pb = b.point(10, 0), pc = b.point(10, 10), pd = b.point(0, 12);
    const auto l1 = b.line(pa, pb), l2 = b.line(pb, pc), l3 = b.line(pc, pd);
    const auto f1 = b.constrain(CT::Fix, {l1});
    const auto f2 = b.constrain(CT::Fix, {l2});
    b.constrain(CT::Distance, {l3}, 11);
    const SolveResult r = solveIt(b.s);
    CHECK(r.status == SolveStatus::OverConstrained);
    CHECK(r.redundant.empty());
    REQUIRE(r.partiallyRedundant.size() == 1);
    CHECK((r.partiallyRedundant[0] == f2 || r.partiallyRedundant[0] == f1));
    CHECK(r.freeDof == 1);
    CHECK(maxError(r.solved, b.s) <= 1e-9);
}

TEST_CASE("Fixing an arc takes exactly its five degrees of freedom", "[sketch][diagnosis]")
{
    Builder b;
    const auto c = b.point(0, 0), s = b.point(3, 0), e = b.point(0, 3);
    const auto arc = b.arc(c, s, e);
    b.constrain(CT::Fix, {arc});
    const SolveResult r = solveIt(b.s);
    CHECK(r.status == SolveStatus::FullyConstrained);
    CHECK(r.freeDof == 0);
    CHECK(r.redundant.empty());
    CHECK(r.partiallyRedundant.empty());
}

TEST_CASE("Degrees of freedom are counted", "[sketch][diagnosis]")
{
    Builder b;
    const auto p = b.point(1, 1);
    CHECK(solveIt(b.s).freeDof == 2);
    const auto q = b.point(5, 2);
    const auto l = b.line(p, q);
    b.constrain(CT::Fix, {p});
    CHECK(solveIt(b.s).freeDof == 2);
    b.constrain(CT::Distance, {l}, 4);
    CHECK(solveIt(b.s).freeDof == 1);
    b.constrain(CT::Angle, {l}, 0.5);
    const SolveResult r = solveIt(b.s);
    CHECK(r.freeDof == 0);
    CHECK(r.status == SolveStatus::FullyConstrained);
    // An empty sketch is trivially solved.
    CHECK(solveIt(Sketch{}).status == SolveStatus::FullyConstrained);
}
