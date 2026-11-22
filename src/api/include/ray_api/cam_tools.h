// What the tool library manager of the desktop app (src/ui/cam/tools*, task T-046) needs from lib beyond
// IApp::libraries(), without Qt: an item's file format and back (the editor checks its values against the item's
// schema through these, and the cam.* commands take items in this form to embed them), the tool type names, and saving
// or removing an item with a check that the library still holds what the editor started from.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls lib's free functions itself. Documents change
// only through the actions (cam.operation.edit, cam.library.embed). Not one of the contract headers in include/ray/.
#pragma once

#include <ray/api.h>

#include <optional>
#include <string>
#include <string_view>

namespace ray::api::cam_tools {

// The item as its library file holds it (lib::toJson: canonical JSON), and back with its schema checked
// (lib::toolFromJson: FormatError "/<pointer>: <the schema's words>" for the first problem).
std::string toolJson(const lib::Tool& tool);
std::string holderJson(const lib::Holder& holder);
Result<lib::Tool> toolFromJson(std::string_view json);
Result<lib::Holder> holderFromJson(std::string_view json);

// The file format's name of a tool type ("flatEndMill", ...), and back.
std::string_view toolTypeName(lib::ToolType type);
std::optional<lib::ToolType> toolTypeFromName(std::string_view name);

// BLAKE3 of the item's file format (as Tool::contentHash for a tool): what the editor started from.
Hash256 contentHash(const lib::Tool& tool);
Hash256 contentHash(const lib::Holder& holder);

// Saves the item into `library` (ILibrary::save), first checking that the library still holds what the editor
// started from: `base` is the content hash of the item as it was opened, or nullopt for a new item (the library must
// not have its id). Otherwise ErrorCode::Locked, "changed" (or "created" / "deleted") "by someone else since you
// opened it", without writing. ILibrary::save's own Locked (the file changed on disk since the library read it, or
// another program has it open) and its other errors come back as they are. Reads and writes the folder, which may be
// a network share: call it on a worker.
Status saveTool(lib::ILibrary& library, const lib::Tool& tool, const std::optional<Hash256>& base);
Status saveHolder(lib::ILibrary& library, const lib::Holder& holder, const std::optional<Hash256>& base);
// Removes the item (ILibrary::remove) after the same check (`base` as the item was shown). On a worker.
Status removeTool(lib::ILibrary& library, const ToolId& id, const Hash256& base);
Status removeHolder(lib::ILibrary& library, const HolderId& id, const Hash256& base);

// T-057: operation templates (the template manager and cam.template.saveFrom), the same way: the file format and back,
// what the editor started from, and the checked save and remove.
std::string templateJson(const lib::OpTemplate& t);
Result<lib::OpTemplate> templateFromJson(std::string_view json);
Hash256 contentHash(const lib::OpTemplate& t);
Status saveTemplate(lib::ILibrary& library, const lib::OpTemplate& t, const std::optional<Hash256>& base);
Status removeTemplate(lib::ILibrary& library, const Uuid& id, const Hash256& base);

} // namespace ray::api::cam_tools
