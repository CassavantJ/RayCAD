// T-043: every golden program of every shipped post, read back by the independent interpreter (gcode_interp.h) and
// compared with the toolpaths it was posted from (roundtrip_compare.h); the same for the pairwise property
// combinations of the shipped test; and in-memory mutations of the goldens that the round trip must catch.
//
// No golden has been run on a control yet (docs/post_review/README.md). Where the interpreter disagrees with a golden,
// the disagreement is a finding: it is listed in kKnownFindings below with the documentation it rests on, the tests
// pass while it is still seen, and fail when a new one appears or a listed one is gone (then remove it from the list).
#include "gcode_interp.h"
#include "roundtrip_compare.h"
#include "shipped_matrix.h"

#include <ray/cam.h>
#include <ray/post.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;
using namespace ray::post::test::shipped;

namespace {

// ---------- configuring the interpreter for a post's control ----------
// The coolant codes the control documentation gives when the program's coolantCodes property is not set: RS274/NGC
// M7 mist, M8 flood, M9 off [N 3.6.4] (FANUC: the same codes, builder-specific [F1, ASSUMPTION]); Haas M8/M9 flood,
// M88/M89 through-spindle, M83/M84 air [H1 p.20-21, p.124-129 per docs/post_review/haas-ngc.md].
std::string defaultCoolant(const std::string& post)
{
    if (post.rfind("haas", 0) == 0) {
        return "flood=M8/M9 through=M88/M89 air=M83/M84";
    }
    if (isMazak(post)) {
        return "flood=M8/M9"; // [ASSUMPTION: the Mazak post's default, mazak-smooth.md; M not read]
    }
    return "flood=M8/M9 mist=M7/M9";
}

std::string prop(const std::map<std::string, std::string>& props, const std::string& key, const std::string& def)
{
    auto it = props.find(key);
    return it == props.end() ? def : it->second;
}

rtgc::Dialect dialectFor(const std::string& post, const Program& program, int mazakReading = 0)
{
    rtgc::Dialect d;
    d.name = post;
    if (isMazak(post)) {
        // No Mazak manual: the shop's statements (properties) say what the control does; the rest is read two ways.
        d.family = rtgc::Family::Mazak;
        d.mazakReading = mazakReading;
        d.mazakM6RetractsZ = prop(program.properties, "m6LeavesZRetracted", "false") == "true";
        d.mazakDwellMilliseconds = prop(program.properties, "dwellUnits", "milliseconds") == "milliseconds";
    } else if (post == "fanuc") {
        d.family = rtgc::Family::Fanuc;
    } else if (post == "haas-ngc" || post == "haas-classic") {
        d.family = rtgc::Family::Haas;
        d.haasClassic = post == "haas-classic";
    } else {
        d.family = rtgc::Family::Ngc;
        // The generic post's dwellUnits = milliseconds describes a control that reads P as milliseconds.
        d.ngcDwellMilliseconds = prop(program.properties, "dwellUnits", "seconds") == "milliseconds";
    }
    const std::string coolant = prop(program.properties, "coolantCodes", defaultCoolant(post));
    REQUIRE(rtgc::parseCoolantCodes(coolant, d.coolant));
    // The control's peck clearance parameter, assumed set to what the toolpath asks for (cam::peckClearanceNm).
    for (const cam::Toolpath& path : program.paths) {
        for (const cam::Move& m : path.moves) {
            if (m.kind == cam::MoveKind::CyclePeck || m.kind == cam::MoveKind::CycleChipBreak) {
                d.peckClearanceMm = static_cast<double>(cam::peckClearanceNm(m, &program.machine)) / 1e6;
                return d;
            }
        }
    }
    return d;
}

unsigned coolantBits(cam::CuttingData::Coolant c)
{
    using C = cam::CuttingData::Coolant;
    switch (c) {
    case C::Flood:
        return rtgc::kFlood;
    case C::Mist:
        return rtgc::kMist;
    case C::ThroughTool:
        return rtgc::kThrough;
    case C::Air:
        return rtgc::kAir;
    case C::Off:
        return 0;
    }
    return 0;
}

rtgc::Expectation expectationFor(const Program& program, const std::vector<std::string>& warnings)
{
    rtgc::Expectation e;
    for (std::size_t i = 0; i < program.ops.size(); ++i) {
        const cam::Operation& op = program.ops[i];
        rtgc::ExpectedSection s;
        s.name = op.name;
        s.path = &program.paths[i];
        s.tool = op.tool.toolNumber;
        s.lengthOffset = op.tool.lengthOffset;
        s.diameterOffset = op.tool.diameterOffset;
        s.rpm = op.cutting.spindleRpm;
        s.clockwise = op.cutting.clockwise;
        s.coolant = coolantBits(op.cutting.coolant);
        s.wcs = rtgc::workOffsetName(program.setup.wcs.workOffset);
        e.sections.push_back(s);
    }
    e.machine = &program.machine;
    if (prop(program.properties, "peckReduction", "false") == "true") {
        e.peckReductionJ = std::stod(prop(program.properties, "peckReductionJ", "0.25"));
        e.peckMinimumK = std::stod(prop(program.properties, "peckMinimumK", "0.25"));
    }
    e.warnings = warnings;
    return e;
}

// ---------- one round trip ----------
enum class Cat { Words, Path, Arcs, Cycles, Rapids, Safety };

const char* catName(Cat c)
{
    switch (c) {
    case Cat::Words:
        return "interpreter";
    case Cat::Path:
        return "end points, feeds and machine state";
    case Cat::Arcs:
        return "arcs";
    case Cat::Cycles:
        return "canned cycles";
    case Cat::Rapids:
        return "added rapids";
    case Cat::Safety:
        return "safety";
    }
    return "?";
}

Cat categoryOf(const std::string& code, bool fromTraceErrors, bool fromSafety)
{
    if (fromTraceErrors) {
        return Cat::Words;
    }
    if (fromSafety) {
        return Cat::Safety;
    }
    if (code.rfind("rt-arc", 0) == 0) {
        return Cat::Arcs;
    }
    if (code == "rt-cycle-return" || code == "rt-dwell" || code == "rt-peck-clearance-silent") {
        return Cat::Cycles;
    }
    if (code == "rt-unsafe-rapid") {
        return Cat::Rapids;
    }
    return Cat::Path;
}

struct Found {
    Cat cat;
    rtgc::Issue issue;
    std::string text; // describe()
    std::string lineText;
};

struct Outcome {
    std::vector<Found> found;
    std::size_t segments = 0;
};

// The names of the two readings of a Mazak program (gcode_interp.h, Dialect::mazakReading).
const char* readingName(int reading)
{
    return reading == 0 ? "[Mazak reading 0, FANUC-compatible] " : "[Mazak reading 1, RS274/NGC-like] ";
}

// Interprets and compares one program. A Mazak program is read both ways (no Mazak manual decides between them) and
// must pass under each; its issues say which reading found them.
Outcome roundTrip(const std::string& post, const std::string& text, const Program& program,
                  const std::vector<std::string>& warnings)
{
    Outcome o;
    const int readings = isMazak(post) ? 2 : 1;
    for (int reading = 0; reading < readings; ++reading) {
        const rtgc::Dialect d = dialectFor(post, program, reading);
        const rtgc::Trace t = rtgc::interpret(text, d);
        o.segments = std::max(o.segments, t.segs.size());
        const std::string tag = isMazak(post) ? readingName(reading) : "";
        auto add = [&](const rtgc::Issue& i, bool err, bool saf) {
            rtgc::Issue tagged = i;
            tagged.text = tag + i.text;
            Found f{categoryOf(i.code, err, saf), tagged, rtgc::describe(tagged, t.lines), {}};
            if (i.line > 0 && static_cast<std::size_t>(i.line) <= t.lines.size()) {
                f.lineText = t.lines[static_cast<std::size_t>(i.line) - 1];
            }
            o.found.push_back(f);
        };
        for (const rtgc::Issue& i : t.errors) {
            add(i, true, false);
        }
        for (const rtgc::Issue& i : t.safety) {
            add(i, false, true);
        }
        for (const rtgc::Issue& i : rtgc::compare(t, expectationFor(program, warnings))) {
            add(i, false, false);
        }
    }
    return o;
}
std::vector<std::string> warningLines(const std::string& text)
{
    std::vector<std::string> out;
    for (const std::string& l : lines(text)) {
        if (!l.empty()) {
            out.push_back(l);
        }
    }
    return out;
}

// ---------- the known findings ----------
// Each disagreement between the interpreter and a golden that has been investigated: the post, the golden file
// (without _mm/_inch: both are meant; "pairwise" for the property combinations), the issue code, a piece of the
// issue's text (the program line it names and the message; "" for any), and why, with the documentation reference.
struct Known {
    const char* post;
    const char* file;
    const char* code;
    const char* lineText;
    const char* why;
};

const std::vector<Known>& knownFindings()
{
    // F5: FANUC with toolChangeRetract = none, at the first tool change.
    static const char* const kNoRetract =
        "With toolChangeRetract = none (which the FANUC and generic posts now refuse unless the property "
        "m6LeavesZRetracted asserts that the machine's M6 retracts; the pairwise set asserts it) the posts write no "
        "retract before the first M6, so the first X/Y move "
        "('G0 G90 G54 G17 X.. Y..') runs at the Z the machine happened to be at when the program started. Whether "
        "M6 retracts is the builder's macro [F1, unverified; docs/post_review/fanuc.md question 2]; RS274/NGC's M6 "
        "leaves the axes where they are [N 3.6.3]. (Haas M06 retracts Z itself [H1 p.19], so the Haas posts are "
        "safe with none.)";
    static const std::vector<Known> k = {
        {"fanuc", "pairwise", "rt-unsafe-rapid", "at Z unknown", kNoRetract},
        {"fanuc", "pairwise", "safety-xy-rapid-z-unknown", "G17 X0", kNoRetract},
        {"generic-iso", "pairwise", "rt-unsafe-rapid", "at Z unknown", kNoRetract},
        {"generic-iso", "pairwise", "safety-xy-rapid-z-unknown", "G17 X0", kNoRetract},
    };
    return k;
}

std::string stemOf(const std::string& file)
{
    const std::size_t u = file.rfind('_');
    return u == std::string::npos ? file : file.substr(0, u);
}

const Known* knownFor(const std::string& post, const std::string& file, const Found& f)
{
    for (const Known& k : knownFindings()) {
        if (post == k.post && (std::string(k.file) == "*" || stemOf(file) == k.file || file == k.file) &&
            f.issue.code == k.code && f.text.find(k.lineText) != std::string::npos) {
            return &k;
        }
    }
    return nullptr;
}

struct GoldenResult {
    std::string post, file;
    Outcome outcome;
};

// Every golden of every post, read back once.
const std::vector<GoldenResult>& goldenResults()
{
    static const std::vector<GoldenResult> results = [] {
        std::vector<GoldenResult> out;
        for (const Run& r : goldenRuns()) {
            const std::string text = readFile(goldenPath(r.post, r.file));
            REQUIRE_FALSE(text.empty());
            const std::string warnFile = r.file.substr(0, r.file.size() - 3) + ".warnings.txt";
            out.push_back(
                GoldenResult{r.post, r.file,
                             roundTrip(r.post, text, r.program, warningLines(readFile(goldenPath(r.post, warnFile))))});
        }
        return out;
    }();
    return results;
}

// Fails on every issue of category `cat` that is not a known finding; returns the known ones seen.
void checkCategory(Cat cat, std::set<const Known*>* seen = nullptr)
{
    std::size_t checked = 0;
    for (const GoldenResult& g : goldenResults()) {
        INFO(g.post << "/" << g.file);
        CHECK(g.outcome.segments > 5);
        ++checked;
        for (const Found& f : g.outcome.found) {
            if (f.cat != cat) {
                continue;
            }
            const Known* k = knownFor(g.post, g.file, f);
            if (k == nullptr) {
                FAIL_CHECK(g.post << "/" << g.file << ": " << f.text);
            } else if (seen != nullptr) {
                seen->insert(k);
            }
        }
    }
    CHECK(checked >= 4 * 17 * 2);
}

} // namespace

TEST_CASE("round trip: the interpreter accepts every word of every golden", "[post][roundtrip]")
{
    // "No unknown word": every word, code and form of every golden is one the interpreter knows for that control.
    checkCategory(Cat::Words);
}

TEST_CASE("round trip: end points, feeds, tools, offsets, spindle, coolant and work offset match the toolpath",
          "[post][roundtrip]")
{
    checkCategory(Cat::Path);
}

TEST_CASE("round trip: arcs match in centre, direction, plane and sweep", "[post][roundtrip]")
{
    checkCategory(Cat::Arcs);
}

TEST_CASE("round trip: canned cycles drill the toolpath's holes", "[post][roundtrip]")
{
    checkCategory(Cat::Cycles);
}

TEST_CASE("round trip: every rapid the post adds is a safe move", "[post][roundtrip]")
{
    checkCategory(Cat::Rapids);
}

TEST_CASE("round trip: the machine-level safety facts hold in every golden", "[post][roundtrip]")
{
    checkCategory(Cat::Safety);
}

TEST_CASE("round trip: every known finding is still seen", "[post][roundtrip][findings]")
{
    std::set<const Known*> seen;
    for (const GoldenResult& g : goldenResults()) {
        for (const Found& f : g.outcome.found) {
            if (const Known* k = knownFor(g.post, g.file, f)) {
                seen.insert(k);
            }
        }
    }
    for (const Known& k : knownFindings()) {
        if (std::string(k.file) == "pairwise") {
            continue;
        }
        INFO(k.post << "/" << k.file << " " << k.code << " '" << k.lineText << "'");
        CHECK(seen.count(&k) == 1); // gone: the post or the interpreter changed; remove the entry
        if (seen.count(&k) != 0) {
            WARN("finding " << k.post << "/" << k.file << " [" << k.code << "] '" << k.lineText << "': " << k.why);
        }
    }
}

// Not run by default: prints every issue of every golden, one per line, known or not (for investigating).
TEST_CASE("round trip: print every issue", "[.][roundtrip-dump]")
{
    for (const GoldenResult& g : goldenResults()) {
        for (const Found& f : g.outcome.found) {
            std::printf("%s/%s [%s]%s %s\n", g.post.c_str(), g.file.c_str(), catName(f.cat),
                        knownFor(g.post, g.file, f) != nullptr ? " (known)" : "", f.text.c_str());
        }
    }
}

// ---------- the pairwise property combinations ----------
TEST_CASE("round trip: every pairwise property combination of the realistic program reads back", "[post][roundtrip]")
{
    std::set<const Known*> seen;
    std::size_t runs = 0;
    for (const char* id : kPosts) {
        QuickJsPostEngine engine;
        const auto info = engine.inspect(shippedPost(id));
        REQUIRE(info.has_value());
        for (const auto& combo : pairwiseCombinations(*info)) {
            std::string label;
            for (const auto& [k, v] : combo) {
                label += k + "=" + v + " ";
            }
            INFO(id << " " << label);
            Program p = realistic();
            p.machine = machineFor(id);
            p.properties = combo;
            const auto out = postRun(id, p, std::string("2026-11-22"));
            if (!out) {
                FAIL_CHECK(out.error().message);
                continue;
            }
            ++runs;
            const Outcome o = roundTrip(id, out->text, p, out->warnings);
            CHECK(o.segments > 50);
            for (const Found& f : o.found) {
                const Known* k = knownFor(id, "pairwise", f);
                if (k == nullptr) {
                    k = knownFor(id, "20-realistic_mm.nc", f); // the realistic golden's findings recur here
                }
                if (k == nullptr) {
                    FAIL_CHECK(id << " [" << catName(f.cat) << "] " << f.text);
                } else {
                    seen.insert(k);
                }
            }
        }
    }
    CHECK(runs >= 4 * 4);
    for (const Known& k : knownFindings()) {
        if (std::string(k.file) == "pairwise") {
            INFO(k.post << " pairwise " << k.code << " '" << k.lineText << "'");
            CHECK(seen.count(&k) == 1);
        }
    }
}

// ---------- mutations: the round trip can fail ----------
namespace {

// The words of a line, comments left out (a mutated line loses its comment).
std::vector<std::string> tokens(const std::string& raw)
{
    std::string line;
    int depth = 0;
    for (const char c : raw) {
        if (c == '(') {
            ++depth;
            line += ' ';
        } else if (c == ')') {
            depth = std::max(depth - 1, 0);
        } else if (depth == 0) {
            line += c;
        }
    }
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ') {
            ++j;
        }
        if (j > i) {
            out.push_back(line.substr(i, j - i));
        }
        i = j;
    }
    return out;
}
std::string join(const std::vector<std::string>& t)
{
    std::string s;
    for (const std::string& x : t) {
        s += (s.empty() ? "" : " ") + x;
    }
    return s;
}
bool isG(const std::string& tok, int g)
{
    if (tok.size() < 2 || tok[0] != 'G') {
        return false;
    }
    for (std::size_t i = 1; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9') {
            return false;
        }
    }
    return std::stoi(tok.substr(1)) == g;
}
bool isM(const std::string& tok, int m)
{
    if (tok.size() < 2 || tok[0] != 'M') {
        return false;
    }
    for (std::size_t i = 1; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9') {
            return false;
        }
    }
    return std::stoi(tok.substr(1)) == m;
}
bool has(const std::vector<std::string>& t, const std::function<bool(const std::string&)>& pred)
{
    return std::any_of(t.begin(), t.end(), pred);
}
// The number of a word, re-written with the same number of decimals after `f` changed it.
std::string rewrite(const std::string& tok, const std::function<double(double)>& f)
{
    const std::string num = tok.substr(1);
    const std::size_t dot = num.find('.');
    const int decimals = dot == std::string::npos ? 0 : static_cast<int>(num.size() - dot - 1);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, f(std::stod(num)));
    std::string s = buf;
    if (dot != std::string::npos && decimals == 0) {
        s += ".";
    }
    return tok.substr(0, 1) + s;
}

struct Mutation {
    const char* name;
    const char* golden; // stem; _mm.nc is used
    // Changes the program; returns the 1-based line the round trip must name (0: a program-level issue is enough),
    // or -1 when the mutation does not apply to this post's golden.
    std::function<int(std::vector<std::string>&, const std::string& post)> apply;
};

// The first line (index) whose tokens satisfy pred, or -1.
int firstLine(const std::vector<std::string>& lines, const std::function<bool(const std::vector<std::string>&)>& pred,
              std::size_t from = 0)
{
    for (std::size_t i = from; i < lines.size(); ++i) {
        if (pred(tokens(lines[i]))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
int lastLine(const std::vector<std::string>& lines, const std::function<bool(const std::vector<std::string>&)>& pred)
{
    for (std::size_t i = lines.size(); i-- > 0;) {
        if (pred(tokens(lines[i]))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
// The first feed move (G1 block, or a block in G1 mode with an axis word) at or after `from`.
int firstFeedAfter(const std::vector<std::string>& lines, std::size_t from)
{
    bool g1 = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto t = tokens(lines[i]);
        for (const std::string& x : t) {
            if (isG(x, 1) || isG(x, 2) || isG(x, 3)) {
                g1 = true;
            } else if (isG(x, 0) || isG(x, 80) || isG(x, 81) || isG(x, 82) || isG(x, 83) || isG(x, 84) || isG(x, 85) ||
                       isG(x, 89) || isG(x, 73) || isG(x, 74)) {
                g1 = isG(x, 81) || isG(x, 82) || isG(x, 83) || isG(x, 84) || isG(x, 85) || isG(x, 89) || isG(x, 73) ||
                     isG(x, 74);
            }
        }
        const bool axis = has(t, [](const std::string& x) { return x[0] == 'X' || x[0] == 'Y' || x[0] == 'Z'; });
        if (i >= from && g1 && axis) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
auto tokStarts(char c)
{
    return [c](const std::string& x) {
        return !x.empty() && x[0] == c;
    };
}
// Replaces the first token satisfying pred in line i.
void replaceTok(std::vector<std::string>& lines, int i, const std::function<bool(const std::string&)>& pred,
                const std::function<std::string(const std::string&)>& with)
{
    auto t = tokens(lines[static_cast<std::size_t>(i)]);
    for (std::string& x : t) {
        if (pred(x)) {
            x = with(x);
            break;
        }
    }
    lines[static_cast<std::size_t>(i)] = join(t);
}

std::vector<Mutation> mutations()
{
    using L = std::vector<std::string>;
    using T = std::vector<std::string>;
    return {
        {"flip an arc direction", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 3); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(
                 l, i, [](const std::string& x) { return isG(x, 3); },
                 [](const std::string& x) { return x.size() == 3 ? std::string("G02") : std::string("G2"); });
             return i + 1;
         }},
        // RS274/NGC reads X37071 as 37071 mm; FANUC and Haas read a number without a point as least increments, so
        // there X37.071 and X37071 are the same and the mutation takes a whole number instead: X30. becomes 0.030.
        {"drop a decimal point", "01-arcs-xy",
         [](L& l, const std::string& post) {
             const bool ngc = post == "generic-iso";
             auto frac = [ngc](const std::string& x) {
                 const std::size_t d = x.find('.');
                 if ((x[0] != 'X' && x[0] != 'Y') || d == std::string::npos) {
                     return false;
                 }
                 const bool fractional = d + 1 < x.size() && x.find_first_not_of('0', d + 1) != std::string::npos;
                 return ngc ? fractional : !fractional && std::stod(x.substr(1)) != 0;
             };
             const int i = firstLine(l, [&](const T& t) { return has(t, frac); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, frac, [](const std::string& x) {
                 std::string y = x;
                 y.erase(y.find('.'), 1);
                 return y;
             });
             return i + 1;
         }},
        {"change an R plane", "09-spot-drill",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 81); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('R'),
                        [](const std::string& x) { return rewrite(x, [](double v) { return v - 1; }); });
             return i + 1;
         }},
        {"swap G98 and G99", "09-spot-drill",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 99); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(
                 l, i, [](const std::string& x) { return isG(x, 99); },
                 [](const std::string&) { return std::string("G98"); });
             return i + 1;
         }},
        {"drop G43", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 43); }); });
             if (i < 0) {
                 return -1;
             }
             T t = tokens(l[static_cast<std::size_t>(i)]);
             t.erase(std::remove_if(t.begin(), t.end(),
                                    [](const std::string& x) { return isG(x, 43) || (!x.empty() && x[0] == 'H'); }),
                     t.end());
             l[static_cast<std::size_t>(i)] = join(t);
             return i + 1;
         }},
        {"change the dwell P units", "10-drill-dwell",
         [](L& l, const std::string& post) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 82); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('P'), [&](const std::string& x) {
                 if (x.find('.') != std::string::npos) { // seconds, written as milliseconds
                     const double s = std::stod(x.substr(1));
                     return "P" + std::to_string(static_cast<int>(std::lround(s * 1000))) +
                            (post.rfind("haas", 0) == 0 ? "." : "");
                 }
                 const double ms = std::stod(x.substr(1)); // milliseconds, written as seconds
                 char buf[32];
                 std::snprintf(buf, sizeof buf, "P%.3f", ms / 1000);
                 return std::string(buf);
             });
             return i + 1;
         }},
        {"delete a G80", "09-spot-drill",
         [](L& l, const std::string&) {
             const int i = firstLine(
                 l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 80); }) && t.size() <= 2; },
                 10);
             if (i < 0) {
                 return -1;
             }
             l.erase(l.begin() + i);
             return i + 1; // the next block now has this line number
         }},
        {"swap G41 and G42", "05-cutter-comp",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 41); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(
                 l, i, [](const std::string& x) { return isG(x, 41); },
                 [](const std::string&) { return std::string("G42"); });
             return i + 1;
         }},
        {"change the tool number of M6", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isM(x, 6); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('T'),
                        [](const std::string& x) { return "T" + std::to_string(std::stoi(x.substr(1)) + 1); });
             return firstFeedAfter(l, static_cast<std::size_t>(i)) + 1;
         }},
        {"change a feed", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i = firstLine(l, [](const T& t) {
                 return has(t, [](const std::string& x) { return isG(x, 3); }) && has(t, tokStarts('F'));
             });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('F'),
                        [](const std::string& x) { return rewrite(x, [](double v) { return v / 2; }); });
             return i + 1;
         }},
        {"retract to work Z0 instead of machine zero", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i = lastLine(l, [](const T& t) {
                 return has(t, [](const std::string& x) { return isG(x, 53) || isG(x, 28); }) && has(t, tokStarts('Z'));
             });
             if (i < 0) {
                 return -1;
             }
             T t = tokens(l[static_cast<std::size_t>(i)]);
             t.erase(std::remove_if(t.begin(), t.end(), [](const std::string& x) { return isG(x, 53) || isG(x, 91); }),
                     t.end());
             l[static_cast<std::size_t>(i)] = join(t);
             return i + 1;
         }},
        {"drop M3", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isM(x, 3); }); });
             if (i < 0) {
                 return -1;
             }
             T t = tokens(l[static_cast<std::size_t>(i)]);
             t.erase(std::remove_if(t.begin(), t.end(), [](const std::string& x) { return isM(x, 3); }), t.end());
             l[static_cast<std::size_t>(i)] = join(t);
             return firstFeedAfter(l, static_cast<std::size_t>(i)) + 1;
         }},
        {"change the peck Q", "11-peck",
         [](L& l, const std::string&) {
             const int i = firstLine(l, [](const T& t) { return has(t, tokStarts('Q')); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('Q'),
                        [](const std::string& x) { return rewrite(x, [](double v) { return v * 1.5; }); });
             return i + 1;
         }},
        {"flip the sign of an arc centre offset", "01-arcs-xy",
         [](L& l, const std::string&) {
             auto nonzeroI = [](const std::string& x) {
                 return x.size() > 1 && x[0] == 'I' && std::stod(x.substr(1)) != 0;
             };
             const int i = firstLine(l, [&](const T& t) { return has(t, nonzeroI); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, nonzeroI, [](const std::string& x) { return rewrite(x, [](double v) { return -v; }); });
             return i + 1;
         }},
        {"turn the coolant off", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isM(x, 8); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(
                 l, i, [](const std::string& x) { return isM(x, 8); },
                 [](const std::string& x) { return x.size() == 3 ? std::string("M09") : std::string("M9"); });
             return firstFeedAfter(l, static_cast<std::size_t>(i)) + 1;
         }},
        {"change the H number", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 43); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('H'),
                        [](const std::string& x) { return "H" + std::to_string(std::stoi(x.substr(1)) + 1); });
             return firstFeedAfter(l, static_cast<std::size_t>(i)) + 1;
         }},
        {"change the work offset", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 54); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(
                 l, i, [](const std::string& x) { return isG(x, 54); },
                 [](const std::string&) { return std::string("G55"); });
             return firstFeedAfter(l, static_cast<std::size_t>(i)) + 1;
         }},
        {"change a helix depth", "03-helical",
         [](L& l, const std::string&) {
             const int i = firstLine(l, [](const T& t) {
                 return has(t, [](const std::string& x) { return isG(x, 3); }) && has(t, tokStarts('Z'));
             });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('Z'),
                        [](const std::string& x) { return rewrite(x, [](double v) { return v - 0.5; }); });
             return i + 1;
         }},
        {"delete a cutting line", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 3); }); });
             if (i < 0) {
                 return -1;
             }
             l.erase(l.begin() + i);
             return i + 1;
         }},
        {"negate an arc radius (the complement arc)", "*arcs-r",
         [](L& l, const std::string&) {
             const int i = firstLine(l, [](const T& t) {
                 return has(t, [](const std::string& x) { return isG(x, 3) || isG(x, 2); }) && has(t, tokStarts('R'));
             });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('R'), [](const std::string& x) { return "R-" + x.substr(1); });
             return i + 1;
         }},
        {"change the tapping feed", "15-tap",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isG(x, 84); }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('F'),
                        [](const std::string& x) { return rewrite(x, [](double v) { return v * 1.25; }); });
             return i + 1;
         }},
        {"move an end point by two least counts (inch)", "01-arcs-xy_inch",
         [](L& l, const std::string&) {
             const int i = firstLine(l, [](const T& t) {
                 return has(t, [](const std::string& x) { return isG(x, 1); }) && has(t, tokStarts('Z'));
             });
             if (i < 0) {
                 return -1;
             }
             replaceTok(l, i, tokStarts('Z'), [](const std::string& x) {
                 const std::size_t d = x.find('.');
                 const int decimals = d == std::string::npos ? 0 : static_cast<int>(x.size() - d - 1);
                 return rewrite(x, [decimals](double v) { return v - 2 * std::pow(10.0, -decimals); });
             });
             return i + 1;
         }},
        {"swap synchronous right- and left-hand tapping (G84.2 / G84.3)", "30-tap-g84-2",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return x == "G84.2"; }); });
             if (i < 0) {
                 return -1;
             }
             replaceTok(
                 l, i, [](const std::string& x) { return x == "G84.2"; },
                 [](const std::string&) { return std::string("G84.3"); });
             return i + 1;
         }},
        {"swap the tool and the next tool of T<n> T<m> M6", "33-preselect-same-block",
         [](L& l, const std::string&) {
             const int i = firstLine(l, [](const T& t) {
                 return std::count_if(t.begin(), t.end(),
                                      [](const std::string& x) { return !x.empty() && x[0] == 'T'; }) == 2;
             });
             if (i < 0) {
                 return -1;
             }
             T t = tokens(l[static_cast<std::size_t>(i)]);
             std::vector<std::size_t> at;
             for (std::size_t k = 0; k < t.size(); ++k) {
                 if (!t[k].empty() && t[k][0] == 'T') {
                     at.push_back(k);
                 }
             }
             std::swap(t[at[0]], t[at[1]]);
             l[static_cast<std::size_t>(i)] = join(t);
             return firstFeedAfter(l, static_cast<std::size_t>(i)) + 1;
         }},
        {"delete M30", "01-arcs-xy",
         [](L& l, const std::string&) {
             const int i =
                 firstLine(l, [](const T& t) { return has(t, [](const std::string& x) { return isM(x, 30); }); });
             if (i < 0) {
                 return -1;
             }
             l.erase(l.begin() + i);
             return 0;
         }},
    };
}

std::string goldenFileFor(const std::string& post, const std::string& stem)
{
    if (stem == "*arcs-r") {
        for (const Run& r : goldenRuns()) {
            if (r.post == post && r.file.find("arcs-r_mm") != std::string::npos) {
                return r.file;
            }
        }
        return {};
    }
    return stem + (stem.find("_inch") != std::string::npos ? ".nc" : "_mm.nc");
}

} // namespace

namespace {

struct MutationResult {
    std::string post, file, name;
    int expectLine = 0;
    bool caught = false;
    std::string by;  // the issue that names the line
    std::string all; // every new issue
};

// Every mutation on every post's golden where it applies; each is caught when the round trip reports a new issue
// (one the unmutated golden does not have) on the line the mutation names.
std::vector<MutationResult> runMutations()
{
    std::vector<MutationResult> out;
    const std::vector<Run> runs = goldenRuns();
    for (const char* post : kPosts) {
        for (const Mutation& m : mutations()) {
            const std::string file = goldenFileFor(post, m.golden);
            const auto run =
                std::find_if(runs.begin(), runs.end(), [&](const Run& r) { return r.post == post && r.file == file; });
            if (file.empty() || run == runs.end()) {
                continue;
            }
            const std::string text = readFile(goldenPath(post, file));
            std::vector<std::string> l = lines(text);
            MutationResult r{post, file, m.name, m.apply(l, post), false, {}, {}};
            if (r.expectLine < 0) {
                continue;
            }
            std::string mutated;
            for (const std::string& x : l) {
                mutated += x + "\n";
            }
            const std::string warnFile = file.substr(0, file.size() - 3) + ".warnings.txt";
            const auto warnings = warningLines(readFile(goldenPath(post, warnFile)));
            const Outcome base = roundTrip(post, text, run->program, warnings);
            const Outcome o = roundTrip(post, mutated, run->program, warnings);
            std::set<std::string> before;
            for (const Found& f : base.found) {
                before.insert(f.issue.code + "|" + f.issue.text);
            }
            for (const Found& f : o.found) {
                if (before.count(f.issue.code + "|" + f.issue.text) != 0) {
                    continue;
                }
                r.all += "\n  " + f.text;
                if (!r.caught && (r.expectLine == 0 || f.issue.line == r.expectLine)) {
                    r.caught = true;
                    r.by = f.text;
                }
            }
            out.push_back(r);
        }
    }
    return out;
}

} // namespace

TEST_CASE("round trip: mutations of the goldens are caught, naming the line", "[post][roundtrip][mutation]")
{
    std::map<std::string, int> caught;
    for (const MutationResult& r : runMutations()) {
        INFO(r.post << "/" << r.file << ": " << r.name << " (expected at line " << r.expectLine << ")");
        INFO("new issues:" << r.all);
        CHECK(r.caught);
        caught[r.post] += r.caught ? 1 : 0;
    }
    // At least ten distinct mutations caught per control family, for each of the four posts.
    for (const char* post : kPosts) {
        INFO(post);
        CHECK(caught[post] >= 10);
    }
}

TEST_CASE("round trip: print what catches each mutation", "[.][roundtrip-dump]")
{
    for (const MutationResult& r : runMutations()) {
        std::printf("%s/%s: %s -> %s\n", r.post.c_str(), r.file.c_str(), r.name.c_str(),
                    r.caught ? r.by.c_str() : "NOT CAUGHT");
    }
}
TEST_CASE("round trip: a peck clearance the native cycle cannot carry must be warned about", "[post][roundtrip]")
{
    // cam.h: a post never drops the IR's peck clearance silently. The FANUC chip-break golden's G73 cannot carry the
    // toolpath's 0.3 mm (parameter 5114 decides), and its warnings file says so; without that warning the round trip
    // reports it.
    for (const Run& r : goldenRuns()) {
        if (r.post != "fanuc" || r.file != "12-chip-break_mm.nc") {
            continue;
        }
        const std::string text = readFile(goldenPath(r.post, r.file));
        const auto warned = roundTrip(r.post, text, r.program,
                                      warningLines(readFile(goldenPath(r.post, "12-chip-break_mm.warnings.txt"))));
        const auto silent = roundTrip(r.post, text, r.program, {});
        auto has = [](const Outcome& o) {
            return std::any_of(o.found.begin(), o.found.end(),
                               [](const Found& f) { return f.issue.code == "rt-peck-clearance-silent"; });
        };
        CHECK_FALSE(has(warned));
        CHECK(has(silent));
        return;
    }
    FAIL("the FANUC chip-break golden is not in the matrix");
}

TEST_CASE("round trip: thread milling keeps the pitch per turn in every post's program", "[post][roundtrip]")
{
    // T-056: the thread milling goldens (18-thread-milling) as each post wrote them, read back by the interpreter. Over
    // every run of helical arcs about one centre whose sweeps add up to a whole turn, Z changes by the thread's pitch
    // (1.25 mm for the hole at X15 Y20, 1.75 mm for the pin at X30 Y20) within one least count of the written numbers.
    std::size_t programs = 0, turns = 0;
    for (const Run& r : goldenRuns()) {
        if (r.file.rfind("18-thread-milling", 0) != 0) {
            continue;
        }
        ++programs;
        INFO(r.post << "/" << r.file);
        const rtgc::Trace t = rtgc::interpret(readFile(goldenPath(r.post, r.file)), dialectFor(r.post, r.program));
        REQUIRE(t.errors.empty());
        const auto helical = [](const rtgc::Segment& s) {
            return s.kind == rtgc::SegKind::Arc && s.plane == 17 && std::abs(s.to.z - s.from.z) > 1e-9;
        };
        for (std::size_t i = 0; i < t.segs.size(); ++i) {
            if (!helical(t.segs[i])) {
                continue;
            }
            const rtgc::Segment& a = t.segs[i];
            double sweep = 0;
            for (std::size_t j = i; j < t.segs.size() && helical(t.segs[j]); ++j) {
                const rtgc::Segment& b = t.segs[j];
                if (std::abs(b.centre.x - a.centre.x) > 1e-9 || std::abs(b.centre.y - a.centre.y) > 1e-9) {
                    break;
                }
                sweep += b.sweep;
                if (std::abs(sweep - 2 * 3.14159265358979323846) < 1e-6) {
                    const double pitch = std::abs(a.centre.x - 15) < 1e-6 ? 1.25 : 1.75;
                    INFO("line " << a.line << " to " << b.line);
                    CHECK(std::abs(std::abs(b.to.z - a.from.z) - pitch) <= b.tol * 1.0001);
                    ++turns;
                    break;
                }
                if (sweep > 2 * 3.14159265358979323846) {
                    break;
                }
            }
        }
    }
    CHECK(programs == 2 * std::size(kPosts));
    CHECK(turns > 100);
}
