// Revision pairs for rematchImported (task T-084), judged by the rematch oracle (rematch_oracle.h), which shares no
// code with the naming module.
//
// PLAN.md asks for "five revision pairs made by editing parts in FreeCAD". FreeCAD is not available on the build
// machine, so the pairs are made with RayCAD's own kernel (OpenCASCADE): a base part is edited the way a customer's
// revision changes a part, written with the project's STEP exporter and read back with the importer, as a new revision
// file would be. Both sides of every pair therefore come from one kernel: these pairs do NOT test the differences
// between CAD systems' STEP writers. To carry at least real export quirks, several bases are NIST test parts exported
// by commercial CAD systems (tests/corpus/files/nist-pmi); the others are corpus parts or parts built here.
//
// For every element of the base (faces, edges, vertices) a reference is made as a user's pick would make it
// (IResolver::makeRef on the base) and resolved on the revision (the map rematchImported gave). The oracle judges each
// answer; the table printed per pair is: elements, matched (Exact and right), flagged (Split, Merged, Repaired, or
// Broken where the element changed), removed (Broken, the element is gone), lost (Broken although the same element is
// still there), new (elements of the revision no reference reaches), WRONG. WRONG must be 0.
#include "rematch_oracle.h"

#include <catch2/catch_test_macros.hpp>

#include <ray/naming.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ray;
using kernel::SubId;
using kernel::SubKind;
namespace ro = ray::rematch_oracle;

template <class T>
T take(Result<T> r, const char* what)
{
    INFO(what << ": " << (r ? std::string() : r.error().message));
    REQUIRE(r.has_value());
    return std::move(*r);
}

FeatureId feature()
{
    FeatureId id;
    for (std::size_t i = 0; i < id.value.bytes.size(); ++i)
        id.value.bytes[i] = static_cast<std::uint8_t>(0x40 + i);
    return id;
}
BodyId body()
{
    BodyId id;
    for (std::size_t i = 0; i < id.value.bytes.size(); ++i)
        id.value.bytes[i] = static_cast<std::uint8_t>(0x90 + i);
    return id;
}

std::filesystem::path scratch()
{
    const auto dir = std::filesystem::temp_directory_path() / "raycad_rematch_pairs";
    std::filesystem::create_directories(dir);
    return dir;
}

kernel::Shape importOne(const std::string& path)
{
    JobContext ctx;
    auto imported = take(kernel::importStep(path, kernel::ImportOptions{}, ctx), "import");
    REQUIRE(imported.bodies.size() == 1);
    return imported.bodies.front().shape;
}

// Written with the project's exporter and read back, as a revision file would come in.
kernel::Shape roundTrip(const kernel::Shape& shape, const std::string& name)
{
    const std::string path = (scratch() / (name + ".stp")).string();
    JobContext ctx;
    const std::vector<kernel::Shape> shapes{shape};
    REQUIRE(kernel::exportStep(path, shapes, ctx).has_value());
    return importOne(path);
}

std::string corpus(const std::string& rel)
{
    return std::string(RAY_CORPUS_DIR) + "/" + rel;
}

Vec3 v(double x, double y, double z)
{
    return {x, y, z};
}

kernel::Shape cut(const kernel::Shape& target, const kernel::Shape& tool)
{
    JobContext ctx;
    return take(kernel::boolean(kernel::BoolOp::Cut, target, tool, ctx), "cut").shape;
}

kernel::Shape box(Vec3 a, Vec3 b)
{
    return take(kernel::makeBox(a, b), "box");
}

kernel::Shape cylinder(Vec3 base, Vec3 axis, double r, double h)
{
    return take(kernel::makeCylinder(base, axis, r, h), "cylinder");
}

// Concave cylinders (holes) of radius below `maxRadius`, in SubId order.
std::vector<SubId> holes(const kernel::Shape& shape, double maxRadius)
{
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const SubId f{SubKind::Face, i};
        const auto info = kernel::surfaceInfo(shape, f);
        const auto sample = kernel::faceSample(shape, f);
        if (!info || !sample || info->type != kernel::SurfaceType::Cylinder || info->radius >= maxRadius)
            continue;
        const Vec3 w{sample->point.x - info->origin.x, sample->point.y - info->origin.y,
                     sample->point.z - info->origin.z};
        const double a = w.x * info->axis.x + w.y * info->axis.y + w.z * info->axis.z;
        const Vec3 radial{w.x - a * info->axis.x, w.y - a * info->axis.y, w.z - a * info->axis.z};
        if (radial.x * sample->normal.x + radial.y * sample->normal.y + radial.z * sample->normal.z < 0)
            out.push_back(f);
    }
    return out;
}

std::vector<SubId> planarFacesByArea(const kernel::Shape& shape)
{
    std::vector<std::pair<double, SubId>> faces;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const auto info = kernel::surfaceInfo(shape, {SubKind::Face, i});
        if (info && info->type == kernel::SurfaceType::Plane)
            faces.emplace_back(-info->area, SubId{SubKind::Face, i});
    }
    std::sort(faces.begin(), faces.end());
    std::vector<SubId> out;
    for (const auto& [a, f] : faces)
        out.push_back(f);
    return out;
}

struct Pair {
    std::string name;
    std::string base; // where the base came from
    std::string edit; // what the revision changed
    kernel::Shape before, after;
};

struct Tally {
    std::map<ro::Verdict, int> faces, all;
    int elements = 0, faceElements = 0, newFaces = 0, newAll = 0;
    // Flagged (Split or Merged) although the same geometry is still there: review asked for without need. Not wrong.
    int overFlaggedFaces = 0, overFlagged = 0;
    std::vector<std::string> wrongs;
};

const char* stateName(naming::ResolveState s)
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

Tally judgePair(const Pair& pair)
{
    const naming::ElementMap oldMap = naming::nameImported(feature(), pair.before);
    REQUIRE(oldMap.size() > 0);
    const naming::RematchResult rr = naming::rematchImported(oldMap, pair.before, feature(), pair.after);
    REQUIRE(rr.map.size() > 0);
    const auto oldResolver = naming::makeResolver({naming::ResolverBody{body(), oldMap, pair.before}});
    const auto newResolver = naming::makeResolver({naming::ResolverBody{body(), rr.map, pair.after}});
    const ro::Samples before(pair.before), after(pair.after);
    REQUIRE(before.ok());
    REQUIRE(after.ok());

    Tally t;
    std::array<std::set<std::uint32_t>, 3> reached;
    for (const SubKind kind : {SubKind::Face, SubKind::Edge, SubKind::Vertex}) {
        for (std::uint32_t i = 0; i < pair.before.count(kind); ++i) {
            const SubId pick{kind, i};
            const naming::TopoRef ref = oldResolver->makeRef(body(), pick);
            if (ref.names.empty())
                continue;
            const naming::Resolved r = newResolver->resolve(ref);
            const ro::State state = r.state == naming::ResolveState::Exact      ? ro::State::Exact
                                    : r.state == naming::ResolveState::Split    ? ro::State::Split
                                    : r.state == naming::ResolveState::Merged   ? ro::State::Merged
                                    : r.state == naming::ResolveState::Repaired ? ro::State::Repaired
                                                                                : ro::State::Broken;
            const ro::Judgement j = ro::judge(before, pick, after, state, r.elements);
            ++t.elements;
            ++t.all[j.verdict];
            if (kind == SubKind::Face) {
                ++t.faceElements;
                ++t.faces[j.verdict];
            }
            if (j.verdict == ro::Verdict::Flagged && state != ro::State::Broken &&
                !before.identicalIn(pick, after).empty()) {
                ++t.overFlagged;
                if (kind == SubKind::Face) {
                    ++t.overFlaggedFaces;
                    if (std::getenv("RAYCAD_REMATCH_DEBUG") != nullptr) {
                        std::printf("  over-flagged face %u (%s)\n", i, stateName(r.state));
                        const auto same = before.identicalIn(pick, after).front();
                        const auto x = kernel::surfaceInfo(pair.before, pick);
                        const auto y = kernel::surfaceInfo(pair.after, same);
                        const auto ex = kernel::adjacent(pair.before, pick, SubKind::Edge);
                        const auto ey = kernel::adjacent(pair.after, same, SubKind::Edge);
                        if (x && y)
                            std::printf("    type %d/%d area %.9g/%.9g centroid d %.3g box d %.3g %.3g edges %zu/%zu "
                                        "origin d %.3g\n",
                                        static_cast<int>(x->type), static_cast<int>(y->type), x->area, y->area,
                                        std::abs(x->centroid.x - y->centroid.x) +
                                            std::abs(x->centroid.y - y->centroid.y) +
                                            std::abs(x->centroid.z - y->centroid.z),
                                        std::abs(x->bounds.min.x - y->bounds.min.x) +
                                            std::abs(x->bounds.min.y - y->bounds.min.y) +
                                            std::abs(x->bounds.min.z - y->bounds.min.z),
                                        std::abs(x->bounds.max.x - y->bounds.max.x) +
                                            std::abs(x->bounds.max.y - y->bounds.max.y) +
                                            std::abs(x->bounds.max.z - y->bounds.max.z),
                                        ex.size(), ey.size(),
                                        std::abs(x->origin.x - y->origin.x) + std::abs(x->origin.y - y->origin.y) +
                                            std::abs(x->origin.z - y->origin.z));
                        for (const SubId& e : ex) {
                            const auto c = kernel::curveInfo(pair.before, e);
                            if (c)
                                std::printf("      old edge %u type %d len %.6f (%.4f %.4f %.4f)-(%.4f %.4f %.4f)\n",
                                            e.index, static_cast<int>(c->type), c->length, c->start.x, c->start.y,
                                            c->start.z, c->end.x, c->end.y, c->end.z);
                        }
                        for (const SubId& e : ey) {
                            const auto c = kernel::curveInfo(pair.after, e);
                            if (c)
                                std::printf("      new edge %u type %d len %.6f (%.4f %.4f %.4f)-(%.4f %.4f %.4f)\n",
                                            e.index, static_cast<int>(c->type), c->length, c->start.x, c->start.y,
                                            c->start.z, c->end.x, c->end.y, c->end.z);
                        }
                    }
                }
            }
            for (const SubId& s : r.elements)
                reached[static_cast<std::size_t>(s.kind)].insert(s.index);
            if (j.verdict == ro::Verdict::Wrong && kind == SubKind::Edge) {
                auto describe = [](const kernel::Shape& s, SubId e) {
                    const auto c = kernel::curveInfo(s, e);
                    if (!c)
                        return std::string("unreadable");
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "type %d len %.6f start (%.4f %.4f %.4f) end (%.4f %.4f %.4f)",
                                  static_cast<int>(c->type), c->length, c->start.x, c->start.y, c->start.z, c->end.x,
                                  c->end.y, c->end.z);
                    return std::string(buf);
                };
                std::printf("  edge %u: %s\n", i, describe(pair.before, pick).c_str());
                for (const SubId& s : r.elements)
                    std::printf("   -> %u: %s\n", s.index, describe(pair.after, s).c_str());
            }
            if (j.verdict == ro::Verdict::Wrong)
                t.wrongs.push_back(std::string(kind == SubKind::Face   ? "face "
                                               : kind == SubKind::Edge ? "edge "
                                                                       : "vertex ") +
                                   std::to_string(i) + " " + stateName(r.state) + ": " + j.why);
        }
    }
    // New: elements of the revision that no reference reaches and that share no ground with any base element.
    for (const SubKind kind : {SubKind::Face, SubKind::Edge, SubKind::Vertex})
        for (std::uint32_t i = 0; i < pair.after.count(kind); ++i) {
            if (reached[static_cast<std::size_t>(kind)].count(i) != 0)
                continue;
            if (!after.sharingIn({kind, i}, before).empty())
                continue;
            ++t.newAll;
            if (kind == SubKind::Face)
                ++t.newFaces;
        }
    return t;
}

void report(const Pair& pair, const Tally& t)
{
    auto at = [](const std::map<ro::Verdict, int>& m, ro::Verdict v) {
        const auto it = m.find(v);
        return it == m.end() ? 0 : it->second;
    };
    std::printf("PAIR %-28s | faces %4d: matched %4d flagged %3d removed %3d lost %3d new %3d WRONG %d"
                " | all %5d: matched %5d flagged %4d removed %4d lost %4d new %4d WRONG %d | base %s | %s\n",
                pair.name.c_str(), t.faceElements, at(t.faces, ro::Verdict::Matched), at(t.faces, ro::Verdict::Flagged),
                at(t.faces, ro::Verdict::Removed), at(t.faces, ro::Verdict::Lost), t.newFaces,
                at(t.faces, ro::Verdict::Wrong), t.elements, at(t.all, ro::Verdict::Matched),
                at(t.all, ro::Verdict::Flagged), at(t.all, ro::Verdict::Removed), at(t.all, ro::Verdict::Lost),
                t.newAll, at(t.all, ro::Verdict::Wrong), pair.base.c_str(), pair.edit.c_str());
    std::printf("     %-28s | flagged although unchanged: faces %d, all %d\n", "", t.overFlaggedFaces, t.overFlagged);
    for (const std::string& w : t.wrongs)
        std::printf("  WRONG %s\n", w.c_str());
    std::fflush(stdout);
}

void run(const Pair& pair, bool everythingMatches = false)
{
    INFO(pair.name);
    const Tally t = judgePair(pair);
    report(pair, t);
    CHECK(t.wrongs.empty());
    CHECK(t.all.count(ro::Verdict::Wrong) == 0);
    if (everythingMatches) {
        CHECK(t.all.at(ro::Verdict::Matched) == t.elements);
    } else {
        // Something changed and the rest was carried over.
        CHECK(t.faces.count(ro::Verdict::Matched) != 0);
    }
}

// ---------------------------------------------------------------- the pairs
const char* kCtc01 = "nist-pmi/nist_ctc_01_asme1_rd_geometry.stp";
const char* kCtc03 = "nist-pmi/nist_ctc_03_asme1_rc_geometry.stp";
const char* kFtc06 = "nist-pmi/nist_ftc_06_asme1_rd_geometry.stp";
const char* kFtc09 = "nist-pmi/nist_ftc_09_asme1_rd_geometry.stp";

Pair nistBase(const char* file, const std::string& name)
{
    Pair p;
    p.name = name;
    p.base = std::string("NIST ") + file;
    p.before = importOne(corpus(std::string("files/") + file));
    return p;
}

// The first edit of `tries` that the kernel accepts.
kernel::Shape firstThatWorks(const kernel::Shape& shape, const std::vector<SubId>& candidates,
                             const std::function<Result<kernel::OpResult>(SubId)>& edit, std::string& what)
{
    for (const SubId& s : candidates) {
        auto r = edit(s);
        if (r) {
            what += " (face " + std::to_string(s.index) + ")";
            return r->shape;
        }
    }
    FAIL("no candidate could be edited: " << what);
    return shape;
}

} // namespace

TEST_CASE("rematch revision pairs: identity re-export of a NIST part keeps every name", "[rematch-pairs]")
{
    Pair p = nistBase(kCtc01, "identity-reexport");
    p.edit = "the same part written again by RayCAD's exporter and read back";
    p.after = roundTrip(p.before, "identity");
    run(p, true);
}

// The faces of the first CLOSED_SHELL of a STEP file listed in reverse order: the same geometry, other face (and so
// edge and vertex) numbering when read back.
kernel::Shape reverseShell(const kernel::Shape& shape, const std::string& name)
{
    const std::string path = (scratch() / (name + ".stp")).string();
    JobContext ctx;
    const std::vector<kernel::Shape> shapes{shape};
    REQUIRE(kernel::exportStep(path, shapes, ctx).has_value());
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t at = text.find("CLOSED_SHELL(");
    REQUIRE(at != std::string::npos);
    const std::size_t open = text.find("(#", at);
    const std::size_t close = text.find(')', open);
    REQUIRE((open != std::string::npos && close != std::string::npos));
    std::vector<std::string> ids;
    std::string id;
    for (std::size_t i = open + 1; i < close; ++i) {
        const char c = text[i];
        if (c == ',') {
            ids.push_back(id);
            id.clear();
        } else if (c != ' ' && c != '\r' && c != '\n') {
            id += c;
        }
    }
    ids.push_back(id);
    REQUIRE(ids.size() > 2);
    std::reverse(ids.begin(), ids.end());
    std::string list;
    for (std::size_t i = 0; i < ids.size(); ++i)
        list += (i ? "," : "") + ids[i];
    text = text.substr(0, open + 1) + list + text.substr(close);
    const std::string shuffled = (scratch() / (name + "_reversed.stp")).string();
    {
        std::ofstream out(shuffled, std::ios::binary);
        out << text;
    }
    return importOne(shuffled);
}

// Some face of `a` has another face's place in `b` (the numbering really differs).
bool numberingDiffers(const kernel::Shape& a, const kernel::Shape& b)
{
    for (std::uint32_t i = 0; i < a.count(SubKind::Face); ++i) {
        const auto x = kernel::surfaceInfo(a, {SubKind::Face, i});
        const auto y = kernel::surfaceInfo(b, {SubKind::Face, i});
        if (!x || !y || std::abs(x->centroid.x - y->centroid.x) > 1e-6 ||
            std::abs(x->centroid.y - y->centroid.y) > 1e-6 || std::abs(x->centroid.z - y->centroid.z) > 1e-6)
            return true;
    }
    return false;
}

TEST_CASE("rematch revision pairs: the same part with its faces listed in another order", "[rematch-pairs]")
{
    {
        Pair p = nistBase(kCtc01, "shuffled-order-nist");
        p.edit = "re-exported with the shell's faces in reverse order (every face, edge and vertex renumbered)";
        p.after = reverseShell(p.before, "shuffled_nist");
        CHECK(numberingDiffers(p.before, p.after));
        run(p, true);
    }
    {
        Pair p;
        p.name = "shuffled-order-block";
        p.base = "built here: block with three holes and a slot";
        p.edit = "re-exported with the shell's faces in reverse order";
        kernel::Shape a = box(v(0, 0, 0), v(80, 50, 15));
        for (const kernel::Shape& tool :
             {cylinder(v(15, 15, -1), v(0, 0, 1), 3, 17), cylinder(v(40, 25, -1), v(0, 0, 1), 4, 17),
              cylinder(v(65, 35, -1), v(0, 0, 1), 5, 17), box(v(-1, 20, 10), v(81, 30, 16))})
            a = cut(a, tool);
        p.before = roundTrip(a, "shuffled_block");
        p.after = reverseShell(p.before, "shuffled_block");
        CHECK(numberingDiffers(p.before, p.after));
        run(p, true);
    }
}

TEST_CASE("rematch revision pairs: noise below the tolerance keeps every name", "[rematch-pairs]")
{
    Pair p = nistBase(kCtc01, "noise-5e-5mm");
    p.edit = "the whole part moved 5e-5 mm (below rematch's 1e-3 mm), re-exported";
    Xform x = Xform::identity();
    x.m[3] = 5e-5;
    x.m[7] = -3e-5;
    p.after = roundTrip(take(kernel::transformed(p.before, x), "move").shape, "noise");
    run(p, true);
}

TEST_CASE("rematch revision pairs: a hole resized", "[rematch-pairs]")
{
    Pair p = nistBase(kCtc03, "hole-resized");
    p.edit = "one hole 0.5 mm larger in diameter";
    JobContext ctx;
    p.after = roundTrip(firstThatWorks(
                            p.before, holes(p.before, 10.0),
                            [&](SubId f) {
                                const auto info = kernel::surfaceInfo(p.before, f);
                                return kernel::resizeHole(p.before, f, 2 * info->radius + 0.5, ctx);
                            },
                            p.edit),
                        "hole_resized");
    run(p);
}

TEST_CASE("rematch revision pairs: a hole moved", "[rematch-pairs]")
{
    Pair p = nistBase(kCtc01, "hole-moved");
    p.edit = "one hole filled and drilled again 3 mm away";
    JobContext ctx;
    p.after =
        roundTrip(firstThatWorks(
                      p.before, holes(p.before, 10.0),
                      [&](SubId f) -> Result<kernel::OpResult> {
                          const auto info = kernel::surfaceInfo(p.before, f);
                          auto filled = kernel::deleteFaces(p.before, std::vector<SubId>{f}, ctx);
                          if (!filled)
                              return filled;
                          // Perpendicular to the axis.
                          const Vec3 a = info->axis;
                          Vec3 d = std::abs(a.z) < 0.9 ? Vec3{-a.y, a.x, 0} : Vec3{1, 0, 0};
                          const double n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                          d = {3 * d.x / n, 3 * d.y / n, 3 * d.z / n};
                          const Box3 b = info->bounds;
                          const double len = std::sqrt((b.max.x - b.min.x) * (b.max.x - b.min.x) +
                                                       (b.max.y - b.min.y) * (b.max.y - b.min.y) +
                                                       (b.max.z - b.min.z) * (b.max.z - b.min.z));
                          const Vec3 c{(b.min.x + b.max.x) / 2 + d.x, (b.min.y + b.max.y) / 2 + d.y,
                                       (b.min.z + b.max.z) / 2 + d.z};
                          auto tool = kernel::makeCylinder(
                              {c.x - a.x * len / 2, c.y - a.y * len / 2, c.z - a.z * len / 2}, a, info->radius, len);
                          if (!tool)
                              return tl::unexpected(tool.error());
                          return kernel::boolean(kernel::BoolOp::Cut, filled->shape, *tool, ctx);
                      },
                      p.edit),
                  "hole_moved");
    run(p);
}

TEST_CASE("rematch revision pairs: a hole added", "[rematch-pairs]")
{
    Pair p = nistBase(kFtc06, "hole-added");
    p.edit = "a 3 mm blind hole, 3 mm deep, drilled into the largest flat face";
    const ro::Samples samples(p.before);
    JobContext ctx;
    p.after = roundTrip(firstThatWorks(
                            p.before, planarFacesByArea(p.before),
                            [&](SubId f) -> Result<kernel::OpResult> {
                                const auto point = samples.interiorPoint(f, 4.0);
                                const auto sample = kernel::faceSample(p.before, f);
                                if (!point || !sample)
                                    return tl::unexpected(Error{ErrorCode::InvalidArgument, "no room", {}});
                                const Vec3 n = sample->normal;
                                auto tool = kernel::makeCylinder(
                                    {point->x - 3 * n.x, point->y - 3 * n.y, point->z - 3 * n.z}, n, 1.5, 4);
                                if (!tool)
                                    return tl::unexpected(tool.error());
                                return kernel::boolean(kernel::BoolOp::Cut, p.before, *tool, ctx);
                            },
                            p.edit),
                        "hole_added");
    run(p);
}

TEST_CASE("rematch revision pairs: a hole removed from a bolt circle", "[rematch-pairs]")
{
    Pair p;
    p.name = "bolt-circle-hole-removed";
    p.base = "corpus raycad-synthetic/flange_bolt_circle_1.stp";
    p.edit = "one hole of the bolt circle removed";
    p.before = importOne(corpus("files/raycad-synthetic/flange_bolt_circle_1.stp"));
    // A hole whose radius several holes share: one of the circle.
    std::map<long long, std::vector<SubId>> byRadius;
    for (const SubId& f : holes(p.before, 20.0))
        byRadius[std::llround(kernel::surfaceInfo(p.before, f)->radius * 1000)].push_back(f);
    std::vector<SubId> circle;
    for (const auto& [r, faces] : byRadius)
        if (faces.size() >= 3 && faces.size() > circle.size())
            circle = faces;
    REQUIRE(circle.size() >= 3);
    JobContext ctx;
    p.after = roundTrip(firstThatWorks(
                            p.before, circle,
                            [&](SubId f) { return kernel::deleteFaces(p.before, std::vector<SubId>{f}, ctx); }, p.edit),
                        "bolt_removed");
    run(p);
}

TEST_CASE("rematch revision pairs: a pocket deepened", "[rematch-pairs]")
{
    Pair p;
    p.name = "pocket-deepened";
    p.base = "corpus raycad-synthetic/closed_pocket_1.stp";
    p.edit = "the pocket floor pushed 1.5 mm deeper";
    p.before = importOne(corpus("files/raycad-synthetic/closed_pocket_1.stp"));
    const Box3 all = kernel::bounds(p.before);
    std::vector<SubId> floors;
    for (const SubId& f : planarFacesByArea(p.before)) {
        const auto s = kernel::faceSample(p.before, f);
        const auto info = kernel::surfaceInfo(p.before, f);
        if (s && info && s->normal.z > 0.99 && info->centroid.z < all.max.z - 0.5)
            floors.push_back(f);
    }
    REQUIRE(!floors.empty());
    JobContext ctx;
    p.after =
        roundTrip(firstThatWorks(
                      p.before, floors,
                      [&](SubId f) { return kernel::offsetFaces(p.before, std::vector<SubId>{f}, -1.5, ctx); }, p.edit),
                  "pocket_deeper");
    run(p);
}

TEST_CASE("rematch revision pairs: a fillet radius changed", "[rematch-pairs]")
{
    Pair p;
    p.name = "fillet-radius";
    p.base = "built here: block with the two vertical edges of one end filleted";
    p.edit = "the two fillets on one end 3 mm -> 5 mm";
    auto filleted = [](double r) {
        const kernel::Shape b = box(v(0, 0, 0), v(60, 40, 20));
        std::vector<SubId> vertical;
        for (std::uint32_t i = 0; i < b.count(SubKind::Edge); ++i) {
            const auto c = kernel::curveInfo(b, {SubKind::Edge, i});
            if (c && std::abs(c->start.z - c->end.z) > 19 && c->start.x > 59)
                vertical.push_back({SubKind::Edge, i});
        }
        REQUIRE(vertical.size() == 2);
        JobContext ctx;
        return take(kernel::fillet(b, vertical, r, ctx), "fillet").shape;
    };
    p.before = roundTrip(filleted(3), "fillet3");
    p.after = roundTrip(filleted(5), "fillet5");
    run(p);
}

TEST_CASE("rematch revision pairs: an outer face moved (part made longer)", "[rematch-pairs]")
{
    Pair p = nistBase(kFtc09, "outer-face-moved");
    p.edit = "a flat outer face pulled out 2 mm";
    JobContext ctx;
    p.after =
        roundTrip(firstThatWorks(
                      p.before, planarFacesByArea(p.before),
                      [&](SubId f) { return kernel::offsetFaces(p.before, std::vector<SubId>{f}, 2.0, ctx); }, p.edit),
                  "longer");
    run(p);
}

TEST_CASE("rematch revision pairs: a chamfer added", "[rematch-pairs]")
{
    Pair p = nistBase(kCtc01, "chamfer-added");
    p.edit = "a 0.8 mm chamfer on a straight outer edge";
    std::vector<std::pair<double, SubId>> lines;
    for (std::uint32_t i = 0; i < p.before.count(SubKind::Edge); ++i) {
        const auto c = kernel::curveInfo(p.before, {SubKind::Edge, i});
        if (c && c->type == kernel::CurveType::Line)
            lines.emplace_back(-c->length, SubId{SubKind::Edge, i});
    }
    std::sort(lines.begin(), lines.end());
    std::vector<SubId> edges;
    for (const auto& [l, e] : lines)
        edges.push_back(e);
    JobContext ctx;
    p.after =
        roundTrip(firstThatWorks(
                      p.before, edges,
                      [&](SubId e) { return kernel::chamfer(p.before, std::vector<SubId>{e}, 0.8, ctx); }, p.edit),
                  "chamfer");
    run(p);
}

TEST_CASE("rematch revision pairs: symmetric trap, bolt circle sizes rotated", "[rematch-pairs]")
{
    Pair p;
    p.name = "trap-bolt-circle-rotated";
    p.base = "built here: plate, four holes on a circle, 6 and 8 mm alternating";
    p.edit = "the sizes swapped round the circle (each hole now looks like its neighbour did)";
    auto plate = [](bool swapped) {
        kernel::Shape s = box(v(-50, -50, 0), v(50, 50, 10));
        const double r = 30;
        const Vec3 at[4] = {v(r, 0, -1), v(0, r, -1), v(-r, 0, -1), v(0, -r, -1)};
        for (int i = 0; i < 4; ++i) {
            const bool big = ((i % 2) == 1) != swapped;
            s = cut(s, cylinder(at[i], v(0, 0, 1), big ? 4.0 : 3.0, 12));
        }
        return s;
    };
    p.before = roundTrip(plate(false), "trap_a");
    p.after = roundTrip(plate(true), "trap_b");
    run(p);
}

TEST_CASE("rematch revision pairs: symmetric trap, mirrored pockets with depths swapped", "[rematch-pairs]")
{
    Pair p;
    p.name = "trap-mirrored-pockets";
    p.base = "built here: block, two mirrored pockets 10 and 5 mm deep";
    p.edit = "the depths swapped (left 5, right 10)";
    auto block = [](double left, double right) {
        kernel::Shape s = box(v(0, 0, 0), v(100, 50, 30));
        s = cut(s, box(v(20, 15, 30 - left), v(40, 35, 31)));
        s = cut(s, box(v(60, 15, 30 - right), v(80, 35, 31)));
        return s;
    };
    p.before = roundTrip(block(10, 5), "mirror_a");
    p.after = roundTrip(block(5, 10), "mirror_b");
    run(p);
}
