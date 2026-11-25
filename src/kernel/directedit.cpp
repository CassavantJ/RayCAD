// Direct edits on history-free solids: deleteFaces, offsetFaces, resizeHole, unifySameDomain, blendRun (task T-080).
// Built as spike S9 measured them (spikes/s9-directedit/REPORT.md); BRepOffset_MakeOffset is not used.
//
// DELETE FACES. BRepAlgoAPI_Defeaturing on a private copy (boolean_internal.h PrivateCopy). A picked face of a hole
// (directedit_internal.h BORE, or a face of its STACK) stands for the whole hole: the call first removes the stack
// (counterbore, countersink, mouth chamfer, drill point: 63 of 63 in S9), and if that fails the drilled bore with its
// bottom alone (the narrowest bore of the stack), with a warning that the rest was left. Other faces are removed as
// picked; a single fillet out of a longer blend usually cannot be removed alone (11 of 62 in S9), and the message then
// says to select the whole blend (blendRun gives it). A plain round between two flat faces that Defeaturing refuses is
// tried once more by rebuilding the sharp corner with a boolean (FILLET WEDGES below).
//
// OFFSET FACES (push/pull). Planar faces only. Each face is swept along its outward normal by `distance` into a prism
// (BRepPrimAPI_MakePrism of a copy of the face); positive distances fuse the prisms onto the body, negative ones cut
// them away (kernel::boolean, with its checks and its same-domain unification). Where a neighbouring face is square to
// the moved face this is the true offset; where it is tangent or at another angle the neighbour is not extended and
// the result is a slab or notch with a step there, which `warnings` says.
//
// RESIZE HOLE. The picked face is widened to its bore (a hole is often two or more faces). Larger: kernel::boolean cuts
// a cylinder of the new radius over the bore's length, running on past open ends. Smaller: a sleeve (old radius
// outside, new inside) is fused into the bore when both bore ends are plain circles; otherwise, or if that fails, the
// bore and its bottom are removed by Defeaturing and the hole is cut again at the new radius (173 of 176 in S9). The
// bottom of a blind hole keeps its shape (a drill point stays the old size; S9 did not test rescaling it).
//
// UNIFY SAME DOMAIN. ShapeUpgrade_UnifySameDomain on a private copy. If the result fails validation the input is
// returned unchanged with a warning (10 of 496 valid solids became invalid in S9); if nothing could be merged the call
// fails (no success is reported for a no-op).
//
// VALIDATION (PLAN.md section 7 rule 6). OCCT's status is not trusted: Defeaturing reported success while doing
// nothing in 69 of 489 attempts in S9 and returned a wrong or invalid body in 22. Every result must be valid closed
// solids (validateSolids), as many as the input had, and not a no-op. Deletes also require that every face asked for
// is gone (Deleted in OCCT's history) and that the volume moved the way the recognised features say: filling a hole
// or a pocket adds material, removing a convex fillet adds material and a concave one takes it away; a point inside
// each removed hole face or analytic fillet face must now lie inside (material added) or outside (taken away) the
// result; and for holes and blends the volume change must be plausible for their size (the bounds S9 scored with).
// Resizes require the new wall to be on the result where it was asked for; push/pull requires every moved face to be
// on the result.
//
// HISTORY (kernel.h ShapeHistory). Built by buildHistory from the OCCT makers and mapped back to the caller's SubIds.
// For resize and push/pull OCCT reports the edited face as removed and the new face as an image of the internal tool
// body; that is rewritten so the edited face is Modified to its new face(s) (S9: recoverable in 1,620 of 1,620): a
// moved face to the image of its prism's end cap, a bore face to the pieces of the new wall that lie over it. Other
// faces of the tool that end up on the result (a prism's sides, a sleeve's rims), where no input face already accounts
// for them, are Generated from the edge of the moved face they were swept from, or from the edited hole face. The tool
// itself never appears as an input. Deleted faces are Deleted. An unchanged element is listed as Modified to its SubId
// in the result (the normalisation every kernel history follows; SubIds are not kept across shapes). A face that
// Defeaturing lists as an old face's Modified image but that lies on the old face's surface looking the other way (the
// cap it puts on what is left of a hole, on the plane of a counterbore floor that is now inside the material) is not
// that face, nor is an image that shares no ground with the old face: either is Generated from it, and the old
// face Deleted if nothing else is its image. The same holds for the faces a push/pull or a resize does not move (a
// small face that a push/pull's boolean listed as Modified into a face 4 mm away). T-081b: naming would otherwise
// resolve a reference to the floor to a face on the other side of the material, or a reference to a fillet to a face
// somewhere else.
//
// CANCEL AND ABANDON. Defeaturing can run for minutes on some fillets (S9: over 150 s) and polls OCCT's progress hook
// rarely (S9: up to 9.4 s from cancel to return). So every direct edit runs its OCCT work on a kernel-owned thread
// (io_internal.h runAbandonable, as the imports do): the caller polls the CancelToken every 10 ms and returns
// Error{Cancelled} at once, and the thread stops at OCCT's next poll and throws its result away. Structured exceptions
// on that thread are caught by guarded() like everywhere else in the kernel. OCCT 8.0.1 ran the face trim of a feature
// removal without a progress range, so a cancel there waited for the trim to end (237 s on a NIST CTC 05 fillet);
// ports/opencascade/0011-bopalgo-removefeatures-cancel.patch passes the range down to the edge/face searches.
#include "directedit_internal.h"

#include "io_internal.h"

#include <BOPAlgo_Alerts.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_Shell.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ray::kernel {

namespace detail {
std::atomic<int>& directEditFaultRequests() noexcept
{
    static std::atomic<int> count{0};
    return count;
}
} // namespace detail

namespace {

using detail::FaceModel;
using detail::HoleGeometry;
using detail::kernelFailure;

constexpr std::string_view kDeleteWhere = "kernel::deleteFaces";
constexpr std::string_view kOffsetWhere = "kernel::offsetFaces";
constexpr std::string_view kResizeWhere = "kernel::resizeHole";
constexpr std::string_view kUnifyWhere = "kernel::unifySameDomain";

constexpr const char* kStage = "Editing the body";
// Volume changes at or below this fraction of the body's volume count as no change (spike S9).
constexpr double kNoOpRelative = 1e-9;
// A neighbour is square to a moved face when their normals are within this of perpendicular (sin of 1 degree).
constexpr double kSquareDot = 0.0175;
// Smallest push/pull distance and smallest change of hole diameter (mm).
constexpr double kMinDistance = 1e-6;
// Tolerance of the "point on the result" checks (mm; spike S9).
constexpr double kOnTolerance = 1e-5;
// How far a hole-enlarging cylinder runs on past an open end, beyond its own radius (mm; spike S9).
constexpr double kOpenEndMargin = 0.1;

const char* const kNothingChanged = " Nothing was changed.";

Error invalidArgument(std::string message, std::string_view where)
{
    return detail::makeError(ErrorCode::InvalidArgument, std::move(message), where);
}

Error failure(std::string message, std::string_view where, std::string context = {})
{
    Error error = kernelFailure(std::move(message), where);
    if (!context.empty())
        error.context.insert(error.context.begin(), std::move(context));
    return error;
}

// "12.5 mm": up to six significant digits, independent of the locale.
std::string mm(double value)
{
    std::array<char, 32> buffer{};
    const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, 6);
    return std::string(buffer.data(), end.ptr) + " mm";
}

// "face 4", "faces 4 and 9", "faces 4, 9 and 12".
std::string faceList(const std::vector<int>& faces)
{
    std::string text = faces.size() == 1 ? "face " : "faces ";
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (i > 0)
            text += i + 1 == faces.size() ? " and " : ", ";
        text += std::to_string(faces[i]);
    }
    return text;
}

void sortUnique(std::vector<SubId>& ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

std::vector<int> sortedUnique(std::vector<int> v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// Checks the arguments every direct edit shares: a body with at least one solid and faces of it.
Status checkBodyAndFaces(const Shape& shape, std::span<const SubId> faces, bool needFaces, std::string_view where)
{
    if (shape.isNull() || shape.count(SubKind::Solid) == 0)
        return tl::unexpected(invalidArgument("The body to edit must be a solid.", where));
    if (needFaces && faces.empty())
        return tl::unexpected(invalidArgument("Select at least one face.", where));
    for (const SubId& id : faces)
        if (id.kind != SubKind::Face || id.index >= shape.count(SubKind::Face))
            return tl::unexpected(invalidArgument("The selection names a face the body does not have.", where));
    return std::monostate{};
}

// ---------- the edit thread ----------

// Runs work(JobContext&) -> Result<OpResult> on a kernel-owned thread under guarded() (see "CANCEL AND ABANDON").
// `work` must own everything it uses.
template <class Work>
Result<OpResult> runEdit(JobContext& ctx, std::string_view where, Work work)
{
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    CancelToken token = ctx.cancel;
    Result<OpResult> result = detail::runAbandonable<OpResult>(
        ctx, where, [token, where, work = std::move(work)](detail::IoTask& task) mutable -> Result<OpResult> {
            Progress progress([&task](double fraction, std::string_view) { task.report(fraction, kStage); });
            JobContext inner{token, progress};
            return detail::guarded(where, [&]() -> Result<OpResult> {
                if (detail::directEditFaultRequests().load(std::memory_order_relaxed) > 0 &&
                    detail::directEditFaultRequests().fetch_sub(1, std::memory_order_relaxed) > 0)
                    detail::raiseTestFault();
                return work(inner);
            });
        });
    if (!result && result.error().code == ErrorCode::KernelFailure &&
        result.error().message.rfind("The geometry kernel failed:", 0) == 0) {
        // An exception inside OCCT (guarded()): say what it means for the user, keep OCCT's words for the log.
        Error& error = result.error();
        error.context.insert(error.context.begin(), error.message);
        error.message = std::string("The geometry kernel failed on this edit.") + kNothingChanged;
    }
    if (result)
        ctx.progress.set(1.0, "Done");
    return result;
}

// ---------- history ----------

// The entries of `history` that belong to input `input`.
ShapeHistory entriesOf(const ShapeHistory& history, int input)
{
    ShapeHistory out;
    for (const HistoryEntry& e : history.entries)
        if (e.inputIndex == input)
            out.entries.push_back(e);
    return out;
}

// Canonical order: by input, then source; a source's Modified or Deleted entry before its Generated one. Generated
// entries of one source are merged.
void canonicalise(ShapeHistory& history)
{
    std::map<std::pair<int, SubId>, std::size_t> generated;
    std::vector<HistoryEntry> merged;
    for (HistoryEntry& e : history.entries) {
        sortUnique(e.results);
        if (e.relation == HistoryEntry::Relation::Generated) {
            const auto key = std::make_pair(e.inputIndex, e.source);
            if (const auto it = generated.find(key); it != generated.end()) {
                std::vector<SubId>& into = merged[it->second].results;
                into.insert(into.end(), e.results.begin(), e.results.end());
                sortUnique(into);
                continue;
            }
            generated.emplace(key, merged.size());
        }
        merged.push_back(std::move(e));
    }
    std::stable_sort(merged.begin(), merged.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
        if (a.inputIndex != b.inputIndex)
            return a.inputIndex < b.inputIndex;
        if (a.source != b.source)
            return a.source < b.source;
        const bool ga = a.relation == HistoryEntry::Relation::Generated;
        const bool gb = b.relation == HistoryEntry::Relation::Generated;
        return !ga && gb;
    });
    history.entries = std::move(merged);
}

// The history of two steps in a row: `first` maps the input to an intermediate shape, `second` (entries of its input
// 0) maps that to the result. An input element is Modified to the images of its intermediate images, else Deleted; it
// is Generated to what its intermediate images generated and to the images of what it generated.
ShapeHistory compose(const ShapeHistory& first, const ShapeHistory& second)
{
    std::map<SubId, std::vector<SubId>> modified2, generated2;
    for (const HistoryEntry& e : second.entries) {
        if (e.inputIndex != 0)
            continue;
        if (e.relation == HistoryEntry::Relation::Modified)
            modified2[e.source] = e.results;
        else if (e.relation == HistoryEntry::Relation::Generated)
            generated2[e.source] = e.results;
    }
    const auto append = [](std::vector<SubId>& into, const std::map<SubId, std::vector<SubId>>& from, SubId key) {
        if (const auto it = from.find(key); it != from.end())
            into.insert(into.end(), it->second.begin(), it->second.end());
    };
    ShapeHistory out;
    for (const HistoryEntry& e : first.entries) {
        if (e.inputIndex != 0)
            continue;
        std::vector<SubId> generated;
        if (e.relation == HistoryEntry::Relation::Generated) {
            for (const SubId& m : e.results) {
                append(generated, modified2, m);
                append(generated, generated2, m);
            }
        } else {
            std::vector<SubId> modified;
            if (e.relation == HistoryEntry::Relation::Modified)
                for (const SubId& m : e.results) {
                    append(modified, modified2, m);
                    append(generated, generated2, m);
                }
            sortUnique(modified);
            if (modified.empty())
                out.entries.push_back({HistoryEntry::Relation::Deleted, 0, e.source, {}});
            else
                out.entries.push_back({HistoryEntry::Relation::Modified, 0, e.source, std::move(modified)});
        }
        sortUnique(generated);
        if (!generated.empty())
            out.entries.push_back({HistoryEntry::Relation::Generated, 0, e.source, std::move(generated)});
    }
    canonicalise(out);
    return out;
}

// See "HISTORY" in the file comment. `body` holds the entries of the edited body (input 0, the caller's SubIds);
// `boolean` the whole history of the boolean with the tool (input 1). `edited` names the input faces the edit moved
// and the result faces they became; `fromTool` names, for tool faces, the input element their uncovered images are
// Generated from; any other uncovered image of a tool face is Generated from `fallback`.
struct EditedFace {
    SubId source;
    std::vector<SubId> results;
};
struct ToolOrigin {
    SubId toolFace;
    SubId source;
};
ShapeHistory rewriteToolHistory(ShapeHistory body, const ShapeHistory& boolean, std::span<const EditedFace> edited,
                                std::span<const ToolOrigin> fromTool, SubId fallback)
{
    for (HistoryEntry& e : body.entries) {
        if (e.relation == HistoryEntry::Relation::Generated)
            continue;
        for (const EditedFace& face : edited)
            if (face.source == e.source) {
                if (e.relation != HistoryEntry::Relation::Modified)
                    e.results.clear();
                e.relation = HistoryEntry::Relation::Modified;
                e.results.insert(e.results.end(), face.results.begin(), face.results.end());
                sortUnique(e.results);
            }
    }
    std::set<SubId> covered;
    for (const HistoryEntry& e : body.entries)
        if (e.relation != HistoryEntry::Relation::Deleted)
            for (const SubId& r : e.results)
                if (r.kind == SubKind::Face)
                    covered.insert(r);
    std::map<SubId, std::vector<SubId>> extra;
    for (const HistoryEntry& e : boolean.entries) {
        if (e.inputIndex != 1 || e.source.kind != SubKind::Face || e.relation == HistoryEntry::Relation::Deleted)
            continue;
        SubId source = fallback;
        for (const ToolOrigin& origin : fromTool)
            if (origin.toolFace == e.source)
                source = origin.source;
        for (const SubId& r : e.results)
            if (r.kind == SubKind::Face && covered.insert(r).second)
                extra[source].push_back(r);
    }
    for (auto& [source, results] : extra)
        body.entries.push_back({HistoryEntry::Relation::Generated, 0, source, std::move(results)});
    canonicalise(body);
    return body;
}

// Does face `image` lie on the surface of face `source` looking the other way (its material on the other side)?
// False when that cannot be told (no interior point, a point off the surface, no normal there). May throw.
bool looksTheOtherWay(const TopoDS_Face& source, const TopoDS_Face& image)
{
    gp_Pnt point;
    gp_Dir normal;
    if (!detail::interiorPoint(image, point, normal))
        return false;
    const occ::handle<Geom_Surface> surface = BRep_Tool::Surface(source);
    if (surface.IsNull())
        return false;
    GeomAPI_ProjectPointOnSurf projection(point, surface);
    if (!projection.IsDone() || projection.NbPoints() == 0 || projection.LowerDistance() > kOnTolerance)
        return false;
    double u = 0.0, v = 0.0;
    projection.LowerDistanceParameters(u, v);
    GeomLProp_SLProps props(surface, u, v, 1, Precision::Confusion());
    if (!props.IsNormalDefined())
        return false;
    gp_Dir own = props.Normal();
    if (source.Orientation() == TopAbs_REVERSED)
        own.Reverse();
    return own.Dot(normal) < 0.0;
}

// Is `point` on `face` (within the tolerance of the "point on the result" checks)? May throw.
bool onFace(const TopoDS_Face& face, const gp_Pnt& point)
{
    BRepExtrema_DistShapeShape measure(BRepBuilderAPI_MakeVertex(point).Vertex(), face);
    return measure.IsDone() && measure.NbSolution() > 0 && measure.Value() < kOnTolerance;
}

// Does face `image` share no ground with face `source`: their boxes (with the tolerance) do not meet, or neither face
// holds a point inside the other? False when that cannot be told (no interior point). May throw.
bool liesApart(const TopoDS_Face& source, const TopoDS_Face& image)
{
    Bnd_Box a, b;
    BRepBndLib::AddOptimal(source, a, false, false);
    BRepBndLib::AddOptimal(image, b, false, false);
    a.Enlarge(kOnTolerance);
    b.Enlarge(kOnTolerance);
    if (a.IsVoid() || b.IsVoid())
        return false;
    if (a.IsOut(b))
        return true;
    gp_Pnt p, q;
    gp_Dir n;
    if (!detail::interiorPoint(image, p, n) || onFace(source, p))
        return false;
    return detail::interiorPoint(source, q, n) && !onFace(image, q);
}

// See "HISTORY": Defeaturing may close what is left of a cavity with the surface of a face that is no longer on the
// body, the new face looking the other way (removing a counterbore's wall caps the bore with the plane of the
// counterbore floor, looking down; the floor, which looked up, is inside the material now), and its history then
// lists that face as the old face's Modified image. It may also list as an old face's image a face on its surface
// that shares no ground with it (a fillet along a removed pocket's edge, gone, and its surface run on elsewhere; a
// piece of a wall\n// listed as the image of the piece beside it). Neither is the old face (a removal does not move
// faces: an image keeps some of its ground): such an image is reported Generated from it, and the old face Deleted when
// nothing else is its image. `history` holds the caller's SubIds of `input`.
void demoteReversedImages(ShapeHistory& history, const Shape& input, const Shape& result,
                          std::span<const int> moved = {})
{
    const detail::ShapeIndex& inFaces = input.impl->subs[detail::kindSlot(SubKind::Face)];
    const detail::ShapeIndex& outFaces = result.impl->subs[detail::kindSlot(SubKind::Face)];
    std::vector<HistoryEntry> generated;
    for (HistoryEntry& e : history.entries) {
        if (e.inputIndex != 0 || e.source.kind != SubKind::Face || e.relation != HistoryEntry::Relation::Modified ||
            std::find(moved.begin(), moved.end(), static_cast<int>(e.source.index)) != moved.end())
            continue;
        const TopoDS_Face source = TopoDS::Face(inFaces.FindKey(static_cast<int>(e.source.index) + 1));
        std::vector<SubId> kept, reversed;
        for (const SubId& r : e.results) {
            if (r.kind != SubKind::Face) {
                kept.push_back(r);
                continue;
            }
            const TopoDS_Face image = TopoDS::Face(outFaces.FindKey(static_cast<int>(r.index) + 1));
            const bool other = looksTheOtherWay(source, image) || liesApart(source, image);
            (other ? reversed : kept).push_back(r);
        }
        if (reversed.empty())
            continue;
        e.results = std::move(kept);
        if (e.results.empty())
            e.relation = HistoryEntry::Relation::Deleted;
        generated.push_back({HistoryEntry::Relation::Generated, 0, e.source, std::move(reversed)});
    }
    if (generated.empty())
        return;
    history.entries.insert(history.entries.end(), generated.begin(), generated.end());
    canonicalise(history);
}

// The result faces of the tool face `toolFace` (its Modified entry in a boolean's history).
std::vector<SubId> toolImages(const ShapeHistory& boolean, SubId toolFace)
{
    for (const HistoryEntry& e : boolean.entries)
        if (e.inputIndex == 1 && e.source == toolFace && e.relation == HistoryEntry::Relation::Modified)
            return e.results;
    return {};
}

// The relation of the primary (Modified or Deleted) entry of `source`.
std::optional<HistoryEntry::Relation> primaryRelation(const ShapeHistory& history, SubId source)
{
    for (const HistoryEntry& e : history.entries)
        if (e.inputIndex == 0 && e.source == source && e.relation != HistoryEntry::Relation::Generated)
            return e.relation;
    return std::nullopt;
}

// ShapeAnalysis_Shell finds no free and no badly oriented edge. validateSolids' incremental BRepCheck passed two
// blend-run removals on NIST CTC 02 whose shells had badly oriented edges (T-080 measurement), so the direct edits
// check this too. May throw.
bool orientedAndClosed(const TopoDS_Shape& shape)
{
    ShapeAnalysis_Shell shells;
    shells.LoadShells(shape);
    shells.CheckOrientedShells(shape, true);
    return !shells.HasFreeEdges() && !shells.HasBadEdges();
}

void addOrphanWarning(OpResult& out)
{
    const std::vector<SubId> orphans = detail::orphanFaces(out.history, out.shape);
    if (!orphans.empty())
        out.warnings.push_back(detail::orphanWarning(orphans));
}

// ---------- delete ----------

// What a set of removed faces must do to the body (see "VALIDATION").
struct Expectation {
    int sign = 0;                                              // +1 the volume must grow, -1 shrink, 0 either
    bool bounded = true;                                       // lo and hi apply
    double lo = 0.0, hi = 0.0;                                 // bounds of |volume change|
    std::vector<std::pair<gp_Pnt, detail::PointState>> points; // where material must now be (In) or not (Out)
};

// A hole picked for deletion: its stack, and the drilled bore with its bottom as the fall-back.
struct HolePick {
    std::vector<int> stack;
    std::vector<int> bore;
};

void addHoleExpectation(FaceModel& model, const std::vector<int>& faces, Expectation& expect)
{
    gp_Ax1 axis;
    double widest = 0.0;
    for (const int k : faces)
        if (model.facts(k).type == GeomAbs_Cylinder && model.facts(k).radius > widest) {
            widest = model.facts(k).radius;
            axis = model.facts(k).axis;
        }
    double tMin = 1e300, tMax = -1e300, reach = 0.0;
    for (const int k : faces) {
        for (TopExp_Explorer it(model.face(k), TopAbs_VERTEX); it.More(); it.Next()) {
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
            tMin = std::min(tMin, detail::axialParameter(axis, p));
            tMax = std::max(tMax, detail::axialParameter(axis, p));
            reach = std::max(reach, detail::radialDistance(axis, p));
        }
        if (model.facts(k).hasPoint)
            expect.points.emplace_back(model.facts(k).point, detail::PointState::In);
    }
    const double radius = std::max(widest, reach); // countersinks and chamfers reach wider than the bore
    const double reference = 3.14159265358979323846 * radius * radius * std::max(0.0, tMax - tMin);
    expect.lo += 0.1 * reference;
    expect.hi += 1.05 * reference;
}

// The expectation of removing `faces`: holes (with what belongs to them) and blends are recognised, anything else
// only has to be gone.
Expectation expectationFor(FaceModel& model, const std::vector<int>& faces, const std::vector<HolePick>& holes)
{
    Expectation expect;
    std::set<int> rest(faces.begin(), faces.end());
    int signs = 0; // bit 1: something adds material, bit 2: something takes it away, bit 4: something unknown
    for (const HolePick& hole : holes) {
        std::vector<int> part;
        for (const int k : faces)
            if (std::binary_search(hole.stack.begin(), hole.stack.end(), k))
                part.push_back(k);
        if (part.empty())
            continue;
        addHoleExpectation(model, part, expect);
        for (const int k : part)
            rest.erase(k);
        signs |= 1;
    }
    // Blends are judged as blends only when nothing else is removed with them: a pocket's corner rounds go with its
    // walls and are filled with them, not sharpened.
    if (std::any_of(rest.begin(), rest.end(), [&](int k) { return !model.isBlendFace(k); })) {
        expect.sign = 0;
        expect.bounded = false;
        return expect;
    }
    double blendSize = 0.0;
    double borrowedRadius = 0.0;
    bool anyBlend = false;
    for (const int k : rest)
        if (model.isFillet(k)) {
            const detail::FaceFacts& x = model.facts(k);
            borrowedRadius = std::max(borrowedRadius, x.type == GeomAbs_Torus ? x.minorRadius : x.radius);
        }
    for (auto it = rest.begin(); it != rest.end();) {
        const int k = *it;
        if (!model.isBlendFace(k)) {
            ++it;
            continue;
        }
        anyBlend = true;
        const detail::FaceFacts& x = model.facts(k);
        const bool analytic = x.type == GeomAbs_Cylinder || x.type == GeomAbs_Torus || x.type == GeomAbs_Sphere;
        const double radius = !analytic ? borrowedRadius : (x.type == GeomAbs_Torus ? x.minorRadius : x.radius);
        blendSize += model.area(k) * radius;
        if (analytic && x.hasPoint) {
            // A convex blend removed: the old surface point ends up inside new material; a concave one: outside.
            expect.points.emplace_back(x.point, x.concave ? detail::PointState::Out : detail::PointState::In);
            signs |= x.concave ? 2 : 1;
        }
        it = rest.erase(it);
    }
    if (anyBlend) {
        expect.lo += 1e-4 * blendSize;
        expect.hi += 1.3 * blendSize;
    }
    if (!rest.empty()) {
        signs |= 4;
        expect.bounded = false;
    }
    expect.sign = signs == 1 ? 1 : (signs == 2 ? -1 : 0);
    return expect;
}

enum class Why { None, Cancelled, NotRemoved, Invalid, Pieces, Wrong };

struct DeleteOutcome {
    std::optional<OpResult> result;
    Why why = Why::None;
    std::string detail; // for the log
};

// One Defeaturing attempt on `faces` (input face indices).
DeleteOutcome defeature(const Shape& input, const std::vector<int>& faces, const Expectation& expect, double fuzzy,
                        JobContext& ctx, std::string_view where)
{
    DeleteOutcome outcome;
    const auto fail = [&](Why why, std::string detail) {
        outcome.why = why;
        outcome.detail = std::move(detail);
        return outcome;
    };
    auto copy = detail::privateCopy(input, true, where);
    if (!copy)
        return fail(Why::Invalid, copy.error().message);
    std::vector<int> toCopy(input.count(SubKind::Face), -1);
    for (std::size_t c = 0; c < copy->toInput[detail::kindSlot(SubKind::Face)].size(); ++c)
        toCopy[copy->toInput[detail::kindSlot(SubKind::Face)][c]] = static_cast<int>(c);
    const detail::ShapeIndex& copyFaces = copy->shape.impl->subs[detail::kindSlot(SubKind::Face)];

    BRepAlgoAPI_Defeaturing maker;
    maker.SetShape(detail::occt(copy->shape));
    for (const int k : faces)
        maker.AddFaceToRemove(copyFaces.FindKey(toCopy[static_cast<std::size_t>(k)] + 1));
    maker.SetRunParallel(false);
    maker.SetToFillHistory(true);
    if (fuzzy > 0.0)
        maker.SetFuzzyValue(fuzzy);
    occ::handle<detail::JobIndicator> indicator = new detail::JobIndicator(ctx, 0.05, 0.85, "Removing faces");
    maker.Build(indicator->Start());
    if (ctx.cancel.cancelled() || maker.HasError(STANDARD_TYPE(BOPAlgo_AlertUserBreak)))
        return fail(Why::Cancelled, "cancelled");
    if (!maker.IsDone() || maker.HasErrors()) {
        std::ostringstream dump;
        maker.DumpErrors(dump);
        return fail(Why::NotRemoved, "OCCT: " + dump.str());
    }
    ctx.progress.set(0.88, "Checking the result");
    const TopoDS_Shape solids = detail::normaliseSolids(maker.Shape());
    if (solids.IsNull())
        return fail(Why::Invalid, "no solid");
    const std::array<const Shape*, 1> originals{&input};
    const std::array<TopoDS_Shape, 1> copies{detail::occt(copy->shape)};
    auto checked = detail::validateSolids(solids, "Removing the faces", where,
                                          detail::allValidated(originals) ? std::span<const TopoDS_Shape>(copies)
                                                                          : std::span<const TopoDS_Shape>());
    if (!checked)
        return fail(Why::Invalid, checked.error().message);
    if (!orientedAndClosed(solids))
        return fail(Why::Invalid, "free or badly oriented edges");
    const double before = detail::volumeOf(input);
    const double after = *checked;
    OpResult out;
    out.shape = detail::wrap(solids);
    if (out.shape.count(SubKind::Solid) != input.count(SubKind::Solid))
        return fail(Why::Pieces, std::to_string(out.shape.count(SubKind::Solid)) + " solids");
    const std::array<const Shape*, 1> inputs{&copy->shape};
    out.history = detail::buildHistory(maker, inputs, out.shape);
    if (auto mapped = detail::remapSources(out.history, 0, *copy, where); !mapped)
        return fail(Why::Invalid, mapped.error().message);
    demoteReversedImages(out.history, input, out.shape);

    const double change = after - before;
    if (std::abs(change) <= kNoOpRelative * std::max(1.0, std::abs(before)))
        return fail(Why::NotRemoved, "volume unchanged");
    for (const int k : faces)
        if (primaryRelation(out.history, SubId{SubKind::Face, static_cast<std::uint32_t>(k)}) !=
            HistoryEntry::Relation::Deleted)
            return fail(Why::NotRemoved, "face " + std::to_string(k) + " is still there");
    if (expect.sign != 0 && change * expect.sign < 0.0)
        return fail(Why::Wrong,
                    "volume " + std::to_string(before) + " -> " + std::to_string(after) + " moved the wrong way");
    if (expect.bounded && (std::abs(change) < expect.lo || std::abs(change) > expect.hi))
        return fail(Why::Wrong, "volume change " + std::to_string(change) + " outside " + std::to_string(expect.lo) +
                                    ".." + std::to_string(expect.hi));
    for (const auto& [point, want] : expect.points) {
        const detail::PointState got = detail::classifyPointFast(solids, point, kOnTolerance);
        if (got != want)
            return fail(Why::Wrong, "a point of a removed face is on the wrong side of the result");
    }
    detail::markValidated(out.shape, after);
    addOrphanWarning(out);
    outcome.result = std::move(out);
    return outcome;
}

std::string deleteMessage(Why why, bool runHint)
{
    if (runHint && (why == Why::NotRemoved || why == Why::Invalid || why == Why::Wrong))
        return std::string("This fillet runs into other fillets and can't be removed on its own. Select the whole "
                           "blend (all the fillets it joins) and try again.") +
               kNothingChanged;
    switch (why) {
    case Why::Invalid:
        return std::string("Removing these faces gave a body that failed its check, so it was discarded.") +
               kNothingChanged;
    case Why::Pieces:
        return std::string("Removing these faces would leave the part in separate pieces.") + kNothingChanged;
    case Why::Wrong:
        return std::string("Removing these faces did not give the expected shape (material would be added or "
                           "removed somewhere else), so the result was discarded.") +
               kNothingChanged;
    case Why::NotRemoved:
    case Why::None:
    case Why::Cancelled:
        break;
    }
    return std::string("Can't remove these faces: the faces around them can't be extended to close the gap.") +
           kNothingChanged;
}

// The hole a picked face stands for: its own bore, or a bore next to it whose stack it is part of. Among several, the
// narrowest (a counterbore floor touches the counterbore and the drilled bore).
const std::vector<int>* holeOf(FaceModel& model, int face)
{
    if (const std::vector<int>* own = model.bore(face))
        return own;
    const std::vector<int>* best = nullptr;
    const std::vector<detail::FaceLink> around = model.links(face);
    for (const detail::FaceLink& link : around) {
        const std::vector<int>* candidate = model.bore(link.other);
        if (candidate == nullptr)
            continue;
        const std::vector<int> stack = model.stack(*candidate);
        if (!std::binary_search(stack.begin(), stack.end(), face))
            continue;
        if (best == nullptr || model.facts(candidate->front()).radius < model.facts(best->front()).radius)
            best = candidate;
    }
    return best;
}

Result<OpResult> removeByWedges(const Shape& shape, FaceModel& model, const std::vector<int>& fillets, JobContext& ctx,
                                std::string_view where);

Result<OpResult> deleteWork(const Shape& shape, const std::vector<int>& picked, JobContext& ctx, std::string_view where)
{
    FaceModel model(shape);
    std::vector<HolePick> holes;
    std::vector<int> others;
    std::set<int> seenBores;
    for (const int f : picked) {
        const std::vector<int>* bore = holeOf(model, f);
        if (bore == nullptr) {
            others.push_back(f);
            continue;
        }
        // The drilled bore of the stack: its narrowest bore.
        std::vector<int> stack = model.stack(*bore);
        const std::vector<int>* drilled = bore;
        for (const int k : stack)
            if (const std::vector<int>* b = model.bore(k);
                b != nullptr && model.facts(b->front()).radius < model.facts(drilled->front()).radius)
                drilled = b;
        if (!seenBores.insert(drilled->front()).second)
            continue;
        HolePick hole;
        hole.stack = model.stack(*drilled);
        hole.bore = model.withCaps(*drilled);
        holes.push_back(std::move(hole));
    }
    // A picked fillet that is one piece of a longer blend which is not picked whole.
    bool runHint = false;
    for (const int f : picked)
        if (model.isBlendFace(f)) {
            const std::vector<int> run = model.blendRun(f);
            if (run.size() > 1 && !std::all_of(run.begin(), run.end(), [&](int k) {
                    return std::binary_search(picked.begin(), picked.end(), k);
                }))
                runHint = true;
        }

    struct Plan {
        std::vector<int> faces;
        bool boreOnly;
        double fuzzy;
    };
    std::vector<Plan> plans;
    {
        std::vector<int> full = others;
        std::vector<int> bores = others;
        bool anyStack = false;
        for (const HolePick& hole : holes) {
            full.insert(full.end(), hole.stack.begin(), hole.stack.end());
            bores.insert(bores.end(), hole.bore.begin(), hole.bore.end());
            anyStack = anyStack || hole.stack != hole.bore;
        }
        plans.push_back({sortedUnique(std::move(full)), false, 0.0});
        if (anyStack)
            plans.push_back({sortedUnique(std::move(bores)), true, 0.0});
    }

    Why firstWhy = Why::None;
    std::string firstDetail;
    for (const Plan& plan : plans) {
        if (auto ok = ctx.cancel.check(); !ok)
            return tl::unexpected(detail::cancelledError(where));
        const Expectation expect = expectationFor(model, plan.faces, holes);
        DeleteOutcome outcome = defeature(shape, plan.faces, expect, plan.fuzzy, ctx, where);
        if (outcome.why == Why::Cancelled)
            return tl::unexpected(detail::cancelledError(where));
        if (outcome.result) {
            if (plan.boreOnly)
                outcome.result->warnings.insert(outcome.result->warnings.begin(),
                                                "The counterbore, countersink or chamfer of the hole could not be "
                                                "removed with it and was left in place.");
            return std::move(*outcome.result);
        }
        if (firstWhy == Why::None) {
            firstWhy = outcome.why;
            firstDetail = outcome.detail;
        }
    }
    // Fillets that Defeaturing could not remove: rebuild the sharp corner with a boolean where the fillet is a plain
    // round between two flat faces (see "FILLET WEDGES").
    if (holes.empty() && firstWhy != Why::Pieces) {
        auto wedged = removeByWedges(shape, model, picked, ctx, where);
        if (!wedged && wedged.error().code == ErrorCode::Cancelled)
            return wedged;
        if (wedged)
            return wedged;
    }
    return tl::unexpected(failure(deleteMessage(firstWhy, runHint), where, firstDetail));
}

// ---------- tools for push/pull and resize ----------

// A boolean of the body with an internal tool, its failures said in the edit's own words.
Result<OpResult> booleanWithTool(BoolOp op, const Shape& body, const Shape& tool, JobContext& ctx,
                                 const std::string& subject, std::string_view where)
{
    Result<OpResult> out = boolean(op, body, tool, ctx);
    if (out && !orientedAndClosed(detail::occt(out->shape)))
        out = tl::unexpected(failure("free or badly oriented edges", where));
    if (out || out.error().code == ErrorCode::Cancelled)
        return out;
    std::string context = out.error().message;
    if (out.error().code == ErrorCode::InvalidArgument)
        return tl::unexpected(failure(subject + " could not be built." + kNothingChanged, where, context));
    return tl::unexpected(failure(subject +
                                      " gives an invalid body, usually because the new face would land exactly on "
                                      "another face or edge of the part. Try a slightly different size." +
                                      kNothingChanged,
                                  where, context));
}

// Warnings of a boolean that still apply to the edit (not its "separate solids" note, which the edits refuse).
void keepBooleanWarnings(const OpResult& from, OpResult& to)
{
    for (const std::string& w : from.warnings)
        if (w.find("separate solids") == std::string::npos && w.find("no recorded origin") == std::string::npos)
            to.warnings.push_back(w);
}

// Smallest distance from `point` to any of `faces` of `shape`.
double distanceToFaces(const Shape& shape, std::span<const SubId> faces, const gp_Pnt& point)
{
    double best = std::numeric_limits<double>::infinity();
    const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
    const detail::ShapeIndex& index = shape.impl->subs[detail::kindSlot(SubKind::Face)];
    for (const SubId& f : faces) {
        BRepExtrema_DistShapeShape measure(vertex, index.FindKey(static_cast<int>(f.index) + 1));
        if (measure.IsDone() && measure.NbSolution() > 0)
            best = std::min(best, measure.Value());
    }
    return best;
}

// ---------- fillet wedges ----------
//
// FILLET WEDGES. Defeaturing refuses some fillets that are nothing but a round of constant radius between two flat
// faces, running from one flat face square to its axis to another (an edge or inside-corner round of a prismatic
// part). For those the sharp corner is rebuilt directly: in the plane square to the axis, the region between the
// round's arc and the corner point where the two flat faces meet is swept over the round's length; a convex round
// gets that wedge added, a concave one gets it cut away. The result goes through the same checks as a Defeaturing
// result (the round gone, volume moved the right way by a plausible amount, the round's point on the right side).

// The wedge of fillet `face`, or nothing if it is not such a round.
std::optional<TopoDS_Shape> wedgeOf(FaceModel& model, int face)
{
    const detail::FaceFacts x = model.facts(face);
    if (x.type != GeomAbs_Cylinder || !x.hasPoint || !model.isFillet(face))
        return std::nullopt;
    const gp_Dir along = x.axis.Direction();
    std::vector<int> sides;
    for (const detail::FaceLink& link : model.links(face)) {
        const detail::FaceFacts& y = model.facts(link.other);
        if (y.type != GeomAbs_Plane || !y.hasPoint)
            return std::nullopt;
        const double dot = std::abs(y.normal.Dot(along));
        if (link.smooth && link.line && dot < 1e-9) {
            if (std::find(sides.begin(), sides.end(), link.other) == sides.end())
                sides.push_back(link.other);
        } else if (std::abs(dot - 1.0) > 1e-9) {
            return std::nullopt; // an end face that is not square to the axis
        }
    }
    if (sides.size() != 2)
        return std::nullopt;
    double tMin = 1e300, tMax = -1e300;
    for (TopExp_Explorer it(model.face(face), TopAbs_VERTEX); it.More(); it.Next()) {
        const double t = detail::axialParameter(x.axis, BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
        tMin = std::min(tMin, t);
        tMax = std::max(tMax, t);
    }
    if (!(tMax - tMin > kMinDistance))
        return std::nullopt;
    const gp_Pnt origin = x.axis.Location().Translated(gp_Vec(along) * tMin);
    const gp_Vec n1(model.facts(sides[0]).normal);
    const gp_Vec n2(model.facts(sides[1]).normal);
    const double s1 = gp_Vec(origin, model.facts(sides[0]).point).Dot(n1);
    const double s2 = gp_Vec(origin, model.facts(sides[1]).point).Dot(n2);
    const double c = n1.Dot(n2);
    if (std::abs(c) > 0.97) // the faces nearly parallel (a round over 165 degrees): no corner to rebuild
        return std::nullopt;
    const double a = (s1 - c * s2) / (1.0 - c * c);
    const double b = (s2 - c * s1) / (1.0 - c * c);
    const gp_Pnt corner = origin.Translated(n1 * a + n2 * b);
    const gp_Pnt touch1 = origin.Translated(n1 * s1);
    const gp_Pnt touch2 = origin.Translated(n2 * s2);
    const gp_Vec towardCorner(origin, corner);
    if (towardCorner.Magnitude() < 1e-12 || std::abs(std::abs(s1) - x.radius) > 1e-6 * std::max(1.0, x.radius) ||
        std::abs(std::abs(s2) - x.radius) > 1e-6 * std::max(1.0, x.radius))
        return std::nullopt;
    const gp_Pnt middle = origin.Translated(towardCorner.Normalized() * x.radius);
    // The round itself must be the arc facing the corner: its point, seen from the axis, points there too.
    const gp_Vec toPoint(detail::footOnAxis(x.axis, x.point), x.point);
    if (toPoint.Dot(towardCorner) <= 0.0)
        return std::nullopt;
    const occ::handle<Geom_TrimmedCurve> arc = GC_MakeArcOfCircle(touch1, middle, touch2).Value();
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(arc).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(touch2, corner).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(corner, touch1).Edge());
    if (!wire.IsDone())
        return std::nullopt;
    BRepBuilderAPI_MakeFace profile(wire.Wire(), true);
    if (!profile.IsDone())
        return std::nullopt;
    BRepPrimAPI_MakePrism prism(profile.Face(), gp_Vec(along) * (tMax - tMin), false, true);
    prism.Build();
    if (!prism.IsDone())
        return std::nullopt;
    return prism.Shape();
}

Result<OpResult> removeByWedges(const Shape& shape, FaceModel& model, const std::vector<int>& fillets, JobContext& ctx,
                                std::string_view where)
{
    const Error notApplicable = failure("not a plain round between flat faces", where);
    std::optional<bool> concave;
    BRep_Builder builder;
    TopoDS_Compound wedges;
    builder.MakeCompound(wedges);
    for (const int f : fillets) {
        const std::optional<TopoDS_Shape> wedge = wedgeOf(model, f);
        if (!wedge || (concave && *concave != model.facts(f).concave))
            return tl::unexpected(notApplicable);
        concave = model.facts(f).concave;
        builder.Add(wedges, *wedge);
    }
    if (!concave)
        return tl::unexpected(notApplicable);
    const Shape tool = detail::wrap(fillets.size() == 1 ? TopoDS_Iterator(wedges).Value() : TopoDS_Shape(wedges));
    auto op = booleanWithTool(*concave ? BoolOp::Cut : BoolOp::Union, shape, tool, ctx, "Removing the fillet", where);
    if (!op)
        return op;
    if (op->shape.count(SubKind::Solid) != shape.count(SubKind::Solid))
        return tl::unexpected(failure("pieces", where));
    for (const int f : fillets)
        if (primaryRelation(op->history, SubId{SubKind::Face, static_cast<std::uint32_t>(f)}) !=
            HistoryEntry::Relation::Deleted)
            return tl::unexpected(failure("the round is still there", where));
    const Expectation expect = expectationFor(model, fillets, {});
    const double change = detail::volumeOf(op->shape) - detail::volumeOf(shape);
    if ((expect.sign != 0 && change * expect.sign < 0.0) ||
        (expect.bounded && (std::abs(change) < expect.lo || std::abs(change) > expect.hi)))
        return tl::unexpected(failure("unexpected volume", where));
    for (const auto& [point, want] : expect.points)
        if (detail::classifyPointFast(detail::occt(op->shape), point, kOnTolerance) != want)
            return tl::unexpected(failure("a point of the round is on the wrong side", where));
    OpResult out;
    out.shape = op->shape;
    out.history = rewriteToolHistory(entriesOf(op->history, 0), op->history, {}, {},
                                     SubId{SubKind::Face, static_cast<std::uint32_t>(fillets.front())});
    keepBooleanWarnings(*op, out);
    addOrphanWarning(out);
    return out;
}

// ---------- push/pull ----------

Result<OpResult> offsetWork(const Shape& shape, const std::vector<int>& picked, double distance, JobContext& ctx,
                            std::string_view where)
{
    FaceModel model(shape);
    for (const int f : picked) {
        if (model.facts(f).type != GeomAbs_Plane)
            return tl::unexpected(invalidArgument(
                "Push/pull works on flat faces only: face " + std::to_string(f) + " is curved.", where));
        if (!model.facts(f).hasPoint)
            return tl::unexpected(
                failure("Face " + std::to_string(f) + " could not be measured, so it can't be moved." + kNothingChanged,
                        where));
    }
    const bool outward = distance > 0.0;
    const std::string amount = mm(std::abs(distance));
    const std::string subject = std::string(outward ? "Pushing " : "Pulling ") + faceList(picked) + " " +
                                (outward ? "out" : "in") + " by " + amount;

    // One prism per face, swept from a copy of the face (the prism shares its bottom with what it sweeps).
    struct Swept {
        int face;
        TopoDS_Shape solid;
        TopoDS_Shape cap;
        std::vector<std::pair<TopoDS_Shape, SubId>> sides; // side face, input edge it was swept from
    };
    std::vector<Swept> swept;
    const detail::ShapeIndex& edges = shape.impl->subs[detail::kindSlot(SubKind::Edge)];
    for (const int f : picked) {
        const TopoDS_Face original = model.face(f);
        BRepBuilderAPI_Copy copier(original, true, false);
        const TopoDS_Face copied = TopoDS::Face(copier.Shape());
        const gp_Vec sweep = gp_Vec(model.facts(f).normal) * distance;
        BRepPrimAPI_MakePrism prism(copied, sweep, false, true);
        prism.Build();
        if (!prism.IsDone())
            return tl::unexpected(failure(subject + " could not be built." + kNothingChanged, where));
        Swept s{f, prism.Shape(), prism.LastShape(), {}};
        for (TopExp_Explorer it(original, TopAbs_EDGE); it.More(); it.Next()) {
            const int e = edges.FindIndex(it.Current()) - 1;
            if (e < 0)
                continue;
            for (const TopoDS_Shape& side : prism.Generated(copier.ModifiedShape(it.Current())))
                if (side.ShapeType() == TopAbs_FACE)
                    s.sides.emplace_back(side, SubId{SubKind::Edge, static_cast<std::uint32_t>(e)});
        }
        swept.push_back(std::move(s));
    }
    TopoDS_Shape toolShape;
    if (swept.size() == 1) {
        toolShape = swept.front().solid;
    } else {
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const Swept& s : swept)
            builder.Add(compound, s.solid);
        toolShape = compound;
    }
    const Shape tool = detail::wrap(toolShape);
    ctx.progress.set(0.1, kStage);

    auto op = booleanWithTool(outward ? BoolOp::Union : BoolOp::Cut, shape, tool, ctx, subject, where);
    if (!op)
        return tl::unexpected(op.error());
    if (op->shape.count(SubKind::Solid) != shape.count(SubKind::Solid))
        return tl::unexpected(failure(subject + " would cut the part in two." + kNothingChanged, where));

    std::vector<EditedFace> edited;
    std::vector<ToolOrigin> origins;
    for (const Swept& s : swept) {
        std::vector<SubId> images;
        for (TopExp_Explorer it(s.cap, TopAbs_FACE); it.More(); it.Next())
            if (const auto id = detail::subIdOf(tool, it.Current())) {
                const std::vector<SubId> r = toolImages(op->history, *id);
                images.insert(images.end(), r.begin(), r.end());
            }
        sortUnique(images);
        // The moved face must be where it was asked for: the face's sample point moved by `distance` lies on it.
        const gp_Pnt target = model.facts(s.face).point.Translated(gp_Vec(model.facts(s.face).normal) * distance);
        if (images.empty() || distanceToFaces(op->shape, images, target) >= kOnTolerance)
            return tl::unexpected(
                failure("Face " + std::to_string(s.face) + " can't be moved " + amount +
                            ": that far it would disappear into other material of the part or pass its far side. Use a "
                            "shorter distance." +
                            kNothingChanged,
                        where));
        edited.push_back({SubId{SubKind::Face, static_cast<std::uint32_t>(s.face)}, std::move(images)});
        for (const auto& [side, edge] : s.sides)
            if (const auto id = detail::subIdOf(tool, side))
                origins.push_back({*id, edge});
    }

    OpResult out;
    out.shape = op->shape;
    out.history = rewriteToolHistory(entriesOf(op->history, 0), op->history, edited, origins,
                                     SubId{SubKind::Face, static_cast<std::uint32_t>(picked.front())});
    // (The moved faces' images lie apart from them by design; any other face keeps its place.)
    demoteReversedImages(out.history, shape, out.shape, picked);
    for (const int f : picked) {
        bool square = true;
        for (const detail::FaceLink& link : model.links(f))
            if (link.smooth || link.normalDot > 1.0 || std::abs(link.normalDot) > kSquareDot)
                square = false;
        if (!square)
            out.warnings.push_back("Face " + std::to_string(f) +
                                   " meets a fillet, chamfer or angled wall, so push/pull moved it as a slab: there is "
                                   "now a step where it meets that face, which was not extended.");
    }
    keepBooleanWarnings(*op, out);
    addOrphanWarning(out);
    return out;
}

// ---------- resize ----------

TopoDS_Shape cylinderAlong(const gp_Ax1& axis, double radius, double t0, double t1)
{
    const gp_Pnt base = axis.Location().Translated(gp_Vec(axis.Direction()) * t0);
    return BRepPrimAPI_MakeCylinder(gp_Ax2(base, axis.Direction()), radius, t1 - t0).Shape();
}

// The cylindrical faces of `tool` of radius `radius` (its new hole wall), as tool SubIds.
std::vector<SubId> wallFacesOf(const Shape& tool, double radius)
{
    std::vector<SubId> out;
    const detail::ShapeIndex& faces = tool.impl->subs[detail::kindSlot(SubKind::Face)];
    for (int i = 1; i <= faces.Extent(); ++i) {
        const BRepAdaptor_Surface surface(TopoDS::Face(faces.FindKey(i)));
        if (surface.GetType() == GeomAbs_Cylinder && std::abs(surface.Cylinder().Radius() - radius) < 1e-9)
            out.push_back(SubId{SubKind::Face, static_cast<std::uint32_t>(i - 1)});
    }
    return out;
}

// Which pieces of the new wall lie over which old bore face (see "HISTORY"): sample points of each new wall face are
// moved radially onto the old radius and tested against the old faces. A wall face over no old face goes to all of
// them, and an old face under no wall face gets all of the wall.
std::vector<EditedFace> mapWall(FaceModel& model, const std::vector<int>& bore, const HoleGeometry& hole,
                                const Shape& result, const std::vector<SubId>& wall)
{
    std::vector<EditedFace> out;
    for (const int k : bore)
        out.push_back({SubId{SubKind::Face, static_cast<std::uint32_t>(k)}, {}});
    const detail::ShapeIndex& faces = result.impl->subs[detail::kindSlot(SubKind::Face)];
    for (const SubId& w : wall) {
        const TopoDS_Face face = TopoDS::Face(faces.FindKey(static_cast<int>(w.index) + 1));
        bool owned = false;
        for (const double fu : {0.1, 0.5, 0.9}) {
            gp_Pnt p;
            gp_Dir n;
            if (!detail::facePointAt(face, fu, 0.5, p, n))
                continue;
            const gp_Pnt foot = detail::footOnAxis(hole.axis, p);
            const gp_Vec radial(foot, p);
            if (radial.Magnitude() < 1e-12)
                continue;
            const gp_Pnt onOld = foot.Translated(radial.Normalized() * hole.radius);
            const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(onOld).Vertex();
            for (std::size_t i = 0; i < bore.size(); ++i) {
                BRepExtrema_DistShapeShape measure(vertex, model.face(bore[i]));
                if (measure.IsDone() && measure.NbSolution() > 0 &&
                    measure.Value() < 1e-4 * std::max(1.0, hole.radius)) {
                    out[i].results.push_back(w);
                    owned = true;
                }
            }
        }
        if (!owned)
            for (EditedFace& e : out)
                e.results.push_back(w);
    }
    for (EditedFace& e : out) {
        if (e.results.empty())
            e.results = wall;
        sortUnique(e.results);
    }
    return out;
}

// Checks a resize boolean's result and rewrites its history. `body` holds the body's entries in the caller's SubIds.
Result<OpResult> finishResize(FaceModel& model, const std::vector<int>& bore, const HoleGeometry& hole,
                              double newRadius, const Shape& input, const Shape& tool, const OpResult& op,
                              ShapeHistory body, const std::string& subject, std::string_view where)
{
    if (op.shape.count(SubKind::Solid) != input.count(SubKind::Solid))
        return tl::unexpected(failure(subject + " would cut the part in two." + kNothingChanged, where));
    std::vector<SubId> wall;
    for (const SubId& toolFace : wallFacesOf(tool, newRadius)) {
        const std::vector<SubId> images = toolImages(op.history, toolFace);
        wall.insert(wall.end(), images.begin(), images.end());
    }
    sortUnique(wall);
    // The new wall must be where it was asked for: beside the old wall point, at the new radius.
    const gp_Pnt foot = detail::footOnAxis(hole.axis, hole.wall);
    const gp_Vec radial(foot, hole.wall);
    const bool placed =
        !wall.empty() && radial.Magnitude() > 1e-12 &&
        distanceToFaces(op.shape, wall, foot.Translated(radial.Normalized() * newRadius)) < kOnTolerance;
    if (!placed)
        return tl::unexpected(failure(subject +
                                          " did not leave a round wall where the hole is: the new size runs into "
                                          "other features of the part." +
                                          kNothingChanged,
                                      where));
    std::vector<EditedFace> edited = mapWall(model, bore, hole, op.shape, wall);
    OpResult out;
    out.shape = op.shape;
    out.history = rewriteToolHistory(std::move(body), op.history, edited, {},
                                     SubId{SubKind::Face, static_cast<std::uint32_t>(bore.front())});
    demoteReversedImages(out.history, input, out.shape, bore);
    keepBooleanWarnings(op, out);
    addOrphanWarning(out);
    return out;
}

Result<OpResult> resizeWork(const Shape& shape, int picked, double newDiameter, JobContext& ctx, std::string_view where)
{
    FaceModel model(shape);
    const std::vector<int>* bore = holeOf(model, picked);
    if (bore == nullptr)
        return tl::unexpected(invalidArgument("Face " + std::to_string(picked) +
                                                  " is not part of a round hole. Pick the round wall of a drilled "
                                                  "or bored hole.",
                                              where));
    const std::vector<int> boreFaces = *bore;
    const HoleGeometry hole = model.holeGeometry(boreFaces);
    const double newRadius = 0.5 * newDiameter;
    if (std::abs(newRadius - hole.radius) <= kMinDistance)
        return tl::unexpected(invalidArgument("The hole is already " + mm(2.0 * hole.radius) + " across.", where));
    const bool larger = newRadius > hole.radius;
    const std::string subject =
        std::string(larger ? "Opening the hole up to " : "Closing the hole down to ") + mm(newDiameter);
    const double length = hole.tMax - hole.tMin;
    if (!(length > kMinDistance))
        return tl::unexpected(
            failure("The hole has no length that can be measured." + std::string(kNothingChanged), where));

    if (larger) {
        const double reach = newRadius + kOpenEndMargin;
        const double t0 = hole.tMin - (hole.openMin ? reach : 0.0);
        const double t1 = hole.tMax + (hole.openMax ? reach : 0.0);
        const Shape tool = detail::wrap(cylinderAlong(hole.axis, newRadius, t0, t1));
        auto op = booleanWithTool(BoolOp::Cut, shape, tool, ctx, subject, where);
        if (!op)
            return tl::unexpected(op.error());
        return finishResize(model, boreFaces, hole, newRadius, shape, tool, *op, entriesOf(op->history, 0), subject,
                            where);
    }

    // Smaller: a sleeve, when both ends of the bore are plain circles.
    Error sleeveError = failure(subject +
                                    " is not possible here: the hole meets other features, so its wall can't "
                                    "be filled in." +
                                    kNothingChanged,
                                where);
    if (model.endsOnPlainCircles(boreFaces)) {
        BRepAlgoAPI_Cut sleeveMaker(cylinderAlong(hole.axis, hole.radius, hole.tMin, hole.tMax),
                                    cylinderAlong(hole.axis, newRadius, hole.tMin, hole.tMax));
        if (sleeveMaker.IsDone() && !sleeveMaker.HasErrors()) {
            const Shape tool = detail::wrap(detail::normaliseSolids(sleeveMaker.Shape()));
            if (!tool.isNull() && tool.count(SubKind::Solid) == 1) {
                auto op = booleanWithTool(BoolOp::Union, shape, tool, ctx, subject, where);
                if (!op && op.error().code == ErrorCode::Cancelled)
                    return op;
                if (op) {
                    auto done = finishResize(model, boreFaces, hole, newRadius, shape, tool, *op,
                                             entriesOf(op->history, 0), subject, where);
                    if (done)
                        return done;
                    sleeveError = done.error();
                } else {
                    sleeveError = op.error();
                }
            }
        }
    }
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    // Else: remove the bore with its bottom, then cut it again at the new size.
    const std::vector<int> boreWithBottom = model.withCaps(boreFaces);
    std::vector<HolePick> asHole{{model.stack(boreFaces), boreWithBottom}};
    Expectation expect = expectationFor(model, boreWithBottom, asHole);
    expect.lo = 0.0; // only the sign and the upper bound: the bore may be part of a larger stack
    DeleteOutcome filled = defeature(shape, boreWithBottom, expect, 0.0, ctx, where);
    if (filled.why == Why::Cancelled)
        return tl::unexpected(detail::cancelledError(where));
    if (!filled.result) {
        sleeveError.context.push_back("filling the bore: " + filled.detail);
        return tl::unexpected(std::move(sleeveError));
    }
    const Shape& mid = filled.result->shape;
    const double reach = newRadius + kOpenEndMargin;
    const Shape tool = detail::wrap(cylinderAlong(hole.axis, newRadius, hole.tMin - (hole.openMin ? reach : 0.0),
                                                  hole.tMax + (hole.openMax ? reach : 0.0)));
    auto op = booleanWithTool(BoolOp::Cut, mid, tool, ctx, subject, where);
    if (!op)
        return tl::unexpected(op.error());
    const double before = detail::volumeOf(shape);
    const double after = detail::volumeOf(op->shape);
    if (!(after > before))
        return tl::unexpected(failure(subject + " did not add material to the hole wall." + kNothingChanged, where));
    return finishResize(model, boreFaces, hole, newRadius, shape, tool, *op,
                        compose(filled.result->history, entriesOf(op->history, 0)), subject, where);
}

// ---------- unify ----------

// BRepTools_History seen through the maker interface buildHistory() reads.
class HistoryMaker final : public BRepBuilderAPI_MakeShape {
public:
    explicit HistoryMaker(occ::handle<BRepTools_History> history) : history_(std::move(history)) {}
    const NCollection_List<TopoDS_Shape>& Modified(const TopoDS_Shape& s) override
    {
        return history_.IsNull() ? empty_ : history_->Modified(s);
    }
    const NCollection_List<TopoDS_Shape>& Generated(const TopoDS_Shape& s) override
    {
        return history_.IsNull() ? empty_ : history_->Generated(s);
    }
    bool IsDeleted(const TopoDS_Shape& s) override { return !history_.IsNull() && history_->IsRemoved(s); }

private:
    occ::handle<BRepTools_History> history_;
    NCollection_List<TopoDS_Shape> empty_;
};

OpResult unchanged(const Shape& shape)
{
    OpResult out;
    out.shape = shape;
    for (std::size_t slot = 0; slot < detail::kSubKindCount; ++slot) {
        const auto kind = static_cast<SubKind>(slot);
        for (std::uint32_t i = 0; i < shape.count(kind); ++i)
            out.history.entries.push_back({HistoryEntry::Relation::Modified, 0, SubId{kind, i}, {SubId{kind, i}}});
    }
    return out;
}

Result<OpResult> unifyWork(const Shape& shape, JobContext& ctx, std::string_view where)
{
    auto copy = detail::privateCopy(shape, true, where);
    if (!copy)
        return tl::unexpected(copy.error());
    ShapeUpgrade_UnifySameDomain unifier(detail::occt(copy->shape), true, true, false);
    unifier.SetSafeInputMode(true);
    unifier.Build();
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    ctx.progress.set(0.6, "Checking the result");
    const TopoDS_Shape solids = detail::normaliseSolids(unifier.Shape());
    const double before = detail::volumeOf(shape);
    const auto keepInput = [&]() -> Result<OpResult> {
        OpResult out = unchanged(shape);
        out.warnings.push_back("Faces that lie on one surface were left as separate faces: merging them made the "
                               "solid invalid.");
        return out;
    };
    if (solids.IsNull())
        return keepInput();
    auto checked = detail::validateSolids(solids, "Merging faces", where);
    if (!checked || std::abs(*checked - before) > 1e-6 * std::max(1.0, std::abs(before)))
        return keepInput();
    OpResult out;
    out.shape = detail::wrap(solids);
    if (out.shape.count(SubKind::Solid) != shape.count(SubKind::Solid))
        return keepInput();
    if (out.shape.count(SubKind::Face) == shape.count(SubKind::Face) &&
        out.shape.count(SubKind::Edge) == shape.count(SubKind::Edge))
        return tl::unexpected(failure(
            "There is nothing to merge: no two neighbouring faces or edges of the body lie on one surface or curve.",
            where));
    HistoryMaker maker(unifier.History());
    const std::array<const Shape*, 1> inputs{&copy->shape};
    out.history = detail::buildHistory(maker, inputs, out.shape);
    if (auto mapped = detail::remapSources(out.history, 0, *copy, where); !mapped)
        return tl::unexpected(mapped.error());
    detail::markValidated(out.shape, *checked);
    addOrphanWarning(out);
    return out;
}

} // namespace

Result<OpResult> deleteFaces(const Shape& shape, std::span<const SubId> faces, JobContext& ctx)
{
    constexpr std::string_view where = kDeleteWhere;
    if (auto ok = checkBodyAndFaces(shape, faces, true, where); !ok)
        return tl::unexpected(ok.error());
    std::vector<int> picked;
    for (const SubId& id : faces)
        picked.push_back(static_cast<int>(id.index));
    picked = sortedUnique(std::move(picked));
    return runEdit(ctx, where,
                   [shape, picked](JobContext& inner) { return deleteWork(shape, picked, inner, kDeleteWhere); });
}

Result<OpResult> offsetFaces(const Shape& shape, std::span<const SubId> faces, double distance, JobContext& ctx)
{
    constexpr std::string_view where = kOffsetWhere;
    if (auto ok = checkBodyAndFaces(shape, faces, true, where); !ok)
        return tl::unexpected(ok.error());
    if (!std::isfinite(distance))
        return tl::unexpected(invalidArgument("The push/pull distance must be a number.", where));
    if (std::abs(distance) < kMinDistance)
        return tl::unexpected(invalidArgument("Enter a push/pull distance other than zero.", where));
    std::vector<int> picked;
    for (const SubId& id : faces)
        picked.push_back(static_cast<int>(id.index));
    picked = sortedUnique(std::move(picked));
    return runEdit(ctx, where, [shape, picked, distance](JobContext& inner) {
        return offsetWork(shape, picked, distance, inner, kOffsetWhere);
    });
}

Result<OpResult> resizeHole(const Shape& shape, SubId anyFaceOfHole, double newDiameter, JobContext& ctx)
{
    constexpr std::string_view where = kResizeWhere;
    const std::array<SubId, 1> face{anyFaceOfHole};
    if (auto ok = checkBodyAndFaces(shape, face, true, where); !ok)
        return tl::unexpected(ok.error());
    if (!std::isfinite(newDiameter) || !(newDiameter > kMinDistance))
        return tl::unexpected(invalidArgument("Enter a hole diameter larger than zero.", where));
    const int picked = static_cast<int>(anyFaceOfHole.index);
    return runEdit(ctx, where, [shape, picked, newDiameter](JobContext& inner) {
        return resizeWork(shape, picked, newDiameter, inner, kResizeWhere);
    });
}

Result<OpResult> unifySameDomain(const Shape& shape, JobContext& ctx)
{
    constexpr std::string_view where = kUnifyWhere;
    if (auto ok = checkBodyAndFaces(shape, {}, false, where); !ok)
        return tl::unexpected(ok.error());
    return runEdit(ctx, where, [shape](JobContext& inner) { return unifyWork(shape, inner, kUnifyWhere); });
}

std::vector<SubId> blendRun(const Shape& shape, SubId face)
{
    constexpr std::string_view where = "kernel::blendRun";
    if (shape.isNull() || face.kind != SubKind::Face || face.index >= shape.count(SubKind::Face))
        return {};
    auto run = detail::guarded(where, [&]() -> Result<std::vector<SubId>> {
        FaceModel model(shape);
        std::vector<SubId> out;
        for (const int k : model.blendRun(static_cast<int>(face.index)))
            out.push_back(SubId{SubKind::Face, static_cast<std::uint32_t>(k)});
        return out;
    });
    return run ? std::move(*run) : std::vector<SubId>{};
}

} // namespace ray::kernel
