// The engraving font and text layout of the "engrave" operation type (task T-056), private to src/cam and its tests.
//
// THE FONT. resources/fonts/raycad-single.rsf, "RayCAD Single Line", is a single-stroke font drawn for RayCAD (original
// work under the project licence; the file's header has the format). src/cam/CMakeLists.txt compiles the file in, so
// the program and its tests read the same bytes and a toolpath never depends on a file on disk. It is parsed once
// (engravingFont) into integer milli-units (a thousandth of a font unit), so parsing is exact.
//
// LAYOUT (layoutText), every result in integer nanometres of the WCS:
//   s = height / cap, the size of one font unit in mm (cap: the font's cap height, 12 units).
//   The characters are placed left to right: character k starts at pen_k = offset + sum over i < k of
//   (advance_i * s + spacing). The text's width W = sum of all advances * s + spacing * (n - 1); offset is 0 (left),
//   -W / 2 (centre) or -W (right), so the anchor (x, y) is on the baseline at the left end, the middle or the right
//   end of the text. A point (u, v) of character k is at local (pen_k + u s, v s) and goes to the WCS as
//   (x + cos(a) lx - sin(a) ly, y + sin(a) lx + cos(a) ly), a the angle (dmath::cos, dmath::sin), each coordinate
//   rounded half to even to the nanometre (geom2d::roundNm).
//   Arcs, so that both ends of every arc lie within 1 nm of their circle (geom2d's rule; the toolpath invariant checker
//   allows 2 nm between the start and end radius): the centre C is the rounded transformed centre; the arc starts at
//   the stroke's current point S (already rounded) and its radius is R = roundNm(|S - C|); its end is
//   C + R (E - C) / |E - C| rounded, E the transformed end, so it lies within 0.71 nm of the circle. A whole circle
//   ends at S. The arc that closes a stroke (its end is the stroke's start in the font) ends at the stroke's start
//   point exactly: its centre is then the transformed centre moved onto the perpendicular bisector of its two ends and
//   rounded, and R the rounded mean of the two distances (both within 1.2 nm of R, their difference under 1.5 nm).
//   A stroke whose end is its start in the font is a closed path.
//   LINES (after M3): a line break ("\n"; "\r\n" and "\r" too) starts a new line. Line i (from 0) has its baseline
//   lineSpacing * i below the first one's (local ly - lineSpacing * i, added only for i > 0, so a single line lays out
//   with exactly the arithmetic it had), and is aligned on its own: its offset is 0, -W_i / 2 or -W_i by its own
//   width W_i. The anchor (x, y) is on the first line's baseline. An empty line takes its place and draws nothing.
// Deterministic: integer font data, IEEE double arithmetic with dmath (src/cam is built with strict floating point),
// one rounding rule; no locale, no clock.
#pragma once

#include "../geom2d.h"

#include <ray/cam.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::cam::ops {

// ---------- The font ----------
// Coordinates are integer milli-units of the font (the file's numbers times 1000).
struct FontSegment {
    bool arc = false;
    bool ccw = false; // arcs
    std::int64_t x = 0, y = 0;
    std::int64_t cx = 0, cy = 0; // arcs: the centre
};
struct FontStroke {
    std::int64_t x = 0, y = 0; // the start point
    std::vector<FontSegment> segments;
};
struct FontGlyph {
    std::int64_t advance = 0;
    std::vector<FontStroke> strokes;
};
struct StrokeFont {
    std::string name;
    std::int64_t cap = 0; // the cap height, milli-units
    std::map<char32_t, FontGlyph> glyphs;
};

// A font from its text (the format in resources/fonts/raycad-single.rsf). FormatError naming the line and the
// problem: an unknown statement, a number with more than three decimals, a glyph without "end", a stroke without a
// segment, an arc whose ends are not on one circle (within 0.001 unit), a code point given twice, no cap height.
Result<StrokeFont> parseStrokeFont(std::string_view text);
// The compiled-in text of resources/fonts/raycad-single.rsf.
std::string_view engravingFontText();
// The shipped font, parsed once and then shared read-only; null if it does not parse (the font tests catch that).
const StrokeFont* engravingFont();

// The code points of UTF-8 text; nullopt when the text is not well-formed UTF-8.
std::optional<std::u32string> decodeUtf8(std::string_view text);

// ---------- Layout ----------
enum class TextAlign : std::uint8_t { Left, Centre, Right };
struct TextLayout {
    std::string text;                  // UTF-8; line breaks separate lines (LINES above)
    double height = 5;                 // mm: the height of the capitals (and digits)
    double spacing = 0;                // mm added between two characters
    TextAlign align = TextAlign::Left; // of each line on its own
    double x = 0, y = 0;               // mm, WCS: the anchor on the first line's baseline
    double angleDeg = 0;               // the baseline's direction, counter-clockwise from +X
    double lineSpacing = 0;            // mm from one baseline to the next; 0: 1.6 times the height
};
// The lines of a text (split at "\n", "\r\n" and "\r"); one line, the text itself, when it has no break.
std::vector<std::string> textLines(std::string_view text);
// The text's width in mm: its widest line's W (LAYOUT above).
double textWidthMm(const StrokeFont& font, const TextLayout& layout);
// The strokes of the text as paths (open, or closed where a stroke ends at its start), in reading order: lines top
// down, characters left to right, each character's strokes in the font's order, each stroke in its drawn direction.
// Characters without strokes (the space) give none. InvalidArgument when the text is not UTF-8, or names a character
// the font has not (the message says which, with its code point), or when the text is so small that an arc's radius
// rounds below 1 nm.
Result<geom2d::Paths> layoutText(const StrokeFont& font, const TextLayout& layout);

} // namespace ray::cam::ops
