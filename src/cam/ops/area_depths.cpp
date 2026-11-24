// The depths of the area above a floor face and the area at each (area_depths.h), shared by pocket2d and adaptive2d.
// Deterministic: integer nanometres (geom2d), std::map and sorted vectors only.
#include "area_depths.h"

#include <algorithm>
#include <map>
#include <utility>

namespace ray::cam::ops {

namespace {

using geom2d::Region;
using geom2d::Regions;

// Whether a and b overlap in a part wider than 2 kFaceMeetNm.
Result<bool> meets(const Regions& a, const Regions& b, JobContext& ctx)
{
    if (a.empty() || b.empty()) {
        return false;
    }
    auto both = geom2d::intersect(a, b, ctx);
    if (!both) {
        return tl::unexpected(both.error());
    }
    if (both->empty()) {
        return false;
    }
    auto wide = geom2d::offset(*both, -kFaceMeetNm, ctx);
    if (!wide) {
        return tl::unexpected(wide.error());
    }
    return !wide->empty();
}

// The candidate faces (area_depths.h THE DEPTHS) by height + stockFloor, highest first.
Result<std::map<Nm, Regions, std::greater<>>> candidates(const AreaDepthsInput& in, JobContext& ctx)
{
    std::map<Nm, Regions, std::greater<>> out;
    const kernel::Shape& shape = *in.part;
    const Xform& f = in.frame;
    const std::uint32_t faces = shape.count(kernel::SubKind::Face);
    for (std::uint32_t i = 0; i < faces; ++i) {
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected(st.error());
        }
        const kernel::SubId face{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(shape, face);
        if (!info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        const auto sample = kernel::faceSample(shape, face);
        if (!sample) {
            continue;
        }
        const Vec3& n = sample->normal;
        const double up = f.m[8] * n.x + f.m[9] * n.y + f.m[10] * n.z; // the normal's Z in the WCS
        if (!(up > 0.999)) {
            continue;
        }
        auto fb = geom2d::faceBoundary(shape, face, f);
        if (!fb || !fb->facesUp) {
            continue;
        }
        const Nm z = fb->z + in.stockFloor;
        if (z <= in.low || z >= in.top) {
            continue;
        }
        const Regions one{fb->region};
        auto inside = meets(one, in.footprint, ctx);
        if (!inside) {
            return tl::unexpected(inside.error());
        }
        if (*inside) {
            out[z].push_back(std::move(fb->region));
        }
    }
    return out;
}

} // namespace

Result<AreaDepths> areaDepths(const AreaDepthsInput& in, const SliceAt& sliceAt, JobContext& ctx)
{
    auto faces = candidates(in, ctx);
    if (!faces) {
        return tl::unexpected(faces.error());
    }
    // The levels and the candidates at other heights, top down (a level repeated at the bottom stays repeated).
    struct Entry {
        Nm z = 0;
        bool level = true;
    };
    std::vector<Entry> list;
    for (const Nm z : in.levels) {
        list.push_back(Entry{z, true});
    }
    for (const auto& [z, regions] : *faces) {
        if (std::find(in.levels.begin(), in.levels.end(), z) == in.levels.end()) {
            list.push_back(Entry{z, false});
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.z > b.z; });

    AreaDepths out;
    const Regions floorOnly{in.floor};
    Regions material;
    for (const Entry& e : list) {
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected(st.error());
        }
        // Material from stockFloor below z up: the stock left on a floor or an island's top is kept.
        auto s = sliceAt(e.z - in.stockFloor);
        if (!s) {
            return tl::unexpected(s.error());
        }
        if (material.empty()) {
            material = **s;
        } else if (!(*s)->empty()) {
            auto u = geom2d::unite(material, **s, ctx);
            if (!u) {
                return tl::unexpected(u.error());
            }
            material = std::move(*u);
        }
        auto free = geom2d::subtract(in.footprint, material, ctx);
        if (!free) {
            return tl::unexpected(free.error());
        }
        Regions kept;
        for (Region& piece : *free) {
            auto meet = geom2d::intersect(Regions{piece}, floorOnly, ctx);
            if (!meet) {
                return tl::unexpected(meet.error());
            }
            if (geom2d::area(*meet) > 0) {
                kept.push_back(std::move(piece));
            }
        }
        if (!e.level) {
            // Where the tool reaches at this depth: the area shrunk by `keep`, grown back by the tool radius.
            auto room = geom2d::offset(kept, -in.keep, ctx);
            if (!room) {
                return tl::unexpected(room.error());
            }
            Regions reached;
            if (!room->empty()) {
                auto grown = geom2d::offset(*room, in.reach, ctx);
                if (!grown) {
                    return tl::unexpected(grown.error());
                }
                reached = std::move(*grown);
            }
            auto matters = meets(reached, faces->at(e.z), ctx);
            if (!matters) {
                return tl::unexpected(matters.error());
            }
            if (!*matters) {
                out.unreached.push_back(e.z);
                continue;
            }
        }
        out.depths.push_back(e.z);
        out.region.push_back(std::move(kept));
        out.material.push_back(material);
    }
    return out;
}

} // namespace ray::cam::ops
