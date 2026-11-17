// The scripted 40-feature part: a prismatic job-shop style block.
// Every dimension is a named parameter so that edit scenarios can change it.
#include "s4.h"

namespace s4 {

Params baseParams() {
    return {
        {"L", 160}, {"W", 100}, {"H", 30},
        {"F01.cut", 0},
        {"F02.r", 6},
        {"F03.x0", 12}, {"F03.y0", 22}, {"F03.x1", 62}, {"F03.y1", 78}, {"F03.h", 14}, {"F03.cut", 0},
        {"F04.cx", 122}, {"F04.cy", 52}, {"F04.r", 20}, {"F04.h", 10},
        {"F05.y0", 88}, {"F05.d", 8}, {"F05.jog", 0},
        {"F06.x0", 72}, {"F06.y0", 14}, {"F06.x1", 96}, {"F06.y1", 70}, {"F06.r", 5}, {"F06.d", 14},
        {"F07.x0", 20}, {"F07.y0", 30}, {"F07.x1", 54}, {"F07.y1", 70}, {"F07.d", 10}, {"F07.cut", 0},
        {"F08.r", 4},
        {"F09.x0", 112}, {"F09.x1", 132}, {"F09.cy", 14}, {"F09.r", 5},
        {"F10.d", 9}, {"F10.h1x", 10}, {"F10.h1y", 10}, {"F10.h4y", 95},
        {"F11.d", 15}, {"F11.depth", 6},
        {"F12.d", 18}, {"F13.d", 28}, {"F13.depth", 4},
        {"F14.d", 4}, {"F14.depth", 10}, {"F14.x1", 16},
        {"F15.r", 5}, {"F16.d", 1.0}, {"F17.d", 1.5}, {"F18.r", 3}, {"F19.d", 0.6}, {"F20.r", 2},
        {"F21.x0", 40}, {"F21.x1", 60}, {"F21.y1", 8}, {"F22.r", 3},
        {"F23.y", 52}, {"F23.z", 14}, {"F23.d", 8}, {"F23.depth", 36},
        {"F24.cx", 84}, {"F24.cy", 42}, {"F24.r", 5}, {"F24.h", 8},
        {"F25.d", 4}, {"F26.d", 1},
        {"F27.d", 5}, {"F27.depth", 12}, {"F27.x2", 80}, {"F28.d", 0.5},
        {"F29.y1", 84}, {"F29.vy", 80}, {"F29.d", 6}, {"F30.r", 2},
        {"F31.d", 6},
        {"F32.len", 18}, {"F32.y0", 62}, {"F32.y1", 86}, {"F32.h", 12}, {"F32.z0", 3},
        {"F33.d", 8}, {"F34.r", 4}, {"F35.d", 1}, {"F36.d", 1}, {"F37.r", 2},
        {"F38.x0", 80}, {"F38.x1", 90}, {"F38.d", 6},
        {"F39.d", 0.5},
        {"F40.z0", 10}, {"F40.z1", 14}, {"F40.depth", 3},
        {"X1.on", 0}, {"X2.on", 0}, {"X3.on", 0}, {"Y1.on", 0},
    };
}

namespace {
Ent line(double x0, double y0, double x1, double y1, int tag) {
    Ent e; e.k = 'L'; e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1; e.tag = tag; return e;
}
Ent arc(double x0, double y0, double x1, double y1, double cx, double cy, double r, int tag) {
    Ent e; e.k = 'A'; e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1; e.cx = cx; e.cy = cy; e.r = r; e.tag = tag; return e;
}
Loop circle(int id, double cx, double cy, double r, int tag) {
    Ent e; e.k = 'C'; e.cx = cx; e.cy = cy; e.r = r; e.tag = tag; e.x0 = e.x1 = cx + r; e.y0 = e.y1 = cy;
    return Loop{id, {e}};
}
// polygon through pts; tags[i] names the edge that starts at pts[i]
Loop poly(int id, const std::vector<std::array<double, 2>>& pts, const std::vector<int>& tags) {
    Loop l; l.id = id;
    for (size_t i = 0; i < pts.size(); ++i) {
        auto a = pts[i], b = pts[(i + 1) % pts.size()];
        l.ents.push_back(line(a[0], a[1], b[0], b[1], tags[i]));
    }
    return l;
}
// rectangle: tags 1 bottom (y=y0), 2 right (x=x1), 3 top (y=y1), 4 left (x=x0).
// cut>0 replaces one corner by a diagonal line with tag 9 ("add a sketch line").
Loop rect(int id, double x0, double y0, double x1, double y1, double cut = 0, int corner = 2) {
    if (cut <= 0) return poly(id, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, {1, 2, 3, 4});
    if (corner == 2) // top-right
        return poly(id, {{x0, y0}, {x1, y0}, {x1, y1 - cut}, {x1 - cut, y1}, {x0, y1}}, {1, 2, 9, 3, 4});
    // bottom-left
    return poly(id, {{x0 + cut, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0 + cut}}, {1, 2, 3, 4, 9});
}
// rounded rectangle: lines 1..4 as rect, arcs 5 (x1,y0) 6 (x1,y1) 7 (x0,y1) 8 (x0,y0)
Loop rrect(int id, double x0, double y0, double x1, double y1, double r) {
    Loop l; l.id = id;
    l.ents = {
        line(x0 + r, y0, x1 - r, y0, 1), arc(x1 - r, y0, x1, y0 + r, x1 - r, y0 + r, r, 5),
        line(x1, y0 + r, x1, y1 - r, 2), arc(x1, y1 - r, x1 - r, y1, x1 - r, y1 - r, r, 6),
        line(x1 - r, y1, x0 + r, y1, 3), arc(x0 + r, y1, x0, y1 - r, x0 + r, y1 - r, r, 7),
        line(x0, y1 - r, x0, y0 + r, 4), arc(x0, y0 + r, x0 + r, y0, x0 + r, y0 + r, r, 8)};
    return l;
}
// obround: 1 bottom line, 2 right arc, 3 top line, 4 left arc
Loop obround(int id, double x0, double x1, double cy, double r) {
    Loop l; l.id = id;
    l.ents = {line(x0, cy - r, x1, cy - r, 1), arc(x1, cy - r, x1, cy + r, x1, cy, r, 2),
              line(x1, cy + r, x0, cy + r, 3), arc(x0, cy + r, x0, cy - r, x0, cy, r, 4)};
    return l;
}
Frame top(double z) { Frame f; f.o = gp_Pnt(0, 0, z); return f; }
Feature sweep(const std::string& id, BOp op, const std::string& role, Frame fr, std::vector<Loop> loops, double dist) {
    Feature f; f.id = id; f.type = FType::Sweep; f.op = op; f.role = role; f.frame = fr; f.loops = std::move(loops); f.dist = dist;
    return f;
}
Feature blend(const std::string& id, bool fillet, double size, std::vector<Target> t) {
    Feature f; f.id = id; f.type = fillet ? FType::Fillet : FType::Chamfer; f.role = fillet ? "fillet" : "chamfer";
    f.size = size; f.targets = std::move(t);
    return f;
}
} // namespace

Script makeScript(const Params& PP) {
    auto P = [&](const char* k) { return PP.at(k); };
    const double L = P("L"), W = P("W"), H = P("H");
    const double THRU = 200; // "through all"
    Script s;
    auto S = [](int tag) { return "/s" + std::to_string(tag); };

    // 1 base plate
    s.push_back(sweep("F01", BOp::New, "extrude", top(0), {rect(1, 0, 0, L, W, P("F01.cut"), 0)}, H));
    // X1: optional feature inserted early: through notch on the x=L side (splits that side face)
    if (P("X1.on") > 0) s.push_back(sweep("X1", BOp::Cut, "extrude", top(H), {rect(1, L - 8, 24, L + 5, 40)}, THRU));
    // X2: optional blind pocket inserted early, in a free area of the top
    if (P("X2.on") > 0) s.push_back(sweep("X2", BOp::Cut, "extrude", top(H), {rect(1, 108, 77, 140, 85)}, 5));
    // X3: optional early feature designed to attack ordinals (see REPORT): a shallow cut in the top of the
    // x=0 side that moves the centroid of the upper piece of that side past the centroid of the lower piece.
    if (P("X3.on") > 0) s.push_back(sweep("X3", BOp::Cut, "extrude", top(H), {rect(1, -5, 10, 2, 30)}, 6));
    // 2 fillet the four vertical corners of the plate
    s.push_back(blend("F02", true, P("F02.r"),
        {{"F01/s4", "F01/s1"}, {"F01/s1", "F01/s2"}, {"F01/s2", "F01/s3"}, {"F01/s3", "F01/s4"}}));
    // 35 chamfer the bottom outer perimeter (two picks; tangent propagation does the rest).
    // Third in the tree: OCCT 8.0.1 fails or returns an invalid solid for this chamfer once the through holes exist.
    s.push_back(blend("F35", false, P("F35.d"),
        {{"F01/s4", "F01/c0/1"}, {"F01/s1", "F01/c0/1"},
         {"F01/s2", "F01/c0/1", true, false}, {"F01/s3", "F01/c0/1", true, false},
         {"F02/b0", "F01/c0/1", true, false}, {"F02/b1", "F01/c0/1", true, false},
         {"F02/b2", "F01/c0/1", true, false}, {"F02/b3", "F01/c0/1", true, false}}));
    // 3 rectangular boss
    s.push_back(sweep("F03", BOp::Join, "extrude", top(H),
        {rect(1, P("F03.x0"), P("F03.y0"), P("F03.x1"), P("F03.y1"), P("F03.cut"), 2)}, P("F03.h")));
    // 4 round boss
    s.push_back(sweep("F04", BOp::Join, "extrude", top(H), {circle(1, P("F04.cx"), P("F04.cy"), P("F04.r"), 1)}, P("F04.h")));
    // 5 step along the y=W side
    if (P("F05.jog") > 0)
        s.push_back(sweep("F05", BOp::Cut, "extrude", top(H),
            {poly(1, {{-5, P("F05.y0")}, {110, P("F05.y0")}, {110, P("F05.y0") - 6}, {L + 5, P("F05.y0") - 6}, {L + 5, W + 5}, {-5, W + 5}}, {1, 9, 10, 2, 3, 4})}, P("F05.d")));
    else
        s.push_back(sweep("F05", BOp::Cut, "extrude", top(H), {rect(1, -5, P("F05.y0"), L + 5, W + 5)}, P("F05.d")));
    // 6 pocket A (rounded rectangle)
    s.push_back(sweep("F06", BOp::Cut, "extrude", top(H),
        {rrect(1, P("F06.x0"), P("F06.y0"), P("F06.x1"), P("F06.y1"), P("F06.r"))}, P("F06.d")));
    // 7 pocket B in the boss top (sharp corners)
    s.push_back(sweep("F07", BOp::Cut, "extrude", top(H + P("F03.h")),
        {rect(1, P("F07.x0"), P("F07.y0"), P("F07.x1"), P("F07.y1"), P("F07.cut"), 2)}, P("F07.d")));
    // 8 fillet the vertical corners of pocket B
    s.push_back(blend("F08", true, P("F08.r"),
        {{"F07/s1", "F07/s2", false}, {"F07/s2", "F07/s3", false}, {"F07/s3", "F07/s4", false}, {"F07/s4", "F07/s1", false}}));
    // 9 obround through slot
    s.push_back(sweep("F09", BOp::Cut, "extrude", top(H), {obround(1, P("F09.x0"), P("F09.x1"), P("F09.cy"), P("F09.r"))}, THRU));
    // 10 four corner through holes
    {
        double r = P("F10.d") / 2;
        s.push_back(sweep("F10", BOp::Cut, "hole", top(H),
            {circle(1, P("F10.h1x"), P("F10.h1y"), r, 1), circle(2, L - 10, 10, r, 2),
             circle(3, L - 10, P("F10.h4y"), r, 3), circle(4, 10, P("F10.h4y"), r, 4)}, THRU));
    }
    // 11 counterbores on the two front holes
    {
        double r = P("F11.d") / 2;
        s.push_back(sweep("F11", BOp::Cut, "hole", top(H),
            {circle(1, P("F10.h1x"), P("F10.h1y"), r, 1), circle(2, L - 10, 10, r, 2)}, P("F11.depth")));
    }
    // 12 central bore, 13 its counterbore
    s.push_back(sweep("F12", BOp::Cut, "hole", top(H + P("F04.h")), {circle(1, P("F04.cx"), P("F04.cy"), P("F12.d") / 2, 1)}, THRU));
    s.push_back(sweep("F13", BOp::Cut, "hole", top(H + P("F04.h")), {circle(1, P("F04.cx"), P("F04.cy"), P("F13.d") / 2, 1)}, P("F13.depth")));
    // 14 two blind holes in the boss rim
    {
        double r = P("F14.d") / 2, y = (P("F03.y0") + P("F03.y1")) / 2;
        s.push_back(sweep("F14", BOp::Cut, "hole", top(H + P("F03.h")),
            {circle(1, P("F14.x1"), y, r, 1), circle(2, 58, y, r, 2)}, P("F14.depth")));
    }
    // 15 fillet the boss vertical edges
    s.push_back(blend("F15", true, P("F15.r"),
        {{"F03/s1", "F03/s2"}, {"F03/s2", "F03/s3"}, {"F03/s3", "F03/s4"}, {"F03/s4", "F03/s1"}}));
    // 16 chamfer the boss top outer loop (lines and the arcs made by F15)
    s.push_back(blend("F16", false, P("F16.d"),
        {{"F03/s1", "F03/c1/1"}, {"F03/s2", "F03/c1/1"}, {"F03/s3", "F03/c1/1"}, {"F03/s4", "F03/c1/1"},
         {"F15/b0", "F03/c1/1"}, {"F15/b1", "F03/c1/1"}, {"F15/b2", "F03/c1/1"}, {"F15/b3", "F03/c1/1"},
         {"F03/s9", "F03/c1/1", true, false}}));
    // 17 chamfer the round boss top, 18 fillet its base
    s.push_back(blend("F17", false, P("F17.d"), {{"F04/s1", "F04/c1/1"}}));
    s.push_back(blend("F18", true, P("F18.r"), {{"F04/s1", "F01/c1/1", false}}));
    // 19 countersink the two blind holes
    s.push_back(blend("F19", false, P("F19.d"), {{"F14/s1", "F03/c1/1"}, {"F14/s2", "F03/c1/1"}}));
    // 20 fillet the floor loop of pocket A; two lines are picked, the rest follows by tangent propagation
    {
        std::vector<Target> t;
        for (int i = 1; i <= 8; ++i) t.push_back({"F06" + S(i), "F06/c1/1", false, i == 1 || i == 3});
        s.push_back(blend("F20", true, P("F20.r"), t));
    }
    // 21 through notch on the y=0 side (splits that side face), 22 fillet its inner corners
    s.push_back(sweep("F21", BOp::Cut, "extrude", top(H), {rect(1, P("F21.x0"), -5, P("F21.x1"), P("F21.y1"))}, THRU));
    s.push_back(blend("F22", true, P("F22.r"), {{"F21/s2", "F21/s3", false}, {"F21/s3", "F21/s4", false}}));
    // 23 cross hole drilled from the x=L side into the central bore
    {
        Frame f; f.o = gp_Pnt(L, 0, 0); f.xd = gp_Dir(0, 1, 0); f.n = gp_Dir(1, 0, 0);
        s.push_back(sweep("F23", BOp::Cut, "hole", f, {circle(1, P("F23.y"), P("F23.z"), P("F23.d") / 2, 1)}, P("F23.depth")));
    }
    // 24 island in pocket A, 25 hole through it, 26 chamfer its top
    s.push_back(sweep("F24", BOp::Join, "extrude", top(H - P("F06.d")), {circle(1, P("F24.cx"), P("F24.cy"), P("F24.r"), 1)}, P("F24.h")));
    s.push_back(sweep("F25", BOp::Cut, "hole", top(H - P("F06.d") + P("F24.h")), {circle(1, P("F24.cx"), P("F24.cy"), P("F25.d") / 2, 1)}, THRU));
    s.push_back(blend("F26", false, P("F26.d"), {{"F24/s1", "F24/c1/1"}}));
    // 27 three blind holes on the step, 28 chamfer their rims
    {
        double r = P("F27.d") / 2, z = H - P("F05.d");
        s.push_back(sweep("F27", BOp::Cut, "hole", top(z),
            {circle(1, 40, 94, r, 1), circle(2, P("F27.x2"), 94, r, 2), circle(3, 120, 94, r, 3)}, P("F27.depth")));
        s.push_back(blend("F28", false, P("F28.d"), {{"F27/s1", "F05/c1/1"}, {"F27/s2", "F05/c1/1"}, {"F27/s3", "F05/c1/1"}}));
    }
    // 29 pocket C (pentagon with one diagonal side), 30 fillet three of its corners
    s.push_back(sweep("F29", BOp::Cut, "extrude", top(H),
        {poly(1, {{70, 74}, {100, 74}, {100, P("F29.vy")}, {96, P("F29.y1")}, {70, P("F29.y1")}}, {1, 2, 3, 4, 5})}, P("F29.d")));
    s.push_back(blend("F30", true, P("F30.r"), {{"F29/s5", "F29/s1", false}, {"F29/s1", "F29/s2", false}, {"F29/s4", "F29/s5", false}}));
    // 31 two dowel holes
    s.push_back(sweep("F31", BOp::Cut, "hole", top(H), {circle(1, 30, 12, P("F31.d") / 2, 1), circle(2, 90, 8, P("F31.d") / 2, 2)}, THRU));
    // 32 mounting lug on the x=L side, 3 mm above the bottom
    s.push_back(sweep("F32", BOp::Join, "extrude", top(P("F32.z0")), {rect(1, L, P("F32.y0"), L + P("F32.len"), P("F32.y1"))}, P("F32.h")));
    // 33 hole through the tab, 34 fillet the tab outer corners
    s.push_back(sweep("F33", BOp::Cut, "hole", top(P("F32.z0") + P("F32.h")),
        {circle(1, L + P("F32.len") / 2, (P("F32.y0") + P("F32.y1")) / 2, P("F33.d") / 2, 1)}, THRU));
    s.push_back(blend("F34", true, P("F34.r"), {{"F32/s1", "F32/s2"}, {"F32/s2", "F32/s3"}}));
    // 36 chamfer the counterbore mouth on the round boss
    s.push_back(blend("F36", false, P("F36.d"), {{"F13/s1", "F04/c1/1"}}));
    // 37 fillet the step inner corner
    s.push_back(blend("F37", true, P("F37.r"), {{"F05/s1", "F05/c1/1", false}}));
    // 38 channel joining pocket A and pocket C; same depth as pocket C, so the two floors are one merged face
    s.push_back(sweep("F38", BOp::Cut, "extrude", top(H), {rect(1, P("F38.x0"), 66, P("F38.x1"), 78)}, P("F38.d")));
    // 39 deburr chamfer on the slot top rim (one pick, closed tangent loop)
    s.push_back(blend("F39", false, P("F39.d"),
        {{"F09/s1", "F01/c1/1"}, {"F09/s2", "F01/c1/1", true, false}, {"F09/s3", "F01/c1/1", true, false}, {"F09/s4", "F01/c1/1", true, false}}));
    // 40 groove along the x=0 side (splits that side face into an upper and a lower piece)
    {
        Frame f; f.o = gp_Pnt(0, 0, 0); f.xd = gp_Dir(0, -1, 0); f.n = gp_Dir(-1, 0, 0);
        s.push_back(sweep("F40", BOp::Cut, "extrude", f, {rect(1, -(W + 5), P("F40.z0"), 5, P("F40.z1"))}, P("F40.depth")));
    }
    // Y1: optional feature appended at the end: a groove across the boss top (splits the boss top face,
    // two chamfer faces and several edges that features and references already use)
    if (P("Y1.on") > 0) s.push_back(sweep("Y1", BOp::Cut, "extrude", top(H + P("F03.h")), {rect(1, 30, 10, 44, 90)}, 3));
    return s;
}

} // namespace s4
