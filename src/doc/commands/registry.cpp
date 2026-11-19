// The command handler registry (document_impl.h).
#include "commands.h"

#include "../document_impl.h"
#include "../features/datum_plane_feature.h"
#include "../features/extrude_feature.h"
#include "../features/feature_props.h"
#include "../features/fillet_feature.h"
#include "../features/import_feature.h"
#include "../features/revolve_feature.h"
#include "../features/sketch_commands.h"
#include "../features/sketch_feature.h"

#include <string_view>
#include <utility>

#include <exception>
#include <new>

namespace ray::doc {

CommandRegistry::CommandRegistry() = default;
CommandRegistry::~CommandRegistry() = default;

Status CommandRegistry::add(std::unique_ptr<ICommandHandler> handler)
{
    try {
        if (!handler) {
            return tl::unexpected(Error{ErrorCode::InvalidArgument, "A command handler must not be null", {}});
        }
        std::string name(handler->name());
        if (name.empty()) {
            return tl::unexpected(Error{ErrorCode::InvalidArgument, "A command handler needs a name", {}});
        }
        if (handlers_.count(name) != 0) {
            return tl::unexpected(
                Error{ErrorCode::InvalidArgument, "The command '" + name + "' is already registered", {}});
        }
        handlers_.emplace(std::move(name), std::move(handler));
        return {};
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while registering a command", {}});
    }
}

const ICommandHandler* CommandRegistry::find(std::string_view name) const
{
    const auto it = handlers_.find(name);
    return it == handlers_.end() ? nullptr : it->second.get();
}

std::vector<std::string> CommandRegistry::names() const
{
    std::vector<std::string> out;
    try {
        for (const auto& entry : handlers_) {
            out.push_back(entry.first);
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

Status registerBuiltinCommands(CommandRegistry& registry)
{
    try {
        for (auto& handler : commands::makeModelCommandHandlers()) {
            if (auto added = registry.add(std::move(handler)); !added) {
                return added;
            }
        }
        if (auto added = registry.add(features::makeImportCommandHandler()); !added) { // document.import (T-010)
            return added;
        }
        // The parametric features (task T-096): feature.<type>.add / .edit, and the sketch editing commands.
        const std::pair<std::string_view, std::string_view> typed[] = {
            {features::kDatumPlaneType, features::datumPlanePropsSchema()},
            {features::kSketchType, features::sketchPropsSchema()},
            {features::kExtrudeType, features::extrudePropsSchema()},
            {features::kRevolveType, features::revolvePropsSchema()},
            {features::kFilletType, features::filletPropsSchema()},
            {features::kChamferType, features::chamferPropsSchema()},
        };
        for (const auto& [type, schema] : typed) {
            for (auto& handler : features::makeTypedFeatureCommands(type, schema)) {
                if (auto added = registry.add(std::move(handler)); !added) {
                    return added;
                }
            }
        }
        for (auto& handler : features::makeSketchCommands()) {
            if (auto added = registry.add(std::move(handler)); !added) {
                return added;
            }
        }
        return {};
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while registering the built-in commands", {}});
    }
}

std::shared_ptr<const CommandRegistry> builtinCommandRegistry()
{
    try {
        static const std::shared_ptr<const CommandRegistry> registry = [] {
            auto made = std::make_shared<CommandRegistry>();
            if (!registerBuiltinCommands(*made)) {
                made.reset();
            }
            return std::shared_ptr<const CommandRegistry>(std::move(made));
        }();
        return registry;
    } catch (...) {
        return {};
    }
}

} // namespace ray::doc
