// The library item kinds, their folders and file names, and the conversion between items and their JSON files.
// Private to src/lib.
#pragma once
#include "json.h"

#include <ray/libraries.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace ray::lib::detail {

enum class Kind { Tool, Holder, Material, Machine, Template };
inline constexpr std::array<Kind, 5> kAllKinds = {Kind::Tool, Kind::Holder, Kind::Material, Kind::Machine,
                                                  Kind::Template};
inline constexpr int kFormatVersion = 1; // the "schema" number this version writes and reads

struct KindInfo {
    Kind kind;
    std::string_view name;   // ILibrary::remove's kind: "tool", ...
    std::string_view folder; // "tools"
    std::string_view suffix; // ".tool.json"
    std::string_view schema; // builtinSchema name: "tool", ..., "optemplate"
};
const KindInfo& kindInfo(Kind kind);
std::optional<Kind> kindFromName(std::string_view name);

// The JSON of an item: every known key, always (so the bytes depend on the item alone).
json::Value toValue(const Tool&);
json::Value toValue(const Holder&);
json::Value toValue(const Material&);
json::Value toValue(const MachineDef&);
json::Value toValue(const OpTemplate&);

// Checks the "schema" number (FormatTooNew above kFormatVersion) and the kind's schema; FormatError otherwise.
Status checkValue(Kind kind, const json::Value& value);
// Parses text and runs checkValue.
Result<json::Value> parseChecked(Kind kind, std::string_view text);

// Items from values that passed checkValue. FormatError for what the schema cannot say (a nil id, a flute length
// longer than the tool, a tap without a pitch, an axis whose minimum is above its maximum, ...).
Result<Tool> decodeTool(const json::Value&);
Result<Holder> decodeHolder(const json::Value&);
Result<Material> decodeMaterial(const json::Value&);
Result<MachineDef> decodeMachine(const json::Value&);
Result<OpTemplate> decodeTemplate(const json::Value&);

// `known` (an object) with the members of `raw` whose keys `known` lacks added: unknown keys survive a save.
json::Value mergeUnknownKeys(const json::Value& raw, json::Value known);

// Tool type names as the files spell them ("flatEndMill"); parsing also accepts the enum spelling ("FlatEndMill").
std::string_view toolTypeName(ToolType type);
std::optional<ToolType> toolTypeFromName(std::string_view name);

} // namespace ray::lib::detail
