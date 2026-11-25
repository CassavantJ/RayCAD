// The "import" feature type (task T-010), private to src/doc and its tests. Its props, how it builds and how an
// import enters a document are described in <ray/document.h> ("The import feature").
#pragma once

#include <ray/document.h>

#include <atomic>
#include <functional>
#include <span>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace ray::doc::features {

std::unique_ptr<IFeatureType> makeImportFeatureType();

struct ImportBody;
// The element map of an imported body as the rebuild makes it (T-084): the names stored with the body (ImportBody::names,
// written by feature.import.replace: the names rematchImported carried over from the revision it replaced) when they
// can be read and fit `shape`, else naming::nameImported. `warning` is set (and nameImported used) when stored names
// cannot be used. KernelFailure when the body cannot be named at all.
Result<naming::ElementMap> importedBodyMap(const FeatureId& feature, const kernel::Shape& shape, const ImportBody& body,
                                           const std::function<Result<std::vector<std::byte>>(const Hash256&)>& object,
                                           std::optional<std::string>& warning);
// Does `map` name exactly the vertices, edges and faces of `shape`?
bool mapFits(const naming::ElementMap& map, const kernel::Shape& shape);
// The import's own steps, for feature.import.replace (T-084): a file read as the import reads it, and a body in its
// stored form (the shape a restore gives, and its BREP bytes, whose BLAKE3 is ImportBody::blob).
Result<kernel::ImportResult> importSourceFile(const std::string& path, ImportFormat format,
                                              const kernel::ImportOptions& options, JobContext& ctx);
struct StoredBody {
    kernel::Shape shape;
    std::vector<std::byte> bytes;
};
Result<StoredBody> storedForm(const kernel::Shape& shape, const std::string& bodyName);
Hash256 objectHash(std::span<const std::byte> bytes);
// The document.import command handler.
std::unique_ptr<ICommandHandler> makeImportCommandHandler();

// Counters for tests: how often a rebuild imported a source file (kernel::importStep / importIges) and how often
// it restored the bodies from stored objects. prepareImport counts neither. Process-wide; tests compare deltas.
struct ImportCounters {
    std::atomic<std::uint64_t> fileImports{0};
    std::atomic<std::uint64_t> restores{0};
};
ImportCounters& importCounters();

} // namespace ray::doc::features
