// The in-process stock of the invariant checker (stock_grid.h).
#include "stock_grid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ray::cam::invariants {

namespace {
constexpr float kNoTop = -std::numeric_limits<float>::infinity();
}

void StockGrid::init(const Box3& stock, double cell, double margin, InitialStock mode, const PartModel* part,
                     std::span<const AllowedCut> allowed)
{
    h_ = cell;
    // Off round coordinates, as the part grid (samples never lie exactly on a face of an axis-aligned part).
    x0_ = stock.min.x - margin + 0.0137137137 * cell;
    y0_ = stock.min.y - margin + 0.0137137137 * cell;
    nx_ = static_cast<int>(std::ceil((stock.max.x + margin - x0_) / h_)) + 1;
    ny_ = static_cast<int>(std::ceil((stock.max.y + margin - y0_) / h_)) + 1;
    const std::size_t n = static_cast<std::size_t>(nx_) * ny_;
    iv_.assign(n * kMaxIntervals, SIv{});
    count_.assign(n, 0);
    bnx_ = (nx_ + kBlock - 1) / kBlock;
    bny_ = (ny_ + kBlock - 1) / kBlock;
    blockTop_.assign(static_cast<std::size_t>(bnx_) * bny_, kNoTop);
    dirty_.assign(blockTop_.size(), 1);
    std::vector<Iv> v, p, keep;
    for (int j = 0; j < ny_; ++j) {
        for (int i = 0; i < nx_; ++i) {
            const double xv = x(i), yv = y(j);
            v.clear();
            const bool inBox = xv >= stock.min.x && xv <= stock.max.x && yv >= stock.min.y && yv <= stock.max.y;
            if (part != nullptr && !part->empty()) {
                part->ray(xv, yv, p);
            } else {
                p.clear();
            }
            if (mode == InitialStock::Box && inBox && stock.max.z > stock.min.z) {
                v.push_back(Iv{stock.min.z, stock.max.z});
                for (const Iv& q : p) {
                    subtractInterval(v, q.lo, q.hi);
                }
            }
            for (const AllowedCut& a : allowed) {
                if (std::hypot(xv - a.x, yv - a.y) <= a.radius) {
                    for (const Iv& q : p) {
                        const double lo = std::max(q.lo, a.zBottom), hi = std::min(q.hi, a.zTop);
                        if (hi > lo) {
                            v.push_back(Iv{lo, hi});
                        }
                    }
                }
            }
            mergeIntervals(v);
            set(index(i, j), v);
        }
    }
}

int StockGrid::col(double xv) const
{
    return std::clamp(static_cast<int>(std::ceil((xv - x0_) / h_)), 0, nx_);
}

int StockGrid::row(double yv) const
{
    return std::clamp(static_cast<int>(std::ceil((yv - y0_) / h_)), 0, ny_);
}

void StockGrid::set(int idx, std::vector<Iv>& v)
{
    while (v.size() > static_cast<std::size_t>(kMaxIntervals)) {
        // Merge the two closest: more material, never less.
        std::size_t best = 0;
        double gap = std::numeric_limits<double>::infinity();
        for (std::size_t k = 0; k + 1 < v.size(); ++k) {
            if (v[k + 1].lo - v[k].hi < gap) {
                gap = v[k + 1].lo - v[k].hi;
                best = k;
            }
        }
        v[best].hi = v[best + 1].hi;
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
    SIv* out = iv_.data() + static_cast<std::size_t>(idx) * kMaxIntervals;
    for (std::size_t k = 0; k < v.size(); ++k) {
        out[k] = SIv{static_cast<float>(v[k].lo), static_cast<float>(v[k].hi)};
    }
    count_[static_cast<std::size_t>(idx)] = static_cast<std::uint8_t>(v.size());
    const int i = idx % nx_, j = idx / nx_;
    dirty_[static_cast<std::size_t>(j / kBlock) * bnx_ + i / kBlock] = 1;
}

double StockGrid::top(int idx) const
{
    const std::uint8_t c = count_[static_cast<std::size_t>(idx)];
    if (c == 0) {
        return -std::numeric_limits<double>::infinity();
    }
    return iv_[static_cast<std::size_t>(idx) * kMaxIntervals + c - 1].hi;
}

void StockGrid::subtract(int idx, double lo, double hi)
{
    const std::uint8_t c = count_[static_cast<std::size_t>(idx)];
    if (c == 0 || !(hi > lo)) {
        return;
    }
    const SIv* cur = iv_.data() + static_cast<std::size_t>(idx) * kMaxIntervals;
    if (cur[c - 1].hi <= lo || cur[0].lo >= hi) {
        return;
    }
    std::vector<Iv> v;
    v.reserve(c + 1);
    for (int k = 0; k < c; ++k) {
        v.push_back(Iv{cur[k].lo, cur[k].hi});
    }
    subtractInterval(v, lo, hi);
    set(idx, v);
}

double StockGrid::blockTop(int bi, int bj) const
{
    if (bi < 0 || bj < 0 || bi >= bnx_ || bj >= bny_) {
        return -std::numeric_limits<double>::infinity();
    }
    const std::size_t b = static_cast<std::size_t>(bj) * bnx_ + bi;
    if (dirty_[b]) {
        float t = kNoTop;
        for (int j = bj * kBlock; j < std::min(ny_, (bj + 1) * kBlock); ++j) {
            for (int i = bi * kBlock; i < std::min(nx_, (bi + 1) * kBlock); ++i) {
                t = std::max(t, static_cast<float>(top(index(i, j))));
            }
        }
        blockTop_[b] = t;
        dirty_[b] = 0;
    }
    return blockTop_[b];
}

} // namespace ray::cam::invariants
