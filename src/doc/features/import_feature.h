// The "import" feature type (task T-010), private to src/doc and its tests. Its props, how it builds and how an
// import enters a document are described in <ray/document.h> ("The import feature").
#pragma once

#include <ray/document.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace ray::doc::features {

std::unique_ptr<IFeatureType> makeImportFeatureType();
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
