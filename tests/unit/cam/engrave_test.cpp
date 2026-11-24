// The engraving operation (task T-056): the font (resources/fonts/raycad-single.rsf), the text layout against
// hand-computed values, the arc rule on random text (both ends of every arc within 1 nm of its circle), arc fitting of
// sampled edges, the levels, and each refusal on its own (validate and generate give the same words).
#include "engrave_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::ops;
using namespace ray::cam::test;

namespace {

const StrokeFont& font()
{
    const StrokeFont* f = engravingFont();
    REQUIRE(f != nullptr);
    return *f;
}

// A plate 80 x 50 x 10 with its top at Z 0 (the engraving plane of the tests).
const kernel::Shape& plate()
{
    static const kernel::Shape s = box(Vec3{0, 0, -10}, Vec3{80, 50, 0});
    return s;
}

double distNm(geom2d::Pt a, geom2d::Pt b)
{
    return std::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y));
}

// splitmix64, the same numbers on every platform.
struct Rng {
    std::uint64_t s;
    std::uint64_t next()
    {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform(double lo, double hi) { return lo + (hi - lo) * static_cast<double>(next() >> 11) * 0x1.0p-53; }
};

} // namespace

TEST_CASE("the engraving font parses and has the characters it promises", "[cam][engrave][font]")
{
    const StrokeFont& f = font();
    CHECK(f.name == "RayCAD-Single-Line");
    CHECK(f.cap == 12'000);
    const std::string needed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,-+/:#() ;=_!?*'\"[]<>%";
    for (const char c : needed) {
        INFO("character '" << c << "'");
        const auto it = f.glyphs.find(static_cast<char32_t>(c));
        REQUIRE(it != f.glyphs.end());
        CHECK(it->second.advance > 0);
        CHECK((c == ' ' || !it->second.strokes.empty()));
    }
    for (const char32_t c : {U'°', U'Ø', U'⌀'}) {
        CHECK(f.glyphs.count(c) == 1);
    }
    // Metrics: nothing above 12 units or below -4.
    for (const auto& [code, g] : f.glyphs) {
        for (const FontStroke& s : g.strokes) {
            CHECK(s.y <= 12'000);
            CHECK(s.y >= -4'000);
            for (const FontSegment& seg : s.segments) {
                CHECK(seg.y <= 12'000);
                CHECK(seg.y >= -4'000);
            }
        }
    }
    // The header says what the font is and under which licence (resources/fonts/README.md, THIRD_PARTY.md).
    const std::string_view text = engravingFontText();
    CHECK(text.find("Original work drawn for RayCAD") != std::string_view::npos);
    CHECK(text.find("LGPL-2.1-or-later") != std::string_view::npos);
    CHECK(text == readText(std::string(RAY_SOURCE_DIR) + "/resources/fonts/raycad-single.rsf"));
}

TEST_CASE("the font reader refuses what is not the format, naming the line", "[cam][engrave][font]")
{
    const auto refused = [](const std::string& text, const std::string& words) {
        auto f = parseStrokeFont(text);
        INFO(text);
        REQUIRE_FALSE(f);
        CHECK(f.error().code == ErrorCode::FormatError);
        CHECK_THAT(f.error().message, ContainsSubstring(words));
    };
    refused("cap 12\nglyph U+0041 10\nM 0 0 L 1 1\n", "the last glyph has no end");
    refused("cap 12\nglyph U+0041 10\nM 0 0\nend\n", "at least one segment");
    refused("cap 12\nglyph U+0041 10\nM 0 0 L 1.2345 0\nend\n", "Font line 3");
    refused("cap 12\nglyph U+0041 10\nM 4 0 A 0 5 0 0 +\nend\n", "not on one circle");
    refused("cap 12\nglyph U+0041 10\nend\nglyph U+0041 10\nend\n", "has a glyph already");
    refused("glyph U+0041 10\nend\n", "no cap height");
    refused("cap 12\nbold\n", "'bold' is not a statement");
    auto ok = parseStrokeFont("font T\r\ncap 12 # comment\r\nglyph U+0041 10\r\nM 0 0 L 1 1 A 3 1 2 1 -\r\nend");
    REQUIRE(ok);
    CHECK(ok->glyphs.at(U'A').strokes.at(0).segments.size() == 2);
}

TEST_CASE("text layout against hand-computed points", "[cam][engrave][font]")
{
    const StrokeFont& f = font();
    // "H" 12 mm high: one font unit is 1 mm. The left stroke from (0, 0) to (0, 12), at (10, 20).
    TextLayout l;
    l.text = "H";
    l.height = 12;
    l.x = 10;
    l.y = 20;
    auto strokes = layoutText(f, l);
    REQUIRE(strokes);
    REQUIRE(strokes->size() == 3);
    CHECK((*strokes)[0].start == geom2d::Pt{10'000'000, 20'000'000});
    CHECK((*strokes)[0].spans.at(0).end == geom2d::Pt{10'000'000, 32'000'000});
    CHECK((*strokes)[2].spans.at(0).end == geom2d::Pt{18'000'000, 26'000'000}); // the bar to (8, 6)
    // Width: "HI" = (11 + 7) units + one extra spacing of 0.5 mm, at height 6 (0.5 mm per unit) = 9.5 mm.
    l.text = "HI";
    l.height = 6;
    l.spacing = 0.5;
    CHECK(std::abs(textWidthMm(f, l) - 9.5) < 1e-12);
    // Centred: the anchor at the middle, so "H" starts 4.75 mm to its left; right: 9.5 mm to its left.
    l.align = TextAlign::Centre;
    auto centred = layoutText(f, l);
    REQUIRE(centred);
    CHECK(centred->front().start == geom2d::Pt{10'000'000 - 4'750'000, 20'000'000});
    l.align = TextAlign::Right;
    auto right = layoutText(f, l);
    REQUIRE(right);
    CHECK(right->front().start == geom2d::Pt{10'000'000 - 9'500'000, 20'000'000});
    // The "I" after it: the pen at 11 units * 0.5 + 0.5 = 6 mm, its top bar from (0, 12) units = (6, 6) mm.
    CHECK(right->at(3).start == geom2d::Pt{10'000'000 - 9'500'000 + 6'000'000, 26'000'000});
    // Turned 90 degrees: the baseline runs along +Y, up becomes -X. "H" at height 12: (0, 12) -> (-12, 0).
    l.text = "H";
    l.height = 12;
    l.spacing = 0;
    l.align = TextAlign::Left;
    l.angleDeg = 90;
    auto turned = layoutText(f, l);
    REQUIRE(turned);
    CHECK((*turned)[0].spans.at(0).end == geom2d::Pt{10'000'000 - 12'000'000, 20'000'000});
    CHECK((*turned)[1].start == geom2d::Pt{10'000'000, 28'000'000}); // the right stroke at x = 8
    // "o": one whole circle of radius 4 units about (4, 4), drawn from (8, 4) counter-clockwise, a closed path.
    l.text = "o";
    l.angleDeg = 0;
    auto o = layoutText(f, l);
    REQUIRE(o);
    REQUIRE(o->size() == 1);
    CHECK(o->front().closed);
    const geom2d::Span& circle = o->front().spans.at(0);
    CHECK(circle.kind == geom2d::SpanKind::ArcCCW);
    CHECK(circle.centre == geom2d::Pt{14'000'000, 24'000'000});
    CHECK(circle.radius == 4'000'000);
    CHECK(circle.end == o->front().start);
}

// After M3: text of several lines, each on its own baseline lineSpacing below the one before, each aligned on its own.
TEST_CASE("text layout in lines, each aligned on its own", "[cam][engrave][font][lines]")
{
    const StrokeFont& f = font();
    CHECK(textLines("HI") == std::vector<std::string>{"HI"});
    CHECK(textLines("H\nII") == std::vector<std::string>{"H", "II"});
    CHECK(textLines("H\r\nI\rI\n") == std::vector<std::string>{"H", "I", "I", ""});
    CHECK(textLines("") == std::vector<std::string>{""});
    // "H" over "II", 12 mm high (1 mm per unit), 20 mm between baselines, at (10, 20).
    TextLayout l;
    l.text = "H\nII";
    l.height = 12;
    l.lineSpacing = 20;
    l.x = 10;
    l.y = 20;
    // The widest line: "II" = 7 + 7 units = 14 mm (H is 11).
    CHECK(std::abs(textWidthMm(f, l) - 14) < 1e-12);
    auto two = layoutText(f, l);
    REQUIRE(two);
    REQUIRE(two->size() == 3 + 2 * 3); // H: three strokes; I: a stem and two bars each
    CHECK((*two)[0].start == geom2d::Pt{10'000'000, 20'000'000});
    // The second line's first "I": its top bar from (0, 12) units, on the baseline 20 mm lower (Y 0): Y 12.
    CHECK((*two)[3].start == geom2d::Pt{10'000'000, 12'000'000});
    // Centred: each line about the anchor by its own width (H 11 mm, II 14 mm).
    l.align = TextAlign::Centre;
    auto centred = layoutText(f, l);
    REQUIRE(centred);
    CHECK(centred->at(0).start.x == 10'000'000 - 5'500'000);
    geom2d::Coord minX2 = std::numeric_limits<geom2d::Coord>::max(), maxX2 = std::numeric_limits<geom2d::Coord>::min();
    for (std::size_t i = 3; i < centred->size(); ++i) {
        const geom2d::Path& p = centred->at(i);
        minX2 = std::min(minX2, p.start.x);
        maxX2 = std::max(maxX2, p.start.x);
        for (const geom2d::Span& s : p.spans) {
            minX2 = std::min(minX2, s.end.x);
            maxX2 = std::max(maxX2, s.end.x);
        }
    }
    CHECK(minX2 == 10'000'000 - 7'000'000); // the second line starts 7 mm left of the anchor
    // Right: each line ends at the anchor.
    l.align = TextAlign::Right;
    auto right = layoutText(f, l);
    REQUIRE(right);
    CHECK(right->at(0).start.x == 10'000'000 - 11'000'000);
    // Lines go down the text's own up direction: turned 90 degrees, the second line is 20 mm towards +X.
    l.align = TextAlign::Left;
    l.angleDeg = 90;
    l.text = "H\nH";
    auto turned = layoutText(f, l);
    REQUIRE(turned);
    CHECK(turned->at(3).start == geom2d::Pt{10'000'000 + 20'000'000, 20'000'000});
    // The default line spacing (0): 1.6 times the height.
    l.angleDeg = 0;
    l.lineSpacing = 0;
    auto byDefault = layoutText(f, l);
    REQUIRE(byDefault);
    CHECK(byDefault->at(3).start == geom2d::Pt{10'000'000, 20'000'000 - 19'200'000});
    // An empty line keeps its place; a single line lays out as it always did (the first line's arithmetic).
    l.text = "H\n\nH";
    l.lineSpacing = 20;
    auto gap = layoutText(f, l);
    REQUIRE(gap);
    CHECK(gap->at(3).start == geom2d::Pt{10'000'000, 20'000'000 - 40'000'000});
    TextLayout one;
    one.text = "Op 10";
    one.height = 4;
    one.x = 40;
    one.y = 10;
    one.angleDeg = 30;
    TextLayout first = one;
    first.text = "Op 10\nX";
    auto a = layoutText(f, one);
    auto b = layoutText(f, first);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(b->size() > a->size());
    for (std::size_t i = 0; i < a->size(); ++i) {
        CHECK(a->at(i).start == b->at(i).start);
        CHECK(a->at(i).spans.size() == b->at(i).spans.size());
    }
}

TEST_CASE("text the font cannot draw is refused, naming the character", "[cam][engrave][font]")
{
    TextLayout l;
    l.text = "Caf\xC3\xA9";
    auto r = layoutText(font(), l);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(r.error().message, ContainsSubstring("no character '\xC3\xA9' (U+00E9)"));
    l.text = "A\xC3";
    auto bad = layoutText(font(), l);
    REQUIRE_FALSE(bad);
    CHECK_THAT(bad.error().message, ContainsSubstring("not valid UTF-8"));
    CHECK(decodeUtf8("\xE2\x8C\x80") == std::u32string(1, U'⌀'));
    CHECK_FALSE(decodeUtf8("\xC0\x80").has_value());     // overlong
    CHECK_FALSE(decodeUtf8("\xED\xA0\x80").has_value()); // a surrogate
}

TEST_CASE("every arc of laid-out text has both ends within 1 nm of its circle (10,000 seeded cases)",
          "[cam][engrave][font]")
{
    const StrokeFont& f = font();
    std::u32string all;
    for (const auto& [code, g] : f.glyphs) {
        all.push_back(code);
    }
    Rng rng{20261122};
    std::size_t arcs = 0;
    double worst = 0, worstDifference = 0;
    for (int n = 0; n < 10'000; ++n) {
        TextLayout l;
        std::u32string text;
        const int length = 1 + static_cast<int>(rng.next() % 6);
        for (int k = 0; k < length; ++k) {
            text.push_back(all[rng.next() % all.size()]);
        }
        for (const char32_t c : text) {
            const auto u = static_cast<std::uint32_t>(c);
            if (u < 0x80) {
                l.text += static_cast<char>(u);
            } else if (u < 0x800) {
                l.text += static_cast<char>(0xC0 | (u >> 6));
                l.text += static_cast<char>(0x80 | (u & 0x3F));
            } else {
                l.text += static_cast<char>(0xE0 | (u >> 12));
                l.text += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                l.text += static_cast<char>(0x80 | (u & 0x3F));
            }
        }
        l.height = rng.uniform(0.5, 60);
        l.spacing = rng.uniform(0, 2);
        l.x = rng.uniform(-500, 500);
        l.y = rng.uniform(-500, 500);
        l.angleDeg = rng.uniform(-360, 360);
        l.align = static_cast<TextAlign>(rng.next() % 3);
        auto strokes = layoutText(f, l);
        REQUIRE(strokes);
        for (const geom2d::Path& p : *strokes) {
            geom2d::Pt at = p.start;
            for (const geom2d::Span& s : p.spans) {
                if (s.kind != geom2d::SpanKind::Line) {
                    ++arcs;
                    const double r0 = distNm(at, s.centre), r1 = distNm(s.end, s.centre);
                    const double r = static_cast<double>(s.radius);
                    worst = std::max({worst, std::abs(r0 - r), std::abs(r1 - r)});
                    worstDifference = std::max(worstDifference, std::abs(r0 - r1));
                }
                at = s.end;
            }
            if (p.closed) {
                CHECK(at == p.start);
            }
        }
    }
    INFO(arcs << " arcs; worst distance from the circle " << worst << " nm, worst start-end difference "
              << worstDifference << " nm");
    CHECK(arcs > 10'000);
    CHECK(worst <= 1.25);          // engrave_font.h: within 1.2 nm (the bisector case), else within 0.71 nm
    CHECK(worstDifference <= 1.5); // the invariant checker allows 2 nm
}

TEST_CASE("runs of short lines are fitted with arcs; real lines and arcs stay", "[cam][engrave]")
{
    // A quarter circle of radius 10 mm sampled every degree, then a straight line and a real arc.
    geom2d::Path p;
    p.start = geom2d::Pt{10'000'000, 0};
    for (int k = 1; k <= 90; ++k) {
        const double a = k * 3.14159265358979323846 / 180;
        p.spans.push_back(geom2d::Span{geom2d::SpanKind::Line,
                                       geom2d::Pt{std::llround(1e7 * std::cos(a)), std::llround(1e7 * std::sin(a))},
                                       {},
                                       0});
    }
    p.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, geom2d::Pt{-5'000'000, 10'000'000}, {}, 0});
    p.spans.push_back(
        geom2d::Span{geom2d::SpanKind::ArcCW, geom2d::Pt{-5'000'000, 20'000'000}, {-5'000'000, 15'000'000}, 5'000'000});
    const geom2d::Path fitted = fitLineRuns(p);
    CHECK(fitted.start == p.start);
    CHECK(fitted.spans.back() == p.spans.back()); // the real arc kept
    CHECK(fitted.spans.size() < 10);
    std::size_t arcs = 0;
    for (const geom2d::Span& s : fitted.spans) {
        arcs += s.kind != geom2d::SpanKind::Line ? 1 : 0;
    }
    CHECK(arcs >= 2);
    // A rectangle stays four lines.
    geom2d::Path rect;
    rect.start = geom2d::Pt{0, 0};
    rect.closed = true;
    for (const geom2d::Pt q :
         {geom2d::Pt{1'000'000, 0}, geom2d::Pt{1'000'000, 500'000}, geom2d::Pt{0, 500'000}, geom2d::Pt{0, 0}}) {
        rect.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, q, {}, 0});
    }
    const geom2d::Path same = fitLineRuns(rect);
    CHECK(same.spans.size() == 4);
    for (const geom2d::Span& s : same.spans) {
        CHECK(s.kind == geom2d::SpanKind::Line);
    }
}

TEST_CASE("engraving levels: the step-down splits the depth exactly", "[cam][engrave]")
{
    OpCase c = engraveCase(plate(), {}, R"({"text":"I","textX":10,"textY":10,"depth":0.5,"stepDown":0.2})");
    JobContext ctx;
    auto plan = planEngrave(c.input(), ctx);
    REQUIRE(plan);
    // ceil(0.5 / 0.2) = 3 levels: 0.5 * 1/3 = 0.1666667 -> 166667 nm, 0.3333333 -> 333333 nm, then exactly 0.5 mm.
    CHECK(plan->levels == std::vector<Nm>{-166'667, -333'333, -500'000});
    // "I" has three strokes, each cut at three levels: nine passes, each entered by a plunge (or, where an earlier pass
    // crossing it cut the material there already, by a feed descent through that cut), none deeper than the depth.
    CHECK(plan->linked.order.size() == 9);
    for (const link::Entered e : plan->linked.entered) {
        CHECK((e == link::Entered::Plunge || e == link::Entered::Free));
    }
    for (const Move& m : plan->linked.moves) {
        CHECK(m.z >= -500'000);
    }
    // The width of the groove of a 60 degree cutter with a 0.1 mm tip at 0.5 mm: 0.1 + 2 * 0.5 * tan 30.
    CHECK(std::abs(engraveWidthMm(c.tool, 0.5) - (0.1 + std::tan(3.14159265358979323846 / 6))) < 1e-12);
    lib::Tool ball = tool(lib::ToolType::BallEndMill, 2, 0);
    CHECK(std::abs(engraveWidthMm(ball, 0.1) - 2 * std::sqrt(0.1 * 1.9)) < 1e-12);
}

TEST_CASE("engraving refuses, naming the cause", "[cam][engrave][validate]")
{
    const std::string text = R"({"text":"AB","textX":10,"textY":10})";
    const auto refused = [](const OpCase& c, ErrorCode code, const std::string& words) {
        const Status v = c.validate();
        REQUIRE_FALSE(v);
        CHECK(v.error().code == code);
        CHECK_THAT(v.error().message, ContainsSubstring(words));
        auto g = c.generate();
        REQUIRE_FALSE(g);
        CHECK(g.error().message == v.error().message);
    };
    SECTION("a tool that does not cut at its centre")
    {
        refused(engraveCase(plate(), {}, text, tool(lib::ToolType::FlatEndMill, 3, 0)), ErrorCode::InvalidArgument,
                "needs a tool that cuts at its centre");
        lib::Tool flatTip = tool(lib::ToolType::ChamferMill, 6, 90);
        flatTip.tipDiameter = 1;
        refused(engraveCase(plate(), {}, text, flatTip), ErrorCode::InvalidArgument, "chamfer mill with a flat tip");
        // After M3: a tool of an engraving type whose library entry says it does not cut at its centre.
        lib::Tool ball = tool(lib::ToolType::BallEndMill, 3, 0);
        ball.centreCutting = false;
        refused(engraveCase(plate(), {}, text, ball), ErrorCode::InvalidArgument,
                "the tool library says this ball end mill does not");
    }
    SECTION("deeper than the point")
    {
        // A 3 mm 60 degree cutter with a 0.1 mm tip reaches its diameter (3 - 0.1) / 2 / tan 30 = 2.511 mm up.
        refused(engraveCase(plate(), {}, R"({"text":"AB","depth":3})"), ErrorCode::InvalidArgument,
                "reaches its full diameter 2.511 mm above its tip");
    }
    SECTION("flutes and stick-out")
    {
        OpCase c = engraveCase(plate(), {}, R"({"text":"A","depth":1.5})", tool(lib::ToolType::BallEndMill, 3, 0, 1));
        refused(c, ErrorCode::InvalidArgument, "the tool's flutes are 1 mm long");
        OpCase s = engraveCase(plate(), {}, R"({"text":"A","depth":0.5})");
        s.op.tool.stickOut = 1.2;
        refused(s, ErrorCode::InvalidArgument, "the holder needs 1 mm above the plane");
    }
    SECTION("cutting data")
    {
        OpCase c = engraveCase(plate(), {}, text);
        c.op.cutting.clockwise = false;
        refused(c, ErrorCode::InvalidArgument, "counter-clockwise (M4)");
        c.op.cutting.clockwise = true;
        c.op.cutting.spindleRpm = 0;
        refused(c, ErrorCode::InvalidArgument, "Set a spindle speed");
        c.op.cutting.spindleRpm = 9000;
        refused(c, ErrorCode::InvalidArgument, "above the machine's maximum of 8100 rpm");
        c.op.cutting.spindleRpm = 8000;
        c.op.cutting.feedCut = 0;
        refused(c, ErrorCode::InvalidArgument, "Set a cutting feed");
    }
    SECTION("heights out of order")
    {
        OpCase c = engraveCase(plate(), {}, text);
        c.op.heights.top = 3; // above the feed plane (2)
        refused(c, ErrorCode::InvalidArgument, "The feed plane (Z2) must not be below the top height");
        c.op.heights.top = 0;
        c.op.heights.retract = 1;
        refused(c, ErrorCode::InvalidArgument, "The retract height (Z1) must not be below the feed plane (Z2)");
        c.op.heights.retract = 20;
        refused(c, ErrorCode::InvalidArgument, "The clearance height (Z15) must not be below the retract height");
        // Linking's own check: the retract plane under the stock's top.
        OpCase low = engraveCase(plate(), {}, text);
        low.op.heights.top = -5;
        low.op.heights.feedPlane = -4;
        low.op.heights.retract = -3;
        refused(low, ErrorCode::InvalidArgument, "is below the top of the stock");
    }
    SECTION("nothing to engrave, a face picked, edges along Z, a character the font has not")
    {
        refused(engraveCase(plate(), {}, R"({"text":""})"), ErrorCode::InvalidArgument,
                "Nothing to engrave: pick edges or type a text");
        refused(engraveCase(plate(), {}, R"({"text":"   "})"), ErrorCode::InvalidArgument, "Nothing to engrave");
        refused(engraveCase(plate(), {kernel::SubId{kernel::SubKind::Face, 0}}, text), ErrorCode::InvalidArgument,
                "Selection 1 is not an edge");
        std::vector<kernel::SubId> vertical;
        for (std::uint32_t i = 0; i < plate().count(kernel::SubKind::Edge); ++i) {
            const kernel::SubId e{kernel::SubKind::Edge, i};
            auto ci = kernel::curveInfo(plate(), e);
            if (ci && std::abs(ci->start.x - ci->end.x) < 1e-9 && std::abs(ci->start.y - ci->end.y) < 1e-9) {
                vertical.push_back(e);
            }
        }
        REQUIRE(vertical.size() == 4);
        refused(engraveCase(plate(), vertical, R"({})"), ErrorCode::InvalidArgument, "all run along the setup's Z");
        refused(engraveCase(plate(), {}, R"({"text":"xé"})"), ErrorCode::InvalidArgument, "(U+00E9)");
    }
    SECTION("parameters out of range")
    {
        refused(engraveCase(plate(), {}, R"({"text":"A","depth":0})"), ErrorCode::InvalidArgument, "Depth");
        refused(engraveCase(plate(), {}, R"({"text":"A","font":"times"})"), ErrorCode::InvalidArgument, "Font");
    }
}

TEST_CASE("engraving warns where the toolpath is right but worth a look", "[cam][engrave][validate]")
{
    // A wide groove for small text, and edges that lie below the plane.
    OpCase c = engraveCase(plate(), {}, R"({"text":"A","textHeight":2,"depth":0.8,"textX":5,"textY":5})");
    auto w = c.check();
    REQUIRE(w);
    REQUIRE(w->size() == 1);
    CHECK_THAT(w->front(), ContainsSubstring("more than a quarter of the 2 mm text height"));
    const kernel::Shape pocketed = cut(plate(), box(Vec3{20, 10, -3}, Vec3{40, 30, 1}));
    OpCase e = engraveCase(pocketed, topEdges(pocketed, -3), R"({"depth":0.1})");
    auto we = e.check();
    REQUIRE(we);
    REQUIRE_FALSE(we->empty());
    CHECK_THAT(we->front(), ContainsSubstring("lie at Z-3; they are engraved on the plane at Z0"));
}
