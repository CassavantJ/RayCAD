// A test-only G-code interpreter (PLAN.md T-043): it reads a posted program back into machine motion, so that a
// mistake in a post or in the post host shows up as a disagreement with the toolpath the program was posted from.
//
// INDEPENDENCE. This file and gcode_interp.cpp use the C++ standard library only: nothing from src/post (no writer, no
// number formatting, no arc or cycle expansion), nothing from include/ray. The rules come from the control
// documentation, never from the posts' scripts:
//   [N]   T. Kramer, F. Proctor, E. Messina, "The NIST RS274NGC Interpreter - Version 3", NISTIR 6556, NIST, 2000
//         (public domain), cited by section, e.g. [N 3.5.16.4].
//   [H1]  Haas Automation, "Mill Series Programming Workbook", 2015, by page, as cited in docs/post_review/haas-ngc.md
//         (this author has not read H1: the rule is taken from that document's citation).
//   [F1]  FANUC operator's manuals: not openly published and not read (docs/post_review/fanuc.md). Every FANUC rule
//         here is an ASSUMPTION of common FANUC behaviour and is marked so.
//   [ASSUMPTION] marks every rule no read document fixes. The report of T-043 lists them.
// The interpreter REJECTS (an error naming the line) every word, code and form it does not know, rather than guess.
//
// Units: lengths in the trace are millimetres in the active work coordinate system, times in seconds, feeds in
// mm/min. An axis the program has sent to machine zero or the reference position (G28, G53) is not known in work
// coordinates: X and Y are then NaN, Z is kHomeZ (machine Z zero is the top of travel on a vertical machining centre,
// [ASSUMPTION] stated in docs/post_review/generic-iso.md note 2 and H1 p.43).
#pragma once
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace rtgc {

inline constexpr double kHomeZ = 1.0e9; // Z at machine zero / the reference position: above everything
inline const double kUnknown = std::numeric_limits<double>::quiet_NaN();

// The control families the four shipped posts are written for.
enum class Family {
    Ngc,   // RS274/NGC as [N] describes it (the generic ISO post's target)
    Fanuc, // FANUC 0i-MF / 30i-MB class [F1, ASSUMPTION throughout]
    Haas,  // Haas mill controls [H1]
};

enum CoolantBit : unsigned { kFlood = 1, kMist = 2, kThrough = 4, kAir = 8 };
struct CoolantCode {
    unsigned on = 0;  // modes this M code turns on
    unsigned off = 0; // modes this M code turns off
};

struct Dialect {
    std::string name;
    Family family = Family::Ngc;
    // Haas classic control: H1 p.17 lists G187 with E only, so P on G187 is not known for it (haas-classic.md).
    bool haasClassic = false;
    // NGC only: the target control reads the G4 / G82 / G89 P word as integer milliseconds instead of [N 3.5.4]
    // seconds (the generic post's property dwellUnits = milliseconds describes such a control).
    bool ngcDwellMilliseconds = false;
    // The coolant M codes, from the shop's coolant code list (the post property coolantCodes, "flood=M8/M9 ...").
    std::map<int, CoolantCode> coolant;
    // Control parameters the program cannot carry: the G73 back-off and the G83 re-entry clearance (FANUC parameters
    // 5114 / 5115 [F1, ASSUMPTION], Haas Setting 22 [H1 p.77, p.95 per haas-ngc.md], RS274/NGC "backed off a bit"
    // [N 3.5.16.4]). The test sets them to what the machine is assumed to be set to.
    double peckClearanceMm = 0.5;
};

// Parses "flood=M8/M9 mist=M7/M9 through=M88/M89 air=M83/M84" (an empty code, "through=/", means none). Returns false
// on a malformed list.
bool parseCoolantCodes(const std::string& list, std::map<int, CoolantCode>& out);

struct P3 {
    double x = kUnknown, y = kUnknown, z = kUnknown;
};

struct Modal {
    bool unitsKnown = false;
    bool inch = false;
    double rpm = 0;          // the S word
    int spindle = 0;         // +1 clockwise, -1 counter-clockwise, 0 stopped
    int tool = -1;           // the tool in the spindle (after M6), -1 unknown
    int lengthH = -1;        // G43 with this H; -1: G49 (no length offset)
    int comp = 0;            // 0 G40, 1 G41 (left), 2 G42 (right)
    int dNumber = -1;        // the D word of the active G41/G42
    unsigned coolant = 0;    // CoolantBit set
    std::string wcs = "G54"; // the active work offset as written, "G54", "G54.1 P3", "G110", "G154 P5", "G59.1"
    bool rigidTap = false;   // FANUC M29 given and no G80 since
};

enum class SegKind { Rapid, Feed, Arc, Dwell };
enum class SegOrigin {
    Program,   // a G0 / G1 / G2 / G3 / G4 block
    Cycle,     // part of a canned cycle's motion
    Reference, // G28: to the intermediate point and on to the reference position
    Machine,   // G53: machine coordinates
};

struct Segment {
    SegKind kind = SegKind::Rapid;
    SegOrigin origin = SegOrigin::Program;
    P3 from, to;
    // Arcs: the centre (in-plane coordinates meaningful), plane 17/18/19, direction, sweep in radians (> 0, 2 pi for a
    // full circle), whether written with R.
    P3 centre;
    int plane = 17;
    bool cw = false;
    double sweep = 0;
    bool rFormat = false;
    double feed = 0;          // mm/min for Feed and Arc
    double dwell = 0;         // seconds for Dwell
    Modal state;              // the modal state while this segment runs
    int line = 0;             // 1-based line of the block that made it
    int cycleCode = 0;        // G code x 10 of the canned cycle (810 ...), 0 outside cycles
    bool cycleReturn = false; // the final retract of a cycle hole (to the R plane or the initial level)
    // Tolerances from the text as written (one least-count unit), mm, mm/min and s.
    double tol = 0.001;
    double feedTol = 1;
    double dwellTol = 0.001;
};

struct Issue {
    int line = 0;     // 1-based; 0 when the issue is about the whole program
    std::string code; // a stable short key, e.g. "unknown-word", "safety-feed-spindle-stopped"
    std::string text;
};

struct Trace {
    std::vector<std::string> lines; // the program, split into lines
    std::vector<Segment> segs;
    std::vector<Issue> errors; // words, codes and forms the interpreter rejects
    std::vector<Issue> safety; // the machine-level safety facts that fail (no toolpath needed)
    double axisLcMm = 0.001;   // the least count of axis words as written (mm)
    bool inch = false;
};

// Interprets a whole program. Never throws; everything it does not accept is an Issue.
Trace interpret(const std::string& text, const Dialect& dialect);

// "line 12 (G1 X10. F200.): <text>" (or "program: <text>" for line 0).
std::string describe(const Issue& issue, const std::vector<std::string>& lines);

} // namespace rtgc
