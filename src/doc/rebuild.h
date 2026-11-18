// Private to src/doc and its tests (task T-010): what the rebuilder's RebuildContext offers built-in feature types
// beyond the public interface.
#pragma once

#include <ray/document.h>

namespace ray::doc::detail {

// The rebuilder's context implements this as well as RebuildContext. Built-in feature types reach it through
// jobSystemOf(); a context that does not implement it (a test double, a script host) means "no job system", and
// the work then runs on the calling thread with the same result.
class RebuildServices {
public:
    virtual ~RebuildServices() = default;
    virtual IJobSystem* jobSystem() const = 0;
};

inline IJobSystem* jobSystemOf(const RebuildContext& ctx)
{
    const auto* services = dynamic_cast<const RebuildServices*>(&ctx);
    return services != nullptr ? services->jobSystem() : nullptr;
}

// A feature type may also implement this to tell the rebuilder, from the props alone, which bodies a feature can
// write without reading them. When such a feature fails, those bodies are not built further (their later features
// report NotBuilt). For a type that does not implement it, the rebuilder uses the bodies the feature read and wrote
// when it last built; for a feature that never built, it assumes every existing body may be affected.
class BodyDeclaration {
public:
    virtual ~BodyDeclaration() = default;
    virtual std::vector<BodyId> bodiesWritten(const Feature&) const = 0;
};

} // namespace ray::doc::detail
