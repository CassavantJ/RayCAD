// The commands of the UI itself (ones no registry action exists for: view.fitAll, model.tool.extrude,
// sketch.tool.line, ...), each declared ONCE, next to the code that runs it, with its default keyboard shortcut and
// the context that shortcut works in. The registry's own actions declare theirs in ActionInfo::defaultShortcut where
// they are registered (src/api), and work everywhere.
//
//   Contexts   Global: the key works everywhere. Model: while no sketch is being edited (the model tools).
//              Sketch: while a sketch is being edited (drawing tools, constraints). Two commands may share a key
//              only when their contexts never apply at the same time (Model and Sketch); the shortcut manager
//              (shortcuts.h) reports any other pair as a conflict, and a test fails on one among the defaults.
//   Fronts     a UI command that is the user's way to a registry action (model.tool.extrude for
//              feature.extrude.add, sketch.tool.trim for sketch.trim, ...) names it, and the palette and the
//              shortcut list then show only the UI command (the action stays in the registry for scripts and agents,
//              and CommandCatalog::find still finds it).
// The tables are plain data (titles and descriptions are marked for translation in the CommandCatalog context).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ray::ui {

enum class CommandContext : std::uint8_t { Global, Model, Sketch };

struct UiCommand {
    const char* id;
    const char* title;
    const char* category;
    const char* description;
    const char* shortcut;   // QKeySequence::PortableText, "" for none
    CommandContext context; // where the shortcut works
    const char* fronts;     // registry action ids this command stands for in the palette, space separated; "" none
};

// Whether two contexts can be active at the same time (so the same key in both is a conflict).
constexpr bool contextsOverlap(CommandContext a, CommandContext b)
{
    return a == CommandContext::Global || b == CommandContext::Global || a == b;
}

// Each table is defined in the file that runs its commands.
std::span<const UiCommand> generalUiCommands();    // palette_commands.cpp: ui.*, view.*, selection.*
std::span<const UiCommand> measureUiCommands();    // measure_model.cpp: measure.tool, measure.pin, ...
std::span<const UiCommand> sectionUiCommands();    // section_model.cpp: view.section.*, view.draft.*
std::span<const UiCommand> sketchUiCommands();     // sketch/sketch_mode.cpp: sketch.mode.*, sketch.tool.*
std::span<const UiCommand> constraintUiCommands(); // sketch/constraints_model.cpp: sketch.constrain.*
std::span<const UiCommand> modelUiCommands();      // model/feature_tools.cpp: model.*
// All of the above, in that order.
const std::vector<UiCommand>& allUiCommands();

} // namespace ray::ui
