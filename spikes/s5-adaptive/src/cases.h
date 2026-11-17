// S5 spike: the six test pockets, in millimetres. Shared by the generator driver and the checker.
// No dependency on Adaptive or Clipper.
#pragma once
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace s5 {
using Pt = std::pair<double, double>;
using Loop = std::vector<Pt>;
using Loops = std::vector<Loop>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kToolDia = 12.7;  // 0.5 in end mill

struct Case {
    std::string id, name;
    Loops pocket;  // outer loops CCW, island loops CW; region = even-odd fill
    Loops stock;   // stock outline at this Z level
};

inline void arc(Loop& l, double cx, double cy, double r, double a0deg, double a1deg) {
    const double sag = 0.001;  // chord error, mm
    double step = 2.0 * std::acos(1.0 - sag / r);
    double a0 = a0deg * kPi / 180.0, a1 = a1deg * kPi / 180.0;
    int n = (int)std::ceil(std::fabs(a1 - a0) / step);
    if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) {
        double a = a0 + (a1 - a0) * i / n;
        l.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
}
inline Loop roundedRect(double x0, double y0, double x1, double y1, double r) {
    Loop l;
    arc(l, x1 - r, y0 + r, r, -90, 0);
    arc(l, x1 - r, y1 - r, r, 0, 90);
    arc(l, x0 + r, y1 - r, r, 90, 180);
    arc(l, x0 + r, y0 + r, r, 180, 270);
    return l;
}
inline Loop rect(double x0, double y0, double x1, double y1) {
    return Loop{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}
inline Loop circle(double cx, double cy, double r) {
    Loop l;
    arc(l, cx, cy, r, 0, 360);
    l.pop_back();
    return l;
}
inline Loop reversed(Loop l) { return Loop(l.rbegin(), l.rend()); }

inline std::vector<Case> makeCases() {
    std::vector<Case> c;
    const double in = 25.4;
    // 1. BP1-style closed rectangular pocket, 2.5 x 1.5 in, 0.25 in corner radii (equal to tool radius).
    c.push_back({"p1", "BP1 rect pocket 63.5x38.1 R6.35", {roundedRect(0, 0, 2.5 * in, 1.5 * in, 0.25 * in)},
                 {rect(-10, -10, 2.5 * in + 10, 1.5 * in + 10)}});
    // 2. BP2-style closed circular pocket, 1.500 in diameter, in a 3.25 in square stock.
    c.push_back({"p2", "BP2 circular pocket D38.1", {circle(41.275, 41.275, 0.75 * in)}, {rect(0, 0, 82.55, 82.55)}});
    // 3. Pocket with two islands (one round, one rectangular).
    c.push_back({"p3", "Pocket 90x60 R8 with two islands",
                 {roundedRect(0, 0, 90, 60, 8), reversed(circle(25, 30, 7)), reversed(roundedRect(55, 24, 71, 36, 2))},
                 {rect(-10, -10, 100, 70)}});
    // 4. Narrow slot, 14.5 mm wide (tool 12.7), 70 mm long, full-radius ends.
    c.push_back({"p4", "Narrow slot 14.5x70", {roundedRect(0, 0, 70, 14.5, 7.25)}, {rect(-10, -10, 80, 24.5)}});
    // 5. Open pocket: BP2-style step, open to the stock edge y = 0.
    {
        Loop l{{16, 0}, {66, 0}};
        arc(l, 58, 22, 8, 0, 90);
        arc(l, 24, 22, 8, 90, 180);
        c.push_back({"p5", "Open pocket 50x30 R8, open at stock edge", {l}, {rect(0, 0, 82.55, 82.55)}});
    }
    // 6. Thin wall: two pockets 40x30 R7 separated by a 1 mm wall.
    c.push_back({"p6", "Thin wall: two 40x30 R7 pockets, 1 mm wall",
                 {roundedRect(0, 0, 40, 30, 7), roundedRect(41, 0, 81, 30, 7)}, {rect(-10, -10, 91, 40)}});
    // Extras (not among the six): other slot widths, to find where entry starts to work.
    c.push_back({"p4b", "Extra: slot 13.2x70", {roundedRect(0, 0, 70, 13.2, 6.6)}, {rect(-10, -10, 80, 23.2)}});
    c.push_back({"p4c", "Extra: slot 15.0x70", {roundedRect(0, 0, 70, 15.0, 7.5)}, {rect(-10, -10, 80, 25.0)}});
    c.push_back({"p4d", "Extra: slot 16.0x70", {roundedRect(0, 0, 70, 16.0, 8.0)}, {rect(-10, -10, 80, 26.0)}});
    c.push_back({"p4e", "Extra: slot 19.05x70", {roundedRect(0, 0, 70, 19.05, 9.525)}, {rect(-10, -10, 80, 29.05)}});
    c.push_back({"p4f", "Extra: slot 23.0x70", {roundedRect(0, 0, 70, 23.0, 11.5)}, {rect(-10, -10, 80, 33.0)}});
    // Extra: a larger pocket, 6 x 4 in with two islands, for run-time scaling only.
    c.push_back({"p7", "Extra: large pocket 152.4x101.6 R8 with two islands",
                 {roundedRect(0, 0, 152.4, 101.6, 8), reversed(circle(45, 50, 12)), reversed(roundedRect(95, 35, 125, 65, 3))},
                 {rect(-10, -10, 162.4, 111.6)}});
    return c;
}
}  // namespace s5
