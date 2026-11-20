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
#include <optional>
#include <string>
#include <thread>
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
    Session() = default;
    explicit Session(DocStatePtr initial) : env(4, std::move(initial)) {}

    DocEnv env;
    std::vector<std::string> moved;    // steps after which a solve of what is stored moves it
    std::vector<std::string> unsolved; // steps after which the sketch does not solve at all
    std::vector<std::string> reopened; // steps after which saving and opening changed the shape

    DocStatePtr state() const { return env.doc->state(); }
    const Feature& feature(const DocState& s, std::uint64_t n = 2) const
    {
        for (const Feature& f : s.features) {
            if (f.id == featureId(n)) {
                return f;
            }
        }
        FAIL("no feature " << n);
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

namespace {

std::string paramSet(std::uint64_t n, const std::string& fields)
{
    return R"({"id":")" + uuid('a', n) + R"(",)" + fields + "}";
}

// A rectangle 40 x 20 with corner 1 fixed, its width the parameter w and its height h (= w / 2), and a 5 mm extrude
// of it into body 'b'; a second sketch (feature 3) uses no parameter.
void dimensionedByParameters(Session& s)
{
    s.run("parameter w", cmd("param.set", paramSet(1, R"("name":"w","expression":"40 mm","kind":"length")")));
    s.run("parameter h", cmd("param.set", paramSet(2, R"("name":"h","expression":"w / 2","kind":"length")")));
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, "w").constraint(7, "distance", {8}, "h");
    s.run("rectangle", addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", r)));
    SketchJson other;
    other.rect(1, 100, 0, 110, 10)
        .constraint(1, "horizontal", {5})
        .constraint(2, "horizontal", {7})
        .constraint(3, "vertical", {8})
        .constraint(4, "vertical", {6})
        .constraint(5, "distance", {5}, {}, 10.0);
    s.run("a sketch with no parameter", addFeature("sketch", 3, sketchProps(R"({"principal":"xy"})", other)));
    s.run("extrude", addFeature("extrude", 4, extrudeProps(2, "5", "new", 'b')));
}

} // namespace

TEST_CASE("a parameter edit stores every sketch it changes solved, in the same undo step",
          "[doc][sketch][solved-store]")
{
    // param.set is not a sketch command, but a dimension that names the parameter (directly, or through another
    // parameter) changes the sketch: the document must hold the shape the rebuild shows, so the saved file, the next
    // solve and the geometry built from it agree.
    Session s;
    dimensionedByParameters(s);
    const std::string untouched = s.feature(*s.state(), 3).propsJson;

    s.run("w set to 15", cmd("param.set", paramSet(1, R"("expression":"15 mm")")));
    CHECK(pointIn(s.stored(), 3).x == Catch::Approx(15));
    CHECK(pointIn(s.stored(), 3).y == Catch::Approx(7.5));  // h = w / 2 follows
    CHECK(s.feature(*s.state(), 3).propsJson == untouched); // a sketch the change does not reach is not rewritten
    {
        auto build = s.env.rebuild(s.state());
        CHECK(report(*build, 2).status == FeatureStatus::Ok);
        INFO("extrude: " << report(*build, 4).message);
        CHECK(volumeOf(*build, 'b') == Catch::Approx(15 * 7.5 * 5).epsilon(1e-9));
    }

    s.undo("undo w");
    CHECK(pointIn(s.stored(), 3).x == Catch::Approx(40));
    s.redo("redo w");
    CHECK(pointIn(s.stored(), 3).x == Catch::Approx(15));

    s.run("h set to a number", cmd("param.set", paramSet(2, R"("expression":"12 mm")")));
    CHECK(pointIn(s.stored(), 3).y == Catch::Approx(12));
    s.run("w renamed (param.rename: values unchanged)", cmd("param.rename", paramSet(1, R"("name":"width")")));
    CHECK(pointIn(s.stored(), 3).x == Catch::Approx(15));
    s.run("width set to 22", cmd("param.set", paramSet(1, R"("expression":"22 mm")")));
    CHECK(pointIn(s.stored(), 3).x == Catch::Approx(22));
    CHECK(s.feature(*s.state(), 3).propsJson == untouched);
    s.requireIntact();
}

TEST_CASE("feature.add and feature.edit store a sketch solved, as feature.sketch.add does",
          "[doc][sketch][solved-store]")
{
    // The generic commands (a script, an agent, a journal replay) write a sketch's props as given; the document must
    // still hold the solved shape. Width dimension 60 on a 40 mm rectangle, corner 1 fixed.
    Session s;
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 60.0).constraint(7, "distance", {8}, {}, 20.0);
    s.run("feature.add of a sketch",
          cmd("feature.add", R"({"id":")" + uuid('f', 2) + R"(","type":"sketch","name":"Sketch 1","props":)" +
                                 sketchProps(R"({"principal":"xy"})", r) + "}"));
    CHECK(pointIn(s.stored(), 2).x == Catch::Approx(60));
    CHECK(pointIn(s.stored(), 3).x == Catch::Approx(60));

    sketch::Sketch m = s.stored();
    for (sketch::Entity& e : m.entities) {
        if (e.id == SketchEntId{Uuid::fromString(ent(3))}) {
            std::get<sketch::Point>(e.geom).p.y += 7; // off its constraints (height 20)
        }
    }
    s.run("feature.edit of a sketch",
          cmd("feature.edit", R"({"id":")" + uuid('f', 2) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" +
                                  modelJson(m) + "}}"));
    CHECK(pointIn(s.stored(), 3).y == Catch::Approx(20));
    s.run("feature.edit of the name only", cmd("feature.edit", R"({"id":")" + uuid('f', 2) + R"(","name":"Base"})"));

    // feature.add checks no expression: a dimension naming a parameter that does not exist yet is stored as given;
    // adding the parameter (param.set) is what makes it solve, and that step stores it solved.
    SketchJson d;
    d.point(20, 100, 0).point(21, 130, 0).line(22, 20, 21);
    d.constraint(20, "fix", {20}).constraint(21, "horizontal", {22}).constraint(22, "distance", {22}, "len");
    s.run("feature.add of a sketch naming an unknown parameter",
          cmd("feature.add", R"({"id":")" + uuid('f', 5) + R"(","type":"sketch","name":"Sketch 2","props":)" +
                                 sketchProps(R"({"principal":"xy"})", d) + "}"));
    const auto lineLength = [&] {
        auto v = features::sketchOfFeature(s.feature(*s.state(), 5), false);
        REQUIRE(v);
        return distance(pointIn(*v, 20), pointIn(*v, 21));
    };
    CHECK(lineLength() == Catch::Approx(30)); // as given: the dimension does not evaluate
    s.run("parameter len added", cmd("param.set", paramSet(3, R"("name":"len","expression":"12 mm","kind":"length")")));
    CHECK(lineLength() == Catch::Approx(12));
    s.requireIntact();
}

TEST_CASE("a parameter renamed to the name a dimension already used stores that sketch solved",
          "[doc][sketch][solved-store]")
{
    // A dimension naming "len" before any parameter has that name (feature.add checks no expression; an older file
    // may hold one). Renaming the parameter "size" (12 mm) to "len" changes no value, but that dimension now
    // evaluates: param.rename must store the sketch solved, as param.set does.
    Session s;
    s.run("parameter size", cmd("param.set", paramSet(1, R"("name":"size","expression":"12 mm","kind":"length")")));
    SketchJson d;
    d.point(20, 100, 0).point(21, 130, 0).line(22, 20, 21);
    d.constraint(20, "fix", {20}).constraint(21, "horizontal", {22}).constraint(22, "distance", {22}, "len");
    REQUIRE(s.env.doc->execute(cmd("feature.add", R"({"id":")" + uuid('f', 5) +
                                                      R"(","type":"sketch","name":"Sketch 2","props":)" +
                                                      sketchProps(R"({"principal":"xy"})", d) + "}")));
    const auto lineLength = [&] {
        auto v = features::sketchOfFeature(s.feature(*s.state(), 5), false);
        REQUIRE(v);
        return distance(pointIn(*v, 20), pointIn(*v, 21));
    };
    CHECK(lineLength() == Catch::Approx(30));
    REQUIRE(s.env.doc->execute(cmd("param.rename", paramSet(1, R"("name":"len")"))));
    CHECK(lineLength() == Catch::Approx(12));
    // Renaming it on (the dimension's expression follows: values unchanged) leaves the sketch as it is.
    const std::string solved = s.feature(*s.state(), 5).propsJson;
    REQUIRE(s.env.doc->execute(cmd("param.rename", paramSet(1, R"("name":"length")"))));
    CHECK(lineLength() == Catch::Approx(12));
    CHECK(s.feature(*s.state(), 5).propsJson.find("\"expression\":\"length\"") != std::string::npos);
    CHECK(solved.find("\"expression\":\"len\"") != std::string::npos);
}

TEST_CASE("a document saved before sketches were stored solved: a parameter edit solves on from the shape shown",
          "[doc][sketch][solved-store]")
{
    // A file from before every command stored its sketch solved: the rectangle's width dimension says 60 while the
    // stored lines are 40 long. Opening it changes nothing (no solve on load: the rebuild shows the solve of what is
    // stored). A parameter edit stores the sketch solved, from the shape that was shown.
    DocState old;
    Parameter h;
    h.id = ParamId{Uuid::fromString(uuid('a', 1))};
    h.name = "h";
    h.expression = "20 mm";
    h.kind = Parameter::Kind::Length;
    old.parameters.push_back(h);
    SketchJson r = rectangle(40, 20);
    r.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 60.0).constraint(7, "distance", {8}, "h");
    Feature f;
    f.id = featureId(2);
    f.type = "sketch";
    f.name = "Sketch 1";
    f.propsJson = canonicalJson(sketchProps(R"({"principal":"xy"})", r));
    old.features.push_back(f);
    Session s(std::make_shared<const DocState>(old));
    CHECK(pointIn(s.stored(), 2).x == 40); // as the file has it
    const sketch::Sketch shown = s.shown();
    CHECK(pointIn(shown, 2).x == Catch::Approx(60));

    s.run("h set to 25", cmd("param.set", paramSet(1, R"("expression":"25 mm")")));
    CHECK(pointIn(s.stored(), 2).x == Catch::Approx(60));
    CHECK(pointIn(s.stored(), 3).y == Catch::Approx(25));
    s.undo("undo: the file's state again");
    CHECK(pointIn(s.stored(), 2).x == 40);
    s.redo("redo");
    // The undo step back to the file's state is the one place a stored sketch is not solved; it is what the file held.
    s.moved.erase(std::remove_if(s.moved.begin(), s.moved.end(),
                                 [](const std::string& step) { return step.rfind("undo: the file's state", 0) == 0; }),
                  s.moved.end());
    s.requireIntact();
}

namespace {

// ---------- Hard sketches for timing the solve a command makes on the calling (UI) thread ----------

// A meander of `periods` x (line, semicircle, line back, semicircle) with every joint tangent, line lengths and arc
// radii dimensioned and the first line's ends fixed: fully constrained, every curve coupled to the one before. 10
// entities per period. With `redundant`, every line is also horizontal (implied by the tangents: redundant).
SketchJson meander(int periods, bool redundant)
{
    constexpr double L = 30, R = 2;
    SketchJson s;
    std::uint64_t e = 10, c = 10;
    std::uint64_t prevEnd = e++;
    s.point(prevEnd, 0, 0);
    s.constraint(c++, "fix", {prevEnd});
    std::uint64_t prevCurve = 0;
    for (int k = 0; k < periods; ++k) {
        const double y0 = 4 * R * k;
        const std::uint64_t p1 = e++, cA = e++, p2 = e++, p3 = e++, cB = e++, p4 = e++;
        const std::uint64_t lA = e++, aA = e++, lB = e++, aB = e++;
        s.point(p1, L, y0).point(cA, L, y0 + R).point(p2, L, y0 + 2 * R);
        s.point(p3, 0, y0 + 2 * R).point(cB, 0, y0 + 3 * R).point(p4, 0, y0 + 4 * R);
        s.line(lA, prevEnd, p1).arc(aA, cA, p1, p2, true).line(lB, p2, p3).arc(aB, cB, p3, p4, false);
        if (k == 0) {
            s.constraint(c++, "fix", {p1});
        } else {
            s.constraint(c++, "tangent", {prevCurve, lA, prevEnd});
            s.constraint(c++, "distance", {lA}, {}, L);
        }
        s.constraint(c++, "tangent", {lA, aA, p1}).constraint(c++, "radius", {aA}, {}, R);
        s.constraint(c++, "tangent", {aA, lB, p2}).constraint(c++, "distance", {lB}, {}, L);
        s.constraint(c++, "tangent", {lB, aB, p3}).constraint(c++, "radius", {aB}, {}, R);
        if (redundant) {
            s.constraint(c++, "horizontal", {lB});
            if (k > 0) {
                s.constraint(c++, "horizontal", {lA});
            }
        }
        prevEnd = p4;
        prevCurve = aB;
    }
    return s;
}

// An n x n grid of 10 mm cells: (n+1)^2 points joined by 2n(n+1) lines sharing them, every line horizontal or
// vertical, the bottom row's and left column's lengths dimensioned and the corner fixed: exactly determined. With
// `allLengths`, every line's length is the parameter s (consistent but heavily redundant).
SketchJson grid(int n, bool allLengths)
{
    SketchJson s;
    const auto pt = [n](int i, int j) {
        return static_cast<std::uint64_t>(100000 + i * (n + 1) + j);
    };
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            s.point(pt(i, j), 10.0 * i, 10.0 * j);
        }
    }
    std::uint64_t line = 500000, c = 10;
    s.constraint(c++, "fix", {pt(0, 0)});
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            if (i < n) {
                s.line(line, pt(i, j), pt(i + 1, j)).constraint(c++, "horizontal", {line});
                if (allLengths) {
                    s.constraint(c++, "distance", {line}, "s");
                } else if (j == 0) {
                    s.constraint(c++, "distance", {line}, {}, 10.0);
                }
                ++line;
            }
            if (j < n) {
                s.line(line, pt(i, j), pt(i, j + 1)).constraint(c++, "vertical", {line});
                if (allLengths) {
                    s.constraint(c++, "distance", {line}, "s");
                } else if (i == 0) {
                    s.constraint(c++, "distance", {line}, {}, 10.0);
                }
                ++line;
            }
        }
    }
    return s;
}

// A chain of `links` lines of length 1 from a fixed point, almost straight (a zig-zag of 1e-3 rad), whose far end is
// held by a point-to-point distance close to the full length: the system is near its singular (fully stretched)
// configuration, where the Jacobian loses rank.
SketchJson stretchedChain(int links, double slack)
{
    SketchJson s;
    double x = 0;
    s.point(10, 0, 0);
    for (int i = 0; i < links; ++i) {
        const double a = (i % 2 ? 1 : -1) * 1e-3;
        x += std::cos(a);
        s.point(static_cast<std::uint64_t>(11 + i), x, (i % 2 ? 0 : -std::sin(1e-3)));
        s.line(static_cast<std::uint64_t>(100000 + i), static_cast<std::uint64_t>(10 + i),
               static_cast<std::uint64_t>(11 + i));
        s.constraint(static_cast<std::uint64_t>(100 + i), "distance", {static_cast<std::uint64_t>(100000 + i)}, {},
                     1.0);
    }
    s.constraint(1, "fix", {10}).constraint(2, "horizontal", {10, static_cast<std::uint64_t>(10 + links)});
    s.constraint(3, "distance", {10, static_cast<std::uint64_t>(10 + links)}, {}, x - slack);
    return s;
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Timing {
    double add = 0;   // feature.sketch.add (the given geometry already satisfies the constraints: a check, no solve)
    double edit = 0;  // the slowest of the edits (each needs a full solve)
    double solve = 0; // a bare solve of the edited sketch (solver time alone)
    std::string status;
    std::size_t entities = 0, constraints = 0;
};

const char* statusName(sketch::SolveStatus s)
{
    switch (s) {
    case sketch::SolveStatus::FullyConstrained:
        return "full";
    case sketch::SolveStatus::UnderConstrained:
        return "under";
    case sketch::SolveStatus::OverConstrained:
        return "redundant";
    case sketch::SolveStatus::Conflicting:
        return "conflicting";
    case sketch::SolveStatus::Failed:
        return "failed";
    }
    return "?";
}

// Adds the sketch (with parameter s = 10 mm), then runs each edit, timing the command (which solves on the calling
// thread) and a bare solve of the result.
Timing timeSketch(const SketchJson& model, const std::vector<Command>& edits)
{
    Session s;
    Timing t;
    REQUIRE(s.env.doc->execute(cmd("param.set", paramSet(9, R"("name":"s","expression":"10 mm","kind":"length")"))));
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(s.env.doc->execute(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", model))));
    t.add = msSince(t0);
    for (const Command& c : edits) {
        t0 = std::chrono::steady_clock::now();
        auto r = s.env.doc->execute(c);
        const double ms = msSince(t0);
        if (!r) {
            FAIL(c.name << ": " << r.error().message);
        }
        t.edit = std::max(t.edit, ms);
    }
    const sketch::Sketch stored = s.stored();
    t.entities = stored.entities.size();
    t.constraints = stored.constraints.size();
    t0 = std::chrono::steady_clock::now();
    const sketch::SolveResult r = solveOf(*s.state(), stored);
    t.solve = msSince(t0);
    t.status = statusName(r.status);
    return t;
}

void printTiming(const char* what, const Timing& t)
{
    std::printf("%-58s %5zu ent %5zu con  add %8.1f  edit %8.1f  solve %8.1f ms  (%s)\n", what, t.entities,
                t.constraints, t.add, t.edit, t.solve, t.status.c_str());
    std::fflush(stdout);
}

} // namespace

TEST_CASE("a sketch command prepared on a worker commits the state execute makes; cancel and stale commits change "
          "nothing",
          "[doc][sketch][solved-store]")
{
    // Document::prepare applies a command on the calling thread (a worker) without touching the document;
    // Document::commit makes it the state on the UI thread. The sketch solve runs inside prepare, cancellably.
    Session s;
    dimensionedByParameters(s);
    Session twin; // the same steps through execute
    dimensionedByParameters(twin);
    REQUIRE(hashOf(s.state()) == hashOf(twin.state()));
    const DocStatePtr start = s.state();
    const std::size_t undos = s.env.doc->undoCount();
    const Command edit = cmd("sketch.constraint.set", constraintSet(6, R"("expression":"33 mm")"), "Edit width");

    std::optional<Result<PreparedCommand>> prepared;
    std::thread worker([&] {
        JobContext ctx;
        prepared.emplace(s.env.doc->prepare(edit, ctx));
    });
    worker.join();
    REQUIRE(prepared->has_value());
    CHECK(s.state() == start); // prepare changes nothing
    CHECK(s.env.doc->undoCount() == undos);
    CHECK((*prepared)->before == start);
    REQUIRE(twin.env.doc->execute(edit));
    CHECK(hashOf((*prepared)->after) == hashOf(twin.state()));

    SECTION("commit makes it the state, with one undo step")
    {
        auto committed = s.env.doc->commit(std::move(**prepared));
        REQUIRE(committed);
        CHECK(hashOf(s.state()) == hashOf(twin.state()));
        CHECK(pointIn(s.stored(), 2).x == Catch::Approx(33));
        CHECK(s.env.doc->undoCount() == undos + 1);
        REQUIRE(s.env.doc->undoCommand());
        CHECK(s.env.doc->undoCommand()->name == "sketch.constraint.set");
        CHECK(s.env.doc->undoCommand()->label == "Edit width");
        s.undo("undo the committed edit");
        CHECK(s.state() == start);
        s.redo("redo it");
        CHECK(hashOf(s.state()) == hashOf(twin.state()));
        s.requireIntact();
    }
    SECTION("a commit after another change is refused and changes nothing")
    {
        REQUIRE(s.env.doc->execute(cmd("param.set", paramSet(1, R"("expression":"35 mm")"))));
        const DocStatePtr now = s.state();
        auto refused = s.env.doc->commit(std::move(**prepared));
        REQUIRE(!refused);
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
        CHECK(s.state() == now);
    }
    SECTION("a cancelled prepare returns Cancelled")
    {
        CancelSource cancel;
        cancel.cancel();
        JobContext ctx{cancel.token(), Progress()};
        auto r = s.env.doc->prepare(edit, ctx);
        REQUIRE(!r);
        CHECK(r.error().code == ErrorCode::Cancelled);
        CHECK(s.state() == start);
        // So do the other commands that may solve: param.set and the generic feature commands.
        auto p = s.env.doc->prepare(cmd("param.set", paramSet(1, R"("expression":"21 mm")")), ctx);
        REQUIRE(!p);
        CHECK(p.error().code == ErrorCode::Cancelled);
        SketchJson off = rectangle(40, 20);
        off.constraint(5, "fix", {1}).constraint(6, "distance", {5}, {}, 70.0);
        auto f = s.env.doc->prepare(cmd("feature.add", R"({"id":")" + uuid('f', 9) +
                                                           R"(","type":"sketch","name":"S","props":)" +
                                                           sketchProps(R"({"principal":"xy"})", off) + "}"),
                                    ctx);
        REQUIRE(!f);
        CHECK(f.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("cancelling a long sketch solve in prepare returns promptly", "[doc][sketch][solved-store]")
{
    // A tangent chain whose solve takes long (hundreds of ms in a release build); cancelled from another thread while
    // the solve runs, prepare returns Cancelled well before the solve would have ended.
    Session s;
    REQUIRE(s.env.doc->execute(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", meander(15, false)))));
    const Command edit = cmd("sketch.constraint.set", constraintSet(15, R"("value":28)"));
    const auto t0 = std::chrono::steady_clock::now();
    {
        JobContext ctx;
        REQUIRE(s.env.doc->prepare(edit, ctx)); // the whole solve, for comparison
    }
    const double full = msSince(t0);
    CancelSource cancel;
    std::optional<Result<PreparedCommand>> r;
    double took = 0;
    std::thread worker([&] {
        JobContext ctx{cancel.token(), Progress()};
        const auto t1 = std::chrono::steady_clock::now();
        r.emplace(s.env.doc->prepare(edit, ctx));
        took = msSince(t1);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(full / 10) + 1));
    cancel.cancel();
    worker.join();
    INFO("full solve " << full << " ms, cancelled prepare " << took << " ms");
    REQUIRE(r);
    if (*r) {
        SUCCEED("the solve ended before the cancel arrived");
    } else {
        CHECK(r->error().code == ErrorCode::Cancelled);
        CHECK(took < full);
    }
}

// The timings of the solve a sketch command makes (printed; run one family with its tag, e.g.
// "[.sketch-solve-timing][meander]"). Each edit forces a full solve inside the command. The constraint ids edited are
// those the generators give: meander dimension c15 (the second line's length, which moves all that follows), the
// grid's first bottom length (c12: moves a column), the stretched chain's end distance c3.
namespace {
Command setValue(std::uint64_t c, double v)
{
    return cmd("sketch.constraint.set", constraintSet(c, R"("value":)" + num(v)));
}
} // namespace

TEST_CASE("time the solve of a tangent chain (printed)", "[.sketch-solve-timing][meander]")
{
    for (int periods : {10, 25, 50, 100}) {
        const std::string what = "meander, tangent chain, " + std::to_string(periods) + " periods";
        printTiming(what.c_str(),
                    timeSketch(meander(periods, false), {setValue(15, 28), setValue(15, 33), setValue(15, 30)}));
        const std::string red = what + ", redundant";
        printTiming(red.c_str(),
                    timeSketch(meander(periods, true), {setValue(15, 28), setValue(15, 33), setValue(15, 30)}));
    }
}

TEST_CASE("time the solve of a grid (printed)", "[.sketch-solve-timing][grid]")
{
    for (int n : {10, 15, 22}) {
        const std::string what = "grid " + std::to_string(n) + "x" + std::to_string(n) + ", exactly determined";
        printTiming(what.c_str(), timeSketch(grid(n, false), {setValue(12, 12), setValue(12, 9), setValue(12, 10)}));
        const std::string red =
            "grid " + std::to_string(n) + "x" + std::to_string(n) + ", every length = s (param.set)";
        printTiming(red.c_str(), timeSketch(grid(n, true), {cmd("param.set", paramSet(9, R"("expression":"12 mm")")),
                                                            cmd("param.set", paramSet(9, R"("expression":"9 mm")"))}));
    }
}

TEST_CASE("time the solve of a chain near its singular configuration (printed)", "[.sketch-solve-timing][stretched]")
{
    for (int links : {200, 500, 1000}) {
        const std::string what = "stretched chain, " + std::to_string(links) + " links, near singular";
        const double full = links; // fully stretched
        printTiming(what.c_str(),
                    timeSketch(stretchedChain(links, 1e-4),
                               {setValue(3, full - 1e-6), setValue(3, full - 1e-3), setValue(3, full - 1e-9)}));
        const std::string bad = "stretched chain, " + std::to_string(links) + " links, edited past full length";
        printTiming(bad.c_str(), timeSketch(stretchedChain(links, 1e-4), {setValue(3, full + 0.5)}));
    }
}

TEST_CASE("time the solve of a long under-constrained chain (printed)", "[.sketch-solve-timing][zigzag]")
{
    // The 200-line zig-zag chain of the timing test below, up to ten times longer.
    for (int lines : {500, 1000, 2000}) {
        SketchJson chain;
        for (int i = 0; i <= lines; ++i) {
            chain.point(static_cast<std::uint64_t>(100000 + i), i * 0.5, (i % 2) * 0.4);
        }
        for (int i = 0; i < lines; ++i) {
            chain.line(static_cast<std::uint64_t>(200000 + i), static_cast<std::uint64_t>(100000 + i),
                       static_cast<std::uint64_t>(100001 + i));
            chain.constraint(static_cast<std::uint64_t>(300000 + i), "distance",
                             {static_cast<std::uint64_t>(200000 + i)}, {}, std::hypot(0.5, 0.4));
        }
        chain.constraint(1, "fix", {100000});
        const std::string what = "zig-zag chain, " + std::to_string(lines) + " lines, under-constrained";
        printTiming(what.c_str(), timeSketch(chain, {setValue(300000, 0.7),
                                                     setValue(static_cast<std::uint64_t>(300000 + lines / 2), 0.8)}));
    }
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
