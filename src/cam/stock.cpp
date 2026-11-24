// In-process stock (task T-075): the carried stock put into a later setup's frame, and its slices. stock.h states the
// rules; this file follows them in that order.
#include "stock.h"

#include "state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>

namespace ray::cam::detail {

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// Cancellation is polled every this many vertices or triangles.
constexpr std::size_t kPollEvery = 1u << 16;
// More grid cells than this (a stock of about 2 m x 2 m at 0.25 mm) is refused rather than allocated.
constexpr std::int64_t kMaxGridCells = std::int64_t{64} << 20;

Vec3 apply(const double* m, double x, double y, double z)
{
    return Vec3{m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                m[8] * x + m[9] * y + m[10] * z + m[11]};
}

double component(const Vec3& v, int i)
{
    return i == 0 ? v.x : i == 1 ? v.y : v.z;
}

// Row i of `rotation` dotted with the earlier setup's Z axis in the model (column 2 of modelFromWcs).
double alongEarlierZ(const Xform& rotation, const Xform& modelFromWcs, int i)
{
    const double* r = rotation.m;
    const double* m = modelFromWcs.m;
    return r[4 * i] * m[2] + r[4 * i + 1] * m[6] + r[4 * i + 2] * m[10];
}

// An axis that is the earlier Z or its reverse (frames are snapped to model axes within 1e-9, setup.cpp).
bool isEarlierZ(double dot)
{
    return std::fabs(dot) >= 1.0 - 1e-12;
}

Nm floorNm(double mm)
{
    return static_cast<Nm>(std::floor(mm * kNmPerMm));
}

Nm ceilNm(double mm)
{
    return static_cast<Nm>(std::ceil(mm * kNmPerMm));
}

} // namespace

Result<OrientedStock> orientCarried(const CarriedStock& carried, const Xform& rotation, JobContext& ctx)
{
    try {
        if (!carried.solid) {
            return failure(ErrorCode::InvalidArgument, "The carried stock has no solid");
        }
        const kernel::TriSoup& in = *carried.solid;
        const std::size_t vertices = in.positions.size() / 3;
        if (in.positions.empty() || in.indices.empty() || in.positions.size() % 3 != 0 || in.indices.size() % 3 != 0) {
            return failure(ErrorCode::InvalidArgument,
                           "The carried stock is empty: the earlier setup left no material");
        }
        for (const std::uint32_t v : in.indices) {
            if (v >= vertices) {
                return failure(ErrorCode::InvalidArgument, "The carried stock has a triangle with no vertex");
            }
        }
        OrientedStock out;
        out.soup.indices = in.indices;
        out.soup.positions.resize(in.positions.size());
        const double* m = carried.modelFromWcs.m;
        const double* r = rotation.m;
        Vec3 lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::infinity()};
        Vec3 hi{-lo.x, -lo.y, -lo.z};
        for (std::size_t v = 0; v < vertices; ++v) {
            if (v % kPollEvery == 0) {
                RAY_CAM_TRY(ctx.cancel.check());
            }
            const Vec3 model = apply(m, in.positions[3 * v], in.positions[3 * v + 1], in.positions[3 * v + 2]);
            const Vec3 q = apply(r, model.x, model.y, model.z);
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
                return failure(ErrorCode::InvalidArgument, "The carried stock has a vertex that is not a number");
            }
            out.soup.positions[3 * v] = q.x;
            out.soup.positions[3 * v + 1] = q.y;
            out.soup.positions[3 * v + 2] = q.z;
            lo = Vec3{std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = Vec3{std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        // The earlier setup's stock box, moved the same way (its eight corners).
        Vec3 srcLo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity()};
        Vec3 srcHi{-srcLo.x, -srcLo.y, -srcLo.z};
        const Box3& b = carried.bounds;
        for (int k = 0; k < 8; ++k) {
            const Vec3 model =
                apply(m, (k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z);
            const Vec3 q = apply(r, model.x, model.y, model.z);
            srcLo = Vec3{std::min(srcLo.x, q.x), std::min(srcLo.y, q.y), std::min(srcLo.z, q.z)};
            srcHi = Vec3{std::max(srcHi.x, q.x), std::max(srcHi.y, q.y), std::max(srcHi.z, q.z)};
        }
        // Each side (stock.h THE BOX).
        for (int i = 0; i < 3; ++i) {
            const bool acrossZ = isEarlierZ(alongEarlierZ(rotation, carried.modelFromWcs, i));
            const double sLo = component(srcLo, i), sHi = component(srcHi, i);
            const double l = component(lo, i), h = component(hi, i);
            if (std::fabs(l - sLo) <= kCarriedSnapMm) {
                out.lo[static_cast<std::size_t>(i)] = toNm(sLo);
            } else {
                out.lo[static_cast<std::size_t>(i)] =
                    floorNm(acrossZ || l < sLo ? l : std::max(sLo, l - carried.cellMm));
            }
            if (std::fabs(h - sHi) <= kCarriedSnapMm) {
                out.hi[static_cast<std::size_t>(i)] = toNm(sHi);
            } else {
                out.hi[static_cast<std::size_t>(i)] =
                    ceilNm(acrossZ || h > sHi ? h : std::min(sHi, h + carried.cellMm));
            }
            if (!(out.lo[static_cast<std::size_t>(i)] < out.hi[static_cast<std::size_t>(i)])) {
                return failure(ErrorCode::InvalidArgument,
                               std::string("The carried stock is flat along ") + "XYZ"[i] + ": no material is left");
            }
        }
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while carrying stock to the next setup");
    }
}

double carriedTopPad(const CarriedStock& carried, const Xform& wcsFromModel)
{
    return isEarlierZ(alongEarlierZ(wcsFromModel, carried.modelFromWcs, 2)) ? 0.0 : carried.cellMm;
}

void shiftSoup(kernel::TriSoup& soup, const Vec3& origin)
{
    for (std::size_t i = 0; i + 2 < soup.positions.size(); i += 3) {
        soup.positions[i] -= origin.x;
        soup.positions[i + 1] -= origin.y;
        soup.positions[i + 2] -= origin.z;
    }
}

namespace {

// Where two filled cells meet only at a corner, the empty cell beside the lower left one is filled (stock.h SLICES).
void fillSaddles(std::vector<std::uint8_t>& mask, std::int64_t nx, std::int64_t ny)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::int64_t j = 0; j + 1 < ny; ++j) {
            for (std::int64_t i = 0; i + 1 < nx; ++i) {
                std::uint8_t& a = mask[static_cast<std::size_t>(j * nx + i)];
                std::uint8_t& b = mask[static_cast<std::size_t>(j * nx + i + 1)];
                const std::uint8_t c = mask[static_cast<std::size_t>((j + 1) * nx + i)];
                const std::uint8_t d = mask[static_cast<std::size_t>((j + 1) * nx + i + 1)];
                if (a != 0 && d != 0 && b == 0 && c == 0) {
                    b = 1;
                    changed = true;
                } else if (b != 0 && c != 0 && a == 0 && d == 0) {
                    a = 1;
                    changed = true;
                }
            }
        }
    }
}

kernel::ProfileCurve line(Vec2 a, Vec2 b)
{
    kernel::ProfileCurve c{};
    c.kind = kernel::ProfileCurve::Kind::Line;
    c.pts = {a, b};
    return c;
}

// The loops of the filled cells of `mask` (no saddles), along the cell edges: filled cells on the left of every edge.
Result<std::vector<kernel::ProfileLoop>> traceLoops(const std::vector<std::uint8_t>& mask, std::int64_t nx,
                                                    std::int64_t ny, const Box3& box, double cx, double cy,
                                                    std::vector<std::int64_t>& next, JobContext& ctx)
{
    const std::int64_t vw = nx + 1;
    next.assign(static_cast<std::size_t>((nx + 1) * (ny + 1)), -1);
    const auto filled = [&](std::int64_t i, std::int64_t j) {
        return i >= 0 && j >= 0 && i < nx && j < ny && mask[static_cast<std::size_t>(j * nx + i)] != 0;
    };
    bool clash = false;
    const auto edge = [&](std::int64_t fi, std::int64_t fj, std::int64_t ti, std::int64_t tj) {
        std::int64_t& slot = next[static_cast<std::size_t>(fj * vw + fi)];
        clash = clash || slot >= 0;
        slot = tj * vw + ti;
    };
    for (std::int64_t j = 0; j < ny; ++j) {
        for (std::int64_t i = 0; i < nx; ++i) {
            if (!filled(i, j)) {
                continue;
            }
            if (!filled(i, j - 1)) {
                edge(i, j, i + 1, j);
            }
            if (!filled(i + 1, j)) {
                edge(i + 1, j, i + 1, j + 1);
            }
            if (!filled(i, j + 1)) {
                edge(i + 1, j + 1, i, j + 1);
            }
            if (!filled(i - 1, j)) {
                edge(i, j + 1, i, j);
            }
        }
    }
    if (clash) {
        return failure(ErrorCode::Internal, "The in-process stock slice has cells that meet at a corner");
    }
    const auto point = [&](std::int64_t v) {
        const std::int64_t i = v % vw, j = v / vw;
        return Vec2{i == nx ? box.max.x : box.min.x + static_cast<double>(i) * cx,
                    j == ny ? box.max.y : box.min.y + static_cast<double>(j) * cy};
    };
    std::vector<kernel::ProfileLoop> out;
    std::vector<std::int64_t> walk;
    for (std::int64_t v = 0; v < static_cast<std::int64_t>(next.size()); ++v) {
        if (next[static_cast<std::size_t>(v)] < 0) {
            continue;
        }
        RAY_CAM_TRY(ctx.cancel.check());
        walk.clear();
        std::int64_t at = v;
        do {
            walk.push_back(at);
            const std::int64_t to = next[static_cast<std::size_t>(at)];
            next[static_cast<std::size_t>(at)] = -1;
            at = to;
        } while (at != v && at >= 0);
        if (at != v) {
            return failure(ErrorCode::Internal, "The in-process stock slice has a loop that does not close");
        }
        // Corners only: the direction changes there. The first vertex (lowest y, then x) is always one.
        const std::size_t n = walk.size();
        std::vector<std::int64_t> corners;
        for (std::size_t k = 0; k < n; ++k) {
            const std::int64_t prev = walk[(k + n - 1) % n], here = walk[k], after = walk[(k + 1) % n];
            if (here - prev != after - here) {
                corners.push_back(here);
            }
        }
        kernel::ProfileLoop loop;
        for (std::size_t k = 0; k < corners.size(); ++k) {
            loop.curves.push_back(line(point(corners[k]), point(corners[(k + 1) % corners.size()])));
        }
        out.push_back(std::move(loop));
    }
    return out;
}

} // namespace

Result<StockSlices> carriedSlices(const kernel::TriSoup& soup, const Box3& box, double cellMm, double topPad,
                                  JobContext& ctx)
{
    try {
        const double ex = box.max.x - box.min.x, ey = box.max.y - box.min.y;
        if (!(cellMm > 0.0) || !(ex > 0.0) || !(ey > 0.0) || !std::isfinite(ex) || !std::isfinite(ey) ||
            !(box.max.z > box.min.z)) {
            return failure(ErrorCode::InvalidArgument, "The in-process stock has no extent to slice");
        }
        const auto cells = [&](double extent) {
            return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(extent / cellMm)));
        };
        const std::int64_t nx = cells(ex), ny = cells(ey);
        if (nx * ny > kMaxGridCells) {
            return failure(ErrorCode::InvalidArgument, "The in-process stock is too large to slice");
        }
        const double cx = ex / static_cast<double>(nx), cy = ey / static_cast<double>(ny);
        constexpr double kNone = -std::numeric_limits<double>::infinity();
        std::vector<double> top(static_cast<std::size_t>(nx * ny), kNone);

        // The highest Z over each cell.
        const std::size_t vertices = soup.positions.size() / 3;
        for (std::size_t t = 0; t + 2 < soup.indices.size(); t += 3) {
            if ((t / 3) % kPollEvery == 0) {
                RAY_CAM_TRY(ctx.cancel.check());
            }
            double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0, z1 = -x0;
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t v = soup.indices[t + static_cast<std::size_t>(k)];
                if (v >= vertices) {
                    ok = false;
                    break;
                }
                const double x = soup.positions[3 * v], y = soup.positions[3 * v + 1], z = soup.positions[3 * v + 2];
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
                z1 = std::max(z1, z);
            }
            if (!ok) {
                return failure(ErrorCode::InvalidArgument, "The in-process stock has a triangle with no vertex");
            }
            const auto first = [&](double v, double origin, double c, std::int64_t n) {
                return std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::floor((v - origin) / c + kCarriedEdgeCells)), 0, n - 1);
            };
            const auto last = [&](double v, double origin, double c, std::int64_t n) {
                return std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::ceil((v - origin) / c - kCarriedEdgeCells)) - 1, -1, n - 1);
            };
            const std::int64_t i0 = first(x0, box.min.x, cx, nx), i1 = last(x1, box.min.x, cx, nx);
            const std::int64_t j0 = first(y0, box.min.y, cy, ny), j1 = last(y1, box.min.y, cy, ny);
            const double z = z1 + topPad;
            for (std::int64_t j = j0; j <= j1; ++j) {
                for (std::int64_t i = i0; i <= i1; ++i) {
                    double& slot = top[static_cast<std::size_t>(j * nx + i)];
                    slot = std::max(slot, z);
                }
            }
        }

        // One cell of growth: the highest top of each 3 x 3 neighbourhood (rows, then columns).
        std::vector<double> rows(top.size(), kNone);
        for (std::int64_t j = 0; j < ny; ++j) {
            for (std::int64_t i = 0; i < nx; ++i) {
                double v = top[static_cast<std::size_t>(j * nx + i)];
                if (i > 0) {
                    v = std::max(v, top[static_cast<std::size_t>(j * nx + i - 1)]);
                }
                if (i + 1 < nx) {
                    v = std::max(v, top[static_cast<std::size_t>(j * nx + i + 1)]);
                }
                rows[static_cast<std::size_t>(j * nx + i)] = v;
            }
        }
        for (std::int64_t j = 0; j < ny; ++j) {
            for (std::int64_t i = 0; i < nx; ++i) {
                double v = rows[static_cast<std::size_t>(j * nx + i)];
                if (j > 0) {
                    v = std::max(v, rows[static_cast<std::size_t>((j - 1) * nx + i)]);
                }
                if (j + 1 < ny) {
                    v = std::max(v, rows[static_cast<std::size_t>((j + 1) * nx + i)]);
                }
                top[static_cast<std::size_t>(j * nx + i)] = v;
            }
        }
        rows = {};

        // The levels.
        std::vector<double> tops;
        for (const double v : top) {
            if (v != kNone) {
                tops.push_back(v);
            }
        }
        std::sort(tops.begin(), tops.end());
        tops.erase(std::unique(tops.begin(), tops.end()), tops.end());
        const double bottom = box.min.z;
        std::vector<double> levels{bottom};
        for (const double t : tops) {
            double level = t;
            if (tops.size() > kCarriedExactLevels) {
                level = bottom + std::floor((t - bottom) / cellMm) * cellMm;
            }
            if (level > levels.back()) {
                levels.push_back(level);
            }
        }

        // The slices.
        StockSlices out;
        std::vector<std::uint8_t> mask(top.size(), 0);
        std::vector<std::int64_t> next;
        for (const double level : levels) {
            RAY_CAM_TRY(ctx.cancel.check());
            for (std::size_t c = 0; c < top.size(); ++c) {
                mask[c] = top[c] > level ? 1 : 0;
            }
            fillSaddles(mask, nx, ny);
            auto loops = traceLoops(mask, nx, ny, box, cx, cy, next, ctx);
            if (!loops) {
                return tl::unexpected(loops.error());
            }
            out.emplace_back(level, std::move(*loops));
        }
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while slicing the in-process stock");
    }
}

} // namespace ray::cam::detail
