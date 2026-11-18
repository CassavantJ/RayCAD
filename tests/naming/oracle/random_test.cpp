// Randomised edit sequences on corpus parts (task T-081a acceptance): 1,000 sequences, 50 on each of 20 parts of
// tests/corpus. Each sequence imports a part, adds 3 to 5 random features (cut, boss, fillet, chamfer), records
// every element the oracle can vouch for as a reference, makes one random edit to the script (move, resize or
// deepen a cut or boss, change a blend size, suppress a feature, insert a cut early, or nothing), rebuilds, and
// judges every reference and every blend pick with the oracle. Gate: zero wrong-element resolutions.
//
// Seeds are fixed (part index and sequence index), the random numbers come from splitmix64 with our own
// conversions (no std distributions, whose output differs between standard libraries), so a run is reproducible.
// A failing sequence is printed with its part and index; RAYCAD_NAMING_ONLY=<part>:<sequence> runs just that one.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace ray;
using namespace ray::naming_oracle;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr int kSequencesPerPart = 50;

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
    std::vector<std::string> wrongs;
    std::string edit;
    int ops = 0;
    long refs = 0;
    int replayFailures = 0;
    bool skipped = false;
};

SequenceResult runSequence(const Part& part, int partIndex, int seq)
{
    SequenceResult out;
    Rng rng{0x5EED0000ull + static_cast<std::uint64_t>(partIndex) * 1000003ull + static_cast<std::uint64_t>(seq)};
    for (int i = 0; i < 4; ++i)
        rng.next();
    const std::string label = part.path + " #" + std::to_string(seq);

    Feature import;
    import.id = "I";
    import.type = FType::Import;
    import.role = "import";
    // Every sequence works on its own copy of the part (a BREP round trip makes new sub-shapes). Kernel operations
    // leave state on the sub-shapes they are given, and with one shared copy a sequence gave a different result
    // when another had run on the part before it (see the T-081a report), so the runs were not reproducible.
    {
        auto copy = kernel::deserialize(kernel::serialize(part.base->shape));
        auto own =
            copy ? analyseImport(*copy) : Result<std::shared_ptr<const ImportBase>>(tl::unexpected(copy.error()));
        if (!own) {
            out.skipped = true;
            return out;
        }
        import.base = *own;
    }
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
        }
    }
    if (out.ops == 0) {
        out.skipped = true;
        return out;
    }
    auto smp0 = sampleShape(b.shape());
    if (!smp0) {
        out.skipped = true;
        return out;
    }
    const Owners owners0 = buildOwners(builtOnly(script, b));
    const std::vector<Ref> refs = recordReferences(b, owners0, **smp0, nullptr);
    out.refs = static_cast<long>(refs.size());

    // ---- one random edit ----
    Script edited = script;
    std::vector<std::size_t> sweeps, blends;
    for (std::size_t i = 1; i < edited.size(); ++i)
        (edited[i].type == FType::Sweep ? sweeps : blends).push_back(i);
    const std::size_t roll = rng.below(100);
    if (roll < 40 && !sweeps.empty()) {
        Feature& f = edited[sweeps[rng.below(sweeps.size())]];
        const std::size_t kind = rng.below(4);
        if (kind == 0) {
            // Move by exactly its own width (spike S4 P23).
            f.frame.o = add(f.frame.o, mul(f.frame.xd, profileWidth(f.loops[0])));
            out.edit = "move " + f.id + " by its width";
        } else if (kind == 1) {
            const double a = rng.uniform(0, 2 * kPi), d = profileWidth(f.loops[0]) * rng.uniform(0.1, 1.2);
            f.frame.o = add(f.frame.o, add(mul(f.frame.xd, d * std::cos(a)), mul(f.frame.yd(), d * std::sin(a))));
            out.edit = "move " + f.id;
        } else if (kind == 2) {
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
            out.edit = "resize " + f.id;
        } else {
            f.dist *= rng.uniform(0.4, 2.0);
            out.edit = "change depth of " + f.id;
        }
    } else if (roll < 55 && !blends.empty()) {
        Feature& f = edited[blends[rng.below(blends.size())]];
        f.size *= rng.uniform(0.5, 1.8);
        out.edit = "change size of " + f.id;
    } else if (roll < 75) {
        const std::size_t i = 1 + rng.below(edited.size() - 1);
        edited[i].suppressed = true;
        out.edit = "suppress " + edited[i].id;
    } else if (roll < 95) {
        auto f = proposeSweep(*part.samples, part.size, rng, "X1", true);
        if (f) {
            const std::size_t at = 1 + rng.below(edited.size());
            edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(at), *f);
            out.edit = "insert a cut at " + std::to_string(at);
        } else {
            out.edit = "no-op";
        }
    } else {
        out.edit = "no-op";
    }

    Builder replay(Mode::Replay, picks);
    for (const Feature& f : edited)
        replay.apply(f);
    out.replayFailures = static_cast<int>(replay.failed().size());
    auto smp1 = sampleShape(replay.shape());
    if (!smp1) {
        out.skipped = true;
        return out;
    }
    const Owners owners1 = buildOwners(builtOnly(edited, replay));
    judgeAll(refs, replay, owners1, **smp1, **smp0, out.tally, out.wrongs, 20, label + " (" + out.edit + ")");
    if (!out.wrongs.empty() && std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr) {
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
        out.wrongs.insert(out.wrongs.begin(), "  script:\n" + describe(script) + "  edited:\n" + describe(edited));
        if (std::getenv("RAYCAD_NAMING_ONLY") != nullptr)
            out.wrongs.push_back("  baseline edges:\n" + dumpElements(b, **smp0, SubKind::Edge) + "  replay edges:\n" +
                                 dumpElements(replay, **smp1, SubKind::Edge));
        for (const std::string& f : replay.failed())
            out.wrongs.push_back("  replay failure: " + f);
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

TEST_CASE("A randomised sequence gives the same result every time, serially and in parallel", "[naming][acceptance]")
{
    const Part part = loadPart(kParts[7]);
    REQUIRE(part.base != nullptr);
    auto summary = [](const SequenceResult& r) {
        std::string s = std::to_string(r.ops) + "/" + std::to_string(r.refs) + "/" + r.edit;
        for (const long n : r.tally.state)
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

TEST_CASE("1,000 randomised edit sequences on 20 corpus parts: no reference resolves to a wrong element", "[random]")
{
    int onlyPart = -1, onlySeq = -1;
    if (const char* only = std::getenv("RAYCAD_NAMING_ONLY"))
        if (std::sscanf(only, "%d:%d", &onlyPart, &onlySeq) != 2)
            onlyPart = onlySeq = -1;

    std::vector<Part> parts(kPartCount);
    {
        std::atomic<int> next{0};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < workerCount(); ++t)
            threads.emplace_back([&] {
                for (int i = next++; i < kPartCount; i = next++)
                    parts[static_cast<std::size_t>(i)] = loadPart(kParts[i]);
            });
        for (std::thread& t : threads)
            t.join();
    }
    if (std::getenv("RAYCAD_NAMING_PROGRESS") != nullptr)
        for (const Part& p : parts)
            if (p.base)
                std::fprintf(stderr, "part %s %s\n", p.path.c_str(), p.base->shape.contentHash().hex().c_str());
    for (const Part& p : parts) {
        INFO(p.path << ": " << p.error);
        REQUIRE(p.base != nullptr);
    }

    struct Job {
        int part, seq;
    };
    std::vector<Job> jobs;
    for (int p = 0; p < kPartCount; ++p)
        for (int s = 0; s < kSequencesPerPart; ++s)
            if (onlyPart < 0 || (p == onlyPart && (s == onlySeq || onlySeq < 0)))
                jobs.push_back({p, s});
    std::vector<SequenceResult> results(jobs.size());
    std::atomic<std::size_t> next{0};
    std::mutex progress;
    std::size_t done = 0;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workerCount(); ++t)
        threads.emplace_back([&] {
            for (std::size_t i = next++; i < jobs.size(); i = next++) {
                if (std::getenv("RAYCAD_NAMING_PROGRESS") != nullptr)
                    std::fprintf(stderr, "sequence %d:%d\n", jobs[i].part, jobs[i].seq);
                results[i] = runSequence(parts[static_cast<std::size_t>(jobs[i].part)], jobs[i].part, jobs[i].seq);
                const std::lock_guard<std::mutex> lock(progress);
                if (++done % 100 == 0)
                    std::printf("  %zu of %zu sequences\n", done, jobs.size());
            }
        });
    for (std::thread& t : threads)
        t.join();

    Tally total;
    long sequences = 0, skipped = 0, ops = 0, replayFailures = 0;
    std::printf("\n| part | sequences | features | refs judged | Exact %% | Split %% | Merged %% | Repaired %% | "
                "Broken %% | gone / missed | WRONG |\n|-|-|-|-|-|-|-|-|-|-|-|\n");
    for (int p = 0; p < kPartCount; ++p) {
        Tally t;
        long n = 0, o = 0;
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].part != p)
                continue;
            const SequenceResult& r = results[i];
            if (r.skipped) {
                ++skipped;
                continue;
            }
            ++n;
            o += r.ops;
            replayFailures += r.replayFailures;
            t.merge(r.tally);
        }
        if (n == 0)
            continue;
        sequences += n;
        ops += o;
        total.merge(t);
        std::printf("| %s | %ld | %ld | %ld | %s | %s | %s | %s | %s | %ld / %ld | %ld |\n", kParts[p], n, o, t.n,
                    percent(t.state[0], t.n).c_str(), percent(t.state[1], t.n).c_str(),
                    percent(t.state[2], t.n).c_str(), percent(t.state[3], t.n).c_str(),
                    percent(t.state[4], t.n).c_str(), t.verdict[static_cast<std::size_t>(Verdict::BrokenGone)],
                    t.verdict[static_cast<std::size_t>(Verdict::Missed)], t.wrong());
    }
    for (const SequenceResult& r : results)
        for (const std::string& w : r.wrongs)
            std::printf("  %s\n", w.c_str());
    // RAYCAD_NAMING_TRACE=<file>: one line per sequence, to compare runs.
    if (const char* trace = std::getenv("RAYCAD_NAMING_TRACE"))
        if (std::FILE* f = std::fopen(trace, "w")) {
            for (std::size_t i = 0; i < jobs.size(); ++i) {
                const SequenceResult& r = results[i];
                std::fprintf(f, "%d:%d ops %d refs %ld edit [%s] n %ld exact %ld broken %ld replayfail %d\n",
                             jobs[i].part, jobs[i].seq, r.ops, r.refs, r.edit.c_str(), r.tally.n, r.tally.state[0],
                             r.tally.state[4], r.replayFailures);
            }
            std::fclose(f);
        }
    std::printf("\nRandomised: %ld sequences run (%ld skipped: no feature could be added), %ld features, %ld feature "
                "failures on replay; %ld resolutions (%ld weak-oracle): Exact %s%%, Split %s%%, Merged %s%%, Repaired "
                "%s%%, Broken %s%% (really gone %ld, missed %ld); among elements that still exist %s%% Exact, Split or "
                "Merged; WRONG %ld; oracle conflicts %ld; mutants caught %ld of %ld; usable Repaired %ld; accepted by "
                "the new-territory rule %ld; not judged (no mesh) %ld\n",
                sequences, skipped, ops, replayFailures, total.n, total.weak, percent(total.state[0], total.n).c_str(),
                percent(total.state[1], total.n).c_str(), percent(total.state[2], total.n).c_str(),
                percent(total.state[3], total.n).c_str(), percent(total.state[4], total.n).c_str(),
                total.verdict[static_cast<std::size_t>(Verdict::BrokenGone)],
                total.verdict[static_cast<std::size_t>(Verdict::Missed)],
                percent(total.existingOk, total.existing).c_str(), total.wrong(), total.conflicts, total.mutantsCaught,
                total.mutants, total.repairedUsable, total.newTerritory, total.unjudged);
    if (onlyPart < 0)
        CHECK(sequences >= 950);
    CHECK(total.mutantsCaught == total.mutants);
    CHECK(total.repairedUsable == 0);
    REQUIRE(total.wrong() == 0);
}
