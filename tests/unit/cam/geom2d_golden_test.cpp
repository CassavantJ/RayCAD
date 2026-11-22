// Golden files of geom2d's integer output (task T-050 acceptance: results identical across OS jobs). Each golden is
// tests/golden/geom2d/<name>.txt: the toText dump (integers only, sorted and canonical, '\n' line ends) of slices of
// the reference parts, offsets (the analytic cases and the rings of a real pocket down to nothing), booleans, open
// offsets and fitted polylines. They must be byte-identical on Windows (MSVC) and Linux (GCC). No floating-point
// value reaches a golden except through roundNm / toNm (round half to even).
//
// To make them again after a reviewed change: set RAY_UPDATE_GEOM2D_GOLDENS=1 and run ray_cam_geom2d_tests
// "[golden]", then read the diff (tools/update_golden.py compares a produced tree with tests/golden/geom2d too).
#include "geom2d_parts.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ray::cam::geom2d::test {
namespace {

std::string goldenPath(const std::string& name)
{
    return std::string(RAY_GEOM2D_GOLDEN_DIR) + "/" + name + ".txt";
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_GEOM2D_GOLDENS");
    return v != nullptr && std::string(v) == "1";
}

void matchesGolden(const std::string& name, const std::string& text)
{
    if (updating()) {
        std::ofstream out(goldenPath(name), std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(out.good());
    }
    std::ifstream in(goldenPath(name), std::ios::binary);
    REQUIRE(in.good());
    std::stringstream stored;
    stored << in.rdbuf();
    INFO("golden " << name << " differs; produced:\n" << text);
    CHECK(stored.str() == text);
}

const Xform kIdentity = Xform::identity();

} // namespace

TEST_CASE("golden: slices and silhouettes of the reference parts", "[geom2d][golden]")
{
    JobContext ctx;
    std::string text;
    const auto add = [&](const std::string& title, const Regions& rs) {
        text += "# " + title + "\n" + toText(rs);
    };
    add("block with holes, z 10", take(sliceRegions(blockWithHoles(), kIdentity, 10 * kMm, ctx)));
    const kernel::Shape pocket = pocketPart();
    add("pocket, z 25", take(sliceRegions(pocket, kIdentity, 25 * kMm, ctx)));
    add("pocket, z 20 (the floor)", take(sliceRegions(pocket, kIdentity, 20 * kMm, ctx)));
    add("pocket, z 19.9", take(sliceRegions(pocket, kIdentity, 19'900'000, ctx)));
    add("cone, z 15", take(sliceRegions(cone(), kIdentity, 15 * kMm, ctx)));
    add("sphere, z 6", take(sliceRegions(sphere(), kIdentity, 6 * kMm, ctx)));
    add("torus, z 3", take(sliceRegions(torus(), kIdentity, 3 * kMm, ctx)));
    add("drafted block, z 5", take(sliceRegions(draftedBlock(), kIdentity, 5 * kMm, ctx)));
    add("silhouette, block with holes", take(silhouetteRegions(blockWithHoles(), kIdentity, ctx)));
    add("silhouette, torus", take(silhouetteRegions(torus(), kIdentity, ctx)));
    // A turned frame: Z = model +X, origin at model x = 100.
    Xform turned{};
    const double m[12] = {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, -100};
    std::copy(std::begin(m), std::end(m), turned.m);
    add("block with holes, turned frame, z -75", take(sliceRegions(blockWithHoles(), turned, -75 * kMm, ctx)));
    matchesGolden("slices", text);
}

TEST_CASE("golden: offsets, pocket rings, booleans and open offsets", "[geom2d][golden]")
{
    JobContext ctx;
    std::string text;
    const auto add = [&](const std::string& title, const Regions& rs) {
        text += "# " + title + "\n" + toText(rs);
    };
    const Regions box{region(rect(0, 0, 100 * kMm, 60 * kMm))};
    add("rectangle +5", take(offset(box, 5 * kMm, ctx)));
    add("rectangle -5", take(offset(box, -5 * kMm, ctx)));
    add("slot -2", take(offset({region(slot(0, 30 * kMm, 0, 5 * kMm))}, -2 * kMm, ctx)));
    add("rounded rectangle -8",
        take(offset({region(roundedRect(10 * kMm, 10 * kMm, 90 * kMm, 70 * kMm, 6 * kMm))}, -8 * kMm, ctx)));
    const Regions pocketWithIsland{
        region(rect(0, 0, 100 * kMm, 100 * kMm), {reverse(rect(40 * kMm, 40 * kMm, 60 * kMm, 60 * kMm))})};
    add("island merging, -22", take(offset(pocketWithIsland, -22 * kMm, ctx)));
    // The rings a pocket operation makes: the real pocket's region (from its floor) shrunk by a 6.35 mm tool radius
    // plus 0.2 mm stock, then by a 4.1 mm stepover each time, until nothing is left.
    const kernel::Shape part = pocketPart();
    kernel::SubId floor{};
    for (std::uint32_t i = 0; i < part.count(kernel::SubKind::Face); ++i) {
        const kernel::SurfaceInfo s = take(kernel::surfaceInfo(part, {kernel::SubKind::Face, i}));
        if (s.type == kernel::SurfaceType::Plane && std::abs(s.centroid.z - 20) < 1e-9)
            floor = {kernel::SubKind::Face, i};
    }
    const Pocket pocket = take(pocketAboveFace(part, floor, kIdentity, ctx));
    Coord inset = 6'350'000 + 200'000;
    for (int ring = 0; ring < 50; ++ring, inset += 4'100'000) {
        const Regions r = take(offset({pocket.region}, -inset, ctx));
        if (r.empty())
            break;
        add("pocket ring " + std::to_string(ring) + ", -" + std::to_string(inset), r);
    }
    add("union of two circles", take(unite({region(Path::circle({0, 0}, 10 * kMm, true))},
                                           {region(Path::circle({15 * kMm, 0}, 10 * kMm, true))}, ctx)));
    add("plate minus a slot", take(subtract(box, {region(slot(20 * kMm, 80 * kMm, 30 * kMm, 7 * kMm))}, ctx)));
    const Path bend =
        openPath({0, 0}, {line({50 * kMm, 0}), arc({80 * kMm, 30 * kMm}, {50 * kMm, 30 * kMm}, 30 * kMm, true),
                          line({80 * kMm, 60 * kMm})});
    for (const Side side : {Side::Left, Side::Right})
        for (const Path& p : take(offsetOpen(bend, 4 * kMm, side, ctx)))
            text += std::string("# open offset ") + (side == Side::Left ? "left" : "right") + "\n" + toText(p);
    add("swept bend r 4", take(sweptArea(std::vector<Path>{bend}, 4 * kMm, ctx)));
    matchesGolden("offsets", text);
}

TEST_CASE("golden: fitted polylines", "[geom2d][golden]")
{
    std::string text;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        Rng rng{seed};
        std::vector<Pt> pts{{0, 0}};
        double heading = 0;
        for (int k = 0; k < 6; ++k) {
            const Pt from = pts.back();
            const double r = rng.between(2'000'000.0, 30'000'000.0);
            const double turn = rng.between(-2.0, 2.0);
            const double side = turn > 0 ? 1 : -1;
            const double cx = from.x - side * r * dmath::sin(heading), cy = from.y + side * r * dmath::cos(heading);
            const double a0 = dmath::atan2(from.y - cy, from.x - cx);
            for (int i = 1; i <= 40; ++i) {
                const double a = a0 + turn * i / 40;
                pts.push_back({static_cast<Coord>(std::llround(cx + r * dmath::cos(a))),
                               static_cast<Coord>(std::llround(cy + r * dmath::sin(a)))});
            }
            heading += turn;
            const double len = rng.between(1'000'000.0, 10'000'000.0);
            const Pt s = pts.back();
            for (int i = 1; i <= 5; ++i)
                pts.push_back({s.x + static_cast<Coord>(std::llround(len * i / 5 * dmath::cos(heading))),
                               s.y + static_cast<Coord>(std::llround(len * i / 5 * dmath::sin(heading)))});
        }
        // The input points are made with ray::dmath, so they are the same on every platform; they are part of the
        // golden too.
        std::string input = "# polyline " + std::to_string(seed) + "\n";
        for (const Pt& p : pts)
            input += std::to_string(p.x) + " " + std::to_string(p.y) + "\n";
        text += input + "# fitted\n" + toText(fitArcs(pts, false));
    }
    matchesGolden("fit", text);
}

} // namespace ray::cam::geom2d::test
