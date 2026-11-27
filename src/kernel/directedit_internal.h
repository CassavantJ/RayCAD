// Private to src/kernel (task T-080): the face analysis behind the direct edits (directedit.cpp) and their corpus
// measurement (directedit_testing.cpp). Includes OCCT headers; nothing outside src/kernel may include it (PLAN.md 0.4
// rule 1).
//
// WHAT IS RECOGNISED (the rules spike S9 measured, spikes/s9-directedit/main.cpp `analyse`; tolerances relative to
// the body's bounding-box diagonal D: lengths 1e-6 * max(1, D), parallel axes within 1e-6 rad):
//   - TANGENT EDGE: an edge between two different faces whose outward normals at the edge's middle are within
//     2 degrees of each other (cos > 0.99939).
//   - BORE: concave cylindrical faces on one cylinder (same radius, coaxial), connected through shared edges, whose
//     angular spans add up to a whole turn. Real holes are often two or more such faces (exporters split them).
//   - CAPS of a set of faces: faces all of whose neighbours are in the set (a flat or drill-point bottom), added
//     until none is left.
//   - STACK of a bore: the bore plus the coaxial concave cylinders and cones and the planes square to the axis
//     bounded by circles only, reached from it (counterbore, countersink, mouth chamfer, drill point), without the
//     planes that touch anything outside, plus caps. A bore "has a stack" when that is more than bore plus caps.
//   - FILLET: a cylindrical face under 175 degrees of arc that is not part of a bore and is tangent across straight
//     edges to two different faces that are not on its own cylinder; or a toroidal face under 175 degrees of its
//     small circle tangent to two different faces that are not on its own torus.
//   - CORNER PATCH: a face that is not a plane, cylinder or cone and not a fillet, all of whose edges are tangent and
//     which touches at least two fillets.
//   - BLEND RUN of a fillet or corner patch: the fillets and corner patches connected to it through shared edges.
// Faces are analysed when first asked about, so a question about one hole of a 600-face part costs a few faces.
#pragma once
#include "boolean_internal.h"

#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ray::kernel::detail {

// What one face is. `point` is a point inside the face (BRepClass_FaceClassifier IN) and `normal` the outward normal
// there; hasPoint is false when no such point was found (then concave is false). For a cylinder, cone or torus `axis`
// is the surface's axis (for a sphere: its centre and +Z); radius is the cylinder's or sphere's radius or the torus's
// large radius, minorRadius the torus's small one. uMin..vMax are BRepTools::UVBounds.
struct FaceFacts {
    GeomAbs_SurfaceType type = GeomAbs_OtherSurface;
    double radius = 0.0;
    double minorRadius = 0.0;
    gp_Ax1 axis;
    double uMin = 0.0, uMax = 0.0, vMin = 0.0, vMax = 0.0;
    bool hasPoint = false;
    gp_Pnt point;
    gp_Dir normal;
    bool concave = false;
};

// One edge between a face and a different face.
struct FaceLink {
    int other = -1;         // the face across the edge (index into the canonical face order)
    int edge = -1;          // the edge (index into the canonical edge order)
    bool smooth = false;    // TANGENT EDGE above
    bool line = false;      // the edge is a straight line
    bool circle = false;    // the edge is a circle
    double normalDot = 2.0; // dot of the two outward normals at the edge's middle; 2 when it could not be measured
    gp_Pnt middle;          // the point at the middle of the edge's range
};

// The axis, radius and axial extent of a bore, and whether each end is open (the bore runs out through a face wider
// than the bore) or closed (a bottom no wider than the bore closes it). t is measured along `axis` from its location.
struct HoleGeometry {
    gp_Ax1 axis;
    double radius = 0.0;
    double tMin = 0.0, tMax = 0.0;
    bool openMin = true, openMax = true;
    gp_Pnt wall; // a point on the bore wall
};

// Lazily built analysis of the faces of one Shape, in its canonical SubId order. Not thread-safe: one per call. Reads
// the shape only (it never writes into its TShapes), so it may run on a Shape other threads use. Methods may throw
// Standard_Failure: call them inside guarded().
class FaceModel {
public:
    explicit FaceModel(const Shape& shape);

    const Shape& shape() const { return shape_; }
    int faceCount() const { return static_cast<int>(facts_.size()); }
    TopoDS_Face face(int index) const;
    double diagonal() const { return diagonal_; }
    double lengthTolerance() const { return lengthTolerance_; }

    const FaceFacts& facts(int index);
    const std::vector<FaceLink>& links(int index);
    double area(int index);

    // The bore `index` belongs to (sorted face indices), or nullptr.
    const std::vector<int>* bore(int index);
    std::vector<int> withCaps(std::vector<int> faces);
    std::vector<int> stack(const std::vector<int>& bore); // sorted; a superset of withCaps(bore)
    HoleGeometry holeGeometry(const std::vector<int>& bore);
    // Every edge from the bore to a face outside it is a circle (the bore ends on plain rims).
    bool endsOnPlainCircles(const std::vector<int>& bore);

    bool isFillet(int index);
    bool isCornerPatch(int index);
    bool isBlendFace(int index) { return isFillet(index) || isCornerPatch(index); }
    // The BLEND RUN of `index` (sorted, with it), or empty if it is not a blend face.
    std::vector<int> blendRun(int index);

    // Whether two axes are the same line, within the model's tolerances.
    bool coaxial(const gp_Ax1& a, const gp_Ax1& b) const;

private:
    Shape shape_;
    double diagonal_ = 0.0;
    double lengthTolerance_ = 1e-6;
    std::vector<std::optional<FaceFacts>> facts_;
    std::vector<std::optional<std::vector<FaceLink>>> links_;
    std::vector<double> area_;                 // < 0: not measured yet
    std::vector<int> boreOf_;                  // -2 not known, -1 none, else index into bores_
    std::deque<std::vector<int>> bores_;       // a deque: bore() hands out pointers into it
    std::vector<std::int8_t> fillet_, corner_; // -1 not known
};

// Position of a point relative to a solid. On: within `onTolerance` of its boundary (measured as a distance to the
// boundary, because BRepClass3d_SolidClassifier answered IN for points on a re-cut cylinder wall in spike S9).
enum class PointState { In, Out, On, Unknown };
PointState classifyPoint(const TopoDS_Shape& solid, const gp_Pnt& point, double onTolerance);
// The classifier alone (no distance), for points known to lie well away from the boundary.
PointState classifyPointFast(const TopoDS_Shape& solid, const gp_Pnt& point, double tolerance);

// The point at parameter fraction (fu, fv) of the face's UV bounds, if it lies inside the face, with the outward
// normal.
bool facePointAt(const TopoDS_Face& face, double fu, double fv, gp_Pnt& point, gp_Dir& normal);
// A point inside the face (BRepClass_FaceClassifier IN) with the outward normal there, tried at fixed fractions of
// the UV bounds; false if none of them is inside.
bool interiorPoint(const TopoDS_Face& face, gp_Pnt& point, gp_Dir& normal);

// Tests only (directedit_testing.h DirectEditFault): how many direct edits from now on fault inside their guarded OCCT
// work, and the fault itself: a structured exception (an access violation raised with RaiseException) on Windows, a
// Standard_Failure elsewhere, where nothing could catch a real fault (directedit_fault.cpp).
std::atomic<int>& directEditFaultRequests() noexcept;
void raiseTestFault();
// Tests only (testing::RawDirectEditHistory): while above 0, demoteReversedImages leaves the history as OCCT gave it.
std::atomic<int>& rawDirectEditHistory() noexcept;
// Tests only (testing::EditEndScope): this thread's record of when its direct edits' kernel threads end their work
// (std::chrono::steady_clock ticks; kEditRunning until then); null: none is kept.
inline constexpr std::int64_t kEditRunning = INT64_MIN;
std::shared_ptr<std::atomic<std::int64_t>>& editEndRecord() noexcept;
// Tests only (testing::HoldDirectEdits): while `holds` is above 0, a direct edit's kernel thread waits before its OCCT
// work (passDirectEditGate) until the hold ends or the edit is cancelled or abandoned; `waiting` counts the edits
// there.
struct DirectEditGate {
    std::mutex mutex;
    std::condition_variable changed;
    int holds = 0;
    int waiting = 0;
};
DirectEditGate& directEditGate() noexcept;

// Distance along `axis` and from `axis`.
double axialParameter(const gp_Ax1& axis, const gp_Pnt& point);
double radialDistance(const gp_Ax1& axis, const gp_Pnt& point);
gp_Pnt footOnAxis(const gp_Ax1& axis, const gp_Pnt& point);

} // namespace ray::kernel::detail
