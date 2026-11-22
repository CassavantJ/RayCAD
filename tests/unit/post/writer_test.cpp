// Writer (src/post/writer.h): the C++ behind the script's `out` object.
#include "format.h"
#include "writer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using ray::post::sanitizeComment;
using ray::post::Writer;
using ray::post::WriterOptions;

namespace {

WriterOptions lf()
{
    WriterOptions o;
    o.crlf = false;
    o.dialect = "generic-iso";
    return o;
}

bool block(Writer& w, std::initializer_list<std::string_view> words)
{
    std::vector<std::string_view> v(words);
    return w.block(v);
}

// A script's number on the writer's grid (mm output: 1e6 per mm; inch output: 25.4e6 per inch).
std::int64_t micro(double v, std::int64_t grid = ray::post::fmt::kMicroGrid)
{
    std::int64_t m = 0;
    ray::post::fmt::snapToGrid(v, m, grid);
    return m;
}
std::int64_t inch(double v)
{
    return micro(v, ray::post::fmt::kInchGrid);
}

} // namespace

TEST_CASE("block joins words, skips empty ones and writes the line ending", "[post][writer]")
{
    Writer w(lf());
    CHECK(block(w, {"G0", "", "X1.", "Y2."}));
    CHECK(block(w, {"", ""})); // nothing at all: no line
    auto out = w.take();
    CHECK(out.text == "G0 X1. Y2.\n");

    WriterOptions o = lf();
    o.crlf = true;
    Writer crlf(o);
    block(crlf, {"M30"});
    crlf.raw("%");
    CHECK(crlf.take().text == "M30\r\n%\r\n");
}

TEST_CASE("sequence numbers: start, increment, only on tool change, wrap at the machine's limit", "[post][writer]")
{
    WriterOptions o = lf();
    o.maxBlockNumber = 30;
    Writer w(o);
    block(w, {"G90"});
    w.setSequenceEnabled(true);
    w.setSequenceStart(10);
    w.setSequenceIncrement(10);
    block(w, {"G0", "X1."});
    w.comment("not numbered");
    w.raw("%");
    block(w, {"G1", "X2."});
    block(w, {"G1", "X3."});
    block(w, {"G1", "X4."}); // 40 > 30: back to the start
    w.setSequenceOnlyOnToolChange(true);
    block(w, {"G1", "X5."});
    w.setToolChangeBlockPending(true);
    block(w, {"T2", "M6"});
    block(w, {"G0", "X0."});
    CHECK(w.take().text == "G90\nN10 G0 X1.\n(NOT NUMBERED)\n%\nN20 G1 X2.\nN30 G1 X3.\nN10 G1 X4.\nG1 X5.\nN20 T2 M6\n"
                           "G0 X0.\n");
}

TEST_CASE("modal words, axes and feed are written only when they change", "[post][writer]")
{
    Writer w(lf());
    std::string word;
    CHECK(w.modal("motion", "G1"));
    CHECK_FALSE(w.modal("motion", "G1"));
    CHECK(w.modal("plane", "G17"));
    CHECK(w.modal("motion", "G2"));
    CHECK(w.axis('X', micro(12.3455), word));
    CHECK(word == "X12.346");
    CHECK_FALSE(w.axis('X', micro(12.3456), word)); // the same at 3 decimals
    CHECK(w.axis('X', micro(-0.0004), word));
    CHECK(word == "X0.");
    CHECK(w.axis('Z', micro(-5.0), word));
    CHECK(word == "Z-5.");
    CHECK(w.feed(micro(1200.0), word));
    CHECK(word == "F1200.");
    CHECK_FALSE(w.feed(micro(1200.04), word));
    CHECK(w.feed(micro(1200.06), word));
    CHECK(word == "F1200.1");

    w.reset("X");
    CHECK(w.axis('X', micro(-0.0004), word)); // reset: written again
    CHECK_FALSE(w.axis('Z', micro(-5.0), word));
    w.reset("axes");
    CHECK(w.axis('Z', micro(-5.0), word));
    w.reset("feed");
    CHECK(w.feed(micro(1200.06), word));
    w.reset("motion");
    CHECK(w.modal("motion", "G2"));
    CHECK_FALSE(w.modal("plane", "G17"));
    w.resetAll();
    CHECK(w.modal("plane", "G17"));
    CHECK(w.axis('Z', micro(-5.0), word));
    CHECK(w.feed(micro(1200.06), word));
    CHECK_FALSE(w.axis('?', 0, word)); // not an axis letter
}

TEST_CASE("inch output uses 4 coordinate decimals and 2 feed decimals", "[post][writer]")
{
    WriterOptions o = lf();
    o.inch = true;
    Writer w(o);
    std::string word;
    CHECK(w.axis('Y', inch(0.49215), word));
    CHECK(word == "Y0.4922"); // tie: 4921 odd, up
    CHECK(w.feed(inch(47.244), word));
    CHECK(word == "F47.24");
    // 5 mm is 0.19685039 in: 0.1969. Snapping to micro-inch first (196850) would round the tie down to 0.1968.
    CHECK(w.axis('Z', inch(5.0 / 25.4), word));
    CHECK(word == "Z0.1969");
    // Every whole nanometre from -2 mm to 2 mm in steps of 3 nm: the word equals integer rounding of nm / 2540.
    for (std::int64_t nm = -2'000'000; nm <= 2'000'000; nm += 3) {
        w.reset("X");
        REQUIRE(w.axis('X', inch(static_cast<double>(nm) / 25.4e6), word));
        std::string want = "X";
        ray::post::fmt::appendFixed(want, ray::post::fmt::divRoundHalfEven(nm, 2540), 4, true, true, true);
        if (word != want) {
            FAIL("nm " << nm << ": " << word << " != " << want);
        }
    }
}

TEST_CASE("comments are sanitised per dialect", "[post][writer]")
{
    CHECK(sanitizeComment("Rough pocket (2 mm stock); 50% feed", "fanuc") == "ROUGH POCKET 2 MM STOCK 50 FEED");
    CHECK(sanitizeComment("Rough pocket (2 mm stock)", "haas-ngc") == "Rough pocket 2 mm stock");
    CHECK(sanitizeComment("  tab\there  ", "generic-iso") == "TAB HERE");
    CHECK(sanitizeComment("ÃƒËœ12 FrÃƒÂ¤ser \xE2\x80\x94 dia", "generic-iso") == "12 FR SER DIA");
    CHECK(sanitizeComment("a.b,c-d+e/f*g=h:i#j_k", "mazak-eia") == "A.B,C-D+E/F*G=H:I#J_K");
    CHECK(sanitizeComment("((()))", "fanuc").empty());

    Writer w(lf());
    w.comment("((()))"); // nothing left: no line
    w.comment("op 1");
    CHECK(w.take().text == "(OP 1)\n");
}

TEST_CASE("raw writes one line per newline", "[post][writer]")
{
    Writer w(lf());
    w.raw("%\r\nO1001\nG90");
    w.raw("");
    CHECK(w.take().text == "%\nO1001\nG90\n\n");
}

TEST_CASE("words with a format", "[post][writer]")
{
    std::string word;
    const Writer w(lf());
    w.word("Q", micro(0.5), ray::post::NumberFormat{3, true, true, true, 1}, word);
    CHECK(word == "Q0.5");
    w.word("P", micro(1.25), ray::post::NumberFormat{3, false, false, true, 1}, word);
    CHECK(word == "P1.250");
    w.word("", micro(-3), ray::post::NumberFormat{0, true, false, true, 1}, word);
    CHECK(word == "-3");
    WriterOptions o = lf();
    o.inch = true;
    const Writer wi(o);
    wi.word("P", inch(0.25), ray::post::NumberFormat{6, true, false, true, 1}, word); // unitless numbers too
    CHECK(word == "P0.25");
    wi.word("Q", inch(0.1234565), ray::post::NumberFormat{6, true, false, true, 1}, word);
    CHECK(word == "Q0.123456"); // 6 decimals on the inch grid: n * 5 / 127, half to even
}

TEST_CASE("the output limit stops writing at the limit", "[post][writer]")
{
    WriterOptions o = lf();
    o.maxOutputBytes = 9;
    Writer w(o);
    CHECK(block(w, {"G0", "X1."})); // 7 bytes
    CHECK_FALSE(block(w, {"G1"}));  // 7 + 3 > 9
    CHECK(w.outputLimitHit());
    CHECK_FALSE(w.raw("x")); // once hit, always refused
    CHECK(w.take().text == "G0 X1.\n");
}

TEST_CASE("warnings are limited in count and length", "[post][writer]")
{
    WriterOptions o = lf();
    o.maxWarnings = 2;
    o.maxWarningBytes = 5;
    Writer w(o);
    CHECK(w.warn("one"));
    CHECK(w.warn("f\xC3\xA4hrt weiter")); // cut before byte 5, which is inside no character here
    CHECK_FALSE(w.warn("three"));
    const auto out = w.take();
    REQUIRE(out.warnings.size() == 2);
    CHECK(out.warnings[0] == "one");
    CHECK(out.warnings[1] == "f\xC3\xA4hr...");
}

TEST_CASE("the line map covers the lines written inside a move", "[post][writer]")
{
    Writer w(lf());
    block(w, {"G90"});
    w.enterMove(0, 0);
    block(w, {"G0", "X1."});
    w.leaveMove();
    w.enterMove(1, 7);
    block(w, {"G1", "X2."});
    w.comment("two lines for one move");
    w.leaveMove();
    block(w, {"M30"});
    const auto out = w.take();
    REQUIRE(out.lineMap.size() == 3);
    CHECK(out.lineMap[0].line == 1);
    CHECK(out.lineMap[0].sectionIndex == 0);
    CHECK(out.lineMap[0].moveIndex == 0);
    CHECK(out.lineMap[1].line == 2);
    CHECK(out.lineMap[1].sectionIndex == 1);
    CHECK(out.lineMap[1].moveIndex == 7);
    CHECK(out.lineMap[2].line == 3);
}

TEST_CASE("PostOutput::hash covers text, line map and warnings", "[post][writer]")
{
    auto make = [](std::string_view extra, bool map, bool warn) {
        Writer w(lf());
        if (map) {
            w.enterMove(0, 1);
        }
        std::vector<std::string_view> words{"G0", extra};
        w.block(words);
        if (warn) {
            w.warn("w");
        }
        return w.take().hash();
    };
    const auto base = make("X1.", false, false);
    CHECK(base == make("X1.", false, false));
    CHECK(base != make("X2.", false, false));
    CHECK(base != make("X1.", true, false));
    CHECK(base != make("X1.", false, true));
}
