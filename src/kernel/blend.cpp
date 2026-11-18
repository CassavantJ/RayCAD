// fillet() and chamfer() (task T-095).
//
// Both run BRepFilletAPI_MakeFillet (constant radius, rational circles) or BRepFilletAPI_MakeChamfer (one distance,
// symmetric) on a private copy of the input (boolean_internal.h PrivateCopy), so OCCT never writes into the caller's
// immutable TShapes (PLAN.md section 7 rules 1 and 3); the copy's SubIds are mapped back to the input's before the
// history is returned.
//
// PICKED EDGES. Every SubId must be an edge of the shape; duplicates are ignored. An edge must lie between exactly two
// different faces that meet at an angle: a seam, an open boundary, a collapsed edge, a non-manifold edge or a smooth
// (tangent) edge is refused with InvalidArgument naming the edge, because there is no corner to blend. OCCT extends
// each picked edge along the chain of edges that continue it tangentially; the extra edges are blended too and
// `warnings` names them (kernel.h).
//
// VALIDATION (section 7 rule 6). OCCT's status is not trusted. The result must be valid closed solids
// (validateSolids), as many as the input had, with the volume changed the expected way: a blend on a convex edge
// takes material away, one on a concave edge adds it; when the blended edges are of both kinds only a change is
// required. A result without any blend face is a no-op and fails. This is not idle: on the synthetic corpus (blend
// stress test, 1052 blends) OCCT reported success for 21 results that BRepCheck rejects. A self-interference check
// (BOPAlgo_CheckerSI) was tried on top and rejected nothing more at 4.5 times the cost, so it is not run.
//
// TOO LARGE. When OCCT fails, throws, or returns a result that fails validation, the same edges are tried again with
// smaller sizes (at most kSearchAttempts more builds, fixed steps, so the message is deterministic). If a smaller
// size works the failure is reported as "too large" with the largest size found to work; if none does, as edges the
// geometry kernel cannot blend, with the corners where four or more edges meet (a case OCCT does not handle).
// Both are ErrorCode::KernelFailure with a message a machinist can act on; OCCT's own status goes into `context`.
//
// HISTORY (boolean_internal.h "HISTORY NORMALISATION"). Measured on OCCT 8.0.1 (spike S4, and blend_test.cpp):
//   - a face next to a blend is reported Modified (its trimmed copy), never Generated; an untouched face is
//     reported Modified to its SubId in the result;
//   - each blend face is Generated from the edge it rounds (picked or added by tangent propagation), and the corner
//     patch where three blended edges meet (a sphere for a fillet, a triangle for a chamfer) is Generated from the
//     vertex;
//   - the end vertices of a blended edge are Deleted, the blended edge itself is Deleted (its Generated entry names
//     the blend faces);
//   - the four boundary edges of a blend face and their vertices have NO ancestor in OCCT's history. They are left
//     without one (naming names them from their faces, PLAN.md 3.3 rule 3); nothing is invented here.
// A result face without any history would be named in `warnings` (none seen on the test set).
//
// CANCEL. ChFi3d_Builder::Compute() takes no progress range (BRepFilletAPI_MakeFillet::Build ignores its argument in
// OCCT 8.0.1), so one OCCT build cannot be interrupted. The token is checked before and after every build, so a
// cancelled call returns Error{Cancelled} at the end of the build that is running and never starts another. The
// longest whole call in the corpus stress test, size search included, took 0.5 s (Windows, release build).
#include "boolean_internal.h"

#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRep_Tool.hxx>
#include <ChFi3d.hxx>
#include <ChFiDS_ErrorStatus.hxx>
#include <ChFiDS_TypeOfConcavity.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ray::kernel {

namespace {

using detail::kernelFailure;

// Sizes at or below this are refused up front (mm). OCCT's confusion tolerance is 1e-7.
constexpr double kMinSize = 1e-6;
// Volume changes at or below this fraction of the input volume count as no change.
constexpr double kNoOpRelative = 1e-12;
// Extra builds after a failure: shrinking steps until one works, then halving the gap to the failing size.
constexpr std::array<double, 4> kShrink{0.5, 0.25, 0.1, 0.01};
constexpr int kRefineSteps = 4;
constexpr int kSearchAttempts = static_cast<int>(kShrink.size()) + kRefineSteps;
// At most this many edges are listed by number in a message.
constexpr std::size_t kListedEdges = 8;

enum class Blend { Fillet, Chamfer };

struct Words {
    const char* subject; // "The fillet"
    const char* noun;    // "fillet"
    const char* size;    // "radius"
    const char* stage;   // progress stage of a build
};

Words wordsFor(Blend blend)
{
    if (blend == Blend::Fillet)
        return {"The fillet", "fillet", "radius", "Filleting"};
    return {"The chamfer", "chamfer", "distance", "Chamfering"};
}

Error invalidArgument(std::string message, std::string_view where)
{
    return detail::makeError(ErrorCode::InvalidArgument, std::move(message), where);
}

// "12.5 mm": up to six significant digits, as typed, independent of the locale.
std::string mm(double value)
{
    std::array<char, 32> buffer{};
    const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, 6);
    return std::string(buffer.data(), end.ptr) + " mm";
}

// "edge 4", "edges 4 and 9", "edges 4, 9 and 12", "edges 1, 2, ..., 8 and 5 more".
std::string edgeList(const std::vector<SubId>& edges)
{
    std::string text = edges.size() == 1 ? "edge " : "edges ";
    const std::size_t shown = std::min(edges.size(), kListedEdges);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0)
            text += (i + 1 == shown && shown == edges.size()) ? " and " : ", ";
        text += std::to_string(edges[i].index);
    }
    if (shown < edges.size())
        text += " and " + std::to_string(edges.size() - shown) + " more";
    return text;
}

// Which way the volume must move.
enum class Expect { Less, More, Either };

// The input copied for OCCT (boolean_internal.h PrivateCopy), and what the picked edges are.
struct Prepared {
    detail::PrivateCopy copied;
    std::vector<SubId> picked;      // input SubIds, sorted, unique
    std::vector<TopoDS_Edge> edges; // the picked edges in the copy
    double volume = 0.0;
    std::uint32_t solids = 0;
    const Shape& copy() const { return copied.shape; }
};

// The input SubId of a sub-shape of the copy; empty if it is not one.
std::optional<SubId> inputId(const Prepared& p, const TopoDS_Shape& sub)
{
    const auto id = detail::subIdOf(p.copy(), sub);
    if (!id)
        return std::nullopt;
    return detail::inputSubId(p.copied, *id);
}

// The distinct faces of `shape` that contain `edge` (ancestor map of the Shape).
std::vector<TopoDS_Face> facesOf(const Shape& shape, const TopoDS_Shape& edge)
{
    std::vector<TopoDS_Face> faces;
    const detail::AncestorIndex& map = shape.impl->ancestors(SubKind::Edge, SubKind::Face);
    if (const NCollection_List<TopoDS_Shape>* list = map.Seek(edge))
        for (const TopoDS_Shape& face : *list)
            faces.push_back(TopoDS::Face(face));
    return faces;
}

// Checks the picked edges on the input. Precondition: every id is a valid edge SubId. May throw.
Status checkEdges(const Shape& shape, const std::vector<SubId>& picked, const Words& words, std::string_view where)
{
    const detail::ShapeIndex& edges = shape.impl->subs[detail::kindSlot(SubKind::Edge)];
    for (const SubId& id : picked) {
        const TopoDS_Edge edge = TopoDS::Edge(edges.FindKey(static_cast<int>(id.index) + 1));
        const std::string name = "Edge " + std::to_string(id.index);
        if (BRep_Tool::Degenerated(edge))
            return tl::unexpected(invalidArgument(
                name + " is collapsed to a point, so there is no corner to " + words.noun + ".", where));
        const std::vector<TopoDS_Face> faces = facesOf(shape, edge);
        if (faces.empty())
            return tl::unexpected(invalidArgument(
                name + " does not bound any face of the body, so there is no corner to " + words.noun + ".", where));
        if (faces.size() == 1)
            return tl::unexpected(invalidArgument(
                name +
                    " has a face on one side only (a seam of a round face or an open edge), so there is no "
                    "corner to " +
                    words.noun + ". Pick an edge where two faces meet.",
                where));
        if (faces.size() > 2)
            return tl::unexpected(invalidArgument(
                name + " is shared by more than two faces, so it cannot be blended. Pick an edge where exactly two "
                       "faces meet.",
                where));
        if (ChFi3d::IsTangentFaces(edge, faces[0], faces[1]))
            return tl::unexpected(invalidArgument(
                name + " is smooth: the faces on either side already meet tangentially, so there is no corner to " +
                    words.noun + ".",
                where));
    }
    return {};
}

// A fresh private copy of `shape` with the picked edges found in it; the rest of `base` is kept. May throw.
Result<Prepared> freshCopy(const Shape& shape, const Prepared& base, std::string_view where)
{
    Prepared p;
    p.picked = base.picked;
    p.volume = base.volume;
    p.solids = base.solids;
    auto copied = detail::privateCopy(shape, /*copyGeometry=*/true, where);
    if (!copied)
        return tl::unexpected(copied.error());
    p.copied = std::move(*copied);
    // The copy's edge whose input SubId is each picked one.
    std::vector<std::uint32_t> toCopy(shape.count(SubKind::Edge), detail::kNoInputIndex);
    const std::vector<std::uint32_t>& edgeMap = p.copied.toInput[detail::kindSlot(SubKind::Edge)];
    for (std::uint32_t i = 0; i < edgeMap.size(); ++i)
        toCopy[edgeMap[i]] = i;
    const detail::ShapeIndex& copyEdges = p.copy().impl->subs[detail::kindSlot(SubKind::Edge)];
    for (const SubId& id : p.picked)
        p.edges.push_back(TopoDS::Edge(copyEdges.FindKey(static_cast<int>(toCopy[id.index]) + 1)));
    return p;
}

// Copies the input, maps the copy back and measures the input. May throw.
Result<Prepared> prepare(const Shape& shape, std::vector<SubId> picked, std::string_view where)
{
    Prepared base;
    base.picked = std::move(picked);
    base.volume = detail::solidVolume(detail::occt(shape));
    base.solids = shape.count(SubKind::Solid);
    return freshCopy(shape, base, where);
}

// Rewrites history sources from copy SubIds to input SubIds (boolean_internal.h remapSources).
Result<ShapeHistory> remapHistory(ShapeHistory history, const Prepared& p, std::string_view where)
{
    if (auto ok = detail::remapSources(history, 0, p.copied, where); !ok)
        return tl::unexpected(ok.error());
    return history;
}

// What a failed build tells about itself.
struct Failure {
    Error error;
    std::vector<SubId> faultyEdges; // input SubIds of the contours OCCT reports as failed (fillet only)
};

struct Attempt {
    std::optional<OpResult> result;
    std::optional<Failure> failure;
    bool cancelled = false;
};

const char* statusName(ChFiDS_ErrorStatus status)
{
    switch (status) {
    case ChFiDS_Ok:
        return "ok";
    case ChFiDS_Error:
        return "error";
    case ChFiDS_WalkingFailure:
        return "walking failure";
    case ChFiDS_StartsolFailure:
        return "no start solution";
    case ChFiDS_TwistedSurface:
        return "twisted surface";
    }
    return "unknown";
}

// The contour edges of a maker, as input SubIds, sorted and unique.
template <class Maker>
std::vector<SubId> contourEdges(const Maker& maker, const Prepared& p, int contour)
{
    std::vector<SubId> out;
    for (int j = 1; j <= maker.NbEdges(contour); ++j)
        if (const auto id = inputId(p, maker.Edge(contour, j)))
            out.push_back(*id);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void addEdge(BRepFilletAPI_MakeFillet& maker, double value, const TopoDS_Edge& edge)
{
    maker.Add(value, edge);
}
void addEdge(BRepFilletAPI_MakeChamfer& maker, double value, const TopoDS_Edge& edge)
{
    maker.Add(value, edge);
}

void describeFaults(BRepFilletAPI_MakeFillet& maker, const Prepared& p, Failure& failure)
{
    for (int i = 1; i <= maker.NbFaultyContours(); ++i) {
        const int contour = maker.FaultyContour(i);
        const std::vector<SubId> edges = contourEdges(maker, p, contour);
        failure.faultyEdges.insert(failure.faultyEdges.end(), edges.begin(), edges.end());
        failure.error.context.insert(failure.error.context.begin(), "OCCT: contour " + std::to_string(contour) +
                                                                        " failed (" +
                                                                        statusName(maker.StripeStatus(contour)) + ")");
    }
    std::sort(failure.faultyEdges.begin(), failure.faultyEdges.end());
    failure.faultyEdges.erase(std::unique(failure.faultyEdges.begin(), failure.faultyEdges.end()),
                              failure.faultyEdges.end());
}
void describeFaults(BRepFilletAPI_MakeChamfer&, const Prepared&, Failure&)
{
    // BRepFilletAPI_MakeChamfer does not expose the failed contours.
}

Failure failed(std::string message, std::string_view where)
{
    return Failure{kernelFailure(std::move(message), where), {}};
}

// One build at `value`, on a fresh private copy of `shape`: OCCT's blend builders leave state on the shape they are
// given, so a copy that the dry run of the contours or an earlier attempt of the size search used is never used again.
// (With one copy for all of them, two of the 1,000 naming random sequences came out differently than with a fresh
// copy each.) Never throws (the OCCT part runs in guarded()).
template <class Maker>
Attempt attempt(const Shape& shape, const Prepared& base, double value, Expect expect, const Words& words,
                JobContext& ctx, std::string_view where)
{
    Attempt out;
    if (ctx.cancel.cancelled()) {
        out.cancelled = true;
        return out;
    }
    auto built = detail::guarded(where, [&]() -> Result<OpResult> {
        auto fresh = freshCopy(shape, base, where);
        if (!fresh) {
            out.failure = Failure{fresh.error(), {}};
            return tl::unexpected(fresh.error());
        }
        const Prepared& p = *fresh;
        Maker maker(detail::occt(p.copy()));
        for (const TopoDS_Edge& edge : p.edges)
            addEdge(maker, value, edge);
        maker.Build();
        if (ctx.cancel.cancelled())
            return tl::unexpected(detail::cancelledError(where));
        if (!maker.IsDone()) {
            Failure failure = failed(std::string(words.subject) + " could not be built.", where);
            describeFaults(maker, p, failure);
            out.failure = std::move(failure);
            return tl::unexpected(out.failure->error);
        }
        const TopoDS_Shape solids = detail::normaliseSolids(maker.Shape());
        if (solids.IsNull()) {
            out.failure = failed(std::string(words.subject) + " produced no solid.", where);
            return tl::unexpected(out.failure->error);
        }
        auto checked = detail::validateSolids(solids, words.subject, where);
        if (!checked) {
            out.failure = Failure{checked.error(), {}};
            return tl::unexpected(checked.error());
        }
        const double volume = *checked;
        OpResult op;
        op.shape = detail::wrap(solids);
        if (op.shape.count(SubKind::Solid) != p.solids) {
            out.failure =
                failed(std::string(words.subject) + " changed the number of solids from " + std::to_string(p.solids) +
                           " to " + std::to_string(op.shape.count(SubKind::Solid)) + ".",
                       where);
            return tl::unexpected(out.failure->error);
        }
        const double change = volume - p.volume;
        const double noise = kNoOpRelative * std::max(p.volume, 1.0);
        const bool wrongWay =
            (expect == Expect::Less && !(change < -noise)) || (expect == Expect::More && !(change > noise));
        if (wrongWay) {
            out.failure = failed(std::string(words.subject) + " changed the volume the wrong way (" +
                                     std::to_string(p.volume) + " -> " + std::to_string(volume) + ").",
                                 where);
            return tl::unexpected(out.failure->error);
        }
        const std::array<const Shape*, 1> inputs{&p.copy()};
        auto history = remapHistory(detail::buildHistory(maker, inputs, op.shape), p, where);
        if (!history) {
            out.failure = Failure{history.error(), {}};
            return tl::unexpected(history.error());
        }
        op.history = std::move(*history);
        const bool blended = std::any_of(op.history.entries.begin(), op.history.entries.end(), [](const auto& e) {
            return e.relation == HistoryEntry::Relation::Generated &&
                   std::any_of(e.results.begin(), e.results.end(),
                               [](const SubId& r) { return r.kind == SubKind::Face; });
        });
        if (!blended) {
            out.failure = failed(std::string(words.subject) + " made no blend face.", where);
            return tl::unexpected(out.failure->error);
        }
        return op;
    });
    if (built) {
        out.result = std::move(*built);
        return out;
    }
    if (built.error().code == ErrorCode::Cancelled || ctx.cancel.cancelled()) {
        out.cancelled = true;
        out.failure.reset();
        return out;
    }
    if (!out.failure)
        out.failure = Failure{built.error(), {}}; // OCCT threw
    return out;
}

// The vertices (input SubIds) of `edges` where four or more edges of the body meet. May throw.
std::vector<SubId> crowdedCorners(const Shape& shape, const std::vector<SubId>& edges)
{
    std::vector<SubId> out;
    const detail::ShapeIndex& edgeMap = shape.impl->subs[detail::kindSlot(SubKind::Edge)];
    const detail::AncestorIndex& vertexEdges = shape.impl->ancestors(SubKind::Vertex, SubKind::Edge);
    for (const SubId& id : edges) {
        const TopoDS_Edge edge = TopoDS::Edge(edgeMap.FindKey(static_cast<int>(id.index) + 1));
        for (const TopoDS_Vertex& vertex : {TopExp::FirstVertex(edge), TopExp::LastVertex(edge)}) {
            if (vertex.IsNull())
                continue;
            const NCollection_List<TopoDS_Shape>* around = vertexEdges.Seek(vertex);
            if (around == nullptr || around->Extent() < 4)
                continue;
            if (const auto v = detail::subIdOf(shape, vertex))
                out.push_back(*v);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Rounds down to three significant digits, so the size quoted is never above the size that worked.
double roundDown3(double value)
{
    if (!(value > 0.0) || !std::isfinite(value))
        return value;
    // Scale into [100, 1000) by powers of ten (exact IEEE steps; no platform math, section 8).
    double scale = 1.0;
    int guard = 0;
    while (value * scale < 100.0 && guard++ < 64)
        scale *= 10.0;
    while (value * scale >= 1000.0 && guard++ < 128)
        scale /= 10.0;
    return std::floor(value * scale) / scale;
}

template <class Maker>
Result<OpResult> blend(Blend kind, const Shape& shape, std::span<const SubId> edgeIds, double value, JobContext& ctx,
                       std::string_view where)
{
    const Words words = wordsFor(kind);
    if (shape.isNull() || shape.count(SubKind::Solid) == 0)
        return tl::unexpected(invalidArgument(std::string("Only a solid body can take a ") + words.noun + ".", where));
    if (edgeIds.empty())
        return tl::unexpected(invalidArgument(std::string("Pick at least one edge to ") + words.noun + ".", where));
    if (!std::isfinite(value) || !(value > kMinSize))
        return tl::unexpected(invalidArgument(std::string("The ") + words.noun + " " + words.size +
                                                  " must be a finite length greater than zero.",
                                              where));
    std::vector<SubId> picked(edgeIds.begin(), edgeIds.end());
    const std::uint32_t edgeCount = shape.count(SubKind::Edge);
    for (const SubId& id : picked) {
        if (id.kind != SubKind::Edge)
            return tl::unexpected(invalidArgument(
                std::string("Only edges can take a ") + words.noun + "; a vertex, face or solid was picked.", where));
        if (id.index >= edgeCount)
            return tl::unexpected(invalidArgument("Edge " + std::to_string(id.index) +
                                                      " does not exist in this body (it has " +
                                                      std::to_string(edgeCount) + " edges).",
                                                  where));
    }
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));

    // Arguments, the copy, the contours and the expected volume change.
    struct Plan {
        Prepared prepared;
        std::vector<SubId> extra; // edges blended by tangent propagation, not picked
        Expect expect = Expect::Either;
    };
    auto planned = detail::guarded(where, [&]() -> Result<Plan> {
        if (auto ok = checkEdges(shape, picked, words, where); !ok)
            return tl::unexpected(ok.error());
        auto prepared = prepare(shape, picked, where);
        if (!prepared)
            return tl::unexpected(prepared.error());
        Plan plan{std::move(*prepared), {}, Expect::Either};
        const Prepared& p = plan.prepared;
        // A dry run of the contours: OCCT builds them in Add(), before Build().
        Maker maker(detail::occt(p.copy()));
        for (std::size_t i = 0; i < p.edges.size(); ++i) {
            addEdge(maker, value, p.edges[i]);
            if (maker.Contour(p.edges[i]) == 0)
                return tl::unexpected(invalidArgument("Edge " + std::to_string(p.picked[i].index) + " cannot take a " +
                                                          words.noun +
                                                          ": the geometry kernel finds no corner to blend there.",
                                                      where));
        }
        bool convex = false;
        bool concave = false;
        bool other = false;
        for (int c = 1; c <= maker.NbContours(); ++c) {
            for (int j = 1; j <= maker.NbEdges(c); ++j) {
                const TopoDS_Edge& edge = maker.Edge(c, j);
                const auto id = inputId(p, edge);
                if (!id)
                    return tl::unexpected(kernelFailure("A blended edge is not an edge of the body.", where));
                if (!std::binary_search(p.picked.begin(), p.picked.end(), *id))
                    plan.extra.push_back(*id);
                const std::vector<TopoDS_Face> faces = facesOf(p.copy(), edge);
                if (faces.size() != 2) {
                    other = true;
                    continue;
                }
                switch (ChFi3d::DefineConnectType(edge, faces[0], faces[1], 1e-5, true)) {
                case ChFiDS_Convex:
                    convex = true;
                    break;
                case ChFiDS_Concave:
                    concave = true;
                    break;
                default:
                    other = true;
                    break;
                }
            }
        }
        std::sort(plan.extra.begin(), plan.extra.end());
        plan.extra.erase(std::unique(plan.extra.begin(), plan.extra.end()), plan.extra.end());
        if (!other && convex != concave)
            plan.expect = convex ? Expect::Less : Expect::More;
        return plan;
    });
    if (!planned)
        return tl::unexpected(planned.error());
    const Plan& plan = *planned;
    const Prepared& p = plan.prepared;

    ctx.progress.set(0.1, words.stage);
    Attempt first = attempt<Maker>(shape, p, value, plan.expect, words, ctx, where);
    if (first.cancelled)
        return tl::unexpected(detail::cancelledError(where));
    if (first.result) {
        OpResult out = std::move(*first.result);
        if (!plan.extra.empty())
            out.warnings.push_back(std::string(words.subject) + " also blends " + edgeList(plan.extra) +
                                   ", which continue the picked edges tangentially.");
        const std::vector<SubId> orphans = detail::orphanFaces(out.history, out.shape);
        if (!orphans.empty())
            out.warnings.push_back(detail::orphanWarning(orphans));
        ctx.progress.set(1.0, "Done");
        return out;
    }

    // Failed: look for a size that works, to tell "too large" from "cannot be blended here".
    Failure failure = std::move(*first.failure);
    ctx.progress.set(0.5, "Finding a size that fits");
    double works = 0.0;
    double fails = value;
    int tried = 0;
    for (const double factor : kShrink) {
        const double size = value * factor;
        if (!(size > kMinSize))
            break;
        ++tried;
        Attempt trial = attempt<Maker>(shape, p, size, plan.expect, words, ctx, where);
        if (trial.cancelled)
            return tl::unexpected(detail::cancelledError(where));
        ctx.progress.set(0.5 + 0.5 * tried / kSearchAttempts, "Finding a size that fits");
        if (trial.result) {
            works = size;
            break;
        }
        fails = size;
    }
    if (works > 0.0) {
        for (int step = 0; step < kRefineSteps; ++step) {
            const double size = 0.5 * (works + fails);
            ++tried;
            Attempt trial = attempt<Maker>(shape, p, size, plan.expect, words, ctx, where);
            if (trial.cancelled)
                return tl::unexpected(detail::cancelledError(where));
            ctx.progress.set(0.5 + 0.5 * tried / kSearchAttempts, "Finding a size that fits");
            (trial.result ? works : fails) = size;
        }
    }

    const std::vector<SubId>& named = failure.faultyEdges.empty() ? p.picked : failure.faultyEdges;
    std::string message;
    if (works > 0.0) {
        message = std::string("A ") + mm(value) + " " + words.noun + " is too large for " + edgeList(named) +
                  ": the faces next to " + (named.size() == 1 ? "it" : "them") +
                  " are too small to hold it, or it runs into another blend. Use a smaller " + words.size +
                  ": the largest that worked in a quick search is about " + mm(roundDown3(works)) + ".";
    } else {
        message = std::string(words.subject) + " could not be built on " + edgeList(named) +
                  ", not even with a much smaller " + words.size + " (" + mm(value * kShrink.back()) + " tried).";
        const auto corners =
            detail::guarded(where, [&]() -> Result<std::vector<SubId>> { return crowdedCorners(shape, named); });
        if (corners && !corners->empty()) {
            message += std::string(" Four or more edges meet at vertex ") + std::to_string(corners->front().index) +
                       ", which the geometry kernel cannot blend into.";
        }
        message += std::string(" Try blending the edges one at a time or in another order, or change the faces "
                               "around them.");
    }
    Error error = kernelFailure(std::move(message), where);
    for (std::string& line : failure.error.context)
        if (line != where)
            error.context.insert(error.context.begin(), std::move(line));
    error.context.insert(error.context.begin(), failure.error.message);
    return tl::unexpected(std::move(error));
}

} // namespace

Result<OpResult> fillet(const Shape& shape, std::span<const SubId> edges, double radius, JobContext& ctx)
{
    return blend<BRepFilletAPI_MakeFillet>(Blend::Fillet, shape, edges, radius, ctx, "kernel::fillet");
}

Result<OpResult> chamfer(const Shape& shape, std::span<const SubId> edges, double distance, JobContext& ctx)
{
    return blend<BRepFilletAPI_MakeChamfer>(Blend::Chamfer, shape, edges, distance, ctx, "kernel::chamfer");
}

} // namespace ray::kernel
