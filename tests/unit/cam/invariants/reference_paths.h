// Tiny reference generators of the invariant checker's tests (task T-058): real-looking milling toolpaths written by
// hand from geometry the tests know exactly, sharing nothing with src/cam. They show the checker passes correct
// milling before the real operations exist, and their mutations are the broken twins of the geometric invariants.
#pragma once

#include <ray/cam.h>

#include <vector>

namespace ray::cam::invariants {

// Builds a toolpath move by move (WCS mm in, nanometres out).
class PathBuilder {
public:
    explicit PathBuilder(Vec3 start);
    PathBuilder& rapid(Vec3 to, MoveRole role = MoveRole::Link);
    PathBuilder& feed(Vec3 to, double mmPerMin, MoveRole role = MoveRole::Cut);
    // An arc in XY about `centre` to `to` (helical when Z changes); a full turn when `to` is the start in XY.
    PathBuilder& arc(Vec3 to, Vec2 centre, bool ccw, double mmPerMin, MoveRole role = MoveRole::Cut);
    Vec3 position() const { return pos_; }
    Move& last() { return path_.moves.back(); }
    Toolpath take() { return std::move(path_); }

private:
    Toolpath path_;
    Vec3 pos_;
};

// Zig-zag facing of the stock top: passes along X, `stepover` apart in Y, the first and last at most half a radius
// inside the stock's Y edges, each running from `overhang` beyond the tool's edge on one X side to the same on the
// other, joined by feed moves outside the stock. Enters and leaves outside the stock, at Z `z`.
struct FaceRef {
    Box3 stock;
    double toolRadius = 5, stepover = 6, overhang = 1;
    double z = 0;
    double clearance = 10;
    double feed = 1000;
};
Toolpath zigzagFace(const FaceRef& r);

// The outside contour of the rectangle [x0, x1] x [y0, y1]: the tool's centre at toolRadius + stockToLeave from it,
// arcs about its corners, one loop per depth. Climb (clockwise round the part, which is then on the cutter's right with
// a clockwise spindle) or conventional. Each loop starts in the middle of the -Y side with a tangent quarter-circle
// lead-in of leadRadius from outside, and leaves by the mirror lead-out; between loops the tool rapids to `retract`.
struct ContourRef {
    double x0 = 0, y0 = 0, x1 = 40, y1 = 30;
    double toolRadius = 3, stockToLeave = 0;
    std::vector<double> depths{-5};
    bool climb = true;
    double leadRadius = 4;
    double clearance = 10, retract = 2;
    double feed = 800;
};
Toolpath contourRect(const ContourRef& r);

// A rectangular pocket [x0, x1] x [y0, y1] with corner radius `cornerRadius` (at least toolRadius + stockToLeave): a
// helix of radius helixRadius and pitch helixPitch at the centre from zTop down to z, a full circle there, a pass along
// the centre line, then rounded-rectangle loops `stepover` apart outward to the wall loop at toolRadius + stockToLeave
// from the walls. Climb (counter-clockwise loops: the wall on the cutter's right) or conventional.
struct PocketRef {
    double x0 = 0, y0 = 0, x1 = 40, y1 = 30;
    double cornerRadius = 6;
    double toolRadius = 3, stockToLeave = 0, stepover = 2;
    double zTop = 0, z = -5;
    double helixRadius = 1.5, helixPitch = 1;
    bool climb = true;
    double clearance = 10, retract = 2;
    double feed = 800;
};
Toolpath pocketRect(const PocketRef& r);

} // namespace ray::cam::invariants
