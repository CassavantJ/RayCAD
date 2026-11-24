// Helpers of the simulator tests (task T-072): SimPrograms from the checks' test programs, curved part soups, a
// reference stock cut without the simulator, and a closed-solid check.
#pragma once

#include "simulator.h"
#include "verify_test_util.h"

#include <ray/cam.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ray::sim::test {

// A Program (verify_test_util.h) as the SimProgram the simulator takes. Keeps the cam::Operations it points at.
struct SimFixture {
    Program prog;
    std::vector<cam::Operation> camOps;
    std::vector<const lib::Tool*> toolDefs;
    SimProgram sp{};
    Xform modelFromWcs = Xform::identity();

    SimProgram& build()
    {
        prog.input(); // fills each op's ToolShape
        camOps.clear();
        for (const Program::Op& o : prog.ops) {
            cam::Operation op;
            op.id = o.id;
            camOps.push_back(op);
        }
        sp = SimProgram{};
        sp.setup = nullptr;
        for (std::size_t k = 0; k < prog.ops.size(); ++k) {
            sp.ops.push_back(&camOps[k]);
            sp.toolpaths.push_back(prog.ops[k].path);
            sp.tools.push_back(prog.ops[k].shape);
            sp.toolDefs.push_back(&prog.ops[k].tool);
        }
        sp.partSolid = prog.part ? *prog.part : kernel::TriSoup{};
        sp.stockSolid = prog.stock ? *prog.stock : kernel::TriSoup{};
        sp.fixtureSolids = prog.fixtures;
        sp.modelFromWcs = modelFromWcs;
        sp.machine = prog.machine ? &*prog.machine : nullptr;
        return sp;
    }
};

// A closed soup of a tube about the Z axis through (cx, cy): radii ri (0: a solid cylinder) and ro, from z0 to z1, with
// `n` sides whose corners lie on the circles (so the soup lies inside the outer circle and outside the inner one, by at
// most the sagitta).
inline kernel::TriSoup tubeSoup(double cx, double cy, double ri, double ro, double z0, double z1, int n)
{
    kernel::TriSoup s;
    const auto vertex = [&](double r, double a, double z) {
        s.positions.push_back(cx + r * dmath::cos(a));
        s.positions.push_back(cy + r * dmath::sin(a));
        s.positions.push_back(z);
        return static_cast<std::uint32_t>(s.positions.size() / 3 - 1);
    };
    const auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        s.indices.insert(s.indices.end(), {a, b, c});
    };
    std::vector<std::uint32_t> o0, o1, i0, i1;
    for (int k = 0; k < n; ++k) {
        const double a = 2.0 * kPi * k / n;
        o0.push_back(vertex(ro, a, z0));
        o1.push_back(vertex(ro, a, z1));
        if (ri > 0) {
            i0.push_back(vertex(ri, a, z0));
            i1.push_back(vertex(ri, a, z1));
        }
    }
    const std::uint32_t c0 = ri > 0 ? 0 : vertex(0, 0, z0);
    const std::uint32_t c1 = ri > 0 ? 0 : vertex(0, 0, z1);
    for (int k = 0; k < n; ++k) {
        const int l = (k + 1) % n;
        // Outer wall, outward.
        tri(o0[k], o0[l], o1[l]);
        tri(o0[k], o1[l], o1[k]);
        if (ri > 0) {
            // Inner wall, facing the axis.
            tri(i0[k], i1[l], i0[l]);
            tri(i0[k], i1[k], i1[l]);
            // Bottom and top rings.
            tri(o0[k], i0[l], o0[l]);
            tri(o0[k], i0[k], i0[l]);
            tri(o1[k], o1[l], i1[l]);
            tri(o1[k], i1[l], i1[k]);
        } else {
            tri(c0, o0[l], o0[k]);
            tri(c1, o1[k], o1[l]);
        }
    }
    return s;
}

// Sides for a circle of radius r whose chords stay within `sagitta` of it.
inline int sidesFor(double r, double sagitta)
{
    const double half = dmath::acos(1.0 - sagitta / r);
    return static_cast<int>(std::ceil(kPi / half));
}

// Two closed soups as one (their union by nonzero winding when they overlap).
inline kernel::TriSoup joined(const kernel::TriSoup& a, const kernel::TriSoup& b)
{
    kernel::TriSoup s = a;
    const auto base = static_cast<std::uint32_t>(a.positions.size() / 3);
    s.positions.insert(s.positions.end(), b.positions.begin(), b.positions.end());
    for (std::uint32_t i : b.indices) {
        s.indices.push_back(base + i);
    }
    return s;
}

// The stock at a timeline position, cut WITHOUT the simulator: the program's sub-motions (the same expansion), whole
// up to the position as one cutBatch per operation, then the part of the last one with cut(). Independent of the
// simulator's keyframes, batches and seek.
inline std::uint64_t referenceHash(const SimProgram& sp, double cell, TimelinePosition pos)
{
    const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
    const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
    Box3 b{{sp.stockSolid.positions[0], sp.stockSolid.positions[1], sp.stockSolid.positions[2]},
           {sp.stockSolid.positions[0], sp.stockSolid.positions[1], sp.stockSolid.positions[2]}};
    for (std::size_t i = 0; i + 2 < sp.stockSolid.positions.size(); i += 3) {
        b.min = {std::min(b.min.x, sp.stockSolid.positions[i]), std::min(b.min.y, sp.stockSolid.positions[i + 1]),
                 std::min(b.min.z, sp.stockSolid.positions[i + 2])};
        b.max = {std::max(b.max.x, sp.stockSolid.positions[i]), std::max(b.max.y, sp.stockSolid.positions[i + 1]),
                 std::max(b.max.z, sp.stockSolid.positions[i + 2])};
    }
    std::unique_ptr<IStockModel> m = makeTriDexelCpu(jobs());
    m->reset(sp.stockSolid, b, StockResolution{cell});
    // Which sub-motions are whole and which is partly cut: every one of an earlier operation or move; of the move, by
    // length along it.
    std::size_t whole = 0;
    double u = 0;
    for (; whole < motions.size(); ++whole) {
        const sim::detail::Motion& mo = motions[whole];
        if (mo.op < pos.opIndex || (mo.op == pos.opIndex && mo.move < pos.moveIndex)) {
            continue;
        }
        if (mo.op > pos.opIndex || mo.move > pos.moveIndex) {
            break;
        }
        // Motions of the move itself.
        double total = 0;
        std::size_t end = whole;
        while (end < motions.size() && motions[end].op == pos.opIndex && motions[end].move == pos.moveIndex) {
            const Vec3 d{motions[end].b.x - motions[end].a.x, motions[end].b.y - motions[end].a.y,
                         motions[end].b.z - motions[end].a.z};
            total += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            ++end;
        }
        const double target = std::clamp(pos.t01, 0.0, 1.0) * total;
        double cum = 0;
        std::size_t k = whole;
        for (; k < end; ++k) {
            const Vec3 d{motions[k].b.x - motions[k].a.x, motions[k].b.y - motions[k].a.y,
                         motions[k].b.z - motions[k].a.z};
            const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (cum + len > target) {
                u = len > 0 ? (target - cum) / len : 0.0;
                if (!(u < 1.0)) {
                    ++k;
                    u = 0;
                }
                break;
            }
            cum += len;
        }
        whole = k;
        break;
    }
    std::size_t i = 0;
    while (i < whole) {
        const std::uint32_t op = motions[i].op;
        std::vector<IStockModel::CutMove> moves;
        while (i < whole && motions[i].op == op) {
            moves.push_back({motions[i].a, motions[i].b, motions[i].rapid});
            ++i;
        }
        m->cutBatch(sp.tools[op], moves, {});
    }
    if (u > 0 && whole < motions.size()) {
        const sim::detail::Motion& mo = motions[whole];
        const Vec3 e{mo.a.x + (mo.b.x - mo.a.x) * u, mo.a.y + (mo.b.y - mo.a.y) * u, mo.a.z + (mo.b.z - mo.a.z) * u};
        m->cut(sp.tools[mo.op], mo.a, e, mo.rapid);
    }
    return sim::detail::triDexelHash(*m);
}

// A soup is a closed, welded, outward solid: every directed edge is met once and its reverse once, and the enclosed
// volume is positive. Returns the volume.
inline double closedSolidVolume(const kernel::TriSoup& s)
{
    REQUIRE(!s.indices.empty());
    REQUIRE(s.indices.size() % 3 == 0);
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
    double volume = 0;
    for (std::size_t t = 0; t < s.indices.size(); t += 3) {
        const std::uint32_t v[3] = {s.indices[t], s.indices[t + 1], s.indices[t + 2]};
        for (int k = 0; k < 3; ++k) {
            ++edges[{v[k], v[(k + 1) % 3]}];
        }
        const auto p = [&](int k) {
            return Vec3{s.positions[3 * v[k]], s.positions[3 * v[k] + 1], s.positions[3 * v[k] + 2]};
        };
        const Vec3 a = p(0), b = p(1), c = p(2);
        volume += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
    }
    std::size_t unpaired = 0;
    for (const auto& [e, n] : edges) {
        const auto rev = edges.find({e.second, e.first});
        if (n != 1 || rev == edges.end() || rev->second != 1) {
            ++unpaired;
        }
    }
    CHECK(unpaired == 0);
    CHECK(volume > 0);
    return volume;
}

// Every field of every event, as text (exact doubles), for comparing runs.
inline std::string eventsText(std::span<const SimEvent> events)
{
    std::string s;
    for (const SimEvent& e : events) {
        const auto bits = [](double d) {
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            return std::to_string(u);
        };
        s += kindName(e.kind) + " " + std::to_string(static_cast<int>(e.severity)) + " " +
             std::to_string(e.operation.value.bytes[0]) + " " + std::to_string(e.moveIndex) + " " + bits(e.position.x) +
             " " + bits(e.position.y) + " " + bits(e.position.z) + " " + bits(e.amountMm) + " " + e.message + "\n";
    }
    return s;
}

} // namespace ray::sim::test
