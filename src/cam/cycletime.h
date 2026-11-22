// Private to src/cam (and the cross-module cycle agreement test, tests/unit/cycles): how the cycle-time estimate
// (cycletime.cpp, cam.h estimateCycleTime) expands a canned-cycle move into the motions it times.
//
// This is src/cam's own expansion of cam.h "Canned cycles". src/sim (the backplot) and src/post (the post host) have
// theirs; cam may depend on neither (PLAN.md 1.1), so the three are written apart and tests/unit/cycles checks that
// they move the same way, step for step.
#pragma once

#include <ray/cam.h>

#include <algorithm>
#include <cstdint>

namespace ray::cam::cycletime {

struct Point {
    Nm x = 0, y = 0, z = 0;
    bool operator==(const Point&) const = default;
};

enum class StepKind : std::uint8_t { Rapid, Feed, Dwell };

// One motion of an expanded cycle: a rapid or a feed to `to`, or a dwell of `dwellMs` where the tool is.
struct CycleStep {
    StepKind kind = StepKind::Rapid;
    MoveRole role = MoveRole::Link;
    Point to;
    std::int32_t feed = 0;    // um/min, feeds only
    std::int32_t dwellMs = 0; // dwells only
};

// Pecks timed per hole at most; past it, the last peck goes to the bottom (as the backplot does at its own, lower,
// limit). Only an absurd toolpath (a peck of a few nanometres) reaches it.
inline constexpr std::uint32_t kMaxPecks = 1u << 20;

// Coordinates of a cycle are clamped so that their sums and differences cannot overflow (2^61 nm is 2.3e12 mm).
inline Nm clampCycleNm(Nm v)
{
    constexpr Nm kClamp = Nm{1} << 61;
    return std::clamp(v, -kClamp, kClamp);
}

// The motions of cycle move `mv` (a Cycle* kind other than CycleCancel) from the tool's position `from`, as cam.h
// "Canned cycles" defines them, given to emit(const CycleStep&) in order; returns where the tool ends, (x, y, return
// level). Motions of zero length are left out. Roles: the XY rapid Link; the rapids down to the clearance and R and
// the re-entries Approach; feeds into the material Plunge; everything back up Retract (a tap's or bore's feed out
// too). A dwell (cycleDwellMs > 0) is given at the bottom for every kind, after the last feed down. A tap's spindle
// reversal takes no time here (the machine definition has no spindle acceleration).
template <class Emit>
Point expandCycle(const Move& mv, Point from, const lib::MachineDef* machine, Emit&& emit)
{
    Point cur = from;
    const auto step = [&](StepKind kind, MoveRole role, Point to, std::int32_t feed) {
        if (to == cur)
            return;
        CycleStep s;
        s.kind = kind;
        s.role = role;
        s.to = to;
        s.feed = kind == StepKind::Feed ? feed : 0;
        emit(static_cast<const CycleStep&>(s));
        cur = to;
    };
    const Nm x = mv.x, y = mv.y;
    const Nm bottom = clampCycleNm(mv.z);
    const Nm r = clampCycleNm(mv.cycleRetract);
    const Nm clearance = clampCycleNm(mv.cycleClearance);
    const Nm c = clampCycleNm(peckClearanceNm(mv, machine));
    const Nm level = mv.cycleReturn == CycleReturn::Initial ? clearance : r;
    const std::int32_t feed = mv.feedMicronPerMin;
    const auto dwell = [&] {
        if (mv.cycleDwellMs > 0) {
            CycleStep s;
            s.kind = StepKind::Dwell;
            s.role = MoveRole::Plunge;
            s.to = cur;
            s.dwellMs = mv.cycleDwellMs;
            emit(static_cast<const CycleStep&>(s));
        }
    };
    // 1 to 3: over the hole at the current height, down to the clearance if above it, then to R.
    step(StepKind::Rapid, MoveRole::Link, {x, y, cur.z}, 0);
    if (cur.z > clearance)
        step(StepKind::Rapid, MoveRole::Approach, {x, y, clearance}, 0);
    step(StepKind::Rapid, MoveRole::Approach, {x, y, r}, 0);
    // 4: by kind.
    const bool pecking = mv.kind == MoveKind::CyclePeck || mv.kind == MoveKind::CycleChipBreak;
    const Nm q = clampCycleNm(mv.cyclePeck);
    if (pecking && q > 0 && bottom < r) {
        const bool g83 = mv.kind == MoveKind::CyclePeck; // G83 goes back to R after each peck; G73 backs off by c
        Nm reached = r;                                  // the depth drilled so far
        for (std::uint32_t k = 1;; ++k) {
            const Nm next = (reached - bottom <= q || k >= kMaxPecks) ? bottom : reached - q;
            if (g83 && k > 1)
                step(StepKind::Rapid, MoveRole::Approach, {x, y, std::min(r, reached + c)}, 0);
            step(StepKind::Feed, MoveRole::Plunge, {x, y, next}, feed);
            if (next == bottom)
                break;
            step(StepKind::Rapid, MoveRole::Retract, {x, y, g83 ? r : std::min(r, next + c)}, 0);
            reached = next;
        }
        dwell();
        step(StepKind::Rapid, MoveRole::Retract, {x, y, level}, 0);
        return cur;
    }
    step(StepKind::Feed, MoveRole::Plunge, {x, y, bottom}, feed);
    dwell();
    if (mv.kind == MoveKind::CycleTap || mv.kind == MoveKind::CycleBore || mv.kind == MoveKind::CycleReam)
        step(StepKind::Feed, MoveRole::Retract, {x, y, r}, feed);
    step(StepKind::Rapid, MoveRole::Retract, {x, y, level}, 0);
    return cur;
}

inline bool isCycleMotion(MoveKind k)
{
    switch (k) {
    case MoveKind::CycleDrill:
    case MoveKind::CyclePeck:
    case MoveKind::CycleChipBreak:
    case MoveKind::CycleTap:
    case MoveKind::CycleBore:
    case MoveKind::CycleReam:
        return true;
    default:
        return false;
    }
}

} // namespace ray::cam::cycletime
