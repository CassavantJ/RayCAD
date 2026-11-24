// The setup sheet (task T-076): renderSetupSheetHtml and renderSetupSheetsHtml (post.h) write one self-contained HTML
// document per call. Internal to src/post; the pieces below are here for the tests.
//
// What a sheet holds, in order (one per setup; the document of several setups starts each on a new page):
//   - the title (the setup's name) and the facts the operator checks first: program number and name, part and file,
//     machine, post, the units the program is written in, the date, the programmer
//   - the notice, while anything is unproven: a post shipped with RayCAD that declares no control it has run on
//     (PostInfo::verifiedOn empty) and a machine definition not checked against the real machine, worded as the post
//     dialog words it, with how to prove the program out; and a tool number given to two different tools
//   - the work offset (G54...), where its origin is on the stock in words, the stock's extent in work coordinates, the
//     flipped axes; the stock's shape, size and material; the picture of the part in its setup (a PNG, embedded)
//   - the tool list by tool number: description, diameter, flutes, stick-out, holder, length and diameter offsets and
//     the operations that use the tool
//   - the operations in program order: what they do, tool, deepest Z and stepdown, spindle and coolant, feeds (and the
//     chip load per tooth of a milling tool), the stock left on walls and floors, the estimated time, and every warning
//     the toolpath carries
//   - the estimated cycle time, stated as an estimate with what it assumed, and lines to sign the setup off.
// Text from the user or a library (names, comments, material, holder, post title, warnings) is HTML-escaped. Numbers
// are formatted with the post host's integer formatting (format.h) in the program's units; no clock, no locale: the
// same input gives the same bytes on every platform.
//
// The HTML uses only what Qt's QTextDocument renders as well as a browser does (resources/setupsheet/setupsheet.css
// says which parts), because the desktop makes the PDF from it with QTextDocument; the HTML stays the one source.
#pragma once
#include <ray/post.h>

#include <span>
#include <string>
#include <string_view>

namespace ray::post::sheet {

// & < > " ' as entities; control characters other than tab and newline become spaces.
std::string escapeHtml(std::string_view text);
// Standard base64 (RFC 4648, with padding).
std::string base64(std::span<const std::byte> bytes);
// h:mm:ss rounded to the nearest second ("0:00:07", "1:02:03"); "" when negative or not finite.
std::string duration(double seconds);
// The work offset's G code: 1..6 G54..G59, then G54.1 P1, P2, ... (the extended offsets of Fanuc and Haas).
std::string workOffsetCode(int workOffset);
// Where the WCS origin is, in shop words ("Top front left corner of the stock").
std::string originWords(const cam::Wcs& wcs);
// The print style (resources/setupsheet/setupsheet.css), as compiled in.
std::string_view styleSheet();

} // namespace ray::post::sheet
