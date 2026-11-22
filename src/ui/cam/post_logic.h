// The non-visual part of posting and the G-code viewer (task T-047), tested without a window
// (tests/ui/cam/post_unit_test.cpp):
//
//   Tokens     tokenize() splits one G-code line into the words the viewer colours: comments ("( ... )" and ";" to
//              the end), sequence numbers (N), program numbers (O), G words, M words, axis words (X Y Z A B C U V W
//              I J K R), feeds and speeds (F S), other words (T H D P Q L E ...), and plain text ("%", spaces, what
//              does not read as a word).
//   Index      GcodeIndex: where each line of a program starts, and both directions of PostOutput::lineMap (a line
//              to its move; a move to its lines), built once on a worker so the UI thread never walks a large program.
//   Find       findAll(): every place a text occurs, case-insensitively: its line and column.
//   Messages   attachTo(): which section (operation) a post's warning or error names ("section 2 (Drill 1)"), and
//              which script line its error carries ("haas-ngc.post.js:215: ...").
// Display only (src/ui is not deterministic code, PLAN.md 8).
#pragma once

#include <ray/api.h> // post::GcodeLineMap (src/ui includes the api, PLAN.md 1.1)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::ui::camui {

// ---------- Tokens ----------
enum class GToken : std::uint8_t { Text, Comment, Sequence, Program, GWord, MWord, Axis, FeedSpeed, Word };
struct GSpan {
    std::uint32_t start = 0, length = 0; // bytes of the line (the posts write ASCII)
    GToken kind = GToken::Text;
};
std::vector<GSpan> tokenize(std::string_view line);
// The token kind's name, for tests: "text", "comment", "sequence", "program", "g", "m", "axis", "feed", "word".
const char* tokenName(GToken kind);

// ---------- Index ----------
struct MoveRef {
    std::uint32_t section = 0, move = 0;
};
struct GcodeIndex {
    std::shared_ptr<const std::string> text;
    std::vector<std::uint32_t> lineStart; // byte offset of each line; the line ends at the next "\n"
    std::vector<std::int32_t> lineEntry;  // per line: its index in `map`, or -1 (no move: header, tool change, end)
    std::vector<post::GcodeLineMap> map;  // PostOutput::lineMap (line order)
    std::vector<std::uint32_t> byMove;    // indices into `map`, sorted by (section, move, line)
    std::uint32_t longestLine = 0;        // bytes of the longest line (the view's width in columns)

    std::size_t lineCount() const { return lineStart.size(); }
    // The line's text without its line ending.
    std::string_view line(std::size_t i) const;
    // The bytes of lines first..last inclusive, line endings included (as the post wrote them).
    std::string_view lines(std::size_t first, std::size_t last) const;
    std::optional<MoveRef> moveOf(std::size_t line) const;
    // The lines of a move: [first, last], or nullopt when no line maps to it.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> linesOf(MoveRef move) const;
};
// Builds the index (a worker: linear in the program's size, sorts the line map).
std::shared_ptr<const GcodeIndex> buildIndex(std::shared_ptr<const std::string> text,
                                             std::vector<post::GcodeLineMap> lineMap);

// ---------- Find ----------
struct FindHit {
    std::uint32_t line = 0, column = 0;
};
// Every place `needle` occurs, case-insensitively (ASCII), in line order; at most `limit` hits.
std::vector<FindHit> findAll(const GcodeIndex& index, std::string_view needle, std::size_t limit = 100000);

// ---------- Messages ----------
struct MessagePlace {
    std::optional<std::uint32_t> section; // "section N" in the text (0-based, as the posts number sections)
    std::optional<int> scriptLine;        // "<script>.post.js:<line>:" at its start
    std::string script;                   // that script's file name
    std::size_t textStart = 0;            // where the message proper starts (after that prefix and a space)
};
MessagePlace attachTo(std::string_view message);

} // namespace ray::ui::camui
