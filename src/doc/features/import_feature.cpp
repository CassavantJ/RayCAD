// The "import" feature type: building it from stored objects or from its source file, preparing an import, and the
// helpers around it (<ray/document.h>, "The import feature").
//
// Building, per body (in parallel through IJobSystem::parallelChunks when the rebuilder offers a job system; the
// result is assembled in body order, so it never depends on the worker count):
//   restore:  RebuildContext::blob(blob) -> verify BLAKE3 -> kernel::deserialize -> naming::nameImported;
//   re-import (no stored bodies, or a stored object is missing): hashFile == fileHash -> kernel::importStep or
//             importIges -> naming::nameImported.
#include "import_feature.h"

#include "import_props.h"

#include "../rebuild.h"

#include <ray/naming.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ray::doc::features {
namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

Hash256 blake3(std::span<const std::byte> bytes)
{
    Hasher hasher;
    hasher.bytes(bytes);
    return hasher.finish();
}

// Runs fn(i) for i in [0, n): in parallel chunks of one when `jobs` is given, else in order on this thread.
Status forEach(std::size_t n, IJobSystem* jobs, JobContext& ctx, const std::function<Status(std::size_t)>& fn)
{
    if (jobs != nullptr) {
        return jobs->parallelChunks(n, 1, ctx, [&](std::size_t, std::size_t begin, std::size_t end) -> Status {
            for (std::size_t i = begin; i < end; ++i) {
                if (auto done = fn(i); !done) {
                    return done;
                }
            }
            return {};
        });
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (auto cancelled = ctx.cancel.check(); !cancelled) {
            return cancelled;
        }
        if (auto done = fn(i); !done) {
            return done;
        }
        ctx.progress.set(static_cast<double>(i + 1) / static_cast<double>(n));
    }
    return {};
}

// nameImported returns an empty map when naming failed; a body with elements must get names.
Result<naming::ElementMap> nameBody(const FeatureId& feature, const kernel::Shape& shape, const std::string& body)
{
    naming::ElementMap map = naming::nameImported(feature, shape);
    const std::uint32_t elements =
        shape.count(kernel::SubKind::Face) + shape.count(kernel::SubKind::Edge) + shape.count(kernel::SubKind::Vertex);
    if (map.size() == 0 && elements > 0) {
        return failure(ErrorCode::KernelFailure, "The faces and edges of body '" + body + "' could not be named.");
    }
    return map;
}

// A shape in the form a restore gives: deserialize(serialize(shape)), and its BREP bytes. Then the stored object,
// the restored shape and a re-imported shape all have the same bytes and contentHash(). (This round trip was added
// when kernel::serialize was not yet canonical on the first round trip for bodies placed by a STEP assembly; the
// kernel now writes the read-back form itself, so it is kept only so the stored and the used shape are one object
// of one form.)
struct Canonical {
    kernel::Shape shape;
    std::vector<std::byte> bytes;
};
Result<Canonical> canonical(const kernel::Shape& shape, const std::string& body)
{
    std::vector<std::byte> first = kernel::serialize(shape);
    if (first.empty()) {
        return failure(ErrorCode::KernelFailure, "Body '" + body + "' cannot be stored.");
    }
    auto again = kernel::deserialize(first);
    if (!again) {
        return failure(again.error().code, "Body '" + body + "' cannot be stored: " + again.error().message);
    }
    std::vector<std::byte> bytes = kernel::serialize(*again);
    if (bytes.empty()) {
        return failure(ErrorCode::KernelFailure, "Body '" + body + "' cannot be stored.");
    }
    return Canonical{std::move(*again), std::move(bytes)};
}

Result<kernel::ImportResult> importFile(const std::string& path, ImportFormat format,
                                        const kernel::ImportOptions& options, JobContext& ctx)
{
    return format == ImportFormat::Iges ? kernel::importIges(path, options, ctx)
                                        : kernel::importStep(path, options, ctx);
}

// The bodies from their stored BREP objects. NotFound if an object is missing (the caller then tries the file).
Result<FeatureOutput> restore(const Feature& feature, const ImportProps& props, RebuildContext& ctx, IJobSystem* jobs)
{
    const std::vector<ImportBody>& bodies = *props.bodies;
    std::vector<std::optional<BodyUpdate>> slots(bodies.size());
    auto done = forEach(bodies.size(), jobs, ctx.job, [&](std::size_t i) -> Status {
        const ImportBody& body = bodies[i];
        auto bytes = ctx.blob(body.blob);
        if (!bytes) {
            if (bytes.error().code == ErrorCode::NotFound) {
                return failure(ErrorCode::NotFound, "The stored geometry of body '" + body.name + "' (object " +
                                                        body.blob.hex() + ") is missing.");
            }
            return tl::unexpected(bytes.error());
        }
        if (blake3(*bytes) != body.blob) {
            return failure(ErrorCode::FormatError,
                           "The stored geometry of body '" + body.name + "' is damaged: its hash does not match.");
        }
        auto shape = kernel::deserialize(*bytes);
        if (!shape) {
            return failure(shape.error().code,
                           "The stored geometry of body '" + body.name + "' cannot be read: " + shape.error().message);
        }
        auto map = nameBody(feature.id, *shape, body.name);
        if (!map) {
            return tl::unexpected(map.error());
        }
        slots[i] = BodyUpdate{body.id, std::move(*shape), std::move(*map), {}};
        return {};
    });
    if (!done) {
        return tl::unexpected(done.error());
    }
    FeatureOutput out;
    for (auto& slot : slots) {
        out.bodies.push_back(std::move(*slot));
    }
    out.warnings = props.warnings;
    importCounters().restores.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// The bodies from the source file. `missing` is why the stored objects could not be used, if they were tried.
Result<FeatureOutput> reimport(const Feature& feature, const ImportProps& props, RebuildContext& ctx, IJobSystem* jobs,
                               const std::optional<Error>& missing)
{
    const std::string& path = props.sourceFile;
    auto explain = [&](const Error& e) {
        std::string message =
            missing ? missing->message + " The source file '" + path + "' cannot be used either: " + e.message
                    : "The source file '" + path + "' cannot be imported: " + e.message;
        return failure(e.code == ErrorCode::Cancelled ? ErrorCode::Cancelled : e.code, std::move(message));
    };
    JobContext hashCtx{ctx.job.cancel, ctx.job.progress.sub(0.05)};
    auto hash = hashFile(path, hashCtx);
    if (!hash) {
        return explain(hash.error());
    }
    if (*hash != props.fileHash) {
        return explain(Error{ErrorCode::IoError, "its contents have changed since it was imported.", {}});
    }
    JobContext importCtx{ctx.job.cancel, ctx.job.progress.sub(0.85)};
    auto imported = importFile(path, props.format, props.options, importCtx);
    if (!imported) {
        return explain(imported.error());
    }
    importCounters().fileImports.fetch_add(1, std::memory_order_relaxed);
    std::vector<kernel::ImportedBody>& fileBodies = imported->bodies;
    if (props.bodies && props.bodies->size() != fileBodies.size()) {
        return explain(Error{ErrorCode::FormatError,
                             "it now gives " + std::to_string(fileBodies.size()) + " bodies, the document stored " +
                                 std::to_string(props.bodies->size()) + ".",
                             {}});
    }
    std::vector<std::optional<BodyUpdate>> slots(fileBodies.size());
    JobContext nameCtx{ctx.job.cancel, ctx.job.progress.sub(0.10)};
    auto done = forEach(fileBodies.size(), jobs, nameCtx, [&](std::size_t i) -> Status {
        const kernel::ImportedBody& body = fileBodies[i];
        const std::string& name = props.bodies ? (*props.bodies)[i].name : body.name;
        auto shape = canonical(body.shape, name);
        if (!shape) {
            return tl::unexpected(shape.error());
        }
        auto map = nameBody(feature.id, shape->shape, name);
        if (!map) {
            return tl::unexpected(map.error());
        }
        const BodyId id = props.bodies ? (*props.bodies)[i].id : importedBodyId(feature.id, i);
        slots[i] = BodyUpdate{id, std::move(shape->shape), std::move(*map), {}};
        return {};
    });
    if (!done) {
        return tl::unexpected(done.error());
    }
    FeatureOutput out;
    if (props.bodies) {
        out.warnings = props.warnings;
        out.warnings.push_back("The stored geometry was missing; the bodies were imported again from '" + path + "'.");
        for (std::size_t i = 0; i < fileBodies.size(); ++i) {
            if (slots[i]->shape.contentHash() != (*props.bodies)[i].blob) {
                out.warnings.push_back("Body '" + (*props.bodies)[i].name +
                                       "' imported again differs from the geometry stored with the document.");
            }
        }
    } else {
        out.warnings = std::move(imported->warnings);
    }
    for (auto& slot : slots) {
        out.bodies.push_back(std::move(*slot));
    }
    return out;
}

constexpr std::string_view kPropsSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object",)"
    R"("required":["sourceFile","format","fileHash"],"properties":{)"
    R"("sourceFile":{"type":"string","minLength":1},"format":{"enum":["step","iges"]},)"
    R"("fileHash":{"type":"string","pattern":"^[0-9a-f]{64}$"},)"
    R"("options":{"type":"object","additionalProperties":false,"properties":{)"
    R"("heal":{"enum":["none","reader","full"]},"sew":{"type":"boolean"},)"
    R"("sewTolerance":{"type":"number","exclusiveMinimum":0}}},)"
    R"("role":{"enum":["part","fixture"]},"sourceUnits":{"enum":["inch","metric"]},)"
    R"("bodies":{"type":"array","minItems":1,"items":{"type":"object","additionalProperties":false,)"
    R"("required":["id","name","color","blob"],"properties":{)"
    R"("id":{"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"},)"
    R"("name":{"type":"string","minLength":1},)"
    R"("color":{"type":"array","minItems":4,"maxItems":4,"items":{"type":"number","minimum":0,"maximum":1}},)"
    R"("blob":{"type":"string","pattern":"^[0-9a-f]{64}$"}}}},)"
    R"("warnings":{"type":"array","items":{"type":"string"}}}})";

class ImportFeatureType final : public IFeatureType, public detail::BodyDeclaration {
public:
    std::string_view typeName() const override { return kImportType; }
    std::string_view propsSchemaJson() const override { return kPropsSchema; }
    std::vector<naming::TopoRef> references(const Feature&) const override { return {}; }

    // An import writes only its own bodies: those its props list, or new ones with ids derived from its own id
    // (which no earlier feature can have made).
    std::vector<BodyId> bodiesWritten(const Feature& feature) const override
    {
        std::vector<BodyId> out;
        try {
            auto props = parseImportProps(feature.propsJson, false, "Import");
            if (props && props->bodies) {
                for (const ImportBody& body : *props->bodies) {
                    out.push_back(body.id);
                }
            }
        } catch (...) {
            out.clear();
        }
        return out;
    }

    Result<FeatureOutput> rebuild(const Feature& feature, RebuildContext& ctx) const override
    {
        try {
            auto props = parseImportProps(feature.propsJson, false, "Import");
            if (!props) {
                return tl::unexpected(props.error());
            }
            IJobSystem* jobs = detail::jobSystemOf(ctx);
            if (!props->bodies) {
                return reimport(feature, *props, ctx, jobs, std::nullopt);
            }
            auto restored = restore(feature, *props, ctx, jobs);
            if (restored || restored.error().code != ErrorCode::NotFound) {
                return restored;
            }
            return reimport(feature, *props, ctx, jobs, restored.error());
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, "Import: out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, "Import: unexpected failure");
        }
    }
};

} // namespace

ImportCounters& importCounters()
{
    static ImportCounters counters;
    return counters;
}

std::unique_ptr<IFeatureType> makeImportFeatureType()
{
    return std::make_unique<ImportFeatureType>();
}

} // namespace ray::doc::features

namespace ray::doc {

BodyId importedBodyId(const FeatureId& importFeature, std::size_t index)
{
    Hasher hasher;
    hasher.str("ray.doc.import.body/1");
    hasher.bytes(std::as_bytes(std::span<const std::uint8_t>(importFeature.value.bytes)));
    hasher.i64(static_cast<std::int64_t>(index));
    const Hash256 digest = hasher.finish();
    Uuid id;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        id.bytes[i] = digest.bytes[i];
    }
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40); // version 4
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80); // RFC 4122 variant
    return BodyId{id};
}

Result<Hash256> hashFile(const std::string& utf8Path, JobContext& ctx)
{
    try {
        if (utf8Path.empty()) {
            return features::failure(ErrorCode::InvalidArgument, "No file name was given.");
        }
        std::filesystem::path path;
        try {
            path = std::filesystem::path(
                std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
        } catch (...) {
            return features::failure(ErrorCode::InvalidArgument, "The file name is not valid UTF-8.");
        }
        std::error_code ec;
        const auto status = std::filesystem::status(path, ec);
        if (ec || !std::filesystem::exists(status)) {
            return features::failure(ErrorCode::NotFound, "The file does not exist.");
        }
        if (!std::filesystem::is_regular_file(status)) {
            return features::failure(ErrorCode::IoError, "It is not a file.");
        }
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return features::failure(ErrorCode::IoError, "The file cannot be opened.");
        }
        constexpr std::size_t kChunk = std::size_t{1} << 20;
        std::vector<char> buffer(kChunk);
        Hasher hasher;
        std::uintmax_t done = 0;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) {
                hasher.bytes(std::as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(got))));
                done += static_cast<std::uintmax_t>(got);
            }
            if (auto cancelled = ctx.cancel.check(); !cancelled) {
                return tl::unexpected(cancelled.error());
            }
            if (!ec && size > 0) {
                ctx.progress.set(static_cast<double>(done) / static_cast<double>(size));
            }
        }
        if (in.bad()) {
            return features::failure(ErrorCode::IoError, "The file cannot be read.");
        }
        ctx.progress.set(1.0);
        return hasher.finish();
    } catch (const std::bad_alloc&) {
        return features::failure(ErrorCode::Internal, "Out of memory while reading the file.");
    } catch (...) {
        return features::failure(ErrorCode::IoError, "The file cannot be read.");
    }
}

Result<PreparedImport> prepareImport(const ImportRequest& request, JobContext& ctx)
{
    using features::failure;
    try {
        if (request.feature.value == Uuid{}) {
            return failure(ErrorCode::InvalidArgument, "Import: the feature id must not be the nil UUID.");
        }
        if (request.role != Body::Role::Part && request.role != Body::Role::Fixture) {
            return failure(ErrorCode::InvalidArgument, "Import: an imported body is a part or a fixture.");
        }
        const std::string fileName(features::fileNameOf(request.path));
        const std::optional<ImportFormat> format =
            request.format ? request.format : features::formatFromPath(request.path);
        if (!format) {
            return failure(ErrorCode::InvalidArgument,
                           "Import: '" + fileName + "' is not a STEP (.step, .stp) or IGES (.iges, .igs) file.");
        }
        auto where = [&](const Error& e) {
            return failure(e.code, "Cannot import '" + fileName + "': " + e.message);
        };
        JobContext hashCtx{ctx.cancel, ctx.progress.sub(0.04)};
        auto before = hashFile(request.path, hashCtx);
        if (!before) {
            return where(before.error());
        }
        JobContext importCtx{ctx.cancel, ctx.progress.sub(0.84)};
        auto imported = features::importFile(request.path, *format, request.options, importCtx);
        if (!imported) {
            return where(imported.error());
        }
        JobContext againCtx{ctx.cancel, ctx.progress.sub(0.04)};
        auto after = hashFile(request.path, againCtx);
        if (!after) {
            return where(after.error());
        }
        if (*after != *before) {
            return where(Error{ErrorCode::IoError, "the file changed while it was being imported.", {}});
        }
        if (imported->bodies.empty()) {
            return where(Error{ErrorCode::FormatError, "the file holds no geometry.", {}});
        }

        features::ImportProps props;
        props.sourceFile = request.path;
        props.format = *format;
        props.fileHash = *before;
        props.options = request.options;
        props.role = request.role;
        props.sourceUnits = imported->sourceUnits;
        props.warnings = imported->warnings;
        PreparedImport out;
        std::vector<features::ImportBody> bodies;
        Progress serializeProgress = ctx.progress.sub(0.08);
        for (std::size_t i = 0; i < imported->bodies.size(); ++i) {
            if (auto cancelled = ctx.cancel.check(); !cancelled) {
                return tl::unexpected(cancelled.error());
            }
            const kernel::ImportedBody& body = imported->bodies[i];
            features::ImportBody b;
            b.id = importedBodyId(request.feature, i);
            b.name = body.name.empty() ? "Body " + std::to_string(i + 1) : body.name;
            b.color = body.color;
            auto stored = features::canonical(body.shape, b.name);
            if (!stored) {
                return where(stored.error());
            }
            b.blob = features::blake3(stored->bytes);
            bodies.push_back(std::move(b));
            out.objects.emplace_back(bodies.back().blob, std::move(stored->bytes));
            serializeProgress.set(static_cast<double>(i + 1) / static_cast<double>(imported->bodies.size()));
        }
        props.bodies = std::move(bodies);

        const std::string name = request.name.empty() ? "Import " + fileName : request.name;
        out.command.name = std::string(features::kImportCommand);
        out.command.label = "Import " + fileName;
        out.command.argsJson = features::writeImportCommandArgs(request.feature, name, props, request.index);
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Import: out of memory");
    } catch (...) {
        return failure(ErrorCode::Internal, "Import: unexpected failure");
    }
}

std::vector<Hash256> referencedObjects(const DocState& state)
{
    std::vector<Hash256> out;
    try {
        for (const Feature& feature : state.features) {
            if (feature.type != features::kImportType) {
                continue;
            }
            auto props = features::parseImportProps(feature.propsJson, false, "Import");
            if (!props || !props->bodies) {
                continue;
            }
            for (const features::ImportBody& body : *props->bodies) {
                out.push_back(body.blob);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    } catch (...) {
        out.clear();
    }
    return out;
}

} // namespace ray::doc
