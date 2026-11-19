// Private to src/doc and its tests (task T-010, extended by T-096): what the rebuilder's RebuildContext offers built-in
// feature types beyond the public interface.
#pragma once

#include <ray/document.h>

#include <memory>

namespace ray::doc::detail {

// Data a feature hands to later features of the same rebuild besides bodies (task T-096): a sketch publishes its
// plane, solved geometry and regions; an extrude reads them. Immutable once published.
class FeatureArtifact {
public:
    virtual ~FeatureArtifact() = default;
};
using ArtifactPtr = std::shared_ptr<const FeatureArtifact>;

// The rebuilder's context implements this as well as RebuildContext. Built-in feature types reach it through
// jobSystemOf(), artifactOf() and publishArtifact(); a context that does not implement it (a test double, a script
// host) means "no job system" (the work then runs on the calling thread with the same result) and "no artifacts".
class RebuildServices {
public:
    virtual ~RebuildServices() = default;
    virtual IJobSystem* jobSystem() const = 0;
    // The artifact `producer` published earlier in this rebuild, or null (it is later in the history, suppressed,
    // did not build or published none). The read is recorded like a body read: the calling feature is rebuilt when
    // the producer's result changes, and reports NotBuilt when the producer failed.
    virtual ArtifactPtr artifactOf(const FeatureId& producer) const = 0;
    // Publishes the artifact of the feature being built (the last call wins). It is cached with the feature's output.
    virtual void publishArtifact(ArtifactPtr artifact) = 0;
};

inline IJobSystem* jobSystemOf(const RebuildContext& ctx)
{
    const auto* services = dynamic_cast<const RebuildServices*>(&ctx);
    return services != nullptr ? services->jobSystem() : nullptr;
}

inline ArtifactPtr artifactOf(const RebuildContext& ctx, const FeatureId& producer)
{
    const auto* services = dynamic_cast<const RebuildServices*>(&ctx);
    return services != nullptr ? services->artifactOf(producer) : nullptr;
}

inline void publishArtifact(RebuildContext& ctx, ArtifactPtr artifact)
{
    if (auto* services = dynamic_cast<RebuildServices*>(&ctx)) {
        services->publishArtifact(std::move(artifact));
    }
}

// A feature type may also implement this to tell the rebuilder, from the props alone, which bodies a feature can
// write without reading them. When such a feature fails, those bodies are not built further (their later features
// report NotBuilt). For a type that does not implement it, the rebuilder uses the bodies the feature read and wrote
// when it last built; for a feature that never built, it assumes every existing body may be affected.
class BodyDeclaration {
public:
    virtual ~BodyDeclaration() = default;
    virtual std::vector<BodyId> bodiesWritten(const Feature&) const = 0;
    // True if the feature never writes a body that bodiesWritten() does not name: then a failure leaves the bodies it
    // only read (e.g. the face a sketch is placed on) buildable for later features. A feature that publishes an
    // artifact counts its artifact as written.
    virtual bool writesOnlyDeclared() const { return false; }
};

// A feature type implements this to say that its output depends on document parameters only through parameter
// names written in string values of its own props (expressions), and on other features only through bodies and
// artifacts. The rebuilder then keys its cache on the values of those parameters alone, so editing a parameter
// rebuilds only the features that use it (and what depends on them). Types that do not implement it are keyed on
// every parameter value.
class ParametersFromProps {
public:
    virtual ~ParametersFromProps() = default;
};

// The id under which the rebuilder tracks the artifact of `producer` among the bodies (never a real body: a hash of
// the feature id). Exposed for tests.
BodyId artifactSlot(const FeatureId& producer);

} // namespace ray::doc::detail
