// Toolpath invariant checker, canned-cycle part (the canned-cycle settlement of cam.h). Test-only code, written from
// cam.h's text and sharing nothing with src/cam, src/sim or src/post. T-058 builds the full invariant checker in this
// folder (PLAN.md section 11); T-035's acceptance ("invariant checker passes") runs its drill toolpaths through this.
//
// checkCycles(path, rpm) returns every broken rule, in move order (empty: the toolpath keeps them all):
//   pad          a Move's pad0 or pad1 is not 0 (any move)
//   feed         a cycle without a feed
//   levels       not  z < cycleTop <= cycleRetract <= cycleClearance
//   return       cycleReturn is neither RPlane nor Initial
//   peck         cyclePeck < 0, or a peck depth on a kind that does not peck (only CyclePeck, CycleChipBreak)
//   peckClear    cyclePeckClearance < 0
//   dwell        cycleDwellMs < 0
//   leftHand     cycleLeftHand other than 0 or 1, or 1 on a kind other than CycleTap
//   pitch        CycleTap without a positive threadPitchNm, or a pitch on another kind
//   tapFeed      CycleTap whose feed is not threadPitchNm * rpm / 1000 (rounded half to even), checked when `rpm` is
//                given; a tap needs a whole-number rpm
//   runStart     the first move of a run (consecutive cycle moves of one kind, cycleLeftHand and cycleClearance) with
//                the tool below its cycleClearance
// The tool position follows cam.h: the end point of a plain move, (x, y, return level) after a cycle, unchanged by
// Dwell and CycleCancel.
#pragma once

#include <ray/cam.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ray::cam::invariants {

struct Violation {
    std::size_t move;
    std::string rule;
    bool operator==(const Violation&) const = default;
};

inline bool isCycle(MoveKind k)
{
    return k == MoveKind::CycleDrill || k == MoveKind::CyclePeck || k == MoveKind::CycleChipBreak ||
           k == MoveKind::CycleTap || k == MoveKind::CycleBore || k == MoveKind::CycleReam;
}

// a / b rounded half to even, b > 0.
inline std::int64_t divHalfEven(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    std::int64_t r = a % b;
    if (r < 0) {
        r += b;
        --q;
    }
    if (2 * r > b || (2 * r == b && (q % 2) != 0)) {
        ++q;
    }
    return q;
}

inline std::vector<Violation> checkCycles(const Toolpath& path, std::optional<double> rpm = std::nullopt)
{
    std::vector<Violation> out;
    Nm z = path.startZ;
    struct Run {
        MoveKind kind;
        std::uint8_t leftHand;
        Nm clearance;
        bool operator==(const Run&) const = default;
    };
    std::optional<Run> previous;
    for (std::size_t n = 0; n < path.moves.size(); ++n) {
        const Move& m = path.moves[n];
        const auto fail = [&](const char* rule) {
            out.push_back({n, rule});
        };
        if (m.pad0 != 0 || m.pad1 != 0) {
            fail("pad");
        }
        if (!isCycle(m.kind)) {
            previous.reset();
            if (m.kind != MoveKind::Dwell && m.kind != MoveKind::CycleCancel) {
                z = m.z;
            }
            continue;
        }
        if (m.feedMicronPerMin <= 0) {
            fail("feed");
        }
        if (!(m.z < m.cycleTop && m.cycleTop <= m.cycleRetract && m.cycleRetract <= m.cycleClearance)) {
            fail("levels");
        }
        const bool initial = m.cycleReturn == CycleReturn::Initial;
        if (!initial && m.cycleReturn != CycleReturn::RPlane) {
            fail("return");
        }
        const bool pecks = m.kind == MoveKind::CyclePeck || m.kind == MoveKind::CycleChipBreak;
        if (m.cyclePeck < 0 || (!pecks && m.cyclePeck != 0)) {
            fail("peck");
        }
        if (m.cyclePeckClearance < 0) {
            fail("peckClear");
        }
        if (m.cycleDwellMs < 0) {
            fail("dwell");
        }
        const bool tap = m.kind == MoveKind::CycleTap;
        if (m.cycleLeftHand > 1 || (!tap && m.cycleLeftHand != 0)) {
            fail("leftHand");
        }
        if (tap ? m.threadPitchNm <= 0 : m.threadPitchNm != 0) {
            fail("pitch");
        }
        if (tap && rpm && m.threadPitchNm > 0) {
            const double s = *rpm;
            const bool whole = s > 0 && s < 1e7 && std::floor(s) == s;
            if (!whole ||
                m.feedMicronPerMin !=
                    divHalfEven(static_cast<std::int64_t>(m.threadPitchNm) * static_cast<std::int64_t>(s), 1000)) {
                fail("tapFeed");
            }
        }
        const Run run{m.kind, m.cycleLeftHand, m.cycleClearance};
        if ((!previous || !(*previous == run)) && z < m.cycleClearance) {
            fail("runStart");
        }
        previous = run;
        z = initial ? m.cycleClearance : m.cycleRetract;
    }
    return out;
}

} // namespace ray::cam::invariants
