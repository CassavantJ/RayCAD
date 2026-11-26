// Randomised edit sequences with direct edits (task T-081b acceptance): the T-081a randomised test (random_test.cpp)
// extended with the T-080 direct edits. Each sequence imports one of the same 20 corpus parts and builds 3 to 5 random
// steps, a third of them direct edits interleaved with the T-094/T-095 operations (cut, boss, fillet, chamfer): delete
// a hole (the part's own or a cut's), delete a pocket, delete a fillet or chamfer (a blend feature's, or a run of the
// part's own rounds), resize a hole, push/pull a flat face (sometimes exactly to the level of another face on the same
// side, so the two unify), merge faces on one surface. It records every element the oracle can vouch for as a
// reference, makes one to three random edits (the T-081a script edits, which then replay the direct edits in the
// history through their own face references, or a further direct edit appended to the part), rebuilds, and judges every
// reference and every replayed pick (blends' edges, direct edits' faces) with the full oracle, whose model of a direct
// edit (oracle.h EditSpec) uses geometry only. A direct edit the kernel refuses (KernelFailure, InvalidArgument), runs
// past its time limit, or whose result the oracle cannot describe is a skipped step, counted by kind and outcome.
//
// The generator below repeats the T-081a one (random_test.cpp: same splitmix64 conversions, profiles, cuts, bosses and
// blends) on purpose, on its own seeds: the T-081a blocks keep their seeds and sequences unchanged.
//
// Gates, as the T-081a holdout blocks: zero wrong-element resolutions, every mutant caught, no usable Repaired, no
// injectivity violation, every new-territory acceptance and congruence conflict on the reviewed lists, and every
// answer not judged on the reviewed list (tests/naming/oracle/reviewed_unjudged.txt, keys "directedit ...").
// RAYCAD_NAMING_ONLY=<part>:<sequence> runs one sequence; RAYCAD_NAMING_TRACE=<file> writes one line per sequence (the
// Windows and Linux runs are compared on it).
//
// Windows and Linux do not give the same answers per sequence, and nobody should pin cross-platform goldens on these
// blocks: the requirement is zero wrong answers on each platform. Measured on the 1,000 sequences of the main block
// (T-081b): 94 sequences differ between the platforms with the generator that picks faces by index, 50 with the one
// that picks them by geometry (SequenceOptions::byIndex; faces sorted by their rounded boxes). The cause is
// OpenCASCADE, not the naming code: its booleans and direct edits give a different topology or a different sub-shape
// order on the two platforms for the same input (a boss joined to a rib plate gives 17 faces on Windows and 16 on
// Linux, the coplanar start cap unified on one only; an offset boolean lists the split faces the other way round).
// Picking by index then picks another face, and every later step and reference differs; picking by geometry still
// differs wherever the topology does. The main block keeps the generator by index (its replays are pinned on it); the
// holdout blocks use the one by geometry.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"

#include <directedit_testing.h>
#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>

using namespace ray;
using namespace ray::naming_oracle;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr int kSequencesPerPart = 50;
constexpr int kSmokePerPart = 2;

const char* const kParts[] = {
    "raycad-synthetic/block_blind_holes_1.stp",
    "raycad-synthetic/chamfer_block_2.stp",
    "raycad-synthetic/circular_pocket_1.stp",
    "raycad-synthetic/closed_pocket_3.stp",
    "raycad-synthetic/fillet_block_1.stp",
    "raycad-synthetic/fixture_plate_1.stp",
    "raycad-synthetic/flange_bolt_circle_1.stp",
    "raycad-synthetic/keyway_tslot_1.stp",
    "raycad-synthetic/l_bracket_1.stp",
    "raycad-synthetic/manifold_block_1.stp",
    "raycad-synthetic/open_pocket_step_1.stp",
    "raycad-synthetic/plate_counterbore_1.stp",
    "raycad-synthetic/plate_countersink_1.stp",
    "raycad-synthetic/plate_thru_holes_1.stp",
    "raycad-synthetic/rib_plate_1.stp",
    "raycad-synthetic/slot_through_1.stp",
    "raycad-synthetic/thin_wall_box_1.stp",
    "nist-pmi/nist_ftc_11_asme1_rb_geometry.stp",
    "nist-d2mi/827-9999-905.stp",
    "nist-pmi/nist_ctc_01_asme1_rd_geometry.stp",
};
constexpr int kPartCount = static_cast<int>(sizeof(kParts) / sizeof(kParts[0]));

// ---------------------------------------------------------------- the T-081a generator (random_test.cpp)
struct Rng {
    std::uint64_t s;
    std::uint64_t next()
    {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform(double a, double b) { return a + (b - a) * static_cast<double>(next() >> 11) * 0x1.0p-53; }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
    bool chance(double p) { return uniform(0, 1) < p; }
};

struct Part {
    std::string path;
    std::shared_ptr<const ImportBase> base;
    std::shared_ptr<const Samples> samples;
    double size = 0; // bounding box diagonal
    std::string error;
};

Part loadPart(const std::string& relative)
{
    Part part;
    part.path = relative;
    JobContext ctx;
    auto imported = kernel::importStep(std::string(RAY_CORPUS_DIR) + "/" + relative, kernel::ImportOptions{}, ctx);
    if (!imported || imported->bodies.empty()) {
        part.error = imported ? "no body" : imported.error().message;
        return part;
    }
    const kernel::Shape shape = imported->bodies.front().shape;
    auto base = analyseImport(shape);
    auto samples = sampleShape(shape);
    if (!base || !samples) {
        part.error = !base ? base.error().message : samples.error().message;
        return part;
    }
    part.base = *base;
    part.samples = *samples;
    const Box3 b = kernel::bounds(shape);
    part.size = norm(sub(b.max, b.min));
    return part;
}

Ent lineEnt(double x0, double y0, double x1, double y1, std::uint64_t tag)
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
Ent arcEnt(double x0, double y0, double x1, double y1, double cx, double cy, double r, std::uint64_t tag)
{
    Ent e = lineEnt(x0, y0, x1, y1, tag);
    e.k = 'A';
    e.cx = cx;
    e.cy = cy;
    e.r = r;
    return e;
}

Loop randomProfile(Rng& rng, double s)
{
    Loop l;
    l.id = 1;
    const double w = s * rng.uniform(0.6, 1.4), h = s * rng.uniform(0.6, 1.4);
    switch (rng.below(4)) {
    case 0:
        l.ents = {lineEnt(-w / 2, -h / 2, w / 2, -h / 2, 1), lineEnt(w / 2, -h / 2, w / 2, h / 2, 2),
                  lineEnt(w / 2, h / 2, -w / 2, h / 2, 3), lineEnt(-w / 2, h / 2, -w / 2, -h / 2, 4)};
        break;
    case 1: {
        Ent e;
        e.k = 'C';
        e.r = s / 2;
        e.tag = 1;
        e.x0 = e.x1 = e.r;
        l.ents = {e};
        break;
    }
    case 2: {
        const double r = h / 3, x = w / 2;
        l.ents = {lineEnt(-x, -r, x, -r, 1), arcEnt(x, -r, x, r, x, 0, r, 2), lineEnt(x, r, -x, r, 3),
                  arcEnt(-x, r, -x, -r, -x, 0, r, 4)};
        break;
    }
    default: {
        const double r = std::min(w, h) * 0.2, x0 = -w / 2, x1 = w / 2, y0 = -h / 2, y1 = h / 2;
        l.ents = {lineEnt(x0 + r, y0, x1 - r, y0, 1), arcEnt(x1 - r, y0, x1, y0 + r, x1 - r, y0 + r, r, 5),
                  lineEnt(x1, y0 + r, x1, y1 - r, 2), arcEnt(x1, y1 - r, x1 - r, y1, x1 - r, y1 - r, r, 6),
                  lineEnt(x1 - r, y1, x0 + r, y1, 3), arcEnt(x0 + r, y1, x0, y1 - r, x0 + r, y1 - r, r, 7),
                  lineEnt(x0, y1 - r, x0, y0 + r, 4), arcEnt(x0, y0 + r, x0 + r, y0, x0 + r, y0 + r, r, 8)};
        break;
    }
    }
    return l;
}

double profileWidth(const Loop& l)
{
    double lo = 1e300, hi = -1e300;
    for (const Ent& e : l.ents) {
        if (e.k == 'C') {
            lo = std::min(lo, e.cx - e.r);
            hi = std::max(hi, e.cx + e.r);
        } else {
            lo = std::min({lo, e.x0, e.x1});
            hi = std::max({hi, e.x0, e.x1});
        }
    }
    return hi - lo;
}

// With SequenceOptions::byIndex false, every choice among elements of a built shape is made from a list sorted by
// geometry rounded to 1e-3 mm, not in the kernel's sub-shape order: that order (and the last bits of the geometry)
// differs between platforms after some edits (a boolean listed two faces the other way round on Linux, a boss join gave
// 17 faces on Windows and 16 on Linux), and a choice by index then gives another sequence there.
// SequenceOptions::byIndex of the sequence the thread is running.
thread_local bool tByIndex = true;

std::array<std::int64_t, 3> roundedKey(const Vec3& p)
{
    return {std::llround(p.x * 1e3), std::llround(p.y * 1e3), std::llround(p.z * 1e3)};
}
std::array<std::int64_t, 6> boxKey(const ElemSamples& es)
{
    const auto lo = roundedKey(es.boxMin), hi = roundedKey(es.boxMax);
    return {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
}

// SequenceOptions::portable of the sequence the thread is running (T-081b round 4, item 9): choices by geometry that do
// not turn on the last bits of the geometry. Opt-in, for blocks authored after round 4: every block above keeps the
// generator it was tuned or held out on (its pinned replays are sequences of that generator).
thread_local bool tPortable = false;

// The ranks of `values` once values within `tol` of the next one up are counted as one (single linkage over the
// sorted values): two values a platform's last bits tell apart get one rank, and the order of the ranks is the order of
// the values. Ties keep their order by the caller's next key.
std::vector<std::size_t> tolerantRanks(const std::vector<double>& values, double tol)
{
    std::vector<std::size_t> order(values.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return values[a] < values[b]; });
    std::vector<std::size_t> rank(values.size(), 0);
    std::size_t r = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        if (k > 0 && values[order[k]] - values[order[k - 1]] > tol)
            ++r;
        rank[order[k]] = r;
    }
    return rank;
}

// Sorts `items` by their boxes (min corner, then max corner), each coordinate compared by tolerantRanks (1e-6 mm), the
// ties by `next` (a key that holds on every platform), then as they came.
template <class Item, class BoxOf, class Next>
void sortByBoxes(std::vector<Item>& items, BoxOf boxOf, Next next)
{
    std::array<std::vector<double>, 6> coords;
    for (const Item& it : items) {
        const std::array<double, 6> b = boxOf(it);
        for (std::size_t c = 0; c < 6; ++c)
            coords[c].push_back(b[c]);
    }
    std::array<std::vector<std::size_t>, 6> ranks;
    for (std::size_t c = 0; c < 6; ++c)
        ranks[c] = tolerantRanks(coords[c], 1e-6);
    std::vector<std::size_t> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        for (std::size_t c = 0; c < 6; ++c)
            if (ranks[c][a] != ranks[c][b])
                return ranks[c][a] < ranks[c][b];
        return next(items[a]) < next(items[b]);
    });
    std::vector<Item> sorted;
    for (const std::size_t i : order)
        sorted.push_back(items[i]);
    items = std::move(sorted);
}

std::array<double, 6> boxOfSamples6(const ElemSamples& es)
{
    return {es.boxMin.x, es.boxMin.y, es.boxMin.z, es.boxMax.x, es.boxMax.y, es.boxMax.z};
}

// v rounded to a multiple of `q` (the portable generator's way of making a value a platform computes in its last bits
// the same everywhere, except within 1e-12 of a rounding boundary).
double snapped(double v, double q)
{
    return std::round(v / q) * q;
}

std::optional<Feature> proposeSweep(const Samples& smp, double partSize, Rng& rng, const std::string& id, bool cut)
{
    std::vector<std::uint32_t> planar;
    const auto& faces = smp.of(SubKind::Face);
    for (std::uint32_t i = 0; i < faces.size(); ++i)
        if (faces[i].type == static_cast<int>(kernel::SurfaceType::Plane) && faces[i].pts.size() >= 3)
            planar.push_back(i);
    if (planar.empty())
        return std::nullopt;
    if (tPortable)
        sortByBoxes(
            planar, [&](std::uint32_t i) { return boxOfSamples6(faces[i]); },
            [&](std::uint32_t i) {
                const Vec3 n = unit(faces[i].nrm);
                return std::array<long long, 3>{std::llround(n.x * 1e3), std::llround(n.y * 1e3),
                                                std::llround(n.z * 1e3)};
            });
    else if (!tByIndex)
        std::stable_sort(planar.begin(), planar.end(),
                         [&](std::uint32_t a, std::uint32_t b) { return boxKey(faces[a]) < boxKey(faces[b]); });
    const ElemSamples& f = faces[planar[rng.below(planar.size())]];
    std::vector<Vec3> pts = f.pts;
    if (!tByIndex && !tPortable)
        std::stable_sort(pts.begin(), pts.end(),
                         [](const Vec3& a, const Vec3& b) { return roundedKey(a) < roundedKey(b); });
    Vec3 point{};
    if (tPortable) {
        // Not a mesh sample (the meshes differ between platforms): a point drawn in the face's box, on its plane
        // (through
        // the samples' mean), snapped to 1e-6 mm. It may lie off the face, beside it: the profile is still placed
        // there.
        Vec3 mean{0, 0, 0};
        for (const Vec3& p : pts)
            mean = add(mean, mul(p, 1.0 / static_cast<double>(pts.size())));
        const Vec3 nn = unit(f.nrm);
        const Vec3 at{f.boxMin.x + (f.boxMax.x - f.boxMin.x) * rng.uniform(0.2, 0.8),
                      f.boxMin.y + (f.boxMax.y - f.boxMin.y) * rng.uniform(0.2, 0.8),
                      f.boxMin.z + (f.boxMax.z - f.boxMin.z) * rng.uniform(0.2, 0.8)};
        const Vec3 on = sub(at, mul(nn, dot(sub(at, mean), nn)));
        point = {snapped(on.x, 1e-6), snapped(on.y, 1e-6), snapped(on.z, 1e-6)};
    } else {
        point = pts[rng.below(pts.size())];
    }
    Vec3 n = unit(f.nrm);
    if (tPortable)
        n = unit(Vec3{snapped(n.x, 1e-9), snapped(n.y, 1e-9), snapped(n.z, 1e-9)});
    const Vec3 e = std::abs(n.x) <= std::abs(n.y) && std::abs(n.x) <= std::abs(n.z) ? Vec3{1, 0, 0}
                   : std::abs(n.y) <= std::abs(n.z)                                 ? Vec3{0, 1, 0}
                                                                                    : Vec3{0, 0, 1};
    const Vec3 x0 = unit(cross(e, n)), y0 = cross(n, x0);
    const double a = rng.uniform(0, kPi);
    Frame fr;
    fr.n = n;
    fr.xd = add(mul(x0, std::cos(a)), mul(y0, std::sin(a)));
    fr.o = point;
    const double extent = norm(sub(f.boxMax, f.boxMin));
    const double s = std::clamp(extent * rng.uniform(0.1, 0.35), 0.8, 40.0);
    Feature feat;
    feat.id = id;
    feat.type = FType::Sweep;
    feat.op = cut ? BOp::Cut : BOp::Join;
    feat.frame = fr;
    feat.loops = {randomProfile(rng, s)};
    feat.role = cut && feat.loops[0].ents.size() == 1 ? "hole" : "extrude";
    feat.dist = cut ? (rng.chance(0.3) ? 2.0 * partSize : partSize * rng.uniform(0.04, 0.3))
                    : partSize * rng.uniform(0.04, 0.25);
    return feat;
}

std::optional<bool> convexity(const Owner& A, const ElemSamples& edge, const ElemSamples& faceB)
{
    if (edge.nodes.empty() || faceB.pts.empty())
        return std::nullopt;
    const Vec3 m = edge.nodes[edge.nodes.size() / 2];
    Vec3 nA;
    if (A.c.k == Carrier::K::Plane) {
        nA = A.c.a;
    } else if (A.c.k == Carrier::K::Cyl) {
        const Vec3 d = sub(m, A.c.p);
        nA = mul(unit(sub(d, mul(A.c.a, dot(d, A.c.a)))), static_cast<double>(A.c.sigma));
    } else {
        return std::nullopt;
    }
    double best = 1e300;
    Vec3 q{};
    for (const Vec3& p : faceB.pts) {
        const double d = norm(sub(p, m));
        if (d > 1e-3 && d < best) {
            best = d;
            q = p;
        }
    }
    const double s = dot(nA, unit(sub(q, m)));
    if (std::abs(s) < 0.05)
        return std::nullopt;
    return s < 0;
}

std::optional<Feature> proposeBlend(const Script& built, const Samples& smp, Rng& rng, const std::string& id,
                                    bool fillet)
{
    const Owners owners = buildOwners(built);
    const TruthBuilder tb(owners, smp);
    const auto& edges = smp.of(SubKind::Edge);
    std::vector<std::pair<std::uint32_t, Target>> candidates;
    for (std::uint32_t e = 0; e < edges.size(); ++e) {
        const auto& fs = smp.facesOf[1][e];
        if (fs.size() != 2 || edges[e].nodes.empty())
            continue;
        const TruthDesc& da = tb.faceDescriptions()[fs[0]];
        const TruthDesc& db = tb.faceDescriptions()[fs[1]];
        if (!da.ok || !db.ok || da.cons[0].alts.size() != 1 || db.cons[0].alts.size() != 1)
            continue;
        const Owner* A = owners.get(da.cons[0].alts[0]);
        if (A == nullptr || da.cons[0].alts[0] == db.cons[0].alts[0])
            continue;
        const auto convex = convexity(*A, edges[e], smp.of(SubKind::Face)[fs[1]]);
        if (!convex)
            continue;
        const Target t{da.cons[0].alts[0], db.cons[0].alts[0], *convex, true};
        if (canBlend(owners, t, fillet))
            candidates.push_back({e, t});
    }
    if (tPortable)
        sortByBoxes(
            candidates, [&](const std::pair<std::uint32_t, Target>& c) { return boxOfSamples6(edges[c.first]); },
            [](const std::pair<std::uint32_t, Target>& c) { return std::make_tuple(c.second.a, c.second.b); });
    else if (!tByIndex)
        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& x, const auto& y) {
            return std::make_tuple(boxKey(edges[x.first]), x.second.a, x.second.b) <
                   std::make_tuple(boxKey(edges[y.first]), y.second.a, y.second.b);
        });
    if (std::getenv("RAYCAD_NAMING_STEPS") != nullptr) {
        std::string list;
        for (const auto& [e, t] : candidates)
            list += " " + std::to_string(e) + ":" + t.a + "/" + t.b + (t.convex ? "+" : "-");
        std::printf("  blend candidates (%zu):%s\n", candidates.size(), list.c_str());
    }
    if (candidates.empty())
        return std::nullopt;
    Feature f;
    f.id = id;
    f.type = fillet ? FType::Fillet : FType::Chamfer;
    f.role = fillet ? "fillet" : "chamfer";
    f.targets = {candidates[rng.below(candidates.size())].second};
    f.size = rng.uniform(0.3, 1.2);
    return f;
}

// ---------------------------------------------------------------- direct edits
// The kinds, by the role the edit gets (its feature role in the names it makes, and its bucket in the counts).
const char* const kEditRoles[] = {"delete_hole", "delete_pocket", "delete_fillet",
                                  "resize_hole", "offset_face",   "unify"};

// The faces of `key` on the shape, each described by that owner alone (what an edit's pick needs); empty if any face
// on it is not.
std::vector<std::uint32_t> ownFaces(const std::string& key, const Owners& owners, const TruthBuilder& tb,
                                    const Samples& smp)
{
    std::vector<std::uint32_t> out = facesOn(key, owners, smp);
    for (const std::uint32_t f : out) {
        const TruthDesc& d = tb.faceDescriptions()[f];
        if (!d.ok || d.cons.size() != 1 || d.cons[0].alts != std::vector<std::string>{key})
            return {};
    }
    return out;
}

bool isHole(const Owner& o)
{
    return o.c.k == Carrier::K::Cyl && o.c.oriented && o.c.sigma < 0;
}

// A random direct edit of the shape `smp` samples, built by `built`; nullopt if the kind drawn has no candidate.
std::optional<Feature> proposeEdit(const Script& built, const kernel::Shape& shape, const Samples& smp, double partSize,
                                   Rng& rng, const std::string& id)
{
    const Owners owners = buildOwners(built);
    const TruthBuilder tb(owners, smp);
    Feature f;
    f.id = id;
    f.type = FType::Edit;
    const std::size_t roll = rng.below(100);
    std::vector<std::string> keys; // candidate owners, in owner order
    if (roll < 22) {
        f.role = "delete_hole";
        f.edit.kind = EKind::Delete;
        for (const Owner& o : owners.v)
            if (isHole(o) && !ownFaces(o.key, owners, tb, smp).empty())
                keys.push_back(o.key);
        if (keys.empty())
            return std::nullopt;
        f.edit.targets = {keys[rng.below(keys.size())]};
        return f;
    }
    if (roll < 32) {
        // A cut that is not a hole: all its faces still on the shape.
        f.role = "delete_pocket";
        f.edit.kind = EKind::Delete;
        std::vector<std::vector<std::string>> pockets;
        for (const auto& [sid, sweep] : owners.sweeps) {
            if (sweep.op != BOp::Cut || sweep.role == "hole")
                continue;
            std::vector<std::string> faces;
            bool ok = true;
            for (const Owner& o : owners.v)
                if (o.key.compare(0, sid.size() + 1, sid + "/") == 0) {
                    if (!facesOn(o.key, owners, smp).empty()) {
                        ok = ok && !ownFaces(o.key, owners, tb, smp).empty();
                        faces.push_back(o.key);
                    }
                }
            if (ok && !faces.empty())
                pockets.push_back(faces);
        }
        if (pockets.empty())
            return std::nullopt;
        f.edit.targets = pockets[rng.below(pockets.size())];
        return f;
    }
    if (roll < 47) {
        // A blend: a fillet or chamfer feature's face, or a run of the part's own rounds (kernel::blendRun picks the
        // run, as the UI does).
        f.role = "delete_fillet";
        f.edit.kind = EKind::Delete;
        std::vector<std::vector<std::string>> runs;
        for (const Owner& o : owners.v)
            if ((o.cat == "fillet face" || o.cat == "chamfer face") && !ownFaces(o.key, owners, tb, smp).empty())
                runs.push_back({o.key});
        const auto& faces = smp.of(SubKind::Face);
        std::set<std::string> seen;
        for (std::uint32_t i = 0; i < faces.size(); ++i) {
            const TruthDesc& d = tb.faceDescriptions()[i];
            if (!d.ok || d.cons.size() != 1 || d.cons[0].alts.size() != 1 || seen.count(d.cons[0].alts[0]) != 0)
                continue;
            const Owner* o = owners.get(d.cons[0].alts[0]);
            if (o == nullptr || o->cat != "imported face")
                continue;
            const std::vector<SubId> run = kernel::blendRun(shape, SubId{SubKind::Face, i});
            if (run.empty())
                continue;
            std::set<std::string> keys2;
            bool ok = true;
            for (const SubId& r : run) {
                const TruthDesc& dr = tb.faceDescriptions()[r.index];
                ok = ok && dr.ok && dr.cons.size() == 1 && dr.cons[0].alts.size() == 1;
                if (ok)
                    keys2.insert(dr.cons[0].alts[0]);
            }
            for (const std::string& k : keys2)
                ok = ok && !ownFaces(k, owners, tb, smp).empty();
            seen.insert(keys2.begin(), keys2.end());
            if (ok && !keys2.empty())
                runs.push_back(std::vector<std::string>(keys2.begin(), keys2.end()));
        }
        // (In the owners' order, not in the order of the faces they were found from.)
        if (!tByIndex) {
            std::sort(runs.begin(), runs.end());
            runs.erase(std::unique(runs.begin(), runs.end()), runs.end());
        }
        if (runs.empty())
            return std::nullopt;
        f.edit.targets = runs[rng.below(runs.size())];
        return f;
    }
    if (roll < 65) {
        f.role = "resize_hole";
        f.edit.kind = EKind::Resize;
        for (const Owner& o : owners.v)
            if (isHole(o) && !ownFaces(o.key, owners, tb, smp).empty())
                keys.push_back(o.key);
        if (keys.empty())
            return std::nullopt;
        const Owner& o = *owners.get(keys[rng.below(keys.size())]);
        f.edit.targets = {o.key};
        const double k = rng.chance(0.5) ? rng.uniform(1.1, 1.5) : rng.uniform(0.6, 0.9);
        f.edit.amount = std::max(0.3, o.c.r * k);
        return f;
    }
    if (roll < 95) {
        // A flat face that is its owner's only face: pushed out or pulled in, sometimes exactly to the level of another
        // face of the same outward normal (they unify).
        f.role = "offset_face";
        f.edit.kind = EKind::Offset;
        for (const Owner& o : owners.v)
            if (o.c.k == Carrier::K::Plane && o.c.oriented && ownFaces(o.key, owners, tb, smp).size() == 1)
                keys.push_back(o.key);
        if (keys.empty())
            return std::nullopt;
        const Owner& o = *owners.get(keys[rng.below(keys.size())]);
        f.edit.targets = {o.key};
        std::vector<double> levels;
        for (const Owner& p : owners.v)
            if (&p != &o && p.c.k == Carrier::K::Plane && p.c.oriented && dot(p.c.a, o.c.a) > 1 - 1e-9 &&
                !facesOn(p.key, owners, smp).empty()) {
                const double d = dot(sub(p.c.p, o.c.p), o.c.a);
                if (std::abs(d) > 0.05 && std::abs(d) < 0.3 * partSize)
                    levels.push_back(d);
            }
        if (!levels.empty() && rng.chance(0.3))
            f.edit.amount = levels[rng.below(levels.size())];
        else
            f.edit.amount = (rng.chance(0.5) ? 1.0 : -1.0) * partSize * rng.uniform(0.005, 0.06);
        return f;
    }
    f.role = "unify";
    f.edit.kind = EKind::Unify;
    return f;
}

// ---------------------------------------------------------------- scripts as text (T-081b round 4)
// A script written out exactly (doubles as hexadecimal floating point, which every platform's strtod reads back to the
// same value), so that a sequence one platform's generator authored is replayed as it is on every platform: the
// platform-independent pins (item 8 of round 4). One token stream, whitespace-separated: per feature
//   F <id> <type> <op> <role> <suppressed> o x y z xd x y z n x y z <dist> <size> <loops> {<loop id> <ents> {<k> x0 y0
//   x1 y1 cx cy r tag}} <targets> {<a> <b> <convex> <picked>} <edit kind> <amount> <edit targets> {<target>}
// The import's base is not written: it is the part's.
std::string scriptToText(const Script& s)
{
    std::string out;
    char buf[64];
    auto num = [&](double v) {
        std::snprintf(buf, sizeof buf, " %a", v);
        out += buf;
    };
    for (const Feature& f : s) {
        out += "F " + f.id + " " + std::to_string(static_cast<int>(f.type)) + " " +
               std::to_string(static_cast<int>(f.op)) + " " + (f.role.empty() ? "-" : f.role) + " " +
               (f.suppressed ? "1" : "0");
        for (const Vec3& v : {f.frame.o, f.frame.xd, f.frame.n}) {
            num(v.x);
            num(v.y);
            num(v.z);
        }
        num(f.dist);
        num(f.size);
        out += " " + std::to_string(f.loops.size());
        for (const Loop& l : f.loops) {
            out += " " + std::to_string(l.id) + " " + std::to_string(l.ents.size());
            for (const Ent& e : l.ents) {
                out += std::string(" ") + e.k;
                for (const double v : {e.x0, e.y0, e.x1, e.y1, e.cx, e.cy, e.r})
                    num(v);
                out += " " + std::to_string(e.tag);
            }
        }
        out += " " + std::to_string(f.targets.size());
        for (const Target& t : f.targets)
            out += " " + t.a + " " + t.b + (t.convex ? " 1" : " 0") + (t.picked ? " 1" : " 0");
        out += " " + std::to_string(static_cast<int>(f.edit.kind));
        num(f.edit.amount);
        out += " " + std::to_string(f.edit.targets.size());
        for (const std::string& t : f.edit.targets)
            out += " " + t;
        out += "\n";
    }
    return out;
}

Script scriptFromText(const std::string& text, const std::shared_ptr<const ImportBase>& base)
{
    std::vector<std::string> tok;
    {
        std::string cur;
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                if (!cur.empty())
                    tok.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty())
            tok.push_back(cur);
    }
    std::size_t i = 0;
    auto next = [&]() -> const std::string& {
        REQUIRE(i < tok.size());
        return tok[i++];
    };
    auto real = [&]() { return std::strtod(next().c_str(), nullptr); };
    auto whole = [&]() { return std::strtoull(next().c_str(), nullptr, 10); };
    Script s;
    while (i < tok.size()) {
        REQUIRE(next() == "F");
        Feature f;
        f.id = next();
        f.type = static_cast<FType>(whole());
        f.op = static_cast<BOp>(whole());
        f.role = next();
        if (f.role == "-")
            f.role.clear();
        f.suppressed = whole() != 0;
        for (Vec3* v : {&f.frame.o, &f.frame.xd, &f.frame.n}) {
            v->x = real();
            v->y = real();
            v->z = real();
        }
        f.dist = real();
        f.size = real();
        const std::size_t loops = whole();
        for (std::size_t k = 0; k < loops; ++k) {
            Loop l;
            l.id = whole();
            const std::size_t ents = whole();
            for (std::size_t m = 0; m < ents; ++m) {
                Ent e;
                e.k = next()[0];
                for (double* v : {&e.x0, &e.y0, &e.x1, &e.y1, &e.cx, &e.cy, &e.r})
                    *v = real();
                e.tag = whole();
                l.ents.push_back(e);
            }
            f.loops.push_back(std::move(l));
        }
        const std::size_t targets = whole();
        for (std::size_t k = 0; k < targets; ++k) {
            Target t;
            t.a = next();
            t.b = next();
            t.convex = whole() != 0;
            t.picked = whole() != 0;
            f.targets.push_back(t);
        }
        f.edit.kind = static_cast<EKind>(whole());
        f.edit.amount = real();
        const std::size_t editTargets = whole();
        for (std::size_t k = 0; k < editTargets; ++k)
            f.edit.targets.push_back(next());
        if (f.type == FType::Import)
            f.base = base;
        s.push_back(std::move(f));
    }
    return s;
}

// ---------------------------------------------------------------- one sequence
struct SequenceResult {
    Tally tally;
    JudgeLog log;
    std::string edit;
    int ops = 0;
    int refused = 0;
    long refs = 0;
    std::map<std::string, int> excluded;
    std::map<std::string, int> editCounts; // authoring and replay, "<role>: <outcome>"
    int timeouts = 0;
    int replayFailures = 0;
    bool skipped = false;
    std::string skipReason;
    // For evidence (the [.evidence] test): the shapes the references were made on and resolved on, and the scripts.
    kernel::Shape baselineShape, replayShape;
    std::string scriptText, editedText, replayText;
};

struct SequenceOptions {
    std::uint64_t seedBase = 0xD1EC7000ull;
    std::string keyPrefix = "directedit ";
    // Choose among the elements of a built shape in the kernel's order (true: the blocks below, on which the oracle was
    // tuned), or sorted by geometry (the hidden block [.random-directedit-sorted], see there).
    bool byIndex = true;
    // Scripted (T-081b round 4): no generator; the baseline is `script`, the edited script `edited` (scriptFromText).
    const Script* script = nullptr;
    const Script* edited = nullptr;
    // The portable generator (tPortable, item 9 of round 4); implies choices by geometry.
    bool portable = false;
    // Only author the sequence (both scripts, SequenceResult::scriptText and editedText as scriptToText), judge
    // nothing:
    // for comparing what two platforms author ([.generator-portability]).
    bool authorOnly = false;
};

// One random edit of `edited` (the import, at 0, is never edited), as in T-081a, or a direct edit appended. Returns it
// in words.
std::string randomEdit(Script& edited, const Part& part, const Builder& baseline, const Samples& now, Rng& rng,
                       int& inserted)
{
    std::vector<std::size_t> sweeps, blends, edits;
    for (std::size_t i = 1; i < edited.size(); ++i)
        (edited[i].type == FType::Sweep ? sweeps : edited[i].type == FType::Edit ? edits : blends).push_back(i);
    const std::size_t roll = rng.below(100);
    if (roll < 45) {
        auto f = proposeEdit(baseline.builtScript(), baseline.shape(), now, part.size, rng,
                             "D" + std::to_string(++inserted));
        if (!f)
            return "no-op";
        edited.push_back(*f);
        return "apply " + f->role + " " + f->id;
    }
    if (roll < 52 && !edits.empty()) {
        Feature& f = edited[edits[rng.below(edits.size())]];
        if (f.edit.kind == EKind::Offset || f.edit.kind == EKind::Resize) {
            f.edit.amount *= rng.uniform(0.5, 1.5);
            return "change amount of " + f.id;
        }
        return "no-op";
    }
    if (roll < 67 && !sweeps.empty()) {
        Feature& f = edited[sweeps[rng.below(sweeps.size())]];
        const std::size_t kind = rng.below(4);
        if (kind == 0) {
            f.frame.o = add(f.frame.o, mul(f.frame.xd, profileWidth(f.loops[0])));
            return "move " + f.id + " by its width";
        }
        if (kind == 1) {
            const double a = rng.uniform(0, 2 * kPi), d = profileWidth(f.loops[0]) * rng.uniform(0.1, 1.2);
            f.frame.o = add(f.frame.o, add(mul(f.frame.xd, d * std::cos(a)), mul(f.frame.yd(), d * std::sin(a))));
            return "move " + f.id;
        }
        if (kind == 2) {
            const double k = rng.uniform(0.6, 1.5);
            for (Ent& e : f.loops[0].ents) {
                e.x0 *= k;
                e.y0 *= k;
                e.x1 *= k;
                e.y1 *= k;
                e.cx *= k;
                e.cy *= k;
                e.r *= k;
            }
            return "resize " + f.id;
        }
        f.dist *= rng.uniform(0.4, 2.0);
        return "change depth of " + f.id;
    }
    if (roll < 74 && !blends.empty()) {
        Feature& f = edited[blends[rng.below(blends.size())]];
        f.size *= rng.uniform(0.5, 1.8);
        return "change size of " + f.id;
    }
    if (roll < 83 && edited.size() > 1) {
        const std::size_t i = 1 + rng.below(edited.size() - 1);
        edited[i].suppressed = true;
        return "suppress " + edited[i].id;
    }
    if (roll < 92) {
        auto f = proposeSweep(*part.samples, part.size, rng, "X" + std::to_string(++inserted), true);
        if (!f)
            return "no-op";
        const std::size_t at = 1 + rng.below(edited.size());
        edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(at), *f);
        return "insert cut " + f->id + " at " + std::to_string(at);
    }
    if (edited.size() > 2) {
        const std::size_t i = 1 + rng.below(edited.size() - 2);
        std::swap(edited[i], edited[i + 1]);
        return "swap " + edited[i + 1].id + " and " + edited[i].id;
    }
    return "no-op";
}

void addCounts(std::map<std::string, int>& into, const Builder& b)
{
    for (const auto& [k, n] : b.editCounts())
        into[k] += n;
}

SequenceResult runSequence(const Part& part, int partIndex, int seq, const SequenceOptions& opt)
{
    SequenceResult out;
    tByIndex = opt.byIndex && !opt.portable;
    tPortable = opt.portable;
    Rng rng{opt.seedBase + static_cast<std::uint64_t>(partIndex) * 1000003ull + static_cast<std::uint64_t>(seq)};
    for (int i = 0; i < 4; ++i)
        rng.next();
    const std::string label = opt.keyPrefix + part.path + " #" + std::to_string(seq);

    Feature import;
    import.id = "I";
    import.type = FType::Import;
    import.role = "import";
    import.base = part.base;
    Script script{import};
    FeatRefs picks;
    Builder b(Mode::Author, picks);
    b.apply(import);
    if (opt.script != nullptr)
        for (std::size_t k = 1; k < opt.script->size(); ++k) {
            const int before = b.built();
            b.apply((*opt.script)[k]);
            if (b.built() > before) {
                script.push_back((*opt.script)[k]);
                ++out.ops;
            } else {
                ++out.refused;
            }
        }
    const int want = opt.script != nullptr ? 0 : 3 + static_cast<int>(rng.below(3));
    for (int attempt = 0; out.ops < want && attempt < 20; ++attempt) {
        auto smp = sampleShape(b.shape());
        if (!smp)
            break;
        const std::string id = "R" + std::to_string(out.ops + 1);
        // RAYCAD_NAMING_STEPS=2: also every face's description before each step.
        if (const char* steps = std::getenv("RAYCAD_NAMING_STEPS"); steps != nullptr && steps[0] == '2') {
            const Owners ow = buildOwners(b.builtScript());
            const TruthBuilder tb(ow, **smp);
            for (std::uint32_t i = 0; i < (*smp)->of(SubKind::Face).size(); ++i) {
                const TruthDesc& d = tb.faceDescriptions()[i];
                std::string alts;
                if (d.ok)
                    for (const std::string& a : d.cons[0].alts)
                        alts += " " + a;
                std::printf("    face %u %s%s\n", i, d.ok ? "ok" : d.why.c_str(), alts.c_str());
            }
        }
        const std::size_t roll = rng.below(100);
        std::optional<Feature> f;
        if (roll < 27)
            f = proposeSweep(**smp, part.size, rng, id, true);
        else if (roll < 40)
            f = proposeSweep(**smp, part.size, rng, id, false);
        else if (roll < 66)
            f = proposeBlend(b.builtScript(), **smp, rng, id, roll < 53);
        else
            f = proposeEdit(b.builtScript(), b.shape(), **smp, part.size, rng, id);
        if (!f)
            continue;
        const int before = b.built();
        const std::size_t failedBefore = b.failed().size();
        b.apply(*f);
        // RAYCAD_NAMING_STEPS: every step proposed while authoring and what became of it (to compare two platforms).
        if (std::getenv("RAYCAD_NAMING_STEPS") != nullptr) {
            std::string what = f->id + " " + f->role;
            for (const std::string& t : f->edit.targets)
                what += " " + t;
            for (const Target& t : f->targets)
                what += " " + t.a + "/" + t.b;
            char buf[200];
            std::snprintf(buf, sizeof buf, " amount %.6f size %.6f dist %.6f faces %u", f->edit.amount, f->size,
                          f->dist, b.shape().count(SubKind::Face));
            what += buf;
            what += b.built() > before ? " built"
                                       : " refused: " + (b.failed().size() > failedBefore ? b.failed().back() : "");
            std::printf("  step %s\n", what.c_str());
        }
        if (b.built() > before) {
            script.push_back(*f);
            ++out.ops;
        } else {
            ++out.refused;
        }
    }
    addCounts(out.editCounts, b);
    out.timeouts += b.editTimeouts();
    if (out.ops == 0) {
        out.skipped = true;
        out.skipReason = "no feature could be added";
        return out;
    }
    auto smp0 = sampleShape(b.shape());
    if (!smp0) {
        out.skipped = true;
        out.skipReason = "sampling the baseline failed";
        return out;
    }
    const auto owners0 = std::make_shared<const Owners>(buildOwners(b.builtScript()));
    const std::vector<Ref> refs = recordReferences(b, owners0, *smp0, &out.excluded);
    out.refs = static_cast<long>(refs.size());

    Script edited = script;
    if (opt.script != nullptr) {
        edited = *opt.edited;
        for (Feature& f : edited)
            if (f.type == FType::Import)
                f.base = part.base;
        out.edit = "scripted";
    } else {
        const std::size_t edits = rng.chance(0.35) ? 2 + rng.below(2) : 1;
        int inserted = 0;
        for (std::size_t k = 0; k < edits; ++k)
            out.edit += (k == 0 ? "" : "; ") + randomEdit(edited, part, b, **smp0, rng, inserted);
    }
    if (opt.authorOnly) {
        out.scriptText = scriptToText(script);
        out.editedText = scriptToText(edited);
        return out;
    }
    // RAYCAD_DUMP_SCRIPTS: both scripts exactly (scriptToText), to pin the sequence on every platform.
    if (std::getenv("RAYCAD_DUMP_SCRIPTS") != nullptr)
        std::printf("SCRIPT %s\nBASELINE\n%sEDITED\n%sEND\n", label.c_str(), scriptToText(script).c_str(),
                    scriptToText(edited).c_str());

    Builder replay(Mode::Replay, picks);
    for (const Feature& f : edited)
        replay.apply(f);
    addCounts(out.editCounts, replay);
    out.timeouts += replay.editTimeouts();
    out.replayFailures = static_cast<int>(replay.failed().size());
    auto smp1 = sampleShape(replay.shape());
    if (!smp1) {
        out.skipped = true;
        out.skipReason = "sampling the replay failed";
        return out;
    }
    const Owners owners1 = buildOwners(replay.builtScript());
    out.log.maxWrongs = std::getenv("RAYCAD_NAMING_ONLY") != nullptr ? 100000 : 20;
    judgeAll(refs, replay, owners1, **smp1, out.tally, out.log, label);
    out.baselineShape = b.shape();
    out.replayShape = replay.shape();
    const bool evidence = std::getenv("RAYCAD_EVIDENCE_DIR") != nullptr;
    if (evidence ||
        ((!out.log.wrongs.empty() || !out.log.notes.empty()) && std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr)) {
        auto describe = [](const Script& s) {
            std::string text;
            char buf[400];
            for (const Feature& f : s) {
                if (f.type == FType::Sweep)
                    std::snprintf(buf, sizeof buf,
                                  "    %s%s %s o=(%.3f %.3f %.3f) n=(%.3f %.3f %.3f) %zu curves width %.3f dist %.3f\n",
                                  f.id.c_str(), f.suppressed ? " (suppressed)" : "", f.op == BOp::Cut ? "cut" : "join",
                                  f.frame.o.x, f.frame.o.y, f.frame.o.z, f.frame.n.x, f.frame.n.y, f.frame.n.z,
                                  f.loops[0].ents.size(), profileWidth(f.loops[0]), f.dist);
                else if (f.type == FType::Import)
                    std::snprintf(buf, sizeof buf, "    %s import\n", f.id.c_str());
                else if (f.type == FType::Edit) {
                    std::string t;
                    for (const std::string& k : f.edit.targets)
                        t += " " + k;
                    std::snprintf(buf, sizeof buf, "    %s%s %s%s amount %.3f\n", f.id.c_str(),
                                  f.suppressed ? " (suppressed)" : "", f.role.c_str(), t.c_str(), f.edit.amount);
                } else
                    std::snprintf(buf, sizeof buf, "    %s%s %s %s/%s size %.3f\n", f.id.c_str(),
                                  f.suppressed ? " (suppressed)" : "", f.role.c_str(), f.targets[0].a.c_str(),
                                  f.targets[0].b.c_str(), f.size);
                text += buf;
            }
            return text;
        };
        out.scriptText = describe(script);
        out.editedText = describe(edited);
        for (const std::string& fail : replay.failed())
            out.replayText += "  replay failure: " + fail + "\n";
        for (const std::string& id : replay.builtIds())
            out.replayText += " " + id;
        out.log.wrongs.insert(out.log.wrongs.begin(),
                              "  script:\n" + describe(script) + "  edited:\n" + describe(edited));
        for (const std::string& fail : replay.failed())
            out.log.wrongs.push_back("  replay failure: " + fail);
        std::string built;
        for (const std::string& id : replay.builtIds())
            built += " " + id;
        out.log.wrongs.push_back("  replay built:" + built);
        if (std::getenv("RAYCAD_NAMING_ONLY") != nullptr)
            out.log.wrongs.push_back("  baseline faces:\n" + dumpElements(b, **smp0, SubKind::Face) +
                                     "  replay faces:\n" + dumpElements(replay, **smp1, SubKind::Face));
    }
    return out;
}

unsigned workerCount()
{
    if (const char* n = std::getenv("RAYCAD_NAMING_WORKERS"))
        return static_cast<unsigned>(std::clamp(std::atoi(n), 1, 64));
    return std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
}

std::vector<Part> loadParts()
{
    std::vector<Part> parts(kPartCount);
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workerCount(); ++t)
        threads.emplace_back([&] {
            for (int i = next++; i < kPartCount; i = next++)
                parts[static_cast<std::size_t>(i)] = loadPart(kParts[i]);
        });
    for (std::thread& t : threads)
        t.join();
    return parts;
}

struct BlockResult {
    Tally total;
    long judged = 0, replaced = 0, ops = 0, refused = 0, replayFailures = 0, refs = 0, timeouts = 0;
    std::vector<int> shortParts;
    std::map<std::string, long> skipped;
    std::map<std::string, long> excluded;
    std::map<std::string, long> edits;
    std::map<std::string, long> editCounts;
};

// The kind of a recorded edit in words: its text without feature ids and numbers.
std::string editKind(const std::string& text)
{
    std::string kind, word;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ' ') {
            const bool id = word.size() >= 2 && (word[0] == 'R' || word[0] == 'X' || word[0] == 'D') &&
                            std::isdigit(static_cast<unsigned char>(word[1])) != 0;
            const bool number = !word.empty() && std::isdigit(static_cast<unsigned char>(word[0])) != 0;
            if (!word.empty() && !id && !number)
                kind += (kind.empty() ? "" : " ") + word;
            word.clear();
        } else {
            word += text[i];
        }
    }
    return kind;
}

BlockResult runBlock(const char* title, const std::vector<Part>& parts, int perPart, const SequenceOptions& opt)
{
    int onlyPart = -1, onlySeq = -1;
    if (const char* only = std::getenv("RAYCAD_NAMING_ONLY"))
        if (std::sscanf(only, "%d:%d", &onlyPart, &onlySeq) != 2)
            onlyPart = onlySeq = -1;
    struct Job {
        int part, seq;
    };
    std::map<std::pair<int, int>, SequenceResult> results;
    std::vector<int> nextSeq(kPartCount, 0), judged(kPartCount, 0);
    const bool progress = std::getenv("RAYCAD_NAMING_PROGRESS") != nullptr;
    for (bool first = true;; first = false) {
        std::vector<Job> jobs;
        if (onlyPart >= 0) {
            if (!first)
                break;
            for (int s = 0; s < (onlySeq < 0 ? perPart : 1); ++s)
                jobs.push_back({onlyPart, onlySeq < 0 ? s : onlySeq});
        } else {
            for (int p = 0; p < kPartCount; ++p)
                for (int k = judged[static_cast<std::size_t>(p)];
                     k < perPart && nextSeq[static_cast<std::size_t>(p)] < 4 * perPart; ++k)
                    jobs.push_back({p, nextSeq[static_cast<std::size_t>(p)]++});
        }
        if (jobs.empty())
            break;
        std::vector<SequenceResult> out(jobs.size());
        std::atomic<std::size_t> next{0};
        std::mutex lock;
        std::size_t done = 0;
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < workerCount(); ++t)
            threads.emplace_back([&] {
                for (std::size_t i = next++; i < jobs.size(); i = next++) {
                    if (progress)
                        std::fprintf(stderr, "sequence %d:%d\n", jobs[i].part, jobs[i].seq);
                    out[i] = runSequence(parts[static_cast<std::size_t>(jobs[i].part)], jobs[i].part, jobs[i].seq, opt);
                    if (progress)
                        std::fprintf(stderr, "finished %d:%d\n", jobs[i].part, jobs[i].seq);
                    const std::lock_guard<std::mutex> guard(lock);
                    if (++done % 100 == 0)
                        std::printf("  %zu of %zu sequences\n", done, jobs.size());
                }
            });
        for (std::thread& t : threads)
            t.join();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (!out[i].skipped)
                ++judged[static_cast<std::size_t>(jobs[i].part)];
            results[{jobs[i].part, jobs[i].seq}] = std::move(out[i]);
        }
    }

    BlockResult r;
    std::printf(
        "\n%s (oracle: %s)\n| part | judged | replaced | steps | refs judged | Exact %% | Split %% | Merged %% | "
        "Repaired %% | Broken %% | gone / missed | exists: E+S+M %% | WRONG | not judged |\n"
        "|-|-|-|-|-|-|-|-|-|-|-|-|-|-|\n",
        title, strictnessName().c_str());
    for (int p = 0; p < kPartCount; ++p) {
        Tally t;
        long n = 0, o = 0, replaced = 0;
        for (const auto& [job, res] : results) {
            if (job.first != p)
                continue;
            // Every direct edit tried counts, judged sequence or not.
            for (const auto& [k, c] : res.editCounts)
                r.editCounts[k] += c;
            r.timeouts += res.timeouts;
            if (res.skipped) {
                ++replaced;
                ++r.skipped[res.skipReason];
                continue;
            }
            ++n;
            o += res.ops;
            r.refused += res.refused;
            r.replayFailures += res.replayFailures;
            r.refs += res.refs;
            for (const auto& [why, k] : res.excluded)
                r.excluded[why] += k;
            std::size_t count = 0;
            for (std::size_t from = 0; from <= res.edit.size(); ++count) {
                std::size_t to = res.edit.find("; ", from);
                if (to == std::string::npos)
                    to = res.edit.size();
                ++r.edits["edit: " + editKind(res.edit.substr(from, to - from))];
                from = to + 2;
            }
            ++r.edits["sequences with " + std::to_string(count) + " edit(s)"];
            t.merge(res.tally);
        }
        if (n == 0 && replaced == 0)
            continue;
        if (onlyPart < 0 && n < perPart)
            r.shortParts.push_back(p);
        r.judged += n;
        r.replaced += replaced;
        r.ops += o;
        r.total.merge(t);
        std::printf("| %s | %ld | %ld | %ld | %ld | %s | %s | %s | %s | %s | %ld / %ld | %s | %ld | %ld |\n", kParts[p],
                    n, replaced, o, t.n, percent(t.state[0], t.n).c_str(), percent(t.state[1], t.n).c_str(),
                    percent(t.state[2], t.n).c_str(), percent(t.state[3], t.n).c_str(),
                    percent(t.state[4], t.n).c_str(), t.verdict[static_cast<std::size_t>(Verdict::BrokenGone)],
                    t.verdict[static_cast<std::size_t>(Verdict::Missed)], percent(t.existingOk, t.existing).c_str(),
                    t.wrong(), t.unjudged());
    }
    for (const auto& [job, res] : results)
        for (const std::string& w : res.log.wrongs)
            std::printf("  [%d:%d %s] %s\n", job.first, job.second, res.edit.c_str(), w.c_str());
    for (const auto& [job, res] : results)
        for (const std::string& n : res.log.notes)
            std::printf("  [%d:%d %s] %s\n", job.first, job.second, res.edit.c_str(), n.c_str());
    if (const char* trace = std::getenv("RAYCAD_NAMING_TRACE"))
        if (std::FILE* f = std::fopen(trace, "w")) {
            for (const auto& [job, res] : results) {
                std::fprintf(f, "%d:%d ops %d refs %ld edit [%s] n %ld states", job.first, job.second, res.ops,
                             res.refs, res.edit.c_str(), res.tally.n);
                for (const long s : res.tally.state)
                    std::fprintf(f, " %ld", s);
                std::fprintf(f, " verdicts");
                for (const long v : res.tally.verdict)
                    std::fprintf(f, " %ld", v);
                std::fprintf(f, " replayfail %d skipped %d edits", res.replayFailures, res.skipped ? 1 : 0);
                for (const auto& [k, c] : res.editCounts)
                    std::fprintf(f, " [%s %d]", k.c_str(), c);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    if (const char* trace = std::getenv("RAYCAD_NAMING_VERDICTS"))
        if (std::FILE* f = std::fopen(trace, "w")) {
            for (const auto& [job, res] : results)
                for (const std::string& line : res.log.trace)
                    std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
        }

    std::printf("\n%s: %ld sequences judged, %ld replaced (", title, r.judged, r.replaced);
    for (const auto& [why, n] : r.skipped)
        std::printf(" %s: %ld;", why.c_str(), n);
    std::printf(" ), parts short of %d judged: %zu\n", perPart, r.shortParts.size());
    std::printf("  %ld steps (%ld proposals refused while authoring, dropped), %ld feature failures on replay, %ld "
                "references recorded\n",
                r.ops, r.refused, r.replayFailures, r.refs);
    std::printf("  direct edits by kind and outcome (authoring and replay, every sequence run; %ld past the time "
                "limit):\n",
                r.timeouts);
    for (const char* role : kEditRoles) {
        long attempted = 0, applied = 0;
        std::string detail;
        for (const auto& [k, c] : r.editCounts)
            if (k.compare(0, std::strlen(role) + 2, std::string(role) + ": ") == 0) {
                attempted += c;
                if (k.substr(std::strlen(role) + 2) == "applied")
                    applied += c;
                detail += " " + k.substr(std::strlen(role) + 2) + " " + std::to_string(c) + ";";
            }
        std::printf("    %s: %ld attempted, %ld applied (%s )\n", role, attempted, applied, detail.c_str());
    }
    std::printf("  baseline elements that are not references:\n");
    for (const auto& [why, n] : r.excluded)
        std::printf("    %s: %ld\n", why.c_str(), n);
    std::printf("  edits:\n");
    for (const auto& [what, n] : r.edits)
        std::printf("    %s: %ld\n", what.c_str(), n);
    printTally(title, r.total);
    return r;
}

// A holdout block (`holdoutPrefix`, its keys' prefix): WRONG must be 0; every answer it leaves not judged, and every
// acceptance by the new-ground rule, must be on a reviewed list (round 3: no budget of unreviewed ones).
void checkGates(const BlockResult& r, int perPart, const std::string& holdoutPrefix = {})
{
    if (std::getenv("RAYCAD_NAMING_ONLY") == nullptr) {
        CHECK(r.judged == static_cast<long>(perPart) * kPartCount);
        CHECK(r.shortParts.empty());
    }
    const Tally& t = r.total;
    CHECK(t.mutantsCaught() == t.mutantsMade());
    CHECK(t.repairedUsable == 0);
    CHECK(t.selfCheckFailures == 0);
    CHECK(t.injectivity == 0);
    if (holdoutPrefix.empty()) {
        CHECK(t.unjudged() == t.unjudgedReviewed);
    } else {
        long listed = 0;
        for (const auto& [key, why] : reviewedUnjudged())
            listed += key.compare(0, holdoutPrefix.size(), holdoutPrefix) == 0 ? 1 : 0;
        std::printf(
            "%sblock: %ld answers not judged, %ld of them on the reviewed list, which lists %ld for this block\n",
            holdoutPrefix.c_str(), t.unjudged(), t.unjudgedReviewed, listed);
        CHECK(t.unjudged() == t.unjudgedReviewed);
    }
    CHECK(t.accepted == t.acceptedReviewed);
    CHECK(t.acceptedNewGround == t.acceptedNewGroundReviewed);
    CHECK(t.conflicts == t.conflictsReviewed);
    REQUIRE(t.wrong() == 0);
}

void checkNoFault(std::uint64_t before)
{
    const std::uint64_t faults = kernel::testing::structuredExceptions() - before;
    INFO("access violations or other faults raised inside the geometry kernel and swallowed: " << faults);
    CHECK(faults == 0);
}

} // namespace

TEST_CASE("1,000 randomised sequences with direct edits on 20 corpus parts: no reference resolves to a wrong element",
          "[random-directedit]")
{
    const std::uint64_t guardBefore = kernel::testing::structuredExceptions();
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    const BlockResult r = runBlock("Randomised with direct edits", parts, kSequencesPerPart, SequenceOptions{});
    checkNoFault(guardBefore);
    checkGates(r, kSequencesPerPart);
}

// The quick tier's share of the block above: its first kSmokePerPart judged sequences of every part, on the same
// seeds, with the same gates.
TEST_CASE("Smoke: the first 2 randomised sequences with direct edits of every part", "[random-directedit-smoke]")
{
    static_assert(kSmokePerPart < kSequencesPerPart);
    const std::uint64_t guardBefore = kernel::testing::structuredExceptions();
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    const BlockResult r = runBlock("Randomised with direct edits, smoke", parts, kSmokePerPart, SequenceOptions{});
    checkNoFault(guardBefore);
    checkGates(r, kSmokePerPart);
}

// The same seeds with the generator choosing by geometry instead of by index (SequenceOptions::byIndex false): other
// sequences, closer to being the same on Windows and Linux (on the T-081b run 50 of 1,000 differed between the two,
// against 94 by index). First run as an unseen check while the block above was being fixed (T-081b round 1: 9 wrong
// answers, each resolved in round 2); a regression block now, gated as a holdout.
TEST_CASE("Holdout: 1,000 randomised sequences with direct edits, choices by geometry", "[random-directedit-sorted]")
{
    const std::uint64_t guardBefore = kernel::testing::structuredExceptions();
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    SequenceOptions sorted;
    sorted.byIndex = false;
    sorted.keyPrefix = "directedit-sorted ";
    const BlockResult r =
        runBlock("Randomised with direct edits, choices by geometry", parts, kSequencesPerPart, sorted);
    checkNoFault(guardBefore);
    checkGates(r, kSequencesPerPart, sorted.keyPrefix);
}
// Holdouts with direct edits, choices by geometry: each block's seeds were first run once, unseen, on Windows and on
// Linux after the fixes the block before it showed (T-081b round 2), and are kept as regression blocks.
namespace {
void runHoldout(const char* title, std::uint64_t seedBase, const std::string& prefix, bool portable = false)
{
    const std::uint64_t guardBefore = kernel::testing::structuredExceptions();
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    SequenceOptions opt;
    opt.byIndex = false;
    opt.seedBase = seedBase;
    opt.keyPrefix = prefix;
    opt.portable = portable;
    const BlockResult r = runBlock(title, parts, kSequencesPerPart, opt);
    checkNoFault(guardBefore);
    checkGates(r, kSequencesPerPart, prefix);
}
} // namespace

TEST_CASE("Direct-edit holdout 1: 1,000 sequences, choices by geometry, seeds first run in T-081b round 2",
          "[random-directedit-holdout1]")
{
    runHoldout("Direct-edit holdout 1", 0xA81B7000ull, "directedit-holdout1 ");
}

TEST_CASE("Direct-edit holdout 2: 1,000 sequences, choices by geometry, seeds first run in T-081b round 2",
          "[random-directedit-holdout2]")
{
    runHoldout("Direct-edit holdout 2", 0xB2081B00ull, "directedit-holdout2 ");
}

TEST_CASE("Direct-edit block of the independent review: 1,000 sequences, choices by geometry, the reviewer's seed",
          "[random-directedit-review1]")
{
    // The reviewer's own block, first run on the frozen oracle (oracle-freeze-1) and adjudicated (T-081b round 4, the
    // review's round-2 holdout), kept as a tuned block: gated as the holdouts.
    runHoldout("Direct-edit review block 1", 0x6B1D3F5A2C800000ull, "directedit-review1 ");
}

TEST_CASE("Direct-edit block of any seed: RAYCAD_NAMING_BLOCK_SEED=<hex>, choices by geometry",
          "[.random-directedit-seed]")
{
    // A fresh block for a reviewer, without a driver: RAYCAD_NAMING_BLOCK_SEED=<hex seed base> (e.g. 6B1D3F5A2C800000),
    // keys "directedit-seed<hex> ". Gated as a holdout block (its reviewed-list entries use that prefix). With
    // RAYCAD_NAMING_PORTABLE=1, authored by the portable generator (SequenceOptions::portable).
    const char* seed = std::getenv("RAYCAD_NAMING_BLOCK_SEED");
    if (seed == nullptr)
        SKIP("RAYCAD_NAMING_BLOCK_SEED is not set");
    const std::uint64_t base = std::strtoull(seed, nullptr, 16);
    REQUIRE(base != 0);
    runHoldout("Direct-edit block of a given seed", base, "directedit-seed" + std::string(seed) + " ",
               std::getenv("RAYCAD_NAMING_PORTABLE") != nullptr);
}
TEST_CASE("Generator portability: what the generators by geometry author, per sequence, to compare platforms",
          "[.generator-portability]")
{
    // T-081b round 4, item 9. Authors (judges nothing) the first RAYCAD_PORTABILITY_PER_PART (default 10) sequences of
    // every part on the sorted block's seeds, with the generator by geometry and with the portable one, and prints one
    // line per sequence: "GEN <generator> <part> <sequence> <digest>", the digest of both scripts with every number to
    // 1e-4 (the same sequence on two platforms gives the same line). Compare two platforms' outputs line by line.
    int perPart = 10;
    if (const char* n = std::getenv("RAYCAD_PORTABILITY_PER_PART"))
        perPart = std::clamp(std::atoi(n), 1, kSequencesPerPart);
    const std::vector<Part> parts = loadParts();
    const auto digest = [](const std::string& text) {
        std::string rounded;
        std::string tok;
        const auto flush = [&] {
            if (tok.empty())
                return;
            if (tok.find("0x") != std::string::npos) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.4f", std::strtod(tok.c_str(), nullptr) + 0.0);
                rounded += std::string(buf) == "-0.0000" ? "0.0000" : buf;
            } else {
                rounded += tok;
            }
            rounded += ' ';
            tok.clear();
        };
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0)
                flush();
            else
                tok += c;
        }
        flush();
        std::uint64_t h = 1469598103934665603ull;
        for (const char c : rounded)
            h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
        return std::string(buf);
    };
    struct Job {
        int part, seq;
        bool portable;
    };
    std::vector<Job> jobs;
    for (const bool portable : {false, true})
        for (int p = 0; p < kPartCount; ++p)
            for (int s = 0; s < perPart; ++s)
                jobs.push_back({p, s, portable});
    std::vector<std::string> lines(jobs.size());
    std::atomic<std::size_t> nextJob{0};
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < workerCount(); ++w)
        workers.emplace_back([&] {
            for (std::size_t j = nextJob++; j < jobs.size(); j = nextJob++) {
                SequenceOptions opt;
                opt.byIndex = false;
                opt.portable = jobs[j].portable;
                opt.authorOnly = true;
                const SequenceResult r = runSequence(parts[static_cast<std::size_t>(jobs[j].part)], jobs[j].part,
                                                     jobs[j].seq, opt);
                lines[j] = std::string("GEN ") + (jobs[j].portable ? "portable " : "sorted ") +
                           std::to_string(jobs[j].part) + " " + std::to_string(jobs[j].seq) + " " +
                           (r.skipped ? "skipped" : digest(r.scriptText + "|" + r.editedText));
            }
        });
    for (std::thread& t : workers)
        t.join();
    for (const std::string& l : lines)
        std::printf("%s\n", l.c_str());
}

// ---------------------------------------------------------------- oracle refinements on the sequences that showed them
namespace {

struct Looked {
    std::string id;
    Answer answer = Answer::Broken;
    std::vector<std::uint32_t> elements;
    Judgement judgement;
    Judgement other;                 // the oracle's verdict on another element answered Exact
    std::vector<Judgement> probes;   // the oracle's verdicts on the answers asked for (replayAndLook probes)
    std::vector<std::string> probed; // what each probe asked, and the expectation it was judged against
    double pickMeasure = 0;          // the picked element's area (face) or length (edge) by its samples' weights
};

// In a probe's elements, kAnswer + k stands for the k-th element of the resolver's own answer (sorted): a negative that
// holds on every platform where element numbers differ (T-081b holdout2 0:18: {13, 47} on Windows, {6, 49} on Linux).
constexpr std::uint32_t kAnswer = 0xFFFFFF00u;

struct Probe {
    Answer answer;
    std::vector<std::uint32_t> elements;
    long offCarrier = -1; // replace this element of elements by the nearest element off the picked one's carriers
    // Elements by a point on each, added to elements (scripted pins: element numbers may differ between platforms).
    std::vector<Vec3> at = {};
};

// The element of kind of samples nearest p (within 1e-2 mm), or none.
std::optional<std::uint32_t> elementAt(const Samples& samples, SubKind kind, const Vec3& p)
{
    const auto& N = samples.of(kind);
    std::optional<std::uint32_t> best;
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
            best = e;
        }
    }
    return best;
}

Looked lookAt(const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
              const Owners& now, const Samples& samples, const std::vector<Probe>& probes);

// Replays sequence `seq` of part `partIndex` of the block and looks at the references `ids`: the resolver's answer and
// its verdict, and the verdict on an Exact answer of another element of the same kind. No answer of the sequence may be
// wrong, whether or not it records the references asked for. `edit`, when given, is set to the sequence's edits.
std::vector<Looked> replayAndLook(int partIndex, int seq, const std::vector<std::string>& ids, bool byIndex = true,
                                  std::uint64_t seedBase = 0, const std::vector<Probe>& probes = {},
                                  std::string* edit = nullptr)
{
    const Part part = loadPart(kParts[partIndex]);
    REQUIRE(part.base != nullptr);
    std::vector<Looked> out;
    SequenceOptions legacy;
    legacy.byIndex = byIndex;
    if (seedBase != 0)
        legacy.seedBase = seedBase;
    setInspector([&](const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
                     const Owners& now, const Samples& samples) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            return;
        out.push_back(lookAt(id, ref, res, rr, now, samples, probes));
    });
    const SequenceResult r = runSequence(part, partIndex, seq, legacy);
    setInspector({});
    if (edit != nullptr)
        *edit = r.edit;
    INFO(kParts[partIndex] << " #" << seq << ": " << r.edit);
    CHECK_FALSE(r.skipped);
    CHECK(r.tally.wrong() == 0);
    CHECK(r.tally.mutantsCaught() == r.tally.mutantsMade());
    return out;
}

// The pinned sequence `baseline` -> `edited` (scriptToText, as one platform's generator authored it) replayed as it is,
// on part `partIndex`: the references to the element whose pick-time fingerprint has centroid `at` (within 1e-3 mm) and
// area or length `measure` (within a thousandth), looked at as replayAndLook does. Element numbers are not used, so the
// pin holds on every platform whose kernel builds the scripts alike (T-081b round 4, item 8).
std::vector<Looked> replayScriptedAndLook(int partIndex, const std::string& baseline, const std::string& edited,
                                          SubKind kind, const Vec3& at, double measure,
                                          const std::vector<Probe>& probes = {})
{
    const Part part = loadPart(kParts[partIndex]);
    REQUIRE(part.base != nullptr);
    const Script s0 = scriptFromText(baseline, part.base);
    const Script s1 = scriptFromText(edited, part.base);
    SequenceOptions opt;
    opt.keyPrefix = "scripted ";
    opt.script = &s0;
    opt.edited = &s1;
    std::vector<Looked> out;
    setInspector([&](const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
                     const Owners& now, const Samples& samples) {
        const naming::Fingerprint& h = ref.ref.hint;
        if (ref.ref.kind != kind)
            return;
        const Vec3 c{static_cast<double>(h.qCx) * 1e-4, static_cast<double>(h.qCy) * 1e-4,
                     static_cast<double>(h.qCz) * 1e-4};
        if (norm(sub(c, at)) > 1e-3 || std::abs(static_cast<double>(h.qArea) * 1e-4 - measure) > 1e-3 * measure + 1e-4)
            return;
        out.push_back(lookAt(id, ref, res, rr, now, samples, probes));
    });
    const SequenceResult r = runSequence(part, partIndex, 0, opt);
    setInspector({});
    INFO(kParts[partIndex] << " scripted: " << r.edit);
    CHECK_FALSE(r.skipped);
    CHECK(r.tally.wrong() == 0);
    CHECK(r.tally.mutantsCaught() == r.tally.mutantsMade());
    return out;
}

// What one reference got (replayAndLook): its answer and verdict, the verdict on another element answered Exact, and
// the probes' verdicts.
Looked lookAt(const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
              const Owners& now, const Samples& samples, const std::vector<Probe>& probes)
{
    {
        Looked l;
        l.id = id;
        for (const double w : ref.record.picked().w)
            l.pickMeasure += w;
        l.answer = rr.answer;
        for (const SubId& s : res.elements)
            l.elements.push_back(s.index);
        l.judgement = rr.judgement;
        const Expectation x = expect(ref.record, now, samples);
        const std::uint32_t got = l.elements.empty() ? 0 : l.elements.front();
        l.other = judgeAnswer(x, ref.record, now, samples, Answer::Exact, {got == 0 ? 1u : got - 1});
        for (const Probe& p : probes) {
            std::vector<std::uint32_t> els = p.elements;
            for (std::uint32_t& e : els)
                if (e >= kAnswer && e - kAnswer < l.elements.size())
                    e = l.elements[e - kAnswer];
            for (const Vec3& q : p.at)
                if (const auto e = elementAt(samples, ref.record.base.kind, q))
                    els.push_back(*e);
            // RAYCAD_LOOK_POINTS: a point on each probed element (for a scripted pin's Probe::at), and the pick's
            // fingerprint centroid and measure.
            if (std::getenv("RAYCAD_LOOK_POINTS") != nullptr) {
                const naming::Fingerprint& h = ref.ref.hint;
                std::printf("POINTS %s pick centroid (%.6f %.6f %.6f) measure %.6f\n", id.c_str(), h.qCx * 1e-4,
                            h.qCy * 1e-4, h.qCz * 1e-4, h.qArea * 1e-4);
                for (const std::uint32_t e : els) {
                    if (e >= samples.of(ref.record.base.kind).size())
                        continue;
                    const ElemSamples& es = samples.of(ref.record.base.kind)[e];
                    const Vec3 q =
                        es.pts.empty() ? (es.nodes.empty() ? Vec3{} : es.nodes.front()) : es.pts[es.pts.size() / 2];
                    std::printf("POINTS   element %u at {%a, %a, %a}\n", e, q.x, q.y, q.z);
                }
            }
            if (p.offCarrier >= 0) {
                const auto& N = samples.of(ref.record.base.kind);
                auto centre = [](const ElemSamples& s) {
                    Vec3 m{0, 0, 0};
                    for (const Vec3& q : s.pts)
                        m = Vec3{m.x + q.x / s.pts.size(), m.y + q.y / s.pts.size(), m.z + q.z / s.pts.size()};
                    return m;
                };
                const Vec3 at = centre(N[static_cast<std::size_t>(p.offCarrier)]);
                long best = -1;
                double dBest = 1e300;
                for (std::uint32_t e = 0; e < N.size(); ++e) {
                    if (N[e].pts.empty() || x.inLogical(e))
                        continue;
                    const Vec3 m = centre(N[e]);
                    const double d =
                        (m.x - at.x) * (m.x - at.x) + (m.y - at.y) * (m.y - at.y) + (m.z - at.z) * (m.z - at.z);
                    if (d < dBest) {
                        dBest = d;
                        best = e;
                    }
                }
                for (std::uint32_t& e : els)
                    if (static_cast<long>(e) == p.offCarrier && best >= 0)
                        e = static_cast<std::uint32_t>(best);
            }
            l.probes.push_back(judgeAnswer(x, ref.record, now, samples, p.answer, els));
            std::string s = "asked";
            for (const std::uint32_t e : els)
                s += " " + std::to_string(e);
            s += "; logical";
            for (const std::uint32_t e : x.logical)
                s += " " + std::to_string(e);
            s += "; E";
            for (const std::uint32_t e : x.expected.elems)
                s += " " + std::to_string(e);
            s += "; no anchor: " + x.noAnchor + (x.sharedAnchor ? " (shared anchor)" : "") +
                 (x.mergeUnknown ? " (merge unknown)" : "");
            l.probed.push_back(s);
        }
        return l;
    }
}

} // namespace

namespace {

// A pinned replay whose case this platform's kernel does not build (the macOS kernel, ARM64 and Apple libm, builds
// other geometry, so the generator authors other sequences there). Allowed on macOS only, where the case is reported
// as skipped (SKIP, shown without -s; also on stderr); anywhere else it fails. Every replayed sequence's own answers
// are judged whether or not its case is built (replayAndLook), so a skip never hides a wrong answer.
#ifdef __APPLE__
constexpr bool kPinnedSkipAllowed = true;
#else
constexpr bool kPinnedSkipAllowed = false;
#endif
#if defined(_WIN32)
constexpr const char* kPlatformName = "windows";
#elif defined(__APPLE__)
constexpr const char* kPlatformName = "macos";
#else
constexpr const char* kPlatformName = "linux";
#endif

// offPlatform: pins authored on another platform only (Case::builtOn), not run here: reported as skipped with their
// count, on every platform (T-081b round 4, item 8: never silently; each has a scripted twin that runs everywhere).
void reportPinnedSkips(const std::vector<std::string>& skipped, const std::vector<std::string>& offPlatform = {})
{
    if (skipped.empty() && offPlatform.empty())
        return;
    std::string all;
    for (const std::string& s : skipped) {
        std::fprintf(stderr, "PINNED REPLAY NOT BUILT ON THIS PLATFORM: %s\n", s.c_str());
        all += (all.empty() ? "" : "; ") + s;
    }
    for (const std::string& s : offPlatform)
        std::fprintf(stderr, "PINNED REPLAY AUTHORED ON ANOTHER PLATFORM, NOT RUN HERE: %s\n", s.c_str());
    if (!kPinnedSkipAllowed && !skipped.empty()) {
        for (const std::string& s : skipped)
            FAIL_CHECK("pinned replay not built on this platform (skips are allowed on macOS only): " << s);
        return;
    }
    std::string text;
    if (!skipped.empty())
        text = std::to_string(skipped.size()) + " pinned replay(s) not built on this platform (allowed on macOS): " +
               all;
    if (!offPlatform.empty()) {
        std::string list;
        for (const std::string& s : offPlatform)
            list += (list.empty() ? "" : "; ") + s;
        text += (text.empty() ? "" : "; and ") + std::to_string(offPlatform.size()) +
                " pinned replay(s) authored on another platform only, not run here: " + list;
    }
    SKIP(text);
}

// The replays of T-081b's findings (the cases below), each a sequence of a randomised block; `keyOnly`: only the few
// the quick tier runs (one per kind of fix: kernel history, resolver, pick outline, oracle remnants and past-end rule).
void replayCases(bool keyOnly)
{
    static const std::vector<std::string> key{"fixture_plate_1.stp #43|face 23", "fixture_plate_1.stp #15|face 54",
                                              "rib_plate_1.stp #49|face 39",     "slot_through_1.stp #12|face 8",
                                              "closed_pocket_3.stp #31|edge 37", "keyway_tslot_1.stp #44|face 27"};
    // Each case replays one sequence of the block (its seed is fixed) and checks the resolver's answer and the oracle's
    // verdict on the reference the refinement is about; for a correct answer, also that another element answered
    // Exact is judged wrong (the oracle is not vacuous there).
    struct Case {
        int part, seq;
        std::string id;
        Answer answer;
        Verdict verdict;
        const char* about;
        bool byIndex = true;       // found with the generator's first version (SequenceOptions::byIndex)
        std::uint64_t seed = 0;    // another block's seeds (SequenceOptions::seedBase)
        bool newGround = false;    // accepted by the new-ground rule (oracle.cpp newGroundRule)
        std::vector<Probe> wrongs; // answers the oracle must still judge wrong (the relaxed rule's negatives)
        // Only this platform's kernel authors the case's sequence this way (a holdout found on Windows whose seeds
        // author another sequence on Linux): elsewhere the case is reported as not built, and not failed.
        const char* builtOn = nullptr;
        // The picked element's area or length where the case was found (0: not checked): another platform may author
        // the sequence with other geometry, and record a reference of the same id to another element.
        double pickMeasure = 0;
    };
    const std::string k = "directedit raycad-synthetic/";
    const std::vector<Case> cases{
        // Kernel (directedit.cpp demoteReversedImages): removing a counterbore wall caps the bore on the floor's plane,
        // looking down; that cap was the floor's Modified image, and the floor resolved Exact to it (a ghost).
        {5, 43, k + "fixture_plate_1.stp #43|face 23", Answer::Broken, Verdict::BrokenGone, "reversed cap: kernel"},
        {9, 13, k + "manifold_block_1.stp #13|face 19", Answer::Broken, Verdict::BrokenGone, "reversed cap: kernel"},

        // The first run of holdout 1 (seeds 0xA81B7000, choices by geometry): its six wrong answers, each with the
        // answers the relaxed rule must still call wrong.
        //  - remnants: a push/pull pulled the separating face away, leaving two strips of the pick (oracle);
        {15,
         12,
         k + "slot_through_1.stp #12|face 8",
         Answer::Split,
         Verdict::Correct,
         "remnants of the pick",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Split, {22}}, {Answer::Split, {22, 28, 14}}}},
        //  - remnants: a boss's start cap and its fillet's edge, bounded by a cut that moved away (oracle);
        //    (its fillet's edge, a replayed pick, is judged in the block itself)
        {17,
         10,
         "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #10|face 7",
         Answer::Split,
         Verdict::Correct,
         "remnants of a bounded pick",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Split, {11}}, {Answer::Exact, {17}}}},
        //  - two vertices 3 um apart, the mapped pick on one (oracle);
        {17,
         7,
         "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #7|vertex 39",
         Answer::Exact,
         Verdict::Correct,
         "vertex the mapped pick lies on",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Exact, {30}}}},
        {17,
         7,
         "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #7|vertex 42",
         Answer::Exact,
         Verdict::Correct,
         "vertex the mapped pick lies on",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Exact, {34}}}},
        //  - an arc piece past the seam vertex, inside where the cut that ended the picked arc was: past the pick's
        //    old end, not its continuation (oracle); with another piece's arc, or that piece alone, still wrong;
        {3,
         31,
         k + "closed_pocket_3.stp #31|edge 37",
         Answer::Merged,
         Verdict::Correct,
         "past the pick's old end",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Split, {40, 43}}, {Answer::Split, {40, 42}}, {Answer::Exact, {42}}}},
        //  - a sliver of new ground on the top face where the moved cut used to be, cut off and joined to the face's
        //    split family, lies outside the picked face's outline at pick time (TopoRef::outline): not a piece
        //    (resolver: piecesInsideOutline); the old answer with it, the sliver alone, and the sliver swapped for a
        //    face off the top, all wrong;
        {14,
         49,
         k + "rib_plate_1.stp #49|face 39",
         Answer::Split,
         Verdict::Correct,
         "split piece outside the pick-time outline: resolver",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Split, {2, 9}}, {Answer::Exact, {9}}, {Answer::Split, {2, 9}, 9}}},

        // The first run of holdout 2 (seeds 0xB2081B00, choices by geometry), as the round-3 independent review ruled:
        //  - a bore opened up: its annular step floor widens with it, and the rim is where it was (oracle); the rim at
        //    another radius, and the seam vertex's nearest vertex off its faces, are wrong;
        {9,
         35,
         k + "manifold_block_1.stp #35|edge 54",
         Answer::Exact,
         Verdict::Correct,
         "flat bottom widens",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {56}}, {Answer::Exact, {55}, 55}}},
        {9,
         35,
         k + "manifold_block_1.stp #35|vertex 38",
         Answer::Exact,
         Verdict::Correct,
         "flat bottom widens",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {38}, 38}}},
        //  - two stretches of a chamfer's edge on a resized cut, split at a vertex on their two faces only, the mapped
        //    pick over both: matched sibling to sibling, the lower continues the lower (oracle); the other stretch, or
        //    both, are wrong;
        {17,
         24,
         "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #24|edge 5",
         Answer::Exact,
         Verdict::Correct,
         "sliding stretches",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {3}}, {Answer::Split, {3, 5}}}},
        //  - a boss's start cap cut in two by an inserted cut, the small piece named after a push/pull: the rest of the
        //    pick inside its outline (resolver: otherFacesInsideOutline); either piece alone is wrong;
        {0,
         18,
         k + "block_blind_holes_1.stp #18|face 10",
         Answer::Split,
         Verdict::Correct,
         "rest inside the outline",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {kAnswer}}, {Answer::Exact, {kAnswer + 1}}},
         "windows",
         598.95},
        // (The same face is face 3 on Linux, whose kernel numbers the part's faces otherwise.)
        {0,
         18,
         k + "block_blind_holes_1.stp #18|face 3",
         Answer::Split,
         Verdict::Correct,
         "rest inside the outline",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {kAnswer}}, {Answer::Exact, {kAnswer + 1}}},
         "linux",
         598.95},
        //  - a window of a boss's start cap at the bottom of a hole, the boss moved by its width: the window's ground
        //  went
        //    to the hole's floor, the cap's name to a patch of its plane outside the part (resolver: Broken; the oracle
        //    expects the floor, flagged Merged); the patch (the structural check) and the floor unflagged are wrong;
        {7,
         44,
         k + "keyway_tslot_1.stp #44|face 27",
         Answer::Broken,
         Verdict::Missed,
         "place under another face",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {20}}, {Answer::Exact, {13}}},
         "windows",
         0.313101}, // authored so on Windows only (Linux's 7:44 of holdout 2 is "resize R2")
        //  - the same boss's end cap, now in two pieces, one in a pocket, one outside the part, both holding part of
        //  the
        //    moved pick (resolver: Broken, moved or not cannot be told); either piece alone is wrong.
        {7,
         44,
         k + "keyway_tslot_1.stp #44|face 30",
         Answer::Broken,
         Verdict::Missed,
         "moved or new ground",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Split, {32}}, {Answer::Exact, {22}}},
         "windows",
         0.550746},
        // The structural check (round 3): a boss's side whose visible patch slid along it as a join moved over it
        // (holdout1 7:33 face 18, 27.0 mm^2; the name leads to face 25, another patch of the same cylinder, 34.0
        // mm^2, holding none of the picked ground in place or moved): gone, and Exact on that patch is wrong. And a
        // cut's wall through the part, the cut moved 12 mm down its profile (holdout1 8:23 face 1): a rigid copy of the
        // pick, the same wall, Exact.
        {7,
         33,
         k + "keyway_tslot_1.stp #33|face 18",
         Answer::Broken,
         Verdict::BrokenGone,
         "another patch, structural",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Exact, {25}}},
         "windows",
         27.03},
        {8,
         23,
         k + "l_bracket_1.stp #23|face 1",
         Answer::Exact,
         Verdict::Correct,
         "rigid copy of the pick",
         false,
         0xA81B7000ull,
         false,
         {},
         "windows",
         434.38}, // The tuned block after item 4 (round 3): a top face pushed down, its edge against a join's side now
                  // two edges
        // on
        // one line; the moved stretch continues the pick (kernel history), the one past its old end does not (oracle).
        {12,
         33,
         k + "plate_countersink_1.stp #33|edge 5",
         Answer::Exact,
         Verdict::Correct,
         "past the mapped old end",
         true,
         0,
         false,
         {{Answer::Exact, {19}}, {Answer::Split, {2, 19}}}},
        // Resolver (propagate_resolve.cpp canContain): a sliver of a boss's cylindrical side, split off by a cut that
        // then moved over it; the rest of the side is whole, but holds none of the sliver: Broken, not Merged.
        // (Authored so on Windows only: Linux, Debug and Release alike, puts the cut R3 at (72.985 101.6 21.028) where
        // Windows puts it at (78.356 101.6 28.031), and its face 54 is another face, 421.5 mm^2; CI's Linux job r4
        // failed the
        // pin on it, before it was marked Windows-only. Its scripted twin, below, runs everywhere.)
        {5,
         15,
         k + "fixture_plate_1.stp #15|face 54",
         Answer::Broken,
         Verdict::BrokenGone,
         "piece cut away: resolver",
         false,
         0,
         false,
         {},
         "windows",
         2.010893},

        // Round 4. The structural check on edges (the reviewer's re-check of round 3's list): a cut moved along its own
        // wall's line, the edge its name leads to the wall's other end, holding none of the pick in place or under the
        // cut's move: Broken (resolver: another stretch of a line that stayed), Exact on it WRONG.
        {4,
         33,
         k + "fillet_block_1.stp #33|edge 9",
         Answer::Broken,
         Verdict::Missed,
         "other end of a wall slid along its line",
         false,
         0xA81B7000ull,
         false,
         {{Answer::Exact, {11}}},
         "windows",
         7.780},
        {1,
         3,
         k + "chamfer_block_2.stp #3|edge 42",
         Answer::Broken,
         Verdict::Missed,
         "another stretch of a line that stayed",
         false,
         0xB2081B00ull,
         false,
         {{Answer::Exact, {41}}},
         "windows",
         0.4771},
        // The reviewer's own block (round 4 rulings), authored on Windows:
        //  - a stretch of a moved cut's edge past its old start, ground inside a bore at pick time, is not a piece; the
        //    resolver cannot tell such ground from new ground the pick grew into (sorted 17:32 edge 32): Broken (a miss:
        //    {104} is right); with that stretch, or it alone, WRONG;
        {1,
         39,
         "directedit raycad-synthetic/chamfer_block_2.stp #39|edge 83",
         Answer::Broken,
         Verdict::Missed,
         "piece past the moved pick's old start",
         false,
         0x6B1D3F5A2C800000ull,
         false,
         {{Answer::Split, {104, 106}}, {Answer::Exact, {106}}},
         "windows",
         13.3226},
        //  - the lead's decision: a bore opened past the step, cone and upper bore above it took their ground along its
        //    axis: Merged, Exact a silent merge, and with the upper bore's remnant WRONG;
        {18,
         33,
         "directedit nist-d2mi/827-9999-905.stp #33|face 2",
         Answer::Merged,
         Verdict::Correct,
         "resized bore past its ends",
         false,
         0x6B1D3F5A2C800000ull,
         false,
         {{Answer::Exact, {kAnswer}}, {Answer::Merged, {kAnswer, 24}}},
         "windows",
         202.4883},
        //  - the picked chamfer edge cut in two by an inserted groove, its sibling going on as a third edge: both
        // pieces
        //    (the sibling match refuses to pair by rank); either piece alone WRONG;
        {15,
         10,
         "directedit raycad-synthetic/slot_through_1.stp #10|edge 8",
         Answer::Split,
         Verdict::Correct,
         "the pick came apart, its sibling elsewhere",
         false,
         0x6B1D3F5A2C800000ull,
         false,
         {{Answer::Exact, {71}}, {Answer::Exact, {13}}},
         "windows",
         29.2864},
        //  - a vertex at the same point on the same surfaces, three of the four pieces around it unified: the pieces of
        //    one surface joined (oracle: joined-surface); the nearest vertex off its carriers WRONG.
        {7,
         8,
         "directedit raycad-synthetic/keyway_tslot_1.stp #8|vertex 1",
         Answer::Exact,
         Verdict::Correct,
         "joined pieces of one surface",
         false,
         0x6B1D3F5A2C800000ull,
         false,
         {{Answer::Exact, {0}}},
         "windows"},
        {7,
         8,
         "directedit raycad-synthetic/keyway_tslot_1.stp #8|vertex 7",
         Answer::Exact,
         Verdict::Correct,
         "joined pieces of one surface",
         false,
         0x6B1D3F5A2C800000ull,
         false,
         {{Answer::Exact, {1}}},
         "windows"},

        // The hidden block's findings (choices by geometry), each resolved in the oracle:
        //  - an edge moved only because a direct edit moved the separator that cut it off: still in place;
        {13, 31, k + "plate_thru_holes_1.stp #31|edge 47", Answer::Exact, Verdict::Correct, "separator moved by edit",
         false},
        //  - a new coplanar face beside the pick, the pick moved only by a separator a direct edit moved;
        {17, 27, "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #27|face 17", Answer::Exact, Verdict::Correct,
         "new piece, separator moved by edit", false},
        //  - the faces around a pushed face run on over the slab it swept (an edge there). Since item 4 (the kernel's
        //    pairing of a pushed face's edges) the edge keeps only its own ambiguous name, has grown, and the resolver
        //    can no longer tell it apart: Broken (a miss; Merged before). The oracle still expects it, merged.
        {18, 20, "directedit nist-d2mi/827-9999-905.stp #20|edge 22", Answer::Broken, Verdict::Missed, "slab of a push",
         false},
        //  - an edge between two pieces of a torus, the pieces joined: the torus's seam, in place;
        {18, 41, "directedit nist-d2mi/827-9999-905.stp #41|edge 94", Answer::Exact, Verdict::Correct,
         "seam of joined pieces", false},
        //  - a face that holds the pick, none of its own samples inside the pick's extent;
        {19, 48, "directedit nist-pmi/nist_ctc_01_asme1_rd_geometry.stp #48|face 117", Answer::Merged, Verdict::Correct,
         "sparse samples", false},
        //  - a piece beside the pick that overlaps no baseline element by more than its boundary;
        {14, 42, k + "rib_plate_1.stp #42|face 1", Answer::Exact, Verdict::Correct, "new piece overlap", false},
        //  - an edge between two pieces of one bore (described by that face twice): the bore's other split lines fit
        //    that description too, but only the pieces on the pick continue it;
        {6, 7, k + "flange_bolt_circle_1.stp #7|edge 15", Answer::Split, Verdict::Correct, "other split lines", false},
        //  - the tuned block, after the oracle changes above: a chamfer corner on a rib's end face, which the end
        //  face's
        //    extent held only by a push/pull's fill, that push/pull suppressed: not judged (reviewed);
        {14, 0, k + "rib_plate_1.stp #0|vertex 7", Answer::Exact, Verdict::Unjudged, "ground only an edit's fill gave"},
        //  - a fillet the kernel rebuilt as a B-spline at its new size: not judged (reviewed);
        {18, 35, "directedit nist-d2mi/827-9999-905.stp #35|vertex 0", Answer::Exact, Verdict::Unjudged,
         "blend built as a B-spline", false},
        // A vertex where two pieces of a blend met a face, the pieces joined by a later push/pull's boolean.
        {7, 0, k + "keyway_tslot_1.stp #0|vertex 0", Answer::Exact, Verdict::Correct, "joined blend pieces"},
        {4, 1, k + "fillet_block_1.stp #1|vertex 15", Answer::Exact, Verdict::Correct, "joined blend pieces"},
        // The reach of a removed face is its own size: a 0.3 mm chamfer ring round a hub does not let the hub's bolt
        // holes grow over each other's faces 50 mm away.
        {6, 9, k + "flange_bolt_circle_1.stp #9|face 5", Answer::Exact, Verdict::Correct, "removed face's reach"},
        // A face beside the picked one on its plane, on ground nobody held (a push/pull slab's side, left a face of its
        // own), is new territory, not part of the picked face.
        {0, 19, k + "block_blind_holes_1.stp #19|face 6", Answer::Exact, Verdict::Correct, "new-territory piece"},
        // Territory of another element counts only on the description's surfaces: an arc tangent to the picked line, a
        // floor a wall stands on, a hole wall another hole crosses.
        {8, 6, k + "l_bracket_1.stp #6|edge 26", Answer::Exact, Verdict::Correct, "tangent arc"},
        {15, 25, k + "slot_through_1.stp #25|face 10", Answer::Exact, Verdict::Correct, "wall on a floor"},
        {7, 47, k + "keyway_tslot_1.stp #47|face 14", Answer::Exact, Verdict::Correct, "crossing hole walls"},
        // Faces bordering a bore closed down run on over the ring between the radii.
        {3, 49, k + "closed_pocket_3.stp #49|edge 5", Answer::Exact, Verdict::Correct, "bore closed down"},
        // Not judged (each on reviewed_unjudged.txt): the pick and another piece mapped onto one place; merged only for
        // an element that cannot be mapped; the owner gone while an element holds all of the pick's ground.
        {6, 3, k + "flange_bolt_circle_1.stp #3|edge 23", Answer::Split, Verdict::Unjudged, "shared anchor"},

        {17, 20, "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #20|face 13", Answer::Merged, Verdict::Unjudged,
         "owner gone, ground held"},
    };
    std::vector<std::string> skipped, offPlatform;
    for (const Case& c : cases) {
        if (keyOnly && std::none_of(key.begin(), key.end(), [&](const std::string& s) {
                return c.id.size() >= s.size() && c.id.compare(c.id.size() - s.size(), s.size(), s) == 0;
            }))
            continue;
        std::string edit;
        const std::vector<Looked> looked = replayAndLook(c.part, c.seq, {c.id}, c.byIndex, c.seed, c.wrongs, &edit);
        INFO(c.id << " (" << c.about << ")");
        // The case's precondition: this platform's kernel builds the sequence so that it records the reference (the
        // macOS kernel builds other geometry, so some sequences differ). If not, the case is skipped, said so; the
        // sequence's own answers were still judged above, and none may be wrong.
        const bool samePick =
            looked.empty() || c.pickMeasure <= 0 ||
            std::abs(looked[0].pickMeasure - c.pickMeasure) <= 2e-2 * c.pickMeasure; // samples' weights
        if (looked.empty() || !samePick) {
            if (!samePick)
                edit += " (the reference is to another element here: picked measure " +
                        std::to_string(looked[0].pickMeasure) + ")";
            if (c.builtOn != nullptr && std::string(c.builtOn) != kPlatformName) {
                offPlatform.push_back(c.id + " (" + c.about + ", authored on " + c.builtOn + ")");
                continue;
            }
            skipped.push_back(c.id + " (" + c.about + ") is not recorded; the sequence's edits: " + edit);
            continue;
        }
        REQUIRE(looked.size() == 1);
        INFO("verdict " << verdictName(looked[0].judgement.verdict));
        CHECK(looked[0].answer == c.answer);
        CHECK(looked[0].judgement.verdict == c.verdict);
        if (c.verdict == Verdict::Correct || c.verdict == Verdict::BrokenGone)
            CHECK(isWrong(looked[0].other.verdict));
        CHECK(looked[0].judgement.newGround.empty() == !c.newGround);
        REQUIRE(looked[0].probes.size() == c.wrongs.size());
        for (std::size_t i = 0; i < c.wrongs.size(); ++i) {
            INFO("negative " << i << ": " << verdictName(looked[0].probes[i].verdict) << "; " << looked[0].probed[i]);
            CHECK(isWrong(looked[0].probes[i].verdict));
        }
    }
    reportPinnedSkips(skipped, offPlatform);
}

} // namespace

TEST_CASE("Oracle and kernel (directedit replays, key cases): one refinement of each kind, on its sequence",
          "[naming][directedit][oracle-gate]")
{
    replayCases(true);
}

TEST_CASE("Oracle and kernel (directedit replays): the refinements of T-081b on the sequences that showed them",
          "[naming][directedit-replays]")
{
    replayCases(false);
}

namespace {

// The baseline and edited scripts of pinned sequence `name` (tests/naming/oracle/pinned_scripts.txt).
std::pair<std::string, std::string> pinnedScripts(const std::string& name)
{
    std::ifstream in(std::filesystem::path(RAY_ORACLE_DIR) / "pinned_scripts.txt", std::ios::binary);
    REQUIRE(in.good());
    std::string line, base, edited;
    int where = 0; // 0 elsewhere, 1 baseline, 2 edited
    bool mine = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("SCRIPT ", 0) == 0) {
            mine = line.substr(7) == name;
            where = 0;
        } else if (mine && line == "BASELINE") {
            where = 1;
        } else if (mine && line == "EDITED") {
            where = 2;
        } else if (line == "END") {
            mine = false;
            where = 0;
        } else if (mine && where == 1) {
            base += line + "\n";
        } else if (mine && where == 2) {
            edited += line + "\n";
        }
    }
    INFO("pinned script " << name);
    REQUIRE(!base.empty());
    REQUIRE(!edited.empty());
    return {base, edited};
}

Vec3 hex(const char* x, const char* y, const char* z)
{
    return {std::strtod(x, nullptr), std::strtod(y, nullptr), std::strtod(z, nullptr)};
}

} // namespace

TEST_CASE("Pinned sequences as scripts (round 4): the Windows-authored pins, on every platform",
          "[naming][directedit-replays]")
{
    // T-081b round 4, item 8: the pins found on sequences that only the Windows generator authors (holdout2 7:44 faces
    // 27
    // and 30, holdout1 7:33 face 18 and 8:23 face 1, sorted 5:15 face 54) as fixed scripts (pinned_scripts.txt), so
    // that
    // every platform replays them: the picked element by its fingerprint, the negatives by a point on each.
    struct Pin {
        const char* script;
        int part;
        const char* about;
        Vec3 at;
        double measure;
        Answer answer;
        Verdict verdict;
        std::vector<Probe> wrongs;
    };
    const std::vector<Pin> pins{
        {"holdout2 7:44",
         7,
         "a window of a moved boss's start cap: its place under another face (face 27)",
         {22.0014, -0.0257, -11.1774},
         0.3131,
         Answer::Broken,
         Verdict::Missed,
         {{Answer::Exact, {}, -1, {hex("0x1.6005ce0000000p+4", "0x1.8eff1d5555555p+0", "-0x1.96bde8aaaaaabp+3")}},
          {Answer::Exact, {}, -1, {hex("0x1.6005ce0000000p+4", "-0x1.5e5f900000000p+0", "-0x1.2376535555556p+3")}}}},
        {"holdout2 7:44",
         7,
         "the boss's end cap in two pieces, moved or new ground (face 30)",
         {43.3498, 0.1598, -11.4613},
         0.5507,
         Answer::Broken,
         Verdict::Missed,
         {{Answer::Split, {}, -1, {hex("0x1.5acc540000000p+5", "0x1.00a4755555555p-1", "-0x1.7412d0aaaaaabp+3")}},
          {Answer::Exact, {}, -1, {hex("0x1.5acc540000000p+5", "0x1.8eff1d5555555p+0", "-0x1.96bde8aaaaaabp+3")}}}},
        {"holdout1 7:33",
         7,
         "another patch of a boss's side, structural (face 18)",
         {39.2339, -2.5489, 10.5459},
         27.0338,
         Answer::Broken,
         Verdict::BrokenGone,
         {{Answer::Exact, {}, -1, {hex("0x1.3941a20000000p+5", "0x1.ea9f34aaaaaa8p+0", "0x1.7b91b3aaaaaabp+3")}}}},
        {"holdout1 8:23",
         8,
         "a cut's wall through the part moved along itself, a rigid copy (face 1)",
         {24.8149, 23.7970, 4.0},
         434.3759,
         Answer::Exact,
         Verdict::Correct,
         {}},
        {"sorted 5:15",
         5,
         "a sliver of a boss's side a moved cut had split off (face 54)",
         {80.6955, 114.4066, 26.7698},
         2.0109,
         Answer::Broken,
         Verdict::BrokenGone,
         {}},
    };
    for (const Pin& p : pins) {
        const auto [baseline, edited] = pinnedScripts(p.script);
        const std::vector<Looked> looked =
            replayScriptedAndLook(p.part, baseline, edited, SubKind::Face, p.at, p.measure, p.wrongs);
        INFO(p.script << ": " << p.about);
        REQUIRE(looked.size() == 1);
        INFO("verdict " << verdictName(looked[0].judgement.verdict));
        CHECK(looked[0].answer == p.answer);
        CHECK(looked[0].judgement.verdict == p.verdict);
        if (p.verdict == Verdict::Correct || p.verdict == Verdict::BrokenGone)
            CHECK(isWrong(looked[0].other.verdict));
        REQUIRE(looked[0].probes.size() == p.wrongs.size());
        for (std::size_t i = 0; i < p.wrongs.size(); ++i) {
            INFO("negative " << i << ": " << verdictName(looked[0].probes[i].verdict) << "; " << looked[0].probed[i]);
            CHECK(isWrong(looked[0].probes[i].verdict));
        }
    }
}

TEST_CASE("Oracle (directedit 7:12, 15:46; hidden 6:47): edits the oracle's model does not describe are not modelled",
          "[naming][directedit-replays]")
{
    // keyway_tslot_1 #12: deleting a hole whose mouth chamfer Defeaturing extends into a cone where the bore was;
    // slot_through_1 #46: deleting an arc wall of a cut leaves a remnant of its straight wall. Which logical face such
    // a face continues is not what the oracle's model of a removal says, so the step is skipped (counted), not judged.
    // flange_bolt_circle_1 #47 of the hidden block (choices by geometry): pushing the bottom face that a boss's start
    // cap had been unified into (by the boss's own join) moves the cap's ground too; the model of an offset moves only
    // its targets, so the step is not modelled.
    struct Case {
        int part, seq;
        bool byIndex;
        const char* outcome;
    };
    const std::vector<Case> cases{
        {7, 12, true, "delete_hole: not modelled (a removed face is still there in part)"},
        {15, 46, true, "delete_hole: not modelled (a removed face is still there in part)"},
        {6, 47, false, "offset_face: not modelled (another face's ground may be on the moved plane)"}};
    std::vector<std::string> skipped;
    for (const Case& c : cases) {
        const Part pt = loadPart(kParts[c.part]);
        REQUIRE(pt.base != nullptr);
        SequenceOptions opt;
        opt.byIndex = c.byIndex;
        const SequenceResult r = runSequence(pt, c.part, c.seq, opt);
        INFO(kParts[c.part] << " #" << c.seq << ": " << r.edit);
        CHECK_FALSE(r.skipped);
        CHECK(r.tally.wrong() == 0); // whether or not this platform's kernel builds the case
        // The case's precondition: the sequence makes the edit it is about (the macOS kernel builds other geometry, so
        // some sequences differ). If not, the case is skipped, said so.
        const bool edited = std::any_of(r.editCounts.begin(), r.editCounts.end(), [&c](const auto& e) {
            return e.first.rfind(std::string(c.outcome).substr(0, std::string(c.outcome).find(':')) + ":", 0) == 0;
        });
        const auto it = r.editCounts.find(c.outcome);
        if (!edited && it == r.editCounts.end()) {
            skipped.push_back(std::string(kParts[c.part]) + " #" + std::to_string(c.seq) + " makes no " + c.outcome +
                              " edit; its edits: " + r.edit);
            continue;
        }
        CHECK((it != r.editCounts.end() && it->second >= 1));
    }
    reportPinnedSkips(skipped);
}
TEST_CASE("debug: look at one sequence", "[.debug-directedit]")
{
    int part = 0, seq = 0;
    if (const char* only = std::getenv("RAYCAD_LOOK"))
        std::sscanf(only, "%d:%d", &part, &seq);
    const Part pt = loadPart(kParts[part]);
    setInspector([&](const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
                     const Owners& now, const Samples& samples) {
        // RAYCAD_LOOK_ID=<text>: every reference whose id ends with it, whatever its verdict, briefly.
        if (const char* want = std::getenv("RAYCAD_LOOK_ID");
            want != nullptr && id.size() >= std::strlen(want) &&
            id.compare(id.size() - std::strlen(want), std::string::npos, want) == 0) {
            std::string els;
            for (const SubId& s : res.elements)
                els += " " + std::to_string(s.index);
            std::printf("LOOK outline %zu chords\n", ref.ref.outline.chords.size());
            std::printf("LOOK %s: names %s; answer %d {%s }, verdict %s; desc %s\n", id.c_str(), ref.names.c_str(),
                        static_cast<int>(rr.answer), els.c_str(), verdictName(rr.judgement.verdict),
                        explain(ref.record.truth, *ref.record.owners, *ref.record.baseline,
                                static_cast<long>(ref.record.base.index))
                            .c_str());
        }
        // RAYCAD_LOOK_UNJUDGED: the answers not judged too.
        if (!isWrong(rr.judgement.verdict) &&
            !(rr.judgement.verdict == Verdict::Unjudged && std::getenv("RAYCAD_LOOK_UNJUDGED") != nullptr))
            return;
        const ElemSamples& p = ref.record.picked();
        {
            // The pieces of the logical element at pick time (the description without its separators).
            TruthDesc whole = ref.record.truth;
            whole.seps.clear();
            for (Constraint& c : whole.cons)
                c.seps.clear();
            for (const std::uint32_t e : evalTruth(whole, *ref.record.owners, *ref.record.baseline).elems) {
                const ElemSamples& b = ref.record.baseline->of(ref.record.truth.kind)[e];
                std::printf(
                    "%s: baseline piece b%u%s type %d nrm (%.3f %.3f %.3f) box (%.3f %.3f %.3f)-(%.3f %.3f %.3f) "
                    "n%zu\n",
                    id.c_str(), e, e == ref.record.base.index ? " (the pick)" : "", b.type, b.nrm.x, b.nrm.y, b.nrm.z,
                    b.boxMin.x, b.boxMin.y, b.boxMin.z, b.boxMax.x, b.boxMax.y, b.boxMax.z, b.pts.size());
                bool moved = false;
                const ElemSamples m = mapElement(*ref.record.baseline, SubId{ref.record.truth.kind, e},
                                                 ref.record.truth, *ref.record.owners, now, moved);
                for (const SubId& s : res.elements)
                    std::printf(
                        "    mapped b%u: %zu samples, %.0f %% of them on answer %u, %.0f %% of the answer on it\n", e,
                        m.pts.size(), 100.0 * shareOnElement(m, samples.of(s.kind)[s.index], s.kind), s.index,
                        100.0 * shareOnElement(samples.of(s.kind)[s.index], m, s.kind));
            }
        }
        std::printf("%s: picked type %d nrm (%.3f %.3f %.3f) sided %d sigma %d box (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n",
                    id.c_str(), p.type, p.nrm.x, p.nrm.y, p.nrm.z, p.sided ? 1 : 0, p.sigma, p.boxMin.x, p.boxMin.y,
                    p.boxMin.z, p.boxMax.x, p.boxMax.y, p.boxMax.z);
        for (const SubId& s : res.elements) {
            const ElemSamples& a = samples.of(s.kind)[s.index];
            std::printf(
                "   answer %u type %d nrm (%.3f %.3f %.3f) sided %d sigma %d box (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n",
                s.index, a.type, a.nrm.x, a.nrm.y, a.nrm.z, a.sided ? 1 : 0, a.sigma, a.boxMin.x, a.boxMin.y,
                a.boxMin.z, a.boxMax.x, a.boxMax.y, a.boxMax.z);
            std::printf("   against the owners now: %s\n", explain(ref.record.truth, now, samples, s.index).c_str());
            std::printf("   against the baseline owners: %s\n",
                        explain(ref.record.truth, *ref.record.owners, samples, s.index).c_str());
            // The answer's faces against each owner of the description, now.
            if (s.kind != SubKind::Face)
                for (const std::uint32_t f : samples.facesOf[s.kind == SubKind::Edge ? 1 : 0][s.index])
                    for (const Constraint& c : ref.record.truth.cons)
                        for (const std::string& alt : c.alts) {
                            TruthDesc d;
                            d.kind = SubKind::Face;
                            d.cons.push_back(Constraint{{alt}, {}});
                            const ElemSamples& fs = samples.of(SubKind::Face)[f];
                            std::printf("     face %u type %d box (%.3f %.3f %.3f)-(%.3f %.3f %.3f) vs %s\n", f,
                                        fs.type, fs.boxMin.x, fs.boxMin.y, fs.boxMin.z, fs.boxMax.x, fs.boxMax.y,
                                        fs.boxMax.z, explain(d, now, samples, f).c_str());
                        }
        }
    });
    // RAYCAD_LOOK_SEED=<hex seed base>: another block's seeds; RAYCAD_LOOK_SORTED: choices by geometry.
    SequenceOptions look;
    look.byIndex = std::getenv("RAYCAD_LOOK_SORTED") == nullptr;
    if (const char* seed = std::getenv("RAYCAD_LOOK_SEED"))
        look.seedBase = std::strtoull(seed, nullptr, 16);
    const SequenceResult r = runSequence(pt, part, seq, look);
    setInspector({});
    for (const std::string& w : r.log.wrongs)
        std::printf("%s\n", w.c_str());
}

// Diagnosis: how long a direct edit abandoned at its time limit goes on running on its kernel thread. RAYCAD_LOOK as
// above; prints, after the sequence, how long until no abandoned edit thread is left.
TEST_CASE("debug: abandoned direct edits stop", "[.debug-cancel]")
{
    int part = 0, seq = 0;
    if (const char* only = std::getenv("RAYCAD_LOOK"))
        std::sscanf(only, "%d:%d", &part, &seq);
    const Part pt = loadPart(kParts[part]);
    SequenceOptions look;
    look.byIndex = std::getenv("RAYCAD_LOOK_SORTED") == nullptr;
    const auto start = std::chrono::steady_clock::now();
    const SequenceResult r = runSequence(pt, part, seq, look);
    const auto done = std::chrono::steady_clock::now();
    std::printf("sequence %.1f s, %d past the time limit; edit threads running at its end: %d\n",
                std::chrono::duration<double>(done - start).count(), r.timeouts, kernel::testing::runningEditThreads());
    while (kernel::testing::runningEditThreads() > 0 &&
           std::chrono::steady_clock::now() - done < std::chrono::minutes(15))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("no edit thread left %.1f s after the sequence ended (%d still running)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - done).count(),
                kernel::testing::runningEditThreads());
}

// ---------------------------------------------------------------- evidence for an independent review
namespace {

std::string describeElement(const kernel::Shape& s, SubId e)
{
    char buf[700];
    if (e.kind == SubKind::Face) {
        const auto i = kernel::surfaceInfo(s, e);
        if (!i)
            return "face " + std::to_string(e.index) + ": unreadable";
        std::snprintf(buf, sizeof buf,
                      "face %u: surface type %d (0 plane, 1 cylinder, 2 cone, 3 sphere, 4 torus, 5 B-spline, 6 other), "
                      "origin (%.6f %.6f %.6f), axis (%.6f %.6f %.6f), radius %.6f, semi-angle %.6f, face reversed %d; "
                      "area %.6f mm^2, centroid (%.6f %.6f %.6f), box (%.6f %.6f %.6f)-(%.6f %.6f %.6f)",
                      e.index, static_cast<int>(i->type), i->origin.x, i->origin.y, i->origin.z, i->axis.x, i->axis.y,
                      i->axis.z, i->radius, i->semiAngle, i->reversed ? 1 : 0, i->area, i->centroid.x, i->centroid.y,
                      i->centroid.z, i->bounds.min.x, i->bounds.min.y, i->bounds.min.z, i->bounds.max.x,
                      i->bounds.max.y, i->bounds.max.z);
        return buf;
    }
    if (e.kind == SubKind::Edge) {
        const auto i = kernel::curveInfo(s, e);
        if (!i)
            return "edge " + std::to_string(e.index) + ": unreadable";
        std::snprintf(
            buf, sizeof buf,
            "edge %u: curve type %d (0 line, 1 circle, 2 ellipse, 3 B-spline, 4 other), start (%.6f %.6f %.6f), "
            "end (%.6f %.6f %.6f), centre (%.6f %.6f %.6f), axis (%.6f %.6f %.6f), radius %.6f, length %.6f mm, "
            "closed %d",
            e.index, static_cast<int>(i->type), i->start.x, i->start.y, i->start.z, i->end.x, i->end.y, i->end.z,
            i->center.x, i->center.y, i->center.z, i->axis.x, i->axis.y, i->axis.z, i->radius, i->length,
            i->closed ? 1 : 0);
        return buf;
    }
    const auto p = kernel::vertexPoint(s, e);
    if (!p)
        return "vertex " + std::to_string(e.index) + ": unreadable";
    std::snprintf(buf, sizeof buf, "vertex %u: point (%.6f %.6f %.6f)", e.index, p->x, p->y, p->z);
    return buf;
}

std::string boxOfSamples(const ElemSamples& s)
{
    char buf[200];
    std::snprintf(buf, sizeof buf, "sample box (%.6f %.6f %.6f)-(%.6f %.6f %.6f)", s.boxMin.x, s.boxMin.y, s.boxMin.z,
                  s.boxMax.x, s.boxMax.y, s.boxMax.z);
    return buf;
}

void writeFile(const std::filesystem::path& p, const std::string& text)
{
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

void writeShape(const std::filesystem::path& dir, const std::string& stem, const kernel::Shape& s)
{
    const std::vector<std::byte> bytes = kernel::serialize(s);
    std::FILE* f = std::fopen((dir / (stem + ".rbrep")).string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    JobContext ctx;
    const std::array<kernel::Shape, 1> one{s};
    CHECK(kernel::exportStep((dir / (stem + ".step")).string(), one, ctx).has_value());
}

} // namespace

// RAYCAD_LOOK=<part>:<seq>, RAYCAD_LOOK_SORTED, RAYCAD_LOOK_SEED=<hex>, RAYCAD_LOOK_ID=<the reference id's end, such
// as "#18|face 10">, RAYCAD_EVIDENCE_DIR=<folder>: the evidence for one judged reference, for a reviewer who has not
// seen the author's reasoning (T-081b round 2): the shapes before and after (kernel serialisation and STEP), the
// elements, the pick-time evidence, the scripts, and the two claims.
TEST_CASE("evidence: one judged reference of one sequence, written out", "[.evidence]")
{
    int part = 0, seq = 0;
    if (const char* only = std::getenv("RAYCAD_LOOK"))
        std::sscanf(only, "%d:%d", &part, &seq);
    const char* want = std::getenv("RAYCAD_LOOK_ID");
    const char* out = std::getenv("RAYCAD_EVIDENCE_DIR");
    REQUIRE(want != nullptr);
    REQUIRE(out != nullptr);
    const std::string suffix = want;
    const Part pt = loadPart(kParts[part]);
    struct Seen {
        bool found = false;
        std::string id, names, verdict, resolver, oracle, description, explanation;
        SubKind kind = SubKind::Face;
        std::uint32_t pick = 0;
        std::vector<std::uint32_t> answer, expected;
        naming::Territory territory;
        naming::PickOutline outline;
        ElemSamples picked;
        std::vector<std::pair<std::uint32_t, ElemSamples>> nowSamples;
    } seen;
    setInspector([&](const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
                     const Owners& now, const Samples& samples) {
        if (id.size() < suffix.size() || id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0)
            return;
        seen.found = true;
        seen.id = id;
        seen.names = ref.names;
        seen.kind = ref.record.base.kind;
        seen.pick = ref.record.base.index;
        seen.picked = ref.record.picked();
        seen.territory = ref.ref.territory;
        seen.outline = ref.ref.outline;
        for (const SubId& s : res.elements)
            seen.answer.push_back(s.index);
        seen.expected = rr.expected;
        seen.verdict = verdictName(rr.judgement.verdict);
        std::string els;
        for (const std::uint32_t e : seen.answer)
            els += " " + std::to_string(e);
        static const char* const states[] = {"Exact", "Split", "Merged", "Repaired", "Broken"};
        seen.resolver = std::string(states[static_cast<int>(res.state)]) + " {" + els + " } because " +
                        (!res.reason.empty() ? res.reason
                         : res.state == naming::ResolveState::Exact
                             ? std::string("a name the reference holds is found on that element, and no pick-time "
                                           "evidence rule objects")
                             : std::string("its names, followed through the build, lead there"));
        std::string ex;
        for (const std::uint32_t e : rr.expected)
            ex += " " + std::to_string(e);
        seen.oracle = "{" + ex + " }" + (rr.merged ? " (larger than described: merged)" : "") +
                      (rr.noAnchor.empty() ? "" : " (" + rr.noAnchor + ")") +
                      (rr.crossing.empty() ? "" : " (sliding crossing: " + rr.crossing + ")");
        seen.description = explain(ref.record.truth, *ref.record.owners, *ref.record.baseline,
                                   static_cast<long>(ref.record.base.index));
        std::set<std::uint32_t> all(seen.answer.begin(), seen.answer.end());
        all.insert(rr.expected.begin(), rr.expected.end());
        for (const std::uint32_t e : all) {
            seen.explanation += "  element " + std::to_string(e) + " against the description, owners now: " +
                                explain(ref.record.truth, now, samples, static_cast<long>(e)) + "\n";
            seen.nowSamples.emplace_back(e, samples.of(seen.kind)[e]);
        }
    });
    SequenceOptions look;
    look.byIndex = std::getenv("RAYCAD_LOOK_SORTED") == nullptr;
    if (const char* s = std::getenv("RAYCAD_LOOK_SEED"))
        look.seedBase = std::strtoull(s, nullptr, 16);
    const SequenceResult r = runSequence(pt, part, seq, look);
    setInspector({});
    REQUIRE(seen.found);
    const std::filesystem::path dir(out);
    std::filesystem::create_directories(dir);
    writeShape(dir, "before", r.baselineShape);
    writeShape(dir, "after", r.replayShape);

    const char* kindName = seen.kind == SubKind::Face ? "face" : seen.kind == SubKind::Edge ? "edge" : "vertex";
    std::string elements = std::string("Indices are SubIds (") + kindName +
                           " order of the kernel's canonical traversal) of the shapes written beside this file.\n\n"
                           "PICKED (in before.*): " +
                           describeElement(r.baselineShape, SubId{seen.kind, seen.pick}) + "; " +
                           boxOfSamples(seen.picked) + "\n\nAFTER (in after.*):\n";
    for (const auto& [e, s] : seen.nowSamples) {
        const bool inAnswer = std::find(seen.answer.begin(), seen.answer.end(), e) != seen.answer.end();
        const bool inExpected = std::find(seen.expected.begin(), seen.expected.end(), e) != seen.expected.end();
        elements += std::string("  [") + (inAnswer ? "resolver" : "") + (inAnswer && inExpected ? ", " : "") +
                    (inExpected ? "oracle" : "") + "] " + describeElement(r.replayShape, SubId{seen.kind, e}) + "; " +
                    boxOfSamples(s) + "\n";
    }
    elements += "\nPICK-TIME EVIDENCE recorded in the reference (quanta of 1e-4 mm):\n  territory box (";
    for (std::size_t i = 0; i < 3; ++i)
        elements += std::to_string(seen.territory.qMin[i]) + (i < 2 ? " " : ")-(");
    for (std::size_t i = 0; i < 3; ++i)
        elements += std::to_string(seen.territory.qMax[i]) + (i < 2 ? " " : "), measure ");
    elements += std::to_string(seen.territory.qMeasure) + ", points:";
    for (const auto& p : seen.territory.points)
        elements += " (" + std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + ")";
    elements += "\n  outline: " + std::to_string(seen.outline.chords.size()) + " chords (x0 y0 z0 x1 y1 z1 slack)\n";
    for (const auto& ch : seen.outline.chords) {
        elements += "   ";
        for (const std::int64_t v : ch)
            elements += " " + std::to_string(v);
        elements += "\n";
    }
    writeFile(dir / "elements.txt", elements);
    writeFile(dir / "operation.txt", "The script the references were made on (before):\n" + r.scriptText +
                                         "\nThe edit: " + r.edit + "\n\nThe edited script (after):\n" + r.editedText +
                                         "\nReplay: built" + r.replayText + "\n");
    writeFile(dir / "claims.md",
              "# " + seen.id + "\n\nReference names: " + seen.names +
                  "\n\n"
                  "**The resolver says** " +
                  seen.resolver +
                  "\n\n"
                  "**The oracle expects** " +
                  seen.oracle + ", and so judges the resolver's answer: " + seen.verdict +
                  ".\n\nThe oracle's description of the picked element (its owners at pick time, and how the picked "
                  "element fits it): " +
                  seen.description +
                  "\n\nEach element of either answer against that description, "
                  "with the owners as the edited script builds them:\n" +
                  seen.explanation);
}

// The round-3 review's probes (the reviewer's, kept hidden): each listed reference of holdout 2 and the tuned block,
// the resolver's answer and the oracle's verdicts on the answers the review asked about, printed.
TEST_CASE("Reviewed answers measured: how much of the pick each answer holds, in place and mapped",
          "[.reviewed-measure]")
{
    // T-081b round 4, item 7: every round-3 entry of reviewed_unjudged.txt re-checked against "holds some of the pick
    // in
    // place or under the exact map". Prints, per entry, the resolver's answer and verdict now, and for each answer
    // element the share of the picked samples on it and of its samples on the pick, in place and mapped through the
    // edit (shareOnElement), and whether the structural check's scope applies.
    struct E {
        int part, seq;
        const char* id;
        std::uint64_t seed;
    };
    const std::uint64_t h1 = 0xA81B7000ull, h2 = 0xB2081B00ull;
    const std::vector<E> entries{
        {0, 0, "#0|face 13", h1},    {0, 0, "#0|edge 28", h1},    {4, 33, "#33|edge 9", h1},
        {6, 33, "#33|face 9", h1},   {9, 16, "#16|vertex 27", h1}, {17, 45, "#45|face 21", h1},
        {17, 45, "#45|edge 27", h1}, {17, 45, "#45|edge 52", h1}, {17, 45, "#45|edge 62", h1},
        {17, 45, "#45|vertex 0", h1}, {17, 45, "#45|vertex 1", h1}, {17, 45, "#45|vertex 24", h1},
        {17, 45, "#45|vertex 31", h1}, {18, 47, "#47|face 18", h1}, {1, 3, "#3|edge 42", h2},
        {11, 41, "#41|vertex 30", h2}, {17, 24, "#24|edge 3", h2},  {19, 37, "#37|face 90", h2},
        // Round 4's own: answers the new moved-face rule gives that the oracle cannot judge.
        {4, 45, "#45|face 20", h1},
        {10, 7, "#7|face 3", 0x6B1D3F5A2C800000ull},
        {10, 7, "#7|edge 18", 0x6B1D3F5A2C800000ull},
        {1, 48, "#48|vertex 18", 0x6B1D3F5A2C800000ull}, {7, 34, "#34|vertex 9", 0x6B1D3F5A2C800000ull}};
    for (const E& e : entries) {
        const Part part = loadPart(kParts[e.part]);
        REQUIRE(part.base != nullptr);
        SequenceOptions opt;
        opt.byIndex = false;
        opt.seedBase = e.seed;
        const std::string want = std::string(" #") + std::to_string(e.seq) + "|" + std::string(e.id).substr(
                                                                                     std::string(e.id).find('|') + 1);
        std::string line;
        setInspector([&](const std::string& id, const Ref& ref, const naming::Resolved& res, const RefResult& rr,
                         const Owners& now, const Samples& samples) {
            if (id.size() < want.size() || id.compare(id.size() - want.size(), want.size(), want) != 0)
                return;
            const Expectation x = expect(ref.record, now, samples);
            const SubKind kind = ref.record.base.kind;
            const ElemSamples& P = ref.record.picked();
            char buf[400];
            static const char* const kAnswerNames[] = {"Exact", "Split", "Merged", "Repaired", "Broken"};
            std::snprintf(buf, sizeof buf, "%s: %s {", id.c_str(), kAnswerNames[static_cast<int>(rr.answer)]);
            line = buf;
            for (const SubId& s : res.elements)
                line += " " + std::to_string(s.index);
            line += std::string(" } verdict ") + verdictName(rr.judgement.verdict) +
                    (rr.judgement.structural.empty() ? "" : " [structural: " + rr.judgement.structural + "]") +
                    "; E {";
            for (const std::uint32_t el : x.expected.elems)
                line += " " + std::to_string(el);
            line += " }" + (x.noAnchor.empty() ? std::string() : " (no anchor: " + x.noAnchor + ")");
            for (const SubId& s : res.elements) {
                const ElemSamples& A = samples.of(kind)[s.index];
                std::snprintf(buf, sizeof buf,
                              "\n    %u: pick on it %.3f, it on pick %.3f; mapped pick on it %.3f, it on mapped %.3f",
                              s.index, shareOnElement(P, A, kind), shareOnElement(A, P, kind),
                              x.mapped.pts.empty() ? -1.0 : shareOnElement(x.mapped, A, kind),
                              x.mapped.pts.empty() ? -1.0 : shareOnElement(A, x.mapped, kind));
                line += buf;
                // The pick carried by each sweep the edit moved (its frame's displacement): its share on the answer.
                for (const auto& [sid, sweep] : now.sweeps) {
                    const auto was = ref.record.owners->sweeps.find(sid);
                    if (was == ref.record.owners->sweeps.end())
                        continue;
                    const Vec3 d = sub(sweep.frame.o, was->second.frame.o);
                    if (norm(d) <= 1e-9)
                        continue;
                    ElemSamples moved = P;
                    for (Vec3& q : moved.pts)
                        q = add(q, d);
                    for (Vec3& q : moved.nodes)
                        q = add(q, d);
                    std::snprintf(buf, sizeof buf, "; pick moved with %s (%.3f %.3f %.3f) on it %.3f, it on that %.3f",
                                  sid.c_str(), d.x, d.y, d.z, shareOnElement(moved, A, kind),
                                  kind == SubKind::Face ? -1.0 : shareOnElement(A, moved, kind));
                    line += buf;
                }
                // The baseline elements it lies on (its samples' share on each, over 1 %), the picked one marked.
                const auto& B = ref.record.baseline->of(kind);
                for (std::uint32_t i = 0; i < B.size(); ++i) {
                    const double share = B[i].pts.empty() ? 0.0 : shareOnElement(A, B[i], kind);
                    if (share > 0.01) {
                        std::snprintf(buf, sizeof buf, "; on baseline %u%s %.3f", i,
                                      i == ref.record.base.index ? " (the pick)" : "", share);
                        line += buf;
                    }
                }
            }
        });
        const SequenceResult r = runSequence(part, e.part, e.seq, opt);
        setInspector({});
        std::printf("== %s %s (%d:%d, seed %llx) edits: %s\n   %s\n", kParts[e.part], e.id, e.part, e.seq,
                    static_cast<unsigned long long>(e.seed), r.edit.c_str(),
                    line.empty() ? "not recorded on this build" : line.c_str());
    }
}

TEST_CASE("review probe: answers asked of the oracle", "[.review-probes]")
{
    struct P {
        int part, seq;
        std::string id;
        bool byIndex;
        std::uint64_t seed;
        std::vector<Probe> probes;
    };
    const std::string k = "directedit raycad-synthetic/";
    const std::uint64_t h2 = 0xB2081B00ull;
    const std::vector<P> ps{
        {0,
         18,
         k + "block_blind_holes_1.stp #18|face 10",
         false,
         h2,
         {{Answer::Exact, {13}}, {Answer::Split, {13, 47}}}},
        {14,
         34,
         k + "rib_plate_1.stp #34|face 0",
         false,
         h2,
         {{Answer::Split, {0, 3, 6, 11, 16}}, {Answer::Split, {0, 6, 11}}, {Answer::Exact, {0}}}},
        {17,
         24,
         "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #24|edge 5",
         false,
         h2,
         {{Answer::Exact, {5}}, {Answer::Exact, {3}}, {Answer::Split, {3, 5}}, {Answer::Merged, {3}}}},
        {7,
         44,
         k + "keyway_tslot_1.stp #44|face 27",
         false,
         h2,
         {{Answer::Exact, {20}}, {Answer::Merged, {13}}, {Answer::Exact, {13}}}},
        {7,
         44,
         k + "keyway_tslot_1.stp #44|face 30",
         false,
         h2,
         {{Answer::Split, {32}}, {Answer::Split, {22, 32}}, {Answer::Exact, {22}}}},
        {9, 35, k + "manifold_block_1.stp #35|edge 54", false, h2, {{Answer::Exact, {55}}, {Answer::Exact, {56}}}},
        {9, 35, k + "manifold_block_1.stp #35|vertex 38", false, h2, {{Answer::Exact, {38}}}},
        {12,
         33,
         k + "plate_countersink_1.stp #33|edge 5",
         true,
         0,
         {{Answer::Exact, {2}}, {Answer::Exact, {19}}, {Answer::Split, {2, 19}}}},
    };
    for (const P& p : ps) {
        std::string edit;
        const std::vector<Looked> looked = replayAndLook(p.part, p.seq, {p.id}, p.byIndex, p.seed, p.probes, &edit);
        std::printf("== %s (%d:%d) edits: %s\n", p.id.c_str(), p.part, p.seq, edit.c_str());
        if (looked.empty()) {
            std::printf("   not recorded on this build\n");
            continue;
        }
        std::string els;
        for (const std::uint32_t e : looked[0].elements)
            els += " " + std::to_string(e);
        std::printf("   resolver answer %d {%s }: %s %s\n", static_cast<int>(looked[0].answer), els.c_str(),
                    verdictName(looked[0].judgement.verdict), looked[0].judgement.structural.c_str());
        for (std::size_t i = 0; i < looked[0].probes.size(); ++i)
            std::printf("   probe %zu: %s; %s\n", i, verdictName(looked[0].probes[i].verdict),
                        looked[0].probed[i].c_str());
    }
}
