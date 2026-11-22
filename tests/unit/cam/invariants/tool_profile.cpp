// The tool profile of the invariant checker (tool_profile.h).
#include "tool_profile.h"

#include <algorithm>
#include <cmath>

namespace ray::cam::invariants {

const char* zoneName(Zone z)
{
    switch (z) {
    case Zone::Cutting:
        return "flutes";
    case Zone::Body:
        return "shank";
    case Zone::Holder:
        return "holder";
    }
    return "?";
}

double Band::radiusAt(double h) const
{
    if (arc) {
        const double dh = ch - h;
        return cr + std::sqrt(std::max(0.0, rad * rad - dh * dh));
    }
    if (!(h1 > h0) || std::isinf(h1) || r0 == r1) {
        return r0;
    }
    return r0 + (r1 - r0) * (h - h0) / (h1 - h0);
}

double Band::inverseUp(double d) const
{
    if (d <= radiusAt(h0)) {
        return h0;
    }
    if (d > radiusAt(h1)) {
        return kInf;
    }
    if (arc) {
        const double dr = d - cr;
        return std::clamp(ch - std::sqrt(std::max(0.0, rad * rad - dr * dr)), h0, h1);
    }
    return std::clamp(h0 + (d - r0) / (r1 - r0) * (h1 - h0), h0, h1);
}

double Band::inverseDown(double d) const
{
    if (d <= r1) {
        return h1;
    }
    if (d > r0) {
        return -kInf;
    }
    return std::clamp(h0 + (r0 - d) / (r0 - r1) * (h1 - h0), h0, h1);
}

double ToolProfile::radiusAt(double h) const
{
    double r = 0;
    for (const Band& b : bands) {
        if (h >= b.h0 && h <= b.h1) {
            r = std::max(r, b.radiusAt(h));
        }
    }
    return r;
}

double ToolProfile::maxRadius(Zone z) const
{
    double r = 0;
    for (const Band& b : bands) {
        if (b.zone == z) {
            r = std::max(r, b.maxRadius());
        }
    }
    return r;
}

double ToolProfile::maxRadius() const
{
    double r = 0;
    for (const Band& b : bands) {
        r = std::max(r, b.maxRadius());
    }
    return r;
}

bool ToolProfile::hasZone(Zone z) const
{
    return std::any_of(bands.begin(), bands.end(), [&](const Band& b) { return b.zone == z; });
}

double ToolProfile::zoneBottom(Zone z) const
{
    for (const Band& b : bands) {
        if (b.zone == z) {
            return b.h0;
        }
    }
    return kInf;
}

double ToolProfile::hlow(double d, std::size_t band, double g) const
{
    double cur = g;
    for (std::size_t n = band + 1; n-- > 0;) {
        const Band& b = bands[n];
        if (n != band) {
            // Coming from above: the run goes on into this band only if its top is wide enough.
            if (b.radiusAt(b.h1) < d) {
                return cur;
            }
        }
        if (b.increasing()) {
            if (b.radiusAt(b.h0) >= d) {
                cur = b.h0;
                continue;
            }
            return std::min(cur, b.inverseUp(d));
        }
        cur = b.h0; // decreasing upward: everything below a wide enough point is wide enough
    }
    return cur;
}

void ToolProfile::occupied(double d, std::vector<std::pair<double, double>>& out) const
{
    out.clear();
    for (const Band& b : bands) {
        double a = 0, c = 0;
        if (b.increasing()) {
            if (b.radiusAt(b.h1) < d) {
                continue;
            }
            a = std::max(b.h0, b.inverseUp(d));
            c = b.h1;
        } else {
            if (b.r0 < d) {
                continue;
            }
            a = b.h0;
            c = std::min(b.h1, b.inverseDown(d));
        }
        if (a > c) {
            continue;
        }
        if (!out.empty() && a <= out.back().second) {
            out.back().second = std::max(out.back().second, c);
        } else {
            out.emplace_back(a, c);
        }
    }
}

namespace {

void addLinear(ToolProfile& p, double h0, double h1, double r0, double r1, Zone z)
{
    if (!(h1 > h0)) {
        return;
    }
    Band b;
    b.h0 = h0;
    b.h1 = h1;
    b.r0 = r0;
    b.r1 = std::isinf(h1) ? r0 : r1;
    b.zone = z;
    p.bands.push_back(b);
}

// Appends the bands of `src` cut to [lo, hi] (heights), each with zone z.
void appendClipped(ToolProfile& p, const std::vector<Band>& src, double lo, double hi, Zone z)
{
    for (Band b : src) {
        const double a = std::max(b.h0, lo), c = std::min(b.h1, hi);
        if (!(c > a)) {
            continue;
        }
        if (!b.arc) {
            const double ra = b.radiusAt(a), rc = std::isinf(c) ? b.r1 : b.radiusAt(c);
            b.r0 = ra;
            b.r1 = rc;
        }
        b.h0 = a;
        b.h1 = c;
        b.zone = z;
        p.bands.push_back(b);
    }
}

} // namespace

ToolProfile makeProfile(const lib::Tool& tool, const lib::Holder* holder, double stickOut)
{
    using T = lib::ToolType;
    constexpr double kPi = 3.14159265358979323846;
    const double R = std::max(tool.diameter, 0.0) / 2;
    const double rs = tool.shankDiameter > 0 ? tool.shankDiameter / 2 : R;
    const double L = tool.overallLength > 0 ? tool.overallLength : 0;
    const double lf = tool.fluteLength > 0 ? tool.fluteLength : (L > 0 ? L : 8 * R);
    const double nose = stickOut > 0 ? stickOut : (L > 0 ? L : 12 * R);

    // The tool alone, from the tip up to infinity (zones assigned below).
    std::vector<Band> shape;
    double h = 0;    // top of what is built so far
    double rTop = R; // radius there
    switch (tool.type) {
    case T::BallEndMill:
    case T::BullEndMill: {
        const double rc = tool.type == T::BallEndMill ? R : std::clamp(tool.cornerRadius, 0.0, R);
        if (rc > 0) {
            Band b;
            b.arc = true;
            b.h0 = 0;
            b.h1 = rc;
            b.cr = R - rc;
            b.ch = rc;
            b.rad = rc;
            shape.push_back(b);
            h = rc;
        }
        break;
    }
    case T::ChamferMill:
    case T::SpotDrill:
    case T::Drill:
    case T::EngraveCutter: {
        const double a = tool.tipAngleDeg;
        const double rt = std::clamp(tool.tipDiameter / 2, 0.0, R);
        if (a > 0 && a < 180 && rt < R) {
            const double hc = (R - rt) / std::tan(a * kPi / 360.0);
            shape.push_back(Band{0, hc, rt, R, false, 0, 0, 0, Zone::Cutting});
            h = hc;
        }
        break;
    }
    default:
        break;
    }
    // Up the flutes.
    if (lf > h) {
        shape.push_back(Band{h, lf, R, R, false, 0, 0, 0, Zone::Cutting});
        h = lf;
    }
    const double top = h; // the top of the cutting shape (the flutes, or a tip taller than them)
    rTop = shape.empty() ? R : shape.back().radiusAt(top);
    // The neck to the shoulder, the step to the shank, the shank.
    double neck = std::min(rTop, rs);
    if (tool.type == T::ThreadMill && tool.threadPitch > 0) {
        neck = std::min(neck, std::max(R - 0.6134 * tool.threadPitch, 0.1 * R));
    }
    if (tool.shoulderLength > h) {
        shape.push_back(Band{h, tool.shoulderLength, neck, neck, false, 0, 0, 0, Zone::Body});
        h = tool.shoulderLength;
    }
    if (std::abs(rs - neck) > 1e-12) {
        const double h1 = h + std::abs(rs - neck);
        shape.push_back(Band{h, h1, neck, rs, false, 0, 0, 0, Zone::Body});
        h = h1;
    }
    shape.push_back(Band{h, kInf, rs, rs, false, 0, 0, 0, Zone::Body});

    ToolProfile p;
    const bool withHolder = holder != nullptr && !holder->segments.empty();
    const double toolTop = withHolder ? nose : kInf;
    appendClipped(p, shape, 0, std::min(lf, toolTop), Zone::Cutting);
    appendClipped(p, shape, std::min(lf, toolTop), toolTop, Zone::Body);
    if (!withHolder) {
        return p;
    }
    // The holder: segments listed from the gauge line down; the stack is put with its top on the gauge line, which is
    // max(gaugeLength, the stack's height) above the nose, and a stack that ends above the nose carries its lowest
    // diameter down to it. The tool's shank inside it counts too (radius max(holder, shank)).
    double stack = 0, widest = rs;
    for (const lib::HolderSegment& s : holder->segments) {
        stack += std::max(0.0, s.height);
        widest = std::max({widest, s.lowerDiameter / 2, s.upperDiameter / 2});
    }
    const double gauge = std::max(holder->gaugeLength, stack);
    double y = nose + gauge;
    std::vector<Band> topDown;
    for (const lib::HolderSegment& s : holder->segments) {
        const double height = std::max(0.0, s.height);
        if (height <= 0) {
            continue;
        }
        topDown.push_back(Band{y - height, y, std::max(s.lowerDiameter / 2, rs), std::max(s.upperDiameter / 2, rs),
                               false, 0, 0, 0, Zone::Holder});
        y -= height;
    }
    if (!topDown.empty() && y > nose) {
        const double r = topDown.back().r0;
        addLinear(p, nose, y, r, r, Zone::Holder);
    }
    for (auto it = topDown.rbegin(); it != topDown.rend(); ++it) {
        p.bands.push_back(*it);
    }
    addLinear(p, nose + gauge, kInf, widest, widest, Zone::Holder);
    return p;
}

PenBounds penetration(const ToolProfile& p, Zone zone, double zlo, double zhi, double ztMin, double ztMax,
                      double ztStar, double dUp, double dLo)
{
    PenBounds out;
    const double zb = p.zoneBottom(zone); // the vertical depth into a zone counts from the zone's bottom
    for (std::size_t i = 0; i < p.bands.size(); ++i) {
        const Band& b = p.bands[i];
        if (b.zone != zone) {
            continue;
        }
        // Upper bound: any tip in [ztMin, ztMax], any distance >= dUp.
        double ga = std::max(zlo - ztMax, b.h0), gb = std::min(zhi - ztMin, b.h1);
        if (ga <= gb) {
            if (b.increasing()) {
                const double r = b.radiusAt(gb);
                if (r >= dUp) {
                    const double v = std::min(r - dUp, gb - std::max(p.hlow(dUp, i, gb), zb));
                    if (v > out.up) {
                        out.up = v;
                        out.zUp = gb + ztMin;
                    }
                }
            } else {
                const double r = b.radiusAt(ga);
                if (r >= dUp) {
                    const double gs = std::min(gb, b.inverseDown(dUp));
                    const double v = std::min(r - dUp, gs - std::max(p.hlow(dUp, i, ga), zb));
                    if (v > out.up) {
                        out.up = v;
                        out.zUp = ga + ztMin;
                    }
                }
            }
        }
        // Lower bound: the tip at ztStar, the distance dLo: a real point's penetration.
        ga = std::max(zlo - ztStar, b.h0);
        gb = std::min(zhi - ztStar, b.h1);
        if (ga <= gb) {
            const double g = b.increasing() ? gb : ga;
            const double r = b.radiusAt(g);
            if (r >= dLo) {
                out.lo = std::max(out.lo, std::min(r - dLo, g - std::max(p.hlow(dLo, i, g), zb)));
            }
        }
    }
    return out;
}

} // namespace ray::cam::invariants
