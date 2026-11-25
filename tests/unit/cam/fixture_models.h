// The shipped fixture models (resources/library/fixtures/, task T-077), built in code with RayCAD's own kernel. This is
// their source: fixture_library_test.cpp checks that the shipped STEP files are exactly these shapes and, with the
// environment variable RAY_T077_WRITE_FIXTURES set to a folder, writes them there (resources/library/fixtures/README.md
// says how). Original, generic shapes drawn for RayCAD from round numbers of the 6 inch class (jaws 6 in wide and
// 1.75 in tall, parallels 1/8 x 1 x 6 in, and after T-077 1/8 x 1-1/4, 1-1/2 and 1-5/8 x 6 in): no vendor's model,
// drawing or dimension table was used.
//
// Frame of the vise (mm): X along the jaws, Y along the screw (the moving jaw opens towards +Y), Z up; the floor
// between the jaws (where parallels stand) is z = 0; the fixed jaw's clamping face is the plane y = 0 looking +Y,
// centred on x = 0. The moving jaw is a body of its own, its clamping face looking -Y, so the jaw opening can be set
// as the gap of a mate between the two clamping faces.
#pragma once

#include <ray/kernel.h>

#include <string>
#include <vector>

namespace ray::cam::test::fixtures {

inline constexpr double kIn = 25.4;
inline constexpr double kJawWidth = 6 * kIn;     // 152.4
inline constexpr double kJawHeight = 1.75 * kIn; // 44.45, above the floor
inline constexpr double kJawThickness = 60;      // the moving jaw, along Y
inline constexpr double kOpening = 2 * kIn;      // the moving jaw as shipped: 50.8 from the fixed jaw
inline constexpr double kBodyWidth = 160;        // the base, along X
inline constexpr double kBaseDepth = 100;        // the base below the floor
inline constexpr double kBackY = -60;            // the back of the fixed jaw and the base
inline constexpr double kFrontY = 330;           // the front end of the base, where the screw comes out
inline constexpr double kScrewRadius = 0.5 * kIn;
inline constexpr double kScrewLength = 90;
inline constexpr double kScrewZ = -50;
inline constexpr double kParallelLength = 6 * kIn;
inline constexpr double kParallelThickness = 0.125 * kIn;
inline constexpr double kParallelHeight = 1 * kIn;

struct NamedShape {
    std::string name;
    kernel::Shape shape;
};

inline kernel::Shape unite(const kernel::Shape& a, const kernel::Shape& b)
{
    JobContext ctx;
    auto r = kernel::boolean(kernel::BoolOp::Union, a, b, ctx);
    return r ? r->shape : kernel::Shape{};
}

// The vise: base, fixed jaw and screw as one body; the moving jaw at kOpening.
inline std::vector<NamedShape> vise()
{
    const double h = kJawWidth / 2;
    auto base = kernel::makeBox({-kBodyWidth / 2, kBackY, -kBaseDepth}, {kBodyWidth / 2, kFrontY, 0});
    auto fixed = kernel::makeBox({-h, kBackY, 0}, {h, 0, kJawHeight});
    auto screw = kernel::makeCylinder({0, kFrontY, kScrewZ}, {0, 1, 0}, kScrewRadius, kScrewLength);
    auto moving = kernel::makeBox({-h, kOpening, 0}, {h, kOpening + kJawThickness, kJawHeight});
    if (!base || !fixed || !screw || !moving) {
        return {};
    }
    const kernel::Shape body = unite(unite(*base, *fixed), *screw);
    return {{"Generic 6 in vise", body}, {"Generic 6 in vise moving jaw", *moving}};
}

// A height in eighths of an inch as a machinist writes it: 8 "1", 10 "1-1/4", 12 "1-1/2", 13 "1-5/8".
inline std::string inchesText(int eighths)
{
    int num = eighths % 8, den = 8;
    while (num != 0 && num % 2 == 0) {
        num /= 2;
        den /= 2;
    }
    const std::string whole = std::to_string(eighths / 8);
    return num == 0 ? whole : whole + "-" + std::to_string(num) + "/" + std::to_string(den);
}

// A pair of parallels, 1/8 in thick and 6 in long, `eighths` eighths of an inch tall, standing on the vise floor along
// each jaw: plain rectangular bars.
template <int Eighths>
std::vector<NamedShape> parallelsOf()
{
    const double h = kParallelLength / 2;
    const double height = Eighths * kIn / 8;
    auto a = kernel::makeBox({-h, 0, 0}, {h, kParallelThickness, height});
    auto b = kernel::makeBox({-h, kOpening - kParallelThickness, 0}, {h, kOpening, height});
    if (!a || !b) {
        return {};
    }
    const std::string name = "Generic parallel 1/8 x " + inchesText(Eighths) + " x 6 in ";
    return {{name + "A", *a}, {name + "B", *b}};
}

// A pair of parallels, 1/8 x 1 x 6 in.
inline std::vector<NamedShape> parallels()
{
    return parallelsOf<8>();
}

// After T-077: taller pairs, so thin stock can stand near the jaws' top. The heights are common shop steps (1/8 in
// apart in a parallel set) from 1 in up to the tallest whose top still lies at least 3 mm (ray_api/cam_fixtures.h
// kMinGripMm) below the jaws' 1-3/4 in: 1-1/4, 1-1/2 and 1-5/8 in (1-3/4 in would be level with the jaws' top and
// hold nothing).
struct ParallelsFile {
    const char* fileName; // the STEP file; its description is the same name with .json
    int eighths;          // the height in eighths of an inch
    std::vector<NamedShape> (*make)();
};
inline const ParallelsFile kParallelsFiles[] = {{"generic-parallels-1x6in.step", 8, &parallelsOf<8>},
                                                {"generic-parallels-1.25x6in.step", 10, &parallelsOf<10>},
                                                {"generic-parallels-1.5x6in.step", 12, &parallelsOf<12>},
                                                {"generic-parallels-1.625x6in.step", 13, &parallelsOf<13>}};

struct LibraryFile {
    std::string fileName;
    std::vector<NamedShape> (*make)();
};
inline const LibraryFile kLibraryFiles[] = {{"generic-vise-6in.step", &vise},
                                            {"generic-parallels-1x6in.step", &parallelsOf<8>},
                                            {"generic-parallels-1.25x6in.step", &parallelsOf<10>},
                                            {"generic-parallels-1.5x6in.step", &parallelsOf<12>},
                                            {"generic-parallels-1.625x6in.step", &parallelsOf<13>}};

} // namespace ray::cam::test::fixtures
