// The engraving font and text layout (engrave_font.h, task T-056).
#include "engrave_font.h"

#include <cmath>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace ray::cam::ops {

namespace detail {
// src/cam/CMakeLists.txt generates these from resources/fonts/raycad-single.rsf.
extern const unsigned char kEngraveFontData[];
extern const std::size_t kEngraveFontSize;
} // namespace detail

namespace {

tl::unexpected<Error> formatError(std::size_t line, const std::string& what)
{
    return tl::unexpected(Error{ErrorCode::FormatError, "Font line " + std::to_string(line) + ": " + what, {}});
}

std::vector<std::string_view> tokens(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') {
            ++j;
        }
        if (j > i) {
            out.push_back(line.substr(i, j - i));
        }
        i = j;
    }
    return out;
}

// A decimal with at most three decimals, in thousandths; nullopt for anything else.
std::optional<std::int64_t> milli(std::string_view s)
{
    bool negative = false;
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        ++i;
    }
    std::int64_t whole = 0;
    std::size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (whole > 1'000'000'000) {
            return std::nullopt;
        }
        whole = whole * 10 + (s[i] - '0');
        ++i;
        ++digits;
    }
    std::int64_t frac = 0;
    int decimals = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (decimals == 3) {
                return std::nullopt;
            }
            frac = frac * 10 + (s[i] - '0');
            ++decimals;
            ++i;
        }
        if (decimals == 0) {
            return std::nullopt;
        }
    }
    if (i != s.size() || digits + static_cast<std::size_t>(decimals) == 0) {
        return std::nullopt;
    }
    while (decimals < 3) {
        frac *= 10;
        ++decimals;
    }
    const std::int64_t v = whole * 1000 + frac;
    return negative ? -v : v;
}

// |p - c| for milli-unit points, exactly enough for the 0.001 unit check.
double distance(std::int64_t px, std::int64_t py, std::int64_t cx, std::int64_t cy)
{
    return dmath::hypot(static_cast<double>(px - cx), static_cast<double>(py - cy));
}

// A stroke: "M x y" then segments.
Result<FontStroke> parseStroke(const std::vector<std::string_view>& t, std::size_t line)
{
    FontStroke s;
    if (t.size() < 3) {
        return formatError(line, "a stroke starts with M x y");
    }
    const auto x0 = milli(t[1]), y0 = milli(t[2]);
    if (!x0 || !y0) {
        return formatError(line, "the start point is not two numbers with at most three decimals");
    }
    s.x = *x0;
    s.y = *y0;
    std::int64_t px = s.x, py = s.y;
    std::size_t i = 3;
    while (i < t.size()) {
        FontSegment seg;
        if (t[i] == "L") {
            if (i + 2 >= t.size()) {
                return formatError(line, "L needs x y");
            }
            const auto x = milli(t[i + 1]), y = milli(t[i + 2]);
            if (!x || !y) {
                return formatError(line, "a line's end is not two numbers with at most three decimals");
            }
            seg.x = *x;
            seg.y = *y;
            i += 3;
        } else if (t[i] == "A") {
            if (i + 5 >= t.size()) {
                return formatError(line, "A needs x y cx cy and + or -");
            }
            const auto x = milli(t[i + 1]), y = milli(t[i + 2]), cx = milli(t[i + 3]), cy = milli(t[i + 4]);
            if (!x || !y || !cx || !cy) {
                return formatError(line, "an arc's end or centre is not two numbers with at most three decimals");
            }
            if (t[i + 5] != "+" && t[i + 5] != "-") {
                return formatError(line, "an arc's direction is + (counter-clockwise) or - (clockwise)");
            }
            seg.arc = true;
            seg.ccw = t[i + 5] == "+";
            seg.x = *x;
            seg.y = *y;
            seg.cx = *cx;
            seg.cy = *cy;
            const double r0 = distance(px, py, seg.cx, seg.cy);
            const double r1 = distance(seg.x, seg.y, seg.cx, seg.cy);
            if (r0 < 1 || std::abs(r0 - r1) > 1.0) {
                return formatError(line, "an arc's start and end are not on one circle about its centre");
            }
            i += 6;
        } else {
            return formatError(line, "'" + std::string(t[i]) + "' is not L or A");
        }
        px = seg.x;
        py = seg.y;
        s.segments.push_back(seg);
    }
    if (s.segments.empty()) {
        return formatError(line, "a stroke needs at least one segment (a dot is a short line)");
    }
    return s;
}

} // namespace

Result<StrokeFont> parseStrokeFont(std::string_view text)
{
    try {
        StrokeFont font;
        FontGlyph* glyph = nullptr;
        char32_t code = 0;
        std::size_t lineNo = 0;
        std::size_t at = 0;
        while (at <= text.size()) {
            std::size_t end = text.find('\n', at);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            std::string_view line = text.substr(at, end - at);
            at = end + 1;
            ++lineNo;
            if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
                line = line.substr(0, hash);
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            const std::vector<std::string_view> t = tokens(line);
            if (t.empty()) {
                if (end == text.size()) {
                    break;
                }
                continue;
            }
            if (t[0] == "font") {
                if (t.size() != 2) {
                    return formatError(lineNo, "font takes one word, the name");
                }
                font.name = std::string(t[1]);
            } else if (t[0] == "cap") {
                const auto c = t.size() == 2 ? milli(t[1]) : std::nullopt;
                if (!c || *c <= 0) {
                    return formatError(lineNo, "cap takes the cap height, a positive number");
                }
                font.cap = *c;
            } else if (t[0] == "glyph") {
                if (glyph != nullptr) {
                    return formatError(lineNo, "a glyph starts before the previous one ends");
                }
                if (t.size() != 3 || t[1].size() < 3 || t[1].substr(0, 2) != "U+") {
                    return formatError(lineNo, "glyph takes U+XXXX and the advance width");
                }
                std::uint32_t cp = 0;
                for (const char ch : t[1].substr(2)) {
                    const int d = ch >= '0' && ch <= '9'   ? ch - '0'
                                  : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                  : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                           : -1;
                    if (d < 0 || cp > 0x10FFFF) {
                        return formatError(lineNo, "the code point is not hexadecimal");
                    }
                    cp = cp * 16 + static_cast<std::uint32_t>(d);
                }
                const auto advance = milli(t[2]);
                if (!advance || *advance <= 0) {
                    return formatError(lineNo, "the advance width must be a positive number");
                }
                code = static_cast<char32_t>(cp);
                if (font.glyphs.count(code) != 0) {
                    return formatError(lineNo, "the code point " + std::string(t[1]) + " has a glyph already");
                }
                glyph = &font.glyphs[code];
                glyph->advance = *advance;
            } else if (t[0] == "end") {
                if (glyph == nullptr) {
                    return formatError(lineNo, "end without a glyph");
                }
                glyph = nullptr;
            } else if (t[0] == "M") {
                if (glyph == nullptr) {
                    return formatError(lineNo, "a stroke outside a glyph");
                }
                auto stroke = parseStroke(t, lineNo);
                if (!stroke) {
                    return tl::unexpected(stroke.error());
                }
                glyph->strokes.push_back(std::move(*stroke));
            } else {
                return formatError(lineNo, "'" + std::string(t[0]) + "' is not a statement of the font format");
            }
            if (end == text.size()) {
                break;
            }
        }
        if (glyph != nullptr) {
            return formatError(lineNo, "the last glyph has no end");
        }
        if (font.cap <= 0) {
            return formatError(lineNo, "the font gives no cap height");
        }
        if (font.glyphs.empty()) {
            return formatError(lineNo, "the font has no glyphs");
        }
        return font;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while reading the engraving font", {}});
    }
}

std::string_view engravingFontText()
{
    return std::string_view(reinterpret_cast<const char*>(detail::kEngraveFontData), detail::kEngraveFontSize);
}

const StrokeFont* engravingFont()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const StrokeFont> font = []() -> std::shared_ptr<const StrokeFont> {
        auto parsed = parseStrokeFont(engravingFontText());
        return parsed ? std::make_shared<const StrokeFont>(std::move(*parsed)) : nullptr;
    }();
    return font.get();
}

std::optional<std::u32string> decodeUtf8(std::string_view text)
{
    std::u32string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        std::size_t n = 0;
        char32_t cp = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            n = 1;
            cp = b0 & 0x1F;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            n = 2;
            cp = b0 & 0x0F;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            n = 3;
            cp = b0 & 0x07;
        } else {
            return std::nullopt;
        }
        if (n > 0 && i + n >= text.size()) {
            return std::nullopt;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            const auto b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) {
                return std::nullopt;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return std::nullopt;
        }
        out.push_back(cp);
        i += n + 1;
    }
    return out;
}

double textWidthMm(const StrokeFont& font, const TextLayout& layout)
{
    const auto codes = decodeUtf8(layout.text);
    if (!codes || codes->empty() || font.cap <= 0) {
        return 0;
    }
    std::int64_t advance = 0;
    for (const char32_t c : *codes) {
        if (const auto it = font.glyphs.find(c); it != font.glyphs.end()) {
            advance += it->second.advance;
        }
    }
    const double s = layout.height / static_cast<double>(font.cap); // mm per milli-unit
    return static_cast<double>(advance) * s + layout.spacing * static_cast<double>(codes->size() - 1);
}

namespace {

std::string codeText(char32_t c)
{
    static const char* const hex = "0123456789ABCDEF";
    std::string digits;
    std::uint32_t v = static_cast<std::uint32_t>(c);
    do {
        digits.insert(digits.begin(), hex[v % 16]);
        v /= 16;
    } while (v != 0);
    while (digits.size() < 4) {
        digits.insert(digits.begin(), '0');
    }
    return "U+" + digits;
}

std::string utf8Of(char32_t c)
{
    std::string s;
    const auto u = static_cast<std::uint32_t>(c);
    if (u < 0x80) {
        s += static_cast<char>(u);
    } else if (u < 0x800) {
        s += static_cast<char>(0xC0 | (u >> 6));
        s += static_cast<char>(0x80 | (u & 0x3F));
    } else if (u < 0x10000) {
        s += static_cast<char>(0xE0 | (u >> 12));
        s += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (u & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (u >> 18));
        s += static_cast<char>(0x80 | ((u >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (u & 0x3F));
    }
    return s;
}

struct Frame {
    double x = 0, y = 0, c = 1, s = 0, scale = 0; // scale: mm per milli-unit
    // The WCS point, mm, of local (pen + u scale, v scale).
    std::pair<double, double> at(double pen, std::int64_t u, std::int64_t v) const
    {
        const double lx = pen + static_cast<double>(u) * scale;
        const double ly = static_cast<double>(v) * scale;
        return {x + c * lx - s * ly, y + s * lx + c * ly};
    }
};

geom2d::Pt rounded(std::pair<double, double> mm)
{
    return geom2d::Pt{geom2d::roundNm(mm.first * kNmPerMm), geom2d::roundNm(mm.second * kNmPerMm)};
}

double dist(geom2d::Pt a, std::pair<double, double> bNm)
{
    return dmath::hypot(static_cast<double>(a.x) - bNm.first, static_cast<double>(a.y) - bNm.second);
}

} // namespace

Result<geom2d::Paths> layoutText(const StrokeFont& font, const TextLayout& layout)
{
    using geom2d::Pt;
    using geom2d::Span;
    using geom2d::SpanKind;
    const auto codes = decodeUtf8(layout.text);
    if (!codes) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "The text is not valid UTF-8", {}});
    }
    for (const char32_t c : *codes) {
        if (font.glyphs.count(c) == 0) {
            const std::string shown = c >= 0x20 && c != 0x7F ? "'" + utf8Of(c) + "' " : std::string();
            return tl::unexpected(Error{ErrorCode::InvalidArgument,
                                        "The engraving font has no character " + shown + "(" + codeText(c) +
                                            ") in the text; it has A to Z, a to z, 0 to 9, space and . , : ; - + = / # "
                                            "( ) [ ] < > _ ! ? * ' \" % and the degree and diameter signs",
                                        {}});
        }
    }
    geom2d::Paths out;
    if (codes->empty()) {
        return out;
    }
    const double a = layout.angleDeg * (3.141592653589793238462643383279502884 / 180.0);
    Frame f;
    f.x = layout.x;
    f.y = layout.y;
    f.c = dmath::cos(a);
    f.s = dmath::sin(a);
    f.scale = layout.height / static_cast<double>(font.cap);
    const double width = textWidthMm(font, layout);
    double pen = layout.align == TextAlign::Left ? 0.0 : layout.align == TextAlign::Centre ? -width / 2 : -width;
    const auto tooSmall = [] {
        return tl::unexpected(Error{
            ErrorCode::InvalidArgument, "The text is too small to engrave: an arc of a character is under 1 nm", {}});
    };
    for (const char32_t c : *codes) {
        const FontGlyph& g = font.glyphs.at(c);
        for (const FontStroke& st : g.strokes) {
            geom2d::Path p;
            p.start = rounded(f.at(pen, st.x, st.y));
            Pt cur = p.start;
            for (std::size_t k = 0; k < st.segments.size(); ++k) {
                const FontSegment& seg = st.segments[k];
                const bool closes = seg.x == st.x && seg.y == st.y;
                if (!seg.arc) {
                    const Pt e = closes ? p.start : rounded(f.at(pen, seg.x, seg.y));
                    if (e != cur) {
                        p.spans.push_back(Span{SpanKind::Line, e, {}, 0});
                    }
                    cur = e;
                    continue;
                }
                const SpanKind kind = seg.ccw ? SpanKind::ArcCCW : SpanKind::ArcCW;
                const auto cMm = f.at(pen, seg.cx, seg.cy);
                const std::pair<double, double> cNm{cMm.first * kNmPerMm, cMm.second * kNmPerMm};
                const bool whole = k == 0 ? closes : (seg.x == st.segments[k - 1].x && seg.y == st.segments[k - 1].y);
                if (whole) {
                    const Pt centre = rounded(cMm);
                    const geom2d::Coord r =
                        geom2d::roundNm(dist(cur, {static_cast<double>(centre.x), static_cast<double>(centre.y)}));
                    if (r < 1) {
                        return tooSmall();
                    }
                    p.spans.push_back(Span{kind, cur, centre, r});
                    continue;
                }
                const auto eMm = f.at(pen, seg.x, seg.y);
                if (closes) {
                    // The end is the stroke's start: the centre onto the bisector of the two ends, then rounded.
                    const Pt e = p.start;
                    const double mx = (static_cast<double>(cur.x) + static_cast<double>(e.x)) / 2;
                    const double my = (static_cast<double>(cur.y) + static_cast<double>(e.y)) / 2;
                    double nx = -(static_cast<double>(e.y) - static_cast<double>(cur.y));
                    double ny = static_cast<double>(e.x) - static_cast<double>(cur.x);
                    const double nl = dmath::hypot(nx, ny);
                    Pt centre = rounded(cMm);
                    if (nl > 0) {
                        nx /= nl;
                        ny /= nl;
                        const double t = (cNm.first - mx) * nx + (cNm.second - my) * ny;
                        centre = Pt{geom2d::roundNm(mx + t * nx), geom2d::roundNm(my + t * ny)};
                    }
                    const double cx = static_cast<double>(centre.x), cy = static_cast<double>(centre.y);
                    const geom2d::Coord r = geom2d::roundNm((dist(cur, {cx, cy}) + dist(e, {cx, cy})) / 2);
                    if (r < 1) {
                        return tooSmall();
                    }
                    p.spans.push_back(Span{kind, e, centre, r});
                    cur = e;
                    continue;
                }
                const Pt centre = rounded(cMm);
                const double cx = static_cast<double>(centre.x), cy = static_cast<double>(centre.y);
                const geom2d::Coord r = geom2d::roundNm(dist(cur, {cx, cy}));
                const double ex = eMm.first * kNmPerMm - cx, ey = eMm.second * kNmPerMm - cy;
                const double el = dmath::hypot(ex, ey);
                if (r < 1 || !(el > 0)) {
                    return tooSmall();
                }
                const Pt e{geom2d::roundNm(cx + static_cast<double>(r) * ex / el),
                           geom2d::roundNm(cy + static_cast<double>(r) * ey / el)};
                p.spans.push_back(Span{kind, e, centre, r});
                cur = e;
            }
            const FontSegment& last = st.segments.back();
            p.closed = last.x == st.x && last.y == st.y && !p.spans.empty() && p.spans.back().end == p.start;
            if (!p.spans.empty()) {
                out.push_back(std::move(p));
            }
        }
        pen += static_cast<double>(g.advance) * f.scale + layout.spacing;
    }
    return out;
}

} // namespace ray::cam::ops
