// The feature tools' helpers (ray_api/feature_edit.h). They reuse doc's own region finder (src/doc/features, private
// to doc and its friends, as sketch_edit.cpp does), so a region the UI picks is the region the extrude rebuild sweeps.
#include <ray_api/feature_edit.h>

#include <ray_api/sketch_edit.h>

#include "../doc/features/sketch_feature.h"
#include "app_impl.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ray::api::feature_edit {
namespace {

constexpr double kPi = 3.14159265358979323846;

double dist2(Vec2 a, Vec2 b)
{
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
}

std::vector<Vec2> arc(Vec2 c, double r, double start, double sweep)
{
    const int n = std::max(4, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 36))));
    std::vector<Vec2> out;
    for (int i = 0; i <= n; ++i) {
        const double a = start + sweep * i / n;
        out.push_back(Vec2{c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return out;
}

// One profile curve as a polyline from its start to its end (as stored).
std::vector<Vec2> curvePolyline(const kernel::ProfileCurve& c)
{
    using Kind = kernel::ProfileCurve::Kind;
    switch (c.kind) {
    case Kind::Line:
        return c.pts.size() == 2 ? c.pts : std::vector<Vec2>{};
    case Kind::Arc: {
        if (c.pts.size() != 3) {
            return {};
        }
        const Vec2 s = c.pts[0], e = c.pts[1], m = c.pts[2];
        const double r = std::hypot(s.x - m.x, s.y - m.y);
        const double a0 = std::atan2(s.y - m.y, s.x - m.x);
        const double a1 = std::atan2(e.y - m.y, e.x - m.x);
        double sweep = a1 - a0;
        if (c.ccw) {
            while (sweep <= 0) {
                sweep += 2 * kPi;
            }
        } else {
            while (sweep >= 0) {
                sweep -= 2 * kPi;
            }
        }
        return arc(m, r, a0, sweep);
    }
    case Kind::Circle:
        return c.pts.empty() ? std::vector<Vec2>{} : arc(c.pts[0], c.radius, 0, 2 * kPi);
    case Kind::Bspline:
        return c.pts;
    }
    return {};
}

// The curves of a loop chained end to start into one closed polyline (curves may come in any order and direction).
std::vector<Vec2> loopPolyline(const kernel::ProfileLoop& loop)
{
    std::vector<std::vector<Vec2>> pieces;
    for (const kernel::ProfileCurve& c : loop.curves) {
        auto p = curvePolyline(c);
        if (p.size() >= 2) {
            pieces.push_back(std::move(p));
        }
    }
    std::vector<Vec2> out;
    if (pieces.empty()) {
        return out;
    }
    std::vector<bool> used(pieces.size(), false);
    out = pieces[0];
    used[0] = true;
    for (std::size_t n = 1; n < pieces.size(); ++n) {
        const Vec2 end = out.back();
        std::size_t best = pieces.size();
        bool reverse = false;
        double bestD = 0;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const double ds = dist2(end, pieces[i].front());
            const double de = dist2(end, pieces[i].back());
            if (best == pieces.size() || std::min(ds, de) < bestD) {
                best = i;
                reverse = de < ds;
                bestD = std::min(ds, de);
            }
        }
        used[best] = true;
        std::vector<Vec2> piece = pieces[best];
        if (reverse) {
            std::reverse(piece.begin(), piece.end());
        }
        out.insert(out.end(), piece.begin() + 1, piece.end());
    }
    return out;
}

double polygonArea(const std::vector<Vec2>& p)
{
    double s = 0;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        s += p[i].x * p[i + 1].y - p[i + 1].x * p[i].y;
    }
    if (!p.empty()) {
        s += p.back().x * p.front().y - p.front().x * p.back().y;
    }
    return std::abs(s) / 2;
}

} // namespace

Result<std::vector<Region>> regionsOf(const doc::DocState& state, const FeatureId& sketch)
{
    try {
        auto model = sketch_edit::modelOf(state, sketch);
        if (!model) {
            return tl::unexpected(model.error());
        }
        std::optional<std::string> error;
        std::vector<Region> out;
        for (const doc::features::SketchRegion& r : doc::features::sketchRegions(*model, &error)) {
            Region region;
            region.entities = r.outerEntities;
            region.outer = loopPolyline(r.outer);
            for (const kernel::ProfileLoop& hole : r.holes) {
                region.holes.push_back(loopPolyline(hole));
            }
            region.area = polygonArea(region.outer);
            out.push_back(std::move(region));
        }
        return out;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while finding the sketch regions", {}});
    }
}

Result<double> evaluate(const doc::DocState& state, std::string_view expression, bool angle)
{
    auto params = doc::evaluateParameters(state.parameters);
    if (!params) {
        return tl::unexpected(params.error());
    }
    return doc::evaluate(expression, *params, angle ? doc::Parameter::Kind::Angle : doc::Parameter::Kind::Length);
}

std::shared_ptr<doc::IRebuilder> makeSideRebuilder(IApp& app, const OpenDocument& document)
{
    auto* impl = dynamic_cast<detail::AppImpl*>(&app);
    if (impl == nullptr) {
        return {};
    }
    auto entry = impl->resolve(document.id);
    if (!entry) {
        return {};
    }
    // The registry lives as long as the app; the rebuilder must not outlive it either (the caller's contract).
    std::shared_ptr<const doc::FeatureRegistry> types(&app.featureTypes(), [](const doc::FeatureRegistry*) {});
    try {
        return doc::makeRebuilder(app.jobs(), std::move(types), (*entry)->objects);
    } catch (...) {
        return {};
    }
}

doc::DocStatePtr stateBefore(const doc::DocState& state, const FeatureId& feature)
{
    try {
        const auto it = std::find_if(state.features.begin(), state.features.end(),
                                     [&](const doc::Feature& f) { return f.id == feature; });
        if (it == state.features.end()) {
            return nullptr;
        }
        auto before = std::make_shared<doc::DocState>(state);
        if (it == state.features.begin()) {
            before->features.clear(); // the rollback bar cannot sit before the first feature
            before->rollback.reset();
        } else {
            before->rollback = std::prev(it)->id; // never the last feature: `feature` comes after it
        }
        return before;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

Result<doc::BuildResultPtr> buildSnapshot(IApp& app, const std::string& documentId, doc::DocStatePtr snapshot,
                                          JobContext& ctx)
{
    auto* impl = dynamic_cast<detail::AppImpl*>(&app);
    if (impl == nullptr || !snapshot) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Nothing to build", {}});
    }
    // Shared: the rebuilder stays alive for this build even if the document closes meanwhile.
    const std::shared_ptr<doc::IRebuilder> rebuilder = impl->rebuilderOf(documentId);
    if (!rebuilder) {
        return tl::unexpected(Error{ErrorCode::NotFound, "No open document has the id " + documentId, {}});
    }
    return rebuilder->rebuild(std::move(snapshot), ctx);
}

std::optional<Located> locate(const doc::BuildResult& build, std::string_view refJson)
{
    try {
        auto ref = naming::topoRefFromJson(refJson);
        if (!ref) {
            return std::nullopt;
        }
        const auto body = build.bodies.find(ref->body);
        if (body == build.bodies.end() || body->second.shape.isNull()) {
            return std::nullopt;
        }
        auto resolver = naming::makeResolver({naming::ResolverBody{ref->body, body->second.map, body->second.shape}});
        if (!resolver) {
            return std::nullopt;
        }
        naming::Resolved resolved = resolver->resolve(*ref);
        if (!resolved.usable()) {
            return std::nullopt;
        }
        return Located{ref->body, std::move(resolved.elements)};
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

} // namespace ray::api::feature_edit
