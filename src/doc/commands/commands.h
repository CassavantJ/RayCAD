// Built-in command handlers, private to src/doc/commands (registered by registerBuiltinCommands).
#pragma once

#include <ray/document.h>

#include <memory>
#include <vector>

namespace ray::doc::commands {

// feature.add, feature.edit, feature.remove, feature.reorder, feature.suppress, param.set, body.set.
std::vector<std::unique_ptr<ICommandHandler>> makeModelCommandHandlers();
// param.rename, param.remove (task T-098, param_commands.cpp).
std::vector<std::unique_ptr<ICommandHandler>> makeParamCommandHandlers();

} // namespace ray::doc::commands
