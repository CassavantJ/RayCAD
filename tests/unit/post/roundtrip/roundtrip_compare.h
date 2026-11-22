// The comparison side of the T-043 round trip: the toolpath IR (include/ray/cam.h) a program was posted from, turned
// into the motion cam.h says it means, against the motion the interpreter (gcode_interp.h) read back from the program.
// Nothing here comes from src/post: the cycle motion is expanded from cam.h's "Canned cycles" section.
//
// The comparison is geometric, not block by block. Cutting motion (feeds, arcs, dwells) must match in order: an IR
// line may be written as several collinear lines, an IR arc as several arcs about the same centre or as lines whose
// vertices lie on it (the host splits arcs and linearises arcs outside the post's planes). Rapids are compared by
// what they risk: a rapid that reproduces an IR rapid is fine; any other rapid the program adds (retracts, G28 / G53
// moves, tool-change positions, cycle restarts) is classified and must be safe: Z up; X/Y travel no lower than the
// highest Z the toolpath itself travels at between the same two cuts; a descent only onto a toolpath point or no
// lower than that height. A canned cycle's return level (G98 / G99) must be reached exactly.
#pragma once
#include "gcode_interp.h"

#include <ray/cam.h>
#include <ray/libraries.h>

#include <string>
#include <vector>

namespace rtgc {

struct ExpectedSection {
    std::string name;
    const ray::cam::Toolpath* path = nullptr;
    int tool = 0;           // T
    int lengthOffset = 0;   // H
    int diameterOffset = 0; // D
    double rpm = 0;
    bool clockwise = true;
    unsigned coolant = 0; // CoolantBit set
    std::string wcs;      // "G54" ..., empty when not checked
};

struct Expectation {
    std::vector<ExpectedSection> sections;
    const ray::lib::MachineDef* machine = nullptr; // for cam::peckClearanceNm
    // Haas G83 I J K peck reduction (properties peckReduction, peckReductionJ, peckMinimumK): I = Q, J and K are
    // these fractions of Q, read per H1 p.77 (first peck I, each next one J less, never less than K). < 0: off.
    double peckReductionJ = -1;
    double peckMinimumK = -1;
    std::vector<std::string> warnings; // the post's warnings for this program
};

// The issues found; each names the program line it is about (Issue::line, 1-based), or 0 for the whole program.
std::vector<Issue> compare(const Trace& trace, const Expectation& expected);

// The expected work-offset word for cam::Wcs::workOffset (1 => G54 ... 6 => G59); empty for others (cam.h does not
// say which extended offset a larger number means).
std::string workOffsetName(int workOffset);

} // namespace rtgc
