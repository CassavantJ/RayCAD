// Tessellation (task T-006): tessellate() for display, triangulateExact() for CAM.
//
// HOW ONE MESH CALL RUNS
//  1. Copy. BRepBuilderAPI_Copy builds new TShapes for every vertex, edge, face and container of the caller's
//     shape. Geometry (surfaces, curves) is shared and only read; no mesh data is copied. BRepMesh writes
//     triangulations and polygons into the TShapes it meshes and sets their flags, so meshing the private copy
//     means that
//       - the caller's Shape is never written to: other threads may read it meanwhile (kernel.h: a Shape is
//         immutable), and serialize() / contentHash() cannot change;
//       - the result never depends on meshes made earlier at other tolerances (BRepMesh keeps an existing
//         triangulation that is fine enough), so triangulateExact() cannot depend on any display option.
//  2. Mesh, under ONE process-wide lock. Spike S3: OCCT 8.0.1 BRepMesh crashed and silently dropped faces
//     when two shapes were meshed at the same time (PLAN.md section 7 rule 3). Inside the lock:
//     BRepMesh_IncrementalMesh with the Watson algorithm named explicitly (the default one; naming it keeps
//     the CSF_MeshAlgo environment variable from switching to Delabella), InParallel on (bit-identical to
//     single-threaded in S3; tests compare the two), and OCCT's thread team capped so that the number of
//     faces in flight stays inside the memory budget (see "MEMORY CAP").
//  3. Extract, outside the lock, from the private copy into Mesh / TriSoup, in canonical SubId order. A face
//     whose triangulation is missing or empty has failed: BRepMesh's status flags cannot be trusted (S3).
//
// WELDING (triangulateExact). Every face has its own triangulation nodes. BRepMesh discretises each edge once
// and gives every face along it the same points in the same order, recorded per face in a
// Poly_PolygonOnTriangulation (node indices into that face's triangulation). Nodes are welded through those
// polygons: point k of edge e on face A is the same vertex as point k of edge e on face B. That is exact (no
// distance tolerance), closes periodic seams (a seam edge has two polygons on its face), and joins nodes at a
// shared vertex because each face shares one node between the two edges meeting there. All points of a
// degenerated edge (the pole of a sphere) are one vertex; triangles that collapse there are dropped. Each
// welded vertex takes the position of its lowest node (face order, then node order), so the result is a
// function of the triangulations alone.
//
// CANCELLATION AND PROGRESS: ctx.cancel is checked before meshing and before extraction. TODO: pass a
// Message_ProgressIndicator whose UserBreak() reads ctx.cancel into BRepMesh_IncrementalMesh::Perform.
// A single slow face still cannot be interrupted (S3: up to 14 s at 0.01 mm); the caller abandons the job
// (kernel.h contract).
#include "io_internal.h"
#include "mesh_testing.h"
#include "shape_impl.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <IMeshTools_Parameters.hxx>
#include <OSD_ThreadPool.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace ray::kernel {

namespace {

// Angular deflection for triangulateExact(). CAM accuracy is set by the chord tolerance, which comes from the
// operation; this only keeps long, gently curved edges from being split too coarsely. Fixed: it must never
// come from a tier preset.
constexpr double kExactAngle = 0.5; // radians, OCCT's default

// MEMORY CAP. The Watson mesher's transient memory grows with the number of faces in flight and with the
// fineness of the tolerance. Spike S3 (47 MiB file, 32 threads) peaked at 2.7 GB at 0.2 mm, 5.6 GB at 0.05 mm
// and 15.6 GB at 0.01 mm: about 85, 175 and 490 MB per face in flight. Model: kFaceMbAtCoarse * (0.2 / tol)^0.6
// per face, which lies above all three points. The thread team is sized so that the model stays inside
// kMeshBudgetMb: the smallest import row of PLAN.md section 9 (3 GB, Low tier) less 0.5 GB for the rest of
// the import. The kernel does not know the tier, and CAM tolerances are not tied to one, so the smallest row
// applies everywhere. That is no cap at 0.2 mm on any current machine, 13 faces at 0.05 mm and 4 at
// 0.01 mm. UNVERIFIED: there is no corpus file yet to measure it on.
constexpr double kMeshBudgetMb = 2560.0;
constexpr double kFaceMbAtCoarse = 85.0;
constexpr double kCoarseTol = 0.2;
constexpr double kFaceMbExponent = 0.6;

std::atomic<bool> gInParallel{true};

// The one process-wide mesh lock, shared with STL export (io_internal.h).
std::mutex& meshMutex()
{
    return detail::processMeshMutex();
}

int poolThreads()
{
    return std::max(1, OSD_ThreadPool::DefaultPool()->NbThreads());
}

int concurrencyFor(double chordTol, int pool)
{
    const double perFaceMb = kFaceMbAtCoarse * std::pow(kCoarseTol / chordTol, kFaceMbExponent);
    const double fits = std::floor(kMeshBudgetMb / perFaceMb);
    if (!(fits >= 1.0))
        return 1;
    if (fits >= static_cast<double>(pool))
        return pool;
    return static_cast<int>(fits);
}

Error invalid(const char* message, std::string_view where)
{
    return detail::makeError(ErrorCode::InvalidArgument, message, where);
}

Status checkTolerances(double chordTol, double angleTol, std::string_view where)
{
    if (!std::isfinite(chordTol) || !(chordTol >= Precision::Confusion()))
        return tl::unexpected(invalid("The chord tolerance must be a positive length.", where));
    if (!std::isfinite(angleTol) || !(angleTol >= Precision::Angular()))
        return tl::unexpected(invalid("The angle tolerance must be a positive angle.", where));
    return std::monostate{};
}

// Copies the caller's shape and meshes the copy (steps 1 and 2 of the file comment). The copier holds the
// map from the caller's sub-shapes to the copy's.
Result<std::shared_ptr<BRepBuilderAPI_Copy>> meshedCopy(const TopoDS_Shape& original, double chordTol, double angleTol,
                                                        std::string_view where)
{
    using Copier = std::shared_ptr<BRepBuilderAPI_Copy>;
    // Outside the lock: copying only reads the caller's shape and writes the new TShapes.
    Result<Copier> copier = detail::guarded(where, [&]() -> Result<Copier> {
        auto made = std::make_shared<BRepBuilderAPI_Copy>(original, /*copyGeom=*/false, /*copyMesh=*/false);
        if (!made->IsDone() || made->Shape().IsNull())
            return tl::unexpected(
                detail::makeError(ErrorCode::KernelFailure, "The shape could not be prepared for meshing.", where));
        return Copier(std::move(made));
    });
    if (!copier)
        return copier;

    const TopoDS_Shape& copy = (*copier)->Shape();
    // The lock and the pool's team are held outside guarded(): a structured exception skips destructors inside it.
    const std::lock_guard<std::mutex> lock(meshMutex());
    const detail::MeshTeamScope team(chordTol);
    Status meshed = detail::guarded(where, [&]() -> Status {
        IMeshTools_Parameters parameters;
        parameters.MeshAlgo = IMeshTools_MeshAlgoType_Watson;
        parameters.Deflection = chordTol;
        parameters.Angle = angleTol;
        parameters.Relative = false;
        parameters.InParallel = gInParallel.load(std::memory_order_relaxed) && team.team() >= 2;
        // TODO: pass a Message_ProgressRange whose indicator reads ctx.cancel and reports to ctx.progress.
        BRepMesh_IncrementalMesh mesher(copy, parameters);
        return std::monostate{};
    });
    if (!meshed)
        return tl::unexpected(meshed.error());
    return copier;
}

// The copy of one of the caller's sub-shapes.
template <class T>
T copied(const BRepBuilderAPI_Copy& copier, const TopoDS_Shape& original)
{
    const TopoDS_Shape result = copier.ModifiedShape(original);
    if constexpr (std::is_same_v<T, TopoDS_Face>)
        return TopoDS::Face(result);
    else
        return TopoDS::Edge(result);
}

// A face's triangulation and where it sits, or a null handle if the face has none (a failed face).
struct FaceMesh {
    occ::handle<Poly_Triangulation> triangulation;
    TopLoc_Location location;
    TopoDS_Face face; // the copy
};

FaceMesh faceMesh(const BRepBuilderAPI_Copy& copier, const TopoDS_Shape& originalFace)
{
    FaceMesh result;
    result.face = copied<TopoDS_Face>(copier, originalFace);
    result.triangulation = BRep_Tool::Triangulation(result.face, result.location);
    if (!result.triangulation.IsNull() &&
        (result.triangulation->NbTriangles() <= 0 || result.triangulation->NbNodes() <= 0))
        result.triangulation.Nullify();
    return result;
}

void pushFloat3(std::vector<float>& out, double x, double y, double z)
{
    out.push_back(static_cast<float>(x));
    out.push_back(static_cast<float>(y));
    out.push_back(static_cast<float>(z));
}

std::uint32_t checkedU32(std::size_t value)
{
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("the mesh has more than 2^32 - 1 vertices or indices");
    return static_cast<std::uint32_t>(value);
}

// The polyline of a non-degenerated edge: the polygon BRepMesh left on the lowest-numbered adjacent face that
// has one, else the edge's 3D polygon (free edges), else a fresh discretisation of the curve (edges whose
// faces all failed, so that a failed face still shows its outline).
void edgePolyline(const Shape& shape, const BRepBuilderAPI_Copy& copier, const TopoDS_Shape& originalEdge,
                  const MeshOptions& options, std::vector<float>& out)
{
    const TopoDS_Edge edge = copied<TopoDS_Edge>(copier, originalEdge);
    const detail::ShapeIndex& faces = shape.impl->subs[detail::kindSlot(SubKind::Face)];
    const detail::AncestorIndex& edgeFaces = shape.impl->ancestors(SubKind::Edge, SubKind::Face);

    std::vector<int> adjacent;
    if (const NCollection_List<TopoDS_Shape>* list = edgeFaces.Seek(originalEdge); list != nullptr)
        for (const TopoDS_Shape& face : *list)
            adjacent.push_back(faces.FindIndex(face));
    std::sort(adjacent.begin(), adjacent.end());

    for (const int faceIndex : adjacent) {
        if (faceIndex <= 0)
            continue;
        const FaceMesh mesh = faceMesh(copier, faces.FindKey(faceIndex));
        if (mesh.triangulation.IsNull())
            continue;
        const occ::handle<Poly_PolygonOnTriangulation>& polygon =
            BRep_Tool::PolygonOnTriangulation(edge, mesh.triangulation, mesh.location);
        if (polygon.IsNull() || polygon->NbNodes() < 2)
            continue;
        const gp_Trsf transform = mesh.location.Transformation();
        for (int k = 1; k <= polygon->NbNodes(); ++k) {
            const gp_Pnt point = mesh.triangulation->Node(polygon->Node(k)).Transformed(transform);
            pushFloat3(out, point.X(), point.Y(), point.Z());
        }
        return;
    }

    TopLoc_Location location;
    const occ::handle<Poly_Polygon3D>& polygon3d = BRep_Tool::Polygon3D(edge, location);
    if (!polygon3d.IsNull() && polygon3d->NbNodes() >= 2) {
        const gp_Trsf transform = location.Transformation();
        for (const gp_Pnt& node : polygon3d->Nodes()) {
            const gp_Pnt point = node.Transformed(transform);
            pushFloat3(out, point.X(), point.Y(), point.Z());
        }
        return;
    }

    if (!BRep_Tool::IsGeometric(edge))
        return;
    const BRepAdaptor_Curve curve(edge);
    const GCPnts_TangentialDeflection points(curve, options.angleTol, options.chordTol);
    for (int i = 1; i <= points.NbPoints(); ++i) {
        const gp_Pnt point = points.Value(i);
        pushFloat3(out, point.X(), point.Y(), point.Z());
    }
}

Mesh buildDisplayMesh(const Shape& shape, const BRepBuilderAPI_Copy& copier, const MeshOptions& options)
{
    Mesh mesh;
    const detail::ShapeIndex& faces = shape.impl->subs[detail::kindSlot(SubKind::Face)];
    mesh.faces.reserve(static_cast<std::size_t>(faces.Extent()));
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Shape& original = faces.FindKey(i);
        FaceRange range{SubId{SubKind::Face, static_cast<std::uint32_t>(i - 1)}, checkedU32(mesh.indices.size()), 0};
        const FaceMesh face = faceMesh(copier, original);
        if (face.triangulation.IsNull()) {
            mesh.failedFaces.push_back(range.face);
            mesh.faces.push_back(range);
            continue;
        }
        const occ::handle<Poly_Triangulation>& triangulation = face.triangulation;
        // Normals from the surface at each node's (u, v); writes into the copy's triangulation only.
        BRepLib_ToolTriangulatedShape::ComputeNormals(face.face, triangulation);
        const gp_Trsf transform = face.location.Transformation();
        const bool reversed = original.Orientation() == TopAbs_REVERSED;

        const std::uint32_t base = checkedU32(mesh.positions.size() / 3);
        checkedU32(static_cast<std::size_t>(base) + static_cast<std::size_t>(triangulation->NbNodes()));
        for (int n = 1; n <= triangulation->NbNodes(); ++n) {
            const gp_Pnt point = triangulation->Node(n).Transformed(transform);
            pushFloat3(mesh.positions, point.X(), point.Y(), point.Z());
            gp_Dir normal = triangulation->HasNormals() ? triangulation->Normal(n) : gp_Dir(0, 0, 1);
            normal.Transform(transform);
            if (reversed)
                normal.Reverse();
            pushFloat3(mesh.normals, normal.X(), normal.Y(), normal.Z());
        }
        for (int t = 1; t <= triangulation->NbTriangles(); ++t) {
            int a = 0;
            int b = 0;
            int c = 0;
            triangulation->Triangle(t).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            for (const int node : {a, b, c})
                mesh.indices.push_back(base + static_cast<std::uint32_t>(node - 1));
        }
        range.indexCount = checkedU32(mesh.indices.size() - range.firstIndex);
        mesh.faces.push_back(range);
    }

    const detail::ShapeIndex& edges = shape.impl->subs[detail::kindSlot(SubKind::Edge)];
    mesh.edges.reserve(static_cast<std::size_t>(edges.Extent()));
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Shape& original = edges.FindKey(i);
        EdgeRange range{SubId{SubKind::Edge, static_cast<std::uint32_t>(i - 1)},
                        checkedU32(mesh.edgePositions.size() / 3), 0};
        if (!BRep_Tool::Degenerated(TopoDS::Edge(original)))
            edgePolyline(shape, copier, original, options, mesh.edgePositions);
        range.vertexCount = checkedU32(mesh.edgePositions.size() / 3 - range.firstVertex);
        mesh.edges.push_back(range);
    }
    return mesh;
}

// Union-find over triangulation nodes; the root of a set is its lowest node.
class NodeSets {
public:
    explicit NodeSets(std::size_t count) : parent_(count)
    {
        std::iota(parent_.begin(), parent_.end(), std::uint32_t{0});
    }
    std::uint32_t find(std::uint32_t node)
    {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }
    void unite(std::uint32_t a, std::uint32_t b)
    {
        a = find(a);
        b = find(b);
        if (a < b)
            parent_[b] = a;
        else if (b < a)
            parent_[a] = b;
    }

private:
    std::vector<std::uint32_t> parent_;
};

Result<TriSoup> buildExactMesh(const Shape& shape, const BRepBuilderAPI_Copy& copier, std::string_view where)
{
    const detail::ShapeIndex& faces = shape.impl->subs[detail::kindSlot(SubKind::Face)];
    if (faces.Extent() == 0)
        return tl::unexpected(invalid("The shape has no faces to triangulate.", where));

    std::vector<FaceMesh> meshes;
    std::vector<std::uint32_t> firstNode; // per face, index of its node 1 among all nodes
    meshes.reserve(static_cast<std::size_t>(faces.Extent()));
    std::vector<int> failed;
    std::size_t nodeCount = 0;
    for (int i = 1; i <= faces.Extent(); ++i) {
        meshes.push_back(faceMesh(copier, faces.FindKey(i)));
        firstNode.push_back(checkedU32(nodeCount));
        if (meshes.back().triangulation.IsNull())
            failed.push_back(i - 1);
        else
            nodeCount += static_cast<std::size_t>(meshes.back().triangulation->NbNodes());
    }
    checkedU32(nodeCount);
    if (!failed.empty()) {
        std::string message = "The geometry kernel could not triangulate " + std::to_string(failed.size()) +
                              " face(s) of the shape (first: face " + std::to_string(failed.front()) +
                              "); the shape cannot be machined from this triangulation.";
        return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, std::move(message), where));
    }

    // Weld through the edge polygons (file comment, WELDING).
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    NodeSets sets(nodeCount);
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> edgeIndex;
    std::vector<std::vector<std::uint32_t>> edgeNodes; // per edge: the first node seen for each polygon point
    for (std::size_t f = 0; f < meshes.size(); ++f) {
        const FaceMesh& mesh = meshes[f];
        for (TopExp_Explorer it(mesh.face, TopAbs_EDGE); it.More(); it.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(it.Current());
            const occ::handle<Poly_PolygonOnTriangulation>& polygon =
                BRep_Tool::PolygonOnTriangulation(edge, mesh.triangulation, mesh.location);
            if (polygon.IsNull() || polygon->NbNodes() <= 0)
                continue;
            const int e = edgeIndex.Add(edge);
            if (static_cast<std::size_t>(e) > edgeNodes.size())
                edgeNodes.resize(static_cast<std::size_t>(e));
            std::vector<std::uint32_t>& points = edgeNodes[static_cast<std::size_t>(e) - 1];
            if (points.empty())
                points.assign(static_cast<std::size_t>(polygon->NbNodes()), kUnset);
            if (points.size() != static_cast<std::size_t>(polygon->NbNodes()))
                return tl::unexpected(detail::makeError(
                    ErrorCode::KernelFailure,
                    "The triangulation of face " + std::to_string(f) +
                        " does not match its neighbour along an edge; the shape cannot be machined from it.",
                    where));
            const bool degenerated = BRep_Tool::Degenerated(edge);
            for (int k = 1; k <= polygon->NbNodes(); ++k) {
                const int local = polygon->Node(k);
                if (local < 1 || local > mesh.triangulation->NbNodes())
                    continue;
                const std::uint32_t node = firstNode[f] + static_cast<std::uint32_t>(local - 1);
                std::uint32_t& slot = points[degenerated ? 0 : static_cast<std::size_t>(k - 1)];
                if (slot == kUnset)
                    slot = node;
                else
                    sets.unite(slot, node);
            }
        }
    }

    TriSoup soup;
    std::vector<std::uint32_t> vertexOf(nodeCount, kUnset);
    auto vertex = [&](std::size_t f, int local) -> std::uint32_t {
        const std::uint32_t root = sets.find(firstNode[f] + static_cast<std::uint32_t>(local - 1));
        if (vertexOf[root] == kUnset) {
            // The root is the set's lowest node; find its face to read its position.
            const auto owner = static_cast<std::size_t>(std::upper_bound(firstNode.begin(), firstNode.end(), root) -
                                                        firstNode.begin() - 1);
            const FaceMesh& mesh = meshes[owner];
            const gp_Pnt point = mesh.triangulation->Node(static_cast<int>(root - firstNode[owner]) + 1)
                                     .Transformed(mesh.location.Transformation());
            vertexOf[root] = checkedU32(soup.positions.size() / 3);
            soup.positions.push_back(point.X());
            soup.positions.push_back(point.Y());
            soup.positions.push_back(point.Z());
        }
        return vertexOf[root];
    };
    for (std::size_t f = 0; f < meshes.size(); ++f) {
        const FaceMesh& mesh = meshes[f];
        const bool reversed = faces.FindKey(static_cast<int>(f) + 1).Orientation() == TopAbs_REVERSED;
        const int nodes = mesh.triangulation->NbNodes();
        for (int t = 1; t <= mesh.triangulation->NbTriangles(); ++t) {
            int a = 0;
            int b = 0;
            int c = 0;
            mesh.triangulation->Triangle(t).Get(a, b, c);
            if (a < 1 || b < 1 || c < 1 || a > nodes || b > nodes || c > nodes)
                continue;
            if (reversed)
                std::swap(b, c);
            const std::uint32_t va = vertex(f, a);
            const std::uint32_t vb = vertex(f, b);
            const std::uint32_t vc = vertex(f, c);
            if (va == vb || vb == vc || va == vc)
                continue; // collapsed at a degenerated edge
            soup.indices.push_back(va);
            soup.indices.push_back(vb);
            soup.indices.push_back(vc);
        }
    }
    return soup;
}

} // namespace

detail::MeshTeamScope::MeshTeamScope(double chordTol) noexcept
{
    try {
        const occ::handle<OSD_ThreadPool>& pool = OSD_ThreadPool::DefaultPool();
        team_ = concurrencyFor(chordTol, std::max(1, pool->NbThreads()));
        previous_ = pool->NbDefaultThreadsToLaunch();
        pool->SetNbDefaultThreadsToLaunch(team_);
    } catch (...) {
        team_ = 1;
        previous_ = -1;
    }
}

detail::MeshTeamScope::~MeshTeamScope()
{
    if (previous_ < 0)
        return;
    try {
        OSD_ThreadPool::DefaultPool()->SetNbDefaultThreadsToLaunch(previous_);
    } catch (...) {
    }
}

Result<Mesh> tessellate(const Shape& shape, const MeshOptions& options, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::tessellate";
    if (auto ok = detail::requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    if (auto ok = checkTolerances(options.chordTol, options.angleTol, where); !ok)
        return tl::unexpected(ok.error());
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());

    auto copier = meshedCopy(detail::occt(shape), options.chordTol, options.angleTol, where);
    if (!copier)
        return tl::unexpected(copier.error());
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());
    ctx.progress.set(0.9, "Preparing the display mesh");
    return detail::guarded(where, [&]() -> Result<Mesh> { return buildDisplayMesh(shape, **copier, options); });
}

Result<TriSoup> triangulateExact(const Shape& shape, double chordTol, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::triangulateExact";
    if (auto ok = detail::requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    if (auto ok = checkTolerances(chordTol, kExactAngle, where); !ok)
        return tl::unexpected(ok.error());
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());

    auto copier = meshedCopy(detail::occt(shape), chordTol, kExactAngle, where);
    if (!copier)
        return tl::unexpected(copier.error());
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());
    ctx.progress.set(0.9, "Welding the triangulation");
    return detail::guarded(where, [&]() -> Result<TriSoup> { return buildExactMesh(shape, **copier, where); });
}

// ---------- Test hooks (mesh_testing.h) ----------

namespace testing {

void setMeshInParallel(bool on) noexcept
{
    gInParallel.store(on, std::memory_order_relaxed);
}

bool meshInParallel() noexcept
{
    return gInParallel.load(std::memory_order_relaxed);
}

int meshConcurrency(double chordTol)
{
    const Result<int> team = detail::guarded("kernel::testing::meshConcurrency",
                                             [&]() -> Result<int> { return concurrencyFor(chordTol, poolThreads()); });
    return team.value_or(1);
}

bool hasMeshData(const Shape& shape)
{
    const Result<bool> found = detail::guarded("kernel::testing::hasMeshData", [&]() -> Result<bool> {
        const TopoDS_Shape& stored = detail::occt(shape);
        if (stored.IsNull())
            return false;
        for (TopExp_Explorer it(stored, TopAbs_FACE); it.More(); it.Next()) {
            TopLoc_Location location;
            if (!BRep_Tool::Triangulation(TopoDS::Face(it.Current()), location).IsNull())
                return true;
        }
        for (TopExp_Explorer it(stored, TopAbs_EDGE); it.More(); it.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(it.Current());
            TopLoc_Location location;
            if (!BRep_Tool::Polygon3D(edge, location).IsNull())
                return true;
            occ::handle<Poly_PolygonOnTriangulation> polygon;
            occ::handle<Poly_Triangulation> triangulation;
            BRep_Tool::PolygonOnTriangulation(edge, polygon, triangulation, location);
            if (!polygon.IsNull())
                return true;
        }
        return false;
    });
    return found.value_or(true);
}

} // namespace testing

} // namespace ray::kernel
