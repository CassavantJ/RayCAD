// Replacing an imported part's file with a new revision (task T-084), private to src/doc, its tests and src/api (the
// action that reads the file). Like an import (<ray/document.h>, "How an import enters a document") it is two steps:
//   1. prepareReplaceImport(state, objects, request, ctx) on a worker: reads the stored geometry and names of the
//   import
//      feature's bodies, imports the new file with the feature's own options, pairs the new bodies with the old ones,
//      carries the names over (naming::rematchImported with the old shape) and stores each new body's BREP and element
//      map as objects. It also says what became of every face, edge and vertex (unchanged, changed, new, removed).
//   2. put PreparedReplace::objects into the document's object pool, then IDocument::execute(PreparedReplace::command):
//      "feature.import.replace", one undo step. The feature keeps its id, name and bodies (ids, names, colours, role);
//      its file, hash, units, warnings and each body's geometry ("blob") and names ("names") change. Undo restores the
//      old props, hence the old geometry and names, hence every reference's old resolution. References themselves are
//      never edited: after the replace they resolve against the carried names (Exact for an unchanged element, Merged
//      or Split for a changed one, Broken for a removed one), and features and CAM operations react as to any model
//      edit (the reference review lists what needs review; CAM operations on the body go Stale or ReferenceBroken).
//
// THE RULES a replacement must meet (else nothing changes and the error says why, in words for a machinist):
//  - the feature is an import whose bodies are stored with the document;
//  - the new file can be read, holds geometry, and is not byte for byte the file the part came from;
//  - it gives as many bodies as the import has, each a solid with as many solids as the body it replaces;
//  - with several bodies, each new body is paired with the old body whose bounding box it overlaps most (intersection
//    over union), and that pairing must be clear: each old body's best new body has it as its own best, and every
//    other candidate overlaps it at most half as much. Otherwise the pairing is not guessed.
// Units: the geometry of both files is read in millimetres (the importer converts), so a file that declares other
// units is compared as it is; the new file's units are stored ("sourceUnits") and the summary says when they differ.
//
// "feature.import.replace" args: {"id": "<import feature uuid>", "props": {import props}} (the props in full, checked
// strictly like document.import's). The command checks that the feature is an import with stored bodies, and that the
// new props keep the same bodies (ids, in order) and role; it reads no file.
#pragma once

#include <ray/document.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray::doc::commands {

inline constexpr std::string_view kReplaceImportCommand = "feature.import.replace";

std::unique_ptr<ICommandHandler> makeReplaceImportCommandHandler();

struct ReplaceRequest {
    FeatureId feature;                  // the import feature
    std::string path;                   // the new revision, UTF-8
    std::optional<ImportFormat> format; // nullopt: from the extension
};

// What became of an element.
enum class ElementFate : std::uint8_t {
    Unchanged, // the same geometry: it holds the old names (references resolve Exact)
    Changed,   // on the same carrier as old elements and sharing ground with them: it holds the old names besides a
               // new one, or is a piece named from them (references resolve Merged, Split or Broken: review)
    New,       // nothing like it before
    Removed    // an old element whose names went nowhere (references resolve Broken)
};
struct ReplacedElement {
    kernel::SubId sub;          // in the new shape; for Removed, in the old one
    std::vector<Hash256> names; // its names in the new map; for Removed, in the old one
};
struct ReplacedBody {
    BodyId body;
    std::string name;
    kernel::Shape before, after;
    naming::ElementMap beforeMap, afterMap;
    // [kind: vertex, edge, face][fate]
    std::array<std::array<std::vector<ReplacedElement>, 4>, 3> elements;
    const std::vector<ReplacedElement>& of(kernel::SubKind kind, ElementFate fate) const
    {
        return elements[static_cast<std::size_t>(kind)][static_cast<std::size_t>(fate)];
    }
};
struct PreparedReplace {
    Command command;                                                 // feature.import.replace
    std::vector<std::pair<Hash256, std::vector<std::byte>>> objects; // each new body's BREP and element map, by hash
    std::vector<ReplacedBody> bodies;                                // in the feature's body order
    std::string oldFile, newFile;                                    // file names (no folders)
    std::optional<UnitSystem> oldUnits, newUnits;                    // as the files declare them
    std::vector<std::string> warnings;                               // the new file's import warnings
};

// Worker only (seconds for a large file). `objects` reads the document's stored objects (its pool, then its file).
// Errors: NotFound for the feature or the file, InvalidArgument for a replacement the rules above refuse (the message
// says why), IoError, FormatError and KernelFailure from reading and importing, Cancelled.
Result<PreparedReplace> prepareReplaceImport(const DocState& state, const IObjectSource& objects,
                                             const ReplaceRequest& request, JobContext& ctx);

// The fates of the elements of `after` (named by `rematched`, the map rematchImported gave) and of `before` (named by
// `beforeMap`), filled into body.elements. Exposed for tests.
void classifyReplacement(const naming::ElementMap& beforeMap, const kernel::Shape& before,
                         const naming::RematchResult& rematched, const kernel::Shape& after, ReplacedBody& body);

} // namespace ray::doc::commands
