// The test-only G-code interpreter (T-043). See gcode_interp.h for the sources and the independence rule: this file
// includes the standard library and its own header only (gcode_interp_test.cpp checks that).
#include "gcode_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rtgc {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isHome(double z)
{
    return z >= kHomeZ * 0.5;
}
bool known(double v)
{
    return !std::isnan(v);
}
bool sameAxis(double a, double b)
{
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

std::string fmt(double v)
{
    if (std::isnan(v)) {
        return "unknown";
    }
    if (isHome(v)) {
        return "machine zero";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

// ---------- lexing [N 3.3] ----------
struct Word {
    char letter = 0;
    double value = 0; // the number as written
    int frac = 0;     // digits after the decimal point
    bool point = false;
    bool sign = false; // written with + or -
    std::string raw;   // the number's text
};

struct Block {
    int line = 0; // 1-based
    bool blank = false;
    bool percent = false;
    bool hasN = false;
    bool oLine = false;
    std::vector<Word> words; // without N and O
};

double pow10i(int n)
{
    double v = 1;
    for (int i = 0; i < n; ++i) {
        v *= 10;
    }
    return v;
}

// Number syntax [N 3.3.2.1]: optional sign, digits, optional point, digits; at least one digit.
bool lexNumber(const std::string& s, std::size_t& i, Word& w)
{
    std::size_t j = i;
    bool neg = false;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
        neg = s[j] == '-';
        w.sign = true;
        ++j;
    }
    std::int64_t mant = 0;
    int digits = 0;
    int frac = 0;
    bool point = false;
    for (; j < s.size(); ++j) {
        const char c = s[j];
        if (c >= '0' && c <= '9') {
            if (digits < 18) {
                mant = mant * 10 + (c - '0');
                if (point) {
                    ++frac;
                }
            }
            ++digits;
        } else if (c == '.' && !point) {
            point = true;
        } else {
            break;
        }
    }
    if (digits == 0) {
        return false;
    }
    w.raw = s.substr(i, j - i);
    w.value = (neg ? -1.0 : 1.0) * static_cast<double>(mant) / pow10i(frac);
    w.frac = frac;
    w.point = point;
    i = j;
    return true;
}

bool letterKnown(char c, const Dialect& d)
{
    // [N table 3] less A B C (no rotary axes here) and L (repeats are not modelled); E for Haas G187 [H1 p.25 per
    // haas-ngc.md].
    if (std::string("DFGHIJKMNOPQRSTXYZ").find(c) != std::string::npos) {
        return true;
    }
    return c == 'E' && d.family == Family::Haas;
}

Block lexLine(const std::string& raw, int line, const Dialect& d, std::vector<Issue>& errors)
{
    Block b;
    b.line = line;
    std::string s = raw;
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    std::size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) {
        b.blank = true;
        return b;
    }
    if (s.substr(first) == "%") {
        b.percent = true; // [N 3.1 file demarcation]
        return b;
    }
    auto err = [&](const std::string& code, const std::string& text) {
        errors.push_back(Issue{line, code, text});
    };
    bool firstItem = true;
    for (std::size_t i = first; i < s.size();) {
        const char c = s[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '(') {
            // A comment ends at the first ')'; a '(' inside one is an error, and it must close on the line [N 3.3.4].
            const std::size_t close = s.find(')', i + 1);
            const std::size_t open = s.find('(', i + 1);
            if (close == std::string::npos) {
                err("comment-unclosed", "a comment without its closing parenthesis [N 3.3.4]");
                return b;
            }
            if (open != std::string::npos && open < close) {
                err("comment-nested", "a parenthesis inside a comment [N 3.3.4]");
            }
            i = close + 1;
            continue;
        }
        char letter = c;
        if (c >= 'a' && c <= 'z') {
            if (d.family != Family::Ngc) {
                // [ASSUMPTION] FANUC and Haas: upper-case addresses only.
                err("lower-case", std::string("lower-case address '") + c + "' outside a comment");
            }
            letter = static_cast<char>(c - 'a' + 'A'); // [N 3.3] case-insensitive outside comments
        }
        if (letter < 'A' || letter > 'Z') {
            err("unknown-character",
                std::string("character '") + c + "' is not part of any word this interpreter knows");
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
            ++j; // [N 3.3] spaces are allowed anywhere
        }
        Word w;
        w.letter = letter;
        if (!lexNumber(s, j, w)) {
            err("malformed-word", std::string("address ") + letter + " without a number");
            i = j;
            continue;
        }
        i = j;
        if (letter == 'N') {
            // [N 3.3.1] the line number: first, an integer without sign, at most five digits.
            if (!firstItem || w.point || w.sign || w.raw.size() > 5) {
                err("bad-line-number", "N must come first, as an unsigned integer of at most five digits [N 3.3.1]");
            }
            b.hasN = true;
            firstItem = false;
            continue;
        }
        if (letter == 'O') {
            b.oLine = true;
            if (!firstItem) {
                err("bad-o-word", "O must start the block");
            }
            if (w.point || w.sign) {
                err("bad-o-word", "the program number must be an unsigned integer");
            }
            // Program number range: Haas O0-O99999 [H1 p.26]; FANUC 4 digits or 8 with the option [F1, ASSUMPTION];
            // RS274/NGC has no O word at all [N table 3]: the ISO 6983 program number is accepted on the first line
            // [ASSUMPTION: ISO 6983-1 not read].
            const std::size_t maxDigits = d.family == Family::Haas ? 5 : 8;
            if (w.raw.size() > maxDigits) {
                err("bad-o-word", "program number with more than " + std::to_string(maxDigits) + " digits");
            }
            firstItem = false;
            continue;
        }
        if (!letterKnown(letter, d)) {
            err("unknown-word", std::string("address ") + letter + " is not known for this control");
        }
        b.words.push_back(w);
        firstItem = false;
    }
    return b;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from < text.size()) {
        std::size_t end = text.find('\n', from);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(from, end - from);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
        from = end + 1;
    }
    return out;
}

// ---------- G and M codes ----------
// G codes are handled as ten times their number (G54.1 = 541). Modal groups follow [N table 4]; FANUC and Haas codes
// not in RS274/NGC are put in the group their meaning belongs to.
int gGroup(int g)
{
    switch (g) {
    case 0:
    case 10:
    case 20:
    case 30:
    case 730:
    case 740:
    case 800:
    case 810:
    case 820:
    case 830:
    case 840:
    case 850:
    case 890:
        return 1;
    case 40:
    case 280:
    case 530:
        return 0;
    case 170:
    case 180:
    case 190:
        return 2;
    case 900:
    case 910:
        return 3;
    case 940:
        return 5;
    case 200:
    case 210:
        return 6;
    case 400:
    case 410:
    case 420:
        return 7;
    case 430:
    case 490:
        return 8;
    case 980:
    case 990:
        return 10;
    case 1870:
        return 20; // Haas G187 smoothing: a group of its own
    default:
        return 12; // work offsets
    }
}

bool isCycle(int g)
{
    return g == 730 || g == 740 || (g >= 810 && g <= 890 && g != 800);
}

// The G codes each control is known to read, with the meaning this file gives them.
bool gKnown(int g, const Dialect& d)
{
    static const std::set<int> common = {0,   10,  20,  30,  40,  170, 180, 190, 200, 210, 280, 400,
                                         410, 420, 430, 490, 530, 540, 550, 560, 570, 580, 590, 800,
                                         810, 820, 830, 840, 850, 890, 900, 910, 940, 980, 990};
    if (common.count(g) != 0) {
        return true;
    }
    switch (d.family) {
    case Family::Ngc:
        return g == 591 || g == 592 || g == 593; // [N 3.5.13]
    case Family::Fanuc:
        return g == 541 || g == 730 || g == 740; // [F1, ASSUMPTION]
    case Family::Haas:
        // G110-G129 and G154 P1-P99 extra work offsets, G73, G74, G187 [H1 per haas-ngc.md word list]
        return (g >= 1100 && g <= 1290 && g % 10 == 0) || g == 1540 || g == 730 || g == 740 || g == 1870;
    }
    return false;
}

std::string gName(int g)
{
    std::string s = "G" + std::to_string(g / 10);
    if (g % 10 != 0) {
        s += "." + std::to_string(g % 10);
    }
    return s;
}

} // namespace

bool parseCoolantCodes(const std::string& list, std::map<int, CoolantCode>& out)
{
    out.clear();
    std::size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && list[i] == ' ') {
            ++i;
        }
        if (i >= list.size()) {
            break;
        }
        std::size_t end = list.find(' ', i);
        if (end == std::string::npos) {
            end = list.size();
        }
        const std::string item = list.substr(i, end - i);
        i = end;
        const std::size_t eq = item.find('=');
        const std::size_t slash = item.find('/');
        if (eq == std::string::npos || slash == std::string::npos || slash < eq) {
            return false;
        }
        const std::string mode = item.substr(0, eq);
        unsigned bit = 0;
        if (mode == "flood") {
            bit = kFlood;
        } else if (mode == "mist") {
            bit = kMist;
        } else if (mode == "through") {
            bit = kThrough;
        } else if (mode == "air") {
            bit = kAir;
        } else {
            return false;
        }
        auto code = [](const std::string& c, int& v) {
            if (c.empty()) {
                v = -1;
                return true;
            }
            if (c.size() < 2 || c[0] != 'M') {
                return false;
            }
            v = 0;
            for (std::size_t k = 1; k < c.size(); ++k) {
                if (c[k] < '0' || c[k] > '9') {
                    return false;
                }
                v = v * 10 + (c[k] - '0');
            }
            return true;
        };
        int on = -1, off = -1;
        if (!code(item.substr(eq + 1, slash - eq - 1), on) || !code(item.substr(slash + 1), off)) {
            return false;
        }
        if (on >= 0) {
            out[on].on |= bit;
        }
        if (off >= 0) {
            out[off].off |= bit;
        }
    }
    return true;
}

std::string describe(const Issue& issue, const std::vector<std::string>& lines)
{
    if (issue.line <= 0 || static_cast<std::size_t>(issue.line) > lines.size()) {
        return "program: " + issue.text + " [" + issue.code + "]";
    }
    return "line " + std::to_string(issue.line) + " (" + lines[static_cast<std::size_t>(issue.line) - 1] +
           "): " + issue.text + " [" + issue.code + "]";
}

namespace {

class Interp {
public:
    Interp(const Dialect& d, Trace& t) : d_(d), t_(t) {}

    void run(const std::string& text)
    {
        t_.lines = splitLines(text);
        std::vector<Block> blocks;
        for (std::size_t n = 0; n < t_.lines.size(); ++n) {
            blocks.push_back(lexLine(t_.lines[n], static_cast<int>(n) + 1, d_, t_.errors));
        }
        leastCounts(blocks);
        // Framing [N 3.1]: a '%' line first and last; the O line, when there is one, is the first block.
        int firstNonBlank = -1, lastNonBlank = -1;
        for (std::size_t n = 0; n < blocks.size(); ++n) {
            if (!blocks[n].blank) {
                if (firstNonBlank < 0) {
                    firstNonBlank = static_cast<int>(n);
                }
                lastNonBlank = static_cast<int>(n);
            }
        }
        if (firstNonBlank < 0) {
            safety(0, "safety-framing", "the program is empty");
            return;
        }
        if (!blocks[static_cast<std::size_t>(firstNonBlank)].percent) {
            safety(firstNonBlank + 1, "safety-framing", "the program does not start with a % line");
        }
        if (!blocks[static_cast<std::size_t>(lastNonBlank)].percent || lastNonBlank == firstNonBlank) {
            safety(lastNonBlank + 1, "safety-framing", "the program does not end with a % line");
        }
        bool seenProgramBlock = false;
        int percents = 0;
        for (const Block& b : blocks) {
            if (b.blank) {
                continue;
            }
            if (b.percent) {
                ++percents;
                if (percents > 2) {
                    error(b.line, "framing", "a third % line");
                }
                continue;
            }
            if (percents >= 2) {
                error(b.line, "framing", "a block after the closing % line");
                continue;
            }
            if (b.oLine && seenProgramBlock) {
                error(b.line, "bad-o-word", "the O line must be the first block of the program");
            }
            if (b.oLine && !b.words.empty()) {
                error(b.line, "bad-o-word", "words after the program number on the O line");
            }
            seenProgramBlock = true;
            if (b.oLine) {
                continue;
            }
            if (m30Line_ > 0) {
                safety(b.line, "safety-after-m30", "a block after M30 (nothing after M30 is run) [N 3.6.1]");
                continue;
            }
            if (!b.words.empty()) {
                execute(b);
            }
        }
        if (m30Line_ == 0) {
            safety(0, "safety-no-m30", "the program has no M30");
        }
        t_.inch = m_.inch;
        t_.axisLcMm = axisLcMm();
    }

private:
    const Dialect& d_;
    Trace& t_;
    Modal m_;
    P3 pos_;
    int motion_ = -1; // G x 10 of the modal motion; -1 after G80 or at the start
    bool distKnown_ = false;
    bool absolute_ = true;
    int plane_ = 170;
    int retractMode_ = 0; // 980 / 990
    double fWord_ = 0;    // the F number as written (program units per minute)
    int fFrac_ = -1;
    int selected_ = -1;
    int axisFracSeen_ = 0;
    int fFracSeen_ = 0;
    struct Cycle {
        bool active = false;
        int code = 0;
        bool haveZ = false, haveR = false, haveQ = false, haveP = false, ijk = false;
        double z = 0, r = 0, q = 0, p = 0, pTol = 0.001; // mm, mm, mm, s
        double i = 0, j = 0, k = 0;                      // Haas G83 I J K, mm
        double initialZ = 0;                             // FANUC / Haas initial level
    } cyc_;
    // safety tracking
    bool freshG43_ = true; // a G43 was given since the last M6
    bool approachReported_ = false;
    bool inCut_ = false;
    int m30Line_ = 0;
    int line_ = 0;

    void error(int line, const std::string& code, const std::string& text)
    {
        t_.errors.push_back(Issue{line, code, text});
    }
    void safety(int line, const std::string& code, const std::string& text)
    {
        t_.safety.push_back(Issue{line, code, text});
    }

    // The least count of axis words as written: the most decimals any axis word carries. When none carries a
    // fraction, the control's least input increment, 0.001 mm or 0.0001 in [ASSUMPTION: the common "IS-B" increment].
    void leastCounts(const std::vector<Block>& blocks)
    {
        for (const Block& b : blocks) {
            for (const Word& w : b.words) {
                if (std::string("XYZIJKRQ").find(w.letter) != std::string::npos) {
                    axisFracSeen_ = std::max(axisFracSeen_, w.frac);
                }
                if (w.letter == 'F') {
                    fFracSeen_ = std::max(fFracSeen_, w.frac);
                }
            }
        }
    }
    double axisLcMm() const
    {
        const int digits = axisFracSeen_ > 0 ? axisFracSeen_ : (m_.inch ? 4 : 3);
        return (m_.inch ? 25.4 : 1.0) / pow10i(digits);
    }
    double toMm(double v) const { return m_.inch ? v * 25.4 : v; }

    // An axis-like word (X Y Z I J K R Q) in program units. FANUC and Haas read a number without a decimal point as a
    // count of least input increments (X10 = 0.010 mm / 0.0010 in) [F1, ASSUMPTION; Haas ASSUMPTION, not in the
    // cited pages]; RS274/NGC reads it as the number itself [N 3.3.2.1].
    double axisWord(const Word& w) const
    {
        if (!w.point && d_.family != Family::Ngc) {
            return w.value * (m_.inch ? 1e-4 : 1e-3);
        }
        return w.value;
    }

    // A dwell word in seconds, with its least count. `cycleP`: the P of a canned cycle.
    bool dwellSeconds(const Word& w, double& s, double& tol, bool cycleP)
    {
        if (w.value < 0) {
            error(line_, "negative-dwell", "a negative dwell [N 3.5.4]");
            return false;
        }
        switch (d_.family) {
        case Family::Ngc:
            if (d_.ngcDwellMilliseconds) {
                if (w.point) {
                    error(line_, "dwell-format",
                          "P with a decimal point where the control reads integer milliseconds (dwellUnits)");
                    return false;
                }
                s = w.value / 1000;
                tol = 0.001;
                return true;
            }
            s = w.value; // seconds [N 3.5.4, 3.5.16.3, 3.5.16.10]
            tol = 1 / pow10i(w.frac);
            return true;
        case Family::Fanuc:
            if (w.letter == 'X') {
                // G4 X: seconds with a decimal point; without one, increments of 0.001 s [F1, ASSUMPTION]
                s = w.point ? w.value : w.value / 1000;
                tol = w.point ? 1 / pow10i(w.frac) : 0.001;
                return true;
            }
            if (w.point) {
                // [F1, ASSUMPTION] P is an integer count of milliseconds and takes no decimal point.
                error(line_, "dwell-format", "P with a decimal point: this control reads P as integer milliseconds");
                return false;
            }
            s = w.value / 1000;
            tol = 0.001;
            return true;
        case Family::Haas:
            // [H1 p.48] P with a decimal point is seconds, without one milliseconds. For a canned cycle's P the same
            // rule is assumed [ASSUMPTION; H1 p.26 lists P in seconds].
            (void)cycleP;
            s = w.point ? w.value : w.value / 1000;
            tol = w.point ? 1 / pow10i(w.frac) : 0.001;
            return true;
        }
        return false;
    }

    void push(Segment s)
    {
        s.line = line_;
        s.state = m_;
        s.tol = axisLcMm();
        if (s.kind == SegKind::Feed || s.kind == SegKind::Arc) {
            const int frac = fFrac_ > 0 ? fFrac_ : fFracSeen_;
            s.feedTol = (m_.inch ? 25.4 : 1.0) / pow10i(frac);
        }
        t_.segs.push_back(s);
    }

    // ----- safety facts -----
    // Never a feed move with the spindle stopped; the documented exceptions are the rigid tapping cycles, which
    // drive the spindle themselves (FANUC after M29 [F1, ASSUMPTION]; Haas G84/G74 start the spindle [H1 p.82]).
    void checkSpindle(const Segment& s, bool tapDrivesSpindle)
    {
        if (tapDrivesSpindle) {
            return;
        }
        if (m_.spindle == 0 || m_.rpm <= 0) {
            safety(line_, "safety-feed-spindle-stopped", "a feed move with the spindle stopped");
        }
        (void)s;
    }
    // The length offset is on, and was given after the last tool change, before the tool approaches in Z.
    void checkApproach(const P3& from, const P3& to, bool feed)
    {
        const bool down = known(to.z) && (!known(from.z) || to.z < from.z);
        if (!down && !feed) {
            return;
        }
        if (m_.lengthH < 0 || !freshG43_) {
            if (!approachReported_) {
                safety(line_, "safety-no-length-offset",
                       m_.lengthH < 0 ? "a Z approach with no tool length offset (G43) active"
                                      : "a Z approach after the tool change without a G43 since the M6");
                approachReported_ = true;
            }
        }
    }
    // No XY rapid with the tool still in the cut: after cutting, the tool must leave in Z alone first.
    void trackCut(const Segment& s)
    {
        const bool xyMoves = !sameAxis(s.from.x, s.to.x) || !sameAxis(s.from.y, s.to.y);
        const bool zUp = !known(s.from.z) || (known(s.to.z) && s.to.z > s.from.z);
        if (s.kind == SegKind::Rapid) {
            if (xyMoves && (std::isnan(s.from.z) || std::isnan(s.to.z))) {
                safety(line_, "safety-xy-rapid-z-unknown",
                       "a rapid moves X/Y with the Z height not known (no retract to a known height first)");
            }
            if (xyMoves && inCut_) {
                safety(line_, "safety-xy-rapid-in-cut",
                       "a rapid moves X/Y while the tool is still in the cut (no Z-only retract since the last cutting "
                       "move)");
                inCut_ = false;
            } else if (!xyMoves && zUp) {
                inCut_ = false;
            }
            return;
        }
        if (s.kind == SegKind::Feed || s.kind == SegKind::Arc) {
            inCut_ = !(s.kind == SegKind::Feed && !xyMoves && zUp);
        }
    }

    // ----- one block -----
    void execute(const Block& b)
    {
        line_ = b.line;
        std::vector<int> gs;
        std::vector<int> ms;
        std::map<char, Word> w;
        for (const Word& word : b.words) {
            if (word.letter == 'G') {
                const double v = word.value * 10;
                const double r = std::round(v);
                if (std::fabs(v - r) > 0.001 || word.sign) {
                    error(line_, "unknown-g", "G" + word.raw + " is not a G code");
                    continue;
                }
                const int g = static_cast<int>(r);
                if (!gKnown(g, d_)) {
                    error(line_, "unknown-g", gName(g) + " is not known for this control");
                    continue;
                }
                gs.push_back(g);
            } else if (word.letter == 'M') {
                if (word.point || word.sign) {
                    error(line_, "unknown-m", "M" + word.raw + " is not an M code");
                    continue;
                }
                ms.push_back(static_cast<int>(word.value));
            } else {
                if (w.count(word.letter) != 0) {
                    error(line_, "repeated-word", std::string("two ") + word.letter + " words in one block [N 3.3.5]");
                }
                w[word.letter] = word;
            }
        }
        // Two G codes of one modal group [N 3.3.5, table 4].
        std::map<int, int> groups;
        for (const int g : gs) {
            if (++groups[gGroup(g)] > 1 && gGroup(g) != 0) {
                error(line_, "modal-group", "two G codes of the same modal group in one block [N 3.3.5]");
            }
        }
        auto hasG = [&](int g) {
            return std::find(gs.begin(), gs.end(), g) != gs.end();
        };
        auto hasW = [&](char c) {
            return w.count(c) != 0;
        };
        // M codes: Haas one per block [H1 p.11, p.19]; FANUC one [F1, ASSUMPTION]; RS274/NGC up to four of different
        // groups [N 3.3.5].
        if (ms.size() > (d_.family == Family::Ngc ? 4u : 1u)) {
            error(line_, "too-many-m", "more M codes in one block than this control takes");
        }
        for (const int mc : ms) {
            if (!mKnown(mc)) {
                error(line_, "unknown-m", "M" + std::to_string(mc) + " is not known for this control");
            }
        }

        // The block's motion mode (group 1).
        int blockMotion = -2;
        for (const int g : gs) {
            if (gGroup(g) == 1) {
                blockMotion = g;
            }
        }
        const bool home = hasG(280);
        const bool machine = hasG(530);
        const int effMotion = blockMotion != -2 ? blockMotion : motion_;
        const bool cycleBlock = isCycle(effMotion) && !home && blockMotion != 800;
        const bool arcBlock = (effMotion == 20 || effMotion == 30) && !home;

        // Words that only mean something in a context.
        if (hasW('H') && !hasG(430)) {
            error(line_, "word-context", "H without G43 [N 3.5.11]");
        }
        if (hasW('D') && !hasG(410) && !hasG(420)) {
            error(line_, "word-context", "D without G41 or G42 [N 3.5.10]");
        }
        if (hasW('E') && !hasG(1870)) {
            error(line_, "word-context", "E without G187");
        }
        const bool haasIjk = cycleBlock && effMotion == 830 && d_.family == Family::Haas && hasW('I');
        if ((hasW('I') || hasW('J') || hasW('K')) && !arcBlock && !haasIjk) {
            error(line_, "word-context", "I, J or K outside an arc (or a Haas G83 with I J K)");
        }
        if (hasW('R') && !arcBlock && !cycleBlock) {
            error(line_, "word-context", "R outside an arc or a canned cycle");
        }
        if (hasW('Q') && !(cycleBlock && (effMotion == 830 || (effMotion == 730 && d_.family != Family::Ngc)))) {
            error(line_, "word-context", "Q outside G83 / G73 [N 3.5.16.4]");
        }
        if (hasW('P')) {
            bool ok = hasG(40) || hasG(541) || hasG(1540) || (hasG(1870) && !d_.haasClassic);
            if (cycleBlock) {
                switch (effMotion) {
                case 820:
                case 890:
                    ok = true; // [N 3.5.16.3, 3.5.16.10]
                    break;
                case 830:
                case 730:
                    ok = ok || d_.family == Family::Haas; // Haas G83 / G73 P [H1 p.26 per haas-ngc.md]
                    break;
                case 840:
                case 740:
                    ok = ok || d_.family == Family::Fanuc; // FANUC G84 P [F1, ASSUMPTION; cam.h]
                    break;
                default:
                    break;
                }
            }
            if (!ok) {
                error(line_, "word-context",
                      hasG(1870) ? "P on G187: the classic control's G187 takes E only [H1 p.17 per haas-classic.md]"
                                 : "P where this control has no use for it");
            }
        }
        if (hasW('X') && hasG(40) && d_.family == Family::Ngc) {
            error(line_, "word-context", "G4 takes P, not X [N 3.5.4]");
        }

        // [N 3.8] order of execution.
        // 2. feed rate mode: G94 is the only one known.
        // 3. F
        if (hasW('F')) {
            const Word& f = w['F'];
            if (f.value < 0) {
                error(line_, "negative-feed", "a negative F");
            }
            fWord_ = f.value;
            fFrac_ = f.frac;
        }
        // 4. S
        if (hasW('S')) {
            const Word& s = w['S'];
            if (s.value < 0) {
                error(line_, "negative-speed", "a negative S [N 3.7.2]");
            }
            m_.rpm = s.value;
        }
        // 5. T
        if (hasW('T')) {
            const Word& t = w['T'];
            if (t.point || t.value < 0) {
                error(line_, "bad-tool", "T must be a non-negative integer [N 3.7.3]");
            }
            selected_ = static_cast<int>(t.value);
        }
        // 6. M6 [N 3.6.3]: the selected tool goes in, the spindle stops, the axes stay where they are.
        if (std::find(ms.begin(), ms.end(), 6) != ms.end()) {
            if (cyc_.active) {
                safety(line_, "safety-g80-missing", "a tool change with a canned cycle still active (no G80)");
            }
            if (m_.comp != 0) {
                safety(line_, "safety-comp-on", "a tool change with cutter compensation still on");
            }
            if (selected_ < 0) {
                error(line_, "bad-tool", "M6 without a tool selected");
            }
            m_.tool = selected_;
            m_.spindle = 0;
            if (d_.family == Family::Haas && !isHome(pos_.z)) {
                // Haas M06 retracts Z to machine zero itself [H1 p.19 per haas-ngc.md]. (RS274/NGC: the axes stay
                // where they are [N 3.6.3]; FANUC: the builder's macro [F1, ASSUMPTION: no motion].)
                Segment s;
                s.kind = SegKind::Rapid;
                s.origin = SegOrigin::Machine;
                s.from = pos_;
                s.to = pos_;
                s.to.z = kHomeZ;
                trackCut(s);
                push(s);
                pos_ = s.to;
            }
            freshG43_ = false;
            approachReported_ = false;
            inCut_ = false;
        }
        // 7. spindle [N 3.6.2]; FANUC M29 rigid tapping mode [F1, ASSUMPTION]
        for (const int mc : ms) {
            if (mc == 3) {
                m_.spindle = 1;
            } else if (mc == 4) {
                m_.spindle = -1;
            } else if (mc == 5) {
                m_.spindle = 0;
            } else if (mc == 29 && d_.family == Family::Fanuc) {
                m_.rigidTap = true;
            }
        }
        // 8. coolant: the shop's codes [N 3.6.4 for M7 M8 M9]. Haas M89 also stops the spindle [H1 p.129 per
        // haas-ngc.md].
        for (const int mc : ms) {
            auto it = d_.coolant.find(mc);
            if (it != d_.coolant.end()) {
                m_.coolant &= ~it->second.off;
                m_.coolant |= it->second.on;
            }
            if (mc == 89 && d_.family == Family::Haas) {
                m_.spindle = 0;
            }
        }
        // 10. dwell [N 3.5.4]
        if (hasG(40)) {
            const bool hasP = hasW('P');
            const bool hasX = hasW('X') && d_.family == Family::Fanuc;
            if (hasP == hasX) {
                error(line_, "dwell-format", "G4 needs exactly one dwell word (P, or X on FANUC)");
            } else {
                double s = 0, tol = 0;
                if (dwellSeconds(hasP ? w['P'] : w['X'], s, tol, false)) {
                    Segment seg;
                    seg.kind = SegKind::Dwell;
                    seg.from = seg.to = pos_;
                    seg.dwell = s;
                    push(seg);
                    t_.segs.back().dwellTol = tol;
                }
            }
            if (hasX) {
                w.erase('X'); // the X of a FANUC dwell is not an axis word
            }
        }
        // 11. plane [N 3.5.6]
        for (const int g : gs) {
            if (g == 170 || g == 180 || g == 190) {
                if (m_.comp != 0 && g != 170) {
                    error(line_, "comp-plane", "cutter compensation needs the XY plane [N 3.5.10]");
                }
                plane_ = g;
            }
        }
        // 12. units [N 3.5.7]
        if (hasG(200) || hasG(210)) {
            m_.inch = hasG(200);
            m_.unitsKnown = true;
        }
        // 13. cutter compensation [N 3.5.10, B.2.3: switched before the block's motion]
        const int compBefore = m_.comp;
        if (hasG(400)) {
            m_.comp = 0;
            m_.dNumber = -1;
        }
        if (hasG(410) || hasG(420)) {
            if (m_.comp != 0) {
                error(line_, "comp-on-on", "cutter compensation turned on when it is already on [N 3.5.10]");
            }
            if (plane_ != 170) {
                error(line_, "comp-plane", "cutter compensation needs the XY plane [N 3.5.10]");
            }
            m_.comp = hasG(410) ? 1 : 2;
            if (hasW('D')) {
                const Word& dw = w['D'];
                if (dw.point || dw.value < 0) {
                    error(line_, "bad-d", "D must be a non-negative integer [N 3.5.10]");
                }
                m_.dNumber = static_cast<int>(dw.value);
            } else if (d_.family == Family::Ngc) {
                m_.dNumber = m_.tool; // [N B.2.4] the slot of the tool in the spindle
            } else {
                error(line_, "bad-d", "G41/G42 without D [H1 p.67; F1 ASSUMPTION]");
            }
        }
        const bool compChanged = m_.comp != compBefore;
        // 14. tool length offset [N 3.5.11]
        if (hasG(490)) {
            m_.lengthH = -1;
        }
        if (hasG(430)) {
            if (!hasW('H')) {
                error(line_, "bad-h", "G43 without H [N 3.5.11]");
            } else {
                const Word& h = w['H'];
                if (h.point || h.value < 0) {
                    error(line_, "bad-h", "H must be a non-negative integer [N 3.5.11]");
                }
                m_.lengthH = static_cast<int>(h.value);
                freshG43_ = true;
            }
        }
        // 15. work offset [N 3.5.13]
        for (const int g : gs) {
            if (gGroup(g) != 12) {
                continue;
            }
            if (m_.comp != 0) {
                error(line_, "comp-wcs", "work offset changed with cutter compensation on [N 3.5.13]");
            }
            std::string name = gName(g);
            if (g == 541 || g == 1540) {
                // G54.1 P1-P48 (P300 with the option) [F1, ASSUMPTION]; G154 P1-P99 [H1 per haas-ngc.md]
                if (!hasW('P') || w['P'].point || w['P'].value < 1 || w['P'].value > (g == 541 ? 300 : 99)) {
                    error(line_, "bad-wcs", name + " needs P with the offset number");
                } else {
                    name += " P" + std::to_string(static_cast<int>(w['P'].value));
                }
            }
            m_.wcs = name;
        }
        // 17. distance mode [N 3.5.17]
        if (hasG(900) || hasG(910)) {
            absolute_ = hasG(900);
            distKnown_ = true;
        }
        // 18. return mode [N 3.5.20]
        if (hasG(980) || hasG(990)) {
            retractMode_ = hasG(980) ? 980 : 990;
        }
        // Haas G187 smoothing: no motion. P1-P3 (NGC), E the corner tolerance, at most 0.25 in [H1 p.25 per
        // haas-ngc.md].
        if (hasG(1870)) {
            if (hasW('P') && !d_.haasClassic) {
                const Word& p = w['P'];
                if (p.point || p.value < 1 || p.value > 3) {
                    error(line_, "bad-g187", "G187 P must be 1, 2 or 3");
                }
            }
            if (hasW('E')) {
                const double e = toMm(w['E'].value);
                if (!w['E'].point || e <= 0 || e > 6.35 + 1e-9) {
                    error(line_, "bad-g187", "G187 E outside 0 to 0.25 in");
                }
            }
        }

        const bool axisWords = hasW('X') || hasW('Y') || hasW('Z');
        // 19. home [N 3.5.8]
        if (home) {
            referenceReturn(w);
        }
        // 20. motion [N 3.5.1-3.5.3, 3.5.12, 3.5.15, 3.5.16]
        if (blockMotion != -2) {
            if (cyc_.active && !isCycle(blockMotion) && blockMotion != 800) {
                safety(line_, "safety-g80-missing",
                       "a " + gName(blockMotion) + " motion while a canned cycle is active, without G80 first");
                endCycle();
            }
            if (blockMotion == 800) {
                endCycle();
                motion_ = -1;
            } else {
                motion_ = blockMotion;
            }
        }
        if (!home && cyc_.active && machine) {
            safety(line_, "safety-g80-missing", "motion outside the canned cycle while it is active, without G80");
            endCycle();
            motion_ = -1;
        }
        if (machine) {
            machineMove(w);
        } else if (!home) {
            if (isCycle(motion_)) {
                if (axisWords || hasW('R') || blockMotion != -2) {
                    runCycle(w);
                }
            } else if (axisWords || ((motion_ == 20 || motion_ == 30) && (hasW('I') || hasW('J') || hasW('K')))) {
                if (motion_ < 0) {
                    error(line_, "no-motion-mode",
                          "axis words with no motion mode in effect (G80, or none given) [N 3.5.15]");
                } else if (motion_ == 0 || motion_ == 10) {
                    linear(w, motion_ == 0, compChanged);
                } else {
                    arc(w, motion_ == 20, compChanged);
                }
            }
        }
        if (compChanged && !lastMotionWasLinearXY_) {
            safety(line_, "safety-comp-switch",
                   std::string("cutter compensation switched ") + (m_.comp != 0 ? "on" : "off") +
                       (lastMotionWasArc_ ? " on an arc" : " without an X/Y move in the block"));
        }
        lastMotionWasLinearXY_ = false;
        lastMotionWasArc_ = false;
        // 21. stop [N 3.6.1]
        for (const int mc : ms) {
            if (mc == 30) {
                if (m_.comp != 0) {
                    safety(line_, "safety-comp-on", "M30 with cutter compensation still on");
                }
                if (cyc_.active) {
                    safety(line_, "safety-g80-missing", "M30 with a canned cycle still active (no G80)");
                }
                if (!isHome(pos_.z)) {
                    safety(line_, "safety-m30-not-retracted",
                           "M30 with the tool not retracted to machine Z zero / the reference position");
                }
                m30Line_ = line_;
                m_.comp = 0;
                m_.spindle = 0;
                m_.coolant = 0;
            }
        }
    }
    bool lastMotionWasLinearXY_ = false;
    bool lastMotionWasArc_ = false;

    bool mKnown(int mc) const
    {
        if (d_.coolant.count(mc) != 0) {
            return true;
        }
        if (mc == 1 || mc == 3 || mc == 4 || mc == 5 || mc == 6 || mc == 30) {
            return true; // [N table 7]; Haas [H1 p.19-21]
        }
        return mc == 29 && d_.family == Family::Fanuc;
    }

    bool motionPreconditions()
    {
        if (!m_.unitsKnown) {
            error(line_, "units-unknown", "motion before G20 / G21 (the power-up unit is a machine setting)");
            return false;
        }
        if (!distKnown_) {
            error(line_, "distance-unknown", "motion before G90 / G91 (the power-up mode is a machine setting)");
            return false;
        }
        return true;
    }

    // The target of the axis words: absolute in the active work offset, or incremental [N 3.5.17].
    bool target(std::map<char, Word>& w, P3& to)
    {
        to = pos_;
        const char letters[3] = {'X', 'Y', 'Z'};
        double* out[3] = {&to.x, &to.y, &to.z};
        for (int a = 0; a < 3; ++a) {
            if (w.count(letters[a]) == 0) {
                continue;
            }
            const double v = toMm(axisWord(w[letters[a]]));
            if (absolute_) {
                *out[a] = v;
            } else if (v != 0) { // a zero increment does not move the axis, wherever it is
                if (!known(*out[a]) || isHome(*out[a])) {
                    error(line_, "incremental-unknown",
                          std::string("an incremental ") + letters[a] +
                              " move from a position not known in work coordinates");
                    return false;
                }
                *out[a] += v;
            }
        }
        return true;
    }

    void linear(std::map<char, Word>& w, bool rapid, bool compChanged)
    {
        if (!motionPreconditions()) {
            return;
        }
        P3 to;
        if (!target(w, to)) {
            return;
        }
        Segment s;
        s.kind = rapid ? SegKind::Rapid : SegKind::Feed;
        s.from = pos_;
        s.to = to;
        if (!rapid) {
            s.feed = toMm(fWord_);
            if (fWord_ <= 0) {
                error(line_, "no-feed", "a feed move with no feed rate");
            }
            if (!known(pos_.x) || !known(pos_.y) || !known(pos_.z) || isHome(pos_.z)) {
                error(line_, "feed-from-unknown", "a feed move from a position not known in work coordinates");
            }
            checkSpindle(s, false);
        }
        lastMotionWasLinearXY_ = s.from.x != s.to.x || s.from.y != s.to.y;
        (void)compChanged;
        const bool moved = !sameAxis(s.from.x, s.to.x) || !sameAxis(s.from.y, s.to.y) || !sameAxis(s.from.z, s.to.z);
        checkApproach(s.from, s.to, !rapid);
        if (moved) {
            trackCut(s);
            push(s);
        }
        pos_ = to;
    }

    // [N 3.5.3] Arcs in the selected plane. In-plane coordinates (u, v) and the axis a: G17 (x, y; z), G18 (z, x; y),
    // G19 (y, z; x), so that counter-clockwise is the positive rotation about the plane's normal (+Z, +Y, +X), seen
    // from the positive end of that axis.
    static void toPlane(int plane, const P3& p, double& u, double& v, double& a)
    {
        if (plane == 170) {
            u = p.x, v = p.y, a = p.z;
        } else if (plane == 180) {
            u = p.z, v = p.x, a = p.y;
        } else {
            u = p.y, v = p.z, a = p.x;
        }
    }
    static P3 fromPlane(int plane, double u, double v, double a)
    {
        P3 p;
        if (plane == 170) {
            p.x = u, p.y = v, p.z = a;
        } else if (plane == 180) {
            p.z = u, p.x = v, p.y = a;
        } else {
            p.y = u, p.z = v, p.x = a;
        }
        return p;
    }

    void arc(std::map<char, Word>& w, bool cw, bool compChanged)
    {
        (void)compChanged;
        lastMotionWasArc_ = true;
        if (!motionPreconditions()) {
            return;
        }
        if (!known(pos_.x) || !known(pos_.y) || !known(pos_.z) || isHome(pos_.z)) {
            error(line_, "feed-from-unknown", "an arc from a position not known in work coordinates");
            return;
        }
        if (fWord_ <= 0) {
            error(line_, "no-feed", "an arc with no feed rate");
        }
        // The plane's axis letters and offset letters.
        const char* axes = plane_ == 170 ? "XY" : (plane_ == 180 ? "ZX" : "YZ");
        const char* offs = plane_ == 170 ? "IJ" : (plane_ == 180 ? "KI" : "JK"); // offsets for (u, v)
        const char other = plane_ == 170 ? 'K' : (plane_ == 180 ? 'J' : 'I');
        if (w.count(axes[0]) == 0 && w.count(axes[1]) == 0) {
            error(line_, "arc-format",
                  std::string("an arc without ") + axes[0] + " or " + axes[1] +
                      " in the selected plane [N 3.5.3.1, 3.5.3.2]");
            return;
        }
        if (w.count(other) != 0) {
            error(line_, "arc-format", std::string(1, other) + " is not an offset of the selected plane");
        }
        P3 to;
        if (!target(w, to)) {
            return;
        }
        double u0, v0, a0, u1, v1, a1;
        toPlane(plane_, pos_, u0, v0, a0);
        toPlane(plane_, to, u1, v1, a1);
        const bool same = std::fabs(u1 - u0) < 1e-9 && std::fabs(v1 - v0) < 1e-9;
        double cu = 0, cv = 0;
        Segment s;
        s.kind = SegKind::Arc;
        s.from = pos_;
        s.to = to;
        s.cw = cw;
        s.plane = plane_ / 10;
        s.feed = toMm(fWord_);
        const double radiusTol = m_.inch ? 0.0002 * 25.4 : 0.002; // [N 3.5.3.2]
        if (w.count('R') != 0) {
            if (w.count(offs[0]) != 0 || w.count(offs[1]) != 0) {
                error(line_, "arc-format", "an arc with both R and centre offsets");
            }
            s.rFormat = true;
            const double r = toMm(axisWord(w['R']));
            if (same) {
                error(line_, "arc-format", "an R arc whose end point is its start point [N 3.5.3.1]");
                return;
            }
            const double du = u1 - u0, dv = v1 - v0;
            const double c = std::sqrt(du * du + dv * dv);
            double h2 = r * r - c * c / 4;
            if (h2 < 0) {
                if (c / 2 - std::fabs(r) > radiusTol) {
                    error(line_, "arc-radius", "R is smaller than half the chord");
                    return;
                }
                h2 = 0;
            }
            const double h = std::sqrt(h2);
            const double nu = -dv / c, nv = du / c; // the left normal of the chord
            const double mu = (u0 + u1) / 2, mv = (v0 + v1) / 2;
            // R > 0: at most 180 degrees; R < 0: more [N 3.5.3.1]. Of the two centres, the one that gives the sweep R
            // asks for in the programmed direction.
            bool found = false;
            for (const double sgn : {1.0, -1.0}) {
                const double tu = mu + sgn * h * nu, tv = mv + sgn * h * nv;
                const double sw = sweepOf(u0 - tu, v0 - tv, u1 - tu, v1 - tv, cw, false);
                if ((r > 0 && sw <= kPi + 1e-9) || (r < 0 && sw >= kPi - 1e-9)) {
                    cu = tu, cv = tv;
                    found = true;
                    break;
                }
            }
            if (!found) {
                error(line_, "arc-radius", "no centre gives the arc R describes");
                return;
            }
        } else {
            if (w.count(offs[0]) == 0 && w.count(offs[1]) == 0) {
                error(line_, "arc-format",
                      std::string("an arc without ") + offs[0] + " or " + offs[1] + " [N 3.5.3.2]");
                return;
            }
            // Offsets from the start point, in every distance mode [N 3.5.17].
            const double ou = w.count(offs[0]) != 0 ? toMm(axisWord(w[offs[0]])) : 0;
            const double ov = w.count(offs[1]) != 0 ? toMm(axisWord(w[offs[1]])) : 0;
            cu = u0 + ou;
            cv = v0 + ov;
            const double r0 = std::hypot(u0 - cu, v0 - cv), r1 = std::hypot(u1 - cu, v1 - cv);
            if (std::fabs(r0 - r1) > radiusTol + 1e-12) {
                error(line_, "arc-radius",
                      "the start and end radii differ by " + fmt(std::fabs(r0 - r1)) + " mm [N 3.5.3.2]");
            }
        }
        s.sweep = sweepOf(u0 - cu, v0 - cv, u1 - cu, v1 - cv, cw, same && !s.rFormat);
        s.centre = fromPlane(plane_, cu, cv, a0);
        if (s.feed <= 0) {
            s.feed = 0;
        }
        checkSpindle(s, false);
        checkApproach(s.from, s.to, true);
        trackCut(s);
        push(s);
        pos_ = to;
    }

    static double sweepOf(double su, double sv, double eu, double ev, bool cw, bool full)
    {
        if (full) {
            return 2 * kPi;
        }
        const double a0 = std::atan2(sv, su), a1 = std::atan2(ev, eu);
        double d = cw ? a0 - a1 : a1 - a0;
        while (d <= 0) {
            d += 2 * kPi;
        }
        while (d > 2 * kPi) {
            d -= 2 * kPi;
        }
        return d;
    }

    // [N 3.5.12] G53: a straight move in machine coordinates, with G0 or G1, never with cutter compensation. The
    // machine position of the work offset is not known here: machine Z0 is the top of travel (kHomeZ) [ASSUMPTION,
    // generic-iso.md note 2, H1 p.43]; any other machine coordinate leaves that axis unknown.
    void machineMove(std::map<char, Word>& w)
    {
        if (!(motion_ == 0 || motion_ == 10)) {
            error(line_, "g53", "G53 without G0 or G1 in effect [N 3.5.12]");
            return;
        }
        if (motion_ == 10) {
            error(line_, "g53", "G1 G53 (a feed move in machine coordinates) is not modelled");
            return;
        }
        if (m_.comp != 0) {
            error(line_, "g53", "G53 with cutter compensation on [N 3.5.12]");
        }
        if (!distKnown_ || !absolute_) {
            error(line_, "g53", "G53 in incremental mode [ASSUMPTION: not modelled]");
            return;
        }
        if (!m_.unitsKnown) {
            error(line_, "units-unknown", "motion before G20 / G21");
            return;
        }
        Segment s;
        s.kind = SegKind::Rapid;
        s.origin = SegOrigin::Machine;
        s.from = pos_;
        P3 to = pos_;
        bool any = false;
        if (w.count('X') != 0) {
            to.x = kUnknown;
            any = true;
        }
        if (w.count('Y') != 0) {
            to.y = kUnknown;
            any = true;
        }
        if (w.count('Z') != 0) {
            to.z = axisWord(w['Z']) == 0 ? kHomeZ : kUnknown;
            any = true;
        }
        if (!any) {
            error(line_, "g53", "G53 without an axis word [N 3.5.12]");
            return;
        }
        s.to = to;
        trackCut(s);
        push(s);
        pos_ = to;
    }

    // [N 3.5.8] G28: a rapid to the intermediate point (the axis words, absolute or incremental), then to the home or
    // reference position. FANUC and Haas move only the axes named [F1, ASSUMPTION; Haas ASSUMPTION]; RS274/NGC moves
    // to the home position of parameters 5161-5166, a whole point, so X and Y go home too. In every case the home /
    // reference position is assumed to be at machine Z zero, above the work [ASSUMPTION].
    void referenceReturn(std::map<char, Word>& w)
    {
        if (!m_.unitsKnown || !distKnown_) {
            motionPreconditions();
            return;
        }
        if (m_.comp != 0) {
            error(line_, "g28", "G28 with cutter compensation on [ASSUMPTION: not modelled]");
        }
        if (cyc_.active) {
            safety(line_, "safety-g80-missing", "G28 while a canned cycle is active, without G80 first");
            endCycle();
            motion_ = -1;
        }
        const bool named[3] = {w.count('X') != 0, w.count('Y') != 0, w.count('Z') != 0};
        if (!named[0] && !named[1] && !named[2] && d_.family == Family::Fanuc) {
            error(line_, "g28", "G28 without axis words [F1, ASSUMPTION]");
            return;
        }
        P3 mid;
        if (!target(w, mid)) {
            return;
        }
        Segment s;
        s.kind = SegKind::Rapid;
        s.origin = SegOrigin::Reference;
        s.from = pos_;
        s.to = mid;
        const bool movesToMid = !sameAxis(mid.x, pos_.x) || !sameAxis(mid.y, pos_.y) || !sameAxis(mid.z, pos_.z);
        if (movesToMid) {
            checkApproach(s.from, s.to, false);
            trackCut(s);
            push(s);
        }
        P3 to = mid;
        const bool all = d_.family == Family::Ngc || (!named[0] && !named[1] && !named[2]);
        if (all || named[0]) {
            to.x = kUnknown;
        }
        if (all || named[1]) {
            to.y = kUnknown;
        }
        if (all || named[2]) {
            to.z = kHomeZ;
        }
        Segment h = s;
        h.from = mid;
        h.to = to;
        trackCut(h);
        push(h);
        pos_ = to;
    }

    void endCycle()
    {
        cyc_ = Cycle{};
        m_.rigidTap = false; // [F1, ASSUMPTION] G80 ends the rigid tapping mode of M29
    }

    // One canned-cycle block [N 3.5.16; FANUC F1 ASSUMPTION; Haas H1 p.72-95 per haas-ngc.md].
    void runCycle(std::map<char, Word>& w)
    {
        if (!motionPreconditions()) {
            return;
        }
        const int code = motion_;
        if (plane_ != 170) {
            error(line_, "cycle-plane", "a canned cycle outside the XY plane is not modelled");
            return;
        }
        if (!absolute_) {
            error(line_, "cycle-incremental", "a canned cycle in incremental mode is not modelled");
            return;
        }
        if (m_.comp != 0) {
            error(line_, "cycle-comp", "a canned cycle with cutter compensation on [N 3.5.16]");
        }
        if (retractMode_ == 0) {
            error(line_, "cycle-return", "a canned cycle with no G98 / G99 given [ASSUMPTION: power-up mode unknown]");
            return;
        }
        const bool sameCycle = cyc_.active && cyc_.code == code;
        if (!cyc_.active) {
            // FANUC and Haas: the initial level is where Z is when the cycle starts [H1 p.72 per haas-ngc.md;
            // F1 ASSUMPTION].
            cyc_.initialZ = pos_.z;
        }
        cyc_.active = true;
        if (cyc_.code != code) {
            cyc_.code = code;
            if (d_.family == Family::Ngc || !sameCycle) {
                // [N 3.5.16] Z must be given again unless the same cycle was already active; Q and P are not sticky
                // at all in RS274/NGC (only R and Z are named sticky). FANUC / Haas: a new cycle code is given its
                // data again [ASSUMPTION].
                cyc_.haveZ = false;
                cyc_.haveQ = false;
                cyc_.haveP = false;
                cyc_.ijk = false;
            }
        }
        if (w.count('Z') != 0) {
            cyc_.z = toMm(axisWord(w['Z']));
            cyc_.haveZ = true;
        }
        if (w.count('R') != 0) {
            cyc_.r = toMm(axisWord(w['R']));
            cyc_.haveR = true;
        }
        if (!cyc_.haveZ) {
            error(line_, "cycle-words", "a canned cycle without Z [N 3.5.16]");
            return;
        }
        if (!cyc_.haveR) {
            error(line_, "cycle-words", "a canned cycle without R [N 3.5.16]");
            return;
        }
        if (cyc_.r < cyc_.z) {
            error(line_, "cycle-words", "R below Z [N 3.5.16]");
            return;
        }
        // Peck words.
        if (code == 830 || code == 730) {
            if (w.count('Q') != 0) {
                cyc_.q = toMm(axisWord(w['Q']));
                cyc_.haveQ = true;
                cyc_.ijk = false;
                if (cyc_.q <= 0) {
                    error(line_, "cycle-words", "Q not positive [N 3.5.16.4]");
                }
            } else if (w.count('I') != 0) {
                // Haas G83 I (first peck), J (reduction per peck), K (smallest peck) [H1 p.77 per haas-ngc.md]
                cyc_.ijk = true;
                cyc_.haveQ = true;
                cyc_.i = toMm(axisWord(w['I']));
                cyc_.j = w.count('J') != 0 ? toMm(axisWord(w['J'])) : 0;
                cyc_.k = w.count('K') != 0 ? toMm(axisWord(w['K'])) : cyc_.i;
                if (cyc_.i <= 0 || cyc_.j < 0 || cyc_.k <= 0) {
                    error(line_, "cycle-words", "G83 I, J, K must be positive");
                }
            } else if (d_.family == Family::Ngc && cyc_.haveQ) {
                error(line_, "cycle-sticky",
                      "G83 without Q on a repeated block: RS274/NGC names only R and Z sticky, and Q is required "
                      "[N 3.5.16, 3.5.16.4]");
            }
            if (!cyc_.haveQ) {
                error(line_, "cycle-words", gName(code) + " without Q");
                return;
            }
        }
        if (w.count('P') != 0) {
            double s = 0, tol = 0;
            if (dwellSeconds(w['P'], s, tol, true)) {
                cyc_.p = s;
                cyc_.pTol = tol;
                cyc_.haveP = true;
            }
        } else if ((code == 820 || code == 890) && d_.family == Family::Ngc && cyc_.haveP) {
            error(line_, "cycle-sticky",
                  gName(code) + " without P on a repeated block: RS274/NGC names only R and Z sticky, and P is "
                                "required [N 3.5.16, 3.5.16.3, 3.5.16.10]");
        }
        if ((code == 820 || code == 890) && !cyc_.haveP) {
            error(line_, "cycle-words", gName(code) + " without P [N 3.5.16.3, 3.5.16.10]");
        }
        if (fWord_ <= 0) {
            error(line_, "no-feed", "a canned cycle with no feed rate");
        }
        // Where the hole is: FANUC / Haas drill only on a block with X or Y [ASSUMPTION; Haas Setting 28 decides a
        // block without]; RS274/NGC on any block with X, Y or Z [N 3.5.16].
        const bool xy = w.count('X') != 0 || w.count('Y') != 0;
        if (!xy && !(d_.family == Family::Ngc && w.count('Z') != 0)) {
            error(line_, "cycle-words", "a canned-cycle block without X or Y (whether it drills is a control setting)");
            return;
        }
        P3 hole;
        if (!target(w, hole)) {
            return;
        }
        if (!known(pos_.z) || !known(hole.x) || !known(hole.y)) {
            error(line_, "feed-from-unknown", "a canned cycle from a position not known in work coordinates");
            return;
        }
        // The level the tool returns to.
        double clear = cyc_.r;
        if (d_.family == Family::Ngc) {
            // [N 3.5.16, 3.5.20] G98: the Z the axis was at "just before the canned cycle started", if above R. Read
            // per block: in the example of [N 3.5.16.2] old Z is simply the current Z before the block [ASSUMPTION
            // where the text is ambiguous for a run of blocks].
            if (retractMode_ == 980 && pos_.z > cyc_.r) {
                clear = pos_.z;
            }
        } else if (retractMode_ == 980) {
            clear = cyc_.initialZ; // FANUC / Haas: the initial level [H1 p.72; F1 ASSUMPTION]
            if (clear < cyc_.r) {
                error(line_, "cycle-return", "G98 with the initial level below R [ASSUMPTION: not modelled]");
                clear = cyc_.r;
            }
        }
        // Spindle for tapping.
        int tapDir = 0;
        bool drives = false;
        if (code == 840 || code == 740) {
            tapDir = code == 840 ? 1 : -1;
            drives = d_.family == Family::Haas || (d_.family == Family::Fanuc && m_.rigidTap);
            if (!drives && m_.spindle != tapDir) {
                // [N 3.5.16.5] the spindle must turn clockwise before G84; G74 the other way [F1, ASSUMPTION].
                error(line_, "tap-spindle",
                      std::string("the spindle is not turning ") + (tapDir > 0 ? "clockwise" : "counter-clockwise") +
                          " before " + gName(code) + " [N 3.5.16.5]");
            }
        }
        const int spindleBefore = m_.spindle;
        auto rapidTo = [&](P3 to, bool ret = false) {
            if (to.x == pos_.x && to.y == pos_.y && to.z == pos_.z) {
                return;
            }
            Segment s;
            s.kind = SegKind::Rapid;
            s.origin = SegOrigin::Cycle;
            s.cycleCode = code;
            s.cycleReturn = ret;
            s.from = pos_;
            s.to = to;
            checkApproach(s.from, s.to, false);
            push(s);
            pos_ = to;
        };
        auto feedTo = [&](double z) {
            Segment s;
            s.kind = SegKind::Feed;
            s.origin = SegOrigin::Cycle;
            s.cycleCode = code;
            s.from = pos_;
            s.to = pos_;
            s.to.z = z;
            s.feed = toMm(fWord_);
            checkSpindle(s, drives);
            checkApproach(s.from, s.to, true);
            push(s);
            pos_ = s.to;
        };
        auto dwell = [&]() {
            if (cyc_.haveP && cyc_.p > 0) {
                Segment s;
                s.kind = SegKind::Dwell;
                s.origin = SegOrigin::Cycle;
                s.cycleCode = code;
                s.from = s.to = pos_;
                s.dwell = cyc_.p;
                push(s);
                t_.segs.back().dwellTol = cyc_.pTol;
            }
        };
        // Preliminary motion [N 3.5.16.1]: below R, first up to R (RS274/NGC); then X/Y at the current Z, then Z to R.
        // FANUC / Haas below a raised R plane (a run whose R changes upwards): X/Y at the current Z first, then up to
        // R, the order of cam.h's steps 1 and 3 [ASSUMPTION: neither F1 nor the cited H1 pages say].
        if (pos_.z < cyc_.r && d_.family == Family::Ngc) {
            rapidTo(P3{pos_.x, pos_.y, cyc_.r});
        }
        inCut_ = false;
        rapidTo(P3{hole.x, hole.y, pos_.z});
        rapidTo(P3{hole.x, hole.y, cyc_.r});
        const double bottom = cyc_.z;
        const double d = d_.peckClearanceMm;
        switch (code) {
        case 810: // [N 3.5.16.2]
            feedTo(bottom);
            rapidTo(P3{hole.x, hole.y, clear}, true);
            break;
        case 820: // [N 3.5.16.3]
            feedTo(bottom);
            dwell();
            rapidTo(P3{hole.x, hole.y, clear}, true);
            break;
        case 830:
        case 730: {
            // G83 [N 3.5.16.4]: feed one peck, rapid out (RS274/NGC: to clear Z; FANUC / Haas: to R [ASSUMPTION;
            // cam.h's reading]), rapid back down to the clearance above the last depth, repeat. G73 (FANUC / Haas):
            // feed one peck, back off by the clearance, repeat [F1 ASSUMPTION; H1 p.95]. Each peck goes Q deeper than
            // the last depth (the first from R) [ASSUMPTION on the NIST wording "downward by delta"].
            double depth = cyc_.r;
            double peck = cyc_.ijk ? cyc_.i : cyc_.q;
            int guard = 0;
            while (depth > bottom + 1e-9) {
                if (++guard > 100000) {
                    error(line_, "cycle-words", "too many pecks");
                    break;
                }
                if (depth < cyc_.r) {
                    if (code == 830) {
                        rapidTo(P3{hole.x, hole.y, d_.family == Family::Ngc ? clear : cyc_.r});
                        rapidTo(P3{hole.x, hole.y, std::min(depth + d, cyc_.r)});
                    } else {
                        rapidTo(P3{hole.x, hole.y, std::min(depth + d, cyc_.r)});
                    }
                }
                const double next = std::max(depth - peck, bottom);
                feedTo(next);
                depth = next;
                if (cyc_.ijk) {
                    peck = std::max(peck - cyc_.j, cyc_.k);
                }
            }
            dwell();
            rapidTo(P3{hole.x, hole.y, clear}, true);
            break;
        }
        case 840:
        case 740: {
            // [N 3.5.16.5] feed in, reverse the spindle, feed out. FANUC / Haas feed out to R and then rapid to the
            // initial level for G98 [ASSUMPTION; H1 p.82]; RS274/NGC feeds out to clear Z.
            if (drives) {
                m_.spindle = tapDir;
            }
            feedTo(bottom);
            dwell();
            m_.spindle = -tapDir;
            if (d_.family == Family::Ngc) {
                feedTo(clear);
                t_.segs.back().cycleReturn = true;
            } else {
                feedTo(cyc_.r);
                if (clear != cyc_.r) {
                    rapidTo(P3{hole.x, hole.y, clear}, true);
                } else {
                    t_.segs.back().cycleReturn = true;
                }
            }
            m_.spindle = drives ? spindleBefore : tapDir;
            break;
        }
        case 850:
        case 890: {
            // [N 3.5.16.6, 3.5.16.10] feed in, (dwell), feed out. FANUC / Haas: out to R, then rapid to the initial
            // level for G98 [ASSUMPTION; H1 p.84, p.88].
            feedTo(bottom);
            if (code == 890) {
                dwell();
            }
            if (d_.family == Family::Ngc) {
                feedTo(clear);
                t_.segs.back().cycleReturn = true;
            } else {
                feedTo(cyc_.r);
                if (clear != cyc_.r) {
                    rapidTo(P3{hole.x, hole.y, clear}, true);
                } else {
                    t_.segs.back().cycleReturn = true;
                }
            }
            break;
        }
        default:
            error(line_, "unknown-g", gName(code) + " is not modelled");
            break;
        }
        inCut_ = false;
    }
};

} // namespace

Trace interpret(const std::string& text, const Dialect& dialect)
{
    Trace t;
    Interp in(dialect, t);
    in.run(text);
    return t;
}

} // namespace rtgc
