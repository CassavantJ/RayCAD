// Helpers of the stock model tests (task T-070): a job system, tools, stocks and the S6-like benchmark workload.
#pragma once

#include "toolshape.h"
#include "tridexel.h"

#include <ray/core.h>
#include <ray/libraries.h>
#include <ray/sim.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace ray::sim::test {

constexpr double kPi = 3.14159265358979323846;

class QueueDispatcher final : public IUiDispatcher {
public:
    void post(std::function<void()> f) override { queue_.push_back(std::move(f)); }

private:
    std::deque<std::function<void()>> queue_;
};

// One job system for the whole test process (oneTBB's worker limit is process-wide).
inline IJobSystem& jobs()
{
    static QueueDispatcher dispatcher;
    static std::unique_ptr<IJobSystem> js = makeJobSystem(dispatcher, 8);
    return *js;
}

inline lib::Tool tool(lib::ToolType type, double diameter, double fluteLength)
{
    lib::Tool t;
    t.type = type;
    t.diameter = diameter;
    t.fluteLength = fluteLength;
    t.shankDiameter = diameter;
    t.overallLength = fluteLength + 40;
    return t;
}
inline ToolShape flat(double diameter, double fluteLength = 40)
{
    return makeToolShape(tool(lib::ToolType::FlatEndMill, diameter, fluteLength), nullptr, 0);
}
inline ToolShape ball(double diameter, double fluteLength = 40)
{
    return makeToolShape(tool(lib::ToolType::BallEndMill, diameter, fluteLength), nullptr, 0);
}
inline ToolShape bull(double diameter, double corner, double fluteLength = 40)
{
    lib::Tool t = tool(lib::ToolType::BullEndMill, diameter, fluteLength);
    t.cornerRadius = corner;
    return makeToolShape(t, nullptr, 0);
}
inline ToolShape pointed(lib::ToolType type, double diameter, double angleDeg, double tipDiameter,
                         double fluteLength = 40)
{
    lib::Tool t = tool(type, diameter, fluteLength);
    t.tipAngleDeg = angleDeg;
    t.tipDiameter = tipDiameter;
    return makeToolShape(t, nullptr, 0);
}

inline std::unique_ptr<IStockModel> blockStock(const Vec3& min, const Vec3& max, double cell)
{
    std::unique_ptr<IStockModel> m = makeTriDexelCpu(jobs());
    m->reset({}, Box3{min, max}, StockResolution{cell});
    return m;
}

// Material volume per grid (Z, X, Y).
struct Volumes {
    double v[3];
};
inline Volumes volumes(const IStockModel& m)
{
    const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(m);
    return {{in.volume[0], in.volume[1], in.volume[2]}};
}

// The S6 benchmark: a 6 x 4 x 1 in stock and a workload of facing, an adaptive-like pocket and ball-end scallops
// (spike S6's description, rebuilt here), all linear moves: 112,092 of them with the defaults. Fewer pocket `lanes`
// and a wider scallop `stepover` thin it out for quick tests. Trigonometry goes through ray::dmath, so the moves, and
// the golden stock hash, are the same on every platform.
struct Workload {
    struct Pass {
        int tool; // 0 flat 12.7, 1 ball 6.35
        std::vector<IStockModel::CutMove> moves;
    };
    std::vector<Pass> passes;
    std::size_t moves() const
    {
        std::size_t n = 0;
        for (const Pass& p : passes) {
            n += p.moves.size();
        }
        return n;
    }
};
inline Box3 benchmarkStock()
{
    return {{0, 0, 0}, {152.4, 101.6, 25.4}};
}
inline Workload benchmarkWorkload(int lanes = 11, double stepover = 0.25)
{
    Workload w;
    std::uint32_t lcg = 12345u;
    const auto next = [&] {
        lcg = lcg * 1664525u + 1013904223u;
        return (lcg >> 8) * (1.0 / 16777216.0);
    };
    const auto seg = [&] {
        return 0.5 + 1.5 * next();
    };
    Vec3 cur{};
    bool have = false;
    Workload::Pass* pass = nullptr;
    const auto jump = [&](double x, double y, double z) {
        cur = {x, y, z};
        have = true;
    };
    const auto to = [&](double x, double y, double z) {
        if (have) {
            pass->moves.push_back({cur, {x, y, z}, false});
        }
        cur = {x, y, z};
        have = true;
    };
    // Facing, flat 12.7, 1 mm deep, 60 % stepover.
    w.passes.push_back({0, {}});
    pass = &w.passes.back();
    {
        const double z = 24.4;
        bool fwd = true;
        jump(-8, 2.0, z);
        for (double y = 2.0; y < 101.6; y += 7.62) {
            to(fwd ? -8 : 160.4, y, z);
            to(fwd ? 160.4 : -8, y, z);
            fwd = !fwd;
        }
    }
    // Adaptive-like pocket 90 x 80 mm, six 3 mm levels, trochoidal loops of radius 3, a helical ramp per level.
    {
        const double rho = 3.0, adv = 0.95, R = 6.35;
        const double x0 = 10 + R + rho, x1 = 100 - R - rho, y0 = 10 + R + rho, y1 = 90 - R - rho;
        std::vector<std::pair<double, double>> spine;
        bool fwd = true;
        for (int l = 0; l < lanes; ++l) {
            const double y = lanes > 1 ? y0 + (y1 - y0) * l / (lanes - 1) : y0;
            spine.push_back({fwd ? x0 : x1, y});
            spine.push_back({fwd ? x1 : x0, y});
            fwd = !fwd;
        }
        for (int level = 1; level <= 6; ++level) {
            const double z = 24.4 - 3.0 * level;
            double th = 0;
            const double cx = spine[0].first, cy = spine[0].second;
            jump(cx + rho, cy, z + 3.0);
            const double helixLen = 2 * 2 * kPi * rho;
            double done = 0;
            while (done < helixLen) {
                done = std::min(helixLen, done + seg());
                th = done / rho;
                to(cx + rho * dmath::cos(th), cy + rho * dmath::sin(th), z + 3.0 * (1 - done / helixLen));
            }
            for (std::size_t k = 0; k + 1 < spine.size(); ++k) {
                const double ax = spine[k].first, ay = spine[k].second, bx = spine[k + 1].first,
                             by = spine[k + 1].second;
                const double len = dmath::hypot(bx - ax, by - ay);
                double sdone = 0;
                while (sdone < len) {
                    const double sl = seg();
                    th += sl / rho;
                    sdone = std::min(len, sdone + adv * sl / (2 * kPi * rho));
                    const double f = sdone / len;
                    to(ax + (bx - ax) * f + rho * dmath::cos(th), ay + (by - ay) * f + rho * dmath::sin(th), z);
                }
            }
        }
    }
    // Ball 6.35 scallops over two wavy surfaces.
    w.passes.push_back({1, {}});
    pass = &w.passes.back();
    have = false;
    {
        bool fwd = true;
        const auto zf = [](double x, double y) {
            return 22.9 + 1.5 * dmath::sin(2 * kPi * x / 38.0) * dmath::cos(2 * kPi * y / 45.0);
        };
        for (double x = 108.0; x <= 146.0; x += stepover) {
            double y = fwd ? 6.0 : 96.0;
            to(x, y, zf(x, y));
            for (;;) {
                const double sl = seg();
                y += fwd ? sl : -sl;
                const bool end = fwd ? y >= 96.0 : y <= 6.0;
                if (end) {
                    y = fwd ? 96.0 : 6.0;
                }
                to(x, y, zf(x, y));
                if (end) {
                    break;
                }
            }
            fwd = !fwd;
        }
    }
    have = false;
    {
        bool fwd = true;
        const auto zf = [](double x, double y) {
            return 5.6 + 0.6 * dmath::sin(2 * kPi * x / 30.0) * dmath::cos(2 * kPi * y / 25.0);
        };
        const double xa = 13.4, xb = 96.6;
        for (double y = 13.4; y <= 86.6; y += stepover) {
            double x = fwd ? xa : xb;
            to(x, y, zf(x, y));
            for (;;) {
                const double sl = seg();
                x += fwd ? sl : -sl;
                const bool end = fwd ? x >= xb : x <= xa;
                if (end) {
                    x = fwd ? xb : xa;
                }
                to(x, y, zf(x, y));
                if (end) {
                    break;
                }
            }
            fwd = !fwd;
        }
    }
    return w;
}
inline const ToolShape& benchmarkTool(int i)
{
    static const ToolShape tools[2] = {flat(12.7, 30.0), ball(6.35, 25.0)};
    return tools[i];
}

// Runs the workload in batches of `batch` moves; returns the total removed volume.
inline double runWorkload(IStockModel& m, const Workload& w, std::size_t batch)
{
    double sum = 0;
    std::vector<double> removed;
    for (const Workload::Pass& p : w.passes) {
        for (std::size_t i = 0; i < p.moves.size(); i += batch) {
            const std::size_t n = std::min(batch, p.moves.size() - i);
            removed.assign(n, 0.0);
            m.cutBatch(benchmarkTool(p.tool), std::span<const IStockModel::CutMove>(p.moves.data() + i, n), removed);
            for (double v : removed) {
                sum += v;
            }
        }
    }
    return sum;
}

} // namespace ray::sim::test
