// The in-process stock as the invariant checker sees it (task T-058): material intervals in Z on the vertical lines
// through the centres of a square grid of points ("samples"), cut down move by move as the tool sweeps them. Test-only.
//
// POINT SAMPLES. Each sample is exact for its own vertical line (up to float storage, 0.01 um at 100 mm): it holds what
// the stock had there minus everything the tool's solid swept through that line. What lies between samples is not
// seen: a contact with remaining stock is found when it covers a sample, so a penetration deeper than the sample
// spacing times sqrt(2) is always found and a shallower one may slip between samples; everything reported is real at a
// sample. A sample keeps at most kMaxIntervals intervals; a fifth one merges the two closest (more material, never
// less).
//
// Initial stock (Claims::initialStock):
//   Box   the stock box (ResolvedGeometry::stockBounds) minus the part, plus the part inside the allowed cuts
//   Part  the part inside the allowed cuts only (earlier operations left exactly the part: drilling, tapping and
//         thread milling of modelled holes)
#pragma once

#include "part_model.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ray::cam::invariants {

enum class InitialStock : std::uint8_t { Box, Part };

class StockGrid {
public:
    static constexpr int kMaxIntervals = 4;
    static constexpr int kBlock = 8;
    struct SIv {
        float lo = 0, hi = 0;
    };

    // Samples every `cell` mm over the stock box grown by `margin` on each side in XY.
    void init(const Box3& stock, double cell, double margin, InitialStock mode, const PartModel* part,
              std::span<const AllowedCut> allowed);

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    double cell() const { return h_; }
    double x(int i) const { return x0_ + i * h_; }
    double y(int j) const { return y0_ + j * h_; }
    int index(int i, int j) const { return j * nx_ + i; }
    int col(double xv) const; // the first column at or after xv (clamped)
    int row(double yv) const;
    std::span<const SIv> at(int idx) const
    {
        return {iv_.data() + static_cast<std::size_t>(idx) * kMaxIntervals, count_[static_cast<std::size_t>(idx)]};
    }
    double top(int idx) const; // -inf when empty
    // Removes [lo, hi] from sample idx.
    void subtract(int idx, double lo, double hi);
    double blockTop(int bi, int bj) const; // the highest stock over the block's samples (-inf: none)
    int blocksX() const { return bnx_; }
    int blocksY() const { return bny_; }

private:
    void set(int idx, std::vector<Iv>& v);

    double x0_ = 0, y0_ = 0, h_ = 1;
    int nx_ = 0, ny_ = 0, bnx_ = 0, bny_ = 0;
    std::vector<SIv> iv_;
    std::vector<std::uint8_t> count_;
    mutable std::vector<float> blockTop_;
    mutable std::vector<std::uint8_t> dirty_;
};

} // namespace ray::cam::invariants
