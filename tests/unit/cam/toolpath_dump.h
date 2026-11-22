// A readable text form of a toolpath (task T-035), test-only: a .rctp golden (Toolpath::serialize) is binary, so each
// one is committed next to its dump (<name>.txt), and the golden test checks that the two agree and that a freshly
// generated toolpath gives both. A reviewer reads the .txt.
//
// Format, one item per line, every length in mm printed exactly from integer nanometres (up to 6 decimals, trailing
// zeros dropped), feeds in mm/min exactly from um/min (up to 3 decimals):
//   RCTP 2
//   operation <uuid>            input <64 hex digits>
//   start X.. Y.. Z..
//   moves <n>
//   <index> <kind> <role> X.. Y.. Z.. [F..] [I.. J.. K.. plane P] [comp N]                 plain moves
//   <index> <kind> <role> X.. Y.. Z.. F.. R.. top.. clear.. return RPlane|Initial [peck.. peckClear..] [dwell N ms]
//          [pitch.. ] [left-hand]                                                          cycle moves
#pragma once

#include <ray/cam.h>

#include <cstdint>
#include <string>

namespace ray::cam::test {

// `v` / `scale` exactly, with up to `decimals` decimals and trailing zeros dropped.
inline std::string fixedText(std::int64_t v, std::int64_t scale, int decimals)
{
    const bool negative = v < 0;
    const std::uint64_t a = negative ? static_cast<std::uint64_t>(-(v + 1)) + 1 : static_cast<std::uint64_t>(v);
    std::string out = (negative ? "-" : "") + std::to_string(a / static_cast<std::uint64_t>(scale));
    std::string frac = std::to_string(a % static_cast<std::uint64_t>(scale));
    frac.insert(0, static_cast<std::size_t>(decimals) - frac.size(), '0');
    while (!frac.empty() && frac.back() == '0') {
        frac.pop_back();
    }
    return frac.empty() ? out : out + "." + frac;
}
inline std::string mmOf(Nm v)
{
    return fixedText(v, 1'000'000, 6);
}
inline std::string feedOf(std::int32_t umPerMin)
{
    return fixedText(umPerMin, 1000, 3);
}

inline const char* kindName(MoveKind k)
{
    static const char* const names[] = {"Rapid",    "Linear",     "ArcCW",     "ArcCCW",
                                        "Dwell",    "CycleDrill", "CyclePeck", "CycleChipBreak",
                                        "CycleTap", "CycleBore",  "CycleReam", "CycleCancel"};
    const auto i = static_cast<std::size_t>(k);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}
inline const char* roleName(MoveRole r)
{
    static const char* const names[] = {"Cut", "Plunge", "Ramp", "LeadIn", "LeadOut", "Link", "Retract", "Approach"};
    const auto i = static_cast<std::size_t>(r);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

inline bool isCycleMove(MoveKind k)
{
    return k == MoveKind::CycleDrill || k == MoveKind::CyclePeck || k == MoveKind::CycleChipBreak ||
           k == MoveKind::CycleTap || k == MoveKind::CycleBore || k == MoveKind::CycleReam;
}

inline std::string dumpToolpath(const Toolpath& path)
{
    std::string s = "RCTP " + std::to_string(kToolpathFormatVersion) + "\n";
    s += "operation " + path.operation.value.toString() + " input " + path.inputHash.hex() + "\n";
    s += "start X" + mmOf(path.startX) + " Y" + mmOf(path.startY) + " Z" + mmOf(path.startZ) + "\n";
    s += "moves " + std::to_string(path.moves.size()) + "\n";
    for (std::size_t n = 0; n < path.moves.size(); ++n) {
        const Move& m = path.moves[n];
        s += std::to_string(n) + " " + kindName(m.kind) + " " + roleName(m.role);
        if (m.kind == MoveKind::Dwell) {
            s += " dwell " + std::to_string(m.cycleDwellMs) + " ms\n";
            continue;
        }
        s += " X" + mmOf(m.x) + " Y" + mmOf(m.y) + " Z" + mmOf(m.z);
        if (m.feedMicronPerMin != 0) {
            s += " F" + feedOf(m.feedMicronPerMin);
        }
        if (isCycleMove(m.kind)) {
            s += " R" + mmOf(m.cycleRetract) + " top" + mmOf(m.cycleTop) + " clear" + mmOf(m.cycleClearance);
            s += m.cycleReturn == CycleReturn::Initial ? " return Initial" : " return RPlane";
            if (m.cyclePeck != 0 || m.cyclePeckClearance != 0) {
                s += " peck" + mmOf(m.cyclePeck) + " peckClear" + mmOf(m.cyclePeckClearance);
            }
            if (m.cycleDwellMs != 0) {
                s += " dwell " + std::to_string(m.cycleDwellMs) + " ms";
            }
            if (m.threadPitchNm != 0) {
                s += " pitch" + mmOf(m.threadPitchNm);
            }
            if (m.cycleLeftHand != 0) {
                s += " left-hand";
            }
        } else if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
            static const char* const planes[] = {"XY", "XZ", "YZ"};
            s += " I" + mmOf(m.i) + " J" + mmOf(m.j) + " K" + mmOf(m.k) + " plane " +
                 planes[static_cast<std::size_t>(m.plane) % 3];
        }
        if (m.compensation != 0) {
            s += " comp " + std::to_string(m.compensation);
        }
        s += "\n";
    }
    return s;
}

} // namespace ray::cam::test
