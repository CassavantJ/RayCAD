// The tool's motion as the invariant checker sees it (task T-058): a toolpath (cam.h) turned into pieces, each a
// straight line or a circular arc in XY (helical when Z changes) of the tool tip, in WCS millimetres. Test-only,
// written from cam.h's text; shares nothing with src/cam, src/sim or src/post.
//
//   Rapid, Linear       one line from the tool's position to the move's end
//   ArcCW, ArcCCW, XY   one arc about (i, j), radius |start - centre|, Z linear in the angle; CCW seen from +Z.
//                       Start equal to end (in nanometres): a full turn
//   arcs in XZ, YZ      chords of the arc in its plane (G18: (Z, X) seen from +Y; G19: (Y, Z) seen from +X), each
//                       carrying its sagitta as an uncertainty (`inflate`, at most kChordSagittaMm)
//   cycles              cam.h steps 1 to 4: a rapid in XY at the tool's Z, a rapid down to the clearance when above it,
//                       a rapid to R, a feed to the bottom (pecks and re-entries stay inside what that feed cuts, so
//                       they are not separate pieces), then out to the return level (feed for tap, bore and ream up to
//                       R, rapid otherwise)
//   Dwell, CycleCancel  no motion
#pragma once

#include <ray/cam.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace ray::cam::invariants {

inline constexpr double kChordSagittaMm = 1e-4;

struct Piece {
    std::size_t move = 0; // index into Toolpath::moves
    MoveKind kind = MoveKind::Rapid;
    MoveRole role = MoveRole::Link;
    bool rapid = false;
    bool arc = false;
    Vec3 a{}, b{};                                     // tip at start and at end, mm
    double cx = 0, cy = 0, ra = 0, th0 = 0, sweep = 0; // arc: centre, radius, start angle, signed sweep (CCW > 0)
    double inflate = 0;                                // extra XY and Z uncertainty of this piece (chords)

    double zmin() const { return std::min(a.z, b.z) - inflate; }
    double zmax() const { return std::max(a.z, b.z) + inflate; }
    double xyLength() const;
    bool vertical() const; // no XY motion
    // The XY point at parameter t in [0, 1], the tip Z there, and the XY direction of travel (unit; 0 0 if none).
    Vec3 at(double t) const;
    Vec2 direction(double t) const;

    struct Near {
        double d = 0; // XY distance from q to the path of the tool axis
        double t = 0; // the parameter of the nearest point
        double z = 0; // the tip Z there (a vertical piece: its lowest Z)
    };
    Near nearest(double qx, double qy) const;
    // The parameter range [t0, t1] holding every position of the piece within XY distance rho of q (at least the
    // nearest one; for an arc the hull of the reachable parts, so a position in it may be farther than rho).
    std::pair<double, double> reach(double qx, double qy, double rho, const Near& n) const;
    // The tip's Z range over those positions.
    std::pair<double, double> zRange(double qx, double qy, double rho, const Near& n) const;
    // The XY box of the path.
    void box(double& x0, double& y0, double& x1, double& y1) const;
    // The same piece cut to the parameter range [t0, t1].
    Piece part(double t0, double t1) const;
};

// The pieces of a toolpath, in order. `startOf[n]` is the tool's position before move n (mm), in moves.size() + 1
// entries (the last: where the toolpath ends).
struct Motion {
    std::vector<Piece> pieces;
    std::vector<Vec3> startOf;
};
Motion motionOf(const Toolpath& path);

inline double mmOf(Nm v)
{
    return static_cast<double>(v) / 1e6;
}

} // namespace ray::cam::invariants
