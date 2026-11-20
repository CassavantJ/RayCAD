// Randomised edit sequences on corpus parts (task T-081a acceptance): 1,000 judged sequences, 50 on each of 20
// parts of tests/corpus. Each sequence imports a part, adds 3 to 5 random features (cut, boss, fillet, chamfer),
// records every element the oracle can vouch for as a reference, makes one to three random edits to the script
// (move, resize or deepen a cut or boss, change a blend size, suppress a feature, insert a cut early, swap two
// features, or nothing), rebuilds, and judges every reference and every blend pick with the full oracle. A sequence
// that cannot be judged (no feature could be added, sampling failed) is replaced by the next seed of its part, so
// exactly 50 per part are judged; every replacement and every exclusion is printed by reason.
// Gates: zero wrong-element resolutions, every answer not judged and every new-territory acceptance and congruence
// conflict on the reviewed lists, no injectivity violation, every mutant caught, no usable Repaired, 1,000 judged.
// A holdout block is gated the same way except for the answers not judged: at most kUnjudgedBudget, and once the
// reviewed list has entries for the block, every one listed (checkGates).
//
// Seeds are fixed (part index and sequence index), the random numbers come from splitmix64 with our own
// conversions (no std distributions, whose output differs between standard libraries), so a run is reproducible.
// A failing sequence is printed with its part and index; RAYCAD_NAMING_ONLY=<part>:<sequence> runs just that one.
// The holdout block ([random-holdout]) uses other seeds, which were not run while the naming code and the oracle
// were developed; it is reported on its own. Its failures were looked at during the T-081a gate fixes, so a second
// block ([random-holdout2]) with fresh seeds was added then and first run after those fixes.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"

#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace ray;
using namespace ray::naming_oracle;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr int kSequencesPerPart = 50;
constexpr int kHoldoutPerPart = 10;

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

// splitmix64, with uniform doubles from the top 53 bits.
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

// A random closed profile centred on the origin of size about `s`: rectangle, circle, obround or rounded
// rectangle, counter-clockwise, stable tags.
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

// A cut or boss on a random planar face of the shape the samples describe.
std::optional<Feature> proposeSweep(const Samples& smp, double partSize, Rng& rng, const std::string& id, bool cut)
{
    std::vector<std::uint32_t> planar;
    const auto& faces = smp.of(SubKind::Face);
    for (std::uint32_t i = 0; i < faces.size(); ++i)
        if (faces[i].type == static_cast<int>(kernel::SurfaceType::Plane) && faces[i].pts.size() >= 3)
            planar.push_back(i);
    if (planar.empty())
        return std::nullopt;
    const ElemSamples& f = faces[planar[rng.below(planar.size())]];
    const Vec3 point = f.pts[rng.below(f.pts.size())];
    const Vec3 n = unit(f.nrm);
    // A canonical direction square to n, turned by a random angle about n.
    const Vec3 e = std::abs(n.x) <= std::abs(n.y) && std::abs(n.x) <= std::abs(n.z) ? Vec3{1, 0, 0}
                   : std::abs(n.y) <= std::abs(n.z)                                 ? Vec3{0, 1, 0}
                                                                                    : Vec3{0, 0, 1};
    const Vec3 x0 = unit(cross(e, n)), y0 = cross(n, x0);
    const double a = rng.uniform(0, kPi);
    Frame fr;
    fr.n = n;
    fr.xd = add(mul(x0, std::cos(a)), mul(y0, std::sin(a)));
    // The face plane through the sample point (samples of a planar face lie on it).
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

// Is the edge between faces described by owners a and b convex (material on the inside of the corner)? From the
// outward normal of a at the edge and the direction from the edge into b.
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

struct SequenceResult {
    Tally tally;
    JudgeLog log;
    std::string edit;
    int ops = 0;
    int refused = 0; // proposed features the kernel refused while authoring (dropped, another was proposed)
    long refs = 0;
    std::map<std::string, int> excluded; // baseline elements that are not references, by reason
    int replayFailures = 0;
    bool skipped = false;
    std::string skipReason;
};

struct SequenceOptions {
    std::uint64_t seedBase = 0x5EED0000ull;
    std::string keyPrefix;    // in the keys of the reviewed lists
    bool freshReplay = false; // replay on a fresh deserialised copy of the imported part (another session)
};

// One random edit of `edited` (the import, at 0, is never edited). Returns it in words.
std::string randomEdit(Script& edited, const Part& part, Rng& rng, int& inserted)
{
    std::vector<std::size_t> sweeps, blends;
    for (std::size_t i = 1; i < edited.size(); ++i)
        (edited[i].type == FType::Sweep ? sweeps : blends).push_back(i);
    const std::size_t roll = rng.below(100);
    if (roll < 35 && !sweeps.empty()) {
        Feature& f = edited[sweeps[rng.below(sweeps.size())]];
        const std::size_t kind = rng.below(4);
        if (kind == 0) {
            // Move by exactly its own width (spike S4 P23).
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
    if (roll < 50 && !blends.empty()) {
        Feature& f = edited[blends[rng.below(blends.size())]];
        f.size *= rng.uniform(0.5, 1.8);
        return "change size of " + f.id;
    }
    if (roll < 65 && edited.size() > 1) {
        const std::size_t i = 1 + rng.below(edited.size() - 1);
        edited[i].suppressed = true;
        return "suppress " + edited[i].id;
    }
    if (roll < 82) {
        auto f = proposeSweep(*part.samples, part.size, rng, "X" + std::to_string(++inserted), true);
        if (!f)
            return "no-op";
        const std::size_t at = 1 + rng.below(edited.size());
        edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(at), *f);
        return "insert cut " + f->id + " at " + std::to_string(at);
    }
    if (roll < 95 && edited.size() > 2) {
        // Swap two neighbouring features (a blend moved before the feature it blends fails, as it would in the
        // product).
        const std::size_t i = 1 + rng.below(edited.size() - 2);
        std::swap(edited[i], edited[i + 1]);
        return "swap " + edited[i + 1].id + " and " + edited[i].id;
    }
    return "no-op";
}

SequenceResult runSequence(const Part& part, int partIndex, int seq, const SequenceOptions& opt = {})
{
    SequenceResult out;
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
        const std::size_t roll = rng.below(100);
        std::optional<Feature> f;
        if (roll < 40)
            f = proposeSweep(**smp, part.size, rng, id, true);
        else if (roll < 60)
            f = proposeSweep(**smp, part.size, rng, id, false);
        else
            f = proposeBlend(builtOnly(script, b), **smp, rng, id, roll < 80);
        if (!f)
            continue;
        const int before = b.built();
        b.apply(*f);
        if (b.built() > before) {
            script.push_back(*f);
            ++out.ops;
        } else {
            ++out.refused;
        }
    }
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
    const auto owners0 = std::make_shared<const Owners>(buildOwners(builtOnly(script, b)));
    const std::vector<Ref> refs = recordReferences(b, owners0, *smp0, &out.excluded);
    out.refs = static_cast<long>(refs.size());

    // ---- one to three random edits ----
    Script edited = script;
    const std::size_t edits = rng.chance(0.35) ? 2 + rng.below(2) : 1;
    int inserted = 0;
    for (std::size_t k = 0; k < edits; ++k)
        out.edit += (k == 0 ? "" : "; ") + randomEdit(edited, part, rng, inserted);
    if (opt.freshReplay) {
        // Another session: the part read back from its serialised form, the references kept.
        auto copy = kernel::deserialize(kernel::serialize(edited[0].base->shape));
        auto own =
            copy ? analyseImport(*copy) : Result<std::shared_ptr<const ImportBase>>(tl::unexpected(copy.error()));
        if (!own) {
            out.skipped = true;
            out.skipReason = "the fresh copy of the part failed";
            return out;
        }
        edited[0].base = *own;
    }

    Builder replay(Mode::Replay, picks);
    for (const Feature& f : edited)
        replay.apply(f);
    out.replayFailures = static_cast<int>(replay.failed().size());
    auto smp1 = sampleShape(replay.shape());
    if (!smp1) {
        out.skipped = true;
        out.skipReason = "sampling the replay failed";
        return out;
    }
    const Owners owners1 = buildOwners(builtOnly(edited, replay));
    // One sequence on its own (RAYCAD_NAMING_ONLY) is being diagnosed: keep every line.
    out.log.maxWrongs = std::getenv("RAYCAD_NAMING_ONLY") != nullptr ? 100000 : 20;
    judgeAll(refs, replay, owners1, **smp1, out.tally, out.log, label);
    if ((!out.log.wrongs.empty() || !out.log.notes.empty()) && std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr) {
        auto describe = [](const Script& s) {
            std::string text;
            char buf[400];
            for (const Feature& f : s) {
                if (f.type == FType::Sweep)
                    std::snprintf(buf, sizeof buf,
                                  "    %s%s %s o=(%.3f %.3f %.3f) n=(%.3f %.3f %.3f) x=(%.3f %.3f %.3f) %zu curves "
                                  "width %.3f dist %.3f\n",
                                  f.id.c_str(), f.suppressed ? " (suppressed)" : "", f.op == BOp::Cut ? "cut" : "join",
                                  f.frame.o.x, f.frame.o.y, f.frame.o.z, f.frame.n.x, f.frame.n.y, f.frame.n.z,
                                  f.frame.xd.x, f.frame.xd.y, f.frame.xd.z, f.loops[0].ents.size(),
                                  profileWidth(f.loops[0]), f.dist);
                else if (f.type == FType::Import)
                    std::snprintf(buf, sizeof buf, "    %s import\n", f.id.c_str());
                else
                    std::snprintf(buf, sizeof buf, "    %s%s %s %s/%s %s size %.3f\n", f.id.c_str(),
                                  f.suppressed ? " (suppressed)" : "", f.role.c_str(), f.targets[0].a.c_str(),
                                  f.targets[0].b.c_str(), f.targets[0].convex ? "convex" : "concave", f.size);
                text += buf;
            }
            return text;
        };
        out.log.wrongs.insert(out.log.wrongs.begin(),
                              "  script:\n" + describe(script) + "  edited:\n" + describe(edited));
        if (std::getenv("RAYCAD_NAMING_ONLY") != nullptr)
            out.log.wrongs.push_back("  baseline edges:\n" + dumpElements(b, **smp0, SubKind::Edge) +
                                     "  replay edges:\n" + dumpElements(replay, **smp1, SubKind::Edge) +
                                     "  baseline faces:\n" + dumpElements(b, **smp0, SubKind::Face) +
                                     "  replay faces:\n" + dumpElements(replay, **smp1, SubKind::Face));
        for (const std::string& f : replay.failed())
            out.log.wrongs.push_back("  replay failure: " + f);
    }
    return out;
}

// Worker threads: up to 8, or RAYCAD_NAMING_WORKERS. The result does not depend on it.
unsigned workerCount()
{
    if (const char* n = std::getenv("RAYCAD_NAMING_WORKERS"))
        return static_cast<unsigned>(std::clamp(std::atoi(n), 1, 64));
    return std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
}

} // namespace

namespace {

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
    long judged = 0, replaced = 0, ops = 0, refused = 0, replayFailures = 0, refs = 0;
    std::vector<int> shortParts;          // parts that ran out of seeds before reaching their count
    std::map<std::string, long> skipped;  // replaced sequences by reason
    std::map<std::string, long> excluded; // baseline elements that are not references, by reason
    std::map<std::string, long> edits;    // edits by kind
};

// Runs `perPart` judged sequences of every part (only the RAYCAD_NAMING_ONLY one if set), replacing a sequence that
// cannot be judged by the next seed of its part (up to 4 x perPart seeds). Prints the table and every bucket.
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
    std::printf("\n%s (oracle: %s)\n| part | judged | replaced | features | refs judged | Exact %% | Split %% | "
                "Merged %% | Repaired %% | Broken %% | gone / missed | exists: E+S+M %% | WRONG | not judged |\n"
                "|-|-|-|-|-|-|-|-|-|-|-|-|-|-|\n",
                title, strictnessName().c_str());
    for (int p = 0; p < kPartCount; ++p) {
        Tally t;
        long n = 0, o = 0, replaced = 0;
        for (const auto& [job, res] : results) {
            if (job.first != p)
                continue;
            if (res.skipped) {
                ++replaced;
                ++r.skipped[res.skipReason.empty() ? "the copy of the part failed" : res.skipReason];
                continue;
            }
            ++n;
            o += res.ops;
            r.refused += res.refused;
            r.replayFailures += res.replayFailures;
            r.refs += res.refs;
            for (const auto& [why, k] : res.excluded)
                r.excluded[why] += k;
            // Each edit by kind (feature ids and positions left out), and the number of edits.
            std::size_t count = 0;
            for (std::size_t from = 0; from <= res.edit.size(); ++count) {
                std::size_t to = res.edit.find("; ", from);
                if (to == std::string::npos)
                    to = res.edit.size();
                std::string kind, word;
                for (std::size_t i = from; i <= to; ++i) {
                    if (i == to || res.edit[i] == ' ') {
                        const bool id = word.size() >= 2 && (word[0] == 'R' || word[0] == 'X') &&
                                        std::isdigit(static_cast<unsigned char>(word[1])) != 0;
                        const bool number = !word.empty() && std::isdigit(static_cast<unsigned char>(word[0])) != 0;
                        if (!word.empty() && !id && !number)
                            kind += (kind.empty() ? "" : " ") + word;
                        word.clear();
                    } else {
                        word += res.edit[i];
                    }
                }
                ++r.edits["edit: " + kind];
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
    // RAYCAD_NAMING_TRACE=<file>: one line per sequence; RAYCAD_NAMING_VERDICTS=<file>: one per judged reference.
    if (const char* trace = std::getenv("RAYCAD_NAMING_TRACE"))
        if (std::FILE* f = std::fopen(trace, "w")) {
            for (const auto& [job, res] : results)
                std::fprintf(f, "%d:%d ops %d refs %ld edit [%s] n %ld exact %ld broken %ld replayfail %d skipped %d\n",
                             job.first, job.second, res.ops, res.refs, res.edit.c_str(), res.tally.n,
                             res.tally.state[0], res.tally.state[4], res.replayFailures, res.skipped ? 1 : 0);
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
    std::printf("  %ld features (%ld proposals refused by the kernel while authoring, dropped), %ld feature failures "
                "on replay, %ld references recorded\n",
                r.ops, r.refused, r.replayFailures, r.refs);
    std::printf("  baseline elements that are not references:\n");
    for (const auto& [why, n] : r.excluded)
        std::printf("    %s: %ld\n", why.c_str(), n);
    std::printf("  edits:\n");
    for (const auto& [what, n] : r.edits)
        std::printf("    %s: %ld\n", what.c_str(), n);
    printTally(title, r.total);
    return r;
}

// The most answers a holdout block may leave NOT JUDGED (oracle.h: no anchor). Every holdout block's first, unseen
// run had at most 4 (holdout 1, holdout2 3, holdout3 4, holdout4 3: about 1 in 10,000 resolutions), before the
// sliding-crossing rule judged most of them; a block with more is not passed on review entries made after the fact.
constexpr long kUnjudgedBudget = 4;

// How many entries of the reviewed list (reviewed_unjudged.txt) belong to the block whose keys start with `prefix`
// ("holdout2 "; the tuned-on block's keys have no prefix and are not counted here).
long listedFor(const std::string& prefix)
{
    long n = 0;
    for (const auto& [key, why] : reviewedUnjudged())
        n += key.compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
    return n;
}

// `holdoutPrefix`: empty for the block the naming code was tuned on, where every answer not judged must be on the
// reviewed list. A holdout block (its key prefix) is first run unseen, when it cannot have reviewed entries: there
// WRONG must be 0 and the answers not judged are printed and counted, and fail the block when there are more than
// kUnjudgedBudget, or, once entries for the block are on the reviewed list, when one is not listed.
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
        const long listed = listedFor(holdoutPrefix);
        std::printf("%sblock: %ld answers not judged (budget %ld), %ld of them on the reviewed list, which lists %ld "
                    "for this block\n",
                    holdoutPrefix.c_str(), t.unjudged(), kUnjudgedBudget, t.unjudgedReviewed, listed);
        CHECK(t.unjudged() <= kUnjudgedBudget);
        if (listed > 0)
            CHECK(t.unjudged() == t.unjudgedReviewed);
    }
    CHECK(t.accepted == t.acceptedReviewed);
    CHECK(t.conflicts == t.conflictsReviewed);
    REQUIRE(t.wrong() == 0);
}

} // namespace

TEST_CASE("A randomised sequence gives the same result every time, serially and in parallel", "[naming][acceptance]")
{
    const Part part = loadPart(kParts[7]);
    REQUIRE(part.base != nullptr);
    auto summary = [](const SequenceResult& r) {
        std::string s = std::to_string(r.ops) + "/" + std::to_string(r.refs) + "/" + r.edit;
        for (const long n : r.tally.state)
            s += "/" + std::to_string(n);
        for (const long n : r.tally.verdict)
            s += "/" + std::to_string(n);
        return s;
    };
    const std::array<std::string, 2> first{summary(runSequence(part, 7, 3)), summary(runSequence(part, 7, 4))};
    CHECK(summary(runSequence(part, 7, 3)) == first[0]);
    std::vector<std::string> parallel(6);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < parallel.size(); ++t)
        threads.emplace_back([&, t] { parallel[t] = summary(runSequence(part, 7, 3 + static_cast<int>(t % 2))); });
    for (std::thread& t : threads)
        t.join();
    for (std::size_t t = 0; t < parallel.size(); ++t)
        CHECK(parallel[t] == first[t % 2]);
}

TEST_CASE("References replayed on a part read back in another session resolve as in the same session",
          "[random-session]")
{
    // One sequence per part, replayed once on the part as built and once on a fresh deserialised copy of it: the
    // imported part's names must come out the same, so every reference must get the same answer and verdict.
    const std::vector<Part> parts = loadParts();
    long refs = 0, wrong = 0;
    for (int p = 0; p < kPartCount; ++p) {
        INFO(kParts[p]);
        REQUIRE(parts[static_cast<std::size_t>(p)].base != nullptr);
        SequenceOptions fresh;
        fresh.freshReplay = true;
        const SequenceResult same = runSequence(parts[static_cast<std::size_t>(p)], p, 0);
        const SequenceResult other = runSequence(parts[static_cast<std::size_t>(p)], p, 0, fresh);
        CHECK(same.skipped == other.skipped);
        CHECK(same.edit == other.edit);
        CHECK(same.tally.state == other.tally.state);
        CHECK(same.tally.verdict == other.tally.verdict);
        refs += other.tally.n;
        wrong += other.tally.wrong();
        for (const std::string& w : other.log.wrongs)
            std::printf("  %s\n", w.c_str());
    }
    std::printf("Another session: %ld resolutions on fresh copies, WRONG %ld\n", refs, wrong);
    CHECK(wrong == 0);
}

TEST_CASE("1,000 randomised edit sequences on 20 corpus parts: no reference resolves to a wrong element", "[random]")
{
    const std::vector<Part> parts = loadParts();
    if (std::getenv("RAYCAD_NAMING_PROGRESS") != nullptr)
        for (const Part& p : parts)
            if (p.base)
                std::fprintf(stderr, "part %s %s\n", p.path.c_str(), p.base->shape.contentHash().hex().c_str());
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    // No OCCT call may fault: on Windows the kernel's guard counts the access violations it catches (a caught one
    // may leave OCCT state corrupt; off Windows it ends the process).
    const std::uint64_t guardBefore = kernel::testing::structuredExceptions();
    const BlockResult r = runBlock("Randomised", parts, kSequencesPerPart, SequenceOptions{});
    CHECK(kernel::testing::structuredExceptions() == guardBefore);
    checkGates(r, kSequencesPerPart);
}

TEST_CASE("Holdout: 200 randomised sequences on seeds not run during development", "[random-holdout]")
{
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    SequenceOptions holdout;
    holdout.seedBase = 0x401D0000ull;
    holdout.keyPrefix = "holdout ";
    // No OCCT call may fault (see the first test case).
    const std::uint64_t guardBefore = kernel::testing::structuredExceptions();
    const BlockResult r = runBlock("Holdout", parts, kHoldoutPerPart, holdout);
    CHECK(kernel::testing::structuredExceptions() == guardBefore);
    checkGates(r, kHoldoutPerPart, holdout.keyPrefix);
}

TEST_CASE("Second holdout: 200 randomised sequences on seeds first run after the T-081a gate fixes",
          "[random-holdout2]")
{
    // The first holdout block's failures were looked at while the naming code and the oracle were fixed (T-081a
    // gate); this block's seeds were not run until those fixes were done, so it is the unseen check.
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    SequenceOptions holdout;
    holdout.seedBase = 0x5EC0D000ull;
    holdout.keyPrefix = "holdout2 ";
    const BlockResult r = runBlock("Second holdout", parts, kHoldoutPerPart, holdout);
    checkGates(r, kHoldoutPerPart, holdout.keyPrefix);
}

TEST_CASE("Third holdout: 200 randomised sequences on seeds first run after the second holdout's findings were fixed",
          "[random-holdout3]")
{
    // The second holdout block's first run found two wrong answers; one was fixed (an edge named from a face that was
    // split since), so this block, with seeds not run before, is the unseen check of that fix.
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    SequenceOptions holdout;
    holdout.seedBase = 0x7A11D000ull;
    holdout.keyPrefix = "holdout3 ";
    const BlockResult r = runBlock("Third holdout", parts, kHoldoutPerPart, holdout);
    checkGates(r, kHoldoutPerPart, holdout.keyPrefix);
}

TEST_CASE("Fourth holdout: 200 randomised sequences on seeds first run after the pick-time evidence was added",
          "[random-holdout4]")
{
    // The second and third blocks' findings were fixed with the pick-time evidence in TopoRef (territories of the
    // picked element and its neighbours) and four oracle fixes; this block's seeds (disjoint from every other block's)
    // were run once, on the final code, unseen, on Windows and on Linux.
    const std::vector<Part> parts = loadParts();
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }
    SequenceOptions holdout;
    holdout.seedBase = 0x2B4D4000ull;
    holdout.keyPrefix = "holdout4 ";
    const BlockResult r = runBlock("Fourth holdout", parts, kHoldoutPerPart, holdout);
    checkGates(r, kHoldoutPerPart, holdout.keyPrefix);
}

// ---------------------------------------------------------------- oracle fixes on the corpus cases that showed them
// Each replays one sequence of a holdout block (deterministic: its seed is fixed) and checks the oracle's verdict on
// the references the fix is about, with a negative case: a wrong answer to the same reference is still judged wrong.
namespace {

struct Looked {
    std::string id;
    Answer answer = Answer::Broken;
    std::vector<std::uint32_t> elements;
    Judgement judgement;
    Judgement other;      // the oracle's verdict on the wrong answer below
    Judgement own;        // its verdict on its own expected answer (Exact, Merged when merged, Split when several)
    std::string crossing; // Expectation::crossing: how the sliding-crossing rule told E, if it did
    std::string noAnchor; // Expectation::noAnchor
    std::string reason;   // the resolver's reason
};

// Replays sequence `seq` of part `partIndex` of a block, and for the reference `id` judges also `wrong` (from its
// expectation and the resolver's answer).
std::vector<Looked> replayAndLook(int partIndex, int seq, std::uint64_t seedBase, const std::string& prefix,
                                  const std::vector<std::string>& ids,
                                  const std::function<std::pair<Answer, std::vector<std::uint32_t>>(
                                      const Expectation&, const std::vector<std::uint32_t>& answered)>& wrong)
{
    const Part part = loadPart(kParts[partIndex]);
    REQUIRE(part.base != nullptr);
    std::vector<Looked> out;
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
        l.reason = res.reason;
        const Expectation x = expect(ref.record, now, samples);
        l.crossing = x.crossing;
        l.noAnchor = x.noAnchor;
        const std::vector<std::uint32_t>& E = x.expected.elems;
        l.own = judgeAnswer(x, ref.record, now, samples,
                            E.size() > 1        ? Answer::Split
                            : x.expected.merged ? Answer::Merged
                                                : Answer::Exact,
                            E);
        const auto [answer, elements] = wrong(x, l.elements);
        l.other = judgeAnswer(x, ref.record, now, samples, answer, elements);
        out.push_back(std::move(l));
    });
    SequenceOptions opt;
    opt.seedBase = seedBase;
    opt.keyPrefix = prefix;
    const SequenceResult r = runSequence(part, partIndex, seq, opt);
    setInspector({});
    CHECK_FALSE(r.skipped);
    CHECK(r.tally.mutantsCaught() == r.tally.mutantsMade());
    return out;
}

} // namespace

TEST_CASE("Oracle (holdout3 17:1): a chamfer made larger lengthens the edges where it meets a face; the stretch it "
          "gains is new territory",
          "[naming][acceptance][oracle-gate]")
{
    // The chamfer R4 0.502 -> 0.712: edges 63 and 64 (chamfer face / import face I/i5) move with the chamfer plane and
    // get longer. Before the fix the oracle called them merged (any stretch of a moved element beyond its description
    // was), and Exact a silent merge; now the gained stretch is new territory: it lies on no baseline element where it
    // was, nor where the edit took one. Negative: the other edge is wrong.
    const std::string a = "holdout3 nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #1|edge 63";
    const std::string b = "holdout3 nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #1|edge 64";
    const std::vector<Looked> looked = replayAndLook(
        17, 1, 0x7A11D000ull, "holdout3 ", {a, b}, [](const Expectation& x, const std::vector<std::uint32_t>& got) {
            // The other one of the two chamfer end edges on I/i5 (the one not answered), Exact.
            for (const std::uint32_t e : x.logical)
                if (got.empty() || e != got.front())
                    return std::make_pair(Answer::Exact, std::vector<std::uint32_t>{e});
            return std::make_pair(Answer::Exact, std::vector<std::uint32_t>{});
        });
    REQUIRE(looked.size() == 2);
    for (const Looked& l : looked) {
        INFO(l.id << ": " << verdictName(l.judgement.verdict));
        CHECK(l.answer == Answer::Exact);
        CHECK(l.judgement.verdict == Verdict::Correct);
        CHECK(isWrong(l.other.verdict));
    }
}

TEST_CASE("Oracle (holdout3 16:1): where a separating cut is gone, the edge whole again continues its stub, merged",
          "[naming][acceptance][oracle-gate]")
{
    // The cut R5 moved off the part (it fails): the import edge it had split is whole again, and continues the lower
    // piece that was picked, merged. Before the fix the cut's frozen separator made the oracle expect the piece gone,
    // and the resolver's Merged a ghost. Negative: Exact on the whole edge is a silent merge.
    const std::string id = "holdout3 raycad-synthetic/thin_wall_box_1.stp #1|edge 5";
    const std::vector<Looked> looked = replayAndLook(
        16, 1, 0x7A11D000ull, "holdout3 ", {id},
        [](const Expectation&, const std::vector<std::uint32_t>& got) { return std::make_pair(Answer::Exact, got); });
    REQUIRE(looked.size() == 1);
    CHECK(looked[0].answer == Answer::Merged);
    CHECK(looked[0].judgement.verdict == Verdict::Correct);
    CHECK(looked[0].other.verdict == Verdict::WrongSilentMerge);
}

// ---------------------------------------------------------------- holdout4: the resolver rules and the oracle rule
TEST_CASE("Regression (holdout4 9:9): a face piece part of whose territory the piece beyond a moved slot took is not "
          "Exact",
          "[naming][acceptance][t081a-gate]")
{
    // manifold_block_1: the slot R3 moves past the boundary of the split piece of import face I/i14 picked beside it,
    // and the piece on the slot's far side takes a strip of what was picked. Names say Exact on the picked piece; the
    // oracle expects both pieces (merged). The pick-time territory shows a point of the picked piece on the other
    // piece now (exactAgainstEvidence): not Exact. Negative: Exact on the picked piece alone is incomplete.
    const std::string id = "holdout4 raycad-synthetic/manifold_block_1.stp #9|face 33";
    const std::vector<Looked> looked = replayAndLook(
        9, 9, 0x2B4D4000ull, "holdout4 ", {id}, [](const Expectation& x, const std::vector<std::uint32_t>&) {
            const std::vector<std::uint32_t>& E = x.expected.elems;
            return std::make_pair(Answer::Exact, std::vector<std::uint32_t>(E.end() - 1, E.end()));
        });
    REQUIRE(looked.size() == 1);
    INFO(looked[0].reason);
    CHECK(looked[0].answer == Answer::Broken);
    CHECK_FALSE(isWrong(looked[0].judgement.verdict));
    CHECK(looked[0].other.verdict == Verdict::WrongIncomplete);
}

TEST_CASE("Regression (holdout4 5:9): an edge that moved and runs on into another edge on the same surfaces is not "
          "Exact",
          "[naming][acceptance][t081a-gate]")
{
    // fixture_plate_1: the cut R4 moved by its width puts its wall on the plane of cut R5's start face (a sliver face
    // between them keeps them apart), and the edge where that wall meets the bottom runs on past a vertex as the edge
    // of R5's start face. Names say Exact on the first stretch; the oracle expects both. Not Exact
    // (exactAgainstEvidence: the moved answer runs on into an element on the same line and planes). Negative: Exact on
    // the first stretch alone is incomplete.
    const std::string id = "holdout4 raycad-synthetic/fixture_plate_1.stp #9|edge 156";
    const std::vector<Looked> looked = replayAndLook(
        5, 9, 0x2B4D4000ull, "holdout4 ", {id}, [](const Expectation& x, const std::vector<std::uint32_t>&) {
            const std::vector<std::uint32_t>& E = x.expected.elems;
            return std::make_pair(Answer::Exact, std::vector<std::uint32_t>(E.begin(), E.begin() + 1));
        });
    REQUIRE(looked.size() == 1);
    INFO(looked[0].reason);
    CHECK(looked[0].answer == Answer::Broken);
    CHECK_FALSE(isWrong(looked[0].judgement.verdict));
    CHECK(looked[0].other.verdict == Verdict::WrongIncomplete);
}

TEST_CASE("Oracle (holdout4 4:1): where a moved hole's seam meets a face, the one seam vertex continues the pick",
          "[naming][acceptance][oracle-gate]")
{
    // fillet_block_1: the hole R5 (slanted to the top face I/i1) moves by its width. Its seam meets the top face in one
    // vertex (on only those two faces) before and after; the map moves the picked vertex with the hole and projects it
    // onto the edited carriers, 1.3 mm from where the seam now meets the face (the vertex slides along the rim), nearer
    // to where the hole's other vertices on the top face at pick time (where its rim crossed the front face, gone now)
    // map to. The sliding-crossing rule (a) takes the one seam vertex. Negative: another vertex is wrong.
    const std::string id = "holdout4 raycad-synthetic/fillet_block_1.stp #1|vertex 15";
    const std::vector<Looked> looked = replayAndLook(
        4, 1, 0x2B4D4000ull, "holdout4 ", {id}, [](const Expectation& x, const std::vector<std::uint32_t>&) {
            const std::uint32_t e = x.expected.elems.empty() ? 0 : x.expected.elems.front();
            return std::make_pair(Answer::Exact, std::vector<std::uint32_t>{e == 0 ? 1u : e - 1});
        });
    REQUIRE(looked.size() == 1);
    INFO(looked[0].crossing << looked[0].noAnchor);
    CHECK(looked[0].crossing.compare(0, 5, "seam:") == 0);
    CHECK(looked[0].answer == Answer::Exact);
    CHECK(looked[0].judgement.verdict == Verdict::Correct);
    CHECK(isWrong(looked[0].other.verdict));
}

TEST_CASE("Oracle (holdout2 18:4): where a resized hole's wall crosses a face in two short arcs, each continues the "
          "one whose mapped place is clearly nearest",
          "[naming][acceptance][oracle-gate]")
{
    // 827-9999-905: the hole R4 is resized (radius x 1.32); its wall meets the import face I/i1 in two short arcs, one
    // on each side, before and after. The map puts each picked arc 0.15 mm from its continuation, which is not overlap
    // (the arcs slide along I/i1), so neither had an anchor. The sliding-crossing rule (b) takes, for each, the arc
    // nearest its mapped place: the other arc, and the other picked arc's mapped place, are 1.6 mm away, over twice as
    // far. Each expectation is judged correct as an answer; negative: each pick answered with the other arc is wrong.
    // (On Linux the kernel's arcs come out within the anchor tolerance of the mapped picks, so the ordinary anchor
    // finds them and the rule is not needed there: the rule is checked where it is used.)
    const std::string a = "holdout2 nist-d2mi/827-9999-905.stp #4|edge 24";
    const std::string b = "holdout2 nist-d2mi/827-9999-905.stp #4|edge 40";
    const std::vector<Looked> looked = replayAndLook(
        18, 4, 0x5EC0D000ull, "holdout2 ", {a, b}, [](const Expectation& x, const std::vector<std::uint32_t>&) {
            for (const std::uint32_t e : x.strict.elems)
                if (x.expected.elems.empty() || e != x.expected.elems.front())
                    return std::make_pair(Answer::Exact, std::vector<std::uint32_t>{e});
            return std::make_pair(Answer::Exact, std::vector<std::uint32_t>{});
        });
    REQUIRE(looked.size() == 2);
    for (const Looked& l : looked) {
        INFO(l.id << ": " << l.crossing << l.noAnchor);
        CHECK((l.crossing.empty() || l.crossing.compare(0, 8, "nearest:") == 0));
        CHECK(l.noAnchor.empty());
        CHECK(l.own.verdict == Verdict::Correct);
        CHECK(isWrong(l.other.verdict));
    }
}

TEST_CASE("Oracle (holdout3 17:0): where another picked piece's mapped place falls on the nearest crossing, the "
          "sliding-crossing rule does not judge",
          "[naming][acceptance][oracle-gate]")
{
    // nist_ftc_11: the round cut R1 moves by its width; its wall crossed the import face I/i4 in four short pieces at
    // pick time and crosses it in two now. The first pick's mapped place falls on one of the two, and so does another
    // picked piece's (it is claimed); the other one is farther from the first pick than that piece's mapped place.
    // No part is clearly the pick's: no anchor, and the resolver's answer is not judged.
    const std::string a = "holdout3 nist-pmi/nist_ftc_11_asme1_rb_geometry.stp #0|edge 5";
    const std::vector<Looked> looked = replayAndLook(
        17, 0, 0x7A11D000ull, "holdout3 ", {a},
        [](const Expectation&, const std::vector<std::uint32_t>& got) { return std::make_pair(Answer::Merged, got); });
    REQUIRE(looked.size() == 1);
    INFO(looked[0].crossing);
    CHECK(looked[0].crossing.empty());
    CHECK_FALSE(looked[0].noAnchor.empty());
    CHECK(looked[0].judgement.verdict == Verdict::Unjudged);
}
