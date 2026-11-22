// The non-visual part of the tool library manager (task T-046), so it can be tested without a window:
//
//   Profile   the side view of a tool, and of its holder, to scale, in mm: an outline (x across the axis, y up from the
//             tip), the cutting part, and the holder's stack of cones above the stick-out. Drawn only from the
//             geometry fields (the ones Tool::geometryHash covers), so the drawing changes when and only when they do.
//   Catalog   one entry per tool or holder the manager lists (the open document's embedded ones and the library's),
//             and the filter: words of the text (name, vendor, order number, type, thread, comment), tool type,
//             diameter range (mm), a material that has a cutting preset. Results are sorted by a key (diameter, name or
//             type) with ties broken by name, then id; the document's items come first. Deterministic.
//   Fields    which fields the editor shows for a tool type, and the editor's values (text as typed, lengths in the
//             display unit) to the item's file format and back. The file format is checked against the item's schema
//             in src/lib (api::cam_tools::toolFromJson); every problem found is named by field in the schema's words
//             (the check gives the first problem, so each one found is set to the value it had and the check runs
//             again).
#pragma once

#include <ray/api.h>

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ray::ui::toolsui {

// ---------- Profile ----------
struct Profile {
    std::vector<Vec2> outline; // closed (the last point joins the first), mm
    std::vector<Vec2> cutter;  // the part from the tip to the flute length, closed
    std::vector<Vec2> holder;  // closed; empty without a holder
    double halfWidth = 0;      // the widest x of the three
    double height = 0;         // the highest y of the three (the tip is at y = 0)
    double stickOut = 0;       // where the holder's face is (0 without a holder)
};
// The tool alone (holder null), or in the holder with the given stick-out (<= 0: the tool's shoulder length, else its
// flute length, as an operation's default; never past the tool's overall length).
Profile toolProfile(const lib::Tool& tool, const lib::Holder* holder = nullptr, double stickOut = 0);
// A holder alone, its face at y = 0.
Profile holderProfile(const lib::Holder& holder);

// ---------- Catalog ----------
enum class SortKey { Diameter, Name, Type };

struct Entry {
    bool document = false;  // embedded in the open document (else from the library)
    bool holder = false;    // a holder (else a tool)
    QString id, name, type; // type: the file format's name ("flatEndMill"); "" for a holder
    double diameter = 0;    // mm: a tool's cutting diameter, a holder's largest
    int flutes = 0;
    int root = -1;               // library root it comes from (-1: the document's copy)
    bool readOnly = false;       // a built-in library item, or the document's copy
    bool libraryChanged = false; // document copy: the library's original has another shape (geometryHash)
    bool libraryDiffers = false; // document copy: the library's original differs in anything
    bool hasLibraryCopy = false;
    QString search;                 // lower-case words searched by the text filter
    std::vector<QString> materials; // ids of the materials it has a preset for (sorted)
};

struct Filter {
    QString text;            // words, all must match (case-insensitive)
    QString type;            // "" any, else the file format's type name
    double minDiameter = -1; // mm; < 0: no bound
    double maxDiameter = -1;
    QString material; // "" any, else a material id
    SortKey sort = SortKey::Diameter;
};

// The searched text of a tool: name, vendor, product id, type name and title, thread designation, body material,
// comment; lower case.
QString searchText(const lib::Tool& tool, const QString& typeTitle);
QString searchText(const lib::Holder& holder);
// Every index of `entries` in display order for the key: the document's first, then by the key, ties by name, then id.
std::vector<int> sortEntries(const std::vector<Entry>& entries, SortKey sort);
// The indices of `entries` that pass, in display order. `order`: sortEntries(entries, filter.sort) made before (a
// list filters again on every key stroke without sorting again); null to sort here.
std::vector<int> filterEntries(const std::vector<Entry>& entries, const Filter& filter,
                               const std::vector<int>* order = nullptr);

// ---------- Fields ----------
// Kinds: "text", "multiline", "choice", "int", "length", "angle", "fraction", "speed".
struct Field {
    const char* key;
    const char* label; // in the ray::ui::ToolLibraryModel translation context
    const char* kind;
};
inline constexpr const char* kTrContext = "ray::ui::ToolLibraryModel";
// The tool fields the editor shows for a type, in order (name and type first; comment last).
std::vector<Field> toolFields(lib::ToolType type);
const std::vector<Field>& presetFields();
const std::vector<Field>& holderFields();
const std::vector<Field>& segmentFields();

// Text <-> value. Lengths: "12.7 mm" / "0.5 in" (the SetupPanel's form: up to 10 significant digits and the unit);
// speeds: "250 m/min" / "820.21 ft/min"; others: the number.
struct Units {
    bool inch = false;
    std::function<double(const QString&)> parseLength; // mm, NaN if it does not read (UnitsObject::parseLength)
};
QString formatValue(const char* kind, double value, const Units& units);
// The value of a text, in the file format's unit; nullopt (and `error` set) if it does not read.
std::optional<double> parseValue(const char* kind, const QString& text, const Units& units, QString* error);

// The editor's state of one item: every field as text (`fields`), the text it was shown with (`shown`), and presets
// (tools) or segments (holders) as rows of the same, each with the JSON it started from (`base`). A field whose text is
// still what was shown keeps the value it had exactly (no round trip through the display unit).
struct Row {
    std::map<QString, QString> fields, shown;
    QJsonObject base;
};
struct Values {
    std::map<QString, QString> fields, shown;
    std::vector<Row> rows;   // presets or segments
    bool rowsEdited = false; // a row was removed
};
Values toolValues(const lib::Tool& tool, const Units& units);
Values holderValues(const lib::Holder& holder, const Units& units);
// A new preset row (the first material, the type's usual kind of cut) and a new segment row.
Row newPresetRow(lib::ToolType type, const QString& material, const Units& units);
Row newSegmentRow(const Units& units);
// The values differ from what was shown (a row added, removed or changed counts).
bool edited(const Values& values);

// The item's file format from the values, starting from `base` (its JSON: keys the editor does not show stay as they
// are), and every problem by field key ("diameter", "presets.0.chipLoad", "segments.1.height"; "" for one that names no
// field).
struct Checked {
    QJsonObject json;
    std::map<QString, QString> problems;
    // The values with each field that has a problem set back to what it was: an item that passes its schema (empty
    // when even that does not), for the preview while a field is being typed.
    QJsonObject valid;
    bool ok() const { return problems.empty(); }
};
Checked checkTool(const Values& values, const QJsonObject& base, const Units& units);
Checked checkHolder(const Values& values, const QJsonObject& base, const Units& units);

// The field key of a schema message's JSON Pointer ("/presets/0/chipLoad: must be ..." -> "presets.0.chipLoad") and
// the message without it; key "" when it names none.
std::pair<QString, QString> splitProblem(const QString& message);

} // namespace ray::ui::toolsui
