// The feature type registry and the built-in feature types (<ray/document.h>).
#include "features/import_feature.h"

#include <ray/document.h>

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace ray::doc {

void FeatureRegistry::add(std::unique_ptr<IFeatureType> type)
{
    try {
        if (!type) {
            log(LogLevel::Warn, "doc", "A null feature type was not registered");
            return;
        }
        std::string name(type->typeName());
        if (name.empty()) {
            log(LogLevel::Warn, "doc", "A feature type without a name was not registered");
            return;
        }
        if (types_.count(name) != 0) {
            log(LogLevel::Warn, "doc", "The feature type '" + name + "' is registered already; the first one is kept");
            return;
        }
        types_.emplace(std::move(name), std::move(type));
    } catch (...) {
        log(LogLevel::Error, "doc", "Out of memory while registering a feature type");
    }
}

const IFeatureType* FeatureRegistry::find(std::string_view typeName) const
{
    const auto it = types_.find(typeName);
    return it == types_.end() ? nullptr : it->second.get();
}

std::vector<std::string> FeatureRegistry::typeNames() const
{
    std::vector<std::string> out;
    try {
        for (const auto& entry : types_) {
            out.push_back(entry.first);
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

Status registerBuiltinFeatureTypes(FeatureRegistry& registry)
{
    try {
        auto import = features::makeImportFeatureType();
        if (registry.find(import->typeName()) != nullptr) {
            return tl::unexpected(
                Error{ErrorCode::InvalidArgument,
                      "The feature type '" + std::string(import->typeName()) + "' is already registered",
                      {}});
        }
        registry.add(std::move(import));
        return {};
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while registering the feature types", {}});
    }
}

std::shared_ptr<const FeatureRegistry> builtinFeatureRegistry()
{
    try {
        static const std::shared_ptr<const FeatureRegistry> registry = [] {
            auto made = std::make_shared<FeatureRegistry>();
            if (!registerBuiltinFeatureTypes(*made)) {
                made.reset();
            }
            return std::shared_ptr<const FeatureRegistry>(std::move(made));
        }();
        return registry;
    } catch (...) {
        return {};
    }
}

} // namespace ray::doc
