// RayCAD interface contract: kernel
// Module: src/kernel   Depends on: core, OCCT (the ONLY module that includes OCCT headers)
// Status: PLAN DRAFT, revision 2 (after spikes S3, S4, S9).
//
// Contract:
//  - Shape is an immutable value handle. Every operation returns a new Shape plus a ShapeHistory.
//  - Every function is safe to call from any worker thread. Meshing functions (tessellate,
//    triangulateExact) take ONE PROCESS-WIDE lock: OCCT 8.0.1 BRepMesh crashed and silently dropped
//    faces when two shapes were meshed at once (spike S3). Inside the lock OCCT's InParallel is ON
//    (bit-identical to single-threaded in S3) and the number of faces meshed at once is capped.
//  - Every function catches Standard_Failure (and structured exceptions on Windows) and returns
//    ErrorCode::KernelFailure.
//  - Every MODIFYING function validates its result (BRepCheck, closed solid, volume changed in the
//    expected direction) and detects no-ops, returning KernelFailure otherwise. OCCT reports success for
//    no-ops and for invalid bodies often enough that its own status cannot be trusted (spike S9).
//  - Every function taking JobContext reports progress and honours cancellation, with two exceptions the
//    CALLER handles by abandoning the job: the model-load stage of importStep (about 30 ms per MiB) and
//    a single slow face inside a mesh call (up to 14 s at fine tolerance). See PLAN.md section 7.
//    fillet and chamfer check cancellation between OCCT builds only: OCCT 8.0.1 ignores the progress hook
//    inside one blend build (longest whole call on the corpus 0.5 s).
//  - OCCT internal parallelism is OFF for anything that produces geometry (determinism); meshing is the
//    one exception above, and CI compares its hashes against a single-threaded run.
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
// from master on 2026-10-13 pending API review; see PLAN.md 0.2 row 2).
// Normalisation guarantees (spike S4): results that are not in the final shape are dropped; history of a
// boolean already includes its same-domain unification; every input vertex, edge, face and
// solid has exactly one Modified or Deleted entry, plus Generated where OCCT reports one; an UNCHANGED
// sub-shape is listed as Modified to its SubId in the result, because SubIds are not stable across shapes;
// result edges and vertices bounding a new blend face may have no ancestor and are named from their faces; for direct edits the edited face is reported Modified, never "Deleted plus a
// face Generated from an internal tool body" (spike S9).
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
// Healing (spike S3): None leaves some B-spline faces unmeshable; Reader is the STEP reader's built-in fix
// (1.2 to 1.9x faster than Full, results not exactly equal); Full is an explicit ShapeFix pass per solid.
enum class Heal : std::uint8_t { None, Reader, Full };
struct ImportOptions { Heal heal = Heal::Full; bool sew = true; double sewTolerance = 1e-3; };
struct ImportedBody { Shape shape; std::string name; std::array<float, 4> color; };
struct ImportResult { std::vector<ImportedBody> bodies; UnitSystem sourceUnits; std::vector<std::string> warnings; };
Result<ImportResult> importStep(const std::string& path, const ImportOptions&, JobContext&);
Result<ImportResult> importIges(const std::string& path, const ImportOptions&, JobContext&);
Status exportStep(const std::string& path, std::span<const Shape>, JobContext&);   // AP214
Status exportIges(const std::string& path, std::span<const Shape>, JobContext&);
Status exportStl (const std::string& path, std::span<const Shape>, double chordTol, JobContext&);
// The exports write `<path>.partial` and rename it over `path` when complete. Just before the rename they close
// ctx.cancel (CancelToken::close): Cancelled means no file was written, and a cancel after the close is too late.

std::vector<std::byte> serialize(const Shape&);                 // canonical binary BREP, deterministic
Result<Shape>          deserialize(std::span<const std::byte>);

// ---------- Construction ----------
struct Plane { Vec3 origin, normal, xDir; };
// A closed planar profile: outer loop first, then holes. Curves are lines, arcs and B-splines.
// ProfileCurve::pts by kind (mm, in the profile plane): Line {start, end}; Arc {start, end, centre} with
// `ccw` about the plane normal (`radius` optional, checked when given); Circle {centre} with `radius`;
// Bspline = control points of a clamped uniform curve of degree min(3, n-1). A spline with its own degree,
// knots or weights cannot be passed yet. Curves of a loop may come in any order and direction; joints must
// meet within 1e-6 mm.
struct ProfileCurve { enum class Kind { Line, Arc, Circle, Bspline } kind; std::vector<Vec2> pts; double radius{}; bool ccw{}; std::uint64_t tag{}; };
struct ProfileLoop { std::vector<ProfileCurve> curves; std::uint64_t tag{}; };   // tag echoed as SweepFaceOrigin::loopTag
struct Profile { Plane plane; std::vector<ProfileLoop> loops; };

Result<Shape> makeBox(Vec3 min, Vec3 max);
Result<Shape> makeCylinder(Vec3 base, Vec3 axis, double radius, double height);
// `tag` on each ProfileCurve is echoed in ExtrudeResult so naming can tie faces to sketch entities.
// For Side faces curveTag identifies the profile curve; for caps loopTag identifies the profile loop (region).
// Both must be stable across sketch edits; naming depends on it (PLAN.md 3.3 rule 1).
struct SweepFaceOrigin { SubId face; std::uint64_t curveTag; std::uint64_t loopTag; enum class Role { Side, StartCap, EndCap } role; };
struct SweepResult { OpResult op; std::vector<SweepFaceOrigin> origins; };
Result<SweepResult> extrude(const Profile&, double distance, double draftAngle, JobContext&);
Result<SweepResult> revolve(const Profile&, Vec3 axisOrigin, Vec3 axisDir, double angle, JobContext&);

enum class BoolOp { Union, Cut, Intersect };
Result<OpResult> boolean(BoolOp, const Shape& target, const Shape& tool, JobContext&);   // inputIndex 0=target 1=tool
// fillet/chamfer: OCCT extends the picked edges along tangent chains; `warnings` lists the extra edges blended.
Result<OpResult> fillet (const Shape&, std::span<const SubId> edges, double radius, JobContext&);
Result<OpResult> chamfer(const Shape&, std::span<const SubId> edges, double distance, JobContext&);
Result<OpResult> shell  (const Shape&, std::span<const SubId> openFaces, double thickness, JobContext&);   // v1.x
Result<OpResult> draft  (const Shape&, std::span<const SubId> faces, Plane neutral, double angle, JobContext&); // v1.x
Result<OpResult> transformed(const Shape&, const Xform&);   // rigid moves only; the identity returns the input shape
Result<OpResult> splitBody(const Shape&, Plane, JobContext&);                                              // v1.x

// ---------- Direct editing (works on history-free imported solids) ----------
// Built as measured in spike S9 (spikes/s9-directedit/REPORT.md). BRepOffset_MakeOffset is NOT used.
// deleteFaces: BRepAlgoAPI_Defeaturing. A hole is removed with its counterbore/countersink/chamfer stack,
//   falling back to the bore alone. Removing one face out of a blend run usually fails: pass the whole run.
Result<OpResult> deleteFaces(const Shape&, std::span<const SubId> faces, JobContext&);            // remove and heal
// Faces connected to `face` by tangent blends, i.e. the run a user means by "this fillet". For the UI.
std::vector<SubId> blendRun(const Shape&, SubId face);
// offsetFaces: PLANAR faces only, built as prism + boolean. If a neighbour is not square to the face the
//   result is a slab or notch with a step, and `warnings` says so (the neighbour is not extended).
Result<OpResult> offsetFaces(const Shape&, std::span<const SubId> faces, double distance, JobContext&);   // push/pull
Result<OpResult> moveFaces  (const Shape&, std::span<const SubId> faces, const Xform&, JobContext&);       // v2 full
// resizeHole: `anyFaceOfHole` is expanded to the whole cylinder (real holes are often 2+ faces).
//   Larger: boolean cut. Smaller: fuse a sleeve, falling back to defeature-then-cut.
Result<OpResult> resizeHole (const Shape&, SubId anyFaceOfHole, double newDiameter, JobContext&);
// Returns the input unchanged (with a warning) if unifying would make the solid invalid (10 of 496 in S9).
Result<OpResult> unifySameDomain(const Shape&, JobContext&);

// ---------- Tessellation and acceleration (mutating cache; ONE process-wide mesh lock, see contract) ----------
struct MeshOptions { double chordTol; double angleTol; };          // from the tier preset; display only
struct FaceRange { SubId face; std::uint32_t firstIndex, indexCount; };
struct EdgeRange { SubId edge; std::uint32_t firstVertex, vertexCount; };
struct Mesh {
    std::vector<float> positions, normals;     // xyz triples
    std::vector<std::uint32_t> indices;
    std::vector<FaceRange> faces;              // lets the renderer pick and highlight per face
    std::vector<float> edgePositions;          // polyline vertices
    std::vector<EdgeRange> edges;
    std::vector<SubId> failedFaces;            // faces BRepMesh left without triangles; shown to the user, never hidden
};
Result<Mesh> tessellate(const Shape&, const MeshOptions&, JobContext&);

// CAM-grade triangulation. Tolerance comes from the operation, NEVER from the hardware tier.
// A face without triangles is an ERROR here (CAM must not run on a mesh with holes in it).
struct TriSoup { std::vector<double> positions; std::vector<std::uint32_t> indices; };
Result<TriSoup> triangulateExact(const Shape&, double chordTol, JobContext&);

// ---------- Sections and projections used by CAM ----------
// Planar slice of a solid at height z in the given frame; returns closed loops (lines and arcs kept exact).
Result<std::vector<ProfileLoop>> sliceAt(const Shape&, const Xform& frame, double z, JobContext&);
// Silhouette of the solid projected along frame Z (for stock and containment boundaries).
Result<std::vector<ProfileLoop>> silhouette(const Shape&, const Xform& frame, JobContext&);

} // namespace ray::kernel
