// The chamfer operation's plan (chamfer2d.h, task T-055): parameters, checks, the edge chains and their material side,
// the tool's centre paths, the collision proof and the passes linking joins. Every rule is stated in chamfer2d.h; this
// file follows them in its order. Deterministic: integer nanometres from the chains on, geom2d for offsets, sweeps and
// booleans, dmath for the one tangent.
#include "chamfer2d.h"
#include "op_params.h"
#include "slot_common.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <new>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_CHAMFER_TRY(expr)                                                                                          \
    do {                                                                                                               \
        if (auto ray_chamfer_status_ = (expr); !ray_chamfer_status_) {                                                 \
            return tl::unexpected(std::move(ray_chamfer_status_.error()));                                             \
        }                                                                                                              \
    } while (false)

namespace ray::cam::ops {

namespace g = geom2d;
using g::Coord;
using g::Pt;
using mill::failure;
using mill::invalid;
using mill::mmText;

Result<ChamferGeometry> chamferGeometry(const lib::Tool& tool, double widthMm, double tipOffsetMm)
{
    const mill::ToolShape s = mill::toolShape(tool);
    if (!(s.tanHalf > 0)) {
        return invalid("The " + std::string(mill::toolWord(tool.type)) +
                       " has no point angle: a chamfer is cut by the flank of a pointed tool (set its tip angle in "
                       "the tool library)");
    }
    ChamferGeometry out;
    out.depth = toNm(widthMm / s.tanHalf);
    const Nm o = toNm(tipOffsetMm);
    out.axis = g::roundNm(static_cast<double>(s.tipRadius) + static_cast<double>(o) * s.tanHalf);
    out.tipBelow = out.depth + o;
    return out;
}

namespace {

Pt spanStart(const g::Path& p, std::size_t k)
{
    return k == 0 ? p.start : p.spans[k - 1].end;
}

double spanLength(const g::Path& p, std::size_t k)
{
    g::Path one;
    one.start = spanStart(p, k);
    one.spans = {p.spans[k]};
    return g::length(one);
}

g::Side other(g::Side s)
{
    return s == g::Side::Left ? g::Side::Right : g::Side::Left;
}

// ---------- Parameters ----------

struct Params {
    double width = 0.5, tipOffset = 0.3, extension = 0;
    bool climb = true;
    bool tangentEdges = true;
};

const OpSchema* chamferSchema()
{
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("chamfer2d"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = chamferSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The chamfer operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.width = p->number("width");
    out.tipOffset = p->number("tipOffset");
    out.extension = p->number("extension");
    out.climb = p->text("direction") != "conventional";
    out.tangentEdges = p->flag("tangentEdges");
    return out;
}

// ---------- The plan ----------

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<ChamferPlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        RAY_CHAMFER_TRY(checkTool());
        RAY_CHAMFER_TRY(mill::checkSpindle(in_));
        RAY_CHAMFER_TRY(checkFeeds());
        RAY_CHAMFER_TRY(mill::checkHeights(in_.op.heights, false));
        RAY_CHAMFER_TRY(findChains());
        RAY_CHAMFER_TRY(centrePaths());
        RAY_CHAMFER_TRY(prove());
        RAY_CHAMFER_TRY(passes());
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (!(t.diameter > 0) || !std::isfinite(t.diameter) || t.diameter > 1000) {
            return invalid("The tool has no diameter");
        }
        if (t.type != lib::ToolType::ChamferMill && t.type != lib::ToolType::SpotDrill) {
            return invalid("The tool is a " + std::string(mill::toolWord(t.type)) +
                           "; chamfering needs a chamfer mill or a spot drill (its flank makes the chamfer)");
        }
        auto geo = chamferGeometry(t, p_.width, p_.tipOffset);
        if (!geo) {
            return tl::unexpected(geo.error());
        }
        plan_.geometry = *geo;
        plan_.width = toNm(p_.width);
        shape_ = mill::toolShape(t);
        const Coord reach = plan_.geometry.axis + plan_.width;
        if (reach > shape_.radius) {
            return invalid("A " + mmText(p_.width) + " mm chamfer with a " + mmText(p_.tipOffset) +
                           " mm tip offset needs the cone to reach " + mmText(reach) +
                           " mm from the tool's axis; the " + mmText(t.diameter) + " mm " + mill::toolWord(t.type) +
                           " reaches " + mmText(shape_.radius) +
                           " mm: use a larger tool, a smaller tip offset or a narrower chamfer");
        }
        if (shape_.fluteLength > 0 && plan_.geometry.tipBelow > shape_.fluteLength) {
            return invalid("The chamfer and its tip offset go " + mmText(plan_.geometry.tipBelow) +
                           " mm below the edge, but the tool's cutting part is " + mmText(t.fluteLength) + " mm long");
        }
        return {};
    }

    Status checkFeeds()
    {
        const CuttingData& c = in_.op.cutting;
        auto cut = mill::feedUm(c.feedCut, "cutting feed", false);
        if (!cut) {
            return tl::unexpected(cut.error());
        }
        auto plunge = mill::feedUm(c.feedPlunge, "plunge feed", true);
        if (!plunge) {
            return tl::unexpected(plunge.error());
        }
        feedCut_ = *cut;
        plan_.options.feeds.plunge = *plunge;
        return {};
    }

    Result<const g::Regions*> sliceOf(const kernel::Shape& body, std::size_t key, Nm z)
    {
        const auto k = std::make_pair(key, z);
        if (const auto it = slices_.find(k); it != slices_.end()) {
            return &it->second;
        }
        auto s = g::sliceRegions(body, in_.geom.wcsFromModel, z, ctx_);
        if (!s) {
            return tl::unexpected(s.error());
        }
        return &slices_.emplace(k, std::move(*s)).first->second;
    }

    Status findChains()
    {
        const ResolvedGeometry& geo = in_.geom;
        if (geo.selected.empty()) {
            return invalid("Pick the edges to chamfer: the top edge of each wall");
        }
        if (geo.selectedShapeIndex.size() != geo.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::map<std::size_t, std::vector<kernel::SubId>> byPart;
        for (std::size_t i = 0; i < geo.selected.size(); ++i) {
            const int partIndex = geo.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= geo.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            if (geo.selected[i].kind != kernel::SubKind::Edge) {
                return invalid("Selection " + std::to_string(i + 1) + " is not an edge: pick the edges to chamfer");
            }
            byPart[static_cast<std::size_t>(partIndex)].push_back(geo.selected[i]);
        }
        g::ChainOptions options;
        options.tangentExtend = p_.tangentEdges;
        for (const auto& [part, edges] : byPart) {
            const kernel::Shape& shape = geo.partShapes[part];
            auto chains = g::chainEdges(shape, edges, geo.wcsFromModel, options, ctx_);
            if (!chains) {
                return tl::unexpected(chains.error());
            }
            if (chains->empty()) {
                return invalid("The picked edges run along the setup's Z axis: pick the top edges of the walls");
            }
            for (g::Chain& c : *chains) {
                PlannedChain pc;
                pc.part = part;
                pc.chain = std::move(c);
                RAY_CHAMFER_TRY(sideOf(pc));
                plan_.chains.push_back(std::move(pc));
            }
        }
        return {};
    }

    Status sideOf(PlannedChain& pc)
    {
        const g::Path& p = pc.chain.path;
        const Pt where = p.start;
        if (pc.chain.zMax - pc.chain.zMin > kLevelTol) {
            return invalid("The edge at " + mill::atXY(where) + " is not level (it runs from Z" +
                           mmText(pc.chain.zMin) + " to Z" + mmText(pc.chain.zMax) +
                           "): a chamfer is cut along level edges");
        }
        pc.edgeZ = pc.chain.zMax;
        auto slice = sliceOf(in_.geom.partShapes[pc.part], pc.part, pc.edgeZ - kSideDepth);
        if (!slice) {
            return tl::unexpected(slice.error());
        }
        int sideVotes = 0; // > 0: material on the left
        double s = 0;
        for (std::size_t k = 0; k < p.spans.size(); ++k) {
            RAY_CHAMFER_TRY(ctx_.cancel.check());
            const double len = spanLength(p, k);
            const double mid = s + len / 2;
            s += len;
            const Pt at = g::pointAt(p, mid);
            const Vec2 t = g::tangentAt(p, mid);
            const Coord dx = g::roundNm(-t.y * static_cast<double>(kSideProbe));
            const Coord dy = g::roundNm(t.x * static_cast<double>(kSideProbe));
            const bool left = g::locate(Pt{at.x + dx, at.y + dy}, **slice) == g::Where::Inside;
            const bool right = g::locate(Pt{at.x - dx, at.y - dy}, **slice) == g::Where::Inside;
            if (left == right) {
                return failure(ErrorCode::ToolpathFailed,
                               "The edge at " + mill::atXYZ(at, pc.edgeZ) + " has " +
                                   (left ? "material on both sides" : "no material on either side") +
                                   " just below it: it is not the top edge of a wall, so it cannot be chamfered");
            }
            const int vote = left ? 1 : -1;
            if (sideVotes != 0 && (sideVotes > 0) != (vote > 0)) {
                return failure(ErrorCode::ToolpathFailed, "The picked edges at " + mill::atXYZ(at, pc.edgeZ) +
                                                              " change sides of the material along the chain: pick "
                                                              "the edges of one wall outline at a time");
            }
            sideVotes += vote;
        }
        pc.air = sideVotes > 0 ? g::Side::Right : g::Side::Left;
        return {};
    }

    Status centrePaths()
    {
        const Coord x = plan_.geometry.axis;
        const g::Side want =
            g::toolSideFor(p_.climb ? g::CutMode::Climb : g::CutMode::Conventional, in_.op.cutting.clockwise);
        const Coord ext = toNm(p_.extension);
        for (PlannedChain& pc : plan_.chains) {
            const g::Path& c = pc.chain.path;
            const g::Side material = other(pc.air);
            std::vector<std::pair<g::Path, g::Side>> paths; // path, the side of its travel the material is on
            if (c.closed) {
                if (x <= 0) {
                    paths.emplace_back(c, material);
                } else {
                    g::Region region;
                    region.outer = c;
                    region = g::canonicalize(std::move(region));
                    const bool islandInside = material == g::insideSide(c);
                    auto grown = g::offset(g::Regions{region}, islandInside ? x : -x, ctx_);
                    if (!grown) {
                        return tl::unexpected(grown.error());
                    }
                    if (grown->empty()) {
                        return failure(ErrorCode::ToolpathFailed,
                                       "The " + mmText(tool().diameter) + " mm " + mill::toolWord(tool().type) +
                                           " does not fit inside the edge loop at " + mill::atXY(c.start) +
                                           " with its axis " + mmText(x) + " mm from the edge");
                    }
                    for (const g::Region& r : *grown) {
                        // Every path of a region has the region on its left: the part's material is inside the grown
                        // island (left) or outside the shrunk opening (right).
                        const g::Side m = islandInside ? g::Side::Left : g::Side::Right;
                        paths.emplace_back(r.outer, m);
                        for (const g::Path& h : r.holes) {
                            paths.emplace_back(h, m);
                        }
                    }
                }
            } else {
                std::vector<g::Path> pieces;
                if (x <= 0) {
                    pieces.push_back(c);
                } else {
                    auto off = g::offsetOpen(c, x, pc.air, ctx_);
                    if (!off) {
                        return tl::unexpected(off.error());
                    }
                    pieces = std::move(*off);
                }
                if (pieces.empty()) {
                    return failure(ErrorCode::ToolpathFailed,
                                   "No tool path runs beside the edge at " + mill::atXY(c.start));
                }
                if (ext > 0) {
                    g::Path& first = pieces.front();
                    const Vec2 t0 = g::tangentAt(first, 0);
                    const Pt s{first.start.x - g::roundNm(t0.x * static_cast<double>(ext)),
                               first.start.y - g::roundNm(t0.y * static_cast<double>(ext))};
                    first.spans.insert(first.spans.begin(), g::Span{g::SpanKind::Line, first.start, {}, 0});
                    first.start = s;
                    g::Path& last = pieces.back();
                    const Vec2 t1 = g::tangentAt(last, g::length(last));
                    const Pt e = last.spans.back().end;
                    last.spans.push_back(g::Span{g::SpanKind::Line,
                                                 Pt{e.x + g::roundNm(t1.x * static_cast<double>(ext)),
                                                    e.y + g::roundNm(t1.y * static_cast<double>(ext))},
                                                 {},
                                                 0});
                }
                for (g::Path& piece : pieces) {
                    paths.emplace_back(g::canonicalize(std::move(piece)), material);
                }
            }
            for (auto& [path, m] : paths) {
                pc.centres.push_back(g::orientForTool(std::move(path), other(m), want));
            }
        }
        return {};
    }

    // CHAMFER, the collision proof: every height, every body.
    Status prove()
    {
        const ResolvedGeometry& geo = in_.geom;
        const Nm top = mill::highestNm(geo);
        const Nm c = plan_.geometry.depth;
        const double t = shape_.tanHalf;
        // Chains by edge height, for the chamfer sections.
        std::map<Nm, std::vector<g::Path>> edgesAt;
        for (const PlannedChain& pc : plan_.chains) {
            edgesAt[pc.edgeZ].push_back(pc.chain.path);
        }
        std::vector<const kernel::Shape*> bodies;
        for (const std::vector<kernel::Shape>* list : {&geo.partShapes, &geo.fixtureShapes}) {
            for (const kernel::Shape& b : *list) {
                if (!b.isNull()) {
                    bodies.push_back(&b);
                }
            }
        }
        for (const PlannedChain& pc : plan_.chains) {
            const Nm e = pc.edgeZ;
            const Nm tip = e - plan_.geometry.tipBelow;
            std::vector<Nm> heights = {tip, e - c, e - kSideDepth, e};
            for (Nm z = tip + kChamferProbeStep; z < e; z += kChamferProbeStep) {
                heights.push_back(z);
            }
            for (Nm z = e + kChamferProbeAbove; z < top; z += kChamferProbeAbove) {
                heights.push_back(z);
            }
            auto faces = mill::faceHeights(geo.partShapes[pc.part], geo.wcsFromModel, tip, top, ctx_);
            if (!faces) {
                return tl::unexpected(faces.error());
            }
            for (const Nm f : *faces) {
                heights.push_back(f);
                if (f - 1000 >= tip) {
                    heights.push_back(f - 1000);
                }
            }
            std::sort(heights.begin(), heights.end());
            heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
            std::erase_if(heights, [&](Nm z) { return z < tip || z >= top; });
            for (const Nm z : heights) {
                RAY_CHAMFER_TRY(ctx_.cancel.check());
                const Coord r = shape_.radiusAt(z - tip);
                if (r < kMinToolPathRadius) {
                    continue;
                }
                auto swept = g::sweptArea(pc.centres, r, ctx_);
                if (!swept) {
                    return tl::unexpected(swept.error());
                }
                // What the chamfers remove at this height.
                g::Regions allowed;
                for (const auto& [edge, paths] : edgesAt) {
                    if (z >= edge) {
                        continue;
                    }
                    const double into = static_cast<double>(z - (edge - c));
                    const Coord band = (into > 0 ? g::roundNm(into * t) : 0) + kBandTol;
                    auto b = g::sweptArea(paths, band, ctx_);
                    if (!b) {
                        return tl::unexpected(b.error());
                    }
                    if (allowed.empty()) {
                        allowed = std::move(*b);
                    } else {
                        auto u = g::unite(allowed, *b, ctx_);
                        if (!u) {
                            return tl::unexpected(u.error());
                        }
                        allowed = std::move(*u);
                    }
                }
                for (std::size_t k = 0; k < bodies.size(); ++k) {
                    auto material = sliceOf(*bodies[k], k + 1000000, z);
                    if (!material) {
                        return tl::unexpected(material.error());
                    }
                    auto hit = mill::contactWith(
                        **material, *swept, allowed.empty() ? std::nullopt : std::optional<g::Regions>(allowed), ctx_);
                    if (!hit) {
                        return tl::unexpected(hit.error());
                    }
                    if (!hit->empty()) {
                        const bool below = z < e - c;
                        return failure(ErrorCode::ToolpathFailed,
                                       "Chamfering the edge at " + mill::atXYZ(pc.chain.path.start, e) + ", the " +
                                           mmText(tool().diameter) + " mm " + mill::toolWord(tool().type) +
                                           (below ? "'s tip" : "") + " would cut the part or a fixture at " +
                                           mill::atXYZ(mill::contactPoint(*hit), z) +
                                           (below ? ", below the chamfer (a wall or floor too close): reduce the tip "
                                                    "offset or the chamfer width"
                                                  : ", outside the chamfer (a wall or floor in the tool's way): use a "
                                                    "smaller tool or chamfer this edge in another way"));
                    }
                }
            }
            // The holder stays clear of the stock and the part.
            const double stickOut = in_.op.tool.stickOut;
            const Nm need = top - tip + toNm(mill::kHolderClearanceMm);
            if (stickOut > 0 && toNm(stickOut) < need) {
                return invalid("The tool sticks out " + mmText(stickOut) + " mm, but the chamfer at " +
                               mill::atXY(pc.chain.path.start) + " needs " + mmText(need) +
                               " mm for the holder to stay " + mmText(mill::kHolderClearanceMm) +
                               " mm above the stock and the part");
            }
        }
        return {};
    }

    Status passes()
    {
        const ResolvedGeometry& geo = in_.geom;
        const Heights& h = in_.op.heights;
        const Nm top = mill::stockTopNm(geo);
        link::Options& o = plan_.options;
        o.clearance = top + toNm(h.clearance);
        o.retract = top + toNm(h.retract);
        o.feedPlane = top + toNm(h.feedPlane);
        o.toolRadius = shape_.radius;
        o.centreCutting = true;
        o.maxStayDown = 0;
        o.entryOrder = {EntryKind::Plunge};
        o.spindleClockwise = in_.op.cutting.clockwise;
        o.from = Pt{0, 0};
        plan_.stock = link::stockOf(geo);
        for (const PlannedChain& pc : plan_.chains) {
            const Nm tip = pc.edgeZ - plan_.geometry.tipBelow;
            for (const g::Path& path : pc.centres) {
                link::Pass pass = link::passFromPath(path, tip, feedCut_);
                pass.rotatable = path.closed;
                plan_.passes.push_back(std::move(pass));
            }
        }
        return {};
    }

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    ChamferPlan plan_;
    mill::ToolShape shape_;
    std::int32_t feedCut_ = 0;
    std::map<std::pair<std::size_t, Nm>, g::Regions> slices_;
};

} // namespace

Result<ChamferPlan> planChamfer(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the chamfer operation");
    }
}

#undef RAY_CHAMFER_TRY

} // namespace ray::cam::ops
