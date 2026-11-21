// Tessellation: tessellate() and triangulateExact() (task T-006).
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"

#include <mesh_testing.h>
#include <shape_testing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <tlhelp32.h>
#endif

using namespace ray;
using namespace ray::kernel;
using ray::kernel::test::take;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Named {
    std::string name;
    Shape shape;
};

// Every test body. All are closed solids or compounds of closed solids, except "planar face".
std::vector<Named> shapes()
{
    return {
        {"box", test::box(Vec3{-5, 0, 2}, Vec3{15, 10, 9})},
        {"cylinder", take(makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0))},
        {"tilted cylinder", take(makeCylinder(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 3.0, 7.0))},
        {"cut body", take(testing::makeCutBody())},
        {"filleted body", take(testing::makeFilletedBody())},
        {"sphere", take(testing::makeSphere(Vec3{1, -2, 3}, 10.0))},
        {"torus", take(testing::makeTorus(20.0, 5.0))},
        {"hole plate", take(testing::makeHolePlate(3, 4))},
        {"located compound", take(testing::makeLocatedCompound())},
    };
}

template <class T>
void appendRaw(std::vector<unsigned char>& out, const std::vector<T>& values)
{
    const std::size_t size = values.size();
    const auto* sizeBytes = reinterpret_cast<const unsigned char*>(&size);
    out.insert(out.end(), sizeBytes, sizeBytes + sizeof(size));
    if (!values.empty()) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
        out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
    }
}

// Exact bytes of a result, so that two results compare equal only if bit-identical.
std::vector<unsigned char> bytesOf(const Mesh& mesh)
{
    std::vector<unsigned char> out;
    appendRaw(out, mesh.positions);
    appendRaw(out, mesh.normals);
    appendRaw(out, mesh.indices);
    appendRaw(out, mesh.edgePositions);
    for (const FaceRange& face : mesh.faces) {
        appendRaw(out, std::vector<std::uint32_t>{static_cast<std::uint32_t>(face.face.kind), face.face.index,
                                                  face.firstIndex, face.indexCount});
    }
    for (const EdgeRange& edge : mesh.edges) {
        appendRaw(out, std::vector<std::uint32_t>{static_cast<std::uint32_t>(edge.edge.kind), edge.edge.index,
                                                  edge.firstVertex, edge.vertexCount});
    }
    for (const SubId& face : mesh.failedFaces)
        appendRaw(out, std::vector<std::uint32_t>{static_cast<std::uint32_t>(face.kind), face.index});
    return out;
}

std::vector<unsigned char> bytesOf(const TriSoup& soup)
{
    std::vector<unsigned char> out;
    appendRaw(out, soup.positions);
    appendRaw(out, soup.indices);
    return out;
}

// Empty on error. Safe on worker threads (no Catch2 assertion).
std::vector<unsigned char> displayBytes(const Shape& shape, const MeshOptions& options)
{
    JobContext ctx;
    auto mesh = tessellate(shape, options, ctx);
    return mesh ? bytesOf(*mesh) : std::vector<unsigned char>{};
}
std::vector<unsigned char> exactBytes(const Shape& shape, double chordTol)
{
    JobContext ctx;
    auto soup = triangulateExact(shape, chordTol, ctx);
    return soup ? bytesOf(*soup) : std::vector<unsigned char>{};
}

Mesh display(const Shape& shape, MeshOptions options = {0.1, 0.35})
{
    JobContext ctx;
    return take(tessellate(shape, options, ctx));
}
TriSoup exact(const Shape& shape, double chordTol)
{
    JobContext ctx;
    return take(triangulateExact(shape, chordTol, ctx));
}

// Every face and edge exactly once, ascending, with consistent contiguous ranges.
void checkLayout(const Mesh& mesh, const Shape& shape)
{
    REQUIRE(mesh.positions.size() % 3 == 0);
    REQUIRE(mesh.normals.size() == mesh.positions.size());
    REQUIRE(mesh.edgePositions.size() % 3 == 0);
    const std::size_t vertices = mesh.positions.size() / 3;

    REQUIRE(mesh.faces.size() == shape.count(SubKind::Face));
    std::uint32_t nextIndex = 0;
    std::vector<SubId> empty;
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        const FaceRange& face = mesh.faces[i];
        CHECK(face.face == SubId{SubKind::Face, static_cast<std::uint32_t>(i)});
        CHECK(face.firstIndex == nextIndex);
        CHECK(face.indexCount % 3 == 0);
        if (face.indexCount == 0)
            empty.push_back(face.face);
        nextIndex = face.firstIndex + face.indexCount;
    }
    CHECK(nextIndex == mesh.indices.size());
    CHECK(mesh.failedFaces == empty);
    CHECK(std::all_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint32_t i) { return i < vertices; }));

    REQUIRE(mesh.edges.size() == shape.count(SubKind::Edge));
    std::uint32_t nextVertex = 0;
    for (std::size_t i = 0; i < mesh.edges.size(); ++i) {
        const EdgeRange& edge = mesh.edges[i];
        CHECK(edge.edge == SubId{SubKind::Edge, static_cast<std::uint32_t>(i)});
        CHECK(edge.firstVertex == nextVertex);
        nextVertex = edge.firstVertex + edge.vertexCount;
    }
    CHECK(nextVertex == mesh.edgePositions.size() / 3);

    for (std::size_t v = 0; v < vertices; ++v) {
        const double nx = mesh.normals[3 * v];
        const double ny = mesh.normals[3 * v + 1];
        const double nz = mesh.normals[3 * v + 2];
        CHECK(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0) < 1e-5);
    }
}

// No boundary and no non-manifold edge: every directed edge occurs once and its reverse once. Indices are in
// range and no triangle is degenerate.
bool watertight(const TriSoup& soup, std::string& why)
{
    const std::size_t vertices = soup.positions.size() / 3;
    if (soup.positions.size() % 3 != 0 || soup.indices.size() % 3 != 0 || soup.indices.empty()) {
        why = "bad sizes";
        return false;
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
    for (std::size_t t = 0; t < soup.indices.size(); t += 3) {
        const std::uint32_t v[3] = {soup.indices[t], soup.indices[t + 1], soup.indices[t + 2]};
        for (int k = 0; k < 3; ++k) {
            if (v[k] >= vertices) {
                why = "index out of range";
                return false;
            }
            if (v[k] == v[(k + 1) % 3]) {
                why = "degenerate triangle";
                return false;
            }
            ++directed[{v[k], v[(k + 1) % 3]}];
        }
    }
    for (const auto& [edge, count] : directed) {
        if (count != 1) {
            why = "edge " + std::to_string(edge.first) + "-" + std::to_string(edge.second) + " used " +
                  std::to_string(count) + " times in one direction (non-manifold or flipped)";
            return false;
        }
        if (directed.find({edge.second, edge.first}) == directed.end()) {
            why = "boundary edge " + std::to_string(edge.first) + "-" + std::to_string(edge.second);
            return false;
        }
    }
    return true;
}

double volumeOf(const TriSoup& soup)
{
    double sum = 0;
    for (std::size_t t = 0; t < soup.indices.size(); t += 3) {
        const double* a = &soup.positions[3 * soup.indices[t]];
        const double* b = &soup.positions[3 * soup.indices[t + 1]];
        const double* c = &soup.positions[3 * soup.indices[t + 2]];
        sum += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
               a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    return sum / 6.0;
}

// Restores the InParallel switch however a test ends.
struct ParallelSwitch {
    explicit ParallelSwitch(bool on) : previous(testing::meshInParallel()) { testing::setMeshInParallel(on); }
    ~ParallelSwitch() { testing::setMeshInParallel(previous); }
    ParallelSwitch(const ParallelSwitch&) = delete;
    ParallelSwitch& operator=(const ParallelSwitch&) = delete;
    bool previous;
};

} // namespace

TEST_CASE("tessellate lists every face and edge once, in SubId order", "[kernel][mesh]")
{
    for (const Named& item : shapes()) {
        INFO(item.name);
        const Mesh mesh = display(item.shape);
        checkLayout(mesh, item.shape);
        CHECK(mesh.failedFaces.empty());
        for (const FaceRange& face : mesh.faces)
            CHECK(face.indexCount > 0);
        for (const EdgeRange& edge : mesh.edges)
            CHECK((edge.vertexCount >= 2 || edge.vertexCount == 0));
    }

    const Shape face = take(testing::makePlanarFace());
    const Mesh mesh = display(face);
    checkLayout(mesh, face);
    REQUIRE(mesh.faces.size() == 1);
    CHECK(mesh.faces[0].indexCount >= 6);
    for (const EdgeRange& edge : mesh.edges)
        CHECK(edge.vertexCount >= 2);
}

TEST_CASE("tessellate gives surface normals that point out of the solid", "[kernel][mesh]")
{
    SECTION("sphere: normal is the radial direction")
    {
        const Vec3 centre{1, -2, 3};
        const Mesh mesh = display(take(testing::makeSphere(centre, 10.0)));
        for (std::size_t v = 0; v < mesh.positions.size() / 3; ++v) {
            const Vec3 radial{(mesh.positions[3 * v] - centre.x) / 10.0, (mesh.positions[3 * v + 1] - centre.y) / 10.0,
                              (mesh.positions[3 * v + 2] - centre.z) / 10.0};
            const Vec3 normal{mesh.normals[3 * v], mesh.normals[3 * v + 1], mesh.normals[3 * v + 2]};
            CHECK(test::dot(radial, normal) > 0.9999);
        }
    }
    SECTION("every shape: triangles wind the way the normals point")
    {
        // A triangle's winding normal agrees with its vertex normals, and on a box face normals are flat.
        for (const Named& item : shapes()) {
            INFO(item.name);
            const Mesh mesh = display(item.shape);
            for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
                auto at = [&](std::size_t k) {
                    const std::uint32_t v = mesh.indices[t + k];
                    return Vec3{mesh.positions[3 * v], mesh.positions[3 * v + 1], mesh.positions[3 * v + 2]};
                };
                const Vec3 winding = test::cross(test::sub(at(1), at(0)), test::sub(at(2), at(0)));
                const double area2 = test::length(winding);
                if (area2 < 1e-9)
                    continue;
                const std::uint32_t v0 = mesh.indices[t];
                const Vec3 normal{mesh.normals[3 * v0], mesh.normals[3 * v0 + 1], mesh.normals[3 * v0 + 2]};
                CHECK(test::dot(winding, normal) / area2 > 0.5);
            }
        }
    }
    SECTION("located compound: the second instance is moved by (200, 0, 0)")
    {
        const Mesh mesh = display(take(testing::makeLocatedCompound()));
        REQUIRE(mesh.faces.size() == 12);
        for (std::size_t f = 0; f < 12; ++f) {
            const FaceRange& range = mesh.faces[f];
            for (std::uint32_t i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i) {
                const float x = mesh.positions[3 * mesh.indices[i]];
                if (f < 6)
                    CHECK((x >= -1e-4f && x <= 10.0001f));
                else
                    CHECK((x >= 199.999f && x <= 210.001f));
            }
        }
    }
}

TEST_CASE("triangulateExact is watertight on closed solids", "[kernel][mesh]")
{
    for (const Named& item : shapes()) {
        for (const double chordTol : {0.2, 0.02}) {
            INFO(item.name << " at chord tolerance " << chordTol);
            const TriSoup soup = exact(item.shape, chordTol);
            std::string why;
            CHECK(watertight(soup, why));
            INFO(why);
            const MassProps props = take(massProps(item.shape));
            CHECK(std::abs(volumeOf(soup) - props.volume) <= props.area * chordTol);
        }
    }
}

TEST_CASE("triangulateExact volume converges to the analytic volume", "[kernel][mesh]")
{
    struct Case {
        std::string name;
        Shape shape;
        double volume;
        double area;
    };
    const std::vector<Case> cases = {
        {"cylinder", take(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 10.0, 20.0)), kPi * 100.0 * 20.0,
         2 * kPi * 100.0 + 2 * kPi * 10.0 * 20.0},
        {"sphere", take(testing::makeSphere(Vec3{0, 0, 0}, 10.0)), 4.0 / 3.0 * kPi * 1000.0, 4 * kPi * 100.0},
        {"torus", take(testing::makeTorus(20.0, 5.0)), 2 * kPi * kPi * 20.0 * 25.0, 4 * kPi * kPi * 20.0 * 5.0},
    };
    for (const Case& c : cases) {
        double previous = std::numeric_limits<double>::infinity();
        for (const double chordTol : {0.5, 0.1, 0.02, 0.004}) {
            INFO(c.name << " at chord tolerance " << chordTol);
            const double error = std::abs(volumeOf(exact(c.shape, chordTol)) - c.volume);
            // Nodes lie on the surface and no point of a triangle is further than chordTol from it.
            CHECK(error <= c.area * chordTol);
            CHECK(error < previous);
            previous = error;
        }
        CHECK(previous <= 2e-3 * c.volume);
    }
}

TEST_CASE("triangulateExact does not depend on display meshing", "[kernel][mesh]")
{
    for (const Named& item : shapes()) {
        INFO(item.name);
        const std::vector<std::byte> bytes = serialize(item.shape);
        const Shape fresh = take(deserialize(bytes));
        const std::vector<unsigned char> reference = exactBytes(fresh, 0.05);
        REQUIRE_FALSE(reference.empty());

        // Display meshes at finer and coarser tolerances first (the tier presets and more).
        for (const MeshOptions options : {MeshOptions{0.2, 20 * kPi / 180}, MeshOptions{0.05, 12 * kPi / 180},
                                          MeshOptions{0.01, 6 * kPi / 180}, MeshOptions{2.0, 1.0}})
            REQUIRE_FALSE(displayBytes(item.shape, options).empty());
        CHECK(exactBytes(item.shape, 0.05) == reference);
        CHECK(exactBytes(item.shape, 0.05) == reference);
    }
}

TEST_CASE("meshing does not change the caller's shape", "[kernel][mesh]")
{
    for (const Named& item : shapes()) {
        INFO(item.name);
        const std::vector<std::byte> before = serialize(item.shape);
        const Hash256 hashBefore = item.shape.contentHash();
        CHECK_FALSE(testing::hasMeshData(item.shape));

        static_cast<void>(display(item.shape, MeshOptions{0.01, 0.1}));
        static_cast<void>(exact(item.shape, 0.02));

        CHECK_FALSE(testing::hasMeshData(item.shape));
        CHECK(serialize(item.shape) == before);
        CHECK(item.shape.contentHash() == hashBefore);
        CHECK(take(deserialize(serialize(item.shape))).contentHash() == hashBefore);
    }
}

TEST_CASE("faces without triangles are reported", "[kernel][mesh]")
{
    const Shape shape = take(testing::makeBodyWithUnmeshableFace());
    REQUIRE(shape.count(SubKind::Face) == 7);
    const Mesh mesh = display(shape);
    checkLayout(mesh, shape);
    REQUIRE(mesh.failedFaces == std::vector<SubId>{SubId{SubKind::Face, 6}});
    CHECK(mesh.faces[6].indexCount == 0);
    for (std::size_t f = 0; f < 6; ++f)
        CHECK(mesh.faces[f].indexCount > 0);
    // The failed face still shows its outline.
    for (const SubId edge : adjacent(shape, SubId{SubKind::Face, 6}, SubKind::Edge))
        CHECK(mesh.edges[edge.index].vertexCount >= 2);

    JobContext ctx;
    const auto soup = triangulateExact(shape, 0.05, ctx);
    REQUIRE_FALSE(soup.has_value());
    CHECK(soup.error().code == ErrorCode::KernelFailure);
    CHECK(soup.error().message.find("face 6") != std::string::npos);
}

TEST_CASE("mesh functions return errors for bad input and never throw", "[kernel][mesh]")
{
    JobContext ctx;
    const Shape good = test::box(Vec3{0, 0, 0}, Vec3{1, 1, 1});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    auto meshError = [&](const Shape& shape, MeshOptions options) {
        Result<Mesh> result = Mesh{};
        REQUIRE_NOTHROW(result = tessellate(shape, options, ctx));
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    auto exactError = [&](const Shape& shape, double chordTol) {
        Result<TriSoup> result = TriSoup{};
        REQUIRE_NOTHROW(result = triangulateExact(shape, chordTol, ctx));
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };

    CHECK(meshError(Shape{}, MeshOptions{0.1, 0.3}) == ErrorCode::KernelFailure);
    CHECK(exactError(Shape{}, 0.1) == ErrorCode::KernelFailure);
    for (const double bad : {0.0, -0.1, nan, inf, -inf, 1e-13}) {
        INFO("tolerance " << bad);
        CHECK(meshError(good, MeshOptions{bad, 0.3}) == ErrorCode::InvalidArgument);
        CHECK(meshError(good, MeshOptions{0.1, bad}) == ErrorCode::InvalidArgument);
        CHECK(exactError(good, bad) == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("mesh with InParallel equals the single-threaded mesh", "[kernel][mesh]")
{
    std::vector<Named> items = shapes();
    items.push_back({"big hole plate", take(testing::makeHolePlate(8, 8))});
    for (const Named& item : items) {
        INFO(item.name);
        std::vector<unsigned char> serialDisplay;
        std::vector<unsigned char> serialExact;
        {
            const ParallelSwitch off(false);
            serialDisplay = displayBytes(item.shape, MeshOptions{0.05, 0.2});
            serialExact = exactBytes(item.shape, 0.02);
        }
        REQUIRE_FALSE(serialDisplay.empty());
        REQUIRE_FALSE(serialExact.empty());
        const ParallelSwitch on(true);
        CHECK(displayBytes(item.shape, MeshOptions{0.05, 0.2}) == serialDisplay);
        CHECK(exactBytes(item.shape, 0.02) == serialExact);
    }
}

TEST_CASE("meshing the naming tests' corpus parts with InParallel gives the serial mesh every time", "[.meshcorpus]")
{
    // The 20 corpus parts of the naming random blocks at their sample tolerance (tests/naming/oracle), each meshed
    // RAYCAD_MESH_REPEATS times (default 3) with InParallel on and compared with the serial mesh. For runs against an
    // OCCT built with ThreadSanitizer or AddressSanitizer: BRepMesh's parallel healing is what they look at.
    const char* const parts[] = {
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
    int repeats = 3;
    if (const char* text = std::getenv("RAYCAD_MESH_REPEATS"))
        repeats = std::max(1, std::atoi(text));
    const MeshOptions options{0.02, 0.2};
    for (const char* part : parts) {
        INFO(part);
        JobContext ctx;
        auto imported = importStep(std::string(RAY_CORPUS_DIR) + "/" + part, ImportOptions{}, ctx);
        REQUIRE(imported.has_value());
        for (const ImportedBody& body : imported->bodies) {
            std::vector<unsigned char> serial;
            {
                const ParallelSwitch off(false);
                serial = displayBytes(body.shape, options);
            }
            REQUIRE_FALSE(serial.empty());
            const ParallelSwitch on(true);
            for (int r = 0; r < repeats; ++r)
                CHECK(displayBytes(body.shape, options) == serial);
        }
    }
}

TEST_CASE("8 threads meshing 8 shapes give the serial results", "[kernel][mesh][threads]")
{
    std::vector<Named> items = shapes();
    items.resize(8);
    const MeshOptions options{0.05, 0.25};
    std::vector<std::vector<unsigned char>> serialDisplay;
    std::vector<std::vector<unsigned char>> serialExact;
    for (const Named& item : items) {
        serialDisplay.push_back(displayBytes(item.shape, options));
        serialExact.push_back(exactBytes(item.shape, 0.05));
        REQUIRE_FALSE(serialDisplay.back().empty());
        REQUIRE_FALSE(serialExact.back().empty());
    }

    constexpr int kRounds = 3;
    SECTION("each thread its own shape")
    {
        std::vector<std::vector<std::vector<unsigned char>>> displays(8), exacts(8);
        std::vector<std::thread> threads;
        for (std::size_t t = 0; t < 8; ++t)
            threads.emplace_back([&, t] {
                for (int round = 0; round < kRounds; ++round) {
                    displays[t].push_back(displayBytes(items[t].shape, options));
                    exacts[t].push_back(exactBytes(items[t].shape, 0.05));
                }
            });
        for (std::thread& thread : threads)
            thread.join();
        for (std::size_t t = 0; t < 8; ++t) {
            INFO(items[t].name);
            for (int round = 0; round < kRounds; ++round) {
                CHECK(displays[t][round] == serialDisplay[t]);
                CHECK(exacts[t][round] == serialExact[t]);
            }
        }
    }
    SECTION("all threads on the same shapes, in different orders")
    {
        std::vector<std::vector<std::vector<unsigned char>>> displays(8, std::vector<std::vector<unsigned char>>(8));
        std::vector<std::thread> threads;
        for (std::size_t t = 0; t < 8; ++t)
            threads.emplace_back([&, t] {
                for (std::size_t k = 0; k < 8; ++k) {
                    const std::size_t s = (t + k) % 8;
                    displays[t][s] = displayBytes(items[s].shape, options);
                }
            });
        for (std::thread& thread : threads)
            thread.join();
        for (std::size_t t = 0; t < 8; ++t)
            for (std::size_t s = 0; s < 8; ++s) {
                INFO(items[s].name << " on thread " << t);
                CHECK(displays[t][s] == serialDisplay[s]);
            }
    }
}

TEST_CASE("the number of faces meshed at once shrinks with the tolerance", "[kernel][mesh]")
{
    const int coarse = testing::meshConcurrency(0.2);
    const int medium = testing::meshConcurrency(0.05);
    const int fine = testing::meshConcurrency(0.01);
    CHECK(coarse >= medium);
    CHECK(medium >= fine);
    CHECK(fine >= 1);
    CHECK(fine <= 4);
    CHECK(testing::meshConcurrency(1e-6) == 1);
}

namespace {
// Threads of this process, from the operating system; 0 where it cannot tell.
int processThreads()
{
#if defined(_WIN32)
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    int n = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        n += entry.th32OwnerProcessID == GetCurrentProcessId() ? 1 : 0;
    CloseHandle(snapshot);
    return n;
#elif defined(__linux__)
    std::error_code ec;
    int n = 0;
    for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end; it.increment(ec))
        ++n;
    return n;
#else
    return 0;
#endif
}
} // namespace

TEST_CASE("a mesh call runs on no more threads than its memory cap allows", "[kernel][mesh][threads]")
{
    // OSD_Parallel ignores SetNbDefaultThreadsToLaunch, so the cap is the size of OCCT's pool (mesh.cpp MEMORY CAP).
    const Shape plate = take(testing::makeHolePlate(8, 8)); // 70 faces: more than the pool has threads
    const int coarseTeam = testing::meshConcurrency(0.2);
    const int fineTeam = testing::meshConcurrency(0.01);
    REQUIRE_FALSE(displayBytes(plate, MeshOptions{0.2, 0.35}).empty());
    CHECK(testing::lastMeshTeam() == coarseTeam);
    CHECK(testing::meshPoolThreads() == coarseTeam);
    const int afterCoarse = processThreads();
    REQUIRE_FALSE(displayBytes(plate, MeshOptions{0.01, 0.35}).empty());
    CHECK(testing::lastMeshTeam() == fineTeam);
    CHECK(testing::meshPoolThreads() == fineTeam);
    const int afterFine = processThreads();
    // The pool started up to coarseTeam - 1 threads of its own for the coarse call and kept them; resizing it to the
    // fine team ended them and started at most fineTeam - 1 (allow one thread of slack for the rest of the process).
    if (afterCoarse > 0 && afterFine > 0) {
        INFO("threads after the coarse call " << afterCoarse << ", after the fine call " << afterFine);
        CHECK(afterFine <= afterCoarse - (coarseTeam - fineTeam) + 1);
    }
    // Back to the coarse size for the next call, and the mesh is the same whatever the team.
    const std::vector<unsigned char> again = displayBytes(plate, MeshOptions{0.2, 0.35});
    CHECK(testing::meshPoolThreads() == coarseTeam);
    const ParallelSwitch off(false);
    CHECK(displayBytes(plate, MeshOptions{0.2, 0.35}) == again);
}

// Not part of the normal run: prints how long meshing takes. Run with "[.timing]".
TEST_CASE("mesh timings", "[.timing]")
{
    std::vector<Named> items = shapes();
    items.push_back({"hole plate 30 x 30", take(testing::makeHolePlate(30, 30))});
    for (const Named& item : items) {
        for (const MeshOptions options :
             {MeshOptions{0.2, 20 * kPi / 180}, MeshOptions{0.05, 12 * kPi / 180}, MeshOptions{0.01, 6 * kPi / 180}}) {
            const auto start = std::chrono::steady_clock::now();
            const Mesh mesh = display(item.shape, options);
            const auto middle = std::chrono::steady_clock::now();
            const TriSoup soup = exact(item.shape, options.chordTol);
            const auto end = std::chrono::steady_clock::now();
            using ms = std::chrono::duration<double, std::milli>;
            std::cout << item.name << " @" << options.chordTol << " mm: tessellate " << ms(middle - start).count()
                      << " ms (" << mesh.indices.size() / 3 << " triangles), triangulateExact "
                      << ms(end - middle).count() << " ms (" << soup.indices.size() / 3 << " triangles)\n";
        }
    }
}
