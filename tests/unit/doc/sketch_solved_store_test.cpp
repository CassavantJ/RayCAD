// A sketch stores the coordinates the solver gives: after every committed command that changes a sketch, solving the
// stored sketch again moves nothing, and saving and opening the document keeps exactly that shape. Otherwise the
// next solve (a new constraint, a dimension edit, a drag, a drawing step attached to the geometry, a reload) starts
// from coordinates the person no longer sees and can land on another solution: the sketch jumps.
//
// The commands are issued as the sketch mode issues them (the shape shown is the solve of what is stored; the
// constraint commands pass it as "solved"), and as a script or agent might (coordinates that do not satisfy the
// constraints), so the document keeps the rule whoever calls it.
#include "parametric_test_util.h"

#include "features/feature_props.h"
#include "features/sketch_feature.h"
#include "features/sketch_json.h"

#include <ray/sketch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;

namespace {

// How far a re-solve may move a stored point (mm) or radius: the solver's own acceptance is 1e-9 on the constraint
// errors.
constexpr double kStill = 1e-7;

std::string sk()
{
    return uuid('f', 2);
}

Vec2 sub(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
}
Vec2 unit(Vec2 v)
{
    const double l = std::hypot(v.x, v.y);
    return {v.x / l, v.y / l};
}
double distance(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

// The sketch mode's tangent arc fit (src/ui/sketch/sketch_geom.cpp, tangentArc): the arc from `start` leaving along
// `dir` through `end`.
struct ArcFit {
    Vec2 center;
    bool ccw = true;
};
ArcFit tangentArc(Vec2 start, Vec2 dir, Vec2 end)
{
    const Vec2 n{-dir.y, dir.x};
    const Vec2 w = sub(end, start);
    const double t = (w.x * w.x + w.y * w.y) / (2 * (w.x * n.x + w.y * n.y));
    return {Vec2{start.x + n.x * t, start.y + n.y * t}, t > 0};
}

// The model with its dimension expressions evaluated over the document parameters, as the rebuild solves it.
sketch::Sketch evaluated(const DocState& state, sketch::Sketch model)
{
    const features::DocumentParams params = features::documentParams(state);
    for (sketch::Constraint& c : model.constraints) {
        if (!c.expression || c.driven) {
            continue;
        }
        const Parameter::Kind kind =
            c.type == sketch::ConstraintType::Angle ? Parameter::Kind::Angle : Parameter::Kind::Length;
        auto v = features::evaluateNumeric(json::Value::string(*c.expression), params.values, kind, "test", "dim");
        REQUIRE(v);
        c.value = *v;
    }
    return model;
}

sketch::SolveResult solveOf(const DocState& state, const sketch::Sketch& model)
{
    JobContext ctx;
    auto r = sketch::makePlaneGcsSolver()->solve(evaluated(state, model), ctx);
    REQUIRE(r);
    return std::move(*r);
}

bool solves(sketch::SolveStatus s)
{
    return s != sketch::SolveStatus::Failed && s != sketch::SolveStatus::Conflicting;
}

// The largest change of a point's position or a circle's radius between two forms of one model (infinite when they
// do not have the same entities).
double largestMove(const sketch::Sketch& a, const sketch::Sketch& b)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    if (a.entities.size() != b.entities.size()) {
        return inf;
    }
    double out = 0;
    for (const sketch::Entity& x : a.entities) {
        const auto it =
            std::find_if(b.entities.begin(), b.entities.end(), [&](const sketch::Entity& y) { return y.id == x.id; });
        if (it == b.entities.end() || it->geom.index() != x.geom.index()) {
            return inf;
        }
        if (const auto* p = std::get_if<sketch::Point>(&x.geom)) {
            out = std::max(out, distance(p->p, std::get<sketch::Point>(it->geom).p));
        } else if (const auto* c = std::get_if<sketch::Circle>(&x.geom)) {
            out = std::max(out, std::abs(c->radius - std::get<sketch::Circle>(it->geom).radius));
        }
    }
    return out;
}

Vec2 pointIn(const sketch::Sketch& s, std::uint64_t n)
{
    const SketchEntId id{Uuid::fromString(ent(n))};
    for (const sketch::Entity& e : s.entities) {
        if (e.id == id) {
            return std::get<sketch::Point>(e.geom).p;
        }
    }
    FAIL("no point " << n);
    return {};
}

std::string modelJson(const sketch::Sketch& s)
{
    return json::write(features::sketchToJson(s));
}

// sketch.entity.add arguments with the entities and constraints of `batch`.
std::string addArgs(const SketchJson& batch)
{
    const std::string j = batch.json();
    return R"({"id":")" + sk() + R"(",)" + j.substr(j.find("\"entities\""));
}

std::string text(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3g", v);
    return buf;
}

// A document whose sketch is checked after every step.
struct Session {
    DocEnv env;
    std::vector<std::string> moved;    // steps after which a solve of what is stored moves it
    std::vector<std::string> unsolved; // steps after which the sketch does not solve at all
    std::vector<std::string> reopened; // steps after which saving and opening changed the shape

    DocStatePtr state() const { return env.doc->state(); }
    const Feature& feature(const DocState& s) const
    {
        for (const Feature& f : s.features) {
            if (f.id == featureId(2)) {
                return f;
            }
        }
        FAIL("no sketch feature");
        return s.features.front();
    }
    sketch::Sketch stored() const
    {
        auto m = features::sketchOfFeature(feature(*state()), false);
        REQUIRE(m);
        return *m;
    }
    // What the sketch mode (and the rebuild) shows: the solve of what is stored.
    sketch::Sketch shown() const { return solveOf(*state(), stored()).solved; }

    void run(const std::string& step, const Command& c)
    {
        auto r = env.doc->execute(c);
        if (!r) {
            FAIL(step << ": " << c.name << " failed: " << r.error().message);
        }
        check(step);
    }
    void undo(const std::string& step)
    {
        REQUIRE(env.doc->undo());
        check(step);
    }
    void redo(const std::string& step)
    {
        REQUIRE(env.doc->redo());
        check(step);
    }

    void check(const std::string& step)
    {
        const DocStatePtr s = state();
        if (std::none_of(s->features.begin(), s->features.end(),
                         [](const Feature& f) { return f.id == featureId(2); })) {
            return; // no sketch yet
        }
        const sketch::Sketch model = stored();
        if (model.entities.empty()) {
            return;
        }
        const sketch::SolveResult r = solveOf(*s, model);
        if (!solves(r.status)) {
            unsolved.push_back(step);
            return;
        }
        const double m = largestMove(model, r.solved);
        if (!(m <= kStill)) {
            moved.push_back(step + " (" + text(m) + " mm)");
        }
        // Saved and opened again: the same coordinates, solved to the same shape.
        const StateObjects objects = serializeState(*s);
        auto loaded = deserializeState(objects.manifest, [&](const Hash256& h) -> Result<std::string> {
            for (const auto& [hash, bytes] : objects.features) {
                if (hash == h) {
                    return bytes;
                }
            }
            return tl::unexpected(Error{ErrorCode::NotFound, "missing object", {}});
        });
        REQUIRE(loaded);
        auto again = features::sketchOfFeature(feature(*loaded), false);
        REQUIRE(again);
        const sketch::SolveResult r2 = solveOf(*loaded, *again);
        if (largestMove(model, *again) != 0 || !solves(r2.status) || !(largestMove(r.solved, r2.solved) <= kStill)) {
            reopened.push_back(step);
        }
    }

    void requireIntact() const
    {
        const auto list = [](const std::vector<std::string>& steps) {
            std::string out;
            for (const std::string& s : steps) {
                out += "\n  " + s;
            }
            return out;
        };
        INFO("the stored sketch moves when solved again after:" << list(moved));
        CHECK(moved.empty());
        INFO("the sketch does not solve after:" << list(unsolved));
        CHECK(unsolved.empty());
        INFO("save and open changed the sketch after:" << list(reopened));
        CHECK(reopened.empty());
    }
};

std::string constraintSet(std::uint64_t c, const std::string& fields)
{
    return R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', c) + R"(",)" + fields + "}";
}

// A rectangle as the rectangle tool draws it: points 1 to 4, lines 5 (bottom), 6 (right), 7 (top), 8 (left), with
// horizontal and vertical constraints c1 to c4.
SketchJson rectangle(double w, double h)
{
    SketchJson s;
    s.rect(1, 0, 0, w, h)
        .constraint(1, "horizontal", {5})
        .constraint(2, "horizontal", {7})
        .constraint(3, "vertical", {8})
        .constraint(4, "vertical", {6});
    return s;
}

} // namespace

TEST_CASE("every sketch command stores solved coordinates: drawing tools, constraints, dimensions, drag, undo",
          "[doc][sketch][solved-store]")
{
    Session s;
    s.run("rectangle (the first step: feature.sketch.add)",
          addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", rectangle(40, 20))));

    // Dimensions typed as other values than the lines have; the sketch mode sends the shape it shows ("solved").
    s.run("width dimension 60 on a 40 line",
          cmd("sketch.constraint.add", R"({"id":")" + sk() + R"(","constraints":[{"id":")" + uuid('c', 5) +
                                           R"(","type":"distance","refs":[")" + ent(5) +
                                           R"("],"value":60}],"solved":)" + modelJson(s.shown()) + "}"));
    s.run("height dimension 30 on a 20 line",
          cmd("sketch.constraint.add", R"({"id":")" + sk() + R"(","constraints":[{"id":")" + uuid('c', 6) +
                                           R"(","type":"distance","refs":[")" + ent(8) +
                                           R"("],"value":30}],"solved":)" + modelJson(s.shown()) + "}"));
    s.run("width edited to 25",
          cmd("sketch.constraint.set", constraintSet(5, R"("value":25,"solved":)" + modelJson(s.shown()))));

    // Line tool: a chain from the top right corner (the corner's id is reused), horizontal then vertical.
    {
        const Vec2 c3 = pointIn(s.shown(), 3);
        SketchJson b;
        b.point(20, c3.x + 15, c3.y).line(21, 3, 20).constraint(7, "horizontal", {21});
        s.run("line from a corner", cmd("sketch.entity.add", addArgs(b)));
    }
    {
        const Vec2 p = pointIn(s.shown(), 20);
        SketchJson b;
        b.point(22, p.x, p.y + 10).line(23, 20, 22).constraint(8, "vertical", {23});
        s.run("line chain, next segment", cmd("sketch.entity.add", addArgs(b)));
    }
    // Tangent arc tool: from the end of the chain, along it, turning left.
    {
        const sketch::Sketch v = s.shown();
        const Vec2 a = pointIn(v, 20), p = pointIn(v, 22);
        const Vec2 end{p.x - 8, p.y + 8};
        const ArcFit f = tangentArc(p, unit(sub(p, a)), end);
        SketchJson b;
        b.point(24, f.center.x, f.center.y).point(25, end.x, end.y).arc(26, 24, 22, 25, f.ccw);
        b.constraint(9, "tangent", {23, 26, 22});
        s.run("tangent arc", cmd("sketch.entity.add", addArgs(b)));
    }
    // Circle tool with a typed diameter.
    {
        SketchJson b;
        b.circleAt(30, 10, 10, 3).constraint(10, "diameter", {31}, {}, 6.0);
        s.run("circle", cmd("sketch.entity.add", addArgs(b)));
    }
    // Three-point arc from the bottom left corner (its id is reused).
    {
        const Vec2 c1 = pointIn(s.shown(), 1);
        const Vec2 e{c1.x - 10, c1.y}, m{c1.x - 5, c1.y - 4};
        // The centre of the circle through c1, e and m.
        const double d = 2 * (c1.x * (e.y - m.y) + e.x * (m.y - c1.y) + m.x * (c1.y - e.y));
        const double n1 = c1.x * c1.x + c1.y * c1.y, n2 = e.x * e.x + e.y * e.y, n3 = m.x * m.x + m.y * m.y;
        const Vec2 c{(n1 * (e.y - m.y) + n2 * (m.y - c1.y) + n3 * (c1.y - e.y)) / d,
                     (n1 * (m.x - e.x) + n2 * (c1.x - m.x) + n3 * (e.x - c1.x)) / d};
        SketchJson b;
        b.point(32, e.x, e.y).point(33, c.x, c.y).arc(34, 33, 32, 1, true);
        s.run("three-point arc", cmd("sketch.entity.add", addArgs(b)));
    }
    // Centre arc tool on the top left corner (the corner is the centre), with a typed radius.
    {
        const Vec2 c4 = pointIn(s.shown(), 4);
        SketchJson b;
        b.point(35, c4.x, c4.y + 5).point(36, c4.x - 5, c4.y).arc(37, 4, 35, 36, true);
        b.constraint(11, "radius", {37}, {}, 5.0);
        s.run("centre arc", cmd("sketch.entity.add", addArgs(b)));
    }
    // Slot tool, drawn horizontally.
    {
        SketchJson b;
        b.point(38, -30, -20).point(39, -10, -20).slot(40, 38, 39, 4).constraint(12, "horizontal", {38, 39});
        s.run("slot", cmd("sketch.entity.add", addArgs(b)));
    }
    // Spline tool.
    {
        SketchJson b;
        b.point(41, 60, 0).point(42, 65, 5).point(43, 70, 0).point(44, 75, 5).spline(45, {41, 42, 43, 44});
        s.run("spline", cmd("sketch.entity.add", addArgs(b)));
    }
    // A line started on the top edge (a point-on-curve snap) going up.
    {
        const sketch::Sketch v = s.shown();
        const Vec2 a = pointIn(v, 3), c = pointIn(v, 4);
        const Vec2 mid{(a.x + c.x) / 2, (a.y + c.y) / 2};
        SketchJson b;
        b.point(46, mid.x, mid.y).point(47, mid.x, mid.y + 6).line(48, 46, 47);
        b.constraint(13, "pointOnCurve", {46, 7}).constraint(14, "vertical", {48});
        s.run("line from a point on an edge", cmd("sketch.entity.add", addArgs(b)));
    }
    // A script's line that is not quite horizontal, with a horizontal constraint.
    {
        SketchJson b;
        b.point(49, 50, 50).point(50, 60, 50.4).line(51, 49, 50).constraint(15, "horizontal", {51});
        s.run("a script's near-horizontal line", cmd("sketch.entity.add", addArgs(b)));
    }
    // Trim: a horizontal line across the left edge, its outer piece trimmed.
    {
        const sketch::Sketch v = s.shown();
        const Vec2 a = pointIn(v, 4), c = pointIn(v, 1);
        const double y = (a.y + c.y) / 2 + 2, x = a.x;
        SketchJson b;
        b.point(52, x - 6, y).point(53, x + 6, y).line(54, 52, 53).constraint(16, "horizontal", {54});
        s.run("line across the left edge", cmd("sketch.entity.add", addArgs(b)));
        const Vec2 l = pointIn(s.shown(), 1);
        s.run("trim", cmd("sketch.trim", R"({"id":")" + sk() + R"(","entity":")" + ent(54) + R"(","at":{"x":)" +
                                             num(l.x - 4) + R"(,"y":)" + num(y) + "}}"));
    }
    // Extend: a horizontal line inside the rectangle grown to the right edge.
    {
        const sketch::Sketch v = s.shown();
        const Vec2 a = pointIn(v, 2), c = pointIn(v, 3);
        const double y = (a.y + c.y) / 2 - 4, x = a.x;
        SketchJson b;
        b.point(55, x - 12, y).point(56, x - 5, y).line(57, 55, 56).constraint(17, "horizontal", {57});
        s.run("line short of the right edge", cmd("sketch.entity.add", addArgs(b)));
        const Vec2 e = pointIn(s.shown(), 56);
        s.run("extend", cmd("sketch.extend", R"({"id":")" + sk() + R"(","entity":")" + ent(57) + R"(","at":{"x":)" +
                                                 num(e.x - 1) + R"(,"y":)" + num(e.y) + "}}"));
    }
    // Offset the line chain.
    {
        const Vec2 p = pointIn(s.shown(), 20);
        s.run("offset", cmd("sketch.offset", R"({"id":")" + sk() + R"(","entities":[")" + ent(21) + R"(",")" + ent(23) +
                                                 R"("],"distance":2,"side":{"x":)" + num(p.x + 1) + R"(,"y":)" +
                                                 num(p.y - 1) + "}}"));
    }
    // Corner fillet at the bottom right corner.
    s.run("fillet", cmd("sketch.fillet", R"({"id":")" + sk() + R"(","point":")" + ent(2) +
                                             R"(","radius":2,"newEntity":")" + ent(58) + R"("})"));

    // Constraints and dimensions.
    s.run("equal constraint",
          cmd("sketch.constraint.add", R"({"id":")" + sk() + R"(","constraints":[{"id":")" + uuid('c', 18) +
                                           R"(","type":"equal","refs":[")" + ent(21) + R"(",")" + ent(23) +
                                           R"("]}],"solved":)" + modelJson(s.shown()) + "}"));
    s.run("height edited to 45",
          cmd("sketch.constraint.set", constraintSet(6, R"("value":45,"solved":)" + modelJson(s.shown()))));
    s.run("width made driven",
          cmd("sketch.constraint.set", constraintSet(5, R"("driven":true,"solved":)" + modelJson(s.shown()))));
    s.run("width driving again at 30",
          cmd("sketch.constraint.set",
              constraintSet(5, R"("driven":false,"value":30,"solved":)" + modelJson(s.shown()))));
    s.run("dimension label moved", cmd("sketch.dimension.place", constraintSet(6, R"("offset":{"x":-4,"y":0})")));
    s.run("height dimension removed",
          cmd("sketch.constraint.remove", R"({"id":")" + sk() + R"(","constraints":[")" + uuid('c', 6) +
                                              R"("],"solved":)" + modelJson(s.shown()) + "}"));
    s.run("construction", cmd("sketch.construction",
                              R"({"id":")" + sk() + R"(","entities":[")" + ent(51) + R"("],"construction":true})"));
    {
        const Vec2 a = pointIn(s.shown(), 46), b = pointIn(s.shown(), 47);
        s.run("split", cmd("sketch.split", R"({"id":")" + sk() + R"(","entity":")" + ent(48) + R"(","at":{"x":)" +
                                               num((a.x + b.x) / 2) + R"(,"y":)" + num((a.y + b.y) / 2) + "}}"));
    }
    s.run("entity removed",
          cmd("sketch.entity.remove", R"({"id":")" + sk() + R"(","entities":[")" + ent(51) + R"("]})"));

    // A drag, committed as the sketch mode commits it (sketch.set with the drag session's result, which started from
    // the shape shown).
    {
        const sketch::Sketch v = evaluated(*s.state(), s.shown());
        JobContext ctx;
        auto session = sketch::makePlaneGcsSolver()->beginDrag(v, SketchEntId{Uuid::fromString(ent(25))}, ctx);
        REQUIRE(session);
        const Vec2 from = pointIn(v, 25);
        auto step = (*session)->move(Vec2{from.x + 3, from.y + 2});
        REQUIRE(step);
        const sketch::Sketch dragged = (*session)->end();
        s.run("drag", cmd("sketch.set", R"({"id":")" + sk() + R"(","sketch":)" + modelJson(dragged) + "}"));
    }
    // sketch.set from a script with a point off its constraints.
    {
        sketch::Sketch m = s.stored();
        for (sketch::Entity& e : m.entities) {
            if (e.id == SketchEntId{Uuid::fromString(ent(22))}) {
                std::get<sketch::Point>(e.geom).p.x += 1.5;
            }
        }
        s.run("a script's sketch.set", cmd("sketch.set", R"({"id":")" + sk() + R"(","sketch":)" + modelJson(m) + "}"));
    }
    // feature.sketch.edit from a script, with the whole model in its props.
    {
        sketch::Sketch m = s.stored();
        for (sketch::Entity& e : m.entities) {
            if (e.id == SketchEntId{Uuid::fromString(ent(20))}) {
                std::get<sketch::Point>(e.geom).p.y -= 2;
            }
        }
        s.run("a script's feature.sketch.edit", editFeature("sketch", 2, R"({"sketch":)" + modelJson(m) + "}"));
    }

    // Undo and redo restore snapshots that were stored solved.
    s.undo("undo 1");
    s.undo("undo 2");
    s.undo("undo 3");
    s.redo("redo 1");
    s.redo("redo 2");
    s.requireIntact();
}

TEST_CASE("a tangent arc drawn after a dimension edit stays where it was drawn", "[doc][sketch][solved-store]")
{
    // A 50 mm line from a fixed point, at an angle of 0 (an angle dimension). The angle is edited to 150 degrees; then
    // a tangent arc is drawn on from the line's end, as the line is shown now. The arc tool reuses the line's end
    // point and adds a tangent constraint. When the document still held the line at 0 degrees (the shape before the
    // edit), the solve started with the arc's start point at the old end, 93 mm away from where the arc was drawn,
    // and the tangency direction was chosen there: the arc came out elsewhere, the other way round.
    Session s;
    SketchJson line;
    line.point(1, 0, 0).point(2, 50, 0).line(3, 1, 2);
    line.constraint(1, "fix", {1}).constraint(2, "distance", {3}, {}, 50.0).constraint(3, "angle", {3}, {}, 0.0);
    s.run("line", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", line)));
    const double angle = 150 * kPi / 180;
    s.run("angle edited to 150 degrees",
          cmd("sketch.constraint.set",
              constraintSet(3, R"("value":)" + num(angle) + R"(,"solved":)" + modelJson(s.shown()))));
    const sketch::Sketch before = s.shown();
    const Vec2 a = pointIn(before, 1), p = pointIn(before, 2);
    REQUIRE(p.x == Catch::Approx(50 * std::cos(angle)));
    REQUIRE(p.y == Catch::Approx(50 * std::sin(angle)));
    const Vec2 dir = unit(sub(p, a));
    // The arc turns left through a point 20 mm on, 45 degrees off the line.
    const double turn = 45 * kPi / 180;
    const Vec2 end{p.x + 20 * (dir.x * std::cos(turn) - dir.y * std::sin(turn)),
                   p.y + 20 * (dir.x * std::sin(turn) + dir.y * std::cos(turn))};
    const ArcFit f = tangentArc(p, dir, end);
    REQUIRE(f.ccw);
    SketchJson b;
    b.point(4, f.center.x, f.center.y).point(5, end.x, end.y).arc(6, 4, 2, 5, f.ccw);
    b.constraint(4, "tangent", {3, 6, 2});
    s.run("tangent arc", cmd("sketch.entity.add", addArgs(b)));

    const sketch::Sketch after = s.shown();
    INFO("arc centre drawn at (" << f.center.x << ", " << f.center.y << "), shown at (" << pointIn(after, 4).x << ", "
                                 << pointIn(after, 4).y << "); arc end drawn at (" << end.x << ", " << end.y
                                 << "), shown at (" << pointIn(after, 5).x << ", " << pointIn(after, 5).y << ")");
    CHECK(distance(pointIn(after, 4), f.center) < 1e-6);
    CHECK(distance(pointIn(after, 5), end) < 1e-6);
    CHECK(distance(pointIn(after, 2), p) < 1e-9);
    // The arc leaves the line smoothly (the same direction), not back along it.
    const Vec2 c = pointIn(after, 4), q = pointIn(after, 2);
    const Vec2 radial = unit(sub(q, c));
    const Vec2 travel{-radial.y, radial.x}; // counter-clockwise
    CHECK(travel.x * dir.x + travel.y * dir.y > 0.999);
    s.requireIntact();
}

TEST_CASE("a rectangle edited narrower keeps a tangent arc drawn at its corner", "[doc][sketch][solved-store]")
{
    // The width of a 40 x 20 rectangle (one corner fixed) is edited to 15; a tangent arc is then drawn on from the top
    // right corner, as it is shown now (x = 15), turning left over the top.
    Session s;
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 40.0).constraint(7, "distance", {8}, {}, 20.0);
    s.run("rectangle", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));
    s.run("width edited to 15",
          cmd("sketch.constraint.set", constraintSet(6, R"("value":15,"solved":)" + modelJson(s.shown()))));
    const sketch::Sketch before = s.shown();
    const Vec2 c2 = pointIn(before, 2), c3 = pointIn(before, 3);
    REQUIRE(c3.x == Catch::Approx(15));
    const Vec2 end{c3.x - 6, c3.y + 6};
    const ArcFit f = tangentArc(c3, unit(sub(c3, c2)), end);
    SketchJson b;
    b.point(20, f.center.x, f.center.y).point(21, end.x, end.y).arc(22, 20, 3, 21, f.ccw);
    b.constraint(8, "tangent", {6, 22, 3});
    s.run("tangent arc at the corner", cmd("sketch.entity.add", addArgs(b)));
    const sketch::Sketch after = s.shown();
    INFO("arc centre drawn at (" << f.center.x << ", " << f.center.y << "), shown at (" << pointIn(after, 20).x << ", "
                                 << pointIn(after, 20).y << "); end drawn at (" << end.x << ", " << end.y
                                 << "), shown at (" << pointIn(after, 21).x << ", " << pointIn(after, 21).y << ")");
    CHECK(distance(pointIn(after, 20), f.center) < 1e-6);
    CHECK(distance(pointIn(after, 21), end) < 1e-6);
    CHECK(largestMove(before, [&] {
              sketch::Sketch old = after;
              old.entities.erase(std::remove_if(old.entities.begin(), old.entities.end(),
                                                [](const sketch::Entity& e) {
                                                    return e.id.value == Uuid::fromString(ent(20)) ||
                                                           e.id.value == Uuid::fromString(ent(21)) ||
                                                           e.id.value == Uuid::fromString(ent(22));
                                                }),
                                 old.entities.end());
              return old;
          }()) < 1e-9);
    s.requireIntact();
}

TEST_CASE("trim and fillet after a dimension edit act on the shape shown", "[doc][sketch][solved-store]")
{
    // A 40 x 20 rectangle (corner 1 fixed, width and height dimensioned); the width is edited to 15. A line drawn
    // across the shape at y = 10 from x = -5 to x = 25 crosses the left edge at 0 and the right edge at 15; trimming
    // its piece beyond the right edge must leave the line from -5 to 15. A fillet of radius 2 at the top right corner
    // (its lines carry no dimension) must round the corner at (15, 20).
    Session s;
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 40.0).constraint(7, "distance", {8}, {}, 20.0);
    s.run("rectangle", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));
    s.run("width edited to 15",
          cmd("sketch.constraint.set", constraintSet(6, R"("value":15,"solved":)" + modelJson(s.shown()))));
    SketchJson b;
    b.point(20, -5, 10).point(21, 25, 10).line(22, 20, 21).constraint(8, "horizontal", {22});
    s.run("line across", cmd("sketch.entity.add", addArgs(b)));
    s.run("trim the piece beyond the right edge",
          cmd("sketch.trim", R"({"id":")" + sk() + R"(","entity":")" + ent(22) + R"(","at":{"x":20,"y":10}})"));
    {
        const sketch::Sketch v = s.shown();
        const sketch::Entity* l = nullptr;
        for (const sketch::Entity& e : v.entities) {
            if (e.id.value == Uuid::fromString(ent(22))) {
                l = &e;
            }
        }
        REQUIRE(l != nullptr);
        const auto& line = std::get<sketch::Line>(l->geom);
        double ax = 0, bx = 0;
        for (const sketch::Entity& e : v.entities) {
            if (e.id == line.a) {
                ax = std::get<sketch::Point>(e.geom).p.x;
            }
            if (e.id == line.b) {
                bx = std::get<sketch::Point>(e.geom).p.x;
            }
        }
        CHECK(std::min(ax, bx) == Catch::Approx(-5));
        CHECK(std::max(ax, bx) == Catch::Approx(15));
    }
    s.run("fillet the top right corner", cmd("sketch.fillet", R"({"id":")" + sk() + R"(","point":")" + ent(3) +
                                                                  R"(","radius":2,"newEntity":")" + ent(30) + R"("})"));
    {
        const sketch::Sketch v = s.shown();
        for (const sketch::Entity& e : v.entities) {
            if (e.id.value == Uuid::fromString(ent(30))) {
                const auto& arc = std::get<sketch::Arc>(e.geom);
                Vec2 c{};
                for (const sketch::Entity& x : v.entities) {
                    if (x.id == arc.center) {
                        c = std::get<sketch::Point>(x.geom).p;
                    }
                }
                CHECK(c.x == Catch::Approx(13));
                CHECK(c.y == Catch::Approx(18));
            }
        }
    }
    s.requireIntact();
}

TEST_CASE("a trim right after a parameter edit acts on the shape the parameter gave", "[doc][sketch][solved-store]")
{
    // The width is the parameter w (40); a line at y = 10 runs from x = -5 to 50 across both sides. w is set to 15
    // (param.set: no sketch command), then the piece of the line at x = 20, now between the right side (15) and the
    // line's end, is trimmed: the line must end at 15.
    Session s;
    s.run("parameter",
          cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"w","expression":"40 mm","kind":"length"})"));
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, "w").constraint(7, "distance", {8}, {}, 20.0);
    r.point(20, -5, 10).point(21, 50, 10).line(22, 20, 21).constraint(8, "horizontal", {22});
    s.run("rectangle and line", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));
    s.run("w set to 15", cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"15 mm"})"));
    s.run("trim", cmd("sketch.trim", R"({"id":")" + sk() + R"(","entity":")" + ent(22) + R"(","at":{"x":20,"y":10}})"));
    const sketch::Sketch v = s.shown();
    int lines = 0;
    double right = -1e9;
    for (const sketch::Entity& e : v.entities) {
        if (const auto* l = std::get_if<sketch::Line>(&e.geom);
            l != nullptr && e.id.value != Uuid::fromString(ent(5)) && e.id.value != Uuid::fromString(ent(6)) &&
            e.id.value != Uuid::fromString(ent(7)) && e.id.value != Uuid::fromString(ent(8))) {
            ++lines;
            for (const sketch::Entity& p : v.entities) {
                if (p.id == l->a || p.id == l->b) {
                    right = std::max(right, std::get<sketch::Point>(p.geom).p.x);
                }
            }
        }
    }
    CHECK(lines == 1); // shortened, not split
    CHECK(right == Catch::Approx(15));
}

TEST_CASE("a sketch that does not solve keeps the coordinates it had", "[doc][sketch][solved-store]")
{
    Session s;
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 40.0).constraint(7, "distance", {8}, {}, 20.0);
    s.run("rectangle", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));
    const sketch::Sketch good = s.stored();

    SECTION("conflicting constraints")
    {
        // A second width on the top line, 30 against 40: the constraints conflict.
        REQUIRE(s.env.doc->execute(cmd("sketch.constraint.add", R"({"id":")" + sk() + R"(","constraints":[{"id":")" +
                                                                    uuid('c', 8) + R"(","type":"distance","refs":[")" +
                                                                    ent(7) + R"("],"value":30}]})")));
        CHECK(largestMove(good, s.stored()) == 0);
        const sketch::SolveResult r2 = solveOf(*s.state(), s.stored());
        CHECK(r2.status == sketch::SolveStatus::Conflicting);
        // The rebuild reports it as before.
        auto build = s.env.rebuild(s.state());
        CHECK(report(*build, 2).status == FeatureStatus::Failed);
        CHECK(report(*build, 2).message.find("conflict") != std::string::npos);
    }
    SECTION("a solve that cannot converge")
    {
        // A triangle whose sides cannot close: 10 + 10 < 50.
        SketchJson t;
        t.point(20, 60, 0).point(21, 70, 0).point(22, 65, 5).line(23, 20, 21).line(24, 21, 22).line(25, 22, 20);
        t.constraint(8, "distance", {23}, {}, 50.0)
            .constraint(9, "distance", {24}, {}, 10.0)
            .constraint(10, "distance", {25}, {}, 10.0);
        REQUIRE(s.env.doc->execute(cmd("sketch.entity.add", addArgs(t))));
        const sketch::Sketch stored = s.stored();
        const sketch::SolveResult r2 = solveOf(*s.state(), stored);
        CHECK(!solves(r2.status));
        // The rectangle and the triangle are where they were given.
        CHECK(pointIn(stored, 21).x == 70);
        CHECK(pointIn(stored, 22).y == 5);
        CHECK(pointIn(stored, 3).x == 40);
        auto build = s.env.rebuild(s.state());
        CHECK(report(*build, 2).status == FeatureStatus::Failed);
    }
}

TEST_CASE("\"solved\" that already satisfies the new constraints is stored as given; any other is solved on",
          "[doc][sketch][solved-store]")
{
    Session s;
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 40.0).constraint(7, "distance", {8}, {}, 20.0);
    s.run("rectangle", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));

    SECTION("the caller's solve of the edited sketch is stored bit for bit")
    {
        SketchJson after = rectangle(55, 20);
        after.constraint(5, "fix", {1})
            .constraint(6, "distance", {5}, {}, 55.0)
            .constraint(7, "distance", {8}, {}, 20.0);
        s.run("width 55 with its solved shape",
              cmd("sketch.constraint.set", constraintSet(6, R"("value":55,"solved":)" + after.json())));
        CHECK(pointIn(s.stored(), 2).x == 55);
        CHECK(pointIn(s.stored(), 3).x == 55);
        CHECK(pointIn(s.stored(), 3).y == 20);
    }
    SECTION("the shape before the edit is a starting point: the stored sketch is solved from it")
    {
        s.run("width 55 with the shape before",
              cmd("sketch.constraint.set", constraintSet(6, R"("value":55,"solved":)" + modelJson(s.shown()))));
        CHECK(pointIn(s.stored(), 2).x == Catch::Approx(55));
    }
    SECTION("a solved form that names other entities is still refused")
    {
        SketchJson other = rectangle(55, 20);
        other.point(30, 1, 1);
        auto refused = s.env.doc->execute(
            cmd("sketch.constraint.set", constraintSet(6, R"("value":55,"solved":)" + other.json())));
        REQUIRE(!refused);
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
    }
    s.requireIntact();
}

TEST_CASE("a drag's result is stored without another solve; a changed sketch is solved once",
          "[doc][sketch][solved-store]")
{
    Session s;
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 40.0);
    s.run("rectangle", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));
    const sketch::Sketch v = evaluated(*s.state(), s.stored());
    JobContext ctx;
    auto session = sketch::makePlaneGcsSolver()->beginDrag(v, SketchEntId{Uuid::fromString(ent(3))}, ctx);
    REQUIRE(session);
    REQUIRE((*session)->move(Vec2{52, 31}));
    const sketch::Sketch dragged = (*session)->end();

    features::StoredSketch outcome = features::StoredSketch::NotSolved;
    const sketch::Sketch kept = features::storedSketch(*s.state(), dragged, &outcome);
    CHECK(outcome == features::StoredSketch::AsGiven);
    CHECK(largestMove(kept, dragged) == 0);

    sketch::Sketch off = dragged;
    for (sketch::Entity& e : off.entities) {
        if (e.id == SketchEntId{Uuid::fromString(ent(3))}) {
            std::get<sketch::Point>(e.geom).p.y += 4;
        }
    }
    const sketch::Sketch solved = features::storedSketch(*s.state(), off, &outcome);
    CHECK(outcome == features::StoredSketch::Solved);
    auto holds = sketch::constraintsHold(evaluated(*s.state(), solved));
    REQUIRE(holds);
    CHECK(*holds);
    CHECK(!*sketch::constraintsHold(evaluated(*s.state(), off)));
}

TEST_CASE("time a sketch command takes on a 200-curve sketch (printed)", "[.sketch-store-timing]")
{
    // A zig-zag chain of 200 lines with fixed lengths and its first point fixed (the drag test's sketch), then one
    // line added at its end by sketch.entity.add, and the chain set again as it is (a drag commit).
    SketchJson chain;
    for (int i = 0; i <= 200; ++i) {
        chain.point(static_cast<std::uint64_t>(1000 + i), i * 0.5, (i % 2) * 0.4);
    }
    for (int i = 0; i < 200; ++i) {
        chain.line(static_cast<std::uint64_t>(2000 + i), static_cast<std::uint64_t>(1000 + i),
                   static_cast<std::uint64_t>(1001 + i));
        chain.constraint(static_cast<std::uint64_t>(3000 + i), "distance", {static_cast<std::uint64_t>(2000 + i)}, {},
                         std::hypot(0.5, 0.4));
    }
    chain.constraint(4000, "fix", {1000});
    Session s;
    auto timed = [&](const char* what, const Command& c) {
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(s.env.doc->execute(c));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%-48s %8.1f ms\n", what, ms);
    };
    timed("feature.sketch.add (200 lines, solved already)",
          addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", chain)));
    timed("sketch.set unchanged (a drag commit)",
          cmd("sketch.set", R"({"id":")" + sk() + R"(","sketch":)" + modelJson(s.stored()) + "}"));
    SketchJson b;
    b.point(5000, 101, 0.4).line(5001, 1200, 5000).constraint(5002, "distance", {5001}, {}, 0.5);
    timed("sketch.entity.add (one line, needs a solve)", cmd("sketch.entity.add", addArgs(b)));
    timed("sketch.constraint.set (needs a solve)", cmd("sketch.constraint.set", constraintSet(5002, R"("value":0.7)")));
}
