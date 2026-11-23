// Helpers shared by the slot and chamfer2d operation types (slot_common.h, task T-055).
#include "slot_common.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ray::cam::ops::mill {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

tl::unexpected<Error> invalid(std::string message)
{
    return failure(ErrorCode::InvalidArgument, std::move(message));
}

std::string mmText(Nm nm)
{
    std::int64_t um = nm / 1000;
    std::int64_t rest = nm % 1000;
    if (rest < 0) {
        rest += 1000;
        --um;
    }
    if (rest > 500 || (rest == 500 && (um % 2) != 0)) {
        ++um;
    }
    const bool negative = um < 0;
    const std::uint64_t a = negative ? static_cast<std::uint64_t>(-(um + 1)) + 1 : static_cast<std::uint64_t>(um);
    std::string out = (negative ? "-" : "") + std::to_string(a / 1000);
    std::uint64_t frac = a % 1000;
    if (frac != 0) {
        std::string digits = std::to_string(frac);
        digits.insert(0, 3 - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        out += "." + digits;
    }
    return out;
}

std::string mmText(double mm)
{
    return std::isfinite(mm) && std::abs(mm) < 1e12 ? mmText(toNm(mm)) : std::string("?");
}

std::string atXY(geom2d::Pt p)
{
    return "X" + mmText(p.x) + " Y" + mmText(p.y);
}

std::string atXYZ(geom2d::Pt p, Nm z)
{
    return atXY(p) + " Z" + mmText(z);
}

const char* toolWord(lib::ToolType t)
{
    switch (t) {
    case lib::ToolType::FlatEndMill:
        return "flat end mill";
    case lib::ToolType::BallEndMill:
        return "ball end mill";
    case lib::ToolType::BullEndMill:
        return "bull nose end mill";
    case lib::ToolType::ChamferMill:
        return "chamfer mill";
    case lib::ToolType::FaceMill:
        return "face mill";
    case lib::ToolType::SpotDrill:
        return "spot drill";
    case lib::ToolType::Drill:
        return "drill";
    case lib::ToolType::Tap:
        return "tap";
    case lib::ToolType::Reamer:
        return "reamer";
    case lib::ToolType::BoringBar:
        return "boring bar";
    case lib::ToolType::ThreadMill:
        return "thread mill";
    case lib::ToolType::EngraveCutter:
        return "engraving cutter";
    }
    return "tool";
}

Status checkSpindle(const GenerateInput& in)
{
    const CuttingData& c = in.op.cutting;
    const double rpm = c.spindleRpm;
    if (!(rpm > 0) || !std::isfinite(rpm)) {
        return invalid("Set a spindle speed");
    }
    if (!c.clockwise) {
        return invalid("The spindle is set to turn counter-clockwise (M4): a right-hand " +
                       std::string(toolWord(in.tool.type)) + " does not cut that way; set it clockwise (M3)");
    }
    if (in.machine.maxRpm > 0 && rpm > in.machine.maxRpm) {
        return invalid("The spindle speed " + mmText(rpm) + " rpm is above the machine's maximum of " +
                       mmText(in.machine.maxRpm) + " rpm");
    }
    if (in.machine.minRpm > 0 && rpm < in.machine.minRpm) {
        return invalid("The spindle speed " + mmText(rpm) + " rpm is below the machine's minimum of " +
                       mmText(in.machine.minRpm) + " rpm");
    }
    return {};
}

Result<std::int32_t> feedUm(double mmPerMin, std::string_view what, bool zeroIsCut)
{
    if (zeroIsCut && !(mmPerMin > 0) && std::isfinite(mmPerMin)) {
        return 0;
    }
    constexpr double kMaxFeed = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    const double um = mmPerMin * 1000.0;
    if (!(mmPerMin > 0) || !std::isfinite(um) || um > kMaxFeed || std::nearbyint(um) < 1) {
        return invalid("Set a " + std::string(what));
    }
    return static_cast<std::int32_t>(std::nearbyint(um)); // the default IEEE rounding: half to even
}

Status checkHeights(const Heights& h, bool withTop)
{
    std::vector<std::pair<double, const char*>> used = {
        {h.clearance, "clearance"}, {h.retract, "retract"}, {h.feedPlane, "feed plane"}};
    if (withTop) {
        used.emplace_back(h.top, "top");
    }
    for (const auto& [v, word] : used) {
        if (!std::isfinite(v) || std::abs(v) > 1e6) {
            return invalid(std::string("The ") + word + " height is not a usable number");
        }
    }
    if (h.clearance < h.retract) {
        return invalid("The clearance height (" + mmText(h.clearance) + " above the stock top) must not be below the " +
                       "retract height (" + mmText(h.retract) + ")");
    }
    if (h.retract < h.feedPlane) {
        return invalid("The retract height (" + mmText(h.retract) + " above the stock top) must not be below the " +
                       "feed plane (" + mmText(h.feedPlane) + ")");
    }
    if (h.retract < 0) {
        return invalid("The retract height (" + mmText(h.retract) +
                       " above the stock top) must not be below the stock top: the tool travels across at it");
    }
    if (withTop && h.feedPlane < h.top) {
        return invalid("The feed plane (" + mmText(h.feedPlane) +
                       " above the stock top) must not be below the top "
                       "height (" +
                       mmText(h.top) + ")");
    }
    return {};
}

Nm stockTopNm(const ResolvedGeometry& g)
{
    return toNm(g.stockBounds.max.z);
}

namespace {

Vec3 apply(const Xform& f, Vec3 p)
{
    const double* m = f.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

double topInWcs(const Xform& f, const Box3& b)
{
    double top = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 8; ++i) {
        const Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
        top = std::max(top, apply(f, c).z);
    }
    return top;
}

} // namespace

Nm highestNm(const ResolvedGeometry& g)
{
    double top = g.stockBounds.max.z;
    for (const std::vector<kernel::Shape>* shapes : {&g.partShapes, &g.fixtureShapes}) {
        for (const kernel::Shape& s : *shapes) {
            if (!s.isNull()) {
                top = std::max(top, topInWcs(g.wcsFromModel, kernel::bounds(s)));
            }
        }
    }
    return std::isfinite(top) ? toNm(top) : stockTopNm(g);
}

Result<std::vector<Nm>> faceHeights(const kernel::Shape& shape, const Xform& frame, Nm lo, Nm hi, JobContext& ctx)
{
    std::vector<Nm> out;
    const std::uint32_t n = shape.count(kernel::SubKind::Face);
    const double* m = frame.m;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected(st.error());
        }
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(shape, f);
        if (!info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        const auto sample = kernel::faceSample(shape, f);
        if (!sample) {
            continue;
        }
        const Vec3 nv = sample->normal;
        const double nz = m[8] * nv.x + m[9] * nv.y + m[10] * nv.z;
        if (std::abs(nz) < 1 - 1e-9) {
            continue;
        }
        const Nm z = toNm(apply(frame, sample->point).z);
        if (z >= lo && z <= hi) {
            out.push_back(z);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Coord ToolShape::radiusAt(Nm h) const
{
    if (fluteLength > 0 && h > fluteLength) {
        return shankRadius;
    }
    if (!(tanHalf > 0)) {
        return radius;
    }
    const double r = static_cast<double>(tipRadius) + static_cast<double>(std::max<Nm>(h, 0)) * tanHalf;
    return std::min(radius, geom2d::roundNm(r));
}

ToolShape toolShape(const lib::Tool& t)
{
    ToolShape s;
    s.radius = toNm(t.diameter / 2);
    const bool pointed = t.type == lib::ToolType::ChamferMill || t.type == lib::ToolType::SpotDrill ||
                         t.type == lib::ToolType::Drill || t.type == lib::ToolType::EngraveCutter;
    if (pointed && t.tipAngleDeg > 0 && t.tipAngleDeg < 180) {
        s.tanHalf = dmath::tan((t.tipAngleDeg * kRadPerDeg) / 2);
        s.tipRadius = std::clamp<Coord>(toNm(t.tipDiameter / 2), 0, s.radius);
    }
    s.fluteLength = t.fluteLength > 0 ? toNm(t.fluteLength) : 0;
    s.shankRadius = t.shankDiameter > 0 ? toNm(t.shankDiameter / 2) : s.radius;
    return s;
}

Result<geom2d::Regions> contactAt(const kernel::Shape& shape, const Xform& frame, Nm z, const geom2d::Regions& swept,
                                  const std::optional<geom2d::Regions>& allowed, JobContext& ctx)
{
    if (swept.empty()) {
        return geom2d::Regions{};
    }
    auto material = geom2d::sliceRegions(shape, frame, z, ctx);
    if (!material) {
        return tl::unexpected(material.error());
    }
    return contactWith(*material, swept, allowed, ctx);
}

Result<geom2d::Regions> contactWith(const geom2d::Regions& material, const geom2d::Regions& swept,
                                    const std::optional<geom2d::Regions>& allowed, JobContext& ctx)
{
    if (material.empty() || swept.empty()) {
        return geom2d::Regions{};
    }
    auto touched = geom2d::intersect(material, swept, ctx);
    if (!touched) {
        return tl::unexpected(touched.error());
    }
    geom2d::Regions out = std::move(*touched);
    if (allowed && !out.empty()) {
        auto rest = geom2d::subtract(out, *allowed, ctx);
        if (!rest) {
            return tl::unexpected(rest.error());
        }
        out = std::move(*rest);
    }
    std::erase_if(out, [](const geom2d::Region& reg) { return geom2d::area(reg) < kContactMinArea; });
    return out;
}

geom2d::Pt contactPoint(const geom2d::Regions& contact)
{
    if (contact.empty()) {
        return {};
    }
    if (const auto p = geom2d::interiorPoint(contact.front())) {
        return *p;
    }
    return contact.front().outer.start;
}

Status checkTravel(const std::vector<Move>& moves, Nm startZ, const lib::MachineDef& machine)
{
    if (moves.empty()) {
        return {};
    }
    Nm xMin = std::numeric_limits<Nm>::max(), xMax = std::numeric_limits<Nm>::min();
    Nm yMin = xMin, yMax = xMax, zMin = startZ, zMax = startZ;
    for (const Move& m : moves) {
        xMin = std::min(xMin, m.x);
        xMax = std::max(xMax, m.x);
        yMin = std::min(yMin, m.y);
        yMax = std::max(yMax, m.y);
        zMin = std::min(zMin, m.z);
        zMax = std::max(zMax, m.z);
    }
    for (const lib::AxisDef& a : machine.axes) {
        if (!(a.maxTravel > a.minTravel)) {
            continue; // not known
        }
        const Nm travel = toNm(a.maxTravel - a.minTravel);
        Nm span = 0;
        if (a.letter == 'X') {
            span = xMax - xMin;
        } else if (a.letter == 'Y') {
            span = yMax - yMin;
        } else if (a.letter == 'Z') {
            span = zMax - zMin;
        } else {
            continue;
        }
        if (span > travel) {
            return failure(ErrorCode::ToolpathFailed, "The toolpath spans " + mmText(span) + " mm in " +
                                                          std::string(1, a.letter) + "; the machine's travel is " +
                                                          mmText(travel) + " mm");
        }
    }
    return {};
}

void fillStats(Toolpath& path, const link::Linked& linked)
{
    path.startX = linked.start.x;
    path.startY = linked.start.y;
    path.startZ = linked.start.z;
    path.moves = linked.moves;
    const link::Lengths l = link::lengthsOf(linked.moves, linked.start);
    path.stats.cutLengthMm = l.feed / kNmPerMm;
    path.stats.rapidLengthMm = l.rapid / kNmPerMm;
    Nm lo = linked.start.z, hi = linked.start.z;
    for (const Move& m : linked.moves) {
        lo = std::min(lo, m.z);
        hi = std::max(hi, m.z);
    }
    path.stats.minZ = toMm(lo);
    path.stats.maxZ = toMm(hi);
}

} // namespace ray::cam::ops::mill
