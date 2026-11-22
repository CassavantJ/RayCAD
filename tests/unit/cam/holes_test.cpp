// Hole recognition (task T-034) on the hand-labelled corpus parts (holes_labels.txt, written by holes_labels_gen.py,
// which says how each label was obtained): every labelled hole is found with its diameters, depths, kind and bottom,
// and nothing else is reported; from above, from below, along Y and in two tilted frames; imported STEP (OCCT and
// two NIST exporters, one of which splits every cylinder in two) and IGES; references that resolve; bit-identical
// results.
#include "holes_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

constexpr double kTol = 2e-6;      // mm: positions, diameters, heights against the labels
constexpr double kAngleTol = 1e-5; // degrees (the NIST files store 58.99999993 degrees for a 118 degree point)

struct LabelSegment {
    bool cone = false;
    double dTop = 0, dBottom = 0, zTop = 0, zBottom = 0, angle = 0;
};
struct LabelHole {
    double x = 0, y = 0, zTop = 0;
    std::string bottom, kind;
    std::vector<LabelSegment> segments;
    std::string line;
};
struct LabelPart {
    std::string id, frame;
    std::vector<LabelHole> holes;
};

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) {
        out.push_back(cur);
    }
    return out;
}

std::vector<LabelPart> readLabels()
{
    std::ifstream in(RAY_HOLES_LABELS);
    REQUIRE(in.good());
    std::vector<LabelPart> parts;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream s(line);
        std::string word;
        s >> word;
        if (word == "part") {
            LabelPart p;
            s >> p.id >> p.frame;
            parts.push_back(p);
            continue;
        }
        REQUIRE(word == "hole");
        REQUIRE(!parts.empty());
        LabelHole h;
        h.line = line;
        s >> h.x >> h.y >> h.zTop >> h.bottom >> h.kind;
        std::string seg;
        while (s >> seg) {
            const std::vector<std::string> f = split(seg, ':');
            LabelSegment g;
            g.cone = f.at(0) == "cone";
            if (g.cone) {
                REQUIRE(f.size() == 6);
                g.dTop = std::stod(f[1]);
                g.dBottom = std::stod(f[2]);
                g.zTop = std::stod(f[3]);
                g.zBottom = std::stod(f[4]);
                g.angle = std::stod(f[5]);
            } else {
                REQUIRE(f.size() == 4);
                g.dTop = g.dBottom = std::stod(f[1]);
                g.zTop = std::stod(f[2]);
                g.zBottom = std::stod(f[3]);
            }
            h.segments.push_back(g);
        }
        REQUIRE(!h.segments.empty());
        parts.back().holes.push_back(h);
    }
    return parts;
}

bool near(double a, double b, double tol = kTol)
{
    return std::abs(a - b) <= tol;
}

std::string kindOf(const RecognizedHole& h)
{
    std::string k;
    for (const auto& [on, word] : {std::pair{h.counterbored, "counterbored"}, std::pair{h.countersunk, "countersunk"},
                                   std::pair{h.chamfered, "chamfered"}}) {
        if (on) {
            k += (k.empty() ? "" : ",") + std::string(word);
        }
    }
    return k.empty() ? "plain" : k;
}

// Every value of the holes as bits (references left out): equal hashes are bit-identical results.
Hash256 hashOf(const std::vector<RecognizedHole>& holes)
{
    Hasher h;
    for (const RecognizedHole& r : holes) {
        h.i64(r.x).i64(r.y).i64(r.zTop).i64(r.zBottom).i64(static_cast<int>(r.bottom)).i64(r.through);
        h.i64(static_cast<std::int64_t>(r.bore)).i64(r.counterbored).i64(r.countersunk).i64(r.chamfered);
        h.f64(r.top.x).f64(r.top.y).f64(r.top.z).f64(r.axis.x).f64(r.axis.y).f64(r.axis.z);
        h.i64(r.axisSub.index).i64(static_cast<std::int64_t>(r.faces.size()));
        for (const kernel::SubId& f : r.faces) {
            h.i64(f.index);
        }
        for (const HoleSegment& s : r.segments) {
            h.f64(s.diameter).f64(s.bottomDiameter).f64(s.depth).f64(s.coneAngleDeg).i64(s.conical);
            h.i64(s.zTop).i64(s.zBottom);
        }
    }
    return h.finish();
}

// The part as the frame sees it: the shape (turned for a tilted frame) and wcsFromModel.
std::pair<kernel::Shape, Xform> placed(const kernel::Shape& shape, const std::string& frame)
{
    if (frame.rfind("tilt:", 0) != 0) {
        return {shape, axisFrame(frame)};
    }
    const Xform turn = frame == "tilt:a" ? rotation(unitOf(Vec3{1, 2, 3}), 0.7, Vec3{10, -20, 5})
                                         : rotation(unitOf(Vec3{-2, 1, 0.5}), 2.5, Vec3{-30, 15, 40});
    auto moved = kernel::transformed(shape, turn);
    REQUIRE(moved);
    return {moved->shape, inverse(turn)};
}

// Checks the holes found against the labels; returns the number matched. Every label must be matched by exactly one
// hole and every hole by a label.
std::size_t check(const LabelPart& part, const std::vector<RecognizedHole>& found)
{
    std::set<std::size_t> used;
    for (const RecognizedHole& h : found) {
        INFO("found " << describe(h));
        std::optional<std::size_t> match;
        for (std::size_t i = 0; i < part.holes.size(); ++i) {
            const LabelHole& l = part.holes[i];
            if (near(toMm(h.x), l.x) && near(toMm(h.y), l.y) && near(toMm(h.zTop), l.zTop)) {
                CHECK(!match); // two labels at one place
                match = i;
            }
        }
        if (!match) {
            FAIL_CHECK("a hole that is not labelled (a non-hole reported, or a label missing)");
            continue;
        }
        CHECK(used.insert(*match).second);
        const LabelHole& l = part.holes[*match];
        INFO("label " << l.line);
        CHECK(bottomWord(h.bottom) == l.bottom);
        CHECK(h.through == (l.bottom == "through"));
        CHECK(kindOf(h) == l.kind);
        REQUIRE(h.segments.size() == l.segments.size());
        for (std::size_t s = 0; s < l.segments.size(); ++s) {
            const HoleSegment& g = h.segments[s];
            const LabelSegment& e = l.segments[s];
            INFO("segment " << s);
            CHECK(g.conical == e.cone);
            CHECK(near(g.diameter, e.dTop));
            CHECK(near(g.bottomDiameter, e.dBottom));
            CHECK(near(toMm(g.zTop), e.zTop));
            CHECK(near(toMm(g.zBottom), e.zBottom));
            CHECK(near(g.depth, e.zTop - e.zBottom));
            CHECK(g.depth == toMm(g.zTop - g.zBottom));
            if (e.cone) {
                CHECK(near(g.coneAngleDeg, e.angle, kAngleTol));
            }
        }
        CHECK(h.zTop == h.segments.front().zTop);
        CHECK(h.zBottom == h.segments.back().zBottom);
        CHECK(!h.segments[h.bore].conical);
        CHECK(std::binary_search(h.faces.begin(), h.faces.end(), h.axisSub));
        CHECK(h.top.x == toMm(h.x));
        CHECK(h.top.z == toMm(h.zTop));
    }
    for (std::size_t i = 0; i < part.holes.size(); ++i) {
        if (used.count(i) == 0) {
            FAIL_CHECK("labelled hole not found: " << part.holes[i].line);
        }
    }
    return used.size();
}

std::map<std::string, kernel::Shape>& shapeCache()
{
    static std::map<std::string, kernel::Shape> cache;
    return cache;
}

const kernel::Shape& corpusShape(const std::string& id)
{
    auto& cache = shapeCache();
    auto it = cache.find(id);
    if (it == cache.end()) {
        it = cache.emplace(id, importCorpus(id)).first;
    }
    return it->second;
}

} // namespace

TEST_CASE("every labelled hole of the corpus parts is found with its sizes, and nothing else", "[cam][holes][corpus]")
{
    const std::vector<LabelPart> parts = readLabels();
    std::set<std::string> withHoles, imported;
    std::map<std::string, int> kinds;
    std::size_t labelled = 0, matched = 0;
    for (const LabelPart& part : parts) {
        INFO(part.id << " seen from " << part.frame);
        const auto [shape, frame] = placed(corpusShape(part.id), part.frame);
        // Through the document's naming of an imported body, as an operation would get its references.
        const BodyId body{Uuid::fromString("00000000-0000-4000-8000-000000000b01")};
        const FeatureId feature{Uuid::fromString("00000000-0000-4000-8000-000000000f01")};
        auto resolver = naming::makeResolver({naming::ResolverBody{body, naming::nameImported(feature, shape), shape}});
        JobContext ctx;
        auto found = recognizeHoles(shape, *resolver, body, frame, ctx);
        REQUIRE(found);
        labelled += part.holes.size();
        matched += check(part, *found);
        for (const RecognizedHole& h : *found) {
            // The reference names the bore wall and resolves to it.
            const naming::Resolved r = resolver->resolve(h.axisFace);
            CHECK(r.state == naming::ResolveState::Exact);
            CHECK(r.elements == std::vector<kernel::SubId>{h.axisSub});
        }
        for (const LabelHole& l : part.holes) {
            withHoles.insert(part.id);
            ++kinds[l.bottom];
            ++kinds[l.kind];
        }
        if (part.id.rfind("raycad-synthetic/", 0) != 0 || part.id.ends_with(".igs")) {
            imported.insert(part.id);
        }
    }
    std::cout << "hole labels: " << parts.size() << " part/frame pairs, " << withHoles.size() << " parts with holes, "
              << labelled << " labelled holes, " << matched << " found;";
    for (const auto& [k, n] : kinds) {
        std::cout << " " << k << " " << n;
    }
    std::cout << "\n";
    CHECK(matched == labelled);
    // The acceptance set: 20 parts or more with holes, every kind and bottom among the labels.
    CHECK(withHoles.size() >= 20);
    for (const char* k : {"plain", "counterbored", "countersunk", "chamfered", "through", "conical"}) {
        INFO(k);
        CHECK(kinds[k] > 0);
    }
    CHECK(imported.size() >= 3); // the NIST exporters and an IGES copy besides OCCT's STEP
}

TEST_CASE("hole recognition is bit-identical from run to run and matches the recorded hash", "[cam][holes][corpus]")
{
    // The same part twice gives the same bits; the hash of every value found on these parts is recorded, so a
    // platform that computes any of them differently fails here (PLAN.md section 8).
    Hasher all;
    for (const auto& [id, frame] : {std::pair{"raycad-synthetic/fixture_plate_2.stp", "z+"},
                                    std::pair{"raycad-synthetic/manifold_block_4.stp", "z+"},
                                    std::pair{"raycad-synthetic/plate_countersink_4.stp", "z+"},
                                    std::pair{"nist-pmi/nist_ftc_09_asme1_rd_geometry.stp", "y+"},
                                    std::pair{"raycad-synthetic/block_blind_holes_1.stp", "tilt:a"}}) {
        INFO(id << " " << frame);
        const auto [shape, xf] = placed(corpusShape(id), frame);
        JobContext ctx1, ctx2;
        auto a = recognizeHoles(shape, xf, ctx1);
        auto b = recognizeHoles(shape, xf, ctx2);
        REQUIRE(a);
        REQUIRE(b);
        const Hash256 ha = hashOf(*a);
        CHECK(ha == hashOf(*b));
        all.bytes(std::as_bytes(std::span(ha.bytes)));
    }
    const std::string hex = all.finish().hex();
    std::cout << "hole recognition hash " << hex << "\n";
    CHECK(hex == "865425ea0097ce3716e54665d5b8b2159c5df951bfa5d4ff0bc5283ebb26bce8");
}

// Not run by default: prints what recognizeHoles finds on the corpus file named by RAY_HOLES_DUMP (an id under
// tests/corpus/files) in the frames RAY_HOLES_FRAMES names ("z+,z-" when unset; see axisFrame), and every cylinder
// and cone face of it, for labelling parts by measurement.
TEST_CASE("dump the holes of a corpus part", "[.dump]")
{
    const char* id = std::getenv("RAY_HOLES_DUMP");
    if (id == nullptr) {
        SKIP("set RAY_HOLES_DUMP to a corpus id");
    }
    const kernel::Shape shape = importCorpus(id);
    const Box3 box = kernel::bounds(shape);
    std::cout.precision(12);
    std::cout << id << ": " << shape.count(kernel::SubKind::Face) << " faces, box (" << box.min.x << ", " << box.min.y
              << ", " << box.min.z << ") - (" << box.max.x << ", " << box.max.y << ", " << box.max.z << ")\n";
    for (std::uint32_t f = 0; f < shape.count(kernel::SubKind::Face); ++f) {
        auto s = kernel::surfaceInfo(shape, kernel::SubId{kernel::SubKind::Face, f});
        auto n = kernel::faceSample(shape, kernel::SubId{kernel::SubKind::Face, f});
        if (!s || !n || (s->type != kernel::SurfaceType::Cylinder && s->type != kernel::SurfaceType::Cone)) {
            continue;
        }
        std::cout << "  face " << f << (s->type == kernel::SurfaceType::Cone ? " cone" : " cyl") << " r " << s->radius
                  << " origin (" << s->origin.x << ", " << s->origin.y << ", " << s->origin.z << ") axis (" << s->axis.x
                  << ", " << s->axis.y << ", " << s->axis.z << ") semi " << s->semiAngle << " box (" << s->bounds.min.x
                  << ", " << s->bounds.min.y << ", " << s->bounds.min.z << ") - (" << s->bounds.max.x << ", "
                  << s->bounds.max.y << ", " << s->bounds.max.z << ") u span " << (n->uMax - n->uMin) << "\n";
    }
    const char* framesEnv = std::getenv("RAY_HOLES_FRAMES"); // e.g. "y+,y-"
    const std::string frames = framesEnv != nullptr ? framesEnv : "z+,z-";
    for (std::size_t at = 0; at < frames.size(); at += 3) {
        const std::string name = frames.substr(at, 2);
        JobContext ctx;
        auto holes = recognizeHoles(shape, axisFrame(name), ctx);
        REQUIRE(holes);
        std::cout << "frame " << name << ": " << holes->size() << " holes\n";
        for (const RecognizedHole& h : *holes) {
            std::cout << "  " << describe(h) << "\n";
        }
    }
}
