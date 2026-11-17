// RayCAD interface contract: kernel
// Module: src/kernel   Depends on: core, OCCT (the ONLY module that includes OCCT headers)
// Status: PLAN DRAFT.
//
// Contract:
//  - Shape is an immutable value handle. Every operation returns a new Shape plus a ShapeHistory.
//  - Every function is safe to call from any worker thread as long as no two threads pass the
//    same Shape to a function documented as "mutating cache" (tessellate, bvh). Those take an
//    internal per-shape lock.
//  - Every function catches Standard_Failure and returns ErrorCode::KernelFailure.
//  - Every function taking JobContext honours cancellation and reports progress.
//  - OCCT internal parallelism is OFF for anything that produces geometry (determinism).
#pragma once
#include "core.h"

namespace ray::kernel {

enum class SubKind : std::uint8_t { Vertex, Edge, Face, Solid };

// Index of a sub-shape inside one Shape, in the kernel's canonical traversal order.
// Valid only for the Shape it came from. Never persisted; persistence goes through naming.h.
struct SubId {
    SubKind kind;
    std::uint32_t index;
    auto operator<=>(const SubId&) const = default;
};

class Shape {
public:
    Shape();                               // null shape
    bool isNull() const;
    std::uint32_t count(SubKind) const;
    Hash256 contentHash() const;           // hash of the canonical BREP serialisation
    struct Impl; std::shared_ptr<const Impl> impl;   // opaque; holds TopoDS_Shape
};

// How an operation changed topology. Built from BRepTools_History / Modified() / Generated() /
// IsDeleted() and normalised to SubId on both sides. BRepGraph is NOT used (withdrawn upstream
// from master on 2026-10-13 pending API review; see PLAN.md 2.2).
struct HistoryEntry {
    enum class Relation : std::uint8_t { Modified, Generated, Deleted };
    Relation relation;
    int inputIndex;                // which input Shape (0-based) the source belongs to
    SubId source;
    std::vector<SubId> results;    // empty for Deleted; sorted ascending
};
struct ShapeHistory { std::vector<HistoryEntry> entries; };

struct OpResult {
    Shape shape;
    ShapeHistory history;
    std::vector<std::string> warnings;
};

// ---------- Geometry queries ----------
enum class SurfaceType { Plane, Cylinder, Cone, Sphere, Torus, Bspline, Other };
struct SurfaceInfo {
    SurfaceType type; Vec3 origin; Vec3 axis; double radius{}; double semiAngle{};
    double area{}; Vec3 centroid; Box3 bounds; bool reversed{};
};
enum class CurveType { Line, Circle, Ellipse, Bspline, Other };
struct CurveInfo { CurveType type; Vec3 start, end, center, axis; double radius{}; double length{}; bool closed{}; };

Result<SurfaceInfo> surfaceInfo(const Shape&, SubId face);
Result<CurveInfo>   curveInfo(const Shape&, SubId edge);
Result<Vec3>        vertexPoint(const Shape&, SubId vertex);
std::vector<SubId>  adjacent(const Shape&, SubId of, SubKind wanted);   // sorted ascending
Box3                bounds(const Shape&);
struct MassProps { double volume, area; Vec3 centroid; };
Result<MassProps>   massProps(const Shape&, double density = 1.0);
struct Distance { double value; Vec3 p1, p2; };
Result<Distance>    minDistance(const Shape&, SubId a, const Shape&, SubId b);
Result<double>      minInternalCornerRadius(const Shape&, Vec3 toolAxis, JobContext&);

// ---------- Import / export ----------
struct ImportOptions { bool heal = true; bool sew = true; double sewTolerance = 1e-3; };
struct ImportedBody { Shape shape; std::string name; std::array<float, 4> color; };
struct ImportResult { std::vector<ImportedBody> bodies; UnitSystem sourceUnits; std::vector<std::string> warnings; };
Result<ImportResult> importStep(const std::string& path, const ImportOptions&, JobContext&);
Result<ImportResult> importIges(const std::string& path, const ImportOptions&, JobContext&);
Status exportStep(const std::string& path, std::span<const Shape>, JobContext&);   // AP214
Status exportIges(const std::string& path, std::span<const Shape>, JobContext&);
Status exportStl (const std::string& path, std::span<const Shape>, double chordTol, JobContext&);

std::vector<std::byte> serialize(const Shape&);                 // canonical binary BREP, deterministic
Result<Shape>          deserialize(std::span<const std::byte>);

// ---------- Construction ----------
struct Plane { Vec3 origin, normal, xDir; };
// A closed planar profile: outer loop first, then holes. Curves are lines, arcs and B-splines.
struct ProfileCurve { enum class Kind { Line, Arc, Circle, Bspline } kind; std::vector<Vec2> pts; double radius{}; bool ccw{}; std::uint64_t tag{}; };
struct ProfileLoop { std::vector<ProfileCurve> curves; };
struct Profile { Plane plane; std::vector<ProfileLoop> loops; };

Result<Shape> makeBox(Vec3 min, Vec3 max);
Result<Shape> makeCylinder(Vec3 base, Vec3 axis, double radius, double height);
// `tag` on each ProfileCurve is echoed in ExtrudeResult so naming can tie faces to sketch entities.
struct SweepFaceOrigin { SubId face; std::uint64_t curveTag; enum class Role { Side, StartCap, EndCap } role; };
struct SweepResult { OpResult op; std::vector<SweepFaceOrigin> origins; };
Result<SweepResult> extrude(const Profile&, double distance, double draftAngle, JobContext&);
Result<SweepResult> revolve(const Profile&, Vec3 axisOrigin, Vec3 axisDir, double angle, JobContext&);

enum class BoolOp { Union, Cut, Intersect };
Result<OpResult> boolean(BoolOp, const Shape& target, const Shape& tool, JobContext&);   // inputIndex 0=target 1=tool
Result<OpResult> fillet (const Shape&, std::span<const SubId> edges, double radius, JobContext&);
Result<OpResult> chamfer(const Shape&, std::span<const SubId> edges, double distance, JobContext&);
Result<OpResult> shell  (const Shape&, std::span<const SubId> openFaces, double thickness, JobContext&);   // v1.x
Result<OpResult> draft  (const Shape&, std::span<const SubId> faces, Plane neutral, double angle, JobContext&); // v1.x
Result<OpResult> transformed(const Shape&, const Xform&);
Result<OpResult> splitBody(const Shape&, Plane, JobContext&);                                              // v1.x

// ---------- Direct editing (works on history-free imported solids) ----------
Result<OpResult> deleteFaces(const Shape&, std::span<const SubId> faces, JobContext&);            // remove and heal
Result<OpResult> offsetFaces(const Shape&, std::span<const SubId> faces, double distance, JobContext&);   // push/pull
Result<OpResult> moveFaces  (const Shape&, std::span<const SubId> faces, const Xform&, JobContext&);       // v2 full
Result<OpResult> resizeHole (const Shape&, SubId cylinderFace, double newDiameter, JobContext&);
Result<OpResult> unifySameDomain(const Shape&, JobContext&);

// ---------- Tessellation and acceleration (mutating cache; internally locked) ----------
struct MeshOptions { double chordTol; double angleTol; };          // from the tier preset; display only
struct FaceRange { SubId face; std::uint32_t firstIndex, indexCount; };
struct EdgeRange { SubId edge; std::uint32_t firstVertex, vertexCount; };
struct Mesh {
    std::vector<float> positions, normals;     // xyz triples
    std::vector<std::uint32_t> indices;
    std::vector<FaceRange> faces;              // lets the renderer pick and highlight per face
    std::vector<float> edgePositions;          // polyline vertices
    std::vector<EdgeRange> edges;
};
Result<Mesh> tessellate(const Shape&, const MeshOptions&, JobContext&);

// CAM-grade triangulation. Tolerance comes from the operation, NEVER from the hardware tier.
struct TriSoup { std::vector<double> positions; std::vector<std::uint32_t> indices; };
Result<TriSoup> triangulateExact(const Shape&, double chordTol, JobContext&);

// ---------- Sections and projections used by CAM ----------
// Planar slice of a solid at height z in the given frame; returns closed loops (lines and arcs kept exact).
Result<std::vector<ProfileLoop>> sliceAt(const Shape&, const Xform& frame, double z, JobContext&);
// Silhouette of the solid projected along frame Z (for stock and containment boundaries).
Result<std::vector<ProfileLoop>> silhouette(const Shape&, const Xform& frame, JobContext&);

} // namespace ray::kernel
