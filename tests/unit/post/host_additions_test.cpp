// The host additions the shipped posts needed (T-040): the output format (decimals, trailing-zero style, comment
// length), per-word feed decimals, the sequence-number wrap limit, the arc and helical plane capabilities, and the
// machine's control limits in ProgramContext. Each through the writer or the motion expander directly and through a
// script.
#include "engine.h"
#include "engine_motion.h"
#include "post_test_util.h"
#include "writer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;

namespace {

struct Collect final : MotionSink {
    std::vector<MotionEvent> events;
    bool onEvent(const MotionEvent& e) override
    {
        events.push_back(e);
        return true;
    }
};

std::vector<MotionEvent> expand(const cam::Toolpath& p, const MotionCaps& caps)
{
    Collect c;
    const auto r = expandSection(p, nullptr, caps, c);
    REQUIRE(r.has_value());
    REQUIRE(*r);
    return c.events;
}

std::string axisWord(Writer& w, char letter, std::int64_t n)
{
    std::string word;
    return w.axis(letter, n, word) ? word : std::string("(null)");
}

// A post whose handlers are the given script text: onProgramStart(ctx, out) gets `body`.
Result<PostOutput> runStart(const std::string& body, const std::string& caps = "", UnitSystem units = UnitSystem::Metric)
{
    const std::string source = "definePost({ title: 't', dialect: 'fanuc', fileExtension: '.nc', apiVersion: 1,\n"
                               "  capabilities: { arcs: 'IJK', helical: true, maxArcDegrees: 360, cannedCycles: [],\n"
                               "    rigidTap: false, cutterComp: false" +
                               caps +
                               " }, properties: {},\n"
                               "  onProgramStart(ctx, out) {\n" +
                               body +
                               "\n  },\n"
                               "  onRapid(m, out) { out.block('G0', out.axis('X', m.x), out.axis('Y', m.y), "
                               "out.axis('Z', m.z)); },\n"
                               "  onLinear(m, out) { out.block('G1', out.axis('X', m.x), out.axis('Y', m.y), "
                               "out.axis('Z', m.z), out.feed(m.feed)); },\n"
                               "  onArc(m, out) { out.block(m.cw ? 'G2' : 'G3', m.plane, out.axis('X', m.x), "
                               "out.axis('Y', m.y), out.axis('Z', m.z)); },\n"
                               "});\n";
    Program p;
    p.units = units;
    p.machine.maxBlockNumber = 99999;
    p.machine.maxProgramNameLength = 4;
    p.machine.controlDialect = "fanuc";
    cam::Toolpath& t = p.section("arcs", "contour2d", 1, makeTool("6 mm end mill", lib::ToolType::FlatEndMill, 6, 3));
    t.startX = mm(10);
    t.startY = 0;
    t.startZ = 0;
    // An XZ half circle and an XY helix (a quarter turn down 1 mm).
    t.moves.push_back(arc(true, -mm(10), 0, 0, 0, 0, 0, mmPerMin(500), cam::ArcPlane::XZ));
    t.moves.push_back(linear(mm(10), 0, 0, mmPerMin(500)));
    t.moves.push_back(arc(false, 0, mm(10), -mm(1), 0, 0, 0, mmPerMin(500)));
    QuickJsPostEngine engine;
    JobContext ctx;
    return engine.runSource(source, "t.post.js", p.job(), ctx);
}

} // namespace

TEST_CASE("setFormat: decimals and trailing-zero styles", "[post][writer][t040]")
{
    Writer w(WriterOptions{});
    CHECK(axisWord(w, 'X', 12'000'000) == "X12.");
    Writer::Format f = w.format();
    CHECK(f.axisDecimals == 3);
    CHECK(f.feedDecimals == 1);
    f.trailing = Writer::Trailing::One;
    w.setFormat(f);
    // The axes are forgotten: the same value is written again, now with one zero.
    CHECK(axisWord(w, 'X', 12'000'000) == "X12.0");
    CHECK(axisWord(w, 'Y', 12'500'000) == "Y12.5");
    CHECK(axisWord(w, 'Z', -250'000) == "Z-0.25");
    std::string word;
    REQUIRE(w.feed(800'000'000, word));
    CHECK(word == "F800.0");
    f.trailing = Writer::Trailing::All;
    f.axisDecimals = 2;
    f.feedDecimals = 0;
    w.setFormat(f);
    CHECK(axisWord(w, 'X', 12'000'000) == "X12.00");
    CHECK(axisWord(w, 'Y', 12'345'000) == "Y12.34"); // a tie at 2 decimals, half to even
    REQUIRE(w.feed(800'000'000, word));
    CHECK(word == "F800.");
    w.word("R", 2'000'000, w.coordinateFormat(), word, true);
    CHECK(word == "R2.00");
}

TEST_CASE("setFormat: rounding at the chosen decimals is half to even", "[post][writer][t040]")
{
    Writer w(WriterOptions{});
    Writer::Format f = w.format();
    f.axisDecimals = 2;
    w.setFormat(f);
    CHECK(axisWord(w, 'X', 12'345'000) == "X12.34");
    CHECK(axisWord(w, 'X', 12'355'000) == "X12.36");
    CHECK(axisWord(w, 'X', 12'356'000) == "(null)"); // rounds to the same 12.36: unchanged
}

TEST_CASE("feed decimals per word, for tapping feeds", "[post][writer][t040]")
{
    WriterOptions o;
    o.inch = true;
    Writer w(o);
    std::string word;
    // 35.714286 in/min (7/16-14 at 500 rpm) on the inch grid of 25.4e6 steps per inch.
    const std::int64_t n = 907'142'864;
    REQUIRE(w.feed(n, word, 4));
    CHECK(word == "F35.7143");
    CHECK_FALSE(w.feed(n, word, 4)); // the same word: modal
    REQUIRE(w.feed(n, word));        // other decimals: written again
    CHECK(word == "F35.71");
    CHECK_FALSE(w.feed(n, word));
}

TEST_CASE("comments are cut to commentMaxLength", "[post][writer][t040]")
{
    WriterOptions o;
    o.crlf = false;
    Writer w(o);
    Writer::Format f = w.format();
    f.commentMaxLength = 10;
    w.setFormat(f);
    REQUIRE(w.comment("pocket (rough) 12 mm deep"));
    REQUIRE(w.comment("short"));
    f.commentMaxLength = 0;
    w.setFormat(f);
    REQUIRE(w.comment("pocket (rough) 12 mm deep"));
    CHECK(w.take().text == "(POCKET ROU)\n(SHORT)\n(POCKET ROUGH 12 MM DEEP)\n");
}

TEST_CASE("arcs outside arcPlanes and helices outside helicalPlanes become lines", "[post][motion][t040]")
{
    cam::Toolpath p{};
    p.startX = mm(10);
    p.moves.push_back(arc(true, -mm(10), 0, 0, 0, 0, 0, mmPerMin(500), cam::ArcPlane::XZ)); // XZ half circle
    p.moves.push_back(linear(mm(10), 0, 0, mmPerMin(500)));
    p.moves.push_back(arc(false, 0, mm(10), -mm(1), 0, 0, 0, mmPerMin(500))); // XY helix
    p.moves.push_back(arc(false, -mm(10), 0, -mm(1), 0, 0, 0, mmPerMin(500))); // XY plain arc
    auto kinds = [](const std::vector<MotionEvent>& ev, std::uint32_t move) {
        std::vector<EventKind> out;
        for (const MotionEvent& e : ev) {
            if (e.move == move) {
                out.push_back(e.kind);
            }
        }
        return out;
    };
    MotionCaps all;
    auto ev = expand(p, all);
    CHECK(kinds(ev, 0) == std::vector<EventKind>{EventKind::Arc});
    CHECK(kinds(ev, 2) == std::vector<EventKind>{EventKind::Arc});

    MotionCaps xyOnly;
    xyOnly.arcPlanes = {true, false, false};
    xyOnly.helicalPlanes = {true, false, false};
    ev = expand(p, xyOnly);
    const auto xz = kinds(ev, 0);
    CHECK(xz.size() > 50); // a 10 mm half circle within 1 um
    for (const EventKind k : xz) {
        CHECK(k == EventKind::Linear);
    }
    CHECK(kinds(ev, 2) == std::vector<EventKind>{EventKind::Arc}); // XY helix allowed
    CHECK(kinds(ev, 3) == std::vector<EventKind>{EventKind::Arc});

    MotionCaps noHelix;
    noHelix.helicalPlanes = {false, true, true};
    ev = expand(p, noHelix);
    CHECK(kinds(ev, 0) == std::vector<EventKind>{EventKind::Arc});
    CHECK(kinds(ev, 2).size() > 10);
    CHECK(kinds(ev, 2).front() == EventKind::Linear);
    CHECK(kinds(ev, 3) == std::vector<EventKind>{EventKind::Arc}); // not helical
    // The linearised helix ends exactly at the IR end point.
    const MotionEvent* last = nullptr;
    for (const MotionEvent& e : ev) {
        last = e.move == 2 ? &e : last;
    }
    REQUIRE(last != nullptr);
    CHECK(last->x == 0);
    CHECK(last->y == mm(10));
    CHECK(last->z == -mm(1));
}

TEST_CASE("scripts: setFormat, feed decimals, sequence.max and the plane capabilities", "[post][engine][t040]")
{
    const auto out = runStart("out.setFormat({ axisDecimals: 2, trailingZeros: 'one', commentMaxLength: 8 });\n"
                              "out.comment('a long comment here');\n"
                              "out.block(out.axis('X', 1), out.word('R', 2), out.feed(35.714286, 4), "
                              "out.feed(35.714286, 4));\n"
                              "out.sequence.enabled = true; out.sequence.start = 99998; out.sequence.increment = 1;\n"
                              "out.block('A'); out.block('B'); out.sequence.max = 0; out.block('C');\n"
                              "out.sequence.max = 100000; out.block('D'); out.sequence.enabled = false;\n"
                              "out.setFormat({ commentMaxLength: 0 });\n"
                              "out.comment(ctx.machine.controlDialect + ' ' + ctx.machine.maxBlockNumber + ' ' + "
                              "ctx.machine.maxProgramNameLength + ' ' + out.sequence.max);",
                              ", arcPlanes: ['XY'], helicalPlanes: []");
    REQUIRE(out.has_value());
    const auto text = lines(out->text);
    REQUIRE(text.size() > 8);
    CHECK(text[0] == "(A LONG C)");
    CHECK(text[1] == "X1.0 R2.0 F35.7143"); // style "one"; the second, equal feed is modal
    CHECK(text[2] == "N99998 A");
    CHECK(text[3] == "N99999 B"); // the machine's maxBlockNumber
    CHECK(text[4] == "N100000 C"); // max 0: no limit
    CHECK(text[5] == "N99998 D");  // past max 100000: wraps to start
    CHECK(text[6] == "(FANUC 99999 4 100000)");
    // The XZ arc (arcPlanes XY only) and the XY helix (no helical plane) reached onLinear as lines.
    std::size_t g1 = 0;
    for (const std::string& line : text) {
        CHECK(line.rfind("G2", 0) != 0);
        CHECK(line.rfind("G3", 0) != 0);
        g1 += line.rfind("G1", 0) == 0 ? 1 : 0;
    }
    CHECK(g1 > 100);

    // Bad values name what is wrong.
    struct Bad {
        const char* body;
        const char* caps;
        const char* message;
    };
    const Bad bad[] = {
        {"", ", arcPlanes: ['XW']", "capabilities.arcPlanes: \"XW\" is not one of XY, XZ, YZ"},
        {"", ", helicalPlanes: 'XY'", "capabilities.helicalPlanes must be an array"},
        {"out.setFormat({ axisDecimals: 7 });", "", "axisDecimals must be a whole number from 0 to 6"},
        {"out.setFormat({ trailingZeros: 'none' });", "", "trailingZeros must be \"trim\", \"one\" or \"all\""},
        {"out.setFormat(3);", "", "out.setFormat: an object"},
        {"out.feed(1, 2.5);", "", "decimals must be a whole number from 0 to 6"},
        {"out.sequence.max = -1;", "", "start and max (0 or more)"},
    };
    for (const Bad& b : bad) {
        INFO(b.body << b.caps);
        const auto r = runStart(b.body, b.caps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find(b.message) != std::string::npos);
    }
}

TEST_CASE("inch output: setFormat decimals apply to inch words", "[post][engine][t040]")
{
    const auto out =
        runStart("out.setFormat({ axisDecimals: 5, feedDecimals: 3, trailingZeros: 'all' });\n"
                 "out.block(out.axis('X', 0.5), out.feed(12.5));",
                 "", UnitSystem::Inch);
    REQUIRE(out.has_value());
    CHECK(lines(out->text)[0] == "X0.50000 F12.500");
}
