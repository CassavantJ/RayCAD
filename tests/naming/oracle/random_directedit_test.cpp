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

std::optional<Feature> proposeSweep(const Samples& smp, double partSize, Rng& rng, const std::string& id, bool cut)
{
    std::vector<std::uint32_t> planar;
    const auto& faces = smp.of(SubKind::Face);
    for (std::uint32_t i = 0; i < faces.size(); ++i)
        if (faces[i].type == static_cast<int>(kernel::SurfaceType::Plane) && faces[i].pts.size() >= 3)
            planar.push_back(i);
    if (planar.empty())
        return std::nullopt;
    if (!tByIndex)
        std::stable_sort(planar.begin(), planar.end(),
                         [&](std::uint32_t a, std::uint32_t b) { return boxKey(faces[a]) < boxKey(faces[b]); });
    const ElemSamples& f = faces[planar[rng.below(planar.size())]];
    std::vector<Vec3> pts = f.pts;
    if (!tByIndex)
        std::stable_sort(pts.begin(), pts.end(),
                         [](const Vec3& a, const Vec3& b) { return roundedKey(a) < roundedKey(b); });
    const Vec3 point = pts[rng.below(pts.size())];
    const Vec3 n = unit(f.nrm);
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
    if (!tByIndex)
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
};

struct SequenceOptions {
    std::uint64_t seedBase = 0xD1EC7000ull;
    std::string keyPrefix = "directedit ";
    // Choose among the elements of a built shape in the kernel's order (true: the blocks below, on which the oracle was
    // tuned), or sorted by geometry (the hidden block [.random-directedit-sorted], see there).
    bool byIndex = true;
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
    tByIndex = opt.byIndex;
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
    const int want = 3 + static_cast<int>(rng.below(3));
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
    const std::size_t edits = rng.chance(0.35) ? 2 + rng.below(2) : 1;
    int inserted = 0;
    for (std::size_t k = 0; k < edits; ++k)
        out.edit += (k == 0 ? "" : "; ") + randomEdit(edited, part, b, **smp0, rng, inserted);

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
    if ((!out.log.wrongs.empty() || !out.log.notes.empty()) && std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr) {
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

// The most answers a holdout block may leave not judged (as random_test.cpp's kUnjudgedBudget, scaled from its 200
// sequences to these blocks' 1,000).
constexpr long kHoldoutUnjudgedBudget = 20;

// A holdout block (`holdoutPrefix`, its keys' prefix) is gated as T-081a's: WRONG must be 0 on its first, unseen run;
// its answers not judged are counted, and must be on the reviewed list once the list has entries for the block, and at
// most kHoldoutUnjudgedBudget before.
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
        std::printf("%sblock: %ld answers not judged (budget %ld), %ld of them on the reviewed list, which lists %ld "
                    "for this block\n",
                    holdoutPrefix.c_str(), t.unjudged(), kHoldoutUnjudgedBudget, t.unjudgedReviewed, listed);
        CHECK(t.unjudged() <= kHoldoutUnjudgedBudget);
        if (listed > 0)
            CHECK(t.unjudged() == t.unjudgedReviewed);
    }
    CHECK(t.accepted == t.acceptedReviewed);
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
void runHoldout(const char* title, std::uint64_t seedBase, const std::string& prefix)
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
// ---------------------------------------------------------------- oracle refinements on the sequences that showed them
namespace {

struct Looked {
    std::string id;
    Answer answer = Answer::Broken;
    std::vector<std::uint32_t> elements;
    Judgement judgement;
    Judgement other; // the oracle's verdict on another element answered Exact
    std::vector<Judgement> probes; // the oracle's verdicts on the answers asked for (replayAndLook probes)
};

struct Probe {
    Answer answer;
    std::vector<std::uint32_t> elements;
    long offCarrier = -1; // replace this element of elements by the nearest element off the picked one's carriers
};

// Replays sequence `seq` of part `partIndex` of the block and looks at the references `ids`: the resolver's answer and
// its verdict, and the verdict on an Exact answer of another element of the same kind.
std::vector<Looked> replayAndLook(int partIndex, int seq, const std::vector<std::string>& ids, bool byIndex = true,
                                  std::uint64_t seedBase = 0, const std::vector<Probe>& probes = {})
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
        Looked l;
        l.id = id;
        l.answer = rr.answer;
        for (const SubId& s : res.elements)
            l.elements.push_back(s.index);
        l.judgement = rr.judgement;
        const Expectation x = expect(ref.record, now, samples);
        const std::uint32_t got = l.elements.empty() ? 0 : l.elements.front();
        l.other = judgeAnswer(x, ref.record, now, samples, Answer::Exact, {got == 0 ? 1u : got - 1});
        for (const Probe& p : probes) {
            std::vector<std::uint32_t> els = p.elements;
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
                    const double d = (m.x - at.x) * (m.x - at.x) + (m.y - at.y) * (m.y - at.y) + (m.z - at.z) * (m.z - at.z);
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
        }
        out.push_back(std::move(l));
    });
    const SequenceResult r = runSequence(part, partIndex, seq, legacy);
    setInspector({});
    CHECK_FALSE(r.skipped);
    CHECK(r.tally.mutantsCaught() == r.tally.mutantsMade());
    return out;
}

} // namespace

TEST_CASE("Oracle and kernel (directedit replays): the refinements of T-081b on the sequences that showed them",
          "[naming][directedit][oracle-gate]")
{
    // Each case replays one sequence of the block (its seed is fixed) and checks the resolver's answer and the oracle's
    // verdict on the reference the refinement is about; for a correct answer, also that another element answered
    // Exact is judged wrong (the oracle is not vacuous there).
    struct Case {
        int part, seq;
        std::string id;
        Answer answer;
        Verdict verdict;
        const char* about;
        bool byIndex = true; // found with the generator's first version (SequenceOptions::byIndex)
        std::uint64_t seed = 0; // another block's seeds (SequenceOptions::seedBase)
        bool newGround = false; // accepted by the new-ground rule (oracle.cpp newGroundRule)
        std::vector<Probe> wrongs; // answers the oracle must still judge wrong (the relaxed rule's negatives)
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
        {15, 12, k + "slot_through_1.stp #12|face 8", Answer::Split, Verdict::Correct, "remnants of the pick", false,
         0xA81B7000ull, false, {{Answer::Split, {22}}, {Answer::Split, {22, 28, 14}}}},
        //  - remnants: a boss's start cap and its fillet's edge, bounded by a cut that moved away (oracle);
        //    (its fillet's edge, a replayed pick, is judged in the block itself)
        {17, 10, "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #10|face 7", Answer::Split, Verdict::Correct,
         "remnants of a bounded pick", false, 0xA81B7000ull, false, {{Answer::Split, {11}}, {Answer::Exact, {17}}}},
        //  - two vertices 3 um apart, the mapped pick on one (oracle);
        {17, 7, "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #7|vertex 39", Answer::Exact, Verdict::Correct,
         "vertex the mapped pick lies on", false, 0xA81B7000ull, false, {{Answer::Exact, {30}}}},
        {17, 7, "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #7|vertex 42", Answer::Exact, Verdict::Correct,
         "vertex the mapped pick lies on", false, 0xA81B7000ull, false, {{Answer::Exact, {34}}}},
        //  - new ground in a flagged answer: an arc past a seam left out, a disconnected sliver of the top taken in
        //    (accepted, counted); with the extra piece on another element's ground or another carrier, still wrong;
        {3, 31, k + "closed_pocket_3.stp #31|edge 37", Answer::Merged, Verdict::Correct, "new ground left out", false,
         0xA81B7000ull, true, {{Answer::Split, {40, 43}}, {Answer::Exact, {40}}}},
        {14, 49, k + "rib_plate_1.stp #49|face 39", Answer::Split, Verdict::Correct, "new ground taken in", false,
         0xA81B7000ull, true, {{Answer::Split, {2, 9}, 9}, {Answer::Exact, {9}}}},

        // Resolver (propagate_resolve.cpp canContain): a sliver of a boss's cylindrical side, split off by a cut that
        // then moved over it; the rest of the side is whole, but holds none of the sliver: Broken, not Merged.
        {5, 15, k + "fixture_plate_1.stp #15|face 54", Answer::Broken, Verdict::BrokenGone, "piece cut away: resolver",
         false},

        // The hidden block's findings (choices by geometry), each resolved in the oracle:
        //  - an edge moved only because a direct edit moved the separator that cut it off: still in place;
        {13, 31, k + "plate_thru_holes_1.stp #31|edge 47", Answer::Exact, Verdict::Correct, "separator moved by edit",
         false},
        //  - a new coplanar face beside the pick, the pick moved only by a separator a direct edit moved;
        {17, 27, "directedit nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #27|face 17", Answer::Exact, Verdict::Correct,
         "new piece, separator moved by edit", false},
        //  - the faces around a pushed face run on over the slab it swept (an edge there);
        {18, 20, "directedit nist-d2mi/827-9999-905.stp #20|edge 22", Answer::Merged, Verdict::Correct,
         "slab of a push", false},
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
    for (const Case& c : cases) {
        const std::vector<Looked> looked = replayAndLook(c.part, c.seq, {c.id}, c.byIndex, c.seed, c.wrongs);
        INFO(c.id << " (" << c.about << ")");
        REQUIRE(looked.size() == 1);
        INFO("verdict " << verdictName(looked[0].judgement.verdict));
        CHECK(looked[0].answer == c.answer);
        CHECK(looked[0].judgement.verdict == c.verdict);
        if (c.verdict == Verdict::Correct || c.verdict == Verdict::BrokenGone)
            CHECK(isWrong(looked[0].other.verdict));
        CHECK(looked[0].judgement.newGround.empty() == !c.newGround);
        REQUIRE(looked[0].probes.size() == c.wrongs.size());
        for (std::size_t i = 0; i < c.wrongs.size(); ++i) {
            INFO("negative " << i << ": " << verdictName(looked[0].probes[i].verdict));
            CHECK(isWrong(looked[0].probes[i].verdict));
        }
    }
}

TEST_CASE("Oracle (directedit 7:12, 15:46; hidden 6:47): edits the oracle's model does not describe are not modelled",
          "[naming][directedit][oracle-gate]")
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
    for (const Case& c : cases) {
        const Part pt = loadPart(kParts[c.part]);
        REQUIRE(pt.base != nullptr);
        SequenceOptions opt;
        opt.byIndex = c.byIndex;
        const SequenceResult r = runSequence(pt, c.part, c.seq, opt);
        INFO(kParts[c.part] << " #" << c.seq);
        CHECK_FALSE(r.skipped);
        const auto it = r.editCounts.find(c.outcome);
        CHECK((it != r.editCounts.end() && it->second >= 1));
        CHECK(r.tally.wrong() == 0);
    }
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
