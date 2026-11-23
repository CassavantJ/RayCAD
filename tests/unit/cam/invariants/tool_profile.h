// The tool as the invariant checker sees it (task T-058): a solid of revolution about the tool axis, given as the
// radius at each height above the tip, in bands from the tip up. Test-only, written from libraries.h and cam.h; shares
// nothing with src/cam, src/sim or the tool library UI (whose drawing of the same tool it agrees with: the neck below
// the shoulder is min(cutting radius, shank radius), the step to the shank a 45 degree cone).
//
// Zones: Cutting (the flutes, tip to fluteLength), Body (the tool above its flutes: neck, shoulder, shank, up to the
// holder) and Holder (from the holder's nose, ToolRef::stickOut above the tip, up; the widest holder segment carries on
// upward without end, for the spindle nose, which no library item describes). Without a holder the shank carries on
// upward without end and no Holder zone exists.
//
// Shapes by tool type (radius R = diameter / 2, from the tip up):
//   FlatEndMill, FaceMill, BoringBar, Reamer, Tap   a flat end: R from h = 0
//   ThreadMill   flat, R (the tips of its teeth); without a stated neck, its neck is taken as R - 0.6134 pitch (ISO
//   68-1
//                external thread depth H 17/24), as it must clear the thread it cuts
// A tool that states a neck (libraries.h neckDiameter, after M3): the neck's radius from the top of the flutes up to
// neckLength (else the shoulder), then to the shoulder min(cutting radius, shank radius); a widening is a 45 degree
// cone.
//   BullEndMill  a quarter circle of radius cornerRadius at the corner, then R   (cornerRadius >= R: a ball)
//   BallEndMill  a quarter circle of radius R
//   ChamferMill, SpotDrill, Drill, EngraveCutter   a cone of included angle tipAngleDeg from tipDiameter / 2 at the tip
//                to R (an angle outside (0, 180): flat)
// A fluteLength of 0 (unknown) is taken as overallLength, else 4 diameters; a shank diameter of 0 as the cutting one.
#pragma once

#include <ray/libraries.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace ray::cam::invariants {

inline constexpr double kInf = std::numeric_limits<double>::infinity();

enum class Zone : std::uint8_t { Cutting, Body, Holder };
const char* zoneName(Zone);

// One band: the radius on [h0, h1] is linear from r0 to r1, or an arc (radius rad about (cr, ch) in (radius, height)
// coordinates, the lower-right quarter: r = cr + sqrt(rad^2 - (ch - h)^2), increasing with h). Within a band the radius
// is monotone. h1 may be infinite (then r0 == r1).
struct Band {
    double h0 = 0, h1 = 0;
    double r0 = 0, r1 = 0;
    bool arc = false;
    double cr = 0, ch = 0, rad = 0;
    Zone zone = Zone::Cutting;

    double radiusAt(double h) const;
    bool increasing() const { return arc || r1 >= r0; }
    double maxRadius() const { return arc ? radiusAt(h1) : (r0 > r1 ? r0 : r1); }
    // Increasing band: the lowest height in it whose radius is >= d (h0 when r(h0) >= d; h1 + something when none).
    double inverseUp(double d) const;
    // Decreasing band: the highest height in it whose radius is >= d.
    double inverseDown(double d) const;
};

class ToolProfile {
public:
    std::vector<Band> bands; // contiguous from h = 0 up; the last one is infinite

    // The radius at height h above the tip: the largest of the bands that hold h; 0 below the tip.
    double radiusAt(double h) const;
    double maxRadius(Zone) const;
    double maxRadius() const;
    bool hasZone(Zone) const;
    double zoneBottom(Zone) const; // +inf when the zone does not exist
    // The bottom of the run of the tool's solid at radius d that holds height g of band `band` (whose radius at g must
    // be >= d): the lowest h <= g with radius >= d all the way from h to g. The "vertical depth" of a point at radius d
    // and height g inside the tool is g - hlow.
    double hlow(double d, std::size_t band, double g) const;
    // Heights (above the tip) the tool's solid holds at radius d, as sorted disjoint intervals.
    void occupied(double d, std::vector<std::pair<double, double>>& out) const;
    // The cutting radius at the tip region (largest radius of the Cutting zone).
    double cuttingRadius() const { return maxRadius(Zone::Cutting); }
};

// The profile of `tool` in `holder` (null: none) with the holder's nose `stickOut` mm above the tip (0: the tool's
// overall length, else 6 diameters).
ToolProfile makeProfile(const lib::Tool& tool, const lib::Holder* holder, double stickOut);

// Penetration of material into the tool, bounded from above and below (see checker.h, "How a contact is measured").
// For material whose WCS Z spans [zlo, zhi] (already grown by the vertical allowance), seen at radial distance dUp
// (smallest possible, for the upper bound) and dLo (largest possible, for the lower bound) from the tool axis, with the
// tip between ztMin and ztMax (upper bound) and at ztStar (lower bound; a real tool position):
//   up   >= the largest penetration any point of that material can have into `zone`
//   lo   <= the penetration of some real point of it (or -inf)
// Penetration of a point at radius d and height g above the tip, inside the tool: min(radius(g) - d, g - hlow), the
// smaller of its horizontal and vertical depths inside the tool's solid (each at least the true normal depth).
struct PenBounds {
    double up = -kInf, lo = -kInf;
    double zUp = 0; // the material height the upper bound was found at
};
PenBounds penetration(const ToolProfile& p, Zone zone, double zlo, double zhi, double ztMin, double ztMax,
                      double ztStar, double dUp, double dLo);

} // namespace ray::cam::invariants
