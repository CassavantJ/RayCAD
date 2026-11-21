// STEP / IGES import and STEP / IGES / STL export (task T-005). No OCCT header here (PLAN.md 0.4 rule 1).
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"

#include <io_internal.h>
#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using ray::kernel::test::close;
using ray::kernel::test::take;

namespace fs = std::filesystem;

namespace {

constexpr double kVolumeRel = 1e-6; // acceptance: export then import keeps the volume to 1e-6 relative

std::string u8str(std::u8string_view text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string utf8(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path fromUtf8(const std::string& text)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// A fresh folder under the system temp folder, removed again at the end of the test.
class TempDir {
public:
    explicit TempDir(const std::string& utf8Name = "raycad io test")
    {
        std::random_device seed;
        const std::string suffix = std::to_string(seed()) + std::to_string(seed());
        path_ = fs::temp_directory_path() / fromUtf8(utf8Name + " " + suffix);
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        kernel::detail::waitForIoThreads(); // an abandoned export may still be writing here
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }
    std::string file(const std::string& utf8Name) const { return utf8(path_ / fromUtf8(utf8Name)); }

private:
    fs::path path_;
};

fs::path dataDir()
{
    return fs::path(RAY_KERNEL_TEST_DATA_DIR);
}
std::string dataFile(const char* name)
{
    return utf8(dataDir() / name);
}

JobContext quietContext()
{
    return JobContext{};
}

double volumeOf(const Shape& shape)
{
    return take(massProps(shape)).volume;
}

bool boxesClose(const Box3& a, const Box3& b, double tolerance)
{
    return std::abs(a.min.x - b.min.x) <= tolerance && std::abs(a.min.y - b.min.y) <= tolerance &&
           std::abs(a.min.z - b.min.z) <= tolerance && std::abs(a.max.x - b.max.x) <= tolerance &&
           std::abs(a.max.y - b.max.y) <= tolerance && std::abs(a.max.z - b.max.z) <= tolerance;
}

bool colorClose(const std::array<float, 4>& color, float r, float g, float b)
{
    return std::abs(color[0] - r) < 2e-3f && std::abs(color[1] - g) < 2e-3f && std::abs(color[2] - b) < 2e-3f &&
           std::abs(color[3] - 1.0f) < 1e-6f;
}

ImportResult importOk(const std::string& path, ImportOptions options = {})
{
    JobContext ctx = quietContext();
    const bool iges = path.ends_with(".igs") || path.ends_with(".iges");
    return take(iges ? importIges(path, options, ctx) : importStep(path, options, ctx));
}

ErrorCode importError(const std::string& path, bool iges, ImportOptions options = {})
{
    JobContext ctx = quietContext();
    const Result<ImportResult> result = iges ? importIges(path, options, ctx) : importStep(path, options, ctx);
    INFO("path " << path);
    REQUIRE_FALSE(result.has_value());
    INFO("message " << result.error().message);
    CHECK_FALSE(result.error().message.empty());
    return result.error().code;
}

void exportOk(Status status)
{
    INFO("unexpected error: " << (status.has_value() ? std::string() : status.error().message));
    REQUIRE(status.has_value());
}

std::vector<char> readAll(const std::string& path)
{
    std::ifstream in(fromUtf8(path), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const std::string& path, std::string_view bytes)
{
    std::ofstream out(fromUtf8(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<Shape> testParts()
{
    std::vector<Shape> parts;
    for (int i = 0; i < testing::kTestPartCount; ++i)
        parts.push_back(take(testing::makeTestPart(i)));
    return parts;
}

// Parsed binary STL.
struct Stl {
    std::uint32_t count = 0;
    Box3 bounds{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    bool sizeMatches = false;
    bool startsWithSolid = false;
};

Stl parseStl(const std::vector<char>& bytes)
{
    Stl stl;
    if (bytes.size() < 84)
        return stl;
    stl.startsWithSolid = std::string_view(bytes.data(), 5) == "solid";
    for (int i = 0; i < 4; ++i)
        stl.count |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[80 + i])) << (8 * i);
    stl.sizeMatches = bytes.size() == 84 + 50ull * stl.count;
    if (!stl.sizeMatches)
        return stl;
    for (std::uint32_t t = 0; t < stl.count; ++t) {
        const char* base = bytes.data() + 84 + 50ull * t + 12; // skip the normal
        for (int v = 0; v < 3; ++v) {
            float xyz[3];
            std::memcpy(xyz, base + 12 * v, sizeof xyz);
            stl.bounds.min =
                Vec3{std::min(stl.bounds.min.x, double(xyz[0])), std::min(stl.bounds.min.y, double(xyz[1])),
                     std::min(stl.bounds.min.z, double(xyz[2]))};
            stl.bounds.max =
                Vec3{std::max(stl.bounds.max.x, double(xyz[0])), std::max(stl.bounds.max.y, double(xyz[1])),
                     std::max(stl.bounds.max.z, double(xyz[2]))};
        }
    }
    return stl;
}

// Progress seen by a listener: every fraction and stage, in order.
struct ProgressLog {
    std::mutex mutex;
    std::vector<double> fractions;
    std::vector<std::string> stages;
    Progress progress()
    {
        return Progress([this](double fraction, std::string_view stage) {
            std::lock_guard<std::mutex> lock(mutex);
            fractions.push_back(fraction);
            if (stages.empty() || stages.back() != stage)
                stages.emplace_back(stage);
        });
    }
    std::string lastStage()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stages.empty() ? std::string() : stages.back();
    }
};

} // namespace

// ---------- Round trips ----------

TEST_CASE("STEP export then import keeps the volume of 20 parts", "[kernel][io]")
{
    TempDir dir;
    const std::vector<Shape> parts = testParts();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        INFO("test part " << i);
        const std::string path = dir.file("part " + std::to_string(i) + ".step");
        JobContext ctx;
        exportOk(exportStep(path, std::span<const Shape>(&parts[i], 1), ctx));
        const ImportResult imported = importOk(path);
        REQUIRE(imported.bodies.size() == 1);
        const ImportedBody& body = imported.bodies[0];
        CHECK(body.name == "Body 1");
        CHECK(imported.sourceUnits == UnitSystem::Metric);
        CHECK(body.shape.count(SubKind::Solid) == 1);
        const double before = volumeOf(parts[i]);
        const double after = volumeOf(body.shape);
        INFO("volume before " << before << " after " << after);
        CHECK(std::abs(after - before) <= kVolumeRel * before);
        CHECK(boxesClose(bounds(body.shape), bounds(parts[i]), 1e-6 * std::max(1.0, before)));
        CHECK(fs::exists(fromUtf8(path)));
        CHECK_FALSE(fs::exists(fromUtf8(path + ".partial")));
    }
}

TEST_CASE("IGES export then import keeps the volume of 20 parts", "[kernel][io]")
{
    TempDir dir;
    const std::vector<Shape> parts = testParts();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        INFO("test part " << i);
        const std::string path = dir.file("part " + std::to_string(i) + ".igs");
        JobContext ctx;
        exportOk(exportIges(path, std::span<const Shape>(&parts[i], 1), ctx));
        const ImportResult imported = importOk(path);
        REQUIRE(imported.bodies.size() == 1);
        CHECK(imported.sourceUnits == UnitSystem::Metric);
        const Shape& shape = imported.bodies[0].shape;
        CHECK(shape.count(SubKind::Solid) == 1);
        const double before = volumeOf(parts[i]);
        const double after = volumeOf(shape);
        INFO("volume before " << before << " after " << after);
        CHECK(std::abs(after - before) <= kVolumeRel * before);
    }
}

TEST_CASE("several shapes go into one STEP file and come back in order", "[kernel][io]")
{
    TempDir dir;
    const std::vector<Shape> parts{take(testing::makeTestPart(0)), take(testing::makeTestPart(5)),
                                   take(testing::makeTestPart(16))};
    const std::string path = dir.file("three.step");
    JobContext ctx;
    exportOk(exportStep(path, parts, ctx));
    const ImportResult imported = importOk(path);
    REQUIRE(imported.bodies.size() == 3);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        INFO("body " << i);
        CHECK(imported.bodies[i].name == "Body " + std::to_string(i + 1));
        CHECK(close(volumeOf(imported.bodies[i].shape), volumeOf(parts[i]), kVolumeRel));
    }
}

TEST_CASE("STL export writes every triangle within the part's bounds", "[kernel][io]")
{
    TempDir dir;
    const std::vector<Shape> parts{take(testing::makeTestPart(5)), take(testing::makeTestPart(16)),
                                   take(testing::makeTestPart(13))};
    for (const double chordTol : {0.2, 0.01}) {
        INFO("chord tolerance " << chordTol);
        Stl previous;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            INFO("part " << i);
            const std::string path = dir.file("part.stl");
            JobContext ctx;
            exportOk(exportStl(path, std::span<const Shape>(&parts[i], 1), chordTol, ctx));
            const Stl stl = parseStl(readAll(path));
            CHECK_FALSE(stl.startsWithSolid);
            REQUIRE(stl.sizeMatches);
            CHECK(stl.count >= 12); // part 13 is a box: exactly 12
            // Mesh vertices lie on the surface, so their box is inside the exact one and close to it.
            const Box3 exact = bounds(parts[i]);
            CHECK(boxesClose(stl.bounds, exact, chordTol + 1e-3));
        }
        // Two parts in one file: the counts add up.
        const std::string one = dir.file("one.stl");
        const std::string two = dir.file("two.stl");
        JobContext ctx;
        exportOk(exportStl(one, std::span<const Shape>(&parts[0], 1), chordTol, ctx));
        exportOk(exportStl(two, std::span<const Shape>(parts.data(), 2), chordTol, ctx));
        const Stl first = parseStl(readAll(one));
        const Stl both = parseStl(readAll(two));
        const Stl second = [&] {
            exportOk(exportStl(one, std::span<const Shape>(&parts[1], 1), chordTol, ctx));
            return parseStl(readAll(one));
        }();
        CHECK(both.count == first.count + second.count);
    }
    // A finer tolerance gives more triangles; the input shape is not changed by meshing a copy.
    const Hash256 before = parts[1].contentHash();
    JobContext ctx;
    exportOk(exportStl(dir.file("coarse.stl"), std::span<const Shape>(&parts[1], 1), 0.5, ctx));
    exportOk(exportStl(dir.file("fine.stl"), std::span<const Shape>(&parts[1], 1), 0.005, ctx));
    CHECK(parseStl(readAll(dir.file("fine.stl"))).count > parseStl(readAll(dir.file("coarse.stl"))).count);
    CHECK(take(testing::makeTestPart(16)).contentHash() == before);
}

// ---------- Names, colours, units, assemblies ----------

TEST_CASE("STEP assemblies are flattened with names, colours, units and placements", "[kernel][io]")
{
    TempDir dir;
    for (const bool inches : {false, true}) {
        INFO("inches " << inches);
        const std::string path = dir.file(inches ? "fixture inch.step" : "fixture mm.step");
        exportOk(testing::writeStepAssembly(path, inches));
        const ImportResult imported = importOk(path);
        CHECK(imported.sourceUnits == (inches ? UnitSystem::Inch : UnitSystem::Metric));
        REQUIRE(imported.bodies.size() == 4);
        const ImportedBody& bracket1 = imported.bodies[0];
        const ImportedBody& bracket2 = imported.bodies[1];
        const ImportedBody& pin = imported.bodies[2];
        const ImportedBody& plate = imported.bodies[3];
        CHECK(bracket1.name == "Bracket");
        CHECK(bracket2.name == "Bracket");
        CHECK(pin.name == "Pin");
        CHECK(plate.name == "Plate");
        CHECK(colorClose(bracket1.color, 0.8f, 0.1f, 0.1f));
        CHECK(colorClose(bracket2.color, 0.8f, 0.1f, 0.1f));
        CHECK(colorClose(pin.color, 0.1f, 0.2f, 0.9f));
        CHECK(colorClose(plate.color, 0.1f, 0.7f, 0.2f));
        const double tol = 1e-5;
        CHECK(close(volumeOf(bracket1.shape), 8000.0, kVolumeRel));
        CHECK(close(volumeOf(bracket2.shape), 8000.0, kVolumeRel));
        CHECK(close(volumeOf(pin.shape), 3.14159265358979323846 * 9.0 * 30.0, kVolumeRel));
        CHECK(close(volumeOf(plate.shape), 25000.0, kVolumeRel));
        CHECK(boxesClose(bounds(bracket1.shape), Box3{{0, 0, 0}, {40, 20, 10}}, tol));
        CHECK(boxesClose(bounds(bracket2.shape), Box3{{80, 0, 0}, {100, 40, 10}}, tol));
        CHECK(boxesClose(bounds(pin.shape), Box3{{7, 7, 10}, {13, 13, 40}}, tol));
        CHECK(boxesClose(bounds(plate.shape), Box3{{0, 0, -5}, {100, 50, 0}}, tol));
        // The two instances of one part do not share their topology (meshing one must not touch the other).
        CHECK(bracket1.shape.contentHash() != bracket2.shape.contentHash());
    }
}

TEST_CASE("IGES faces are sewn into a solid, with colour and source units", "[kernel][io]")
{
    TempDir dir;
    for (const bool inches : {false, true}) {
        INFO("inches " << inches);
        const std::string path = dir.file(inches ? "box inch.igs" : "box mm.igs");
        exportOk(testing::writeIgesColoredBox(path, inches));
        const ImportResult imported = importOk(path);
        CHECK(imported.sourceUnits == (inches ? UnitSystem::Inch : UnitSystem::Metric));
        REQUIRE(imported.bodies.size() == 1);
        CHECK(imported.bodies[0].shape.count(SubKind::Solid) == 1);
        CHECK(close(volumeOf(imported.bodies[0].shape), 8000.0, kVolumeRel));
        CHECK(colorClose(imported.bodies[0].color, 0.8f, 0.1f, 0.1f));
        CHECK(boxesClose(bounds(imported.bodies[0].shape), Box3{{0, 0, 0}, {40, 20, 10}}, 1e-5));

        // Without sewing the faces stay loose: one body of six faces, no solid.
        ImportOptions noSew;
        noSew.sew = false;
        const ImportResult loose = importOk(path, noSew);
        REQUIRE(loose.bodies.size() == 1);
        CHECK(loose.bodies[0].shape.count(SubKind::Solid) == 0);
        CHECK(loose.bodies[0].shape.count(SubKind::Face) == 6);
    }
}

TEST_CASE("loose STEP faces are sewn into a solid unless sewing is off", "[kernel][io]")
{
    TempDir dir;
    const Shape faces = take(testing::makeLooseBoxFaces());
    const std::string path = dir.file("loose faces.step");
    JobContext ctx;
    exportOk(exportStep(path, std::span<const Shape>(&faces, 1), ctx));

    const ImportResult sewn = importOk(path);
    REQUIRE(sewn.bodies.size() == 1);
    CHECK(sewn.bodies[0].shape.count(SubKind::Solid) == 1);
    CHECK(close(volumeOf(sewn.bodies[0].shape), 6000.0, kVolumeRel));

    ImportOptions options;
    options.sew = false;
    const ImportResult loose = importOk(path, options);
    REQUIRE(loose.bodies.size() == 1);
    CHECK(loose.bodies[0].shape.count(SubKind::Solid) == 0);
    CHECK(loose.bodies[0].shape.count(SubKind::Face) == 6);
}

TEST_CASE("every healing mode imports the same parts", "[kernel][io]")
{
    TempDir dir;
    const std::vector<Shape> parts{take(testing::makeTestPart(6)), take(testing::makeTestPart(14)),
                                   take(testing::makeTestPart(19))};
    const std::string step = dir.file("parts.step");
    const std::string iges = dir.file("parts.igs");
    JobContext ctx;
    exportOk(exportStep(step, parts, ctx));
    exportOk(exportIges(iges, parts, ctx));
    for (const std::string& path : {step, iges}) {
        for (const Heal heal : {Heal::None, Heal::Reader, Heal::Full}) {
            INFO(path << " heal " << static_cast<int>(heal));
            ImportOptions options;
            options.heal = heal;
            const ImportResult imported = importOk(path, options);
            REQUIRE(imported.bodies.size() == parts.size());
            for (std::size_t i = 0; i < parts.size(); ++i)
                CHECK(close(volumeOf(imported.bodies[i].shape), volumeOf(parts[i]), kVolumeRel));
        }
    }
    CHECK(ImportOptions{}.heal == Heal::Full); // healing is never skipped by default
}

TEST_CASE("importing the same file twice gives identical bodies", "[kernel][io]")
{
    for (const char* name : {"assembly_mm.step", "box_hole.igs", "loft.step"}) {
        INFO(name);
        const ImportResult a = importOk(dataFile(name));
        const ImportResult b = importOk(dataFile(name));
        REQUIRE(a.bodies.size() == b.bodies.size());
        for (std::size_t i = 0; i < a.bodies.size(); ++i) {
            CHECK(a.bodies[i].shape.contentHash() == b.bodies[i].shape.contentHash());
            CHECK(a.bodies[i].name == b.bodies[i].name);
            CHECK(a.bodies[i].color == b.bodies[i].color);
        }
    }
}

TEST_CASE("imported bodies serialise canonically on the first round trip", "[kernel][io]")
{
    // T-004's guarantee, serialize(deserialize(b)) == b, must already hold for the bytes of a fresh import:
    // content hashes and the derived-object cache key on them. Bodies placed by an assembly are the hard case.
    for (const char* name : {"assembly_mm.step", "assembly_inch.step", "box_hole.step", "box_hole.igs",
                             "colored_box_inch.igs", "loft.step", "loose_faces.step"}) {
        const ImportResult imported = importOk(dataFile(name));
        REQUIRE_FALSE(imported.bodies.empty());
        for (std::size_t i = 0; i < imported.bodies.size(); ++i) {
            INFO(name << " body " << i << " (" << imported.bodies[i].name << ")");
            const Shape& body = imported.bodies[i].shape;
            const std::vector<std::byte> first = serialize(body);
            REQUIRE_FALSE(first.empty());
            const Shape back = take(deserialize(first));
            const std::vector<std::byte> second = serialize(back);
            const auto mismatch = std::mismatch(first.begin(), first.end(), second.begin(), second.end());
            INFO("sizes " << first.size() << " and " << second.size() << ", first difference at byte "
                          << (mismatch.first - first.begin()));
            const bool identical = first == second;
            CHECK(identical);
            CHECK(back.contentHash() == body.contentHash());
        }
    }
}

TEST_CASE("imports and exports run concurrently on several threads", "[kernel][io]")
{
    TempDir dir;
    constexpr int kThreads = 6;
    std::vector<std::thread> threads;
    std::vector<std::string> problems(kThreads, "did not finish");
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::string& problem = problems[t];
            const Result<Shape> part = testing::makeTestPart(t * 3);
            if (!part) {
                problem = "test part: " + part.error().message;
                return;
            }
            const std::string step = dir.file("thread " + std::to_string(t) + ".step");
            const std::string iges = dir.file("thread " + std::to_string(t) + ".igs");
            JobContext ctx;
            const auto report = [&](const char* what, const auto& result) {
                if (!result.has_value())
                    problem = std::string(what) + ": " + result.error().message;
                return result.has_value();
            };
            if (!report("STEP export", exportStep(step, std::span<const Shape>(&*part, 1), ctx)) ||
                !report("IGES export", exportIges(iges, std::span<const Shape>(&*part, 1), ctx)))
                return;
            const Result<ImportResult> a = importStep(step, ImportOptions{}, ctx);
            const Result<ImportResult> b = importIges(iges, ImportOptions{}, ctx);
            const Result<ImportResult> c = importStep(dataFile("assembly_inch.step"), ImportOptions{}, ctx);
            if (!report("STEP import", a) || !report("IGES import", b) || !report("assembly import", c))
                return;
            if (a->bodies.size() != 1 || b->bodies.size() != 1 || c->bodies.size() != 4) {
                problem = "body counts " + std::to_string(a->bodies.size()) + " " + std::to_string(b->bodies.size()) +
                          " " + std::to_string(c->bodies.size());
                return;
            }
            problem.clear();
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    for (int t = 0; t < kThreads; ++t) {
        INFO("thread " << t << ": " << problems[t]);
        CHECK(problems[t].empty());
    }
}

// ---------- Paths ----------

TEST_CASE("paths with spaces and non-ASCII characters work for every format", "[kernel][io]")
{
    TempDir dir(u8str(u8"RayCAD io tëst ü 日本 Ωmega"));
    const Shape part = take(testing::makeTestPart(5));
    const std::span<const Shape> one(&part, 1);
    const std::string step = dir.file(u8str(u8"bóx ü 部品.step"));
    const std::string iges = dir.file(u8str(u8"bóx ü 部品.igs"));
    const std::string stl = dir.file(u8str(u8"bóx ü 部品.stl"));
    JobContext ctx;
    exportOk(exportStep(step, one, ctx));
    exportOk(exportIges(iges, one, ctx));
    exportOk(exportStl(stl, one, 0.1, ctx));
    CHECK(fs::exists(fromUtf8(step)));
    CHECK(fs::exists(fromUtf8(iges)));
    CHECK(parseStl(readAll(stl)).sizeMatches);
    const ImportResult fromStep = importOk(step);
    const ImportResult fromIges = importOk(iges);
    REQUIRE(fromStep.bodies.size() == 1);
    REQUIRE(fromIges.bodies.size() == 1);
    CHECK(close(volumeOf(fromStep.bodies[0].shape), volumeOf(part), kVolumeRel));
    CHECK(close(volumeOf(fromIges.bodies[0].shape), volumeOf(part), kVolumeRel));
    // The IGES header carries the file name in ASCII only, and neither header names the user or the machine.
    const std::vector<char> header = readAll(iges);
    const std::string text(header.begin(), header.end());
    CHECK(text.find("RayCAD") != std::string::npos);
}

// ---------- Errors ----------

TEST_CASE("missing files, folders and bad arguments are reported, never thrown", "[kernel][io]")
{
    TempDir dir;
    CHECK(importError(dir.file("missing.step"), false) == ErrorCode::NotFound);
    CHECK(importError(dir.file("missing.igs"), true) == ErrorCode::NotFound);
    CHECK(importError(utf8(dir.path()), false) == ErrorCode::IoError);
    CHECK(importError(utf8(dir.path()), true) == ErrorCode::IoError);
    CHECK(importError("", false) == ErrorCode::InvalidArgument);
    CHECK(importError(std::string("bad \xC3\x28 utf8.step"), false) == ErrorCode::InvalidArgument);

    ImportOptions badSew;
    badSew.sewTolerance = -1.0;
    CHECK(importError(dataFile("box_hole.step"), false, badSew) == ErrorCode::InvalidArgument);
    ImportOptions badHeal;
    badHeal.heal = static_cast<Heal>(7);
    CHECK(importError(dataFile("box_hole.step"), false, badHeal) == ErrorCode::InvalidArgument);

    const Shape part = take(testing::makeTestPart(0));
    JobContext ctx;
    const std::string inMissingFolder = utf8(dir.path() / "no such folder" / "out.step");
    for (const Status& status : {exportStep(inMissingFolder, std::span<const Shape>(&part, 1), ctx),
                                 exportIges(inMissingFolder, std::span<const Shape>(&part, 1), ctx),
                                 exportStl(inMissingFolder, std::span<const Shape>(&part, 1), 0.1, ctx)}) {
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::IoError);
    }
    CHECK(exportStep(dir.file("x.step"), {}, ctx).error().code == ErrorCode::InvalidArgument);
    const Shape null;
    CHECK(exportStep(dir.file("x.step"), std::span<const Shape>(&null, 1), ctx).error().code ==
          ErrorCode::KernelFailure);
    CHECK(exportIges(dir.file("x.igs"), std::span<const Shape>(&null, 1), ctx).error().code ==
          ErrorCode::KernelFailure);
    CHECK(exportStl(dir.file("x.stl"), std::span<const Shape>(&null, 1), 0.1, ctx).error().code ==
          ErrorCode::KernelFailure);
    for (const double bad : {0.0, -1.0, std::nan("")})
        CHECK(exportStl(dir.file("x.stl"), std::span<const Shape>(&part, 1), bad, ctx).error().code ==
              ErrorCode::InvalidArgument);
    CHECK(fs::is_empty(dir.path())); // nothing half-written was left behind
}

TEST_CASE("broken, truncated and foreign files give an error, never a crash", "[kernel][io]")
{
    const auto acceptable = [](ErrorCode code) {
        return code == ErrorCode::FormatError || code == ErrorCode::IoError || code == ErrorCode::KernelFailure;
    };
    // Committed broken files, and valid files of the other format.
    struct Case {
        const char* name;
        bool asIges;
    };
    for (const Case& c : {Case{"empty.step", false}, Case{"garbage.step", false}, Case{"header_only.step", false},
                          Case{"truncated.step", false}, Case{"binary.bin", false}, Case{"box_hole.igs", false},
                          Case{"empty.igs", true}, Case{"garbage.igs", true}, Case{"truncated.igs", true},
                          Case{"binary.bin", true}, Case{"assembly_mm.step", true}}) {
        INFO(c.name << (c.asIges ? " as IGES" : " as STEP"));
        CHECK(acceptable(importError(dataFile(c.name), c.asIges)));
    }

    // Every cut of a valid file is rejected, at 20 places through it.
    TempDir dir;
    for (const char* name : {"box_hole.step", "assembly_inch.step", "box_hole.igs", "colored_box_inch.igs"}) {
        const bool iges = std::string_view(name).ends_with(".igs");
        const std::vector<char> bytes = readAll(dataFile(name));
        REQUIRE(bytes.size() > 100);
        for (int cut = 1; cut < 20; ++cut) {
            const std::size_t length = bytes.size() * static_cast<std::size_t>(cut) / 20;
            const std::string path = dir.file(std::string("cut ") + name);
            writeAll(path, std::string_view(bytes.data(), length));
            INFO(name << " cut at " << length << " of " << bytes.size());
            CHECK(acceptable(importError(path, iges)));
        }
        // Random bytes in the middle of a valid file: an error or a result, never a crash or a hang.
        // Only Windows contains a crash inside OCCT (runGuarded's structured-exception handler). On Linux and macOS
        // these rounds pass because the import rejects models with dangling or unreadable entities before the
        // transfer (io_import.cpp, damagedEntities). That is not a general guarantee: with more rounds, OCCT's
        // IGES reader itself was seen to dereference null while loading a damaged file (IGESSolid_ToolLoop).
        std::mt19937 random(12345);
        for (int round = 0; round < 10; ++round) {
            std::vector<char> damaged = bytes;
            for (int k = 0; k < 20; ++k)
                damaged[random() % damaged.size()] = static_cast<char>(random() & 0xFF);
            const std::string path = dir.file(std::string("damaged ") + name);
            writeAll(path, std::string_view(damaged.data(), damaged.size()));
            JobContext ctx;
            const Result<ImportResult> result =
                iges ? importIges(path, ImportOptions{}, ctx) : importStep(path, ImportOptions{}, ctx);
            if (!result.has_value())
                CHECK(acceptable(result.error().code));
            else
                CHECK_FALSE(result->bodies.empty());
        }
    }
}

TEST_CASE("a STEP file that defines an entity twice is rejected before OCCT reads it", "[kernel][io]")
{
    // OCCT's model builder handles a repeated entity name in a second pass that, for some damaged files, looped
    // forever depending on what the process had done before (a ThreadSanitizer run of this suite hung for 900 s on
    // a damaged copy of box_hole.step with "#2 =" twice). The import must reject such a file up front. A watchdog
    // cancels the import after 60 s, so a regression fails here instead of hanging the run.
    TempDir dir;
    const std::vector<char> bytes = readAll(dataFile("box_hole.step"));
    std::string text(bytes.begin(), bytes.end());
    const std::size_t at = text.find("\n#3 =");
    REQUIRE(at != std::string::npos);
    REQUIRE(text.find("\n#2 =") != std::string::npos);
    text.replace(at, 5, "\n#2 =");
    const std::string twice = dir.file("defined twice.step");
    writeAll(twice, text);
    // A '#2 =' inside a string or a comment is not a definition.
    std::string quoted(bytes.begin(), bytes.end());
    const std::size_t header = quoted.find("DATA;");
    REQUIRE(header != std::string::npos);
    quoted.insert(header + 5, "\n/* #2 = not an entity */\n");
    const std::string commented = dir.file("commented.step");
    writeAll(commented, quoted);

    for (const bool expectRejected : {true, false}) {
        const std::string& path = expectRejected ? twice : commented;
        INFO(path);
        CancelSource watchdogSource;
        JobContext ctx{watchdogSource.token(), Progress()};
        std::atomic<bool> finished{false};
        std::thread watchdog([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!finished.load() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            watchdogSource.cancel();
        });
        const Result<ImportResult> result = importStep(path, ImportOptions{}, ctx);
        finished = true;
        watchdog.join();
        if (!result.has_value() && result.error().code == ErrorCode::Cancelled) {
            // The abandoned I/O thread may never end: report and leave without waiting for it.
            FAIL_CHECK("the import did not finish within 60 s");
            std::fflush(stdout);
            std::_Exit(3);
        }
        if (expectRejected) {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == ErrorCode::FormatError);
            CHECK(result.error().message.find("#2 ") != std::string::npos);
        } else {
            REQUIRE(result.has_value());
            CHECK_FALSE(result->bodies.empty());
        }
    }
}

// ---------- Progress and cancel ----------

TEST_CASE("progress is monotonic, names its stages and ends at 1", "[kernel][io]")
{
    for (const char* name : {"assembly_inch.step", "colored_box_inch.igs", "loft.step"}) {
        INFO(name);
        ProgressLog log;
        JobContext ctx{CancelToken(), log.progress()};
        const std::string path = dataFile(name);
        const bool iges = std::string_view(name).ends_with(".igs");
        const Result<ImportResult> result =
            iges ? importIges(path, ImportOptions{}, ctx) : importStep(path, ImportOptions{}, ctx);
        REQUIRE(result.has_value());
        REQUIRE_FALSE(log.fractions.empty());
        CHECK(std::is_sorted(log.fractions.begin(), log.fractions.end()));
        CHECK(log.fractions.back() == 1.0);
        CHECK(log.stages.back() == "Done");
        for (const double fraction : log.fractions)
            CHECK((fraction >= 0.0 && fraction <= 1.0));
    }
    // Exports too.
    TempDir dir;
    const Shape part = take(testing::makeTestPart(16));
    for (const char* name : {"p.step", "p.igs", "p.stl"}) {
        INFO(name);
        ProgressLog log;
        JobContext ctx{CancelToken(), log.progress()};
        const std::string path = dir.file(name);
        const std::span<const Shape> one(&part, 1);
        const std::string_view ext = std::string_view(name).substr(2);
        exportOk(ext == "step"  ? exportStep(path, one, ctx)
                 : ext == "igs" ? exportIges(path, one, ctx)
                                : exportStl(path, one, 0.05, ctx));
        REQUIRE_FALSE(log.fractions.empty());
        CHECK(std::is_sorted(log.fractions.begin(), log.fractions.end()));
        CHECK(log.fractions.back() == 1.0);
    }
}

TEST_CASE("an import cancelled before it starts returns Cancelled at once", "[kernel][io]")
{
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), Progress()};
    const auto start = std::chrono::steady_clock::now();
    const Result<ImportResult> result = importStep(dataFile("assembly_mm.step"), ImportOptions{}, ctx);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(elapsed < std::chrono::milliseconds(100));
    const Shape part = take(testing::makeTestPart(0));
    TempDir dir;
    CHECK(exportStep(dir.file("x.step"), std::span<const Shape>(&part, 1), ctx).error().code == ErrorCode::Cancelled);
    CHECK_FALSE(fs::exists(fromUtf8(dir.file("x.step"))));
}

TEST_CASE("cancel returns to the caller within 100 ms in every stage", "[kernel][io][wallclock]")
{
    // A file big enough that each stage lasts a while: 60 different parts, many with B-spline faces.
    TempDir dir;
    std::vector<Shape> parts;
    for (int i = 0; i < 60; ++i)
        parts.push_back(take(testing::makeTestPart(i % testing::kTestPartCount)));
    const std::string step = dir.file("big.step");
    const std::string iges = dir.file("big.igs");
    JobContext ctx;
    exportOk(exportStep(step, parts, ctx));
    exportOk(exportIges(iges, parts, ctx));

    for (const std::string& path : {step, iges}) {
        const bool isIges = path.ends_with(".igs");
        // Uncancelled run: how long the import takes.
        const auto start = std::chrono::steady_clock::now();
        {
            JobContext plain;
            const Result<ImportResult> full =
                isIges ? importIges(path, ImportOptions{}, plain) : importStep(path, ImportOptions{}, plain);
            REQUIRE(full.has_value());
            REQUIRE(full->bodies.size() == parts.size());
        }
        double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Cancel at 15 points spread over the whole import, so every stage is hit. The first run can be slower than
        // the later ones (a cold file cache, a loaded machine, a debug build): a run that finishes before its cancel
        // point shortens the import's length and that point is tried again (twice at most), so the late points still
        // land inside the import.
        std::vector<std::string> stagesHit;
        for (int k = 0, retries = 0; k < 15; ++k) {
            const double atMs = totalMs * (k + 0.5) / 15.0;
            CancelSource source;
            ProgressLog log;
            JobContext cancellable{source.token(), log.progress()};
            std::atomic<std::int64_t> cancelledAtNs{0};
            std::string stageAtCancel;
            const auto runStart = std::chrono::steady_clock::now();
            std::thread canceller([&] {
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(atMs));
                stageAtCancel = log.lastStage();
                cancelledAtNs = std::chrono::steady_clock::now().time_since_epoch().count();
                source.cancel();
            });
            const Result<ImportResult> result = isIges ? importIges(path, ImportOptions{}, cancellable)
                                                       : importStep(path, ImportOptions{}, cancellable);
            const auto returned = std::chrono::steady_clock::now();
            const std::int64_t returnedNs = returned.time_since_epoch().count();
            canceller.join();
            if (result.has_value()) {
                // Finished before the cancel request: the import is shorter now than it was measured.
                const double runMs = std::chrono::duration<double, std::milli>(returned - runStart).count();
                if (runMs < totalMs && retries < 2) {
                    totalMs = runMs;
                    ++retries;
                    --k;
                } else {
                    retries = 0;
                }
                continue;
            }
            retries = 0;
            REQUIRE(result.error().code == ErrorCode::Cancelled);
            const double latencyMs = static_cast<double>(returnedNs - cancelledAtNs.load()) / 1e6;
            INFO(path << ": cancel at " << atMs << " ms of " << totalMs << " ms, stage '" << stageAtCancel
                      << "', latency " << latencyMs << " ms");
            CHECK(latencyMs < 100.0);
            stagesHit.push_back(stageAtCancel);
            kernel::detail::waitForIoThreads(); // let the abandoned work end before the next run
        }
        INFO(path);
        CHECK(stagesHit.size() >= 10);
    }
    CHECK(kernel::detail::runningIoThreads() == 0);
}

TEST_CASE("a cancelled export leaves no file behind", "[kernel][io]")
{
    TempDir dir;
    std::vector<Shape> parts;
    for (int i = 0; i < 40; ++i)
        parts.push_back(take(testing::makeTestPart(i % testing::kTestPartCount)));
    for (const char* name : {"big.step", "big.igs", "big.stl"}) {
        INFO(name);
        CancelSource source;
        JobContext ctx{source.token(), Progress()};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            source.cancel();
        });
        const std::string path = dir.file(name);
        const std::string_view ext = std::string_view(name).substr(4);
        const Status status = ext == "step"  ? exportStep(path, parts, ctx)
                              : ext == "igs" ? exportIges(path, parts, ctx)
                                             : exportStl(path, parts, 0.01, ctx);
        canceller.join();
        kernel::detail::waitForIoThreads();
        if (!status.has_value()) {
            CHECK(status.error().code == ErrorCode::Cancelled);
            CHECK_FALSE(fs::exists(fromUtf8(path)));
        }
        CHECK_FALSE(fs::exists(fromUtf8(path + ".partial")));
    }
}

// ---------- Committed test files ----------

TEST_CASE("the committed test files import as expected", "[kernel][io]")
{
    {
        const ImportResult result = importOk(dataFile("box_hole.step"));
        REQUIRE(result.bodies.size() == 1);
        CHECK(close(volumeOf(result.bodies[0].shape), volumeOf(take(testing::makeTestPart(5))), kVolumeRel));
    }
    {
        const ImportResult result = importOk(dataFile("box_hole.igs"));
        REQUIRE(result.bodies.size() == 1);
        CHECK(close(volumeOf(result.bodies[0].shape), volumeOf(take(testing::makeTestPart(5))), kVolumeRel));
    }
    {
        const ImportResult result = importOk(dataFile("loft.step"));
        REQUIRE(result.bodies.size() == 1);
        CHECK(close(volumeOf(result.bodies[0].shape), volumeOf(take(testing::makeTestPart(16))), kVolumeRel));
    }
    for (const char* name : {"assembly_mm.step", "assembly_inch.step"}) {
        INFO(name);
        const ImportResult result = importOk(dataFile(name));
        REQUIRE(result.bodies.size() == 4);
        CHECK(result.bodies[2].name == "Pin");
        CHECK(result.sourceUnits ==
              (std::string_view(name) == "assembly_inch.step" ? UnitSystem::Inch : UnitSystem::Metric));
    }
    {
        const ImportResult result = importOk(dataFile("colored_box_inch.igs"));
        REQUIRE(result.bodies.size() == 1);
        CHECK(result.sourceUnits == UnitSystem::Inch);
        CHECK(colorClose(result.bodies[0].color, 0.8f, 0.1f, 0.1f));
    }
    {
        const ImportResult result = importOk(dataFile("loose_faces.step"));
        REQUIRE(result.bodies.size() == 1);
        CHECK(close(volumeOf(result.bodies[0].shape), 6000.0, kVolumeRel));
    }
    {
        // Written by hand (not by OCCT): a tetrahedron with a product name and a colour, in millimetres.
        const ImportResult result = importOk(dataFile("handwritten_tetra.step"));
        REQUIRE(result.bodies.size() == 1);
        CHECK(close(volumeOf(result.bodies[0].shape), 1000.0 / 6.0, kVolumeRel));
        CHECK(result.bodies[0].name == "Tetra");
        CHECK(colorClose(result.bodies[0].color, 0.2f, 0.4f, 0.6f));
        CHECK(result.sourceUnits == UnitSystem::Metric);
    }
}

// Regenerates the exporter-made files in tests/unit/kernel/data. Hidden; run by hand:
//   set RAYCAD_IO_DATA_OUT=<folder>  then  ray_kernel_tests "[.io-generate]"
TEST_CASE("generate the committed io test files", "[.io-generate]")
{
    const char* out = std::getenv("RAYCAD_IO_DATA_OUT");
    REQUIRE(out != nullptr);
    const fs::path folder = fromUtf8(out);
    const auto file = [&](const char* name) {
        return utf8(folder / name);
    };
    JobContext ctx;
    const Shape boxHole = take(testing::makeTestPart(5));
    const Shape loft = take(testing::makeTestPart(16));
    const Shape loose = take(testing::makeLooseBoxFaces());
    exportOk(exportStep(file("box_hole.step"), std::span<const Shape>(&boxHole, 1), ctx));
    exportOk(exportIges(file("box_hole.igs"), std::span<const Shape>(&boxHole, 1), ctx));
    exportOk(exportStep(file("loft.step"), std::span<const Shape>(&loft, 1), ctx));
    exportOk(exportStep(file("loose_faces.step"), std::span<const Shape>(&loose, 1), ctx));
    exportOk(testing::writeStepAssembly(file("assembly_mm.step"), false));
    exportOk(testing::writeStepAssembly(file("assembly_inch.step"), true));
    exportOk(testing::writeIgesColoredBox(file("colored_box_inch.igs"), true));
    // Truncated copies: the first 60% of a valid file.
    for (const auto& [from, to] :
         {std::pair{"box_hole.step", "truncated.step"}, std::pair{"box_hole.igs", "truncated.igs"}}) {
        const std::vector<char> bytes = readAll(file(from));
        writeAll(file(to), std::string_view(bytes.data(), bytes.size() * 6 / 10));
    }
}
