// The CAM workspace's read-only helpers (ray_api/cam_setup.h, task T-044).
#include "files.h"

#include <ray_api/cam_setup.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <system_error>
#include <variant>

namespace ray::api::cam_setup {

namespace {

Vec3 apply(const Xform& xf, Vec3 p)
{
    const double* m = xf.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

const cam::Setup* findSetup(const cam::CamState& cam, const SetupId& id)
{
    const auto it = std::find_if(cam.setups.begin(), cam.setups.end(), [&](const cam::Setup& s) { return s.id == id; });
    return it == cam.setups.end() ? nullptr : &*it;
}

const naming::ResolverBody* bodyOf(const cam::CamSnapshot& snap, const BodyId& id)
{
    const auto it =
        std::find_if(snap.bodies.begin(), snap.bodies.end(), [&](const naming::ResolverBody& b) { return b.id == id; });
    return it == snap.bodies.end() ? nullptr : &*it;
}

// A closed soup (mm) moved by `xf` as a flat-shaded display mesh with one face range.
std::shared_ptr<const kernel::Mesh> meshOf(const kernel::TriSoup& soup, const Xform& xf)
{
    auto mesh = std::make_shared<kernel::Mesh>();
    const std::size_t triangles = soup.indices.size() / 3;
    mesh->positions.reserve(triangles * 9);
    mesh->normals.reserve(triangles * 9);
    mesh->indices.reserve(triangles * 3);
    const auto at = [&](std::uint32_t v) {
        return apply(xf, Vec3{soup.positions[3 * v], soup.positions[3 * v + 1], soup.positions[3 * v + 2]});
    };
    for (std::size_t t = 0; t < triangles; ++t) {
        const Vec3 a = at(soup.indices[3 * t]), b = at(soup.indices[3 * t + 1]), c = at(soup.indices[3 * t + 2]);
        const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, w{c.x - a.x, c.y - a.y, c.z - a.z};
        Vec3 n{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
        const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 0) {
            n = Vec3{n.x / len, n.y / len, n.z / len};
        }
        for (const Vec3& p : {a, b, c}) {
            mesh->positions.insert(mesh->positions.end(),
                                   {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
            mesh->normals.insert(mesh->normals.end(),
                                 {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
            mesh->indices.push_back(static_cast<std::uint32_t>(mesh->indices.size()));
        }
    }
    mesh->faces.push_back(kernel::FaceRange{kernel::SubId{kernel::SubKind::Face, 0}, 0,
                                            static_cast<std::uint32_t>(mesh->indices.size())});
    return mesh;
}

} // namespace

std::vector<SetupId> carriedSetups(const doc::DocState& state)
{
    std::vector<SetupId> out;
    if (!state.cam) {
        return out;
    }
    for (const cam::Setup& s : state.cam->setups) {
        if (std::holds_alternative<cam::FromSetupStock>(s.stock)) {
            out.push_back(s.id);
        }
    }
    return out;
}

Result<Preview> previewSetup(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const SetupId& setupId,
                             JobContext& ctx, const cam::CarriedStock* carried)
{
    if (!state || !build) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "The model is not built yet", {}});
    }
    const cam::CamSnapshot snap = doc::camSnapshot(state, build);
    const cam::Setup* setup = snap.cam ? findSetup(*snap.cam, setupId) : nullptr;
    if (setup == nullptr) {
        return tl::unexpected(Error{ErrorCode::NotFound, "The document has no setup " + setupId.value.toString(), {}});
    }
    const auto resolver = naming::makeResolver(snap.bodies);
    if (!resolver) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory", {}});
    }
    // A solid stock is shown as its body lies in the model: no copy of the body moved into the WCS and back.
    const auto* solid = std::get_if<cam::SolidStock>(&setup->stock);
    const bool fromSetup = std::holds_alternative<cam::FromSetupStock>(setup->stock);
    cam::SetupResolveOptions options{solid == nullptr && !fromSetup, fromSetup ? carried : nullptr};
    auto resolved = cam::resolveSetup(snap, *setup, *resolver, ctx, options);
    if (!resolved) {
        return tl::unexpected(resolved.error());
    }
    if (auto ok = ctx.cancel.check(); !ok) {
        return tl::unexpected(ok.error());
    }
    Preview out;
    if (fromSetup) {
        // T-074: the carried stock, in this setup's WCS, moved back into the model.
        if (!resolved->carriedStock) {
            return tl::unexpected(Error{ErrorCode::Internal, "The carried stock did not resolve", {}});
        }
        out.stockMesh = meshOf(*resolved->carriedStock, resolved->modelFromWcs);
        resolved->carriedStock.reset();
    } else if (solid != nullptr) {
        const naming::ResolverBody* body = bodyOf(snap, solid->body);
        if (body == nullptr) {
            return tl::unexpected(Error{ErrorCode::ReferenceBroken, "The stock body is not built", {}});
        }
        out.stock = body->shape;
    } else {
        // The stock moved back into the model (kernel::transformed hands the identity back as the shape itself).
        auto moved = kernel::transformed(resolved->stockSolid, resolved->modelFromWcs);
        if (!moved) {
            return tl::unexpected(moved.error());
        }
        out.stock = moved->shape;
    }
    resolved->stockSolid = kernel::Shape{};
    const Box3& b = resolved->stockBounds;
    for (int k = 0; k < 8; ++k) {
        const bool xMax = k == 1 || k == 2 || k == 5 || k == 6;
        const bool yMax = k == 2 || k == 3 || k == 6 || k == 7;
        const Vec3 wcs{xMax ? b.max.x : b.min.x, yMax ? b.max.y : b.min.y, k < 4 ? b.max.z : b.min.z};
        out.markers[static_cast<std::size_t>(k)] = apply(resolved->modelFromWcs, wcs);
    }
    out.markers[8] = apply(resolved->modelFromWcs, Vec3{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.max.z});
    const double* m = resolved->wcsFromModel.m;
    out.x = Vec3{m[0], m[1], m[2]};
    out.y = Vec3{m[4], m[5], m[6]};
    out.z = Vec3{m[8], m[9], m[10]};
    out.origin = apply(resolved->modelFromWcs, Vec3{0, 0, 0});
    out.resolved = std::move(*resolved);
    return out;
}

std::map<SetupId, Status> setupStates(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, JobContext& ctx,
                                      const CarriedStocks* carried)
{
    std::map<SetupId, Status> out;
    if (!state || !build || !state->cam) {
        return out;
    }
    const cam::CamSnapshot snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    if (!resolver || !snap.cam) {
        return out;
    }
    for (const cam::Setup& setup : snap.cam->setups) {
        if (ctx.cancel.cancelled()) {
            break;
        }
        if (std::holds_alternative<cam::FromSetupStock>(setup.stock)) {
            // T-074: from its carried stock when the caller has it; else everything but the stock (see the header).
            const auto it = carried != nullptr ? carried->find(setup.id) : CarriedStocks::const_iterator{};
            if (carried != nullptr && it != carried->end() && it->second) {
                auto resolved =
                    cam::resolveSetup(snap, setup, *resolver, ctx, cam::SetupResolveOptions{false, it->second.get()});
                out[setup.id] = resolved ? Status{} : Status(tl::unexpected(resolved.error()));
            } else {
                cam::Setup frameOnly = setup;
                frameOnly.stock = cam::BoxStock{};
                auto resolved = cam::resolveSetup(snap, frameOnly, *resolver, ctx);
                out[setup.id] = resolved ? Status{} : Status(tl::unexpected(resolved.error()));
            }
            continue;
        }
        auto resolved = cam::resolveSetup(snap, setup, *resolver, ctx);
        out[setup.id] = resolved ? Status{} : Status(tl::unexpected(resolved.error()));
    }
    return out;
}

std::string machineJson(const lib::MachineDef& machine)
{
    return lib::toJson(machine);
}

std::string referenceJson(const naming::TopoRef& ref)
{
    return naming::toJson(ref);
}

std::vector<std::string> postStems(const std::vector<std::string>& rootDirs)
{
    std::set<std::string> stems;
    constexpr std::string_view kSuffix = ".post.js";
    for (const std::string& root : rootDirs) {
        std::error_code ec;
        const std::filesystem::path dir = detail::toFsPath(root) / "posts";
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = detail::fromFsPath(it->path().filename());
            if (name.size() > kSuffix.size() &&
                name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
                stems.insert(name.substr(0, name.size() - kSuffix.size()));
            }
        }
    }
    return {stems.begin(), stems.end()};
}

} // namespace ray::api::cam_setup
