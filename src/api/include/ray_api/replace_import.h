// Replacing an imported part with a new revision of its file (task T-084), for the replace dialog in src/ui
// (model/replace_import.h, qml/model/ReplaceImport.qml), without Qt.
//
// The action file.replaceImport {"feature": "<import feature uuid>", "path": "<new file>", "document"?} reads and
// matches the new file on a worker (doc's prepareReplaceImport: the rules a replacement must meet are in
// src/doc/commands/replace_import.h), then executes feature.import.replace on the document as one undo step. Its result
// is the change summary, a canonical JSON object:
//   {"document", "feature", "label",                       the command's undo label
//    "oldFile", "newFile", "oldUnits"?, "newUnits"?,        "inch" | "metric", as the files declare them
//    "warnings": [...],                                    the new file's import warnings
//    "counts": {"faces"|"edges"|"vertices": {"unchanged", "changed", "new", "removed"}},
//    "toolpaths": n,                                       operations (not suppressed) of setups using a replaced body
//    "bodies": [{"body", "name",
//                "faces": [{"fate": "changed" | "new" | "removed",
//                           "index": n,                    the face in the new geometry (in the old one when removed)
//                           "names": ["<64 hex>", ...],    its names (new map; old map when removed): locate() finds it
//                           "surface": "plane" | "cylinder" | "cone" | "sphere" | "torus" | "freeform",
//                           "hole": bool,                  a cylinder with the material outside it
//                           "radius": mm, "area": mm^2,
//                           "box": [xmin, ymin, zmin, xmax, ymax, zmax]}]}]}   mm; unchanged faces are only counted
// A face, edge or vertex is unchanged when it is the same geometry as before (references to it stay Exact), changed
// when it lies where an old one did but its extent differs (references to it need review), new, or removed.
// Not one of the contract headers in include/ray/.
#pragma once

#include <ray/api.h>

#include <optional>
#include <string>
#include <vector>

namespace ray::api::replace_import {

inline constexpr const char* kAction = "file.replaceImport";

// The element of `body` in `build` that holds any of `names` (an element of the replaced import, as the summary lists
// it), when the build is of the replaced state and a later feature kept it; nullopt otherwise.
std::optional<kernel::SubId> locate(const doc::BuildResult& build, const BodyId& body, kernel::SubKind kind,
                                    const std::vector<Hash256>& names);
// The import features of `state` whose bodies are stored with the document (those that can be replaced), in history
// order.
std::vector<const doc::Feature*> replaceableImports(const doc::DocState& state);

} // namespace ray::api::replace_import
