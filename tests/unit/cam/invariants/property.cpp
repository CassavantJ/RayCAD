// The seeded property harness of the invariant checker (property.h).
#include "property.h"

#include "mini_json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace ray::cam::invariants {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string num(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

const char* toolWord(lib::ToolType t)
{
    using T = lib::ToolType;
    switch (t) {
    case T::FlatEndMill:
        return "flat end mill";
    case T::BallEndMill:
        return "ball end mill";
    case T::BullEndMill:
        return "bull end mill";
    case T::ChamferMill:
        return "chamfer mill";
    case T::FaceMill:
        return "face mill";
    case T::SpotDrill:
        return "spot drill";
    case T::Drill:
        return "drill";
    case T::Tap:
        return "tap";
    case T::Reamer:
        return "reamer";
    case T::BoringBar:
        return "boring bar";
    case T::ThreadMill:
        return "thread mill";
    case T::EngraveCutter:
        return "engraving cutter";
    }
    return "tool";
}

// ----- Part building -----

kernel::ProfileCurve lineC(Vec2 a, Vec2 b)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0, false, 0};
}
kernel::ProfileCurve arcC(Vec2 a, Vec2 b, Vec2 c, double r)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Arc, {a, b, c}, r, true, 0};
}

// A counter-clockwise rounded rectangle (r = 0: sharp corners).
kernel::ProfileLoop roundedRect(double x0, double y0, double x1, double y1, double r)
{
    kernel::ProfileLoop l;
    if (r <= 0) {
        l.curves = {lineC({x0, y0}, {x1, y0}), lineC({x1, y0}, {x1, y1}), lineC({x1, y1}, {x0, y1}),
                    lineC({x0, y1}, {x0, y0})};
        return l;
    }
    l.curves = {lineC({x0 + r, y0}, {x1 - r, y0}), arcC({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}, r),
                lineC({x1, y0 + r}, {x1, y1 - r}), arcC({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}, r),
                lineC({x1 - r, y1}, {x0 + r, y1}), arcC({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}, r),
                lineC({x0, y1 - r}, {x0, y0 + r}), arcC({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r}, r)};
    // A side as long as the two corners (a slot's ends) has no straight part.
    std::erase_if(l.curves, [](const kernel::ProfileCurve& c) {
        return c.kind == kernel::ProfileCurve::Kind::Line &&
               std::hypot(c.pts[1].x - c.pts[0].x, c.pts[1].y - c.pts[0].y) < 1e-9;
    });
    return l;
}

// The same as a polygon (each corner in 8 chords).
std::vector<Vec2> roundedRectPoly(double x0, double y0, double x1, double y1, double r)
{
    std::vector<Vec2> p;
    const auto corner = [&](double cx, double cy, double a0) {
        for (int k = 0; k <= 8; ++k) {
            const double a = a0 + k * (kPi / 2) / 8;
            p.push_back(Vec2{cx + r * std::cos(a), cy + r * std::sin(a)});
        }
    };
    if (r <= 0) {
        return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    corner(x1 - r, y0 + r, -kPi / 2);
    corner(x1 - r, y1 - r, 0);
    corner(x0 + r, y1 - r, kPi / 2);
    corner(x0 + r, y0 + r, kPi);
    return p;
}

Result<kernel::Shape> cutWith(const kernel::Shape& part, const kernel::Shape& tool)
{
    JobContext ctx;
    auto r = kernel::boolean(kernel::BoolOp::Cut, part, tool, ctx);
    if (!r) {
        return tl::unexpected(r.error());
    }
    return r->shape;
}

// The prism of `loops` (outer first, then holes) from floorZ up to Z 1.
Result<kernel::Shape> prism(std::vector<kernel::ProfileLoop> loops, double floorZ)
{
    const kernel::Profile p{kernel::Plane{Vec3{0, 0, floorZ}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, std::move(loops)};
    JobContext ctx;
    auto r = kernel::extrude(p, 1 - floorZ, 0, ctx);
    if (!r) {
        return tl::unexpected(r.error());
    }
    return r->op.shape;
}

// A hole of diameter d at (x, y) from above the top: through a plate of thickness t, or blind with its full diameter
// to bottomZ and a 118 degree point below.
Result<kernel::Shape> holeTool(double x, double y, double d, bool through, double bottomZ, double t)
{
    JobContext ctx;
    if (through) {
        auto c = kernel::makeCylinder(Vec3{x, y, -t - 1}, Vec3{0, 0, 1}, d / 2, t + 2);
        return c;
    }
    const double r = d / 2;
    const double point = r / std::tan(59.0 * kPi / 180.0);
    const std::vector<Vec2> rz = {{0, 1}, {r, 1}, {r, bottomZ}, {0, bottomZ - point}};
    kernel::ProfileLoop loop;
    for (std::size_t i = 0; i < rz.size(); ++i) {
        loop.curves.push_back(lineC(rz[i], rz[(i + 1) % rz.size()]));
    }
    const kernel::Profile p{kernel::Plane{Vec3{x, y, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}}, {loop}};
    auto rv = kernel::revolve(p, Vec3{x, y, 0}, Vec3{0, 0, 1}, 2 * kPi, ctx);
    if (!rv) {
        return tl::unexpected(rv.error());
    }
    return rv->op.shape;
}

const char* kindWord(Feature::Kind k)
{
    switch (k) {
    case Feature::Kind::Pocket:
        return "pocket";
    case Feature::Kind::Slot:
        return "slot";
    case Feature::Kind::Holes:
        return "holes";
    case Feature::Kind::Step:
        return "step";
    case Feature::Kind::Boss:
        return "boss";
    }
    return "?";
}

std::vector<kernel::SubId> facesOf(const kernel::Shape& s)
{
    std::vector<kernel::SubId> out;
    const std::uint32_t n = s.count(kernel::SubKind::Face);
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back(kernel::SubId{kernel::SubKind::Face, i});
    }
    return out;
}

Box3 regionBox(const Feature& f)
{
    Box3 b{Vec3{1e300, 1e300, 0}, Vec3{-1e300, -1e300, 0}};
    for (const auto& loop : f.region) {
        for (const Vec2& p : loop) {
            b.min.x = std::min(b.min.x, p.x);
            b.min.y = std::min(b.min.y, p.y);
            b.max.x = std::max(b.max.x, p.x);
            b.max.y = std::max(b.max.y, p.y);
        }
    }
    return b;
}

// Planar faces with outward normal along +Z at height z, whose box lies in `within` (grown by 1e-3).
std::vector<kernel::SubId> upFacesAt(const kernel::Shape& s, double z, const Box3* within)
{
    std::vector<kernel::SubId> out;
    for (const kernel::SubId f : facesOf(s)) {
        const auto info = kernel::surfaceInfo(s, f);
        if (!info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        const auto sample = kernel::faceSample(s, f);
        if (!sample || sample->normal.z < 0.999) {
            continue;
        }
        if (std::abs(info->bounds.min.z - z) > 1e-6 || std::abs(info->bounds.max.z - z) > 1e-6) {
            continue;
        }
        if (within != nullptr &&
            (info->bounds.min.x < within->min.x - 1e-3 || info->bounds.max.x > within->max.x + 1e-3 ||
             info->bounds.min.y < within->min.y - 1e-3 || info->bounds.max.y > within->max.y + 1e-3)) {
            continue;
        }
        out.push_back(f);
    }
    return out;
}

std::string paramsFromSchema(const IOperationType& type, const PropertyPlugin& p, Rng& rng)
{
    const auto schema = json::parse(type.paramsSchemaJson());
    std::map<std::string, std::string> out;
    const json::Value* props = schema ? schema->find("properties") : nullptr;
    if (props == nullptr) {
        return "{}";
    }
    for (const auto& [name, prop] : props->object) {
        if (const auto f = p.fixed.find(name); f != p.fixed.end()) {
            out[name] = f->second;
            continue;
        }
        if (const json::Value* e = prop.find("enum"); e != nullptr && !e->array.empty()) {
            const json::Value& v = rng.pick(e->array);
            out[name] = v.type == json::Value::Type::String ? "\"" + v.string + "\"" : num(v.number);
            continue;
        }
        const json::Value* t = prop.find("type");
        if (t == nullptr) {
            continue;
        }
        if (t->string == "boolean") {
            out[name] = rng.coin() ? "true" : "false";
            continue;
        }
        if (t->string != "number" && t->string != "integer") {
            continue;
        }
        double lo = 0, hi = 0;
        if (const auto r = p.ranges.find(name); r != p.ranges.end()) {
            lo = r->second.first;
            hi = r->second.second;
        } else {
            const json::Value* mn = prop.find("minimum");
            const json::Value* xmn = prop.find("exclusiveMinimum");
            const json::Value* mx = prop.find("maximum");
            const json::Value* df = prop.find("default");
            lo = mn ? mn->number : (xmn ? xmn->number + 0.001 * (1 + std::abs(xmn->number)) : 0);
            const double d = df ? df->number : lo + 1;
            hi = std::max(2 * d, lo + 1);
            if (mx) {
                hi = std::min(hi, mx->number);
            }
        }
        const double v = hi > lo ? rng.uniform(lo, hi) : lo;
        out[name] = t->string == "integer" ? num(std::round(v)) : num(std::round(v * 1000) / 1000);
    }
    std::string s = "{";
    for (const auto& [k, v] : out) {
        if (s.size() > 1) {
            s += ",";
        }
        s += "\"" + k + "\":" + v;
    }
    return s + "}";
}

} // namespace

// ----- Rng -----

std::uint64_t Rng::next()
{
    std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

double Rng::uniform(double lo, double hi)
{
    const double u = static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
    return lo + (hi - lo) * u;
}

int Rng::range(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
}

bool Rng::coin(double p)
{
    return uniform(0, 1) < p;
}

// ----- Parts -----

std::string Feature::describe() const
{
    std::ostringstream s;
    s << kindWord(kind);
    if (kind == Kind::Holes) {
        for (const Hole& h : holes) {
            s << " [" << num(h.diameter) << " at (" << num(h.x) << ", " << num(h.y) << ") "
              << (h.through ? "through" : "to Z " + num(h.bottomZ)) << "]";
        }
        return s.str();
    }
    const Box3 b = regionBox(*this);
    s << " (" << num(b.min.x) << ", " << num(b.min.y) << ")..(" << num(b.max.x) << ", " << num(b.max.y) << ") floor Z "
      << num(floorZ) << " corner r " << num(cornerRadius);
    if (region.size() > 1) {
        s << (kind == Kind::Boss ? " round a boss" : " with an island");
    }
    return s.str();
}

std::string RandomPart::describe() const
{
    std::ostringstream s;
    s << "plate " << num(length) << " x " << num(width) << " x " << num(thickness);
    for (const Feature& f : features) {
        s << "; " << f.describe();
    }
    return s.str();
}

Result<RandomPart> randomPart(Rng& rng, const std::vector<Feature::Kind>& needs)
{
    RandomPart p;
    p.length = std::round(rng.uniform(60, 110));
    p.width = std::round(rng.uniform(40, 80));
    p.thickness = std::round(rng.uniform(12, 25));
    const double T = p.thickness;
    auto shape = kernel::makeBox(Vec3{0, 0, -T}, Vec3{p.length, p.width, 0});
    if (!shape) {
        return tl::unexpected(shape.error());
    }
    kernel::Shape s = *shape;
    const int cols = p.length > 85 ? 3 : 2, rows = 2;
    const double cw = p.length / cols, ch = p.width / rows, m = 4;
    // The kinds the plug-in needs go first, each to a cell of its own (a step only fits the front row).
    std::vector<int> forced(static_cast<std::size_t>(cols * rows), -1);
    for (const Feature::Kind k : needs) {
        for (int tries = 0; tries < 20; ++tries) {
            const int cell = rng.range(0, cols * rows - 1);
            if (forced[static_cast<std::size_t>(cell)] < 0 && (k != Feature::Kind::Step || cell / cols == 0)) {
                forced[static_cast<std::size_t>(cell)] = static_cast<int>(k);
                break;
            }
        }
    }
    const auto apply = [&](Result<kernel::Shape> tool) -> Status {
        if (!tool) {
            return tl::unexpected(tool.error());
        }
        auto cut = cutWith(s, *tool);
        if (!cut) {
            return tl::unexpected(cut.error());
        }
        s = *cut;
        return {};
    };
    const auto r1 = [](double v) {
        return std::round(v * 10) / 10;
    }; // whole tenths keep the parts readable
    for (int rj = 0; rj < rows; ++rj) {
        for (int ci = 0; ci < cols; ++ci) {
            const double ix0 = ci * cw + m, ix1 = (ci + 1) * cw - m, iy0 = rj * ch + m, iy1 = (rj + 1) * ch - m;
            const double iw = ix1 - ix0, ih = iy1 - iy0;
            std::vector<Feature::Kind> kinds = {Feature::Kind::Pocket, Feature::Kind::Pocket, Feature::Kind::Slot,
                                                Feature::Kind::Holes,  Feature::Kind::Holes,  Feature::Kind::Boss};
            if (rj == 0) {
                kinds.push_back(Feature::Kind::Step);
            }
            const int want = forced[static_cast<std::size_t>(rj * cols + ci)];
            if (want < 0 && rng.coin(0.1)) {
                continue; // an empty cell
            }
            Feature f;
            f.kind = want >= 0 ? static_cast<Feature::Kind>(want) : rng.pick(kinds);
            const double depth = r1(rng.uniform(2, T - 4));
            f.floorZ = -depth;
            Status st;
            switch (f.kind) {
            case Feature::Kind::Pocket: {
                const double w = r1(iw * rng.uniform(0.6, 1)), h = r1(ih * rng.uniform(0.6, 1));
                const double x0 = r1(ix0 + rng.uniform(0, iw - w)), y0 = r1(iy0 + rng.uniform(0, ih - h));
                const double x1 = x0 + w, y1 = y0 + h;
                f.cornerRadius =
                    rng.coin(0.3) ? 0 : r1(rng.uniform(1, std::max(1.0, std::min(5.0, std::min(w, h) / 2 - 0.5))));
                std::vector<kernel::ProfileLoop> loops = {roundedRect(x0, y0, x1, y1, f.cornerRadius)};
                f.region = {roundedRectPoly(x0, y0, x1, y1, f.cornerRadius)};
                if (std::min(w, h) >= 26 && rng.coin(0.4)) {
                    const double c = r1(rng.uniform(7, 10));
                    const double ir = rng.coin() ? 0 : r1(rng.uniform(0.5, 2));
                    loops.push_back(roundedRect(x0 + c, y0 + c, x1 - c, y1 - c, ir));
                    f.region.push_back(roundedRectPoly(x0 + c, y0 + c, x1 - c, y1 - c, ir));
                }
                st = apply(prism(std::move(loops), f.floorZ));
                break;
            }
            case Feature::Kind::Slot: {
                const double sw = r1(rng.uniform(6, std::max(6.5, std::min(12.0, ih - 1))));
                const double len = r1(rng.uniform(std::min(2 * sw, iw), iw));
                const double x0 = r1(ix0 + rng.uniform(0, iw - len)), yc = r1(iy0 + ih / 2);
                f.cornerRadius = sw / 2;
                st = apply(prism({roundedRect(x0, yc - sw / 2, x0 + len, yc + sw / 2, sw / 2)}, f.floorZ));
                f.region = {roundedRectPoly(x0, yc - sw / 2, x0 + len, yc + sw / 2, sw / 2)};
                break;
            }
            case Feature::Kind::Holes: {
                static const std::vector<double> diameters = {3.3, 4.2, 5.0, 6.8, 8.5, 10.2};
                const int n = rng.range(1, 3);
                const double pitch = iw / n;
                for (int k = 0; k < n; ++k) {
                    Feature::Hole h;
                    do {
                        h.diameter = rng.pick(diameters);
                    } while (h.diameter > pitch - 3 && h.diameter > 3.3);
                    h.x = r1(ix0 + pitch * (k + 0.5));
                    h.y = r1(iy0 + ih / 2);
                    h.through = rng.coin();
                    h.bottomZ = h.through ? -T : -r1(rng.uniform(4, T - 3));
                    st = apply(holeTool(h.x, h.y, h.diameter, h.through, h.bottomZ, T));
                    if (!st) {
                        break;
                    }
                    f.holes.push_back(h);
                }
                f.floorZ = 0;
                break;
            }
            case Feature::Kind::Step: {
                const double sw = r1(rng.uniform(5, std::max(5.5, ch - m - 2)));
                auto box = kernel::makeBox(Vec3{ix0, -1, f.floorZ}, Vec3{ix1, sw, 1});
                st = apply(box);
                f.region = {{{ix0, 0}, {ix1, 0}, {ix1, sw}, {ix0, sw}}};
                break;
            }
            case Feature::Kind::Boss: {
                f.floorZ = -r1(rng.uniform(2, T / 2));
                f.cornerRadius = rng.coin() ? 0 : r1(rng.uniform(1, 3));
                const double bw = r1(iw * rng.uniform(0.35, 0.6)), bh = r1(ih * rng.uniform(0.35, 0.6));
                const double bx0 = r1(ix0 + (iw - bw) / 2), by0 = r1(iy0 + (ih - bh) / 2);
                const double br = rng.coin() ? 0 : r1(rng.uniform(1, std::min(bw, bh) / 3));
                st = apply(prism(
                    {roundedRect(ix0, iy0, ix1, iy1, f.cornerRadius), roundedRect(bx0, by0, bx0 + bw, by0 + bh, br)},
                    f.floorZ));
                f.region = {roundedRectPoly(ix0, iy0, ix1, iy1, f.cornerRadius),
                            roundedRectPoly(bx0, by0, bx0 + bw, by0 + bh, br)};
                break;
            }
            }
            if (!st) {
                return tl::unexpected(st.error());
            }
            p.features.push_back(std::move(f));
        }
    }
    p.shape = s;
    return p;
}

// ----- Cases -----

lib::Tool randomTool(Rng& rng, lib::ToolType type, double diameter)
{
    using T = lib::ToolType;
    static const std::vector<double> sizes = {3, 4, 5, 6, 8, 10, 12};
    lib::Tool t;
    t.type = type;
    t.diameter = diameter > 0 ? diameter : rng.pick(sizes);
    t.flutes = rng.range(2, 4);
    t.fluteLength = std::round(t.diameter * rng.uniform(1.5, 4) * 10) / 10;
    t.shankDiameter = rng.coin(0.8) ? t.diameter : std::max(3.0, std::round(t.diameter * 0.8));
    t.shoulderLength = t.fluteLength + (rng.coin(0.3) ? std::round(rng.uniform(2, 15)) : 0);
    t.overallLength = t.shoulderLength + std::round(rng.uniform(30, 50));
    switch (type) {
    case T::BullEndMill:
        t.cornerRadius = std::round(t.diameter * rng.uniform(0.1, 0.25) * 10) / 10;
        break;
    case T::Drill:
        t.tipAngleDeg = rng.pick(std::vector<double>{118, 118, 135, 140});
        t.flutes = 2;
        break;
    case T::SpotDrill:
        t.tipAngleDeg = rng.pick(std::vector<double>{90, 120});
        break;
    case T::ChamferMill:
    case T::EngraveCutter:
        t.tipAngleDeg = rng.pick(std::vector<double>{60, 90});
        t.tipDiameter = rng.coin() ? 0 : 0.2;
        break;
    case T::Tap:
    case T::ThreadMill: {
        // ISO 261 coarse pitches by nominal diameter.
        static const std::vector<std::pair<double, double>> coarse = {{3, 0.5},  {4, 0.7},  {5, 0.8},  {6, 1},
                                                                      {8, 1.25}, {10, 1.5}, {12, 1.75}};
        double pitch = 1;
        for (const auto& [d, p] : coarse) {
            if (d <= t.diameter + 1e-9) {
                pitch = p;
            }
        }
        t.threadPitch = pitch;
        break;
    }
    default:
        break;
    }
    return t;
}

Claims defaultClaims(const PropertyCase& c)
{
    Claims k = claimsFor(c.op, c.geom);
    k.holderClearance = 0.5;
    k.wcsInMachine = Vec3{100, 100, -400}; // the harness's machine: X 0..762, Y 0..406, Z -508..0
    return k;
}

std::string PropertyCase::describe() const
{
    std::ostringstream s;
    const Box3& b = geom.stockBounds;
    s << "seed " << seed << ": " << part.describe() << "\n    stock (" << num(b.min.x) << ", " << num(b.min.y) << ", "
      << num(b.min.z) << ")..(" << num(b.max.x) << ", " << num(b.max.y) << ", " << num(b.max.z) << ")\n    tool "
      << toolWord(tool.type) << " D " << num(tool.diameter) << " flutes " << num(tool.fluteLength) << " shank "
      << num(tool.shankDiameter) << " shoulder " << num(tool.shoulderLength) << " overall " << num(tool.overallLength);
    if (tool.cornerRadius > 0) {
        s << " corner r " << num(tool.cornerRadius);
    }
    if (tool.tipAngleDeg > 0) {
        s << " tip " << num(tool.tipAngleDeg) << " deg";
    }
    if (tool.threadPitch > 0) {
        s << " pitch " << num(tool.threadPitch);
    }
    s << " stick-out " << num(op.tool.stickOut) << (withHolder ? " in a holder" : " without a holder") << "\n    rpm "
      << num(op.cutting.spindleRpm) << " feed " << num(op.cutting.feedCut) << " heights clearance "
      << num(op.heights.clearance) << " retract " << num(op.heights.retract) << " feed plane "
      << num(op.heights.feedPlane) << " top " << num(op.heights.top) << " bottom " << num(op.heights.bottom)
      << "\n    params " << op.paramsJson << "\n    " << geom.selected.size() << " face(s) selected";
    return s.str();
}

std::vector<std::pair<kernel::SubId, Feature::Hole>> holeFaces(const PropertyCase& c)
{
    std::vector<std::pair<kernel::SubId, Feature::Hole>> out;
    const kernel::Shape& s = c.part.shape;
    const std::vector<kernel::SubId> faces = facesOf(s);
    for (const Feature& f : c.part.features) {
        for (const Feature::Hole& h : f.holes) {
            for (const kernel::SubId face : faces) {
                const auto info = kernel::surfaceInfo(s, face);
                if (info && info->type == kernel::SurfaceType::Cylinder &&
                    std::abs(std::abs(info->axis.z) - 1) < 1e-9 && std::abs(info->origin.x - h.x) < 1e-6 &&
                    std::abs(info->origin.y - h.y) < 1e-6 && std::abs(info->radius - h.diameter / 2) < 1e-6) {
                    out.emplace_back(face, h);
                    break;
                }
            }
        }
    }
    return out;
}

namespace pick {

std::vector<kernel::SubId> none(PropertyCase&)
{
    return {};
}

std::vector<kernel::SubId> holes(PropertyCase& c)
{
    std::vector<kernel::SubId> out;
    for (const auto& [face, hole] : holeFaces(c)) {
        out.push_back(face);
    }
    return out;
}

std::vector<kernel::SubId> pocketFloors(PropertyCase& c)
{
    std::vector<kernel::SubId> out;
    for (const Feature& f : c.part.features) {
        if (f.kind == Feature::Kind::Holes) {
            continue;
        }
        const Box3 b = regionBox(f);
        for (const kernel::SubId face : upFacesAt(c.part.shape, f.floorZ, &b)) {
            if (std::find(out.begin(), out.end(), face) == out.end()) {
                out.push_back(face);
            }
        }
    }
    return out;
}

std::vector<kernel::SubId> topFaces(PropertyCase& c)
{
    return upFacesAt(c.part.shape, 0, nullptr);
}

std::vector<kernel::SubId> outerWalls(PropertyCase& c)
{
    std::vector<kernel::SubId> out;
    const kernel::Shape& s = c.part.shape;
    for (const kernel::SubId f : facesOf(s)) {
        const auto info = kernel::surfaceInfo(s, f);
        if (!info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        const Box3& b = info->bounds;
        const bool onX = std::abs(b.min.x - b.max.x) < 1e-9 &&
                         (std::abs(b.min.x) < 1e-9 || std::abs(b.min.x - c.part.length) < 1e-9);
        const bool onY =
            std::abs(b.min.y - b.max.y) < 1e-9 && (std::abs(b.min.y) < 1e-9 || std::abs(b.min.y - c.part.width) < 1e-9);
        if (onX || onY) {
            out.push_back(f);
        }
    }
    return out;
}

} // namespace pick

std::optional<AllowedCut> chamferCut(const kernel::Shape& shape, kernel::SubId edge, double depth, double slope,
                                     double grow)
{
    // The edge in the XY plane by the kernel's curve rules (lines, arcs with their direction, whole circles).
    const auto curves = kernel::edgeCurves(shape, edge, Xform::identity());
    if (!curves || curves->curves.empty() || curves->zMax - curves->zMin > 1e-6) {
        return std::nullopt;
    }
    AllowedCut a;
    a.radius = grow + 0.001; // arcs are chorded within 1 um, inside the arc
    a.slope = slope;
    a.zBottom = curves->zMax - depth;
    a.zTop = curves->zMax + 1;
    for (const kernel::ProfileCurve& c : curves->curves) {
        using K = kernel::ProfileCurve::Kind;
        if (c.kind == K::Line) {
            if (a.path.empty()) {
                a.path.push_back(c.pts[0]);
            }
            a.path.push_back(c.pts[1]);
            continue;
        }
        if (c.kind != K::Arc && c.kind != K::Circle) {
            return std::nullopt;
        }
        const Vec2 centre = c.kind == K::Arc ? c.pts[2] : c.pts[0];
        const Vec2 start = c.kind == K::Arc ? c.pts[0] : Vec2{centre.x + c.radius, centre.y};
        const double r = std::hypot(start.x - centre.x, start.y - centre.y);
        const double a0 = std::atan2(start.y - centre.y, start.x - centre.x);
        const double dir = c.kind == K::Circle || c.ccw ? 1.0 : -1.0;
        double sweep = 2 * kPi;
        if (c.kind == K::Arc) {
            sweep = dir * (std::atan2(c.pts[1].y - centre.y, c.pts[1].x - centre.x) - a0);
            while (sweep <= 1e-12) {
                sweep += 2 * kPi;
            }
            while (sweep > 2 * kPi) {
                sweep -= 2 * kPi;
            }
        } else {
            a.closed = true;
        }
        // Chords within 1 um of the arc (sag r (1 - cos(step / 2))), at least 8 per turn.
        const double step = std::min(kPi / 4, 2 * std::acos(std::max(-1.0, 1 - 0.001 / std::max(r, 1e-9))));
        const int n = std::max(1, static_cast<int>(std::ceil(sweep / step)));
        for (int k = a.path.empty() ? 0 : 1; k <= n; ++k) {
            const double t = a0 + dir * sweep * k / n;
            a.path.push_back(Vec2{centre.x + r * std::cos(t), centre.y + r * std::sin(t)});
        }
    }
    a.pathLo = Vec2{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    a.pathHi = Vec2{-a.pathLo.x, -a.pathLo.y};
    for (const Vec2& p : a.path) {
        a.pathLo = Vec2{std::min(a.pathLo.x, p.x), std::min(a.pathLo.y, p.y)};
        a.pathHi = Vec2{std::max(a.pathHi.x, p.x), std::max(a.pathHi.y, p.y)};
    }
    return a;
}

std::vector<ClearClaim> coverFeatures(const PropertyCase& c, std::vector<Feature::Kind> kinds, double scallop,
                                      std::string id)
{
    std::vector<ClearClaim> out;
    for (const Feature& f : c.part.features) {
        if (std::find(kinds.begin(), kinds.end(), f.kind) == kinds.end() || f.region.empty()) {
            continue;
        }
        ClearClaim cl;
        cl.region = Region::of(f.region);
        cl.floorZ = f.floorZ;
        cl.allowedAbove = scallop;
        cl.id = id;
        out.push_back(std::move(cl));
    }
    return out;
}

std::vector<std::uint64_t> seedsFor(const std::string& type, int count)
{
    std::uint64_t h = 1469598103934665603ULL; // FNV-1a of the type name: each type its own seeds
    for (const char ch : type) {
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    }
    std::vector<std::uint64_t> out;
    for (int k = 0; k < count; ++k) {
        out.push_back((h % 1000000) * 1000 + static_cast<std::uint64_t>(k));
    }
    return out;
}

PropertyOutcome runProperty(const PropertyPlugin& p, const std::vector<std::uint64_t>& seeds)
{
    PropertyOutcome o;
    const auto t0 = std::chrono::steady_clock::now();
    auto registry = std::make_unique<OperationRegistry>();
    std::unique_ptr<IOperationType> own;
    const IOperationType* type = nullptr;
    if (p.make) {
        own = p.make();
        type = own.get();
    } else {
        registerBuiltinOperationTypes(*registry);
        type = registry->find(p.type);
    }
    if (type == nullptr) {
        o.failures.push_back("the operation type '" + p.type + "' is not registered");
        return o;
    }
    for (const std::uint64_t seed : seeds) {
        ++o.cases;
        PropertyCase c;
        c.seed = seed;
        c.rng = Rng(seed);
        const auto tPart = std::chrono::steady_clock::now();
        auto part = randomPart(c.rng, p.needs);
        o.partSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - tPart).count();
        if (!part) {
            o.failures.push_back("seed " + std::to_string(seed) +
                                 ": the random part did not build: " + part.error().message);
            continue;
        }
        c.part = std::move(*part);
        // Stock: the part's box, sometimes grown at the sides and on top.
        Box3 stock{Vec3{0, 0, -c.part.thickness}, Vec3{c.part.length, c.part.width, 0}};
        if (c.rng.coin(0.4)) {
            const double side = std::round(c.rng.uniform(0.5, 3) * 10) / 10;
            stock.min.x -= side;
            stock.min.y -= side;
            stock.max.x += side;
            stock.max.y += side;
        }
        if (c.rng.coin(0.4)) {
            stock.max.z += std::round(c.rng.uniform(0.2, 2) * 10) / 10;
        }
        c.geom.wcsFromModel = Xform::identity();
        c.geom.partShapes = {c.part.shape};
        c.geom.stockBounds = stock;
        c.tool = randomTool(c.rng, c.rng.pick(p.tools));
        c.withHolder = c.rng.coin(0.6);
        // A collet chuck: listed from the gauge line down, a flange then a taper to the nose.
        c.holder.segments = {lib::HolderSegment{20, 44, 44}, lib::HolderSegment{40, 24, 32}};
        c.holder.gaugeLength = 60;
        c.machine.kind = lib::MachineDef::Kind::Mill3Axis;
        c.machine.axes = {lib::AxisDef{'X', 0, 762, 25400, 16500, 5000}, lib::AxisDef{'Y', 0, 406, 25400, 16500, 5000},
                          lib::AxisDef{'Z', -508, 0, 25400, 16500, 5000}};
        c.machine.maxRpm = 12000;
        c.machine.rigidTapping = true;
        c.op.type = p.type;
        c.op.name = "Op";
        c.op.tool.stickOut = std::round(c.tool.fluteLength + c.rng.uniform(5, 25));
        c.op.cutting.spindleRpm = c.rng.range(1000, 10000);
        c.op.cutting.clockwise = true;
        c.op.cutting.feedCut = std::round(c.rng.uniform(200, 2000));
        c.op.cutting.feedPlunge = std::round(c.op.cutting.feedCut / 2);
        c.op.cutting.feedRamp = std::round(c.op.cutting.feedCut * 0.7);
        c.op.cutting.feedLeadIn = c.op.cutting.feedCut;
        c.op.cutting.feedLeadOut = c.op.cutting.feedCut;
        c.op.cutting.coolant = CuttingData::Coolant::Flood;
        c.op.heights.clearance = std::round(c.rng.uniform(10, 25));
        c.op.heights.retract = std::round(c.rng.uniform(2, 5));
        c.op.heights.feedPlane = std::round(c.rng.uniform(5, 20)) / 10;
        c.op.heights.top = 0;
        c.op.heights.bottom = -std::round(c.rng.uniform(1, c.part.thickness - 2));
        c.op.paramsJson = paramsFromSchema(*type, p, c.rng);
        const std::vector<kernel::SubId> sel = p.geometry ? p.geometry(c) : std::vector<kernel::SubId>{};
        c.geom.selected = sel;
        c.geom.selectedShapeIndex.assign(sel.size(), 0);
        if (p.adjust) {
            p.adjust(c);
        }
        JobContext ctx1, ctx2;
        const auto tGen = std::chrono::steady_clock::now();
        auto first = type->generate(c.input(), ctx1);
        if (!first) {
            ++o.refused;
            o.refusals.push_back(std::to_string(seed) + ": " + first.error().message);
            continue;
        }
        ++o.generated;
        auto second = type->generate(c.input(), ctx2);
        if (!second || second->serialize() != first->serialize()) {
            o.failures.push_back("[determinism] the same input gave different toolpath bytes\n    " + c.describe());
            continue;
        }
        const auto tCheck = std::chrono::steady_clock::now();
        o.generateSeconds += std::chrono::duration<double>(tCheck - tGen).count();
        const Claims claims = p.claims ? p.claims(c) : defaultClaims(c);
        const std::vector<Finding> found = check(
            CheckInput{*first, c.geom, c.tool, c.withHolder ? &c.holder : nullptr, &c.machine, claims}, p.options);
        o.checkSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - tCheck).count();
        if (!found.empty()) {
            o.failures.push_back(c.describe() + "\n" + describe(found));
        }
    }
    o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return o;
}

} // namespace ray::cam::invariants
