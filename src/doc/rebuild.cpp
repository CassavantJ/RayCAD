// The rebuilder (<ray/document.h>, "The rebuilder"; PLAN.md 3.2, 7, 8).
//
// Walk: features run in history order on the calling job. The table of bodies holds, per body, its shape, element
// map and a revision hash. Before a feature runs, its static key is hashed from (type, id, props, parameter
// values). The cache maps a static key to entries, each holding the bodies the feature read last time (with their
// revisions) and its output. An entry is valid when every body it read has the same revision now (absent bodies
// included), so a hit needs no geometry comparison. A miss runs the feature through a context that records every
// body it reads through shapeOf, mapOf and the resolver. The revision of a body a feature writes is the hash of the
// entry key (static key plus the recorded reads) and the body id, so revisions, like outputs, are the same on every
// run and for every worker count.
//
// Failure: the bodies a failed feature affects are poisoned: those it read, those its type declares it writes
// (detail::BodyDeclaration), and those it read or wrote in its last successful build. A failed feature that never
// built and declares nothing may have affected anything, so every existing body is poisoned. A later feature that
// reads a poisoned body, or (once any feature failed) a body that does not exist, cannot build: it reports NotBuilt
// and poisons what it affects in the same way (except that unknown means nothing more). A feature that writes a body
// gives that body a fresh start. The table keeps the last good state of every body, which is what
// BuildResult::bodies returns.
//
// Artifacts (task T-096): what a feature publishes for later features (a sketch's plane and regions) is kept in the
// same table under artifactSlot(feature), a pseudo body with a null shape, so reading it is recorded, cached,
// poisoned and revised exactly like a body read. Pseudo bodies are left out of BuildResult::bodies.
// Parameters: a type that implements detail::ParametersFromProps is keyed on the values of the parameters whose names
// occur in the string values of its props, not on every parameter.
#include "rebuild.h"

#include "document_impl.h"
#include "json.h"

#include <ray/naming.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ray::doc {
namespace {

using Clock = std::chrono::steady_clock;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

struct BodyState {
    kernel::Shape shape; // null for an artifact slot
    naming::ElementMap map;
    Hash256 revision;
    detail::ArtifactPtr artifact; // artifact slots only
};
using BodyTable = std::map<BodyId, BodyState>;
using Reads = std::map<BodyId, std::optional<Hash256>>; // revision when read; nullopt = the body did not exist

void hashUuid(Hasher& h, const Uuid& id)
{
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(id.bytes)));
}

void hashHash(Hasher& h, const Hash256& value)
{
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(value.bytes)));
}

Hash256 parametersHash(const std::map<std::string, double>& values)
{
    Hasher h;
    h.str("ray.doc.rebuild.parameters/1");
    h.i64(static_cast<std::int64_t>(values.size()));
    for (const auto& [name, value] : values) { // std::map: sorted by name
        h.str(name);
        h.f64(value);
    }
    return h.finish();
}

// Every identifier ([A-Za-z_][A-Za-z0-9_]*) in the string values of a JSON value.
void collectNames(const json::Value& value, std::set<std::string>& out)
{
    if (value.isString()) {
        const std::string& s = value.asString();
        const auto letter = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        const auto digit = [](char c) {
            return c >= '0' && c <= '9';
        };
        std::size_t i = 0;
        while (i < s.size()) {
            if (letter(s[i]) && (i == 0 || !(letter(s[i - 1]) || digit(s[i - 1]) || s[i - 1] == '.'))) {
                std::size_t j = i + 1;
                while (j < s.size() && (letter(s[j]) || digit(s[j]))) {
                    ++j;
                }
                out.insert(s.substr(i, j - i));
                i = j;
            } else {
                ++i;
            }
        }
    } else if (value.isArray()) {
        for (const json::Value& v : value.asArray()) {
            collectNames(v, out);
        }
    } else if (value.isObject()) {
        for (const json::Member& m : value.asObject()) {
            collectNames(m.value, out);
        }
    }
}

// The hash of the values of the parameters a feature's props name (detail::ParametersFromProps).
Hash256 usedParametersHash(const Feature& f, const std::map<std::string, double>& values)
{
    std::set<std::string> names;
    if (auto parsed = json::parse(f.propsJson.empty() ? std::string_view("{}") : std::string_view(f.propsJson))) {
        collectNames(*parsed, names);
    }
    Hasher h;
    h.str("ray.doc.rebuild.used-parameters/1");
    for (const std::string& name : names) { // std::set: sorted
        const auto it = values.find(name);
        if (it != values.end()) {
            h.str(name);
            h.f64(it->second);
        }
    }
    return h.finish();
}

Hash256 staticKey(const Feature& f, const Hash256& parameters)
{
    Hasher h;
    h.str("ray.doc.rebuild.feature/1");
    h.str(f.type);
    hashUuid(h, f.id.value);
    h.str(f.propsJson.empty() ? std::string_view("{}") : std::string_view(f.propsJson));
    hashHash(h, parameters);
    return h.finish();
}

Hash256 entryKey(const Hash256& key, const Reads& reads)
{
    Hasher h;
    h.str("ray.doc.rebuild.entry/1");
    hashHash(h, key);
    h.i64(static_cast<std::int64_t>(reads.size()));
    for (const auto& [body, revision] : reads) {
        hashUuid(h, body.value);
        h.i64(revision ? 1 : 0);
        if (revision) {
            hashHash(h, *revision);
        }
    }
    return h.finish();
}

Hash256 bodyRevision(const Hash256& entry, const BodyId& body)
{
    Hasher h;
    h.str("ray.doc.rebuild.body/1");
    hashHash(h, entry);
    hashUuid(h, body.value);
    return h.finish();
}

// Records the bodies one feature reads. Thread-safe: a feature may read from parallel chunks.
class Recorder {
public:
    Recorder(const BodyTable& bodies, const std::set<BodyId>& poisoned, bool afterFailure)
        : bodies_(bodies), poisoned_(poisoned), afterFailure_(afterFailure)
    {
    }

    // The body, or null if it is poisoned or does not exist.
    const BodyState* read(const BodyId& id)
    {
        const auto it = bodies_.find(id);
        const bool poisoned = poisoned_.count(id) != 0;
        std::lock_guard<std::mutex> lock(mutex_);
        reads_.emplace(id, it == bodies_.end() ? std::nullopt : std::optional<Hash256>(it->second.revision));
        if (poisoned || (it == bodies_.end() && afterFailure_)) {
            blocked_ = true;
            if (it == bodies_.end() || it->second.shape.isNull()) {
                blockedByArtifact_ = blockedByArtifact_ || isArtifactRead(id);
            }
        }
        return poisoned || it == bodies_.end() ? nullptr : &it->second;
    }

    // Marks `slot` as an artifact slot, so that a block through it is reported as such.
    void noteArtifactSlot(const BodyId& slot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        artifactSlots_.insert(slot);
    }

    bool blockedByArtifact() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return blockedByArtifact_;
    }

    bool isPoisoned(const BodyId& id) const { return poisoned_.count(id) != 0; }

    Reads reads() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return reads_;
    }
    // True if the feature read a body an earlier failure left unbuilt.
    bool blocked() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocked_;
    }

private:
    bool isArtifactRead(const BodyId& id) const { return artifactSlots_.count(id) != 0; } // mutex_ held

    const BodyTable& bodies_;
    const std::set<BodyId>& poisoned_;
    const bool afterFailure_;
    mutable std::mutex mutex_;
    Reads reads_;
    std::set<BodyId> artifactSlots_;
    bool blocked_ = false;
    bool blockedByArtifact_ = false;
};

std::string unavailable(const Recorder& recorder, const BodyId& id)
{
    return recorder.isPoisoned(id) ? "The body " + id.value.toString() + " was not built: an earlier feature failed."
                                   : "The body " + id.value.toString() + " does not exist at this point.";
}

// Resolves references against the bodies as they are just before the feature, recording which bodies it reads.
class TrackingResolver final : public naming::IResolver {
public:
    explicit TrackingResolver(Recorder& recorder) : recorder_(recorder) {}

    naming::Resolved resolve(const naming::TopoRef& ref) const override
    {
        try {
            const BodyState* body = recorder_.read(ref.body);
            if (body == nullptr) {
                return naming::Resolved{naming::ResolveState::Broken, {}, unavailable(recorder_, ref.body)};
            }
            const naming::IResolver* inner = resolverFor(ref.body, *body);
            if (inner == nullptr) {
                return naming::Resolved{naming::ResolveState::Broken, {}, "Out of memory while resolving."};
            }
            return inner->resolve(ref);
        } catch (...) {
            return naming::Resolved{naming::ResolveState::Broken, {}, "The reference could not be resolved."};
        }
    }

    naming::TopoRef makeRef(BodyId bodyId, kernel::SubId sub) const override
    {
        naming::TopoRef empty{};
        empty.body = bodyId;
        empty.kind = sub.kind;
        try {
            const BodyState* body = recorder_.read(bodyId);
            const naming::IResolver* inner = body != nullptr ? resolverFor(bodyId, *body) : nullptr;
            return inner != nullptr ? inner->makeRef(bodyId, sub) : empty;
        } catch (...) {
            return empty;
        }
    }

private:
    const naming::IResolver* resolverFor(const BodyId& id, const BodyState& body) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = resolvers_.find(id);
        if (it == resolvers_.end()) {
            std::vector<naming::ResolverBody> one;
            one.push_back(naming::ResolverBody{id, body.map, body.shape});
            it = resolvers_.emplace(id, naming::makeResolver(std::move(one))).first;
        }
        return it->second.get();
    }

    Recorder& recorder_;
    mutable std::mutex mutex_;
    mutable std::map<BodyId, std::unique_ptr<naming::IResolver>> resolvers_;
};

class Context final : public RebuildContext, public detail::RebuildServices {
public:
    Context(const DocState& state, const std::map<std::string, double>& params, const naming::IResolver& resolver,
            JobContext& job, Recorder& recorder, const IObjectSource* objects, IJobSystem* jobs)
        : RebuildContext(state, params, resolver, job), recorder_(recorder), objects_(objects), jobs_(jobs)
    {
    }

    Result<kernel::Shape> shapeOf(BodyId id) const override
    {
        const BodyState* body = recorder_.read(id);
        if (body == nullptr) {
            return failure(recorder_.isPoisoned(id) ? ErrorCode::ReferenceBroken : ErrorCode::NotFound,
                           unavailable(recorder_, id));
        }
        return body->shape;
    }

    const naming::ElementMap& mapOf(BodyId id) const override
    {
        static const naming::ElementMap empty;
        const BodyState* body = recorder_.read(id);
        return body != nullptr ? body->map : empty;
    }

    Result<std::vector<std::byte>> blob(const Hash256& hash) const override
    {
        if (objects_ == nullptr) {
            return failure(ErrorCode::NotFound, "No stored objects are available (object " + hash.hex() + ").");
        }
        return objects_->object(hash);
    }

    IJobSystem* jobSystem() const override { return jobs_; }

    detail::ArtifactPtr artifactOf(const FeatureId& producer) const override
    {
        const BodyId slot = detail::artifactSlot(producer);
        recorder_.noteArtifactSlot(slot);
        const BodyState* body = recorder_.read(slot);
        return body != nullptr ? body->artifact : nullptr;
    }

    void publishArtifact(detail::ArtifactPtr artifact) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        artifact_ = std::move(artifact);
    }

    detail::ArtifactPtr published() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return artifact_;
    }

private:
    Recorder& recorder_;
    const IObjectSource* objects_;
    IJobSystem* jobs_;
    mutable std::mutex mutex_;
    detail::ArtifactPtr artifact_; // guarded by mutex_
};

std::string joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& line : lines) {
        out += out.empty() ? "" : "; ";
        out += line;
    }
    return out;
}

Status validate(const FeatureOutput& out)
{
    std::set<BodyId> seen;
    for (const BodyUpdate& update : out.bodies) {
        if (update.body.value == Uuid{}) {
            return failure(ErrorCode::Internal, "The feature produced a body without an id.");
        }
        if (!seen.insert(update.body).second) {
            return failure(ErrorCode::Internal,
                           "The feature produced the body " + update.body.value.toString() + " twice.");
        }
    }
    return {};
}

struct CacheEntry {
    Reads reads;
    FeatureOutput output;
    detail::ArtifactPtr artifact;
    std::uint64_t lastUse = 0;
};

class Rebuilder final : public IRebuilder {
public:
    Rebuilder(IJobSystem& jobs, std::shared_ptr<const FeatureRegistry> types,
              std::shared_ptr<const IObjectSource> objects, RebuilderOptions options)
        : jobs_(jobs), types_(std::move(types)), objects_(std::move(objects)), options_(options)
    {
    }

    Result<BuildResultPtr> rebuild(DocStatePtr state, JobContext& ctx) override
    {
        try {
            return run(state, ctx);
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, "The rebuild ran out of memory.");
        } catch (...) {
            return failure(ErrorCode::Internal, "The rebuild failed unexpectedly.");
        }
    }

private:
    Result<BuildResultPtr> run(const DocStatePtr& state, JobContext& ctx)
    {
        if (!state) {
            return failure(ErrorCode::InvalidArgument, "There is no document snapshot to rebuild.");
        }
        if (auto cancelled = ctx.cancel.check(); !cancelled) {
            return tl::unexpected(cancelled.error());
        }
        // Features that use parameters fail with their own message when the parameters do not evaluate.
        auto evaluated = evaluateParameters(state->parameters);
        const std::map<std::string, double> params =
            evaluated ? std::move(*evaluated) : std::map<std::string, double>{};
        const Hash256 paramsHash = parametersHash(params);

        BodyTable bodies;
        std::set<BodyId> poisoned;
        bool afterFailure = false;
        auto result = std::make_shared<BuildResult>();
        const std::size_t count = state->features.size();
        const double weight = count == 0 ? 1.0 : 1.0 / static_cast<double>(count);
        // The rollback bar: features after it are kept but not built (the model as it was after the bar's feature).
        const std::optional<std::size_t> bar = detail::rollbackIndex(*state);

        for (std::size_t position = 0; position < count; ++position) {
            const Feature& feature = state->features[position];
            if (auto cancelled = ctx.cancel.check(); !cancelled) {
                return tl::unexpected(cancelled.error());
            }
            JobContext featureCtx{ctx.cancel, ctx.progress.sub(weight)};
            FeatureReport report{feature.id, FeatureStatus::Ok, {}, 0.0, false};
            if (bar && position > *bar) {
                report.status = FeatureStatus::NotBuilt;
                report.message = "Rolled back: it comes after the rollback bar.";
                result->reports.push_back(std::move(report));
                featureCtx.progress.set(1.0);
                continue;
            }
            if (feature.suppressed) {
                report.status = FeatureStatus::Suppressed;
                result->reports.push_back(std::move(report));
                featureCtx.progress.set(1.0);
                continue;
            }
            const IFeatureType* type = types_ ? types_->find(feature.type) : nullptr;
            if (type == nullptr) {
                report.status = FeatureStatus::Failed;
                report.message = "Unknown feature type '" + feature.type + "'.";
                afterFailure = true;
                poisonAffected(feature, nullptr, {}, bodies, poisoned, true);
                result->reports.push_back(std::move(report));
                continue;
            }
            const Hash256 key = staticKey(feature, dynamic_cast<const detail::ParametersFromProps*>(type) != nullptr
                                                       ? usedParametersHash(feature, params)
                                                       : paramsHash);
            if (auto hit = lookup(key, bodies, poisoned, afterFailure)) {
                remember(feature.id, hit->reads, hit->output, hit->artifact != nullptr);
                apply(feature.id, entryKey(key, hit->reads), hit->output, hit->artifact, bodies, poisoned,
                      result->lineage);
                report.cacheHit = true;
                report.status = hit->output.warnings.empty() ? FeatureStatus::Ok : FeatureStatus::Warning;
                report.message = joined(hit->output.warnings);
                result->reports.push_back(std::move(report));
                featureCtx.progress.set(1.0);
                continue;
            }

            Recorder recorder(bodies, poisoned, afterFailure);
            TrackingResolver resolver(recorder);
            Context context(*state, params, resolver, featureCtx, recorder, objects_.get(), &jobs_);
            const auto start = Clock::now();
            Result<FeatureOutput> out = [&]() -> Result<FeatureOutput> {
                try {
                    return type->rebuild(feature, context);
                } catch (const std::bad_alloc&) {
                    return failure(ErrorCode::Internal, "The feature ran out of memory.");
                } catch (...) {
                    return failure(ErrorCode::Internal, "The feature failed with an internal error.");
                }
            }();
            report.seconds = std::chrono::duration<double>(Clock::now() - start).count();
            if (out) {
                if (auto valid = validate(*out); !valid) {
                    out = tl::unexpected(valid.error());
                }
            }
            const Reads reads = recorder.reads();
            const detail::ArtifactPtr artifact = out ? context.published() : nullptr;
            if (ctx.cancel.cancelled()) {
                if (out && !recorder.blocked()) {
                    store(key, reads, *out, artifact); // finished work is kept for the next rebuild
                }
                return failure(ErrorCode::Cancelled, "The rebuild was cancelled.");
            }
            if (recorder.blocked()) {
                report.status = FeatureStatus::NotBuilt;
                report.message = recorder.blockedByArtifact()
                                     ? "Not built: it uses a sketch or datum that an earlier failure left unbuilt."
                                     : "Not built: it needs a body that an earlier failed feature should have built.";
                poisonAffected(feature, type, reads, bodies, poisoned, false);
                result->reports.push_back(std::move(report));
                continue;
            }
            if (!out) {
                report.status = FeatureStatus::Failed;
                report.message = out.error().message.empty() ? "The feature failed." : out.error().message;
                afterFailure = true;
                poisonAffected(feature, type, reads, bodies, poisoned, true);
                result->reports.push_back(std::move(report));
                continue;
            }
            remember(feature.id, reads, *out, artifact != nullptr);
            store(key, reads, *out, artifact);
            apply(feature.id, entryKey(key, reads), *out, artifact, bodies, poisoned, result->lineage);
            report.status = out->warnings.empty() ? FeatureStatus::Ok : FeatureStatus::Warning;
            report.message = joined(out->warnings);
            result->reports.push_back(std::move(report));
        }

        for (auto& [id, body] : bodies) {
            if (!body.shape.isNull()) { // artifact slots are not bodies
                result->bodies.emplace(id, BuiltBody{std::move(body.shape), std::move(body.map)});
            }
        }
        evict();
        ctx.progress.set(1.0);
        return BuildResultPtr(std::move(result));
    }

    // A feature that did not build: the bodies it read, the bodies its type declares it writes, and the bodies it
    // read or wrote when it last built are not built further. `unknownMeansAll`: if none of that is known (a failed
    // feature that never built and declares nothing), every existing body is affected.
    void poisonAffected(const Feature& feature, const IFeatureType* type, const Reads& reads, const BodyTable& bodies,
                        std::set<BodyId>& poisoned, bool unknownMeansAll)
    {
        bool known = false;
        // A feature that did not build publishes no artifact: features using it are not built either.
        poisoned.insert(detail::artifactSlot(feature.id));
        const auto* declaration = dynamic_cast<const detail::BodyDeclaration*>(type);
        if (declaration == nullptr || !declaration->writesOnlyDeclared()) {
            for (const auto& read : reads) {
                poisoned.insert(read.first);
            }
        }
        if (declaration != nullptr) {
            for (const BodyId& id : declaration->bodiesWritten(feature)) {
                poisoned.insert(id);
            }
            known = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = touched_.find(feature.id);
            if (it != touched_.end()) {
                poisoned.insert(it->second.begin(), it->second.end());
                known = true;
            }
        }
        if (!known && unknownMeansAll) {
            for (const auto& entry : bodies) {
                poisoned.insert(entry.first);
            }
        }
    }

    // Records the bodies a feature read and wrote in its last successful build.
    void remember(const FeatureId& id, const Reads& reads, const FeatureOutput& out, bool artifact)
    {
        try {
            std::set<BodyId> touched;
            for (const auto& read : reads) {
                touched.insert(read.first);
            }
            for (const BodyUpdate& update : out.bodies) {
                touched.insert(update.body);
            }
            if (artifact) {
                touched.insert(detail::artifactSlot(id));
            }
            std::lock_guard<std::mutex> lock(mutex_);
            touched_[id] = std::move(touched);
        } catch (...) {
        }
    }

    static void apply(const FeatureId& feature, const Hash256& entry, const FeatureOutput& out,
                      const detail::ArtifactPtr& artifact, BodyTable& bodies, std::set<BodyId>& poisoned,
                      std::vector<naming::LineageEvent>& lineage)
    {
        for (const BodyUpdate& update : out.bodies) {
            poisoned.erase(update.body);
            if (update.shape.isNull()) {
                bodies.erase(update.body);
            } else {
                bodies[update.body] = BodyState{update.shape, update.map, bodyRevision(entry, update.body), nullptr};
            }
            lineage.insert(lineage.end(), update.lineage.begin(), update.lineage.end());
        }
        const BodyId slot = detail::artifactSlot(feature);
        if (artifact) {
            poisoned.erase(slot);
            bodies[slot] = BodyState{kernel::Shape{}, naming::ElementMap{}, bodyRevision(entry, slot), artifact};
        } else {
            bodies.erase(slot);
        }
    }

    static bool matches(const Reads& reads, const BodyTable& bodies, const std::set<BodyId>& poisoned,
                        bool afterFailure)
    {
        for (const auto& [id, revision] : reads) {
            if (poisoned.count(id) != 0) {
                return false;
            }
            const auto it = bodies.find(id);
            if (it == bodies.end()) {
                if (revision || afterFailure) {
                    return false;
                }
            } else if (!revision || *revision != it->second.revision) {
                return false;
            }
        }
        return true;
    }

    std::optional<CacheEntry> lookup(const Hash256& key, const BodyTable& bodies, const std::set<BodyId>& poisoned,
                                     bool afterFailure)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }
        for (CacheEntry& entry : it->second) {
            if (matches(entry.reads, bodies, poisoned, afterFailure)) {
                entry.lastUse = ++tick_;
                return entry;
            }
        }
        return std::nullopt;
    }

    void store(const Hash256& key, const Reads& reads, const FeatureOutput& out, const detail::ArtifactPtr& artifact)
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<CacheEntry>& entries = cache_[key];
            for (CacheEntry& entry : entries) {
                if (entry.reads == reads) {
                    entry.output = out;
                    entry.artifact = artifact;
                    entry.lastUse = ++tick_;
                    return;
                }
            }
            entries.push_back(CacheEntry{reads, out, artifact, ++tick_});
            ++size_;
        } catch (...) {
            // Out of memory: the result is simply not cached.
        }
    }

    void evict()
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (size_ <= options_.cacheEntries) {
                return;
            }
            std::vector<std::tuple<std::uint64_t, Hash256, std::size_t>> uses;
            for (const auto& [key, entries] : cache_) {
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    uses.emplace_back(entries[i].lastUse, key, i);
                }
            }
            std::sort(uses.begin(), uses.end());
            const std::size_t drop = size_ - options_.cacheEntries;
            std::set<std::pair<Hash256, std::size_t>> doomed;
            for (std::size_t i = 0; i < drop; ++i) {
                doomed.emplace(std::get<1>(uses[i]), std::get<2>(uses[i]));
            }
            for (auto it = cache_.begin(); it != cache_.end();) {
                std::vector<CacheEntry> kept;
                for (std::size_t i = 0; i < it->second.size(); ++i) {
                    if (doomed.count({it->first, i}) == 0) {
                        kept.push_back(std::move(it->second[i]));
                    }
                }
                it->second = std::move(kept);
                it = it->second.empty() ? cache_.erase(it) : std::next(it);
            }
            size_ -= drop;
        } catch (...) {
        }
    }

    IJobSystem& jobs_;
    std::shared_ptr<const FeatureRegistry> types_;
    std::shared_ptr<const IObjectSource> objects_;
    RebuilderOptions options_;
    std::mutex mutex_;
    std::map<Hash256, std::vector<CacheEntry>> cache_; // guarded by mutex_
    std::size_t size_ = 0;                             // guarded by mutex_: entries in cache_
    std::uint64_t tick_ = 0;                           // guarded by mutex_
    std::map<FeatureId, std::set<BodyId>> touched_;    // guarded by mutex_: see remember()
};

} // namespace

BodyId detail::artifactSlot(const FeatureId& producer)
{
    Hasher h;
    h.str("ray.doc.rebuild.artifact/1");
    hashUuid(h, producer.value);
    const Hash256 digest = h.finish();
    BodyId out;
    std::copy_n(digest.bytes.begin(), out.value.bytes.size(), out.value.bytes.begin());
    return out;
}

std::shared_ptr<IRebuilder> makeRebuilder(IJobSystem& jobs, std::shared_ptr<const FeatureRegistry> types,
                                          std::shared_ptr<const IObjectSource> objects, RebuilderOptions options)
{
    try {
        return std::make_shared<Rebuilder>(jobs, std::move(types), std::move(objects), options);
    } catch (...) {
        return nullptr;
    }
}

} // namespace ray::doc
