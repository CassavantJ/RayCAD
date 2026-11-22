// The JSON form of the CAM state (task T-032), private to src/doc: the stored CAM object and embedded library items
// (state.cpp writes and reads them with the snapshot) and the arguments of the cam.* commands (cam_commands.cpp).
//
// Stored objects (canonical JSON; the manifest names them by hash, PLAN.md 3.1):
//   manifest  ..."cam":"<CAM object hash>","embedded":{"holders":{"<uuid>":"<hash>"},"machines":{...},
//              "materials":{...},"tools":{...}}...   (both only when the state has CAM state)
//   CAM       {"operations":[Operation...],"schema":1,"setups":[Setup...]}    operations sorted by id
//   Setup     {"fixtureBodies":["<uuid>"],"id","machine":"<uuid>"|null,"name","operations":["<uuid>"],
//              "partBodies":["<uuid>"],"post":"haas-ngc","programComment":"","programNumber":1001,"stock":Stock,
//              "wcs":Wcs}
//   Stock     {"type":"box","xNeg","xPos","yNeg","yPos","zNeg","zPos"} | {"type":"cylinder","axis":"x"|"y"|"z",
//              "diameter","endOffset","length"} | {"type":"solid","body":"<uuid>"} | {"type":"fromSetup",
//              "setup":"<uuid>"}
//   Wcs       {"origin":{"mode":"stockCorner","corner":0..7} | {"mode":"stockTopCenter"} | {"mode":"modelPoint"},
//              "originVertex"?:TopoRef,"workOffset":1,"xEdge"?:TopoRef,"zFace"?:TopoRef}
//   Operation {"cutting":Cutting,"geometry":[TopoRef],"heights":Heights,"id","name","params":{...},
//              "suppressed":false,"template":"","tool":ToolRef,"type":"drill"}
//   Cutting   {"clockwise":true,"coolant":"off"|"flood"|"mist"|"throughTool"|"air","feedCut","feedLeadIn",
//              "feedLeadOut","feedPlunge","feedRamp","rpm"}                    feeds in mm/min
//   Heights   {"bottom","clearance","feedPlane","retract","top"}              mm
//   ToolRef   {"diameterOffset","holder":"<uuid>"|null,"lengthOffset","number","stickOut","tool":"<uuid>"}
//   TopoRef   naming::toJson (naming.h)
// Embedded library items (tools, holders, machines, materials), one object each: exactly the library's file format
// (schemas/*.schema.json, libraries.h toJson), read and written by the library's codec through cam.h embeddedJson and
// embedded*FromJson, so a library file and the same item embedded in a document are the same JSON. The item's schema
// is checked in a stored object and in command arguments alike; the keys it does not know are kept in the item's
// extraJson in both (they come from the library file the item was taken from).
// Optional members (with their defaults) are the same in a stored object and in command arguments; a stored object
// always has all of them. Unknown keys of a stored setup, operation or CAM object, at any depth of nested objects
// (not inside arrays), are kept in the struct's extraJson and written back unchanged; in command arguments they are
// refused.
#pragma once

#include "../json.h"

#include <ray/cam.h>
#include <ray/document.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray::doc::camjson {

// Where a value is read from: a stored object (FormatError "Invalid document: <where>: ...", unknown keys kept) or
// command arguments (InvalidArgument "<command>: ...", unknown keys refused).
struct Source {
    enum class Kind { Stored, Args } kind;
    std::string where; // "CAM object", "tool 1f3c...", or the command name
    tl::unexpected<Error> error(std::string_view field, const std::string& what) const;
    tl::unexpected<Error> error(const std::string& what) const;
};

// ---------- Values (each reader takes the value of one member; `field` names it in messages) ----------
Result<cam::StockDef> readStock(const json::Value&, const Source&, std::string_view field);
Result<cam::Wcs> readWcs(const json::Value&, const Source&, std::string_view field);
// `base`: the value an edit changes (only the given members change); null: a new value, with defaults.
Result<cam::ToolRef> readToolRef(const json::Value&, const Source&, std::string_view field, const cam::ToolRef* base,
                                 const cam::CamState* forDefaults);
Result<cam::CuttingData> readCutting(const json::Value&, const Source&, std::string_view field,
                                     const cam::CuttingData* base);
Result<cam::Heights> readHeights(const json::Value&, const Source&, std::string_view field, const cam::Heights* base);
Result<std::vector<naming::TopoRef>> readGeometry(const json::Value&, const Source&, std::string_view field);
Result<std::vector<BodyId>> readBodies(const json::Value&, const Source&, std::string_view field);

json::Value toJson(const cam::StockDef&);
json::Value toJson(const cam::Wcs&);
json::Value toJson(const cam::ToolRef&);
json::Value toJson(const cam::CuttingData&);
json::Value toJson(const cam::Heights&);
json::Value toJson(const std::vector<naming::TopoRef>&);
json::Value refToJson(const naming::TopoRef&);

// ---------- Embedded library items (the library's codec; written with cam::embeddedJson) ----------
Result<cam::Embedded<lib::Tool>> readTool(const json::Value&, const Source&, std::string_view field);
Result<cam::Embedded<lib::Holder>> readHolder(const json::Value&, const Source&, std::string_view field);
Result<cam::Embedded<lib::MachineDef>> readMachine(const json::Value&, const Source&, std::string_view field);
Result<cam::Embedded<lib::Material>> readMaterial(const json::Value&, const Source&, std::string_view field);

// ---------- Whole objects ----------
json::Value toJson(const cam::Setup&);
json::Value toJson(const cam::Operation&);
Result<cam::Setup> readSetup(const json::Value&, const Source&);
Result<cam::Operation> readOperation(const json::Value&, const Source&);

// The CAM object and the embedded items of `cam` as stored objects, appended to `objects`, and the manifest members
// that name them ("cam" and "embedded"). Nothing for a null or empty state.
std::vector<json::Member> writeCam(const std::shared_ptr<const cam::CamState>& cam,
                                   std::vector<std::pair<Hash256, std::string>>& objects);
// Reads them back: null when the manifest has neither "cam" nor "embedded". FormatError for a damaged or
// inconsistent object (an operation no setup lists, or one listed twice), FormatTooNew for a newer "schema".
Result<std::shared_ptr<const cam::CamState>> readCam(const json::Value& manifest,
                                                     const std::function<Result<std::string>(const Hash256&)>& object);

// Unknown keys: the members of `original` (at any depth of nested objects) that `known` lacks, as canonical JSON, or
// empty when there are none; and `known` with them added back.
std::string extrasOf(const json::Value& original, const json::Value& known);
json::Value withExtras(json::Value known, const std::string& extraJson);

} // namespace ray::doc::camjson
