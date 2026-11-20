// A Shape is an immutable value (PLAN.md section 7 rule 1) and a kernel call's result depends on its arguments only
// (section 8). These tests run a fixed chain of every modifying kernel function, with meshing, queries and export in
// between, on built parts and on imported corpus parts, and check after EVERY call that no shape seen so far changed:
// neither its canonical serialisation (serialize()) nor anything else OCCT keeps on its TShapes (meshes, flags; see
// testing::rawState). Then the chain runs again on the same inputs, and on a fresh copy of them, and must give the same
// results bit for bit.
#include <catch2/catch_test_macros.hpp>

#include "history_test_util.h"
#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <shape_testing.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

namespace fs = std::filesystem;

// Every shape a chain has seen, with its state when first seen.
class Watch {
public:
    explicit Watch(bool check) : check_(check) {}

    void add(std::string name, const Shape& shape)
    {
        if (!check_ || shape.isNull())
            return;
        entries_.push_back({std::move(name), shape, serialize(shape), testing::rawState(shape)});
    }

    // Compares every watched shape with its state before `call`; names in `changes` those that changed, with the
    // first line of testing::rawState() that differs, and takes the new state as the reference for the next call.
    void after(const std::string& call)
    {
        if (!check_)
            return;
        for (Entry& e : entries_) {
            std::vector<std::byte> canonical = serialize(e.shape);
            std::string raw = testing::rawState(e.shape);
            if (canonical != e.canonical)
                changes.push_back(call + " changed the canonical bytes of " + e.name + firstDifference(e.raw, raw));
            else if (raw != e.raw)
                changes.push_back(call + " changed OCCT state outside the canonical bytes of " + e.name +
                                  firstDifference(e.raw, raw));
            e.canonical = std::move(canonical);
            e.raw = std::move(raw);
        }
    }

    std::vector<std::string> changes;

private:
    struct Entry {
        std::string name;
        Shape shape;
        std::vector<std::byte> canonical;
        std::string raw;
    };

    static std::string firstDifference(const std::string& before, const std::string& after)
    {
        std::size_t a = 0;
        std::size_t b = 0;
        while (a < before.size() || b < after.size()) {
            const std::size_t ea = std::min(before.find('\n', a), before.size());
            const std::size_t eb = std::min(after.find('\n', b), after.size());
            const std::string la = before.substr(a, ea - a);
            const std::string lb = after.substr(b, eb - b);
            if (la != lb)
                return ": [" + la + "] -> [" + lb + "]";
            a = ea + 1;
            b = eb + 1;
        }
        return "";
    }
    bool check_;
    std::vector<Entry> entries_;
};

void appendText(std::vector<unsigned char>& out, const std::string& text)
{
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

void appendError(std::vector<unsigned char>& out, const Error& error)
{
    out.push_back(0);
    out.push_back(static_cast<unsigned char>(error.code));
    appendText(out, error.message);
    for (const std::string& line : error.context)
        appendText(out, line);
}

void appendOp(std::vector<unsigned char>& out, const Result<OpResult>& op)
{
    if (!op) {
        appendError(out, op.error());
        return;
    }
    out.push_back(1);
    appendHash(out, op->shape);
    appendHistory(out, op->history);
    for (const std::string& w : op->warnings)
        appendText(out, w);
}

// What a chain produced, step by step (a call or a group of queries each), so that two chains that differ can
// say where they first differ.
class Trace {
public:
    // Starts a step; what is appended to the returned bytes belongs to it. The reference stays valid.
    std::vector<unsigned char>& step(std::string label)
    {
        steps_.emplace_back(std::move(label), std::vector<unsigned char>{});
        return steps_.back().second;
    }
    std::size_t size() const { return steps_.size(); }
    bool operator==(const Trace& other) const { return steps_ == other.steps_; }

    // Empty if equal; else the first step that differs, with a hash of its bytes in each trace.
    std::string firstDifference(const Trace& other) const
    {
        const std::size_t n = std::max(steps_.size(), other.steps_.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (i < steps_.size() && i < other.steps_.size() && steps_[i] == other.steps_[i])
                continue;
            const auto describe = [i](const Trace& t) -> std::string {
                if (i >= t.steps_.size())
                    return "(no such step)";
                std::uint64_t h = 1469598103934665603ull;
                for (const unsigned char c : t.steps_[i].second)
                    h = (h ^ c) * 1099511628211ull;
                std::string text(t.steps_[i].second.begin(), t.steps_[i].second.end());
                for (char& c : text)
                    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F)
                        c = '.';
                char hash[17];
                std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(h));
                return "'" + t.steps_[i].first + "' " + std::to_string(t.steps_[i].second.size()) + " bytes, hash " +
                       hash + ", text [" + text.substr(0, 1200) + "]";
            };
            return "step " + std::to_string(i) + ": " + describe(*this) + " versus " + describe(other);
        }
        return {};
    }

private:
    std::deque<std::pair<std::string, std::vector<unsigned char>>> steps_;
};

void appendStatus(std::vector<unsigned char>& out, const Status& status)
{
    if (status) {
        out.push_back(1);
        return;
    }
    appendError(out, status.error());
}

// Queries, meshes and exports of `shape`, one step each.
void probe(const Shape& shape, const std::string& name, const fs::path& dir, Watch& watch, Trace& trace)
{
    JobContext ctx;
    std::vector<unsigned char>& out = trace.step("queries on " + name);
    const Box3 box = bounds(shape);
    appendBits(out, box.min);
    appendBits(out, box.max);
    if (auto m = massProps(shape)) {
        appendBits(out, m->volume);
        appendBits(out, m->centroid);
    }
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i)
        if (auto s = surfaceInfo(shape, SubId{SubKind::Face, i})) {
            appendBits(out, s->area);
            appendBits(out, s->centroid);
        }
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const SubId edge{SubKind::Edge, i};
        if (auto c = curveInfo(shape, edge))
            appendBits(out, c->length);
        for (const SubId& f : adjacent(shape, edge, SubKind::Face))
            out.push_back(static_cast<unsigned char>(f.index));
    }
    for (std::uint32_t i = 0; i < shape.count(SubKind::Vertex); ++i)
        if (auto v = vertexPoint(shape, SubId{SubKind::Vertex, i}))
            appendBits(out, *v);
    if (shape.count(SubKind::Face) >= 2)
        if (auto d = minDistance(shape, SubId{SubKind::Face, 0}, shape,
                                 SubId{SubKind::Face, shape.count(SubKind::Face) - 1}))
            appendBits(out, d->value);
    watch.after("queries on " + name);

    if (auto mesh = tessellate(shape, MeshOptions{0.2, 0.5}, ctx)) {
        std::vector<unsigned char>& m = trace.step("tessellate on " + name);
        appendBits(m, static_cast<double>(mesh->positions.size()));
        appendBits(m, static_cast<double>(mesh->indices.size()));
    }
    watch.after("tessellate on " + name);
    if (auto soup = triangulateExact(shape, 0.05, ctx)) {
        std::vector<unsigned char>& m = trace.step("triangulateExact on " + name);
        appendBits(m, static_cast<double>(soup->positions.size()));
        appendBits(m, static_cast<double>(soup->indices.size()));
    }
    watch.after("triangulateExact on " + name);

    const std::span<const Shape> one(&shape, 1);
    appendStatus(trace.step("exportStep of " + name), exportStep((dir / "probe.step").string(), one, ctx));
    watch.after("exportStep of " + name);
    appendStatus(trace.step("exportIges of " + name), exportIges((dir / "probe.igs").string(), one, ctx));
    watch.after("exportIges of " + name);
    appendStatus(trace.step("exportStl of " + name), exportStl((dir / "probe.stl").string(), one, 0.1, ctx));
    watch.after("exportStl of " + name);
}

// The planar faces of `shape` by decreasing area (ties by SubId), with their outward normal.
struct PlanarFace {
    SubId id;
    Vec3 centroid;
    Vec3 normal;
    double area;
};
std::vector<PlanarFace> planarFaces(const Shape& shape)
{
    std::vector<PlanarFace> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const SubId id{SubKind::Face, i};
        auto s = surfaceInfo(shape, id);
        if (!s || s->type != SurfaceType::Plane || !(s->area > 1.0))
            continue;
        const Vec3 n = s->reversed ? Vec3{-s->axis.x, -s->axis.y, -s->axis.z} : s->axis;
        out.push_back({id, s->centroid, n, s->area});
    }
    std::stable_sort(out.begin(), out.end(), [](const PlanarFace& a, const PlanarFace& b) { return a.area > b.area; });
    return out;
}

Plane planeOn(const PlanarFace& face)
{
    const Vec3 n = face.normal;
    const Vec3 e = std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 x = cross(e, n);
    const double len = length(x);
    x = Vec3{x.x / len, x.y / len, x.z / len};
    return Plane{face.centroid, n, x};
}

// Straight edges between two planar faces that are not parallel, and circular edges between a plane and a
// cylinder, in SubId order.
void blendCandidates(const Shape& shape, std::vector<SubId>& lines, std::vector<SubId>& circles)
{
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const SubId id{SubKind::Edge, i};
        const std::vector<SubId> faces = adjacent(shape, id, SubKind::Face);
        if (faces.size() != 2)
            continue;
        auto info = curveInfo(shape, id);
        auto a = surfaceInfo(shape, faces[0]);
        auto b = surfaceInfo(shape, faces[1]);
        if (!info || !a || !b)
            continue;
        if (info->type == CurveType::Line && a->type == SurfaceType::Plane && b->type == SurfaceType::Plane &&
            std::abs(std::abs(dot(a->axis, b->axis)) - 1.0) > 1e-6 && info->length > 2.0)
            lines.push_back(id);
        const bool planeCyl = (a->type == SurfaceType::Plane && b->type == SurfaceType::Cylinder) ||
                              (a->type == SurfaceType::Cylinder && b->type == SurfaceType::Plane);
        if (info->type == CurveType::Circle && planeCyl && info->radius > 0.5)
            circles.push_back(id);
    }
}

// The chain: cut, boss, fillet, chamfer, move, intersect, with queries, meshes and exports of every shape in between.
// Returns every result and error, step by step. With `watch` checking, every call is followed by a check of every
// shape seen so far. No Catch2 assertion inside (safe on worker threads).
Trace chain(const Shape& base, const fs::path& dir, Watch& watch)
{
    Trace out;
    JobContext ctx;
    watch.add("the input", base);
    probe(base, "the input", dir, watch, out);

    const Box3 box = bounds(base);
    const double size = length(sub(box.max, box.min));
    const std::vector<PlanarFace> faces = planarFaces(base);
    if (faces.empty())
        return out;

    // A round pocket into the largest planar face.
    const double r1 = std::clamp(0.15 * std::sqrt(faces[0].area), 0.5, 12.0);
    auto tool1 = extrude(profile(planeOn(faces[0]), {circleLoop({0, 0}, r1, 1, 100)}), -0.2 * size, 0.0, ctx);
    watch.after("extrude of the pocket tool");
    if (!tool1)
        return out;
    watch.add("the pocket tool", tool1->op.shape);
    Shape current = base;
    auto cut = boolean(BoolOp::Cut, current, tool1->op.shape, ctx);
    watch.after("the cut");
    appendOp(out.step("the cut"), cut);
    if (cut) {
        current = cut->shape;
        watch.add("the cut result", current);
        probe(current, "the cut result", dir, watch, out);
    }

    // A rectangular boss on the second largest planar face (or the largest).
    const PlanarFace& bossFace = faces.size() > 1 ? faces[1] : faces[0];
    const double w = std::clamp(0.2 * std::sqrt(bossFace.area), 0.5, 15.0);
    auto tool2 = extrude(profile(planeOn(bossFace), {rectangle(-w, -w / 2, w, w / 2, 1, 100)}), 0.1 * size, 0.0, ctx);
    watch.after("extrude of the boss");
    if (tool2) {
        watch.add("the boss", tool2->op.shape);
        auto fused = boolean(BoolOp::Union, current, tool2->op.shape, ctx);
        watch.after("the union");
        appendOp(out.step("the union"), fused);
        if (fused) {
            current = fused->shape;
            watch.add("the union result", current);
            probe(current, "the union result", dir, watch, out);
        }
    }

    // Fillets and chamfers: a straight edge, a circular edge, a group of straight edges; a small and a large size, so
    // that the size search runs too.
    std::vector<SubId> lines;
    std::vector<SubId> circles;
    blendCandidates(current, lines, circles);
    std::vector<std::vector<SubId>> picks;
    if (!lines.empty())
        picks.push_back({lines[lines.size() / 2]});
    if (!circles.empty())
        picks.push_back({circles.front()});
    if (lines.size() > 4)
        picks.emplace_back(lines.begin(), lines.begin() + 4);
    int blendIndex = 0;
    for (const std::vector<SubId>& pick : picks)
        for (const bool isFillet : {true, false})
            for (const double s : {0.4, 0.15 * size}) {
                const std::string what = std::string(isFillet ? "fillet " : "chamfer ") + std::to_string(blendIndex++);
                auto blended = isFillet ? fillet(current, pick, s, ctx) : chamfer(current, pick, s, ctx);
                watch.after(what);
                appendOp(out.step(what), blended);
                if (blended)
                    watch.add("the " + what + " result", blended->shape);
            }
    if (!picks.empty()) {
        auto blended = fillet(current, picks.front(), 0.4, ctx);
        watch.after("the kept fillet");
        if (blended) {
            current = blended->shape;
            probe(current, "the kept fillet result", dir, watch, out);
            if (!circles.empty())
                if (auto again = chamfer(current, std::vector<SubId>{circles.front()}, 0.3, ctx)) {
                    // A blend on a blend result: the circle's SubId is not kept, so this may pick another edge.
                    watch.after("a chamfer after the fillet");
                    appendOp(out.step("a chamfer after the fillet"), again);
                    watch.add("the chamfer after the fillet", again->shape);
                }
        }
    }

    // A rigid move and an intersection with a box that trims the moved body.
    const double c = std::cos(0.3);
    const double s = std::sin(0.3);
    auto moved = transformed(current, Xform{{c, -s, 0, 5, s, c, 0, -3, 0, 0, 1, 2}});
    watch.after("transformed");
    appendOp(out.step("transformed"), moved);
    if (moved) {
        watch.add("the moved body", moved->shape);
        const Box3 mb = bounds(moved->shape);
        const Vec3 mid{(mb.min.x + mb.max.x) / 2, (mb.min.y + mb.max.y) / 2, (mb.min.z + mb.max.z) / 2};
        if (auto trim =
                makeBox(Vec3{mb.min.x - 1, mb.min.y - 1, mb.min.z - 1}, Vec3{mid.x, mb.max.y + 1, mb.max.z + 1})) {
            watch.add("the trim box", *trim);
            auto common = boolean(BoolOp::Intersect, moved->shape, *trim, ctx);
            watch.after("the intersection");
            appendOp(out.step("the intersection"), common);
            if (common) {
                watch.add("the intersection result", common->shape);
                probe(common->shape, "the intersection result", dir, watch, out);
            }
        }
    }
    // Serialisation round trip of the final shape is itself a call on an input.
    if (auto back = deserialize(serialize(current)))
        appendHash(out.step("serialize"), *back);
    watch.after("serialize");
    return out;
}

class TempDir {
public:
    explicit TempDir(const std::string& name)
    {
        path_ = fs::temp_directory_path() / (name + " " + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

Shape importFirst(const std::string& relative)
{
    JobContext ctx;
    auto imported = importStep((fs::path(RAY_CORPUS_DIR) / relative).string(), ImportOptions{}, ctx);
    INFO(relative);
    REQUIRE(imported.has_value());
    const auto solid = std::find_if(imported->bodies.begin(), imported->bodies.end(),
                                    [](const ImportedBody& body) { return body.shape.count(SubKind::Solid) > 0; });
    // One return after the check: MSVC 14.51 Debug reports a return after FAIL as unreachable code (C4702).
    if (solid == imported->bodies.end())
        FAIL("no solid in " << relative);
    return solid == imported->bodies.end() ? Shape{} : solid->shape;
}

// Runs the chain on `base` with every check: no call changes any shape, and the chain gives the same bytes when run
// again on the same shapes and on a copy made by a serialisation round trip.
void checkChain(const Shape& base, const std::string& name)
{
    INFO(name);
    TempDir dir("raycad immutability");
    Watch first(true);
    const Trace reference = chain(base, dir.path(), first);
    REQUIRE(reference.size() > 10);
    for (const std::string& change : first.changes)
        UNSCOPED_INFO(change);
    CHECK(first.changes.empty());

    Watch second(true);
    const std::string again = reference.firstDifference(chain(base, dir.path(), second));
    CHECK(again == std::string());
    for (const std::string& change : second.changes)
        UNSCOPED_INFO(change);
    CHECK(second.changes.empty());

    const Shape copy = take(deserialize(serialize(base)));
    Watch none(false);
    const std::string onCopy = reference.firstDifference(chain(copy, dir.path(), none));
    CHECK(onCopy == std::string());
}

} // namespace

TEST_CASE("no kernel call changes a built shape, and a repeated chain gives the same results", "[kernel][immutable]")
{
    checkChain(take(testing::makeCutBody()), "cut body");
    checkChain(take(testing::makeFilletedBody()), "filleted body");
    JobContext ctx;
    const Profile plate = profile(
        xyPlane(), {rectangle(0, 0, 120, 80, 1, 100), circleLoop({20, 20}, 6, 11, 101), slot(50, 90, 50, 8, 21, 102)});
    checkChain(take(extrude(plate, 25.0, 0.0, ctx)).op.shape, "extruded plate");
}

TEST_CASE("no kernel call changes an imported shape, and a repeated chain gives the same results",
          "[kernel][immutable][corpus]")
{
    for (const char* part : {"raycad-synthetic/block_blind_holes_1.stp", "raycad-synthetic/keyway_tslot_1.stp",
                             "raycad-synthetic/fillet_block_1.stp", "raycad-synthetic/l_bracket_1.stp",
                             "nist-pmi/nist_ftc_11_asme1_rb_geometry.stp", "nist-d2mi/827-9999-905.stp"})
        checkChain(importFirst(part), part);
}

TEST_CASE("chains on one shared shape in parallel give the serial results and leave it untouched",
          "[kernel][immutable][threads]")
{
    const Shape base = importFirst("raycad-synthetic/keyway_tslot_1.stp");
    const std::vector<std::byte> before = serialize(base);
    const std::string rawBefore = testing::rawState(base);
    TempDir dir("raycad immutability threads");
    Watch none(false);
    const Trace reference = chain(base, dir.path(), none);
    constexpr int kThreads = 4;
    std::vector<Trace> results(kThreads);
    runOnThreads(kThreads, [&](int t) {
        TempDir mine("raycad immutability thread " + std::to_string(t));
        Watch off(false);
        results[static_cast<std::size_t>(t)] = chain(base, mine.path(), off);
    });
    for (const Trace& r : results)
        CHECK(reference.firstDifference(r) == std::string());
    const bool canonicalKept = serialize(base) == before;
    CHECK(canonicalKept);
    const bool rawKept = testing::rawState(base) == rawBefore;
    CHECK(rawKept);
}
