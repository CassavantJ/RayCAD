// The part as the invariant checker sees it (task T-058): the triangle mesh of the setup's part and fixture bodies
// (kernel::triangulateExact at a stated chord tolerance, moved into the WCS), on a square grid in XY. Test-only; the
// kernel is used only to mesh the shapes.
//
// CONSERVATIVE Z-SETS. For any axis-aligned square Q of the grid (a cell, or a quarter of one, and so on down), the
// checker needs the heights at which material may lie somewhere over Q. zset() returns the union of
//   (a) the material intervals of the vertical line through Q's centre (ray parity against the mesh), and
//   (b) for every triangle that meets the column over Q, the Z range of the triangle clipped to that column.
// Claim: if material is at (x, y, z) with (x, y) in Q, then z is in that union. Either the centre's line holds
// material at z (a), or the horizontal segment at height z from the centre to (x, y) crosses the surface, at a point
// of a triangle that meets Q's column at height z (b). So the set is a superset (nothing is missed), and every height
// in it does hold material somewhere over Q (a: at the centre; b: next to the triangle, inside Q's closed column).
// Both directions are what the checker's upper and lower bounds rely on (checker.h).
#pragma once

#include <ray/cam.h>

#include <memory>
#include <span>
#include <vector>

namespace ray::cam::invariants {

struct Iv {
    double lo = 0, hi = 0;
};
struct Tri {
    Vec3 a, b, c;
};
// A vertical cylinder of the WCS (mm) inside which cutting the part is intended (a thread, a tapped bore, a drill
// point below a flat floor that the user asked for): material there is not protected.
// Added by T-055 (chamfers the model does not have): with `slope` > 0 the cut is a cone opening upwards, of radius
// radius + (z - zBottom) slope at height z (a chamfer mill's flank: slope = tan(half the tip angle)); with a `path` of
// two or more points (WCS XY mm; `closed` joins the last to the first) the radius is measured from that polyline
// instead of from (x, y), so the cut runs along it: a cone along an edge is the wedge a chamfer of that edge removes.
// `pathLo` / `pathHi` are the path's box when known (lo.x <= hi.x; chamferCut sets them): points far from it are
// rejected without measuring the path.
struct AllowedCut {
    double x = 0, y = 0, radius = 0, zBottom = 0, zTop = 0;
    double slope = 0;
    std::vector<Vec2> path;
    bool closed = false;
    Vec2 pathLo{1, 1}, pathHi{0, 0};
};
// The horizontal distance from (px, py) to the cut's axis, or to its path.
double allowedDistance(const AllowedCut& a, double px, double py);
// The lowest height at which every point of a disc of radius `spread` about a point at distance d from the cut's
// axis or path lies inside the cut (+infinity: none; the cut ends at zTop).
double allowedFrom(const AllowedCut& a, double d, double spread);

void mergeIntervals(std::vector<Iv>& v);
void subtractInterval(std::vector<Iv>& v, double lo, double hi);

class PartModel {
public:
    static constexpr int kBlock = 8; // cells per block side (the block tops are the quick rejection)

    // The mesh of geom's part and fixture shapes in the WCS on a grid of `cell` mm. KernelFailure when a shape does not
    // mesh. An empty geometry gives an empty model.
    static Result<std::shared_ptr<const PartModel>> build(const ResolvedGeometry& geom, double cell, double chord);
    // A model of the given WCS triangles (a closed surface), for tests.
    static std::shared_ptr<const PartModel> fromTriangles(std::vector<Tri> tris, double cell, double chord);

    bool empty() const { return tris_.empty(); }
    double cell() const { return h_; }
    double chord() const { return chord_; }
    int nx() const { return nx_; }
    int ny() const { return ny_; }
    double x0() const { return x0_; }
    double y0() const { return y0_; }
    double centreX(int i) const { return x0_ + (i + 0.5) * h_; }
    double centreY(int j) const { return y0_ + (j + 0.5) * h_; }
    std::size_t triangleCount() const { return tris_.size(); }
    int oddRays() const { return oddRays_; } // cell-centre lines with an odd number of crossings (a mesh defect)

    // The conservative Z-set of the square of half side s centred at (qx, qy), which lies in cell (i, j); material
    // inside an allowed cut is left out only where the whole square lies inside the cut's circle.
    // T-055: `wholeBelow`, when given, is set to the height below which no allowed cut reaches any point of the square
    // (+infinity when none does): only there is every height of the set known to hold protected material, which is
    // what a lower bound of a contact may rely on.
    void zset(int i, int j, double qx, double qy, double s, std::span<const AllowedCut> allowed, std::vector<Iv>& out,
              double* wholeBelow = nullptr) const;
    // The material intervals (of the mesh) on the vertical line through (x, y).
    void ray(double x, double y, std::vector<Iv>& out) const;
    // The highest material over block (bi, bj) (-inf: none).
    double blockTop(int bi, int bj) const;
    int blocksX() const { return bnx_; }
    int blocksY() const { return bny_; }
    double maxTop() const { return maxTop_; }

private:
    void index();
    void rayCell(int i, int j, double x, double y, std::vector<Iv>& out, bool& odd) const;

    std::vector<Tri> tris_;
    double h_ = 1, chord_ = 0, x0_ = 0, y0_ = 0, maxTop_ = 0;
    int nx_ = 0, ny_ = 0, bnx_ = 0, bny_ = 0, oddRays_ = 0;
    std::vector<std::uint32_t> triStart_, triOf_; // CSR: the triangles meeting each cell's column
    std::vector<std::uint32_t> zStart_;           // CSR: each cell's Z-set
    std::vector<Iv> z_;
    std::vector<double> blockTop_;
};

} // namespace ray::cam::invariants
