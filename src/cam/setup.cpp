// Setup resolution (cam.h, task T-033): a setup's WCS frame and stock (resolveSetup), and the ResolvedGeometry an
// operation is generated on (resolveSetupGeometry, the engine's default ResolveGeometryFn).
//
// cam.h states the rules (frame, stock, corner numbering, rounding); this file follows them in this order:
//   1. the WCS axes (rows of R) from the Z face, the X edge or the model axes: directions rounded to 2^-40 per
//      component, then IEEE arithmetic and dmath::sqrt only, so the same model gives the same bits everywhere; the
//      flips (Wcs::flipZ before X is chosen, Wcs::flipX after) are exact negations;
//   2. the part box in the WCS orientation ("O": p -> R p, no translation): taken from the model box exactly when R
//      only permutes and negates axes (the usual case, and cheap), else measured on the body rotated into O
//      (kernel::boundsIn: the rotation as a location, nothing copied);
//   3. everything after that in integer nanometres in O: part box, stock box, origin; the WCS is O shifted by the
//      origin, so stockBounds and partBounds are whole nanometres in the WCS.
// What it reads is what operationInputHash covers (state.cpp putSetup): the part and stock bodies by content, the
// stock definition, the origin mode and corner, the flips, and the WCS references through the geometry of what they
// resolve to (surface normal and orientation, curve direction, vertex point). Nothing else of the setup is read.
#include "state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <new>
#include <utility>

namespace ray::cam {

namespace {

using detail::label;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// ---------- Vectors (IEEE arithmetic only; ray_cam is built without floating-point contraction) ----------

Vec3 add(Vec3 a, Vec3 b)
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(Vec3 a, Vec3 b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(Vec3 a, double s)
{
    return Vec3{a.x * s, a.y * s, a.z * s};
}
double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(Vec3 a)
{
    return dmath::sqrt(dot(a, a));
}
double component(Vec3 a, int i)
{
    return i == 0 ? a.x : i == 1 ? a.y : a.z;
}
Vec3 axis(int i, double sign = 1.0)
{
    return Vec3{i == 0 ? sign : 0.0, i == 1 ? sign : 0.0, i == 2 ? sign : 0.0};
}

// A direction read from the model, rounded per component to a multiple of 2^-40, ties to even. Scaling by a power
// of two is exact, so this is exactly that rounding (std::nearbyint in the default rounding mode).
Vec3 quantized(Vec3 v)
{
    constexpr double kUp = 0x1p40;
    constexpr double kDown = 0x1p-40;
    return Vec3{std::nearbyint(v.x * kUp) * kDown, std::nearbyint(v.y * kUp) * kDown,
                std::nearbyint(v.z * kUp) * kDown};
}

// The unit vector along `v`; nullopt for a zero or non-finite vector.
std::optional<Vec3> unit(Vec3 v)
{
    const double len = length(v);
    if (!(len > 0.0) || !std::isfinite(len)) {
        return std::nullopt;
    }
    return Vec3{v.x / len, v.y / len, v.z / len};
}

// A unit vector within 1e-9 of a model axis (every other component that small) is that axis exactly.
constexpr double kAxisSnap = 1e-9;
Vec3 snapped(Vec3 u)
{
    int big = 0;
    for (int i = 1; i < 3; ++i) {
        if (std::fabs(component(u, i)) > std::fabs(component(u, big))) {
            big = i;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (i != big && std::fabs(component(u, i)) > kAxisSnap) {
            return u;
        }
    }
    return axis(big, component(u, big) < 0.0 ? -1.0 : 1.0);
}

// `u` snapped, unless snapping would make it stop being square to `z` (then `u` as it is).
Vec3 snappedSquareTo(Vec3 u, Vec3 z)
{
    const Vec3 s = snapped(u);
    return dot(s, z) == 0.0 ? s : u;
}

// ---------- Nanometre boxes ----------

struct NmBox {
    std::array<Nm, 3> lo{}, hi{};
};

NmBox toNmBox(const Box3& b)
{
    return NmBox{{toNm(b.min.x), toNm(b.min.y), toNm(b.min.z)}, {toNm(b.max.x), toNm(b.max.y), toNm(b.max.z)}};
}

// (a + b) / 2 rounded to the nearest nanometre, ties to even.
Nm midpoint(Nm a, Nm b)
{
    const Nm s = a + b;
    const Nm floorHalf = s >= 0 ? s / 2 : -((-s + 1) / 2);
    if (s - 2 * floorHalf == 0) {
        return floorHalf;
    }
    return floorHalf % 2 == 0 ? floorHalf : floorHalf + 1;
}

Box3 toWcs(const NmBox& b, const std::array<Nm, 3>& origin)
{
    return Box3{Vec3{toMm(b.lo[0] - origin[0]), toMm(b.lo[1] - origin[1]), toMm(b.lo[2] - origin[2])},
                Vec3{toMm(b.hi[0] - origin[0]), toMm(b.hi[1] - origin[1]), toMm(b.hi[2] - origin[2])}};
}

// ---------- References ----------

struct Element {
    const naming::ResolverBody* body = nullptr;
    kernel::SubId sub{};
};

const char* kindWord(kernel::SubKind kind)
{
    switch (kind) {
    case kernel::SubKind::Vertex:
        return "vertex";
    case kernel::SubKind::Edge:
        return "edge";
    case kernel::SubKind::Face:
        return "face";
    case kernel::SubKind::Solid:
        return "body";
    }
    return "element";
}

std::string withArticle(kernel::SubKind kind)
{
    const std::string word = kindWord(kind);
    return (kind == kernel::SubKind::Edge ? "an " : "a ") + word;
}

// One element of kind `kind` that `ref` resolves to, usable for CAM (detail::resolveForCam).
Result<Element> resolveOne(const CamSnapshot& snapshot, const naming::IResolver& resolver, const naming::TopoRef& ref,
                           kernel::SubKind kind, const std::string& what)
{
    if (ref.kind != kind) {
        return failure(ErrorCode::InvalidArgument,
                       what + " must be " + withArticle(kind) + ", not " + withArticle(ref.kind));
    }
    auto resolved = detail::resolveForCam(resolver, snapshot, ref, what);
    if (!resolved) {
        return tl::unexpected(resolved.error());
    }
    if (resolved->elements.size() != 1 || resolved->elements[0].kind != kind) {
        return failure(ErrorCode::InvalidArgument, what + " stands for " + std::to_string(resolved->elements.size()) +
                                                       " elements; pick a single " + kindWord(kind));
    }
    return Element{detail::bodyOf(snapshot, ref.body), resolved->elements[0]};
}

const char* surfaceWord(kernel::SurfaceType type)
{
    switch (type) {
    case kernel::SurfaceType::Plane:
        return "a plane";
    case kernel::SurfaceType::Cylinder:
        return "a cylinder";
    case kernel::SurfaceType::Cone:
        return "a cone";
    case kernel::SurfaceType::Sphere:
        return "a sphere";
    case kernel::SurfaceType::Torus:
        return "a torus";
    case kernel::SurfaceType::Bspline:
        return "a free-form surface";
    case kernel::SurfaceType::Other:
        break;
    }
    return "curved";
}

const char* curveWord(kernel::CurveType type)
{
    switch (type) {
    case kernel::CurveType::Line:
        return "a line";
    case kernel::CurveType::Circle:
        return "an arc";
    case kernel::CurveType::Ellipse:
        return "an elliptical arc";
    case kernel::CurveType::Bspline:
        return "a free-form curve";
    case kernel::CurveType::Other:
        break;
    }
    return "curved";
}

// ---------- The frame ----------

// The WCS axes in model coordinates: the rows of R.
struct Axes {
    Vec3 x, y, z;
};

constexpr double kParallelSin = 1e-3;                      // sine of the smallest X edge to Z angle (0.057 degrees)
constexpr double kCos45 = 0.70710678118654752440084436210; // the double nearest cos 45 degrees

Result<Axes> frameAxes(const CamSnapshot& snapshot, const Setup& setup, const naming::IResolver& resolver)
{
    const Wcs& wcs = setup.wcs;
    Vec3 z{0.0, 0.0, 1.0};
    if (wcs.zFace) {
        const std::string what = "The Z face of setup " + label(setup);
        auto face = resolveOne(snapshot, resolver, *wcs.zFace, kernel::SubKind::Face, what);
        if (!face) {
            return tl::unexpected(face.error());
        }
        auto info = kernel::surfaceInfo(face->body->shape, face->sub);
        if (!info) {
            return failure(ErrorCode::KernelFailure, what + " could not be read: " + info.error().message);
        }
        if (info->type != kernel::SurfaceType::Plane) {
            return failure(ErrorCode::InvalidArgument, what + " is not flat (it is " + surfaceWord(info->type) +
                                                           "); pick a planar face for the Z axis");
        }
        Vec3 n = quantized(info->axis);
        if (info->reversed) {
            n = mul(n, -1.0); // the face's outward side is against its surface normal
        }
        const auto u = unit(n);
        if (!u) {
            return failure(ErrorCode::KernelFailure, what + " has no usable normal");
        }
        z = snapped(*u);
    }
    if (wcs.flipZ) {
        z = mul(z, -1.0); // exact; X is chosen below for the reversed Z
    }

    Vec3 x{};
    if (wcs.xEdge) {
        const std::string what = "The X edge of setup " + label(setup);
        auto edge = resolveOne(snapshot, resolver, *wcs.xEdge, kernel::SubKind::Edge, what);
        if (!edge) {
            return tl::unexpected(edge.error());
        }
        auto info = kernel::curveInfo(edge->body->shape, edge->sub);
        if (!info) {
            return failure(ErrorCode::KernelFailure, what + " could not be read: " + info.error().message);
        }
        if (info->type != kernel::CurveType::Line) {
            return failure(ErrorCode::InvalidArgument, what + " is not straight (it is " + curveWord(info->type) +
                                                           "); pick a straight edge for the X axis");
        }
        const auto d = unit(quantized(info->axis));
        if (!d) {
            return failure(ErrorCode::KernelFailure, what + " has no usable direction");
        }
        const Vec3 p = sub(*d, mul(z, dot(*d, z)));
        const double len = length(p);
        if (!(len >= kParallelSin)) {
            return failure(ErrorCode::InvalidArgument,
                           what + " runs along the Z axis (within 0.06 degrees), so it gives no X direction; pick an "
                                  "edge that runs across the Z face");
        }
        x = snappedSquareTo(Vec3{p.x / len, p.y / len, p.z / len}, z);
    } else if (std::fabs(z.z) >= kCos45) {
        // Model +X projected: its length is at least sin 45 here.
        const Vec3 p = sub(axis(0), mul(z, z.x));
        x = snappedSquareTo(unit(p).value_or(axis(0)), z);
    } else {
        // Model +Z projected is Y: its length is above sin 45 here.
        const Vec3 q = sub(axis(2), mul(z, z.z));
        const Vec3 y = snappedSquareTo(unit(q).value_or(axis(2)), z);
        x = cross(y, z);
    }
    if (wcs.flipX) {
        x = mul(x, -1.0); // exact
    }
    // Y = Z x X, then X again from Y and Z, so the three are orthonormal to rounding (exact for axis-aligned frames).
    const Vec3 y = unit(cross(z, x)).value_or(Vec3{});
    x = cross(y, z);
    return Axes{x, y, z};
}

// Whether every row of R is a model axis or its negative (R then only permutes and negates coordinates).
bool permutesAxes(const Axes& r)
{
    for (const Vec3& row : {r.x, r.y, r.z}) {
        int ones = 0;
        for (int i = 0; i < 3; ++i) {
            const double c = component(row, i);
            if (c == 1.0 || c == -1.0) {
                ++ones;
            } else if (c != 0.0) {
                return false;
            }
        }
        if (ones != 1) {
            return false;
        }
    }
    return true;
}

Xform rotationOf(const Axes& r)
{
    return Xform{{r.x.x, r.x.y, r.x.z, 0.0, r.y.x, r.y.y, r.y.z, 0.0, r.z.x, r.z.y, r.z.z, 0.0}};
}

// The box of `shape` in the WCS orientation O (p -> R p), or an error naming `what`.
Result<Box3> boundsIn(const kernel::Shape& shape, const Axes& r, const std::string& what)
{
    const auto measured = [&](const Box3& b) -> Result<Box3> {
        const double v[] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
        const bool numbers = std::all_of(std::begin(v), std::end(v), [](double c) { return std::isfinite(c); });
        const bool zero = std::all_of(std::begin(v), std::end(v), [](double c) { return c == 0.0; });
        if (!numbers || zero || b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z) {
            return failure(ErrorCode::KernelFailure, what + " could not be measured");
        }
        return b;
    };
    if (permutesAxes(r)) {
        auto model = measured(kernel::bounds(shape));
        if (!model) {
            return model;
        }
        // Row i of R picks model coordinate j with a sign: exact.
        Box3 out{};
        double* lo[] = {&out.min.x, &out.min.y, &out.min.z};
        double* hi[] = {&out.max.x, &out.max.y, &out.max.z};
        const Vec3 rows[] = {r.x, r.y, r.z};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const double c = component(rows[i], j);
                if (c == 1.0) {
                    *lo[i] = component(model->min, j);
                    *hi[i] = component(model->max, j);
                } else if (c == -1.0) {
                    *lo[i] = -component(model->max, j);
                    *hi[i] = -component(model->min, j);
                }
            }
        }
        return out;
    }
    // Measured with the rotation as a location (T-044): no copy of the body is moved and checked, so a tilted frame
    // costs about what an axis-aligned one does (the live setup preview resolves on every change).
    auto box = kernel::boundsIn(shape, rotationOf(r));
    if (!box) {
        return failure(ErrorCode::KernelFailure,
                       what + " could not be measured in the WCS orientation: " + box.error().message);
    }
    return measured(*box);
}

Box3 unite(const Box3& a, const Box3& b)
{
    return Box3{Vec3{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
                Vec3{std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

bool allFinite(std::initializer_list<double> values)
{
    return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

} // namespace

// ---------- resolveSetup ----------

Result<ResolvedSetup> resolveSetup(const CamSnapshot& snapshot, const Setup& setup, const naming::IResolver& resolver,
                                   JobContext& ctx, SetupResolveOptions options)
{
    try {
        RAY_CAM_TRY(ctx.cancel.check());
        const std::string where = "setup " + label(setup);

        // 1. Axes.
        auto axes = frameAxes(snapshot, setup, resolver);
        if (!axes) {
            return tl::unexpected(axes.error());
        }
        const Axes& r = *axes;

        // 2. The part box in the WCS orientation.
        std::optional<Box3> part;
        for (std::size_t i = 0; i < setup.partBodies.size(); ++i) {
            RAY_CAM_TRY(ctx.cancel.check());
            const BodyId& id = setup.partBodies[i];
            const naming::ResolverBody* body = detail::bodyOf(snapshot, id);
            const std::string what = "Part body " + std::to_string(i + 1) + " of " + where;
            if (body == nullptr || body->shape.isNull()) {
                return failure(ErrorCode::ReferenceBroken, what + " (body " + id.value.toString() + ") is not built");
            }
            auto b = boundsIn(body->shape, r, what);
            if (!b) {
                return tl::unexpected(b.error());
            }
            part = part ? unite(*part, *b) : *b;
        }
        const NmBox partNm = part ? toNmBox(*part) : NmBox{};

        // 3. The stock box, in nanometres in the WCS orientation; `cylinderAxis` >= 0 for a cylinder.
        NmBox stockNm;
        int cylinderAxis = -1;
        Nm cylinderRadius = 0;
        const naming::ResolverBody* stockBody = nullptr;
        const auto needPart = [&](const char* kind) -> Status {
            if (!part) {
                return failure(ErrorCode::InvalidArgument, std::string(kind) + " stock of " + where +
                                                               " is sized from the part, and the setup has no part "
                                                               "bodies");
            }
            return {};
        };
        if (const auto* box = std::get_if<BoxStock>(&setup.stock)) {
            RAY_CAM_TRY(needPart("The box"));
            if (!allFinite({box->xNeg, box->xPos, box->yNeg, box->yPos, box->zNeg, box->zPos})) {
                return failure(ErrorCode::InvalidArgument, "The box stock offsets of " + where + " must be finite");
            }
            const double neg[] = {box->xNeg, box->yNeg, box->zNeg};
            const double pos[] = {box->xPos, box->yPos, box->zPos};
            for (int i = 0; i < 3; ++i) {
                stockNm.lo[i] = partNm.lo[i] - toNm(neg[i]);
                stockNm.hi[i] = partNm.hi[i] + toNm(pos[i]);
                if (!(stockNm.lo[i] < stockNm.hi[i])) {
                    return failure(ErrorCode::InvalidArgument,
                                   "The box stock offsets of " + where + " leave no stock along " + "XYZ"[i]);
                }
            }
        } else if (const auto* cyl = std::get_if<CylinderStock>(&setup.stock)) {
            RAY_CAM_TRY(needPart("The cylinder"));
            if (!allFinite({cyl->diameter, cyl->length, cyl->endOffset}) || !(cyl->diameter > 0.0) ||
                !(cyl->length > 0.0) || cyl->axis < 0 || cyl->axis > 2) {
                return failure(ErrorCode::InvalidArgument,
                               "The cylinder stock of " + where +
                                   " needs a diameter and a length above 0 and an axis of X, Y or Z");
            }
            cylinderAxis = cyl->axis;
            cylinderRadius = toNm(cyl->diameter / 2.0);
            const Nm lengthNm = toNm(cyl->length);
            if (cylinderRadius <= 0 || lengthNm <= 0) {
                return failure(ErrorCode::InvalidArgument,
                               "The cylinder stock of " + where + " is thinner or shorter than a nanometre");
            }
            for (int i = 0; i < 3; ++i) {
                if (i == cylinderAxis) {
                    stockNm.hi[i] = partNm.hi[i] + toNm(cyl->endOffset);
                    stockNm.lo[i] = stockNm.hi[i] - lengthNm;
                } else {
                    const Nm centre = midpoint(partNm.lo[i], partNm.hi[i]);
                    stockNm.lo[i] = centre - cylinderRadius;
                    stockNm.hi[i] = centre + cylinderRadius;
                }
            }
        } else if (const auto* solid = std::get_if<SolidStock>(&setup.stock)) {
            stockBody = detail::bodyOf(snapshot, solid->body);
            const std::string what = "The stock body of " + where;
            if (stockBody == nullptr || stockBody->shape.isNull()) {
                return failure(ErrorCode::ReferenceBroken,
                               what + " (body " + solid->body.value.toString() + ") is not built");
            }
            RAY_CAM_TRY(ctx.cancel.check());
            auto b = boundsIn(stockBody->shape, r, what);
            if (!b) {
                return tl::unexpected(b.error());
            }
            stockNm = toNmBox(*b);
        } else if (const auto* from = std::get_if<FromSetupStock>(&setup.stock)) {
            // The seam for in-process stock (PLAN.md T-075): the simulated result of the earlier setup, re-oriented
            // into this one, once the stock simulation exists. Until then nothing honest can be returned.
            const Setup* previous = snapshot.cam ? detail::findSetup(*snapshot.cam, from->previous) : nullptr;
            return failure(ErrorCode::InvalidArgument,
                           "The stock of " + where + " comes from " +
                               (previous ? "setup " + label(*previous) : "setup " + from->previous.value.toString()) +
                               ", and stock carried over from an earlier setup is not available yet: use box, "
                               "cylinder or solid stock");
        }

        // 4. The origin, in nanometres in the WCS orientation.
        std::array<Nm, 3> origin{};
        const Wcs& wcs = setup.wcs;
        switch (wcs.originMode) {
        case Wcs::OriginMode::StockCorner: {
            const int k = wcs.stockCornerIndex;
            if (k < 0 || k > 7) {
                return failure(ErrorCode::InvalidArgument,
                               "The stock corner of " + where + " must be 0 to 7, not " + std::to_string(k));
            }
            origin[0] = (k == 1 || k == 2 || k == 5 || k == 6) ? stockNm.hi[0] : stockNm.lo[0];
            origin[1] = (k == 2 || k == 3 || k == 6 || k == 7) ? stockNm.hi[1] : stockNm.lo[1];
            origin[2] = k < 4 ? stockNm.hi[2] : stockNm.lo[2];
            break;
        }
        case Wcs::OriginMode::StockTopCenter:
            origin = {midpoint(stockNm.lo[0], stockNm.hi[0]), midpoint(stockNm.lo[1], stockNm.hi[1]), stockNm.hi[2]};
            break;
        case Wcs::OriginMode::ModelPoint: {
            const std::string what = "The origin of " + where;
            if (!wcs.originVertex) {
                return failure(ErrorCode::InvalidArgument, what + " is a model point, and no vertex is picked");
            }
            auto vertex = resolveOne(snapshot, resolver, *wcs.originVertex, kernel::SubKind::Vertex, what);
            if (!vertex) {
                return tl::unexpected(vertex.error());
            }
            auto p = kernel::vertexPoint(vertex->body->shape, vertex->sub);
            if (!p) {
                return failure(ErrorCode::KernelFailure, what + " could not be read: " + p.error().message);
            }
            origin = {toNm(dot(r.x, *p)), toNm(dot(r.y, *p)), toNm(dot(r.z, *p))};
            break;
        }
        default:
            return failure(ErrorCode::InvalidArgument, "The origin mode of " + where + " is not known");
        }

        // 5. The transforms: WCS = O shifted by the origin.
        ResolvedSetup out;
        const Vec3 o{toMm(origin[0]), toMm(origin[1]), toMm(origin[2])};
        out.wcsFromModel = Xform{{r.x.x, r.x.y, r.x.z, -o.x, r.y.x, r.y.y, r.y.z, -o.y, r.z.x, r.z.y, r.z.z, -o.z}};
        // The inverse: p = R^T (q + o).
        const Vec3 t = add(add(mul(r.x, o.x), mul(r.y, o.y)), mul(r.z, o.z));
        out.modelFromWcs = Xform{{r.x.x, r.y.x, r.z.x, t.x, r.x.y, r.y.y, r.z.y, t.y, r.x.z, r.y.z, r.z.z, t.z}};
        out.partBounds = part ? toWcs(partNm, origin) : Box3{};
        out.stockBounds = toWcs(stockNm, origin);

        // 6. The stock solid, in the WCS, when asked for.
        if (options.stockSolid) {
            RAY_CAM_TRY(ctx.cancel.check());
            const Box3& s = out.stockBounds;
            Result<kernel::Shape> made = kernel::Shape{};
            if (stockBody != nullptr) {
                // kernel::transformed hands the identity back as the input shape itself.
                if (auto moved = kernel::transformed(stockBody->shape, out.wcsFromModel)) {
                    made = moved->shape;
                } else {
                    made = tl::unexpected(moved.error());
                }
            } else if (cylinderAxis >= 0) {
                Vec3 base{(s.min.x + s.max.x) / 2.0, (s.min.y + s.max.y) / 2.0, (s.min.z + s.max.z) / 2.0};
                double* along[] = {&base.x, &base.y, &base.z};
                *along[cylinderAxis] = component(s.min, cylinderAxis);
                made = kernel::makeCylinder(base, axis(cylinderAxis), toMm(cylinderRadius),
                                            component(s.max, cylinderAxis) - component(s.min, cylinderAxis));
            } else {
                made = kernel::makeBox(s.min, s.max);
            }
            if (!made || made->isNull()) {
                return failure(ErrorCode::KernelFailure, "The stock solid of " + where + " could not be made" +
                                                             (made ? std::string() : ": " + made.error().message));
            }
            out.stockSolid = *made;
        }
        ctx.progress.set(1.0);
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while resolving a setup");
    }
}

// ---------- resolveSetupGeometry ----------

Result<ResolvedGeometry> resolveSetupGeometry(const GeometryRequest& request, JobContext& ctx)
{
    try {
        const CamSnapshot& snapshot = request.snapshot;
        const Setup& setup = request.setup;
        auto resolved = resolveSetup(snapshot, setup, request.resolver, ctx);
        if (!resolved) {
            return tl::unexpected(resolved.error());
        }
        // T-077: a fixture is never machined, whatever the setup lists.
        const auto isFixture = [&](const BodyId& id) {
            return std::binary_search(snapshot.documentFixtures.begin(), snapshot.documentFixtures.end(), id) ||
                   std::find(setup.fixtureBodies.begin(), setup.fixtureBodies.end(), id) != setup.fixtureBodies.end();
        };
        for (std::size_t i = 0; i < setup.partBodies.size(); ++i) {
            if (isFixture(setup.partBodies[i])) {
                return failure(ErrorCode::InvalidArgument,
                               "Part body " + std::to_string(i + 1) + " of setup " + label(setup) + " (body " +
                                   setup.partBodies[i].value.toString() +
                                   ") is a fixture: fixtures are never machined. Make it a fixture body of the setup.");
            }
        }
        for (std::size_t i = 0; i < request.op.geometry.size(); ++i) {
            if (isFixture(request.op.geometry[i].body)) {
                return failure(ErrorCode::InvalidArgument,
                               "Geometry " + std::to_string(i + 1) + " of " + label(request.op) +
                                   " is on a fixture (body " + request.op.geometry[i].body.value.toString() +
                                   "): fixtures are never machining geometry. Pick it on the part.");
            }
        }
        ResolvedGeometry geom;
        geom.wcsFromModel = resolved->wcsFromModel;
        geom.stockBounds = resolved->stockBounds;
        for (const BodyId& id : setup.partBodies) {
            const naming::ResolverBody* body = detail::bodyOf(snapshot, id); // resolveSetup checked they are built
            geom.partShapes.push_back(body->shape);
            geom.partNames.push_back(body->map); // T-035: the thread pickup of hole recognition (holeOptionsFor)
        }
        geom.holeThreads = snapshot.holeThreads;
        for (std::size_t i = 0; i < setup.fixtureBodies.size(); ++i) {
            const BodyId& id = setup.fixtureBodies[i];
            const naming::ResolverBody* body = detail::bodyOf(snapshot, id);
            if (body == nullptr || body->shape.isNull()) {
                return failure(ErrorCode::ReferenceBroken, "Fixture body " + std::to_string(i + 1) + " of setup " +
                                                               label(setup) + " (body " + id.value.toString() +
                                                               ") is not built");
            }
            geom.fixtureShapes.push_back(body->shape);
        }
        RAY_CAM_TRY(ctx.cancel.check());
        for (std::size_t i = 0; i < request.op.geometry.size(); ++i) {
            const naming::TopoRef& ref = request.op.geometry[i];
            const auto part = std::find(setup.partBodies.begin(), setup.partBodies.end(), ref.body);
            const std::string what = "Geometry " + std::to_string(i + 1) + " of " + label(request.op);
            if (part == setup.partBodies.end()) {
                return failure(ErrorCode::ReferenceBroken, what + " is not on a part body of setup " + label(setup));
            }
            auto element = detail::resolveForCam(request.resolver, snapshot, ref, what);
            if (!element) {
                return tl::unexpected(element.error());
            }
            for (const kernel::SubId& e : element->elements) {
                geom.selected.push_back(e);
                geom.selectedShapeIndex.push_back(static_cast<int>(part - setup.partBodies.begin()));
            }
        }
        return geom;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while resolving a setup");
    }
}

} // namespace ray::cam
