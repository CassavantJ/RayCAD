// The desktop viewer's non-Qt half (task T-020). See ray_api/viewer.h.
#include <ray_api/viewer.h>

#include <ray/kernel.h>
#include <ray/naming.h>

// naming.h declares IResolver but no factory; src/naming keeps it in a private header until the document rebuilder
// (T-010) needs it public. Reported in the T-020 hand-off: the factory belongs in naming.h.
#include "../naming/resolver.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace ray::api::viewer {

namespace {

std::string lowerExtension(std::string_view path)
{
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string ext(path.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool isStep(std::string_view path)
{
    const std::string ext = lowerExtension(path);
    return ext == "step" || ext == "stp";
}

bool isIges(std::string_view path)
{
    const std::string ext = lowerExtension(path);
    return ext == "iges" || ext == "igs";
}

double distance(Vec3 a, Vec3 b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kImportShare = 0.92; // of the job's progress; naming takes the rest

} // namespace

bool isImportable(std::string_view path)
{
    return isStep(path) || isIges(path);
}

Result<ImportedModel> importModel(const std::string& utf8Path, JobContext& ctx)
{
    if (!isImportable(utf8Path))
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Only STEP and IGES files can be opened here.", {}});
    JobContext importCtx{ctx.cancel, ctx.progress.sub(kImportShare)};
    const kernel::ImportOptions options;
    Result<kernel::ImportResult> imported = isStep(utf8Path) ? kernel::importStep(utf8Path, options, importCtx)
                                                             : kernel::importIges(utf8Path, options, importCtx);
    if (!imported)
        return tl::unexpected(imported.error());
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());

    ImportedModel out;
    out.sourceUnits = imported->sourceUnits;
    out.warnings = std::move(imported->warnings);
    const FeatureId importFeature{Uuid::generate()};
    Progress naming = ctx.progress.sub(1.0 - kImportShare);
    const std::size_t count = imported->bodies.size();
    out.bodies.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (auto ok = ctx.cancel.check(); !ok)
            return tl::unexpected(ok.error());
        kernel::ImportedBody& source = imported->bodies[i];
        Body body;
        body.id = BodyId{Uuid::generate()};
        body.name = source.name.empty() ? "Body " + std::to_string(i + 1) : source.name;
        body.color = source.color;
        body.map = naming::nameImported(importFeature, source.shape);
        body.shape = std::move(source.shape);
        out.bodies.push_back(std::move(body));
        naming.set(double(i + 1) / double(count), "Naming");
    }
    naming.set(1.0, "Naming");
    return out;
}

std::vector<Body> bodiesFromDocument(const doc::DocState& state, const doc::BuildResult& build)
{
    std::vector<Body> out;
    out.reserve(state.bodies.size());
    for (const doc::Body& source : state.bodies) {
        const auto it = build.bodies.find(source.id);
        if (it == build.bodies.end() || it->second.shape.isNull())
            continue;
        Body body;
        body.id = source.id;
        body.name = source.name;
        body.shape = it->second.shape;
        if (it->second.map.size() > 0)
            body.map = it->second.map;
        body.color = source.color;
        body.visible = source.visible;
        out.push_back(std::move(body));
    }
    return out;
}

std::vector<FeatureRow> featuresOf(const doc::DocState& state, const doc::BuildResult* build)
{
    std::vector<FeatureRow> out;
    out.reserve(state.features.size());
    for (const doc::Feature& feature : state.features) {
        FeatureRow row;
        row.id = feature.id;
        row.name = feature.name.empty() ? feature.type : feature.name;
        row.type = feature.type;
        row.suppressed = feature.suppressed;
        if (build != nullptr) {
            for (const doc::FeatureReport& report : build->reports) {
                if (report.id == feature.id)
                    row.status = report.status;
            }
        }
        out.push_back(std::move(row));
    }
    return out;
}

Result<kernel::Mesh> meshBody(const kernel::Shape& shape, double chordMm, double angleDeg, JobContext& ctx)
{
    return kernel::tessellate(shape, kernel::MeshOptions{chordMm, angleDeg * kDegToRad}, ctx);
}

std::optional<kernel::SubId> vertexOfEdgeEnd(const kernel::Shape& shape, std::uint32_t edgeIndex, Vec3 point)
{
    const std::vector<kernel::SubId> ends =
        kernel::adjacent(shape, kernel::SubId{kernel::SubKind::Edge, edgeIndex}, kernel::SubKind::Vertex);
    std::optional<kernel::SubId> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const kernel::SubId vertex : ends) {
        const Result<Vec3> at = kernel::vertexPoint(shape, vertex);
        if (!at)
            continue;
        const double d = distance(*at, point);
        if (d < bestDistance) {
            bestDistance = d;
            best = vertex;
        }
    }
    return best;
}

SelectionResolver::SelectionResolver(std::span<const Body> bodies)
{
    std::vector<naming::ResolverBody> named;
    for (const Body& body : bodies) {
        if (!body.map)
            continue;
        named.push_back(naming::ResolverBody{body.id, *body.map, body.shape});
        named_.push_back(body.id);
    }
    std::sort(named_.begin(), named_.end());
    resolver_ = naming::makeResolver(std::move(named));
}

SelectionResolver::~SelectionResolver() = default;

std::optional<naming::TopoRef> SelectionResolver::makeRef(BodyId body, kernel::SubId sub) const
{
    if (!resolver_ || sub.kind == kernel::SubKind::Solid || !std::binary_search(named_.begin(), named_.end(), body))
        return std::nullopt;
    return resolver_->makeRef(body, sub);
}

naming::Resolved SelectionResolver::resolve(const naming::TopoRef& ref) const
{
    if (!resolver_)
        return naming::Resolved{naming::ResolveState::Broken, {}, "No element names are available."};
    return resolver_->resolve(ref);
}

Result<ElementInfo> describeElement(const kernel::Shape& shape, kernel::SubId sub, JobContext& ctx)
{
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());
    ElementInfo info;
    info.kind = sub.kind;
    switch (sub.kind) {
    case kernel::SubKind::Face: {
        Result<kernel::SurfaceInfo> surface = kernel::surfaceInfo(shape, sub);
        if (!surface)
            return tl::unexpected(surface.error());
        info.surface = *surface;
        break;
    }
    case kernel::SubKind::Edge: {
        Result<kernel::CurveInfo> curve = kernel::curveInfo(shape, sub);
        if (!curve)
            return tl::unexpected(curve.error());
        info.curve = *curve;
        break;
    }
    case kernel::SubKind::Vertex: {
        Result<Vec3> point = kernel::vertexPoint(shape, sub);
        if (!point)
            return tl::unexpected(point.error());
        info.point = *point;
        break;
    }
    case kernel::SubKind::Solid: {
        Result<kernel::MassProps> mass = kernel::massProps(shape);
        if (!mass)
            return tl::unexpected(mass.error());
        info.mass = *mass;
        info.bounds = kernel::bounds(shape);
        break;
    }
    }
    return info;
}

std::string_view surfaceTypeName(kernel::SurfaceType type)
{
    switch (type) {
    case kernel::SurfaceType::Plane:
        return "Plane";
    case kernel::SurfaceType::Cylinder:
        return "Cylinder";
    case kernel::SurfaceType::Cone:
        return "Cone";
    case kernel::SurfaceType::Sphere:
        return "Sphere";
    case kernel::SurfaceType::Torus:
        return "Torus";
    case kernel::SurfaceType::Bspline:
        return "B-spline";
    case kernel::SurfaceType::Other:
        break;
    }
    return "Other";
}

std::string_view curveTypeName(kernel::CurveType type)
{
    switch (type) {
    case kernel::CurveType::Line:
        return "Line";
    case kernel::CurveType::Circle:
        return "Circle";
    case kernel::CurveType::Ellipse:
        return "Ellipse";
    case kernel::CurveType::Bspline:
        return "B-spline";
    case kernel::CurveType::Other:
        break;
    }
    return "Other";
}

std::unique_ptr<IJobSystem> makeViewerJobs(IUiDispatcher& dispatcher, int workerCount)
{
    return makeJobSystem(dispatcher, workerCount);
}

} // namespace ray::api::viewer
